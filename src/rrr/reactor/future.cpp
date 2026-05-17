// rrr.future — FiberPromise<T> / FiberFuture<T> (formerly future.h).
//
// One-shot async value delivery between fibers; producer side
// (FiberPromise) calls `set_value` exactly once, consumer side
// (FiberFuture) blocks in `get()` until the value is delivered.
// Wraps `rrr::BoxEvent<T>` for the underlying wait/notify; see
// rrr.reactor for the event primitive.
module;

#include <cstddef>
#include <cstdint>

#include <rusty/option.hpp>

export module rrr.future;

import std;
import rrr.reactor;

export namespace rrr {

// Forward declaration
template <typename T>
class FiberFuture;

// =============================================================================
// FiberPromise<T> - Producer side of async value delivery
// =============================================================================

/**
 * FiberPromise<T> represents the producer side of a one-shot async channel.
 *
 * A FiberPromise can set a value exactly once, which will unblock any fiber
 * waiting on the associated FiberFuture.
 *
 * @tparam T The type of value to deliver (must be copyable)
 *
 * Example:
 *   FiberPromise<std::string> promise;
 *   auto future = promise.get_future();
 *
 *   // Later...
 *   promise.set_value("hello");  // Unblocks future.get()
 */
template <typename T>
class FiberPromise {
 public:
  FiberPromise() : state_(Reactor::create_sp_event<BoxEvent<T>>()) {}

  // Non-copyable (each promise is unique)
  FiberPromise(const FiberPromise&) = delete;
  FiberPromise& operator=(const FiberPromise&) = delete;

  // Movable
  FiberPromise(FiberPromise&& other) noexcept : state_(std::move(other.state_)) {}
  FiberPromise& operator=(FiberPromise&& other) noexcept {
    state_ = std::move(other.state_);
    return *this;
  }

  /**
   * Get the FiberFuture associated with this FiberPromise. Can only be
   * called once per FiberPromise; subsequent calls throw.
   */
  FiberFuture<T> get_future() {
    if (future_retrieved_) {
      throw std::logic_error("FiberFuture already retrieved from FiberPromise");
    }
    future_retrieved_ = true;
    return FiberFuture<T>(state_);
  }

  /**
   * Set the value, fulfilling the promise. Can only be called once;
   * subsequent calls throw. Unblocks any fiber waiting on the future.
   */
  void set_value(const T& value) {
    if (!state_) {
      throw std::logic_error("FiberPromise has no state (moved-from?)");
    }
    if (state_->is_set_) {
      throw std::logic_error("FiberPromise value already set");
    }
    state_->set(value);
  }

  /** Move-flavoured `set_value`. */
  void set_value(T&& value) {
    if (!state_) {
      throw std::logic_error("FiberPromise has no state (moved-from?)");
    }
    if (state_->is_set_) {
      throw std::logic_error("FiberPromise value already set");
    }
    state_->set(std::move(value));
  }

  bool is_ready() const noexcept {
    return state_ && state_->is_set_;
  }

 private:
  std::shared_ptr<BoxEvent<T>> state_;
  bool future_retrieved_{false};
};

// =============================================================================
// FiberFuture<T> - Consumer side of async value delivery
// =============================================================================

/**
 * FiberFuture<T> represents the consumer side of a one-shot async channel.
 *
 * A FiberFuture waits for and retrieves a value set by its paired
 * FiberPromise. `get()` blocks the current fiber until the value is
 * available.
 *
 * @tparam T The type of value to receive
 */
template <typename T>
class FiberFuture {
 public:
  // Default constructor creates invalid future
  FiberFuture() = default;

  // Non-copyable (use shared_future for multiple consumers)
  FiberFuture(const FiberFuture&) = delete;
  FiberFuture& operator=(const FiberFuture&) = delete;

  // Movable
  FiberFuture(FiberFuture&& other) noexcept : state_(std::move(other.state_)) {}
  FiberFuture& operator=(FiberFuture&& other) noexcept {
    state_ = std::move(other.state_);
    return *this;
  }

  /**
   * Wait for and retrieve the value. Blocks the current fiber until the
   * paired FiberPromise sets a value. Can be called multiple times —
   * returns the same value each time.
   */
  T& get() {
    if (!state_) {
      throw std::logic_error("FiberFuture has no state (invalid or moved-from?)");
    }
    if (!state_->is_set_) {
      state_->wait();
    }
    return state_->get();
  }

  /** Const-flavoured `get`. */
  const T& get() const {
    if (!state_) {
      throw std::logic_error("FiberFuture has no state (invalid or moved-from?)");
    }
    if (!state_->is_set_) {
      const_cast<BoxEvent<T>*>(state_.get())->wait();
    }
    return state_->get();
  }

  /**
   * Wait for the value with timeout. Returns true if ready, false if
   * timed out. `timeout_us == 0` means no timeout (block indefinitely).
   */
  bool wait_for(uint64_t timeout_us) {
    if (!state_) {
      return false;
    }
    if (state_->is_set_) {
      return true;
    }
    state_->wait(timeout_us);
    return state_->is_set_;
  }

  bool is_ready() const noexcept {
    return state_ && state_->is_set_;
  }

  bool valid() const noexcept {
    return state_ != nullptr;
  }

 private:
  friend class FiberPromise<T>;

  // Private constructor — only FiberPromise can create valid Futures.
  explicit FiberFuture(std::shared_ptr<BoxEvent<T>> state) : state_(std::move(state)) {}

  std::shared_ptr<BoxEvent<T>> state_;
};

// =============================================================================
// Convenience Factory Functions
// =============================================================================

/**
 * Create a FiberPromise/FiberFuture pair in one call.
 */
template <typename T>
std::pair<FiberPromise<T>, FiberFuture<T>> make_promise() {
  FiberPromise<T> promise;
  FiberFuture<T> future = promise.get_future();
  return {std::move(promise), std::move(future)};
}

/**
 * Create a FiberFuture that is immediately ready with a value.
 * Useful for returning computed values from async interfaces.
 */
template <typename T>
FiberFuture<T> make_ready_future(T value) {
  FiberPromise<T> promise;
  FiberFuture<T> future = promise.get_future();
  promise.set_value(std::move(value));
  return future;
}

}  // export namespace rrr
