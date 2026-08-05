// rrr.future — FiberPromise<T> / FiberFuture<T> (formerly future.h).
//
// One-shot async value delivery between fibers; producer side
// (FiberPromise) calls `set_value` exactly once, consumer side
// (FiberFuture) blocks in `get()` until the value is delivered.
// Wraps `rrr::BoxEvent<T>` for the underlying wait/notify; see
// rrr.reactor for the event primitive.
//
// Authored as inline Rust DSL: each struct's `#if RUSTYCPP_RUST` block
// is the source of truth; the transpiler regenerates the matching
// `RUSTYCPP:GEN-BEGIN ... END` block with the C++ template + members.
// Both structs are *generic* and *move-only* one-shot handles. The DSL
// derives copyability from fields, so a `rusty::Cell<bool>` field (which
// is itself move-only: copy `= delete`, move `= default`) is what keeps
// each struct non-copyable — mirroring the hand-written classes' deleted
// copy ctors without needing an `impl Drop`. For FiberPromise that field
// is the genuine `future_retrieved_` flag; FiberFuture has no natural
// second field, so it carries a `nc_` marker purely for move-only-ness.
//
// The state is a `rusty::Option<rusty::Arc<BoxEvent<T>>>` (nullable: a
// default/moved-from handle is `None`; the reactor hands out `Arc` from
// `create_sp_event`). The `set` / `wait` / `get` Arc derefs live in the DSL
// method bodies themselves: `let ev = self.state_.as_ref().unwrap()` binds
// the handle as a C++ reference (no refcount churn) and `(*ev).m()` lowers
// through `rusty::detail::deref_if_pointer_like` onto BoxEvent's (all-const)
// methods. Spelling that deref is mandatory, not cosmetic: `get`/`set` also
// name members of `Arc` itself, so a bare `ev.get()` binds to the HANDLE
// (`Arc::get()` returns a pointer) instead of to the event.
module;

#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
// Reachability: the GEN'd `.clone()` on the Option<Arc<...>> state lowers
// to rusty::clone (without this it resolves to glibc ::clone).
#include <rusty/move.hpp>
// rusty::detail::deref_if_pointer_like / rust_not, which the GEN'd `(*ev)`
// and `!` lower through (same GMF include as rrr.fiber's).
#include <rusty/slice.hpp>

export module rrr.future;

import std;
import rrr.reactor;

// @safe - FiberPromise<T> / FiberFuture<T>: one-shot async value
// delivery between fibers. See the file header for the state model and
// the move-only-via-Cell convention.
export namespace rrr {

// Forward declarations (the DSL GEN blocks below also self-declare;
// `struct` to match the transpiler's emitted tag).
template <typename T>
struct FiberPromise;
template <typename T>
struct FiberFuture;

// =============================================================================
// @unsafe helpers the DSL bodies call.
//
// Construction and hand-off only: the two state factories the DSL
// constructors call, plus `fiber_promise_get_future` (defined after
// FiberFuture). The state accessors that used to live here — set_value /
// is_ready / get / wait_for / valid — are now the DSL method bodies; the
// note that once explained their pointer parameters described a transpiler
// limitation that no longer exists.
// =============================================================================

// @unsafe - constructs a BoxEvent<T> as an Arc via Reactor internals.
template <typename T>
rusty::Arc<BoxEvent<T>> fiber_make_state() {
  return create_sp_box_event<T>();
}

// @safe - the empty (None) state, for a default/invalid FiberFuture.
template <typename T>
rusty::Option<rusty::Arc<BoxEvent<T>>> fiber_null_state() {
  return rusty::None;
}

// @unsafe - throws if already retrieved, then shares the state into a fresh
// FiberFuture. Takes the promise by reference (the DSL lowers a bare `self`
// argument to `(*this)`). Defined after FiberFuture below.
template <typename T>
FiberFuture<T> fiber_promise_get_future(FiberPromise<T>& self);

// =============================================================================
// FiberPromise<T> - Producer side of async value delivery
// =============================================================================
//
// Producer side of a one-shot async channel. `set_value` may be called
// exactly once; `get_future` may be retrieved exactly once. Move-only
// (each promise is the unique producer): the `future_retrieved_`
// rusty::Cell<bool> makes the struct non-copyable.
//
//   FiberPromise<std::string> promise;
//   auto future = promise.get_future();
//   promise.set_value("hello");        // unblocks future.get()
//
// @safe - see file header.
#if RUSTYCPP_RUST
struct FiberPromise<T> {
    state_: rusty::Option<rusty::Arc<BoxEvent<T>>>,
    future_retrieved_: rusty::Cell<bool>,
}

impl<T> FiberPromise<T> {
    // Constructs the shared BoxEvent up front (matches the old default ctor).
    #[cpp_ctor]
    fn new() -> FiberPromise<T> {
        FiberPromise { state_: fiber_make_state::<T>(), future_retrieved_: rusty::Cell::new(false) }
    }

