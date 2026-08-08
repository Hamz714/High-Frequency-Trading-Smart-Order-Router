#include "analytics/AnalyticsEngine.h"

#include "common/CpuRelax.h"

AnalyticsEngine::AnalyticsEngine(size_t trade_inbox_capacity, size_t order_inbox_capacity)
    : trade_inbox(trade_inbox_capacity), order_inbox(order_inbox_capacity) {}

AnalyticsEngine::~AnalyticsEngine() {
    stop();
}

void AnalyticsEngine::set_clock(const SimClock* c) {
    this->clock = c;
}

void AnalyticsEngine::on_report(std::function<void(const ExecutionReport&)> cb) {
    report_callback = std::move(cb);
}

MPSCQueue<TradeEvent>* AnalyticsEngine::get_trade_inbox() {
    return &trade_inbox;
}

SPSCQueue<OrderLifecycleEvent>* AnalyticsEngine::get_order_inbox() {
    return &order_inbox;
}

void AnalyticsEngine::start() {
    if (running.exchange(true)) return;
    worker_thread = std::thread(&AnalyticsEngine::worker_loop, this);
}

void AnalyticsEngine::stop() {
    if (!running.exchange(false)) return;
    if (worker_thread.joinable()) {
        worker_thread.join();
    }
}

void AnalyticsEngine::worker_loop() {
    TradeEvent trade;
    OrderLifecycleEvent event;

    while (running.load(std::memory_order_relaxed)) {
        bool idle = true;

        int processed = 0;
        while (processed < BATCH_LIMIT && trade_inbox.try_pop(trade)) {
            handle_trade(trade);
            idle = false;
            processed++;
        }

        processed = 0;
        while (processed < BATCH_LIMIT && order_inbox.try_pop(event)) {
            handle_order_event(event);
            idle = false;
            processed++;
        }

        if (idle) {
            cpu_relax();
        }
    }
}

void AnalyticsEngine::handle_trade(const TradeEvent& trade) {
    if (pending_orders.empty()) return;
    trade_buffer.push_back(trade);
}

void AnalyticsEngine::trim_trade_buffer() {
    while (!decision_order.empty() && pending_orders.find(decision_order.front()) == pending_orders.end()) {
        decision_order.pop_front();
    }

    if (decision_order.empty()) {
        trade_buffer.clear();
        return;
    }

    double earliest_decision = pending_orders.at(decision_order.front()).decision_time;

    while (!trade_buffer.empty() && trade_buffer.front().timestamp < earliest_decision) {
        trade_buffer.pop_front();
    }
}

void AnalyticsEngine::handle_order_event(const OrderLifecycleEvent& event) {
    switch (event.type) {
        case OrderEventType::DECISION: {
            PendingOrder order;
            order.side = event.side;
            order.intended_qty = event.quantity;
            order.decision_price = event.price;
            order.decision_time = event.timestamp;
            pending_orders[event.parent_id] = order;
            decision_order.push_back(event.parent_id);
            break;
        }
        case OrderEventType::FILL: {
            auto it = pending_orders.find(event.parent_id);
            if (it == pending_orders.end()) break;

            PendingOrder& order = it->second;
            order.filled_qty += event.quantity;
            order.filled_notional += event.price * static_cast<double>(event.quantity);

            VenueStats& stats = order.venue_breakdown[event.venue_id];
            stats.filled_qty += event.quantity;
            stats.total_notional += event.price * static_cast<double>(event.quantity);
            stats.fill_count += 1;
            break;
        }
        case OrderEventType::COMPLETION: {
            finalize_report(event.parent_id, event.timed_out);
            break;
        }
    }
}

void AnalyticsEngine::finalize_report(OrderID parent_id, bool timed_out) {
    auto it = pending_orders.find(parent_id);
    if (it == pending_orders.end()) return;

    PendingOrder& order = it->second;

    ExecutionReport report;
    report.parent_id = parent_id;
    report.side = order.side;
    report.intended_size = order.intended_qty;
    report.filled_size = order.filled_qty;
    report.fill_rate = order.intended_qty > 0
        ? static_cast<double>(order.filled_qty) / static_cast<double>(order.intended_qty)
        : 0.0;
    report.avg_fill_price = order.filled_qty > 0
        ? order.filled_notional / static_cast<double>(order.filled_qty)
        : 0.0;
    report.decision_price = order.decision_price;
    report.timed_out = timed_out;
    report.venue_breakdown = order.venue_breakdown;

    double completion_time = clock ? clock->now() : order.decision_time;

    double window_notional = 0.0;
    int64_t window_qty = 0;
    for (const auto& trade : trade_buffer) {
        if (trade.timestamp >= order.decision_time && trade.timestamp <= completion_time
            && trade.sender_type != SenderType::SOR) {
            window_notional += static_cast<double>(trade.price) * static_cast<double>(trade.quantity);
            window_qty += trade.quantity;
        }
    }
    report.window_vwap = window_qty > 0 ? window_notional / static_cast<double>(window_qty) : report.decision_price;

    double side_sign = (order.side == Side::BUY) ? 1.0 : -1.0;
    report.implementation_shortfall = side_sign * (report.avg_fill_price - report.decision_price) * static_cast<double>(report.filled_size);
    report.vwap_slippage = side_sign * (report.avg_fill_price - report.window_vwap);

    if (report_callback) {
        report_callback(report);
    }

    pending_orders.erase(it);
    trim_trade_buffer();
}
