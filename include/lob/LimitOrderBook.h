#pragma once
#include <cstdint>
#include "common/Types.h"
#include <array>
#include <map>
#include <functional>
#include <vector>

constexpr int64_t LADDER_DEPTH = 256; //must be power of 2
constexpr int64_t MASK_MODULO = LADDER_DEPTH - 1;

class LimitOrderBook {
    std::array<PriceLevel, LADDER_DEPTH> bid_ladder;
    std::array<PriceLevel, LADDER_DEPTH> ask_ladder;

    int64_t bid_ladder_lower;
    int64_t bid_ladder_higher;
    int64_t ask_ladder_lower;
    int64_t ask_ladder_higher;

    int64_t best_bid = 0;
    int64_t best_ask = INFINITY;

    std::array<int64_t, LADDER_DEPTH/64> bid_bitmask;
    std::array<int64_t, LADDER_DEPTH/64> ask_bitmask;

    std::map<int64_t, PriceLevel, std::greater<int64_t>> bid_overflow;
    std::map<int64_t, PriceLevel> ask_overflow;

    int64_t next_order_index = 1;

    std::vector<Order> global_order_pool{10'000'000};

    PriceLevel& get_price_level(Side side);

    void remove_price_level(Side side, int64_t price);

    void add_price_level(Side side, int64_t price, int64_t quantity, OrderID order_id);

    void find_next_best_ask();

    void find_next_best_bid();

    void shift_ask_window();
    void evict_ask_range(int64_t low_price, int64_t high_price);

    void shift_bid_window();
    void evict_bid_range(int64_t low_price, int64_t high_price);

    OrderID generate_order_id(Side side, int64_t price);

    public:
        OrderID submit(Side side, OrderType type, int64_t price, int64_t quantity);

        bool cancel(OrderID id);

        BookSnapshot get_snapshot(int levels) const;

        int64_t available_liquidity(Side side, int64_t worst_price);

        void on_fill(std::function<void(BookSnapshot)> callback);
};