#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "common/Constants.h"

template<typename T>
class MPSCQueue {
    struct alignas(64) Slot {
        std::atomic<size_t> sequence;
        T data;
    };

    alignas(64) std::atomic<size_t> enqueue_pos_{0};

    alignas(64) size_t dequeue_pos_{0};

    alignas(64) std::atomic<uint64_t> dropped_{0};

    alignas(64) const size_t mask_;
    const std::unique_ptr<Slot[]> buffer_;

public:
    explicit MPSCQueue(size_t capacity = DEFAULT_QUEUE_CAPACITY)
        : mask_(std::bit_ceil(capacity) - 1),
          buffer_(std::make_unique<Slot[]>(mask_ + 1)) {
        for (size_t i = 0; i <= mask_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;

    size_t capacity() const { return mask_ + 1; }

    bool push(const T& item) {
        Slot* slot;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        while (true) {
            slot = &buffer_[pos & mask_];
            size_t seq = slot->sequence.load(std::memory_order_acquire);

            intptr_t diff = (intptr_t)seq - (intptr_t)pos;

            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        slot->data = item;

        slot->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        Slot* slot = &buffer_[dequeue_pos_ & mask_];
        size_t seq = slot->sequence.load(std::memory_order_acquire);

        intptr_t diff = (intptr_t)seq - (intptr_t)(dequeue_pos_ + 1);

        if (diff == 0) {
            item = slot->data;

            slot->sequence.store(dequeue_pos_ + mask_ + 1, std::memory_order_release);

            dequeue_pos_++;
            return true;
        }

        return false;
    }

    uint64_t dropped() const {
        return dropped_.load(std::memory_order_relaxed);
    }
};
