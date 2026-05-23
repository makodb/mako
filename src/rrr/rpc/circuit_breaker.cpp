module;

#include <cstdint>
#include <rusty/cell.hpp>
#include <time.h>

export module rrr.circuit_breaker;

import std;

export namespace rrr {

inline uint64_t current_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

enum class CircuitState : int {
    CLOSED = 0,
    OPEN = 1,
    HALF_OPEN = 2
};

inline const char* circuit_state_to_string(CircuitState state) {
    switch (state) {
        case CircuitState::CLOSED: return "CLOSED";
        case CircuitState::OPEN: return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF_OPEN";
        default: return "UNKNOWN";
    }
}

struct CircuitBreakerConfig {
    uint32_t failure_threshold;
    uint32_t success_threshold;
    uint32_t timeout_ms;
    bool enabled;

    CircuitBreakerConfig()
        : failure_threshold(5)
        , success_threshold(3)
        , timeout_ms(30000)
        , enabled(true)
    {}

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

    static CircuitBreakerConfig sensitive() {
        return CircuitBreakerConfig(3, 5, 60000, true);
    }

    static CircuitBreakerConfig relaxed() {
        return CircuitBreakerConfig(10, 2, 15000, true);
    }

    static CircuitBreakerConfig disabled() {
        return CircuitBreakerConfig(0, 0, 0, false);
    }
};

// @safe - Single-threaded circuit breaker state machine. All fields are
// rusty::Cell<T> for trivially-copyable interior mutability; no raw
// pointers, syscalls, or operator-overload chains.
class CircuitBreaker {
private:
    CircuitBreakerConfig config_;
    rusty::Cell<CircuitState> state_{CircuitState::CLOSED};
    rusty::Cell<uint32_t> failure_count_{0};
    rusty::Cell<uint32_t> success_count_{0};
    rusty::Cell<uint64_t> last_failure_time_{0};
    rusty::Cell<bool> probe_in_progress_{false};

public:
    explicit CircuitBreaker(const CircuitBreakerConfig& config = CircuitBreakerConfig())
        : config_(config)
    {}

    void set_config(const CircuitBreakerConfig& config) {
        config_ = config;
        reset();
    }

    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;
    CircuitBreaker(CircuitBreaker&&) = delete;
    CircuitBreaker& operator=(CircuitBreaker&&) = delete;

    bool allow_request() const {
        if (!config_.enabled) {
            return true;
        }

        CircuitState current = state_.get();

        switch (current) {
            case CircuitState::CLOSED:
                return true;

            case CircuitState::OPEN: {
                uint64_t now = current_time_us();
                uint64_t last = last_failure_time_.get();
                uint64_t timeout_us = static_cast<uint64_t>(config_.timeout_ms) * 1000;

                if (now - last >= timeout_us) {
                    state_.set(CircuitState::HALF_OPEN);
                    probe_in_progress_.set(true);
                    return true;
                }
                return false;
            }

            case CircuitState::HALF_OPEN:
                if (!probe_in_progress_.get()) {
                    probe_in_progress_.set(true);
                    return true;
                }
                return false;

            default:
                return false;
        }
    }

    void record_success() {
        if (!config_.enabled) {
            return;
        }

        CircuitState current = state_.get();

        switch (current) {
            case CircuitState::CLOSED:
                failure_count_.set(0);
                break;

            case CircuitState::HALF_OPEN: {
                probe_in_progress_.set(false);
                uint32_t count = success_count_.get() + 1;
                success_count_.set(count);

                if (count >= config_.success_threshold) {
                    state_.set(CircuitState::CLOSED);
                    failure_count_.set(0);
                    success_count_.set(0);
                }
                break;
            }

            case CircuitState::OPEN:
                probe_in_progress_.set(false);
                break;
        }
    }

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
                    state_.set(CircuitState::OPEN);
                    last_failure_time_.set(current_time_us());
                    failure_count_.set(0);
                    success_count_.set(0);
                }
                break;
            }

            case CircuitState::HALF_OPEN:
                probe_in_progress_.set(false);
                state_.set(CircuitState::OPEN);
                last_failure_time_.set(current_time_us());
                success_count_.set(0);
                break;

            case CircuitState::OPEN:
                last_failure_time_.set(current_time_us());
                break;
        }
    }

    CircuitState state() const {
        return state_.get();
    }

    bool is_open() const {
        return state_.get() == CircuitState::OPEN;
    }

    bool is_closed() const {
        return state_.get() == CircuitState::CLOSED;
    }

    bool is_half_open() const {
        return state_.get() == CircuitState::HALF_OPEN;
    }

    void reset() {
        state_.set(CircuitState::CLOSED);
        failure_count_.set(0);
        success_count_.set(0);
        last_failure_time_.set(0);
        probe_in_progress_.set(false);
    }

    uint32_t failure_count() const {
        return failure_count_.get();
    }

    uint32_t success_count() const {
        return success_count_.get();
    }

    const CircuitBreakerConfig& config() const {
        return config_;
    }
};

} // export namespace rrr
