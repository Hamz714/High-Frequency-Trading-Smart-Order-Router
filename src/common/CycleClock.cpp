#include "common/CycleClock.h"

#include <algorithm>
#include <vector>

namespace {

double calibrate_once(std::chrono::nanoseconds window) {
    auto wall_start = std::chrono::steady_clock::now();
    uint64_t tick_start = cycle_now();

    while (std::chrono::steady_clock::now() - wall_start < window) {
    }

    uint64_t tick_end = cycle_now();
    auto wall_end = std::chrono::steady_clock::now();

    double elapsed_ns = std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
    if (elapsed_ns <= 0.0) return 0.0;
    return static_cast<double>(tick_end - tick_start) / elapsed_ns;
}

double calibrate() {
#if HFT_CYCLE_CLOCK_TSC
    constexpr int SAMPLES = 5;
    constexpr auto WINDOW = std::chrono::milliseconds(20);

    std::vector<double> rates;
    rates.reserve(SAMPLES);
    for (int i = 0; i < SAMPLES; ++i) {
        double rate = calibrate_once(WINDOW);
        if (rate > 0.0) rates.push_back(rate);
    }

    if (rates.empty()) return 1.0;
    std::sort(rates.begin(), rates.end());
    return rates[rates.size() / 2];
#else
    return 1.0;
#endif
}

}  // namespace

double cycles_per_ns() {
    static const double rate = calibrate();
    return rate;
}

const char* cycle_clock_backend() {
#if HFT_CYCLE_CLOCK_TSC
    return "rdtsc";
#else
    return "steady_clock";
#endif
}
