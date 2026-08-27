#include <stddef.h>

#include <rusty/option.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
/**
 * Unit tests for RPC Request Buffering.
 * Tests buffering requests during disconnection and replay on reconnect.
 */

#include <gtest/gtest.h>
#define RPC_TEST_HOOKS
#include "../srpc.hpp"

import std;
#undef RPC_TEST_HOOKS

using namespace srpc;

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
    auto bc = BufferingConfig::defaults();
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
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Check default config
    const auto& default_config = conn->buffering_config();
    EXPECT_EQ(default_config.behavior, DisconnectBehavior::QUEUE);
    EXPECT_TRUE(default_config.enabled);

    // Set new config
    auto new_config = BufferingConfig::defaults();
    new_config.behavior = DisconnectBehavior::FAIL_FAST;
    new_config.max_pending = 100;
    conn->set_buffering_config(new_config);

    const auto& updated_config = conn->buffering_config();
    EXPECT_EQ(updated_config.behavior, DisconnectBehavior::FAIL_FAST);
    EXPECT_EQ(updated_config.max_pending, 100u);
}

TEST_F(RequestBufferingTest, PendingRequestCount) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Initially no pending requests
    EXPECT_EQ(conn->pending_request_count(), 0u);
}

TEST_F(RequestBufferingTest, DISABLED_RequestWhenDisconnectedQueues) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Connection starts in NEW state (not connected)
    EXPECT_FALSE(conn->connected());

    // Make a request - should be queued since buffering is enabled by default
    auto result = conn->request(1, FutureAttr(), [](BinaryWriteArchive& m) {
        i32 val = 42;
        srpc::Serialize_::serialize(val, m);
    });

    // Should succeed (request queued)
    EXPECT_TRUE(result.is_ok());

    // Should have one pending request
    EXPECT_EQ(conn->pending_request_count(), 1u);
}

TEST_F(RequestBufferingTest, RequestWhenDisconnectedFailsFast) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Disable buffering (fail fast)
    conn->set_buffering_config(BufferingConfig::disabled());

    // Connection starts in NEW state (not connected)
    EXPECT_FALSE(conn->connected());

    // Make a request - should fail immediately
    auto result = conn->request(1, FutureAttr(), [](BinaryWriteArchive& m) {
        i32 val = 42;
        srpc::Serialize_::serialize(val, m);
    });

    // Should fail with ENOTCONN
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.unwrap_err(), ENOTCONN);

    // Should have no pending requests
    EXPECT_EQ(conn->pending_request_count(), 0u);
}

TEST_F(RequestBufferingTest, DISABLED_MultipleRequestsQueued) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Make multiple requests
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(i, m);
        });
        EXPECT_TRUE(result.is_ok());
    }

    EXPECT_EQ(conn->pending_request_count(), 5u);
}

TEST_F(RequestBufferingTest, DISABLED_ClearPendingRequests) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Queue some requests
    for (int i = 0; i < 3; i++) {
        conn->request(i, FutureAttr(), [](BinaryWriteArchive&) {});
    }

    EXPECT_EQ(conn->pending_request_count(), 3u);

    // Clear all pending
    conn->clear_pending_requests(ECONNABORTED);

    EXPECT_EQ(conn->pending_request_count(), 0u);
}

TEST_F(RequestBufferingTest, DISABLED_QueueOverflowDropsOldest) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Set small queue
    BufferingConfig config;
    config.max_pending = 3;
    config.overflow = OverflowStrategy::DROP_OLDEST;
    conn->set_buffering_config(config);

    // Queue 5 requests
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(i, m);
        });
        EXPECT_TRUE(result.is_ok());
    }

    // Should only have 3 (oldest dropped)
    EXPECT_EQ(conn->pending_request_count(), 3u);
}

TEST_F(RequestBufferingTest, DISABLED_QueueOverflowDropsNewest) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Set small queue with DROP_NEWEST
    BufferingConfig config;
    config.max_pending = 3;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    // Queue 5 requests - only first 3 should succeed with OK,
    // remaining will return EAGAIN when rejected
    int ok_count = 0;
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(i, m);
        });
        if (result.is_ok()) ok_count++;
    }

    // Only first 3 should succeed
    EXPECT_EQ(ok_count, 3);
    EXPECT_EQ(conn->pending_request_count(), 3u);
}

