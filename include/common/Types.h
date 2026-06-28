#pragma once
#include <cstdint>
#include <deque>
#include <vector>

using OrderID = int64_t;

using VenueID = int64_t;

enum Side {BUY, SELL};

enum OrderType {LIMIT, MARKET, IOC, FOK};

enum SenderType {SOR, MM};

enum OrderStatus {PARTIAL, FILLED, CANCELLED};

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
    int64_t price;
    int64_t quantity;
    int32_t head_order_index;
    int32_t tail_order_index;
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
    VenueID venue_id;
    int64_t filled_quantity;
    int64_t fill_price;
    int64_t remaining_quantity;
    OrderStatus status;
};

enum VenueType {LIT, DARK};

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

struct RouterConfig {
    int64_t lot_size;
    int64_t latency_cost_factor;
    double dark_pool_decay_rate;
};

struct BookDelta {
    VenueID venue_id;
    Side side;
    int64_t price;
    int64_t new_quantity;
};

struct OrderRequest {
    OrderID child_id;
    SenderType sender_type;
    Side side;
    OrderType order_type;
    int64_t price;
    int64_t quantity;
};

struct VenueState {
    int venue_id;
    VenueConfig config;
    const LimitOrderBook* local_lob;

    int64_t get_visible_liquidity(Side side, int64_t worst_price) const {
        if (!local_lob || config.type == DARK) return 0;
        return local_lob->available_liquidity(side, worst_price); 
    }

    double half_spread() const {
        if (!local_lob) return 0.0;
        return local_lob->half_spread(); 
    }
};