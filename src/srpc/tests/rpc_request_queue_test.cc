/**
 * Unit tests for RPC RequestQueue.
 * Tests queue operations, overflow strategies, and TTL expiration.
 */

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>
#include "../srpc.hpp"

import std;

using namespace srpc;
using namespace std::chrono;

// ============================================================================
// Basic Operations Tests
// ============================================================================

TEST(RequestQueueTest, InitiallyEmpty) {
    auto queue = RequestQueue::new_();
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_FALSE(queue.full());
}

// update_config had NO coverage, which is how a conversion that wrote the
// new config to a temporary (`config_.get().set(cfg)`) shipped, compiled,
// and passed all 30 tests. Assert the swap is actually observable.
TEST(RequestQueueTest, UpdateConfigIsObservable) {
    auto queue = RequestQueue::new_();
    const size_t original_max = queue.max_size();

    auto cfg = queue.config();
    cfg.max_size = original_max + 7;
    cfg.enabled = !queue.enabled();
    queue.update_config(cfg);

    EXPECT_EQ(queue.max_size(), original_max + 7);
    EXPECT_EQ(queue.enabled(), cfg.enabled);
    EXPECT_EQ(queue.config().max_size, original_max + 7);
}

TEST(RequestQueueTest, EnqueueSingleRequest) {
    auto queue = RequestQueue::new_();

    auto req = QueuedRequest::new_();
    req.xid = 12345;
    req.rpc_id = 1;

    EXPECT_TRUE(queue.enqueue(std::move(req)));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size(), 1u);
}

TEST(RequestQueueTest, DequeueRequest) {
    auto queue = RequestQueue::new_();

    auto req = QueuedRequest::new_();
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
    auto queue = RequestQueue::new_();
    auto result = queue.dequeue();
    EXPECT_TRUE(result.is_none());
}

TEST(RequestQueueTest, FifoOrder) {
    auto queue = RequestQueue::new_();

    for (int i = 0; i < 5; i++) {
        auto req = QueuedRequest::new_();
        req.xid = i;
        queue.enqueue(std::move(req));
    }

    for (int i = 0; i < 5; i++) {
        auto result = queue.dequeue();
        ASSERT_TRUE(result.is_some());
        EXPECT_EQ(result.unwrap().xid, i);
    }
}

// the prior `Peek` and `PeekEmptyReturnsFalse`
// tests went away with the `RequestQueue::peek(QueuedRequest&)`
// method itself — once `QueuedRequest::callback` migrated to
// move-only rusty::Function, peek's `out = guard->front();` copy
// no longer compiled.  The method was tests-only (no production
// callers); equivalent post-enqueue inspection happens via
// `size()` / `empty()` and via dequeue (which moves out).

TEST(RequestQueueTest, EnqueueIncreasesSize) {
    auto queue = RequestQueue::new_();
    EXPECT_EQ(queue.size(), 0u);
    EXPECT_TRUE(queue.empty());

    auto req = QueuedRequest::new_();
    req.xid = 999;
    queue.enqueue(std::move(req));

    EXPECT_EQ(queue.size(), 1u);
    EXPECT_FALSE(queue.empty());
}

TEST(RequestQueueTest, EmptyQueueDequeueReturnsNone) {
    auto queue = RequestQueue::new_();
    auto result = queue.dequeue();
    EXPECT_TRUE(result.is_none());
}

// ============================================================================
// Size Limits Tests
// ============================================================================

TEST(RequestQueueTest, RespectMaxSize) {
    auto config = RequestQueueConfig::new_();
    config.max_size = 5;
    config.overflow_strategy = OverflowStrategy::DROP_NEWEST;
    auto queue = RequestQueue::with_config(config);

    for (int i = 0; i < 10; i++) {
        auto req = QueuedRequest::new_();
        req.xid = i;
        queue.enqueue(std::move(req));
    }

    // Only first 5 should be in queue
    EXPECT_EQ(queue.size(), 5u);
    EXPECT_TRUE(queue.full());
}

TEST(RequestQueueTest, FullCheck) {
    auto config = RequestQueueConfig::new_();
    config.max_size = 3;
    auto queue = RequestQueue::with_config(config);

    EXPECT_FALSE(queue.full());

    for (int i = 0; i < 3; i++) {
        auto req = QueuedRequest::new_();
        queue.enqueue(std::move(req));
    }

    EXPECT_TRUE(queue.full());
}

TEST(RequestQueueTest, RemainingCapacity) {
    auto config = RequestQueueConfig::new_();
    config.max_size = 10;
    auto queue = RequestQueue::with_config(config);

    EXPECT_EQ(queue.remaining_capacity(), 10u);

    for (int i = 0; i < 3; i++) {
        auto req = QueuedRequest::new_();
        queue.enqueue(std::move(req));
    }

    EXPECT_EQ(queue.remaining_capacity(), 7u);
}

// ============================================================================
// Overflow Strategy Tests
// ============================================================================

