module;

#include <rusty/cell.hpp>
#include <rusty/move.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>
#include <cstdint>
#include <time.h>

export module rrr.circuit_breaker;

import std;

export namespace rrr {

// Wrapper around rusty::sys::time::clock_monotonic_us. Authored as
// inline Rust DSL: the `#if RUSTYCPP_RUST` block below is the source
// of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block.
//
// Previously called `clock_gettime(CLOCK_MONOTONIC)` directly — a raw
// libc syscall the DSL doesn't model. Now delegates to
// `rusty::sys::time::clock_monotonic_us`, the @safe rusty wrapper
// that already underpins `queued_request_time_us` in request_queue.cpp.
#if RUSTYCPP_RUST
fn current_time_us() -> u64 {
    rusty::sys::time::clock_monotonic_us()
}
#endif
/*RUSTYCPP:GEN-BEGIN id=circuit_breaker.current_time_us version=1 rust_sha256=59d1205c379f1e9809a003db13d7fab7a67a858c4a53d9a7b77085ae984b22a0*/
uint64_t current_time_us();

uint64_t current_time_us() {
    return rusty::sys::time::clock_monotonic_us();
}
/*RUSTYCPP:GEN-END id=circuit_breaker.current_time_us*/

// `CircuitState` — circuit-breaker FSM state. Authored as inline Rust
// DSL: the `#if RUSTYCPP_RUST` block below is the source of truth;
// the transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block.
#if RUSTYCPP_RUST
#[repr(i32)]
enum CircuitState {
    CLOSED = 0,
    OPEN = 1,
    HALF_OPEN = 2,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=circuit_breaker.circuit_state version=1 rust_sha256=07bc3717701d61ea9ba1c278fe0410c40e6b2eb99468f298e2449462497a3489*/
enum class CircuitState;
constexpr CircuitState CircuitState_CLOSED();
constexpr CircuitState CircuitState_OPEN();
constexpr CircuitState CircuitState_HALF_OPEN();

enum class CircuitState {
    CLOSED = 0,
    OPEN = 1,
    HALF_OPEN = 2
};
inline constexpr CircuitState CircuitState_CLOSED() { return CircuitState::CLOSED; }
inline constexpr CircuitState CircuitState_OPEN() { return CircuitState::OPEN; }
inline constexpr CircuitState CircuitState_HALF_OPEN() { return CircuitState::HALF_OPEN; }
/*RUSTYCPP:GEN-END id=circuit_breaker.circuit_state*/

inline const char* circuit_state_to_string(CircuitState state) {
    switch (state) {
        case CircuitState::CLOSED: return "CLOSED";
        case CircuitState::OPEN: return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF_OPEN";
        default: return "UNKNOWN";
    }
}

// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block. The plain `fn new()` lowers to
// a `static CircuitBreakerConfig new_()` factory (the balanced
// default). Callers use `::sensitive()`, `::relaxed()`,
// `::disabled()`, `::defaults()`, or brace-init. Test sites that
// previously default-constructed `CircuitBreakerConfig cfg;` move to
// `auto cfg = CircuitBreakerConfig::defaults();` (same pattern as the
// other migrated config structs).
#if RUSTYCPP_RUST
struct CircuitBreakerConfig {
    failure_threshold: u32,
    success_threshold: u32,
    timeout_ms: u32,
    enabled: bool,
}

impl CircuitBreakerConfig {
    fn new() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 5u32,
            success_threshold: 3u32,
            timeout_ms: 30000u32,
            enabled: true,
        }
    }

    fn defaults() -> CircuitBreakerConfig {
        CircuitBreakerConfig::new()
    }

