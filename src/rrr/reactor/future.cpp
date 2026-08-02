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
// `create_sp_event`). Every operation that drives `->set` / `->wait` /
// `->get` is an `@unsafe` Arc deref (through a const-view handle), so each
// lives in a generic free function below rather than in the DSL struct body.
module;

#include <cstddef>
#include <cstdint>

#include <rusty/arc.hpp>
#include <rusty/cell.hpp>
#include <rusty/option.hpp>

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
// @unsafe free functions backing the Arc-deref method bodies.
//
// Pointer parameters (`const rusty::Option<rusty::Arc<BoxEvent<T>>>* state`)
// are deliberate: the DSL lowers `&self.state_` to `&this->state_`, i.e. an
// address-of yielding a pointer. Events are const-view Arc handles — every
// BoxEvent method reached through `(*state).as_ref().unwrap()->…` is already
// `const` (set/wait/get and the `is_set_` Cell), so no const_cast is needed
// (unlike the old const FiberFuture::get shim).
// =============================================================================

// @unsafe - constructs a BoxEvent<T> as an Arc via Reactor internals.
template <typename T>
rusty::Arc<BoxEvent<T>> fiber_make_state() {
  return reactor_create_sp_event<BoxEvent<T>>();
}

// @safe - the empty (None) state, for a default/invalid FiberFuture.
template <typename T>
rusty::Option<rusty::Arc<BoxEvent<T>>> fiber_null_state() {
  return rusty::None;
}

// @unsafe - Arc deref through `(*state).as_ref().unwrap()->is_set_.get()` /
// `->set(value)`. Takes the value by `const T&` (BoxEvent::set copies
// regardless), so there is no extra copy for lvalue callers.
template <typename T>
void fiber_promise_set_value(const rusty::Option<rusty::Arc<BoxEvent<T>>>* state, const T& value) {
  if (state->is_none()) {
    throw std::logic_error("FiberPromise has no state (moved-from?)");
  }
  if ((*state).as_ref().unwrap()->is_set_.get()) {
    throw std::logic_error("FiberPromise value already set");
  }
  (*state).as_ref().unwrap()->set(value);
}

// @unsafe - Arc deref through `(*state).as_ref().unwrap()->is_set_.get()`.
template <typename T>
bool fiber_promise_is_ready(const rusty::Option<rusty::Arc<BoxEvent<T>>>* state) {
  return state->is_some() && (*state).as_ref().unwrap()->is_set_.get();
}

// @unsafe - blocks in `wait()` then returns a copy of the shared value.
template <typename T>
T fiber_future_get(const rusty::Option<rusty::Arc<BoxEvent<T>>>* state) {
  if (state->is_none()) {
    throw std::logic_error("FiberFuture has no state (invalid or moved-from?)");
  }
  if (!(*state).as_ref().unwrap()->is_set_.get()) {
    (*state).as_ref().unwrap()->wait();
  }
  return (*state).as_ref().unwrap()->get();
}

// @unsafe - bounded wait; Arc deref through `(*state).as_ref().unwrap()->wait_timeout(timeout_us)`.
// `timeout_us == 0` blocks indefinitely (Event::wait's default).
template <typename T>
bool fiber_future_wait_for(const rusty::Option<rusty::Arc<BoxEvent<T>>>* state, uint64_t timeout_us) {
  if (state->is_none()) {
    return false;
  }
  if ((*state).as_ref().unwrap()->is_set_.get()) {
    return true;
  }
  (*state).as_ref().unwrap()->wait_timeout(timeout_us);
  return (*state).as_ref().unwrap()->is_set_.get();
}

// @unsafe - Arc deref through `(*state).as_ref().unwrap()->is_set_.get()`.
template <typename T>
bool fiber_future_is_ready(const rusty::Option<rusty::Arc<BoxEvent<T>>>* state) {
  return state->is_some() && (*state).as_ref().unwrap()->is_set_.get();
}

// @safe - presence check on the shared state (Option is_some, no deref).
template <typename T>
bool fiber_future_valid(const rusty::Option<rusty::Arc<BoxEvent<T>>>* state) {
  return state->is_some();
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

    fn set_value(&mut self, value: &T) {
        fiber_promise_set_value(&self.state_, value);
    }

    fn is_ready(&self) -> bool {
        fiber_promise_is_ready(&self.state_)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=future.fiber_promise version=1 rust_sha256=8a5a93c7106ab1f3f75bef45dace581e334f58a7ba23d67fcb1981b77abff2e4*/
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
        fiber_promise_set_value(&this->state_, value);
    }
    bool is_ready() const {
        return fiber_promise_is_ready(&this->state_);
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
        fiber_future_get(&self.state_)
    }

    fn wait_for(&mut self, timeout_us: u64) -> bool {
        fiber_future_wait_for(&self.state_, timeout_us)
    }

    fn is_ready(&self) -> bool {
        fiber_future_is_ready(&self.state_)
    }

    fn valid(&self) -> bool {
        fiber_future_valid(&self.state_)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=future.fiber_future version=1 rust_sha256=460e6ad6219c674869c16a1d8ab1c4f080ded853de712f930a7c13ff950d2cc9*/
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
        return fiber_future_get(&this->state_);
    }
    bool wait_for(uint64_t timeout_us) {
        return fiber_future_wait_for(&this->state_, std::move(timeout_us));
    }
    bool is_ready() const {
        return fiber_future_is_ready(&this->state_);
    }
    bool valid() const {
        return fiber_future_valid(&this->state_);
    }
};
/*RUSTYCPP:GEN-END id=future.fiber_future*/

// @unsafe - shares the promise's BoxEvent into a new FiberFuture; throws on a
// second retrieval. The state clone is the only @unsafe step (Arc refcount).
template <typename T>
FiberFuture<T> fiber_promise_get_future(FiberPromise<T>& self) {
  if (self.future_retrieved_.get()) {
    throw std::logic_error("FiberFuture already retrieved from FiberPromise");
  }
  self.future_retrieved_.set(true);
  FiberFuture<T> f;
  f.state_ = self.state_.clone();
  return f;
}

// =============================================================================
// Convenience Factory Functions
// =============================================================================

// @safe - create a FiberPromise/FiberFuture pair in one call.
template <typename T>
std::pair<FiberPromise<T>, FiberFuture<T>> make_promise() {
  FiberPromise<T> promise;
  FiberFuture<T> future = promise.get_future();
  return {std::move(promise), std::move(future)};
}

// @safe - create a FiberFuture that is immediately ready with `value`.
template <typename T>
FiberFuture<T> make_ready_future(T value) {
  FiberPromise<T> promise;
  FiberFuture<T> future = promise.get_future();
  promise.set_value(value);
  return future;
}

}  // export namespace rrr
