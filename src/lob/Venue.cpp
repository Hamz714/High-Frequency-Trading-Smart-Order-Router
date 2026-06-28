#include "lob/Venue.h"

Venue::Venue(int id, const VenueConfig& cfg,
            ThreadSafeQueue<BookDelta>* md_queue,
            ThreadSafeQueue<FillEvent>* fill_queue):
    venue_id(id), config(cfg), lob(),
    market_data_queue(md_queue), sor_fill_queue(fill_queue) {
    if (config.type == LIT) {
            lob.on_book_update([this](Side side, int64_t price, int64_t new_qty) {
                if (this->market_data_queue) {
                    this->market_data_queue->push({
                        this->venue_id, 
                        side, 
                        price, 
                        new_qty
                    });
                }
            });
        }
    }

void Venue::start() {
    worker_thread = std::thread(&Venue::worker_loop, this);
}

int Venue::get_id() const {
    return venue_id;
}

VenueType Venue::get_type() const {
    return config.type;
}

const VenueConfig& Venue::get_config() const {
    return config;
}

void Venue::route_order(const OrderRequest& req) {
    inbox.push(req);
}

void Venue::worker_loop() {
    OrderRequest req;
    
    while (true) {
        inbox.pop(req);

        std::this_thread::sleep_for(std::chrono::microseconds(config.latency_us));

        std::vector<Fill> fills = lob.submit(req.side, req.order_type, req.price, req.quantity);

        if (req.sender_type == SOR && sor_fill_queue != nullptr) {
            int64_t running_remaining = req.quantity;

            if (fills.empty()) {
                sor_fill_queue->push({
                    req.child_id, venue_id, 0, 0, running_remaining, CANCELLED
                });
                continue;
            }

            for (size_t i = 0; i < fills.size(); ++i) {
                running_remaining -= fills[i].filled_quantity;
                OrderStatus current_status = PARTIAL;

                if (i == fills.size() - 1) {
                    current_status = (running_remaining == 0) ? FILLED : CANCELLED;
                }

                sor_fill_queue->push({
                    req.child_id,
                    venue_id,
                    fills[i].filled_quantity,
                    fills[i].fill_price,
                    running_remaining,
                    current_status
                });
            }
        }
    }
}