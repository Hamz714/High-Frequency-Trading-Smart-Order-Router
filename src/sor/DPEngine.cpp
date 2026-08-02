#include "sor/DPEngine.h"

DPEngine::DPEngine(const RouterConfig& cfg):
    config(cfg) {}

double DPEngine::calculate_lit_cost(const VenueState& venue, int64_t quantity, int64_t visible_liquidity) const {
    if (quantity == 0) return 0.0;

    if (visible_liquidity == 0) {return std::numeric_limits<double>::max();}

    double spread_cost = venue.half_spread() * quantity;
    double impact_cost = venue.config.impact_coefficient * (quantity*quantity) / visible_liquidity;
    double fee_cost = venue.config.fee_per_share * quantity;
    double latency_cost = venue.config.latency_us * config.latency_cost_factor;

    return spread_cost + impact_cost + fee_cost + latency_cost;
}

double DPEngine::calculate_dark_cost(const VenueState& venue, int64_t quantity, std::span<const double> lit_dp_table) const {
    if (quantity == 0) return 0.0;

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

double DPEngine::calculate_miss_penalty(int64_t unfilled_quantity, std::span<const double> lit_dp_table) const {
    if (unfilled_quantity <= 0) return 0.0;

    int64_t index = unfilled_quantity / config.lot_size;

    double fallback_lit_cost = lit_dp_table[index];
    double delay_penalty = config.latency_cost_factor * unfilled_quantity;

    return fallback_lit_cost + delay_penalty;
}

SplitResult DPEngine::compute_optimal_split(int64_t total_size, Side side, int64_t worst_price, const std::vector<VenueState>& venues) {
    int64_t num_lots = total_size / config.lot_size;
    int64_t W = num_lots + 1;
    int64_t V = static_cast<int64_t>(venues.size());

    std::vector<std::pair<const VenueState*, int>> lit_venues;
    std::vector<std::pair<const VenueState*, int>> dark_venues;

    for (size_t i = 0; i < venues.size(); ++i) {
        if (venues[i].config.type == LIT) {lit_venues.push_back({&venues[i], i});} else {dark_venues.push_back({&venues[i], i});}
    }

    const double INF = std::numeric_limits<double>::max();

    std::vector<double> dp_table(static_cast<size_t>(V + 1) * W, INF);
    std::vector<int64_t> choice_table(static_cast<size_t>(V + 1) * W, 0);

    dp_table[0] = 0.0;

    std::vector<double> prev_reversed(W);
    std::vector<double> cost_for_x(W);

    for (int k = 1; k <= (int)lit_venues.size(); ++k) {
        const VenueState* venue = lit_venues[k-1].first;
        int64_t visible_liquidity = venue->get_visible_liquidity(side, worst_price);

        for (int64_t x = 0; x < W; ++x) {
            cost_for_x[x] = calculate_lit_cost(*venue, x * config.lot_size, visible_liquidity);
        }

        const double* prev_row = &dp_table[static_cast<size_t>(k - 1) * W];
        for (int64_t i = 0; i < W; ++i) prev_reversed[i] = prev_row[W - 1 - i];

        double* cur_row = &dp_table[static_cast<size_t>(k) * W];
        int64_t* cur_choice = &choice_table[static_cast<size_t>(k) * W];

        for (int64_t n = 0; n < W; ++n) {
            double min_cost = INF;
            int64_t best_x = 0;
            const double* prev_slice = &prev_reversed[W - 1 - n];

            for (int64_t x = 0; x <= n; ++x) {
                double prev_val = prev_slice[x];
                if (prev_val != INF) {
                    double total_cost = cost_for_x[x] + prev_val;
                    if (total_cost < min_cost) {
                        min_cost = total_cost;
                        best_x = x;
                    }
                }
            }
            cur_row[n] = min_cost;
            cur_choice[n] = best_x;
        }
    }

    std::span<const double> lit_dp_table(&dp_table[static_cast<size_t>(lit_venues.size()) * W], W);

    for (int k = (int)lit_venues.size() + 1; k <= V; ++k) {
        const VenueState* venue = dark_venues[k - lit_venues.size() - 1].first;

        for (int64_t x = 0; x < W; ++x) {
            cost_for_x[x] = calculate_dark_cost(*venue, x * config.lot_size, lit_dp_table);
        }

        const double* prev_row = &dp_table[static_cast<size_t>(k - 1) * W];
        for (int64_t i = 0; i < W; ++i) prev_reversed[i] = prev_row[W - 1 - i];

        double* cur_row = &dp_table[static_cast<size_t>(k) * W];
        int64_t* cur_choice = &choice_table[static_cast<size_t>(k) * W];

        for (int64_t n = 0; n < W; ++n) {
            double min_cost = INF;
            int64_t best_x = 0;
            const double* prev_slice = &prev_reversed[W - 1 - n];

            for (int64_t x = 0; x <= n; ++x) {
                double prev_val = prev_slice[x];
                if (prev_val != INF) {
                    double total_cost = cost_for_x[x] + prev_val;
                    if (total_cost < min_cost) {
                        min_cost = total_cost;
                        best_x = x;
                    }
                }
            }
            cur_row[n] = min_cost;
            cur_choice[n] = best_x;
        }
    }

    SplitResult result;
    result.expected_cost = dp_table[static_cast<size_t>(V) * W + num_lots];
    result.allocations.assign(venues.size(), 0);
    int64_t remaining_lots = num_lots;

    for (int k = V; k >= 1; --k) {
        int64_t lots_to_send = choice_table[static_cast<size_t>(k) * W + remaining_lots];

        int index = (k <= static_cast<int>(lit_venues.size()))
                        ? lit_venues[k - 1].second
                        : dark_venues[k - lit_venues.size() - 1].second;

        result.allocations[index] = lots_to_send * config.lot_size;
        remaining_lots -= lots_to_send;
    }

    int64_t remainder = total_size % config.lot_size;
    if (remainder > 0 && !result.allocations.empty()) {
        double best_cost = INF;
        size_t best_index = 0;
        bool found_viable_venue = false;

        for (const auto& [venue, index] : lit_venues) {
            int64_t visible_liquidity = venue->get_visible_liquidity(side, worst_price);
            double cost = calculate_lit_cost(*venue, remainder, visible_liquidity);
            if (cost < best_cost) {
                best_cost = cost;
                best_index = index;
                found_viable_venue = true;
            }
        }

        for (const auto& [venue, index] : dark_venues) {
            double cost = calculate_dark_cost(*venue, remainder, lit_dp_table);
            if (cost < best_cost) {
                best_cost = cost;
                best_index = index;
                found_viable_venue = true;
            }
        }

        if (found_viable_venue) {
            result.allocations[best_index] += remainder;
        }
    }

    return result;
}

SplitResult DPEngine::compute_naive_split(int64_t total_size, Side side, int64_t worst_price, const std::vector<VenueState>& venues) {
    SplitResult result;
    result.allocations.assign(venues.size(), 0);
    result.expected_cost = std::numeric_limits<double>::max();

    int best_index = -1;
    int64_t best_price = (side == BUY) ? std::numeric_limits<int64_t>::max() : 0;

    for (size_t i = 0; i < venues.size(); ++i) {
        if (venues[i].config.type != LIT) continue;

        if (side == BUY) {
            int64_t ask = venues[i].get_best_ask();
            if (ask < best_price) {
                best_price = ask;
                best_index = i;
            }
        } else {
            int64_t bid = venues[i].get_best_bid();
            if (bid > best_price) {
                best_price = bid;
                best_index = i;
            }
        }
    }

    if (best_index == -1) return result;

    result.allocations[best_index] = total_size;
    int64_t visible_liquidity = venues[best_index].get_visible_liquidity(side, worst_price);
    result.expected_cost = calculate_lit_cost(venues[best_index], total_size, visible_liquidity);

    return result;
}