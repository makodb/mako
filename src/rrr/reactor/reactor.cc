
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include "../base/all.hpp"
#include "reactor.h"
#include "coroutine.h"
#include "event.h"
#include "epoll_wrapper.h"

namespace rrr {

thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_reactor_th_{};
thread_local rusty::Option<rusty::Rc<Coroutine>> Reactor::sp_running_coro_th_{};

// @unsafe - Returns current coroutine with single-threaded reference counting
// SAFETY: Returns copy of thread-local Rc - single-threaded, no synchronization needed
// Returns None if called outside of a coroutine context
rusty::Option<rusty::Rc<Coroutine>> Coroutine::CurrentCoroutine() {
  if (Reactor::sp_running_coro_th_.is_none()) {
    return rusty::None;
  }
  return rusty::Some(Reactor::sp_running_coro_th_.unwrap_ref().clone());
}

// @safe - Creates and runs a new coroutine with rusty::Rc ownership
// SAFETY: Reactor manages coroutine lifecycle properly with Rc
rusty::Rc<Coroutine>
Coroutine::CreateRun(std::move_only_function<void()> func) {
  auto reactor_rc = Reactor::GetReactor();
  // Rc gives const access, CreateRunCoroutine is const (safe: thread-local, single owner)
  auto coro = reactor_rc->CreateRunCoroutine(std::move(func));
  // some events might be triggered in the last coroutine.
  return coro;
}

// @safe - Returns thread-local reactor instance, creates if needed
// SAFETY: Thread-local storage with Rc ensures single-threaded access
rusty::Rc<Reactor>
Reactor::GetReactor() {
  if (sp_reactor_th_.is_none()) {
    Log_debug("create a coroutine scheduler");
    sp_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());  // In-place construction
    // Use as_ref() to borrow, then initialize thread_id_ - safe because we just created it
    const_cast<Reactor&>(*sp_reactor_th_.unwrap_ref()).thread_id_ = std::this_thread::get_id();
  }
  return sp_reactor_th_.unwrap_ref().clone();
}

/**
 * @param func
 * @return
 */
// @safe - Creates and runs coroutine with rusty::Rc single-threaded reference counting
// SAFETY: Proper lifecycle management with Rc, single-threaded execution
rusty::Rc<Coroutine>
Reactor::CreateRunCoroutine(std::move_only_function<void()> func) const {
  rusty::Option<rusty::Rc<Coroutine>> sp_coro;
  if (REUSING_CORO && available_coros_.size() > 0) {
    //Log_info("Reusing stuff");
    sp_coro = rusty::Some(available_coros_.back().clone());
    available_coros_.pop_back();
    // Rc provides const access, use const_cast to modify (safe: single-threaded)
    auto& coro = const_cast<Coroutine&>(*sp_coro.unwrap_ref());
    coro.func_ = std::move(func);
    // Reset boost_coro_task_ when reusing a recycled coroutine for a new function
    coro.boost_coro_task_ = rusty::None;
    coro.status_ = Coroutine::INIT;
  } else {
    sp_coro = rusty::Some(rusty::Rc<Coroutine>::make(std::move(func)));
  }

  // Save old coroutine context - clone to avoid moving
  auto sp_old_coro = sp_running_coro_th_.is_some()
    ? rusty::Some(sp_running_coro_th_.unwrap_ref().clone())
    : rusty::Option<rusty::Rc<Coroutine>>{};
  sp_running_coro_th_ = rusty::Some(sp_coro.unwrap_ref().clone());

  if (sp_coro.is_none()) {
    Log_error("[DEBUG] CreateRunCoroutine: sp_coro is null!");
  }
  verify(sp_coro.is_some());
  auto pair = coros_.insert(sp_coro.unwrap_ref().clone());
  if (!pair.second) {
    Log_error("[DEBUG] CreateRunCoroutine: Failed to insert coroutine into coros_ set!");
    Log_error("[DEBUG] coros_ size before insert: %zu", coros_.size());
    Log_error("[DEBUG] REUSING_CORO: %d", REUSING_CORO);
  }
  verify(pair.second);
  verify(coros_.size() > 0);

  sp_coro.unwrap_ref()->Run();
  if (sp_coro.unwrap_ref()->Finished()) {
    coros_.erase(sp_coro.unwrap_ref().clone());
  }

  Loop();

  // yielded or finished, reset to old coro.
  sp_running_coro_th_ = sp_old_coro;
  return sp_coro.unwrap_ref().clone();
}

