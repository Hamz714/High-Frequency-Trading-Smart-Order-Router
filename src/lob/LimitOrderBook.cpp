#include "lob/LimitOrderBook.h"

PriceLevel& LimitOrderBook::get_price_level(Side side) {
    if (side == BUY) {
        if (best_ask >= ask_ladder_lower && best_ask <= ask_ladder_higher) {
            return ask_ladder[best_ask % LADDER_DEPTH];
        }
        return ask_overflow.begin()->second;
    } else {
        if (best_bid >= bid_ladder_lower && best_bid <= bid_ladder_higher) {
            return bid_ladder[best_bid % LADDER_DEPTH];
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

void remove_price_level(Side side, int64_t price) {

}

void add_price_level(Side side, int64_t price, int64_t quantity);


OrderID LimitOrderBook::submit(Side side, OrderType type, int64_t price, int64_t quantity) {
    if (type == FOK) {
        // get remaining liquidity above limit
        // return if not enough liquidity
        int64_t liquidity = available_liquidity(side, price);
        if (liquidity < quantity) {return -1;}
    }

    int64_t remaining_quantity = quantity;
    while (remaining_quantity > 0) {
        //get best price and update best price quantity and remaining quantity
        // exit early if book empty or no quantity better than price
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
            remove_price_level(side, price);
        }
    }

    if (remaining_quantity && type == LIMIT) {
        //store the remaining quantity
        add_price_level(side, price, remaining_quantity);
    }
}


int64_t available_liquidity(Side side, int64_t worst_price) {

}