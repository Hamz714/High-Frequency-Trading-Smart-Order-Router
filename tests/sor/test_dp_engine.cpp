#include <gtest/gtest.h>
#include <limits>

#include "sor/DPEngine.h"
#include "lob/LimitOrderBook.h"
#include "lob/Venue.h"

namespace {

constexpr double kMaxCost = std::numeric_limits<double>::max();

RouterConfig make_router_config(int64_t lot_size = 10, int64_t latency_cost_factor = 0) {
    return RouterConfig{
        .lot_size = lot_size,
        .latency_cost_factor = latency_cost_factor,
        .dark_pool_decay_rate = 0.05,
        .strategy = RoutingStrategy::DP_OPTIMAL,
        .max_reroute_attempts = 3
    };
}

VenueConfig make_venue_config(VenueType type, double fee = 0.0, int64_t latency_us = 0,
                               double impact = 0.0, double dark_fill_ratio = 0.0) {
    return VenueConfig{
        .type = type,
        .fee_per_share = fee,
        .latency_us = latency_us,
        .impact_coefficient = impact,
        .historical_fill_ratio = dark_fill_ratio
    };
}

TEST(DPEngineNaiveSplitTest, PicksLowestAskVenue_ForBuy) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(BUY, LIMIT, 95, 1000);
    lob_a.submit(SELL, LIMIT, 105, 1000);
    lob_b.submit(BUY, LIMIT, 95, 1000);
    lob_b.submit(SELL, LIMIT, 100, 1000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &lob_a},
        VenueState{2, cfg, &lob_b}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_naive_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 100);
    EXPECT_DOUBLE_EQ(result.expected_cost, 250.0);
}

TEST(DPEngineNaiveSplitTest, PicksHighestBidVenue_ForSell) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 105, 1000);
    lob_a.submit(BUY, LIMIT, 90, 1000);
    lob_b.submit(SELL, LIMIT, 105, 1000);
    lob_b.submit(BUY, LIMIT, 95, 1000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &lob_a},
        VenueState{2, cfg, &lob_b}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_naive_split(100, SELL, 0, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 100);
    EXPECT_DOUBLE_EQ(result.expected_cost, 500.0);
}

TEST(DPEngineNaiveSplitTest, IgnoresDarkVenues) {
    LimitOrderBook dark_lob, lit_lob;
    lit_lob.submit(SELL, LIMIT, 100, 1000);

    VenueConfig dark_cfg = make_venue_config(DARK, 0.0, 0, 0.0, 0.4);
    VenueConfig lit_cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, dark_cfg, &dark_lob},
        VenueState{2, lit_cfg, &lit_lob}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_naive_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 100);
}

TEST(DPEngineNaiveSplitTest, NoLitVenues_ReturnsZeroAllocationsAndMaxCost) {
    LimitOrderBook dark_lob;
    VenueConfig dark_cfg = make_venue_config(DARK, 0.0, 0, 0.0, 0.3);
    std::vector<VenueState> venues{VenueState{1, dark_cfg, &dark_lob}};

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_naive_split(100, BUY, 200, venues);

    ASSERT_EQ(result.allocations.size(), 1u);
    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.expected_cost, kMaxCost);
}

TEST(DPEngineNaiveSplitTest, IgnoresDepth_CanPickVenueThatCannotAbsorbTheOrder) {
    LimitOrderBook thin_lob, deep_lob;
    thin_lob.submit(SELL, LIMIT, 100, 1);
    deep_lob.submit(SELL, LIMIT, 101, 100000);

    VenueConfig cfg = make_venue_config(LIT, 0.0, 0, 1.0);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &thin_lob},
        VenueState{2, cfg, &deep_lob}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_naive_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 100);
    EXPECT_EQ(result.allocations[1], 0);
    EXPECT_DOUBLE_EQ(result.expected_cost, 10000.0);
}

TEST(DPEngineProportionalSplitTest, SplitsInProportionToDisplayedSize) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 100, 750);
    lob_b.submit(SELL, LIMIT, 100, 250);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &lob_a},
        VenueState{2, cfg, &lob_b}
    };

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_proportional_split(200, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 150);
    EXPECT_EQ(result.allocations[1], 50);
}

