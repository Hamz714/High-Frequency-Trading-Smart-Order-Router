#pragma once

#include <cstdint>
#include <random>
#include <unordered_map>
#include <atomic>
#include <cmath>
#include <memory>

#include "common/Types.h"
#include "common/SPSCQueue.h"
#include "lob/Venue.h"

class MarketMaker {
private:
    Venue* target_venue;
    MarketMakerConfig config;
    
    std::unordered_map<OrderID, OrderRequest> active_bids;
    std::unordered_map<OrderID, OrderRequest> active_asks;
    
    std::unordered_map<OrderID, OrderID> mm_id_to_lob_id;
    
    std::unique_ptr<SPSCQueue<FillEvent>> mm_fill_queue;
    
    std::mt19937 rng;
    std::lognormal_distribution<double> size_distribution;

    std::atomic<OrderID> next_mm_id{100'000'000}; 

    int64_t generate_random_quantity();
    void post_limit_order(Side side, int64_t price, int64_t quantity);
    void cancel_stale_orders(Side side, int64_t current_fair_value);
    bool cancel_order(const OrderID& mm_id);
    void process_fills();

    double generate_gaussian_noise();
    double calculate_half_spread(int64_t fair_value_ticks, double volatility) const;
    void replenish_side(Side side, int64_t fair_value_ticks, double volatility, double half_spread);

public:
    MarketMaker(Venue* venue, const MarketMakerConfig& cfg, uint32_t seed = std::random_device{}(),
                size_t fill_queue_capacity = DEFAULT_QUEUE_CAPACITY);

    MarketMaker(const MarketMaker&) = delete;
    MarketMaker& operator=(const MarketMaker&) = delete;
    
    void update(double fair_value, double volatility);

    uint64_t get_dropped_total() const { return mm_fill_queue->dropped(); }
};