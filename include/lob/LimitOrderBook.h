#pragma once
#include <cstdint>
#include "common/Types.h"
#include <array>
#include <map>
#include <functional>

constexpr int64_t LADDER_DEPTH = 100;

class LimitOrderBook {
    std::array<PriceLevel, LADDER_DEPTH> bid_ladder;
    std::array<PriceLevel, LADDER_DEPTH> ask_ladder;

    std::map<int64_t, PriceLevel> bid_overflow;
    std::map<int64_t, PriceLevel> ask_overflow;

    public:
        OrderID submit(Side side, OrderType type, int64_t price, int64_t quantity);

        bool cancel(OrderID id);

        BookSnapshot get_snapshot(int levels);

        int64_t available_liquidity(Side side, int64_t worst_price);

        void on_fill(std::function<void(BookSnapshot)> callback);
};