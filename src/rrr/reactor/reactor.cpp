// rrr.reactor — consolidated event/fiber/reactor module.
//
// Combines what were event.h+.cc, fiber_impl.h+.cc, quorum_event.h+.cc,
// reactor.h+.cc, and fiber_context_runtime.cc into a single C++23
// named-module interface unit. The cluster's class types
// (`rrr::Event`, `rrr::Fiber`, `rrr::Reactor`, `janus::QuorumEvent`,
// `rrr::fiber_task_t`, etc.) form a mutually-recursive web of forward
// declarations and out-of-line member definitions; a single module unit
// is the natural shape (separate module interfaces would require
// circular `import` lines, which the standard forbids).
//
// The arch-specific context-switch trampolines stay outside the module
// — `fiber_context_x86_64.cc` and `fiber_context_aarch64.cc` are tiny
// `extern "C"` asm-only TUs and don't need module attachment.
module;

#include <std_compat.hpp>
#include <std_annotation.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/times.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include <rusty/arc.hpp>
#include <rusty/async.hpp>
#include <rusty/box.hpp>
#include <rusty/btreeset.hpp>
#include <rusty/cell.hpp>
#include <rusty/fn.hpp>
#include <rusty/function.hpp>
#include <rusty/mutex.hpp>
#include <rusty/option.hpp>
#include <rusty/rc.hpp>
#include <rusty/refcell.hpp>
#include <rusty/rusty.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/sync/mpsc.hpp>
#include <rusty/thread.hpp>
#include <rusty/vecdeque.hpp>

export module rrr.reactor;

import std;
import rrr.basetypes;
import rrr.debugging;
import rrr.logging;
import rrr.misc;
import rrr.strop;
import rrr.threading;
import rrr.epoll_wrapper;
import rrr.pollable_proxy;


// ===========================================================================
// Class declarations (from former event.h, fiber_impl.h, reactor.h block 1)
// ===========================================================================
// @safe - Reactor / Event / Fiber declarations. Class declarations
// carry their own annotations; methods that genuinely cross into
// fiber context switching / raw pointer access / thread-local lookup
// have per-method `// @unsafe` overrides. The rest is analyzed as
// @safe by default.
export namespace rrr {

// --- from event.h --------------------------------------------------------

class Reactor;
class Fiber;
class Event {
 protected:
  // Self-reference for adding to queues (using weak_ptr for shared ownership)
  // Set by CreateSpEvent after construction
  std::weak_ptr<Event> self_;
//class Event {
 public:
  int __debug_creator{0};
  enum EventStatus { INIT = 0, WAIT = 1, READY = 2,
      DONE = 3, TIMEOUT = 4, DEBUG};

#ifdef EVENT_TIMEOUT_CHECK
  bool __debug_timeout_{false};
#endif
  rusty::Cell<EventStatus> status_{INIT};
  void* _dbg_p_scheduler_{nullptr};  // Jetpack: for debugging
  uint64_t type_{0};
  rusty::Function<bool(int)> test_{};
	bool needs_finalize_{false};
  uint64_t wakeup_time_; // calculated by timeout, unit: microsecond
  bool rcd_wait_ = false;
  std::string wait_place_{"not recorded"};
  bool in_waiting_list_{false};

  // An event is usually allocated on a fiber stack, thus it cannot own a
  //   shared_ptr to the fiber it is.
  // In this case there is no shared pointer to the event.
  // When the stack that contains the event frees, the event frees.
  // Weak reference to a fiber using rusty::rc::Weak with proper reference counting
  rusty::rc::Weak<Fiber> wp_fiber_{};

  // @unsafe
  virtual void wait(uint64_t timeout=0) final;

  void wait(rusty::Function<bool(int)> f) {
    test_ = std::move(f);
    wait();
  }

  virtual void log(){return;}
  virtual uint64_t get_fiber_id();
  void record_place(const char* file, int line);

  // @safe - Tests if event is ready
  virtual bool test();
  virtual bool is_slow();
  virtual bool is_ready() {
    if (!test_) return false;
    return test_(0);
  }

  // Composite events (WaitAll, WaitAny, QuorumEvent) need periodic polling
  // Added at END to preserve vtable layout for binary compatibility
  virtual bool is_composite_event() { return false; }

  // Self-reference management (uses shared_ptr for polymorphism support)
  void set_self(std::weak_ptr<Event> self) { self_ = self; }
  std::shared_ptr<Event> get_self() const { return self_.lock(); }

  friend Reactor;
// protected:
  Event();
};

template <class Type>
class BoxEvent : public Event {
 public:
  Type content_{};
  bool is_set_{false};
  Type& get() {
    return content_;
  }
  void set(const Type& c) {
    is_set_ = true;
    content_ = c;
    test();
  }
  void clear() {
    is_set_ = false;
    content_ = {};
  }
  virtual bool is_ready() override {
    return is_set_;
  }
};

class IntEvent : public Event {

 public:
  IntEvent() {}
  IntEvent(int tar) :target_(tar) {}
  int value_{0};
  int target_{1};


  bool test_trigger();

  int get() {
    return value_;
  }

  // @safe - integer assignment + virtual `test()` (itself @safe).
  int set(int n) {
    int t = value_;
    value_ = n;
    // test_trigger();
    test();
    return t;
  };

  bool is_ready() override {
    if (test_) {
      return test_(value_);
    } else {
      return (value_ >= target_);
    }
  }
};

class SharedIntEvent {
 public:
  int value_{};
  rusty::Vec<std::shared_ptr<IntEvent>> events_;
  // Declaration only - definition in event.cc
  int set(const int& v);

  void wait(rusty::Function<bool(int)> f);
  bool wait_until_gte(int x, int timeout=0);
};


class NeverEvent: public Event {
 public:
  bool is_ready() override {
    return false;
  }
};

class TimeoutEvent : public Event {
 public:
  uint64_t wakeup_time_{0};
  uint64_t wait_us_{0};
  TimeoutEvent(uint64_t wait_us)
      : wakeup_time_{Time::now(true) + wait_us}, wait_us_(wait_us) {}

  bool is_ready() override {
//    Log_debug("test timeout");
    return (Time::now(true) > wakeup_time_);
  }

  // @unsafe
  void wait() {
    Event::wait(wait_us_);
  }
};

class WaitAny : public Event {
 public:
  rusty::Vec<std::shared_ptr<Event>> events_;

  void add_event() {
    // empty func for recursive variadic parameters
  }

  template<typename X, typename... Args>
  void add_event(X& x, Args&... rest) {
    events_.push(x);
    add_event(rest...);
  }

  template<typename... Args>
  WaitAny(Args&&... args) {
    add_event(args...);
  }

  bool is_ready() override {
    for (const auto& e : events_) {
      if (e && e->is_ready()) {
        return true;
      }
    }
    return false;
  }

  // Mark as composite event - will be polled in reactor loop
  bool is_composite_event() override { return true; }
};

class WaitAll : public Event {
 public:
  rusty::Vec<std::shared_ptr<Event>> events_;

  // Default constructor (mako-dev)
  WaitAll() {}

  // Constructor for vector of events
  explicit WaitAll(const rusty::Vec<std::shared_ptr<Event>>& evs) {
    events_.reserve(evs.len());
    for (const auto& ev : evs) {
      events_.push(ev);
    }
  }

  void add_event() {
    // empty func for recursive variadic parameters
  }

  template<typename... Args>
  void add_event(std::shared_ptr<Event> x, Args... rest) {
    events_.push(std::move(x));
    add_event(rest...);
  }

  template<typename... Args>
  WaitAll(std::shared_ptr<Event> first, Args... rest) {
    add_event(std::move(first), rest...);
  }

  void log() override {
    for(size_t i = 0; i < events_.len(); i++){
      events_[i]->log();
    }
  }

  bool is_ready() override {
    // All events must be ready (or DONE) for WaitAll to be ready.
    for (const auto& e : events_) {
      if (!e) {
        return false;
      }
      if (!(e->is_ready() || e->status_.get() == Event::DONE)) {
        return false;
      }
    }
    return true;
  }

  // Mark as composite event - will be polled in reactor loop
  bool is_composite_event() override { return true; }
};

class WaitN : public Event {
 public:
  rusty::Vec<std::shared_ptr<Event>> events_;
  int number;

  void add_event() {
    // empty func for recursive variadic parameters
  }

  template<typename... Args>
  void add_event(std::shared_ptr<Event> x, Args... rest) {
    events_.push(std::move(x));
    add_event(rest...);
  }

  template<typename... Args>
  WaitN(std::shared_ptr<Event> first, Args... rest) {
    add_event(std::move(first), rest...);
  }

  bool is_ready() override {
    int count = 0;
    for(auto index = events_.begin(); index != events_.end(); index++){
      if((*index)->is_ready()){
        count++;
        if(count == number){
          return true;
        }
      }
    }
    return false;
  }
};

class DispatchEvent: public Event{
  public:
    uint32_t n_dispatch_;
    uint32_t n_dispatch_ack_ = 0;
    rusty::BTreeMap<uint32_t, bool> dispatch_acks_ = {};
    bool aborted_ = false;
    bool more = false;

    DispatchEvent() : Event(){

    }

    bool is_ready() override{
      if(n_dispatch_ == n_dispatch_ack_){
        if(aborted_){
          return true;
        }
        else{
          for (const auto& [_, acked] : dispatch_acks_) {
            if (!acked) {
              return false;
            }
          }
          return true;
        }
      }
      else if(more){
        return true;
      }
      return false;
    }

};

class SingleRPCEvent: public Event{
  public:
    uint32_t cli_id_;
    uint32_t coo_id_;
    int32_t& res_;
    std::string log_file = "logs.txt";
    rusty::HashSet<int> dep{};
    SingleRPCEvent(uint32_t cli_id, int32_t res): Event(),
                                                   cli_id_(cli_id),
                                                   res_(res){
    }
    void add_dep(int tgtId){
      if (!dep.contains(tgtId)) {
        dep.insert(tgtId);
      }
    }
    void log() override {
      std::ofstream of(log_file, std::fstream::app);
      //of << "hello\n";
      of << "{ " << cli_id_ << ": ";
      for(auto it = dep.begin(); it != dep.end(); ++it){
        of << *it << " ";
      }
      of << "}\n";
      of.close();
    }
    bool is_ready() override{
      // SUCCESS=0, REJECT=-10 (macros removed to avoid conflict with mako ErrorCode)
      return res_ == 0 || res_ == -10;
    }
};


// --- from fiber_impl.h ---------------------------------------------------

// Forward declaration
class Fiber;

/**
 * CPU context for x86_64 SysV user-space context switching.
 *
 * Stores callee-saved registers plus stack/instruction pointer.
 */
struct FiberContext {
#if defined(__x86_64__)
  void* rsp{nullptr};
  void* rip{nullptr};
  std::uintptr_t rbx{0};
  std::uintptr_t rbp{0};
  std::uintptr_t r12{0};
  std::uintptr_t r13{0};
  std::uintptr_t r14{0};
  std::uintptr_t r15{0};
#elif defined(__aarch64__)
  // AAPCS64 callee-saved: x19-x28, x29 (fp), x30 (lr used as pc), sp.
  void* sp{nullptr};          // offset  0
  void* pc{nullptr};          // offset  8 (lr on entry = resume address)
  std::uintptr_t x19{0};      // offset 16
  std::uintptr_t x20{0};      // offset 24
  std::uintptr_t x21{0};      // offset 32
  std::uintptr_t x22{0};      // offset 40
  std::uintptr_t x23{0};      // offset 48
  std::uintptr_t x24{0};      // offset 56
  std::uintptr_t x25{0};      // offset 64
  std::uintptr_t x26{0};      // offset 72
  std::uintptr_t x27{0};      // offset 80
  std::uintptr_t x28{0};      // offset 88
  std::uintptr_t fp{0};       // offset 96 (x29)
#endif
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
 * QUARANTINE — the stackful-fiber context-switch primitive lives in
 * `fiber_context_{x86_64,aarch64}.cc` as raw assembly, and is invoked
 * through `fiber_task_t::resume()`/`yield_to_caller()`/`entry()` in
 * the impl section of this file. Those callers carry `// @unsafe`
 * annotations.
 *
 * Public API on Fiber (`run`, `yield_`, `continue_`, `create_run`,
 * `current_fiber`, `sleep`, ctor/dtor/finished/do_finalize) is `@safe`
 * — callers can use it from @safe code. Each method's body wraps its
 * genuinely-unsafe internals (Rc/RefCell unwrap, fiber-runtime dispatch,
 * std::bind / function-pointer construction) in inline `@unsafe { ... }`
 * blocks. Two implementation-detail methods stay `@unsafe`:
 *   - `run_wrapper(yield)`: invoked from the asm trampoline; the
 *     contract is fixed.
 *   - `create_run_impl(...)`: builds the heap-allocated task via raw
 *     `new chunk` shapes the analyzer can't yet see through.
 */
// @safe
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


// --- from reactor.h (block 1: Reactor, PollCommand) ----------------------

// Note: Fiber is the primary class (defined in fiber_impl.h)
// The full definition is available via #include "fiber_impl.h" above

/**
 * @class Reactor
 * @brief Thread-local event loop and fiber scheduler
 *
 * MEMORY SAFETY MODEL:
 *
 * 1. THREAD AFFINITY
 *    - Each Reactor is pinned to its creating thread via thread_id_
 *    - Loop() verifies thread ownership at entry
 *    - Thread-local storage (sp_reactor_th_) prevents cross-thread access
 *
 * 2. INTERIOR MUTABILITY (RustyCpp Patterns)
 *    - Cell<T>: Used for primitive counters and flags (looping_, thread_id_, etc.)
 *    - RefCell<T>: Used for complex types (sp_running_fiber_th_)
 *    - mutable containers: STL containers with const method access
 *
 * 3. SMART POINTER USAGE
 *    - Rc<Fiber>: Single-threaded reference counting for fibers
 *    - shared_ptr<Event>: For polymorphic event types
 *    - Weak<Fiber>: Events hold weak refs to avoid cycles
 *
 * 4. SYNCHRONIZATION
 *    - All reactor operations are single-threaded (no locks needed)
 *
 * SAFETY INVARIANTS:
 * - One active fiber per reactor at any time
 * - Events never outlive their fibers (weak refs)
 * - Loop() only called from owning thread
 */
// @safe - Single-threaded reactor; data lives in RefCell / Cell / Rc /
// HashMap with rusty borrow rules. Methods that genuinely cross into
// unsafe territory (fiber context switching via Fiber::yield_ /
// continue_, raw pointer access through the class-static thread_local
// fields, get_reactor returning thread-local Rc) carry their own
// `// @unsafe` overrides; the rest is now analyzed as @safe by default.
class Reactor {
 public:
  // Default constructor - all fields have default constructors
  Reactor() = default;

