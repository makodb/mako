/**
 * Unit tests for RPC RequestQueue.
 * Tests queue operations, overflow strategies, and TTL expiration.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include "../rrr.hpp"

using namespace rrr;
using namespace std::chrono;

// ============================================================================
// Basic Operations Tests
// ============================================================================

TEST(RequestQueueTest, InitiallyEmpty) {
    RequestQueue queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_FALSE(queue.full());
}

TEST(RequestQueueTest, EnqueueSingleRequest) {
    RequestQueue queue;

    QueuedRequest req;
    req.xid = 12345;
    req.rpc_id = 1;

    EXPECT_TRUE(queue.enqueue(std::move(req)));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size(), 1u);
}

TEST(RequestQueueTest, DequeueRequest) {
    RequestQueue queue;

    QueuedRequest req;
    req.xid = 12345;
    req.rpc_id = 42;

    queue.enqueue(std::move(req));

    auto result = queue.dequeue();
    ASSERT_TRUE(result.is_some());

    auto dequeued = result.unwrap();
    EXPECT_EQ(dequeued.xid, 12345);
    EXPECT_EQ(dequeued.rpc_id, 42);
    EXPECT_TRUE(queue.empty());
}

TEST(RequestQueueTest, DequeueFromEmptyReturnsNone) {
    RequestQueue queue;
    auto result = queue.dequeue();
    EXPECT_TRUE(result.is_none());
}

TEST(RequestQueueTest, FifoOrder) {
    RequestQueue queue;

    for (int i = 0; i < 5; i++) {
        QueuedRequest req;
        req.xid = i;
        queue.enqueue(std::move(req));
    }

    for (int i = 0; i < 5; i++) {
        auto result = queue.dequeue();
        ASSERT_TRUE(result.is_some());
        EXPECT_EQ(result.unwrap().xid, i);
    }
}

// L5o (2026-05-01): the prior `Peek` and `PeekEmptyReturnsFalse`
// tests went away with the `RequestQueue::peek(QueuedRequest&)`
// method itself — once `QueuedRequest::callback` migrated to
// move-only rusty::Function, peek's `out = guard->front();` copy
// no longer compiled.  The method was tests-only (no production
// callers); equivalent post-enqueue inspection happens via
// `size()` / `empty()` and via dequeue (which moves out).

TEST(RequestQueueTest, EnqueueIncreasesSize) {
    RequestQueue queue;
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_TRUE(queue.empty());

    QueuedRequest req;
    req.xid = 999;
    queue.enqueue(std::move(req));

    EXPECT_EQ(queue.size(), 1u);
    EXPECT_FALSE(queue.empty());
}

TEST(RequestQueueTest, EmptyQueueDequeueReturnsNone) {
    RequestQueue queue;
    auto result = queue.dequeue();
    EXPECT_TRUE(result.is_none());
}

// ============================================================================
// Size Limits Tests
// ============================================================================

TEST(RequestQueueTest, RespectMaxSize) {
    RequestQueueConfig config;
    config.max_size = 5;
    config.overflow_strategy = OverflowStrategy::DROP_NEWEST;
    RequestQueue queue(config);

    for (int i = 0; i < 10; i++) {
        QueuedRequest req;
        req.xid = i;
        queue.enqueue(std::move(req));
    }

    // Only first 5 should be in queue
    EXPECT_EQ(queue.size(), 5u);
    EXPECT_TRUE(queue.full());
}

TEST(RequestQueueTest, FullCheck) {
    RequestQueueConfig config;
    config.max_size = 3;
    RequestQueue queue(config);

    EXPECT_FALSE(queue.full());

    for (int i = 0; i < 3; i++) {
        QueuedRequest req;
        queue.enqueue(std::move(req));
    }

    EXPECT_TRUE(queue.full());
}

TEST(RequestQueueTest, RemainingCapacity) {
    RequestQueueConfig config;
    config.max_size = 10;
    RequestQueue queue(config);

    EXPECT_EQ(queue.remaining_capacity(), 10u);

    for (int i = 0; i < 3; i++) {
        QueuedRequest req;
        queue.enqueue(std::move(req));
    }

    EXPECT_EQ(queue.remaining_capacity(), 7u);
}

// ============================================================================
// Overflow Strategy Tests
// ============================================================================

TEST(RequestQueueTest, OverflowDropOldest) {
    RequestQueueConfig config;
    config.max_size = 3;
    config.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    RequestQueue queue(config);

    for (int i = 0; i < 5; i++) {
        QueuedRequest req;
        req.xid = i;
        EXPECT_TRUE(queue.enqueue(std::move(req)));
    }

    EXPECT_EQ(queue.size(), 3u);

    // Should have 2, 3, 4 (oldest 0, 1 dropped)
    auto r1 = queue.dequeue();
    ASSERT_TRUE(r1.is_some());
    EXPECT_EQ(r1.unwrap().xid, 2);

    auto r2 = queue.dequeue();
    ASSERT_TRUE(r2.is_some());
    EXPECT_EQ(r2.unwrap().xid, 3);

    auto r3 = queue.dequeue();
    ASSERT_TRUE(r3.is_some());
    EXPECT_EQ(r3.unwrap().xid, 4);
}

TEST(RequestQueueTest, OverflowDropNewest) {
    RequestQueueConfig config;
    config.max_size = 3;
    config.overflow_strategy = OverflowStrategy::DROP_NEWEST;
    RequestQueue queue(config);

    for (int i = 0; i < 5; i++) {
        QueuedRequest req;
        req.xid = i;
        bool result = queue.enqueue(std::move(req));
        if (i < 3) {
            EXPECT_TRUE(result) << "First 3 should succeed";
        } else {
            EXPECT_FALSE(result) << "4th and 5th should fail";
        }
    }

    EXPECT_EQ(queue.size(), 3u);

    // Should have 0, 1, 2
    for (int i = 0; i < 3; i++) {
        auto result = queue.dequeue();
        ASSERT_TRUE(result.is_some());
        EXPECT_EQ(result.unwrap().xid, i);
    }
}

TEST(RequestQueueTest, OverflowDropNewestCallsCallback) {
    RequestQueueConfig config;
    config.max_size = 2;
    config.overflow_strategy = OverflowStrategy::DROP_NEWEST;
    RequestQueue queue(config);

    // Fill queue
    for (int i = 0; i < 2; i++) {
        QueuedRequest req;
        queue.enqueue(std::move(req));
    }

    int callback_count = 0;
    int callback_error = 0;
    QueuedRequest req;
    req.callback = [&callback_count, &callback_error](int err) {
        callback_count++;
        callback_error = err;
    };

    EXPECT_FALSE(queue.enqueue(std::move(req)));
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(callback_error, kRequestQueueRejectedError);
    EXPECT_EQ(queue.size(), 2u);
}

TEST(RequestQueueTest, OverflowFailFastCallsCallback) {
    RequestQueueConfig config;
    config.max_size = 2;
    config.overflow_strategy = OverflowStrategy::FAIL_FAST;
    RequestQueue queue(config);

    // Fill queue
    for (int i = 0; i < 2; i++) {
        QueuedRequest req;
        queue.enqueue(std::move(req));
    }

    // Third request should fail and call callback
    int callback_error = 0;
    QueuedRequest req;
    req.callback = [&callback_error](int err) { callback_error = err; };

    EXPECT_FALSE(queue.enqueue(std::move(req)));
    EXPECT_EQ(callback_error, kRequestQueueRejectedError);
}

TEST(RequestQueueTest, DropOldestCallsCallback) {
    RequestQueueConfig config;
    config.max_size = 2;
    config.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    RequestQueue queue(config);

    int dropped_count = 0;

    // Fill queue with callbacks
    for (int i = 0; i < 2; i++) {
        QueuedRequest req;
        req.xid = i;
        req.callback = [&dropped_count](int err) {
            if (err == kRequestQueueRejectedError) dropped_count++;
        };
        queue.enqueue(std::move(req));
    }

    // Third request should drop oldest
    QueuedRequest req;
    req.xid = 2;
    queue.enqueue(std::move(req));

    EXPECT_EQ(dropped_count, 1);  // First request should have been dropped
}

// ============================================================================
// TTL and Expiration Tests
// ============================================================================

TEST(RequestQueueTest, RequestIsExpiredCheck) {
    QueuedRequest req;
    req.ttl_ms = 10;  // 10ms TTL

    EXPECT_FALSE(req.is_expired());

    std::this_thread::sleep_for(milliseconds(20));

    EXPECT_TRUE(req.is_expired());
}

TEST(RequestQueueTest, RequestAgeMs) {
    QueuedRequest req;

    std::this_thread::sleep_for(milliseconds(50));

    uint32_t age = req.age_ms();
    EXPECT_GE(age, 50u);
    EXPECT_LT(age, 100u);  // Shouldn't be much more than 50ms
}

TEST(RequestQueueTest, ExpireStaleRequests) {
    RequestQueue queue;

    for (int i = 0; i < 5; i++) {
        QueuedRequest req;
        req.xid = i;
        req.ttl_ms = 10;  // Very short TTL - explicitly set
        queue.enqueue(std::move(req));
    }

    EXPECT_EQ(queue.size(), 5u);

    // Wait for expiration
    std::this_thread::sleep_for(milliseconds(20));

    size_t expired = queue.expire_stale();
    EXPECT_EQ(expired, 5u);
    EXPECT_TRUE(queue.empty());
}

TEST(RequestQueueTest, ExpireCallsCallbacks) {
    RequestQueue queue;

    int expired_count = 0;

    QueuedRequest req;
    req.ttl_ms = 10;  // Very short TTL - explicitly set
    req.callback = [&expired_count](int err) {
        if (err == kRequestQueueExpiredError) expired_count++;
    };
    queue.enqueue(std::move(req));

    std::this_thread::sleep_for(milliseconds(20));
    queue.expire_stale();

    EXPECT_EQ(expired_count, 1);
}

TEST(RequestQueueTest, MixedExpirationTimes) {
    RequestQueue queue;

    // Add request with short TTL
    QueuedRequest short_req;
    short_req.xid = 1;
    short_req.ttl_ms = 10;
    queue.enqueue(std::move(short_req));

    // Add request with long TTL
    QueuedRequest long_req;
    long_req.xid = 2;
    long_req.ttl_ms = 10000;
    queue.enqueue(std::move(long_req));

    EXPECT_EQ(queue.size(), 2u);

    std::this_thread::sleep_for(milliseconds(20));

    size_t expired = queue.expire_stale();
    EXPECT_EQ(expired, 1u);  // Only short TTL request expired
    EXPECT_EQ(queue.size(), 1u);

    auto result = queue.dequeue();
    ASSERT_TRUE(result.is_some());
    EXPECT_EQ(result.unwrap().xid, 2);  // Long TTL request still there
}

// ============================================================================
// Clear Tests
// ============================================================================

TEST(RequestQueueTest, ClearAll) {
    RequestQueue queue;

    for (int i = 0; i < 5; i++) {
        QueuedRequest req;
        queue.enqueue(std::move(req));
    }

    EXPECT_EQ(queue.size(), 5u);

    queue.clear_all();

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
}

TEST(RequestQueueTest, ClearAllCallsCallbacks) {
    RequestQueue queue;
    int callback_count = 0;
    int error_code_received = 0;

    for (int i = 0; i < 3; i++) {
        QueuedRequest req;
        req.callback = [&callback_count, &error_code_received](int err) {
            callback_count++;
            error_code_received = err;
        };
        queue.enqueue(std::move(req));
    }

    queue.clear_all(-99);

    EXPECT_EQ(callback_count, 3);
    EXPECT_EQ(error_code_received, -99);
}

// ============================================================================
// Disabled Queue Tests
// ============================================================================

TEST(RequestQueueTest, DisabledQueueRejectsAll) {
    auto config = RequestQueueConfig::disabled();
    RequestQueue queue(config);

    QueuedRequest req;
    req.xid = 1;

    EXPECT_FALSE(queue.enqueue(std::move(req)));
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.enabled());
}

TEST(RequestQueueTest, DisabledQueueRejectCallsCallback) {
    auto config = RequestQueueConfig::disabled();
    RequestQueue queue(config);

    int callback_count = 0;
    int callback_error = 0;
    QueuedRequest req;
    req.callback = [&callback_count, &callback_error](int err) {
        callback_count++;
        callback_error = err;
    };

    EXPECT_FALSE(queue.enqueue(std::move(req)));
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(callback_error, kRequestQueueRejectedError);
    EXPECT_TRUE(queue.empty());
}

// ============================================================================
// Configuration Presets Tests
// ============================================================================

TEST(RequestQueueTest, SmallPreset) {
    auto config = RequestQueueConfig::small();
    EXPECT_EQ(config.max_size, 10u);
    EXPECT_EQ(config.default_ttl_ms, 5000u);
}

TEST(RequestQueueTest, LargePreset) {
    auto config = RequestQueueConfig::large();
    EXPECT_EQ(config.max_size, 10000u);
    EXPECT_EQ(config.default_ttl_ms, 60000u);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST(RequestQueueTest, ConcurrentEnqueue) {
    RequestQueueConfig config;
    config.max_size = 10000;  // Large enough to not overflow
    RequestQueue queue(config);

    std::vector<std::thread> threads;
    int requests_per_thread = 100;
    int num_threads = 10;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&queue, t, requests_per_thread]() {
            for (int i = 0; i < requests_per_thread; i++) {
                QueuedRequest req;
                req.xid = t * 1000 + i;
                queue.enqueue(std::move(req));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(queue.size(), static_cast<size_t>(num_threads * requests_per_thread));
}

TEST(RequestQueueTest, ConcurrentDequeue) {
    RequestQueueConfig config;
    config.max_size = 1000;
    RequestQueue queue(config);

    // Pre-fill queue
    for (int i = 0; i < 1000; i++) {
        QueuedRequest req;
        req.xid = i;
        queue.enqueue(std::move(req));
    }

    std::atomic<int> dequeued_count{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < 10; t++) {
        threads.emplace_back([&queue, &dequeued_count]() {
            while (true) {
                auto result = queue.dequeue();
                if (result.is_none()) break;
                dequeued_count++;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(dequeued_count, 1000);
    EXPECT_TRUE(queue.empty());
}

TEST(RequestQueueTest, ConcurrentEnqueueDequeue) {
    RequestQueueConfig config;
    config.max_size = 100;
    RequestQueue queue(config);

    std::atomic<bool> stop{false};
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    // Producer threads
    std::vector<std::thread> producers;
    for (int t = 0; t < 3; t++) {
        producers.emplace_back([&queue, &stop, &enqueued]() {
            while (!stop) {
                QueuedRequest req;
                if (queue.enqueue(std::move(req))) {
                    enqueued++;
                }
            }
        });
    }

    // Consumer threads
    std::vector<std::thread> consumers;
    for (int t = 0; t < 3; t++) {
        consumers.emplace_back([&queue, &stop, &dequeued]() {
            while (!stop || !queue.empty()) {
                auto result = queue.dequeue();
                if (result.is_some()) {
                    dequeued++;
                }
            }
        });
    }

    // Let it run for a bit
    std::this_thread::sleep_for(milliseconds(100));
    stop = true;

    for (auto& t : producers) {
        t.join();
    }
    for (auto& t : consumers) {
        t.join();
    }

    // All enqueued should eventually be dequeued (or still in queue)
    EXPECT_LE(dequeued + queue.size(), static_cast<size_t>(enqueued.load()));
}

// ============================================================================
// Utility Tests
// ============================================================================

TEST(RequestQueueTest, OverflowStrategyToString) {
    EXPECT_STREQ(overflow_strategy_to_string(OverflowStrategy::DROP_OLDEST), "DROP_OLDEST");
    EXPECT_STREQ(overflow_strategy_to_string(OverflowStrategy::DROP_NEWEST), "DROP_NEWEST");
    EXPECT_STREQ(overflow_strategy_to_string(OverflowStrategy::FAIL_FAST), "FAIL_FAST");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
