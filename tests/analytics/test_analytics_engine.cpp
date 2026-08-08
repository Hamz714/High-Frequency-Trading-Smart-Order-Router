#include <gtest/gtest.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "analytics/AnalyticsEngine.h"
#include "common/SimClock.h"

namespace {

class AnalyticsEngineTest : public ::testing::Test {
protected:
    AnalyticsEngine engine;
    std::mutex reports_mutex;
    std::unordered_map<OrderID, ExecutionReport> reports;

    void SetUp() override {
        engine.on_report([this](const ExecutionReport& report) {
            std::lock_guard<std::mutex> lock(reports_mutex);
            reports[report.parent_id] = report;
        });
        engine.start();
    }

    void TearDown() override {
        engine.stop();
    }

    void push_trade(VenueID venue, Side side, int64_t price, int64_t qty, double ts, SenderType sender = MM) {
        engine.get_trade_inbox()->push(TradeEvent{
            .venue_id = venue,
            .side = side,
            .price = price,
            .quantity = qty,
            .timestamp = ts,
            .sender_type = sender
        });
    }

    void push_decision(OrderID parent, Side side, int64_t qty, double price, double ts) {
        engine.get_order_inbox()->push(OrderLifecycleEvent{
            .type = OrderEventType::DECISION,
            .parent_id = parent,
            .side = side,
            .quantity = qty,
            .price = price,
            .venue_id = 0,
            .timestamp = ts,
            .timed_out = false
        });
    }

    void push_fill(OrderID parent, Side side, int64_t qty, double price, VenueID venue, double ts) {
        engine.get_order_inbox()->push(OrderLifecycleEvent{
            .type = OrderEventType::FILL,
            .parent_id = parent,
            .side = side,
            .quantity = qty,
            .price = price,
            .venue_id = venue,
            .timestamp = ts,
            .timed_out = false
        });
    }

    void push_completion(OrderID parent, bool timed_out, double ts) {
        engine.get_order_inbox()->push(OrderLifecycleEvent{
            .type = OrderEventType::COMPLETION,
            .parent_id = parent,
            .side = BUY,
            .quantity = 0,
            .price = 0.0,
            .venue_id = 0,
            .timestamp = ts,
            .timed_out = timed_out
        });
    }

    bool wait_for_report(OrderID parent_id, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(reports_mutex);
                if (reports.find(parent_id) != reports.end()) return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    ExecutionReport get_report(OrderID parent_id) {
        std::lock_guard<std::mutex> lock(reports_mutex);
        return reports.at(parent_id);
    }

    bool has_report(OrderID parent_id) {
        std::lock_guard<std::mutex> lock(reports_mutex);
        return reports.find(parent_id) != reports.end();
    }
};

TEST_F(AnalyticsEngineTest, Completion_NoFills_ReportsZeroFillRateAndFallsBackToDecisionPrice) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_completion(1, true, 1.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.fill_rate, 0.0);
    EXPECT_DOUBLE_EQ(report.avg_fill_price, 0.0);
    EXPECT_DOUBLE_EQ(report.window_vwap, 100.0);
    EXPECT_TRUE(report.timed_out);
}

TEST_F(AnalyticsEngineTest, Completion_FullFill_BuySide_ComputesPositiveShortfallWhenOverpaying) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 101.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.fill_rate, 1.0);
    EXPECT_DOUBLE_EQ(report.avg_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(report.implementation_shortfall, 100.0);
    EXPECT_FALSE(report.timed_out);
}

TEST_F(AnalyticsEngineTest, Completion_FullFill_SellSide_ShortfallSignMatchesBuyForABadFill) {
    push_decision(1, SELL, 100, 100.0, 0.0);
    push_fill(1, SELL, 100, 99.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.avg_fill_price, 99.0);
    EXPECT_DOUBLE_EQ(report.implementation_shortfall, 100.0);
}

TEST_F(AnalyticsEngineTest, Completion_PartialFill_ComputesFractionalFillRate) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 40, 100.0, 1, 0.0);
    push_completion(1, true, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.fill_rate, 0.4);
}

