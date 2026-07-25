#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>

#include "common/SPSCQueue.h"

namespace {

struct Point {
    int64_t x;
    int64_t y;
};

TEST(SPSCQueueTest, NewQueue_IsEmpty_PopFails) {
    SPSCQueue<int, 4> queue;
    int out;
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(SPSCQueueTest, PushThenPop_ReturnsSameValue) {
    SPSCQueue<int, 4> queue;
    EXPECT_TRUE(queue.push(42));

    int out = 0;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 42);
}

TEST(SPSCQueueTest, PushMultiple_PopsInFifoOrder) {
    SPSCQueue<int, 8> queue;
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.push(i));
    }

    for (int i = 0; i < 5; ++i) {
        int out = -1;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(SPSCQueueTest, UsableCapacity_IsOneLessThanTemplateCapacity) {
    SPSCQueue<int, 4> queue;
    EXPECT_TRUE(queue.push(1));
    EXPECT_TRUE(queue.push(2));
    EXPECT_TRUE(queue.push(3));
    EXPECT_FALSE(queue.push(4));
}

TEST(SPSCQueueTest, Push_FailsWhenFull_WithoutOverwritingOldestEntry) {
    SPSCQueue<int, 4> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);

    EXPECT_FALSE(queue.push(99));

    int out = -1;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 1);
}

TEST(SPSCQueueTest, PopOnEmpty_LeavesOutputParameterUnchanged) {
    SPSCQueue<int, 4> queue;
    int out = 12345;
    EXPECT_FALSE(queue.try_pop(out));
    EXPECT_EQ(out, 12345);
}

TEST(SPSCQueueTest, FullThenDrain_FreesSlotsForMorePushes) {
    SPSCQueue<int, 4> queue;
    queue.push(1);
    queue.push(2);
    queue.push(3);
    ASSERT_FALSE(queue.push(4));

    int out;
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 1);

    EXPECT_TRUE(queue.push(4));

    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 2);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 3);
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out, 4);
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(SPSCQueueTest, RepeatedPushPopCycles_WrapAroundStaysConsistent) {
    SPSCQueue<int, 4> queue;
    int next_pushed = 0;
    int next_expected = 0;

    for (int cycle = 0; cycle < 50; ++cycle) {
        ASSERT_TRUE(queue.push(next_pushed++));
        ASSERT_TRUE(queue.push(next_pushed++));

        int out;
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, next_expected++);
        ASSERT_TRUE(queue.try_pop(out));
        EXPECT_EQ(out, next_expected++);
    }
}

TEST(SPSCQueueTest, CapacityOne_QueueCanNeverHoldAnItem) {
    SPSCQueue<int, 1> queue;
    EXPECT_FALSE(queue.push(1));

    int out;
    EXPECT_FALSE(queue.try_pop(out));
}

TEST(SPSCQueueTest, PreservesFullStructContents) {
    SPSCQueue<Point, 4> queue;
    ASSERT_TRUE(queue.push(Point{7, -3}));

    Point out{};
    ASSERT_TRUE(queue.try_pop(out));
    EXPECT_EQ(out.x, 7);
    EXPECT_EQ(out.y, -3);
}

TEST(SPSCQueueTest, ConcurrentSingleProducerSingleConsumer_AllItemsDeliveredInOrder) {
    constexpr int kCount = 200'000;
    SPSCQueue<int, 64> queue;

    std::vector<int> received;
    received.reserve(kCount);

    std::thread producer([&queue] {
        for (int i = 0; i < kCount; ++i) {
            while (!queue.push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&queue, &received] {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        int out;
        while (static_cast<int>(received.size()) < kCount && std::chrono::steady_clock::now() < deadline) {
            if (queue.try_pop(out)) {
                received.push_back(out);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[i], i) << "mismatch at index " << i;
    }
}

}  // namespace
