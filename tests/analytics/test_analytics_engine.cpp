#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "analytics/AnalyticsEngine.h"
#include "common/SimClock.h"

namespace {

class ThreadSafeStreamBuf : public std::streambuf {
public:
    std::string str_safe() {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_;
    }

protected:
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            std::lock_guard<std::mutex> lock(mutex_);
            buffer_.push_back(static_cast<char>(ch));
        }
        return ch;
    }

private:
    std::mutex mutex_;
    std::string buffer_;
};

class AnalyticsEngineTest : public ::testing::Test {
protected:
    AnalyticsEngine engine;
    ThreadSafeStreamBuf captured;
    std::streambuf* original_cout_buf = nullptr;

    void SetUp() override {
        original_cout_buf = std::cout.rdbuf(&captured);
        engine.start();
    }

    void TearDown() override {
        engine.stop();
        std::cout.rdbuf(original_cout_buf);
    }

    void push_trade(VenueID venue, Side side, int64_t price, int64_t qty, double ts) {
        engine.get_trade_inbox()->push(TradeEvent{
            .venue_id = venue,
            .side = side,
            .price = price,
            .quantity = qty,
            .timestamp = ts,
            .sender_type = MM
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
        std::string marker = "RESULT,parent_id=" + std::to_string(parent_id) + ",";
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string text = captured.str_safe();
            size_t pos = text.find(marker);
            if (pos != std::string::npos && text.find('\n', pos) != std::string::npos) {
                return true;
            }
            std::this_thread::yield();
        }
        return false;
    }

    std::string extract_result_line(OrderID parent_id) {
        std::string text = captured.str_safe();
        std::string marker = "RESULT,parent_id=" + std::to_string(parent_id) + ",";
        size_t pos = text.find(marker);
        if (pos == std::string::npos) return "";
        size_t end = text.find('\n', pos);
        return text.substr(pos, end - pos);
    }

    std::string extract_report_block(OrderID parent_id) {
        std::string text = captured.str_safe();
        std::string start_marker = "Execution Report for Order " + std::to_string(parent_id) + "\n";
        size_t start = text.find(start_marker);
        if (start == std::string::npos) return "";
        std::string end_marker = "RESULT,parent_id=" + std::to_string(parent_id) + ",";
        size_t end = text.find(end_marker, start);
        if (end == std::string::npos) return "";
        size_t line_end = text.find('\n', end);
        return text.substr(start, line_end - start);
    }

    double extract_field(const std::string& line, const std::string& key) {
        size_t pos = line.find(key);
        if (pos == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
        pos += key.size();
        size_t end = line.find_first_of(",\n", pos);
        return std::stod(line.substr(pos, end - pos));
    }
};

TEST_F(AnalyticsEngineTest, Completion_NoFills_ReportsZeroFillRateAndFallsBackToDecisionPrice) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_completion(1, true, 1.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "fill_rate="), 0.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "avg_fill_price="), 0.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "window_vwap="), 100.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "timed_out="), 1.0);
}

TEST_F(AnalyticsEngineTest, Completion_FullFill_BuySide_ComputesPositiveShortfallWhenOverpaying) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 101.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "fill_rate="), 1.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "avg_fill_price="), 101.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "implementation_shortfall="), 100.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "timed_out="), 0.0);
}

TEST_F(AnalyticsEngineTest, Completion_FullFill_SellSide_ShortfallSignMatchesBuyForABadFill) {
    push_decision(1, SELL, 100, 100.0, 0.0);
    push_fill(1, SELL, 100, 99.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "avg_fill_price="), 99.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "implementation_shortfall="), 100.0);
}

TEST_F(AnalyticsEngineTest, Completion_PartialFill_ComputesFractionalFillRate) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 40, 100.0, 1, 0.0);
    push_completion(1, true, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "fill_rate="), 0.4);
}

TEST_F(AnalyticsEngineTest, Completion_ZeroIntendedQuantity_FillRateIsZeroWithoutDivByZero) {
    push_decision(1, BUY, 0, 100.0, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "fill_rate="), 0.0);
}

TEST_F(AnalyticsEngineTest, Completion_FillsAcrossTwoVenues_AggregatesWeightedAvgPriceAndPerVenueBreakdown) {
    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 60, 100.0, 1, 0.0);
    push_fill(1, BUY, 40, 102.0, 2, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string result_line = extract_result_line(1);
    std::string report_block = extract_report_block(1);

    EXPECT_DOUBLE_EQ(extract_field(result_line, "avg_fill_price="), 100.8);
    EXPECT_NE(report_block.find("Venue 1: qty=60 fills=1"), std::string::npos);
    EXPECT_NE(report_block.find("Venue 2: qty=40 fills=1"), std::string::npos);
}

TEST_F(AnalyticsEngineTest, UnknownParentId_FillEvent_IsSilentlyIgnored) {
    push_fill(999, BUY, 50, 100.0, 1, 0.0);

    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 100.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "fill_rate="), 1.0);
    EXPECT_DOUBLE_EQ(extract_field(line, "avg_fill_price="), 100.0);
}

TEST_F(AnalyticsEngineTest, UnknownParentId_CompletionEvent_ProducesNoReportAndDoesNotBlockLaterOrders) {
    push_completion(999, false, 0.0);

    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 100.0, 1, 0.0);
    push_completion(1, false, 0.0);

    ASSERT_TRUE(wait_for_report(1));
    EXPECT_EQ(captured.str_safe().find("RESULT,parent_id=999,"), std::string::npos);
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
    std::string line = extract_result_line(1);

    EXPECT_DOUBLE_EQ(extract_field(line, "window_vwap="), 105.0);
}

TEST_F(AnalyticsEngineTest, Start_CalledTwice_DoesNotSpawnSecondWorkerOrCrash) {
    engine.start();

    push_decision(1, BUY, 100, 100.0, 0.0);
    push_fill(1, BUY, 100, 100.0, 1, 0.0);
    push_completion(1, false, 0.0);

    EXPECT_TRUE(wait_for_report(1));
}

}  // namespace