TEST_F(RequestBufferingTest, DISABLED_DropNewestOverflowDoesNotLeakPendingFutures) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    BufferingConfig config;
    config.max_pending = 3;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    int ok_count = 0;
    int err_count = 0;
    for (int i = 0; i < 5; i++) {
        auto result = conn->request(i, FutureAttr(), [i](BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(i, m);
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

    conn->clear_pending_requests(ECONNABORTED);
    EXPECT_EQ(conn->pending_request_count(), 0u);
    EXPECT_EQ(conn->pending_future_count(), 0u);
}

TEST_F(RequestBufferingTest, DISABLED_ReplayReenqueueRejectDoesNotLeaveFuturePending) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    BufferingConfig config;
    config.max_pending = 8;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    auto result = conn->request(1, FutureAttr(), [](BinaryWriteArchive& m) {
        i32 val = 42;
        srpc::Serialize_::serialize(val, m);
    });
    ASSERT_TRUE(result.is_ok());
    auto future = result.unwrap();

    ASSERT_EQ(conn->pending_request_count(), 1u);
    ASSERT_EQ(conn->pending_future_count(), 1u);
    ASSERT_FALSE(future->ready());

    // Force replay re-enqueue rejection path deterministically:
    // - stay disconnected (NEW state)
    // - disable queue policy without clearing queued entry
    auto disabled_qc = config.to_queue_config();
    disabled_qc.enabled = false;
    conn->update_pending_queue_config_for_test(disabled_qc);

    EXPECT_EQ(conn->replay_pending_requests_for_test(), 0u);

    future->timed_wait(0.2);
    EXPECT_TRUE(future->ready());
    if (future->ready()) {
        EXPECT_EQ(future->get_error_code(), kRequestQueueRejectedError);
    }
    EXPECT_EQ(conn->pending_request_count(), 0u);
    EXPECT_EQ(conn->pending_future_count(), 0u);
}

TEST_F(RequestBufferingTest, DISABLED_ReplayExpiredRequestUsesTimeoutErrorCode) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    BufferingConfig config;
    config.max_pending = 8;
    config.default_ttl_ms = 10;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    auto result = conn->request(1, FutureAttr(), [](BinaryWriteArchive& m) {
        i32 val = 7;
        srpc::Serialize_::serialize(val, m);
    });
    ASSERT_TRUE(result.is_ok());
    auto future = result.unwrap();

    ASSERT_EQ(conn->pending_request_count(), 1u);
    ASSERT_EQ(conn->pending_future_count(), 1u);
    ASSERT_FALSE(future->ready());

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(conn->replay_pending_requests_for_test(), 0u);

    future->timed_wait(0.2);
    EXPECT_TRUE(future->ready());
    if (future->ready()) {
        EXPECT_EQ(future->get_error_code(), kRequestQueueExpiredError);
    }
    EXPECT_EQ(conn->pending_request_count(), 0u);
    EXPECT_EQ(conn->pending_future_count(), 0u);
}

TEST_F(RequestBufferingTest, DISABLED_OverflowAndExpiryDoNotLeavePendingFutures) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    BufferingConfig config;
    config.max_pending = 8;
    config.default_ttl_ms = 20;
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    std::vector<rusty::Arc<Future>> accepted;
    accepted.reserve(128);

    int rejected = 0;
    int unexpected_err = 0;
    for (int i = 0; i < 128; i++) {
        auto result = conn->request(i, FutureAttr(), [i](BinaryWriteArchive& m) {
            srpc::Serialize_::serialize(i, m);
        });
        if (result.is_ok()) {
            accepted.push_back(result.unwrap());
        } else {
            int err = result.unwrap_err();
            if (err == kRequestQueueRejectedError) {
                rejected++;
            } else {
                unexpected_err++;
            }
        }
    }

    ASSERT_EQ(unexpected_err, 0);
    ASSERT_EQ(rejected + static_cast<int>(accepted.size()), 128);
    ASSERT_EQ(conn->pending_request_count(), accepted.size());
    ASSERT_EQ(conn->pending_future_count(), accepted.size());

    // Let all queued requests expire, then process expiry via replay path.
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(conn->replay_pending_requests_for_test(), 0u);

    size_t not_ready = 0;
    for (const auto& fu : accepted) {
        if (!fu->ready()) {
            fu->timed_wait(0.2);
        }
        if (!fu->ready()) {
            not_ready++;
            continue;
        }
        EXPECT_EQ(fu->get_error_code(), kRequestQueueExpiredError);
    }

    // Give callbacks a short window to settle any final map removals.
    for (int i = 0; i < 50 && conn->pending_future_count() != 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(not_ready, 0u);
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
    client->clear_pending_requests(ECONNABORTED);
}

