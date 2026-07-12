#include "sim/NoiseTrader.h"

NoiseTrader::NoiseTrader(Venue* venue, const NoiseTraderConfig& cfg, double initial_price, uint32_t seed)
    : target_venue(venue), 
      config(cfg),
      rng(seed),
      arrival_dist(cfg.arrival_rate),
      size_dist(cfg.size_mu, cfg.size_sigma),
      last_observed_price(initial_price) {
          
    time_to_next_order = arrival_dist(rng);
}

void NoiseTrader::update(double dt, double current_fair_value) {
    time_to_next_order -= dt;

    while (time_to_next_order <= 0.0) {
        execute_noise_order(current_fair_value);
        
        time_to_next_order += arrival_dist(rng);
    }
}

void NoiseTrader::execute_noise_order(double current_fair_value) {
    double log_return = std::log(current_fair_value / last_observed_price);
    last_observed_price = current_fair_value;

    double buy_prob = 0.5 + (log_return * config.trend_sensitivity);    
    buy_prob = std::max(0.0, std::min(1.0, buy_prob));

    std::bernoulli_distribution direction(buy_prob);
    
    Side side = direction(rng) ? BUY : SELL;
    
    int64_t quantity = static_cast<int64_t>(std::round(size_dist(rng)));
    quantity = std::max<int64_t>(1, quantity);

    OrderID new_id = next_noise_id.fetch_add(1, std::memory_order_relaxed);

    OrderRequest req;
    req.order_id = new_id;
    
    req.sender_type = SenderType::NOISE;
    req.request_type = RequestType::ORDER;
    req.side = side;    
    req.order_type = OrderType::MARKET;    
    req.quantity = quantity;

    target_venue->route_order(req);
}