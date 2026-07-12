#include "sim/PriceProcess.h"

PriceProcess::PriceProcess(double initial_price, double drift_rate, double vol, double time_step, uint32_t seed)
    : current_price(initial_price), 
      drift(drift_rate), 
      volatility(vol), 
      dt(time_step),
      rng(seed),
      standard_normal(0.0, 1.0) {}

double PriceProcess::step() {
    double z = standard_normal(rng);
    
    double drift_component = (drift - 0.5 * volatility * volatility) * dt;
    double stochastic_component = volatility * std::sqrt(dt) * z;
    
    current_price *= std::exp(drift_component + stochastic_component);
    
    return current_price;
}

double PriceProcess::get_current_price() const {
    return current_price;
}

double PriceProcess::get_volatility() const {
    return volatility;
}