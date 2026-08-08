#include "lob/Venue.h"

Venue::Venue(int id, const VenueConfig& cfg, bool simulate_latency):
    venue_id(id), config(cfg), simulate_latency(simulate_latency), lob() {
    if (config.type == LIT) {
            lob.on_book_update([this](Side side, int64_t price, int64_t new_qty) {
                if (this->market_data_queue) {
                    this->market_data_queue->push({
                        this->venue_id,
                        side,
                        price,
                        new_qty,
                        this->stamp_publication()
                    });
                }
            });
        }
    }

Venue::~Venue() {
    stop();
}

void Venue::set_sor_queues(SPSCQueue<BookDelta, QUEUE_SIZE>* md_queue, 
                    SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue) {
    this->market_data_queue = md_queue;
    this->sor_fill_queue = fill_queue;
}

void Venue::set_mm_fill_queue(SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue) {
    this->mm_fill_queue = fill_queue;
}

void Venue::set_analytics_queue(MPSCQueue<TradeEvent, QUEUE_SIZE>* trade_queue) {
    this->analytics_trade_queue = trade_queue;
}

void Venue::set_clock(const SimClock* c) {
    this->clock = c;
}

void Venue::start() {
    if (running.exchange(true)) return;
    worker_thread = std::thread(&Venue::worker_loop, this);
}

void Venue::stop() {
    if (!running.exchange(false)) return;
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
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

int64_t Venue::stamp_arrival(SenderType sender) const {
    if (!simulate_latency || config.latency_us <= 0 || sender != SenderType::SOR) return 0;
    return wire_arrival_ns(config.latency_us);
}

int64_t Venue::stamp_publication() const {
    if (!simulate_latency || config.latency_us <= 0) return 0;
    return wire_arrival_ns(config.latency_us);
}

void Venue::route_order(const OrderRequest& req) {
    OrderRequest stamped = req;
    stamped.arrival_ns = stamp_arrival(req.sender_type);
    inbox.push(stamped);
}

void Venue::worker_loop() {
    OrderRequest req;
    std::deque<OrderRequest> in_flight;

    while (running.load(std::memory_order_acquire)) {
        if (!in_flight.empty()) {
            int64_t now = wire_now_ns();
            while (!in_flight.empty() && in_flight.front().arrival_ns <= now) {
                process_request(in_flight.front());
                in_flight.pop_front();
            }
        }

        if (!inbox.try_pop(req)) {
            if (in_flight.empty()) cpu_relax();
            continue;
        }

        if (req.arrival_ns != 0 && req.arrival_ns > wire_now_ns()) {
            in_flight.push_back(req);
        } else {
            process_request(req);
        }
    }
}

void Venue::process_request(const OrderRequest& req) {
    if (req.request_type == RequestType::CANCEL) {
        lob.cancel(req.order_id);
        return;
    }

    std::vector<Fill> fills = lob.submit(req.side, req.order_type, req.price, req.quantity);

    if (analytics_trade_queue != nullptr) {
        double timestamp = clock ? clock->now() : 0.0;
        for (const Fill& fill : fills) {
            analytics_trade_queue->push({
                venue_id, req.side, fill.fill_price, fill.filled_quantity, timestamp, req.sender_type
            });
        }
    }

    OrderID lob_order_id = (!fills.empty() && fills.back().filled_quantity == 0)
        ? fills.back().order_id
        : -1;

    if (req.sender_type == SenderType::SOR && sor_fill_queue != nullptr) {
        int64_t running_remaining = req.quantity;
        int64_t visible_ns = stamp_publication();

        if (fills.empty()) {
            sor_fill_queue->push({
                req.order_id, -1, venue_id, 0, 0, running_remaining, CANCELLED, visible_ns
            });
            return;
        }

        for (size_t i = 0; i < fills.size(); ++i) {
            running_remaining -= fills[i].filled_quantity;

            OrderStatus current_status = OrderStatus::PARTIAL;

            if (i == fills.size() - 1) {
                if (running_remaining == 0) {
                    current_status = OrderStatus::FILLED;
                } else {
                    current_status = (lob_order_id != -1) ? OrderStatus::PARTIAL : OrderStatus::CANCELLED;
                }
            }

            sor_fill_queue->push({
                req.order_id,
                lob_order_id,
                venue_id,
                fills[i].filled_quantity,
                fills[i].fill_price,
                running_remaining,
                current_status,
                visible_ns
            });
        }
    } else if (req.sender_type == SenderType::MM && mm_fill_queue != nullptr) {
        int64_t running_remaining = req.quantity;

        if (fills.empty()) {
            mm_fill_queue->push({
                req.order_id, -1, venue_id, 0, 0, running_remaining, PARTIAL
            });
            return;
        }

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