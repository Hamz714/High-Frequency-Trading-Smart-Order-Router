#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>

template <typename T>
class ThreadSafeQueue {
    std::queue<T> raw_queue;
    mutable std::mutex mtx;
    std::condition_variable cv;

    public:
        ThreadSafeQueue() = default;

        ThreadSafeQueue(const ThreadSafeQueue&) = delete;
        ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

        void push(T value) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                raw_queue.push(std::move(value));
            }
            cv.notify_one();
        }

        T pop() {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] {return !raw_queue.empty();});
            
            T value = std::move(raw_queue.front());
            raw_queue.pop();

            return value;
        }

        bool empty() const {
            std::lock_guard<std::mutex> lock(mtx);
            return raw_queue.empty();
        }
};