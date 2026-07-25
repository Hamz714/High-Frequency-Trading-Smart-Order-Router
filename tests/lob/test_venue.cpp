#include <gtest/gtest.h>
#include <chrono>
#include <memory>

#include "lob/Venue.h"
#include "common/Types.h"
#include "common/Constants.h"
#include "common/SimClock.h"

namespace {

template <typename Queue, typename T>
bool wait_pop(Queue& queue, T& out, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (queue.try_pop(out)) return true;
    }
    return false;
}

VenueConfig make_config(VenueType type) {
    return VenueConfig{
        .type = type,
        .fee_per_share = 0.0,
        .latency_us = 0,
        .impact_coefficient = 0.0,
        .historical_fill_ratio = 0.0
    };
}

OrderRequest make_order(OrderID id, SenderType sender, Side side, OrderType type, int64_t price, int64_t qty) {
    return OrderRequest{
        .order_id = id,
        .sender_type = sender,
        .request_type = RequestType::ORDER,
        .side = side,
        .order_type = type,
        .price = price,
        .quantity = qty
    };
}

OrderRequest make_cancel(OrderID lob_order_id, SenderType sender) {
    return OrderRequest{
        .order_id = lob_order_id,
        .sender_type = sender,
        .request_type = RequestType::CANCEL,
        .side = BUY,
        .order_type = LIMIT,
        .price = 0,
        .quantity = 0
    };
}

class VenueTest : public ::testing::Test {
protected:
    SPSCQueue<BookDelta, QUEUE_SIZE> market_data_queue;
    SPSCQueue<FillEvent, QUEUE_SIZE> sor_fill_queue;
    SPSCQueue<FillEvent, QUEUE_SIZE> mm_fill_queue;
    MPSCQueue<TradeEvent, QUEUE_SIZE> analytics_queue;

    std::unique_ptr<Venue> venue;

    void make_venue(VenueType type = LIT) {
        venue = std::make_unique<Venue>(1, make_config(type));
        venue->set_sor_queues(&market_data_queue, &sor_fill_queue);
        venue->set_mm_fill_queue(&mm_fill_queue);
        venue->set_analytics_queue(&analytics_queue);
        venue->start();
    }

    void TearDown() override {
        if (venue) venue->stop();
    }
};

TEST_F(VenueTest, Accessors_ReturnConstructedValues) {
    make_venue(LIT);

    EXPECT_EQ(venue->get_id(), 1);
    EXPECT_EQ(venue->get_type(), LIT);
    EXPECT_EQ(venue->get_config().type, LIT);
}

TEST_F(VenueTest, RouteOrder_SOR_RestingLimit_PublishesZeroFillPartialStatus) {
    make_venue();
    venue->route_order(make_order(1, SOR, BUY, LIMIT, 100, 10));

    FillEvent fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, fe));
    EXPECT_EQ(fe.child_id, 1);
    EXPECT_NE(fe.lob_order_id, -1);
    EXPECT_EQ(fe.filled_quantity, 0);
    EXPECT_EQ(fe.fill_price, 100);
    EXPECT_EQ(fe.remaining_quantity, 10);
    EXPECT_EQ(fe.status, PARTIAL);
}

TEST_F(VenueTest, RouteOrder_SOR_FullMatch_PublishesFilledStatus) {
    make_venue();
    venue->route_order(make_order(1, MM, SELL, LIMIT, 100, 10));
    FillEvent mm_fe;
    ASSERT_TRUE(wait_pop(mm_fill_queue, mm_fe));

    venue->route_order(make_order(2, SOR, BUY, LIMIT, 100, 10));

    FillEvent fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, fe));
    EXPECT_EQ(fe.child_id, 2);
    EXPECT_EQ(fe.filled_quantity, 10);
    EXPECT_EQ(fe.fill_price, 100);
    EXPECT_EQ(fe.remaining_quantity, 0);
    EXPECT_EQ(fe.status, FILLED);
    EXPECT_EQ(fe.lob_order_id, -1);
}

TEST_F(VenueTest, RouteOrder_SOR_PartialMatchThenRest_SecondEventCarriesRestingId) {
    make_venue();
    venue->route_order(make_order(1, MM, SELL, LIMIT, 100, 10));
    FillEvent mm_fe;
    ASSERT_TRUE(wait_pop(mm_fill_queue, mm_fe));

    venue->route_order(make_order(2, SOR, BUY, LIMIT, 100, 30));

    FillEvent match_fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, match_fe));
    EXPECT_EQ(match_fe.filled_quantity, 10);
    EXPECT_EQ(match_fe.remaining_quantity, 20);
    EXPECT_EQ(match_fe.status, PARTIAL);

    FillEvent rest_fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, rest_fe));
    EXPECT_EQ(rest_fe.filled_quantity, 0);
    EXPECT_EQ(rest_fe.remaining_quantity, 20);
    EXPECT_EQ(rest_fe.status, PARTIAL);
    EXPECT_NE(rest_fe.lob_order_id, -1);
    EXPECT_EQ(rest_fe.lob_order_id, match_fe.lob_order_id);
}

