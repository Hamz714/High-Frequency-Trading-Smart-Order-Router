#include "lob/LimitOrderBook.h"

PriceLevel& LimitOrderBook::get_price_level(Side side) {
    if (side == BUY) {
        if (best_ask >= ask_ladder_lower && best_ask <= ask_ladder_higher) {
            return ask_ladder[best_ask & MASK_MODULO];
        }
        return ask_overflow.begin()->second;
    } else {
        if (best_bid >= bid_ladder_lower && best_bid <= bid_ladder_higher) {
            return bid_ladder[best_bid & MASK_MODULO];
        }
        return bid_overflow.begin()->second;
    }
}

bool valid_price(Side side, int64_t book_best_price, int64_t order_price) {
    if (side == BUY) {
        if (order_price >= book_best_price) {return true;}
        return false;
    } else {
        if (order_price <= book_best_price) {return true;}
        return false;
    }
}

void LimitOrderBook::remove_price_level(Side side, int64_t price) {
    int64_t global_index = price & MASK_MODULO;
    int64_t word_index = global_index / 64;
    int64_t bit_index = global_index % 64;

    if (side == BUY) {
        if (price >= ask_ladder_lower && price <= ask_ladder_higher) {
            ask_bitmask[word_index] &= ~(1ULL << bit_index); 
        } else {
            ask_overflow.erase(price);
        }

        if (price == best_ask) {
            find_next_best_ask();
        }
    } else {
        if (price >= bid_ladder_lower && price <= bid_ladder_higher) {
            bid_bitmask[word_index] &= ~(1ULL << bit_index);
        } else {
            bid_overflow.erase(price);
        }

        if (price == best_bid) {
            find_next_best_bid();
        }
    }
}

void LimitOrderBook::find_next_best_ask() {
    if (best_ask >= ask_ladder_lower && best_ask <= ask_ladder_higher) {
        int64_t global_index = best_ask & MASK_MODULO;
        int64_t word_index = global_index / 64;
        int64_t bit_index = global_index % 64;

        uint64_t search_mask = (bit_index == 63) ? 0ULL : (~0ULL) << (bit_index + 1);
        uint64_t masked_word = ask_bitmask[word_index] & search_mask;

        if (masked_word != 0) {
            int next_bit_index = __builtin_ctzll(masked_word);
            best_ask = ask_ladder[word_index * 64 + next_bit_index].price;
            return;
        }

        for (int i = 1; i < ask_bitmask.size(); i++) {
            int next_word_index = (word_index + i) % ask_bitmask.size();
            int64_t window_top_word_index = (ask_ladder_higher & MASK_MODULO) / 64;

            if (next_word_index == window_top_word_index) {
                int64_t window_top_bit_index = (ask_ladder_higher & MASK_MODULO) % 64;
                uint64_t boundary_mask = (~0ULL) >> (63 - window_top_bit_index);
                uint64_t masked_word_boundary = ask_bitmask[next_word_index] & boundary_mask;
                
                if (masked_word_boundary != 0) {
                    int next_bit_index = __builtin_ctzll(masked_word_boundary);
                    best_ask = ask_ladder[next_word_index * 64 + next_bit_index].price;
                    return;
                } else {
                    break;
                } 
            } else {
                if (ask_bitmask[next_word_index] != 0) {
                    int next_local_bit = __builtin_ctzll(ask_bitmask[next_word_index]);
                    best_ask = ask_ladder[next_word_index * 64 + next_local_bit].price;
                    return;
                }
            }
        }
    }
    if (ask_overflow.empty()) {
        best_ask = INT64_MAX;
    } else {
        best_ask = ask_overflow.begin()->first;
    }
}


