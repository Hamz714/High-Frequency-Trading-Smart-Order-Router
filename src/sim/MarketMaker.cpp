#include "sim/MarketMaker.h"

MarketMaker::MarketMaker(Venue* venue, const MarketMakerConfig& cfg, uint32_t seed):
    target_venue(venue), config(cfg),
    mm_fill_queue(std::make_unique<SPSCQueue<FillEvent, QUEUE_SIZE>>()), rng(seed) {

    double m = static_cast<double>(config.quantity_mean);
    double v = config.quantity_variance;

    double sigma_sq = std::log(1.0 + (v / (m * m)));
    double mu = std::log(m) - (sigma_sq / 2.0);
    double sigma = std::sqrt(sigma_sq);

    size_distribution = std::lognormal_distribution<double>(mu, sigma);

    active_bids.reserve(config.max_orders_per_side);
    active_asks.reserve(config.max_orders_per_side);
    
    target_venue->set_mm_fill_queue(mm_fill_queue.get());
    }

int64_t MarketMaker::generate_random_quantity() {
    double raw_size = size_distribution(rng);
    
    int64_t quantity = static_cast<int64_t>(std::round(raw_size));
    
    return std::max<int64_t>(1, quantity);
}

void MarketMaker::post_limit_order(Side side, int64_t price, int64_t quantity) {
    OrderID new_id = next_mm_id.fetch_add(1, std::memory_order_relaxed);

    OrderRequest req;
    req.order_id = new_id;
    req.sender_type = SenderType::MM;
    req.request_type = RequestType::ORDER;
    req.side = side;                 
    req.order_type = OrderType::LIMIT;
    req.price = price;                
    req.quantity = quantity;           

    if (side == Side::BUY) {          
        active_bids[new_id] = req;
    } else {
        active_asks[new_id] = req;
    }

    target_venue->route_order(req);
}

void MarketMaker::cancel_stale_orders(Side side, int64_t current_fair_value) {
    auto& active_orders = (side == Side::BUY) ? active_bids : active_asks;

    for (auto it = active_orders.begin(); it != active_orders.end(); ) {
        const OrderRequest& req = it->second;
        int64_t distance = std::abs(req.price - current_fair_value);

        if (distance > config.stale_distance_ticks) {
            if (cancel_order(req.order_id)) {
                it = active_orders.erase(it);
            } else {
                ++it; 
            }
        } else {
            ++it;
        }
    }
}

bool MarketMaker::cancel_order(const OrderID& mm_id) {
    auto it = mm_id_to_lob_id.find(mm_id);
    if (it != mm_id_to_lob_id.end()) {
        OrderID lob_id = it->second;
        
        OrderRequest cancel_req;
        cancel_req.order_id = lob_id;
        cancel_req.sender_type = SenderType::MM;
        cancel_req.request_type = RequestType::CANCEL;
        
        target_venue->route_order(cancel_req);
        mm_id_to_lob_id.erase(it);
        return true; 
    }
    return false;
}

void MarketMaker::process_fills() {
    FillEvent fill;
    while (mm_fill_queue->try_pop(fill)) {
        auto bid_it = active_bids.find(fill.child_id);
        auto ask_it = active_asks.find(fill.child_id);
        
        if (bid_it != active_bids.end()) {
            if (fill.filled_quantity == 0 && fill.lob_order_id != -1) {
                mm_id_to_lob_id[fill.child_id] = fill.lob_order_id;
            } else if (fill.status == OrderStatus::FILLED) {
                mm_id_to_lob_id.erase(fill.child_id);
                active_bids.erase(bid_it);
            }
        } else if (ask_it != active_asks.end()) {
            if (fill.filled_quantity == 0 && fill.lob_order_id != -1) {
                mm_id_to_lob_id[fill.child_id] = fill.lob_order_id;
            } else if (fill.status == OrderStatus::FILLED) {
                mm_id_to_lob_id.erase(fill.child_id);
                active_asks.erase(ask_it);
            }
        }
    }
}

double MarketMaker::generate_gaussian_noise() {
    std::uniform_real_distribution<double> uniform(-1.0, 1.0);
    double u, v, s;
    
    do {
        u = uniform(rng);
        v = uniform(rng);
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    
    return u * std::sqrt(-2.0 * std::log(s) / s);
}

double MarketMaker::calculate_half_spread(int64_t fair_value_ticks, double volatility) const {
    double vol_ticks = fair_value_ticks * volatility;
    return (config.base_spread / 2.0) + (vol_ticks * config.spread_sensitivity);
}

void MarketMaker::replenish_side(Side side, int64_t fair_value_ticks, double volatility, double half_spread) {
    auto& active_orders = (side == Side::BUY) ? active_bids : active_asks;

    double vol_ticks = fair_value_ticks * volatility;
    double dispersion = (config.stale_distance_ticks / 2.0) + vol_ticks * config.spread_sensitivity;

    while (active_orders.size() < config.max_orders_per_side) {
        double noise = generate_gaussian_noise() * dispersion;
        int64_t target_price;
        
        if (side == Side::BUY) {
            target_price = static_cast<int64_t>(std::round(fair_value_ticks - half_spread + noise));
            target_price = std::min(target_price, fair_value_ticks - 1);
        } else {
            target_price = static_cast<int64_t>(std::round(fair_value_ticks + half_spread + noise));
            target_price = std::max(target_price, fair_value_ticks + 1);
        }
        
        post_limit_order(side, target_price, generate_random_quantity());
    }
}

void MarketMaker::update(double fair_value, double volatility) {
    process_fills(); 

    int64_t fair_value_ticks = static_cast<int64_t>(std::round(fair_value));
    cancel_stale_orders(Side::BUY, fair_value_ticks);
    cancel_stale_orders(Side::SELL, fair_value_ticks);

    double half_spread = calculate_half_spread(fair_value_ticks, volatility);

    replenish_side(Side::BUY, fair_value_ticks, volatility, half_spread);
    replenish_side(Side::SELL, fair_value_ticks, volatility, half_spread);
}