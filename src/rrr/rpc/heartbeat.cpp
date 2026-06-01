module;

#include <rusty/cell.hpp>
#include <rusty/function.hpp>
#include <rusty/move.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>
#include <cstdint>
#include <cstdlib>  // std::abort referenced by rusty/function.hpp
#include <time.h>

export module rrr.heartbeat;

import std;

export namespace rrr {

inline uint64_t heartbeat_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

// Type alias for the heartbeat timeout callback. Defined outside the
// DSL block so the inline-Rust source can refer to it by an opaque
// type name (the DSL transpiler cannot parse C++ function-type
// template arguments like `rusty::Function<void()>` directly).
using HeartbeatTimeoutCallback = rusty::Function<void()>;

// HeartbeatConfig is a plain aggregate POD: no user-declared
// constructors, fields carry in-class default initializers that the
// previous default ctor body matched. Intentionally kept in plain
// C++ (not migrated to inline-Rust DSL) because the DSL does not
// emit per-field in-class `= default` initializers, and the few
// `HeartbeatConfig config; config.X = ...;` default-mutate-customize
// callers need those documented defaults present after default
// construction.
//
// The previous user-defined default and parameterized ctors were
// dropped; the parameterized form was used only by the three named
// factories below, which now use positional aggregate-init.
struct HeartbeatConfig {
    bool enabled = true;
    uint32_t interval_ms = 10000;
    uint32_t timeout_ms = 5000;
    uint32_t max_missed = 3;

    static HeartbeatConfig aggressive() {
        return HeartbeatConfig{true, 5000, 2000, 2};
    }

    static HeartbeatConfig relaxed() {
        return HeartbeatConfig{true, 30000, 15000, 5};
    }

    static HeartbeatConfig disabled() {
        return HeartbeatConfig{false, 0, 0, 0};
    }
};

// `HeartbeatManager` — single-threaded heartbeat tracker. All
// time/count/flag state is `rusty::Cell<T>` for trivially-copyable
// interior mutability; the only non-Cell fields are the owned
// `HeartbeatConfig` value (replaced by `set_config()`) and the
// `HeartbeatTimeoutCallback` (a `rusty::Function<void()>`, replaced
// by `set_on_timeout()`).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The plain `fn new(config)`
// lowers to a `static HeartbeatManager new_(const HeartbeatConfig&)`
// factory; callers construct via the factory rather than direct ctor
// syntax.
//
// Behavioral diffs from the original C++ class:
//   * Methods that previously were non-const but only touched Cell
//     fields (`on_heartbeat_sent`, `on_pong_received`, `reset`) are
//     now `const`. The body still mutates state, only through Cells.
//     Callers that held a non-const ref keep working.
//   * `check_timeout()` stays non-const: it both mutates Cells AND
//     fires the `on_timeout_` callback (a `rusty::Function<void()>`
//     whose `operator()` is non-const).
//   * `set_config()` and `set_on_timeout()` stay non-const (they
//     overwrite the by-value `config_` field / move-assign the
//     `on_timeout_` Function field).
//   * Fields are no longer marked `private`. No callers reach into
//     them.
#if RUSTYCPP_RUST
struct HeartbeatManager {
    config_field: HeartbeatConfig,
    last_send_time: Cell<u64>,
    last_recv_time: Cell<u64>,
    missed_count_field: Cell<u32>,
    pending_pong: Cell<bool>,
    timed_out: Cell<bool>,
    on_timeout: HeartbeatTimeoutCallback,
}

impl HeartbeatManager {
    fn new(config: &HeartbeatConfig) -> HeartbeatManager {
        HeartbeatManager {
            config_field: config.clone(),
            last_send_time: Cell::<u64>::new(0u64),
            last_recv_time: Cell::<u64>::new(0u64),
            missed_count_field: Cell::<u32>::new(0u32),
            pending_pong: Cell::<bool>::new(false),
            timed_out: Cell::<bool>::new(false),
            on_timeout: HeartbeatTimeoutCallback {},
        }
    }

    fn set_config(&mut self, config: &HeartbeatConfig) {
        self.config_field = config.clone();
        self.reset();
    }

