#pragma once

#include <atomic>
#include <cstddef>

template<typename T, size_t Capacity>
class SPSCQueue {
private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    T buffer_[Capacity];

public:
    SPSCQueue() = default;
    
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    bool push(const T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) % Capacity;

        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; 
        }

        buffer_[current_head] = item;

        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        if (current_tail == head_.load(std::memory_order_acquire)) {
            return false; 
        }

        item = buffer_[current_tail];
        const size_t next_tail = (current_tail + 1) % Capacity;

        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
};