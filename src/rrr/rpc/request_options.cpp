module;

#include <cstdint>

#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>

export module rrr.request_options;

import std;
import rrr.rand;

// @safe - POD options struct + TimeoutType enum + factory helpers
// + simple jitter computation. No raw pointers, syscalls, or
// operator-overload chains.
export namespace rrr {

// `TimeoutType` — categorical tag for which timeout fired in a
// request lifecycle. Authored as inline Rust DSL: the
// `#if RUSTYCPP_RUST` block below is the source of truth; the
// transpiler regenerates the matching `RUSTYCPP:GEN-BEGIN ... END`
// block with the C++ `enum class : uint8_t`.
#if RUSTYCPP_RUST
#[repr(u8)]
enum TimeoutType {
    NONE = 0,
    CONNECT_TIMEOUT,
    REQUEST_TIMEOUT,
    RESPONSE_TIMEOUT,
    TOTAL_TIMEOUT,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_options.timeout_type version=1 rust_sha256=ee966c5adf41577267e2ffd0f3b6ddaf04f80abb05651cb07d588f30cfe6cfac*/
enum class TimeoutType;
constexpr TimeoutType TimeoutType_NONE();
constexpr TimeoutType TimeoutType_CONNECT_TIMEOUT();
constexpr TimeoutType TimeoutType_REQUEST_TIMEOUT();
constexpr TimeoutType TimeoutType_RESPONSE_TIMEOUT();
constexpr TimeoutType TimeoutType_TOTAL_TIMEOUT();

enum class TimeoutType {
    NONE = 0,
    CONNECT_TIMEOUT,
    REQUEST_TIMEOUT,
    RESPONSE_TIMEOUT,
    TOTAL_TIMEOUT
};
inline constexpr TimeoutType TimeoutType_NONE() { return TimeoutType::NONE; }
inline constexpr TimeoutType TimeoutType_CONNECT_TIMEOUT() { return TimeoutType::CONNECT_TIMEOUT; }
inline constexpr TimeoutType TimeoutType_REQUEST_TIMEOUT() { return TimeoutType::REQUEST_TIMEOUT; }
inline constexpr TimeoutType TimeoutType_RESPONSE_TIMEOUT() { return TimeoutType::RESPONSE_TIMEOUT; }
inline constexpr TimeoutType TimeoutType_TOTAL_TIMEOUT() { return TimeoutType::TOTAL_TIMEOUT; }
/*RUSTYCPP:GEN-END id=request_options.timeout_type*/

// `RequestOptions` — per-request timeout + retry config POD plus a
// handful of preset factories and a stateless `calculate_delay_ms`
// jitter/backoff helper.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The plain `fn new()`
// lowers to a `static RequestOptions new_()` factory; the original
// `defaults()` preset stays under the same C++ name (forwarding to
// `::new_()`).
//
// Behavioral diffs from the pre-DSL form:
//   * No more in-class `= 1000;` style default-field initializers.
//     Callers that wrote `RequestOptions opts;` must switch to
//     `auto opts = RequestOptions::defaults();` (or any other preset).
//   * Static factories drop their default-arg forms
//     (`with_retry(max_retries, timeout_ms = 1000)` becomes
//     `with_retry(max_retries, timeout_ms)`, `idempotent_retry(=3)`
//     becomes `idempotent_retry(max_retries)`). The defaults are
//     baked into the only known caller paths or repeated explicitly
//     at each site.
//   * `static_cast<uint64_t>(delay)` (which is UB for negative
//     doubles in standard C++) becomes the Rust `as u64` lowering,
//     which the transpiler emits as `static_cast<uint64_t>(delay)`
//     too — but the body still guards `delay < 0.0` before reaching
//     it, so the conversion only ever sees a non-negative value.
#if RUSTYCPP_RUST
struct RequestOptions {
    timeout_ms: u64,
    total_timeout_ms: u64,
    max_retries: u16,
    base_delay_ms: u16,
    max_delay_ms: u16,
    jitter_factor: f32,
    idempotent: bool,
}

impl RequestOptions {
    fn new() -> RequestOptions {
        RequestOptions {
            timeout_ms: 1000u64,
            total_timeout_ms: 0u64,
            max_retries: 0u16,
            base_delay_ms: 50u16,
            max_delay_ms: 5000u16,
            jitter_factor: 0.1f32,
            idempotent: false,
        }
    }

