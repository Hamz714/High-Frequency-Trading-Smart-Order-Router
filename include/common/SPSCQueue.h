#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/Constants.h"

template<typename T>
class SPSCQueue {
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    alignas(64) std::atomic<uint64_t> dropped_{0};

    alignas(64) const size_t mask_;
    const std::unique_ptr<T[]> buffer_;

public:
    explicit SPSCQueue(size_t capacity = DEFAULT_QUEUE_CAPACITY)
        : mask_(std::bit_ceil(capacity) - 1),
          buffer_(std::make_unique<T[]>(mask_ + 1)) {}

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    size_t capacity() const { return mask_ + 1; }

    bool push(const T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & mask_;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        buffer_[current_head] = item;

        head_.store(next_head, std::memory_order_release);
        return true;
    }

    uint64_t dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }

    bool try_pop(T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false;
        }

        item = buffer_[current_tail];
        const size_t next_tail = (current_tail + 1) & mask_;

        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
};
