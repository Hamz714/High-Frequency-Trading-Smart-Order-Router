#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <iomanip>

#include "common/Types.h"
#include "sor/SmartOrderRouter.h"
#include "lob/Venue.h"
#include "sim/SimulationEngine.h"
#include "sim/PriceProcess.h"

// Helper function to format terminal output cleanly
void log_event(const std::string& component, const std::string& message) {
    std::cout << "[ " << std::left << std::setw(15) << component << "] " << message << "\n";
}

int main() {
    log_event("MAIN", "Initializing Quantitative SOR Simulation Engine...");

    // ==========================================
    // 1. CONFIGURATION SETUP
    // ==========================================
    
    RouterConfig sor_cfg{100, 2, 0.05};
    VenueConfig lit_a_cfg{VenueType::LIT, 0.0010, 500, 0.01, 1.0};
    VenueConfig lit_b_cfg{VenueType::LIT, -0.0005, 1200, 0.015, 1.0};
    VenueConfig dark_c_cfg{VenueType::DARK, 0.0, 2500, 0.0, 0.15};

    // ==========================================
    // 2. COMPONENT INSTANTIATION
    // ==========================================
    
    log_event("MAIN", "Allocating hardware threads and lock-free queues...");
    
    SmartOrderRouter sor(sor_cfg);
    Venue venue_a(1, lit_a_cfg);
    Venue venue_b(2, lit_b_cfg);
    Venue venue_c(3, dark_c_cfg);
    
    sor.add_venue(&venue_a);
    sor.add_venue(&venue_b);
    sor.add_venue(&venue_c);

    auto pp = std::make_unique<PriceProcess>(10000.0, 0.0, 0.002, 0.001);
    SimulationEngine sim(std::move(pp));

    // Lit Market Makers (Capturing the spread)
    MarketMakerConfig mm_cfg_a{2, 1.5, 500, 50.0, 5, 10};
    MarketMakerConfig mm_cfg_b{4, 2.0, 300, 30.0, 5, 15};
    sim.add_market_maker(&venue_a, mm_cfg_a);
    sim.add_market_maker(&venue_b, mm_cfg_b);

    // Dark Pool "Market Maker" (Simulating Institutional Block Liquidity)
    // Spread = 0 (pegged to midpoint), huge quantity, slow refresh tick
    MarketMakerConfig mm_cfg_dark{0, 0.0, 5000, 500.0, 2, 50};
    sim.add_market_maker(&venue_c, mm_cfg_dark);
    
    NoiseTraderConfig nt_cfg{10.0, 4.0, 1.0, 0.5};
    sim.add_noise_trader(&venue_a, nt_cfg);
    sim.add_noise_trader(&venue_b, nt_cfg);
    sim.add_noise_trader(&venue_c, nt_cfg); 

    // ==========================================
    // 3. START THREADS
    // ==========================================
    
    log_event("MAIN", "Spinning up execution threads...");
    
    venue_a.start();
    venue_b.start();
    venue_c.start();
    sor.start();
    
    // Start simulation engine internally with dt = 1 millisecond
    sim.start(0.001);

    // ==========================================
    // 4. THE WARM-UP PHASE
    // ==========================================
    
    log_event("MAIN", "Letting the market warm up (populating order books)...");
    // Allow the simulation to run untouched for 3 wall-clock seconds. 
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // ==========================================
    // 5. THE TEST ORDER
    // ==========================================
    
    log_event("CLIENT", "Submitting massive parent order: BUY 50,000 shares @ 102.00 IOC");
    
    OrderRequest client_order;
    client_order.order_id = 999999;
    client_order.sender_type = SenderType::SOR;
    client_order.request_type = RequestType::ORDER;
    client_order.side = Side::BUY;
    client_order.order_type = OrderType::IOC; 
    client_order.price = 10200; // Aggressively crosses the spread
    client_order.quantity = 50000; 
    
    sor.submit_order(client_order);

    // Give the system time to run the DP engine, route child orders, and print the fills
    std::this_thread::sleep_for(std::chrono::seconds(2)); 

    // ==========================================
    // 6. TEARDOWN
    // ==========================================
    
    log_event("MAIN", "Simulation complete. Objects going out of scope...");

    return 0; // Destructors automatically trigger stop() logic and thread joins here.
}