    fn get_future(&mut self) -> FiberFuture<T> {
        fiber_promise_get_future(self)
    }

    // Each `assert!` emits `if (!cond) throw std::logic_error(msg)`, so the
    // conditions are the inverse of the old guards' throw tests.
    fn set_value(&mut self, value: &T) {
        assert!(self.state_.is_some(), "FiberPromise has no state (moved-from?)");
        let ev = self.state_.as_ref().unwrap();
        assert!(!(*ev).is_set_.get(), "FiberPromise value already set");
        (*ev).set(value);
    }

    fn is_ready(&self) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let ev = self.state_.as_ref().unwrap();
        (*ev).is_set_.get()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=future.fiber_promise version=1 rust_sha256=b3f8bef10415547a9342bc7d849b6558ea023cfb55290a00adb0273f492b65dd*/
template<typename T>
struct FiberPromise;

template<typename T>
struct FiberPromise {
    rusty::Option<rusty::Arc<BoxEvent<T>>> state_;
    rusty::Cell<bool> future_retrieved_;

    FiberPromise()
        : state_(fiber_make_state<T>())
        , future_retrieved_(rusty::Cell<bool>::new_(false))
    {}
    FiberFuture<T> get_future() {
        return fiber_promise_get_future((*this));
    }
    void set_value(const T& value) {
        if (!(this->state_.is_some())) { throw std::logic_error("FiberPromise has no state (moved-from?)"); }
        auto& ev = this->state_.as_ref().unwrap();
        if (!(rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(ev)).is_set_.get()))) { throw std::logic_error("FiberPromise value already set"); }
        ((rusty::detail::deref_if_pointer_like(ev))).set(std::move(value));
    }
    bool is_ready() const {
        if (this->state_.is_none()) {
            return false;
        }
        auto& ev = this->state_.as_ref().unwrap();
        return (rusty::detail::deref_if_pointer_like(ev)).is_set_.get();
    }
};
/*RUSTYCPP:GEN-END id=future.fiber_promise*/

// =============================================================================
// FiberFuture<T> - Consumer side of async value delivery
// =============================================================================
//
// Consumer side of a one-shot async channel. `get()` blocks the current
// fiber until the paired FiberPromise sets a value (and may be called
// repeatedly — it returns the same value each time). Move-only: the `nc_`
// rusty::Cell<bool> marker carries no state; it exists solely to make the
// struct non-copyable, preserving the hand-written class's deleted copy
// ctor (a FiberFuture is a single consumer handle).
//
// @safe - see file header.
#if RUSTYCPP_RUST
struct FiberFuture<T> {
    state_: rusty::Option<rusty::Arc<BoxEvent<T>>>,
    nc_: rusty::Cell<bool>,
}

impl<T> FiberFuture<T> {
    // Default: an invalid future (null shared state).
    #[cpp_ctor]
    fn new() -> FiberFuture<T> {
        FiberFuture { state_: fiber_null_state::<T>(), nc_: rusty::Cell::new(false) }
    }

    fn get(&mut self) -> T {
        assert!(self.state_.is_some(), "FiberFuture has no state (invalid or moved-from?)");
        let ev = self.state_.as_ref().unwrap();
        if !(*ev).is_set_.get() {
            (*ev).wait();
        }
        (*ev).get()
    }

