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

void LimitOrderBook::remove_price_level(Side side) {
    if (side == BUY) {
        if (best_ask >= ask_ladder_lower && best_ask <= ask_ladder_higher) {
            int64_t global_index = best_ask & MASK_MODULO;
            int64_t word_index = global_index / 64;
            int64_t bit_index = global_index % 64;

            uint64_t search_mask = (bit_index == 63) ? 0ULL : (~0ULL) << (bit_index + 1);
            uint64_t masked_word = ask_bitmask[word_index] & search_mask;
            
            if (masked_word != 0) {
                int next_local_bit = __builtin_ctzll(masked_word);
                best_ask = ask_ladder[word_index * 64 + next_local_bit].price;
                return;
            } else {
                bool overflow = false;
                for (int i = 1; i < ask_bitmask.size(); i++) {
                    int next_word_index = (word_index + i) % ask_bitmask.size(); 
                    int64_t window_top_word_index = (ask_ladder_higher & MASK_MODULO) / 64;
                    
                    if (next_word_index == window_top_word_index) {
                        int64_t window_top_bit_index = (ask_ladder_higher & MASK_MODULO) % 64;
                        uint64_t boundary_mask = (~0ULL) >> (63 - window_top_bit_index);
                        uint64_t masked_word_boundary = ask_bitmask[next_word_index] & boundary_mask;
                        
                        if (masked_word_boundary != 0) {
                            int next_local_bit = __builtin_ctzll(masked_word_boundary);
                            best_ask = ask_ladder[next_word_index * 64 + next_local_bit].price;
                            return;
                        } else {
                            overflow = true;
                        }
                    } else {
                        if (ask_bitmask[next_word_index] != 0) {
                            int next_local_bit = __builtin_ctzll(ask_bitmask[next_word_index]);
                            best_ask = ask_ladder[next_word_index * 64 + next_local_bit].price;
                            return;
                        }
                    }
                    if (overflow) {break;}
                }
            }
        } else {
            if (!ask_overflow.empty()) {
                ask_overflow.erase(ask_overflow.begin());
            }
        }
        
        if (ask_overflow.empty()) {
            best_ask = INFINITY;
        } else {
            best_ask = ask_overflow.begin()->first;
        }

    } else {
        if (best_bid >= bid_ladder_lower && best_bid <= bid_ladder_higher) {
            int64_t global_index = best_bid & MASK_MODULO;
            int64_t word_index = global_index / 64;
            int64_t bit_index = global_index % 64;

            uint64_t search_mask = (1ULL << bit_index) - 1;
            uint64_t masked_word = bid_bitmask[word_index] & search_mask;
            if (masked_word != 0) {
                int next_local_bit = 63 - __builtin_clzll(masked_word);
                best_bid = bid_ladder[word_index * 64 + next_local_bit].price;
                return;
            } else {
                bool overflow = false;
                for (int i = 1; i < bid_bitmask.size(); i++) {
                    int next_word_index = (word_index - i + bid_bitmask.size()) % bid_bitmask.size();
                    int64_t window_bottom_word_index = (bid_ladder_lower & MASK_MODULO) / 64;

                    if (next_word_index == window_bottom_word_index) {
                        int64_t window_bottom_bit_index = (bid_ladder_lower & MASK_MODULO) % 64;
                        uint64_t boundary_mask = (~0ULL) << window_bottom_bit_index;
                        uint64_t masked_word = bid_bitmask[next_word_index] & boundary_mask;

                        if (masked_word != 0) {
                            int next_local_bit = 63 - __builtin_clzll(masked_word);
                            best_bid = bid_ladder[next_word_index * 64 + next_local_bit].price;
                            return;
                        } else {
                            overflow = true;
                        }
                    } else {
                        if (bid_bitmask[next_word_index] != 0) {
                            int next_local_bit = 63 - __builtin_clzll(bid_bitmask[next_word_index]);
                            best_bid = bid_ladder[next_word_index * 64 + next_local_bit].price;
                            return;
                        }
                    }
                    if (overflow) {break;}
                }
            }
        } else {
            bid_overflow.erase(bid_overflow.begin());
        }

        if (bid_overflow.empty()) {
            best_bid = 0;
        } else {
            best_bid = bid_overflow.begin()->first;
        }
    }
}

void LimitOrderBook::add_price_level(Side side, int64_t price, int64_t quantity, OrderID order_id) {
    Order order{order_id, price, quantity, side};
    int64_t global_index = price & MASK_MODULO;
    int64_t word_index = global_index / 64;
    int64_t bit_index = global_index % 64;
    
    if (side == SELL) {
        if (price >= ask_ladder_lower && price <= ask_ladder_higher) {
            ask_ladder[global_index].orders.push_back(order); 
            ask_ladder[global_index].quantity += quantity;
            ask_ladder[global_index].price = price;
            ask_bitmask[word_index] |= 1ULL << bit_index;
        } else {
            PriceLevel& price_level = ask_overflow[price];
            price_level.orders.push_back(order);
            price_level.quantity += quantity;
            price_level.price = price;
        }

        if (price < best_ask) {
            best_ask = price;
        }
    } else {
        if (price >= bid_ladder_lower && price <= bid_ladder_higher) {
            bid_ladder[global_index].orders.push_back(order);
            bid_ladder[global_index].quantity += quantity;
            bid_ladder[global_index].price = price;
            bid_bitmask[word_index] |= 1ULL << bit_index;
        } else {
            PriceLevel& price_level = bid_overflow[price];
            price_level.orders.push_back(order);
            price_level.quantity += quantity;
            price_level.price = price;
        }

        if (price > best_bid) {
            best_bid = price;
        }
    }
}


OrderID LimitOrderBook::submit(Side side, OrderType type, int64_t price, int64_t quantity) {
    if (type == FOK) {
        int64_t liquidity = available_liquidity(side, price);
        if (liquidity < quantity) {return -1;}
    }

    OrderID order_id = generate_order_id();

    int64_t remaining_quantity = quantity;
    while (remaining_quantity > 0) {
        if (available_liquidity(side, price) == 0) {break;}

        PriceLevel& next_price_level = get_price_level(side);
        if (!valid_price(side, next_price_level.price, price)) {break;}

        while (remaining_quantity && next_price_level.quantity) {
            Order& next_book_order = next_price_level.orders.front();

            if (remaining_quantity >= next_book_order.quantity) {
                remaining_quantity -= next_book_order.quantity;
                next_price_level.quantity -= next_book_order.quantity;
                next_price_level.orders.pop_front(); //order complete

            } else {
                next_price_level.quantity -= remaining_quantity;
                next_book_order.quantity -= remaining_quantity;
                remaining_quantity = 0;
            }
        }

        if (next_price_level.quantity == 0) {
            remove_price_level(side);
        }
    }

    if (remaining_quantity && type == LIMIT) {
        add_price_level(side, price, remaining_quantity, order_id);
    }

    return order_id;
}


int64_t available_liquidity(Side side, int64_t worst_price) {

}