#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>





namespace rrr {

/**
 * @safe - Enum types are trivially safe
 *
 * TimeoutType distinguishes between different timeout scenarios
 * to help with error handling and debugging.
 */
enum class TimeoutType : uint8_t {
    NONE = 0,           // No timeout occurred
    CONNECT_TIMEOUT,    // Failed to establish connection in time
    REQUEST_TIMEOUT,    // Request send operation timed out
    RESPONSE_TIMEOUT,   // Waiting for response timed out
    TOTAL_TIMEOUT       // Overall operation timeout exceeded (across retries)
};

/**
 * @safe - POD struct with trivially safe operations
 *
 * RequestOptions configures timeout and retry behavior for RPC requests.
 * Designed for use with rusty::Cell for thread-safe interior mutability.
 */
struct RequestOptions {
    // =========================================================================
    // Timeout Configuration
    // =========================================================================

    /**
     * Per-attempt timeout in milliseconds.
     * Each individual request attempt will timeout after this duration.
     * Default: 1000ms (1 second)
     */
    uint64_t timeout_ms = 1000;

    /**
     * Total operation timeout in milliseconds.
     * The entire operation (including all retries) must complete within this time.
     * 0 = no total timeout limit (retries continue until max_retries exhausted)
     * Default: 0 (no limit)
     */
    uint64_t total_timeout_ms = 0;

    // =========================================================================
    // Retry Configuration
    // =========================================================================

    /**
     * Maximum number of retry attempts after initial failure.
     * 0 = no retries (fail immediately on timeout)
     * Default: 0
     */
    uint16_t max_retries = 0;

    /**
     * Base delay for exponential backoff in milliseconds.
     * Actual delay = base_delay_ms * 2^attempt_number (capped at max_delay_ms)
     * Default: 50ms
     */
    uint16_t base_delay_ms = 50;

    /**
     * Maximum delay between retry attempts in milliseconds.
     * Backoff is capped at this value regardless of attempt number.
     * Default: 5000ms (5 seconds)
     */
    uint16_t max_delay_ms = 5000;

    /**
     * Jitter factor for backoff randomization.
     * Delay is randomized by +/- (jitter_factor * delay) to avoid thundering herd.
     * Range: 0.0 to 1.0
     * Default: 0.1 (10% jitter)
     */
    float jitter_factor = 0.1f;

    // =========================================================================
    // Idempotency Configuration
    // =========================================================================

    /**
     * Whether this request is idempotent (safe to retry).
     * Only idempotent requests will be automatically retried on timeout.
     * Non-idempotent requests fail immediately on timeout.
     * Default: false
     */
    bool idempotent = false;

    // =========================================================================
    // Factory Methods (Presets)
    // =========================================================================

    /**
     * @unsafe - POD constructor (rusty-cpp sees implicit constructor as unsafe)
     * Default options: 1 second timeout, no retry
     */
    static RequestOptions defaults() {
        // @unsafe { POD construction }
        return RequestOptions{};
    }

    /**
     * @unsafe - POD constructor (rusty-cpp sees implicit constructor as unsafe)
     * Options with retry enabled
     * @param max_retries Maximum retry attempts
     * @param timeout_ms Per-attempt timeout (default 1000ms)
     */
    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms = 1000) {
        // @unsafe { POD construction }
        RequestOptions opts;
        opts.timeout_ms = timeout_ms;
        opts.max_retries = max_retries;
        opts.idempotent = true;  // Retry only makes sense for idempotent ops
        return opts;
    }

    /**
     * @unsafe - POD constructor (rusty-cpp sees implicit constructor as unsafe)
     * Options for idempotent requests with automatic retry
     * @param max_retries Maximum retry attempts (default 3)
     */
    static RequestOptions idempotent_retry(uint16_t max_retries = 3) {
        // @unsafe { POD construction }
        RequestOptions opts;
        opts.max_retries = max_retries;
        opts.idempotent = true;
        return opts;
    }

    /**
     * @unsafe - POD constructor (rusty-cpp sees implicit constructor as unsafe)
     * Options with no timeout (wait indefinitely)
     */
    static RequestOptions no_timeout() {
        // @unsafe { POD construction }
        RequestOptions opts;
        opts.timeout_ms = 0;
        opts.total_timeout_ms = 0;
        return opts;
    }

