module;

#include <stdint.h>
#include <rusty/rusty.hpp>

export module rrr.request_options;

import std;

// @safe - POD options struct + TimeoutType enum + factory helpers
// + simple jitter computation. No raw pointers, syscalls, or
// operator-overload chains.
export namespace rrr {

enum class TimeoutType : uint8_t {
    NONE = 0,
    CONNECT_TIMEOUT,
    REQUEST_TIMEOUT,
    RESPONSE_TIMEOUT,
    TOTAL_TIMEOUT
};

// Free helpers backing `RequestOptions::can_retry`,
// `is_total_timeout_exceeded`, and `remaining_time_ms`. Pure predicates
// / sentinel-arithmetic over struct fields; member methods are thin
// forwarders. Authored as inline Rust DSL.
#if RUSTYCPP_RUST
fn request_can_retry(idempotent: bool, current_retry_count: u16, max_retries: u16) -> bool {
    idempotent && current_retry_count < max_retries
}

fn request_total_timeout_exceeded(total_timeout_ms: u64, elapsed_ms: u64) -> bool {
    total_timeout_ms > 0 && elapsed_ms >= total_timeout_ms
}

fn request_remaining_time_ms(total_timeout_ms: u64, elapsed_ms: u64) -> u64 {
    if total_timeout_ms == 0 {
        return u64::MAX;
    }
    if elapsed_ms >= total_timeout_ms {
        return 0;
    }
    total_timeout_ms - elapsed_ms
}
#endif
/*RUSTYCPP:GEN-BEGIN id=request_options.1 version=1 rust_sha256=a81c8f018bca3693c1162d3274454d5e840a8bc58ac674f32830ae7eca785871*/
bool request_can_retry(bool idempotent, uint16_t current_retry_count, uint16_t max_retries);
bool request_total_timeout_exceeded(uint64_t total_timeout_ms, uint64_t elapsed_ms);
uint64_t request_remaining_time_ms(uint64_t total_timeout_ms, uint64_t elapsed_ms);

bool request_can_retry(bool idempotent, uint16_t current_retry_count, uint16_t max_retries) {
    return rusty::detail::deref_if_pointer_like(idempotent) && (rusty::detail::deref_if_pointer_like(current_retry_count) < rusty::detail::deref_if_pointer_like(max_retries));
}

bool request_total_timeout_exceeded(uint64_t total_timeout_ms, uint64_t elapsed_ms) {
    return (rusty::detail::deref_if_pointer_like(total_timeout_ms) > 0) && (rusty::detail::deref_if_pointer_like(elapsed_ms) >= rusty::detail::deref_if_pointer_like(total_timeout_ms));
}

uint64_t request_remaining_time_ms(uint64_t total_timeout_ms, uint64_t elapsed_ms) {
    if (rusty::detail::deref_if_pointer_like(total_timeout_ms) == static_cast<uint64_t>(0)) {
        return std::numeric_limits<uint64_t>::max();
    }
    if (rusty::detail::deref_if_pointer_like(elapsed_ms) >= rusty::detail::deref_if_pointer_like(total_timeout_ms)) {
        return static_cast<uint64_t>(0);
    }
    return rusty::detail::deref_if_pointer_like(total_timeout_ms) - rusty::detail::deref_if_pointer_like(elapsed_ms);
}
/*RUSTYCPP:GEN-END id=request_options.1*/

struct RequestOptions {
    uint64_t timeout_ms = 1000;
    uint64_t total_timeout_ms = 0;

    uint16_t max_retries = 0;
    uint16_t base_delay_ms = 50;
    uint16_t max_delay_ms = 5000;
    float jitter_factor = 0.1f;

    bool idempotent = false;

    static RequestOptions defaults() {
        return RequestOptions{};
    }

    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms = 1000) {
        RequestOptions opts;
        opts.timeout_ms = timeout_ms;
        opts.max_retries = max_retries;
        opts.idempotent = true;
        return opts;
    }

    static RequestOptions idempotent_retry(uint16_t max_retries = 3) {
        RequestOptions opts;
        opts.max_retries = max_retries;
        opts.idempotent = true;
        return opts;
    }

    static RequestOptions no_timeout() {
        RequestOptions opts;
        opts.timeout_ms = 0;
        opts.total_timeout_ms = 0;
        return opts;
    }

    static RequestOptions fast() {
        RequestOptions opts;
        opts.timeout_ms = 100;
        opts.max_retries = 2;
        opts.base_delay_ms = 10;
        opts.max_delay_ms = 100;
        opts.idempotent = true;
        return opts;
    }

    static RequestOptions patient() {
        RequestOptions opts;
        opts.timeout_ms = 10000;
        opts.total_timeout_ms = 60000;
        opts.max_retries = 5;
        opts.base_delay_ms = 500;
        opts.max_delay_ms = 10000;
        opts.idempotent = true;
        return opts;
    }

    bool can_retry(uint16_t current_retry_count) const {
        return request_can_retry(idempotent, current_retry_count, max_retries);
    }

    uint64_t calculate_delay_ms(uint16_t attempt) const {
        double delay = static_cast<double>(base_delay_ms) * std::pow(2.0, attempt);

        if (delay > static_cast<double>(max_delay_ms)) {
            delay = static_cast<double>(max_delay_ms);
        }

        if (jitter_factor > 0.0f) {
            thread_local std::mt19937 gen(std::random_device{}());
            thread_local std::uniform_real_distribution<double> dist(-0.5, 0.5);

            double jitter = delay * static_cast<double>(jitter_factor) * dist(gen);
            delay += jitter;

            if (delay < 0.0) {
                delay = 0.0;
            }
        }

        return static_cast<uint64_t>(delay);
    }

    bool is_total_timeout_exceeded(uint64_t elapsed_ms) const {
        return request_total_timeout_exceeded(total_timeout_ms, elapsed_ms);
    }

    uint64_t remaining_time_ms(uint64_t elapsed_ms) const {
        return request_remaining_time_ms(total_timeout_ms, elapsed_ms);
    }
};

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

} // export namespace rrr
