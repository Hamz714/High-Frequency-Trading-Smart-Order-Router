#pragma once
#include "common/Types.h"
#include "lob/Venue.h"

class DPEngine{
    RouterConfig config;

    double calculate_lit_cost(const VenueState& venue, Side side, int64_t quantity, int64_t worst_price) const;

    double calculate_dark_cost(const VenueState& venue, int64_t quantity, const std::vector<double>& lit_dp_table) const;

    double estimate_dark_fill_ratio(const VenueState& venue, int64_t quantity) const;

    double calculate_miss_penalty(int64_t unfilled_quantity, const std::vector<double>& lit_dp_table) const;

    public:
        DPEngine(const RouterConfig& cfg);

        SplitResult compute_optimal_split(int64_t total_size, Side side, int64_t worst_price, const std::vector<VenueState>& venues);
};