    /**
     * @unsafe - POD constructor (rusty-cpp sees implicit constructor as unsafe)
     * Fast timeout for health checks and probes
     * Short timeout, few retries, small backoff
     */
    static RequestOptions fast() {
        // @unsafe { POD construction }
        RequestOptions opts;
        opts.timeout_ms = 100;  // 100ms
        opts.max_retries = 2;
        opts.base_delay_ms = 10;
        opts.max_delay_ms = 100;
        opts.idempotent = true;
        return opts;
    }

    /**
     * @unsafe - POD constructor (rusty-cpp sees implicit constructor as unsafe)
     * Patient options for slow operations
     * Long timeout, more retries, larger backoff
     */
    static RequestOptions patient() {
        // @unsafe { POD construction }
        RequestOptions opts;
        opts.timeout_ms = 10000;  // 10 seconds
        opts.total_timeout_ms = 60000;  // 1 minute total
        opts.max_retries = 5;
        opts.base_delay_ms = 500;
        opts.max_delay_ms = 10000;
        opts.idempotent = true;
        return opts;
    }

    // =========================================================================
    // Helper Methods
    // =========================================================================

    /**
     * @safe - Check if retry is allowed based on current attempt
     * @param current_retry_count Current number of retries attempted
     * @return true if another retry is allowed
     */
    bool can_retry(uint16_t current_retry_count) const {
        return idempotent && current_retry_count < max_retries;
    }

    /**
     * @safe - Calculate delay for a given retry attempt with jitter
     * Uses exponential backoff: base_delay * 2^attempt (capped at max_delay)
     * @param attempt Retry attempt number (0-based)
     * @return Delay in milliseconds
     */
    uint64_t calculate_delay_ms(uint16_t attempt) const {
        // Calculate exponential delay: base * 2^attempt
        double delay = static_cast<double>(base_delay_ms) * std::pow(2.0, attempt);

        // Cap at max_delay
        if (delay > static_cast<double>(max_delay_ms)) {
            delay = static_cast<double>(max_delay_ms);
        }

        // Apply jitter: +/- (jitter_factor * delay / 2)
        if (jitter_factor > 0.0f) {
            // Thread-local random engine for jitter
            thread_local std::mt19937 gen(std::random_device{}());
            thread_local std::uniform_real_distribution<double> dist(-0.5, 0.5);

            double jitter = delay * static_cast<double>(jitter_factor) * dist(gen);
            delay += jitter;

            // Ensure non-negative
            if (delay < 0.0) {
                delay = 0.0;
            }
        }

        return static_cast<uint64_t>(delay);
    }

    /**
     * @safe - Check if total timeout has been exceeded
     * @param elapsed_ms Time elapsed since operation started
     * @return true if total timeout exceeded
     */
    bool is_total_timeout_exceeded(uint64_t elapsed_ms) const {
        return total_timeout_ms > 0 && elapsed_ms >= total_timeout_ms;
    }

    /**
     * @safe - Get remaining time before total timeout
     * @param elapsed_ms Time elapsed since operation started
     * @return Remaining milliseconds (0 if no total timeout or already exceeded)
     */
    uint64_t remaining_time_ms(uint64_t elapsed_ms) const {
        if (total_timeout_ms == 0) {
            return UINT64_MAX;  // No limit
        }
        if (elapsed_ms >= total_timeout_ms) {
            return 0;
        }
        return total_timeout_ms - elapsed_ms;
    }
};

/**
 * @safe - Convert TimeoutType to string for logging/debugging
 */
inline const char* timeout_type_to_string(TimeoutType type) {
    switch (type) {
        case TimeoutType::NONE: return "NONE";
        case TimeoutType::CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
        case TimeoutType::REQUEST_TIMEOUT: return "REQUEST_TIMEOUT";
        case TimeoutType::RESPONSE_TIMEOUT: return "RESPONSE_TIMEOUT";
        case TimeoutType::TOTAL_TIMEOUT: return "TOTAL_TIMEOUT";
        default: return "UNKNOWN";
    }
}

} // namespace rrr