void LimitOrderBook::find_next_best_bid() {
    if (best_bid >= bid_ladder_lower && best_bid <= bid_ladder_higher) {
        int64_t global_index = best_bid & MASK_MODULO;
        int64_t word_index = global_index / 64;
        int64_t bit_index = global_index % 64;

        uint64_t search_mask = (1ULL << bit_index) - 1;
        uint64_t masked_word = bid_bitmask[word_index] & search_mask;

        if (masked_word != 0) {
            int next_bit_index = 63 - __builtin_clzll(masked_word);
            best_bid = bid_ladder[word_index * 64 + next_bit_index].price;
            return;
        }

        for (int i = 1; i < bid_bitmask.size(); i++) {
            int next_word_index = (word_index - i + bid_bitmask.size()) % bid_bitmask.size();
            int64_t window_bottom_word_index = (bid_ladder_lower & MASK_MODULO) / 64;

            if (next_word_index == window_bottom_word_index) {
                int64_t window_bottom_bit_index = (bid_ladder_lower & MASK_MODULO) % 64;
                uint64_t boundary_mask = (~0ULL) << window_bottom_bit_index;
                uint64_t masked_word_boundary = bid_bitmask[next_word_index] & boundary_mask;
                
                if (masked_word_boundary != 0) {
                    int next_bit_index = 63 - __builtin_clzll(masked_word_boundary);
                    best_bid = bid_ladder[next_word_index * 64 + next_bit_index].price;
                    return;
                } else {
                    break; 
                } 
            } else {
                if (bid_bitmask[next_word_index] != 0) {
                    int next_local_bit = 63 - __builtin_clzll(bid_bitmask[next_word_index]);
                    best_bid = bid_ladder[next_word_index * 64 + next_local_bit].price;
                    return;
                }
            }
        }
    }    
    if (bid_overflow.empty()) {
        best_bid = 0;
    } else {
        best_bid = bid_overflow.begin()->first;
    }
}


void LimitOrderBook::add_price_level(Side side, int64_t price, int64_t quantity, OrderID order_id) {
    Order order{order_id, price, quantity, side, -1, -1, true};
    int64_t global_index = price & MASK_MODULO;
    int64_t word_index = global_index / 64;
    int64_t bit_index = global_index % 64;

    int64_t order_index = order_id & 0x00000000FFFFFFFF;
    global_order_pool[order_index] = order;

    PriceLevel& price_level = PriceLevel();
    
    if (side == SELL) {
        if (price >= ask_ladder_lower && price <= ask_ladder_higher) {
            price_level = ask_ladder[global_index];
            ask_bitmask[word_index] |= 1ULL << bit_index;
        } else {
            price_level = ask_overflow[price];
        }

        if (price < best_ask) {
            best_ask = price;
        }
    } else {
        if (price >= bid_ladder_lower && price <= bid_ladder_higher) {
            price_level = bid_ladder[global_index];
            bid_bitmask[word_index] |= 1ULL << bit_index;
        } else {
            price_level = bid_overflow[price];
        }

        if (price > best_bid) {
            best_bid = price;
        }
    }

    global_order_pool[order_index].prev_index = price_level.tail_order_index;
    global_order_pool[price_level.tail_order_index].next_index = order_index;
    price_level.tail_order_index = order_index;

    price_level.quantity += quantity;
    price_level.price = price;
}


OrderID LimitOrderBook::submit(Side side, OrderType type, int64_t price, int64_t quantity) {
    if (type == FOK) {
        int64_t liquidity = available_liquidity(side, price);
        if (liquidity < quantity) {return -1;}
    }

    OrderID order_id = generate_order_id(side, price);

    int64_t remaining_quantity = quantity;
    while (remaining_quantity > 0) {
        if (available_liquidity(side, price) == 0) {break;}

        PriceLevel& next_price_level = get_price_level(side);
        if (!valid_price(side, next_price_level.price, price)) {break;}

        while (remaining_quantity && next_price_level.quantity) {
            Order& next_book_order = global_order_pool[next_price_level.head_order_index];

            if (remaining_quantity >= next_book_order.quantity) {
                remaining_quantity -= next_book_order.quantity;
                next_price_level.quantity -= next_book_order.quantity;
                next_book_order.quantity = 0;
                next_book_order.is_active = false; // order complete
                next_price_level.head_order_index = next_book_order.next_index;

            } else {
                next_price_level.quantity -= remaining_quantity;
                next_book_order.quantity -= remaining_quantity;
                remaining_quantity = 0;
            }
        }

        if (next_price_level.quantity == 0) {
            remove_price_level(side, next_price_level.price);
        }
    }

    if (remaining_quantity && type == LIMIT) {
        add_price_level(side, price, remaining_quantity, order_id);
    }

    return order_id;
}


OrderID LimitOrderBook::generate_order_id(Side side, int64_t price) {
    uint64_t id = 0;
    if (side == SELL) id |= (1ULL << 63);
    id |= (static_cast<uint64_t>(price) << 32);
    id |= static_cast<uint32_t>(next_order_index);
    next_order_index++;
    return id;
}


