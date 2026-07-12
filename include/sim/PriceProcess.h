#pragma once

#include <random>
#include <cstdint>
#include <cmath>

class PriceProcess {
private:
    double current_price;
    double drift;
    double volatility;
    double dt;
    
    std::mt19937 rng;
    std::normal_distribution<double> standard_normal;

public:
    PriceProcess(double initial_price, double drift_rate, double vol, double time_step, uint32_t seed = std::random_device{}());

    double step();

    double get_current_price() const;
    double get_volatility() const;
};