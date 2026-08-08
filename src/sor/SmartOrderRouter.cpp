#include "sor/SmartOrderRouter.h"
#include <algorithm>

SmartOrderRouter::SmartOrderRouter(const RouterConfig& cfg, const QueueSizingConfig& queues):
    config(cfg), dp_engine(cfg), queue_sizing(queues), client_inbox(queues.order_inbox),
    running(false) {}

SmartOrderRouter::~SmartOrderRouter() {
    stop();
}

void SmartOrderRouter::add_venue(Venue* venue) {
    VenueID id = venue->get_id(); 
    
    venues[id] = venue;

    venue_md_queues[id] = std::make_unique<SPSCQueue<BookDelta>>(queue_sizing.market_data);
    venue_fill_queues[id] = std::make_unique<SPSCQueue<FillEvent>>(queue_sizing.fill);

    mirror_books.try_emplace(id);

    venue->set_sor_queues(venue_md_queues[id].get(), venue_fill_queues[id].get());
}

void SmartOrderRouter::set_clock(const SimClock* c) {
    this->clock = c;
}

void SmartOrderRouter::set_analytics_queue(SPSCQueue<OrderLifecycleEvent>* queue) {
    this->analytics_queue = queue;
}

SplitResult SmartOrderRouter::compute_split(int64_t size, Side side, int64_t worst_price) {
    return config.use_naive_split
        ? dp_engine.compute_naive_split(size, side, worst_price, venue_states)
        : dp_engine.compute_optimal_split(size, side, worst_price, venue_states);
}

double SmartOrderRouter::compute_consolidated_mid() const {
    int64_t best_bid = 0;
    int64_t best_ask = std::numeric_limits<int64_t>::max();

    for (const auto& vs : venue_states) {
        if (vs.config.type != LIT) continue;
        best_bid = std::max(best_bid, vs.get_best_bid());
        best_ask = std::min(best_ask, vs.get_best_ask());
    }

    bool have_bid = best_bid != 0;
    bool have_ask = best_ask != std::numeric_limits<int64_t>::max();

    if (have_bid && have_ask) return (static_cast<double>(best_bid) + static_cast<double>(best_ask)) / 2.0;
    if (have_ask) return static_cast<double>(best_ask);
    return static_cast<double>(best_bid);
}

void SmartOrderRouter::initialise_venue_states() {
    venue_states.reserve(mirror_books.size());
    
    for (const auto& [venue_id, lob] : mirror_books) {
        VenueState vs;
        vs.venue_id = venue_id;
        vs.config = venues.at(venue_id)->get_config(); 
        
        vs.local_lob = &mirror_books.at(venue_id); 
        
        venue_states.push_back(vs);
    }
}

void SmartOrderRouter::start() {
    if (running.exchange(true)) return;

    initialise_venue_states();

    md_thread = std::thread(&SmartOrderRouter::market_data_loop, this);
    fill_thread = std::thread(&SmartOrderRouter::fill_loop, this);
    order_thread = std::thread(&SmartOrderRouter::client_order_loop, this);
}

void SmartOrderRouter::stop() {
    if (!running.exchange(false)) return; 

    if (md_thread.joinable()) md_thread.join();
    if (fill_thread.joinable()) fill_thread.join();
    if (order_thread.joinable()) order_thread.join();
}

void SmartOrderRouter::submit_order(const OrderRequest& client_request) {
    client_inbox.push(client_request);
}

uint64_t SmartOrderRouter::get_dropped_total() const {
    uint64_t total = client_inbox.dropped();

    for (const auto& [venue_id, queue] : venue_md_queues) {
        total += queue->dropped();
    }
    for (const auto& [venue_id, queue] : venue_fill_queues) {
        total += queue->dropped();
    }

    return total;
}

void SmartOrderRouter::market_data_loop() {
    BookDelta delta;

    std::unordered_map<VenueID, std::deque<BookDelta>> in_flight;
    for (const auto& [venue_id, queue] : venue_md_queues) {
        in_flight[venue_id];
    }

    while (running.load(std::memory_order_relaxed)) {
        bool idle = true;

        for (auto& [venue_id, queue] : venue_md_queues) {
            std::deque<BookDelta>& pending = in_flight[venue_id];

            int processed_count = 0;

            while (processed_count < BATCH_LIMIT && queue->try_pop(delta)) {
                processed_count++;
                pending.push_back(delta);
            }

            if (pending.empty()) continue;

            int64_t now = wire_now_ns();
            int applied_count = 0;

            while (applied_count < BATCH_LIMIT && !pending.empty() && pending.front().visible_ns <= now) {
                idle = false;
                applied_count++;

                std::unique_lock<std::shared_mutex> lock(book_mutex);
                mirror_books[venue_id].apply_delta(pending.front());
                pending.pop_front();
            }
        }

        if (idle) {
            cpu_relax();
        }
    }
}

