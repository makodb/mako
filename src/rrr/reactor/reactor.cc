
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>  // For SYS_gettid
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <mutex>
#include <utility>
#include <cstdlib>
#include <atomic>
#include "../base/all.hpp"
#include "reactor.h"
#include "coroutine.h"
#include "event.h"
#include "quorum_event.h"
#include "epoll_wrapper.h"
#include "sys/times.h"
#include <std_annotation.hpp>

// @external: {
//   rrr::Log::debug: [safe],
//   rrr::Log::error: [safe],
//   rrr::Event::Test: [unsafe]
// }

// #define DEBUG_WAIT

namespace rrr {

const int64_t n_max_coroutine = 2000;

thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_reactor_th_{};
thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_disk_reactor_th_{};
thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> Reactor::sp_running_coro_th_{};
thread_local std::unordered_map<std::string, std::vector<rusty::Arc<rrr::Pollable>>> Reactor::clients_{};

// Thread-local storage for PollThreadWorker (raw pointer for direct access)
// Safe because worker outlives all coroutines on its thread
thread_local PollThreadWorker* PollThreadWorker::current_worker_ = nullptr;
thread_local std::unordered_set<std::string> Reactor::dangling_ips_{};
SpinLock Reactor::trying_job_;

// @safe - Returns current fiber with single-threaded reference counting
// SAFETY: Returns copy of thread-local Rc - single-threaded, no synchronization needed
// Returns None if called outside of a fiber context
rusty::Option<rusty::Rc<Fiber>> Fiber::current_fiber() {
  // @unsafe - RefCell::borrow, Rc::clone
  {
    auto guard = Reactor::sp_running_coro_th_.borrow();
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
  // Rc gives const access, create_run_coroutine is const (safe: thread-local, single owner)
  auto coro = reactor_rc->create_run_coroutine(std::move(func), file, line);
  // some events might be triggered in the last fiber.
  return coro;
}

void Fiber::sleep(uint64_t microseconds) {
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
 * - Reactor's thread_id_ matches std::this_thread::get_id()
 */
rusty::Rc<Reactor>
Reactor::get_reactor() {
  if (sp_reactor_th_.is_none()) {
    Log_debug("create a coroutine scheduler");
    if (!REUSING_CORO)
      Log_warn("reusing coroutine not enabled!");
    sp_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());
    (*sp_reactor_th_.as_ref().unwrap()).thread_id_.set(std::this_thread::get_id());
  }
  return sp_reactor_th_.as_ref().unwrap().clone();
}

rusty::Rc<Reactor>
Reactor::get_disk_reactor() {
  if (sp_disk_reactor_th_.is_none()) {
    Log_debug("create a disk coroutine scheduler");
    sp_disk_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());
    (*sp_disk_reactor_th_.as_ref().unwrap()).thread_id_.set(std::this_thread::get_id());
  }
  return sp_disk_reactor_th_.as_ref().unwrap().clone();
}

// =============================================================================
// Helper functions for CreateRunCoroutine
// =============================================================================

// @safe - Gets a recycled coroutine or creates a new one
rusty::Rc<Fiber>
Reactor::get_or_create_coroutine(rusty::Function<void()> func, const char* file, int64_t line) const {
  // @unsafe
  {
    auto available_guard = available_coros_.borrow_mut();
    if (REUSING_CORO && available_guard->size() > 0) {
      n_idle_coroutines_.set(n_idle_coroutines_.get() - 1);
      auto coro = available_guard->back().clone();
      available_guard->pop_back();
      // Use Cell/RefCell for interior mutability (safe: single-threaded)
      const auto& coro_ref = *coro;
      const_cast<Fiber&>(coro_ref).id = Fiber::global_id++;  // id is not Cell yet
      *coro_ref.func_.borrow_mut() = std::move(func);
      *coro_ref.boost_coro_task_.borrow_mut() = rusty::None;
      coro_ref.boost_coro_yield_.set(nullptr);
      coro_ref.status_.set(Fiber::INIT);
      return coro;
    } else {
      auto coro = rusty::Rc<Fiber>::make(std::move(func));
      n_created_coroutines_.set(n_created_coroutines_.get() + 1);
      if (n_created_coroutines_.get() % 1024 == 0) {
        Log_debug("created %d, busy %d, idle %d coroutines on server %d, recent %s:%lld",
                 (int)n_created_coroutines_.get(),
                 (int)n_busy_coroutines_.get(),
                 (int)n_idle_coroutines_.get(),
                 server_id_.get(),
                 file,
                 (long long)line);
      }
      return coro;
    }
  }
}

// @safe - Saves current running coroutine to allow nesting
rusty::Option<rusty::Rc<Fiber>>
Reactor::save_running_coroutine() const {
  // @unsafe
  {
    auto guard = sp_running_coro_th_.borrow();
    if ((*guard).is_some()) {
      return rusty::Some((*guard).as_ref().unwrap().clone());
    }
    return rusty::Option<rusty::Rc<Fiber>>{};
  }
}

// @safe - Restores previously saved running coroutine
void Reactor::restore_running_coroutine(rusty::Option<rusty::Rc<Fiber>> old_coro) const {
  // @unsafe
  {
    *sp_running_coro_th_.borrow_mut() = std::move(old_coro);
  }
}

// @safe - Sets the current running coroutine
void Reactor::set_running_coroutine(const rusty::Rc<Fiber>& coro) const {
  // @unsafe
  {
    *sp_running_coro_th_.borrow_mut() = rusty::Some(coro.clone());
  }
}

// @safe - Registers coroutine in the active set
void Reactor::register_coroutine(const rusty::Rc<Fiber>& coro) const {
  // BTreeSet::insert returns bool (true if newly inserted)
  auto coros_guard = coros_.borrow_mut();
  bool inserted = coros_guard->insert(coro.clone());
  if (!inserted) {
    Log_error("[DEBUG] RegisterCoroutine: Failed to insert coroutine into coros_ set!");
    Log_error("[DEBUG] coros_ len: %zu, REUSING_CORO: %d", coros_guard->len(), REUSING_CORO);
  }
  // @unsafe
  { verify(inserted); }
  // @unsafe
  { verify(coros_guard->len() > 0); }
}

// =============================================================================
// Main CreateRunCoroutine - orchestrates the helper functions
// =============================================================================

/**
 * @param func
 * @return
 */
// @safe - Creates and runs coroutine using safe helper functions
rusty::Rc<Fiber>
Reactor::create_run_coroutine(rusty::Function<void()> func, const char* file, int64_t line) const {
  // Step 1: Get or create a coroutine
  auto coro = get_or_create_coroutine(std::move(func), file, line);

  // @unsafe
  {
    n_busy_coroutines_.set(n_busy_coroutines_.get() + 1);
  }

  // Step 2: Save current running coroutine context (for nesting)
  auto old_coro = save_running_coroutine();

  // Step 3: Set this as the running coroutine
  set_running_coroutine(coro);

  // Step 4: Register in the active coroutines set
  register_coroutine(coro);

  // Step 5: Run the coroutine
  // @unsafe
  {
    coro->run();
    if (coro->finished()) {
      coros_.borrow_mut()->remove(coro);
    }
  }

  // Step 6: Process events
  // @unsafe
  {
    loop(false, true);
  }

  // Step 7: Restore previous running coroutine
  restore_running_coroutine(std::move(old_coro));

  return coro;
}

// @safe - Uses RefCell for safe interior mutability
void Reactor::check_timeout(rusty::VecDeque<std::shared_ptr<Event>>& ready_events) const {
  int64_t time_now = 0;  // Initialize to 0
  // @unsafe - Time::now is external
  { time_now = Time::now(true); }

  // Borrow timeout_events_ for all operations
  auto guard = timeout_events_.borrow_mut();

  // First pass: update status of timed-out events
  for (size_t i = 0; i < guard->len(); ++i) {
    auto& sp = (*guard)[i];
    Event& event = *sp;
    auto status = event.status_.get();
    if (status == Event::WAIT) {
      const auto& wakeup_time = event.wakeup_time_;
      // @unsafe - verify is external
      { verify(wakeup_time > 0); }
      if (time_now > wakeup_time) {
        if (event.is_ready()) {
          event.status_.set(Event::READY);
        } else {
          event.status_.set(Event::TIMEOUT);
        }
      }
    }
  }

  // Extract events that are READY or TIMEOUT (timed out)
  // @unsafe - rusty::Function constructor
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
  // @unsafe - rusty::Function constructor
  {
    guard->retain(
      rusty::Function<bool(const std::shared_ptr<Event>&)>(
        [](const std::shared_ptr<Event>& sp) {
          return sp->status_.get() != Event::DONE;
        }));
  }
}

// @unsafe - rusty-cpp false positive: found_ready_events IS initialized inside do-while loop
void Reactor::loop(bool infinite, bool do_check_timeout) const {
  verify(std::this_thread::get_id() == thread_id_.get());

  looping_.set(infinite);

  do {
    bool found_ready_events = true;
    while (found_ready_events) {
      found_ready_events = false;
      rusty::VecDeque<std::shared_ptr<Event>> ready_events;

      // Process waiting events using RefCell
      {
        auto waiting_guard = waiting_events_.borrow_mut();
        // Test waiting events
        for (size_t i = 0; i < waiting_guard->len(); ++i) {
          (*waiting_guard)[i]->test();
        }
        // Extract READY events
        // @unsafe - rusty::Function constructor
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
        // @unsafe - rusty::Function constructor
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
        // @unsafe - rusty::Function constructor
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
        // @unsafe - rusty::Function constructor
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
        check_timeout(ready_events);
        if (ready_events.len() > before) {
          found_ready_events = true;
        }
      }

      // Process ready events
      // @unsafe - Weak::upgrade, continue_coro with potential use-after-move patterns
      {
        for (size_t i = 0; i < ready_events.len(); ++i) {
          auto& ev = ready_events[i];
          if (ev->status_.get() == Event::DONE) {
            continue;
          }
          auto option_coro = ev->wp_coro_.upgrade();
          if (option_coro.is_none()) {
            continue;
          }
          auto coro = option_coro.unwrap();
          if (!coros_.borrow()->contains(coro)) {
            continue;
          }
          verify(coro->status_.get() == Fiber::PAUSED);
          if (ev->status_.get() == Event::READY) {
            ev->status_.set(Event::DONE);
          } else {
            verify(ev->status_.get() == Event::TIMEOUT);
          }
          continue_coro(coro);
        }
      }

      if (!infinite && !found_ready_events) {
        break;
      }
    }

  } while (looping_.get());
}

// @safe - Continues execution of a paused coroutine
void Reactor::continue_coro(rusty::Rc<Fiber> coro) const {
  // Save current running coroutine for nesting support
  rusty::Option<rusty::Rc<Fiber>> old_coro;
  {
    auto guard = sp_running_coro_th_.borrow();
    old_coro = (*guard).is_some()
      ? rusty::Some((*guard).as_ref().unwrap().clone())
      : rusty::Option<rusty::Rc<Fiber>>{};
  }

  *sp_running_coro_th_.borrow_mut() = rusty::Some(coro.clone());

  // @unsafe - Fiber::finished() is not marked @safe
  {
    auto guard = sp_running_coro_th_.borrow();
    verify(!(*guard).as_ref().unwrap()->finished());
  }

  n_active_coroutines_.set(n_active_coroutines_.get() + 1);

  if (coro->status_.get() == Fiber::INIT) {
    coro->run();
  } else {
    // Don't hold borrow during continue_() as coroutine may call create_run()
    // This fixes RefCell double-borrow crash during server restart
    coro->continue_();
  }

  // @unsafe - Fiber::finished() is not marked @safe
  {
    auto guard = sp_running_coro_th_.borrow();
    if ((*guard).as_ref().unwrap()->finished()) {
      auto coro_ref = (*guard).as_ref().unwrap().clone();
      recycle(coro_ref);
    }
  }

  *sp_running_coro_th_.borrow_mut() = std::move(old_coro);
}

// @unsafe - Uses RefCell interior mutability (rusty-cpp doesn't fully support RefCell semantics)
void Reactor::recycle(rusty::Rc<Fiber>& coro) const {
  // This fixes the bug that coroutines are not recycling if they don't finish immediately.
  if (REUSING_CORO) {
    // Use Cell/RefCell for interior mutability (safe: single-threaded)
    const auto& coro_ref = *coro;
    coro_ref.status_.set(Fiber::RECYCLED);
    *coro_ref.func_.borrow_mut() = {};
    n_idle_coroutines_.set(n_idle_coroutines_.get() + 1);
    available_coros_.borrow_mut()->push_back(coro.clone());  // @unsafe
  }
  n_busy_coroutines_.set(n_busy_coroutines_.get() - 1);
  // @unsafe - rusty-cpp false positive: Rc::clone() doesn't move, coro is still valid
  { coros_.borrow_mut()->remove(coro); }
}

void Reactor::display_waiting_ev() const {
  Log_info("waiting_events_: %zu, composite_events_: %zu",
           waiting_events_.borrow()->len(), composite_events_.borrow()->len());
}

// =============================================================================
// PollThreadWorker Implementation
// =============================================================================

PollThreadWorker::PollThreadWorker(rusty::sync::mpsc::Receiver<PollCommand> receiver)
    : receiver_(std::move(receiver)),
      poll_(),
      fd_to_pollable_(),
      fd_to_legacy_pollable_(),
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
    // Pass callback to handle mode updates from handle_write() return values
    poll_.Wait([this](Pollable* poll, int new_mode) {
      do_update_mode(poll->fd(), new_mode);
    });

