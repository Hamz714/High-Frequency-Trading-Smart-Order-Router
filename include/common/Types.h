#pragma once
#include <cstdint>
#include <deque>
#include <vector>

using OrderID = int64_t;

enum Side {BUY, SELL};

enum OrderType {LIMIT, MARKET, IOC, FOK};

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