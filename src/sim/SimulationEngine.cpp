#include "sim/SimulationEngine.h"

#include <cmath>

SimulationEngine::SimulationEngine(std::unique_ptr<PriceProcess> pp, SimClock* clock_in)
    : price_process(std::move(pp)), clock(clock_in) {}

SimulationEngine::~SimulationEngine() {
    stop();
}

void SimulationEngine::add_market_maker(Venue* venue, const MarketMakerConfig& cfg, uint32_t seed,
                                         size_t fill_queue_capacity) {
    market_makers.push_back(std::make_unique<MarketMaker>(venue, cfg, seed, fill_queue_capacity));
}

void SimulationEngine::add_noise_trader(Venue* venue, const NoiseTraderConfig& cfg, uint32_t seed) {
    noise_traders.push_back(std::make_unique<NoiseTrader>(
        venue,
        cfg,
        price_process->get_current_price(),
        seed
    ));
}

void SimulationEngine::start(double dt) {
    if (running.exchange(true)) return;
    worker_thread = std::thread(&SimulationEngine::worker_loop, this, dt);
}

void SimulationEngine::worker_loop(double dt) {
    while (running.load(std::memory_order_acquire)) {
        double current_fair_value = price_process->step();
        double per_tick_volatility = price_process->get_volatility() * std::sqrt(dt);

        for (auto& mm : market_makers) {
            mm->update(current_fair_value, per_tick_volatility);
        }

        for (auto& nt : noise_traders) {
            nt->update(dt, current_fair_value);
        }

        clock->advance(dt);
        std::this_thread::yield();
    }
}

uint64_t SimulationEngine::get_dropped_total() const {
    uint64_t total = 0;
    for (const auto& mm : market_makers) {
        total += mm->get_dropped_total();
    }
    return total;
}

void SimulationEngine::stop() {
    if (running.exchange(false, std::memory_order_release)) {
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }
}