TEST_F(VenueTest, RouteOrder_SOR_FOK_Unfillable_PublishesCancelledWithFullRemaining) {
    make_venue();
    venue->route_order(make_order(1, SOR, BUY, FOK, 100, 20));

    FillEvent fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, fe));
    EXPECT_EQ(fe.filled_quantity, 0);
    EXPECT_EQ(fe.remaining_quantity, 20);
    EXPECT_EQ(fe.status, CANCELLED);
    EXPECT_EQ(fe.lob_order_id, -1);
}

TEST_F(VenueTest, RouteOrder_MM_PublishesToMmQueueOnly) {
    make_venue();
    venue->route_order(make_order(1, MM, BUY, LIMIT, 100, 10));

    FillEvent fe;
    ASSERT_TRUE(wait_pop(mm_fill_queue, fe));
    EXPECT_EQ(fe.child_id, 1);
    EXPECT_EQ(fe.status, PARTIAL);

    FillEvent unused;
    EXPECT_FALSE(sor_fill_queue.try_pop(unused));
}

TEST_F(VenueTest, RouteOrder_Cancel_RemovesRestingOrder) {
    make_venue();
    venue->route_order(make_order(1, SOR, SELL, LIMIT, 100, 10));

    FillEvent rest_fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, rest_fe));
    ASSERT_NE(rest_fe.lob_order_id, -1);

    venue->route_order(make_cancel(rest_fe.lob_order_id, SOR));
    venue->route_order(make_order(2, SOR, BUY, IOC, 100, 10));

    FillEvent ioc_fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, ioc_fe));
    EXPECT_EQ(ioc_fe.filled_quantity, 0);
    EXPECT_EQ(ioc_fe.status, CANCELLED);
}

TEST_F(VenueTest, RouteOrder_PublishesTradeEventOnMatch) {
    make_venue();
    venue->route_order(make_order(1, MM, SELL, LIMIT, 100, 10));
    FillEvent mm_fe;
    ASSERT_TRUE(wait_pop(mm_fill_queue, mm_fe));

    TradeEvent rest_trade;
    ASSERT_TRUE(wait_pop(analytics_queue, rest_trade));
    EXPECT_EQ(rest_trade.quantity, 0);

    venue->route_order(make_order(2, SOR, BUY, LIMIT, 100, 10));
    FillEvent sor_fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, sor_fe));

    TradeEvent match_trade;
    ASSERT_TRUE(wait_pop(analytics_queue, match_trade));
    EXPECT_EQ(match_trade.venue_id, venue->get_id());
    EXPECT_EQ(match_trade.side, BUY);
    EXPECT_EQ(match_trade.price, 100);
    EXPECT_EQ(match_trade.quantity, 10);
    EXPECT_EQ(match_trade.sender_type, SOR);
}

TEST_F(VenueTest, DarkVenue_DoesNotPublishBookUpdates) {
    make_venue(DARK);
    venue->route_order(make_order(1, SOR, BUY, LIMIT, 100, 10));

    FillEvent fe;
    ASSERT_TRUE(wait_pop(sor_fill_queue, fe));

    BookDelta delta;
    EXPECT_FALSE(market_data_queue.try_pop(delta));
}

TEST_F(VenueTest, LitVenue_PublishesBookUpdateOnRest) {
    make_venue(LIT);
    venue->route_order(make_order(1, SOR, BUY, LIMIT, 100, 10));

    BookDelta delta;
    ASSERT_TRUE(wait_pop(market_data_queue, delta));
    EXPECT_EQ(delta.venue_id, venue->get_id());
    EXPECT_EQ(delta.side, BUY);
    EXPECT_EQ(delta.price, 100);
    EXPECT_EQ(delta.new_quantity, 10);
}

TEST_F(VenueTest, RouteOrder_UsesClockForTradeTimestamp_WhenSet) {
    make_venue();
    SimClock clock;
    clock.advance(42.5);
    venue->set_clock(&clock);

    venue->route_order(make_order(1, SOR, BUY, LIMIT, 100, 10));

    TradeEvent trade;
    ASSERT_TRUE(wait_pop(analytics_queue, trade));
    EXPECT_DOUBLE_EQ(trade.timestamp, 42.5);
}

TEST_F(VenueTest, RouteOrder_TradeTimestampDefaultsToZero_WithoutClock) {
    make_venue();
    venue->route_order(make_order(1, SOR, BUY, LIMIT, 100, 10));

    TradeEvent trade;
    ASSERT_TRUE(wait_pop(analytics_queue, trade));
    EXPECT_DOUBLE_EQ(trade.timestamp, 0.0);
}

}  // namespace
