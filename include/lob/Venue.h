#pragma once

#include <thread>
#include <immintrin.h>

#include "common/Types.h"
#include "lob/LimitOrderBook.h"
#include "common/SPSCQueue.h"

class Venue {
    VenueID venue_id;
    VenueConfig config;
    LimitOrderBook lob;

    SPSCQueue<OrderRequest, QUEUE_SIZE> inbox;
    std::thread worker_thread;

    SPSCQueue<BookDelta, QUEUE_SIZE>* market_data_queue;
    SPSCQueue<FillEvent, QUEUE_SIZE>* sor_fill_queue;

    void worker_loop();

    public:
        Venue(int id, const VenueConfig& cfg,
            SPSCQueue<BookDelta, QUEUE_SIZE>* md_queue,
            SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue);

        void start();

        int get_id() const;
        VenueType get_type() const;
        const VenueConfig& get_config() const;

        void route_order(const OrderRequest& req);
};