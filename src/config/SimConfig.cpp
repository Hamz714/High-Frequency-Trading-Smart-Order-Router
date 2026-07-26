#include "config/SimConfig.h"

SimConfig default_sim_config() {
    SimConfig config;

    config.market.lit_venue_1 = VenueConfig{ .type = LIT, .fee_per_share = 0.0004265839, .latency_us = 202,
                                               .impact_coefficient = 6.8286654515, .historical_fill_ratio = 0.0 };
    config.market.lit_venue_2 = VenueConfig{ .type = LIT, .fee_per_share = 0.0009485735, .latency_us = 46,
                                               .impact_coefficient = 4.7917416000, .historical_fill_ratio = 0.0 };
    config.market.dark_venue = VenueConfig{ .type = DARK, .fee_per_share = 0.0000894663, .latency_us = 71,
                                              .impact_coefficient = 0.0, .historical_fill_ratio = 0.6989994222 };

    config.market.mm_lit_venue_1 = MarketMakerConfig{ .base_spread = 10, .spread_sensitivity = 0.8812188427,
                                                        .quantity_mean = 53, .quantity_variance = 9355.8095284739,
                                                        .max_orders_per_side = 1000, .stale_distance_ticks = 148 };
    config.market.mm_lit_venue_2 = MarketMakerConfig{ .base_spread = 1, .spread_sensitivity = 0.5517382395,
                                                        .quantity_mean = 21, .quantity_variance = 3343.0394462906,
                                                        .max_orders_per_side = 1000, .stale_distance_ticks = 138 };
    config.market.mm_dark_venue = MarketMakerConfig{ .base_spread = 11, .spread_sensitivity = 1.0799872399,
                                                       .quantity_mean = 58, .quantity_variance = 6921.5769478983,
                                                       .max_orders_per_side = 1000, .stale_distance_ticks = 40 };

    config.market.noise_lit_venue_1 = NoiseTraderConfig{ .arrival_rate = 12.1450868156, .size_mu = 2.0958266771,
                                                           .size_sigma = 0.3140317357, .trend_sensitivity = 2.8694167210 };
    config.market.noise_lit_venue_2 = NoiseTraderConfig{ .arrival_rate = 58.2657343948, .size_mu = 4.6301407139,
                                                           .size_sigma = 0.3418247850, .trend_sensitivity = 3.8248750478 };

    config.market.reference_price = 10'000.0;
    config.market.price_drift = -0.0040022139;
    config.market.price_volatility = 0.0179987903;
    config.market.price_dt = 0.001;

    config.router.lot_size = 27;
    config.router.latency_cost_factor = 3;
    config.router.dark_pool_decay_rate = 0.0001040522;
    config.router.max_reroute_attempts = 2;

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