// @safe - Checks timeout events and moves ready ones to ready list with std::shared_ptr
void Reactor::CheckTimeout(std::vector<std::shared_ptr<Event>>& ready_events ) const {
  auto time_now = Time::now(true);
  for (auto it = timeout_events_.begin(); it != timeout_events_.end();) {
    Event& event = **it;
    auto status = event.status_;
    switch (status) {
      case Event::INIT:
        verify(0);
      case Event::WAIT: {
        const auto &wakeup_time = event.wakeup_time_;
        verify(wakeup_time > 0);
        if (time_now > wakeup_time) {
          if (event.IsReady()) {
            // This is because our event mechanism is not perfect, some events
            // don't get triggered with arbitrary condition change.
            event.status_ = Event::READY;
          } else {
            event.status_ = Event::TIMEOUT;
          }
          ready_events.push_back(*it);
          it = timeout_events_.erase(it);
        } else {
          it++;
        }
        break;
      }
      case Event::READY:
      case Event::DONE:
        it = timeout_events_.erase(it);
        break;
      default:
        verify(0);
    }
  }

}

//  be careful this could be called from different coroutines.
// @unsafe - Main event loop with complex event processing
// SAFETY: Thread-safe via thread_id verification
void Reactor::Loop(bool infinite) const {
  verify(std::this_thread::get_id() == thread_id_);
  looping_ = infinite;
  do {
    // Keep processing events until no new ready events are found
    // This fixes the event chain propagation issue
    bool found_ready_events = true;
    while (found_ready_events) {
      found_ready_events = false;
      std::vector<std::shared_ptr<Event>> ready_events;

      // Check waiting events
      auto& events = waiting_events_;
      for (auto it = events.begin(); it != events.end();) {
        Event& event = **it;
        event.Test();
        if (event.status_ == Event::READY) {
          ready_events.push_back(*it);
          it = events.erase(it);
          found_ready_events = true;
        } else if (event.status_ == Event::DONE) {
          it = events.erase(it);
        } else {
          it ++;
        }
      }

      CheckTimeout(ready_events);

      // Process ready events
      for (auto& sp_ev: ready_events) {
        Event& event = *sp_ev;
        auto option_coro = event.wp_coro_.upgrade();
        verify(option_coro.is_some());
        auto sp_coro = option_coro.unwrap();
        verify(coros_.find(sp_coro) != coros_.end());
        if (event.status_ == Event::READY) {
          event.status_ = Event::DONE;
        } else {
          verify(event.status_ == Event::TIMEOUT);
        }
        ContinueCoro(sp_coro);
      }

      // If we're not in infinite mode and found no events, stop inner loop
      if (!infinite && !found_ready_events) {
        break;
      }
    }
  } while (looping_);
}

// @safe - Continues execution of paused coroutine with rusty::Rc
// SAFETY: Manages coroutine state transitions properly, single-threaded Rc
void Reactor::ContinueCoro(rusty::Rc<Coroutine> sp_coro) const {
//  verify(!sp_running_coro_th_.is_none()); // disallow nested coros
  // Clone to avoid moving - must preserve the old value
  auto sp_old_coro = sp_running_coro_th_.is_some()
    ? rusty::Some(sp_running_coro_th_.unwrap_ref().clone())
    : rusty::Option<rusty::Rc<Coroutine>>{};
  sp_running_coro_th_ = rusty::Some(sp_coro.clone());
  verify(!sp_running_coro_th_.unwrap_ref()->Finished());
  if (sp_coro->status_ == Coroutine::INIT) {
    sp_coro->Run();
  } else {
    // PAUSED or RECYCLED
    sp_running_coro_th_.unwrap_ref()->Continue();
  }
  if (sp_running_coro_th_.unwrap_ref()->Finished()) {
    if (REUSING_CORO) {
      // Rc provides const access, use const_cast to modify (safe: single-threaded)
      const_cast<Coroutine&>(*sp_coro).status_ = Coroutine::RECYCLED;
      available_coros_.push_back(sp_running_coro_th_.unwrap_ref().clone());
    }
    coros_.erase(sp_running_coro_th_.unwrap_ref().clone());
  }
  sp_running_coro_th_ = sp_old_coro;
}

