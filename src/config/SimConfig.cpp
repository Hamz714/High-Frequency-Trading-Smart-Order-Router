#include "config/SimConfig.h"

SimConfig default_sim_config() {
    SimConfig config;

    config.market.lit_venue_1 = VenueConfig{ .type = VenueType::LIT, .fee_per_share = 0.0004, .latency_us = 200,
                                               .impact_coefficient = 6.8, .historical_fill_ratio = 0.0 };
    config.market.lit_venue_2 = VenueConfig{ .type = VenueType::LIT, .fee_per_share = 0.0009, .latency_us = 45,
                                               .impact_coefficient = 4.8, .historical_fill_ratio = 0.0 };
    config.market.dark_venue = VenueConfig{ .type = VenueType::DARK, .fee_per_share = 0.0001, .latency_us = 75,
                                              .impact_coefficient = 0.0, .historical_fill_ratio = 0.70 };

    config.market.mm_lit_venue_1 = MarketMakerConfig{ .base_spread = 10, .spread_sensitivity = 0.88,
                                                        .quantity_mean = 50, .quantity_variance = 9000.0,
                                                        .max_orders_per_side = 1000, .stale_distance_ticks = 150 };
    config.market.mm_lit_venue_2 = MarketMakerConfig{ .base_spread = 1, .spread_sensitivity = 0.55,
                                                        .quantity_mean = 20, .quantity_variance = 3300.0,
                                                        .max_orders_per_side = 1000, .stale_distance_ticks = 140 };
    config.market.mm_dark_venue = MarketMakerConfig{ .base_spread = 11, .spread_sensitivity = 1.08,
                                                       .quantity_mean = 60, .quantity_variance = 7000.0,
                                                       .max_orders_per_side = 1000, .stale_distance_ticks = 40 };

    config.market.noise_lit_venue_1 = NoiseTraderConfig{ .arrival_rate = 12.0, .size_mu = 2.1,
                                                           .size_sigma = 0.31, .trend_sensitivity = 2.9 };
    config.market.noise_lit_venue_2 = NoiseTraderConfig{ .arrival_rate = 58.0, .size_mu = 4.6,
                                                           .size_sigma = 0.34, .trend_sensitivity = 3.8 };

    config.market.reference_price = 10'000.0;
    config.market.price_drift = -0.004;
    config.market.price_volatility = 0.018;
    config.market.price_dt = 0.001;
    config.market.simulate_latency = true;
    config.market.venue_order_pool_capacity = 65'536;

    config.router.lot_size = 27;
    config.router.latency_cost_factor = 3;
    config.router.dark_pool_decay_rate = 0.0001;
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
    config.harness.arms = { RoutingStrategy::DP_OPTIMAL, RoutingStrategy::PROPORTIONAL, RoutingStrategy::NAIVE };

    config.queues = QueueSizingConfig{ .order_inbox = 16'384, .market_data = 32'768, .fill = 16'384,
                                        .analytics_trade = 262'144, .analytics_order = 16'384 };

    config.verbose_reports = false;

    return config;
}
