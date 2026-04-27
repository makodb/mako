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

#include <rusty/cell.hpp>




namespace rrr {

/**
 * Configuration for automatic reconnection behavior.
 *
 * This is a simple POD struct - no thread safety needed for the config itself.
 * The config is typically set once at construction and not modified.
 */
struct ReconnectPolicy {
    bool auto_reconnect;         // Enable automatic reconnection
    uint32_t max_retries;        // Maximum reconnection attempts (0 = unlimited)
    uint32_t initial_delay_ms;   // Initial delay before first retry
    uint32_t max_delay_ms;       // Maximum delay between retries
    double backoff_multiplier;   // Exponential backoff multiplier (e.g., 2.0)
    bool jitter_enabled;         // Add randomness to prevent thundering herd

    // @safe - Default constructor with reasonable defaults
    ReconnectPolicy()
        : auto_reconnect(true)
        , max_retries(5)
        , initial_delay_ms(1000)     // 1 second
        , max_delay_ms(30000)        // 30 seconds
        , backoff_multiplier(2.0)
        , jitter_enabled(true)
    {}

    // @safe - Full constructor
    ReconnectPolicy(
        bool auto_reconnect_,
        uint32_t max_retries_,
        uint32_t initial_delay_ms_,
        uint32_t max_delay_ms_,
        double backoff_multiplier_,
        bool jitter_enabled_
    )
        : auto_reconnect(auto_reconnect_)
        , max_retries(max_retries_)
        , initial_delay_ms(initial_delay_ms_)
        , max_delay_ms(max_delay_ms_)
        , backoff_multiplier(backoff_multiplier_)
        , jitter_enabled(jitter_enabled_)
    {}

    // =========================================================================
    // Policy Presets
    // =========================================================================

    // @safe - Aggressive reconnection: fast retries, unlimited attempts
    // Good for internal services where quick recovery is important
    static ReconnectPolicy aggressive() {
        return ReconnectPolicy(
            true,    // auto_reconnect
            0,       // max_retries (unlimited)
            100,     // initial_delay_ms (100ms)
            5000,    // max_delay_ms (5 seconds)
            1.5,     // backoff_multiplier
            true     // jitter_enabled
        );
    }

    // @safe - Conservative reconnection: slower retries, limited attempts
    // Good for external services where we don't want to overwhelm
    static ReconnectPolicy conservative() {
        return ReconnectPolicy(
            true,    // auto_reconnect
            5,       // max_retries
            1000,    // initial_delay_ms (1 second)
            30000,   // max_delay_ms (30 seconds)
            2.0,     // backoff_multiplier
            true     // jitter_enabled
        );
    }

    // @safe - No automatic reconnection
    // For cases where manual control is needed
    static ReconnectPolicy no_retry() {
        return ReconnectPolicy(
            false,   // auto_reconnect
            0,       // max_retries
            0,       // initial_delay_ms
            0,       // max_delay_ms
            1.0,     // backoff_multiplier
            false    // jitter_enabled
        );
    }
};

/**
 * Calculator for exponential backoff delays with optional jitter.
 *
 * Thread-safe via rusty::Cell for interior mutability.
 *
 * Usage:
 *   ReconnectCalculator calc(policy);
 *   while (calc.should_retry()) {
 *       uint32_t delay = calc.next_delay_ms();
 *       sleep(delay);
 *       if (try_connect()) {
 *           calc.reset();
 *           break;
 *       }
 *   }
 */
// @safe - Thread-safe exponential backoff calculator
class ReconnectCalculator {
private:
    const ReconnectPolicy& policy_;
    rusty::Cell<uint32_t> retry_count_{0};

public:
    // @safe - Constructor with policy reference
    explicit ReconnectCalculator(const ReconnectPolicy& policy)
        : policy_(policy)
    {}

    // Delete copy (policy reference would become invalid)
    ReconnectCalculator(const ReconnectCalculator&) = delete;
    ReconnectCalculator& operator=(const ReconnectCalculator&) = delete;

    // Move is allowed
    ReconnectCalculator(ReconnectCalculator&&) = default;
    ReconnectCalculator& operator=(ReconnectCalculator&&) = default;

    // @safe - Check if we should attempt another retry
    bool should_retry() const {
        if (!policy_.auto_reconnect) {
            return false;
        }
        // max_retries == 0 means unlimited
        if (policy_.max_retries == 0) {
            return true;
        }
        return retry_count_.get() < policy_.max_retries;
    }

    // @safe - Calculate the next delay and increment retry count
    // Returns delay in milliseconds
    uint32_t next_delay_ms() {
        uint32_t count = retry_count_.get();
        retry_count_.set(count + 1);

        // Calculate base delay with exponential backoff
        // delay = initial * (multiplier ^ count)
        double delay = static_cast<double>(policy_.initial_delay_ms);
        for (uint32_t i = 0; i < count; ++i) {
            delay *= policy_.backoff_multiplier;
            // Early exit if we've already hit max
            if (delay >= static_cast<double>(policy_.max_delay_ms)) {
                delay = static_cast<double>(policy_.max_delay_ms);
                break;
            }
        }

        // Cap at max delay
        delay = std::min(delay, static_cast<double>(policy_.max_delay_ms));

        // Apply jitter if enabled (+/- 50%)
        if (policy_.jitter_enabled && delay > 0) {
            // @unsafe - std::random_device and distribution operations
            {
                std::random_device rd;
                std::uniform_real_distribution<double> dist(0.5, 1.5);
                delay *= dist(rd);
            }
        }

        return static_cast<uint32_t>(delay);
    }

    // @safe - Peek at what the next delay would be without incrementing
    uint32_t peek_delay_ms() const {
        uint32_t count = retry_count_.get();

        double delay = static_cast<double>(policy_.initial_delay_ms);
        for (uint32_t i = 0; i < count; ++i) {
            delay *= policy_.backoff_multiplier;
            if (delay >= static_cast<double>(policy_.max_delay_ms)) {
                delay = static_cast<double>(policy_.max_delay_ms);
                break;
            }
        }

        delay = std::min(delay, static_cast<double>(policy_.max_delay_ms));

        // Note: jitter is not applied in peek since it's random
        return static_cast<uint32_t>(delay);
    }

    // @safe - Reset retry count (call on successful connection)
    void reset() {
        retry_count_.set(0);
    }

    // @safe - Get current retry count
    uint32_t retry_count() const {
        return retry_count_.get();
    }

    // @safe - Check if retries are exhausted
    bool retries_exhausted() const {
        if (!policy_.auto_reconnect) {
            return true;
        }
        if (policy_.max_retries == 0) {
            return false;  // Unlimited retries
        }
        return retry_count_.get() >= policy_.max_retries;
    }
};

} // namespace rrr
