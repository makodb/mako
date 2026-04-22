module;

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/rusty.hpp>
/**
 * @file fiber_impl.h
 * @brief Core stackful fiber implementation.
 *
 * This file contains the Fiber class, which provides stackful execution
 * contexts without Boost.Coroutine2. "Fiber" is the preferred terminology
 * for stackful fibers, aligning with industry conventions.
 *
 * For the modern API, see fiber.h which provides:
 *   - this_fiber namespace (like std::this_thread)
 *   - Future/Promise for async value delivery
 *   - WaitAll/WaitAny/WaitN event combinators
 *
 * Fiber is the primary class for stackful fibers.
 */


#include <rusty/box.hpp>
#include <rusty/rc.hpp>
#include <rusty/option.hpp>
#include <rusty/cell.hpp>
#include <rusty/refcell.hpp>
#include <rusty/function.hpp>


export module rrr:reactor.fiber_impl;

import std;


export namespace rrr {

// Forward declaration
class Fiber;

/**
 * CPU context for x86_64 SysV user-space context switching.
 *
 * Stores callee-saved registers plus stack/instruction pointer.
 */
struct FiberContext {
  void* rsp{nullptr};
  void* rip{nullptr};
  std::uintptr_t rbx{0};
  std::uintptr_t rbp{0};
  std::uintptr_t r12{0};
  std::uintptr_t r13{0};
  std::uintptr_t r14{0};
  std::uintptr_t r15{0};
};

extern "C" void fiber_swap_context(FiberContext* from, FiberContext* to);

class fiber_task_t;

class fiber_yield_t {
 public:
  explicit fiber_yield_t(fiber_task_t& task) : task_(&task) {}
  void operator()();

 private:
  fiber_task_t* task_{nullptr};
};

class fiber_task_t {
 public:
  using TaskFn = rusty::Function<void(fiber_yield_t&)>;

  explicit fiber_task_t(TaskFn fn);

  template <typename Fn,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, fiber_task_t>>>
  explicit fiber_task_t(Fn&& fn)
      : fiber_task_t(TaskFn(std::forward<Fn>(fn))) {}

  ~fiber_task_t();

  fiber_task_t(const fiber_task_t&) = delete;
  fiber_task_t& operator=(const fiber_task_t&) = delete;
  fiber_task_t(fiber_task_t&&) = delete;
  fiber_task_t& operator=(fiber_task_t&&) = delete;

  void operator()();

 private:
  friend class fiber_yield_t;

  enum class State : uint8_t {
    NEW = 0,
    RUNNING,
    SUSPENDED,
    FINISHED
  };

  static constexpr std::size_t kDefaultStackBytes = 1u << 20;  // 1 MiB
  static thread_local fiber_task_t* tls_active_task_;

  static void entry_trampoline();
  [[noreturn]] void entry();

  void init_context();
  void resume();
  void yield_to_caller();

  TaskFn fn_;
  fiber_yield_t yield_;
  FiberContext caller_ctx_{};
  FiberContext fiber_ctx_{};
  void* stack_mapping_{nullptr};
  std::size_t stack_mapping_bytes_{0};
  State state_{State::NEW};
};

class Reactor;
class Event;

/**
 * Fiber - A stackful fiber (execution context).
 *
 * Fibers provide cooperative multitasking within a single thread.
 * They are scheduled by the Reactor and can yield/resume execution.
 *
 * Key differences from C++20 coroutines:
 *   - C++20 coroutines are stackless (state machines)
 *   - Fibers use custom stackful execution
 *   - Stackful contexts are properly called "fibers"
 *
 * @unsafe - Uses rusty::Rc ownership and mutable fields for interior mutability
 */
class Fiber {
 public:
  /**
   * Get the currently executing fiber.
   *
   * @return Some(fiber) if in fiber context, None otherwise
   */
  static rusty::Option<rusty::Rc<Fiber>> current_fiber();

  /**
   * Create and run a new fiber with the given function.
   *
   * @param func The function to execute in the fiber
   * @param file Source file (for debugging)
   * @param line Source line (for debugging)
   * @return Rc<Fiber> handle to the created fiber
   *
   * @safe - Wraps callable and delegates to create_run_impl. Memory-safe:
   *   - rusty::Function safely captures the callable
   *   - Returns rusty::Rc for safe reference counting
   *   - Internal fiber state is managed by Reactor
   */
  template <typename Func>
  static rusty::Rc<Fiber> create_run(Func&& func, const char* file = "", int64_t line = 0) {
    // @unsafe - create_run_impl uses raw pointer operations internally
    { return create_run_impl(rusty::Function<void()>(std::forward<Func>(func)), file, line); }
  }

  /**
   * Sleep the current fiber for the specified duration.
   *
   * @param microseconds Duration to sleep in microseconds
   */
  static void sleep(uint64_t microseconds);

  static thread_local uint64_t global_id;
  uint64_t dep_id_{0};
  bool need_finalize_{false};
  uint64_t id{0};

  enum Status { INIT = 0, STARTED, PAUSED, RESUMED, FINISHED, FINALIZING, RECYCLED };

  // Interior mutability using Cell/RefCell for use with rusty::Rc
  // Cell<T> for Copy types, RefCell<T> for non-Copy types
  rusty::Cell<Status> status_{INIT};
  rusty::Cell<bool> needs_finalize_{false};
  rusty::RefCell<rusty::Function<void()>> func_{};

  // Uses rusty::Box with Option for nullable semantics
  rusty::RefCell<rusty::Option<rusty::Box<fiber_task_t>>> fiber_task_{};
  // Non-owning pointer to the yield handle owned by fiber_task_t.
  // Cell provides interior mutability for const yield_().
  rusty::Cell<fiber_yield_t*> fiber_yield_{nullptr};

  Fiber() = delete;
  explicit Fiber(rusty::Function<void()> func);
  ~Fiber();

  // @unsafe - Uses std::bind and function pointers
  void run_wrapper(fiber_yield_t& yield);

  /**
   * Initialize and start the fiber.
   * @safe - Uses Cell/RefCell for interior mutability, Box for ownership.
   */
  void run() const;

  /**
   * Yield control back to the reactor.
   * @safe - Uses non-owning yield pointer and Cell for status.
   */
  void yield_() const;

  /**
   * Resume a paused fiber.
   * @safe - Uses RefCell for fiber_task_ access.
   */
  void continue_() const;

  bool finished() const;
  void do_finalize();

  // Comparison operator for rusty::BTreeSet<rusty::Rc<Fiber>>
  friend bool operator<(const rusty::Rc<Fiber>& lhs, const rusty::Rc<Fiber>& rhs) {
    return lhs.get() < rhs.get();
  }

 private:
  // @unsafe - Creates and runs a new fiber (uses raw pointer operations)
  static rusty::Rc<Fiber> create_run_impl(rusty::Function<void()> func, const char* file, int64_t line);
};

} // namespace rrr
