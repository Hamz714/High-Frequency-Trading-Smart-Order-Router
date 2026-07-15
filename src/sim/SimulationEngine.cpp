#include "sim/SimulationEngine.h"

SimulationEngine::SimulationEngine(std::unique_ptr<PriceProcess> pp)
    : price_process(std::move(pp)) {}

SimulationEngine::~SimulationEngine() {
    stop();
}

void SimulationEngine::add_market_maker(Venue* venue, const MarketMakerConfig& cfg) {
    market_makers.push_back(std::make_unique<MarketMaker>(venue, cfg));
}

void SimulationEngine::add_noise_trader(Venue* venue, const NoiseTraderConfig& cfg) {
    noise_traders.push_back(std::make_unique<NoiseTrader>(
        venue, 
        cfg, 
        price_process->get_current_price()
    ));
}

void SimulationEngine::start(double dt) {
    if (running.exchange(true)) return;
    worker_thread = std::thread(&SimulationEngine::worker_loop, this, dt);
}

void SimulationEngine::worker_loop(double dt) {
    while (running.load(std::memory_order_acquire)) {
        double current_fair_value = price_process->step();
        double volatility = price_process->get_volatility();

        for (auto& mm : market_makers) {
            mm->update(current_fair_value, volatility);
        }

        for (auto& nt : noise_traders) {
            nt->update(dt, current_fair_value);
        }

        current_time += dt;
        std::this_thread::yield(); 
    }
}

void SimulationEngine::stop() {
    if (running.exchange(false, std::memory_order_release)) {
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }
}

double SimulationEngine::get_current_time() const {
    return current_time;
}