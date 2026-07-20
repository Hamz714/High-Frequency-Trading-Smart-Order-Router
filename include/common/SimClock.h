#pragma once

#include <atomic>

class SimClock {
    std::atomic<double> current_time{0.0};

public:
    void advance(double dt) {
        current_time.fetch_add(dt, std::memory_order_relaxed);
    }

    double now() const {
        return current_time.load(std::memory_order_relaxed);
    }
};
