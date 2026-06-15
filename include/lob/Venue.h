#pragma once
#include "common/Types.h"
#include "lob/LimitOrderBook.h"

class Venue {
    int venue_id;
    VenueConfig config;
    LimitOrderBook lob;

    public:
        Venue(int id, const VenueConfig& cfg);

        int get_id() const;
        VenueType get_type() const;
        const VenueConfig& get_config() const;

        OrderID route_order(Side side, OrderType type, int64_t price, int64_t quantity);

        BookSnapshot get_venue_snapshot(int levels) const;

        int64_t get_visible_liquidity(Side side, int64_t worst_price) const;

        LimitOrderBook& get_raw_lob();

        void set_sor_callbacks(std::function<void(Fill)> on_fill, std::function<void(BookSnapshot)> on_update);
};