    fn set_on_timeout(&mut self, callback: HeartbeatTimeoutCallback) {
        self.on_timeout = callback;
    }

    fn should_send_heartbeat(&self) -> bool {
        if !self.config_field.enabled || self.timed_out.get() {
            return false;
        }
        if self.pending_pong.get() {
            return false;
        }

        let now: u64 = heartbeat_time_us();
        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.interval_ms as u64) * 1000u64;

        (now - last) >= interval_us
    }

    fn on_heartbeat_sent(&self) {
        if !self.config_field.enabled {
            return;
        }
        self.last_send_time.set(heartbeat_time_us());
        self.pending_pong.set(true);
    }

    fn on_pong_received(&self) {
        if !self.config_field.enabled {
            return;
        }
        self.last_recv_time.set(heartbeat_time_us());
        self.pending_pong.set(false);
        self.missed_count_field.set(0u32);
        self.timed_out.set(false);
    }

    fn check_timeout(&mut self) -> bool {
        if !self.config_field.enabled || self.timed_out.get() {
            return false;
        }
        if !self.pending_pong.get() {
            return false;
        }

        let now: u64 = heartbeat_time_us();
        let sent: u64 = self.last_send_time.get();
        let timeout_us: u64 = (self.config_field.timeout_ms as u64) * 1000u64;

        if (now - sent) >= timeout_us {
            self.pending_pong.set(false);
            let count: u32 = self.missed_count_field.get() + 1u32;
            self.missed_count_field.set(count);

            if count >= self.config_field.max_missed {
                self.timed_out.set(true);
                if self.on_timeout {
                    self.on_timeout();
                }
                return true;
            }
        }
        false
    }

    fn time_until_next_heartbeat_ms(&self) -> u32 {
        if !self.config_field.enabled
            || self.timed_out.get()
            || self.pending_pong.get()
        {
            return self.config_field.interval_ms;
        }

        let now: u64 = heartbeat_time_us();
        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.interval_ms as u64) * 1000u64;

        if (now - last) >= interval_us {
            return 0u32;
        }

