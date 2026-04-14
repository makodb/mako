#pragma once

#include <rusty/cell.hpp>
#include <cstdint>

namespace rrr {

// Forward declare Time utility from base
// We'll use a simple time function
namespace {
    // @safe - Get current time in microseconds
    inline uint64_t current_time_us() {
        // @unsafe - system call
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
        }
    }
}

/**
 * Circuit breaker states.
 *
 * CLOSED: Normal operation, requests allowed
 * OPEN: Too many failures, requests blocked (fail-fast)
 * HALF_OPEN: Testing if service recovered, allow one probe request
 */
enum class CircuitState : int {
    CLOSED = 0,    // Normal, requests allowed
    OPEN = 1,      // Tripped, requests blocked
    HALF_OPEN = 2  // Testing, allow probe
};

// @safe - Convert CircuitState to string for logging
inline const char* circuit_state_to_string(CircuitState state) {
    switch (state) {
        case CircuitState::CLOSED: return "CLOSED";
        case CircuitState::OPEN: return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF_OPEN";
        default: return "UNKNOWN";
    }
}

/**
 * Configuration for circuit breaker behavior.
 */
struct CircuitBreakerConfig {
    uint32_t failure_threshold;     // Failures before opening (default: 5)
    uint32_t success_threshold;     // Successes to close from half-open (default: 3)
    uint32_t timeout_ms;            // Time in OPEN before trying (default: 30000ms = 30s)
    bool enabled;                   // Enable/disable circuit breaker (default: true)

    // @safe - Default constructor with reasonable defaults
    CircuitBreakerConfig()
        : failure_threshold(5)
        , success_threshold(3)
        , timeout_ms(30000)
        , enabled(true)
    {}

    // @safe - Full constructor
    CircuitBreakerConfig(
        uint32_t failure_threshold_,
        uint32_t success_threshold_,
        uint32_t timeout_ms_,
        bool enabled_
    )
        : failure_threshold(failure_threshold_)
        , success_threshold(success_threshold_)
        , timeout_ms(timeout_ms_)
        , enabled(enabled_)
    {}

    // =========================================================================
    // Config Presets
    // =========================================================================

    // @safe - Sensitive circuit breaker: trips quickly, recovers slowly
    static CircuitBreakerConfig sensitive() {
        return CircuitBreakerConfig(
            3,       // failure_threshold (3 failures)
            5,       // success_threshold (5 successes to close)
            60000,   // timeout_ms (60 seconds)
            true     // enabled
        );
    }

    // @safe - Relaxed circuit breaker: tolerates more failures
    static CircuitBreakerConfig relaxed() {
        return CircuitBreakerConfig(
            10,      // failure_threshold (10 failures)
            2,       // success_threshold (2 successes to close)
            15000,   // timeout_ms (15 seconds)
            true     // enabled
        );
    }

    // @safe - Disabled circuit breaker
    static CircuitBreakerConfig disabled() {
        return CircuitBreakerConfig(
            0,       // failure_threshold (never trip)
            0,       // success_threshold
            0,       // timeout_ms
            false    // enabled
        );
    }
};

/**
 * Circuit breaker implementation for RPC clients.
 *
 * The circuit breaker prevents cascading failures by failing fast when
 * a remote service appears to be unhealthy.
 *
 * Usage:
 *   CircuitBreaker cb(config);
 *
 *   // Before each request:
 *   if (!cb.allow_request()) {
 *       return EBUSY;  // Fail fast
 *   }
 *
 *   // After request:
 *   if (success) {
 *       cb.record_success();
 *   } else {
 *       cb.record_failure();
 *   }
 */
// @safe - Thread-safe circuit breaker using rusty::Cell for interior mutability
class CircuitBreaker {
private:
    CircuitBreakerConfig config_;
    rusty::Cell<CircuitState> state_{CircuitState::CLOSED};
    rusty::Cell<uint32_t> failure_count_{0};
    rusty::Cell<uint32_t> success_count_{0};
    rusty::Cell<uint64_t> last_failure_time_{0};
    rusty::Cell<bool> probe_in_progress_{false};

public:
    // @safe - Constructor with config
    explicit CircuitBreaker(const CircuitBreakerConfig& config = CircuitBreakerConfig())
        : config_(config)
    {}

    // @safe - Replace runtime config and reset circuit state.
    void set_config(const CircuitBreakerConfig& config) {
        // @unsafe - assignment operator is currently modeled as non-safe.
        { config_ = config; }
        reset();
    }

