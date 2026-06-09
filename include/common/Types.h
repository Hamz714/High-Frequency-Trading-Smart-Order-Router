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
};

struct PriceLevel {
    int64_t price;
    int64_t quantity;
    std::deque<Order> orders;
};

struct SnapshotLevel {
    int64_t price;
    int64_t quantity;
};

struct BookSnapshot {
    std::vector<SnapshotLevel> bids;
    std::vector<SnapshotLevel> asks;
};