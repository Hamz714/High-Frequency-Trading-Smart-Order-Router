#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "sor/SmartOrderRouter.h"

namespace {

template <typename Queue, typename T>
bool wait_pop(Queue& queue, T& out, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (queue.try_pop(out)) return true;
    }
    return false;
}

RouterConfig make_router_config(int64_t lot_size = 10, bool use_naive = false, int max_reroute_attempts = 3) {
    return RouterConfig{
        .lot_size = lot_size,
        .latency_cost_factor = 0,
        .dark_pool_decay_rate = 0.05,
        .use_naive_split = use_naive,
        .max_reroute_attempts = max_reroute_attempts
    };
}

VenueConfig make_venue_config(VenueType type = LIT, double fee = 0.0, double impact = 0.0,
                               int64_t latency_us = 0) {
    return VenueConfig{
        .type = type,
        .fee_per_share = fee,
        .latency_us = latency_us,
        .impact_coefficient = impact,
        .historical_fill_ratio = 0.0
    };
}

struct RoutingOutcome {
    OrderLifecycleEvent decision{};
    bool got_decision = false;
    int64_t total_filled = 0;
    int fill_count = 0;
    std::vector<VenueID> fill_venues;
    OrderLifecycleEvent completion{};
    bool completed = false;
};

class SmartOrderRouterTest : public ::testing::Test {
protected:
    SPSCQueue<OrderLifecycleEvent, QUEUE_SIZE> analytics_queue;
    std::vector<std::unique_ptr<Venue>> venue_storage;
    std::unique_ptr<SmartOrderRouter> router;
    OrderID next_order_id = 1;

    Venue* make_venue(VenueID id, VenueConfig cfg = make_venue_config()) {
        venue_storage.push_back(std::make_unique<Venue>(id, cfg));
        return venue_storage.back().get();
    }

    void start_all(const RouterConfig& cfg) {
        router = std::make_unique<SmartOrderRouter>(cfg);
        for (auto& v : venue_storage) router->add_venue(v.get());
        router->set_analytics_queue(&analytics_queue);
        router->start();
        for (auto& v : venue_storage) v->start();
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

    RoutingOutcome submit_and_drain(Side side, int64_t price, int64_t quantity,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        RoutingOutcome outcome;
        OrderID id = next_order_id++;

        router->submit_order(OrderRequest{
            .order_id = id,
            .sender_type = SOR,
            .request_type = RequestType::ORDER,
            .side = side,
            .order_type = LIMIT,
            .price = price,
            .quantity = quantity
        });

        auto deadline = std::chrono::steady_clock::now() + timeout;
        OrderLifecycleEvent event;
        while (std::chrono::steady_clock::now() < deadline) {
            if (analytics_queue.try_pop(event)) {
                if (event.parent_id != id) continue;

                if (event.type == OrderEventType::DECISION) {
                    outcome.decision = event;
                    outcome.got_decision = true;
                } else if (event.type == OrderEventType::FILL) {
                    outcome.total_filled += event.quantity;
                    outcome.fill_count++;
                    outcome.fill_venues.push_back(event.venue_id);
                } else if (event.type == OrderEventType::COMPLETION) {
                    outcome.completion = event;
                    outcome.completed = true;
                    return outcome;
                }
            } else {
                std::this_thread::yield();
            }
        }
        return outcome;
    }

    bool warm_up(Side side, int64_t price, int64_t lot_size = 10,
                 std::chrono::milliseconds overall_timeout = std::chrono::seconds(3)) {
        auto deadline = std::chrono::steady_clock::now() + overall_timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            RoutingOutcome probe = submit_and_drain(side, price, lot_size, std::chrono::milliseconds(200));
            if (probe.completed && probe.total_filled > 0) return true;
        }
        return false;
    }

    bool confirm_venue_ready(Venue* venue, Side side, int64_t canary_price) {
        seed_liquidity(venue, side == BUY ? SELL : BUY, canary_price, 10);
        return warm_up(side, canary_price);
    }

    void TearDown() override {
        if (router) router->stop();
        for (auto& v : venue_storage) v->stop();
    }
};

TEST_F(SmartOrderRouterTest, SufficientLiquidity_SingleVenue_FillsCompletely) {
    Venue* v1 = make_venue(1);
    start_all(make_router_config());

    seed_liquidity(v1, SELL, 100, 1000);
    ASSERT_TRUE(warm_up(BUY, 200));

    RoutingOutcome outcome = submit_and_drain(BUY, 200, 100);

    ASSERT_TRUE(outcome.got_decision);
    EXPECT_EQ(outcome.decision.side, BUY);
    EXPECT_EQ(outcome.decision.quantity, 100);

    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.total_filled, 100);
    EXPECT_EQ(outcome.completion.quantity, 100);
    EXPECT_FALSE(outcome.completion.timed_out);
}

