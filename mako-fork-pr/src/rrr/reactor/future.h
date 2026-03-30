/**
 * @file future.h
 * @brief Future/Promise API for fiber-based asynchronous programming.
 *
 * This provides a clean interface for one-shot asynchronous value delivery,
 * following the Future/Promise pattern used in many async frameworks.
 *
 * Key concepts:
 *   - Promise<T>: Producer side - can set a value exactly once
 *   - Future<T>: Consumer side - can wait for and retrieve the value
 *   - A Promise creates its paired Future via get_future()
 *
 * Example usage:
 *   // Producer fiber
 *   Promise<int> promise;
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
 *   - Promise and Future share state via shared_ptr
 *   - Safe to pass Future across fibers
 *   - NOT safe for concurrent access from multiple threads
 *
 * Note: This header uses the rrr reactor system, NOT std::future/promise.
 */

#pragma once

#include "event.h"
#include "reactor.h"
#include <rusty/option.hpp>
#include <stdexcept>

namespace rrr {

// Forward declaration
template <typename T>
class Future;

// =============================================================================
// Promise<T> - Producer side of async value delivery
// =============================================================================

/**
 * Promise<T> represents the producer side of a one-shot async channel.
 *
 * A Promise can set a value exactly once, which will unblock any fiber
 * waiting on the associated Future.
 *
 * @tparam T The type of value to deliver (must be copyable)
 *
 * Example:
 *   Promise<std::string> promise;
 *   auto future = promise.get_future();
 *
 *   // Later...
 *   promise.set_value("hello");  // Unblocks future.get()
 */
template <typename T>
class Promise {
 public:
  /**
   * Construct a new Promise with shared state.
   *
   * @safe - Only creates shared_ptr and initializes event
   */
  Promise() : state_(Reactor::create_sp_event<BoxEvent<T>>()) {}

  // Non-copyable (each promise is unique)
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  // Movable
  Promise(Promise&& other) noexcept : state_(std::move(other.state_)) {}
  Promise& operator=(Promise&& other) noexcept {
    state_ = std::move(other.state_);
    return *this;
  }

  /**
   * Get the Future associated with this Promise.
   *
   * Can only be called once per Promise. Subsequent calls throw.
   *
   * @return Future<T> that will receive the value set on this Promise
   * @throws std::logic_error if called more than once
   *
   * @unsafe { Accesses shared state }
   */
  Future<T> get_future() {
    if (future_retrieved_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("Future already retrieved from Promise");
    }
    future_retrieved_ = true;
    return Future<T>(state_);
  }

  /**
   * Set the value, fulfilling the promise.
   *
   * Can only be called once. Subsequent calls throw.
   * This unblocks any fiber waiting on the associated Future.
   *
   * @param value The value to deliver
   * @throws std::logic_error if called more than once
   *
   * @unsafe { Modifies shared state }
   */
  void set_value(const T& value) {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("Promise has no state (moved-from?)");
    }
    if (state_->is_set_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("Promise value already set");
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
      throw std::logic_error("Promise has no state (moved-from?)");
    }
    if (state_->is_set_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("Promise value already set");
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
// Future<T> - Consumer side of async value delivery
// =============================================================================

/**
 * Future<T> represents the consumer side of a one-shot async channel.
 *
 * A Future can wait for and retrieve a value set by its paired Promise.
 * The get() method blocks the current fiber until the value is available.
 *
 * @tparam T The type of value to receive
 *
 * Example:
 *   Future<int> future = promise.get_future();
 *
 *   // Later, in a fiber...
 *   int value = future.get();  // Blocks until promise.set_value() called
 */
template <typename T>
class Future {
 public:
  // Default constructor creates invalid future
  Future() = default;

  // Non-copyable (use shared_future for multiple consumers)
  Future(const Future&) = delete;
  Future& operator=(const Future&) = delete;

  // Movable
  Future(Future&& other) noexcept : state_(std::move(other.state_)) {}
  Future& operator=(Future&& other) noexcept {
    state_ = std::move(other.state_);
    return *this;
  }

  /**
   * Wait for and retrieve the value.
   *
   * Blocks the current fiber until the associated Promise sets a value.
   * Can be called multiple times - returns the same value each time.
   *
   * @return Reference to the value
   * @throws std::logic_error if Future is invalid (default constructed or moved-from)
   *
   * @unsafe { Blocks fiber, accesses shared state }
   */
  T& get() {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("Future has no state (invalid or moved-from?)");
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
   * @throws std::logic_error if Future is invalid
   *
   * @unsafe { Blocks fiber, accesses shared state }
   */
  const T& get() const {
    if (!state_) {
      // @unsafe { std::logic_error constructor }
      throw std::logic_error("Future has no state (invalid or moved-from?)");
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
   * @return true if value has been set by Promise
   *
   * @safe - Read-only access to boolean flag
   */
  bool is_ready() const noexcept {
    return state_ && state_->is_set_;
  }

  /**
   * Check if this Future is valid (has associated state).
   *
   * @return true if this Future can be waited on
   *
   * @safe - Null pointer check only
   */
  bool valid() const noexcept {
    return state_ != nullptr;
  }

 private:
  friend class Promise<T>;

  // Private constructor - only Promise can create valid Futures
  explicit Future(std::shared_ptr<BoxEvent<T>> state) : state_(std::move(state)) {}

  std::shared_ptr<BoxEvent<T>> state_;
};

// =============================================================================
// Convenience Factory Functions
// =============================================================================

/**
 * Create a Promise/Future pair.
 *
 * This is a convenience function that creates both sides at once.
 *
 * @return std::pair<Promise<T>, Future<T>>
 *
 * Example:
 *   auto [promise, future] = make_promise<int>();
 *   // Pass future to consumer, keep promise for producer
 *
 * @unsafe { Creates shared state }
 */
template <typename T>
std::pair<Promise<T>, Future<T>> make_promise() {
  Promise<T> promise;
  Future<T> future = promise.get_future();
  return {std::move(promise), std::move(future)};
}

/**
 * Create a Future that is immediately ready with a value.
 *
 * Useful for returning computed values from async interfaces.
 *
 * @param value The value to wrap
 * @return Future<T> that is already ready
 *
 * Example:
 *   Future<int> get_cached_or_compute() {
 *     if (have_cached) {
 *       return make_ready_future(cached_value);
 *     }
 *     // ... return future from async computation
 *   }
 *
 * @unsafe { Creates and sets shared state }
 */
template <typename T>
Future<T> make_ready_future(T value) {
  Promise<T> promise;
  Future<T> future = promise.get_future();
  promise.set_value(std::move(value));
  return future;
}

}  // namespace rrr
