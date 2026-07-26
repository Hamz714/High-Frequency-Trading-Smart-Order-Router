#include "config/SimConfig.h"

SimConfig default_sim_config() {
    SimConfig config;

    config.market.lit_venue_1 = VenueConfig{ .type = LIT, .fee_per_share = 0.0005, .latency_us = 50,
                                               .impact_coefficient = 4.0, .historical_fill_ratio = 0.0 };
    config.market.lit_venue_2 = VenueConfig{ .type = LIT, .fee_per_share = 0.0003, .latency_us = 90,
                                               .impact_coefficient = 3.0, .historical_fill_ratio = 0.0 };
    config.market.dark_venue = VenueConfig{ .type = DARK, .fee_per_share = 0.0001, .latency_us = 200,
                                              .impact_coefficient = 0.0, .historical_fill_ratio = 0.4 };

    config.market.mm_lit_venue_1 = MarketMakerConfig{ .base_spread = 4, .spread_sensitivity = 1.0,
                                                        .quantity_mean = 50, .quantity_variance = 4000.0,
                                                        .max_orders_per_side = 1000, .stale_distance_ticks = 60 };
    config.market.mm_lit_venue_2 = MarketMakerConfig{ .base_spread = 6, .spread_sensitivity = 0.8,
                                                        .quantity_mean = 40, .quantity_variance = 3000.0,
                                                        .max_orders_per_side = 1000, .stale_distance_ticks = 60 };
    config.market.mm_dark_venue = MarketMakerConfig{ .base_spread = 2, .spread_sensitivity = 0.6,
                                                       .quantity_mean = 30, .quantity_variance = 5000.0,
                                                       .max_orders_per_side = 1000, .stale_distance_ticks = 60 };

    config.market.noise_lit_venue_1 = NoiseTraderConfig{ .arrival_rate = 80.0, .size_mu = 4.0,
                                                           .size_sigma = 0.4, .trend_sensitivity = 3.0 };
    config.market.noise_lit_venue_2 = NoiseTraderConfig{ .arrival_rate = 60.0, .size_mu = 3.8,
                                                           .size_sigma = 0.4, .trend_sensitivity = 3.0 };

    config.market.reference_price = 10'000.0;
    config.market.price_drift = 0.0;
    config.market.price_volatility = 0.02;
    config.market.price_dt = 0.001;

    config.router.lot_size = 10;
    config.router.latency_cost_factor = 1;
    config.router.dark_pool_decay_rate = 0.0012;
    config.router.max_reroute_attempts = 3;

    config.harness.num_trials = 25;
    config.harness.orders_per_trial = 6;
    config.harness.min_order_qty = 500;
    config.harness.max_order_qty = 10'000;
    config.harness.submit_delay_min_ms = 50;
    config.harness.submit_delay_max_ms = 200;
    config.harness.warmup_ms = 100;
    config.harness.report_wait_timeout_s = 3;
    config.harness.seed_base = 1000;

    config.verbose_reports = false;

    return config;
}
