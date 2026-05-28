module;

#include <rusty/cell.hpp>
#include <rusty/rusty.hpp>
#include <stdint.h>

export module rrr.reconnect_policy;

import std;

// @safe - POD ReconnectPolicy struct + ReconnectCalculator (stateless
// backoff math). No raw pointers, syscalls, or operator-overload chains.
export namespace rrr {

struct ReconnectPolicy {
    bool auto_reconnect;
    uint32_t max_retries;
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    double backoff_multiplier;
    bool jitter_enabled;

    ReconnectPolicy()
        : auto_reconnect(true)
        , max_retries(5)
        , initial_delay_ms(1000)
        , max_delay_ms(30000)
        , backoff_multiplier(2.0)
        , jitter_enabled(true)
    {}

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

    static ReconnectPolicy aggressive() {
        return ReconnectPolicy(true, 0, 100, 5000, 1.5, true);
    }

    static ReconnectPolicy conservative() {
        return ReconnectPolicy(true, 5, 1000, 30000, 2.0, true);
    }

    static ReconnectPolicy no_retry() {
        return ReconnectPolicy(false, 0, 0, 0, 1.0, false);
    }
};

// Free helpers backing `ReconnectCalculator::should_retry` and
// `ReconnectCalculator::retries_exhausted`. Both classify the same
// (auto_reconnect, max_retries, retry_count) tuple; co-located in
// a single inline Rust DSL block.
#if RUSTYCPP_RUST
fn reconnect_should_retry(auto_reconnect: bool, max_retries: u32, retry_count: u32) -> bool {
    if !auto_reconnect {
        return false;
    }
    if max_retries == 0 {
        return true;
    }
    retry_count < max_retries
}

fn reconnect_retries_exhausted(auto_reconnect: bool, max_retries: u32, retry_count: u32) -> bool {
    if !auto_reconnect {
        return true;
    }
    if max_retries == 0 {
        return false;
    }
    retry_count >= max_retries
}

// Deterministic exponential backoff used by both `peek_delay_ms` and the
// pure part of `next_delay_ms` (the jitter step is C++-side because it
// pulls a random sample). Multiplies `initial_delay_ms` by
// `backoff_multiplier` once per retry, clamping at `max_delay_ms`.
fn reconnect_peek_delay_ms_impl(
    initial_delay_ms: u32,
    max_delay_ms: u32,
    backoff_multiplier: f64,
    retry_count: u32,
) -> u32 {
    let mut delay: f64 = initial_delay_ms as f64;
    let max_delay: f64 = max_delay_ms as f64;
    let mut i: u32 = 0;
    while i < retry_count {
        delay *= backoff_multiplier;
        if delay >= max_delay {
            delay = max_delay;
            break;
        }
        i += 1;
    }
    if delay > max_delay {
        delay = max_delay;
    }
    delay as u32
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reconnect_policy.1 version=1 rust_sha256=0e4abdd3e05b5d86e56260ce54c1c801ba8e023d2f6b957f22b797873c483b6b*/
bool reconnect_should_retry(bool auto_reconnect, uint32_t max_retries, uint32_t retry_count);
bool reconnect_retries_exhausted(bool auto_reconnect, uint32_t max_retries, uint32_t retry_count);
uint32_t reconnect_peek_delay_ms_impl(uint32_t initial_delay_ms, uint32_t max_delay_ms, double backoff_multiplier, uint32_t retry_count);

bool reconnect_should_retry(bool auto_reconnect, uint32_t max_retries, uint32_t retry_count) {
    if (!auto_reconnect) {
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(max_retries) == static_cast<uint32_t>(0)) {
        return true;
    }
    return rusty::detail::deref_if_pointer_like(retry_count) < rusty::detail::deref_if_pointer_like(max_retries);
}

bool reconnect_retries_exhausted(bool auto_reconnect, uint32_t max_retries, uint32_t retry_count) {
    if (!auto_reconnect) {
        return true;
    }
    if (rusty::detail::deref_if_pointer_like(max_retries) == static_cast<uint32_t>(0)) {
        return false;
    }
    return rusty::detail::deref_if_pointer_like(retry_count) >= rusty::detail::deref_if_pointer_like(max_retries);
}

uint32_t reconnect_peek_delay_ms_impl(uint32_t initial_delay_ms, uint32_t max_delay_ms, double backoff_multiplier, uint32_t retry_count) {
    double delay = static_cast<double>(initial_delay_ms);
    const double max_delay = static_cast<double>(max_delay_ms);
    uint32_t i = static_cast<uint32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(retry_count)) {
        delay *= backoff_multiplier;
        if (rusty::detail::deref_if_pointer_like(delay) >= rusty::detail::deref_if_pointer_like(max_delay)) {
            delay = std::move(max_delay);
            break;
        }
        i += 1;
    }
    if (rusty::detail::deref_if_pointer_like(delay) > rusty::detail::deref_if_pointer_like(max_delay)) {
        delay = std::move(max_delay);
    }
    return static_cast<uint32_t>(delay);
}
/*RUSTYCPP:GEN-END id=reconnect_policy.1*/

class ReconnectCalculator {
private:
    const ReconnectPolicy& policy_;
    rusty::Cell<uint32_t> retry_count_{0};

public:
    explicit ReconnectCalculator(const ReconnectPolicy& policy)
        : policy_(policy)
    {}

    ReconnectCalculator(const ReconnectCalculator&) = delete;
    ReconnectCalculator& operator=(const ReconnectCalculator&) = delete;

    ReconnectCalculator(ReconnectCalculator&&) = default;
    ReconnectCalculator& operator=(ReconnectCalculator&&) = default;

    bool should_retry() const {
        return reconnect_should_retry(
            policy_.auto_reconnect, policy_.max_retries, retry_count_.get());
    }

    uint32_t next_delay_ms() {
        uint32_t count = retry_count_.get();
        retry_count_.set(count + 1);

        // Deterministic exponential backoff shared with peek_delay_ms.
        double delay = static_cast<double>(
            reconnect_peek_delay_ms_impl(
                policy_.initial_delay_ms,
                policy_.max_delay_ms,
                policy_.backoff_multiplier,
                count));

        // Jitter step stays C++-side (uses std::random_device).
        if (policy_.jitter_enabled && delay > 0) {
            std::random_device rd;
            std::uniform_real_distribution<double> dist(0.5, 1.5);
            delay *= dist(rd);
        }

        return static_cast<uint32_t>(delay);
    }

    uint32_t peek_delay_ms() const {
        return reconnect_peek_delay_ms_impl(
            policy_.initial_delay_ms,
            policy_.max_delay_ms,
            policy_.backoff_multiplier,
            retry_count_.get());
    }

    void reset() {
        retry_count_.set(0);
    }

    uint32_t retry_count() const {
        return retry_count_.get();
    }

    bool retries_exhausted() const {
        return reconnect_retries_exhausted(
            policy_.auto_reconnect, policy_.max_retries, retry_count_.get());
    }
};

} // export namespace rrr
