
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
//   rrr::Log::debug: [unsafe],
//   rrr::Log::error: [unsafe],
//   rrr::Event::Test: [unsafe]
// }

// #define DEBUG_WAIT

namespace rrr {

const int64_t n_max_coroutine = 2000;

thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_reactor_th_{};
thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_disk_reactor_th_{};
thread_local rusty::Option<rusty::Rc<Coroutine>> Reactor::sp_running_coro_th_{};
thread_local std::unordered_map<std::string, std::vector<rusty::Arc<rrr::Pollable>>> Reactor::clients_{};

// Thread-local storage for PollThreadWorker (raw pointer for direct access)
// Safe because worker outlives all coroutines on its thread
thread_local PollThreadWorker* PollThreadWorker::current_worker_ = nullptr;
thread_local std::unordered_set<std::string> Reactor::dangling_ips_{};
SpinLock Reactor::disk_job_;
SpinLock Reactor::trying_job_;

// @safe - Returns current coroutine with single-threaded reference counting
// SAFETY: Returns copy of thread-local Rc - single-threaded, no synchronization needed
// Returns None if called outside of a coroutine context
rusty::Option<rusty::Rc<Coroutine>> Coroutine::CurrentCoroutine() {
  if (Reactor::sp_running_coro_th_.is_none()) {
    return rusty::None;
  }
  return rusty::Some(Reactor::sp_running_coro_th_.as_ref().unwrap().clone());
}

// @unsafe - Creates and runs a new coroutine with rusty::Rc ownership
rusty::Rc<Coroutine>
Coroutine::CreateRunImpl(rusty::Function<void()> func, const char* file, int64_t line) {
  auto reactor_rc = Reactor::GetReactor();
  // Rc gives const access, CreateRunCoroutine is const (safe: thread-local, single owner)
  auto coro = reactor_rc->CreateRunCoroutine(std::move(func), file, line);
  // some events might be triggered in the last coroutine.
  return coro;
}

void Coroutine::Sleep(uint64_t microseconds) {
  auto x = Reactor::CreateSpEvent<TimeoutEvent>(microseconds);
  x->Wait();
}

// @safe - Returns thread-local reactor instance, creates if needed
// SAFETY: Thread-local storage with Rc ensures single-threaded access
rusty::Rc<Reactor>
Reactor::GetReactor() {
  if (sp_reactor_th_.is_none()) {
    Log_debug("create a coroutine scheduler");
    if (!REUSING_CORO)
      Log_warn("reusing coroutine not enabled!");
    sp_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());  // In-place construction
    // @unsafe - SAFETY: const_cast to initialize thread_id_ immediately after creation
    { const_cast<Reactor&>(*sp_reactor_th_.as_ref().unwrap()).thread_id_ = std::this_thread::get_id(); }
  }
  return sp_reactor_th_.as_ref().unwrap().clone();
}

rusty::Rc<Reactor>
Reactor::GetDiskReactor() {
  if (sp_disk_reactor_th_.is_none()) {
    Log_debug("create a disk coroutine scheduler");
    sp_disk_reactor_th_ = rusty::Some(rusty::Rc<Reactor>::make());
    const_cast<Reactor&>(*sp_disk_reactor_th_.as_ref().unwrap()).thread_id_ = std::this_thread::get_id();
  }
  return sp_disk_reactor_th_.as_ref().unwrap().clone();
}

/**
 * @param func
 * @return
 */
