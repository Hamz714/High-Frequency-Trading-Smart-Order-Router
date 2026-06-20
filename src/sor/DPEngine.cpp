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