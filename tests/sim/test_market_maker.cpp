#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "sim/MarketMaker.h"
#include "lob/Venue.h"

namespace {

MarketMakerConfig make_mm_config(int64_t base_spread = 4, double spread_sensitivity = 0.0,
                                  int64_t quantity_mean = 100, double quantity_variance = 1.0,
                                  size_t max_orders_per_side = 3, int64_t stale_distance_ticks = 50) {
    return MarketMakerConfig{
        .base_spread = base_spread,
        .spread_sensitivity = spread_sensitivity,
        .quantity_mean = quantity_mean,
        .quantity_variance = quantity_variance,
        .max_orders_per_side = max_orders_per_side,
        .stale_distance_ticks = stale_distance_ticks
    };
}

class MarketMakerTest : public ::testing::Test {
protected:
    SPSCQueue<BookDelta, QUEUE_SIZE> md_queue;
    SPSCQueue<FillEvent, QUEUE_SIZE> sor_fill_queue;
    std::unique_ptr<Venue> venue;
    std::unique_ptr<MarketMaker> mm;

    void start(const MarketMakerConfig& cfg, uint32_t seed = 42) {
        VenueConfig vcfg{
            .type = LIT,
            .fee_per_share = 0.0,
            .latency_us = 0,
            .impact_coefficient = 0.0,
            .historical_fill_ratio = 0.0
        };
        venue = std::make_unique<Venue>(1, vcfg);
        venue->set_sor_queues(&md_queue, &sor_fill_queue);
        mm = std::make_unique<MarketMaker>(venue.get(), cfg, seed);
        venue->start();
    }

    std::vector<BookDelta> drain_deltas(size_t expected_count,
                                         std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::vector<BookDelta> deltas;
        auto deadline = std::chrono::steady_clock::now() + timeout;
        BookDelta d;
        while (deltas.size() < expected_count && std::chrono::steady_clock::now() < deadline) {
            if (md_queue.try_pop(d)) {
                deltas.push_back(d);
            } else {
                std::this_thread::yield();
            }
        }
        return deltas;
    }

    void TearDown() override {
        if (venue) venue->stop();
    }
};

TEST_F(MarketMakerTest, Update_EmptyBook_PostsOrdersOnBothSidesAwayFromFairValue) {
    start(make_mm_config());

    mm->update(1000.0, 0.0);

    std::vector<BookDelta> deltas = drain_deltas(6);
    ASSERT_EQ(deltas.size(), 6u);

    int bid_count = 0, ask_count = 0;
    for (const BookDelta& d : deltas) {
        EXPECT_GT(d.new_quantity, 0);
        if (d.side == BUY) {
            EXPECT_LT(d.price, 1000);
            bid_count++;
        } else {
            EXPECT_GT(d.price, 1000);
            ask_count++;
        }
    }
    EXPECT_EQ(bid_count, 3);
    EXPECT_EQ(ask_count, 3);
}

TEST_F(MarketMakerTest, Update_ZeroQuantityVariance_PostsExactMultiplesOfMeanQuantity) {
    start(make_mm_config(4, 0.0, 100, 0.0001));

    mm->update(1000.0, 0.0);

    std::vector<BookDelta> deltas = drain_deltas(6);
    ASSERT_EQ(deltas.size(), 6u);
    for (const BookDelta& d : deltas) {
        EXPECT_GT(d.new_quantity, 0);
        EXPECT_EQ(d.new_quantity % 100, 0);
    }
}

TEST_F(MarketMakerTest, Update_RepeatedCalls_DoesNotExceedMaxOrdersPerSide) {
    start(make_mm_config());

    mm->update(1000.0, 0.0);
    std::vector<BookDelta> first_batch = drain_deltas(6);
    ASSERT_EQ(first_batch.size(), 6u);

    mm->update(1000.0, 0.0);
    mm->update(1000.0, 0.0);

    std::vector<BookDelta> extra = drain_deltas(1, std::chrono::milliseconds(300));
    EXPECT_TRUE(extra.empty());
}

// spread_sensitivity is deliberately negative here: combined with a nonzero
// volatility, it cancels out the stale_distance_ticks/2 term in dispersion(),
// collapsing the noise term to ~0 so every order clamps to exactly fair-1 /
// fair+1. That makes posted prices deterministic instead of a coin flip
// against the stale-distance threshold (dispersion == stale/2 when
// volatility == 0, so ordinary noise can already exceed it at posting time).
TEST_F(MarketMakerTest, Update_MovingFairValueFarAway_CancelsStaleOrdersAndReplenishesNearby) {
    start(make_mm_config(4, -0.099, 100, 1.0, 3, 200));

    mm->update(1000.0, 1.0);
    ASSERT_EQ(drain_deltas(6).size(), 6u);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mm->update(1000.0, 1.0);

    mm->update(2000.0, 1.0);
    std::vector<BookDelta> deltas = drain_deltas(12);
    ASSERT_EQ(deltas.size(), 12u);

    int64_t last_old_bid_qty = -1, last_old_ask_qty = -1;
    int new_side_count = 0;
    for (const BookDelta& d : deltas) {
        if (d.price < 1500) {
            if (d.side == BUY) {
                last_old_bid_qty = d.new_quantity;
            } else {
                last_old_ask_qty = d.new_quantity;
            }
        } else {
            new_side_count++;
            EXPECT_GT(d.new_quantity, 0);
            if (d.side == BUY) {
                EXPECT_LT(d.price, 2000);
            } else {
                EXPECT_GT(d.price, 2000);
            }
        }
    }
    EXPECT_EQ(last_old_bid_qty, 0);
    EXPECT_EQ(last_old_ask_qty, 0);
    EXPECT_EQ(new_side_count, 6);
}

TEST_F(MarketMakerTest, Update_FairValueWithinStaleDistance_DoesNotCancelOrReplenish) {
    start(make_mm_config(4, -0.099, 100, 1.0, 3, 200));

    mm->update(1000.0, 1.0);
    ASSERT_EQ(drain_deltas(6).size(), 6u);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    mm->update(1000.0, 1.0);

    mm->update(1010.0, 1.0);

    std::vector<BookDelta> extra = drain_deltas(1, std::chrono::milliseconds(500));
    EXPECT_TRUE(extra.empty());
}

}  // namespace
