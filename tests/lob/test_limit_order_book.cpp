#include <gtest/gtest.h>
#include <limits>
#include <vector>
#include <tuple>

#include "lob/LimitOrderBook.h"
#include "common/Types.h"

namespace {

constexpr int64_t kMax = std::numeric_limits<int64_t>::max();

class LimitOrderBookTest : public ::testing::Test {
protected:
    LimitOrderBook book;
};

TEST_F(LimitOrderBookTest, EmptyBook_InitialState) {
    EXPECT_EQ(book.get_best_bid(), 0);
    EXPECT_EQ(book.get_best_ask(), kMax);
    EXPECT_DOUBLE_EQ(book.half_spread(), 0.0);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 0);
    EXPECT_EQ(book.available_liquidity(Side::BUY, 1000), 0);
    EXPECT_EQ(book.available_liquidity(Side::SELL, 1), 0);

    BookSnapshot snap = book.get_snapshot(5);
    EXPECT_TRUE(snap.bids.empty());
    EXPECT_TRUE(snap.asks.empty());
}

TEST_F(LimitOrderBookTest, Submit_LimitBuy_RestsWhenBookEmpty) {
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 10);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 0);
    EXPECT_EQ(fills[0].fill_price, 100);
    EXPECT_NE(fills[0].order_id, 0);

    EXPECT_EQ(book.get_best_bid(), 100);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 10);
}

TEST_F(LimitOrderBookTest, Submit_LimitSell_RestsWhenBookEmpty) {
    std::vector<Fill> fills = book.submit(Side::SELL, OrderType::LIMIT, 200, 15);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 0);
    EXPECT_EQ(fills[0].fill_price, 200);

    EXPECT_EQ(book.get_best_ask(), 200);
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 200), 15);
}

TEST_F(LimitOrderBookTest, Submit_LimitBuy_DoesNotCrossWhenPriceBelowBestAsk) {
    book.submit(Side::SELL, OrderType::LIMIT, 110, 5);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 5);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 110), 5);
    EXPECT_EQ(book.get_best_bid(), 100);
}

TEST_F(LimitOrderBookTest, Submit_LimitSell_DoesNotCrossWhenPriceAboveBestBid) {
    book.submit(Side::BUY, OrderType::LIMIT, 100, 5);
    std::vector<Fill> fills = book.submit(Side::SELL, OrderType::LIMIT, 110, 5);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 5);
    EXPECT_EQ(book.get_best_ask(), 110);
}

TEST_F(LimitOrderBookTest, Submit_AggressorFullyFilled_RestingLevelReduced) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 50);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 30);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 30);
    EXPECT_EQ(fills[0].fill_price, 100);

    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 20);
    EXPECT_EQ(book.get_best_ask(), 100);
    EXPECT_EQ(book.get_best_bid(), 0);
}

TEST_F(LimitOrderBookTest, Submit_RestingLevelFullyConsumed_LevelRemovedAndBestUpdates) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 20);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 20);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 20);

    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 0);
    EXPECT_EQ(book.get_best_ask(), kMax);

    BookSnapshot snap = book.get_snapshot(5);
    EXPECT_TRUE(snap.asks.empty());
}

TEST_F(LimitOrderBookTest, Submit_MatchesAcrossMultiplePriceLevels_InPriceOrder) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 5);
    book.submit(Side::SELL, OrderType::LIMIT, 101, 5);
    book.submit(Side::SELL, OrderType::LIMIT, 102, 5);

    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 102, 12);

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].fill_price, 100);
    EXPECT_EQ(fills[0].filled_quantity, 5);
    EXPECT_EQ(fills[1].fill_price, 101);
    EXPECT_EQ(fills[1].filled_quantity, 5);
    EXPECT_EQ(fills[2].fill_price, 102);
    EXPECT_EQ(fills[2].filled_quantity, 2);

    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 101), 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 102), 3);
    EXPECT_EQ(book.get_best_ask(), 102);
}

TEST_F(LimitOrderBookTest, Submit_PriceTimePriority_FIFOWithinLevel) {
    std::vector<Fill> f1 = book.submit(Side::SELL, OrderType::LIMIT, 100, 5);
    std::vector<Fill> f2 = book.submit(Side::SELL, OrderType::LIMIT, 100, 5);
    OrderID first_id = f1[0].order_id;
    OrderID second_id = f2[0].order_id;

    book.submit(Side::BUY, OrderType::LIMIT, 100, 5);

    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 5);
    EXPECT_FALSE(book.cancel(first_id));
    EXPECT_TRUE(book.cancel(second_id));
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 0);
}

