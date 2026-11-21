#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "lib/helper_queue.h"

#include <gtest/gtest.h>

namespace {

TEST(HelperQueueTest, PreservesFifoWhileSnapshotsRaceWithTraffic) {
    constexpr std::size_t kItemCount = 4096;
    mako::HelperQueue queue(0, true);
    std::vector<std::size_t> values(kItemCount);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = i;
    }

    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        for (std::size_t i = 0; i < values.size(); ++i) {
            while (queue.is_req_buffer_full()) {
                std::this_thread::yield();
            }
            if (!queue.add_one_req(&values[i], i)) {
                failed.store(true, std::memory_order_relaxed);
                break;
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::size_t expected = 0;
        while (expected < values.size()) {
            void* handle = nullptr;
            std::size_t msg_size = 0;
            if (!queue.fetch_one_req(&handle, msg_size)) {
                if (producer_done.load(std::memory_order_acquire) &&
                    queue.is_req_buffer_empty()) {
                    failed.store(true, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
                continue;
            }
            if (handle != &values[expected] || msg_size != expected) {
                failed.store(true, std::memory_order_relaxed);
                break;
            }
            ++expected;
        }
        consumer_done.store(true, std::memory_order_release);
    });

    std::thread observer([&] {
        while (!consumer_done.load(std::memory_order_acquire)) {
            if (queue.get_size() > HELPER_QUEUE_SIZE) {
                failed.store(true, std::memory_order_relaxed);
            }
            (void)queue.is_req_buffer_empty();
            (void)queue.is_req_buffer_full();
        }
    });

    producer.join();
    consumer.join();
    observer.join();

    EXPECT_FALSE(failed.load(std::memory_order_relaxed));
    EXPECT_TRUE(queue.is_req_buffer_empty());
    EXPECT_EQ(queue.get_size(), 0U);
}

TEST(HelperQueueTest, SuspendWakesForAnEnqueuedRequest) {
    mako::HelperQueue queue(0, true);
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::size_t token = 7;

    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        queue.suspend();
        returned.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    ASSERT_TRUE(queue.add_one_req(&token, sizeof(token)));
    waiter.join();
    EXPECT_TRUE(returned.load(std::memory_order_acquire));

    void* handle = nullptr;
    std::size_t msg_size = 0;
    ASSERT_TRUE(queue.fetch_one_req(&handle, msg_size));
    EXPECT_EQ(handle, &token);
    EXPECT_EQ(msg_size, sizeof(token));
}

TEST(HelperQueueTest, SuspendWakesForStop) {
    mako::HelperQueue queue(0, true);
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};

    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        queue.suspend();
        returned.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    queue.request_stop();
    waiter.join();
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
    EXPECT_TRUE(queue.should_stop());
    EXPECT_TRUE(queue.is_req_buffer_empty());
}

}  // namespace
