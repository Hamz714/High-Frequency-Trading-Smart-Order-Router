#pragma once

#include <shared_mutex>
#include "common/Types.h"
#include "DPEngine.h"

class SmartOrderRouter {
    RouterConfig config;
    DPEngine dp_engine;

    std::unordered_map<VenueID, Venue*> venues;

    mutable std::shared_mutex state_mutex;

    std::unordered_map<VenueID, LimitOrderBook> mirror_books;
    
    std::unordered_map<OrderID, ParentOrder> active_parent_orders;

    std::unordered_map<OrderID, OrderID> child_to_parent;

    ThreadSafeQueue<OrderRequest> client_inbox;
    ThreadSafeQueue<BookDelta> md_queue;
    ThreadSafeQueue<FillEvent> fill_queue;

    std::atomic<bool> running;
    std::thread md_thread;
    std::thread fill_thread;
    std::thread order_thread;

    void market_data_loop();
    void fill_loop();
    void client_order_loop();

    std::vector<VenueState> build_venue_states() const;

    void execute_routing_decision(const ParentOrder& parent, const SplitResult& split);

    public:
        SmartOrderRouter(const RouterConfig& cfg);
        ~SmartOrderRouter();

        void add_venue(Venue* venue);

        ThreadSafeQueue<BookDelta>* get_md_queue() {return &md_queue;}
        ThreadSafeQueue<FillEvent>* get_fill_queue() {return &fill_queue;}

        void start();
        void stop();

        void submit_order(const OrderRequest& client_request);
};