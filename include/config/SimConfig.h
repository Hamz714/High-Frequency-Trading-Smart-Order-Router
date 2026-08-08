#pragma once

#include <cstdint>

#include "common/Types.h"

struct MarketScenarioConfig {
    VenueConfig lit_venue_1;
    VenueConfig lit_venue_2;
    VenueConfig dark_venue;

    MarketMakerConfig mm_lit_venue_1;
    MarketMakerConfig mm_lit_venue_2;
    MarketMakerConfig mm_dark_venue;

    NoiseTraderConfig noise_lit_venue_1;
    NoiseTraderConfig noise_lit_venue_2;  // no noise trader on the dark venue

    double reference_price;
    double price_drift;
    double price_volatility;
    double price_dt;

    bool simulate_latency;
};

struct RouterTuningConfig {
    int64_t lot_size;
    int64_t latency_cost_factor;
    double dark_pool_decay_rate;
    int max_reroute_attempts;
};

struct HarnessConfig {
    int num_trials;
    int orders_per_trial;
    int64_t min_order_qty;
    int64_t max_order_qty;
    int submit_delay_min_ms;
    int submit_delay_max_ms;
    int warmup_ms;
    int report_wait_timeout_s;
    uint32_t seed_base;
};

struct SimConfig {
    MarketScenarioConfig market;
    RouterTuningConfig router;
    HarnessConfig harness;
    bool verbose_reports = false;
};

SimConfig default_sim_config();
