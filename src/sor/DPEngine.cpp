#include "sor/DPEngine.h"

DPEngine::DPEngine(const RouterConfig& cfg):
    config(cfg) {}

double DPEngine::calculate_lit_cost(const Venue* venue, Side side, int64_t quantity, int64_t worst_price) const {
    const VenueConfig& venue_config = venue->get_config();
    int64_t visible_liquidity = venue->get_visible_liquidity(side, worst_price);

    if (visible_liquidity == 0) {return std::numeric_limits<double>::max();}

    double spread_cost = venue->half_spread() * quantity;
    double impact_cost = venue_config.impact_coefficient * (quantity*quantity) / visible_liquidity;
    double fee_cost = venue_config.fee_per_share * quantity;
    double latency_cost = venue_config.latency_us * config.latency_cost_factor;

    return spread_cost + impact_cost + fee_cost + latency_cost;
}

double DPEngine::calculate_dark_cost(const Venue* venue, Side side, int64_t quantity, const std::vector<int64_t>& lit_dp_table) const {

}

double DPEngine::estimate_dark_fill_ratio(const Venue* venue, int64_t quantity) const {
    const VenueConfig& venue_config = venue->get_config();
    double base_ratio = venue_config.historical_fill_ratio;

    double exponent = -config.dark_pool_decay_rate * quantity;
    return base_ratio * std::exp(exponent);
}

double DPEngine::calculate_miss_penalty(int64_t unfilled_quantity, const std::vector<int64_t>& lit_dp_table) const {
    if (unfilled_quantity <= 0) return 0.0;

    int64_t index = unfilled_quantity / config.lot_size;

    int64_t fallback_lit_cost = lit_dp_table[index];
    double delay_penalty = config.latency_cost_factor * unfilled_quantity;

    return fallback_lit_cost + delay_penalty;
}