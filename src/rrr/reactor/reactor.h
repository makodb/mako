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
#include <unistd.h>
#include <rusty/rusty.hpp>
#include <rusty/thread.hpp>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include <rusty/sync/mpsc.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/async.hpp>
#include <rusty/vecdeque.hpp>
#include <rusty/btreeset.hpp>

// External safety annotations for system functions used in this module
// @external: {
//   pthread_setname_np: [unsafe, (pthread_t, const char*) -> int]
//   epoll_create: [unsafe, (int) -> int]
//   epoll_ctl: [unsafe, (int, int, int, struct epoll_event*) -> int]
//   epoll_wait: [unsafe, (int, struct epoll_event*, int, int) -> int]
//   kqueue: [unsafe, () -> int]
//   kevent: [unsafe, (int, const struct kevent*, int, struct kevent*, int, const struct timespec*) -> int]
//   close: [unsafe, (int) -> int]
//   std::__atomic_base::load: [unsafe, (std::memory_order) -> auto]
//   rusty::Function::Function: [safe]
//   rusty::Function::operator(): [safe]
//   rusty::Rc::Rc: [safe]
//   rusty::Rc::make: [safe]
//   rusty::Rc::clone: [safe]
// }

// External safety annotations for STL and RustyCpp operations
// @external: {
//   operator!=: [unsafe, (auto, auto) -> bool]
//   operator==: [unsafe, (auto, auto) -> bool]
//   std::*::find: [unsafe, (auto) -> auto]
//   std::*::end: [unsafe, () -> auto]
//   std::*::begin: [unsafe, () -> auto]
//   std::*::insert: [unsafe, (auto...) -> auto]
//   std::*::erase: [unsafe, (auto) -> auto]
//   std::*::clear: [unsafe, () -> void]
//   std::*::empty: [unsafe, () -> bool]
//   std::*::size: [unsafe, () -> size_t]
//   std::*::back: [unsafe, () -> auto&]
//   std::*::pop_back: [unsafe, () -> void]
//   std::*::push_back: [unsafe, (auto) -> void]
//   std::*::insert_or_assign: [unsafe, (auto...) -> auto]
//   std::*::operator[]: [unsafe, (auto) -> auto&]
//   rusty::Arc::make: [unsafe, (auto...) -> rusty::Arc<auto>]
//   rusty::Arc::operator*: [unsafe, () -> auto&]
//   rusty::Arc::operator->: [unsafe, () -> auto*]
//   rusty::Arc::get: [unsafe, () -> auto*]
//   rusty::Arc::operator=: [unsafe, (const rusty::Arc<auto>&) -> rusty::Arc<auto>&]
//   rusty::Arc::Arc: [unsafe, (auto...) -> void]
//   std::visit: [unsafe, (auto...) -> auto]
//   visit: [unsafe, (auto...) -> auto]
//   std::move: [unsafe, (auto) -> auto]
//   rusty::rc::Weak::Weak: [safe, () -> void]
//   rusty::Option::Option: [unsafe, (auto...) -> void]
//   rusty::Option::is_none: [unsafe, () -> bool]
//   rusty::Option::is_some: [unsafe, () -> bool]
//   rusty::Option::as_ref: [unsafe, () -> auto]
//   rusty::Option::unwrap: [unsafe, () -> auto]
//   rusty::Rc::clone: [unsafe, () -> auto]
//   rusty::Arc::Arc: [unsafe, (auto...) -> void]
//   rusty::Arc::clone: [unsafe, () -> auto]
//   rusty::Arc::get: [unsafe, () -> auto*]
//   rusty::sync::mpsc::Receiver::Receiver: [unsafe, (auto...) -> void]
//   rusty::sync::mpsc::Receiver::try_recv: [unsafe, () -> auto]
//   rusty::RefCell::borrow_mut: [unsafe, () -> auto]
//   rrr::Log::debug: [safe, (auto...) -> void]
//   rrr::Log::error: [safe, (auto...) -> void]
//   Log_debug: [safe, (auto...) -> void]
//   Log_error: [safe, (auto...) -> void]
//   rrr::Event::Test: [unsafe, () -> bool]
//   rrr::Time::now: [unsafe, (auto...) -> auto]
//   verify: [unsafe, (auto) -> void]
// }




#include "../base/all.hpp"
#include "../rpc/pollable_proxy.h"


#include "event.h"
#include "quorum_event.h"
#include "fiber_impl.h"
#include "epoll_wrapper.h"

namespace rrr {

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
  static thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_;
  static thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_;
  // Thread-local current fiber with single-threaded Rc
  // Wrapped in RefCell for explicit interior mutability (Cell<T> requires trivially_copyable)
  static thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_fiber_th_;

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
  static thread_local rusty::HashMap<std::string, rusty::Vec<PollableProxy>> clients_;
  static thread_local rusty::HashSet<std::string> dangling_ips_;
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
  struct StacklessTaskEntry {
    bool active = false;
    bool queued = false;
    std::function<bool(rusty::Context&)> poll_once;
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
  size_t register_stackless_poller(std::function<bool(rusty::Context&)> poller) const;

  // @safe - Poll all queued stackless tasks once.
  // Returns true if at least one stackless task was polled.
  bool process_stackless_tasks() const;

  // @safe - Arc::make wrapper with localized unsafe allocation boundary.
  template <typename U, typename... Args>
  static rusty::Arc<U> make_arc(Args&&... args) {
    // @unsafe
    { return rusty::Arc<U>::make(std::forward<Args>(args)...); }
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

} // namespace rrr

// Mark PollCommand as Send for use with rusty::sync::mpsc channel
namespace rusty {
template<>
struct is_send<rrr::PollCommand> : std::true_type {};
} // namespace rusty

namespace rrr {

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
    // Only accessed via with_current_worker() which provides safe reference access
    static thread_local PollThreadWorker* current_worker_;

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

} // namespace rrr

// Trait specializations for PollThread
// PollThread is Send + Sync because channel operations are thread-safe
namespace rusty {
template<>
struct is_send<rrr::PollThread> : std::true_type {};

template<>
struct is_sync<rrr::PollThread> : std::true_type {};
} // namespace rusty
