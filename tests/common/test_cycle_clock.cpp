#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <thread>

#include "common/CycleClock.h"
#include "common/HostInfo.h"

namespace {

TEST(CycleClockTest, CalibrationIsPositiveAndStable) {
    double rate = cycles_per_ns();
    EXPECT_GT(rate, 0.0);
    EXPECT_DOUBLE_EQ(rate, cycles_per_ns());
}

TEST(CycleClockTest, CounterIsMonotonic) {
    uint64_t previous = cycle_now();
    for (int i = 0; i < 1000; ++i) {
        uint64_t current = cycle_now();
        ASSERT_GE(current, previous);
        previous = current;
    }
}

TEST(CycleClockTest, MeasuredSleepLandsInSaneBand) {
    constexpr auto SLEEP = std::chrono::milliseconds(20);

    uint64_t start = cycle_now();
    std::this_thread::sleep_for(SLEEP);
    double measured_ns = cycles_to_ns(cycle_now() - start);

    EXPECT_GT(measured_ns, 5e6);
    EXPECT_LT(measured_ns, 2e9);
}

TEST(CycleClockTest, BackToBackReadsCostLessThanAMicrosecond) {
    double best_ns = 0.0;
    for (int i = 0; i < 2000; ++i) {
        uint64_t a = cycle_now();
        uint64_t b = cycle_now();
        if (b > a) {
            double ns = cycles_to_ns(b - a);
            if (best_ns == 0.0 || ns < best_ns) best_ns = ns;
        }
    }

    ASSERT_GT(best_ns, 0.0);
    EXPECT_LT(best_ns, 1000.0);
}

TEST(HostInfoTest, ReportsPopulatedFields) {
    HostInfo host = collect_host_info();

    EXPECT_FALSE(host.os.empty());
    EXPECT_FALSE(host.cpu_brand.empty());
    EXPECT_FALSE(host.timer_backend.empty());
    EXPECT_GT(host.cycles_per_ns, 0.0);
    EXPECT_GE(host.logical_cpus, 1u);
    EXPECT_EQ(host.pinned_cpu, -1);
}

TEST(HostInfoTest, DescribeHostMentionsTimerBackend) {
    HostInfo host = collect_host_info();
    std::string description = describe_host(host);

    EXPECT_NE(description.find("timer="), std::string::npos);
    EXPECT_NE(description.find(host.timer_backend), std::string::npos);
}

TEST(HostInfoTest, PinRejectsNegativeCpu) {
    EXPECT_FALSE(pin_current_thread(-1));
}

}  // namespace