// TODO PollThreadWorker -> Reactor

// Private constructor - doesn't start thread
PollThreadWorker::PollThreadWorker()
    : poll_(Epoll()),
      l_(rusty::make_box<SpinLock>()),
      fd_to_pollable_(),
      mode_(),
      set_sp_jobs_(),
      pending_remove_(),
      pending_remove_l_(rusty::make_box<SpinLock>()),
      lock_job_(rusty::make_box<SpinLock>()),
      join_handle_(rusty::None),
      stop_flag_(rusty::make_box<std::atomic<bool>>(false)) {
  // Don't start thread here - factory will do it
}

// Factory method creates Arc<PollThreadWorker> and starts thread
rusty::Arc<PollThreadWorker> PollThreadWorker::create() {
  // Create Arc with PollThreadWorker
  auto arc = rusty::Arc<PollThreadWorker>::make();

  // Clone Arc for thread
  auto thread_arc = arc.clone();

  // Spawn thread with explicit parameter passing (enforces Send trait checking)
  // This properly validates that Arc<PollThreadWorker> is Send
  auto handle = rusty::thread::spawn(
    [](rusty::Arc<PollThreadWorker> arc) {
      arc->poll_loop();
    },
    thread_arc
  );

  // Store handle (using const method with mutex)
  {
    auto guard = arc->join_handle_.lock();
    *guard = rusty::Some(std::move(handle));
  }

  return arc;
}

// Explicit shutdown method
void PollThreadWorker::shutdown() const {
  // Signal thread to stop
  (*stop_flag_).store(true);

  // Join thread - poll_loop will handle cleanup of all registered pollables
  {
    auto guard = join_handle_.lock();
    if (guard->is_some()) {
      guard->take().unwrap().join();
    }
  }
}

// Destructor ensures clean shutdown
PollThreadWorker::~PollThreadWorker() {
  shutdown();
}

// @unsafe - Triggers ready jobs in coroutines
// SAFETY: Uses spinlock for thread safety
void PollThreadWorker::TriggerJob() const {
  (*lock_job_).lock();
  auto jobs_exec = set_sp_jobs_;
  set_sp_jobs_.clear();
  (*lock_job_).unlock();

  // Process shared_ptr<Job> jobs
  auto it = jobs_exec.begin();
  while (it != jobs_exec.end()) {
    auto sp_job = *it;
    // shared_ptr provides const access, but Job methods need mutable access
    // Safe: we're managing the job lifecycle and calling virtual methods
    Job* job_ptr = sp_job.get();
    if (job_ptr->Ready()) {
      // IMPORTANT: Capture sp_job by value to keep the shared_ptr alive!
      // If the coroutine yields, we need to keep the Job alive until it resumes.
      // Previously we captured only job_ptr (raw pointer), which caused use-after-free
      // when the shared_ptr was erased from jobs_exec below and the Job was destroyed.
      Coroutine::CreateRun([sp_job]() {
        Job* job_ptr = sp_job.get();
        job_ptr->Work();
      });
      it = jobs_exec.erase(it);
    }
    else {
      it++;
    }
  }
}

