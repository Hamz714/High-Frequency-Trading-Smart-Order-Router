#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/CycleClock.h"
#include "common/HostInfo.h"
#include "common/Types.h"
#include "config/SimConfig.h"
#include "lob/LimitOrderBook.h"
#include "lob/Venue.h"
#include "sor/DPEngine.h"

namespace {

constexpr int64_t TOUCH_BID = 10'000'000;
constexpr int64_t TOUCH_ASK = 10'000'100;
constexpr int64_t DEPTH_LEVELS = 200;
constexpr int64_t LEVEL_QTY = 500;
constexpr int64_t WORST_PRICE = TOUCH_ASK + DEPTH_LEVELS;
constexpr size_t BOOK_POOL_CAPACITY = 1024;

constexpr int DEFAULT_REPEATS = 9;
constexpr int TIMER_FLOOR_OPS = 50'000;

constexpr double INNER_WORK_BUDGET = 1.0e8;
constexpr int MIN_MEASURED_OPS = 400;
constexpr int MAX_MEASURED_OPS = 20'000;
constexpr int CHEAP_MEASURED_OPS = 20'000;

constexpr int64_t STRATEGY_COMPARE_SIZE = 10'000;
constexpr int64_t VENUE_SWEEP_SIZE = 5'000;

const std::vector<int64_t> SIZE_SWEEP = { 500, 1'000, 2'500, 5'000, 10'000, 25'000 };
const std::vector<int> VENUE_SWEEP = { 3, 6, 12 };

struct BenchResult {
    std::string name;
    std::string strategy;
    int64_t parent_size = 0;
    int64_t width = 0;
    int venue_count = 0;
    int64_t ops = 0;
    double total_seconds = 0.0;
    double p50_ns = 0.0, p95_ns = 0.0, p99_ns = 0.0, p999_ns = 0.0, max_ns = 0.0;

    double ops_per_sec() const {
        return total_seconds > 0.0 ? static_cast<double>(ops) / total_seconds : 0.0;
    }
};

struct AggregatedResult {
    std::string name;
    std::string strategy;
    int64_t parent_size = 0;
    int64_t width = 0;
    int venue_count = 0;
    int runs = 0;
    int64_t ops_per_run = 0;
    double median_ops_per_sec = 0.0, min_ops_per_sec = 0.0, max_ops_per_sec = 0.0;
    double p50_ns = 0.0, p95_ns = 0.0, p99_ns = 0.0, p999_ns = 0.0, max_ns = 0.0;
    double adj_p50_ns = 0.0, adj_p95_ns = 0.0, adj_p99_ns = 0.0, adj_p999_ns = 0.0, adj_max_ns = 0.0;

    double work_units() const {
        return static_cast<double>(venue_count) * static_cast<double>(width)
               * static_cast<double>(width) / 2.0;
    }

    double ns_per_work_unit() const {
        double units = work_units();
        return units > 0.0 ? adj_p50_ns / units : 0.0;
    }
};

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t idx = static_cast<size_t>(p * static_cast<double>(values.size() - 1));
    return values[idx];
}

double median_of(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t mid = values.size() / 2;
    if (values.size() % 2 != 0) return values[mid];
    return (values[mid - 1] + values[mid]) / 2.0;
}

BenchResult summarize(const std::string& name, const std::vector<double>& latencies_ns, double total_seconds) {
    BenchResult r;
    r.name = name;
    r.ops = static_cast<int64_t>(latencies_ns.size());
    r.total_seconds = total_seconds;
    r.p50_ns = percentile(latencies_ns, 0.50);
    r.p95_ns = percentile(latencies_ns, 0.95);
    r.p99_ns = percentile(latencies_ns, 0.99);
    r.p999_ns = percentile(latencies_ns, 0.999);
    r.max_ns = latencies_ns.empty() ? 0.0 : *std::max_element(latencies_ns.begin(), latencies_ns.end());
    return r;
}

double elapsed_seconds(uint64_t start_ticks) {
    return cycles_to_ns(cycle_now() - start_ticks) / 1e9;
}

struct VenueFixture {
    std::vector<std::unique_ptr<LimitOrderBook>> books;
    std::vector<VenueState> states;
};

void seed_book(LimitOrderBook& book) {
    for (int64_t i = 0; i < DEPTH_LEVELS; ++i) {
        book.submit(Side::SELL, OrderType::LIMIT, TOUCH_ASK + i, LEVEL_QTY);
        book.submit(Side::BUY, OrderType::LIMIT, TOUCH_BID - i, LEVEL_QTY);
    }
}

VenueFixture build_fixture(const MarketScenarioConfig& market, int num_lit, int num_dark) {
    VenueFixture fixture;
    int total = num_lit + num_dark;
    fixture.books.reserve(static_cast<size_t>(total));
    fixture.states.reserve(static_cast<size_t>(total));

    for (int i = 0; i < total; ++i) {
        fixture.books.push_back(std::make_unique<LimitOrderBook>(BOOK_POOL_CAPACITY));
        seed_book(*fixture.books.back());
    }

    for (int i = 0; i < num_lit; ++i) {
        VenueConfig cfg = (i % 2 == 0) ? market.lit_venue_1 : market.lit_venue_2;
        double skew = 1.0 + 0.05 * i;
        cfg.fee_per_share *= skew;
        cfg.impact_coefficient *= skew;
        fixture.states.push_back(VenueState{ i, cfg, fixture.books[static_cast<size_t>(i)].get() });
    }

    for (int i = 0; i < num_dark; ++i) {
        VenueConfig cfg = market.dark_venue;
        double skew = 1.0 + 0.05 * i;
        cfg.fee_per_share *= skew;
        cfg.historical_fill_ratio = std::min(0.95, cfg.historical_fill_ratio * skew);
        int index = num_lit + i;
        fixture.states.push_back(VenueState{ index, cfg, fixture.books[static_cast<size_t>(index)].get() });
    }

    return fixture;
}

int measured_ops_for(int64_t width, int venue_count) {
    double inner = static_cast<double>(venue_count) * static_cast<double>(width)
                   * static_cast<double>(width) / 2.0;
    if (inner < 1.0) inner = 1.0;
    double ops = INNER_WORK_BUDGET / inner;
    return static_cast<int>(std::clamp(ops, static_cast<double>(MIN_MEASURED_OPS),
                                            static_cast<double>(MAX_MEASURED_OPS)));
}

BenchResult run_timer_floor_benchmark() {
    std::vector<double> latencies_ns;
    latencies_ns.reserve(TIMER_FLOOR_OPS);

    uint64_t bench_start = cycle_now();
    for (int i = 0; i < TIMER_FLOOR_OPS; ++i) {
        uint64_t t0 = cycle_now();
        uint64_t t1 = cycle_now();
        latencies_ns.push_back(cycles_to_ns(t1 - t0));
    }
    double total_seconds = elapsed_seconds(bench_start);

    return summarize("timer noise floor (no work between reads)", latencies_ns, total_seconds);
}

using SplitFn = SplitResult (DPEngine::*)(int64_t, Side, int64_t, const std::vector<VenueState>&);

SplitFn split_fn_for(RoutingStrategy strategy) {
    switch (strategy) {
        case RoutingStrategy::DP_OPTIMAL:   return &DPEngine::compute_optimal_split;
        case RoutingStrategy::PROPORTIONAL: return &DPEngine::compute_proportional_split;
        case RoutingStrategy::NAIVE:        return &DPEngine::compute_naive_split;
    }
    return &DPEngine::compute_optimal_split;
}

volatile double g_sink = 0.0;

BenchResult run_split_benchmark(const std::string& name, RoutingStrategy strategy,
                                DPEngine& engine, const std::vector<VenueState>& venues,
                                int64_t total_size, int measured_ops, bool instrument) {
    SplitFn fn = split_fn_for(strategy);
    int warmup_ops = std::max(8, measured_ops / 10);

    double sink = 0.0;
    for (int i = 0; i < warmup_ops; ++i) {
        SplitResult r = (engine.*fn)(total_size, Side::BUY, WORST_PRICE, venues);
        sink += static_cast<double>(r.allocations.front());
    }

    std::vector<double> latencies_ns;
    if (instrument) latencies_ns.reserve(static_cast<size_t>(measured_ops));

    uint64_t bench_start = cycle_now();
    if (instrument) {
        for (int i = 0; i < measured_ops; ++i) {
            uint64_t t0 = cycle_now();
            {
                SplitResult r = (engine.*fn)(total_size, Side::BUY, WORST_PRICE, venues);
                sink += static_cast<double>(r.allocations.front());
            }
            uint64_t t1 = cycle_now();

            latencies_ns.push_back(cycles_to_ns(t1 - t0));
        }
    } else {
        for (int i = 0; i < measured_ops; ++i) {
            SplitResult r = (engine.*fn)(total_size, Side::BUY, WORST_PRICE, venues);
            sink += static_cast<double>(r.allocations.front());
        }
    }
    double total_seconds = elapsed_seconds(bench_start);
    g_sink = sink;

    BenchResult r = summarize(name, latencies_ns, total_seconds);
    if (!instrument) r.ops = measured_ops;
    r.strategy = routing_strategy_tag(strategy);
    r.parent_size = total_size;
    r.venue_count = static_cast<int>(venues.size());
    return r;
}

BenchResult run_scenario(const std::string& name, RoutingStrategy strategy, DPEngine& engine,
                         const std::vector<VenueState>& venues, int64_t total_size,
                         int64_t width, int measured_ops) {
    BenchResult timed = run_split_benchmark(name, strategy, engine, venues, total_size, measured_ops, true);
    BenchResult untimed = run_split_benchmark(name, strategy, engine, venues, total_size, measured_ops, false);
    timed.total_seconds = untimed.total_seconds;
    timed.ops = untimed.ops;
    timed.width = width;
    return timed;
}

AggregatedResult aggregate(const std::vector<BenchResult>& runs, double floor_ns) {
    AggregatedResult agg;
    if (runs.empty()) return agg;

    agg.name = runs.front().name;
    agg.strategy = runs.front().strategy;
    agg.parent_size = runs.front().parent_size;
    agg.width = runs.front().width;
    agg.venue_count = runs.front().venue_count;
    agg.ops_per_run = runs.front().ops;
    agg.runs = static_cast<int>(runs.size());

    std::vector<double> throughputs;
    std::vector<double> p50s, p95s, p99s, p999s, maxes;
    throughputs.reserve(runs.size());

    for (const auto& r : runs) {
        throughputs.push_back(r.ops_per_sec());
        p50s.push_back(r.p50_ns);
        p95s.push_back(r.p95_ns);
        p99s.push_back(r.p99_ns);
        p999s.push_back(r.p999_ns);
        maxes.push_back(r.max_ns);
    }

    agg.median_ops_per_sec = median_of(throughputs);
    agg.min_ops_per_sec = *std::min_element(throughputs.begin(), throughputs.end());
    agg.max_ops_per_sec = *std::max_element(throughputs.begin(), throughputs.end());
    agg.p50_ns = median_of(p50s);
    agg.p95_ns = median_of(p95s);
    agg.p99_ns = median_of(p99s);
    agg.p999_ns = median_of(p999s);
    agg.max_ns = *std::max_element(maxes.begin(), maxes.end());

    auto adjust = [floor_ns](double raw) { return std::max(0.0, raw - floor_ns); };
    agg.adj_p50_ns = adjust(agg.p50_ns);
    agg.adj_p95_ns = adjust(agg.p95_ns);
    agg.adj_p99_ns = adjust(agg.p99_ns);
    agg.adj_p999_ns = adjust(agg.p999_ns);
    agg.adj_max_ns = adjust(agg.max_ns);

    return agg;
}

void print_host_banner(const HostInfo& host) {
    std::cout << "\n=== Measurement environment ===\n\n";
    std::cout << "  os                    " << host.os << "\n";
    std::cout << "  cpu                   " << host.cpu_brand << "\n";
    std::cout << "  logical cpus          " << host.logical_cpus << "\n";
    std::cout << "  pinned to cpu         "
              << (host.pinned_cpu >= 0 ? std::to_string(host.pinned_cpu)
                                       : std::string("not pinned")) << "\n";
    std::cout << "  timer backend         " << host.timer_backend;
    if (host.timer_backend == "rdtsc") {
        std::cout << " (" << std::fixed << std::setprecision(3) << host.cycles_per_ns << " GHz, "
                  << (host.tsc_invariant ? "invariant" : "NOT invariant") << ")";
    }
    std::cout << "\n";
    std::cout << "  min timer delta       " << std::fixed << std::setprecision(2)
              << host.timer_resolution_ns << " ns\n";
    std::cout << "  scaling governor      " << host.scaling_governor << "\n";
    std::cout << "  turbo                 " << host.turbo << "\n";
    std::cout << "  hypervisor            " << (host.hypervisor ? "yes (virtualised host)" : "no") << "\n";

    if (!host.tsc_invariant && host.timer_backend == "rdtsc") {
        std::cout << "\n  WARNING: TSC is not invariant on this host - cycle counts may drift with\n"
                     "  frequency or across cores. Treat latencies as indicative only.\n";
    }
    if (host.pinned_cpu < 0) {
        std::cout << "\n  WARNING: thread is not pinned - migrations between cores will show up in\n"
                     "  the tail percentiles.\n";
    }
    if (host.hypervisor) {
        std::cout << "\n  NOTE: running under a hypervisor (VM, WSL2, or cloud instance). Resolution\n"
                     "  is real, but core isolation and frequency policy are not under your control.\n";
    }
}

void print_floor_summary(const AggregatedResult& floor) {
    std::cout << "\n=== Timer noise floor ===\n\n";
    std::cout << "  Two back-to-back timer reads, no work between them, "
              << TIMER_FLOOR_OPS << " samples x " << floor.runs << " runs.\n";
    std::cout << "  p50 " << std::fixed << std::setprecision(2) << floor.p50_ns << " ns"
              << "   p95 " << floor.p95_ns << " ns"
              << "   p99 " << floor.p99_ns << " ns"
              << "   max " << floor.max_ns << " ns\n";
    std::cout << "  Subtracted from every percentile below. At these magnitudes it is negligible;\n"
                 "  it is reported and applied only for consistency with bench_lob.\n";
}

std::string format_ns(double ns) {
    std::ostringstream oss;
    if (ns >= 1'000'000.0) {
        oss << std::fixed << std::setprecision(2) << ns / 1e6 << " ms";
    } else if (ns >= 1'000.0) {
        oss << std::fixed << std::setprecision(2) << ns / 1e3 << " us";
    } else {
        oss << std::fixed << std::setprecision(0) << ns << " ns";
    }
    return oss.str();
}

void print_decision_table(const std::string& title, const std::string& preamble,
                          const std::vector<AggregatedResult>& results, bool show_work_unit) {
    std::cout << "\n=== " << title << " ===\n\n";
    std::cout << preamble << "\n";

    std::cout << std::left << std::setw(34) << "Scenario"
              << std::right << std::setw(8) << "size"
              << std::setw(7) << "W"
              << std::setw(4) << "V"
              << std::setw(8) << "ops"
              << std::setw(14) << "decisions/s"
              << std::setw(13) << "p50"
              << std::setw(13) << "p95"
              << std::setw(13) << "p99"
              << std::setw(13) << "max";
    if (show_work_unit) std::cout << std::setw(14) << "ns/(V*W^2/2)";
    std::cout << "\n";
    std::cout << std::string(show_work_unit ? 141 : 127, '-') << "\n";

    for (const auto& r : results) {
        std::cout << std::left << std::setw(34) << r.name
                  << std::right << std::setw(8) << r.parent_size
                  << std::setw(7) << r.width
                  << std::setw(4) << r.venue_count
                  << std::setw(8) << r.ops_per_run
                  << std::setw(14) << std::fixed << std::setprecision(0) << r.median_ops_per_sec
                  << std::setw(13) << format_ns(r.adj_p50_ns)
                  << std::setw(13) << format_ns(r.adj_p95_ns)
                  << std::setw(13) << format_ns(r.adj_p99_ns)
                  << std::setw(13) << format_ns(r.adj_max_ns);
        if (show_work_unit) {
            std::cout << std::setw(14) << std::fixed << std::setprecision(3) << r.ns_per_work_unit();
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

void print_size_scaling(const std::vector<AggregatedResult>& results) {
    std::cout << "Growth check - the DP is O(V*W^2) with W = size/lot_size + 1:\n";
    for (size_t i = 1; i < results.size(); ++i) {
        const AggregatedResult& prev = results[i - 1];
        const AggregatedResult& cur = results[i];
        if (prev.adj_p50_ns <= 0.0 || prev.width <= 0) continue;

        double measured = cur.adj_p50_ns / prev.adj_p50_ns;
        double predicted = (static_cast<double>(cur.width) * static_cast<double>(cur.width))
                           / (static_cast<double>(prev.width) * static_cast<double>(prev.width));

        std::cout << "  W " << std::setw(4) << prev.width << " -> " << std::setw(4) << cur.width
                  << ":  measured " << std::fixed << std::setprecision(2) << measured
                  << "x   W^2 predicts " << predicted << "x\n";
    }
    std::cout << "\n";
}

void print_venue_scaling(const std::vector<AggregatedResult>& results) {
    std::cout << "Growth check - cost should be linear in the venue count V:\n";
    for (size_t i = 1; i < results.size(); ++i) {
        const AggregatedResult& prev = results[i - 1];
        const AggregatedResult& cur = results[i];
        if (prev.adj_p50_ns <= 0.0 || prev.venue_count <= 0) continue;

        double measured = cur.adj_p50_ns / prev.adj_p50_ns;
        double predicted = static_cast<double>(cur.venue_count) / static_cast<double>(prev.venue_count);

        std::cout << "  V " << std::setw(3) << prev.venue_count << " -> " << std::setw(3) << cur.venue_count
                  << ":  measured " << std::fixed << std::setprecision(2) << measured
                  << "x   V predicts " << predicted << "x\n";
    }
    std::cout << "\n";
}

void print_strategy_comparison(const std::vector<AggregatedResult>& results) {
    if (results.empty()) return;
    const AggregatedResult& dp = results.front();

    std::cout << "Cost of optimality at size " << dp.parent_size << " (W = " << dp.width
              << ", V = " << dp.venue_count << "):\n";
    for (size_t i = 1; i < results.size(); ++i) {
        const AggregatedResult& other = results[i];
        if (other.adj_p50_ns <= 0.0) continue;
        std::cout << "  DP p50 is " << std::fixed << std::setprecision(1)
                  << dp.adj_p50_ns / other.adj_p50_ns << "x the p50 of " << other.name << "\n";
    }
    std::cout << "\n";
}

std::string timestamp_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << "_"
        << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void export_csv(const std::vector<AggregatedResult>& results, const AggregatedResult& floor,
                const HostInfo& host, const RouterConfig& router, const std::string& path) {
    std::ofstream out(path);
    out << "# " << describe_host(host) << "\n";
    out << "# lot_size=" << router.lot_size << "; runs=" << floor.runs
        << "; timer_floor_p50_ns=" << floor.p50_ns << "\n";
    out << "# throughput measured on an uninstrumented pass; adj_* percentiles have the"
           " timer floor p50 subtracted\n";

    out << "scenario,strategy,parent_size,lots_W,venues,runs,median_decisions_per_sec,"
           "min_decisions_per_sec,max_decisions_per_sec,"
           "p50_ns,p95_ns,p99_ns,p999_ns,max_ns,"
           "adj_p50_ns,adj_p95_ns,adj_p99_ns,adj_p999_ns,adj_max_ns,ns_per_work_unit\n";

    auto write_row = [&out](const AggregatedResult& r) {
        out << '"' << r.name << '"' << "," << r.strategy << "," << r.parent_size << ","
            << r.width << "," << r.venue_count << "," << r.runs << ","
            << r.median_ops_per_sec << "," << r.min_ops_per_sec << "," << r.max_ops_per_sec << ","
            << r.p50_ns << "," << r.p95_ns << "," << r.p99_ns << "," << r.p999_ns << "," << r.max_ns << ","
            << r.adj_p50_ns << "," << r.adj_p95_ns << "," << r.adj_p99_ns << ","
            << r.adj_p999_ns << "," << r.adj_max_ns << "," << r.ns_per_work_unit() << "\n";
    };

    write_row(floor);
    for (const auto& r : results) write_row(r);
}

}  // namespace

int main(int argc, char** argv) {
    SimConfig sim = default_sim_config();

    int repeats = DEFAULT_REPEATS;
    int requested_cpu = -2;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeats = std::max(1, std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--pin") == 0 && i + 1 < argc) {
            requested_cpu = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--no-pin") == 0) {
            requested_cpu = -1;
        } else if (std::strcmp(argv[i], "--lot-size") == 0 && i + 1 < argc) {
            sim.router.lot_size = std::max<int64_t>(1, std::atoll(argv[++i]));
        }
    }

    unsigned available = std::max(1u, std::thread::hardware_concurrency());
    if (requested_cpu == -2) requested_cpu = static_cast<int>(available) - 1;

    HostInfo host = collect_host_info();
    if (requested_cpu >= 0 && pin_current_thread(requested_cpu)) {
        host.pinned_cpu = requested_cpu;
    }

    print_host_banner(host);

    RouterConfig router{ sim.router.lot_size, sim.router.latency_cost_factor,
                         sim.router.dark_pool_decay_rate, RoutingStrategy::DP_OPTIMAL,
                         sim.router.max_reroute_attempts };
    DPEngine engine(router);

    VenueFixture base_fixture = build_fixture(sim.market, 2, 1);

    std::vector<VenueFixture> sweep_fixtures;
    sweep_fixtures.reserve(VENUE_SWEEP.size());
    for (int v : VENUE_SWEEP) {
        sweep_fixtures.push_back(build_fixture(sim.market, v - v / 3, v / 3));
    }

    std::cout << "\n[ BENCH_SOR ] running routing-decision microbenchmark (" << repeats
              << " runs, lot_size " << router.lot_size << ")...\n";

    std::vector<std::vector<BenchResult>> size_runs(SIZE_SWEEP.size());
    std::vector<std::vector<BenchResult>> venue_runs(VENUE_SWEEP.size());
    std::vector<std::vector<BenchResult>> strategy_runs(NUM_ROUTING_STRATEGIES);
    std::vector<BenchResult> floor_runs;

    const std::vector<RoutingStrategy> strategies = { RoutingStrategy::DP_OPTIMAL,
                                                      RoutingStrategy::PROPORTIONAL,
                                                      RoutingStrategy::NAIVE };

    for (int run = 0; run < repeats; ++run) {
        floor_runs.push_back(run_timer_floor_benchmark());

        for (size_t s = 0; s < SIZE_SWEEP.size(); ++s) {
            int64_t size = SIZE_SWEEP[s];
            int64_t width = size / router.lot_size + 1;
            std::ostringstream name;
            name << "DP split, parent " << size;
            size_runs[s].push_back(run_scenario(name.str(), RoutingStrategy::DP_OPTIMAL, engine,
                                                base_fixture.states, size, width,
                                                measured_ops_for(width, 3)));
        }

        for (size_t v = 0; v < VENUE_SWEEP.size(); ++v) {
            int64_t width = VENUE_SWEEP_SIZE / router.lot_size + 1;
            std::ostringstream name;
            name << "DP split, " << VENUE_SWEEP[v] << " venues";
            venue_runs[v].push_back(run_scenario(name.str(), RoutingStrategy::DP_OPTIMAL, engine,
                                                 sweep_fixtures[v].states, VENUE_SWEEP_SIZE, width,
                                                 measured_ops_for(width, VENUE_SWEEP[v])));
        }

        for (size_t k = 0; k < strategies.size(); ++k) {
            int64_t width = STRATEGY_COMPARE_SIZE / router.lot_size + 1;
            int ops = strategies[k] == RoutingStrategy::DP_OPTIMAL
                          ? measured_ops_for(width, 3)
                          : CHEAP_MEASURED_OPS;
            strategy_runs[k].push_back(run_scenario(routing_strategy_label(strategies[k]),
                                                    strategies[k], engine, base_fixture.states,
                                                    STRATEGY_COMPARE_SIZE, width, ops));
        }
    }

    AggregatedResult agg_floor = aggregate(floor_runs, 0.0);
    double floor_ns = agg_floor.p50_ns;

    std::vector<AggregatedResult> size_results, venue_results, strategy_results;
    for (const auto& runs : size_runs) size_results.push_back(aggregate(runs, floor_ns));
    for (const auto& runs : venue_runs) venue_results.push_back(aggregate(runs, floor_ns));
    for (const auto& runs : strategy_runs) strategy_results.push_back(aggregate(runs, floor_ns));

    print_floor_summary(agg_floor);

    print_decision_table("Routing decision latency vs. parent size (2 lit + 1 dark venue)",
                         "One decision is one full compute_optimal_split call: cost tables, the DP\n"
                         "sweep, backtracking, and the odd-lot remainder pass. W = size/lot_size + 1.\n"
                         "Throughput is from an uninstrumented pass; percentiles from an instrumented one.\n",
                         size_results, true);
    print_size_scaling(size_results);

    print_decision_table("Routing decision latency vs. venue count (parent "
                             + std::to_string(VENUE_SWEEP_SIZE) + ")",
                         "Venue counts are two-thirds lit, one-third dark. Both venue classes run the\n"
                         "same inner loop, so cost should track V linearly at fixed W.\n",
                         venue_results, true);
    print_venue_scaling(venue_results);

    print_decision_table("Routing strategies at parent " + std::to_string(STRATEGY_COMPARE_SIZE),
                         "What the exact DP costs relative to the two baselines it is measured against.\n",
                         strategy_results, false);
    print_strategy_comparison(strategy_results);

    std::vector<AggregatedResult> all;
    all.insert(all.end(), size_results.begin(), size_results.end());
    all.insert(all.end(), venue_results.begin(), venue_results.end());
    all.insert(all.end(), strategy_results.begin(), strategy_results.end());

    std::filesystem::create_directories("results");
    std::string path = "results/sor_benchmark_" + timestamp_string() + ".csv";
    export_csv(all, agg_floor, host, router, path);
    std::cout << "[ BENCH_SOR ] wrote results to " << path << "\n";

    return 0;
}
