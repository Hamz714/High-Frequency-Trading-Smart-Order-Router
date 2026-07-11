#include "SmartOrderRouter.h"

SmartOrderRouter::SmartOrderRouter(const RouterConfig& cfg):
    config(cfg), dp_engine(cfg), running(false) {}

SmartOrderRouter::~SmartOrderRouter() {
    stop();
}

void SmartOrderRouter::add_venue(Venue* venue) {
    VenueID id = venue->get_id(); 
    
    venues[id] = venue;

    venue_md_queues[id] = std::make_unique<SPSCQueue<BookDelta, QUEUE_SIZE>>();
    venue_fill_queues[id] = std::make_unique<SPSCQueue<FillEvent, QUEUE_SIZE>>();

    mirror_books[id] = LimitOrderBook();

    venue->set_sor_queues(venue_md_queues[id].get(), venue_fill_queues[id].get());
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

void SmartOrderRouter::market_data_loop() {
    BookDelta delta; 

    while (running.load(std::memory_order_relaxed)) {
        bool idle = true;

        for (auto& [venue_id, queue] : venue_md_queues) {
            
            int processed_count = 0;

            while (processed_count < BATCH_LIMIT && queue->try_pop(delta)) {
                idle = false;
                processed_count++;
                
                std::unique_lock<std::shared_mutex> lock(state_mutex);
                mirror_books[venue_id].apply_delta(delta);
            }
        }

        if (idle) {
            _mm_pause();
        }
    }
}

void SmartOrderRouter::fill_loop() {
    FillEvent fill;
    
    while (running.load(std::memory_order_relaxed)) {
        bool idle = true;

        for (auto& [venue_id, queue] : venue_fill_queues) {
            
            int processed_count = 0;

            while (processed_count < BATCH_LIMIT && queue->try_pop(fill)) {
                idle = false;
                processed_count++;
                
                std::unique_lock<std::shared_mutex> lock(state_mutex);

                auto parent_it = child_to_parent.find(fill.child_id);
                if (parent_it == child_to_parent.end()) continue; 

                OrderID parent_id = parent_it->second;
                ParentOrder& parent = active_parent_orders[parent_id];

                parent.filled_qty += fill.filled_quantity;

                if (fill.status == FILLED || fill.status == CANCELLED) {
                    

                    if (fill.remaining_quantity > 0) {
                        
                        SplitResult new_split = dp_engine.compute_optimal_split(
                            fill.remaining_quantity, 
                            parent.side,
                            parent.price,
                            venue_states
                        );
                        
                        execute_routing_decision(parent, new_split);
                    }

                    if (parent.filled_qty == parent.total_qty) {
                        active_parent_orders.erase(parent_id);
                    }

                    child_to_parent.erase(parent_it);
                }
            }
        }

        if (idle) {
            _mm_pause();
        }
    }
}

void SmartOrderRouter::client_order_loop() {
    OrderRequest req;
    
    while (running.load(std::memory_order_relaxed)) {

        if (client_inbox.try_pop(req)) {
            
            std::unique_lock<std::shared_mutex> lock(state_mutex);

            ParentOrder parent;
            parent.parent_id = req.order_id;
            parent.side = req.side;
            parent.price = req.price;
            parent.total_qty = req.quantity;
            parent.filled_qty = 0;

            active_parent_orders[parent.parent_id] = parent;

            SplitResult split = dp_engine.compute_optimal_split(
                req.quantity, 
                req.side, 
                req.price, 
                venue_states
            );

            execute_routing_decision(active_parent_orders[parent.parent_id], split);
            
        } else {
            std::this_thread::yield(); 
        }
    }
}

void SmartOrderRouter::execute_routing_decision(const ParentOrder& parent, const SplitResult& split) {
                                                    
    static std::atomic<uint64_t> child_id_generator{1000000};

    for (size_t i = 0; i < split.allocations.size(); ++i) {
        int64_t allocated_qty = split.allocations[i];
        
        if (allocated_qty > 0) {
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
}

