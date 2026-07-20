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
// The state is a `std::shared_ptr<BoxEvent<T>>` (an rrr/reactor boundary
// type — see CLAUDE.md). Every operation that drives `state_->set` /
// `->wait` / `->get` is an `@unsafe` shared_ptr deref, so each lives in a
// generic free function below rather than inside the DSL struct body.
module;

#include <cstddef>
#include <cstdint>

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
// @unsafe free functions backing the shared_ptr-deref method bodies.
//
// Pointer parameters (`const std::shared_ptr<BoxEvent<T>>* state`) are
// deliberate: the DSL lowers `&self.state_` to `&this->state_`, i.e. an
// address-of yielding a pointer. shared_ptr's const does not propagate to
// the pointee, so a `const shared_ptr*` still reaches the non-const
// BoxEvent (no const_cast needed, unlike the old const FiberFuture::get).
// =============================================================================

// @unsafe - constructs a BoxEvent<T> through shared_ptr via Reactor internals.
template <typename T>
std::shared_ptr<BoxEvent<T>> fiber_make_state() {
  return Reactor::create_sp_event<BoxEvent<T>>();
}

// @unsafe - a null shared state, for a default/invalid FiberFuture.
template <typename T>
std::shared_ptr<BoxEvent<T>> fiber_null_state() {
  return std::shared_ptr<BoxEvent<T>>();
}

// @unsafe - shared_ptr deref through `(*state)->is_set_.get()` / `->set(value)`.
// Takes the value by `const T&` (BoxEvent::set copies regardless), so there
// is no extra copy for lvalue callers.
template <typename T>
void fiber_promise_set_value(const std::shared_ptr<BoxEvent<T>>* state, const T& value) {
  if (!*state) {
    throw std::logic_error("FiberPromise has no state (moved-from?)");
  }
  if ((*state)->is_set_.get()) {
    throw std::logic_error("FiberPromise value already set");
  }
  (*state)->set(value);
}

// @unsafe - shared_ptr deref through `(*state)->is_set_.get()`.
template <typename T>
bool fiber_promise_is_ready(const std::shared_ptr<BoxEvent<T>>* state) {
  return *state && (*state)->is_set_.get();
}

// @unsafe - blocks in `wait()` then returns a copy of the shared value.
template <typename T>
T fiber_future_get(const std::shared_ptr<BoxEvent<T>>* state) {
  if (!*state) {
    throw std::logic_error("FiberFuture has no state (invalid or moved-from?)");
  }
  if (!(*state)->is_set_.get()) {
    (*state)->wait();
  }
  return (*state)->get();
}

// @unsafe - bounded wait; shared_ptr deref through `(*state)->wait_timeout(timeout_us)`.
// `timeout_us == 0` blocks indefinitely (Event::wait's default).
template <typename T>
bool fiber_future_wait_for(const std::shared_ptr<BoxEvent<T>>* state, uint64_t timeout_us) {
  if (!*state) {
    return false;
  }
  if ((*state)->is_set_.get()) {
    return true;
  }
  (*state)->wait_timeout(timeout_us);
  return (*state)->is_set_.get();
}

// @unsafe - shared_ptr deref through `(*state)->is_set_.get()`.
template <typename T>
bool fiber_future_is_ready(const std::shared_ptr<BoxEvent<T>>* state) {
  return *state && (*state)->is_set_.get();
}

// @unsafe - presence check on the shared state (touches std::shared_ptr).
template <typename T>
bool fiber_future_valid(const std::shared_ptr<BoxEvent<T>>* state) {
  return *state != nullptr;
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
    state_: std::shared_ptr<BoxEvent<T>>,
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
/*RUSTYCPP:GEN-BEGIN id=future.fiber_promise version=1 rust_sha256=9b8f3650704187b3dc2aaadc1eb1d7720123711e41dbe7ec336a0c14d249dab6*/
template<typename T>
struct FiberPromise;

template<typename T>
struct FiberPromise {
    std::shared_ptr<BoxEvent<T>> state_;
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
    state_: std::shared_ptr<BoxEvent<T>>,
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
/*RUSTYCPP:GEN-BEGIN id=future.fiber_future version=1 rust_sha256=ff2490b968c7d3fcdff6595143e363fd02d1ccf368f3deebab2ee2db552b81ef*/
template<typename T>
struct FiberFuture;

template<typename T>
struct FiberFuture {
    std::shared_ptr<BoxEvent<T>> state_;
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
// second retrieval. The state copy is the only @unsafe step (shared_ptr).
template <typename T>
FiberFuture<T> fiber_promise_get_future(FiberPromise<T>& self) {
  if (self.future_retrieved_.get()) {
    throw std::logic_error("FiberFuture already retrieved from FiberPromise");
  }
  self.future_retrieved_.set(true);
  FiberFuture<T> f;
  f.state_ = self.state_;
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
