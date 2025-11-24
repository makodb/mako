#pragma once
#include <algorithm>
#include <list>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <rusty/rusty.hpp>
#include <rusty/thread.hpp>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include "base/misc.hpp"
#include "event.h"
#include "quorum_event.h"
#include "coroutine.h"
#include "epoll_wrapper.h"

// External safety annotations for system functions used in this module
// @external: {
//   pthread_setname_np: [unsafe, (pthread_t, const char*) -> int]
// }

// External safety annotations for STL operations
// @external: {
//   operator!=: [unsafe, (auto, auto) -> bool]
//   operator==: [unsafe, (auto, auto) -> bool]
//   std::*::find: [unsafe, (auto) -> auto]
//   std::*::end: [unsafe, () -> auto]
//   std::make_shared: [unsafe, (auto...) -> std::shared_ptr<auto>]
//   std::shared_ptr::operator*: [unsafe, () -> auto&]
//   std::shared_ptr::operator->: [unsafe, () -> auto*]
//   std::shared_ptr::get: [unsafe, () -> auto*]
//   std::shared_ptr::operator=: [unsafe, (const std::shared_ptr<auto>&) -> std::shared_ptr<auto>&]
//   std::shared_ptr::shared_ptr: [unsafe, (auto...) -> void]
//   std::list::push_back: [unsafe, (auto) -> void]
//   std::vector::push_back: [unsafe, (auto) -> void]
// }

namespace rrr {

using std::make_unique;
using std::make_shared;

class Coroutine;
// TODO for now we depend on the rpc services, fix in the future.
// @unsafe - Thread-safe reactor with thread-local storage and mutable fields for interior mutability
class Reactor {
 public:
  // Default constructor - all fields have default constructors
  Reactor() = default;

  // Delete copy and move constructors (RefCell and Cell are not copyable/movable)
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;
  Reactor(Reactor&&) = delete;
  Reactor& operator=(Reactor&&) = delete;

  // Returns thread-local reactor instance with single-threaded Rc
  // SAFETY: Thread-local storage, single-threaded access only
  static rusty::Rc<Reactor> GetReactor();
  static thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_;
  // Thread-local current coroutine with single-threaded Rc
  static thread_local rusty::Option<rusty::Rc<Coroutine>> sp_running_coro_th_;
  /**
   * A reactor needs to keep reference to all coroutines created,
   * in case it is freed by the caller after a yield.
   */
  // Events managed with std::shared_ptr (polymorphism support)
  // Interior mutability for const methods
  mutable std::list<std::shared_ptr<Event>> all_events_{};
  mutable std::list<std::shared_ptr<Event>> waiting_events_{};
  // Coroutines managed with single-threaded Rc
  mutable std::set<rusty::Rc<Coroutine>> coros_{};
  mutable std::vector<rusty::Rc<Coroutine>> available_coros_{};
  mutable std::unordered_map<uint64_t, std::function<void(Event&)>> processors_{};
  mutable std::list<std::shared_ptr<Event>> timeout_events_{};
  mutable bool looping_{false};
  std::thread::id thread_id_{};
#ifdef REUSE_CORO
#define REUSING_CORO (true)
#else
#define REUSING_CORO (false)
#endif

  // Checks and processes timeout events with std::shared_ptr
  void CheckTimeout(std::vector<std::shared_ptr<Event>>&) const;
  /**
   * @param ev. is usually allocated on coroutine stack. memory managed by user.
   */
  // Creates and runs a new coroutine with rusty::Rc ownership
  rusty::Rc<Coroutine> CreateRunCoroutine(std::move_only_function<void()> func) const;
  // Main event loop
  void Loop(bool infinite = false) const;
  // Continues execution of a paused coroutine with rusty::Rc
  void ContinueCoro(rusty::Rc<Coroutine> sp_coro) const;

  ~Reactor() {
//    verify(0);
  }
  friend Event;

  // @unsafe - Creates std::shared_ptr event with perfect forwarding
  // SAFETY: Uses std::shared_ptr for polymorphism support. Lifetime is safe because:
  //   1. shared_ptr is stored in all_events_ list (owned by reactor)
  //   2. Reactor lives for entire program duration
  //   3. Events are never removed from all_events_ until reactor destruction
  // Manual verification required due to template complexity and std::shared_ptr usage
  template <typename Ev, typename... Args>
  static std::shared_ptr<Ev> CreateSpEvent(Args&&... args) {  // @unsafe
    auto sp_ev = std::make_shared<Ev>(args...);
    sp_ev->__debug_creator = 1;
    // TODO push them into a wait queue when they actually wait.
    auto reactor = GetReactor();
    // Rc gives const access, use const_cast for mutation (safe: thread-local, single owner)
    auto& events = const_cast<Reactor&>(*reactor).all_events_;
    events.push_back(sp_ev);
    return sp_ev;
  }