  // Delete copy and move constructors (RefCell and Cell are not copyable/movable)
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;
  Reactor(Reactor&&) = delete;
  Reactor& operator=(Reactor&&) = delete;

  // @unsafe - Returns thread-local reactor instance with single-threaded Rc
  // SAFETY: Thread-local storage, single-threaded access only
  static rusty::Rc<Reactor> get_reactor();
  // @unsafe - Returns thread-local disk reactor instance
  static rusty::Rc<Reactor> get_disk_reactor();
  // `inline` keeps these in vague linkage. Without it, clang 21 emits the
  // module-attached class-static thread_local storage as a strong external
  // in every TU that uses it via an inline accessor, causing duplicate-
  // definition linker errors. clang 22 happened to avoid this; we use
  // `inline` to make the linkage explicit and toolchain-independent.
  static inline thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_{};
  static inline thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_{};
  static inline thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_fiber_th_{};

  // Jetpack: Server ID for logging/debugging (set by server_worker.cc)
  // Using Cell for safe interior mutability (int is trivially copyable)
  rusty::Cell<int> server_id_{0};

  /**
   * A reactor needs to keep reference to all fibers created,
   * in case it is freed by the caller after a yield.
   */
  // Events managed with std::shared_ptr<Event> for polymorphism support
  // Using RefCell<VecDeque> for safe interior mutability in const methods
  rusty::RefCell<rusty::VecDeque<std::shared_ptr<Event>>> all_events_{};
  rusty::RefCell<rusty::VecDeque<std::shared_ptr<Event>>> waiting_events_{};
  rusty::RefCell<rusty::VecDeque<std::shared_ptr<Event>>> timeout_events_{};
  rusty::RefCell<rusty::VecDeque<std::shared_ptr<Event>>> composite_events_{}; // WaitAll, WaitAny, QuorumEvent
  // Note: network_events_ and ready_network_events_ were removed as dead code (never used)
  // Fibers managed with single-threaded Rc
  // Using rusty::BTreeSet for @safe contains() checks
  // Using RefCell for safe interior mutability in const methods
  rusty::RefCell<rusty::BTreeSet<rusty::Rc<Fiber>>> fibers_{};
  rusty::RefCell<rusty::Vec<rusty::Rc<Fiber>>> available_fibers_{};
  // Note: processors_ and opened_files_ were removed as dead code (never used)
  // `inline` keeps these in vague linkage — see sp_reactor_th_ above for why.
  // (Function-local-static accessor `clients()` was used during the
  // module-attached TLS dup-symbol investigation; the `static inline
  // thread_local` pattern at class scope is the cleaner equivalent fix
  // that matches sp_reactor_th_ et al.)
  static inline thread_local rusty::HashMap<std::string, rusty::Vec<PollableProxy>> clients_{};
  static inline thread_local rusty::HashSet<std::string> dangling_ips_{};
  // Interior mutability using Cell<T> for safe const method access
  rusty::Cell<bool> looping_{false};
  rusty::Cell<bool> slow_{false};
  rusty::Cell<int> slow_count_{0};
  rusty::Cell<int> trying_count_{0};
  rusty::Cell<rusty::thread::ThreadId> thread_id_{};
  // Jetpack fiber counters - using Cell for interior mutability
  rusty::Cell<int64_t> n_created_fibers_{0};
  rusty::Cell<int64_t> n_busy_fibers_{0};
  rusty::Cell<int64_t> n_active_fibers_{0};
  rusty::Cell<int64_t> n_active_fibers_2_{0};
  rusty::Cell<int64_t> n_idle_fibers_{0};
  // Stackless coroutine task slots managed by the reactor loop.
  // `poll_once` is move-only (rusty::Function): `process_stackless_tasks`
  // moves the function out of its slot before invoking it (so the
  // reactor's RefCell guards on `stackless_tasks_` aren't held across
  // user code), then moves it back if the poll didn't return Ready.
  struct StacklessTaskEntry {
    bool active = false;
    bool queued = false;
    rusty::Function<bool(rusty::Context&)> poll_once;
  };
  rusty::RefCell<rusty::Vec<StacklessTaskEntry>> stackless_tasks_{};
  rusty::RefCell<rusty::Vec<size_t>> free_stackless_task_slots_{};
  rusty::RefCell<rusty::VecDeque<size_t>> ready_stackless_tasks_{};
  static SpinLock trying_job_;
#if defined(REUSE_FIBER) || defined(REUSE_CORO)
#define REUSING_FIBER (true)
#else
#define REUSING_FIBER (false)
#endif

  // Checks and processes timeout events with std::shared_ptr<Event>
  void check_timeout(rusty::VecDeque<std::shared_ptr<Event>>&) const;
  /**
   * @param ev. is usually allocated on a fiber stack. memory managed by user.
   */
  // @safe - Creates and runs a new fiber with rusty::Rc ownership
  // Refactored into smaller safe helper functions for clarity and safety.
  // Jetpack: file/line parameters for debugging fiber creation location
  rusty::Rc<Fiber> create_run_fiber(rusty::Function<void()> func,
                                            const char* file = "",
                                            int64_t line = 0) const;

 private:
  // Helper functions for create_run_fiber - each is @safe with internal @unsafe blocks

  // @safe - Gets a recycled fiber or creates a new one
  rusty::Rc<Fiber> get_or_create_fiber(rusty::Function<void()> func,
                                               const char* file,
                                               int64_t line) const;

  // @safe - Saves current running fiber to allow nesting
  rusty::Option<rusty::Rc<Fiber>> save_running_fiber() const;

  // @safe - Restores previously saved running fiber
  void restore_running_fiber(rusty::Option<rusty::Rc<Fiber>> old_fiber) const;

  // @safe - Sets the current running fiber
  void set_running_fiber(const rusty::Rc<Fiber>& fiber) const;

  // @safe - Registers a fiber in the active set
  void register_fiber(const rusty::Rc<Fiber>& fiber) const;

  // @safe - Queue a stackless task slot for polling if not already queued.
  void enqueue_stackless_task(size_t idx) const;

  // @safe - Register a stackless task poller and return slot index.
  size_t register_stackless_poller(rusty::Function<bool(rusty::Context&)> poller) const;

  // @safe - Poll all queued stackless tasks once.
  // Returns true if at least one stackless task was polled.
  bool process_stackless_tasks() const;

  // @safe - Arc::make is @safe in the library.
  template <typename U, typename... Args>
  static rusty::Arc<U> make_arc(Args&&... args) {
    return rusty::Arc<U>::make(std::forward<Args>(args)...);
  }

 public:
  // @safe - Main event loop
  void loop(bool infinite = false, bool do_check_timeout = true) const;
  // @safe - Continues execution of a paused fiber
  void continue_fiber(rusty::Rc<Fiber> fiber) const;
  void recycle(rusty::Rc<Fiber>& fiber) const;
  void display_waiting_ev() const;
  // @safe - Spawn a stackless C++20 coroutine task managed by the reactor.
  void spawn_stackless_task(rusty::Task<void> task) const;
  // @safe - Spawn a stackless task with a completion callback when ready.
  template <typename T, typename OnReady>
  void spawn_stackless_task_with_result(rusty::Task<T> task, OnReady on_ready) const {
    constexpr size_t kUnregisteredSlot = std::numeric_limits<size_t>::max();
    struct EarlyWakeState {
      explicit EarlyWakeState(const Reactor* reactor_ptr) : reactor(reactor_ptr) {}
      const Reactor* reactor;
      mutable std::atomic<size_t> idx{kUnregisteredSlot};
      mutable std::atomic<bool> pending_wake{false};
    };

    // SAFETY: shared state is heap-owned; reactor outlives callback execution.
    auto early_wake = make_arc<EarlyWakeState>(this);

    rusty::Waker early_waker{[early_wake, kUnregisteredSlot]() {
      size_t idx = early_wake->idx.load(std::memory_order_acquire);
      if (idx == kUnregisteredSlot) {
        early_wake->pending_wake.store(true, std::memory_order_release);
        return;
      }
      early_wake->reactor->enqueue_stackless_task(idx);
    }};
    rusty::Context early_ctx{&early_waker};
    auto early_poll = task.poll(early_ctx);
    if (early_poll.is_ready()) {
      on_ready(std::move(early_poll.value));
      return;
    }

    struct TaskState {
      mutable rusty::Task<T> task;
      mutable rusty::Option<OnReady> on_ready;
      rusty::Arc<EarlyWakeState> early_wake;

      TaskState(rusty::Task<T> t, OnReady cb, rusty::Arc<EarlyWakeState> ew)
          : task(std::move(t)), on_ready(std::move(cb)), early_wake(std::move(ew)) {}
    };

    // SAFETY: TaskState is only accessed through the Arc captured by the poller.
    auto state = make_arc<TaskState>(std::move(task), std::move(on_ready), std::move(early_wake));
    auto idx = register_stackless_poller([state](rusty::Context& ctx) mutable {
      auto poll_result = state->task.poll(ctx);
      if (!poll_result.is_ready()) {
        return false;
      }
      state->early_wake->idx.store(kUnregisteredSlot, std::memory_order_release);
      if (state->on_ready.is_some()) {
        // unwrap() consumes: moves out and sets to None in one step.
        auto cb = state->on_ready.unwrap();
        cb(std::move(poll_result.value));
      }
      return true;
    });
    state->early_wake->idx.store(idx, std::memory_order_release);
    if (state->early_wake->pending_wake.exchange(false, std::memory_order_acq_rel)) {
      enqueue_stackless_task(idx);
    }
  }

  ~Reactor() {
    Log_debug("[Reactor::~Reactor] Starting destruction, all_events_.len()=%zu, fibers_.len()=%zu",
              all_events_.borrow()->len(), fibers_.borrow()->len());
    // Note: destructor body runs BEFORE member variables are destroyed
    Log_debug("[Reactor::~Reactor] Destructor body complete, about to destroy member variables");
  }
  friend Event;

