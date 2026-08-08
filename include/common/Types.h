#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#include <unordered_map>

#include "common/Constants.h"

using OrderID = int64_t;

using VenueID = int64_t;

enum class Side : uint8_t { BUY, SELL };

enum class OrderType : uint8_t { LIMIT, MARKET, IOC, FOK };

enum class SenderType : uint8_t { SOR, MM, NOISE };

enum class OrderStatus : uint8_t { PARTIAL, FILLED, CANCELLED };

enum class RequestType : uint8_t { ORDER, CANCEL };

struct Order {
    OrderID id;
    int64_t price;
    int64_t quantity;
    Side side;
    int32_t prev_index;
    int32_t next_index;
    bool is_active;
};

struct PriceLevel {
    int64_t price = 0;
    int64_t quantity = 0;
    int32_t head_order_index = -1;
    int32_t tail_order_index = -1;
};

struct SnapshotLevel {
    int64_t price;
    int64_t quantity;
};

struct BookSnapshot {
    std::vector<SnapshotLevel> bids;
    std::vector<SnapshotLevel> asks;
};


struct Fill {
    OrderID order_id;
    int64_t filled_quantity;
    int64_t fill_price;
};

struct FillEvent {
    OrderID child_id;
    OrderID lob_order_id;
    VenueID venue_id;
    int64_t filled_quantity;
    int64_t fill_price;
    int64_t remaining_quantity;
    OrderStatus status;
    int64_t visible_ns = 0;
};

enum class VenueType : uint8_t { LIT, DARK };

struct VenueConfig {
    VenueType type;
    double fee_per_share;
    int64_t latency_us;
    double impact_coefficient;
    double historical_fill_ratio;
};

struct SplitResult {
    std::vector<int64_t> allocations;
    double expected_cost;
};

struct QueueSizingConfig {
    size_t order_inbox;
    size_t market_data;
    size_t fill;
    size_t analytics_trade;
    size_t analytics_order;
};

inline QueueSizingConfig default_queue_sizing() {
    return QueueSizingConfig{ DEFAULT_QUEUE_CAPACITY, DEFAULT_QUEUE_CAPACITY,
                              DEFAULT_QUEUE_CAPACITY, DEFAULT_QUEUE_CAPACITY,
                              DEFAULT_QUEUE_CAPACITY };
}

enum class RoutingStrategy : uint8_t { DP_OPTIMAL, PROPORTIONAL, NAIVE };

inline constexpr size_t NUM_ROUTING_STRATEGIES = 3;

inline const char* routing_strategy_tag(RoutingStrategy strategy) {
    switch (strategy) {
        case RoutingStrategy::DP_OPTIMAL:   return "sor";
        case RoutingStrategy::PROPORTIONAL: return "proportional";
        case RoutingStrategy::NAIVE:        return "naive";
    }
    return "unknown";
}

inline const char* routing_strategy_label(RoutingStrategy strategy) {
    switch (strategy) {
        case RoutingStrategy::DP_OPTIMAL:   return "SOR (DP)";
        case RoutingStrategy::PROPORTIONAL: return "Proportional";
        case RoutingStrategy::NAIVE:        return "Naive";
    }
    return "Unknown";
}

struct RouterConfig {
    int64_t lot_size;
    int64_t latency_cost_factor;
    double dark_pool_decay_rate;
    RoutingStrategy strategy;
    int max_reroute_attempts;
};

struct BookDelta {
    VenueID venue_id;
    Side side;
    int64_t price;
    int64_t new_quantity;
    int64_t visible_ns = 0;
};

struct OrderRequest {
    OrderID order_id;
    SenderType sender_type;
    RequestType request_type;
    Side side;
    OrderType order_type;
    int64_t price;
    int64_t quantity;
    int64_t arrival_ns = 0;
};

struct ParentOrder {
    OrderID parent_id;
    Side side;
    int64_t price;
    int64_t total_qty;
    int64_t filled_qty;
    double decision_time;
    int reroute_count;
};

struct MarketMakerConfig {
    int64_t base_spread;
    double spread_sensitivity;
    int64_t quantity_mean;
    double quantity_variance;
    size_t max_orders_per_side;
    int64_t stale_distance_ticks;
};

struct NoiseTraderConfig {
    double arrival_rate;
    double size_mu;
    double size_sigma;
    double trend_sensitivity;
};

struct TradeEvent {
    VenueID venue_id;
    Side side;
    int64_t price;
    int64_t quantity;
    double timestamp;
    SenderType sender_type;
};

enum class OrderEventType : uint8_t { DECISION, FILL, COMPLETION };

struct OrderLifecycleEvent {
    OrderEventType type;
    OrderID parent_id;
    Side side;
    int64_t quantity;
    double price;
    VenueID venue_id;
    double timestamp;
    bool timed_out;
};

struct VenueStats {
    int64_t filled_qty = 0;
    double total_notional = 0.0;
    int fill_count = 0;
};

struct PendingOrder {
    Side side;
    int64_t intended_qty;
    double decision_price;
    double decision_time;
    int64_t filled_qty = 0;
    double filled_notional = 0.0;
    std::unordered_map<VenueID, VenueStats> venue_breakdown;
};

struct ExecutionReport {
    OrderID parent_id;
    Side side;
    int64_t intended_size;
    int64_t filled_size;
    double fill_rate;
    double avg_fill_price;
    double decision_price;
    double implementation_shortfall;
    double vwap_slippage;
    double window_vwap;
    bool timed_out;
    std::unordered_map<VenueID, VenueStats> venue_breakdown;
};