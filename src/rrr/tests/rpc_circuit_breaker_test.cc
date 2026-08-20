/**
 * Unit tests for CircuitBreaker
 * Tests state transitions, timeout behavior, and configuration presets.
 */

#include <gtest/gtest.h>
#include "../rrr.hpp"

// Trimmed from the consumer umbrella (08b68144) — import directly.
import rrr.circuit_breaker;

import std;

using namespace rrr;
using namespace std::chrono;

// ============================================================================
// Configuration Tests
// ============================================================================

TEST(CircuitBreakerConfigTest, DefaultValues) {
    auto config = CircuitBreakerConfig::defaults();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.failure_threshold, 5u);
    EXPECT_EQ(config.success_threshold, 3u);
    EXPECT_EQ(config.timeout_ms, 30000u);
}

TEST(CircuitBreakerConfigTest, SensitivePreset) {
    auto config = CircuitBreakerConfig::sensitive();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.failure_threshold, 3u);
    EXPECT_EQ(config.success_threshold, 5u);
    EXPECT_EQ(config.timeout_ms, 60000u);
}

TEST(CircuitBreakerConfigTest, RelaxedPreset) {
    auto config = CircuitBreakerConfig::relaxed();
    EXPECT_TRUE(config.enabled);
    EXPECT_EQ(config.failure_threshold, 10u);
    EXPECT_EQ(config.success_threshold, 2u);
    EXPECT_EQ(config.timeout_ms, 15000u);
}

TEST(CircuitBreakerConfigTest, DisabledPreset) {
    auto config = CircuitBreakerConfig::disabled();
    EXPECT_FALSE(config.enabled);
}

TEST(CircuitBreakerStateTest, StateToString) {
    EXPECT_EQ(circuit_state_to_string(CircuitState::CLOSED), "CLOSED");
    EXPECT_EQ(circuit_state_to_string(CircuitState::OPEN), "OPEN");
    EXPECT_EQ(circuit_state_to_string(CircuitState::HALF_OPEN), "HALF_OPEN");
}

// ============================================================================
// Initial State Tests
// ============================================================================

TEST(CircuitBreakerTest, InitialStateClosed) {
    auto cb = CircuitBreaker::new_(CircuitBreakerConfig{});
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_TRUE(cb.is_closed());
    EXPECT_FALSE(cb.is_open());
    EXPECT_FALSE(cb.is_half_open());
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.failure_count(), 0u);
}

// ============================================================================
// State Transition Tests
// ============================================================================

TEST(CircuitBreakerTest, ClosedToOpenAfterThresholdFailures) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 3;
    auto cb = CircuitBreaker::new_(config);

    // Record failures up to threshold
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);

    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);

    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
    EXPECT_TRUE(cb.is_open());
    EXPECT_FALSE(cb.allow_request());
}

TEST(CircuitBreakerTest, SuccessResetsFailureCount) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 5;
    auto cb = CircuitBreaker::new_(config);

    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.failure_count(), 2u);

    cb.record_success();
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
}

TEST(CircuitBreakerTest, OpenToHalfOpenAfterTimeout) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.timeout_ms = 50;  // Short timeout for testing
    auto cb = CircuitBreaker::new_(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
    EXPECT_FALSE(cb.allow_request());

    // Wait for recovery timeout
    std::this_thread::sleep_for(milliseconds(100));

    // Should transition to HALF_OPEN on allow_request
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);
}

TEST(CircuitBreakerTest, HalfOpenToClosedOnSuccessThreshold) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.success_threshold = 2;
    config.timeout_ms = 10;
    auto cb = CircuitBreaker::new_(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();

    // Wait for HALF_OPEN
    std::this_thread::sleep_for(milliseconds(50));
    cb.allow_request();  // Triggers HALF_OPEN
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

    // First success
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);
    EXPECT_EQ(cb.success_count(), 1u);

    // Allow another probe
    cb.allow_request();

    // Second success should close circuit
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_EQ(cb.failure_count(), 0u);
}

TEST(CircuitBreakerTest, HalfOpenToOpenOnFailure) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.timeout_ms = 10;
    auto cb = CircuitBreaker::new_(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();

    // Wait for HALF_OPEN
    std::this_thread::sleep_for(milliseconds(50));
    cb.allow_request();  // Triggers HALF_OPEN
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

    // Failure should re-open circuit
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
}