// ============================================================================
// Future Completion Tests
// ============================================================================

TEST_F(RequestBufferingTest, DISABLED_QueuedRequestReturnsFuture) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    auto result = conn->request(1, FutureAttr(), [](BinaryWriteArchive& m) {
        i32 val = 42;
        srpc::Serialize_::serialize(val, m);
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

TEST_F(RequestBufferingTest, DISABLED_ConcurrentQueueing) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

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
                auto result = conn->request(t * 1000 + i, FutureAttr(), [t, i](BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(t, m);
                    srpc::Serialize_::serialize(i, m);
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

TEST_F(RequestBufferingTest, DISABLED_ConcurrentQueueAndClearHasNoStuckFutures) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    BufferingConfig config;
    config.max_pending = 16;  // Keep queue small to force contention/overflow
    config.overflow = OverflowStrategy::DROP_NEWEST;
    conn->set_buffering_config(config);

    constexpr int kProducerThreads = 4;
    constexpr int kRequestsPerProducer = 250;

    std::atomic<bool> stop_clearer{false};
    std::atomic<int> ok_count{0};
    std::atomic<int> expected_err_count{0};
    std::atomic<int> unexpected_err_count{0};

    std::mutex futures_mu;
    std::vector<rusty::Arc<Future>> futures;
    futures.reserve(kProducerThreads * kRequestsPerProducer);

    // Clearer thread continuously drains pending queue while producers run.
    std::thread clearer([&conn, &stop_clearer]() {
        while (!stop_clearer.load()) {
            conn->clear_pending_requests(ECONNABORTED);
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(kProducerThreads);
    for (int t = 0; t < kProducerThreads; t++) {
        producers.emplace_back([&conn, &ok_count, &expected_err_count, &unexpected_err_count,
                                &futures_mu, &futures, t]() {
            for (int i = 0; i < kRequestsPerProducer; i++) {
                auto result = conn->request(t * 10000 + i, FutureAttr(), [](BinaryWriteArchive&) {});
                if (result.is_ok()) {
                    ok_count++;
                    std::lock_guard<std::mutex> lock(futures_mu);
                    futures.push_back(result.unwrap());
                } else {
                    int err = result.unwrap_err();
                    if (err == kRequestQueueRejectedError) {
                        expected_err_count++;
                    } else {
                        unexpected_err_count++;
                    }
                }
            }
        });
    }

    for (auto& p : producers) {
        p.join();
    }
    stop_clearer = true;
    clearer.join();

    // Final sweep to ensure queue is drained and callbacks fired.
    conn->clear_pending_requests(ECONNABORTED);

    // Wait briefly for pending map to settle to zero under concurrent callbacks.
    for (int i = 0; i < 50 && conn->pending_future_count() != 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::vector<rusty::Arc<Future>> captured;
    {
        std::lock_guard<std::mutex> lock(futures_mu);
        captured = futures;
    }

    size_t stuck_count = 0;
    for (const auto& fu : captured) {
        if (!fu->ready()) {
            fu->timed_wait(0.2);
        }
        if (!fu->ready()) {
            stuck_count++;
        }
    }

    EXPECT_GT(ok_count.load(), 0);
    EXPECT_EQ(unexpected_err_count.load(), 0);
    EXPECT_EQ(stuck_count, 0u);
    EXPECT_EQ(conn->pending_request_count(), 0u);
    EXPECT_EQ(conn->pending_future_count(), 0u);
    EXPECT_EQ(static_cast<size_t>(ok_count.load() + expected_err_count.load()),
              static_cast<size_t>(kProducerThreads * kRequestsPerProducer));
}

// ============================================================================
// TTL Tests (integration with RequestQueue)
// ============================================================================

TEST_F(RequestBufferingTest, DISABLED_QueuedRequestHasTTL) {
    auto conn = rusty::Arc<ClientConnection>::new_(ClientConnection::new_(get_poll_thread()));

    // Set short TTL
    BufferingConfig config;
    config.default_ttl_ms = 100;  // 100ms TTL
    conn->set_buffering_config(config);

    // Queue a request
    conn->request(1, FutureAttr(), [](BinaryWriteArchive&) {});
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
