#include "lob/Venue.h"

Venue::Venue(int id, const VenueConfig& cfg):
    venue_id(id), config(cfg), lob() {}

int Venue::get_id() const {
    return venue_id;
}

VenueType Venue::get_type() const {
    return config.type;
}

const VenueConfig& Venue::get_config() const {
    return config;
}

double Venue::half_spread() const {
    return lob.half_spread();
}

OrderID Venue::route_order(Side side, OrderType type, int64_t price, int64_t quantity) {
    return lob.submit(side, type, price, quantity);
}

BookSnapshot Venue::get_venue_snapshot(int levels) const {
    if (config.type == DARK) {
        return BookSnapshot();
    }

    return lob.get_snapshot(levels);
}

int64_t Venue::get_visible_liquidity(Side side, int64_t worst_price) const {
    if (config.type == DARK) {
        return 0;
    }

    return lob.available_liquidity(side, worst_price);
}

LimitOrderBook& Venue::get_raw_lob() {
    return lob;
}

void Venue::set_sor_callbacks(std::function<void(Fill)> on_fill, std::function<void(BookSnapshot)> on_update) {
    lob.on_fill(on_fill);
    lob.on_book_update(on_update);
}