TEST(DPEngineProportionalSplitTest, IgnoresPrice_SendsMostToDeepestVenueEvenIfItIsWorse) {
    LimitOrderBook deep_expensive, thin_cheap;
    deep_expensive.submit(SELL, LIMIT, 105, 900);
    thin_cheap.submit(SELL, LIMIT, 100, 100);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &deep_expensive},
        VenueState{2, cfg, &thin_cheap}
    };

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_proportional_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 90);
    EXPECT_EQ(result.allocations[1], 10);
}

TEST(DPEngineProportionalSplitTest, IgnoresDarkVenues) {
    LimitOrderBook dark_lob, lit_lob;
    lit_lob.submit(SELL, LIMIT, 100, 1000);

    VenueConfig dark_cfg = make_venue_config(DARK, 0.0, 0, 0.0, 0.4);
    VenueConfig lit_cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, dark_cfg, &dark_lob},
        VenueState{2, lit_cfg, &lit_lob}
    };

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_proportional_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 100);
}

TEST(DPEngineProportionalSplitTest, SkipsVenuesWithNothingDisplayedAtTheLimitPrice) {
    LimitOrderBook in_range, out_of_range;
    in_range.submit(SELL, LIMIT, 100, 500);
    out_of_range.submit(SELL, LIMIT, 300, 500);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &in_range},
        VenueState{2, cfg, &out_of_range}
    };

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_proportional_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 100);
    EXPECT_EQ(result.allocations[1], 0);
}

TEST(DPEngineProportionalSplitTest, NoDisplayedLiquidity_ReturnsZeroAllocationsAndMaxCost) {
    LimitOrderBook empty_lob;
    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{VenueState{1, cfg, &empty_lob}};

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_proportional_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.expected_cost, kMaxCost);
}

TEST(DPEngineProportionalSplitTest, AllocatesEveryShare_IncludingSubLotRemainder) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 100, 700);
    lob_b.submit(SELL, LIMIT, 100, 300);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &lob_a},
        VenueState{2, cfg, &lob_b}
    };

    DPEngine engine(make_router_config(27));
    SplitResult result = engine.compute_proportional_split(1000, BUY, 200, venues);

    int64_t total_allocated = 0;
    for (int64_t allocation : result.allocations) total_allocated += allocation;
    EXPECT_EQ(total_allocated, 1000);
    EXPECT_GT(result.allocations[0], result.allocations[1]);
}

TEST(DPEngineProportionalSplitTest, OrderSmallerThanOneLot_StillFullyAllocated) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 100, 100);
    lob_b.submit(SELL, LIMIT, 100, 900);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &lob_a},
        VenueState{2, cfg, &lob_b}
    };

    DPEngine engine(make_router_config(50));
    SplitResult result = engine.compute_proportional_split(20, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 20);
}

TEST(DPEngineOptimalSplitTest, SingleVenueSufficientLiquidity_AllocatesEverything) {
    LimitOrderBook lob;
    lob.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{VenueState{1, cfg, &lob}};

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(100, BUY, 200, venues);

    ASSERT_EQ(result.allocations.size(), 1u);
    EXPECT_EQ(result.allocations[0], 100);
    EXPECT_DOUBLE_EQ(result.expected_cost, 0.0);
}

TEST(DPEngineOptimalSplitTest, PrefersCheaperVenue_WhenImpactIsZero) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 100, 100000);
    lob_b.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg_a = make_venue_config(LIT, 0.01);
    VenueConfig cfg_b = make_venue_config(LIT, 0.001);
    std::vector<VenueState> venues{
        VenueState{1, cfg_a, &lob_a},
        VenueState{2, cfg_b, &lob_b}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 100);
    EXPECT_DOUBLE_EQ(result.expected_cost, 0.1);
}

TEST(DPEngineOptimalSplitTest, SplitsEvenlyAcrossIdenticalVenues_WhenImpactPenalizesConcentration) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 100, 100000);
    lob_b.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg = make_venue_config(LIT, 0.0, 0, 1.0);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &lob_a},
        VenueState{2, cfg, &lob_b}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 50);
    EXPECT_EQ(result.allocations[1], 50);
    EXPECT_DOUBLE_EQ(result.expected_cost, 0.05);
}

