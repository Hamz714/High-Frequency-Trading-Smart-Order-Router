#pragma once
#include <thread>
#include "common/Types.h"
#include "lob/LimitOrderBook.h"
#include "common/ThreadSafeQueue.h"

class Venue {
    VenueID venue_id;
    VenueConfig config;
    LimitOrderBook lob;

    ThreadSafeQueue<OrderRequest> inbox;
    std::thread worker_thread;

    ThreadSafeQueue<BookDelta>* market_data_queue;
    ThreadSafeQueue<FillEvent>* sor_fill_queue;

    void worker_loop();

    public:
        Venue(int id, const VenueConfig& cfg,
            ThreadSafeQueue<BookDelta>* md_queue,
            ThreadSafeQueue<FillEvent>* fill_queue);

        void start();

        int get_id() const;
        VenueType get_type() const;
        const VenueConfig& get_config() const;

        void route_order(const OrderRequest& req);
};