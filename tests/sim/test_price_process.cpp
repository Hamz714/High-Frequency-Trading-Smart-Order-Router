#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "sim/PriceProcess.h"

namespace {

TEST(PriceProcessTest, Constructor_ReportsInitialPriceBeforeAnyStep) {
    PriceProcess process(100.0, 0.05, 0.2, 1.0 / 252, 42);
    EXPECT_DOUBLE_EQ(process.get_current_price(), 100.0);
}

TEST(PriceProcessTest, GetVolatility_ReturnsConfiguredValue_UnaffectedBySteps) {
    PriceProcess process(100.0, 0.05, 0.2, 1.0 / 252, 42);
    process.step();
    process.step();
    EXPECT_DOUBLE_EQ(process.get_volatility(), 0.2);
}

TEST(PriceProcessTest, ZeroDriftZeroVolatility_PriceNeverChanges) {
    PriceProcess process(100.0, 0.0, 0.0, 1.0 / 252, 42);
    for (int i = 0; i < 100; ++i) {
        EXPECT_DOUBLE_EQ(process.step(), 100.0);
    }
    EXPECT_DOUBLE_EQ(process.get_current_price(), 100.0);
}

TEST(PriceProcessTest, ZeroVolatility_FollowsDeterministicDriftPath) {
    double initial = 100.0, drift = 0.1, dt = 1.0 / 252;
    PriceProcess process(initial, drift, 0.0, dt, 42);

    double expected = initial;
    for (int i = 0; i < 50; ++i) {
        expected *= std::exp(drift * dt);
        double actual = process.step();
        EXPECT_NEAR(actual, expected, expected * 1e-9);
    }
}

TEST(PriceProcessTest, Step_ReturnValueMatchesGetCurrentPrice) {
    PriceProcess process(100.0, 0.05, 0.2, 1.0 / 252, 42);
    double returned = process.step();
    EXPECT_DOUBLE_EQ(returned, process.get_current_price());
}

TEST(PriceProcessTest, SameSeed_ProducesIdenticalPricePath) {
    PriceProcess a(100.0, 0.05, 0.3, 1.0 / 252, 7);
    PriceProcess b(100.0, 0.05, 0.3, 1.0 / 252, 7);

    for (int i = 0; i < 20; ++i) {
        EXPECT_DOUBLE_EQ(a.step(), b.step());
    }
}

TEST(PriceProcessTest, DifferentSeeds_ProduceDivergentPricePaths) {
    PriceProcess a(100.0, 0.05, 0.3, 1.0 / 252, 1);
    PriceProcess b(100.0, 0.05, 0.3, 1.0 / 252, 2);

    bool diverged = false;
    for (int i = 0; i < 20; ++i) {
        if (a.step() != b.step()) diverged = true;
    }
    EXPECT_TRUE(diverged);
}

TEST(PriceProcessTest, HighVolatility_PriceStaysStrictlyPositive) {
    PriceProcess process(100.0, 0.0, 1.5, 1.0 / 252, 123);
    for (int i = 0; i < 5000; ++i) {
        EXPECT_GT(process.step(), 0.0);
    }
}

TEST(PriceProcessTest, LogReturns_SampleMeanMatchesTheoreticalDrift) {
    double drift = 0.4, vol = 0.2, dt = 1.0 / 252;
    PriceProcess process(100.0, drift, vol, dt, 99);

    const int N = 200000;
    double sum_log_returns = 0.0;
    double prev = process.get_current_price();
    for (int i = 0; i < N; ++i) {
        double next = process.step();
        sum_log_returns += std::log(next / prev);
        prev = next;
    }

    double sample_mean = sum_log_returns / N;
    double theoretical_mean = (drift - 0.5 * vol * vol) * dt;
    double standard_error = vol * std::sqrt(dt) / std::sqrt(static_cast<double>(N));

    EXPECT_NEAR(sample_mean, theoretical_mean, 5 * standard_error);
}

TEST(PriceProcessTest, LogReturns_SampleVarianceMatchesTheoreticalVolatility) {
    double drift = 0.0, vol = 0.3, dt = 1.0 / 252;
    PriceProcess process(100.0, drift, vol, dt, 99);

    const int N = 200000;
    std::vector<double> log_returns;
    log_returns.reserve(N);
    double prev = process.get_current_price();
    for (int i = 0; i < N; ++i) {
        double next = process.step();
        log_returns.push_back(std::log(next / prev));
        prev = next;
    }

    double mean = 0.0;
    for (double r : log_returns) mean += r;
    mean /= N;

    double sum_sq = 0.0;
    for (double r : log_returns) sum_sq += (r - mean) * (r - mean);
    double sample_variance = sum_sq / N;

    double theoretical_variance = vol * vol * dt;
    EXPECT_NEAR(sample_variance, theoretical_variance, theoretical_variance * 0.05);
}

}  // namespace
