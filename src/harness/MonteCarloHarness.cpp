#include "harness/MonteCarloHarness.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>

#include "analytics/AnalyticsEngine.h"
#include "analytics/ReportPrinter.h"
#include "common/SimClock.h"
#include "lob/Venue.h"
#include "sim/PriceProcess.h"
#include "sim/SimulationEngine.h"
#include "sor/SmartOrderRouter.h"

namespace harness {

std::vector<ClientOrderSpec> generate_order_flow(uint32_t trial_seed, const SimConfig& config) {
    std::mt19937 rng(trial_seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int64_t> qty_dist(config.harness.min_order_qty, config.harness.max_order_qty);
    std::uniform_int_distribution<int> delay_dist(config.harness.submit_delay_min_ms, config.harness.submit_delay_max_ms);

    constexpr int64_t kAggressiveOffsetTicks = 1000;

    std::vector<ClientOrderSpec> script;
    script.reserve(config.harness.orders_per_trial);

    for (int i = 0; i < config.harness.orders_per_trial; ++i) {
        Side side = side_dist(rng) == 0 ? BUY : SELL;
        int64_t price = side == BUY
            ? static_cast<int64_t>(config.market.reference_price) + kAggressiveOffsetTicks
            : static_cast<int64_t>(config.market.reference_price) - kAggressiveOffsetTicks;

        script.push_back(ClientOrderSpec{ i, delay_dist(rng), side, price, qty_dist(rng) });
    }

    return script;
}

bool operator==(const SubmitKey& a, const SubmitKey& b) {
    return a.trial == b.trial && a.naive == b.naive && a.order_id == b.order_id;
}

size_t SubmitKeyHash::operator()(const SubmitKey& key) const noexcept {
    size_t h = std::hash<int>{}(key.trial);
    h ^= std::hash<bool>{}(key.naive) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<OrderID>{}(key.order_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

void ResultsCollector::record_submit(int trial, bool naive, OrderID id) {
    std::lock_guard<std::mutex> lock(mtx);
    submit_times_[SubmitKey{trial, naive, id}] = std::chrono::steady_clock::now();
}

bool ResultsCollector::wait_for_trial_reports(int trial, bool naive, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            bool any_pending = false;
            for (const auto& [key, submit_time] : submit_times_) {
                (void)submit_time;
                if (key.trial == trial && key.naive == naive) {
                    any_pending = true;
                    break;
                }
            }
            if (!any_pending) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void ResultsCollector::record_report(int trial, bool naive, const ExecutionReport& report,
                                      const std::unordered_map<VenueID, double>& fee_schedule) {
    auto now = std::chrono::steady_clock::now();
    double latency_us = -1.0;
    double fees = 0.0;

    for (const auto& [venue_id, stats] : report.venue_breakdown) {
        auto fee_it = fee_schedule.find(venue_id);
        if (fee_it != fee_schedule.end()) {
            fees += static_cast<double>(stats.filled_qty) * fee_it->second;
        }
    }

    std::lock_guard<std::mutex> lock(mtx);

    SubmitKey key{trial, naive, report.parent_id};
    auto it = submit_times_.find(key);
    if (it != submit_times_.end()) {
        latency_us = std::chrono::duration<double, std::micro>(now - it->second).count();
        submit_times_.erase(it);
    }

    rows.push_back(TrialRow{
        trial, naive, report.parent_id, report.side,
        report.intended_size, report.filled_size, report.fill_rate,
        report.decision_price, report.avg_fill_price,
        report.implementation_shortfall, report.vwap_slippage,
        report.timed_out, latency_us, fees
    });
}

TrialOutcome run_trial(int trial, uint32_t trial_seed, bool use_naive_split,
                        const std::vector<ClientOrderSpec>& script,
                        const SimConfig& config, ResultsCollector& collector) {
    SimClock clock;

    const VenueConfig& lit_cfg_1 = config.market.lit_venue_1;
    const VenueConfig& lit_cfg_2 = config.market.lit_venue_2;
    const VenueConfig& dark_cfg = config.market.dark_venue;

    std::unordered_map<VenueID, double> fee_schedule{
        { 1, lit_cfg_1.fee_per_share }, { 2, lit_cfg_2.fee_per_share }, { 3, dark_cfg.fee_per_share }
    };

    bool simulate_latency = config.market.simulate_latency;

    size_t pool_capacity = config.market.venue_order_pool_capacity;
    const QueueSizingConfig& queues = config.queues;

    auto venue_1 = std::make_unique<Venue>(1, lit_cfg_1, simulate_latency, pool_capacity, queues.order_inbox);
    auto venue_2 = std::make_unique<Venue>(2, lit_cfg_2, simulate_latency, pool_capacity, queues.order_inbox);
    auto venue_dark = std::make_unique<Venue>(3, dark_cfg, simulate_latency, pool_capacity, queues.order_inbox);

    venue_1->set_clock(&clock);
    venue_2->set_clock(&clock);
    venue_dark->set_clock(&clock);

    auto analytics = std::make_unique<AnalyticsEngine>(queues.analytics_trade, queues.analytics_order);
    analytics->set_clock(&clock);
    analytics->on_report([&](const ExecutionReport& report) {
        collector.record_report(trial, use_naive_split, report, fee_schedule);
        if (config.verbose_reports) print_report(report);
    });

    venue_1->set_analytics_queue(analytics->get_trade_inbox());
    venue_2->set_analytics_queue(analytics->get_trade_inbox());
    venue_dark->set_analytics_queue(analytics->get_trade_inbox());

    RouterConfig router_cfg{ .lot_size = config.router.lot_size,
                              .latency_cost_factor = config.router.latency_cost_factor,
                              .dark_pool_decay_rate = config.router.dark_pool_decay_rate,
                              .use_naive_split = use_naive_split,
                              .max_reroute_attempts = config.router.max_reroute_attempts };

    auto router = std::make_unique<SmartOrderRouter>(router_cfg, queues);
    router->add_venue(venue_1.get());
    router->add_venue(venue_2.get());
    router->add_venue(venue_dark.get());
    router->set_clock(&clock);
    router->set_analytics_queue(analytics->get_order_inbox());

    auto price_process = std::make_unique<PriceProcess>(
        config.market.reference_price, config.market.price_drift,
        config.market.price_volatility, config.market.price_dt, trial_seed);
    SimulationEngine sim_engine(std::move(price_process), &clock);

    sim_engine.add_market_maker(venue_1.get(), config.market.mm_lit_venue_1, trial_seed + 101, queues.fill);
    sim_engine.add_market_maker(venue_2.get(), config.market.mm_lit_venue_2, trial_seed + 102, queues.fill);
    sim_engine.add_market_maker(venue_dark.get(), config.market.mm_dark_venue, trial_seed + 103, queues.fill);
    sim_engine.add_noise_trader(venue_1.get(), config.market.noise_lit_venue_1, trial_seed + 201);
    sim_engine.add_noise_trader(venue_2.get(), config.market.noise_lit_venue_2, trial_seed + 202);

    venue_1->start();
    venue_2->start();
    venue_dark->start();
    analytics->start();
    router->start();
    sim_engine.start(config.market.price_dt);

    std::this_thread::sleep_for(std::chrono::milliseconds(config.harness.warmup_ms));

    for (const auto& spec : script) {
        std::this_thread::sleep_for(std::chrono::milliseconds(spec.delay_ms));

        OrderID order_id = spec.order_index;
        collector.record_submit(trial, use_naive_split, order_id);

        router->submit_order({ .order_id = order_id, .sender_type = SenderType::SOR,
                                .request_type = RequestType::ORDER, .side = spec.side,
                                .order_type = OrderType::IOC,
                                .price = spec.price, .quantity = spec.quantity });
    }

    bool completed = collector.wait_for_trial_reports(
        trial, use_naive_split, std::chrono::seconds(config.harness.report_wait_timeout_s));
    if (!completed) {
        std::cerr << "[ WARN ] trial " << trial << " (" << (use_naive_split ? "naive" : "sor")
                  << ") timed out waiting for all order reports\n";
    }

    sim_engine.stop();
    router->stop();
    analytics->stop();
    venue_1->stop();
    venue_2->stop();
    venue_dark->stop();

    TrialOutcome outcome;
    outcome.completed_without_timeout = completed;
    outcome.total_drops = venue_1->get_dropped_total() + venue_2->get_dropped_total()
                         + venue_dark->get_dropped_total() + router->get_dropped_total()
                         + analytics->get_dropped_total() + sim_engine.get_dropped_total();
    return outcome;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
    return values[idx];
}

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

ArmSummary summarize(const std::vector<TrialRow>& rows) {
    ArmSummary summary;
    std::vector<double> shortfall_bps, slippage_bps, fill_rates, latencies;

    for (const auto& row : rows) {
        double notional = row.decision_price * static_cast<double>(row.filled_size);
        if (notional > 0.0) {
            shortfall_bps.push_back(row.implementation_shortfall / notional * 1e4);
            slippage_bps.push_back(row.vwap_slippage / row.decision_price * 1e4);
        }
        fill_rates.push_back(row.fill_rate);
        summary.total_fees += row.fees_paid;
        if (row.tick_to_trade_us >= 0.0) latencies.push_back(row.tick_to_trade_us);
    }

    summary.sample_count = static_cast<int>(rows.size());
    summary.mean_shortfall_bps = mean(shortfall_bps);
    summary.mean_slippage_bps = mean(slippage_bps);
    summary.mean_fill_rate = mean(fill_rates);
    summary.p50_latency_us = percentile(latencies, 0.50);
    summary.p95_latency_us = percentile(latencies, 0.95);
    summary.p99_latency_us = percentile(latencies, 0.99);

    return summary;
}

bool passes_validity_gate(const ArmSummary& sor, const ArmSummary& naive,
                           uint64_t total_drops, const ValidityGate& gate) {
    if (total_drops > gate.max_allowed_drops) return false;
    if (sor.mean_fill_rate < gate.min_fill_rate) return false;
    if (naive.mean_fill_rate < gate.min_fill_rate) return false;
    return true;
}

MonteCarloRunResult run_monte_carlo(const SimConfig& config, bool show_progress) {
    ResultsCollector collector;
    MonteCarloRunResult result;

    for (int trial = 0; trial < config.harness.num_trials; ++trial) {
        uint32_t trial_seed = config.harness.seed_base + static_cast<uint32_t>(trial);
        std::vector<ClientOrderSpec> script = generate_order_flow(trial_seed, config);

        TrialOutcome sor_outcome = run_trial(trial, trial_seed, /*use_naive_split=*/false, script, config, collector);
        TrialOutcome naive_outcome = run_trial(trial, trial_seed, /*use_naive_split=*/true, script, config, collector);

        result.total_drops += sor_outcome.total_drops + naive_outcome.total_drops;
        if (!sor_outcome.completed_without_timeout) result.timed_out_trial_arms++;
        if (!naive_outcome.completed_without_timeout) result.timed_out_trial_arms++;

        if (show_progress && ((trial + 1) % 5 == 0 || trial + 1 == config.harness.num_trials)) {
            std::cout << "[ MAIN ] completed trial " << (trial + 1) << "/" << config.harness.num_trials << "\n";
        }
    }

    {
        std::lock_guard<std::mutex> lock(collector.mtx);
        for (const auto& row : collector.rows) {
            (row.naive ? result.naive_rows : result.sor_rows).push_back(row);
        }
    }

    return result;
}

void print_row(const std::string& label, double sor_v, double naive_v) {
    std::cout << std::left << std::setw(30) << label
              << std::right << std::setw(15) << std::fixed << std::setprecision(3) << sor_v
              << std::setw(15) << naive_v
              << std::setw(15) << (sor_v - naive_v) << "\n";
}

void print_comparison(const ArmSummary& sor, const ArmSummary& naive, const SimConfig& config) {
    std::cout << "\n=== SOR (DP-Optimal) vs. Naive Baseline: Monte Carlo Comparison ===\n";
    std::cout << "Trials: " << config.harness.num_trials << "  Orders/trial: " << config.harness.orders_per_trial
              << "  Samples/arm: " << sor.sample_count << "\n\n";

    std::cout << std::left << std::setw(30) << "Metric"
              << std::right << std::setw(15) << "SOR"
              << std::setw(15) << "Naive"
              << std::setw(15) << "Delta" << "\n";
    std::cout << std::string(75, '-') << "\n";

    print_row("Impl. Shortfall (bps)", sor.mean_shortfall_bps, naive.mean_shortfall_bps);
    print_row("VWAP Slippage (bps)", sor.mean_slippage_bps, naive.mean_slippage_bps);
    print_row("Fill Rate (%)", sor.mean_fill_rate * 100.0, naive.mean_fill_rate * 100.0);
    print_row("Total Fees ($)", sor.total_fees, naive.total_fees);
    print_row("Tick-to-Trade p50 (us)", sor.p50_latency_us, naive.p50_latency_us);
    print_row("Tick-to-Trade p95 (us)", sor.p95_latency_us, naive.p95_latency_us);
    print_row("Tick-to-Trade p99 (us)", sor.p99_latency_us, naive.p99_latency_us);
    std::cout << "\nNote: tick-to-trade here measures wall-clock submit->completion through the full\n"
                 "concurrent pipeline (8 OS threads/trial, spawned and joined fresh per run), on\n"
                 "whatever else is running on this machine - it is not an isolated measurement of\n"
                 "routing-algorithm cost alone, and will vary run to run with host load. SOR's\n"
                 "multi-venue split + reroute cascade needs more cross-thread round trips than\n"
                 "naive's single-venue dispatch, so it is structurally more exposed to that noise.\n"
                 "For a clean, threading-free view of raw data-structure speed, see bench_lob.\n";
}

std::string timestamp_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return oss.str();
}

void export_csv(const std::vector<TrialRow>& sor_rows, const std::vector<TrialRow>& naive_rows,
                 const std::string& path) {
    std::ofstream out(path);
    out << "trial,arm,order_id,side,intended_size,filled_size,fill_rate,decision_price,"
           "avg_fill_price,implementation_shortfall,vwap_slippage,timed_out,tick_to_trade_us,fees_paid\n";

    auto write_rows = [&out](const std::vector<TrialRow>& rows) {
        for (const auto& row : rows) {
            out << row.trial << ","
                << (row.naive ? "naive" : "sor") << ","
                << row.order_id << ","
                << (row.side == BUY ? "BUY" : "SELL") << ","
                << row.intended_size << ","
                << row.filled_size << ","
                << row.fill_rate << ","
                << row.decision_price << ","
                << row.avg_fill_price << ","
                << row.implementation_shortfall << ","
                << row.vwap_slippage << ","
                << (row.timed_out ? 1 : 0) << ","
                << row.tick_to_trade_us << ","
                << row.fees_paid << "\n";
        }
    };

    write_rows(sor_rows);
    write_rows(naive_rows);
}

}  // namespace harness
