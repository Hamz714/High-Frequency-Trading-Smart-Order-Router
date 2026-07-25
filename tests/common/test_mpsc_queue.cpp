#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "common/MPSCQueue.h"

namespace {

struct Point {
    int64_t x;
    int64_t y;
};

TEST(MPSCQueueTest, NewQueue_IsEmpty_PopFails) {
    MPSCQueue<int, 4> queue;
    int out;
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(MPSCQueueTest, PushThenPop_ReturnsSameValue) {
    MPSCQueue<int, 4> queue;
    EXPECT_TRUE(queue.push(42));

    int out = 0;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 42);
}

TEST(MPSCQueueTest, PushMultiple_PopsInFifoOrder) {
    MPSCQueue<int, 8> queue;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.push(i));
    }

    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(MPSCQueueTest, FullCapacityIsUsable_AllSlotsHoldValues) {
    MPSCQueue<int, 4> queue;
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.push(3));
    EXPECT_TRUE(queue.push(4));

    EXPECT_FALSE(queue.push(5));
}

TEST(MPSCQueueTest, PopThenPush_ReusesFreedSlot) {
    MPSCQueue<int, 4> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);
    queue.push(4);
    ASSERT_FALSE(queue.push(5));

    int out;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 1);

    EXPECT_TRUE(queue.push(5));

    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 2);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 3);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 4);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 5);
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(MPSCQueueTest, PopOnEmpty_LeavesOutputParameterUnchanged) {
    MPSCQueue<int, 4> queue;
    int out = 12345;
    EXPECT_FALSE(queue.try_pop(out));
    EXPECT_EQ(out, 12345);
}

TEST(MPSCQueueTest, RepeatedFullDrainCycles_WrapAroundStaysConsistent) {
    MPSCQueue<int, 4> queue;
    int next_pushed = 0;
    int next_expected = 0;

    for (int cycle = 0; cycle < 50; ++cycle) {
        ASSERT_TRUE(queue.push(next_pushed++));
        ASSERT_TRUE(queue.push(next_pushed++));
        ASSERT_TRUE(queue.push(next_pushed++));

        int out;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, next_expected++);
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, next_expected++);
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, next_expected++);
    }
}

TEST(MPSCQueueTest, PreservesFullStructContents) {
    MPSCQueue<Point, 4> queue;
    ASSERT_TRUE(queue.push(Point{7, -3}));

    Point out{};
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out.x, 7);
    EXPECT_EQ(out.y, -3);
}

TEST(MPSCQueueTest, ConcurrentMultipleProducersSingleConsumer_AllItemsDeliveredExactlyOnce) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 50'000;
    constexpr int kTotal = kProducers * kPerProducer;

    MPSCQueue<int, 64> queue;
    std::atomic<int> next_value{0};
    std::vector<int> received;
    received.reserve(kTotal);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                int value = next_value.fetch_add(1, std::memory_order_relaxed);
                while (!queue.push(value)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::thread consumer([&] {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        int out;
        while (static_cast<int>(received.size()) < kTotal && std::chrono::steady_clock::now() < deadline) {
            if (queue.try_pop(out)) {
                received.push_back(out);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kTotal));

    std::sort(received.begin(), received.end());
    for (int i = 0; i < kTotal; ++i) {
        ASSERT_EQ(received[i], i) << "missing or duplicated value at sorted index " << i;
    }
}

TEST(MPSCQueueTest, CapacityOne_SecondPushPermanentlyWedgesPop) {
    MPSCQueue<int, 1> queue;
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));

    int out;
    EXPECT_FALSE(queue.try_pop(out));

    EXPECT_TRUE(queue.push(3));
    EXPECT_FALSE(queue.try_pop(out));
}

}  // namespace
