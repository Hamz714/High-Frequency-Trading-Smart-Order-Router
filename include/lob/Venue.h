#pragma once

#include <thread>
#include <atomic>

#include "common/CpuRelax.h"
#include "common/Types.h"
#include "lob/LimitOrderBook.h"
#include "common/SPSCQueue.h"
#include "common/MPSCQueue.h"
#include "common/SimClock.h"

struct VenueState {
    VenueID venue_id;
    VenueConfig config;
    const LimitOrderBook* local_lob;

    int64_t get_visible_liquidity(Side side, int64_t worst_price) const {
        if (!local_lob || config.type == DARK) return 0;
        return local_lob->available_liquidity(side, worst_price); 
    }

    double half_spread() const {
        if (!local_lob) return 0.0;
        return local_lob->half_spread();
    }

    int64_t get_best_bid() const {
        if (!local_lob || config.type == DARK) return 0;
        return local_lob->get_best_bid();
    }

    int64_t get_best_ask() const {
        if (!local_lob || config.type == DARK) return std::numeric_limits<int64_t>::max();
        return local_lob->get_best_ask();
    }
};

class Venue {
    VenueID venue_id;
    VenueConfig config;
    LimitOrderBook lob;

    MPSCQueue<OrderRequest, QUEUE_SIZE> inbox;
    std::thread worker_thread;
    std::atomic<bool> running{false};

    SPSCQueue<BookDelta, QUEUE_SIZE>* market_data_queue = nullptr;
    SPSCQueue<FillEvent, QUEUE_SIZE>* sor_fill_queue = nullptr;
    SPSCQueue<FillEvent, QUEUE_SIZE>* mm_fill_queue = nullptr;

    MPSCQueue<TradeEvent, QUEUE_SIZE>* analytics_trade_queue = nullptr;
    const SimClock* clock = nullptr;

    void worker_loop();

    public:
        Venue(int id, const VenueConfig& cfg);
        ~Venue();

        void set_sor_queues(SPSCQueue<BookDelta, QUEUE_SIZE>* md_queue,
                    SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue);

        void set_mm_fill_queue(SPSCQueue<FillEvent, QUEUE_SIZE>* fill_queue);

        void set_analytics_queue(MPSCQueue<TradeEvent, QUEUE_SIZE>* trade_queue);

        void set_clock(const SimClock* c);

        void start();
        void stop();

        int get_id() const;
        VenueType get_type() const;
        const VenueConfig& get_config() const;

        uint64_t get_dropped_total() const { return inbox.dropped(); }

    void route_order(const OrderRequest& req);
};