  // @unsafe - Creates std::shared_ptr<Event> with perfect forwarding and polymorphism support
  // SAFETY: Uses std::shared_ptr for mutable access and polymorphism. Lifetime is safe because:
  //   1. shared_ptr is stored in all_events_ list (owned by reactor)
  //   2. Reactor lives for entire program duration
  //   3. Events are never removed from all_events_ until reactor destruction
  // Cross-thread notification uses raw pointers (safe: reactor owns all events)
  template <typename Ev, typename... Args>
  static std::shared_ptr<Ev> create_sp_event(Args&&... args) {  // @unsafe
    auto ev = std::make_shared<Ev>(args...);
    ev->__debug_creator = 1;
    // Set self-reference for cross-thread signaling (uses raw pointer now)
    ev->set_self(ev);
    // Store in all_events_ using RefCell borrow_mut()
    auto reactor = get_reactor();
    reactor->all_events_.borrow_mut()->push_back(ev);
    return ev;
  }

  // @unsafe - Creates event and returns reference to shared_ptr content
  // SAFETY: Returned reference is valid because:
  //   1. Event is created via create_sp_event and stored in all_events_
  //   2. all_events_ is never cleared during reactor lifetime
  //   3. Returned reference points to heap-allocated Event managed by shared_ptr
  // Manual verification required: reference lifetime extends beyond function scope
  template <typename Ev, typename... Args>
  static Ev& create_event(Args&&... args) {  // @unsafe
    auto sp = create_sp_event<Ev>(args...);
    return *sp;
  }
};

// Forward declarations
class PollThread;
class PollThreadWorker;

// =============================================================================
// Channel-based communication between PollThread and PollThreadWorker
// =============================================================================

// Commands sent from PollThread to PollThreadWorker via channel
// Using std::variant for type-safe discriminated union
struct CmdAddPollable {
    PollableProxy pollable;
};
struct CmdRemovePollable { int fd; };
struct CmdClosePollable { int fd; };  // Close socket and drop Arc (thread-safe close)
struct CmdUpdateMode { int fd; int new_mode; };
struct CmdAddJob { rusty::Arc<Job> job; };
struct CmdRemoveJob { rusty::Arc<Job> job; };
struct CmdShutdown {};

using PollCommand = std::variant<
    CmdAddPollable,
    CmdRemovePollable,
    CmdClosePollable,
    CmdUpdateMode,
    CmdAddJob,
    CmdRemoveJob,
    CmdShutdown
>;

}  // export namespace rrr

// --- from reactor.h (trait spec for PollCommand) -------------------------
namespace rusty {
template<>
struct is_send<rrr::PollCommand> : std::true_type {};
} // namespace rusty

// @safe - PollThreadWorker / PollThread declarations. Class-level
// annotations + per-method `// @unsafe` overrides on the syscalls
// (epoll_wait, eventfd_write, futex) and raw pointer paths.
export namespace rrr {
// --- from reactor.h (block 2: PollThreadWorker, PollThread) -------------

// =============================================================================
// PollThreadWorker - Owns all polling state, runs in dedicated thread
// =============================================================================

// Worker class that owns all polling state
// Runs entirely in the spawned thread
// Receives commands from PollThread via mpsc channel
//
// @safe - Single-threaded worker with RefCell for interior mutability
// Design rationale - PollThreadWorker is memory-safe because:
// 1. Single-threaded: Runs only on its dedicated poll thread, no data races
// 2. Ownership: Owns all Pollables via fd_to_pollable_ map
// 3. Lifetime: Worker outlives all Pollables - on shutdown, clears before destruction
// 4. Channel: Cross-thread communication only via thread-safe mpsc channel
// 5. No re-entrancy: handle_write() returns new mode instead of calling back,
//    so RefCell borrow is never held across handler calls
class PollThreadWorker {
    friend class PollThread;
    friend class rusty::Rc<rusty::RefCell<PollThreadWorker>>;

public:
    // @unsafe - Factory method - creates worker wrapped in Rc<RefCell<>>
    static rusty::Rc<rusty::RefCell<PollThreadWorker>> create(rusty::sync::mpsc::Receiver<PollCommand> receiver);

    // Constructor is public for Rc::make(), but prefer create() factory
    explicit PollThreadWorker(rusty::sync::mpsc::Receiver<PollCommand> receiver);

    ~PollThreadWorker() = default;

    // Delete copy - worker is owned by Rc<RefCell<>>
    PollThreadWorker(const PollThreadWorker&) = delete;
    PollThreadWorker& operator=(const PollThreadWorker&) = delete;
    // Allow move - needed for RefCell construction
    PollThreadWorker(PollThreadWorker&&) = default;
    PollThreadWorker& operator=(PollThreadWorker&&) = delete;

    // @unsafe - Main polling loop - processes epoll events and channel commands
    // Non-const because it modifies state (no more mutable fields)
    void poll_loop();

    // @safe - Check if current thread is a poll thread
    // Returns true if called from a poll thread, false otherwise.
    static bool is_on_poll_thread() { return current_worker_ != nullptr; }

    // @unsafe - Add a pollable from within the poll thread (e.g., from handle_read)
    // Must only be called from the poll thread (asserts if not)
    // SAFETY: Dereferences raw pointer current_worker_ and calls do_add_pollable
    static void add_pollable_from_current_thread(PollableProxy poll) {
        verify(current_worker_ != nullptr);
        current_worker_->do_add_pollable(std::move(poll));
    }

    template <typename T>
    static void add_pollable_from_current_thread(rusty::Arc<T> poll) {
        verify(current_worker_ != nullptr);
        auto poll_proxy = make_pollable_proxy_from_typed_arc(std::move(poll));
        current_worker_->do_add_pollable(std::move(poll_proxy));
    }

    // @unsafe - Update poll mode directly (bypasses channel)
    // Only safe to call from the poll thread (e.g., from ServerConnection::end_reply)
    // SAFETY: Internal @unsafe block handles epoll operations and address-of
    void update_mode(Pollable& poll, int new_mode);

private:
    // Thread-local storage for current worker (raw pointer for internal use only)
    // Only accessed via with_current_worker() which provides safe reference access.
    // `inline` keeps the symbol in vague linkage — see Reactor::sp_reactor_th_ above.
    static inline thread_local PollThreadWorker* current_worker_ = nullptr;

private:
    // @unsafe - For testing: get number of epoll Remove() calls
    // SAFETY: Atomic load is safe but requires @unsafe annotation
    int get_remove_count() const { return poll_.remove_count_.load(); }

private:
    // Process incoming commands from channel
    void process_commands();

    // Triggers ready jobs in fibers
    void trigger_job();

    // Internal implementations (single-threaded, no races)
    void do_add_pollable(PollableProxy poll);
    void do_remove_pollable(int fd);
    void do_close_pollable(int fd);  // Close socket and drop Arc
    void do_update_mode(int fd, int new_mode);
    void do_add_job(rusty::Arc<Job> job);
    void do_remove_job(rusty::Arc<Job> job);

    // Process deferred removals
    void process_pending_removals();

private:
    // MPSC receiver for commands from PollThread
    rusty::sync::mpsc::Receiver<PollCommand> receiver_;

    // Epoll instance
    Epoll poll_;

    // Pollable state - single owner in worker thread
    rusty::HashMap<int, PollableProxy> fd_to_pollable_;
    rusty::HashMap<int, int> mode_;  // fd -> mode
    rusty::HashSet<int> pending_remove_;

    // Jobs - single owner in worker thread
    rusty::BTreeSet<rusty::Arc<Job>> jobs_;

    // Stop flag
    bool stop_ = false;
};

// =============================================================================
// PollThread - Handle for controlling the poll thread
// =============================================================================

// @unsafe - Handle for controlling the poll thread (has mutable fields)
// SAFETY: Despite @unsafe annotation, PollThread is thread-safe because:
// 1. All cross-thread communication via thread-safe mpsc channel
// 2. Mutable fields use proper synchronization (mutex for join_handle_, atomic for shutdown_called_)
class PollThread {
    // Friend Arc to allow make access to private constructor
    friend class rusty::Arc<PollThread>;

private:
    // MPSC sender for commands to worker
    mutable rusty::sync::mpsc::Sender<PollCommand> sender_;

    // Join handle for the thread (Mutex provides interior mutability)
    rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<void>>> join_handle_;

    // Thread ID of the poll thread - used to detect self-join attempts.
    // rusty::Atomic wraps std::atomic<ThreadId> — ThreadId is TriviallyCopyable so
    // this stays lock-free on typical platforms.
    mutable rusty::sync::atomic::Atomic<rusty::thread::ThreadId> poll_thread_id_{};

    // Track if shutdown was called
    mutable std::atomic<bool> shutdown_called_{false};

    // Private constructor - use create() factory
    explicit PollThread(rusty::sync::mpsc::Sender<PollCommand> sender);

public:
    ~PollThread();

    // Factory method returns Arc<PollThread>
    static rusty::Arc<PollThread> create();

    // Explicit shutdown
    void shutdown() const;

    // Delete copy/move
    PollThread(const PollThread&) = delete;
    PollThread& operator=(const PollThread&) = delete;
    PollThread(PollThread&& other) = delete;
    PollThread& operator=(PollThread&& other) = delete;

    // Send commands to worker via channel
    void add_proxy(PollableProxy poll) const;
    void remove(Pollable& poll) const;
    void request_close(int fd) const;  // Thread-safe close: removes from epoll, closes socket, drops proxy ownership
    // @safe - Sends update mode command via channel
    // SAFETY: Channel send is thread-safe, Pollable is only read (fd())
    void update_mode(int fd, int new_mode) const;
    void update_mode(const Pollable& poll, int new_mode) const;
    void add(rusty::Arc<Job> job) const;
    void remove(rusty::Arc<Job> job) const;

    // For testing - NOTE: This won't work with channel design
    // since worker state is not accessible. Return 0 for now.
    int get_remove_count() const { return 0; }
};

}  // export namespace rrr

// --- from reactor.h (trait specs for PollThread) -------------------------
namespace rusty {
template<>
struct is_send<rrr::PollThread> : std::true_type {};

template<>
struct is_sync<rrr::PollThread> : std::true_type {};
} // namespace rusty

// --- from quorum_event.h --------------------------------------------------
// @safe - QuorumEvent declarations under the janus namespace.
export namespace janus {

// Pulled in from former `quorum_event.h` (lines 26-29). Folded into the
// janus-namespace purview of the consolidated `rrr.reactor` module so
// `QuorumEvent` can name `Event` / `IntEvent` / `verify` / `shared_ptr`
// unqualified, matching the original source.
using rrr::Event;
using rrr::IntEvent;
using rrr::verify;
using std::shared_ptr;

class QuorumEvent : public Event {
 public:
	static uint64_t count;
  int32_t n_voted_yes_{0};
  int32_t n_voted_no_{0};
  rusty::HashMap<uint16_t, rrr::i64> xids_;
  uint64_t begin_timestamp_;

 public:
  int32_t n_total_ = -1;
  int32_t quorum_ = -1;
  int64_t highest_term_{0} ;
  bool timeouted_ = false;
  uint64_t cmt_idx_{0} ;
  uint32_t leader_id_{0} ;
  uint64_t coro_id_ = -1;
  int64_t par_id_ = -1;
  uint64_t id_ = -1;
	uint64_t server_id_ = -1;
  std::chrono::steady_clock::time_point ready_time;
  // fast vote result.
  rusty::Vec<uint64_t> vec_timestamp_{};
  shared_ptr<IntEvent> finalize_event_;

  QuorumEvent() = delete;

  QuorumEvent(int n_total, int quorum);

  /**
   * Record the TXid of an issued RPC and which site it's issued to
   * in the dangling RPC list
   *
   * @param site site id of the RPC issuing to
   * @param xid TXid of the RPC
   */
  void add_xid(uint16_t site, rrr::i64 xid);

  /**
   * Remove an replied RPC from the dangling RPC list
   *
   * @param site site id of the reply coming from
   */
  void remove_xid(uint16_t site);

  /**
   * call finalize before/after wait() to cleanup the side-effect of the quorum-event
   * (e.g. free dangling RPCs). However, finalize should not block execution after wait.
   * That is, finalize should be a background task, with respect to the main fiber (
   * the fiber where wait() is called)
   * TODO: find a proper way to achieve this
   *
   * @param timeout time to wait after event-ready to do finalize
   * @param finalize_func what to do in finalization, take a list of dangling RPC
   */
  void finalize(uint64_t timeout,
                rusty::Function<bool(rusty::Vec<std::pair<uint16_t, rrr::i64> >&)> finalize_func);