    // @safe - Copy constructor
    CircuitBreaker(const CircuitBreaker&) = default;
    CircuitBreaker& operator=(const CircuitBreaker&) = default;

    // @safe - Move constructor
    CircuitBreaker(CircuitBreaker&&) = default;
    CircuitBreaker& operator=(CircuitBreaker&&) = default;

    // @safe - Check if a request should be allowed
    // Returns true if request can proceed, false if should fail-fast
    bool allow_request() const {
        if (!config_.enabled) {
            return true;  // Always allow if disabled
        }

        CircuitState current = state_.get();

        switch (current) {
            case CircuitState::CLOSED:
                return true;

            case CircuitState::OPEN: {
                // Check if timeout has elapsed
                uint64_t now = current_time_us();
                uint64_t last = last_failure_time_.get();
                uint64_t timeout_us = static_cast<uint64_t>(config_.timeout_ms) * 1000;

                if (now - last >= timeout_us) {
                    // Transition to HALF_OPEN, allow probe
                    state_.set(CircuitState::HALF_OPEN);
                    probe_in_progress_.set(true);
                    return true;
                }
                return false;  // Still in timeout, fail fast
            }

            case CircuitState::HALF_OPEN:
                // Allow one probe at a time
                if (!probe_in_progress_.get()) {
                    probe_in_progress_.set(true);
                    return true;
                }
                return false;  // Probe in progress, fail fast

            default:
                return false;
        }
    }

    // @safe - Record a successful request
    void record_success() {
        if (!config_.enabled) {
            return;
        }

        CircuitState current = state_.get();

        switch (current) {
            case CircuitState::CLOSED:
                // Reset failure count on success
                failure_count_.set(0);
                break;

            case CircuitState::HALF_OPEN: {
                probe_in_progress_.set(false);
                uint32_t count = success_count_.get() + 1;
                success_count_.set(count);

                if (count >= config_.success_threshold) {
                    // Enough successes, close the circuit
                    state_.set(CircuitState::CLOSED);
                    failure_count_.set(0);
                    success_count_.set(0);
                }
                break;
            }

            case CircuitState::OPEN:
                // Shouldn't happen, but reset probe flag just in case
                probe_in_progress_.set(false);
                break;
        }
    }

    // @safe - Record a failed request
    void record_failure() {
        if (!config_.enabled) {
            return;
        }

        CircuitState current = state_.get();

        switch (current) {
            case CircuitState::CLOSED: {
                uint32_t count = failure_count_.get() + 1;
                failure_count_.set(count);

                if (count >= config_.failure_threshold) {
                    // Too many failures, open the circuit
                    state_.set(CircuitState::OPEN);
                    last_failure_time_.set(current_time_us());
                    failure_count_.set(0);
                    success_count_.set(0);
                }
                break;
            }

            case CircuitState::HALF_OPEN:
                // Single failure in half-open, re-open immediately
                probe_in_progress_.set(false);
                state_.set(CircuitState::OPEN);
                last_failure_time_.set(current_time_us());
                success_count_.set(0);
                break;

            case CircuitState::OPEN:
                // Already open, update failure time
                last_failure_time_.set(current_time_us());
                break;
        }
    }

    // @safe - Get current state
    CircuitState state() const {
        return state_.get();
    }

    // @safe - Check if circuit is open (failing fast)
    bool is_open() const {
        return state_.get() == CircuitState::OPEN;
    }

    // @safe - Check if circuit is closed (normal operation)
    bool is_closed() const {
        return state_.get() == CircuitState::CLOSED;
    }

    // @safe - Check if circuit is half-open (testing)
    bool is_half_open() const {
        return state_.get() == CircuitState::HALF_OPEN;
    }

    // @safe - Reset circuit breaker to closed state
    void reset() {
        state_.set(CircuitState::CLOSED);
        failure_count_.set(0);
        success_count_.set(0);
        last_failure_time_.set(0);
        probe_in_progress_.set(false);
    }

    // @safe - Get failure count
    uint32_t failure_count() const {
        return failure_count_.get();
    }

    // @safe - Get success count (in half-open state)
    uint32_t success_count() const {
        return success_count_.get();
    }

    // @safe - Get the configuration
    // @lifetime: (&'a) -> &'a
    const CircuitBreakerConfig& config() const {
        return config_;
    }
};

} // namespace rrr