void SmartOrderRouter::fill_loop() {
    FillEvent fill;

    std::unordered_map<VenueID, std::deque<FillEvent>> in_flight;
    for (const auto& [venue_id, queue] : venue_fill_queues) {
        in_flight[venue_id];
    }

    while (running.load(std::memory_order_relaxed)) {
        bool idle = true;

        for (auto& [venue_id, queue] : venue_fill_queues) {
            std::deque<FillEvent>& pending = in_flight[venue_id];

            int processed_count = 0;

            while (processed_count < BATCH_LIMIT && queue->try_pop(fill)) {
                processed_count++;
                pending.push_back(fill);
            }

            if (pending.empty()) continue;

            int64_t now = wire_now_ns();
            int delivered_count = 0;

            while (delivered_count < BATCH_LIMIT && !pending.empty() && pending.front().visible_ns <= now) {
                idle = false;
                delivered_count++;

                process_fill(pending.front());
                pending.pop_front();
            }
        }

        if (idle) {
            cpu_relax();
        }
    }
}

void SmartOrderRouter::process_fill(const FillEvent& fill) {
    std::unique_lock<std::mutex> order_lock(order_mutex);

    auto parent_it = child_to_parent.find(fill.child_id);
    if (parent_it == child_to_parent.end()) return;

    OrderID parent_id = parent_it->second;
    ParentOrder& parent = active_parent_orders[parent_id];

    parent.filled_qty += fill.filled_quantity;

    if (analytics_queue) {
        analytics_queue->push({
            OrderEventType::FILL, parent_id, parent.side,
            fill.filled_quantity, static_cast<double>(fill.fill_price),
            fill.venue_id, clock ? clock->now() : 0.0, false
        });
    }

    if (fill.status == FILLED || fill.status == CANCELLED) {

        bool order_finished = false;
        bool timed_out = false;

        if (fill.remaining_quantity > 0) {
            parent.reroute_count++;

            if (parent.reroute_count <= config.max_reroute_attempts) {
                SplitResult new_split;
                {
                    std::shared_lock<std::shared_mutex> book_lock(book_mutex);
                    new_split = compute_split(
                        fill.remaining_quantity,
                        parent.side,
                        parent.price
                    );
                }

                int64_t routed = execute_routing_decision(parent, new_split);
                if (routed == 0) {
                    order_finished = true;
                    timed_out = true;
                }
            } else {
                order_finished = true;
                timed_out = true;
            }
        }

        if (parent.filled_qty == parent.total_qty) {
            order_finished = true;
        }

        if (order_finished) {
            if (analytics_queue) {
                analytics_queue->push({
                    OrderEventType::COMPLETION, parent_id, parent.side,
                    parent.filled_qty, 0.0, -1,
                    clock ? clock->now() : 0.0, timed_out
                });
            }
            active_parent_orders.erase(parent_id);
        }

        child_to_parent.erase(parent_it);
    }
}

void SmartOrderRouter::client_order_loop() {
    OrderRequest req;
    
    while (running.load(std::memory_order_relaxed)) {

        if (client_inbox.try_pop(req)) {
            
            ParentOrder parent;
            parent.parent_id = req.order_id;
            parent.side = req.side;
            parent.price = req.price;
            parent.total_qty = req.quantity;
            parent.filled_qty = 0;
            parent.reroute_count = 0;

            const int max_initial_attempts = 10;
            int64_t routed = 0;

            for (int attempt = 0; attempt <= max_initial_attempts; ++attempt) {
                std::unique_lock<std::mutex> order_lock(order_mutex);

                if (attempt == 0) {
                    parent.decision_time = clock ? clock->now() : 0.0;
                    active_parent_orders[parent.parent_id] = parent;

                    if (analytics_queue) {
                        double mid;
                        {
                            std::shared_lock<std::shared_mutex> book_lock(book_mutex);
                            mid = compute_consolidated_mid();
                        }
                        analytics_queue->push({
                            OrderEventType::DECISION, parent.parent_id, parent.side,
                            parent.total_qty, mid, -1,
                            parent.decision_time, false
                        });
                    }
                }

                SplitResult split;
                {
                    std::shared_lock<std::shared_mutex> book_lock(book_mutex);
                    split = compute_split(req.quantity, req.side, req.price);
                }
                routed = execute_routing_decision(active_parent_orders[parent.parent_id], split);

                order_lock.unlock();

                if (routed > 0 || attempt == max_initial_attempts) break;

                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }

            if (routed == 0) {
                std::unique_lock<std::mutex> order_lock(order_mutex);
                if (analytics_queue) {
                    analytics_queue->push({
                        OrderEventType::COMPLETION, parent.parent_id, parent.side,
                        0, 0.0, -1, clock ? clock->now() : 0.0, true
                    });
                }
                active_parent_orders.erase(parent.parent_id);
            }

        } else {
            cpu_relax();
        }
    }
}

int64_t SmartOrderRouter::execute_routing_decision(const ParentOrder& parent, const SplitResult& split) {

    static std::atomic<uint64_t> child_id_generator{1000000};

    int64_t total_routed = 0;

    for (size_t i = 0; i < split.allocations.size(); ++i) {
        int64_t allocated_qty = split.allocations[i];

        if (allocated_qty > 0) {
            total_routed += allocated_qty;

            VenueID target_venue = venue_states[i].venue_id;

            OrderID child_id = child_id_generator.fetch_add(1, std::memory_order_relaxed);

            child_to_parent[child_id] = parent.parent_id;

            OrderRequest child_req;
            child_req.order_id = child_id;
            child_req.side = parent.side;
            child_req.order_type = IOC;
            child_req.price = parent.price;
            child_req.quantity = allocated_qty;
            child_req.sender_type = SOR;
            child_req.request_type = RequestType::ORDER;

            venues[target_venue]->route_order(child_req);
        }
    }

    return total_routed;
}