  virtual bool yes() {
    return n_voted_yes_ >= quorum_;
  }

  virtual bool no() {
    verify(n_total_ >= quorum_);
    return n_voted_no_ > (n_total_ - quorum_);
  }

  // @safe - test(), Time::now(), rusty::Vec::push, IntEvent::set
  // are all @safe; Cell::get on `finalize_event_->status_` is @safe.
  void vote_yes();

  // @safe - test() and IntEvent::set are @safe; Cell::get on
  // `finalize_event_->status_` is @safe.
  void vote_no();

  bool is_ready() override {
    if (timeouted_) {
      // TODO add time out support
      return true;
    }
    if (yes()) {
//      Log_info("voted: %d is equal or greater than quorum: %d",
//                (int)n_voted_yes_, (int) quorum_);
      ready_time = std::chrono::steady_clock::now();
      return true;
    } else if (no()) {
      return true;
    }
//    Log_debug("voted: %d is smaller than quorum: %d",
//              (int)n_voted_, (int) quorum_);
    return false;
  }

  // Mark as composite event - will be polled in reactor loop
  bool is_composite_event() override { return true; }

  void log_event();

};

}  // export namespace janus

// ===========================================================================
// Out-of-line definitions (from former event.cc, fiber_impl.cc, reactor.cc,
// fiber_context_runtime.cc)
// ===========================================================================
// @safe - Implementation namespace. Out-of-class definitions inherit
// per-method `// @unsafe` annotations from the declarations above.
// The anonymous-namespace `stat_*` / `stackless_profile_*` helpers
// (line 1582+) and other free-function impl details carry their own
// `// @unsafe` markers individually where needed.
namespace rrr {

// --- from event.cc -------------------------------------------------------

uint64_t Event::get_fiber_id(){
  auto fiber_opt = Fiber::current_fiber();
  verify(fiber_opt.is_some());
  return fiber_opt.unwrap()->id;
}

bool Event::is_slow() {
	bool result = Reactor::get_reactor()->slow_.get();
	Reactor::get_reactor()->slow_.set(false);
	return result;
}

// void Event::Wait(uint64_t timeoutuint64_t timeout) {
// //  verify(__debug_creator); // if this fails, the event is not created by reactor.

//   verify(Reactor::sp_reactor_th_);
//   verify(Reactor::sp_reactor_th_->thread_id_ == rusty::thread::current_id());
//   if (IsReady()) {
//     status_ = DONE; // does not need to wait.
//     return;
//   } else {
//     verify(status_ == INIT);
//     status_= DEBUG;
//     // the event may be created in a different fiber.
//     // this value is set when wait is called.
//     // for now only one fiber can wait on an event.
//     auto sp_fiber = Fiber::current_fiber();
// //    verify(sp_fiber);
// //    verify(_dbg_p_scheduler_ == nullptr);
// //    _dbg_p_scheduler_ = Reactor::get_reactor().get();
//     auto& events = Reactor::get_reactor()->waiting_events_;
//     events.push_back(shared_from_this());
//     wp_fiber_ = sp_fiber;
//     status_ = WAIT;
//     sp_fiber->yield_();
//   }
// }

void Event::wait(uint64_t timeout) {
//  verify(__debug_creator); // if this fails, the event is not created by reactor.
  verify(Reactor::sp_reactor_th_.is_some());
  verify(Reactor::sp_reactor_th_.as_ref().unwrap()->thread_id_.get() == rusty::thread::current_id());
  if (status_.get() == DONE) return; // TODO: yidawu add for the second use the event.
  // verify(status_.get() == INIT);
  if (is_ready()) {
    status_.set(DONE); // no need to wait.
    return;
  } else {
//    if (status_ == WAIT) {
//      // this does not look right, fix later
//      Log_fatal("multiple waits on the same event; no support at the moment");
//    }
//    verify(status_ == INIT); // does not support multiple wait so far. maybe we can support it in the future.
//    status_= DEBUG;
    // the event may be created in a different fiber.
    // this value is set when wait is called.
    // for now only one fiber can wait on an event.
    auto fiber_opt = Fiber::current_fiber();
    verify(fiber_opt.is_some());  // Can't wait outside a fiber
    auto fiber = fiber_opt.unwrap();

    // Use RefCell borrow_mut() for safe interior mutability
    auto reactor_rc = Reactor::get_reactor();
    reactor_rc->waiting_events_.borrow_mut()->push_back(get_self());

    // Composite events (WaitAll, WaitAny, QuorumEvent) need periodic polling
    // Add them to a separate queue that gets scanned (much smaller than all events)
    // Regular RPC events (Raft) self-notify via test() - zero overhead!
    if (is_composite_event()) {
      Reactor::get_reactor()->composite_events_.borrow_mut()->push_back(get_self());
    }

#ifdef EVENT_TIMEOUT_CHECK
    if (timeout == 0) {
      __debug_timeout_ = true;
      timeout = 200 * 1000 * 1000;
//#ifdef SIMULATE_WAN
//      timeout = 600 * 1000 * 1000;
//#endif
    }
#endif
    if (timeout > 0) {
      auto now = Time::now(true);
      wakeup_time_ = now + timeout;
      //Log_info("WAITING: %p", get_self().get());
      // Log_info("wake up %lld, now %lld", wakeup_time_, now);
      reactor_rc->timeout_events_.borrow_mut()->push_back(get_self());
    }
    // TODO optimize timeout_events, sort by wakeup time.
//      auto it = timeout_events.end();
//      timeout_events.push_back(rc_this_event);
//      while (it != events.begin()) {
//        it--;
//        auto& it_event = *it;
//        if (it_event->wakeup_time_ < wakeup_time_) {
//          it++; // list insert happens before position.
//          break;
//        }
//      }
//      events.insert(it, shared_from_this());

    wp_fiber_ = fiber;
    status_.set(WAIT);
    auto fiber_status = fiber->status_.get();
    verify(fiber_status != Fiber::FINISHED && fiber_status != Fiber::RECYCLED);
    fiber->yield_();
#ifdef EVENT_TIMEOUT_CHECK
    if (__debug_timeout_ && status_.get() == TIMEOUT) {
      Log_info("timeout");
      verify(0);
    }
#endif
  }
}

void Event::record_place(const char* file, int line) {
  char buff[200];
  sprintf(buff, "%s:%d", file, line);
  wait_place_ += std::string(buff);
  rcd_wait_ = true;
}

// @safe - verify(), is_ready(), Cell::get/set, Weak::upgrade, Option::is_some
// and Log_debug are all @safe.
bool Event::test() {
  verify(__debug_creator);
  if (is_ready()) {
    if (status_.get() == INIT) {
      status_.set(DONE);
    } else if (status_.get() == WAIT) {
      auto option_fiber = wp_fiber_.upgrade();
      verify(option_fiber.is_some());
      verify(status_.get() != DEBUG);
      status_.set(READY);
    } else if (status_.get() == READY) {
      Log_debug("event status ready, triggered?");
    } else if (status_.get() == DONE) {
      // do nothing
    } else if (status_.get() == TIMEOUT) {
      // do nothing
    } else {
      verify(0);
    }
    return true;
  } else {
    if (status_.get() == DONE) {
      status_.set(INIT);
    }
  }
  return false;
}

Event::Event() {
  auto fiber_opt = Fiber::current_fiber();
  // It's OK if no fiber is running - event might be created outside a fiber
  // and Wait() called later from within one
  if (fiber_opt.is_some()) {
    wp_fiber_ = fiber_opt.unwrap();
  }
  // Otherwise wp_fiber_ stays as default empty weak pointer
}

bool IntEvent::test_trigger() {
  verify(status_.get() <= WAIT);
  if (value_ == target_) {
    if (status_.get() == INIT) {
      // do nothing until wait happens.
      status_.set(DONE);
    } else if (status_.get() == WAIT) {
      status_.set(READY);
    } else {
      verify(0);
    }
    return true;
  }
  return false;
}

int SharedIntEvent::set(const int& v) {
  auto ret = value_;
  value_ = v;
  for (auto& ev : events_) {
    if (ev->status_.get() <= Event::WAIT) {
      if (ev->target_ <= v) {
        ev->set(v);
      }
    }
  }
  return ret;
}

// @unsafe - holds a raw `IntEvent*` (`ev_ptr = ev.get()`) across the
// retain() lambda capture to identity-compare against shared_ptr<IntEvent>
// entries in `events_`. The shared_ptr keeps the target alive for the
// duration of the call.
bool SharedIntEvent::wait_until_gte(int x, int timeout) {
  if (value_ >= x) {
    return false;
  }
  auto ev =  Reactor::create_sp_event<IntEvent>();
  ev->value_ = value_;
  ev->target_ = x;
  events_.push(ev);
  ev->wait(timeout);
  // verify(ev->status_.get() != Event::TIMEOUT);  // why can't it be timeout?
  // remove the event from event vector after it entering a terminate state (READY or TIMEOUT)
  bool if_timeout = (ev->status_.get() == Event::TIMEOUT);
  auto* ev_ptr = ev.get();
  events_.retain(rusty::Function<bool(const std::shared_ptr<IntEvent>&)>(
      [ev_ptr](const std::shared_ptr<IntEvent>& item) {
        return item.get() != ev_ptr;
      }));
  return if_timeout;
}

void SharedIntEvent::wait(rusty::Function<bool(int v)> f) {
  if (f(value_)) {
    return;
  }
  auto ev =  Reactor::create_sp_event<IntEvent>();
  ev->value_ = value_;
  ev->test_ = std::move(f);
  events_.push(ev);
//  ev->wait(1000*1000*1000);
//  verify(ev->status_ != Event::TIMEOUT);
  ev->wait();
}


// --- from fiber_impl.cc --------------------------------------------------
thread_local uint64_t Fiber::global_id = 0;

// @safe - Trivial member-initializer ctor; std::move + post-increment of
// a thread-local uint64_t. Cells/RefCells default-construct via class
// initializers above; func_ takes a moved-in rusty::Function.
Fiber::Fiber(rusty::Function<void()> func)
    : status_(INIT),
      needs_finalize_(false),
      func_(std::move(func)),
      fiber_task_(rusty::None),
      id(Fiber::global_id++) {
}

// @safe - Empty dtor; rusty::Box / rusty::RefCell members release on drop.
Fiber::~Fiber() {
  // rusty::Box automatically handles cleanup
//  verify(0);
}

void Fiber::run_wrapper(fiber_yield_t& yield) {
  fiber_yield_.set(&yield);
  verify(static_cast<bool>(*func_.borrow()));
  auto reactor = Reactor::get_reactor();
  while (true) {
    auto sz = reactor->fibers_.borrow()->len();
    verify(sz > 0);
    verify(static_cast<bool>(*func_.borrow()));
    (*func_.borrow_mut())();  // borrow_mut needed because operator() is non-const
    *func_.borrow_mut() = {};
    status_.set(FINISHED);
    if (needs_finalize_.get()) {
      Log_info("Warning: We did not deal with backlog issues");
      needs_finalize_.set(false);
    }
    auto reactor = Reactor::get_reactor();
    reactor->n_active_fibers_.set(reactor->n_active_fibers_.get() - 1);
    yield();
  }
}

// @safe - Initializes and starts a fiber
// SAFETY: Single-threaded fiber execution, no concurrent mutation.
// Uses @unsafe blocks for: RefCell operations, get_reactor, STL, const_cast, std::bind, fiber runtime calls.
void Fiber::run() const {
  // @unsafe
  {
    verify((*fiber_task_.borrow()).is_none());
    verify(status_.get() == INIT);
    status_.set(STARTED);
    auto reactor = Reactor::get_reactor();
    auto sz = reactor->fibers_.borrow()->len();
    verify(sz > 0);
    auto task = std::bind(&Fiber::run_wrapper, const_cast<Fiber*>(this), std::placeholders::_1);
    *fiber_task_.borrow_mut() = rusty::Some(rusty::make_box<fiber_task_t>(std::move(task)));
#ifdef USE_FIBER_RUNTIME1
    (*(*fiber_task_.borrow()).as_ref().unwrap())();
#endif
  }
}

// @safe - Yields control back to the reactor
// SAFETY: Single-threaded fiber execution
void Fiber::yield_() const {
  // @unsafe
  {
    auto* yield_ptr = fiber_yield_.get();
    verify(yield_ptr != nullptr);
    auto s = status_.get();
    verify(s == STARTED || s == RESUMED || s == FINALIZING);
    status_.set(PAUSED);
    {
      auto reactor = Reactor::get_reactor();
      reactor->n_active_fibers_.set(reactor->n_active_fibers_.get() - 1);
    }
    (*yield_ptr)();
  }
}

// @safe - Resumes a paused fiber
// SAFETY: Single-threaded fiber execution
void Fiber::continue_() const {
  // @unsafe
  {
    auto s = status_.get();
    verify(s == PAUSED || s == RECYCLED);
    verify((*fiber_task_.borrow()).is_some());
    status_.set(RESUMED);
    (*(*fiber_task_.borrow_mut()).as_mut().unwrap())();
  }
  // some events might have been triggered from last fiber,
  // but you have to manually call the scheduler to loop.
}

// @safe - Reads Cell<Status>::get() and returns a bool.
bool Fiber::finished() const {
  auto s = status_.get();
  return s == FINISHED || s == RECYCLED;
}

// @safe - One Cell<bool>::set call.
void Fiber::do_finalize() {
  // Handle finalization logic if needed
  needs_finalize_.set(false);
}


// --- from reactor.cc -----------------------------------------------------

const int64_t n_max_fiber = 2000;
// `REUSING_FIBER` is provided as a macro by reactor.h (line 203).
// The original module-attached `constexpr bool REUSING_FIBER`
// shadowed the macro inside the rrr module's purview; with
// de-modularization (header-textual inclusion) the macro now
// expands at parse time and the constexpr is redundant — the
// existing call sites in this TU (lines below) consume the macro
// directly.

namespace {

inline bool stackless_profile_enabled() {
  static bool enabled = []() {
    const char* env = std::getenv("MAKO_ASYNC_PROFILE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

struct StacklessProfileCounters {
  std::atomic<uint64_t> reg_calls{0};
  std::atomic<uint64_t> reg_scan_steps{0};
  std::atomic<uint64_t> reg_reuse{0};
  std::atomic<uint64_t> reg_new{0};
  std::atomic<uint64_t> poll_calls{0};
  std::atomic<uint64_t> poll_ready{0};
  std::atomic<uint64_t> enqueue_calls{0};
  std::atomic<size_t> max_slots{0};
};

StacklessProfileCounters g_stackless_profile;

inline void stackless_profile_update_max_slots(size_t slots) {
  size_t old = g_stackless_profile.max_slots.load(std::memory_order_relaxed);
  while (slots > old &&
         !g_stackless_profile.max_slots.compare_exchange_weak(
             old, slots, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

inline void stackless_profile_report_periodic() {
  if (!stackless_profile_enabled()) {
    return;
  }
  static thread_local uint64_t last_report_us = 0;
  uint64_t now_us = Time::now(true);
  if (last_report_us == 0) {
    last_report_us = now_us;
    return;
  }
  if (now_us - last_report_us < 1000000) {
    return;
  }
  last_report_us = now_us;

  uint64_t reg_calls = g_stackless_profile.reg_calls.load(std::memory_order_relaxed);
  uint64_t reg_scans = g_stackless_profile.reg_scan_steps.load(std::memory_order_relaxed);
  uint64_t reg_reuse = g_stackless_profile.reg_reuse.load(std::memory_order_relaxed);
  uint64_t reg_new = g_stackless_profile.reg_new.load(std::memory_order_relaxed);
  uint64_t poll_calls = g_stackless_profile.poll_calls.load(std::memory_order_relaxed);
  uint64_t poll_ready = g_stackless_profile.poll_ready.load(std::memory_order_relaxed);
  uint64_t enqueue_calls = g_stackless_profile.enqueue_calls.load(std::memory_order_relaxed);
  size_t max_slots = g_stackless_profile.max_slots.load(std::memory_order_relaxed);

  double avg_scan = (reg_calls > 0) ? static_cast<double>(reg_scans) / static_cast<double>(reg_calls) : 0.0;
  Log_info("[async-prof] reg_calls=%llu avg_scan=%.2f reuse=%llu new=%llu max_slots=%zu poll_calls=%llu poll_ready=%llu enqueue_calls=%llu",
           static_cast<unsigned long long>(reg_calls),
           avg_scan,
           static_cast<unsigned long long>(reg_reuse),
           static_cast<unsigned long long>(reg_new),
           max_slots,
           static_cast<unsigned long long>(poll_calls),
           static_cast<unsigned long long>(poll_ready),
           static_cast<unsigned long long>(enqueue_calls));
}

}  // namespace

// sp_reactor_th_ / sp_disk_reactor_th_ / sp_running_fiber_th_ are
// `static inline thread_local` in the class declaration above (vague linkage).
// Same for PollThreadWorker::current_worker_, clients_, and dangling_ips_.
SpinLock Reactor::trying_job_;

// @safe - Returns current fiber with single-threaded reference counting
// SAFETY: Returns copy of thread-local Rc - single-threaded, no synchronization needed
// Returns None if called outside of a fiber context
rusty::Option<rusty::Rc<Fiber>> Fiber::current_fiber() {
  // @unsafe - RefCell::borrow, Rc::clone
  {
    auto guard = Reactor::sp_running_fiber_th_.borrow();
    if ((*guard).is_none()) {
      return rusty::None;
    }
    return rusty::Some((*guard).as_ref().unwrap().clone());
  }
}

// @unsafe - Creates and runs a new fiber with rusty::Rc ownership
rusty::Rc<Fiber>
Fiber::create_run_impl(rusty::Function<void()> func, const char* file, int64_t line) {
  auto reactor_rc = Reactor::get_reactor();
  // Rc gives const access, create_run_fiber is const (safe: thread-local, single owner)
  auto fiber = reactor_rc->create_run_fiber(std::move(func), file, line);
  // some events might be triggered in the last fiber.
  return fiber;
}

void Fiber::sleep(uint64_t microseconds) {
  if (microseconds == 0) {
    return;
  }
  auto x = Reactor::create_sp_event<TimeoutEvent>(microseconds);
  x->wait();
}

/**
 * @safe - Returns thread-local reactor instance, creates if needed
 *
 * SAFETY CONTRACT:
 * 1. Thread-Local Storage: sp_reactor_th_ is thread_local, ensuring no data races.
 * 2. Lazy Initialization: Reactor created on first access per thread.
 * 3. Thread Pinning: thread_id_ set on creation, verified in Loop().
 * 4. Reference Counting: Rc<Reactor> returned, safe to clone within same thread.
 *
 * POSTCONDITIONS:
 * - Returns valid Rc<Reactor> pinned to current thread
 * - Reactor's thread_id_ matches rusty::thread::current_id()
 */
rusty::Rc<Reactor>
Reactor::get_reactor() {
  // @unsafe { Option operator=, unwrap, Rc::make are not borrow-checked }
  {
  if (sp_reactor_th_.is_none()) {
    Log_debug("create a fiber scheduler");
    if (!REUSING_FIBER)
      Log_warn("reusing fiber not enabled!");
    sp_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());
    (*sp_reactor_th_.as_ref().unwrap()).thread_id_.set(rusty::thread::current_id());
  }
  return sp_reactor_th_.as_ref().unwrap().clone();
  }
}

rusty::Rc<Reactor>
Reactor::get_disk_reactor() {
  if (sp_disk_reactor_th_.is_none()) {
    Log_debug("create a disk fiber scheduler");
    sp_disk_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());
    (*sp_disk_reactor_th_.as_ref().unwrap()).thread_id_.set(rusty::thread::current_id());
  }
  return sp_disk_reactor_th_.as_ref().unwrap().clone();
}

// =============================================================================
// Helper functions for create_run_fiber
// =============================================================================

// @safe - Gets a recycled fiber or creates a new one
rusty::Rc<Fiber>
Reactor::get_or_create_fiber(rusty::Function<void()> func, const char* file, int64_t line) const {
  // @unsafe
  {
    auto available_guard = available_fibers_.borrow_mut();
    if (REUSING_FIBER && available_guard->size() > 0) {
      n_idle_fibers_.set(n_idle_fibers_.get() - 1);
      auto fiber = available_guard->back().clone();
      available_guard->pop();
      // Use Cell/RefCell for interior mutability (safe: single-threaded)
      const auto& fiber_ref = *fiber;
      const_cast<Fiber&>(fiber_ref).id = Fiber::global_id++;  // id is not Cell yet
      *fiber_ref.func_.borrow_mut() = std::move(func);
      // Keep the existing task/stack so continue_() can resume from the fiber's yield point.
      verify((*fiber_ref.fiber_task_.borrow()).is_some());
      fiber_ref.status_.set(Fiber::RECYCLED);
      return fiber;
    } else {
      auto fiber = rusty::Rc<Fiber>::make(std::move(func));
      n_created_fibers_.set(n_created_fibers_.get() + 1);
      if (n_created_fibers_.get() % 1024 == 0) {
        Log_debug("created %d, busy %d, idle %d fibers on server %d, recent %s:%lld",
                 (int)n_created_fibers_.get(),
                 (int)n_busy_fibers_.get(),
                 (int)n_idle_fibers_.get(),
                 server_id_.get(),
                 file,
                 (long long)line);
      }
      return fiber;
    }
  }
}

// @safe - Saves current running fiber to allow nesting
rusty::Option<rusty::Rc<Fiber>>
Reactor::save_running_fiber() const {
  // @unsafe
  {
    auto guard = sp_running_fiber_th_.borrow();
    if ((*guard).is_some()) {
      return rusty::Some((*guard).as_ref().unwrap().clone());
    }
    return rusty::Option<rusty::Rc<Fiber>>{};
  }
}

// @safe - Restores previously saved running fiber
void Reactor::restore_running_fiber(rusty::Option<rusty::Rc<Fiber>> old_fiber) const {
  // @unsafe
  {
    *sp_running_fiber_th_.borrow_mut() = std::move(old_fiber);
  }
}

// @safe - Sets the current running fiber
void Reactor::set_running_fiber(const rusty::Rc<Fiber>& fiber) const {
  // @unsafe
  {
    *sp_running_fiber_th_.borrow_mut() = rusty::Some(fiber.clone());
  }
}

// @safe - Registers a fiber in the active set
void Reactor::register_fiber(const rusty::Rc<Fiber>& fiber) const {
  // @unsafe { RefCell::borrow_mut, BTreeSet::insert are not borrow-checked }
  {
  // BTreeSet::insert returns bool (true if newly inserted)
  auto fibers_guard = fibers_.borrow_mut();
  bool inserted = fibers_guard->insert(fiber.clone());
  if (!inserted) {
    Log_error("[DEBUG] RegisterFiber: Failed to insert fiber into fibers_ set!");
    Log_error("[DEBUG] fibers_ len: %zu, REUSING_FIBER: %d", fibers_guard->len(), REUSING_FIBER);
  }
  verify(inserted);
  verify(fibers_guard->len() > 0);
  }
}

// @unsafe - Queue a stackless task slot for polling if not already queued.
void Reactor::enqueue_stackless_task(size_t idx) const {
  if (stackless_profile_enabled()) {
    g_stackless_profile.enqueue_calls.fetch_add(1, std::memory_order_relaxed);
  }
  // @unsafe
  {
    auto tasks_guard = stackless_tasks_.borrow();
    if (idx >= tasks_guard->size()) {
      return;
    }
    if (!(*tasks_guard)[idx].active || (*tasks_guard)[idx].queued) {
      return;
    }
  }
  {
    auto tasks_guard = stackless_tasks_.borrow_mut();
    if (idx >= tasks_guard->size()) {
      return;
    }
    auto& entry = (*tasks_guard)[idx];
    if (!entry.active || entry.queued) {
      return;
    }
    entry.queued = true;
  }
  ready_stackless_tasks_.borrow_mut()->push_back(idx);
}

// @unsafe - Register a stackless task poller and return slot index.
size_t Reactor::register_stackless_poller(rusty::Function<bool(rusty::Context&)> poller) const {
  size_t scanned = 0;
  {
    auto free_guard = free_stackless_task_slots_.borrow_mut();
    if (!free_guard->is_empty()) {
      size_t idx = free_guard->back();
      free_guard->pop();
      auto tasks_guard = stackless_tasks_.borrow_mut();
      if (idx < tasks_guard->size()) {
        auto& entry = (*tasks_guard)[idx];
        entry.active = true;
        entry.queued = false;
        entry.poll_once = std::move(poller);
        if (stackless_profile_enabled()) {
          g_stackless_profile.reg_calls.fetch_add(1, std::memory_order_relaxed);
          g_stackless_profile.reg_scan_steps.fetch_add(scanned, std::memory_order_relaxed);
          g_stackless_profile.reg_reuse.fetch_add(1, std::memory_order_relaxed);
        }
        return idx;
      }
    }
  }

  auto tasks_guard = stackless_tasks_.borrow_mut();
  StacklessTaskEntry entry;
  entry.active = true;
  entry.queued = false;
  entry.poll_once = std::move(poller);
  tasks_guard->push(std::move(entry));
  if (stackless_profile_enabled()) {
    g_stackless_profile.reg_calls.fetch_add(1, std::memory_order_relaxed);
    g_stackless_profile.reg_scan_steps.fetch_add(scanned, std::memory_order_relaxed);
    g_stackless_profile.reg_new.fetch_add(1, std::memory_order_relaxed);
    stackless_profile_update_max_slots(tasks_guard->size());
  }
  return tasks_guard->size() - 1;
}

// @unsafe - Poll all queued stackless tasks once.
bool Reactor::process_stackless_tasks() const {
  bool did_work = false;
  for (;;) {
    size_t idx = 0;
    {
      auto ready_guard = ready_stackless_tasks_.borrow_mut();
      if (ready_guard->is_empty()) {
        break;
      }
      idx = (*ready_guard)[0];
      ready_guard->pop_front();
    }

    // Move the poll function out of its slot before invoking it; rusty::Function
    // is move-only so we can't keep a copy in-place. Reactor is single-threaded,
    // so any synchronous waker callback during poll only mutates queued/active
    // flags (never poll_once), and we put the function back below if the poll
    // didn't complete the task.
    rusty::Function<bool(rusty::Context&)> poll_fn;
    {
      auto tasks_guard = stackless_tasks_.borrow_mut();
      if (idx >= tasks_guard->size()) {
        continue;
      }
      auto& entry = (*tasks_guard)[idx];
      entry.queued = false;
      if (!entry.active || !entry.poll_once) {
        continue;
      }
      poll_fn = std::move(entry.poll_once);
    }

    did_work = true;
    if (stackless_profile_enabled()) {
      g_stackless_profile.poll_calls.fetch_add(1, std::memory_order_relaxed);
    }
    rusty::Waker waker{[this, idx]() {
      this->enqueue_stackless_task(idx);
    }};
    rusty::Context ctx{&waker};
    bool ready = poll_fn(ctx);

    {
      auto tasks_guard = stackless_tasks_.borrow_mut();
      if (idx < tasks_guard->size()) {
        auto& entry = (*tasks_guard)[idx];
        if (ready) {
          if (stackless_profile_enabled()) {
            g_stackless_profile.poll_ready.fetch_add(1, std::memory_order_relaxed);
          }
          entry.active = false;
          entry.queued = false;
          entry.poll_once = {};
          free_stackless_task_slots_.borrow_mut()->push(idx);
        } else {
          // Put the function back so the next poll iteration can fire it.
          entry.poll_once = std::move(poll_fn);
        }
      }
    }
  }
  stackless_profile_report_periodic();
  return did_work;
}

// =============================================================================
// Main create_run_fiber - orchestrates the helper functions
// =============================================================================

/**
 * @param func
 * @return
 */
// @safe - Creates and runs a fiber using safe helper functions
rusty::Rc<Fiber>
Reactor::create_run_fiber(rusty::Function<void()> func, const char* file, int64_t line) const {
  // Step 1: Get or create a fiber
  auto fiber = get_or_create_fiber(std::move(func), file, line);

  // @unsafe
  {
    n_busy_fibers_.set(n_busy_fibers_.get() + 1);
  }

  // Step 2: Save current running fiber context (for nesting)
  auto old_fiber = save_running_fiber();

  // Step 3: Set this as the running fiber
  set_running_fiber(fiber);

  // Step 4: Register in the active fibers set
  register_fiber(fiber);

  // Step 5: Run the fiber
  // @unsafe
  {
    auto status = fiber->status_.get();
    if (status == Fiber::INIT) {
      fiber->run();
    } else {
      verify(status == Fiber::RECYCLED);
      fiber->continue_();
    }
    if (fiber->finished()) {
      recycle(fiber);
    }
  }

  // Step 6: Process events
  // @unsafe
  {
    loop(false, true);
  }

  // Step 7: Restore previous running fiber
  restore_running_fiber(std::move(old_fiber));

  return fiber;
}

// @unsafe - Uses RefCell::borrow_mut (not borrow-checked)
void Reactor::check_timeout(rusty::VecDeque<std::shared_ptr<Event>>& ready_events) const {
  // Time::now is @safe via rusty::sys::time::clock_monotonic_us.
  int64_t time_now = Time::now(true);

  // @unsafe { RefCell::borrow_mut is not borrow-checked }
  auto guard = timeout_events_.borrow_mut();

  // First pass: update status of timed-out events
  for (size_t i = 0; i < guard->len(); ++i) {
    auto& sp = (*guard)[i];
    Event& event = *sp;
    auto status = event.status_.get();
    if (status == Event::WAIT) {
      const auto& wakeup_time = event.wakeup_time_;
      verify(wakeup_time > 0);
      if (time_now >= wakeup_time) {
        if (event.is_ready()) {
          event.status_.set(Event::READY);
        } else {
          event.status_.set(Event::TIMEOUT);
        }
      }
    }
  }

  // Extract events that are READY or TIMEOUT (timed out)
  {
    auto timed_out = guard->extract_if(
      rusty::Function<bool(const std::shared_ptr<Event>&)>(
        [](const std::shared_ptr<Event>& sp) {
          auto status = sp->status_.get();
          return status == Event::READY || status == Event::TIMEOUT;
        }));
    ready_events.append(std::move(timed_out));
  }

  // Remove events that are DONE (shouldn't happen often, but clean up)
  {
    guard->retain(
      rusty::Function<bool(const std::shared_ptr<Event>&)>(
        [](const std::shared_ptr<Event>& sp) {
          return sp->status_.get() != Event::DONE;
        }));
  }
}

void Reactor::loop(bool infinite, bool do_check_timeout) const {
  verify(rusty::thread::current_id() == thread_id_.get());

  looping_.set(infinite);

  do {
    bool found_ready_events = true;
    while (found_ready_events) {
      found_ready_events = false;
      if (process_stackless_tasks()) {
        found_ready_events = true;
      }
      rusty::VecDeque<std::shared_ptr<Event>> ready_events;

      // Process waiting events using RefCell
      {
        auto waiting_guard = waiting_events_.borrow_mut();
        // Test waiting events
        for (size_t i = 0; i < waiting_guard->len(); ++i) {
          (*waiting_guard)[i]->test();
        }
        // Extract READY events
        {
          auto ready_from_waiting = waiting_guard->extract_if(
            rusty::Function<bool(const std::shared_ptr<Event>&)>(
              [](const std::shared_ptr<Event>& ev) {
                return ev->status_.get() == Event::READY;
              }));
          if (!ready_from_waiting.is_empty()) {
            ready_events.append(std::move(ready_from_waiting));
            found_ready_events = true;
          }
        }
        // Remove DONE events
        {
          waiting_guard->retain(
            rusty::Function<bool(const std::shared_ptr<Event>&)>(
              [](const std::shared_ptr<Event>& ev) {
                return ev->status_.get() != Event::DONE;
              }));
        }
      }

      // Process composite events using RefCell
      {
        auto composite_guard = composite_events_.borrow_mut();
        for (size_t i = 0; i < composite_guard->len(); ++i) {
          (*composite_guard)[i]->test();
        }
        {
          auto ready_from_composite = composite_guard->extract_if(
            rusty::Function<bool(const std::shared_ptr<Event>&)>(
              [](const std::shared_ptr<Event>& ev) {
                return ev->status_.get() == Event::READY;
              }));
          if (!ready_from_composite.is_empty()) {
            ready_events.append(std::move(ready_from_composite));
            found_ready_events = true;
          }
        }
        {
          composite_guard->retain(
            rusty::Function<bool(const std::shared_ptr<Event>&)>(
              [](const std::shared_ptr<Event>& ev) {
                return ev->status_.get() != Event::DONE;
              }));
        }
      }

      // Check timeouts using RefCell-based check_timeout
      if (do_check_timeout) {
        size_t before = ready_events.len();
        // @unsafe { check_timeout is per-method @unsafe due to raw
        // std::shared_ptr<Event> handling + Status::TIMEOUT mutation. }
        { check_timeout(ready_events); }
        if (ready_events.len() > before) {
          found_ready_events = true;
        }
      }

      // Process ready events
      // @unsafe - Weak::upgrade, continue_fiber with potential use-after-move patterns
      {
        for (size_t i = 0; i < ready_events.len(); ++i) {
          auto& ev = ready_events[i];
          if (ev->status_.get() == Event::DONE) {
            continue;
          }
          auto option_fiber = ev->wp_fiber_.upgrade();
          if (option_fiber.is_none()) {
            continue;
          }
          auto fiber = option_fiber.unwrap();
          if (!fibers_.borrow()->contains(fiber)) {
            continue;
          }
          verify(fiber->status_.get() == Fiber::PAUSED);
          if (ev->status_.get() == Event::READY) {
            ev->status_.set(Event::DONE);
          } else {
            verify(ev->status_.get() == Event::TIMEOUT);
          }
          continue_fiber(fiber);
        }
      }

      if (!infinite && !found_ready_events) {
        break;
      }
    }

  } while (looping_.get());
}

// @unsafe - Continues execution of a paused fiber; RefCell ops and fiber calls
void Reactor::continue_fiber(rusty::Rc<Fiber> fiber) const {
  // Save current running fiber for nesting support
  rusty::Option<rusty::Rc<Fiber>> old_fiber;
  // @unsafe { RefCell::borrow, Option operator=, unwrap are not borrow-checked }
  {
    auto guard = sp_running_fiber_th_.borrow();
    old_fiber = (*guard).is_some()
      ? rusty::Some((*guard).as_ref().unwrap().clone())
      : rusty::Option<rusty::Rc<Fiber>>{};
  }

  // RefCell::borrow_mut + Option::operator= are both @safe.
  { *sp_running_fiber_th_.borrow_mut() = rusty::Some(fiber.clone()); }

  // RefCell::borrow + Option::as_ref + Fiber::finished() are all @safe.
  {
    auto guard = sp_running_fiber_th_.borrow();
    verify(!(*guard).as_ref().unwrap()->finished());
  }

  n_active_fibers_.set(n_active_fibers_.get() + 1);

  if (fiber->status_.get() == Fiber::INIT) {
    fiber->run();
  } else {
    // Don't hold borrow during continue_() as fiber may call create_run().
    // This fixes RefCell double-borrow crash during server restart.
    fiber->continue_();
  }

  // RefCell::borrow + Option::as_ref + Fiber::finished() are all @safe.
  {
    auto guard = sp_running_fiber_th_.borrow();
    if ((*guard).as_ref().unwrap()->finished()) {
      auto fiber_ref = (*guard).as_ref().unwrap().clone();
      recycle(fiber_ref);
    }
  }

  // RefCell::borrow_mut + Option::operator= are both @safe.
  { *sp_running_fiber_th_.borrow_mut() = std::move(old_fiber); }
}

// @unsafe - Uses RefCell interior mutability (rusty-cpp doesn't fully support RefCell semantics)
void Reactor::recycle(rusty::Rc<Fiber>& fiber) const {
  // This fixes the bug that fibers are not recycled if they don't finish immediately.
  if (REUSING_FIBER) {
    // Use Cell/RefCell for interior mutability (safe: single-threaded)
    const auto& fiber_ref = *fiber;
    fiber_ref.status_.set(Fiber::RECYCLED);
    *fiber_ref.func_.borrow_mut() = {};
    n_idle_fibers_.set(n_idle_fibers_.get() + 1);
    available_fibers_.borrow_mut()->push(fiber.clone());  // @unsafe
  }
  n_busy_fibers_.set(n_busy_fibers_.get() - 1);
  // @unsafe - rusty-cpp false positive: Rc::clone() doesn't move, fiber is still valid
  { fibers_.borrow_mut()->remove(fiber); }
}

void Reactor::display_waiting_ev() const {
  Log_info("waiting_events_: %zu, composite_events_: %zu",
           waiting_events_.borrow()->len(), composite_events_.borrow()->len());
}

// @unsafe - Spawn a stackless task and schedule first poll on this reactor.
void Reactor::spawn_stackless_task(rusty::Task<void> task) const {
  verify(rusty::thread::current_id() == thread_id_.get());
  constexpr size_t kUnregisteredSlot = std::numeric_limits<size_t>::max();
  // @unsafe - mutable atomic fields are storage for cross-thread
  // wake-state mutations from the early_waker lambda. The struct is
  // local to this method body and does not inherit Reactor's
  // class-level @safe in intent — the rusty-cpp mutable-field rule
  // fires here because libclang qualifies local types under the
  // enclosing class scope.
  struct EarlyWakeState {
    explicit EarlyWakeState(const Reactor* reactor_ptr) : reactor(reactor_ptr) {}
    const Reactor* reactor;
    mutable std::atomic<size_t> idx{kUnregisteredSlot};
    mutable std::atomic<bool> pending_wake{false};
  };

  auto early_wake = make_arc<EarlyWakeState>(this);

  rusty::Waker early_waker{[early_wake, kUnregisteredSlot]() {
    size_t idx = early_wake->idx.load(std::memory_order_acquire);
    if (idx == kUnregisteredSlot) {
      early_wake->pending_wake.store(true, std::memory_order_release);
      return;
    }
    early_wake->reactor->enqueue_stackless_task(idx);
  }};
  rusty::Context early_ctx{&early_waker};
  auto early_poll = task.poll(early_ctx);
  if (early_poll.is_ready()) {
    return;
  }

  // @unsafe - mutable Task field is needed because the registered
  // poller closure must call `task.poll(ctx)` which mutates the Task,
  // and the closure receives `TaskState` by const Arc.
  struct TaskState {
    mutable rusty::Task<void> task;
    rusty::Arc<EarlyWakeState> early_wake;

    TaskState(rusty::Task<void> t, rusty::Arc<EarlyWakeState> ew)
        : task(std::move(t)), early_wake(std::move(ew)) {}
  };

  auto state = make_arc<TaskState>(std::move(task), std::move(early_wake));
  auto idx = register_stackless_poller([state](rusty::Context& ctx) mutable {
    auto poll_result = state->task.poll(ctx);
    if (poll_result.is_ready()) {
      state->early_wake->idx.store(kUnregisteredSlot, std::memory_order_release);
      return true;
    }
    return false;
  });
  state->early_wake->idx.store(idx, std::memory_order_release);
  if (state->early_wake->pending_wake.exchange(false, std::memory_order_acq_rel)) {
    enqueue_stackless_task(idx);
  }
}

// =============================================================================
// PollThreadWorker Implementation
// =============================================================================

PollThreadWorker::PollThreadWorker(rusty::sync::mpsc::Receiver<PollCommand> receiver)
    : receiver_(std::move(receiver)),
      poll_(),
      fd_to_pollable_(),
      mode_(),
      pending_remove_(),
      jobs_(),
      stop_(false) {
  // No eventfd needed - we poll the channel with try_recv() after each epoll_wait
}

// @unsafe - factory function creates worker and wraps in Rc<RefCell> (rustycpp false positive on move)
rusty::Rc<rusty::RefCell<PollThreadWorker>> PollThreadWorker::create(rusty::sync::mpsc::Receiver<PollCommand> receiver) {
  // Create worker, then wrap in RefCell
  PollThreadWorker worker(std::move(receiver));
  return rusty::Rc<rusty::RefCell<PollThreadWorker>>::make(std::move(worker));
}

void PollThreadWorker::poll_loop() {
  Log_debug("[poll_loop] Starting poll loop");
  while (!stop_) {
    trigger_job();

    // Wait for events (epoll_wait with short timeout)
    // Dispatch through proxy storage by fd; no Pollable* userdata assumptions.
    poll_.Wait([this](int fd, int ready_events) {
      auto poll_opt = fd_to_pollable_.get(fd);
      if (poll_opt.is_none()) {
        return;
      }
      auto& poll = poll_opt.unwrap();

      if (ready_events & PollReady::READABLE) {
        poll->handle_read();
      }
      if (ready_events & PollReady::WRITABLE) {
        int new_mode = poll->handle_write();
        if (new_mode != PollMode::NO_CHANGE) {
          do_update_mode(fd, new_mode);
        }
      }
      if (ready_events & PollReady::ERROR) {
        poll->handle_error();
      }
    });

    // Process commands from channel (non-blocking try_recv)
    process_commands();

    trigger_job();

    // Process deferred removals
    process_pending_removals();

    trigger_job();
    Reactor::get_reactor()->loop();

    // Check for pending write updates (set by end_reply() during fiber execution)
    // @unsafe - const_cast needed because Arc provides const access, but we know the
    // underlying Pollable uses interior mutability (mutable pending_write_update_ flag)
    for (auto [fd, poll] : fd_to_pollable_) {
      if (poll->check_pending_write_update()) {
        do_update_mode(fd, PollMode::READ | PollMode::WRITE);
      }
    }

    // Check for pollables closed by handle_error() and remove them
    // This prevents fd reuse issues when old connection is closed but not removed
    rusty::Vec<int> closed_fds;
    for (auto [fd, poll] : fd_to_pollable_) {
      if (poll->is_closed()) {
        closed_fds.push(fd);
      }
    }
    for (int fd : closed_fds) {
      auto proxy_opt = fd_to_pollable_.get(fd);
      if (proxy_opt.is_some()) {
        // Remove from epoll if still registered
        if (mode_.contains_key(fd)) {
          poll_.Remove(fd);
        }

        // Invoke close callback before erasing map entry so cleanup hooks run.
        // HashMap::get now returns Option<V&>; unwrap() is already the
        // PollableProxy reference, no extra deref.
        proxy_opt.unwrap()->close();

        fd_to_pollable_.remove(fd);
        mode_.remove(fd);
      }
    }
  }

  Log_debug("[poll_loop] Exited while loop (stop_=true), starting cleanup");
  // Shutdown cleanup - remove all registered pollables
  for (auto [fd, poll] : fd_to_pollable_) {
    if (mode_.contains_key(fd)) {
      poll_.Remove(fd);
    }
  }
  fd_to_pollable_.clear();
  mode_.clear();
  pending_remove_.clear();
  Log_debug("[poll_loop] Cleanup complete, poll_loop exiting");
}

// @unsafe - calls try_recv and std::visit
void PollThreadWorker::process_commands() {
  // Non-blocking receive: process all pending commands
  int cmd_count = 0;
  while (true) {
    auto result = receiver_.try_recv();
    if (result.is_err()) {
      // Empty or disconnected - either way, stop processing
      break;
    }
    cmd_count++;
    auto cmd = result.unwrap();
    std::visit([this](auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, CmdAddPollable>) {
        do_add_pollable(std::move(arg.pollable));
      } else if constexpr (std::is_same_v<T, CmdRemovePollable>) {
        do_remove_pollable(arg.fd);
      } else if constexpr (std::is_same_v<T, CmdClosePollable>) {
        do_close_pollable(arg.fd);
      } else if constexpr (std::is_same_v<T, CmdUpdateMode>) {
        do_update_mode(arg.fd, arg.new_mode);
      } else if constexpr (std::is_same_v<T, CmdAddJob>) {
        do_add_job(std::move(arg.job));
      } else if constexpr (std::is_same_v<T, CmdRemoveJob>) {
        do_remove_job(std::move(arg.job));
      } else if constexpr (std::is_same_v<T, CmdShutdown>) {
        stop_ = true;
      }
    }, cmd);
  }
}

// @safe - rusty::BTreeSet::clone/clear/insert and rusty::Arc are @safe;
// only the raw `Job*` extraction + virtual dispatch escapes into inner
// @unsafe blocks.
void PollThreadWorker::trigger_job() {
  // Copy jobs to process (in case jobs modify the set).
  rusty::BTreeSet<rusty::Arc<Job>> jobs_exec = jobs_.clone();
  jobs_.clear();

  for (const auto& job : jobs_exec) {
    bool ready;
    // @unsafe { const_cast<Job*> + virtual Ready() dispatch }
    {
      Job* job_ptr = const_cast<Job*>(job.get());
      ready = job_ptr->Ready();
    }
    if (ready) {
      // Capture job by value to keep the Arc alive.
      Fiber::create_run([job]() {
        // @unsafe { const_cast<Job*> + virtual Work() dispatch }
        {
          Job* job_ptr = const_cast<Job*>(job.get());
          job_ptr->Work();
        }
      });
      // Don't re-add ready jobs that were executed.
    } else {
      // Re-add jobs that aren't ready yet - they should be checked again later.
      jobs_.insert(job);
    }
  }
}

// @unsafe - PollableProxy accessors and Epoll::Add are not borrow-checked
void PollThreadWorker::do_add_pollable(PollableProxy poll) {
  int fd;
  int poll_mode;
  // @unsafe { PollableProxy::fd, poll_mode are not borrow-checked }
  {
    fd = poll->fd();
    poll_mode = poll->poll_mode();
  }

  // Check if already exists
  if (fd_to_pollable_.contains_key(fd)) {
    return;
  }

  // Store in maps
  fd_to_pollable_.insert(fd, std::move(poll));
  mode_.insert(fd, poll_mode);

  // @unsafe { Epoll::Add is not borrow-checked }
  { poll_.Add(fd, poll_mode); }
}

// @safe - rusty::HashMap::contains_key + rusty::HashSet::insert are @safe.
void PollThreadWorker::do_remove_pollable(int fd) {
  if (!fd_to_pollable_.contains_key(fd)) {
    return;
  }
  // Add to pending_remove (actual removal happens after epoll_wait).
  pending_remove_.insert(fd);
}

// @safe - rusty::HashMap / HashSet ops are @safe; only the
// Epoll::Remove syscall path and the virtual Pollable::close()
// dispatch escape into inner @unsafe blocks.
void PollThreadWorker::do_close_pollable(int fd) {
  // Remove from pending_remove if present.
  pending_remove_.remove(fd);

  auto proxy_opt = fd_to_pollable_.get(fd);
  if (proxy_opt.is_none()) {
    return;
  }

  // Remove from epoll if still registered.
  if (mode_.contains_key(fd)) {
    // @unsafe { Epoll::Remove issues an epoll_ctl/kevent syscall }
    { poll_.Remove(fd); }
  }

  // Close the socket via Pollable's close() method.
  // HashMap::get now returns Option<V&>; unwrap() yields the proxy ref.
  // @unsafe { virtual Pollable::close() dispatch }
  { proxy_opt.unwrap()->close(); }

  // Erase from maps, dropping storage references.
  fd_to_pollable_.remove(fd);
  mode_.remove(fd);
}

// @unsafe - Uses raw pointers for epoll userdata and calls Epoll::Update
void PollThreadWorker::do_update_mode(int fd, int new_mode) {
  if (!fd_to_pollable_.contains_key(fd)) {
    return;
  }

  auto mode_opt = mode_.get(fd);
  if (mode_opt.is_none()) {
    return;
  }

  int old_mode = mode_opt.unwrap();
  mode_.insert(fd, new_mode);

  if (new_mode != old_mode) {
    poll_.Update(fd, new_mode, old_mode);
  }
}

// @safe - rusty::BTreeSet::insert is @safe via namespace inheritance.
void PollThreadWorker::do_add_job(rusty::Arc<Job> job) {
  jobs_.insert(job);
}

// @safe - rusty::BTreeSet::remove is @safe via namespace inheritance.
void PollThreadWorker::do_remove_job(rusty::Arc<Job> job) {
  jobs_.remove(job);
}

// @safe - the rusty::HashSet / HashMap ops are @safe; only `poll_.Remove(fd)`
// (Epoll::Remove, a syscall-issuing path) escapes into an inner @unsafe block.
void PollThreadWorker::process_pending_removals() {
  rusty::HashSet<int> remove_fds = pending_remove_.clone();
  pending_remove_.clear();

  for (int fd : remove_fds) {
    if (!fd_to_pollable_.contains_key(fd)) {
      continue;
    }

    // Check if fd was NOT reused (still in mode map).
    if (mode_.contains_key(fd)) {
      // @unsafe { Epoll::Remove issues an epoll_ctl/kevent syscall }
      { poll_.Remove(fd); }
    }

    fd_to_pollable_.remove(fd);
    mode_.remove(fd);
  }
}


// @safe - Update poll mode directly (bypasses channel)
// Only safe to call from the poll thread (e.g., from ServerConnection::end_reply)
// SAFETY: Internal @unsafe block handles epoll operations and address-of
void PollThreadWorker::update_mode(Pollable& poll, int new_mode) {
  // @unsafe - address-of operation and epoll modification
  { do_update_mode(poll.fd(), new_mode); }
}

// =============================================================================
// PollThread Implementation
// =============================================================================

PollThread::PollThread(rusty::sync::mpsc::Sender<PollCommand> sender)
    : sender_(std::move(sender)),
      join_handle_(rusty::None),
      poll_thread_id_(),
      shutdown_called_(false) {
}

// @unsafe - takes address-of an atomic field (`&arc->poll_thread_id_`)
// and passes the raw pointer into a spawned thread closure. The Arc
// keeps the PollThread (and thus the atomic) alive until the worker
// thread finishes; rusty-cpp can't express that lifetime relationship.
rusty::Arc<PollThread> PollThread::create() {
  // Create MPSC channel
  auto [sender, receiver] = rusty::sync::mpsc::channel<PollCommand>();

  // Create PollThread with sender
  auto arc = rusty::Arc<PollThread>::make(std::move(sender));

  // Pointer to atomic thread ID for safe cross-thread access
  rusty::sync::atomic::Atomic<rusty::thread::ThreadId>* thread_id_ptr = &arc->poll_thread_id_;

  // Spawn thread - worker owns the receiver
  auto handle = rusty::thread::spawn(
    [thread_id_ptr](rusty::sync::mpsc::Receiver<PollCommand> rx) {
      auto tid = rusty::thread::current_id();
      thread_id_ptr->store(tid, rusty::sync::atomic::Ordering::Release);
      // Create worker wrapped in Rc<RefCell<>>
      auto worker = PollThreadWorker::create(std::move(rx));
      // Store raw pointer in TLS for direct access from same thread
      // The borrow_mut guard keeps RefCell borrowed during poll_loop()
      // Using raw pointer avoids RefCell re-borrow issues in fibers
      auto guard = worker->borrow_mut();
      PollThreadWorker::current_worker_ = &*guard;
      guard->poll_loop();
      PollThreadWorker::current_worker_ = nullptr;  // Clear on exit
    },
    std::move(receiver)
  );

  // Store handle
  {
    auto guard = arc->join_handle_.lock().unwrap();
    *guard = rusty::Some(std::move(handle));
  }

  return arc;
}

PollThread::~PollThread() {
  pid_t tid = syscall(SYS_gettid);
  Log_debug("[PollThread::~PollThread] Destructor called from TID=%d", (int)tid);
  shutdown();
  Log_debug("[PollThread::~PollThread] Destructor complete");
}

void PollThread::shutdown() const {
  pid_t main_tid = syscall(SYS_gettid);
  Log_debug("[PollThread::shutdown] Called from TID=%d", (int)main_tid);
  if (shutdown_called_.exchange(true)) {
    Log_debug("[PollThread::shutdown] Already called, returning");
    return;  // Already called
  }

  // Send shutdown command via channel
  Log_debug("[PollThread::shutdown] Sending CmdShutdown");
  sender_.send(CmdShutdown{});
  Log_debug("[PollThread::shutdown] CmdShutdown sent");

  // Check if we're on the poll thread (atomic load for thread-safe read)
  auto current_tid = rusty::thread::current_id();
  auto poll_tid = poll_thread_id_.load(rusty::sync::atomic::Ordering::Acquire);
  if (current_tid == poll_tid) {
    Log_debug("[PollThread::shutdown] Called from poll thread, skipping join");
    return;
  }

  // Join thread
  Log_debug("[PollThread::shutdown] Acquiring join_handle lock...");
  {
    auto guard = join_handle_.lock().unwrap();
    Log_debug("[PollThread::shutdown] join_handle lock acquired");
    if ((*guard).is_some()) {
      Log_debug("[PollThread::shutdown] Calling thread.join()...");
      (*guard).take().unwrap().join();
      Log_debug("[PollThread::shutdown] thread.join() completed!");
    } else {
      Log_debug("[PollThread::shutdown] join_handle is None, thread already joined");
    }
  }
  Log_debug("[PollThread::shutdown] Released join_handle lock");
  Log_debug("[PollThread::shutdown] Complete");
}

void PollThread::add_proxy(PollableProxy poll) const {
  sender_.send(CmdAddPollable{std::move(poll)});
}

void PollThread::remove(Pollable& poll) const {
  sender_.send(CmdRemovePollable{poll.fd()});
}

void PollThread::request_close(int fd) const {
  sender_.send(CmdClosePollable{fd});
}

// @safe - Sends update mode command via channel (send wrapped @unsafe)
// SAFETY: Channel send is thread-safe
void PollThread::update_mode(int fd, int new_mode) const {
  // @unsafe { mpsc::Sender::send is not borrow-checked }
  {
  auto result = sender_.send(CmdUpdateMode{fd, new_mode});
  if (result.is_err()) {
    Log_error("PollThread::update_mode: send failed! Channel disconnected?");
  }
  }
}

void PollThread::update_mode(const Pollable& poll, int new_mode) const {
  update_mode(poll.fd(), new_mode);
}

void PollThread::add(rusty::Arc<Job> job) const {
  sender_.send(CmdAddJob{std::move(job)});
}

void PollThread::remove(rusty::Arc<Job> job) const {
  sender_.send(CmdRemoveJob{std::move(job)});
}


// --- from fiber_context_runtime.cc --------------------------------------

// fiber_swap_context is implemented in arch-specific files:
//   fiber_context_x86_64.cc  (x86_64)
//   fiber_context_aarch64.cc (AArch64/ARM64)

thread_local fiber_task_t* fiber_task_t::tls_active_task_ = nullptr;

void fiber_yield_t::operator()() {
  verify(task_ != nullptr);
  task_->yield_to_caller();
}

fiber_task_t::fiber_task_t(TaskFn fn)
    : fn_(std::move(fn)),
      yield_(*this) {
  init_context();
  // Match Boost.Coroutine2 pull_type behavior: run immediately on construction.
  resume();
}

fiber_task_t::~fiber_task_t() {
  if (stack_mapping_ != nullptr) {
    int rc = munmap(stack_mapping_, stack_mapping_bytes_);
    verify(rc == 0);
    stack_mapping_ = nullptr;
    stack_mapping_bytes_ = 0;
  }
}

void fiber_task_t::operator()() {
  resume();
}

// @unsafe - mmap stack region, install guard page via mprotect,
// reinterpret_cast the trampoline address and stack-top into the
// ABI-specific FiberContext (rsp/rip on x86_64, sp/pc on aarch64).
// The whole body is raw-pointer arithmetic by design.
void fiber_task_t::init_context() {
  std::size_t page_sz =
      static_cast<std::size_t>(rusty::sys::process::sysconf(_SC_PAGESIZE));
  if (page_sz == 0) {
    page_sz = 4096;
  }

  stack_mapping_bytes_ = kDefaultStackBytes + page_sz;
  void* mapping = mmap(nullptr,
                       stack_mapping_bytes_,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS,
                       -1,
                       0);
  verify(mapping != MAP_FAILED);
  int protect_rc = mprotect(mapping, page_sz, PROT_NONE);
  verify(protect_rc == 0);
  stack_mapping_ = mapping;

  std::uintptr_t stack_top =
      reinterpret_cast<std::uintptr_t>(static_cast<char*>(stack_mapping_) + stack_mapping_bytes_);
  stack_top &= ~static_cast<std::uintptr_t>(0xF);

  fiber_ctx_ = FiberContext{};
  caller_ctx_ = FiberContext{};
  const auto trampoline_addr =
      reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(&fiber_task_t::entry_trampoline));
#if defined(__x86_64__)
  // SysV x86_64 ABI: %rsp % 16 == 8 on function entry (simulates a call pushing ret addr).
  stack_top -= sizeof(void*);
  *reinterpret_cast<void**>(stack_top) = nullptr;
  fiber_ctx_.rsp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.rip = trampoline_addr;
#elif defined(__aarch64__)
  // AAPCS64: sp must be 16-byte aligned on function entry. No fake return address needed;
  // the trampoline address is stored in pc (= lr) and entered via ret.
  fiber_ctx_.sp = reinterpret_cast<void*>(stack_top);
  fiber_ctx_.pc = trampoline_addr;
#endif
}

// @unsafe - fiber context switch via raw `fiber_task_t*` thread-local
// (`tls_active_task_`) save/restore + `&caller_ctx_`/`&fiber_ctx_`
// address-of into `fiber_swap_context`. The whole call is the fiber-
// switching primitive.
void fiber_task_t::resume() {
  if (state_ == State::FINISHED) {
    return;
  }
  auto* old = tls_active_task_;
  tls_active_task_ = this;
  fiber_swap_context(&caller_ctx_, &fiber_ctx_);
  tls_active_task_ = old;
}

// @unsafe - companion to resume() — `&fiber_ctx_`/`&caller_ctx_` into
// the fiber-switching primitive.
void fiber_task_t::yield_to_caller() {
  verify(state_ == State::RUNNING);
  state_ = State::SUSPENDED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  if (state_ != State::FINISHED) {
    state_ = State::RUNNING;
  }
}

// @unsafe - reads the raw `fiber_task_t*` thread-local set by resume()
// and dispatches into the fiber's entry routine.
void fiber_task_t::entry_trampoline() {
  auto* task = tls_active_task_;
  verify(task != nullptr);
  task->entry();
}

// @unsafe - uses raw `this` for the fiber-finished callback dispatch.
[[noreturn]] void fiber_task_t::entry() {
  state_ = State::RUNNING;
  verify(static_cast<bool>(fn_));
  fn_(yield_);
  state_ = State::FINISHED;
  fiber_swap_context(&fiber_ctx_, &caller_ctx_);
  std::abort();
}

}  // namespace rrr (definitions)

// --- from quorum_event.cc ------------------------------------------------
// @safe - QuorumEvent impl. Methods carry per-method annotations.
namespace janus {

using rrr::Event;
using rrr::IntEvent;
using rrr::Fiber;
using rrr::Time;
using rrr::verify;

QuorumEvent::QuorumEvent(int n_total, int quorum)
    : Event(), n_total_(n_total), quorum_(quorum) {
  finalize_event_ = std::make_shared<IntEvent>(n_total_);
  finalize_event_->__debug_creator = 1;
  begin_timestamp_ = Time::now(true);
}

void QuorumEvent::finalize(
    uint64_t timeout,
    rusty::Function<bool(rusty::Vec<std::pair<uint16_t, rrr::i64> > &)> finalize_func) {


  // rusty::Function is move-only, so capture the callback by move
  // into the background fiber's lambda.  The lambda must also be
  // `mutable` so the captured (non-const) Function can be invoked.
  Fiber::create_run([timeout, finalize_func = std::move(finalize_func), this]() mutable {
    bool ret = false;

    auto final_ev = finalize_event_;  // have to make a copy of finalized event (for reason, see comment A)
    rusty::Vec<std::pair<uint16_t, rrr::i64> > dangling_rpc;
    for (auto it : xids_)
      dangling_rpc.push(it);  // fetch out dangling rpc info before it's freed (see comment A)

    final_ev->wait(timeout);
    /* A: by the time this fires, the quorum event could have been freed. Thus,
     avoid accesing the quorum event object or its members after this line */

    // didn't receive all RPC replies
    if (final_ev->status_.get() == Event::TIMEOUT) {
      // Log_info("finalized timeout");
      ret = finalize_func(dangling_rpc);
    }
    (void)ret;
  }, __FILE__, __LINE__);
}

void QuorumEvent::add_xid(uint16_t site, rrr::i64 xid) {
  xids_[site] = xid;
}

void QuorumEvent::remove_xid(uint16_t site) {
  xids_.remove(site);
}

void QuorumEvent::vote_yes() {
  n_voted_yes_++;
  test();
  vec_timestamp_.push(Time::now(true) - begin_timestamp_);

  if (finalize_event_->status_.get() != Event::TIMEOUT)
    finalize_event_->set(n_voted_yes_ + n_voted_no_);
}

void QuorumEvent::vote_no() {
  n_voted_no_++;
  test();

  if (finalize_event_->status_.get() != Event::TIMEOUT)
    finalize_event_->set(n_voted_yes_ + n_voted_no_);
}

void QuorumEvent::log_event() {
  for (auto t : vec_timestamp_)
    std::cout << " " << t;
  std::cout << std::endl;
}

}  // namespace janus (definitions)
