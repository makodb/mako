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
#endif
/*RUSTYCPP:GEN-BEGIN id=reconnect_policy.1 version=1 rust_sha256=650e4aca5bc6f4b32d2e9c3d6866cc9f1a0b77648f4283190c041999427c859c*/
bool reconnect_should_retry(bool auto_reconnect, uint32_t max_retries, uint32_t retry_count);
bool reconnect_retries_exhausted(bool auto_reconnect, uint32_t max_retries, uint32_t retry_count);

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

        double delay = static_cast<double>(policy_.initial_delay_ms);
        for (uint32_t i = 0; i < count; ++i) {
            delay *= policy_.backoff_multiplier;
            if (delay >= static_cast<double>(policy_.max_delay_ms)) {
                delay = static_cast<double>(policy_.max_delay_ms);
                break;
            }
        }

        delay = std::min(delay, static_cast<double>(policy_.max_delay_ms));

        if (policy_.jitter_enabled && delay > 0) {
            std::random_device rd;
            std::uniform_real_distribution<double> dist(0.5, 1.5);
            delay *= dist(rd);
        }

        return static_cast<uint32_t>(delay);
    }

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

        return static_cast<uint32_t>(delay);
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
