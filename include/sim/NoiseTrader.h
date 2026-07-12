#pragma once

#include <random>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <algorithm>

#include "common/Types.h"
#include "lob/Venue.h"

class NoiseTrader {
private:
    Venue* target_venue;
    NoiseTraderConfig config;

    std::mt19937 rng;
    std::exponential_distribution<double> arrival_dist;
    std::lognormal_distribution<double> size_dist;

    double time_to_next_order;
    double last_observed_price;

    std::atomic<OrderID> next_noise_id{200'000'000}; 

    void execute_noise_order(double current_fair_value);

public:
    NoiseTrader(Venue* venue, const NoiseTraderConfig& cfg, double initial_price, uint32_t seed = std::random_device{}());

    void update(double dt, double current_fair_value);
};