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