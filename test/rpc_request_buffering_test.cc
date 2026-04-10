/**
 * Unit tests for RPC Request Buffering.
 * Tests buffering requests during disconnection and replay on reconnect.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <thread>
#include <vector>
#include "rpc/client.hpp"

using namespace rrr;

// ============================================================================
// BufferingConfig Tests
// ============================================================================

TEST(BufferingConfigTest, DefaultConfig) {
    auto config = BufferingConfig::defaults();
    EXPECT_EQ(config.behavior, DisconnectBehavior::QUEUE);
    EXPECT_EQ(config.max_pending, 1000u);
    EXPECT_EQ(config.default_ttl_ms, 30000u);
    EXPECT_EQ(config.overflow, OverflowStrategy::DROP_OLDEST);
    EXPECT_TRUE(config.enabled);
}

TEST(BufferingConfigTest, DisabledConfig) {
    auto config = BufferingConfig::disabled();
    EXPECT_EQ(config.behavior, DisconnectBehavior::FAIL_FAST);
    EXPECT_FALSE(config.enabled);
}

TEST(BufferingConfigTest, ToQueueConfig) {
    BufferingConfig bc;
    bc.max_pending = 500;
    bc.default_ttl_ms = 10000;
    bc.overflow = OverflowStrategy::DROP_NEWEST;
    bc.enabled = true;

    auto qc = bc.to_queue_config();
    EXPECT_EQ(qc.max_size, 500u);
    EXPECT_EQ(qc.default_ttl_ms, 10000u);
    EXPECT_EQ(qc.overflow_strategy, OverflowStrategy::DROP_NEWEST);
    EXPECT_TRUE(qc.enabled);
}

// ============================================================================
// DisconnectBehavior Tests
// ============================================================================

TEST(DisconnectBehaviorTest, EnumValues) {
    EXPECT_NE(DisconnectBehavior::QUEUE, DisconnectBehavior::FAIL_FAST);
}

// ============================================================================
// ClientConnection Buffering Tests (using real poll thread)
// ============================================================================

// Test fixture for buffering tests
class RequestBufferingTest : public ::testing::Test {
protected:
    std::shared_ptr<rusty::Arc<PollThread>> poll_thread_;

    void SetUp() override {
        poll_thread_ = std::make_shared<rusty::Arc<PollThread>>(PollThread::create());
    }

    void TearDown() override {
        if (poll_thread_ && *poll_thread_) {
            (*poll_thread_)->shutdown();
        }
        poll_thread_.reset();
    }

    // Helper to get the poll thread Arc
    rusty::Arc<PollThread> get_poll_thread() {
        return *poll_thread_;
    }
};

TEST_F(RequestBufferingTest, BufferingConfigMethods) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Check default config
    const auto& default_config = conn->buffering_config();
    EXPECT_EQ(default_config.behavior, DisconnectBehavior::QUEUE);
    EXPECT_TRUE(default_config.enabled);

    // Set new config
    BufferingConfig new_config;
    new_config.behavior = DisconnectBehavior::FAIL_FAST;
    new_config.max_pending = 100;
    conn->set_buffering_config(new_config);

    const auto& updated_config = conn->buffering_config();
    EXPECT_EQ(updated_config.behavior, DisconnectBehavior::FAIL_FAST);
    EXPECT_EQ(updated_config.max_pending, 100u);
}

TEST_F(RequestBufferingTest, PendingRequestCount) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Initially no pending requests
    EXPECT_EQ(conn->pending_request_count(), 0u);
}

TEST_F(RequestBufferingTest, RequestWhenDisconnectedQueues) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Connection starts in NEW state (not connected)
    EXPECT_FALSE(conn->connected());

    // Make a request - should be queued since buffering is enabled by default
    auto result = conn->request(1, FutureAttr(), [](Marshal& m) {
        i32 val = 42;
        m << val;
    });

    // Should succeed (request queued)
    EXPECT_TRUE(result.is_ok());

    // Should have one pending request
    EXPECT_EQ(conn->pending_request_count(), 1u);
}

TEST_F(RequestBufferingTest, RequestWhenDisconnectedFailsFast) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Disable buffering (fail fast)
    conn->set_buffering_config(BufferingConfig::disabled());

    // Connection starts in NEW state (not connected)
    EXPECT_FALSE(conn->connected());

    // Make a request - should fail immediately
    auto result = conn->request(1, FutureAttr(), [](Marshal& m) {
        i32 val = 42;
        m << val;
    });

    // Should fail with ENOTCONN
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), ENOTCONN);

    // Should have no pending requests
    EXPECT_EQ(conn->pending_request_count(), 0u);
}

TEST_F(RequestBufferingTest, MultipleRequestsQueued) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Make multiple requests
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](Marshal& m) {
            m << i;
        });
        EXPECT_TRUE(result.is_ok());
    }

    EXPECT_EQ(conn->pending_request_count(), 5u);
}

TEST_F(RequestBufferingTest, ClearPendingRequests) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Queue some requests
    for (int i = 0; i < 3; i++) {
        conn->request(i, FutureAttr(), [](Marshal&) {});
    }

    EXPECT_EQ(conn->pending_request_count(), 3u);

    // Clear all pending
    conn->clear_pending_requests();

    EXPECT_EQ(conn->pending_request_count(), 0u);
}

TEST_F(RequestBufferingTest, QueueOverflowDropsOldest) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Set small queue
    BufferingConfig config;
    config.max_pending = 3;
    config.overflow = OverflowStrategy::DROP_OLDEST;
    conn->set_buffering_config(config);

    // Queue 5 requests
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](Marshal& m) {
            m << i;
        });
        EXPECT_TRUE(result.is_ok());
    }

    // Should only have 3 (oldest dropped)
    EXPECT_EQ(conn->pending_request_count(), 3u);
}

TEST_F(RequestBufferingTest, QueueOverflowDropsNewest) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Set small queue with DROP_NEWEST
    BufferingConfig config;
    config.max_pending = 3;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    // Queue 5 requests - only first 3 should succeed with OK,
    // remaining will return EAGAIN when rejected
    int ok_count = 0;
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](Marshal& m) {
            m << i;
        });
        if (result.is_ok()) ok_count++;
    }

    // Only first 3 should succeed
    EXPECT_EQ(ok_count, 3);
    EXPECT_EQ(conn->pending_request_count(), 3u);
}

TEST_F(RequestBufferingTest, DropNewestOverflowDoesNotLeakPendingFutures) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    BufferingConfig config;
    config.max_pending = 3;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    int ok_count = 0;
    int err_count = 0;
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](Marshal& m) {
            m << i;
        });
        if (result.is_ok()) {
            ok_count++;
            auto future = result.unwrap();
            EXPECT_FALSE(future->ready());
        } else {
            err_count++;
            EXPECT_EQ(result.unwrap_err(), EAGAIN);
        }
    }

    EXPECT_EQ(ok_count, 3);
    EXPECT_EQ(err_count, 2);
    EXPECT_EQ(conn->pending_request_count(), 3u);
    EXPECT_EQ(conn->pending_future_count(), 3u);

    conn->clear_pending_requests();
    EXPECT_EQ(conn->pending_request_count(), 0u);
    EXPECT_EQ(conn->pending_future_count(), 0u);
}

// ============================================================================
// Client Buffering Tests
// ============================================================================

TEST_F(RequestBufferingTest, ClientBufferingConfig) {
    auto client = Client::create(get_poll_thread());

    // Initially no connection, so methods should be safe no-ops
    EXPECT_EQ(client->pending_request_count(), 0u);

    // Set config should be safe even without connection
    client->set_buffering_config(BufferingConfig::disabled());
    client->clear_pending_requests();
}

// ============================================================================
// Future Completion Tests
// ============================================================================

TEST_F(RequestBufferingTest, QueuedRequestReturnsFuture) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    auto result = conn->request(1, FutureAttr(), [](Marshal& m) {
        i32 val = 42;
        m << val;
    });

    ASSERT_TRUE(result.is_ok());

    auto future = result.unwrap();
    EXPECT_NE(future.get(), nullptr);

    // Future should not be ready yet (waiting for response after replay)
    EXPECT_FALSE(future->ready());
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST_F(RequestBufferingTest, ConcurrentQueueing) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Set larger queue for concurrent test
    BufferingConfig config;
    config.max_pending = 1000;
    conn->set_buffering_config(config);

    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    const int requests_per_thread = 50;
    const int num_threads = 4;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([&conn, &success_count, t, requests_per_thread]() {
            for (int i = 0; i < requests_per_thread; i++) {
                auto result = conn->request(t * 1000 + i, FutureAttr(), [t, i](Marshal& m) {
                    m << t << i;
                });
                if (result.is_ok()) {
                    success_count++;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All requests should have been queued
    EXPECT_EQ(success_count.load(), num_threads * requests_per_thread);
    EXPECT_EQ(conn->pending_request_count(), static_cast<size_t>(num_threads * requests_per_thread));
}

TEST_F(RequestBufferingTest, ConcurrentQueueAndClear) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    BufferingConfig config;
    config.max_pending = 100;
    conn->set_buffering_config(config);

    std::atomic<bool> stop{false};

    // Producer thread
    std::thread producer([&conn, &stop]() {
        while (!stop) {
            conn->request(1, FutureAttr(), [](Marshal&) {});
        }
    });

    // Clearer thread
    std::thread clearer([&conn, &stop]() {
        while (!stop) {
            conn->clear_pending_requests();
            std::this_thread::yield();
        }
    });

    // Run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop = true;

    producer.join();
    clearer.join();

    // Should not crash, final state should be consistent
    // Pending count should be >= 0
    EXPECT_GE(conn->pending_request_count(), 0u);
}

// ============================================================================
// TTL Tests (integration with RequestQueue)
// ============================================================================

TEST_F(RequestBufferingTest, QueuedRequestHasTTL) {
    auto conn = rusty::Arc<ClientConnection>::make(get_poll_thread());

    // Set short TTL
    BufferingConfig config;
    config.default_ttl_ms = 100;  // 100ms TTL
    conn->set_buffering_config(config);

    // Queue a request
    conn->request(1, FutureAttr(), [](Marshal&) {});
    EXPECT_EQ(conn->pending_request_count(), 1u);

    // Wait for TTL to expire
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Note: The request is still in the queue until replay or explicit expiration
    // TTL check happens during replay or expire_stale()
    EXPECT_EQ(conn->pending_request_count(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
