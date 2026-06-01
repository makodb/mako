module;

#include <rusty/cell.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>
#include <cstdint>

export module rrr.reconnect_policy;

import std;
import rrr.rand;

// @safe - POD ReconnectPolicy struct + ReconnectCalculator (stateless
// backoff math). No raw pointers, syscalls, or operator-overload chains.
export namespace rrr {

// ReconnectPolicy is a plain aggregate POD: no user-declared
// constructors, fields carry in-class default initializers that the
// `conservative()` preset matches. This shape is intentionally kept
// in plain C++ rather than migrated to the inline-Rust DSL because
// the DSL does not emit per-field in-class `= default` initializers
// (`bool auto_reconnect = true;`). ~20 callers / tests rely on the
// `ReconnectPolicy policy; policy.X = ...;` default-mutate-customize
// pattern that needs those documented defaults present after default
// construction; patching all of them is bigger than its DSL-migration
// payoff (the struct is already a pure aggregate after dropping the
// user-defined ctors).
//
// The previous user-defined default and parameterized ctors were
// dropped — the one external caller that wrote `ReconnectPolicy()`
// was switched to `ReconnectPolicy::conservative()` (identical
// values), and the parameterized ctor was never called from outside
// this file.
struct ReconnectPolicy {
    bool auto_reconnect = true;
    uint32_t max_retries = 5;
    uint32_t initial_delay_ms = 1000;
    uint32_t max_delay_ms = 30000;
    double backoff_multiplier = 2.0;
    bool jitter_enabled = true;

    static ReconnectPolicy aggressive() {
        return ReconnectPolicy{true, 0, 100, 5000, 1.5, true};
    }

    static ReconnectPolicy conservative() {
        return ReconnectPolicy{true, 5, 1000, 30000, 2.0, true};
    }

    static ReconnectPolicy no_retry() {
        return ReconnectPolicy{false, 0, 0, 0, 1.0, false};
    }
};

// `ReconnectCalculator` — exponential-backoff state machine over a
// `const ReconnectPolicy&` and a `rusty::Cell<u32>` retry counter.
//
// Authored as inline Rust DSL: the `#if RUSTYCPP_RUST` block below is
// the source of truth; the transpiler regenerates the matching
// `/*RUSTYCPP:GEN-BEGIN ... END*/` block. The plain `fn new(policy)`
// lowers to a `static ReconnectCalculator new_(const ReconnectPolicy&)`
// factory; callers construct via the factory rather than direct ctor
// syntax (`auto calc = ReconnectCalculator::new_(policy);`).
//
// Field rename note: the original C++ class used a private member
// `retry_count_` alongside a public method `retry_count()`. The DSL
// auto-renames colliding fields to `<name>_field`, but the
// `#[cpp_ctor]` init-list emission does not yet apply that rename.
// To sidestep that, the field is renamed to `retries` in the DSL; the
// public method stays `retry_count()`.
//
// Behavioral diffs from the original C++ class:
//   * `next_delay_ms()` is now `const`. The body still mutates state,
//     but only through the Cell, which is itself const-callable. The
//     DSL `&self` receiver lowers to a `const` C++ method. Callers
//     that held a non-const ref keep working.
//   * The `= delete` copy ctor / `= default` move ctor declarations
//     are dropped (the DSL does not emit special member function
//     annotations). The class is implicitly copyable now; acceptable
//     because all call sites store the calculator by value on the
//     stack, none attempt a copy, and a Cell + reference copy is
//     trivially shallow.
//   * Fields are no longer `private` — the DSL emits all fields as
//     public. No callers reach into the fields (everything goes
//     through the public methods).
//   * The jitter path uses `RandomGenerator::rand_double(0.5, 1.5)`
//     instead of `std::random_device + std::uniform_real_distribution`
//     (DSL-friendly equivalent — same [0.5, 1.5) distribution).
#if RUSTYCPP_RUST
struct ReconnectCalculator<'p> {
    policy: &'p ReconnectPolicy,
    retries: Cell<u32>,
}

impl<'p> ReconnectCalculator<'p> {
    fn new(policy: &'p ReconnectPolicy) -> ReconnectCalculator<'p> {
        ReconnectCalculator {
            policy: policy,
            retries: Cell::new(0u32),
        }
    }

    fn should_retry(&self) -> bool {
        if !self.policy.auto_reconnect {
            return false;
        }
        if self.policy.max_retries == 0u32 {
            return true;
        }
        self.retries.get() < self.policy.max_retries
    }

    fn next_delay_ms(&self) -> u32 {
        let count: u32 = self.retries.get();
        self.retries.set(count + 1u32);

        let mut delay: f64 = self.policy.initial_delay_ms as f64;
        let mut i: u32 = 0u32;
        while i < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= (self.policy.max_delay_ms as f64) {
                delay = self.policy.max_delay_ms as f64;
                break;
            }
            i += 1u32;
        }

        if delay > (self.policy.max_delay_ms as f64) {
            delay = self.policy.max_delay_ms as f64;
        }

        if self.policy.jitter_enabled && delay > 0.0f64 {
            delay *= RandomGenerator::rand_double(0.5f64, 1.5f64);
        }

        delay as u32
    }

