/**
 * Unit tests for Server Restart Detection
 *
 * Tests the server instance ID generation, client-side tracking,
 * and restart detection callback functionality.
 */
#include <stdint.h>

#include "gtest/gtest.h"
#include "../rrr.hpp"

import std;

namespace rrr {

class RestartDetectionTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ========== Server Instance ID Tests ==========

TEST_F(RestartDetectionTest, InstanceIdGenerated) {
    Server server;
    // Server should generate a non-zero instance ID
    EXPECT_NE(0u, server.instance_id());
}

TEST_F(RestartDetectionTest, InstanceIdUnique) {
    Server server1;
    Server server2;
    // Different servers should get different IDs
    EXPECT_NE(server1.instance_id(), server2.instance_id());
}

TEST_F(RestartDetectionTest, InstanceIdStableAcrossRequests) {
    Server server;
    uint64_t id1 = server.instance_id();
    uint64_t id2 = server.instance_id();
    uint64_t id3 = server.instance_id();
    // Same server should keep the same ID
    EXPECT_EQ(id1, id2);
    EXPECT_EQ(id2, id3);
}

TEST_F(RestartDetectionTest, InstanceIdUniqueAcrossThreads) {
    const int num_threads = 10;
    std::vector<uint64_t> ids(num_threads);
    std::vector<std::thread> threads;

    // Create servers in parallel
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&ids, i]() {
            Server server;
            ids[i] = server.instance_id();
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All IDs should be unique
    std::set<uint64_t> unique_ids(ids.begin(), ids.end());
    EXPECT_EQ(num_threads, static_cast<int>(unique_ids.size()));
}

// ========== Client-Side Tracking Tests ==========

TEST_F(RestartDetectionTest, ClientInitialIdIsZero) {
    auto poll_thread = PollThread::create();
    auto client = Client::create(poll_thread);
    // Initial server instance ID should be 0 (no connection yet)
    EXPECT_EQ(0u, client->server_instance_id());
}

TEST_F(RestartDetectionTest, ClientTracksServerId) {
    auto poll_thread = PollThread::create();
    ClientConnection conn(poll_thread);

    // Initially should be 0
    EXPECT_EQ(0u, conn.server_instance_id());

    // Check with a new ID - should not trigger callback since old was 0
    bool restart_detected = conn.check_server_instance(12345);
    EXPECT_FALSE(restart_detected);
    EXPECT_EQ(12345u, conn.server_instance_id());
}

TEST_F(RestartDetectionTest, RestartCallbackCalled) {
    auto poll_thread = PollThread::create();
    ClientConnection conn(poll_thread);

    std::atomic<bool> callback_called{false};
    uint64_t old_id_received = 0;
    uint64_t new_id_received = 0;

    conn.set_on_server_restart([&](uint64_t old_id, uint64_t new_id) {
        callback_called = true;
        old_id_received = old_id;
        new_id_received = new_id;
    });

    // Set initial ID
    conn.check_server_instance(1000);
    EXPECT_FALSE(callback_called);  // First ID doesn't trigger callback

    // Change ID - should trigger callback
    bool restart_detected = conn.check_server_instance(2000);
    EXPECT_TRUE(restart_detected);
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(1000u, old_id_received);
    EXPECT_EQ(2000u, new_id_received);
    EXPECT_EQ(2000u, conn.server_instance_id());
}

TEST_F(RestartDetectionTest, RestartCallbackNotCalledSameId) {
    auto poll_thread = PollThread::create();
    ClientConnection conn(poll_thread);

    std::atomic<int> callback_count{0};

    conn.set_on_server_restart([&](uint64_t, uint64_t) {
        callback_count++;
    });

    // Set initial ID
    conn.check_server_instance(1000);
    EXPECT_EQ(0, callback_count);

    // Same ID - should not trigger callback
    bool restart_detected = conn.check_server_instance(1000);
    EXPECT_FALSE(restart_detected);
    EXPECT_EQ(0, callback_count);

    // Same ID again
    restart_detected = conn.check_server_instance(1000);
    EXPECT_FALSE(restart_detected);
    EXPECT_EQ(0, callback_count);
}

TEST_F(RestartDetectionTest, MultipleRestarts) {
    auto poll_thread = PollThread::create();
    ClientConnection conn(poll_thread);

    std::vector<std::pair<uint64_t, uint64_t>> restarts;

    conn.set_on_server_restart([&](uint64_t old_id, uint64_t new_id) {
        restarts.push_back({old_id, new_id});
    });

    // Simulate multiple restarts
    conn.check_server_instance(100);  // Initial
    conn.check_server_instance(200);  // Restart 1
    conn.check_server_instance(300);  // Restart 2
    conn.check_server_instance(400);  // Restart 3

    ASSERT_EQ(3u, restarts.size());
    EXPECT_EQ(100u, restarts[0].first);
    EXPECT_EQ(200u, restarts[0].second);
    EXPECT_EQ(200u, restarts[1].first);
    EXPECT_EQ(300u, restarts[1].second);
    EXPECT_EQ(300u, restarts[2].first);
    EXPECT_EQ(400u, restarts[2].second);
}

TEST_F(RestartDetectionTest, NoCallbackIfNotSet) {
    auto poll_thread = PollThread::create();
    ClientConnection conn(poll_thread);

    // No callback set - should not crash
    conn.check_server_instance(1000);  // Initial
    bool restart_detected = conn.check_server_instance(2000);  // Change

    // Should detect restart without crashing
    EXPECT_TRUE(restart_detected);
    EXPECT_EQ(2000u, conn.server_instance_id());
}

// ========== Client Wrapper Tests ==========

TEST_F(RestartDetectionTest, ClientWrapperServerInstanceId) {
    auto poll_thread = PollThread::create();
    auto client = Client::create(poll_thread);

    // No connection - should return 0
    EXPECT_EQ(0u, client->server_instance_id());

    // Connect to establish connection
    // Note: We can't test full functionality without a real server,
    // but we can verify the API works when no connection exists
    bool restart_detected = client->check_server_instance(1234);
    EXPECT_FALSE(restart_detected);  // No connection, so no restart detection
}

} // namespace rrr