// @unsafe - Creates and runs coroutine, dereferences raw pointers internally
rusty::Rc<Coroutine>
Reactor::CreateRunCoroutine(rusty::Function<void()> func, const char* file, int64_t line) const {
  rusty::Option<rusty::Rc<Coroutine>> sp_coro;
  if (REUSING_CORO && available_coros_.size() > 0) {
    n_idle_coroutines_--;
    sp_coro = rusty::Some(available_coros_.back().clone());
    available_coros_.pop_back();
    // Rc provides const access, use const_cast to modify (safe: single-threaded)
    auto& coro = const_cast<Coroutine&>(*sp_coro.as_ref().unwrap());
    coro.id = Coroutine::global_id++;
    coro.func_ = std::move(func);
    // Reset boost_coro_task_ when reusing a recycled coroutine for a new function
    coro.boost_coro_task_ = rusty::None;
    coro.status_ = Coroutine::INIT;
  } else {
    sp_coro = rusty::Some(rusty::Rc<Coroutine>::make(std::move(func)));
    n_created_coroutines_++;
    if (n_created_coroutines_ % 1024 == 0) {
      // Jetpack: Include server_id_ for debugging in distributed environment
      Log_info("created %d, busy %d, idle %d coroutines on server %d, recent %s:%lld",
               (int)n_created_coroutines_,
               (int)n_busy_coroutines_,
               (int)n_idle_coroutines_,
               server_id_,
               file,
               (long long)line);
    }
  }

  n_busy_coroutines_++;

  // Save old coroutine context - clone to avoid moving
  auto sp_old_coro = sp_running_coro_th_.is_some()
    ? rusty::Some(sp_running_coro_th_.as_ref().unwrap().clone())
    : rusty::Option<rusty::Rc<Coroutine>>{};
  sp_running_coro_th_ = rusty::Some(sp_coro.as_ref().unwrap().clone());

  if (sp_coro.is_none()) {
    Log_error("[DEBUG] CreateRunCoroutine: sp_coro is null!");
  }
  verify(sp_coro.is_some());
  auto pair = coros_.insert(sp_coro.as_ref().unwrap().clone());
  if (!pair.second) {
    Log_error("[DEBUG] CreateRunCoroutine: Failed to insert coroutine into coros_ set!");
    Log_error("[DEBUG] coros_ size before insert: %zu", coros_.size());
    Log_error("[DEBUG] REUSING_CORO: %d", REUSING_CORO);
  }
  verify(pair.second);
  verify(coros_.size() > 0);

  sp_coro.as_ref().unwrap()->Run();
  if (sp_coro.as_ref().unwrap()->Finished()) {
    coros_.erase(sp_coro.as_ref().unwrap().clone());
  }

  Loop(false, true);  // Process events AND check timeouts

  // yielded or finished, reset to old coro.
  sp_running_coro_th_ = sp_old_coro;
  return sp_coro.as_ref().unwrap().clone();
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
          // Event will be removed from waiting_events_ when reactor loop scans it
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
// NOTE: Cannot mark @safe because const method modifies mutable fields (looping_, waiting_events_).
// In RustyCpp, const = &self which doesn't allow mutation. Use @unsafe for interior mutability.
// SAFETY: Thread-safe via thread_id verification
// Merged implementation supporting both Paxos (mako) and Raft (jetpack) paths
void Reactor::Loop(bool infinite, bool check_timeout) const {
  verify(std::this_thread::get_id() == thread_id_);
  looping_ = infinite;

  do {
    // Process disk events (jetpack path)
    disk_job_.lock();
    bool has_disk_events = !ready_disk_events_.empty();
    if (has_disk_events) {
      auto disk_event = ready_disk_events_.front();
      ready_disk_events_.pop_front();
      disk_job_.unlock();

      auto option_coro = disk_event->wp_coro_.upgrade();
      if (option_coro.is_some()) {
        auto sp_coro = option_coro.unwrap();
        disk_event->status_ = Event::READY;
        if (disk_event->status_ == Event::READY) {
          disk_event->status_ = Event::DONE;
        }
        ContinueCoro(sp_coro);
      }
    } else {
      disk_job_.unlock();
    }

    // Keep processing events until no new ready events are found
    bool found_ready_events = true;
    while (found_ready_events) {
      found_ready_events = false;
      std::vector<std::shared_ptr<Event>> ready_events;

      // Get thread-safe ready events (jetpack/raft path)
      {
        std::lock_guard<std::mutex> lock(ready_events_mutex_);
        if (!ready_events_.empty()) {
          ready_events = std::move(ready_events_);
          ready_events_.clear();
          found_ready_events = true;
        }
      }

      // Check waiting events (mako-dev path)
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
          ++it;
        }
      }

      // Scan ONLY composite events (AndEvent, OrEvent, QuorumEvent)
      // Raft has zero composite events → this loop does nothing → zero overhead!
      // Paxos has a few QuorumEvents → small list → minimal overhead
      auto& composite_events = composite_events_;
      for (auto it = composite_events.begin(); it != composite_events.end();) {
        Event& event = **it;
        event.Test();
        if (event.status_ == Event::READY) {
          ready_events.push_back(std::move(*it));
          it = composite_events.erase(it);
          found_ready_events = true;
        } else if (event.status_ == Event::DONE) {
          it = composite_events.erase(it);
        } else {
          ++it;
        }
      }

      // Check timeouts if requested
      if (check_timeout) {
        size_t before = ready_events.size();
        CheckTimeout(ready_events);
        if (ready_events.size() > before) {
          found_ready_events = true;
        }
      }

      // Process all ready events
      for (auto& sp_event : ready_events) {
        // Event might already be DONE (processed by another thread in multi-threaded Raft)
        if (sp_event->status_ == Event::DONE) {
          continue;
        }
        Event& event = *sp_event;
        auto option_coro = event.wp_coro_.upgrade();
        if (option_coro.is_none()) {
          continue;
        }
        auto sp_coro = option_coro.unwrap();
        // Check if coroutine still exists (might have finished already)
        if (coros_.find(sp_coro) == coros_.end()) {
          continue;
        }
        verify(sp_coro->status_ == Coroutine::PAUSED);
        if (sp_event->status_ == Event::READY) {
          sp_event->status_ = Event::DONE;
        } else {
          verify(sp_event->status_ == Event::TIMEOUT);
        }
        ContinueCoro(sp_coro);
      }

      // If not in infinite mode and no events found, stop inner loop
      if (!infinite && !found_ready_events) {
        break;
      }
    }

  } while (looping_);
}

