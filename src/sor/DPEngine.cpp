#include "sor/DPEngine.h"

DPEngine::DPEngine(const RouterConfig& cfg):
    config(cfg) {}

double DPEngine::calculate_lit_cost(const VenueState& venue, Side side, int64_t quantity, int64_t worst_price) const {;
    int64_t visible_liquidity = venue.get_visible_liquidity(side, worst_price);

    if (visible_liquidity == 0) {return std::numeric_limits<double>::max();}

    double spread_cost = venue.half_spread() * quantity;
    double impact_cost = venue.config.impact_coefficient * (quantity*quantity) / visible_liquidity;
    double fee_cost = venue.config.fee_per_share * quantity;
    double latency_cost = venue.config.latency_us * config.latency_cost_factor;

    return spread_cost + impact_cost + fee_cost + latency_cost;
}

double DPEngine::calculate_dark_cost(const VenueState& venue, int64_t quantity, const std::vector<double>& lit_dp_table) const {
    double p_fill = estimate_dark_fill_ratio(venue, quantity);

    double fee_cost = venue.config.fee_per_share * quantity;
    double latency_cost = venue.config.latency_us * config.latency_cost_factor;
    double cost_if_filled = fee_cost + latency_cost;

    double miss_penalty = calculate_miss_penalty(quantity, lit_dp_table);

    return (p_fill * cost_if_filled) + ((1.0 - p_fill) * miss_penalty);
}

double DPEngine::estimate_dark_fill_ratio(const VenueState& venue, int64_t quantity) const {
    double base_ratio = venue.config.historical_fill_ratio;

    double exponent = -config.dark_pool_decay_rate * quantity;
    return base_ratio * std::exp(exponent);
}

double DPEngine::calculate_miss_penalty(int64_t unfilled_quantity, const std::vector<double>& lit_dp_table) const {
    if (unfilled_quantity <= 0) return 0.0;

    int64_t index = unfilled_quantity / config.lot_size;

    double fallback_lit_cost = lit_dp_table[index];
    double delay_penalty = config.latency_cost_factor * unfilled_quantity;

    return fallback_lit_cost + delay_penalty;
}

SplitResult DPEngine::compute_optimal_split(int64_t total_size, Side side, int64_t worst_price, const std::vector<VenueState>& venues) {
    int64_t num_lots = total_size / config.lot_size;

    std::vector<std::pair<const VenueState*, int>> lit_venues;
    std::vector<std::pair<const VenueState*, int>> dark_venues;

    for (int i = 0; i < venues.size(); ++i) {
        if (venues[i].config.type == LIT) {lit_venues.push_back({&venues[i], i});} else {dark_venues.push_back({&venues[i], i});}
    }

    const double INF = std::numeric_limits<double>::max();
    std::vector<std::vector<double>> dp_table(venues.size() + 1, std::vector<double>(num_lots + 1, INF));
    std::vector<std::vector<int64_t>> choice_table(venues.size() + 1, std::vector<int64_t>(num_lots + 1, 0));

    dp_table[0][0] = 0.0;

    for (int k = 1; k <= lit_venues.size(); ++k) {
        const VenueState* venue = lit_venues[k-1].first;

        for (int n = 0; n <= num_lots; ++n) {
            double min_cost = INF;
            int64_t best_x = 0;

            for (int x = 0; x <= n; ++x) {
                double cost_k = calculate_lit_cost(*venue, side, x * config.lot_size, worst_price);

                if (dp_table[k - 1][n - x] != INF) {
                    double total_cost = cost_k + dp_table[k - 1][n - x];

                    if (total_cost < min_cost) {
                        min_cost = total_cost;
                        best_x = x;
                    }
                }
            }
            dp_table[k][n] = min_cost;
            choice_table[k][n] = best_x;
        }
    }

    const std::vector<double>& lit_dp_table = dp_table[lit_venues.size()];

    for (int k = lit_venues.size() + 1; k <= venues.size(); ++k) {
        const VenueState* venue = dark_venues[k - lit_venues.size() - 1].first;

        for (int n = 0; n <= num_lots; ++n) {
            double min_cost = INF;
            int64_t best_x = 0;

            for (int x = 0; x <= n; ++x) {
                double expected_dark_cost = calculate_dark_cost(*venue, x * config.lot_size, lit_dp_table);

                if (dp_table[k - 1][n - x] != INF) {
                    double total_cost = expected_dark_cost + dp_table[k - 1][n - x];

                    if (total_cost < min_cost) {
                        min_cost = total_cost;
                        best_x = x;
                    }
                }
            }
            dp_table[k][n] = min_cost;
            choice_table[k][n] = best_x;
        }
    }

    SplitResult result;
    result.expected_cost = dp_table[venues.size()][num_lots];
    result.allocations.assign(venues.size(), 0);
    int64_t remaining_lots = num_lots;

    for (int k = venues.size(); k >= 1; --k) {
        int64_t lots_to_send = choice_table[k][remaining_lots];

        int index = (k <= lit_venues.size())
                        ? lit_venues[k - 1].second
                        : dark_venues[k - lit_venues.size() - 1].second;

        result.allocations[index] = lots_to_send * config.lot_size;
        remaining_lots -= lots_to_send;
    }

    return result;
}