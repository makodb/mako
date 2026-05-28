module;

#include <stdint.h>
#include <stdlib.h>  // abort() referenced by rusty/function.hpp
#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/rusty.hpp>
#include <time.h>

export module rrr.heartbeat;

import std;

export namespace rrr {

// Free helper backing the timespec→microseconds conversion used by
// `heartbeat_time_us`. Pure u64 arithmetic; the C++ wrapper owns the
// `clock_gettime` syscall and hands the resulting (sec, nsec) pair
// in. File-prefixed name avoids link-time collision with the
// equivalent helper in circuit_breaker.cpp. Authored as inline Rust
// DSL.
#if RUSTYCPP_RUST
fn heartbeat_timespec_to_us(tv_sec: u64, tv_nsec: u64) -> u64 {
    tv_sec * 1000000 + tv_nsec / 1000
}
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.1 version=1 rust_sha256=017382a17e9a7e3c5b8333c349a1793c7ab03dbd3d61e49fb09278fec3c88038*/
uint64_t heartbeat_timespec_to_us(uint64_t tv_sec, uint64_t tv_nsec);

uint64_t heartbeat_timespec_to_us(uint64_t tv_sec, uint64_t tv_nsec) {
    return (rusty::detail::deref_if_pointer_like(tv_sec) * static_cast<uint64_t>(1000000)) + (rusty::detail::deref_if_pointer_like(tv_nsec) / static_cast<uint64_t>(1000));
}
/*RUSTYCPP:GEN-END id=heartbeat.1*/

inline uint64_t heartbeat_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return heartbeat_timespec_to_us(
        static_cast<uint64_t>(ts.tv_sec),
        static_cast<uint64_t>(ts.tv_nsec));
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

// Free helpers backing the timing-arithmetic portions of
// `HeartbeatManager::should_send_heartbeat`, `check_timeout`, and
// `time_until_next_heartbeat_ms`. All three convert `_ms` to `_us`
// and compare against a "now - last" elapsed window. Authored as
// inline Rust DSL.
#if RUSTYCPP_RUST
fn heartbeat_interval_elapsed(now_us: u64, last_send_us: u64, interval_ms: u32) -> bool {
    let interval_us: u64 = (interval_ms as u64) * 1000;
    now_us - last_send_us >= interval_us
}

fn heartbeat_timeout_elapsed(now_us: u64, last_send_us: u64, timeout_ms: u32) -> bool {
    let timeout_us: u64 = (timeout_ms as u64) * 1000;
    now_us - last_send_us >= timeout_us
}

fn heartbeat_time_until_next_ms(now_us: u64, last_send_us: u64, interval_ms: u32) -> u32 {
    let interval_us: u64 = (interval_ms as u64) * 1000;
    let elapsed_us: u64 = now_us - last_send_us;
    if elapsed_us >= interval_us {
        return 0;
    }
    ((interval_us - elapsed_us) / 1000) as u32
}
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.2 version=1 rust_sha256=ad0dc63f92a1763ba1d8538d2f3dcce52c56109bf1c6a96eec09b5ea43d3e67d*/
bool heartbeat_interval_elapsed(uint64_t now_us, uint64_t last_send_us, uint32_t interval_ms);
bool heartbeat_timeout_elapsed(uint64_t now_us, uint64_t last_send_us, uint32_t timeout_ms);
uint32_t heartbeat_time_until_next_ms(uint64_t now_us, uint64_t last_send_us, uint32_t interval_ms);

bool heartbeat_interval_elapsed(uint64_t now_us, uint64_t last_send_us, uint32_t interval_ms) {
    const uint64_t interval_us = ((static_cast<uint64_t>(interval_ms))) * static_cast<uint64_t>(1000);
    return (rusty::detail::deref_if_pointer_like(now_us) - rusty::detail::deref_if_pointer_like(last_send_us)) >= rusty::detail::deref_if_pointer_like(interval_us);
}

bool heartbeat_timeout_elapsed(uint64_t now_us, uint64_t last_send_us, uint32_t timeout_ms) {
    const uint64_t timeout_us = ((static_cast<uint64_t>(timeout_ms))) * static_cast<uint64_t>(1000);
    return (rusty::detail::deref_if_pointer_like(now_us) - rusty::detail::deref_if_pointer_like(last_send_us)) >= rusty::detail::deref_if_pointer_like(timeout_us);
}

uint32_t heartbeat_time_until_next_ms(uint64_t now_us, uint64_t last_send_us, uint32_t interval_ms) {
    const uint64_t interval_us = ((static_cast<uint64_t>(interval_ms))) * static_cast<uint64_t>(1000);
    const uint64_t elapsed_us = rusty::detail::deref_if_pointer_like(now_us) - rusty::detail::deref_if_pointer_like(last_send_us);
    if (rusty::detail::deref_if_pointer_like(elapsed_us) >= rusty::detail::deref_if_pointer_like(interval_us)) {
        return static_cast<uint32_t>(0);
    }
    return static_cast<uint32_t>((((rusty::detail::deref_if_pointer_like(interval_us) - rusty::detail::deref_if_pointer_like(elapsed_us))) / 1000));
}
/*RUSTYCPP:GEN-END id=heartbeat.2*/

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

        return heartbeat_interval_elapsed(
            heartbeat_time_us(),
            last_send_time_.get(),
            config_.interval_ms);
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

        if (heartbeat_timeout_elapsed(
                heartbeat_time_us(),
                last_send_time_.get(),
                config_.timeout_ms)) {
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

        return heartbeat_time_until_next_ms(
            heartbeat_time_us(),
            last_send_time_.get(),
            config_.interval_ms);
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