TEST_F(LimitOrderBookTest, Submit_PartialFillThenRest_ReturnsIdentifiableRestingOrder) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 10);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 30);

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].filled_quantity, 10);
    EXPECT_EQ(fills[0].fill_price, 100);

    EXPECT_EQ(fills[1].filled_quantity, 0);
    EXPECT_EQ(fills[1].fill_price, 100);
    EXPECT_NE(fills[1].order_id, 0);

    EXPECT_EQ(book.get_best_bid(), 100);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 20);

    EXPECT_TRUE(book.cancel(fills[1].order_id));
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 0);
}

TEST_F(LimitOrderBookTest, Submit_Market_IgnoresLimitPriceAndSweepsBook) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 5);
    book.submit(Side::SELL, OrderType::LIMIT, 200, 5);

    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::MARKET, 0, 10);

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].fill_price, 100);
    EXPECT_EQ(fills[1].fill_price, 200);
    EXPECT_EQ(book.get_best_ask(), kMax);
}

TEST_F(LimitOrderBookTest, Submit_Market_PartialWhenInsufficientLiquidity_DoesNotRest) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 5);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::MARKET, 0, 20);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 5);

    EXPECT_EQ(book.get_best_bid(), 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 0), 0);
}

TEST_F(LimitOrderBookTest, Submit_Market_EmptyBook_ReturnsNoFills) {
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::MARKET, 0, 10);
    EXPECT_TRUE(fills.empty());
}

TEST_F(LimitOrderBookTest, Submit_IOC_PartialFill_DoesNotRestRemainder) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 5);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::IOC, 100, 20);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].filled_quantity, 5);

    EXPECT_EQ(book.get_best_bid(), 0);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 0);
}

TEST_F(LimitOrderBookTest, Submit_IOC_NoCrossableLiquidity_ReturnsEmptyFills) {
    book.submit(Side::SELL, OrderType::LIMIT, 150, 5);
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::IOC, 100, 20);

    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 150), 5);
}

TEST_F(LimitOrderBookTest, Submit_FOK_SufficientLiquidity_FillsCompletely) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 10);
    book.submit(Side::SELL, OrderType::LIMIT, 101, 10);

    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::FOK, 101, 20);

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].filled_quantity, 10);
    EXPECT_EQ(fills[1].filled_quantity, 10);
    EXPECT_EQ(book.get_best_ask(), kMax);
}

TEST_F(LimitOrderBookTest, Submit_FOK_InsufficientLiquidity_KillsWithNoBookChange) {
    book.submit(Side::SELL, OrderType::LIMIT, 100, 5);

    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::FOK, 100, 20);

    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 5)
        << "FOK order must not partially execute against the book when it cannot "
           "be filled in full.";
}

TEST_F(LimitOrderBookTest, Submit_FOK_NotFooledByRestingSameSideLiquidity) {
    book.submit(Side::BUY, OrderType::LIMIT, 90, 1000);
    book.submit(Side::SELL, OrderType::LIMIT, 100, 5);

    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::FOK, 100, 20);

    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 5);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 90), 1000);
}

TEST_F(LimitOrderBookTest, Cancel_RestingOrder_RemovesQuantityAndSucceeds) {
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 10);
    OrderID id = fills[0].order_id;

    EXPECT_TRUE(book.cancel(id));
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 0);
    EXPECT_EQ(book.get_best_bid(), 0);
}

TEST_F(LimitOrderBookTest, Cancel_OneOfTwoOrdersAtLevel_KeepsTheOther) {
    std::vector<Fill> f1 = book.submit(Side::SELL, OrderType::LIMIT, 100, 10);
    book.submit(Side::SELL, OrderType::LIMIT, 100, 15);

    EXPECT_TRUE(book.cancel(f1[0].order_id));
    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 15);
    EXPECT_EQ(book.get_best_ask(), 100);
}

TEST_F(LimitOrderBookTest, Cancel_AlreadyCancelled_ReturnsFalse) {
    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 100, 10);
    OrderID id = fills[0].order_id;

    EXPECT_TRUE(book.cancel(id));
    EXPECT_FALSE(book.cancel(id));
}

TEST_F(LimitOrderBookTest, Cancel_AlreadyFullyMatched_ReturnsFalse) {
    std::vector<Fill> fills = book.submit(Side::SELL, OrderType::LIMIT, 100, 10);
    OrderID id = fills[0].order_id;
    book.submit(Side::BUY, OrderType::LIMIT, 100, 10);

    EXPECT_FALSE(book.cancel(id));
}

TEST_F(LimitOrderBookTest, Cancel_UnknownId_ReturnsFalseSafely) {
    EXPECT_FALSE(book.cancel(0));
    EXPECT_FALSE(book.cancel(123456));
}