    // Process commands from channel (non-blocking try_recv)
    process_commands();

    trigger_job();

    // Process deferred removals
    process_pending_removals();

    trigger_job();
    Reactor::get_reactor()->loop();

    // Check for pending write updates (set by end_reply() during coroutine execution)
    // @unsafe - const_cast needed because Arc provides const access, but we know the
    // underlying Pollable uses interior mutability (mutable pending_write_update_ flag)
    for (auto& [fd, poll] : fd_to_pollable_) {
      if (poll->check_pending_write_update()) {
        do_update_mode(fd, PollMode::READ | PollMode::WRITE);
      }
    }

    // Check for pollables closed by handle_error() and remove them
    // This prevents fd reuse issues when old connection is closed but not removed
    std::vector<int> closed_fds;
    for (auto& [fd, poll] : fd_to_pollable_) {
      if (poll->is_closed()) {
        closed_fds.push_back(fd);
      }
    }
    for (int fd : closed_fds) {
      auto proxy_it = fd_to_pollable_.find(fd);
      if (proxy_it != fd_to_pollable_.end()) {
        auto legacy_it = fd_to_legacy_pollable_.find(fd);
        // Remove from epoll if still registered
        if (mode_.find(fd) != mode_.end() && legacy_it != fd_to_legacy_pollable_.end()) {
          poll_.Remove(legacy_it->second);
        }

        // Invoke close callback before erasing map entry so cleanup hooks run.
        proxy_it->second->close();

        fd_to_pollable_.erase(proxy_it);
        fd_to_legacy_pollable_.erase(fd);
        mode_.erase(fd);
      }
    }
  }

  Log_debug("[poll_loop] Exited while loop (stop_=true), starting cleanup");
  // Shutdown cleanup - remove all registered pollables
  for (auto& [fd, poll] : fd_to_pollable_) {
    auto legacy_it = fd_to_legacy_pollable_.find(fd);
    if (mode_.find(fd) != mode_.end() && legacy_it != fd_to_legacy_pollable_.end()) {
      poll_.Remove(legacy_it->second);
    }
  }
  fd_to_pollable_.clear();
  fd_to_legacy_pollable_.clear();
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
        do_add_pollable(std::move(arg.pollable), std::move(arg.legacy_arc));
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