TEST_F(SmartOrderRouterTest, SplitsAcrossMultipleVenues_WhenImpactFavorsSpreading) {
    VenueConfig cfg = make_venue_config(LIT, 0.0, 1.0);
    Venue* v1 = make_venue(1, cfg);
    Venue* v2 = make_venue(2, cfg);
    start_all(make_router_config());

    seed_liquidity(v1, SELL, 100, 100000);
    seed_liquidity(v2, SELL, 100, 100000);
    ASSERT_TRUE(confirm_venue_ready(v1, BUY, 50));
    ASSERT_TRUE(confirm_venue_ready(v2, BUY, 50));

    RoutingOutcome outcome = submit_and_drain(BUY, 200, 100);

    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.total_filled, 100);
    EXPECT_FALSE(outcome.completion.timed_out);

    std::set<VenueID> distinct_venues(outcome.fill_venues.begin(), outcome.fill_venues.end());
    EXPECT_EQ(distinct_venues.size(), 2u);
}

TEST_F(SmartOrderRouterTest, NoLiquidityAnywhere_CompletesWithTimedOutAndNoFills) {
    make_venue(1);
    start_all(make_router_config());

    RoutingOutcome outcome = submit_and_drain(BUY, 200, 100);

    ASSERT_TRUE(outcome.got_decision);
    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.total_filled, 0);
    EXPECT_EQ(outcome.fill_count, 0);
    EXPECT_TRUE(outcome.completion.timed_out);
    EXPECT_EQ(outcome.completion.quantity, 0);
}

TEST_F(SmartOrderRouterTest, PartialLiquidity_CompletesPartiallyFilledAndTimedOut) {
    Venue* v1 = make_venue(1);
    start_all(make_router_config(10, false, 2));

    seed_liquidity(v1, SELL, 100, 60);
    ASSERT_TRUE(warm_up(BUY, 200));

    RoutingOutcome outcome = submit_and_drain(BUY, 200, 100, std::chrono::seconds(10));

    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.total_filled, 50);
    EXPECT_EQ(outcome.completion.quantity, 50);
    EXPECT_TRUE(outcome.completion.timed_out);
}

TEST_F(SmartOrderRouterTest, DecisionEventReflectsConsolidatedMidPrice) {
    Venue* v1 = make_venue(1);
    start_all(make_router_config());

    seed_liquidity(v1, BUY, 95, 500);
    seed_liquidity(v1, SELL, 105, 500);
    ASSERT_TRUE(warm_up(BUY, 200));
    ASSERT_TRUE(warm_up(SELL, 0));

    RoutingOutcome outcome = submit_and_drain(BUY, 200, 30);

    ASSERT_TRUE(outcome.got_decision);
    EXPECT_EQ(outcome.decision.side, BUY);
    EXPECT_EQ(outcome.decision.quantity, 30);
    EXPECT_DOUBLE_EQ(outcome.decision.price, 100.0);
}

TEST_F(SmartOrderRouterTest, TwoIndependentOrders_TrackedSeparatelyByParentId) {
    Venue* v1 = make_venue(1);
    start_all(make_router_config());

    seed_liquidity(v1, SELL, 100, 1000);
    seed_liquidity(v1, BUY, 90, 1000);
    ASSERT_TRUE(warm_up(BUY, 200));
    ASSERT_TRUE(warm_up(SELL, 0));

    RoutingOutcome buy_outcome = submit_and_drain(BUY, 200, 40);
    RoutingOutcome sell_outcome = submit_and_drain(SELL, 0, 25);

    ASSERT_TRUE(buy_outcome.completed);
    EXPECT_EQ(buy_outcome.total_filled, 40);
    EXPECT_FALSE(buy_outcome.completion.timed_out);

    ASSERT_TRUE(sell_outcome.completed);
    EXPECT_EQ(sell_outcome.total_filled, 25);
    EXPECT_FALSE(sell_outcome.completion.timed_out);
}

TEST_F(SmartOrderRouterTest, LatencyVenue_StillFillsThroughDelayedMarketDataAndFills) {
    constexpr int64_t kLatencyUs = 2'000;
    Venue* v1 = make_venue(1, make_venue_config(LIT, 0.0, 0.0, kLatencyUs));
    start_all(make_router_config());

    seed_liquidity(v1, SELL, 100, 1000);
    ASSERT_TRUE(warm_up(BUY, 200));

    auto start = std::chrono::steady_clock::now();
    RoutingOutcome outcome = submit_and_drain(BUY, 200, 100);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.total_filled, 100);
    EXPECT_FALSE(outcome.completion.timed_out);
    EXPECT_GE(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count(), 2 * kLatencyUs);
}

TEST_F(SmartOrderRouterTest, NaiveSplit_RoutesEntirelyToOneVenue) {
    Venue* v1 = make_venue(1);
    Venue* v2 = make_venue(2);
    start_all(make_router_config(10, true));

    seed_liquidity(v1, SELL, 105, 1000);
    seed_liquidity(v2, SELL, 100, 1000);
    ASSERT_TRUE(warm_up(BUY, 100));

    RoutingOutcome outcome = submit_and_drain(BUY, 200, 100);

    ASSERT_TRUE(outcome.completed);
    EXPECT_EQ(outcome.total_filled, 100);
    EXPECT_FALSE(outcome.completion.timed_out);

    std::set<VenueID> distinct_venues(outcome.fill_venues.begin(), outcome.fill_venues.end());
    EXPECT_EQ(distinct_venues.size(), 1u);
    EXPECT_EQ(*distinct_venues.begin(), 2);
}

}  // namespace
