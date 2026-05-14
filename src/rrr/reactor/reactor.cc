
// import std; replacement — see <std_compat.hpp> for rationale.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>


// @c-compat-added

#include <rusty/rusty.hpp>
#include <rusty/thread.hpp>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>
#include <rusty/sync/mpsc.hpp>
#include <rusty/async.hpp>
#include <rusty/vecdeque.hpp>
#include <rusty/btreeset.hpp>


#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>  // For SYS_gettid
#include <sys/times.h>
#include <std_annotation.hpp>




#include "reactor.h"


#include "../rrr.hpp"

import std;

// @external: {
//   rrr::Log::debug: [safe],
//   rrr::Log::error: [safe],
//   rrr::Event::Test: [unsafe]
// }

// #define DEBUG_WAIT

namespace rrr {

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

thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_reactor_th_{};
thread_local rusty::Option<rusty::Rc<Reactor>> Reactor::sp_disk_reactor_th_{};
thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> Reactor::sp_running_fiber_th_{};
thread_local rusty::HashMap<std::string, rusty::Vec<PollableProxy>> Reactor::clients_{};

// Thread-local storage for PollThreadWorker (raw pointer for direct access)
// Safe because worker outlives all fibers on its thread
thread_local PollThreadWorker* PollThreadWorker::current_worker_ = nullptr;
thread_local rusty::HashSet<std::string> Reactor::dangling_ips_{};
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
  int64_t time_now = 0;  // Initialize to 0
  // @unsafe - Time::now is external
  { time_now = Time::now(true); }

  // @unsafe { RefCell::borrow_mut is not borrow-checked }
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

  // @unsafe { RefCell::borrow_mut, Option operator= are not borrow-checked }
  { *sp_running_fiber_th_.borrow_mut() = rusty::Some(fiber.clone()); }

  // @unsafe - Fiber::finished() is not marked @safe
  {
    auto guard = sp_running_fiber_th_.borrow();
    verify(!(*guard).as_ref().unwrap()->finished());
  }

  n_active_fibers_.set(n_active_fibers_.get() + 1);

  if (fiber->status_.get() == Fiber::INIT) {
    fiber->run();
  } else {
    // Don't hold borrow during continue_() as fiber may call create_run()
    // This fixes RefCell double-borrow crash during server restart
    fiber->continue_();
  }

  // @unsafe - Fiber::finished() is not marked @safe
  {
    auto guard = sp_running_fiber_th_.borrow();
    if ((*guard).as_ref().unwrap()->finished()) {
      auto fiber_ref = (*guard).as_ref().unwrap().clone();
      recycle(fiber_ref);
    }
  }

  // @unsafe { RefCell::borrow_mut, Option operator= are not borrow-checked }
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
      auto& poll = *poll_opt.unwrap();

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
        (*proxy_opt.unwrap())->close();

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

// @unsafe - uses rusty::BTreeSet operations
void PollThreadWorker::trigger_job() {
  // Copy jobs to process (in case jobs modify the set)
  rusty::BTreeSet<rusty::Arc<Job>> jobs_exec = jobs_.clone();
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

// @unsafe - uses STL operations
void PollThreadWorker::do_remove_pollable(int fd) {
  if (!fd_to_pollable_.contains_key(fd)) {
    return;
  }
  // Add to pending_remove (actual removal happens after epoll_wait)
  pending_remove_.insert(fd);
}

// @unsafe - Closes socket and drops Arc (thread-safe close from poll thread)
// SAFETY: Called only from poll thread, owns the Pollable via Arc
void PollThreadWorker::do_close_pollable(int fd) {
  // Remove from pending_remove if present
  pending_remove_.remove(fd);

  auto proxy_opt = fd_to_pollable_.get(fd);
  if (proxy_opt.is_none()) {
    return;
  }

  // Remove from epoll if still registered
  if (mode_.contains_key(fd)) {
    poll_.Remove(fd);
  }

  // Close the socket via Pollable's close() method
  (*proxy_opt.unwrap())->close();

  // Erase from maps, dropping storage references
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

  int old_mode = *mode_opt.unwrap();
  mode_.insert(fd, new_mode);

  if (new_mode != old_mode) {
    poll_.Update(fd, new_mode, old_mode);
  }
}

// @unsafe - uses rusty::BTreeSet::insert
void PollThreadWorker::do_add_job(rusty::Arc<Job> job) {
  jobs_.insert(job);
}

// @unsafe - uses rusty::BTreeSet::remove
void PollThreadWorker::do_remove_job(rusty::Arc<Job> job) {
  jobs_.remove(job);
}

// @unsafe - uses rusty::HashSet::clone (via clear/swap)
void PollThreadWorker::process_pending_removals() {
  rusty::HashSet<int> remove_fds = pending_remove_.clone();
  pending_remove_.clear();

  for (int fd : remove_fds) {
    if (!fd_to_pollable_.contains_key(fd)) {
      continue;
    }

    // Check if fd was NOT reused (still in mode map)
    if (mode_.contains_key(fd)) {
      poll_.Remove(fd);
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

} // namespace rrr
