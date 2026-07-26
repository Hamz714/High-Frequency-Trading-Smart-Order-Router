#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "config/SimConfig.h"
#include "harness/MonteCarloHarness.h"

namespace {

struct ParamSpec {
    std::string name;
    double lo;
    double hi;
    bool is_int;
    std::function<void(SimConfig&, double)> apply;
};

std::vector<ParamSpec> build_param_specs() {
    std::vector<ParamSpec> specs;

    auto add = [&](std::string name, double lo, double hi, bool is_int, std::function<void(SimConfig&, double)> apply) {
        specs.push_back(ParamSpec{std::move(name), lo, hi, is_int, std::move(apply)});
    };

    add("router.lot_size", 1, 50, true, [](SimConfig& c, double v) { c.router.lot_size = static_cast<int64_t>(v); });
    add("router.latency_cost_factor", 0, 10, true, [](SimConfig& c, double v) { c.router.latency_cost_factor = static_cast<int64_t>(v); });
    add("router.dark_pool_decay_rate", 0.0001, 0.006, false, [](SimConfig& c, double v) { c.router.dark_pool_decay_rate = v; });
    add("router.max_reroute_attempts", 1, 6, true, [](SimConfig& c, double v) { c.router.max_reroute_attempts = static_cast<int>(v); });

    add("lit1.fee_per_share", 0.0001, 0.0010, false, [](SimConfig& c, double v) { c.market.lit_venue_1.fee_per_share = v; });
    add("lit2.fee_per_share", 0.0001, 0.0010, false, [](SimConfig& c, double v) { c.market.lit_venue_2.fee_per_share = v; });
    add("dark.fee_per_share", 0.00002, 0.0005, false, [](SimConfig& c, double v) { c.market.dark_venue.fee_per_share = v; });
    add("lit1.latency_us", 10, 300, true, [](SimConfig& c, double v) { c.market.lit_venue_1.latency_us = static_cast<int64_t>(v); });
    add("lit2.latency_us", 10, 300, true, [](SimConfig& c, double v) { c.market.lit_venue_2.latency_us = static_cast<int64_t>(v); });
    add("dark.latency_us", 50, 500, true, [](SimConfig& c, double v) { c.market.dark_venue.latency_us = static_cast<int64_t>(v); });
    add("lit1.impact_coefficient", 0.5, 8.0, false, [](SimConfig& c, double v) { c.market.lit_venue_1.impact_coefficient = v; });
    add("lit2.impact_coefficient", 0.5, 8.0, false, [](SimConfig& c, double v) { c.market.lit_venue_2.impact_coefficient = v; });
    add("dark.historical_fill_ratio", 0.1, 0.8, false, [](SimConfig& c, double v) { c.market.dark_venue.historical_fill_ratio = v; });

    add("mm_lit1.base_spread", 1, 12, true, [](SimConfig& c, double v) { c.market.mm_lit_venue_1.base_spread = static_cast<int64_t>(v); });
    add("mm_lit2.base_spread", 1, 12, true, [](SimConfig& c, double v) { c.market.mm_lit_venue_2.base_spread = static_cast<int64_t>(v); });
    add("mm_dark.base_spread", 1, 12, true, [](SimConfig& c, double v) { c.market.mm_dark_venue.base_spread = static_cast<int64_t>(v); });
    add("mm_lit1.spread_sensitivity", 0.1, 2.0, false, [](SimConfig& c, double v) { c.market.mm_lit_venue_1.spread_sensitivity = v; });
    add("mm_lit2.spread_sensitivity", 0.1, 2.0, false, [](SimConfig& c, double v) { c.market.mm_lit_venue_2.spread_sensitivity = v; });
    add("mm_dark.spread_sensitivity", 0.1, 2.0, false, [](SimConfig& c, double v) { c.market.mm_dark_venue.spread_sensitivity = v; });
    add("mm_lit1.quantity_mean", 10, 150, true, [](SimConfig& c, double v) { c.market.mm_lit_venue_1.quantity_mean = static_cast<int64_t>(v); });
    add("mm_lit2.quantity_mean", 10, 150, true, [](SimConfig& c, double v) { c.market.mm_lit_venue_2.quantity_mean = static_cast<int64_t>(v); });
    add("mm_dark.quantity_mean", 10, 150, true, [](SimConfig& c, double v) { c.market.mm_dark_venue.quantity_mean = static_cast<int64_t>(v); });
    add("mm_lit1.quantity_variance", 500, 10000, false, [](SimConfig& c, double v) { c.market.mm_lit_venue_1.quantity_variance = v; });
    add("mm_lit2.quantity_variance", 500, 10000, false, [](SimConfig& c, double v) { c.market.mm_lit_venue_2.quantity_variance = v; });
    add("mm_dark.quantity_variance", 500, 10000, false, [](SimConfig& c, double v) { c.market.mm_dark_venue.quantity_variance = v; });
    add("mm_lit1.stale_distance_ticks", 20, 150, true, [](SimConfig& c, double v) { c.market.mm_lit_venue_1.stale_distance_ticks = static_cast<int64_t>(v); });
    add("mm_lit2.stale_distance_ticks", 20, 150, true, [](SimConfig& c, double v) { c.market.mm_lit_venue_2.stale_distance_ticks = static_cast<int64_t>(v); });
    add("mm_dark.stale_distance_ticks", 20, 150, true, [](SimConfig& c, double v) { c.market.mm_dark_venue.stale_distance_ticks = static_cast<int64_t>(v); });

    add("noise1.arrival_rate", 10, 200, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_1.arrival_rate = v; });
    add("noise2.arrival_rate", 10, 200, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_2.arrival_rate = v; });
    add("noise1.size_mu", 2.0, 6.0, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_1.size_mu = v; });
    add("noise2.size_mu", 2.0, 6.0, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_2.size_mu = v; });
    add("noise1.size_sigma", 0.1, 1.0, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_1.size_sigma = v; });
    add("noise2.size_sigma", 0.1, 1.0, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_2.size_sigma = v; });
    add("noise1.trend_sensitivity", 0.0, 8.0, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_1.trend_sensitivity = v; });
    add("noise2.trend_sensitivity", 0.0, 8.0, false, [](SimConfig& c, double v) { c.market.noise_lit_venue_2.trend_sensitivity = v; });

    add("price.drift", -0.01, 0.01, false, [](SimConfig& c, double v) { c.market.price_drift = v; });
    add("price.volatility", 0.005, 0.06, false, [](SimConfig& c, double v) { c.market.price_volatility = v; });

    return specs;
}