bool LimitOrderBook::cancel(OrderID id) {
    Side side = (id & (1ULL << 63)) ? SELL : BUY;
    int64_t price = (id >> 32) & 0x7FFFFFFF;
    int64_t order_index = id & 0xFFFFFFFF;

    Order& order = global_order_pool[order_index];

    if (!order.is_active) return false;

    int64_t global_index = price & MASK_MODULO;
    PriceLevel* price_level = nullptr;

    if (side == BUY) {
        if (price >= bid_ladder_lower && price <= bid_ladder_higher) {
            price_level = &bid_ladder[global_index];
        } else {
            price_level = &bid_overflow[price];
        }
    } else {
        if (price >= ask_ladder_lower && price <= ask_ladder_higher) {
            price_level = &ask_ladder[global_index];
        } else {
            price_level = &ask_overflow[price];
        }
    }

    order.is_active = false;
    price_level->quantity -= order.quantity;

    if (order.prev_index != -1) {
        global_order_pool[order.prev_index].next_index = order.next_index;
    } else {
        price_level->head_order_index = order.next_index;
    }

    if (order.next_index != -1) {
        global_order_pool[order.next_index].prev_index = order.prev_index;
    } else {
        price_level->tail_order_index = order.prev_index;
    }

    if (price_level->quantity == 0) {
        remove_price_level(side, price);
    }

    return true;
}


BookSnapshot LimitOrderBook::get_snapshot(int max_levels) const {
    BookSnapshot snapshot;
    snapshot.asks.reserve(max_levels); 
    snapshot.bids.reserve(max_levels);

    if (best_ask != INT64_MAX) { 
        if (best_ask >= ask_ladder_lower && best_ask <= ask_ladder_higher) {
            int64_t global_index = best_ask & MASK_MODULO;
            int64_t word_index = global_index / 64;

            for (int i = 0; i < ask_bitmask.size() && snapshot.asks.size() < max_levels; i++) {
                int next_word_index = (word_index + i) % ask_bitmask.size();
                uint64_t word = ask_bitmask[next_word_index];

                int64_t window_top_word_index = (ask_ladder_higher & MASK_MODULO) / 64;
                if (next_word_index == window_top_word_index) {
                    int64_t window_top_bit_index = (ask_ladder_higher & MASK_MODULO) % 64;
                    word &= (~0ULL) >> (63 - window_top_bit_index);
                }

                while (word != 0 && snapshot.asks.size() < max_levels) {
                    int active_bit = __builtin_ctzll(word);
                    int64_t exact_index = next_word_index * 64 + active_bit;
                    
                    snapshot.asks.push_back({
                        ask_ladder[exact_index].price,
                        ask_ladder[exact_index].quantity
                    });

                    word &= ~(1ULL << active_bit);
                }

                if (next_word_index == window_top_word_index) break;
            }
        }

        for (auto it = ask_overflow.begin(); it != ask_overflow.end() && snapshot.asks.size() < max_levels; ++it) {
            snapshot.asks.push_back({it->first, it->second.quantity});
        }
    }

    if (best_bid != 0) { 
        if (best_bid >= bid_ladder_lower && best_bid <= bid_ladder_higher) {
            int64_t global_index = best_bid & MASK_MODULO;
            int64_t word_index = global_index / 64;

            for (int i = 0; i < bid_bitmask.size() && snapshot.bids.size() < max_levels; i++) {
                int next_word_index = (word_index - i + bid_bitmask.size()) % bid_bitmask.size();
                uint64_t word = bid_bitmask[next_word_index];

                int64_t window_bottom_word_index = (bid_ladder_lower & MASK_MODULO) / 64;
                if (next_word_index == window_bottom_word_index) {
                    int64_t window_bottom_bit_index = (bid_ladder_lower & MASK_MODULO) % 64;
                    word &= (~0ULL) << window_bottom_bit_index;
                }

                while (word != 0 && snapshot.bids.size() < max_levels) {
                    int active_bit = 63 - __builtin_clzll(word);
                    int64_t exact_index = next_word_index * 64 + active_bit;
                    
                    snapshot.bids.push_back({
                        bid_ladder[exact_index].price,
                        bid_ladder[exact_index].quantity
                    });

                    word &= ~(1ULL << active_bit);
                }

                if (next_word_index == window_bottom_word_index) break;
            }
        }

        for (auto it = bid_overflow.begin(); it != bid_overflow.end() && snapshot.bids.size() < max_levels; ++it) {
            snapshot.bids.push_back({it->first, it->second.quantity});
        }
    }

    return snapshot;
}


int64_t available_liquidity(Side side, int64_t worst_price) {

}