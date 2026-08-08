#pragma once

#include <atomic>
#include <thread>
#include <deque>
#include <functional>
#include <unordered_map>

#include "common/Types.h"
#include "common/MPSCQueue.h"
#include "common/SPSCQueue.h"
#include "common/Constants.h"
#include "common/SimClock.h"

class AnalyticsEngine {
private:
    MPSCQueue<TradeEvent> trade_inbox;
    SPSCQueue<OrderLifecycleEvent> order_inbox;

    std::deque<TradeEvent> trade_buffer;
    std::unordered_map<OrderID, PendingOrder> pending_orders;
    std::deque<OrderID> decision_order;

    std::atomic<bool> running{false};
    std::thread worker_thread;
    const SimClock* clock = nullptr;
    std::function<void(const ExecutionReport&)> report_callback;

    void worker_loop();
    void handle_trade(const TradeEvent& trade);
    void handle_order_event(const OrderLifecycleEvent& event);
    void finalize_report(OrderID parent_id, bool timed_out);
    void trim_trade_buffer();

public:
    explicit AnalyticsEngine(size_t trade_inbox_capacity = DEFAULT_QUEUE_CAPACITY,
                             size_t order_inbox_capacity = DEFAULT_QUEUE_CAPACITY);
    ~AnalyticsEngine();

    void set_clock(const SimClock* c);

    void on_report(std::function<void(const ExecutionReport&)> cb);

    MPSCQueue<TradeEvent>* get_trade_inbox();
    SPSCQueue<OrderLifecycleEvent>* get_order_inbox();

    uint64_t get_dropped_total() const { return trade_inbox.dropped() + order_inbox.dropped(); }

    void start();
    void stop();
};