SimConfig perturb(const SimConfig& base, const std::vector<ParamSpec>& specs, std::mt19937& rng,
                   std::vector<std::pair<std::string, double>>& sampled_out) {
    SimConfig config = base;
    std::uniform_real_distribution<double> unit(0.0, 1.0);

    for (const auto& spec : specs) {
        double raw = spec.lo + unit(rng) * (spec.hi - spec.lo);
        double value = spec.is_int ? std::round(raw) : raw;
        spec.apply(config, value);
        sampled_out.push_back({spec.name, value});
    }

    return config;
}

double objective(const harness::ArmSummary& sor, const harness::ArmSummary& naive) {
    double shortfall_edge = naive.mean_shortfall_bps - sor.mean_shortfall_bps;
    double slippage_edge = naive.mean_slippage_bps - sor.mean_slippage_bps;
    double fillrate_edge = sor.mean_fill_rate - naive.mean_fill_rate;
    return 2.0 * shortfall_edge + 1.0 * slippage_edge + 50.0 * fillrate_edge;
}

void print_config_literal(const SimConfig& config) {
    std::cout << std::setprecision(10);
    std::cout << "\n--- Paste into src/config/SimConfig.cpp's default_sim_config() body ---\n\n";

    auto venue = [](const char* var, const VenueConfig& v) {
        std::cout << "    config.market." << var << " = VenueConfig{ .type = " << (v.type == LIT ? "LIT" : "DARK")
                   << ", .fee_per_share = " << v.fee_per_share << ", .latency_us = " << v.latency_us
                   << ",\n                                               .impact_coefficient = " << v.impact_coefficient
                   << ", .historical_fill_ratio = " << v.historical_fill_ratio << " };\n";
    };
    venue("lit_venue_1", config.market.lit_venue_1);
    venue("lit_venue_2", config.market.lit_venue_2);
    venue("dark_venue", config.market.dark_venue);

    auto mm = [](const char* var, const MarketMakerConfig& m) {
        std::cout << "    config.market." << var << " = MarketMakerConfig{ .base_spread = " << m.base_spread
                   << ", .spread_sensitivity = " << m.spread_sensitivity
                   << ",\n                                                    .quantity_mean = " << m.quantity_mean
                   << ", .quantity_variance = " << m.quantity_variance
                   << ",\n                                                    .max_orders_per_side = " << m.max_orders_per_side
                   << ", .stale_distance_ticks = " << m.stale_distance_ticks << " };\n";
    };
    mm("mm_lit_venue_1", config.market.mm_lit_venue_1);
    mm("mm_lit_venue_2", config.market.mm_lit_venue_2);
    mm("mm_dark_venue", config.market.mm_dark_venue);

    auto noise = [](const char* var, const NoiseTraderConfig& n) {
        std::cout << "    config.market." << var << " = NoiseTraderConfig{ .arrival_rate = " << n.arrival_rate
                   << ", .size_mu = " << n.size_mu
                   << ",\n                                                     .size_sigma = " << n.size_sigma
                   << ", .trend_sensitivity = " << n.trend_sensitivity << " };\n";
    };
    noise("noise_lit_venue_1", config.market.noise_lit_venue_1);
    noise("noise_lit_venue_2", config.market.noise_lit_venue_2);

    std::cout << "\n    config.market.reference_price = " << config.market.reference_price << ";\n";
    std::cout << "    config.market.price_drift = " << config.market.price_drift << ";\n";
    std::cout << "    config.market.price_volatility = " << config.market.price_volatility << ";\n";
    std::cout << "    config.market.price_dt = " << config.market.price_dt << ";\n";

    std::cout << "\n    config.router.lot_size = " << config.router.lot_size << ";\n";
    std::cout << "    config.router.latency_cost_factor = " << config.router.latency_cost_factor << ";\n";
    std::cout << "    config.router.dark_pool_decay_rate = " << config.router.dark_pool_decay_rate << ";\n";
    std::cout << "    config.router.max_reroute_attempts = " << config.router.max_reroute_attempts << ";\n";
    std::cout << "\n--- end paste block (HarnessConfig fields intentionally omitted - not calibrated) ---\n";
}

}  // namespace