// @unsafe - Continues execution of paused coroutine, dereferences raw pointers internally
void Reactor::ContinueCoro(rusty::Rc<Coroutine> sp_coro) const {
//  verify(!sp_running_coro_th_.is_none()); // disallow nested coros
  // Clone to avoid moving - must preserve the old value
  auto sp_old_coro = sp_running_coro_th_.is_some()
    ? rusty::Some(sp_running_coro_th_.as_ref().unwrap().clone())
    : rusty::Option<rusty::Rc<Coroutine>>{};
  sp_running_coro_th_ = rusty::Some(sp_coro.clone());
  verify(!sp_running_coro_th_.as_ref().unwrap()->Finished());
  n_active_coroutines_++;

  if (sp_coro->status_ == Coroutine::INIT) {
    sp_coro->Run();
  } else {
    // PAUSED or RECYCLED
    sp_running_coro_th_.as_ref().unwrap()->Continue();
  }
  if (sp_running_coro_th_.as_ref().unwrap()->Finished()) {
    auto sp_coro_ref = sp_running_coro_th_.as_ref().unwrap().clone();
    Recycle(sp_coro_ref);
  }
  sp_running_coro_th_ = sp_old_coro;
}

void Reactor::Recycle(rusty::Rc<Coroutine>& sp_coro) const {
  // This fixes the bug that coroutines are not recycling if they don't finish immediately.
  if (REUSING_CORO) {
    // Rc provides const access, use const_cast to modify (safe: single-threaded)
    const_cast<Coroutine&>(*sp_coro).status_ = Coroutine::RECYCLED;
    const_cast<Coroutine&>(*sp_coro).func_ = {};
    n_idle_coroutines_++;
    available_coros_.push_back(sp_coro.clone());
  }
  n_busy_coroutines_--;
  coros_.erase(sp_coro);
}

void Reactor::DisplayWaitingEv() const {
  Log_info("waiting_events_: %zu, composite_events_: %zu, ready_events_: %zu",
           waiting_events_.size(), composite_events_.size(), ready_events_.size());
}

void Reactor::ReadyEventsThreadSafePushBack(std::shared_ptr<Event> ev) const {
  std::lock_guard<std::mutex> lock(ready_events_mutex_);
  ready_events_.push_back(ev);
}