    fn defaults() -> RequestOptions {
        RequestOptions::new()
    }

    fn with_retry(max_retries: u16, timeout_ms: u64) -> RequestOptions {
        RequestOptions {
            timeout_ms: timeout_ms,
            total_timeout_ms: 0u64,
            max_retries: max_retries,
            base_delay_ms: 50u16,
            max_delay_ms: 5000u16,
            jitter_factor: 0.1f32,
            idempotent: true,
        }
    }

    fn idempotent_retry(max_retries: u16) -> RequestOptions {
        RequestOptions {
            timeout_ms: 1000u64,
            total_timeout_ms: 0u64,
            max_retries: max_retries,
            base_delay_ms: 50u16,
            max_delay_ms: 5000u16,
            jitter_factor: 0.1f32,
            idempotent: true,
        }
    }

    fn no_timeout() -> RequestOptions {
        RequestOptions {
            timeout_ms: 0u64,
            total_timeout_ms: 0u64,
            max_retries: 0u16,
            base_delay_ms: 50u16,
            max_delay_ms: 5000u16,
            jitter_factor: 0.1f32,
            idempotent: false,
        }
    }

    fn fast() -> RequestOptions {
        RequestOptions {
            timeout_ms: 100u64,
            total_timeout_ms: 0u64,
            max_retries: 2u16,
            base_delay_ms: 10u16,
            max_delay_ms: 100u16,
            jitter_factor: 0.1f32,
            idempotent: true,
        }
    }

    fn patient() -> RequestOptions {
        RequestOptions {
            timeout_ms: 10000u64,
            total_timeout_ms: 60000u64,
            max_retries: 5u16,
            base_delay_ms: 500u16,
            max_delay_ms: 10000u16,
            jitter_factor: 0.1f32,
            idempotent: true,
        }
    }

    fn can_retry(&self, current_retry_count: u16) -> bool {
        self.idempotent && current_retry_count < self.max_retries
    }

    fn calculate_delay_ms(&self, attempt: u16) -> u64 {
        let mut delay: f64 = self.base_delay_ms as f64;
        let mut i: u16 = 0u16;
        while i < attempt {
            delay *= 2.0f64;
            if delay > (self.max_delay_ms as f64) {
                delay = self.max_delay_ms as f64;
                break;
            }
            i += 1u16;
        }

        if delay > (self.max_delay_ms as f64) {
            delay = self.max_delay_ms as f64;
        }

        if self.jitter_factor > 0.0f32 {
            let jitter: f64 = delay * (self.jitter_factor as f64) *
                              RandomGenerator::rand_double(-0.5f64, 0.5f64);
            delay += jitter;

            if delay < 0.0f64 {
                delay = 0.0f64;
            }
        }

        delay as u64
    }

    fn is_total_timeout_exceeded(&self, elapsed_ms: u64) -> bool {
        self.total_timeout_ms > 0u64 && elapsed_ms >= self.total_timeout_ms
    }

