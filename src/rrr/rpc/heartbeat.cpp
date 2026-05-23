module;

#include <cstdint>
#include <cstdlib>  // std::abort referenced by rusty/function.hpp
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <time.h>

export module rrr.heartbeat;

import std;

export namespace rrr {

inline uint64_t heartbeat_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

struct HeartbeatConfig {
    bool enabled;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t max_missed;

    HeartbeatConfig()
        : enabled(true)
        , interval_ms(10000)
        , timeout_ms(5000)
        , max_missed(3)
    {}

    HeartbeatConfig(
        bool enabled_,
        uint32_t interval_ms_,
        uint32_t timeout_ms_,
        uint32_t max_missed_
    )
        : enabled(enabled_)
        , interval_ms(interval_ms_)
        , timeout_ms(timeout_ms_)
        , max_missed(max_missed_)
    {}

    static HeartbeatConfig aggressive() {
        return HeartbeatConfig(true, 5000, 2000, 2);
    }

    static HeartbeatConfig relaxed() {
        return HeartbeatConfig(true, 30000, 15000, 5);
    }

    static HeartbeatConfig disabled() {
        return HeartbeatConfig(false, 0, 0, 0);
    }
};

// @safe - Heartbeat tracker. Fields are rusty::Cell<T> for trivially-
// copyable interior mutability + rusty::Function<void()> for the timeout
// callback. No raw pointers, syscalls, or operator-overload chains.
class HeartbeatManager {
private:
    HeartbeatConfig config_;

    rusty::Cell<uint64_t> last_send_time_{0};
    rusty::Cell<uint64_t> last_recv_time_{0};

    rusty::Cell<uint32_t> missed_count_{0};
    rusty::Cell<bool> pending_pong_{false};
    rusty::Cell<bool> timed_out_{false};

    rusty::Function<void()> on_timeout_;

public:
    explicit HeartbeatManager(const HeartbeatConfig& config = HeartbeatConfig())
        : config_(config)
    {}

    void set_config(const HeartbeatConfig& config) {
        config_ = config;
        reset();
    }

    void set_on_timeout(rusty::Function<void()> callback) {
        on_timeout_ = std::move(callback);
    }

    bool should_send_heartbeat() const {
        if (!config_.enabled || timed_out_.get()) {
            return false;
        }

        if (pending_pong_.get()) {
            return false;
        }

        uint64_t now = heartbeat_time_us();
        uint64_t last = last_send_time_.get();
        uint64_t interval_us = static_cast<uint64_t>(config_.interval_ms) * 1000;

        return (now - last >= interval_us);
    }

    void on_heartbeat_sent() {
        if (!config_.enabled) return;

        last_send_time_.set(heartbeat_time_us());
        pending_pong_.set(true);
    }

    void on_pong_received() {
        if (!config_.enabled) return;

        last_recv_time_.set(heartbeat_time_us());
        pending_pong_.set(false);
        missed_count_.set(0);
        timed_out_.set(false);
    }

    bool check_timeout() {
        if (!config_.enabled || timed_out_.get()) {
            return false;
        }

        if (!pending_pong_.get()) {
            return false;
        }

        uint64_t now = heartbeat_time_us();
        uint64_t sent = last_send_time_.get();
        uint64_t timeout_us = static_cast<uint64_t>(config_.timeout_ms) * 1000;

        if (now - sent >= timeout_us) {
            pending_pong_.set(false);
            uint32_t count = missed_count_.get() + 1;
            missed_count_.set(count);

            if (count >= config_.max_missed) {
                timed_out_.set(true);

                if (on_timeout_) {
                    on_timeout_();
                }
                return true;
            }
        }

        return false;
    }

    uint32_t time_until_next_heartbeat_ms() const {
        if (!config_.enabled || timed_out_.get() || pending_pong_.get()) {
            return config_.interval_ms;
        }

        uint64_t now = heartbeat_time_us();
        uint64_t last = last_send_time_.get();
        uint64_t interval_us = static_cast<uint64_t>(config_.interval_ms) * 1000;

        if (now - last >= interval_us) {
            return 0;
        }

        return static_cast<uint32_t>((interval_us - (now - last)) / 1000);
    }

    bool is_timed_out() const {
        return timed_out_.get();
    }

    uint32_t missed_count() const {
        return missed_count_.get();
    }

    bool is_pending_pong() const {
        return pending_pong_.get();
    }

    void reset() {
        last_send_time_.set(0);
        last_recv_time_.set(0);
        missed_count_.set(0);
        pending_pong_.set(false);
        timed_out_.set(false);
    }

    const HeartbeatConfig& config() const {
        return config_;
    }
};

} // export namespace rrr
