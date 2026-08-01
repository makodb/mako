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

// Wrapper around rusty::sys::time::clock_monotonic_us. Authored as
// inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the source
// of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. Same shape as
// `current_time_us` (circuit_breaker.cpp) and `queued_request_time_us`
// (request_queue.cpp) — body delegates to the @safe rusty wrapper
// instead of calling `clock_gettime(CLOCK_MONOTONIC)` directly.
#if RUSTYCPP_RUST
fn heartbeat_time_us() -> u64 {
    rusty::sys::time::clock_monotonic_us()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.heartbeat_time_us version=1 rust_sha256=7074d1b727630247c224432d96fd2828d55e7b0e8017939f22176b0e4699428e*/
uint64_t heartbeat_time_us();

uint64_t heartbeat_time_us() {
    return rusty::sys::time::clock_monotonic_us();
}
/*RUSTYCPP:GEN-END id=heartbeat.heartbeat_time_us*/

// Type alias for the heartbeat timeout callback.
//
// Authored as DSL. It used to live outside the DSL block because the
// transpiler could not spell a C++ function-type template argument: the
// literal `rusty::Function<void()>` is not Rust grammar and failed to
// parse, while every Rust spelling silently produced a DIFFERENT type
// (`rusty::Function<std::function<void()>>` and friends, which compile).
// Fixed upstream — `rusty::Function` now takes a bare signature, and
// Rust's `FnMut` (callable through `&mut self`) is the non-const form.
//
// Explicit block id: auto-numbering would collide with the existing
// `heartbeat.1` / `heartbeat.2` blocks (§7.32).
#if RUSTYCPP_RUST
type HeartbeatTimeoutCallback = rusty::Function<dyn FnMut()>;
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.callback_alias version=1 rust_sha256=c6f9dfd0a10d88d96cc6416f69da5a3dac4daf87d320954920686392c165228b*/
using HeartbeatTimeoutCallback = rusty::Function<void()>;
/*RUSTYCPP:GEN-END id=heartbeat.callback_alias*/

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
// a `static HeartbeatConfig new_()` factory (the balanced default).
// Callers use `::aggressive()`, `::relaxed()`, `::disabled()`,
// `::defaults()`, or brace-init. Test sites that previously
// default-constructed `HeartbeatConfig cfg;` move to
// `auto cfg = HeartbeatConfig::defaults();`.
#if RUSTYCPP_RUST
struct HeartbeatConfig {
    enabled: bool,
    interval_ms: u32,
    timeout_ms: u32,
    max_missed: u32,
}

impl HeartbeatConfig {
    fn new() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 10000u32,
            timeout_ms: 5000u32,
            max_missed: 3u32,
        }
    }

    fn defaults() -> HeartbeatConfig {
        HeartbeatConfig::new()
    }

    fn aggressive() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 5000u32,
            timeout_ms: 2000u32,
            max_missed: 2u32,
        }
    }

    fn relaxed() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: true,
            interval_ms: 30000u32,
            timeout_ms: 15000u32,
            max_missed: 5u32,
        }
    }

    fn disabled() -> HeartbeatConfig {
        HeartbeatConfig {
            enabled: false,
            interval_ms: 0u32,
            timeout_ms: 0u32,
            max_missed: 0u32,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.2 version=1 rust_sha256=ce1955104c60643b36350d6c096ba3ed8bd4b54d5049a8c3d16ea914e4d69b49*/
struct HeartbeatConfig;

struct HeartbeatConfig {
    bool enabled;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t max_missed;

    static HeartbeatConfig new_();
    static HeartbeatConfig defaults();
    static HeartbeatConfig aggressive();
    static HeartbeatConfig relaxed();
    static HeartbeatConfig disabled();
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


HeartbeatConfig HeartbeatConfig::new_() {
    return HeartbeatConfig{.enabled = true, .interval_ms = static_cast<uint32_t>(10000), .timeout_ms = static_cast<uint32_t>(5000), .max_missed = static_cast<uint32_t>(3)};
}

HeartbeatConfig HeartbeatConfig::defaults() {
    return HeartbeatConfig::new_();
}

HeartbeatConfig HeartbeatConfig::aggressive() {
    return HeartbeatConfig{.enabled = true, .interval_ms = static_cast<uint32_t>(5000), .timeout_ms = static_cast<uint32_t>(2000), .max_missed = static_cast<uint32_t>(2)};
}

HeartbeatConfig HeartbeatConfig::relaxed() {
    return HeartbeatConfig{.enabled = true, .interval_ms = static_cast<uint32_t>(30000), .timeout_ms = static_cast<uint32_t>(15000), .max_missed = static_cast<uint32_t>(5)};
}

HeartbeatConfig HeartbeatConfig::disabled() {
    return HeartbeatConfig{.enabled = false, .interval_ms = static_cast<uint32_t>(0), .timeout_ms = static_cast<uint32_t>(0), .max_missed = static_cast<uint32_t>(0)};
}
/*RUSTYCPP:GEN-END id=heartbeat.2*/

// `HeartbeatManager` — single-threaded heartbeat tracker. All state is
// interior-mutable: the time/count/flag fields are `rusty::Cell<T>`
// (trivially-copyable), the owned `HeartbeatConfig` value is a
// `rusty::Cell<HeartbeatConfig>` (it is Copy/POD; replaced by
// `set_config()`), and the `HeartbeatTimeoutCallback` (a
// `rusty::Function<void()>`, non-Copy) is a
// `rusty::RefCell<HeartbeatTimeoutCallback>` (replaced by
// `set_on_timeout()`).
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The plain `fn new(config)`
// lowers to a `static HeartbeatManager new_(const HeartbeatConfig&)`
// factory; callers construct via the factory rather than direct ctor
// syntax.
//
// Behavioral diffs from the original C++ class:
//   * Every method is now `const` (`&self`). All bodies still mutate
//     state, only through the interior-mutable Cell/RefCell fields.
//     Callers that held a non-const ref keep working; a const ref /
//     shared `Arc` can now invoke the setters and `check_timeout()`
//     without `const_cast`.
//   * `check_timeout()` (formerly non-const) is `const`: it mutates
//     Cells AND fires the `on_timeout_` callback (a
//     `rusty::Function<void()>` whose `operator()` is non-const) via
//     `on_timeout_.borrow_mut()`.
//   * `set_config()` / `set_on_timeout()` (formerly non-const) are
//     `const`: they store through `config_field.set()` /
//     `on_timeout.replace()`.
//   * `config()` returns `HeartbeatConfig` by value (`Cell` exposes no
//     stable `&T`); the original by-ref getter is gone. All callers
//     already consume the result by value.
//   * Fields are no longer marked `private`. No callers reach into
//     them.
#if RUSTYCPP_RUST
struct HeartbeatManager {
    config_field: Cell<HeartbeatConfig>,
    last_send_time: Cell<u64>,
    last_recv_time: Cell<u64>,
    missed_count_field: Cell<u32>,
    pending_pong: Cell<bool>,
    timed_out: Cell<bool>,
    on_timeout: RefCell<HeartbeatTimeoutCallback>,
}

impl HeartbeatManager {
    fn new(config: &HeartbeatConfig) -> HeartbeatManager {
        HeartbeatManager {
            config_field: Cell::<HeartbeatConfig>::new(config.clone()),
            last_send_time: Cell::<u64>::new(0u64),
            last_recv_time: Cell::<u64>::new(0u64),
            missed_count_field: Cell::<u32>::new(0u32),
            pending_pong: Cell::<bool>::new(false),
            timed_out: Cell::<bool>::new(false),
            on_timeout: RefCell::<HeartbeatTimeoutCallback>::new(HeartbeatTimeoutCallback {}),
        }
    }

    fn set_config(&self, config: &HeartbeatConfig) {
        self.config_field.set(config.clone());
        self.reset();
    }

    fn set_on_timeout(&self, callback: HeartbeatTimeoutCallback) {
        self.on_timeout.replace(callback);
    }

    fn should_send_heartbeat(&self) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if self.pending_pong.get() {
            return false;
        }

        let now: u64 = heartbeat_time_us();
        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.get().interval_ms as u64) * 1000u64;

        (now - last) >= interval_us
    }

    fn on_heartbeat_sent(&self) {
        if !self.config_field.get().enabled {
            return;
        }
        self.last_send_time.set(heartbeat_time_us());
        self.pending_pong.set(true);
    }

    fn on_pong_received(&self) {
        if !self.config_field.get().enabled {
            return;
        }
        self.last_recv_time.set(heartbeat_time_us());
        self.pending_pong.set(false);
        self.missed_count_field.set(0u32);
        self.timed_out.set(false);
    }

    fn check_timeout(&self) -> bool {
        if !self.config_field.get().enabled || self.timed_out.get() {
            return false;
        }
        if !self.pending_pong.get() {
            return false;
        }

        let now: u64 = heartbeat_time_us();
        let sent: u64 = self.last_send_time.get();
        let timeout_us: u64 = (self.config_field.get().timeout_ms as u64) * 1000u64;

        if (now - sent) >= timeout_us {
            self.pending_pong.set(false);
            let count: u32 = self.missed_count_field.get() + 1u32;
            self.missed_count_field.set(count);

            if count >= self.config_field.get().max_missed {
                self.timed_out.set(true);
                let cb = self.on_timeout.borrow_mut();
                if *cb {
                    (*cb)();
                }
                return true;
            }
        }
        false
    }

    fn time_until_next_heartbeat_ms(&self) -> u32 {
        if !self.config_field.get().enabled
            || self.timed_out.get()
            || self.pending_pong.get()
        {
            return self.config_field.get().interval_ms;
        }

        let now: u64 = heartbeat_time_us();
        let last: u64 = self.last_send_time.get();
        let interval_us: u64 = (self.config_field.get().interval_ms as u64) * 1000u64;

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

    fn config(&self) -> HeartbeatConfig {
        self.config_field.get()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=heartbeat.1 version=1 rust_sha256=a0efbc96874bea7d1dbf00902da57fe346a72c7ed0508358c3d536a421e89bfd*/
struct HeartbeatManager;

struct HeartbeatManager {
    rusty::Cell<HeartbeatConfig> config_field;
    rusty::Cell<uint64_t> last_send_time;
    rusty::Cell<uint64_t> last_recv_time;
    rusty::Cell<uint32_t> missed_count_field;
    rusty::Cell<bool> pending_pong;
    rusty::Cell<bool> timed_out;
    rusty::RefCell<HeartbeatTimeoutCallback> on_timeout;

    static HeartbeatManager new_(const HeartbeatConfig& config);
    void set_config(const HeartbeatConfig& config) const;
    void set_on_timeout(HeartbeatTimeoutCallback callback) const;
    bool should_send_heartbeat() const;
    void on_heartbeat_sent() const;
    void on_pong_received() const;
    bool check_timeout() const;
    uint32_t time_until_next_heartbeat_ms() const;
    bool is_timed_out() const;
    uint32_t missed_count() const;
    bool is_pending_pong() const;
    void reset() const;
    HeartbeatConfig config() const;
};


HeartbeatManager HeartbeatManager::new_(const HeartbeatConfig& config) {
    return HeartbeatManager{.config_field = rusty::Cell<HeartbeatConfig>::new_(rusty::clone(config)), .last_send_time = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .last_recv_time = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .missed_count_field = rusty::Cell<uint32_t>::new_(static_cast<uint32_t>(0)), .pending_pong = rusty::Cell<bool>::new_(false), .timed_out = rusty::Cell<bool>::new_(false), .on_timeout = rusty::RefCell<HeartbeatTimeoutCallback>::new_(HeartbeatTimeoutCallback{})};
}

void HeartbeatManager::set_config(const HeartbeatConfig& config) const {
    this->config_field.set(rusty::clone(config));
    this->reset();
}

void HeartbeatManager::set_on_timeout(HeartbeatTimeoutCallback callback) const {
    this->on_timeout.replace(std::move(callback));
}

bool HeartbeatManager::should_send_heartbeat() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled) || this->timed_out.get()) {
        return false;
    }
    if (this->pending_pong.get()) {
        return false;
    }
    const uint64_t now = heartbeat_time_us();
    const uint64_t last = this->last_send_time.get();
    const uint64_t interval_us = ((static_cast<uint64_t>(this->config_field.get().interval_ms))) * static_cast<uint64_t>(1000);
    return ((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(last))) >= rusty::detail::deref_if_pointer_like(interval_us);
}

void HeartbeatManager::on_heartbeat_sent() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled)) {
        return;
    }
    this->last_send_time.set(heartbeat_time_us());
    this->pending_pong.set(true);
}