// @unsafe - uses std::set operations
void PollThreadWorker::trigger_job() {
  // Copy jobs to process (in case jobs modify the set)
  std::set<rusty::Arc<Job>> jobs_exec = jobs_;
  jobs_.clear();

  for (const auto& job : jobs_exec) {
    Job* job_ptr = const_cast<Job*>(job.get());
    if (job_ptr->Ready()) {
      // Capture job by value to keep the Arc alive
      Fiber::create_run([job]() {
        Job* job_ptr = const_cast<Job*>(job.get());
        job_ptr->Work();
      });
      // Don't re-add ready jobs that were executed
    } else {
      // Re-add jobs that aren't ready yet - they should be checked again later
      jobs_.insert(job);
    }
  }
}

// @unsafe - Uses raw pointer cast for epoll userdata
void PollThreadWorker::do_add_pollable(PollableProxy poll, rusty::Arc<Pollable> legacy_arc) {
  int fd = poll->fd();
  int poll_mode = poll->poll_mode();

  // Check if already exists
  if (fd_to_pollable_.find(fd) != fd_to_pollable_.end()) {
    return;
  }

  // Store in maps
  fd_to_pollable_.insert_or_assign(fd, std::move(poll));
  fd_to_legacy_pollable_.insert_or_assign(fd, legacy_arc.clone());
  mode_[fd] = poll_mode;

  // userdata = raw Pollable* for lookup (temporary bridge until Leaf 3)
  void* userdata = const_cast<void*>(static_cast<const void*>(legacy_arc.get()));
  poll_.Add(legacy_arc, userdata);
}

