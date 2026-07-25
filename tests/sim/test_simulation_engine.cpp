#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <vector>

#include "sim/SimulationEngine.h"
#include "lob/Venue.h"
#include "common/SimClock.h"

namespace {

MarketMakerConfig make_mm_config() {
    return MarketMakerConfig{
        .base_spread = 4,
        .spread_sensitivity = 0.0,
        .quantity_mean = 100,
        .quantity_variance = 1.0,
        .max_orders_per_side = 3,
        .stale_distance_ticks = 50
    };
}

NoiseTraderConfig make_noise_config() {
    return NoiseTraderConfig{
        .arrival_rate = 20.0,
        .size_mu = std::log(50.0),
        .size_sigma = 0.3,
        .trend_sensitivity = 0.0
    };
}

VenueConfig make_venue_config() {
    return VenueConfig{
        .type = LIT,
        .fee_per_share = 0.0,
        .latency_us = 0,
        .impact_coefficient = 0.0,
        .historical_fill_ratio = 0.0
    };
}

class SimulationEngineTest : public ::testing::Test {
protected:
    SimClock clock;
    std::vector<std::unique_ptr<SPSCQueue<BookDelta, QUEUE_SIZE>>> md_queues;
    std::vector<std::unique_ptr<SPSCQueue<FillEvent, QUEUE_SIZE>>> fill_queues;
    std::vector<std::unique_ptr<Venue>> venues;
    std::unique_ptr<SimulationEngine> engine;

    Venue* make_venue(VenueID id) {
        md_queues.push_back(std::make_unique<SPSCQueue<BookDelta, QUEUE_SIZE>>());
        fill_queues.push_back(std::make_unique<SPSCQueue<FillEvent, QUEUE_SIZE>>());
        venues.push_back(std::make_unique<Venue>(id, make_venue_config()));

        Venue* venue = venues.back().get();
        venue->set_sor_queues(md_queues.back().get(), fill_queues.back().get());
        venue->start();
        return venue;
    }

    void make_engine(double initial_price, double drift, double vol, double pp_dt, uint32_t seed = 1) {
        auto pp = std::make_unique<PriceProcess>(initial_price, drift, vol, pp_dt, seed);
        engine = std::make_unique<SimulationEngine>(std::move(pp), &clock);
    }

    void seed_liquidity(Venue* venue, Side side, int64_t price, int64_t qty) {
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

    void drain_n(SPSCQueue<BookDelta, QUEUE_SIZE>& queue, size_t count,
                 std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        size_t seen = 0;
        BookDelta d;
        while (seen < count && std::chrono::steady_clock::now() < deadline) {
            if (queue.try_pop(d)) {
                seen++;
            } else {
                std::this_thread::yield();
            }
        }
        ASSERT_EQ(seen, count);
    }

    std::vector<BookDelta> drain_for(SPSCQueue<BookDelta, QUEUE_SIZE>& queue,
                                      std::chrono::milliseconds duration) {
        std::vector<BookDelta> deltas;
        auto deadline = std::chrono::steady_clock::now() + duration;
        BookDelta d;
        while (std::chrono::steady_clock::now() < deadline) {
            if (queue.try_pop(d)) {
                deltas.push_back(d);
            } else {
                std::this_thread::yield();
            }
        }
        return deltas;
    }

    void TearDown() override {
        if (engine) engine->stop();
        for (auto& v : venues) v->stop();
    }
};

TEST_F(SimulationEngineTest, Start_AdvancesClockOverTime) {
    make_engine(100.0, 0.0, 0.01, 0.001);
    engine->start(0.001);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_GT(clock.now(), 0.0);
}

TEST_F(SimulationEngineTest, Stop_HaltsClockAdvancement) {
    make_engine(100.0, 0.0, 0.01, 0.001);
    engine->start(0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    engine->stop();
    double after_stop = clock.now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_DOUBLE_EQ(clock.now(), after_stop);
}

TEST_F(SimulationEngineTest, Start_CalledTwice_DoesNotSpawnSecondWorkerOrCrash) {
    make_engine(100.0, 0.0, 0.01, 0.001);
    engine->start(0.001);
    engine->start(0.001);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    engine->stop();
    SUCCEED();
}

TEST_F(SimulationEngineTest, Destructor_JoinsRunningWorker_WithoutHanging) {
    make_engine(100.0, 0.0, 0.01, 0.001);
    engine->start(0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    engine.reset();
    SUCCEED();
}

TEST_F(SimulationEngineTest, Update_DrivesMarketMakerToPostQuotes_WithoutExceedingMaxOrders) {
    Venue* venue = make_venue(1);
    make_engine(1000.0, 0.0, 0.0, 0.001);
    engine->add_market_maker(venue, make_mm_config(), 42);

    engine->start(0.001);
    std::vector<BookDelta> deltas = drain_for(*md_queues[0], std::chrono::milliseconds(500));

    ASSERT_EQ(deltas.size(), 6u);
    for (const BookDelta& d : deltas) {
        EXPECT_GT(d.new_quantity, 0);
        if (d.side == BUY) {
            EXPECT_LT(d.price, 1000);
        } else {
            EXPECT_GT(d.price, 1000);
        }
    }
}

TEST_F(SimulationEngineTest, Update_DrivesNoiseTraderToConsumeSeededLiquidity) {
    Venue* venue = make_venue(1);
    seed_liquidity(venue, BUY, 95, 1'000'000);
    seed_liquidity(venue, SELL, 105, 1'000'000);
    drain_n(*md_queues[0], 2);

    make_engine(100.0, 0.0, 0.0, 0.001);
    engine->add_noise_trader(venue, make_noise_config(), 7);

    engine->start(0.001);
    std::vector<BookDelta> deltas = drain_for(*md_queues[0], std::chrono::milliseconds(500));

    bool saw_consumption = false;
    for (const BookDelta& d : deltas) {
        if ((d.side == BUY && d.price == 95) || (d.side == SELL && d.price == 105)) {
            saw_consumption = true;
        }
    }
    EXPECT_TRUE(saw_consumption);
}

TEST_F(SimulationEngineTest, MultipleMarketMakers_OnDifferentVenues_BothGetUpdatedEachTick) {
    Venue* v1 = make_venue(1);
    Venue* v2 = make_venue(2);
    make_engine(1000.0, 0.0, 0.0, 0.001);
    engine->add_market_maker(v1, make_mm_config(), 42);
    engine->add_market_maker(v2, make_mm_config(), 43);

    engine->start(0.001);
    std::vector<BookDelta> deltas1 = drain_for(*md_queues[0], std::chrono::milliseconds(500));
    std::vector<BookDelta> deltas2 = drain_for(*md_queues[1], std::chrono::milliseconds(500));

    EXPECT_EQ(deltas1.size(), 6u);
    EXPECT_EQ(deltas2.size(), 6u);
}

}  // namespace
