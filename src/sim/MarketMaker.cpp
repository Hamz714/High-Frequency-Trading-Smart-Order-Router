#include "sim/MarketMaker.h"

MarketMaker::MarketMaker(Venue* venue, const MarketMakerConfig& cfg, uint32_t seed):
    target_venue(venue), config(cfg), rng(seed) {

    double m = static_cast<double>(config.quantity_mean);
    double v = config.quantity_variance;

    double sigma_sq = std::log(1.0 + (v / (m * m)));
    double mu = std::log(m) - (sigma_sq / 2.0);
    double sigma = std::sqrt(sigma_sq);

    size_distribution = std::lognormal_distribution<double>(mu, sigma);

    active_bids.reserve(config.max_orders_per_side);
    active_asks.reserve(config.max_orders_per_side);
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