TEST(RequestQueueTest, OverflowDropOldest) {
    auto config = RequestQueueConfig::new_();
    config.max_size = 3;
    config.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    auto queue = RequestQueue::with_config(config);

    for (int i = 0; i < 5; i++) {
        auto req = QueuedRequest::new_();
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
    auto config = RequestQueueConfig::new_();
    config.max_size = 3;
    config.overflow_strategy = OverflowStrategy::DROP_NEWEST;
    auto queue = RequestQueue::with_config(config);

    for (int i = 0; i < 5; i++) {
        auto req = QueuedRequest::new_();
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
    auto config = RequestQueueConfig::new_();
    config.max_size = 2;
    config.overflow_strategy = OverflowStrategy::DROP_NEWEST;
    auto queue = RequestQueue::with_config(config);

    // Fill queue
    for (int i = 0; i < 2; i++) {
        auto req = QueuedRequest::new_();
        queue.enqueue(std::move(req));
    }

    int callback_count = 0;
    int callback_error = 0;
    auto req = QueuedRequest::new_();
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
    auto config = RequestQueueConfig::new_();
    config.max_size = 2;
    config.overflow_strategy = OverflowStrategy::FAIL_FAST;
    auto queue = RequestQueue::with_config(config);

    // Fill queue
    for (int i = 0; i < 2; i++) {
        auto req = QueuedRequest::new_();
        queue.enqueue(std::move(req));
    }

    // Third request should fail and call callback
    int callback_error = 0;
    auto req = QueuedRequest::new_();
    req.callback = [&callback_error](int err) { callback_error = err; };

    EXPECT_FALSE(queue.enqueue(std::move(req)));
    EXPECT_EQ(callback_error, kRequestQueueRejectedError);
}

TEST(RequestQueueTest, DropOldestCallsCallback) {
    auto config = RequestQueueConfig::new_();
    config.max_size = 2;
    config.overflow_strategy = OverflowStrategy::DROP_OLDEST;
    auto queue = RequestQueue::with_config(config);

    int dropped_count = 0;

    // Fill queue with callbacks
    for (int i = 0; i < 2; i++) {
        auto req = QueuedRequest::new_();
        req.xid = i;
        req.callback = [&dropped_count](int err) {
            if (err == kRequestQueueRejectedError) dropped_count++;
        };
        queue.enqueue(std::move(req));
    }

    // Third request should drop oldest
    auto req = QueuedRequest::new_();
    req.xid = 2;
    queue.enqueue(std::move(req));

    EXPECT_EQ(dropped_count, 1);  // First request should have been dropped
}

// ============================================================================
// TTL and Expiration Tests
// ============================================================================

TEST(RequestQueueTest, RequestIsExpiredCheck) {
    auto req = QueuedRequest::new_();
    req.ttl_ms = 10;  // 10ms TTL

    EXPECT_FALSE(req.is_expired());

    std::this_thread::sleep_for(milliseconds(20));

    EXPECT_TRUE(req.is_expired());
}

TEST(RequestQueueTest, RequestAgeMs) {
    auto req = QueuedRequest::new_();

    std::this_thread::sleep_for(milliseconds(50));

    uint32_t age = req.age_ms();
    EXPECT_GE(age, 50u);
    EXPECT_LT(age, 100u);  // Shouldn't be much more than 50ms
}

TEST(RequestQueueTest, ExpireStaleRequests) {
    auto queue = RequestQueue::new_();

    for (int i = 0; i < 5; i++) {
        auto req = QueuedRequest::new_();
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
    auto queue = RequestQueue::new_();

    int expired_count = 0;

    auto req = QueuedRequest::new_();
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
    auto queue = RequestQueue::new_();

    // Add request with short TTL
    auto short_req = QueuedRequest::new_();
    short_req.xid = 1;
    short_req.ttl_ms = 10;
    queue.enqueue(std::move(short_req));

    // Add request with long TTL
    auto long_req = QueuedRequest::new_();
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
    auto queue = RequestQueue::new_();

    for (int i = 0; i < 5; i++) {
        auto req = QueuedRequest::new_();
        queue.enqueue(std::move(req));
    }

    EXPECT_EQ(queue.size(), 5u);

    queue.clear_all(-3);  // was clear_all() with a default arg; DSL drops defaults

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0u);
}

TEST(RequestQueueTest, ClearAllCallsCallbacks) {
    auto queue = RequestQueue::new_();
    int callback_count = 0;
    int error_code_received = 0;

    for (int i = 0; i < 3; i++) {
        auto req = QueuedRequest::new_();
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
    auto queue = RequestQueue::with_config(config);

    auto req = QueuedRequest::new_();
    req.xid = 1;

    EXPECT_FALSE(queue.enqueue(std::move(req)));
    EXPECT_TRUE(queue.empty());
    EXPECT_FALSE(queue.enabled());
}

TEST(RequestQueueTest, DisabledQueueRejectCallsCallback) {
    auto config = RequestQueueConfig::disabled();
    auto queue = RequestQueue::with_config(config);

    int callback_count = 0;
    int callback_error = 0;
    auto req = QueuedRequest::new_();
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
    auto config = RequestQueueConfig::new_();
    config.max_size = 10000;  // Large enough to not overflow
    auto queue = RequestQueue::with_config(config);

    std::vector<std::thread> threads;
    int requests_per_thread = 100;
    int num_threads = 10;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&queue, t, requests_per_thread]() {
            for (int i = 0; i < requests_per_thread; i++) {
                auto req = QueuedRequest::new_();
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
    auto config = RequestQueueConfig::new_();
    config.max_size = 1000;
    auto queue = RequestQueue::with_config(config);

    // Pre-fill queue
    for (int i = 0; i < 1000; i++) {
        auto req = QueuedRequest::new_();
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
    auto config = RequestQueueConfig::new_();
    config.max_size = 100;
    auto queue = RequestQueue::with_config(config);

    std::atomic<bool> stop{false};
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};

    // Producer threads
    std::vector<std::thread> producers;
    for (int t = 0; t < 3; t++) {
        producers.emplace_back([&queue, &stop, &enqueued]() {
            while (!stop) {
                auto req = QueuedRequest::new_();
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
    EXPECT_EQ(overflow_strategy_to_string(OverflowStrategy::DROP_OLDEST), "DROP_OLDEST");
    EXPECT_EQ(overflow_strategy_to_string(OverflowStrategy::DROP_NEWEST), "DROP_NEWEST");
    EXPECT_EQ(overflow_strategy_to_string(OverflowStrategy::FAIL_FAST), "FAIL_FAST");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