// ============================================================================
// Disabled Circuit Breaker Tests
// ============================================================================

TEST(CircuitBreakerTest, DisabledAlwaysAllowsRequests) {
    auto config = CircuitBreakerConfig::disabled();
    auto cb = CircuitBreaker::new_(config);

    // Record many failures
    for (int i = 0; i < 100; i++) {
        cb.record_failure();
    }

    // Should still allow requests
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
}

// ============================================================================
// Reset Tests
// ============================================================================

TEST(CircuitBreakerTest, ResetToClosed) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    auto cb = CircuitBreaker::new_(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);

    // Reset should go back to CLOSED
    cb.reset();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_TRUE(cb.allow_request());
}

// ============================================================================
// Half-Open Probe Limiting Tests
// ============================================================================

TEST(CircuitBreakerTest, HalfOpenLimitsProbes) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.timeout_ms = 10;
    auto cb = CircuitBreaker::new_(config);

    // Open the circuit
    cb.record_failure();
    cb.record_failure();

    // Wait for HALF_OPEN
    std::this_thread::sleep_for(milliseconds(50));

    // First request allowed, transitions to HALF_OPEN
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::HALF_OPEN);

    // Second request blocked (probe in progress)
    EXPECT_FALSE(cb.allow_request());
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(CircuitBreakerTest, ThresholdOne) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 1;
    auto cb = CircuitBreaker::new_(config);

    // Single failure opens circuit
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::OPEN);
    EXPECT_FALSE(cb.allow_request());
}

TEST(CircuitBreakerTest, VeryHighThreshold) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 1000000;
    auto cb = CircuitBreaker::new_(config);

    // Many failures, but not enough
    for (int i = 0; i < 1000; i++) {
        cb.record_failure();
    }

    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_TRUE(cb.allow_request());
}

TEST(CircuitBreakerTest, SuccessInClosedState) {
    auto cb = CircuitBreaker::new_(CircuitBreakerConfig{});

    // Success in CLOSED state should be no-op (just reset failure count)
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_EQ(cb.failure_count(), 0u);
}

// ============================================================================
// Repeated Access Tests
// ============================================================================

TEST(CircuitBreakerTest, RepeatedFailuresRemainSingleThreaded) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 100;
    auto cb = CircuitBreaker::new_(config);

    // CircuitBreaker is Cell-backed (Send, not Sync). Exercise repeated
    // mutation on its supported single-threaded path instead of racing Cell.
    for (int i = 0; i < 50; ++i) {
        cb.record_failure();
    }

    EXPECT_EQ(cb.failure_count(), 50u);
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);  // Below threshold
}

TEST(CircuitBreakerTest, RepeatedStateQueries) {
    auto cb = CircuitBreaker::new_(CircuitBreakerConfig{});

    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(cb.state(), CircuitState::CLOSED);
        EXPECT_TRUE(cb.allow_request());
    }
}

// ============================================================================
// Config Access Tests
// ============================================================================

TEST(CircuitBreakerTest, ConfigAccess) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 10;
    config.timeout_ms = 5000;
    auto cb = CircuitBreaker::new_(config);

    EXPECT_EQ(cb.config().failure_threshold, 10u);
    EXPECT_EQ(cb.config().timeout_ms, 5000u);
}

// ============================================================================
// Success Count Tests
// ============================================================================

TEST(CircuitBreakerTest, SuccessCountTracking) {
    auto config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.success_threshold = 3;
    config.timeout_ms = 10;
    auto cb = CircuitBreaker::new_(config);

    // Open circuit
    cb.record_failure();
    cb.record_failure();

    // Wait and go to HALF_OPEN
    std::this_thread::sleep_for(milliseconds(50));
    cb.allow_request();

    EXPECT_EQ(cb.success_count(), 0u);

    // Record successes
    cb.record_success();
    EXPECT_EQ(cb.success_count(), 1u);

    cb.allow_request();
    cb.record_success();
    EXPECT_EQ(cb.success_count(), 2u);

    cb.allow_request();
    cb.record_success();
    // After hitting threshold, should be CLOSED with reset counts
    EXPECT_EQ(cb.state(), CircuitState::CLOSED);
    EXPECT_EQ(cb.success_count(), 0u);
}