int main(int argc, char** argv) {
    int samples = 300;
    int trials_per_sample = 6;
    uint32_t seed = 12345;
    double time_budget_minutes = 90.0;

    for (int i = 1; i < argc; ++i) {
        auto next_double = [&](int& i) { return std::stod(argv[++i]); };
        auto next_int = [&](int& i) { return std::stoi(argv[++i]); };

        if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc) samples = next_int(i);
        else if (std::strcmp(argv[i], "--trials-per-sample") == 0 && i + 1 < argc) trials_per_sample = next_int(i);
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = static_cast<uint32_t>(next_int(i));
        else if (std::strcmp(argv[i], "--time-budget-minutes") == 0 && i + 1 < argc) time_budget_minutes = next_double(i);
    }

    std::cout << "[ CALIBRATE ] samples=" << samples << " trials/sample=" << trials_per_sample
              << " seed=" << seed << " time_budget_minutes=" << time_budget_minutes << "\n";

    std::vector<ParamSpec> specs = build_param_specs();
    std::mt19937 rng(seed);
    SimConfig base = default_sim_config();

    std::filesystem::create_directories("results");
    std::string csv_path = "results/calibration_" + harness::timestamp_string() + ".csv";
    std::ofstream csv(csv_path);
    csv << "sample_index,status,score,shortfall_edge_bps,slippage_edge_bps,fillrate_edge,"
           "sor_fill_rate,naive_fill_rate,total_drops";
    for (const auto& spec : specs) csv << "," << spec.name;
    csv << "\n";

    struct SampleResult {
        double score;
        SimConfig config;
    };
    std::vector<SampleResult> valid_samples;

    auto start_time = std::chrono::steady_clock::now();
    int rejected_drops = 0, rejected_fillrate = 0, valid_count = 0;
    double best_score_seen = -std::numeric_limits<double>::infinity();

    for (int i = 0; i < samples; ++i) {
        std::vector<std::pair<std::string, double>> sampled;
        SimConfig candidate = perturb(base, specs, rng, sampled);
        candidate.harness.num_trials = trials_per_sample;
        candidate.harness.seed_base = 1000 + static_cast<uint32_t>(i) * 10000;
        candidate.verbose_reports = false;

        harness::MonteCarloRunResult result = harness::run_monte_carlo(candidate, /*show_progress=*/false);
        harness::ArmSummary sor = harness::summarize(result.sor_rows);
        harness::ArmSummary naive = harness::summarize(result.naive_rows);

        harness::ValidityGate gate{ .max_allowed_drops = 0, .min_fill_rate = 0.5 };
        bool drops_ok = result.total_drops <= gate.max_allowed_drops;
        bool fillrate_ok = sor.mean_fill_rate >= gate.min_fill_rate && naive.mean_fill_rate >= gate.min_fill_rate;
        bool valid = drops_ok && fillrate_ok;

        double shortfall_edge = naive.mean_shortfall_bps - sor.mean_shortfall_bps;
        double slippage_edge = naive.mean_slippage_bps - sor.mean_slippage_bps;
        double fillrate_edge = sor.mean_fill_rate - naive.mean_fill_rate;
        double score = objective(sor, naive);

        std::string status = valid ? "valid" : (!drops_ok ? "rejected_drops" : "rejected_fillrate");
        if (valid) valid_count++; else if (!drops_ok) rejected_drops++; else rejected_fillrate++;

        csv << i << "," << status << "," << score << "," << shortfall_edge << "," << slippage_edge << ","
            << fillrate_edge << "," << sor.mean_fill_rate << "," << naive.mean_fill_rate << "," << result.total_drops;
        for (const auto& [name, value] : sampled) csv << "," << value;
        csv << "\n";
        csv.flush();

        if (valid) {
            valid_samples.push_back(SampleResult{score, candidate});
            best_score_seen = std::max(best_score_seen, score);
        }

        double elapsed_min = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count() / 60.0;
        if ((i + 1) % 10 == 0 || i + 1 == samples) {
            std::cout << "[ CALIBRATE ] sample " << (i + 1) << "/" << samples
                      << " elapsed=" << std::fixed << std::setprecision(1) << elapsed_min << "min"
                      << " valid=" << valid_count << " rejected_drops=" << rejected_drops
                      << " rejected_fillrate=" << rejected_fillrate
                      << " best_score_seen=" << std::setprecision(3) << best_score_seen << "\n";
        }
        if (elapsed_min > time_budget_minutes) {
            std::cout << "[ CALIBRATE ] time budget exceeded, stopping sweep early after " << (i + 1) << " samples\n";
            break;
        }
    }

    csv.close();
    std::cout << "[ CALIBRATE ] sweep complete, wrote " << csv_path << "\n";

    if (valid_samples.empty()) {
        std::cerr << "[ CALIBRATE ] no valid sample found - every candidate was rejected "
                     "(queue drops or degenerate fill rate). Nothing to confirm.\n";
        return 1;
    }

    std::sort(valid_samples.begin(), valid_samples.end(),
              [](const SampleResult& a, const SampleResult& b) { return a.score > b.score; });

    constexpr int kTopKToRevalidate = 5;
    constexpr int kRevalidationTrialMultiplier = 3;
    int top_k = std::min(static_cast<int>(valid_samples.size()), kTopKToRevalidate);
    int revalidation_trials = std::min(25, trials_per_sample * kRevalidationTrialMultiplier);

    std::cout << "\n[ CALIBRATE ] re-validating top " << top_k << " sweep candidates with "
              << revalidation_trials << " trials each on fresh seeds (guards against noise overfitting)...\n";

    double best_revalidated_score = -std::numeric_limits<double>::infinity();
    SimConfig best_config = base;
    bool found_valid_revalidated = false;

    for (int k = 0; k < top_k; ++k) {
        SimConfig candidate = valid_samples[k].config;
        candidate.harness.num_trials = revalidation_trials;
        candidate.harness.seed_base = 500000 + static_cast<uint32_t>(k) * 10000;  // independent of the sweep draw

        harness::MonteCarloRunResult result = harness::run_monte_carlo(candidate, /*show_progress=*/false);
        harness::ArmSummary sor = harness::summarize(result.sor_rows);
        harness::ArmSummary naive = harness::summarize(result.naive_rows);

        bool valid = result.total_drops == 0 && sor.mean_fill_rate >= 0.5 && naive.mean_fill_rate >= 0.5;
        double score = objective(sor, naive);

        std::cout << "[ CALIBRATE ] revalidate #" << k << " sweep_score=" << valid_samples[k].score
                   << " revalidated_score=" << (valid ? std::to_string(score) : std::string("REJECTED")) << "\n";

        if (valid && score > best_revalidated_score) {
            best_revalidated_score = score;
            best_config = candidate;
            found_valid_revalidated = true;
        }
    }

    if (!found_valid_revalidated) {
        std::cerr << "[ CALIBRATE ] none of the top sweep candidates survived re-validation - "
                     "the sweep did not find a genuine improvement over defaults.\n";
        return 1;
    }

    std::cout << "\n[ CALIBRATE ] running full-rigor (25-trial) before/after confirmatory comparison...\n";

    SimConfig confirm_default = base;  // already num_trials=25, seed_base=1000
    SimConfig confirm_best = best_config;
    confirm_best.harness.num_trials = 25;
    confirm_best.harness.seed_base = 1000;

    harness::MonteCarloRunResult before_result = harness::run_monte_carlo(confirm_default);
    harness::ArmSummary before_sor = harness::summarize(before_result.sor_rows);
    harness::ArmSummary before_naive = harness::summarize(before_result.naive_rows);

    harness::MonteCarloRunResult after_result = harness::run_monte_carlo(confirm_best);
    harness::ArmSummary after_sor = harness::summarize(after_result.sor_rows);
    harness::ArmSummary after_naive = harness::summarize(after_result.naive_rows);

    if (before_result.total_drops > 0 || after_result.total_drops > 0) {
        std::cerr << "[ WARN ] confirmatory run saw drops (before=" << before_result.total_drops
                  << ", after=" << after_result.total_drops << ") - treat these numbers cautiously.\n";
    }

    std::cout << "\n=== BEFORE: current defaults ===";
    harness::print_comparison(before_sor, before_naive, confirm_default);
    std::cout << "\n=== AFTER: calibrated config ===";
    harness::print_comparison(after_sor, after_naive, confirm_best);

    double edge_before = objective(before_sor, before_naive);
    double edge_after = objective(after_sor, after_naive);
    std::cout << "\nEdge score (2*shortfall_edge + slippage_edge + 50*fillrate_edge):\n"
              << "  before = " << edge_before << "\n"
              << "  after  = " << edge_after << "\n"
              << "  improvement = " << (edge_after - edge_before) << "\n";

    std::string before_path = "results/calibration_confirmatory_before_" + harness::timestamp_string() + ".csv";
    harness::export_csv(before_result.sor_rows, before_result.naive_rows, before_path);
    std::string after_path = "results/calibration_confirmatory_after_" + harness::timestamp_string() + ".csv";
    harness::export_csv(after_result.sor_rows, after_result.naive_rows, after_path);
    std::cout << "[ CALIBRATE ] wrote " << before_path << " and " << after_path << "\n";

    print_config_literal(best_config);

    return 0;
}
