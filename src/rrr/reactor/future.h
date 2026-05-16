#pragma once

// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
/**
 * @file future.h
 * @brief FiberFuture/FiberPromise API for fiber-based asynchronous programming.
 *
 * This provides a clean interface for one-shot asynchronous value delivery,
 * following the FiberFuture/FiberPromise pattern used in many async frameworks.
 *
 * Key concepts:
 *   - FiberPromise<T>: Producer side - can set a value exactly once
 *   - FiberFuture<T>: Consumer side - can wait for and retrieve the value
 *   - A FiberPromise creates its paired FiberFuture via get_future()
 *
 * Example usage:
 *   // Producer fiber
 *   FiberPromise<int> promise;
 *   auto future = promise.get_future();
 *
 *   // ... pass future to consumer fiber ...
 *
 *   // Consumer waits for value
 *   int result = future.get();  // Blocks until value is set
 *
 *   // Producer sets value (unblocks consumer)
 *   promise.set_value(42);
 *
 * Thread Safety:
 *   - FiberPromise and FiberFuture share state via shared_ptr
 *   - Safe to pass FiberFuture across fibers
 *   - NOT safe for concurrent access from multiple threads
 *
 * Note: This header uses the rrr reactor system, NOT std::future/promise.
 */


#include <rusty/option.hpp>

import rrr.reactor;

namespace rrr {

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
  /**
   * Construct a new FiberPromise with shared state.
   *
   * @safe - Only creates shared_ptr and initializes event
   */
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
   * Get the FiberFuture associated with this FiberPromise.
   *
   * Can only be called once per FiberPromise. Subsequent calls throw.
   *
   * @return FiberFuture<T> that will receive the value set on this FiberPromise
   * @throws std::logic_error if called more than once
   *
   * @unsafe { Accesses shared state }
   */
  FiberFuture<T> get_future() {
    if (future_retrieved_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberFuture already retrieved from FiberPromise");
    }
    future_retrieved_ = true;
    return FiberFuture<T>(state_);
  }

  /**
   * Set the value, fulfilling the promise.
   *
   * Can only be called once. Subsequent calls throw.
   * This unblocks any fiber waiting on the associated FiberFuture.
   *
   * @param value The value to deliver
   * @throws std::logic_error if called more than once
   *
   * @unsafe { Modifies shared state }
   */
  void set_value(const T& value) {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberPromise has no state (moved-from?)");
    }
    if (state_->is_set_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberPromise value already set");
    }
    // @unsafe { BoxEvent::set modifies shared state }
    state_->set(value);
  }

  /**
   * Set the value with move semantics.
   *
   * @param value The value to deliver (moved)
   * @throws std::logic_error if called more than once
   *
   * @unsafe { Modifies shared state }
   */
  void set_value(T&& value) {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberPromise has no state (moved-from?)");
    }
    if (state_->is_set_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberPromise value already set");
    }
    // @unsafe { BoxEvent::set modifies shared state }
    state_->set(std::move(value));
  }

  /**
   * Check if a value has been set.
   *
   * @return true if set_value has been called
   *
   * @safe - Read-only access to boolean flag
   */
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
 * A FiberFuture can wait for and retrieve a value set by its paired FiberPromise.
 * The get() method blocks the current fiber until the value is available.
 *
 * @tparam T The type of value to receive
 *
 * Example:
 *   FiberFuture<int> future = promise.get_future();
 *
 *   // Later, in a fiber...
 *   int value = future.get();  // Blocks until promise.set_value() called
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
   * Wait for and retrieve the value.
   *
   * Blocks the current fiber until the associated FiberPromise sets a value.
   * Can be called multiple times - returns the same value each time.
   *
   * @return Reference to the value
   * @throws std::logic_error if FiberFuture is invalid (default constructed or moved-from)
   *
   * @unsafe { Blocks fiber, accesses shared state }
   */
  T& get() {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberFuture has no state (invalid or moved-from?)");
    }
    if (!state_->is_set_) {
      // @unsafe { Event::wait blocks current fiber }
      state_->wait();
    }
    return state_->get();
  }

  /**
   * Wait for and retrieve the value (const version).
   *
   * @return Const reference to the value
   * @throws std::logic_error if FiberFuture is invalid
   *
   * @unsafe { Blocks fiber, accesses shared state }
   */
  const T& get() const {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("FiberFuture has no state (invalid or moved-from?)");
    }
    if (!state_->is_set_) {
      // @unsafe { Event::wait blocks current fiber }
      const_cast<BoxEvent<T>*>(state_.get())->wait();
    }
    return state_->get();
  }

  /**
   * Wait for the value with timeout.
   *
   * @param timeout_us Timeout in microseconds (0 = no timeout)
   * @return true if value is ready, false if timed out
   *
   * @unsafe { Blocks fiber }
   */
  bool wait_for(uint64_t timeout_us) {
    if (!state_) {
      return false;
    }
    if (state_->is_set_) {
      return true;
    }
    // @unsafe { Event::wait with timeout }
    state_->wait(timeout_us);
    return state_->is_set_;
  }

  /**
   * Check if the value is ready (non-blocking).
   *
   * @return true if value has been set by FiberPromise
   *
   * @safe - Read-only access to boolean flag
   */
  bool is_ready() const noexcept {
    return state_ && state_->is_set_;
  }

  /**
   * Check if this FiberFuture is valid (has associated state).
   *
   * @return true if this FiberFuture can be waited on
   *
   * @safe - Null pointer check only
   */
  bool valid() const noexcept {
    return state_ != nullptr;
  }

 private:
  friend class FiberPromise<T>;

  // Private constructor - only FiberPromise can create valid Futures
  explicit FiberFuture(std::shared_ptr<BoxEvent<T>> state) : state_(std::move(state)) {}

  std::shared_ptr<BoxEvent<T>> state_;
};

// =============================================================================
// Convenience Factory Functions
// =============================================================================

/**
 * Create a FiberPromise/FiberFuture pair.
 *
 * This is a convenience function that creates both sides at once.
 *
 * @return std::pair<FiberPromise<T>, FiberFuture<T>>
 *
 * Example:
 *   auto [promise, future] = make_promise<int>();
 *   // Pass future to consumer, keep promise for producer
 *
 * @unsafe { Creates shared state }
 */
template <typename T>
std::pair<FiberPromise<T>, FiberFuture<T>> make_promise() {
  FiberPromise<T> promise;
  FiberFuture<T> future = promise.get_future();
  return {std::move(promise), std::move(future)};
}

/**
 * Create a FiberFuture that is immediately ready with a value.
 *
 * Useful for returning computed values from async interfaces.
 *
 * @param value The value to wrap
 * @return FiberFuture<T> that is already ready
 *
 * Example:
 *   FiberFuture<int> get_cached_or_compute() {
 *     if (have_cached) {
 *       return make_ready_future(cached_value);
 *     }
 *     // ... return future from async computation
 *   }
 *
 * @unsafe { Creates and sets shared state }
 */
template <typename T>
FiberFuture<T> make_ready_future(T value) {
  FiberPromise<T> promise;
  FiberFuture<T> future = promise.get_future();
  promise.set_value(std::move(value));
  return future;
}

}  // namespace rrr
