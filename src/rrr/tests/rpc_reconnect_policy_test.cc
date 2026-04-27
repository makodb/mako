/**
 * Unit tests for ReconnectPolicy and ReconnectCalculator
 * Tests exponential backoff, jitter, max delay/retries, and presets.
 */

#include <gtest/gtest.h>
#include <cmath>
#include <set>
#include "../rrr.hpp"

using namespace rrr;

// ============================================================================
// ReconnectPolicy Configuration Tests
// ============================================================================

TEST(ReconnectPolicyTest, DefaultValues) {
    ReconnectPolicy policy;
    EXPECT_TRUE(policy.auto_reconnect);
    EXPECT_EQ(policy.max_retries, 5u);
    EXPECT_EQ(policy.initial_delay_ms, 1000u);  // 1 second
    EXPECT_EQ(policy.max_delay_ms, 30000u);     // 30 seconds
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 2.0);
    EXPECT_TRUE(policy.jitter_enabled);
}

TEST(ReconnectPolicyTest, AggressivePreset) {
    auto policy = ReconnectPolicy::aggressive();
    EXPECT_TRUE(policy.auto_reconnect);
    EXPECT_EQ(policy.max_retries, 0u);          // 0 = unlimited
    EXPECT_EQ(policy.initial_delay_ms, 100u);   // 100ms
    EXPECT_EQ(policy.max_delay_ms, 5000u);      // 5 seconds
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 1.5);
    EXPECT_TRUE(policy.jitter_enabled);
}

TEST(ReconnectPolicyTest, ConservativePreset) {
    auto policy = ReconnectPolicy::conservative();
    EXPECT_TRUE(policy.auto_reconnect);
    EXPECT_EQ(policy.max_retries, 5u);
    EXPECT_EQ(policy.initial_delay_ms, 1000u);  // 1 second
    EXPECT_EQ(policy.max_delay_ms, 30000u);     // 30 seconds
    EXPECT_DOUBLE_EQ(policy.backoff_multiplier, 2.0);
    EXPECT_TRUE(policy.jitter_enabled);
}

TEST(ReconnectPolicyTest, NoRetryPreset) {
    auto policy = ReconnectPolicy::no_retry();
    EXPECT_FALSE(policy.auto_reconnect);
    EXPECT_EQ(policy.max_retries, 0u);
}

// ============================================================================
// ReconnectCalculator Basic Tests
// ============================================================================

TEST(ReconnectCalculatorTest, InitialState) {
    ReconnectPolicy policy;
    ReconnectCalculator calc(policy);

    EXPECT_EQ(calc.retry_count(), 0u);
    EXPECT_TRUE(calc.should_retry());
}

TEST(ReconnectCalculatorTest, ShouldRetryCountsDown) {
    ReconnectPolicy policy;
    policy.max_retries = 3;
    ReconnectCalculator calc(policy);

    EXPECT_TRUE(calc.should_retry());
    calc.next_delay_ms();  // Retry 1

    EXPECT_TRUE(calc.should_retry());
    calc.next_delay_ms();  // Retry 2

    EXPECT_TRUE(calc.should_retry());
    calc.next_delay_ms();  // Retry 3

    EXPECT_FALSE(calc.should_retry());  // Max reached
}

TEST(ReconnectCalculatorTest, RetryCountIncrementsOnNextDelay) {
    ReconnectPolicy policy;
    ReconnectCalculator calc(policy);

    EXPECT_EQ(calc.retry_count(), 0u);

    calc.next_delay_ms();
    EXPECT_EQ(calc.retry_count(), 1u);

    calc.next_delay_ms();
    EXPECT_EQ(calc.retry_count(), 2u);
}

TEST(ReconnectCalculatorTest, ResetClearsRetryCount) {
    ReconnectPolicy policy;
    ReconnectCalculator calc(policy);

    calc.next_delay_ms();
    calc.next_delay_ms();
    EXPECT_EQ(calc.retry_count(), 2u);

    calc.reset();
    EXPECT_EQ(calc.retry_count(), 0u);
    EXPECT_TRUE(calc.should_retry());
}

// ============================================================================
// Exponential Backoff Tests (without jitter)
// ============================================================================

TEST(ReconnectCalculatorTest, ExponentialBackoffNoJitter) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 100;
    policy.backoff_multiplier = 2.0;
    policy.max_delay_ms = 10000;
    policy.jitter_enabled = false;
    policy.max_retries = 10;

    ReconnectCalculator calc(policy);

    // First retry: initial_delay
    EXPECT_EQ(calc.next_delay_ms(), 100u);

    // Second retry: 100 * 2 = 200
    EXPECT_EQ(calc.next_delay_ms(), 200u);

    // Third retry: 200 * 2 = 400
    EXPECT_EQ(calc.next_delay_ms(), 400u);

    // Fourth retry: 400 * 2 = 800
    EXPECT_EQ(calc.next_delay_ms(), 800u);

    // Fifth retry: 800 * 2 = 1600
    EXPECT_EQ(calc.next_delay_ms(), 1600u);
}

TEST(ReconnectCalculatorTest, MaxDelayEnforced) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 1000;
    policy.backoff_multiplier = 10.0;
    policy.max_delay_ms = 5000;
    policy.jitter_enabled = false;
    policy.max_retries = 10;

    ReconnectCalculator calc(policy);

    // First: 1000
    EXPECT_EQ(calc.next_delay_ms(), 1000u);

    // Second: would be 10000, but max is 5000
    EXPECT_EQ(calc.next_delay_ms(), 5000u);

    // Third: still max
    EXPECT_EQ(calc.next_delay_ms(), 5000u);
}