    fn remaining_time_ms(&self, elapsed_ms: u64) -> u64 {
        if self.total_timeout_ms == 0u64 {
            return u64::MAX;
        }
        if elapsed_ms >= self.total_timeout_ms {
            return 0u64;
        }
        self.total_timeout_ms - elapsed_ms
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_options.0 version=1 rust_sha256=0623c5692e9125fe0d56fba3ef332ada7bf338dd6c196ba13b8bfbb61bfa18ea*/
struct RequestOptions;

struct RequestOptions {
    uint64_t timeout_ms;
    uint64_t total_timeout_ms;
    uint16_t max_retries;
    uint16_t base_delay_ms;
    uint16_t max_delay_ms;
    float jitter_factor;
    bool idempotent;

    static RequestOptions new_();
    static RequestOptions defaults();
    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms);
    static RequestOptions idempotent_retry(uint16_t max_retries);
    static RequestOptions no_timeout();
    static RequestOptions fast();
    static RequestOptions patient();
    bool can_retry(uint16_t current_retry_count) const;
    uint64_t calculate_delay_ms(uint16_t attempt) const;
    bool is_total_timeout_exceeded(uint64_t elapsed_ms) const;
    uint64_t remaining_time_ms(uint64_t elapsed_ms) const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


RequestOptions RequestOptions::new_() {
    return RequestOptions{.timeout_ms = static_cast<uint64_t>(1000), .total_timeout_ms = static_cast<uint64_t>(0), .max_retries = static_cast<uint16_t>(0), .base_delay_ms = static_cast<uint16_t>(50), .max_delay_ms = static_cast<uint16_t>(5000), .jitter_factor = 0.1f, .idempotent = false};
}

RequestOptions RequestOptions::defaults() {
    return RequestOptions::new_();
}

RequestOptions RequestOptions::with_retry(uint16_t max_retries, uint64_t timeout_ms) {
    return RequestOptions{.timeout_ms = std::move(timeout_ms), .total_timeout_ms = static_cast<uint64_t>(0), .max_retries = std::move(max_retries), .base_delay_ms = static_cast<uint16_t>(50), .max_delay_ms = static_cast<uint16_t>(5000), .jitter_factor = 0.1f, .idempotent = true};
}

RequestOptions RequestOptions::idempotent_retry(uint16_t max_retries) {
    return RequestOptions{.timeout_ms = static_cast<uint64_t>(1000), .total_timeout_ms = static_cast<uint64_t>(0), .max_retries = std::move(max_retries), .base_delay_ms = static_cast<uint16_t>(50), .max_delay_ms = static_cast<uint16_t>(5000), .jitter_factor = 0.1f, .idempotent = true};
}

RequestOptions RequestOptions::no_timeout() {
    return RequestOptions{.timeout_ms = static_cast<uint64_t>(0), .total_timeout_ms = static_cast<uint64_t>(0), .max_retries = static_cast<uint16_t>(0), .base_delay_ms = static_cast<uint16_t>(50), .max_delay_ms = static_cast<uint16_t>(5000), .jitter_factor = 0.1f, .idempotent = false};
}

RequestOptions RequestOptions::fast() {
    return RequestOptions{.timeout_ms = static_cast<uint64_t>(100), .total_timeout_ms = static_cast<uint64_t>(0), .max_retries = static_cast<uint16_t>(2), .base_delay_ms = static_cast<uint16_t>(10), .max_delay_ms = static_cast<uint16_t>(100), .jitter_factor = 0.1f, .idempotent = true};
}

RequestOptions RequestOptions::patient() {
    return RequestOptions{.timeout_ms = static_cast<uint64_t>(10000), .total_timeout_ms = static_cast<uint64_t>(60000), .max_retries = static_cast<uint16_t>(5), .base_delay_ms = static_cast<uint16_t>(500), .max_delay_ms = static_cast<uint16_t>(10000), .jitter_factor = 0.1f, .idempotent = true};
}

bool RequestOptions::can_retry(uint16_t current_retry_count) const {
    return rusty::detail::deref_if_pointer_like(this->idempotent) && (rusty::detail::deref_if_pointer_like(current_retry_count) < rusty::detail::deref_if_pointer_like(this->max_retries));
}

uint64_t RequestOptions::calculate_delay_ms(uint16_t attempt) const {
    double delay = static_cast<double>(this->base_delay_ms);
    uint16_t i = static_cast<uint16_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(attempt)) {
        delay *= 2.0;
        if (rusty::detail::deref_if_pointer_like(delay) > ((static_cast<double>(this->max_delay_ms)))) {
            delay = static_cast<double>(this->max_delay_ms);
            break;
        }
        i += static_cast<uint16_t>(1);
    }
    if (rusty::detail::deref_if_pointer_like(delay) > ((static_cast<double>(this->max_delay_ms)))) {
        delay = static_cast<double>(this->max_delay_ms);
    }
    if (rusty::detail::deref_if_pointer_like(this->jitter_factor) > 0.0f) {
        const double jitter = (rusty::detail::deref_if_pointer_like(delay) * ((static_cast<double>(this->jitter_factor)))) * RandomGenerator::rand_double(-0.5, 0.5);
        delay += jitter;
        if (rusty::detail::deref_if_pointer_like(delay) < 0.0) {
            delay = 0.0;
        }
    }
    return rusty::float_to_int_cast<uint64_t>(delay);
}

bool RequestOptions::is_total_timeout_exceeded(uint64_t elapsed_ms) const {
    return (rusty::detail::deref_if_pointer_like(this->total_timeout_ms) > static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(elapsed_ms) >= rusty::detail::deref_if_pointer_like(this->total_timeout_ms));
}

uint64_t RequestOptions::remaining_time_ms(uint64_t elapsed_ms) const {
    if (rusty::detail::deref_if_pointer_like(this->total_timeout_ms) == static_cast<uint64_t>(0)) {
        return std::numeric_limits<uint64_t>::max();
    }
    if (rusty::detail::deref_if_pointer_like(elapsed_ms) >= rusty::detail::deref_if_pointer_like(this->total_timeout_ms)) {
        return static_cast<uint64_t>(0);
    }
    return rusty::detail::deref_if_pointer_like(this->total_timeout_ms) - rusty::detail::deref_if_pointer_like(elapsed_ms);
}
/*RUSTYCPP:GEN-END id=request_options.0*/

// Returns &'static str (-> std::string_view), not const char*: the DSL
// cannot spell a literal as a raw pointer. Callers use EXPECT_EQ, not
// EXPECT_STREQ. The varargs-UB objection to this is expired -- Log_* is a
// std::format variadic template now, not C varargs. See playbook 7.26.
#if RUSTYCPP_RUST
// NOTE: the parameter is `ty`, not `type` -- `type` is a Rust keyword and
// the DSL parser rejects it (CLAUDE.md documents this for fields; it applies
// to parameters too). C++ callers pass positionally, so the rename is local.
fn timeout_type_to_string(ty: TimeoutType) -> &'static str {
    match ty {
        TimeoutType::NONE => "NONE",
        TimeoutType::CONNECT_TIMEOUT => "CONNECT_TIMEOUT",
        TimeoutType::REQUEST_TIMEOUT => "REQUEST_TIMEOUT",
        TimeoutType::RESPONSE_TIMEOUT => "RESPONSE_TIMEOUT",
        TimeoutType::TOTAL_TIMEOUT => "TOTAL_TIMEOUT",
        _ => "UNKNOWN",
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_options.3 version=1 rust_sha256=eb3d5bd030447754c91ca6c42382923cb6a5afd8eb4a352b096d694a7bcdf35a*/
std::string_view timeout_type_to_string(TimeoutType ty) {
    return ({ auto&& _m = ty; std::optional<std::string_view> _match_value; bool _m_matched = false; if (!_m_matched && (_m == TimeoutType::NONE)) { _match_value.emplace(std::move(std::string_view("NONE"))); _m_matched = true; } if (!_m_matched && (_m == TimeoutType::CONNECT_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("CONNECT_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == TimeoutType::REQUEST_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("REQUEST_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == TimeoutType::RESPONSE_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("RESPONSE_TIMEOUT"))); _m_matched = true; } if (!_m_matched && (_m == TimeoutType::TOTAL_TIMEOUT)) { _match_value.emplace(std::move(std::string_view("TOTAL_TIMEOUT"))); _m_matched = true; } if (!_m_matched) { _match_value.emplace(std::move(std::string_view("UNKNOWN"))); _m_matched = true; } if (!_m_matched) { rusty::intrinsics::unreachable_panic(); } std::move(_match_value).value(); });
}
/*RUSTYCPP:GEN-END id=request_options.3*/

} // export namespace rrr