// @unsafe - uses STL operations
void PollThreadWorker::do_remove_pollable(int fd) {
  if (fd_to_pollable_.find(fd) == fd_to_pollable_.end()) {
    return;
  }
  // Add to pending_remove (actual removal happens after epoll_wait)
  pending_remove_.insert(fd);
}

// @unsafe - Closes socket and drops Arc (thread-safe close from poll thread)
// SAFETY: Called only from poll thread, owns the Pollable via Arc
void PollThreadWorker::do_close_pollable(int fd) {
  // Remove from pending_remove if present
  pending_remove_.erase(fd);

  auto proxy_it = fd_to_pollable_.find(fd);
  if (proxy_it == fd_to_pollable_.end()) {
    return;
  }
  auto legacy_it = fd_to_legacy_pollable_.find(fd);

  // Remove from epoll if still registered
  if (mode_.find(fd) != mode_.end() && legacy_it != fd_to_legacy_pollable_.end()) {
    poll_.Remove(legacy_it->second);
  }

  // Close the socket via Pollable's close() method
  proxy_it->second->close();

  // Erase from maps, dropping storage references
  fd_to_pollable_.erase(proxy_it);
  fd_to_legacy_pollable_.erase(fd);
  mode_.erase(fd);
}

void PollThreadWorker::do_update_mode(int fd, int new_mode) {
  if (fd_to_pollable_.find(fd) == fd_to_pollable_.end()) {
    return;
  }

  auto mode_it = mode_.find(fd);
  if (mode_it == mode_.end()) {
    return;
  }

  int old_mode = mode_it->second;
  mode_[fd] = new_mode;

  if (new_mode != old_mode) {
    auto legacy_it = fd_to_legacy_pollable_.find(fd);
    if (legacy_it == fd_to_legacy_pollable_.end()) {
      return;
    }
    auto& legacy_poll = legacy_it->second;
    void* userdata = const_cast<void*>(static_cast<const void*>(legacy_poll.get()));
    poll_.Update(*legacy_poll, userdata, new_mode, old_mode);
  }
}