void HeartbeatManager::on_pong_received() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled)) {
        return;
    }
    this->last_recv_time.set(heartbeat_time_us());
    this->pending_pong.set(false);
    this->missed_count_field.set(static_cast<uint32_t>(0));
    this->timed_out.set(false);
}

bool HeartbeatManager::check_timeout() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled) || this->timed_out.get()) {
        return false;
    }
    if (rusty::detail::rust_not(this->pending_pong.get())) {
        return false;
    }
    const uint64_t now = heartbeat_time_us();
    const uint64_t sent = this->last_send_time.get();
    const uint64_t timeout_us = ((static_cast<uint64_t>(this->config_field.get().timeout_ms))) * static_cast<uint64_t>(1000);
    if (((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(sent))) >= rusty::detail::deref_if_pointer_like(timeout_us)) {
        this->pending_pong.set(false);
        uint32_t count = this->missed_count_field.get() + static_cast<uint32_t>(1);
        this->missed_count_field.set(std::move(count));
        if (rusty::detail::deref_if_pointer_like(count) >= rusty::detail::deref_if_pointer_like(this->config_field.get().max_missed)) {
            this->timed_out.set(true);
            auto cb = this->on_timeout.borrow_mut();
            if (*cb) {
                (*cb)();
            }
            return true;
        }
    }
    return false;
}

uint32_t HeartbeatManager::time_until_next_heartbeat_ms() const {
    if ((rusty::detail::rust_not(this->config_field.get().enabled) || this->timed_out.get()) || this->pending_pong.get()) {
        return this->config_field.get().interval_ms;
    }
    const uint64_t now = heartbeat_time_us();
    const uint64_t last = this->last_send_time.get();
    const uint64_t interval_us = ((static_cast<uint64_t>(this->config_field.get().interval_ms))) * static_cast<uint64_t>(1000);
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

HeartbeatConfig HeartbeatManager::config() const {
    return this->config_field.get();
}
/*RUSTYCPP:GEN-END id=heartbeat.1*/

} // export namespace rrr