TEST(DPEngineOptimalSplitTest, TotalSizeNotMultipleOfLotSize_RemainderStillAllocated) {
    LimitOrderBook lob;
    lob.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{VenueState{1, cfg, &lob}};

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_optimal_split(105, BUY, 200, venues);

    int64_t total_allocated = 0;
    for (int64_t a : result.allocations) total_allocated += a;
    EXPECT_EQ(total_allocated, 105);
    EXPECT_EQ(result.allocations[0], 105);
}

TEST(DPEngineOptimalSplitTest, RemainderGoesToVenueCheapestForThatSmallAmount) {
    LimitOrderBook lob_a, lob_b;
    lob_a.submit(SELL, LIMIT, 100, 100000);
    lob_b.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg_a = make_venue_config(LIT, 0.01);
    VenueConfig cfg_b = make_venue_config(LIT, 0.001);
    std::vector<VenueState> venues{
        VenueState{1, cfg_a, &lob_a},
        VenueState{2, cfg_b, &lob_b}
    };

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_optimal_split(105, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 105);
}

TEST(DPEngineOptimalSplitTest, RemainderIsNotAllocated_WhenNoVenueCanAbsorbIt) {
    LimitOrderBook empty_lob;

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{VenueState{1, cfg, &empty_lob}};

    DPEngine engine(make_router_config(10));
    SplitResult result = engine.compute_optimal_split(5, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
}

TEST(DPEngineOptimalSplitTest, ZeroTotalSize_ReturnsZeroCostAndZeroAllocations) {
    LimitOrderBook lob;
    lob.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{VenueState{1, cfg, &lob}};

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(0, BUY, 200, venues);

    ASSERT_EQ(result.allocations.size(), 1u);
    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_DOUBLE_EQ(result.expected_cost, 0.0);
}

TEST(DPEngineOptimalSplitTest, EmptyVenues_ReturnsEmptyAllocations) {
    std::vector<VenueState> venues{};

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(100, BUY, 200, venues);

    EXPECT_TRUE(result.allocations.empty());
    EXPECT_EQ(result.expected_cost, kMaxCost);
}

TEST(DPEngineOptimalSplitTest, LatencyCostOnlyAppliesWhenSomethingIsActuallyRouted) {
    LimitOrderBook lob;
    lob.submit(SELL, LIMIT, 100, 100000);

    VenueConfig cfg = make_venue_config(LIT, 0.0, 100);
    std::vector<VenueState> venues{VenueState{1, cfg, &lob}};

    DPEngine engine(make_router_config(10, 1));

    SplitResult zero_size = engine.compute_optimal_split(0, BUY, 200, venues);
    EXPECT_EQ(zero_size.allocations[0], 0);
    EXPECT_DOUBLE_EQ(zero_size.expected_cost, 0.0);

    SplitResult full_size = engine.compute_optimal_split(100, BUY, 200, venues);
    EXPECT_EQ(full_size.allocations[0], 100);
    EXPECT_DOUBLE_EQ(full_size.expected_cost, 100.0);
}

TEST(DPEngineOptimalSplitTest, IlliquidVenueIsSkipped_LiquidVenueLast) {
    LimitOrderBook liquid_lob, illiquid_lob;
    liquid_lob.submit(SELL, LIMIT, 100, 1000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &liquid_lob},
        VenueState{2, cfg, &illiquid_lob}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 100);
    EXPECT_EQ(result.allocations[1], 0);
    EXPECT_DOUBLE_EQ(result.expected_cost, 0.0);
}

TEST(DPEngineOptimalSplitTest, IlliquidVenueIsSkipped_LiquidVenueFirst) {
    LimitOrderBook liquid_lob, illiquid_lob;
    liquid_lob.submit(SELL, LIMIT, 100, 1000);

    VenueConfig cfg = make_venue_config(LIT);
    std::vector<VenueState> venues{
        VenueState{1, cfg, &illiquid_lob},
        VenueState{2, cfg, &liquid_lob}
    };

    DPEngine engine(make_router_config());
    SplitResult result = engine.compute_optimal_split(100, BUY, 200, venues);

    EXPECT_EQ(result.allocations[0], 0);
    EXPECT_EQ(result.allocations[1], 100);
    EXPECT_DOUBLE_EQ(result.expected_cost, 0.0);
}

}  // namespace
