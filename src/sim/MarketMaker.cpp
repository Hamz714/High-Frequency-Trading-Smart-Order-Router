#include "sim/MarketMaker.h"

MarketMaker::MarketMaker(Venue* venue, const MarketMakerConfig& cfg, uint32_t seed):
    target_venue(venue), config(cfg), rng(seed) {

    double m = static_cast<double>(config.quantity_mean);
    double v = config.quantity_variance;

    double sigma_sq = std::log(1.0 + (v / (m * m)));
    double mu = std::log(m) - (sigma_sq / 2.0);
    double sigma = std::sqrt(sigma_sq);

    size_distribution = std::lognormal_distribution<double>(mu, sigma);
    
    active_bids.reserve(config.max_orders_per_side);
    active_asks.reserve(config.max_orders_per_side);
    }