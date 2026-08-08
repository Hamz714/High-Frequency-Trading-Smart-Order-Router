#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define HFT_CYCLE_CLOCK_TSC 1
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <x86intrin.h>
    #endif
#else
    #define HFT_CYCLE_CLOCK_TSC 0
#endif

inline uint64_t cycle_now() {
#if HFT_CYCLE_CLOCK_TSC
    _mm_lfence();
    uint64_t ticks = __rdtsc();
    _mm_lfence();
    return ticks;
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

double cycles_per_ns();

const char* cycle_clock_backend();

inline double cycles_to_ns(uint64_t ticks) {
    return static_cast<double>(ticks) / cycles_per_ns();
}