  // @unsafe - Creates event and returns reference to shared_ptr content
  // SAFETY: Returned reference is valid because:
  //   1. Event is created via CreateSpEvent and stored in all_events_
  //   2. all_events_ is never cleared during reactor lifetime
  //   3. Returned reference points to heap-allocated Event managed by shared_ptr
  // Manual verification required: reference lifetime extends beyond function scope
  template <typename Ev, typename... Args>
  static Ev& CreateEvent(Args&&... args) {  // @unsafe
    return *CreateSpEvent<Ev>(args...);
  }
};

// @unsafe - Uses mutable fields for thread-safe interior mutability
// SAFETY: All mutable state is protected by SpinLocks for thread-safety
class PollThreadWorker {
    // Friend Arc to allow make access to private constructor
    friend class rusty::Arc<PollThreadWorker>;

private:
    // Use mutable for thread-shared state (thread-safe with SpinLocks)
    // RefCell is NOT thread-safe and causes "already mutably borrowed" panics
    mutable Epoll poll_;

    // Wrap non-movable SpinLocks in rusty::Box to make class movable
    mutable rusty::Box<SpinLock> l_;
    // Uses rusty::Arc<Pollable> for polymorphic thread-safe reference counting
    // SAFETY: Arc provides thread-safe reference counting with built-in polymorphism support
    // Pollable is abstract base class with multiple derived types (Client, ServerConnection, etc.)
    // Authoritative storage: fd -> Arc<Pollable>
    mutable std::unordered_map<int, rusty::Arc<Pollable>> fd_to_pollable_;
    mutable std::unordered_map<int, int> mode_; // fd->mode

    // Uses std::shared_ptr<Job> for polymorphic thread-safe reference counting
    // Note: Arc doesn't support polymorphism, so we use shared_ptr for Job storage
    mutable std::set<std::shared_ptr<Job>> set_sp_jobs_;

    mutable std::unordered_set<int> pending_remove_;  // Store fds to remove
    mutable rusty::Box<SpinLock> pending_remove_l_;
    mutable rusty::Box<SpinLock> lock_job_;

    // join_handle_ accessed during shutdown - use mutex for thread safety
    // Mutex is needed because PollThreadWorker is shared via Arc
    mutable rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<void>>> join_handle_;
    mutable rusty::Box<std::atomic<bool>> stop_flag_;  // Wrap atomic to make movable

    // Private constructor - use create() factory
    PollThreadWorker();

    // @unsafe - Triggers ready jobs in coroutines
    // SAFETY: Uses spinlock for thread safety
    void TriggerJob() const;

public:
    ~PollThreadWorker();

    // Factory method returns Arc<PollThreadWorker>
    static rusty::Arc<PollThreadWorker> create();

    // Member function for thread - not static!
    void poll_loop() const;

    // Explicit shutdown (replaces RAII)
    void shutdown() const;

    PollThreadWorker(const PollThreadWorker&) = delete;
    PollThreadWorker& operator=(const PollThreadWorker&) = delete;

    // Move operations deleted - RefCell is not movable, use Arc for sharing
    PollThreadWorker(PollThreadWorker&& other) = delete;
    PollThreadWorker& operator=(PollThreadWorker&& other) = delete;

    // Thread-safe addition of polymorphic pollable object
    // SAFETY: Arc provides built-in polymorphism support, protected by spinlock
    void add(rusty::Arc<Pollable> poll) const;

    // Thread-safe removal of pollable object
    void remove(Pollable& poll) const;
    // Thread-safe mode update
    void update_mode(Pollable& poll, int new_mode) const;

    // Frequent Job
    // Thread-safe job management with polymorphic shared_ptr
    // SAFETY: shared_ptr provides built-in polymorphism support, protected by spinlock
    void add(std::shared_ptr<Job> sp_job) const;
    void remove(std::shared_ptr<Job> sp_job) const;

    // For testing: get number of epoll Remove() calls
    int get_remove_count() const { return poll_.remove_count_.load(); }
};

} // namespace rrr

// Trait specializations for PollThreadWorker
// PollThreadWorker is Send + Sync because:
// - All methods are const with interior mutability via internal SpinLocks
// - All members are mutable
// - Designed for thread-safe concurrent access
namespace rusty {
template<>
struct is_send<rrr::PollThreadWorker> : std::true_type {};

template<>
struct is_sync<rrr::PollThreadWorker> : std::true_type {};
} // namespace rusty
