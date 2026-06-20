#pragma once
#include "common/Types.h"
#include "lob/Venue.h"

struct SplitResult {
    std::vector<int64_t> allocations;
    double expected_cost;
};

class DPEngine{
    int64_t lot_size;

    double calculate_lit_cost(const Venue* venue, Side side, int64_t quantity, int64_t worst_price) const;

    double calculate_dark_cost(const Venue* venue, Side side, int64_t quantity, int64_t worst_price) const;

    double estimate_dark_fill_probability(const Venue* venue, int64_t size) const;

    double calculate_miss_penalty(int64_t unfilled_size) const;

    public:
        explicit DPEngine(int64_t lot_size=100);

        SplitResult compute_optimal_split(int64_t total_size, Side side, int64_t worst_price, std::vector<Venue*>& venues);
};