// ============================================================================
// Jitter Tests
// ============================================================================

TEST(ReconnectCalculatorTest, JitterAddRandomness) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 1000;
    policy.backoff_multiplier = 1.0;  // No backoff, just jitter
    policy.max_delay_ms = 10000;
    policy.jitter_enabled = true;
    policy.max_retries = 100;

    std::set<uint32_t> delays;

    for (int i = 0; i < 20; i++) {
        ReconnectCalculator calc(policy);
        uint32_t delay = calc.next_delay_ms();
        delays.insert(delay);

        // Jitter should keep delay between 500 and 1500 (±50%)
        EXPECT_GE(delay, 500u);
        EXPECT_LE(delay, 1500u);
    }

    // With jitter, we should see at least 2 different values in 20 tries
    EXPECT_GT(delays.size(), 1u);
}

TEST(ReconnectCalculatorTest, JitterWithBackoff) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 100;
    policy.backoff_multiplier = 2.0;
    policy.max_delay_ms = 10000;
    policy.jitter_enabled = true;
    policy.max_retries = 10;

    ReconnectCalculator calc(policy);

    // First: around 100 (±50%)
    uint32_t d1 = calc.next_delay_ms();
    EXPECT_GE(d1, 50u);
    EXPECT_LE(d1, 150u);

    // Second: around 200 (±50%)
    uint32_t d2 = calc.next_delay_ms();
    EXPECT_GE(d2, 100u);
    EXPECT_LE(d2, 300u);
}

// ============================================================================
// Unlimited Retries Tests
// ============================================================================

TEST(ReconnectCalculatorTest, ZeroMaxRetriesIsUnlimited) {
    ReconnectPolicy policy;
    policy.max_retries = 0;  // 0 = unlimited
    policy.auto_reconnect = true;

    ReconnectCalculator calc(policy);

    // Even after many retries, should_retry returns true
    for (int i = 0; i < 100; i++) {
        EXPECT_TRUE(calc.should_retry());
        calc.next_delay_ms();
    }
    EXPECT_TRUE(calc.should_retry());  // Still true
}

TEST(ReconnectCalculatorTest, AutoReconnectDisabled) {
    ReconnectPolicy policy;
    policy.auto_reconnect = false;

    ReconnectCalculator calc(policy);
    EXPECT_FALSE(calc.should_retry());
}

TEST(ReconnectCalculatorTest, ZeroInitialDelay) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 0;
    policy.jitter_enabled = false;
    policy.max_retries = 5;

    ReconnectCalculator calc(policy);

    // Should return 0 for first delay
    uint32_t delay = calc.next_delay_ms();
    EXPECT_EQ(delay, 0u);
}

TEST(ReconnectCalculatorTest, BackoffMultiplierOne) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 100;
    policy.backoff_multiplier = 1.0;
    policy.max_delay_ms = 10000;
    policy.jitter_enabled = false;
    policy.max_retries = 5;

    ReconnectCalculator calc(policy);

    // All delays should be the same
    EXPECT_EQ(calc.next_delay_ms(), 100u);
    EXPECT_EQ(calc.next_delay_ms(), 100u);
    EXPECT_EQ(calc.next_delay_ms(), 100u);
}

TEST(ReconnectCalculatorTest, VeryHighBackoffMultiplier) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 1;
    policy.backoff_multiplier = 100.0;
    policy.max_delay_ms = 1000;
    policy.jitter_enabled = false;
    policy.max_retries = 10;

    ReconnectCalculator calc(policy);

    // First: 1
    EXPECT_EQ(calc.next_delay_ms(), 1u);

    // Second: would be 100, still within max
    EXPECT_EQ(calc.next_delay_ms(), 100u);

    // Third: would be 10000, capped to max 1000
    EXPECT_EQ(calc.next_delay_ms(), 1000u);
}

// ============================================================================
// Policy Reference Tests
// ============================================================================

TEST(ReconnectCalculatorTest, PolicyIsReference) {
    ReconnectPolicy policy;
    policy.max_retries = 3;
    policy.auto_reconnect = true;

    ReconnectCalculator calc(policy);

    // Exhaust retries
    calc.next_delay_ms();
    calc.next_delay_ms();
    calc.next_delay_ms();
    EXPECT_FALSE(calc.should_retry());  // 3 retries exhausted

    // Modify the original policy
    policy.max_retries = 10;

    // Calculator uses reference, so it sees the change
    EXPECT_TRUE(calc.should_retry());  // Now has more retries available
}

// ============================================================================
// Peek Delay Tests
// ============================================================================

TEST(ReconnectCalculatorTest, PeekDoesNotIncrement) {
    ReconnectPolicy policy;
    policy.initial_delay_ms = 100;
    policy.backoff_multiplier = 2.0;
    policy.jitter_enabled = false;
    policy.max_retries = 10;

    ReconnectCalculator calc(policy);

    // Peek should return same value multiple times
    EXPECT_EQ(calc.peek_delay_ms(), 100u);
    EXPECT_EQ(calc.peek_delay_ms(), 100u);
    EXPECT_EQ(calc.retry_count(), 0u);

    // After one actual call, peek should show next value
    calc.next_delay_ms();
    EXPECT_EQ(calc.peek_delay_ms(), 200u);
    EXPECT_EQ(calc.retry_count(), 1u);
}