TEST_F(AnalyticsEngineTest, Completion_ZeroIntendedQuantity_FillRateIsZeroWithoutDivByZero) {
    push_decision(1, BUY, 0, 100.0, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.fill_rate, 0.0);
}

TEST_F(AnalyticsEngineTest, Completion_FillsAcrossTwoVenues_AggregatesWeightedAvgPriceAndPerVenueBreakdown) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 60, 100.0, 1, 0.0);
    push_fill(1, BUY, 40, 102.0, 2, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.avg_fill_price, 100.8);

    ASSERT_EQ(report.venue_breakdown.count(1), 1u);
    EXPECT_EQ(report.venue_breakdown.at(1).filled_qty, 60);
    EXPECT_EQ(report.venue_breakdown.at(1).fill_count, 1);

    ASSERT_EQ(report.venue_breakdown.count(2), 1u);
    EXPECT_EQ(report.venue_breakdown.at(2).filled_qty, 40);
    EXPECT_EQ(report.venue_breakdown.at(2).fill_count, 1);
}

TEST_F(AnalyticsEngineTest, UnknownParentId_FillEvent_IsSilentlyIgnored) {
    push_fill(999, BUY, 50, 100.0, 1, 0.0);

    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 100.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.fill_rate, 1.0);
    EXPECT_DOUBLE_EQ(report.avg_fill_price, 100.0);
}

TEST_F(AnalyticsEngineTest, UnknownParentId_CompletionEvent_ProducesNoReportAndDoesNotBlockLaterOrders) {
    push_completion(999, false, 0.0);

    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 100.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    EXPECT_FALSE(has_report(999));
}

TEST_F(AnalyticsEngineTest, Trade_ArrivingBeforeAnyDecision_IsDroppedAndExcludedFromWindowVwap) {
    SimClock clock;
    clock.advance(100.0);
    engine.set_clock(&clock);

    push_trade(1, BUY, 99999, 1, -1.0);

    push_decision(1, BUY, 100, 100.0, 0.0);

    push_decision(997, BUY, 1, 1.0, 0.0);
    push_fill(997, BUY, 1, 1.0, 1, 0.0);
    push_completion(997, true, 0.0);
    ASSERT_TRUE(wait_for_report(997));

    push_trade(1, BUY, 105, 50, 1.0);

    push_decision(998, BUY, 1, 1.0, 0.0);
    push_fill(998, BUY, 1, 1.0, 1, 0.0);
    push_completion(998, true, 0.0);
    ASSERT_TRUE(wait_for_report(998));

    push_fill(1, BUY, 100, 101.0, 1, 1.0);
    push_completion(1, false, 2.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.window_vwap, 105.0);
}

TEST_F(AnalyticsEngineTest, Trade_SORSelfTrade_ExcludedFromWindowVwap_ButMMTradeIncluded) {
    SimClock clock;
    clock.advance(100.0);
    engine.set_clock(&clock);

    push_decision(1, BUY, 100, 100.0, 0.0);

    push_decision(997, BUY, 1, 1.0, 0.0);
    push_fill(997, BUY, 1, 1.0, 1, 0.0);
    push_completion(997, true, 0.0);
    ASSERT_TRUE(wait_for_report(997));

    push_trade(1, BUY, 999, 50, 1.0, SOR);
    push_trade(1, BUY, 105, 50, 1.0, MM);

    push_decision(998, BUY, 1, 1.0, 0.0);
    push_fill(998, BUY, 1, 1.0, 1, 0.0);
    push_completion(998, true, 0.0);
    ASSERT_TRUE(wait_for_report(998));

    push_fill(1, BUY, 100, 101.0, 1, 1.0);
    push_completion(1, false, 2.0);

    ASSERT_TRUE(wait_for_report(1));
    ExecutionReport report = get_report(1);

    EXPECT_DOUBLE_EQ(report.window_vwap, 105.0);
}

TEST_F(AnalyticsEngineTest, Start_CalledTwice_DoesNotSpawnSecondWorkerOrCrash) {
    engine.start();

    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 100.0, 1, 0.0);
    push_completion(1, false, 0.0);

    EXPECT_TRUE(wait_for_report(1));
}

}  // namespace
