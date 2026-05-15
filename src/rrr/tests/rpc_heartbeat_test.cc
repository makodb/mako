/**
 * Unit tests for HeartbeatManager
 * Tests configuration, timing, timeout detection, and callbacks.
 */

#include <stdint.h>

#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;

using namespace rrr;
using namespace std::chrono;

// ============================================================================
// Configuration Tests
// ============================================================================

TEST(HeartbeatConfigTest, DefaultValues) {
    HeartbeatConfig config;
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.interval_ms, 10000u);
    EXPECT_EQ(config.timeout_ms, 5000u);
    EXPECT_EQ(config.max_missed, 3u);
}

TEST(HeartbeatConfigTest, AggressivePreset) {
    auto config = HeartbeatConfig::aggressive();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.interval_ms, 5000u);
    EXPECT_EQ(config.timeout_ms, 2000u);
    EXPECT_EQ(config.max_missed, 2u);
}

TEST(HeartbeatConfigTest, RelaxedPreset) {
    auto config = HeartbeatConfig::relaxed();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.interval_ms, 30000u);
    EXPECT_EQ(config.timeout_ms, 15000u);
    EXPECT_EQ(config.max_missed, 5u);
}

TEST(HeartbeatConfigTest, DisabledPreset) {
    auto config = HeartbeatConfig::disabled();
    EXPECT_FALSE(config.enabled);
}

// ============================================================================
// Initial State Tests
// ============================================================================

TEST(HeartbeatManagerTest, InitialState) {
    HeartbeatManager hb;
    EXPECT_FALSE(hb.is_pending_pong());
    EXPECT_FALSE(hb.is_timed_out());
    EXPECT_EQ(hb.missed_count(), 0u);
}

TEST(HeartbeatManagerTest, DisabledDoesNotSend) {
    HeartbeatManager hb(HeartbeatConfig::disabled());

    // Should never send heartbeat when disabled
    EXPECT_FALSE(hb.should_send_heartbeat());
}

// ============================================================================
// Heartbeat Send Tests
// ============================================================================

TEST(HeartbeatManagerTest, ShouldSendAfterInterval) {
    HeartbeatConfig config;
    config.interval_ms = 10;  // Very short for testing
    HeartbeatManager hb(config);

    // Initially should send (no previous heartbeat)
    EXPECT_TRUE(hb.should_send_heartbeat());

    // After sending, wait for pong
    hb.on_heartbeat_sent();
    EXPECT_FALSE(hb.should_send_heartbeat());  // Pending pong
    EXPECT_TRUE(hb.is_pending_pong());

    // Receive pong
    hb.on_pong_received();
    EXPECT_FALSE(hb.is_pending_pong());

    // Should not send immediately after pong
    EXPECT_FALSE(hb.should_send_heartbeat());

    // Wait for interval
    std::this_thread::sleep_for(milliseconds(20));
    EXPECT_TRUE(hb.should_send_heartbeat());
}

TEST(HeartbeatManagerTest, PongReceiveResetsMissedCount) {
    HeartbeatConfig config;
    config.interval_ms = 10;
    config.timeout_ms = 5;
    config.max_missed = 5;
    HeartbeatManager hb(config);

    // Send heartbeat
    hb.on_heartbeat_sent();

    // Wait for timeout (but not max_missed)
    std::this_thread::sleep_for(milliseconds(10));
    hb.check_timeout();  // Should increment missed_count

    EXPECT_EQ(hb.missed_count(), 1u);

    // Receive late pong
    hb.on_pong_received();
    EXPECT_EQ(hb.missed_count(), 0u);
    EXPECT_FALSE(hb.is_pending_pong());
}

// ============================================================================
// Timeout Tests
// ============================================================================

TEST(HeartbeatManagerTest, TimeoutAfterMaxMissed) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 5;  // Very short timeout for testing
    config.max_missed = 2;
    HeartbeatManager hb(config);

    std::atomic<int> timeout_count{0};
    hb.set_on_timeout([&]() {
        timeout_count++;
    });

    // First heartbeat
    hb.on_heartbeat_sent();
    std::this_thread::sleep_for(milliseconds(10));
    EXPECT_FALSE(hb.check_timeout());  // First miss
    EXPECT_EQ(hb.missed_count(), 1u);
    EXPECT_FALSE(hb.is_timed_out());

    // Second heartbeat
    hb.on_heartbeat_sent();
    std::this_thread::sleep_for(milliseconds(10));
    EXPECT_TRUE(hb.check_timeout());  // Second miss triggers timeout
    EXPECT_EQ(hb.missed_count(), 2u);
    EXPECT_TRUE(hb.is_timed_out());
    EXPECT_EQ(timeout_count.load(), 1);
}

TEST(HeartbeatManagerTest, NoTimeoutIfPongReceived) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 50;
    config.max_missed = 2;
    HeartbeatManager hb(config);

    std::atomic<int> timeout_count{0};
    hb.set_on_timeout([&]() {
        timeout_count++;
    });

    // Send heartbeat
    hb.on_heartbeat_sent();

    // Receive pong before timeout
    std::this_thread::sleep_for(milliseconds(10));
    hb.on_pong_received();

    // Check timeout - should return false
    EXPECT_FALSE(hb.check_timeout());
    EXPECT_FALSE(hb.is_timed_out());
    EXPECT_EQ(timeout_count.load(), 0);
}

