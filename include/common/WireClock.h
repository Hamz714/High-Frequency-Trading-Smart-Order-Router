#pragma once

#include <chrono>
#include <cstdint>

inline int64_t wire_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline int64_t wire_arrival_ns(int64_t latency_us) {
    return wire_now_ns() + latency_us * 1000;
}
