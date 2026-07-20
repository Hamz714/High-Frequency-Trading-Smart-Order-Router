#include "analytics/AnalyticsEngine.h"

#include <iostream>
#include <iomanip>
#include <immintrin.h>

AnalyticsEngine::AnalyticsEngine() {}

AnalyticsEngine::~AnalyticsEngine() {
    stop();
}

void AnalyticsEngine::set_clock(const SimClock* c) {
    this->clock = c;
}

MPSCQueue<TradeEvent, QUEUE_SIZE>* AnalyticsEngine::get_trade_inbox() {
    return &trade_inbox;
}

SPSCQueue<OrderLifecycleEvent, QUEUE_SIZE>* AnalyticsEngine::get_order_inbox() {
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
            _mm_pause();
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
        if (trade.timestamp >= order.decision_time && trade.timestamp <= completion_time) {
            window_notional += static_cast<double>(trade.price) * static_cast<double>(trade.quantity);
            window_qty += trade.quantity;
        }
    }
    report.window_vwap = window_qty > 0 ? window_notional / static_cast<double>(window_qty) : report.decision_price;

    double side_sign = (order.side == Side::BUY) ? 1.0 : -1.0;
    report.implementation_shortfall = side_sign * (report.avg_fill_price - report.decision_price) * static_cast<double>(report.filled_size);
    report.vwap_slippage = side_sign * (report.avg_fill_price - report.window_vwap);

    print_report(report);

    pending_orders.erase(it);
    trim_trade_buffer();
}

void AnalyticsEngine::print_report(const ExecutionReport& report) const {
    std::cout << "\n[ ANALYTICS      ] Execution Report for Order " << report.parent_id << "\n";
    std::cout << "  Side:                     " << (report.side == Side::BUY ? "BUY" : "SELL") << "\n";
    std::cout << "  Intended / Filled:        " << report.intended_size << " / " << report.filled_size << "\n";
    std::cout << "  Fill Rate:                " << std::fixed << std::setprecision(2) << (report.fill_rate * 100.0) << "%\n";
    std::cout << "  Decision Price:           " << report.decision_price << "\n";
    std::cout << "  Avg Fill Price:           " << report.avg_fill_price << "\n";
    std::cout << "  Window VWAP:              " << report.window_vwap << "\n";
    std::cout << "  Implementation Shortfall: " << report.implementation_shortfall << "\n";
    std::cout << "  VWAP Slippage:            " << report.vwap_slippage << "\n";
    std::cout << "  Timed Out:                " << (report.timed_out ? "yes" : "no") << "\n";
    std::cout << "  Venue Breakdown:\n";
    for (const auto& [venue_id, stats] : report.venue_breakdown) {
        double avg_price = stats.filled_qty > 0 ? stats.total_notional / static_cast<double>(stats.filled_qty) : 0.0;
        std::cout << "    Venue " << venue_id << ": qty=" << stats.filled_qty
                  << " fills=" << stats.fill_count << " avg_price=" << avg_price << "\n";
    }

    std::cout << "RESULT,parent_id=" << report.parent_id
              << ",side=" << (report.side == Side::BUY ? "BUY" : "SELL")
              << ",intended=" << report.intended_size
              << ",filled=" << report.filled_size
              << ",fill_rate=" << report.fill_rate
              << ",decision_price=" << report.decision_price
              << ",avg_fill_price=" << report.avg_fill_price
              << ",window_vwap=" << report.window_vwap
              << ",implementation_shortfall=" << report.implementation_shortfall
              << ",vwap_slippage=" << report.vwap_slippage
              << ",timed_out=" << (report.timed_out ? 1 : 0)
              << "\n";
}