        ((interval_us - (now - last)) / 1000u64) as u32
    }

    fn is_timed_out(&self) -> bool {
        self.timed_out.get()
    }

    fn missed_count(&self) -> u32 {
        self.missed_count_field.get()
    }

    fn is_pending_pong(&self) -> bool {
        self.pending_pong.get()
    }

    fn reset(&self) {
        self.last_send_time.set(0u64);
        self.last_recv_time.set(0u64);
        self.missed_count_field.set(0u32);
        self.pending_pong.set(false);
        self.timed_out.set(false);
    }

    fn config(&self) -> &HeartbeatConfig {
        &self.config_field
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.1 version=1 rust_sha256=9623c51035abd26d5771427ef7af4a2e7764da6bcf25a3ee8d8ed11c79c166dd*/
struct HeartbeatManager;

struct HeartbeatManager {
    HeartbeatConfig config_field;
    rusty::Cell<uint64_t> last_send_time;
    rusty::Cell<uint64_t> last_recv_time;
    rusty::Cell<uint32_t> missed_count_field;
    rusty::Cell<bool> pending_pong;
    rusty::Cell<bool> timed_out;
    HeartbeatTimeoutCallback on_timeout;

    static HeartbeatManager new_(const HeartbeatConfig& config);
    void set_config(const HeartbeatConfig& config);
    void set_on_timeout(HeartbeatTimeoutCallback callback);
    bool should_send_heartbeat() const;
    void on_heartbeat_sent() const;
    void on_pong_received() const;
    bool check_timeout();
    uint32_t time_until_next_heartbeat_ms() const;
    bool is_timed_out() const;
    uint32_t missed_count() const;
    bool is_pending_pong() const;
    void reset() const;
    const HeartbeatConfig& config() const;
};


HeartbeatManager HeartbeatManager::new_(const HeartbeatConfig& config) {
    return HeartbeatManager{.config_field = rusty::clone(config), .last_send_time = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .last_recv_time = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .missed_count_field = rusty::Cell<uint32_t>::new_(static_cast<uint32_t>(0)), .pending_pong = rusty::Cell<bool>::new_(false), .timed_out = rusty::Cell<bool>::new_(false), .on_timeout = HeartbeatTimeoutCallback{}};
}

void HeartbeatManager::set_config(const HeartbeatConfig& config) {
    this->config_field = rusty::clone(config);
    this->reset();
}

void HeartbeatManager::set_on_timeout(HeartbeatTimeoutCallback callback) {
    this->on_timeout = std::move(callback);
}

bool HeartbeatManager::should_send_heartbeat() const {
    if (!this->config_field.enabled || this->timed_out.get()) {
        return false;
    }
    if (this->pending_pong.get()) {
        return false;
    }
    const uint64_t now = heartbeat_time_us();
    const uint64_t last = this->last_send_time.get();
    const uint64_t interval_us = ((static_cast<uint64_t>(this->config_field.interval_ms))) * static_cast<uint64_t>(1000);
    return ((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(last))) >= rusty::detail::deref_if_pointer_like(interval_us);
}

void HeartbeatManager::on_heartbeat_sent() const {
    if (!this->config_field.enabled) {
        return;
    }
    this->last_send_time.set(heartbeat_time_us());
    this->pending_pong.set(true);
}

void HeartbeatManager::on_pong_received() const {
    if (!this->config_field.enabled) {
        return;
    }
    this->last_recv_time.set(heartbeat_time_us());
    this->pending_pong.set(false);
    this->missed_count_field.set(static_cast<uint32_t>(0));
    this->timed_out.set(false);
}

bool HeartbeatManager::check_timeout() {
    if (!this->config_field.enabled || this->timed_out.get()) {
        return false;
    }
    if (!this->pending_pong.get()) {
        return false;
    }
    const uint64_t now = heartbeat_time_us();
    const uint64_t sent = this->last_send_time.get();
    const uint64_t timeout_us = ((static_cast<uint64_t>(this->config_field.timeout_ms))) * static_cast<uint64_t>(1000);
    if (((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(sent))) >= rusty::detail::deref_if_pointer_like(timeout_us)) {
        this->pending_pong.set(false);
        uint32_t count = this->missed_count_field.get() + static_cast<uint32_t>(1);
        this->missed_count_field.set(std::move(count));
        if (rusty::detail::deref_if_pointer_like(count) >= rusty::detail::deref_if_pointer_like(this->config_field.max_missed)) {
            this->timed_out.set(true);
            if (this->on_timeout) {
                this->on_timeout();
            }
            return true;
        }
    }
    return false;
}

uint32_t HeartbeatManager::time_until_next_heartbeat_ms() const {
    if ((!this->config_field.enabled || this->timed_out.get()) || this->pending_pong.get()) {
        return this->config_field.interval_ms;
    }
    const uint64_t now = heartbeat_time_us();
    const uint64_t last = this->last_send_time.get();
    const uint64_t interval_us = ((static_cast<uint64_t>(this->config_field.interval_ms))) * static_cast<uint64_t>(1000);
    if (((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(last))) >= rusty::detail::deref_if_pointer_like(interval_us)) {
        return static_cast<uint32_t>(0);
    }
    return static_cast<uint32_t>((((rusty::detail::deref_if_pointer_like(interval_us) - ((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(last))))) / static_cast<uint64_t>(1000)));
}

bool HeartbeatManager::is_timed_out() const {
    return this->timed_out.get();
}

uint32_t HeartbeatManager::missed_count() const {
    return this->missed_count_field.get();
}

bool HeartbeatManager::is_pending_pong() const {
    return this->pending_pong.get();
}

void HeartbeatManager::reset() const {
    this->last_send_time.set(static_cast<uint64_t>(0));
    this->last_recv_time.set(static_cast<uint64_t>(0));
    this->missed_count_field.set(static_cast<uint32_t>(0));
    this->pending_pong.set(false);
    this->timed_out.set(false);
}

const HeartbeatConfig& HeartbeatManager::config() const {
    return this->config_field;
}
/*RUSTYCPP:GEN-END id=heartbeat.1*/

} // export namespace rrr