TEST_F(LimitOrderBookTest, Cancel_LastOrderAtBestLevel_AdvancesBestPrice) {
    std::vector<Fill> f1 = book.submit(Side::BUY, OrderType::LIMIT, 105, 10);
    book.submit(Side::BUY, OrderType::LIMIT, 100, 10);

    EXPECT_EQ(book.get_best_bid(), 105);
    EXPECT_TRUE(book.cancel(f1[0].order_id));
    EXPECT_EQ(book.get_best_bid(), 100);
}

TEST_F(LimitOrderBookTest, Snapshot_BidsOrderedBestToWorst) {
    book.submit(Side::BUY, OrderType::LIMIT, 100, 1);
    book.submit(Side::BUY, OrderType::LIMIT, 105, 1);
    book.submit(Side::BUY, OrderType::LIMIT, 102, 1);

    BookSnapshot snap = book.get_snapshot(10);
    ASSERT_EQ(snap.bids.size(), 3u);
    EXPECT_EQ(snap.bids[0].price, 105);
    EXPECT_EQ(snap.bids[1].price, 102);
    EXPECT_EQ(snap.bids[2].price, 100);
}

TEST_F(LimitOrderBookTest, Snapshot_AsksOrderedBestToWorst) {
    book.submit(Side::SELL, OrderType::LIMIT, 105, 1);
    book.submit(Side::SELL, OrderType::LIMIT, 100, 1);
    book.submit(Side::SELL, OrderType::LIMIT, 102, 1);

    BookSnapshot snap = book.get_snapshot(10);
    ASSERT_EQ(snap.asks.size(), 3u);
    EXPECT_EQ(snap.asks[0].price, 100);
    EXPECT_EQ(snap.asks[1].price, 102);
    EXPECT_EQ(snap.asks[2].price, 105);
}

TEST_F(LimitOrderBookTest, Snapshot_RespectsLevelCap) {
    for (int i = 0; i < 5; ++i) {
        book.submit(Side::SELL, OrderType::LIMIT, 100 + i, 1);
    }

    BookSnapshot snap = book.get_snapshot(2);
    EXPECT_EQ(snap.asks.size(), 2u);
    EXPECT_EQ(snap.asks[0].price, 100);
    EXPECT_EQ(snap.asks[1].price, 101);
}

TEST_F(LimitOrderBookTest, Snapshot_IncludesOverflowLevelsBeyondWindow) {
    book.submit(Side::SELL, OrderType::LIMIT, 10, 1);
    book.submit(Side::SELL, OrderType::LIMIT, 15, 1);
    book.submit(Side::SELL, OrderType::LIMIT, 5000, 1);

    BookSnapshot snap = book.get_snapshot(10);
    ASSERT_EQ(snap.asks.size(), 3u);
    EXPECT_EQ(snap.asks[0].price, 10);
    EXPECT_EQ(snap.asks[1].price, 15);
    EXPECT_EQ(snap.asks[2].price, 5000);
}

TEST_F(LimitOrderBookTest, AvailableLiquidity_SumsLevelsUpToWorstPrice) {
    book.submit(Side::SELL, OrderType::LIMIT, 10, 5);
    book.submit(Side::SELL, OrderType::LIMIT, 15, 7);
    book.submit(Side::SELL, OrderType::LIMIT, 20, 3);

    EXPECT_EQ(book.available_liquidity(Side::BUY, 15), 12);
    EXPECT_EQ(book.available_liquidity(Side::BUY, 20), 15);
    EXPECT_EQ(book.available_liquidity(Side::BUY, 9), 0);
}

TEST_F(LimitOrderBookTest, AvailableLiquidity_BidSide_SumsDownToWorstPrice) {
    book.submit(Side::BUY, OrderType::LIMIT, 100, 5);
    book.submit(Side::BUY, OrderType::LIMIT, 95, 7);
    book.submit(Side::BUY, OrderType::LIMIT, 90, 3);

    EXPECT_EQ(book.available_liquidity(Side::SELL, 95), 12);
    EXPECT_EQ(book.available_liquidity(Side::SELL, 90), 15);
    EXPECT_EQ(book.available_liquidity(Side::SELL, 101), 0);
}

TEST_F(LimitOrderBookTest, HalfSpread_EmptyBook_ReturnsZero) {
    EXPECT_DOUBLE_EQ(book.half_spread(), 0.0);
}

TEST_F(LimitOrderBookTest, HalfSpread_EvenSpread_ComputesExactly) {
    book.submit(Side::BUY, OrderType::LIMIT, 100, 1);
    book.submit(Side::SELL, OrderType::LIMIT, 104, 1);
    EXPECT_DOUBLE_EQ(book.half_spread(), 2.0);
}