TEST(HeartbeatManagerTest, TimeoutCallbackOnlyOnce) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 5;
    config.max_missed = 1;
    HeartbeatManager hb(config);

    std::atomic<int> timeout_count{0};
    hb.set_on_timeout([&]() {
        timeout_count++;
    });

    hb.on_heartbeat_sent();
    std::this_thread::sleep_for(milliseconds(10));

    // First check triggers timeout
    hb.check_timeout();
    EXPECT_EQ(timeout_count.load(), 1);

    // Subsequent checks should not trigger again
    hb.check_timeout();
    hb.check_timeout();
    EXPECT_EQ(timeout_count.load(), 1);
}

// ============================================================================
// Disabled Heartbeat Tests
// ============================================================================

TEST(HeartbeatManagerTest, DisabledNeverTimesOut) {
    HeartbeatManager hb(HeartbeatConfig::disabled());

    std::atomic<int> timeout_count{0};
    hb.set_on_timeout([&]() {
        timeout_count++;
    });

    // Operations should be no-ops
    hb.on_heartbeat_sent();
    hb.check_timeout();

    EXPECT_FALSE(hb.is_timed_out());
    EXPECT_EQ(timeout_count.load(), 0);
}

// ============================================================================
// Reset Tests
// ============================================================================

TEST(HeartbeatManagerTest, ResetClearsState) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 5;
    config.max_missed = 1;
    HeartbeatManager hb(config);

    // Trigger timeout
    hb.on_heartbeat_sent();
    std::this_thread::sleep_for(milliseconds(10));
    hb.check_timeout();
    EXPECT_TRUE(hb.is_timed_out());
    EXPECT_EQ(hb.missed_count(), 1u);

    // Reset
    hb.reset();
    EXPECT_FALSE(hb.is_timed_out());
    EXPECT_FALSE(hb.is_pending_pong());
    EXPECT_EQ(hb.missed_count(), 0u);
}

TEST(HeartbeatManagerTest, PongResetsTimeout) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 5;
    config.max_missed = 1;
    HeartbeatManager hb(config);

    // Trigger timeout
    hb.on_heartbeat_sent();
    std::this_thread::sleep_for(milliseconds(10));
    hb.check_timeout();
    EXPECT_TRUE(hb.is_timed_out());

    // Receive pong (late but indicates connection is alive)
    hb.on_pong_received();
    EXPECT_FALSE(hb.is_timed_out());  // Reset by pong
    EXPECT_EQ(hb.missed_count(), 0u);
}

// ============================================================================
// Time Until Next Heartbeat Tests
// ============================================================================

TEST(HeartbeatManagerTest, TimeUntilNextHeartbeat) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    HeartbeatManager hb(config);

    // Initially 0 (never sent)
    EXPECT_EQ(hb.time_until_next_heartbeat_ms(), 0u);

    hb.on_heartbeat_sent();
    hb.on_pong_received();

    // After sending, should be close to interval
    uint32_t time_until = hb.time_until_next_heartbeat_ms();
    EXPECT_GT(time_until, 0u);
    EXPECT_LE(time_until, 100u);
}

TEST(HeartbeatManagerTest, TimeUntilNextWithPendingPong) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    HeartbeatManager hb(config);

    hb.on_heartbeat_sent();

    // With pending pong, returns interval (waiting for pong)
    EXPECT_EQ(hb.time_until_next_heartbeat_ms(), 100u);
}

// ============================================================================
// Config Access Tests
// ============================================================================

TEST(HeartbeatManagerTest, ConfigAccess) {
    HeartbeatConfig config;
    config.interval_ms = 1234;
    config.timeout_ms = 5678;
    HeartbeatManager hb(config);

    EXPECT_EQ(hb.config().interval_ms, 1234u);
    EXPECT_EQ(hb.config().timeout_ms, 5678u);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(HeartbeatManagerTest, ImmediateTimeout) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 0;  // Immediate timeout
    config.max_missed = 1;
    HeartbeatManager hb(config);

    std::atomic<int> timeout_count{0};
    hb.set_on_timeout([&]() {
        timeout_count++;
    });

    hb.on_heartbeat_sent();

    // Should timeout immediately (or after very short time)
    std::this_thread::sleep_for(milliseconds(1));
    hb.check_timeout();

    EXPECT_TRUE(hb.is_timed_out());
    EXPECT_EQ(timeout_count.load(), 1);
}

TEST(HeartbeatManagerTest, MaxMissedOne) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 5;
    config.max_missed = 1;
    HeartbeatManager hb(config);

    hb.on_heartbeat_sent();
    std::this_thread::sleep_for(milliseconds(10));

    // Single miss triggers timeout
    EXPECT_TRUE(hb.check_timeout());
    EXPECT_TRUE(hb.is_timed_out());
}

TEST(HeartbeatManagerTest, VeryHighMaxMissed) {
    HeartbeatConfig config;
    config.interval_ms = 100;
    config.timeout_ms = 1;
    config.max_missed = 1000;
    HeartbeatManager hb(config);

    std::atomic<int> timeout_count{0};
    hb.set_on_timeout([&]() {
        timeout_count++;
    });

    // Miss 10 times - should not timeout
    for (int i = 0; i < 10; i++) {
        hb.on_heartbeat_sent();
        std::this_thread::sleep_for(milliseconds(5));
        hb.check_timeout();
    }

    EXPECT_FALSE(hb.is_timed_out());
    EXPECT_EQ(hb.missed_count(), 10u);
    EXPECT_EQ(timeout_count.load(), 0);
}
