#pragma once

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <immintrin.h>

#include "common/Types.h"
#include "common/SPSCQueue.h"
#include "common/MPSCQueue.h"
#include "lob/Venue.h"
#include "sor/DPEngine.h"
#include "common/Constants.h"
#include "common/SimClock.h"

class SmartOrderRouter {
private:
    RouterConfig config;
    DPEngine dp_engine;

    std::unordered_map<VenueID, Venue*> venues;

    mutable std::shared_mutex book_mutex;
    std::mutex order_mutex;

    std::unordered_map<VenueID, LimitOrderBook> mirror_books;
    std::unordered_map<OrderID, ParentOrder> active_parent_orders;
    std::unordered_map<OrderID, OrderID> child_to_parent;

    MPSCQueue<OrderRequest, QUEUE_SIZE> client_inbox;

    std::unordered_map<VenueID, std::unique_ptr<SPSCQueue<BookDelta, QUEUE_SIZE>>> venue_md_queues;
    std::unordered_map<VenueID, std::unique_ptr<SPSCQueue<FillEvent, QUEUE_SIZE>>> venue_fill_queues;

    std::vector<VenueState> venue_states;

    const SimClock* clock = nullptr;
    SPSCQueue<OrderLifecycleEvent, QUEUE_SIZE>* analytics_queue = nullptr;

    std::atomic<bool> running;
    std::thread md_thread;
    std::thread fill_thread;
    std::thread order_thread;

    void market_data_loop();
    void fill_loop();
    void client_order_loop();

    void initialise_venue_states();
    int64_t execute_routing_decision(const ParentOrder& parent, const SplitResult& split);
    SplitResult compute_split(int64_t size, Side side, int64_t worst_price);
    double compute_consolidated_mid() const;

public:
    SmartOrderRouter(const RouterConfig& cfg);
    ~SmartOrderRouter();

    void add_venue(Venue* venue);

    void set_clock(const SimClock* c);
    void set_analytics_queue(SPSCQueue<OrderLifecycleEvent, QUEUE_SIZE>* queue);

    void start();
    void stop();

    void submit_order(const OrderRequest& client_request);

    uint64_t get_dropped_total() const;
};