    // `timeout_us == 0` blocks indefinitely (Event::wait's default).
    fn wait_for(&mut self, timeout_us: u64) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let ev = self.state_.as_ref().unwrap();
        if (*ev).is_set_.get() {
            return true;
        }
        (*ev).wait_timeout(timeout_us);
        (*ev).is_set_.get()
    }

    fn is_ready(&self) -> bool {
        if self.state_.is_none() {
            return false;
        }
        let ev = self.state_.as_ref().unwrap();
        (*ev).is_set_.get()
    }

    // Presence check on the shared state (Option is_some, no deref).
    fn valid(&self) -> bool {
        self.state_.is_some()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=future.fiber_future version=1 rust_sha256=4b5a5ae1f5da3e75d52aaa00257da747cf1527406ecd2ad509a3ea725fcf47cd*/
template<typename T>
struct FiberFuture;

template<typename T>
struct FiberFuture {
    rusty::Option<rusty::Arc<BoxEvent<T>>> state_;
    rusty::Cell<bool> nc_;

    FiberFuture()
        : state_(fiber_null_state<T>())
        , nc_(rusty::Cell<bool>::new_(false))
    {}
    T get() {
        if (!(this->state_.is_some())) { throw std::logic_error("FiberFuture has no state (invalid or moved-from?)"); }
        auto& ev = this->state_.as_ref().unwrap();
        if (rusty::detail::rust_not((rusty::detail::deref_if_pointer_like(ev)).is_set_.get())) {
            ((rusty::detail::deref_if_pointer_like(ev))).wait();
        }
        return ((rusty::detail::deref_if_pointer_like(ev))).get();
    }
    bool wait_for(uint64_t timeout_us) {
        if (this->state_.is_none()) {
            return false;
        }
        auto& ev = this->state_.as_ref().unwrap();
        if ((rusty::detail::deref_if_pointer_like(ev)).is_set_.get()) {
            return true;
        }
        ((rusty::detail::deref_if_pointer_like(ev))).wait_timeout(std::move(timeout_us));
        return (rusty::detail::deref_if_pointer_like(ev)).is_set_.get();
    }
    bool is_ready() const {
        if (this->state_.is_none()) {
            return false;
        }
        auto& ev = this->state_.as_ref().unwrap();
        return (rusty::detail::deref_if_pointer_like(ev)).is_set_.get();
    }
    bool valid() const {
        return this->state_.is_some();
    }
};
/*RUSTYCPP:GEN-END id=future.fiber_future*/

// =============================================================================
// Hand-off + convenience factories
// =============================================================================
//
// `fiber_promise_get_future` lives down here, after FiberFuture's GEN block,
// because it needs FiberFuture to be a complete type; the declaration near the
// top of the file is what lets `FiberPromise::get_future` (defined earlier)
// call it. All three bodies are plain control flow, so all three are DSL —
// three lowerings worth knowing:
//   * `Default::default()` in typed-let position becomes
//     `rusty::default_like<V>()`. Neither struct has a `default_()` static
//     (both keep a real `#[cpp_ctor]` constructor), so it falls through to
//     `V{}` — value-init through that constructor, i.e. exactly the old
//     `FiberFuture<T> f;`. No construction kernel is needed.
//   * `.clone()` becomes `rusty::clone(...)`, which routes through
//     `Option::clone` to an Arc refcount bump. That name lives in
//     <rusty/move.hpp>, which is why the GMF includes it.
//   * `let mut` is load-bearing on every binding that is moved out at the end:
//     a plain `let` emits a `const` local, and `std::move` on a const
//     move-only local degrades to a (deleted) copy.
//
// @safe - the Arc share is a refcount bump through Option::clone; the
// retrieval guard is the same one-shot check the hand-written version had.
#if RUSTYCPP_RUST
// Throws on a second retrieval, then shares the promise's BoxEvent into a
// fresh (default-constructed, therefore None-state) FiberFuture. `assert!`
// emits `if (!cond) throw`, so the condition is the inverse of the old guard.
fn fiber_promise_get_future<T>(self_: &mut FiberPromise<T>) -> FiberFuture<T> {
    assert!(!self_.future_retrieved_.get(), "FiberFuture already retrieved from FiberPromise");
    self_.future_retrieved_.set(true);
    let mut f: FiberFuture<T> = Default::default();
    f.state_ = self_.state_.clone();
    f
}

// Create a FiberPromise/FiberFuture pair in one call. The `std::pair` return
// survives the DSL verbatim, so tests/fiber_test.cc's structured binding needs
// no edit.
fn make_promise<T>() -> std::pair<FiberPromise<T>, FiberFuture<T>> {
    let mut promise: FiberPromise<T> = Default::default();
    let mut future: FiberFuture<T> = promise.get_future();
    std::make_pair(promise, future)
}

// Create a FiberFuture that is immediately ready with `value`.
fn make_ready_future<T>(value: T) -> FiberFuture<T> {
    let mut promise: FiberPromise<T> = Default::default();
    let mut future: FiberFuture<T> = promise.get_future();
    promise.set_value(&value);
    future
}
#endif
/*RUSTYCPP:GEN-BEGIN id=future.3 version=1 rust_sha256=63a2f3f46f3897da0883b7673398ce8a52bc8f04d4c36878fd3a27b83d109861*/
template<typename T>
FiberFuture<T> fiber_promise_get_future(FiberPromise<T>& self_) {
    if (!(rusty::detail::rust_not(self_.future_retrieved_.get()))) { throw std::logic_error("FiberFuture already retrieved from FiberPromise"); }
    self_.future_retrieved_.set(true);
    FiberFuture<T> f = rusty::default_like<FiberFuture<T>>();
    f.state_ = rusty::clone(self_.state_);
    return std::move(f);
}

template<typename T>
std::pair<FiberPromise<T>, FiberFuture<T>> make_promise() {
    FiberPromise<T> promise = rusty::default_like<FiberPromise<T>>();
    FiberFuture<T> future = promise.get_future();
    return std::make_pair(std::move(promise), std::move(future));
}

template<typename T>
FiberFuture<T> make_ready_future(T value) {
    FiberPromise<T> promise = rusty::default_like<FiberPromise<T>>();
    FiberFuture<T> future = promise.get_future();
    promise.set_value(value);
    return std::move(future);
}
/*RUSTYCPP:GEN-END id=future.3*/

}  // export namespace rrr