    fn sensitive() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 3u32,
            success_threshold: 5u32,
            timeout_ms: 60000u32,
            enabled: true,
        }
    }

    fn relaxed() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 10u32,
            success_threshold: 2u32,
            timeout_ms: 15000u32,
            enabled: true,
        }
    }

    fn disabled() -> CircuitBreakerConfig {
        CircuitBreakerConfig {
            failure_threshold: 0u32,
            success_threshold: 0u32,
            timeout_ms: 0u32,
            enabled: false,
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=circuit_breaker.2 version=1 rust_sha256=9e16f13820a674c5f66baf8335b2f4c47090932b2adf693aacb347c3b5e93c6f*/
struct CircuitBreakerConfig;

struct CircuitBreakerConfig {
    uint32_t failure_threshold;
    uint32_t success_threshold;
    uint32_t timeout_ms;
    bool enabled;

    static CircuitBreakerConfig new_();
    static CircuitBreakerConfig defaults();
    static CircuitBreakerConfig sensitive();
    static CircuitBreakerConfig relaxed();
    static CircuitBreakerConfig disabled();
};


CircuitBreakerConfig CircuitBreakerConfig::new_() {
    return CircuitBreakerConfig{.failure_threshold = static_cast<uint32_t>(5), .success_threshold = static_cast<uint32_t>(3), .timeout_ms = static_cast<uint32_t>(30000), .enabled = true};
}

CircuitBreakerConfig CircuitBreakerConfig::defaults() {
    return CircuitBreakerConfig::new_();
}

CircuitBreakerConfig CircuitBreakerConfig::sensitive() {
    return CircuitBreakerConfig{.failure_threshold = static_cast<uint32_t>(3), .success_threshold = static_cast<uint32_t>(5), .timeout_ms = static_cast<uint32_t>(60000), .enabled = true};
}

CircuitBreakerConfig CircuitBreakerConfig::relaxed() {
    return CircuitBreakerConfig{.failure_threshold = static_cast<uint32_t>(10), .success_threshold = static_cast<uint32_t>(2), .timeout_ms = static_cast<uint32_t>(15000), .enabled = true};
}

CircuitBreakerConfig CircuitBreakerConfig::disabled() {
    return CircuitBreakerConfig{.failure_threshold = static_cast<uint32_t>(0), .success_threshold = static_cast<uint32_t>(0), .timeout_ms = static_cast<uint32_t>(0), .enabled = false};
}
/*RUSTYCPP:GEN-END id=circuit_breaker.2*/

// `CircuitBreaker` — single-threaded state machine that tracks
// success/failure counts and a Cell<CircuitState>. All mutable state
// is `rusty::Cell<T>` for trivially-copyable interior mutability,
// including the `CircuitBreakerConfig` value (POD: three u32 + bool),
// which `set_config()` replaces via `Cell::set`.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The plain `fn new(config)`
// lowers to a `static CircuitBreaker new_(CircuitBreakerConfig)`
// factory; callers construct via the factory rather than direct ctor
// syntax.
//
// Behavioral diffs from the original C++ class:
//   * Methods that previously were non-const but only touched Cell
//     fields (`record_success`, `record_failure`, `reset`,
//     `set_config`) are now `const`. The body still mutates state,
//     only through the Cells. Callers that held a non-const ref keep
//     working, and the breaker is now drivable through a shared/const
//     ref without const_cast.
//   * The `= delete` copy/move ctor + assignment declarations are
//     dropped; the DSL does not yet emit special-member-function
//     annotations. The class is no longer move/copy-suppressed.
//     Acceptable here: every call site holds a CircuitBreaker by
//     value or by `mutable` member, none clone it or move it.
//   * Fields are no longer marked `private`. No callers reach into
//     them.
#if RUSTYCPP_RUST
struct CircuitBreaker {
    config_field: Cell<CircuitBreakerConfig>,
    state_field: Cell<CircuitState>,
    failure_count_field: Cell<u32>,
    success_count_field: Cell<u32>,
    last_failure_time: Cell<u64>,
    probe_in_progress: Cell<bool>,
}

impl CircuitBreaker {
    fn new(config: CircuitBreakerConfig) -> CircuitBreaker {
        CircuitBreaker {
            config_field: Cell::<CircuitBreakerConfig>::new(config),
            state_field: Cell::<CircuitState>::new(CircuitState::CLOSED),
            failure_count_field: Cell::<u32>::new(0u32),
            success_count_field: Cell::<u32>::new(0u32),
            last_failure_time: Cell::<u64>::new(0u64),
            probe_in_progress: Cell::<bool>::new(false),
        }
    }

    fn set_config(&self, config: CircuitBreakerConfig) {
        self.config_field.set(config);
        self.reset();
    }

    fn allow_request(&self) -> bool {
        if !self.config_field.get().enabled {
            return true;
        }

        let current: CircuitState = self.state_field.get();

        if (current as i32) == (CircuitState::CLOSED as i32) {
            return true;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            let now: u64 = current_time_us();
            let last: u64 = self.last_failure_time.get();
            let timeout_us: u64 = (self.config_field.get().timeout_ms as u64) * 1000u64;

            if now - last >= timeout_us {
                let next: CircuitState = CircuitState::HALF_OPEN;
                self.state_field.set(next);
                self.probe_in_progress.set(true);
                return true;
            }
            return false;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            if !self.probe_in_progress.get() {
                self.probe_in_progress.set(true);
                return true;
            }
            return false;
        }
        false
    }

    fn record_success(&self) {
        if !self.config_field.get().enabled {
            return;
        }

        let current: CircuitState = self.state_field.get();

        if (current as i32) == (CircuitState::CLOSED as i32) {
            self.failure_count_field.set(0u32);
            return;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            self.probe_in_progress.set(false);
            let count: u32 = self.success_count_field.get() + 1u32;
            self.success_count_field.set(count);

            if count >= self.config_field.get().success_threshold {
                let closed: CircuitState = CircuitState::CLOSED;
                self.state_field.set(closed);
                self.failure_count_field.set(0u32);
                self.success_count_field.set(0u32);
            }
            return;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            self.probe_in_progress.set(false);
        }
    }

    fn record_failure(&self) {
        if !self.config_field.get().enabled {
            return;
        }

        let current: CircuitState = self.state_field.get();

        if (current as i32) == (CircuitState::CLOSED as i32) {
            let count: u32 = self.failure_count_field.get() + 1u32;
            self.failure_count_field.set(count);

            if count >= self.config_field.get().failure_threshold {
                let open: CircuitState = CircuitState::OPEN;
                self.state_field.set(open);
                self.last_failure_time.set(current_time_us());
                self.failure_count_field.set(0u32);
                self.success_count_field.set(0u32);
            }
            return;
        }
        if (current as i32) == (CircuitState::HALF_OPEN as i32) {
            self.probe_in_progress.set(false);
            let open: CircuitState = CircuitState::OPEN;
            self.state_field.set(open);
            self.last_failure_time.set(current_time_us());
            self.success_count_field.set(0u32);
            return;
        }
        if (current as i32) == (CircuitState::OPEN as i32) {
            self.last_failure_time.set(current_time_us());
        }
    }

    fn state(&self) -> CircuitState {
        self.state_field.get()
    }

    fn is_open(&self) -> bool {
        (self.state_field.get() as i32) == (CircuitState::OPEN as i32)
    }

    fn is_closed(&self) -> bool {
        (self.state_field.get() as i32) == (CircuitState::CLOSED as i32)
    }

    fn is_half_open(&self) -> bool {
        (self.state_field.get() as i32) == (CircuitState::HALF_OPEN as i32)
    }

    fn reset(&self) {
        let closed: CircuitState = CircuitState::CLOSED;
        self.state_field.set(closed);
        self.failure_count_field.set(0u32);
        self.success_count_field.set(0u32);
        self.last_failure_time.set(0u64);
        self.probe_in_progress.set(false);
    }

    fn failure_count(&self) -> u32 {
        self.failure_count_field.get()
    }

    fn success_count(&self) -> u32 {
        self.success_count_field.get()
    }

    fn config(&self) -> CircuitBreakerConfig {
        self.config_field.get()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=circuit_breaker.1 version=1 rust_sha256=460bc3032dab24cb7b5682072e142f93a8779bd214b66cfea4de0e0abbff701f*/
struct CircuitBreaker;

struct CircuitBreaker {
    rusty::Cell<CircuitBreakerConfig> config_field;
    rusty::Cell<CircuitState> state_field;
    rusty::Cell<uint32_t> failure_count_field;
    rusty::Cell<uint32_t> success_count_field;
    rusty::Cell<uint64_t> last_failure_time;
    rusty::Cell<bool> probe_in_progress;

    static CircuitBreaker new_(CircuitBreakerConfig config);
    void set_config(CircuitBreakerConfig config) const;
    bool allow_request() const;
    void record_success() const;
    void record_failure() const;
    CircuitState state() const;
    bool is_open() const;
    bool is_closed() const;
    bool is_half_open() const;
    void reset() const;
    uint32_t failure_count() const;
    uint32_t success_count() const;
    CircuitBreakerConfig config() const;
};


CircuitBreaker CircuitBreaker::new_(CircuitBreakerConfig config) {
    return CircuitBreaker{.config_field = rusty::Cell<CircuitBreakerConfig>::new_(std::move(config)), .state_field = rusty::Cell<CircuitState>::new_(rusty::clone(rusty::clone(CircuitState::CLOSED))), .failure_count_field = rusty::Cell<uint32_t>::new_(static_cast<uint32_t>(0)), .success_count_field = rusty::Cell<uint32_t>::new_(static_cast<uint32_t>(0)), .last_failure_time = rusty::Cell<uint64_t>::new_(static_cast<uint64_t>(0)), .probe_in_progress = rusty::Cell<bool>::new_(false)};
}

void CircuitBreaker::set_config(CircuitBreakerConfig config) const {
    this->config_field.set(std::move(config));
    this->reset();
}

bool CircuitBreaker::allow_request() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled)) {
        return true;
    }
    const CircuitState current = this->state_field.get();
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::CLOSED)))) {
        return true;
    }
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::OPEN)))) {
        const uint64_t now = current_time_us();
        const uint64_t last = this->last_failure_time.get();
        const uint64_t timeout_us = ((static_cast<uint64_t>(this->config_field.get().timeout_ms))) * static_cast<uint64_t>(1000);
        if ((rusty::detail::deref_if_pointer_like(now) - rusty::detail::deref_if_pointer_like(last)) >= rusty::detail::deref_if_pointer_like(timeout_us)) {
            CircuitState next = rusty::clone(CircuitState::HALF_OPEN);
            this->state_field.set(std::move(next));
            this->probe_in_progress.set(true);
            return true;
        }
        return false;
    }
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::HALF_OPEN)))) {
        if (rusty::detail::rust_not(this->probe_in_progress.get())) {
            this->probe_in_progress.set(true);
            return true;
        }
        return false;
    }
    return false;
}