// @unsafe - uses std::set::insert
void PollThreadWorker::do_add_job(rusty::Arc<Job> job) {
  jobs_.insert(job);
}

// @unsafe - uses std::set::erase
void PollThreadWorker::do_remove_job(rusty::Arc<Job> job) {
  jobs_.erase(job);
}

// @unsafe - uses std::unordered_set::swap
void PollThreadWorker::process_pending_removals() {
  std::unordered_set<int> remove_fds;
  remove_fds.swap(pending_remove_);
  // pending_remove_ is now empty after swap

  for (int fd : remove_fds) {
    auto proxy_it = fd_to_pollable_.find(fd);
    if (proxy_it == fd_to_pollable_.end()) {
      continue;
    }
    auto legacy_it = fd_to_legacy_pollable_.find(fd);

    // Check if fd was NOT reused (still in mode map)
    if (mode_.find(fd) != mode_.end() && legacy_it != fd_to_legacy_pollable_.end()) {
      poll_.Remove(legacy_it->second);
    }

    fd_to_pollable_.erase(proxy_it);
    fd_to_legacy_pollable_.erase(fd);
    mode_.erase(fd);
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

rusty::Arc<PollThread> PollThread::create() {
  // Create MPSC channel
  auto [sender, receiver] = rusty::sync::mpsc::channel<PollCommand>();

  // Create PollThread with sender
  auto arc = rusty::Arc<PollThread>::make(std::move(sender));

  // Pointer to atomic thread ID for safe cross-thread access
  std::atomic<std::thread::id>* thread_id_ptr = &arc->poll_thread_id_;

  // Spawn thread - worker owns the receiver
  auto handle = rusty::thread::spawn(
    [thread_id_ptr](rusty::sync::mpsc::Receiver<PollCommand> rx) {
      auto tid = std::this_thread::get_id();
      thread_id_ptr->store(tid, std::memory_order_release);
      // Create worker wrapped in Rc<RefCell<>>
      auto worker = PollThreadWorker::create(std::move(rx));
      // Store raw pointer in TLS for direct access from same thread
      // The borrow_mut guard keeps RefCell borrowed during poll_loop()
      // Using raw pointer avoids RefCell re-borrow issues in coroutines
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
  auto current_tid = std::this_thread::get_id();
  auto poll_tid = poll_thread_id_.load(std::memory_order_acquire);
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

void PollThread::add(rusty::Arc<Pollable> poll) const {
  auto poll_proxy = make_pollable_proxy_from_arc(poll.clone());
  sender_.send(CmdAddPollable{std::move(poll_proxy), std::move(poll)});
}

void PollThread::remove(Pollable& poll) const {
  sender_.send(CmdRemovePollable{poll.fd()});
}

void PollThread::request_close(int fd) const {
  sender_.send(CmdClosePollable{fd});
}

// @safe - Sends update mode command via channel
// SAFETY: Channel send is thread-safe
void PollThread::update_mode(const Pollable& poll, int new_mode) const {
  // @unsafe - channel send and pointer operations
  {
    auto result = sender_.send(CmdUpdateMode{poll.fd(), new_mode});
    if (result.is_err()) {
      Log_error("PollThread::update_mode: send failed! Channel disconnected?");
    }
  }
}

void PollThread::add(rusty::Arc<Job> job) const {
  sender_.send(CmdAddJob{std::move(job)});
}

void PollThread::remove(rusty::Arc<Job> job) const {
  sender_.send(CmdRemoveJob{std::move(job)});
}

} // namespace rrr
