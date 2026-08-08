#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/Types.h"
#include "config/SimConfig.h"

namespace harness {

struct ClientOrderSpec {
    int order_index;
    int delay_ms;
    Side side;
    int64_t price;      // aggressive limit price, guaranteed to sweep available liquidity
    int64_t quantity;
};

std::vector<ClientOrderSpec> generate_order_flow(uint32_t trial_seed, const SimConfig& config);

struct TrialRow {
    int trial;
    RoutingStrategy arm;
    OrderID order_id;
    Side side;
    int64_t intended_size;
    int64_t filled_size;
    double fill_rate;
    double decision_price;
    double avg_fill_price;
    double implementation_shortfall;
    double vwap_slippage;
    bool timed_out;
    double tick_to_trade_us;  // wall-clock latency, submit_order() -> ExecutionReport
    double fees_paid;
};

struct SubmitKey {
    int trial;
    RoutingStrategy arm;
    OrderID order_id;
};

bool operator==(const SubmitKey& a, const SubmitKey& b);

struct SubmitKeyHash {
    size_t operator()(const SubmitKey& key) const noexcept;
};

class ResultsCollector {
public:
    std::mutex mtx;
    std::vector<TrialRow> rows;

    void record_submit(int trial, RoutingStrategy arm, OrderID id);

    bool wait_for_trial_reports(int trial, RoutingStrategy arm, std::chrono::milliseconds timeout);

    void record_report(int trial, RoutingStrategy arm, const ExecutionReport& report,
                        const std::unordered_map<VenueID, double>& fee_schedule);

private:
    std::unordered_map<SubmitKey, std::chrono::steady_clock::time_point, SubmitKeyHash> submit_times_;
};

struct TrialOutcome {
    uint64_t total_drops = 0;
    bool completed_without_timeout = true;
};

TrialOutcome run_trial(int trial, uint32_t trial_seed, RoutingStrategy arm,
                        const std::vector<ClientOrderSpec>& script,
                        const SimConfig& config, ResultsCollector& collector);

struct ArmSummary {
    int sample_count = 0;
    double mean_shortfall_bps = 0.0;
    double mean_slippage_bps = 0.0;
    double mean_fill_rate = 0.0;
    double total_fees = 0.0;
    double p50_latency_us = 0.0;
    double p95_latency_us = 0.0;
    double p99_latency_us = 0.0;
};

ArmSummary summarize(const std::vector<TrialRow>& rows);

using ArmSummaries = std::array<ArmSummary, NUM_ROUTING_STRATEGIES>;

struct ValidityGate {
    uint64_t max_allowed_drops = 0;
    double min_fill_rate = 0.5;
};

bool passes_validity_gate(const std::vector<RoutingStrategy>& arms, const ArmSummaries& summaries,
                           uint64_t total_drops, const ValidityGate& gate);

struct MonteCarloRunResult {
    std::array<std::vector<TrialRow>, NUM_ROUTING_STRATEGIES> arm_rows;
    uint64_t total_drops = 0;
    int timed_out_trial_arms = 0;

    const std::vector<TrialRow>& rows(RoutingStrategy arm) const {
        return arm_rows[static_cast<size_t>(arm)];
    }
};

MonteCarloRunResult run_monte_carlo(const SimConfig& config, bool show_progress = true);

ArmSummaries summarize_all(const MonteCarloRunResult& result);

void print_comparison(const std::vector<RoutingStrategy>& arms, const ArmSummaries& summaries,
                       const SimConfig& config);
void export_csv(const MonteCarloRunResult& result, const std::string& path);
std::string timestamp_string();

}  // namespace harness