void CircuitBreaker::record_success() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled)) {
        return;
    }
    const CircuitState current = this->state_field.get();
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::CLOSED)))) {
        this->failure_count_field.set(static_cast<uint32_t>(0));
        return;
    }
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::HALF_OPEN)))) {
        this->probe_in_progress.set(false);
        uint32_t count = this->success_count_field.get() + static_cast<uint32_t>(1);
        this->success_count_field.set(std::move(count));
        if (rusty::detail::deref_if_pointer_like(count) >= rusty::detail::deref_if_pointer_like(this->config_field.get().success_threshold)) {
            CircuitState closed = rusty::clone(CircuitState::CLOSED);
            this->state_field.set(std::move(closed));
            this->failure_count_field.set(static_cast<uint32_t>(0));
            this->success_count_field.set(static_cast<uint32_t>(0));
        }
        return;
    }
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::OPEN)))) {
        this->probe_in_progress.set(false);
    }
}

void CircuitBreaker::record_failure() const {
    if (rusty::detail::rust_not(this->config_field.get().enabled)) {
        return;
    }
    const CircuitState current = this->state_field.get();
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::CLOSED)))) {
        uint32_t count = this->failure_count_field.get() + static_cast<uint32_t>(1);
        this->failure_count_field.set(std::move(count));
        if (rusty::detail::deref_if_pointer_like(count) >= rusty::detail::deref_if_pointer_like(this->config_field.get().failure_threshold)) {
            CircuitState open = rusty::clone(CircuitState::OPEN);
            this->state_field.set(std::move(open));
            this->last_failure_time.set(current_time_us());
            this->failure_count_field.set(static_cast<uint32_t>(0));
            this->success_count_field.set(static_cast<uint32_t>(0));
        }
        return;
    }
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::HALF_OPEN)))) {
        this->probe_in_progress.set(false);
        CircuitState open = rusty::clone(CircuitState::OPEN);
        this->state_field.set(std::move(open));
        this->last_failure_time.set(current_time_us());
        this->success_count_field.set(static_cast<uint32_t>(0));
        return;
    }
    if (((static_cast<int32_t>(current))) == ((static_cast<int32_t>(CircuitState::OPEN)))) {
        this->last_failure_time.set(current_time_us());
    }
}

