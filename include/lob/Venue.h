#pragma once

#include <thread>
#include <atomic>
#include <deque>

#include "common/CpuRelax.h"
#include "common/Types.h"
#include "lob/LimitOrderBook.h"
#include "common/SPSCQueue.h"
#include "common/MPSCQueue.h"
#include "common/SimClock.h"
#include "common/WireClock.h"

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
    bool simulate_latency;
    LimitOrderBook lob;

    MPSCQueue<OrderRequest> inbox;
    std::thread worker_thread;
    std::atomic<bool> running{false};

    SPSCQueue<BookDelta>* market_data_queue = nullptr;
    SPSCQueue<FillEvent>* sor_fill_queue = nullptr;
    SPSCQueue<FillEvent>* mm_fill_queue = nullptr;

    MPSCQueue<TradeEvent>* analytics_trade_queue = nullptr;
    const SimClock* clock = nullptr;

    void worker_loop();
    void process_request(const OrderRequest& req);

    int64_t stamp_arrival(SenderType sender) const;
    int64_t stamp_publication() const;

    public:
        Venue(int id, const VenueConfig& cfg, bool simulate_latency = true,
              size_t order_pool_capacity = DEFAULT_ORDER_POOL_CAPACITY,
              size_t inbox_capacity = DEFAULT_QUEUE_CAPACITY);
        ~Venue();

        void set_sor_queues(SPSCQueue<BookDelta>* md_queue,
                    SPSCQueue<FillEvent>* fill_queue);

        void set_mm_fill_queue(SPSCQueue<FillEvent>* fill_queue);

        void set_analytics_queue(MPSCQueue<TradeEvent>* trade_queue);

        void set_clock(const SimClock* c);

        void start();
        void stop();

        int get_id() const;
        VenueType get_type() const;
        const VenueConfig& get_config() const;

        uint64_t get_dropped_total() const { return inbox.dropped(); }

    void route_order(const OrderRequest& req);
};