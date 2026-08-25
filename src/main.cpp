#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "config/SimConfig.h"
#include "harness/MonteCarloHarness.h"

int main(int argc, char** argv) {
    SimConfig config = default_sim_config();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0 || std::strcmp(argv[i], "-v") == 0) {
            config.verbose_reports = true;
        } else if (std::strcmp(argv[i], "--no-latency") == 0) {
            config.market.simulate_latency = false;
        } else if (std::strcmp(argv[i], "--trials") == 0 && i + 1 < argc) {
            int trials = std::atoi(argv[++i]);
            if (trials > 0) config.harness.num_trials = trials;
        } else if (std::strcmp(argv[i], "--no-proportional") == 0) {
            config.harness.arms = { RoutingStrategy::DP_OPTIMAL, RoutingStrategy::NAIVE };
        } else if (std::strcmp(argv[i], "--lot-size") == 0 && i + 1 < argc) {
            int64_t lot_size = std::atoll(argv[++i]);
            if (lot_size > 0) config.router.lot_size = lot_size;
        } else if (std::strcmp(argv[i], "--seed-base") == 0 && i + 1 < argc) {
            int seed_base = std::atoi(argv[++i]);
            if (seed_base > 0) config.harness.seed_base = static_cast<uint32_t>(seed_base);
        }
    }

    std::cout << "[ MAIN ] Monte Carlo routing comparison: " << config.harness.num_trials
              << " trials x " << config.harness.orders_per_trial << " orders/trial x "
              << config.harness.arms.size() << " arms\n";
    std::cout << "[ MAIN ] venue latency simulation: "
              << (config.market.simulate_latency ? "on" : "off (--no-latency)") << "\n";

    harness::MonteCarloRunResult result = harness::run_monte_carlo(config);

    if (result.total_drops > 0) {
        std::cerr << "[ WARN ] " << result.total_drops << " message(s) were silently dropped by full "
                     "queues during this run - metrics may be biased. Consider increasing "
                     "config.queues or reducing load.\n";
    }
    if (result.timed_out_trial_arms > 0) {
        std::cerr << "[ WARN ] " << result.timed_out_trial_arms << " trial/arm(s) timed out waiting "
                     "for order reports - some orders are missing from the results below.\n";
    }

    harness::ArmSummaries summaries = harness::summarize_all(result);

    harness::print_comparison(config.harness.arms, summaries, config);

    std::filesystem::create_directories("results");
    std::string suffix = config.market.simulate_latency ? "" : "nolatency_";
    std::string path = "results/sor_vs_baselines_" + suffix + harness::timestamp_string() + ".csv";
    harness::export_csv(result, path);
    std::cout << "[ MAIN ] wrote per-order results to " << path << "\n";

    return 0;
}