void Reactor::DiskLoop() const {
  Reactor::GetReactor()->disk_job_.lock();
  auto disk_events = Reactor::GetReactor()->disk_events_;
  auto it = Reactor::GetReactor()->disk_events_.begin();
  std::vector<std::shared_ptr<DiskEvent>> pending_disk_events_{};
  while(it != Reactor::GetReactor()->disk_events_.end()){
    auto disk_event = std::static_pointer_cast<DiskEvent>(*it);
    it = Reactor::GetReactor()->disk_events_.erase(it);
    pending_disk_events_.push_back(disk_event);
  }
  Reactor::GetReactor()->disk_job_.unlock();

  int total_written = 0;
  std::unordered_set<std::string> sync_set{};
  for (size_t i = 0; i < pending_disk_events_.size(); i++) {
    total_written += pending_disk_events_[i]->Handle();
    if (pending_disk_events_[i]->sync) {
      auto it = sync_set.find(pending_disk_events_[i]->file);
      if (it == sync_set.end()) {
        sync_set.insert(pending_disk_events_[i]->file);
      }
    }
  }

  for (auto it = sync_set.begin(); it != sync_set.end(); it++) {
    int fd = ::open(it->c_str(), O_WRONLY | O_APPEND | O_CREAT, 0777);
    ::fsync(fd);
    ::close(fd);
  }

  for(size_t i = 0; i < pending_disk_events_.size(); i++){
    Reactor::GetReactor()->disk_job_.lock();
    Reactor::GetReactor()->ready_disk_events_.push_back(pending_disk_events_[i]);
    Reactor::GetReactor()->disk_job_.unlock();
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
    TriggerJob();

    // Wait for events (epoll_wait with short timeout)
    // Pass callback to handle mode updates from handle_write() return values
    poll_.Wait([this](Pollable* poll, int new_mode) {
      do_update_mode(poll->fd(), new_mode, poll);
    });

    // Process commands from channel (non-blocking try_recv)
    process_commands();

    TriggerJob();

    // Process deferred removals
    process_pending_removals();

    TriggerJob();
    Reactor::GetReactor()->Loop();

    // Check for pending write updates (set by end_reply() during coroutine execution)
    // @unsafe - const_cast needed because Arc provides const access, but we know the
    // underlying Pollable uses interior mutability (mutable pending_write_update_ flag)
    for (auto& [fd, sp_poll] : fd_to_pollable_) {
      if (sp_poll->check_pending_write_update()) {
        do_update_mode(fd, Pollable::READ | Pollable::WRITE, const_cast<Pollable*>(sp_poll.get()));
      }
    }
  }

  Log_debug("[poll_loop] Exited while loop (stop_=true), starting cleanup");
  // Shutdown cleanup - remove all registered pollables
  for (auto& [fd, sp_poll] : fd_to_pollable_) {
    if (mode_.find(fd) != mode_.end()) {
      poll_.Remove(sp_poll);
    }
  }
  fd_to_pollable_.clear();
  mode_.clear();
  pending_remove_.clear();
  Log_debug("[poll_loop] Cleanup complete, poll_loop exiting");
}

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
      } else if constexpr (std::is_same_v<T, CmdUpdateMode>) {
        do_update_mode(arg.fd, arg.new_mode, arg.poll_ptr);
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

void PollThreadWorker::TriggerJob() {
  // Copy jobs to process (in case jobs modify the set)
  std::set<rusty::Arc<Job>> jobs_exec = jobs_;
  jobs_.clear();

  for (const auto& sp_job : jobs_exec) {
    Job* job_ptr = const_cast<Job*>(sp_job.get());
    if (job_ptr->Ready()) {
      // Capture sp_job by value to keep the Arc alive
      Coroutine::CreateRun([sp_job]() {
        Job* job_ptr = const_cast<Job*>(sp_job.get());
        job_ptr->Work();
      });
      // Don't re-add ready jobs that were executed
    } else {
      // Re-add jobs that aren't ready yet - they should be checked again later
      jobs_.insert(sp_job);
    }
  }
}

// @unsafe - Uses raw pointer cast for epoll userdata
void PollThreadWorker::do_add_pollable(rusty::Arc<Pollable> sp_poll) {
  int fd = sp_poll->fd();
  int poll_mode = sp_poll->poll_mode();

  // Check if already exists
  if (fd_to_pollable_.find(fd) != fd_to_pollable_.end()) {
    return;
  }

  // Store in maps
  fd_to_pollable_.insert_or_assign(fd, sp_poll.clone());
  mode_[fd] = poll_mode;

  // userdata = raw Pollable* for lookup
  void* userdata = const_cast<void*>(static_cast<const void*>(sp_poll.get()));
  poll_.Add(sp_poll, userdata);
}

void PollThreadWorker::do_remove_pollable(int fd) {
  if (fd_to_pollable_.find(fd) == fd_to_pollable_.end()) {
    return;
  }
  // Add to pending_remove (actual removal happens after epoll_wait)
  pending_remove_.insert(fd);
}

// @unsafe - Uses raw pointer dereference for poll_ptr
// SAFETY: poll_ptr is guaranteed to be valid by caller (from fd_to_pollable_ map)
void PollThreadWorker::do_update_mode(int fd, int new_mode, Pollable* poll_ptr) {
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
    // @unsafe {
    void* userdata = poll_ptr;
    poll_.Update(*poll_ptr, userdata, new_mode, old_mode);
    // }
  }
}