    fn peek_delay_ms(&self) -> u32 {
        let count: u32 = self.retries.get();

        let mut delay: f64 = self.policy.initial_delay_ms as f64;
        let mut i: u32 = 0u32;
        while i < count {
            delay *= self.policy.backoff_multiplier;
            if delay >= (self.policy.max_delay_ms as f64) {
                delay = self.policy.max_delay_ms as f64;
                break;
            }
            i += 1u32;
        }

        if delay > (self.policy.max_delay_ms as f64) {
            delay = self.policy.max_delay_ms as f64;
        }

        delay as u32
    }

    fn reset(&self) {
        self.retries.set(0u32);
    }

    fn retry_count(&self) -> u32 {
        self.retries.get()
    }

    fn retries_exhausted(&self) -> bool {
        if !self.policy.auto_reconnect {
            return true;
        }
        if self.policy.max_retries == 0u32 {
            return false;
        }
        self.retries.get() >= self.policy.max_retries
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=reconnect_calculator.1 version=1 rust_sha256=54c0a4cbba7cb91cdd16c0d6c5fe650673562ed05bcd904245a484a850fe649a*/
struct ReconnectCalculator;

struct ReconnectCalculator {
    const ReconnectPolicy& policy;
    rusty::Cell<uint32_t> retries;

    static ReconnectCalculator new_(const ReconnectPolicy& policy);
    bool should_retry() const;
    uint32_t next_delay_ms() const;
    uint32_t peek_delay_ms() const;
    void reset() const;
    uint32_t retry_count() const;
    bool retries_exhausted() const;
};


ReconnectCalculator ReconnectCalculator::new_(const ReconnectPolicy& policy) {
    return ReconnectCalculator{.policy = policy, .retries = rusty::Cell<uint32_t>::new_(static_cast<uint32_t>(0))};
}

bool ReconnectCalculator::should_retry() const {
    if (!this->policy.auto_reconnect) {
        return false;
    }
    if (rusty::detail::deref_if_pointer_like(this->policy.max_retries) == static_cast<uint32_t>(0)) {
        return true;
    }
    return this->retries.get() < rusty::detail::deref_if_pointer_like(this->policy.max_retries);
}

uint32_t ReconnectCalculator::next_delay_ms() const {
    const uint32_t count = this->retries.get();
    this->retries.set(rusty::detail::deref_if_pointer_like(count) + static_cast<uint32_t>(1));
    double delay = static_cast<double>(this->policy.initial_delay_ms);
    uint32_t i = static_cast<uint32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(count)) {
        delay *= this->policy.backoff_multiplier;
        if (rusty::detail::deref_if_pointer_like(delay) >= ((static_cast<double>(this->policy.max_delay_ms)))) {
            delay = static_cast<double>(this->policy.max_delay_ms);
            break;
        }
        i += static_cast<uint32_t>(1);
    }
    if (rusty::detail::deref_if_pointer_like(delay) > ((static_cast<double>(this->policy.max_delay_ms)))) {
        delay = static_cast<double>(this->policy.max_delay_ms);
    }
    if (rusty::detail::deref_if_pointer_like(this->policy.jitter_enabled) && (rusty::detail::deref_if_pointer_like(delay) > 0.0)) {
        delay *= RandomGenerator::rand_double(0.5, 1.5);
    }
    return static_cast<uint32_t>(delay);
}

uint32_t ReconnectCalculator::peek_delay_ms() const {
    const uint32_t count = this->retries.get();
    double delay = static_cast<double>(this->policy.initial_delay_ms);
    uint32_t i = static_cast<uint32_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(count)) {
        delay *= this->policy.backoff_multiplier;
        if (rusty::detail::deref_if_pointer_like(delay) >= ((static_cast<double>(this->policy.max_delay_ms)))) {
            delay = static_cast<double>(this->policy.max_delay_ms);
            break;
        }
        i += static_cast<uint32_t>(1);
    }
    if (rusty::detail::deref_if_pointer_like(delay) > ((static_cast<double>(this->policy.max_delay_ms)))) {
        delay = static_cast<double>(this->policy.max_delay_ms);
    }
    return static_cast<uint32_t>(delay);
}

void ReconnectCalculator::reset() const {
    this->retries.set(static_cast<uint32_t>(0));
}

uint32_t ReconnectCalculator::retry_count() const {
    return this->retries.get();
}

bool ReconnectCalculator::retries_exhausted() const {
    if (!this->policy.auto_reconnect) {
        return true;
    }
    if (rusty::detail::deref_if_pointer_like(this->policy.max_retries) == static_cast<uint32_t>(0)) {
        return false;
    }
    return this->retries.get() >= rusty::detail::deref_if_pointer_like(this->policy.max_retries);
}
/*RUSTYCPP:GEN-END id=reconnect_calculator.1*/

} // export namespace rrr