// @unsafe - Main polling loop with complex synchronization
// SAFETY: Uses spinlocks and proper synchronization primitives
void PollThreadWorker::poll_loop() const {
  while (!(*stop_flag_).load()) {
    TriggerJob();
    // Wait() now directly casts userdata to Pollable* and calls handlers
    // Safe because deferred removal guarantees object stays in fd_to_pollable_ map
    poll_.Wait();
    TriggerJob();

    // Process deferred removals AFTER all events handled
    (*pending_remove_l_).lock();
    std::unordered_set<int> remove_fds = std::move(pending_remove_);
    pending_remove_.clear();
    (*pending_remove_l_).unlock();

    for (int fd : remove_fds) {
      (*l_).lock();

      auto it = fd_to_pollable_.find(fd);
      if (it == fd_to_pollable_.end()) {
        (*l_).unlock();
        continue;
      }

      auto sp_poll = it->second;

      // Check if fd was NOT reused (still in mode_ map)
      if (mode_.find(fd) != mode_.end()) {
        poll_.Remove(sp_poll);
      }

      // Remove from map - object may be destroyed here
      fd_to_pollable_.erase(it);
      mode_.erase(fd);

      (*l_).unlock();
    }
    TriggerJob();
    Reactor::GetReactor()->Loop();
  }

  // When shutting down, add ALL remaining registered pollables to pending_remove_
  // This ensures proper cleanup even if remove() was never explicitly called
  (*l_).lock();
  for (auto& [fd, _] : fd_to_pollable_) {
    pending_remove_.insert(fd);
  }
  (*l_).unlock();

  // Process all pending removals (both explicit and from shutdown)
  for (int fd : pending_remove_) {
    (*l_).lock();

    auto it = fd_to_pollable_.find(fd);
    if (it == fd_to_pollable_.end()) {
      (*l_).unlock();
      continue;
    }

    auto sp_poll = it->second;

    // Check if fd was NOT reused (still in mode_ map)
    if (mode_.find(fd) != mode_.end()) {
      poll_.Remove(sp_poll);
    }

    // Remove from map - object may be destroyed here
    fd_to_pollable_.erase(it);
    mode_.erase(fd);

    (*l_).unlock();
  }
  pending_remove_.clear();
}

// @safe - Thread-safe job addition with polymorphic Arc
void PollThreadWorker::add(std::shared_ptr<Job> sp_job) const {
  (*lock_job_).lock();
  set_sp_jobs_.insert(sp_job);
  (*lock_job_).unlock();
}

// @safe - Thread-safe job removal with polymorphic shared_ptr
void PollThreadWorker::remove(std::shared_ptr<Job> sp_job) const {
  (*lock_job_).lock();
  set_sp_jobs_.erase(sp_job);
  (*lock_job_).unlock();
}

// @safe - Adds pollable with polymorphic Arc ownership
// SAFETY: Stores Arc in map, passes raw pointer to epoll for fast lookup
void PollThreadWorker::add(rusty::Arc<Pollable> sp_poll) const{
  int fd = sp_poll->fd();
  int poll_mode = sp_poll->poll_mode();

  (*l_).lock();

  // Check if already exists
  if (fd_to_pollable_.find(fd) != fd_to_pollable_.end()) {
    (*l_).unlock();
    return;
  }

  // Store in map
  fd_to_pollable_.insert_or_assign(fd, sp_poll.clone());
  mode_[fd] = poll_mode;

  // userdata = raw Pollable* for lookup (safe - kept alive by fd_to_pollable_ map)
  // Arc::get() returns const pointer, but epoll needs void* userdata
  // Safe: userdata is only used as an opaque identifier, not for mutation
  void* userdata = const_cast<void*>(static_cast<const void*>(sp_poll.get()));

  poll_.Add(sp_poll, userdata);

  (*l_).unlock();
}

// @unsafe - Removes pollable with deferred cleanup
// SAFETY: Deferred removal ensures safe cleanup
void PollThreadWorker::remove(Pollable& poll) const {
  int fd = poll.fd();

  (*l_).lock();
  bool found = (fd_to_pollable_.find(fd) != fd_to_pollable_.end());
  (*l_).unlock();

  if (!found) {
    return;  // Not found
  }

  // Add to pending_remove (actual removal happens after epoll_wait)
  (*pending_remove_l_).lock();
  pending_remove_.insert(fd);
  (*pending_remove_l_).unlock();
}

// @unsafe - Updates poll mode
// SAFETY: Protected by spinlock, validates poll existence
void PollThreadWorker::update_mode(Pollable& poll, int new_mode) const {
  int fd = poll.fd();
  (*l_).lock();

  // Verify the pollable is registered
  if (fd_to_pollable_.find(fd) == fd_to_pollable_.end()) {
    (*l_).unlock();
    return;
  }

  auto mode_it = mode_.find(fd);
  verify(mode_it != mode_.end());
  int old_mode = mode_it->second;
  mode_[fd] = new_mode;

  if (new_mode != old_mode) {
    void* userdata = &poll;  // Use address of reference
    poll_.Update(poll, userdata, new_mode, old_mode);
  }

  (*l_).unlock();
}

} // namespace rrr
