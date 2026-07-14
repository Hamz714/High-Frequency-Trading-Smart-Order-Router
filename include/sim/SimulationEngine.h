#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <thread>

#include "sim/PriceProcess.h"
#include "sim/MarketMaker.h"
#include "sim/NoiseTrader.h"
#include "lob/Venue.h"

class SimulationEngine {
private:
    std::unique_ptr<PriceProcess> price_process;
    
    std::vector<std::unique_ptr<MarketMaker>> market_makers;
    std::vector<std::unique_ptr<NoiseTrader>> noise_traders;

    double current_time{0.0};
    std::atomic<bool> running{false};

public:
    SimulationEngine(std::unique_ptr<PriceProcess> pp);

    void add_market_maker(Venue* venue, const MarketMakerConfig& cfg);
    void add_noise_trader(Venue* venue, const NoiseTraderConfig& cfg);

    void run(double dt);
    
    void stop();
    
    double get_current_time() const;
};