CircuitState CircuitBreaker::state() const {
    return this->state_field.get();
}

bool CircuitBreaker::is_open() const {
    return ((static_cast<int32_t>(this->state_field.get()))) == ((static_cast<int32_t>(CircuitState::OPEN)));
}

bool CircuitBreaker::is_closed() const {
    return ((static_cast<int32_t>(this->state_field.get()))) == ((static_cast<int32_t>(CircuitState::CLOSED)));
}

bool CircuitBreaker::is_half_open() const {
    return ((static_cast<int32_t>(this->state_field.get()))) == ((static_cast<int32_t>(CircuitState::HALF_OPEN)));
}

void CircuitBreaker::reset() const {
    CircuitState closed = rusty::clone(CircuitState::CLOSED);
    this->state_field.set(std::move(closed));
    this->failure_count_field.set(static_cast<uint32_t>(0));
    this->success_count_field.set(static_cast<uint32_t>(0));
    this->last_failure_time.set(static_cast<uint64_t>(0));
    this->probe_in_progress.set(false);
}

uint32_t CircuitBreaker::failure_count() const {
    return this->failure_count_field.get();
}

uint32_t CircuitBreaker::success_count() const {
    return this->success_count_field.get();
}

CircuitBreakerConfig CircuitBreaker::config() const {
    return this->config_field.get();
}
/*RUSTYCPP:GEN-END id=circuit_breaker.1*/

} // export namespace rrr