void PollThreadWorker::do_add_job(rusty::Arc<Job> sp_job) {
  jobs_.insert(sp_job);
}

void PollThreadWorker::do_remove_job(rusty::Arc<Job> sp_job) {
  jobs_.erase(sp_job);
}

void PollThreadWorker::process_pending_removals() {
  std::unordered_set<int> remove_fds;
  remove_fds.swap(pending_remove_);
  // pending_remove_ is now empty after swap

  for (int fd : remove_fds) {
    auto it = fd_to_pollable_.find(fd);
    if (it == fd_to_pollable_.end()) {
      continue;
    }

    auto sp_poll = it->second;

    // Check if fd was NOT reused (still in mode map)
    if (mode_.find(fd) != mode_.end()) {
      poll_.Remove(sp_poll);
    }

    fd_to_pollable_.erase(it);
    mode_.erase(fd);
  }
}


// Update poll mode directly (bypasses channel)
// Only safe to call from the poll thread (e.g., from ServerConnection::end_reply)
void PollThreadWorker::update_mode(int fd, int new_mode, Pollable* poll_ptr) {
  do_update_mode(fd, new_mode, poll_ptr);
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
    auto guard = arc->join_handle_.lock();
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
    auto guard = join_handle_.lock();
    Log_debug("[PollThread::shutdown] join_handle lock acquired");
    if (guard->is_some()) {
      Log_debug("[PollThread::shutdown] Calling thread.join()...");
      guard->take().unwrap().join();
      Log_debug("[PollThread::shutdown] thread.join() completed!");
    } else {
      Log_debug("[PollThread::shutdown] join_handle is None, thread already joined");
    }
  }
  Log_debug("[PollThread::shutdown] Released join_handle lock");
  Log_debug("[PollThread::shutdown] Complete");
}

void PollThread::add(rusty::Arc<Pollable> poll) const {
  sender_.send(CmdAddPollable{std::move(poll)});
}

void PollThread::remove(Pollable& poll) const {
  sender_.send(CmdRemovePollable{poll.fd()});
}

void PollThread::update_mode(Pollable& poll, int new_mode) const {
  auto result = sender_.send(CmdUpdateMode{poll.fd(), new_mode, &poll});
  if (result.is_err()) {
    Log_error("PollThread::update_mode: send failed! Channel disconnected?");
  }
}

void PollThread::add(rusty::Arc<Job> sp_job) const {
  sender_.send(CmdAddJob{std::move(sp_job)});
}

void PollThread::remove(rusty::Arc<Job> sp_job) const {
  sender_.send(CmdRemoveJob{std::move(sp_job)});
}

} // namespace rrr
