#pragma once

#include <vector>
#include <memory>
#include <atomic>
#include <thread>

#include "sim/PriceProcess.h"
#include "sim/MarketMaker.h"
#include "sim/NoiseTrader.h"
#include "lob/Venue.h"
#include "common/SimClock.h"

class SimulationEngine {
private:
    std::unique_ptr<PriceProcess> price_process;

    std::vector<std::unique_ptr<MarketMaker>> market_makers;
    std::vector<std::unique_ptr<NoiseTrader>> noise_traders;

    SimClock* clock;
    std::atomic<bool> running{false};
    std::thread worker_thread;

    void worker_loop(double dt);

public:
    SimulationEngine(std::unique_ptr<PriceProcess> pp, SimClock* clock);
    ~SimulationEngine();

    void add_market_maker(Venue* venue, const MarketMakerConfig& cfg, uint32_t seed);
    void add_noise_trader(Venue* venue, const NoiseTraderConfig& cfg, uint32_t seed);

    void start(double dt);
    void stop();
};