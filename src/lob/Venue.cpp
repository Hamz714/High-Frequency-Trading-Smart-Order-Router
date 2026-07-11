#include "lob/Venue.h"

Venue::Venue(int id, const VenueConfig& cfg):
    venue_id(id), config(cfg), lob() {
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

void Venue::set_sor_queues(SPSCQueue<BookDelta, QUEUE_SIZE>* md_queue, 
                    SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue) {
    this->market_data_queue = md_queue;
    this->sor_fill_queue = fill_queue;
}

void Venue::set_mm_fill_queue(SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue) {
    this->mm_fill_queue = fill_queue;
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
        while (!inbox.try_pop(req)) {
            _mm_pause(); 
        }

        std::this_thread::sleep_for(std::chrono::microseconds(config.latency_us));

        if (req.request_type == RequestType::CANCEL) {
            lob.cancel(req.order_id);
            continue;
        }

        std::vector<Fill> fills = lob.submit(req.side, req.order_type, req.price, req.quantity);

        if (req.sender_type == SenderType::SOR && sor_fill_queue != nullptr) {
            int64_t running_remaining = req.quantity;

            if (fills.empty()) {
                sor_fill_queue->push({
                    req.order_id, -1, venue_id, 0, 0, running_remaining, CANCELLED
                });
                continue;
            }

            OrderID lob_order_id = fills[0].order_id;
            for (size_t i = 0; i < fills.size(); ++i) {
                running_remaining -= fills[i].filled_quantity; 
                
                OrderStatus current_status = OrderStatus::PARTIAL;

                if (i == fills.size() - 1) {
                    current_status = (running_remaining == 0) ? OrderStatus::FILLED : CANCELLED;
                }

                sor_fill_queue->push({
                    req.order_id,
                    lob_order_id,
                    venue_id,
                    fills[i].filled_quantity,
                    fills[i].fill_price,
                    running_remaining,
                    current_status
                });
            }
        } else if (req.sender_type == SenderType::MM && mm_fill_queue != nullptr) {
            int64_t running_remaining = req.quantity;

            if (fills.empty()) {
                mm_fill_queue->push({
                    req.order_id, -1, venue_id, 0, 0, running_remaining, PARTIAL
                });
                continue;
            }

            OrderID lob_order_id = fills[0].order_id;
            for (size_t i = 0; i < fills.size(); ++i) {
                running_remaining -= fills[i].filled_quantity; 
                
                OrderStatus current_status = OrderStatus::PARTIAL;

                if (i == fills.size() - 1) {
                    current_status = (running_remaining == 0) ? OrderStatus::FILLED : PARTIAL;
                }

                mm_fill_queue->push({
                    req.order_id,
                    lob_order_id,
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