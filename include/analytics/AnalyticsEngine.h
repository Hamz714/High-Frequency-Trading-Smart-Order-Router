#pragma once

#include <atomic>
#include <thread>
#include <deque>
#include <unordered_map>

#include "common/Types.h"
#include "common/MPSCQueue.h"
#include "common/SPSCQueue.h"
#include "common/Constants.h"
#include "common/SimClock.h"

class AnalyticsEngine {
private:
    MPSCQueue<TradeEvent, QUEUE_SIZE> trade_inbox;
    SPSCQueue<OrderLifecycleEvent, QUEUE_SIZE> order_inbox;

    std::deque<TradeEvent> trade_buffer;
    std::unordered_map<OrderID, PendingOrder> pending_orders;
    std::deque<OrderID> decision_order;

    std::atomic<bool> running{false};
    std::thread worker_thread;
    const SimClock* clock = nullptr;

    void worker_loop();
    void handle_trade(const TradeEvent& trade);
    void handle_order_event(const OrderLifecycleEvent& event);
    void finalize_report(OrderID parent_id, bool timed_out);
    void trim_trade_buffer();
    void print_report(const ExecutionReport& report) const;

public:
    AnalyticsEngine();
    ~AnalyticsEngine();

    void set_clock(const SimClock* c);

    MPSCQueue<TradeEvent, QUEUE_SIZE>* get_trade_inbox();
    SPSCQueue<OrderLifecycleEvent, QUEUE_SIZE>* get_order_inbox();

    void start();
    void stop();
};