TEST_F(LimitOrderBookTest, HalfSpread_OddSpread_PreservesFraction) {
    book.submit(Side::BUY, OrderType::LIMIT, 100, 1);
    book.submit(Side::SELL, OrderType::LIMIT, 105, 1);
    EXPECT_DOUBLE_EQ(book.half_spread(), 2.5);
}

TEST_F(LimitOrderBookTest, OnBookUpdate_FiresOnRestAndMatchAndCancel) {
    std::vector<std::tuple<Side, int64_t, int64_t>> events;
    auto recorder = [&](Side side, int64_t price, int64_t qty) {
        events.emplace_back(side, price, qty);
    };
    book.on_book_update(recorder);

    std::vector<Fill> f1 = book.submit(Side::SELL, OrderType::LIMIT, 100, 10);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(std::get<0>(events.back()), Side::SELL);
    EXPECT_EQ(std::get<1>(events.back()), 100);
    EXPECT_EQ(std::get<2>(events.back()), 10);

    book.submit(Side::BUY, OrderType::LIMIT, 100, 4);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(std::get<2>(events.back()), 6);

    book.cancel(f1[0].order_id);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(std::get<2>(events.back()), 0);
}

TEST_F(LimitOrderBookTest, ApplyDelta_NewLevel_UpdatesQuantityAndBestPrice) {
    book.apply_delta(BookDelta{1, Side::SELL, 100, 25});

    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 25);
    EXPECT_EQ(book.get_best_ask(), 100);
}

TEST_F(LimitOrderBookTest, ApplyDelta_ZeroQuantity_RemovesLevelAndFindsNextBest) {
    book.apply_delta(BookDelta{1, Side::SELL, 100, 10});
    book.apply_delta(BookDelta{1, Side::SELL, 110, 5});
    ASSERT_EQ(book.get_best_ask(), 100);

    book.apply_delta(BookDelta{1, Side::SELL, 100, 0});

    EXPECT_EQ(book.get_quantity_at_price(Side::SELL, 100), 0);
    EXPECT_EQ(book.get_best_ask(), 110);
}

TEST_F(LimitOrderBookTest, ApplyDelta_FarOutsideWindow_StillQueryable) {
    book.apply_delta(BookDelta{1, Side::BUY, 5000, 40});

    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 5000), 40);
    EXPECT_EQ(book.get_best_bid(), 5000);
}

TEST_F(LimitOrderBookTest, ApplyDelta_UpdatesQuantityInPlace) {
    book.apply_delta(BookDelta{1, Side::BUY, 100, 40});
    book.apply_delta(BookDelta{1, Side::BUY, 100, 15});

    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, 100), 15);
}

TEST_F(LimitOrderBookTest, SlidingWindow_FarBidBelowWindow_DoesNotDisturbBestBid) {
    book.submit(Side::BUY, OrderType::LIMIT, 100, 10);
    book.submit(Side::BUY, OrderType::LIMIT, -500, 5);

    EXPECT_EQ(book.get_best_bid(), 100);
    EXPECT_EQ(book.get_quantity_at_price(Side::BUY, -500), 5);
}

TEST_F(LimitOrderBookTest, SlidingWindow_MatchingWalksFromLadderIntoOverflow) {
    book.submit(Side::SELL, OrderType::LIMIT, 10, 5);
    book.submit(Side::SELL, OrderType::LIMIT, 5000, 5);

    std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, 5000, 10);

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].fill_price, 10);
    EXPECT_EQ(fills[1].fill_price, 5000);
    EXPECT_EQ(book.get_best_ask(), kMax);
}

TEST_F(LimitOrderBookTest, SlidingWindow_ManyLevelsRemainConsistentAfterShifts) {
    std::vector<OrderID> ids;
    for (int i = 0; i < 20; ++i) {
        int64_t price = 100 + i * 500;
        std::vector<Fill> fills = book.submit(Side::BUY, OrderType::LIMIT, price, 1);
        ASSERT_EQ(fills.size(), 1u);
        ids.push_back(fills[0].order_id);
        EXPECT_EQ(book.get_best_bid(), price);
    }

    for (int i = 0; i < 20; ++i) {
        int64_t price = 100 + i * 500;
        EXPECT_EQ(book.get_quantity_at_price(Side::BUY, price), 1) << "price=" << price;
    }

    for (OrderID id : ids) {
        EXPECT_TRUE(book.cancel(id));
    }
    EXPECT_EQ(book.get_best_bid(), 0);
}

}  // namespace
