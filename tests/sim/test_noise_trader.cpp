#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "sim/NoiseTrader.h"
#include "lob/Venue.h"

namespace {

NoiseTraderConfig make_noise_config(double arrival_rate = 20.0, double size_mu = std::log(100.0),
                                     double size_sigma = 0.3, double trend_sensitivity = 0.0) {
    return NoiseTraderConfig{
        .arrival_rate = arrival_rate,
        .size_mu = size_mu,
        .size_sigma = size_sigma,
        .trend_sensitivity = trend_sensitivity
    };
}

class NoiseTraderTest : public ::testing::Test {
protected:
    SPSCQueue<BookDelta, QUEUE_SIZE> md_queue;
    SPSCQueue<FillEvent, QUEUE_SIZE> sor_fill_queue;
    std::unique_ptr<Venue> venue;
    std::unique_ptr<NoiseTrader> trader;

    void start(const NoiseTraderConfig& cfg, double initial_price, uint32_t seed = 42) {
        VenueConfig vcfg{
            .type = LIT,
            .fee_per_share = 0.0,
            .latency_us = 0,
            .impact_coefficient = 0.0,
            .historical_fill_ratio = 0.0
        };
        venue = std::make_unique<Venue>(1, vcfg);
        venue->set_sor_queues(&md_queue, &sor_fill_queue);
        venue->start();
        trader = std::make_unique<NoiseTrader>(venue.get(), cfg, initial_price, seed);
    }

    void seed_liquidity(Side side, int64_t price, int64_t qty) {
        venue->route_order(OrderRequest{
            .order_id = 0,
            .sender_type = MM,
            .request_type = RequestType::ORDER,
            .side = side,
            .order_type = LIMIT,
            .price = price,
            .quantity = qty
        });
    }

    std::vector<BookDelta> drain_for(std::chrono::milliseconds duration) {
        std::vector<BookDelta> deltas;
        auto deadline = std::chrono::steady_clock::now() + duration;
        BookDelta d;
        while (std::chrono::steady_clock::now() < deadline) {
            if (md_queue.try_pop(d)) {
                deltas.push_back(d);
            } else {
                std::this_thread::yield();
            }
        }
        return deltas;
    }

    void drain_seed_deltas(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        size_t seen = 0;
        BookDelta d;
        while (seen < count && std::chrono::steady_clock::now() < deadline) {
            if (md_queue.try_pop(d)) {
                seen++;
            } else {
                std::this_thread::yield();
            }
        }
        ASSERT_EQ(seen, count);
    }

    bool fire_next_order(double fair_value, BookDelta& out,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            trader->update(0.01, fair_value);
            if (md_queue.try_pop(out)) return true;
        }
        return false;
    }

    void TearDown() override {
        if (venue) venue->stop();
    }
};

TEST_F(NoiseTraderTest, Update_OnlyMatchesExistingLiquidity_NeverRests) {
    start(make_noise_config(20.0, std::log(50.0), 0.3, 0.0), 100.0);

    seed_liquidity(BUY, 95, 1'000'000);
    seed_liquidity(SELL, 105, 1'000'000);
    drain_seed_deltas(2);

    trader->update(0.5, 100.0);
    std::vector<BookDelta> deltas = drain_for(std::chrono::milliseconds(500));

    ASSERT_FALSE(deltas.empty());
    for (const BookDelta& d : deltas) {
        if (d.side == BUY) {
            EXPECT_EQ(d.price, 95);
        } else {
            EXPECT_EQ(d.price, 105);
        }
        EXPECT_GE(d.new_quantity, 0);
    }
}

TEST_F(NoiseTraderTest, Update_StrongPositiveTrend_FirstOrderIsBuy) {
    start(make_noise_config(20.0, std::log(50.0), 0.3, 1000.0), 100.0);

    seed_liquidity(BUY, 95, 1'000'000);
    seed_liquidity(SELL, 105, 1'000'000);
    drain_seed_deltas(2);

    BookDelta first;
    ASSERT_TRUE(fire_next_order(200.0, first));
    EXPECT_EQ(first.side, SELL);
    EXPECT_EQ(first.price, 105);
}

TEST_F(NoiseTraderTest, Update_StrongNegativeTrend_FirstOrderIsSell) {
    start(make_noise_config(20.0, std::log(50.0), 0.3, 1000.0), 100.0);

    seed_liquidity(BUY, 95, 1'000'000);
    seed_liquidity(SELL, 105, 1'000'000);
    drain_seed_deltas(2);

    BookDelta first;
    ASSERT_TRUE(fire_next_order(50.0, first));
    EXPECT_EQ(first.side, BUY);
    EXPECT_EQ(first.price, 95);
}

TEST_F(NoiseTraderTest, Update_LargeOrderExhaustsSmallRestingLiquidity_WithoutResting) {
    start(make_noise_config(20.0, std::log(10000.0), 0.01, 1000.0), 100.0);

    seed_liquidity(SELL, 105, 500);
    drain_seed_deltas(1);

    BookDelta first;
    ASSERT_TRUE(fire_next_order(200.0, first));
    EXPECT_EQ(first.side, SELL);
    EXPECT_EQ(first.price, 105);
    EXPECT_EQ(first.new_quantity, 0);

    std::vector<BookDelta> extra = drain_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(extra.empty());
}

TEST_F(NoiseTraderTest, Update_QuantityIsApproximatelyDeterministic_WhenSigmaIsTiny) {
    start(make_noise_config(20.0, std::log(100.0), 0.0001, 0.0), 100.0);

    seed_liquidity(BUY, 95, 1'000'000);
    seed_liquidity(SELL, 105, 1'000'000);
    drain_seed_deltas(2);

    BookDelta first;
    ASSERT_TRUE(fire_next_order(100.0, first));

    int64_t consumed = 1'000'000 - first.new_quantity;
    EXPECT_EQ(consumed, 100);
}

}  // namespace
