#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <rusty/rusty.hpp>   // rusty::addr_of_temp for the by-reference DSL args
#include <rusty/array.hpp>
#include <rusty/winnow_stream.hpp>   // rusty::contains for BTreeSet, per the compiler diagnostic   // rusty::len over the authority's BTreeSets
#include <rusty/slice.hpp>
#include <rusty/mutex.hpp>
// rusty::clone, which the generated ReplicationWakeGate methods call to copy an
// Option<Arc<...>> out from under its mutex guard.
#include <rusty/move.hpp>
#include <rusty/sync/atomic.hpp>

#include "server.h"
#include "frame.h"
#include "../legacy_raft_log_payload.h"
#include "../tpc_command.h"
#include "rust_facade_types.h"
#include "memory_snapshot_manager.hpp"
#include "quorum.hpp"

import std;
import rusty;   // rusty::BTreeSet is a btree_port C++20 module, not a header

// @external: {
//   rrr::RandomGenerator::rand_double: [safe, (double, double) -> double]
//   rrr::RandomGenerator::rand: [safe, (int, int) -> int]
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   Log_warn: [safe, (...) -> void]
//   Log_error: [safe, (...) -> void]
//   Log_fatal: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   Time::now: [safe, () -> uint64_t]
//   strcmp: [safe, (const char*, const char*) -> int]
//   std::getenv: [safe, (const char*) -> const char*]
//   std::tolower: [safe, (int) -> int]
//   std::transform: [safe, (...) -> void]
//   std::stoull: [safe, (const std::string&) -> uint64_t]
//   std::stoll: [safe, (const std::string&) -> int64_t]
//   std::to_string: [safe, (...) -> owned std::string]
//   std::min: [safe, (...) -> T]
//   std::max: [safe, (...) -> T]
//   std::sort: [safe, (iterator, iterator) -> void]
//   std::copy: [safe, (...) -> void]
//   std::make_shared: [safe, (...) -> owned]
//   std::dynamic_pointer_cast: [safe, (...) -> owned]
//   std::static_pointer_cast: [safe, (...) -> owned]
//   std::lock_guard: [safe, (...) -> owned]
//   std::recursive_mutex::lock: [safe, (&'a mut) -> void]
//   std::recursive_mutex::unlock: [safe, (&'a mut) -> void]
//   std::atomic::store: [safe, (&'a mut, ...) -> void]
//   std::atomic::load: [safe, (&'a) -> T]
//   std::vector::push_back: [safe, (&'a mut, T) -> void]
//   std::vector::operator[]: [safe, (&'a, size_t) -> &'a]
//   std::vector::reserve: [safe, (&'a mut, size_t) -> void]
//   std::vector::size: [safe, (&'a) -> size_t]
//   std::vector::empty: [safe, (&'a) -> bool]
//   std::vector::begin: [safe, (&'a) -> iterator]
//   std::vector::end: [safe, (&'a) -> iterator]
//   std::map::find: [safe, (&'a, ...) -> iterator]
//   std::map::insert: [safe, (&'a mut, ...) -> pair]
//   std::map::end: [safe, (&'a) -> iterator]
//   std::map::erase: [safe, (&'a mut, ...) -> void]
//   std::map::size: [safe, (&'a) -> size_t]
//   std::shared_ptr::operator=: [safe, (&'a mut, &'a) -> &'a mut]
//   std::shared_ptr::get: [safe, (&'a) -> *]
//   operator bool: [safe, (&'a) -> bool]
//   rrr::Fiber::create_run: [safe, (...) -> owned]
//   rrr::Fiber::sleep: [safe, (int) -> void]
//   Reactor::create_sp_event: [safe, (...) -> owned]
//   Config::GetConfig: [safe, () -> *]
//   janus::TpcBatchCommand::AddCmds: [safe, (&'a mut, &'a mut) -> void]
//   std::this_thread::sleep_for: [safe, (...) -> void]
//   std::thread::joinable: [safe, (&'a) -> bool]
//   std::thread::join: [safe, (&'a mut) -> void]
//   std::thread::detach: [safe, (&'a mut) -> void]
//   rrr::IntEvent::set: [safe, (&'a mut, int) -> void]
//   rrr::IntEvent::wait: [safe, (&'a, int) -> void]
//   rrr::Event::wait: [safe, (&'a, int) -> void]
//   rrr::EventStatus::TIMEOUT: [safe, () -> int]
//   janus::View::View: [safe, (...) -> owned]
//   janus::View::operator=: [safe, (&'a mut, const &'a) -> &'a mut]
//   janus::TxLogServer::DestroyTx: [safe, (&'a mut, uint64_t) -> void]
//   janus::RaftCommo::SendAppendEntries2: [safe, (...) -> owned]
//   janus::RaftCommo::BroadcastVote: [safe, (...) -> owned]
// }

namespace janus {

// ReplicationWakeGate: the first src/deptran/raft conversion that is not a
// scalar predicate, and the first that proves `impl` at all. See
// docs/migration/raft/cpp-refactor-plan.md tranche 3.
//
// The two wait entry points are SPLIT rather than moved wholesale, for one
// reason: creating an `IntEvent` calls the reactor factory
// `::rrr::create_sp_int_event`, which the DSL cannot name. The rustc facade
// exposes it only as `rusty::rrr::reactor::create_sp_int_event`, and inline
// mode has no `--type-map` to rewrite that path, so spelling it would require
// a nested `rusty::rrr::reactor` namespace in C++ merely to hold a factory.
// Instead the two wait entry points take the event as a PARAMETER and
// RaftServer creates it -- `WaitForReplicationOrHeartbeat` and
// `WaitForElectionTimeoutOrShutdown` in this file, which are the C++ kernels
// CLAUDE.md describes: the DSL owns the shape, C++ owns the surgery.
//
// The fast path is preserved exactly. `begin_wait_for_work` returns
// Some(answer) when it could decide without a waiter and None when the caller
// must arm one, so no event is allocated on the path that today allocates
// none. That split is the only behavioural seam in the conversion; every
// other body below is a statement-for-statement transcription.
//
// TWO THINGS THE DSL GIVES UP HERE, recorded so neither reads as a decision:
//   * `final` and `private` have no DSL spelling, so the two Disarm* helpers
//     are public and the type is open. Both are still called only from this
//     file.
//   * a C++ constructor becomes `fn new` -> `ReplicationWakeGate::new_()`,
//     because the DSL has no default member initializers. The owning Arc is
//     therefore built with `Arc::make_with`, the entry point rusty-cpp
//     documents for a non-movable payload built by a factory (arc.hpp:170-184)
//     -- ReplicationWakeGate holds AtomicBools and so has no move constructor.
#if RUSTYCPP_RUST
pub struct ReplicationWakeGate {
    owner_: rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>>>,
    waiter_: rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>>>,
    election_waiter_: rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>>>,
    pending_: rusty::sync::atomic::AtomicBool,
    waiter_armed_: rusty::sync::atomic::AtomicBool,
    election_waiter_armed_: rusty::sync::atomic::AtomicBool,
    wake_job_queued_: rusty::sync::atomic::AtomicBool,
    shutdown_job_queued_: rusty::sync::atomic::AtomicBool,
    accepting_: rusty::sync::atomic::AtomicBool,
}

// A DECISION, not a deferral, so no TODO: clippy asks for `impl Default`
// alongside `fn new`, but a trait impl here would emit a second C++
// construction path into the generated struct that no C++ caller uses, and
// the owning `Arc::make_with` call in RaftServer's constructor names `new_()`
// explicitly. The Rust-only ergonomic is not worth the extra emitted surface.
#[allow(clippy::new_without_default)]
impl ReplicationWakeGate {
    pub fn new() -> ReplicationWakeGate {
        ReplicationWakeGate {
            owner_: rusty::Mutex::new(rusty::None),
            waiter_: rusty::Mutex::new(rusty::None),
            election_waiter_: rusty::Mutex::new(rusty::None),
            pending_: rusty::sync::atomic::AtomicBool::new(false),
            waiter_armed_: rusty::sync::atomic::AtomicBool::new(false),
            election_waiter_armed_: rusty::sync::atomic::AtomicBool::new(false),
            wake_job_queued_: rusty::sync::atomic::AtomicBool::new(false),
            shutdown_job_queued_: rusty::sync::atomic::AtomicBool::new(false),
            accepting_: rusty::sync::atomic::AtomicBool::new(true),
        }
    }

    pub fn bind_owner(&self, owner: rusty::sync::Arc<rusty::ReactorPollThread>) {
        let mut guard = self.owner_.lock().unwrap();
        *guard = rusty::Some(owner);
        self.accepting_.store(true, rusty::sync::atomic::Ordering::Release);
    }

    pub fn publish(&self) -> bool {
        self.pending_.store(true, rusty::sync::atomic::Ordering::Release);
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn close(&self) {
        self.accepting_.store(false, rusty::sync::atomic::Ordering::Release);
        self.pending_.store(true, rusty::sync::atomic::Ordering::Release);
    }

    pub fn clear_owner(&self) {
        let mut guard = self.owner_.lock().unwrap();
        *guard = rusty::None;
    }

    pub fn accepting(&self) -> bool {
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn reserve_wake_owner(
        &self,
    ) -> rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>> {
        if !self.waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire) {
            return rusty::None;
        }
        let guard = self.owner_.lock().unwrap();
        if self.wake_job_queued_.swap(true, rusty::sync::atomic::Ordering::AcqRel) {
            return rusty::None;
        }
        if (*guard).is_none() {
            self.wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
            return rusty::None;
        }
        (*guard).clone()
    }

    pub fn reserve_shutdown_wake_owner(
        &self,
    ) -> rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>> {
        if !self.waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire)
            && !self
                .election_waiter_armed_
                .load(rusty::sync::atomic::Ordering::Acquire)
        {
            return rusty::None;
        }
        let guard = self.owner_.lock().unwrap();
        if self
            .shutdown_job_queued_
            .swap(true, rusty::sync::atomic::Ordering::AcqRel)
        {
            return rusty::None;
        }
        if (*guard).is_none() {
            self.shutdown_job_queued_
                .store(false, rusty::sync::atomic::Ordering::Release);
            return rusty::None;
        }
        (*guard).clone()
    }

    // TODO(raft-dsl): drop this allow once the emitter lowers an `if let`
    // binding of an Option<Arc<T>> THROUGH the Arc. Clippy's suggested
    // `if let rusty::Some(event) = &waiter { event.set(1) }` is the better
    // Rust, and it transpiles, but the binding is emitted as `event.set(1)`
    // on a `rusty::Arc<rrr::IntEvent>` -- a dot, not an arrow -- which does
    // not compile. `as_ref().unwrap()` is emitted as `->set(1)`, which is
    // what the hand-written C++ this replaces already did, but ONLY when the
    // local carries an explicit type; an inferred `let` emits `const auto`
    // and the dot comes back. Hence the annotations below, which are
    // load-bearing rather than documentation. Verify by switching the two
    // bodies back to `if let` and rebuilding src/deptran/raft.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn wake_on_owner(&self) {
        if !self.waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire) {
            self.wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
            return;
        }
        // The guard is a temporary of this statement, so waiter_ is unlocked
        // again before the set() below: set() may make the heartbeat fiber
        // runnable, and that fiber takes waiter_ in DisarmWaiter.
        let waiter: rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>> =
            (*self.waiter_.lock().unwrap()).clone();
        if waiter.is_some() {
            waiter.as_ref().unwrap().set(1);
        }
    }

    // See the TODO on wake_on_owner for why this is not `if let`.
    #[allow(clippy::unnecessary_unwrap)]
    pub fn wake_shutdown_on_owner(&self) {
        // Both guards are statement temporaries; neither lock is held across
        // the set() calls below, for the reason given in wake_on_owner.
        let heartbeat_waiter: rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>> =
            (*self.waiter_.lock().unwrap()).clone();
        let election_waiter: rusty::Option<rusty::sync::Arc<rusty::ReactorIntEvent>> =
            (*self.election_waiter_.lock().unwrap()).clone();
        if heartbeat_waiter.is_some() {
            heartbeat_waiter.as_ref().unwrap().set(1);
        }
        if election_waiter.is_some() {
            election_waiter.as_ref().unwrap().set(1);
        }
    }

    pub fn begin_wait_for_work(&self) -> rusty::Option<bool> {
        if !self.accepting_.load(rusty::sync::atomic::Ordering::Acquire) {
            return rusty::Some(false);
        }
        if self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
            return rusty::Some(self.accepting_.load(rusty::sync::atomic::Ordering::Acquire));
        }
        rusty::None
    }

    pub fn finish_wait_for_work(
        &self,
        waiter: rusty::sync::Arc<rusty::ReactorIntEvent>,
        timeout_us: u64,
    ) -> bool {
        waiter.set(0);
        {
            let mut guard = self.waiter_.lock().unwrap();
            *guard = rusty::Some(waiter.clone());
        }
        self.waiter_armed_.store(true, rusty::sync::atomic::Ordering::Release);
        if self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel) {
            self.disarm_waiter();
            return self.accepting_.load(rusty::sync::atomic::Ordering::Acquire);
        }
        waiter.wait_timeout(timeout_us);
        self.pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel);
        self.disarm_waiter();
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn wait_for_election_timeout(
        &self,
        waiter: rusty::sync::Arc<rusty::ReactorIntEvent>,
        timeout_us: u64,
    ) -> bool {
        waiter.set(0);
        {
            let mut guard = self.election_waiter_.lock().unwrap();
            *guard = rusty::Some(waiter.clone());
        }
        self.election_waiter_armed_
            .store(true, rusty::sync::atomic::Ordering::Release);
        if !self.accepting_.load(rusty::sync::atomic::Ordering::Acquire) {
            self.disarm_election_waiter();
            return false;
        }
        waiter.wait_timeout(timeout_us);
        self.disarm_election_waiter();
        self.accepting_.load(rusty::sync::atomic::Ordering::Acquire)
    }

    pub fn disarm_waiter(&self) {
        self.waiter_armed_.store(false, rusty::sync::atomic::Ordering::Release);
        {
            let mut guard = self.waiter_.lock().unwrap();
            *guard = rusty::None;
        }
        self.wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
    }

    pub fn disarm_election_waiter(&self) {
        self.election_waiter_armed_
            .store(false, rusty::sync::atomic::Ordering::Release);
        let mut guard = self.election_waiter_.lock().unwrap();
        *guard = rusty::None;
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.replication_wake_gate version=1 rust_sha256=0cfb2f750ce062bf97302a95b6754f707202bebb2a61d5b5189dd77cc8b1e959*/
struct ReplicationWakeGate;

struct ReplicationWakeGate {
    rusty::Mutex<rusty::Option<rusty::Arc<rusty::ReactorPollThread>>> owner_;
    rusty::Mutex<rusty::Option<rusty::Arc<rusty::ReactorIntEvent>>> waiter_;
    rusty::Mutex<rusty::Option<rusty::Arc<rusty::ReactorIntEvent>>> election_waiter_;
    rusty::sync::atomic::AtomicBool pending_;
    rusty::sync::atomic::AtomicBool waiter_armed_;
    rusty::sync::atomic::AtomicBool election_waiter_armed_;
    rusty::sync::atomic::AtomicBool wake_job_queued_;
    rusty::sync::atomic::AtomicBool shutdown_job_queued_;
    rusty::sync::atomic::AtomicBool accepting_;

    static ReplicationWakeGate new_();
    void bind_owner(rusty::Arc<rusty::ReactorPollThread> owner) const;
    bool publish() const;
    void close() const;
    void clear_owner() const;
    bool accepting() const;
    rusty::Option<rusty::Arc<rusty::ReactorPollThread>> reserve_wake_owner() const;
    rusty::Option<rusty::Arc<rusty::ReactorPollThread>> reserve_shutdown_wake_owner() const;
    void wake_on_owner() const;
    void wake_shutdown_on_owner() const;
    rusty::Option<bool> begin_wait_for_work() const;
    bool finish_wait_for_work(rusty::Arc<rusty::ReactorIntEvent> waiter, uint64_t timeout_us) const;
    bool wait_for_election_timeout(rusty::Arc<rusty::ReactorIntEvent> waiter, uint64_t timeout_us) const;
    void disarm_waiter() const;
    void disarm_election_waiter() const;
};


inline ReplicationWakeGate ReplicationWakeGate::new_() {
    return ReplicationWakeGate{.owner_ = rusty::Mutex<rusty::Option<rusty::Arc<rusty::ReactorPollThread>>>::new_(rusty::None), .waiter_ = rusty::Mutex<rusty::Option<rusty::Arc<rusty::ReactorIntEvent>>>::new_(rusty::None), .election_waiter_ = rusty::Mutex<rusty::Option<rusty::Arc<rusty::ReactorIntEvent>>>::new_(rusty::None), .pending_ = rusty::sync::atomic::AtomicBool::new_(false), .waiter_armed_ = rusty::sync::atomic::AtomicBool::new_(false), .election_waiter_armed_ = rusty::sync::atomic::AtomicBool::new_(false), .wake_job_queued_ = rusty::sync::atomic::AtomicBool::new_(false), .shutdown_job_queued_ = rusty::sync::atomic::AtomicBool::new_(false), .accepting_ = rusty::sync::atomic::AtomicBool::new_(true)};
}

inline void ReplicationWakeGate::bind_owner(rusty::Arc<rusty::ReactorPollThread> owner) const {
    auto guard = this->owner_.lock().unwrap();
    *guard = rusty::Option<rusty::Arc<rusty::ReactorPollThread>>(std::move(owner));
    this->accepting_.store(true, rusty::sync::atomic::Ordering::Release);
}

inline bool ReplicationWakeGate::publish() const {
    this->pending_.store(true, rusty::sync::atomic::Ordering::Release);
    return this->accepting_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline void ReplicationWakeGate::close() const {
    this->accepting_.store(false, rusty::sync::atomic::Ordering::Release);
    this->pending_.store(true, rusty::sync::atomic::Ordering::Release);
}

inline void ReplicationWakeGate::clear_owner() const {
    auto guard = this->owner_.lock().unwrap();
    *guard = rusty::None;
}

inline bool ReplicationWakeGate::accepting() const {
    return this->accepting_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline rusty::Option<rusty::Arc<rusty::ReactorPollThread>> ReplicationWakeGate::reserve_wake_owner() const {
    if (rusty::detail::rust_not(this->waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire))) {
        return rusty::None;
    }
    auto guard = this->owner_.lock().unwrap();
    if (this->wake_job_queued_.swap(true, rusty::sync::atomic::Ordering::AcqRel)) {
        return rusty::None;
    }
    if (((*guard)).is_none()) {
        this->wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
        return rusty::None;
    }
    return rusty::clone(((*guard)));
}

inline rusty::Option<rusty::Arc<rusty::ReactorPollThread>> ReplicationWakeGate::reserve_shutdown_wake_owner() const {
    if (rusty::detail::rust_not(this->waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire)) && rusty::detail::rust_not(this->election_waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire))) {
        return rusty::None;
    }
    auto guard = this->owner_.lock().unwrap();
    if (this->shutdown_job_queued_.swap(true, rusty::sync::atomic::Ordering::AcqRel)) {
        return rusty::None;
    }
    if (((*guard)).is_none()) {
        this->shutdown_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
        return rusty::None;
    }
    return rusty::clone(((*guard)));
}

inline void ReplicationWakeGate::wake_on_owner() const {
    if (rusty::detail::rust_not(this->waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire))) {
        this->wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
        return;
    }
    const rusty::Option<rusty::Arc<rusty::ReactorIntEvent>> waiter = rusty::clone(((*this->waiter_.lock().unwrap())));
    if (waiter.is_some()) {
        waiter.as_ref().unwrap()->set(1);
    }
}

inline void ReplicationWakeGate::wake_shutdown_on_owner() const {
    const rusty::Option<rusty::Arc<rusty::ReactorIntEvent>> heartbeat_waiter = rusty::clone(((*this->waiter_.lock().unwrap())));
    const rusty::Option<rusty::Arc<rusty::ReactorIntEvent>> election_waiter = rusty::clone(((*this->election_waiter_.lock().unwrap())));
    if (heartbeat_waiter.is_some()) {
        heartbeat_waiter.as_ref().unwrap()->set(1);
    }
    if (election_waiter.is_some()) {
        election_waiter.as_ref().unwrap()->set(1);
    }
}

inline rusty::Option<bool> ReplicationWakeGate::begin_wait_for_work() const {
    if (rusty::detail::rust_not(this->accepting_.load(rusty::sync::atomic::Ordering::Acquire))) {
        return rusty::Option<bool>(false);
    }
    if (this->pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel)) {
        return rusty::Option<bool>(this->accepting_.load(rusty::sync::atomic::Ordering::Acquire));
    }
    return rusty::None;
}

inline bool ReplicationWakeGate::finish_wait_for_work(rusty::Arc<rusty::ReactorIntEvent> waiter, uint64_t timeout_us) const {
    waiter->set(0);
    {
        auto guard = this->waiter_.lock().unwrap();
        *guard = rusty::Option<rusty::Arc<rusty::ReactorIntEvent>>(rusty::clone(waiter));
    }
    this->waiter_armed_.store(true, rusty::sync::atomic::Ordering::Release);
    if (this->pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel)) {
        this->disarm_waiter();
        return this->accepting_.load(rusty::sync::atomic::Ordering::Acquire);
    }
    waiter->wait_timeout(std::move(timeout_us));
    this->pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel);
    this->disarm_waiter();
    return this->accepting_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline bool ReplicationWakeGate::wait_for_election_timeout(rusty::Arc<rusty::ReactorIntEvent> waiter, uint64_t timeout_us) const {
    waiter->set(0);
    {
        auto guard = this->election_waiter_.lock().unwrap();
        *guard = rusty::Option<rusty::Arc<rusty::ReactorIntEvent>>(rusty::clone(waiter));
    }
    this->election_waiter_armed_.store(true, rusty::sync::atomic::Ordering::Release);
    if (rusty::detail::rust_not(this->accepting_.load(rusty::sync::atomic::Ordering::Acquire))) {
        this->disarm_election_waiter();
        return false;
    }
    waiter->wait_timeout(std::move(timeout_us));
    this->disarm_election_waiter();
    return this->accepting_.load(rusty::sync::atomic::Ordering::Acquire);
}

inline void ReplicationWakeGate::disarm_waiter() const {
    this->waiter_armed_.store(false, rusty::sync::atomic::Ordering::Release);
    {
        auto guard = this->waiter_.lock().unwrap();
        *guard = rusty::None;
    }
    this->wake_job_queued_.store(false, rusty::sync::atomic::Ordering::Release);
}

inline void ReplicationWakeGate::disarm_election_waiter() const {
    this->election_waiter_armed_.store(false, rusty::sync::atomic::Ordering::Release);
    auto guard = this->election_waiter_.lock().unwrap();
    *guard = rusty::None;
}
/*RUSTYCPP:GEN-END id=raft_server.replication_wake_gate*/

namespace {

// @unsafe - Thread-safe PollThread::add bridge.  The queued closure captures
// only the gate Arc, never a RaftServer pointer.
void QueueReplicationWake(
    const rusty::Arc<ReplicationWakeGate>& replication_wake_gate) {
  auto owner = replication_wake_gate->reserve_wake_owner();
  if (owner.is_none()) {
    return;
  }

  auto gate_for_job = replication_wake_gate.clone();
  auto wake_job = rusty::Arc<OneTimeJob>::new_(
      OneTimeJob::new_([gate_for_job]() {
        gate_for_job->wake_on_owner();
      }));
  owner.as_ref().unwrap()->add(rusty::Arc<Job>(wake_job));
}

// @unsafe - Thread-safe shutdown bridge.  The queued closure captures only
// the gate Arc, never the RaftServer whose loops it wakes.
void QueueReplicationShutdownWake(
    const rusty::Arc<ReplicationWakeGate>& replication_wake_gate) {
  auto owner = replication_wake_gate->reserve_shutdown_wake_owner();
  if (owner.is_none()) {
    return;
  }

  auto gate_for_job = replication_wake_gate.clone();
  auto wake_job = rusty::Arc<OneTimeJob>::new_(
      OneTimeJob::new_([gate_for_job]() {
        gate_for_job->wake_shutdown_on_owner();
      }));
  owner.as_ref().unwrap()->add(rusty::Arc<Job>(wake_job));
}

}  // namespace

namespace {

// RaftLab snapshots contain no external application state. This transaction
// preserves the same prepare/commit ordering as production while Commit is a
// one-shot no-op after strict marker validation.
class PreparedRaftLabSnapshotInstall final
    : public PreparedStateMachineSnapshotInstall {
 public:
  // @safe - Publishes no external state.
  bool Commit() override {
    if (committed_) {
      return false;
    }
    committed_ = true;
    return true;
  }

 private:
  bool committed_ = false;
};

uint64_t ParseEnvUint64OrDefault(const char* env_name, uint64_t default_value) {
  const char* env = std::getenv(env_name);
  if (env == nullptr || *env == '\0') {
    return default_value;
  }

  char* endptr = nullptr;
  unsigned long long parsed = std::strtoull(env, &endptr, 10);
  if (endptr != env && *endptr == '\0' && parsed > 0) {
    Log_info("[LEADER-ELECTION] Using {}={}", env_name, parsed);
    return static_cast<uint64_t>(parsed);
  }

  Log_warn("[LEADER-ELECTION] Invalid {}='{}'; using default {}",
           env_name, env, static_cast<unsigned long>(default_value));
  return default_value;
}

uint64_t GetPreferredLeaderGracePeriodUs() {
  constexpr uint64_t kDefaultGracePeriodUs = 5000000ULL;  // 5s
  static uint64_t grace_period_us =
      ParseEnvUint64OrDefault("MAKO_RAFT_PREFERRED_GRACE_US", kDefaultGracePeriodUs);
  return grace_period_us;
}

uint64_t GetNonPreferredGraceElectionMinUs() {
  constexpr uint64_t kDefaultMinUs = 1000000ULL;  // 1s
  static uint64_t min_us = ParseEnvUint64OrDefault(
      "MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US", kDefaultMinUs);
  return min_us;
}

uint64_t GetNonPreferredGraceElectionMaxUs() {
  constexpr uint64_t kDefaultMaxUs = 2000000ULL;  // 2s
  static uint64_t max_us = ParseEnvUint64OrDefault(
      "MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US", kDefaultMaxUs);
  return max_us;
}

uint64_t RandomInRangeUs(uint64_t min_us, uint64_t max_us) {
  if (max_us < min_us) {
    std::swap(min_us, max_us);
  }
  if (max_us == min_us) {
    return min_us;
  }
  uint64_t range = max_us - min_us;
  if (range > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    range = static_cast<uint64_t>(std::numeric_limits<int>::max());
  }
  return min_us + static_cast<uint64_t>(RandomGenerator::rand(0, static_cast<int>(range)));
}

constexpr uint64_t kPreferredElectionMinUs = 150000ULL;
constexpr uint64_t kPreferredElectionMaxUs = 300000ULL;
constexpr uint64_t kNonPreferredSteadyElectionMinUs = 500000ULL;
constexpr uint64_t kNonPreferredSteadyElectionMaxUs = 1000000ULL;

uint64_t GetPreferredElectionTimeoutUs() {
  return RandomInRangeUs(kPreferredElectionMinUs,
                         kPreferredElectionMaxUs);
}

uint64_t GetNonPreferredGraceElectionTimeoutUs() {
  return RandomInRangeUs(GetNonPreferredGraceElectionMinUs(),
                         GetNonPreferredGraceElectionMaxUs());
}

uint64_t GetNonPreferredSteadyElectionTimeoutUs() {
  return RandomInRangeUs(kNonPreferredSteadyElectionMinUs,
                         kNonPreferredSteadyElectionMaxUs);
}

uint64_t GetAppendEntriesBatchMaxEntries() {
  // Keep catch-up payload bounded to avoid oversized RPCs and timeout stalls
  // when a follower is far behind.
  constexpr uint64_t kDefaultMaxEntries = 256ULL;
  static uint64_t max_entries = ParseEnvUint64OrDefault(
      "MAKO_RAFT_APPEND_BATCH_MAX_ENTRIES", kDefaultMaxEntries);
  return max_entries;
}

#if RUSTYCPP_RUST
#[allow(dead_code, non_snake_case)]
fn IsPreferredLeaderConfigured(preferred_leader_site_id: u16) -> bool {
    preferred_leader_site_id != u16::MAX
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.preferred_leader_predicate version=1 rust_sha256=fb616b2ee515d24b66cd4df76d251f96158cbd9444d1a0a9d0678db3dcfb166d*/
bool IsPreferredLeaderConfigured(uint16_t preferred_leader_site_id);

bool IsPreferredLeaderConfigured(uint16_t preferred_leader_site_id) {
    return rusty::detail::deref_if_pointer_like(preferred_leader_site_id) != rusty::detail::deref_if_pointer_like(std::numeric_limits<uint16_t>::max());
}
/*RUSTYCPP:GEN-END id=raft_server.preferred_leader_predicate*/

static_assert(std::is_same_v<siteid_t, uint16_t>);
static_assert(static_cast<uint16_t>(INVALID_SITEID) ==
              std::numeric_limits<uint16_t>::max());

}  // namespace

// @unsafe - Caller holds the state-machine apply gate followed by mtx_. The
// production callback must validate and stage without changing live state.
// RaftLab has no application state, so it validates a strict index+term marker.
std::unique_ptr<PreparedStateMachineSnapshotInstall>
RaftServer::PrepareStateMachineSnapshotLocked(
    const std::string& data,
    uint64_t last_included_index,
    uint64_t last_included_term) {
  if (prepare_sm_snapshot_cb_) {
    try {
      auto prepared =
          prepare_sm_snapshot_cb_(data, last_included_index);
      if (prepared == nullptr) {
        Log_error("[RAFT-SNAPSHOT] Site {} state-machine prepare rejected "
                  "snapshot index={} term={}",
                  site_id_, last_included_index, last_included_term);
      }
      return prepared;
    } catch (const std::exception& error) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine prepare threw for "
                "snapshot index={} term={}: {}",
                site_id_, last_included_index, last_included_term,
                error.what());
      return nullptr;
    } catch (...) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine prepare threw for "
                "snapshot index={} term={}",
                site_id_, last_included_index, last_included_term);
      return nullptr;
    }
  }

#ifdef RAFT_TEST_CORO
  constexpr size_t kMarkerSize = sizeof(uint64_t) * 2;
  if (data.size() != kMarkerSize) {
    Log_error("[RAFT-SNAPSHOT] Site {} RaftLab marker has {} bytes, expected {}",
              site_id_, data.size(), kMarkerSize);
    return nullptr;
  }

  uint64_t marker_index = 0;
  uint64_t marker_term = 0;
  std::memcpy(&marker_index, data.data(), sizeof(marker_index));
  std::memcpy(&marker_term, data.data() + sizeof(marker_index),
              sizeof(marker_term));
  const bool matches = raft_server_snapshot_marker_matches(
      data.size(), kMarkerSize, marker_index, marker_term,
      last_included_index, last_included_term);
  if (!matches) {
    Log_error("[RAFT-SNAPSHOT] Site {} RaftLab marker mismatch: "
              "payload=({}, {}) metadata=({}, {})",
              site_id_, marker_index, marker_term,
              last_included_index, last_included_term);
  }
  if (!matches) {
    return nullptr;
  }
  return std::make_unique<PreparedRaftLabSnapshotInstall>();
#else
  Log_error("[RAFT-SNAPSHOT] Site {} has no state-machine snapshot prepare "
            "callback for "
            "index={} term={}",
            site_id_, last_included_index, last_included_term);
  return nullptr;
#endif
}

// @unsafe - Startup uses this only after SnapshotManager has verified and
// durably discovered the exact Raft snapshot bytes.
bool RaftServer::LoadStateMachineSnapshotLocked(
    const std::string& data,
    uint64_t last_included_index,
    uint64_t last_included_term) {
  auto prepared = PrepareStateMachineSnapshotLocked(
      data, last_included_index, last_included_term);
  if (prepared == nullptr) {
    return false;
  }
  try {
    return prepared->Commit();
  } catch (const std::exception& error) {
    Log_error("[RAFT-SNAPSHOT] Site {} state-machine commit threw for "
              "snapshot index={} term={}: {}",
              site_id_, last_included_index, last_included_term, error.what());
  } catch (...) {
    Log_error("[RAFT-SNAPSHOT] Site {} state-machine commit threw for "
              "snapshot index={} term={}",
              site_id_, last_included_index, last_included_term);
  }
  return false;
}

// @unsafe - Discovers, verifies, and restores SnapshotManager state before
// publishing the recovered boundary to application waiters.
bool RaftServer::InitializeSnapshotManager() {
  try {
  const char* snapshot_flag = std::getenv("MAKO_RAFT_SNAPSHOTS");  // @unsafe
  bool should_enable = (snapshot_flag &&
                       (strcmp(snapshot_flag, "1") == 0 ||
                        strcmp(snapshot_flag, "true") == 0));

  if (!should_enable) {
    std::lock_guard<std::mutex> lock(mtx_);
    const bool has_orphaned_compacted_suffix =
        state_.snapidx_ == 0 && !raft_log_.is_empty() &&
        raft_log_.base() > 1;
    const bool has_uncovered_empty_progress =
        state_.snapidx_ == 0 && raft_log_.is_empty() && state_.commit_index_ != 0;
    if (has_orphaned_compacted_suffix || has_uncovered_empty_progress) {
      Log_error("[RAFT-SNAPSHOT] Site {} has recovered progress without its "
                "covering snapshot (first={} commit={}); "
                "snapshots are disabled",
                site_id_,
                raft_log_.is_empty() ? 0 : raft_log_.base(),
                state_.commit_index_);
      rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
      stop_.store(true, rusty::sync::atomic::Ordering::Release);
      looping_.store(false, rusty::sync::atomic::Ordering::Release);
      apply_thread_running_.store(false);
      return false;
    }
    Log_info("[RAFT-SNAPSHOT] Snapshots disabled for site {} (set MAKO_RAFT_SNAPSHOTS=1 to enable)",
             site_id_);
    return true;
  }

  // Check for custom snapshot interval
  uint64_t snapshot_interval = GetSnapshotThreshold();
  const char* interval_str = std::getenv("MAKO_RAFT_SNAPSHOT_INTERVAL");  // @unsafe
  if (interval_str && interval_str[0] != '\0') {
    try {
      snapshot_interval = std::stoull(interval_str);
    } catch (const std::exception& error) {
      Log_error("[RAFT-SNAPSHOT] Invalid snapshot interval '{}': {}",
                interval_str, error.what());
      return false;
    }
    SetSnapshotThreshold(snapshot_interval);
  }

  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::mutex> lock(mtx_);

  // Memory-only Raft has no on-disk snapshot store. A manager injected through
  // SetSnapshotManager() before Setup keeps the latest snapshot it holds and
  // restores that boundary below; otherwise start from an empty in-memory
  // manager.
  std::shared_ptr<janus::raft::SnapshotManager> manager = snapshot_manager_;
  if (!manager) {
    manager = std::make_shared<janus::raft::MemorySnapshotManager>();
  }

  auto fail_recovery = [this](const char* reason) {
    Log_error("[RAFT-SNAPSHOT] Site {} recovery failed: {}", site_id_, reason);
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    apply_thread_running_.store(false);
    return false;
  };

  const auto latest = manager->GetLatestSnapshot();
  if (latest.is_none()) {
    const bool has_orphaned_compacted_suffix =
        state_.snapidx_ == 0 && !raft_log_.is_empty() &&
        raft_log_.base() > 1;
    const bool has_uncovered_empty_progress =
        state_.snapidx_ == 0 && raft_log_.is_empty() && state_.commit_index_ != 0;
    if (state_.snapidx_ != 0 || has_orphaned_compacted_suffix ||
        has_uncovered_empty_progress) {
      return fail_recovery(
          "empty snapshot manager cannot cover the compacted live log");
    }
    snapshot_manager_ = manager;
    snapshot_manager_configured_.store(
        true, rusty::sync::atomic::Ordering::Release);
    Log_info("[RAFT-SNAPSHOT] Initialized empty in-memory manager for site {} partition {}: interval={}",
             site_id_, partition_id_, snapshot_interval);
    return true;
  }

  const auto discovered = latest.unwrap();
  janus::raft::SnapshotMetadata metadata;
  std::string snapshot_data;
  if (!manager->LoadLatestSnapshot(&metadata, &snapshot_data)) {
    return fail_recovery("latest snapshot bytes failed to load");
  }
  if (metadata.last_included_index != discovered.last_included_index ||
      metadata.last_included_term != discovered.last_included_term) {
    return fail_recovery(
        "snapshot manager metadata does not match its loaded snapshot");
  }
  if (metadata.last_included_index == 0 ||
      !raft_server_log_index_has_successor(metadata.last_included_index)) {
    return fail_recovery("snapshot boundary is outside the recoverable log range");
  }
  if (metadata.last_included_index < state_.snapidx_ ||
      (metadata.last_included_index == state_.snapidx_ && state_.snapidx_ != 0 &&
       metadata.last_included_term != state_.snapterm_)) {
    return fail_recovery("snapshot manager would move the live boundary backward or change its term");
  }
  if (prepare_sm_snapshot_cb_ &&
      GetAppliedIndex() > metadata.last_included_index) {
    return fail_recovery(
        "refusing to rewind a live state machine to an older snapshot");
  }

  const uint64_t recovered_snapshot_index = metadata.last_included_index;
  const uint64_t recovered_snapshot_term = metadata.last_included_term;
  const uint64_t previous_snapshot_index = state_.snapidx_;
  const uint64_t previous_snapshot_term = state_.snapterm_;
  const uint64_t previous_last_log_index = raft_log_.last_index();
  const uint64_t previous_min_active_slot = raft_log_.base();

  // Reconstruct Figure 13's suffix decision from the old boundary when it is
  // still present.  A live reinitialization would use its exact existing
  // snapshot tuple as the same proof; that proof is unreachable today because
  // Setup() is the only caller and state_.snapidx_ is still 0 there.
  const RaftEntry* boundary = FindRaftInstance(recovered_snapshot_index);
  const bool has_boundary =
      boundary != nullptr && boundary->cmd().has_value();
  const uint64_t local_boundary_term =
      has_boundary ? boundary->term() : 0;
  const bool boundary_matches = raft_server_snapshot_boundary_matches(
      has_boundary, local_boundary_term, recovered_snapshot_term);
  const bool has_recovered_suffix = raft_server_log_index_above(
      previous_last_log_index, recovered_snapshot_index);

  const bool live_snapshot_proves_suffix =
      previous_snapshot_index == recovered_snapshot_index &&
      previous_snapshot_term == recovered_snapshot_term &&
      previous_min_active_slot == recovered_snapshot_index + 1;
  const bool retain_suffix =
      raft_server_snapshot_recovery_retains_suffix(
          has_recovered_suffix, has_boundary, boundary_matches,
          live_snapshot_proves_suffix);

  if (raft_server_snapshot_recovery_has_unproven_gap(
          has_recovered_suffix, has_boundary,
          live_snapshot_proves_suffix)) {
    return fail_recovery(
        "recovered suffix has no snapshot boundary or live-snapshot proof");
  }
  if (has_recovered_suffix && has_boundary && !boundary_matches) {
    Log_warn("[RAFT-SNAPSHOT] Site {} discarding recovered suffix after "
             "snapshot boundary term mismatch: local=({}, {}) snapshot=({}, {})",
             site_id_, recovered_snapshot_index, local_boundary_term,
             recovered_snapshot_index, recovered_snapshot_term);
  }

  if (!LoadStateMachineSnapshotLocked(
          snapshot_data, metadata.last_included_index,
          metadata.last_included_term)) {
    return fail_recovery("state-machine snapshot validation/load failed");
  }

  state_.snapidx_ = recovered_snapshot_index;
  state_.snapterm_ = recovered_snapshot_term;
  if (retain_suffix) {
    raft_log_.compact_through(state_.snapidx_);
  } else {
    raft_log_.reset(state_.snapidx_ + 1);
  }
  state_.commit_index_ = raft_server_snapshot_progress_clamp(
      state_.commit_index_, state_.snapidx_, raft_log_.last_index());


  if (state_.current_term_ < state_.snapterm_) {
    Log_warn("[RAFT-SNAPSHOT] Site {} advancing recovered term {} -> {} "
             "to cover snapshot boundary",
             site_id_, state_.current_term_, state_.snapterm_);
    state_.current_term_ = state_.snapterm_;
    state_.vote_for_ = INVALID_SITEID;
  }

  verify(state_.commit_index_ <= raft_log_.last_index());

  snapshot_manager_ = manager;
  snapshot_manager_configured_.store(
      true, rusty::sync::atomic::Ordering::Release);
  snapshot_trigger_index_.store(
      state_.snapidx_, rusty::sync::atomic::Ordering::Release);

  if (state_.snapidx_ > GetAppliedIndex()) {
    PublishAppliedIndexLocked(state_.snapidx_);
  }

  Log_info("[RAFT-SNAPSHOT] Restored snapshot for site {}: index={} term={} "
           "size={} commit={} last={} min_active={} retain_suffix={}",
           site_id_, state_.snapidx_, state_.snapterm_, metadata.size_bytes,
           state_.commit_index_, raft_log_.last_index(), raft_log_.base(), retain_suffix);

  Log_info("[RAFT-SNAPSHOT] Initialized for site {} partition {}: interval={}",
           site_id_, partition_id_, snapshot_interval);
  return true;
  } catch (const std::exception& error) {
    Log_error("[RAFT-SNAPSHOT] Site {} recovery threw: {}",
              site_id_, error.what());
  } catch (...) {
    Log_error("[RAFT-SNAPSHOT] Site {} recovery threw an unknown exception",
              site_id_);
  }
  rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
  stop_.store(true, rusty::sync::atomic::Ordering::Release);
  looping_.store(false, rusty::sync::atomic::Ordering::Release);
  apply_thread_running_.store(false);
  return false;
}

void RaftServer::SetSnapshotManager(
    std::shared_ptr<janus::raft::SnapshotManager> manager) {
  std::lock_guard<std::mutex> lock(mtx_);
  SetSnapshotManagerLocked(std::move(manager));
}

// CALLER MUST HOLD mtx_.
void RaftServer::SetSnapshotManagerLocked(
    std::shared_ptr<janus::raft::SnapshotManager> manager) {
  snapshot_manager_ = std::move(manager);
  snapshot_manager_configured_.store(
      snapshot_manager_ != nullptr,
      rusty::sync::atomic::Ordering::Release);
}

std::shared_ptr<janus::raft::SnapshotManager>
RaftServer::GetSnapshotManager() {
  std::lock_guard<std::mutex> lock(mtx_);
  return snapshot_manager_;
}

void RaftServer::SetSnapshotThreshold(uint64_t threshold) {
  std::lock_guard<std::mutex> lock(mtx_);
  SetSnapshotThresholdLocked(threshold);
}

// CALLER MUST HOLD mtx_.
void RaftServer::SetSnapshotThresholdLocked(uint64_t threshold) {
  state_.snapshot_threshold_ = threshold;
  snapshot_trigger_threshold_.store(
      threshold, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - Copies the manager while holding mtx_ before external I/O.
bool RaftServer::HasSnapshot() {
  auto manager = GetSnapshotManager();
  if (!manager) return false;
  auto latest = manager->GetLatestSnapshot();
  return latest.is_some();
}

// @unsafe - Returns the last snapshotted log index under mtx_.
uint64_t RaftServer::GetSnapshotIndex() {
  std::lock_guard<std::mutex> lock(mtx_);
  return GetSnapshotIndexLocked();
}

// CALLER MUST HOLD mtx_.
uint64_t RaftServer::GetSnapshotIndexLocked() const {
  return state_.snapidx_;
}

// @unsafe - Returns the snapshot boundary term under mtx_.
uint64_t RaftServer::GetSnapshotTerm() {
  std::lock_guard<std::mutex> lock(mtx_);
  return GetSnapshotTermLocked();
}

// CALLER MUST HOLD mtx_.
uint64_t RaftServer::GetSnapshotTermLocked() const {
  return state_.snapterm_;
}

// @unsafe - In-memory log compaction behind the snapshot boundary.
// Acquiring entry point, for callers that do not already hold mtx_.
size_t RaftServer::CompactLog(slotid_t up_to_index) {
  std::lock_guard<std::mutex> lock(mtx_);
  return CompactLogLocked(up_to_index);
}

// CALLER MUST HOLD mtx_.
size_t RaftServer::CompactLogLocked(slotid_t up_to_index) {

  // Compaction is safe only through the prefix represented by both committed
  // state and the installed/local snapshot boundary.
  const slotid_t requested_index = up_to_index;
  up_to_index = raft_server_compaction_safe_index(
      up_to_index, state_.commit_index_, state_.snapidx_);
  if (up_to_index != requested_index) {
    Log_warn("[RAFT-COMPACT] Site {}: Clamped compaction {} -> {} "
             "(state_.commit_index_={}, snapidx={})",
             site_id_, requested_index, up_to_index, state_.commit_index_, state_.snapidx_);
  }

  if (!raft_server_log_index_has_successor(up_to_index)) {
    Log_error("[RAFT-COMPACT] Site {}: Refusing terminal compaction index {}; "
              "the exclusive storage bound and min_active_slot would wrap",
              site_id_, up_to_index);
    return 0;
  }

  const size_t removed_memory = raft_log_.compact_through(up_to_index);

  // up_to_index was proven to have a representable successor above.


  Log_info("[RAFT-COMPACT] Site {}: Compacted in-memory entries through {} "
           "(memory={})",
           site_id_, up_to_index, removed_memory);
  return removed_memory;
}

uint64_t RaftServer::SetStateMachineSnapshotCallbacks(
    std::function<std::string(uint64_t)> create_cb,
    std::function<std::unique_ptr<PreparedStateMachineSnapshotInstall>(
        const std::string&, uint64_t)> prepare_cb) {
  std::lock_guard<std::mutex> lock(mtx_);
  if (state_.next_snapshot_callback_owner_token_ == 0) {
    state_.next_snapshot_callback_owner_token_ = 1;
  }
  const uint64_t owner_token = state_.next_snapshot_callback_owner_token_++;
  create_sm_snapshot_cb_ = std::move(create_cb);
  prepare_sm_snapshot_cb_ = std::move(prepare_cb);
  state_.snapshot_callback_owner_token_ = owner_token;
  return owner_token;
}

bool RaftServer::ClearStateMachineSnapshotCallbacks(
    uint64_t callback_owner_token) {
  if (callback_owner_token == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mtx_);
  if (state_.snapshot_callback_owner_token_ != callback_owner_token) {
    return false;
  }

  create_sm_snapshot_cb_ = {};
  prepare_sm_snapshot_cb_ = {};
  state_.snapshot_callback_owner_token_ = 0;
  return true;
}


// @unsafe - Slow path for the atomic apply-thread hint. Rechecking the
// canonical fields under both locks makes stale hints harmless.
void RaftServer::MaybeCreateSnapshot() {
  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::mutex> lock(mtx_);
  if (!snapshot_manager_ ||
      !raft_server_snapshot_is_due(
          state_.snapidx_, state_.execute_index_, state_.snapshot_threshold_)) {
    return;
  }
  (void)CreateSnapshotLocked();
}

// @unsafe - Caller holds state_machine_apply_mtx_ then mtx_. This keeps the
// callback's serialized bytes, state_.execute_index_, and boundary term in one applied
// state-machine epoch.
bool RaftServer::CreateSnapshotLocked() {

  if (!snapshot_manager_) {
    Log_debug("[RAFT-SNAPSHOT] Site {}: No snapshot manager, skipping CreateSnapshot",
              site_id_);
    return false;
  }

  slotid_t snap_index = state_.execute_index_;
  if (snap_index == 0) {
    Log_debug("[RAFT-SNAPSHOT] Site {}: state_.execute_index_ is 0, nothing to snapshot",
              site_id_);
    return false;
  }
  if (!raft_server_log_index_has_successor(snap_index)) {
    Log_error("[RAFT-SNAPSHOT] Site {}: Cannot snapshot terminal log index {}; "
              "no successor index is representable",
              site_id_, snap_index);
    return false;
  }

  // Determine the term at the snapshot index
  ballot_t snap_term = 0;
  if (raft_server_snapshot_term_uses_boundary(snap_index, state_.snapidx_)) {
    // The boundary entry is intentionally absent after compaction. Its term is
    // carried by snapshot metadata; do not recreate the entry or rewind
    // raft_log_.base() by appending it again.
    snap_term = state_.snapterm_;
  } else {
    const RaftEntry* instance = FindRaftInstance(snap_index);
    if (instance != nullptr) {
      snap_term = instance->term();
    } else {
      // A missing historical term cannot be inferred from state_.current_term_: doing
      // so would forge the snapshot boundary tuple and could make a follower
      // retain a conflicting suffix. Preserve the existing snapshot/log state
      // and wait until a trustworthy boundary is available.
      Log_error("[RAFT-SNAPSHOT] Site {}: Cannot determine term at applied "
                "index {}; aborting snapshot creation",
                site_id_, snap_index);
      return false;
    }
  }

  // Serialize state-machine data. Production may compact only behind a real
  // state-machine checkpoint. RaftLab has no application state and therefore
  // uses a strict 16-byte index+term marker.
  // @unsafe { string operations, callback invocation }
  std::string state_data;
  if (create_sm_snapshot_cb_) {
    try {
      state_data = create_sm_snapshot_cb_(snap_index);
    } catch (const std::exception& error) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback threw: {}",
                site_id_, error.what());
      return false;
    } catch (...) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback threw",
                site_id_);
      return false;
    }
    if (state_data.empty()) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback "
                "returned an empty checkpoint; retaining the log",
                site_id_);
      return false;
    }
    Log_info("[RAFT-SNAPSHOT] Site {}: State machine snapshot callback produced {} bytes",
             site_id_, state_data.size());
  } else {
#ifdef RAFT_TEST_CORO
    // Fallback: 8 bytes state_.execute_index_ + 8 bytes term
    state_data.resize(sizeof(uint64_t) * 2);
    char* ptr = state_data.data();
    std::memcpy(ptr, &snap_index, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    std::memcpy(ptr, &snap_term, sizeof(uint64_t));
#else
    Log_error("[RAFT-SNAPSHOT] Site {} has no state-machine snapshot callback; "
              "production compaction is disabled",
              site_id_);
    return false;
#endif
  }

  // Persist the snapshot via the snapshot manager
  // @unsafe { snapshot_manager_ I/O operations }
  bool saved = snapshot_manager_->TakeSnapshot(
      snap_index, snap_term,
      state_data.data(), state_data.size());

  if (!saved) {
    Log_error("[RAFT-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              site_id_, snap_index, snap_term);
    return false;
  }

  // Update snapshot metadata
  slotid_t old_snapidx = state_.snapidx_;
  state_.snapidx_ = snap_index;
  state_.snapterm_ = snap_term;
  snapshot_trigger_index_.store(
      state_.snapidx_, rusty::sync::atomic::Ordering::Release);

  Log_info("[RAFT-SNAPSHOT] Site {}: Snapshot saved at index={} term={} (prev snapidx={})",
           site_id_, snap_index, snap_term, old_snapidx);

  // Compact the log up to the snapshot index
  size_t compacted = CompactLogLocked(snap_index);
  Log_info("[RAFT-SNAPSHOT] Site {}: Compacted {} entries up to index={}",
           site_id_, compacted, snap_index);
  return true;
}

// ============================================================================

// @unsafe - Logs term changes (Log_info marked safe via @external)
void RaftServer::LogTermChange(const char* reason,
                               uint64_t old_term,
                               uint64_t new_term,
                               siteid_t source) {
  if (old_term == new_term) {
    return;
  }
  // @unsafe
  {
  const char* why = reason ? reason : "unspecified";
  if (source != INVALID_SITEID) {
    Log_info("[RAFT-TERM] server {} term {} -> {} ({}, source_site={})",
             site_id_, old_term, new_term, why, source);
  } else {
    Log_info("[RAFT-TERM] server {} term {} -> {} ({})",
             site_id_, old_term, new_term, why);
  }
  }
}

RaftServer::RaftServer()
  : replication_wake_gate_(rusty::Arc<ReplicationWakeGate>::make_with(
        []() { return ReplicationWakeGate::new_(); }))
{
  async_callback_lifetime_->server = this;
  // Keep the immutable kind-4 compatibility factory registered as soon as a
  // Raft server exists so a legacy payload relayed by a peer still decodes.
  EnsureLegacyRaftLogPayloadRegistered();
#ifdef RAFT_TEST_CORO
  setIsLeader(false);
#endif
  stop_.store(false, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - Binds the gate before the owner starts HeartbeatLoop.
void RaftServer::BindReplicationWakeOwner(
    rusty::Arc<rrr::PollThread> owner) {
  replication_wake_gate_->bind_owner(std::move(owner));
}

// @unsafe - Any-thread publication followed by a gate-only PollThread job.
void RaftServer::RequestReplication() {
  if (!replication_wake_gate_->publish()) {
    return;
  }
  QueueReplicationWake(replication_wake_gate_);
}

// @unsafe - Called only by HeartbeatLoop on its bound PollThread.
//
// The gate's own fast path lives in begin_wait_for_work(): Some(answer) when
// it could decide without arming a waiter, None when it could not. Everything
// this function adds is the one thing the DSL cannot spell -- calling the
// reactor factory create_sp_int_event -- and it is called ONLY on the slow
// path, so a round that finds work already pending still allocates nothing.
bool RaftServer::WaitForReplicationOrHeartbeat(uint64_t timeout_us) {
  auto decided = replication_wake_gate_->begin_wait_for_work();
  if (decided.is_some()) {
    return decided.unwrap();
  }
  return replication_wake_gate_->finish_wait_for_work(
      create_sp_int_event(1), timeout_us);
}

// @unsafe - Called only by the election fiber on the bound PollThread.
//
// Same split, and the accepting() check stays ahead of the factory call for
// the same reason it did when this was one function: a closed gate must not
// allocate an event it will never wait on.
bool RaftServer::WaitForElectionTimeoutOrShutdown(uint64_t timeout_us) {
  if (!replication_wake_gate_->accepting()) {
    return false;
  }
  return replication_wake_gate_->wait_for_election_timeout(
      create_sp_int_event(1), timeout_us);
}

// @unsafe - Close ordering is intentional: make new submissions inert, queue
// one owner-thread wake for an armed waiter, then drop the owner's gate handle.
void RaftServer::CloseReplicationWakeGate() {
  replication_wake_gate_->close();
  QueueReplicationShutdownWake(replication_wake_gate_);
  replication_wake_gate_->clear_owner();
}

// @unsafe - Reactor-fiber completion barrier used before deleting a live
// RaftServer.  Both runtime loops publish their running state with Release.
void RaftServer::PrepareForShutdown() {
  {
    // Linearize admission closure with every RPC/local mutation under mtx_.
    std::lock_guard<std::mutex> admission_lock(mtx_);
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
  }
  CloseReplicationWakeGate();

  while (heartbeat_loop_running_.load(
             rusty::sync::atomic::Ordering::Acquire) ||
         election_loop_running_.load(
             rusty::sync::atomic::Ordering::Acquire)) {
    if (Fiber::current_fiber().is_some()) {
      Fiber::sleep(1000);
    } else {
      // Production shutdown runs from a native worker thread rather than a
      // reactor fiber.  The owner PollThread remains live until this barrier
      // completes, so a short native sleep lets it drain both loop fibers.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Applying an entry can trigger snapshot compaction. Stop and join that
  // producer while the server is still fully alive.
  apply_thread_running_.store(false);
  if (apply_thread_.joinable()) {
    apply_thread_.join();
  }
}

// @unsafe - Election timeout calculation (Time::now and RandomGenerator::rand marked safe via @external)
uint64_t RaftServer::GetElectionTimeout() {
  // Must be called with mtx_ held. The one caller repo-wide is resetTimer(),
  // which takes mtx_ before reaching here, so the recursive re-acquisition
  // that used to sit on this line was a no-op. The configured identity is
  // still stable for the whole decision -- it is the caller's lock that makes
  // it so. Tranche 4b.
  uint64_t current_time = Time::now(true);
  const uint64_t grace_period_us = GetPreferredLeaderGracePeriodUs();
  bool in_grace_period = (current_time - startup_timestamp_) < grace_period_us;
  uint64_t randomized_timeout = 0;
  const bool preferred_leader_configured =
      IsPreferredLeaderConfigured(preferred_leader_site_id_);

  if (!preferred_leader_configured) {
    // Traditional Raft behavior when no preferred leader is configured.
    randomized_timeout = GetNonPreferredSteadyElectionTimeoutUs();
  } else if (AmIPreferredLeader()) {
    randomized_timeout = GetPreferredElectionTimeoutUs();
  } else if (in_grace_period) {
    // Startup grace timeout is tunable via env for test stability.
    randomized_timeout = GetNonPreferredGraceElectionTimeoutUs();
  } else {
    randomized_timeout = GetNonPreferredSteadyElectionTimeoutUs();
  }

  // Memory-only Raft configures no log storage, so the randomized timeout is
  // the effective election timeout: there is no persistence floor to add.
  return randomized_timeout;
}

// Enqueue newly committed entries for the background apply thread.
// Called from OnAppendEntries (already under mtx_) when state_.commit_index_ advances.
void RaftServer::EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit) {
  // apply_queue_ now holds Command — direct copy from
  // RaftEntry::cmd_ (also Command after prep2).
  std::vector<std::pair<slotid_t, Command>> batch;
  slotid_t first_missing = 0;
  for (slotid_t id = old_commit + 1; id <= new_commit; id++) {
    const RaftEntry* it = FindRaftInstance(id);
    if (it != nullptr && it->cmd().has_value()) {
      batch.emplace_back(id, it->cmd());
    } else {
      first_missing = id;
      break;  // Gap in log — stop here
    }
  }
  if (!batch.empty()) {
    std::lock_guard<std::mutex> lock(apply_queue_mtx_);
    for (auto& entry : batch) {
      apply_queue_.push_back(QueuedApplyEntry{
          entry.first, std::move(entry.second), apply_queue_epoch_});
    }
  }
  // Log if we couldn't enqueue the full range
  if (first_missing > 0) {
    Log_info("[ENQUEUE] Site {}: gap at slot {} (range {}..{}, enqueued {})",
             site_id_, first_missing, old_commit + 1, new_commit, batch.size());
  }
  static uint64_t enqueue_log_counter = 0;
  if (enqueue_log_counter++ % 50 == 0) {
    size_t qsize = 0;
    {
      std::lock_guard<std::mutex> lock(apply_queue_mtx_);
      qsize = apply_queue_.size();
    }
    Log_info("[ENQUEUE] Site {}: enqueued {} entries ({}..{}) queue_total={}",
             site_id_, batch.size(), old_commit + 1, new_commit, qsize);
  }
}

// Background OS thread for entry application.
// Drains from apply_queue_ (populated by OnAppendEntries) to avoid contention on mtx_.
// Acquiring entry point. Every caller owns the state-machine apply gate;
// taking the Raft mutex here completes the documented apply-gate -> Raft-state
// lock order and keeps the legacy state_.execute_index_ field synchronized with
// consensus readers.
void RaftServer::PublishAppliedIndex(uint64_t index) {
  std::lock_guard<std::mutex> lock(mtx_);
  PublishAppliedIndexLocked(index);
}

// CALLER MUST HOLD mtx_.
void RaftServer::PublishAppliedIndexLocked(uint64_t index) {
  const uint64_t published = GetAppliedIndex();
  if (raft_server_log_index_above(published, index)) {
    Log_warn("[RAFT-APPLY] Site {} refusing to move applied index backward "
             "from {} to {}",
             site_id_, published, index);
    return;
  }
  state_.execute_index_ = index;
  appliedIndexForWait_.store(
      index, rusty::sync::atomic::Ordering::Release);
}

void RaftServer::StartApplyThread() {
  apply_thread_running_.store(true);
  apply_thread_ = std::thread([this]() {
    Log_info("[APPLY-THREAD] Site {}: Started background apply thread", site_id_);
    uint64_t apply_count = 0;
    auto last_log_time = std::chrono::steady_clock::now();
    while (!stop_.load(rusty::sync::atomic::Ordering::Acquire) &&
           apply_thread_running_.load()) {
      // Drain entries from the queue
      QueuedApplyEntry entry;
      bool got_entry = false;
      size_t queue_size = 0;
      {
        std::lock_guard<std::mutex> lock(apply_queue_mtx_);
        queue_size = apply_queue_.size();
        if (!apply_queue_.empty()) {
          entry = std::move(apply_queue_.front());
          apply_queue_.pop_front();
          got_entry = true;
        }
      }

      if (got_entry) {
        slotid_t id = entry.index;
        auto& log_entry = entry.command;
        bool applied_entry = false;
        {
          // An InstallSnapshot can acquire this gate after the entry is popped
          // but before its callback starts. Re-check the published applied
          // index inside the gate so a snapshot-covered entry is skipped after
          // the snapshot state has been loaded.
          std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
          uint64_t current_epoch = 0;
          {
            std::lock_guard<std::mutex> queue_lock(apply_queue_mtx_);
            current_epoch = apply_queue_epoch_;
          }
          const uint64_t applied_index = GetAppliedIndex();
          if (!raft_server_apply_epoch_is_current(
                  entry.epoch, current_epoch)) {
            Log_debug("[APPLY-THREAD] Site {}: Skipping invalidated entry {} "
                      "(entry_epoch={} current_epoch={})",
                      site_id_, id, entry.epoch, current_epoch);
          } else if (raft_server_log_index_at_or_below(
                         id, applied_index)) {
            Log_debug("[APPLY-THREAD] Site {}: Skipping snapshot-covered entry {} "
                      "(applied={})",
                      site_id_, id, applied_index);
          } else {
            // Log entries near the stall point for debugging
            if (id >= 470 && id <= 500) {
              Log_info("[APPLY-THREAD] Site {}: ABOUT TO APPLY entry {} (queue_remaining={})",
                       site_id_, id, queue_size);
            }
            // @unsafe - callback may have side effects
            try {
              if (!raft_server_command_is_internal_noop(
                      log_entry.kind_, TpcNoopCommand::static_kind())) {
                app_next_(id, log_entry);
              }
            } catch (const std::exception& error) {
              Log_error("[RAFT-APPLY] Site {} callback failed at slot {}: {}",
                        site_id_, id, error.what());
              rpc_ready_.store(
                  false, rusty::sync::atomic::Ordering::Release);
              stop_.store(true, rusty::sync::atomic::Ordering::Release);
              looping_.store(false, rusty::sync::atomic::Ordering::Release);
              continue;
            } catch (...) {
              Log_error("[RAFT-APPLY] Site {} callback failed at slot {}",
                        site_id_, id);
              rpc_ready_.store(
                  false, rusty::sync::atomic::Ordering::Release);
              stop_.store(true, rusty::sync::atomic::Ordering::Release);
              looping_.store(false, rusty::sync::atomic::Ordering::Release);
              continue;
            }
            if (id >= 470 && id <= 500) {
              Log_info("[APPLY-THREAD] Site {}: DONE APPLYING entry {}", site_id_, id);
            }
            PublishAppliedIndex(id);
            applied_entry = true;
          }
        }
        if (!applied_entry) {
          continue;
        }
        apply_count++;

        // Log progress periodically
        if (apply_count % 100 == 0) {
          Log_info("[APPLY-THREAD] Site {}: applied {} entries, state_.execute_index_={} queue_remaining={}",
                   site_id_, apply_count, GetAppliedIndex(), queue_size);
        }

        // Snapshot trigger for queued apply path. The hot precheck reads only
        // atomic mirrors; the slow path revalidates canonical state under the
        // apply-gate -> Raft-mutex order.
        if (snapshot_manager_configured_.load(
                rusty::sync::atomic::Ordering::Acquire)) {
          const uint64_t trigger_snapshot_index =
              snapshot_trigger_index_.load(
                  rusty::sync::atomic::Ordering::Acquire);
          const uint64_t trigger_threshold =
              snapshot_trigger_threshold_.load(
                  rusty::sync::atomic::Ordering::Acquire);
          if (raft_server_snapshot_is_due(
                  trigger_snapshot_index, GetAppliedIndex(),
                  trigger_threshold)) {
            MaybeCreateSnapshot();
          }
        }

        // Route periodic cleanup through the snapshot-aware compactor. It will
        // retain any prefix not yet covered by a snapshot.
        if (id % 5000 == 0) {
          const slotid_t cutoff =
              (GetAppliedIndex() > 10000) ? GetAppliedIndex() - 10000 : 0;
          CompactLog(cutoff);
        }
      } else {
        // Periodic heartbeat when queue is empty
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
          uint64_t commit_index_snapshot = 0;
          {
            std::lock_guard<std::mutex> lock(mtx_);
            commit_index_snapshot = state_.commit_index_;
          }
          Log_info("[APPLY-THREAD] Site {}: IDLE state_.execute_index_={} state_.commit_index_={} queue_size={} applied_total={}",
                   site_id_, GetAppliedIndex(), commit_index_snapshot,
                   queue_size, apply_count);
          last_log_time = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    Log_info("[APPLY-THREAD] Site {}: Background apply thread exiting", site_id_);
  });
  // Keep the thread joinable so the destructor can await it. Detaching here
  // causes use-after-free: the thread captures `this` and keeps running after
  // ~RaftServer destroys the RaftServer, resulting in an empty std::function
  // invocation when it next pulls from apply_queue_.
}

// @unsafe - Server setup (Time::now, Log_debug, Fiber::create_run marked safe via @external)
bool RaftServer::SetupInternal() {
  // RPC services may already be listening when this owner-thread job begins.
  // Keep every handler fail-closed until snapshot loading has completed.
  rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);

  // Record startup time for grace period logic
  startup_timestamp_ = Time::now(true);

  // ========== HEARTBEAT INTERVAL (runtime override) ==========
  // @unsafe { std::getenv and Log_info are not borrow-checked }
  {
    const char* hb_str = std::getenv("MAKO_RAFT_HEARTBEAT_INTERVAL_US");
    if (hb_str && hb_str[0] != '\0') {
      try {
        heartbeat_interval_us_ = std::stoull(hb_str);
      } catch (const std::exception& error) {
        Log_error("[RAFT] Invalid heartbeat interval '{}': {}",
                  hb_str, error.what());
        stop_.store(true, rusty::sync::atomic::Ordering::Release);
        looping_.store(false, rusty::sync::atomic::Ordering::Release);
        return false;
      }
      Log_info("[RAFT] Heartbeat interval set to {} us from env", heartbeat_interval_us_);
    }
  }

  // Bind before HeartbeatLoop can publish its owner-thread-only IntEvent.
  // The communicator always retains the PollThread it created or was given.
  rusty::Option<rusty::Arc<rrr::PollThread>> replication_poll = rusty::None;
  if (commo() != nullptr) {
    replication_poll = commo()->PollThread();
  }
  if (replication_poll.is_some()) {
    BindReplicationWakeOwner(replication_poll.unwrap());
  } else {
    Log_error("[RAFT-WAKE] Site {} has no PollThread owner during Setup",
              site_id_);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    return false;
  }

  // ========== LOG RETENTION WINDOW (runtime override) ==========
  // @unsafe { std::getenv and Log_info are not borrow-checked }
  {
    const char* lrw_str = std::getenv("MAKO_RAFT_LOG_RETENTION_WINDOW");
    if (lrw_str && lrw_str[0] != '\0') {
      uint64_t val = 0;
      try {
        val = std::stoull(lrw_str);
      } catch (const std::exception& error) {
        Log_error("[RAFT] Invalid log retention window '{}': {}",
                  lrw_str, error.what());
        stop_.store(true, rusty::sync::atomic::Ordering::Release);
        looping_.store(false, rusty::sync::atomic::Ordering::Release);
        return false;
      }
      log_retention_window_ = raft_server_retention_window_normalize(val);
      Log_info("[RAFT] Log retention window set to {} from env", log_retention_window_);
    }
  }

  // ========== INITIALIZE SNAPSHOT MANAGER ==========
  if (!InitializeSnapshotManager()) {
    Log_error("[RAFT-SNAPSHOT] Site {} cannot start after snapshot recovery failure",
              site_id_);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    return false;
  }

  // ========== INITIALIZE MEMBERSHIP CONFIGURATION ==========
  // Populate current_config_ from the static partition configuration. This is
  // the fixed replica set for this partition's lifetime; memory-only Raft has
  // no membership change.
  {
    auto config = Config::GetConfig();
    auto replicas = config->SitesByPartitionId(partition_id_);
    for (auto& site : replicas) {
      current_config_.insert(site.id);
    }
    Log_info("[RAFT-CONFIG] Initialized current_config_ for site {} partition {} with {} replicas",
             site_id_, partition_id_, current_config_.size());
  }

  StartApplyThread();
  rpc_ready_.store(true, rusty::sync::atomic::Ordering::Release);

// Unconditional. This was written twice, once under #ifdef
// RAFT_TEST_CORO and once under #ifndef, with CHARACTER-IDENTICAL
// bodies -- so it always ran, and editing one arm without the other
// was a standing trap.
  if (heartbeat_) {
		Log_debug("starting heartbeat loop at site {}", site_id_);
    heartbeat_loop_running_.store(
        true, rusty::sync::atomic::Ordering::Release);
    Fiber::create_run([this](){
      this->HeartbeatLoop();
    });
    // Start election timeout loop
    if (failover_) {
      election_loop_running_.store(
          true, rusty::sync::atomic::Ordering::Release);
      Fiber::create_run([this](){
        StartElectionTimer();
      });
    }
	}

  // Election timer will be started in Start() method when first command is submitted
  return true;
}

// @unsafe - Converts every startup exit, including exceptions from storage or
// callbacks, into one observable completion state for worker readiness.
void RaftServer::Setup() {
  bool succeeded = false;
  try {
    succeeded = SetupInternal();
  } catch (const std::exception& error) {
    Log_error("[RAFT-STARTUP] Site {} setup threw: {}", site_id_, error.what());
  } catch (...) {
    Log_error("[RAFT-STARTUP] Site {} setup threw an unknown exception",
              site_id_);
  }

  if (!succeeded) {
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
  }
  {
    std::lock_guard<std::mutex> lock(startup_mtx_);
    startup_succeeded_ = succeeded && IsRpcReady();
    startup_finished_ = true;
  }
  startup_cv_.notify_all();
}

// @safe
bool RaftServer::WaitForStartup() {
  std::unique_lock<std::mutex> lock(startup_mtx_);
  startup_cv_.wait(lock, [this]() { return startup_finished_; });
  return startup_succeeded_;
}

void RaftServer::Disconnect(const bool disconnect) {
  std::lock_guard<std::mutex> lock(mtx_);
  verify(disconnected_.load(std::memory_order_acquire) != disconnect);
  commo()->SetNetworkEnabled(!disconnect);
  disconnected_.store(disconnect, std::memory_order_release);
}

// @unsafe - Synchronizes with Disconnect() through the Raft state mutex.
bool RaftServer::IsDisconnected() {
  return disconnected_.load(std::memory_order_acquire);
}

// @unsafe - Synchronizes with role/leader publication through the Raft mutex.
siteid_t RaftServer::GetLeaderHint() {
  std::lock_guard<std::mutex> lock(mtx_);
  if (state_.is_leader_) {
    return site_id_;
  }
  return state_.current_leader_id_;
}

// @unsafe - Leadership state transition (callbacks and logging wrapped in @unsafe blocks)
// Must be called with mtx_ held. Every caller reaches here from inside
// RequestVoteImpl, OnAppendEntries, OnRequestVote (via doVote),
// OnInstallSnapshot or stepDown, all of which hold mtx_. The sole exception is
// the RAFT_TEST_CORO line in RaftServer's own constructor, where no other
// thread can observe the object yet. The recursive re-acquisition removed from
// this line was therefore always a no-op -- one of the 24 nested acquisitions
// docs/migration/raft/cpp-refactor-plan.md tranche 4b enumerates.
void RaftServer::setIsLeader(bool isLeader) {
  bool prev_is_leader = state_.is_leader_;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site {} (loc {}) term {}: prev_is_leader={} new_is_leader={}",
           site_id_, loc_id_, state_.current_term_, prev_is_leader, isLeader);
#endif

  if (isLeader && !prev_is_leader) {
    // Leadership publication must not proceed once shutdown has begun.
    const uint64_t publication_term = state_.current_term_;
    if (stop_.load(rusty::sync::atomic::Ordering::Acquire) ||
        state_.current_term_ != publication_term) {
      Log_warn("[RAFT_STATE] Site {} suppressing stale leadership publication "
               "for term {} (current={}, stopping={})",
               site_id_, publication_term, state_.current_term_,
               stop_.load(rusty::sync::atomic::Ordering::Acquire));
      return;
    }
  }

  if (isLeader) {
    // A heartbeat proof belongs to exactly one leadership term. Reset the
    // local generation before publishing this server as leader so delayed or
    // historical acknowledgements cannot prove a quorum in the new term.
    state_.heartbeat_round_ = 0;
    state_.read_quorum_confirmed_term_ = 0;
    state_.read_quorum_confirmed_round_ = 0;
  }

  if (isLeader && failover_) {
    // Every caller of setIsLeader already holds mtx_ (server.cc:1754-1759).
    RebuildPeerTables(raft_log_.last_index() + 1);
    for (size_t ord = 0; ord < peers_.len(); ord++) {
      Log_debug("loc_id_={} match_index_[{}]={}, next_index_[{}]={}",
                loc_id_, peer_sites_[ord], peers_.match_index(ord),
                peer_sites_[ord], peers_.next_index(ord));
    }
  }


  // This 2 lines MUST put BEFORE state_.is_leader_ = isLeader ! otherwise they will become 0
  bool become_new_leader = isLeader && (!state_.is_leader_);
  bool become_new_follower = (!isLeader) && state_.is_leader_;

  // Update the leader state
  state_.is_leader_ = isLeader;

  // Becoming leader establishes self as the known leader. Becoming a follower
  // deliberately preserves a hint learned from AppendEntries/InstallSnapshot;
  // transitions without a known leader clear it at their call sites.
  state_.current_leader_id_ = raft_server_leader_hint_after_transition(
      isLeader,
      !isLeader && state_.current_leader_id_ != INVALID_SITEID,
      site_id_, state_.current_leader_id_);

  // Only log on actual transitions, not no-op calls
  if (become_new_leader || become_new_follower) {
    Log_info("RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}", site_id_, become_new_leader, become_new_follower, isLeader);
  }

  // Only act when transitioning from non-leader to leader
  if (become_new_leader) {
    Log_info("[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}",
             site_id_, state_.current_term_, prev_is_leader, become_new_leader);

#ifndef RAFT_TEST_CORO
    // Raft only commits prior-term entries after committing an entry from the
    // current term. Append one internal no-op on becoming leader, so old
    // client submissions resolve even when every client is blocked on the
    // former leader. The apply paths consume this protocol entry without
    // invoking the application state machine.
    uint64_t noop_previous_index = 0;
    uint64_t noop_term = 0;
    auto noop = rusty::Arc<TpcNoopCommand>::make();
    const RaftStartResult noop_result = SetLocalAppend(
        janus::Command::pack_aliased<TpcNoopCommand>(std::move(noop)),
        &noop_term, &noop_previous_index);
    verify(raft_server_start_was_appended(noop_result));
    verify(noop_term == state_.current_term_);
    verify(raft_log_.last_index() == noop_previous_index + 1);
    Log_info("[RAFT-NOOP] Site {} appended leader no-op at index {} term {}",
             site_id_, raft_log_.last_index(), state_.current_term_);
    RequestReplication();
#endif

  } else if (become_new_follower) {
    Log_info("[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}",
             site_id_, state_.current_term_, prev_is_leader, become_new_follower);

    // ============================================================================
    // CRITICAL FIX: Reset election timer when becoming follower
    // ============================================================================
    // This prevents instant elections after recovery/resume. When a node resumes
    // from SIGSTOP/pause, state_.last_heartbeat_time_ is stale (from before pause).
    // Resetting it here ensures the election timer counts from NOW, giving the
    // current leader time to send heartbeats before this node starts an election.
    // This is standard Raft behavior: followers reset their timer when stepping down.
    // setIsLeader is caller-holds; see the enumeration above.
    resetTimerLocked("became follower");
    Log_info("[RAFT_TIMER] Site {} reset election timer when becoming follower (last_hb now={})",
             site_id_, state_.last_heartbeat_time_);

    // When transitioning from leader to non-leader
    Log_info("[RAFT_VIEW] Server {} stepping down as leader for partition {}", site_id_, partition_id_);
  }

  // CRITICAL: Fire leadership change callback so RaftWorker can update its state
  // This allows clients to retarget to the new leader after elections
  if (leader_change_cb_) {
    // @unsafe
    {
    if (become_new_leader) {
      Log_info("[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(true) - became leader", site_id_);
      leader_change_cb_(true);
    } else if (become_new_follower) {
      Log_info("[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(false) - became follower", site_id_);
      leader_change_cb_(false);
    }
    }
  }
}


// @unsafe - external calls marked @external [safe], core replication loop
// TODO: Revisit borrow checker errors in this function.
// The checker reports "use after move" for loop-local variables (matchedIndices,
// batch_buffer_, batch_cmd, cmd) due to 2-iteration loop simulation. These variables
// are declared fresh each iteration, but the checker may not be resetting state
// correctly for loop-local declarations. Additionally, SendAppendEntries2 takes
// shared_ptr<Marshallable> by value (moves), which compounds the issue.
// Potential fixes: (1) Change SendAppendEntries2 to take const shared_ptr&,
// (2) Investigate checker's loop-local variable handling.
// ============================================================================
// PARALLEL HEARTBEAT FIX
// ============================================================================
// This struct holds context for each pending AppendEntries RPC.
// Used to send RPCs in parallel and process responses without blocking.
// The response field uses shared_ptr to ensure memory validity when callback fires.
// The in-flight AppendEntries table, owned by Rust.
//
// FIRST OPAQUE CARRY OF WIRE TYPES. PendingAppend holds the RPC's
// shared_ptr<AppendEntriesResponse> and its janus::Command, neither of which
// has a DSL spelling. Rust holds both and hands them back; it cannot construct
// or dereference either, because the rustc facade models them as zero-sized
// opaque structs (rusty-rustc/src/lib.rs). "Carried, never followed" is
// therefore checkable rather than a convention.
//
// ONE SLOT PER FOLLOWER, indexed by the same ordinal PeerTable uses. The
// invariant that at most one AppendEntries is in flight per follower used to
// be emergent -- one entry in a std::map keyed by site -- and is now
// structural: there is one slot and it is either occupied or not.
//
// A NOTE ON Option AND unwrap(), which is a live hazard in this runtime.
// rusty::Option has two unwrap overloads: `const T& unwrap() const` returns a
// reference, while the non-const `T unwrap()` MOVES OUT and clears the Option
// (option.hpp:293-314). A `match` inside a `&mut self` method would bind the
// second and silently empty the slot on what reads like an inspection. Every
// read below is `&self`, so the emitter reaches for std::as_const and gets the
// reference overload; mutation is whole-slot assignment only, never
// match-and-modify. Keep it that way.
#if RUSTYCPP_RUST
pub struct PendingAppend {
    follower_: u16,
    sent_term_: u64,
    sent_round_: u64,
    // Inclusive end of the exact prefix proved by this RPC's wire payload. A
    // heartbeat proves only prevLogIndex; raw and batched payloads extend it
    // by their encoded entry count.
    sent_end_index_: u64,
    response_: rusty::RaftResponsePtr,
    // Empty Command (has_value() == false) signals a heartbeat.
    cmd_: rusty::RaftCommand,
}

impl PendingAppend {
    pub fn new(follower: u16, sent_term: u64, sent_round: u64,
               sent_end_index: u64, response: rusty::RaftResponsePtr,
               cmd: rusty::RaftCommand) -> PendingAppend {
        PendingAppend {
            follower_: follower,
            sent_term_: sent_term,
            sent_round_: sent_round,
            sent_end_index_: sent_end_index,
            response_: response,
            cmd_: cmd,
        }
    }
}

pub struct PendingTable {
    slots_: rusty::Vec<rusty::Option<PendingAppend>>,
}

#[allow(clippy::new_without_default)]
impl PendingTable {
    pub fn new() -> PendingTable {
        PendingTable { slots_: rusty::Vec::new() }
    }

    // One slot per follower, all empty. Called wherever the peer table is
    // sized, so the two always agree on what an ordinal means.
    pub fn resize(&mut self, peers: usize) {
        self.slots_.clear();
        let mut i: usize = 0;
        while i < peers {
            self.slots_.push(rusty::None);
            i += 1;
        }
    }

    // Drops every in-flight context. Used on leadership loss and on a term
    // change, so a prior epoch's RPC can never occupy a slot.
    pub fn abandon(&mut self) {
        let peers = self.slots_.len();
        self.resize(peers);
    }

    pub fn len(&self) -> usize {
        self.slots_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.slots_.is_empty()
    }

    pub fn occupied(&self, ordinal: usize) -> bool {
        self.slots_[ordinal].is_some()
    }

    pub fn place(&mut self, ordinal: usize, pending: PendingAppend) {
        self.slots_[ordinal] = rusty::Some(pending);
    }

    pub fn release(&mut self, ordinal: usize) {
        self.slots_[ordinal] = rusty::None;
    }

    pub fn follower(&self, ordinal: usize) -> u16 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().follower_
    }

    pub fn sent_term(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_term_
    }

    pub fn sent_round(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_round_
    }

    pub fn sent_end_index(&self, ordinal: usize) -> u64 {
        if self.slots_[ordinal].is_none() {
            return 0;
        }
        self.slots_[ordinal].as_ref().unwrap().sent_end_index_
    }

    // Both of these hand a carried C++ value back to C++. The reference is
    // safe because the method is &self: the emitter binds the const unwrap
    // overload, which returns a reference into the live Option rather than a
    // moved-out temporary.
    // Callers check occupied() first; unwrap is the assertion of that.
    pub fn response(&self, ordinal: usize) -> &rusty::RaftResponsePtr {
        &self.slots_[ordinal].as_ref().unwrap().response_
    }

    // Callers check occupied() first; unwrap is the assertion of that.
    pub fn cmd(&self, ordinal: usize) -> &rusty::RaftCommand {
        &self.slots_[ordinal].as_ref().unwrap().cmd_
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.pending_table version=1 rust_sha256=5054676e845496033d7e41a846b5bb2eda6087d1d53c43e46fc6a80743e09190*/
struct PendingAppend;
struct PendingTable;

struct PendingAppend {
    uint16_t follower_;
    uint64_t sent_term_;
    uint64_t sent_round_;
    uint64_t sent_end_index_;
    rusty::RaftResponsePtr response_;
    rusty::RaftCommand cmd_;

    static PendingAppend new_(uint16_t follower, uint64_t sent_term, uint64_t sent_round, uint64_t sent_end_index, rusty::RaftResponsePtr response, rusty::RaftCommand cmd);
};

struct PendingTable {
    rusty::Vec<rusty::Option<PendingAppend>> slots_;

    static PendingTable new_();
    void resize(size_t peers);
    void abandon();
    size_t len() const;
    bool is_empty() const;
    bool occupied(size_t ordinal) const;
    void place(size_t ordinal, PendingAppend pending);
    void release(size_t ordinal);
    uint16_t follower(size_t ordinal) const;
    uint64_t sent_term(size_t ordinal) const;
    uint64_t sent_round(size_t ordinal) const;
    uint64_t sent_end_index(size_t ordinal) const;
    const rusty::RaftResponsePtr& response(size_t ordinal) const;
    const rusty::RaftCommand& cmd(size_t ordinal) const;
};


inline PendingAppend PendingAppend::new_(uint16_t follower, uint64_t sent_term, uint64_t sent_round, uint64_t sent_end_index, rusty::RaftResponsePtr response, rusty::RaftCommand cmd) {
    return PendingAppend{.follower_ = std::move(follower), .sent_term_ = std::move(sent_term), .sent_round_ = std::move(sent_round), .sent_end_index_ = std::move(sent_end_index), .response_ = std::move(response), .cmd_ = std::move(cmd)};
}

inline PendingTable PendingTable::new_() {
    return PendingTable{.slots_ = rusty::Vec<rusty::Option<PendingAppend>>::new_()};
}

inline void PendingTable::resize(size_t peers) {
    this->slots_.clear();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(peers)) {
        this->slots_.push(rusty::None);
        i += 1;
    }
}

inline void PendingTable::abandon() {
    auto peers = rusty::len(this->slots_);
    this->resize(std::move(peers));
}

inline size_t PendingTable::len() const {
    return rusty::len(this->slots_);
}

inline bool PendingTable::is_empty() const {
    return rusty::is_empty(this->slots_);
}

inline bool PendingTable::occupied(size_t ordinal) const {
    return this->slots_[ordinal].is_some();
}

inline void PendingTable::place(size_t ordinal, PendingAppend pending) {
    this->slots_[ordinal] = rusty::Option<PendingAppend>(std::move(pending));
}

inline void PendingTable::release(size_t ordinal) {
    this->slots_[ordinal] = rusty::None;
}

inline uint16_t PendingTable::follower(size_t ordinal) const {
    if (this->slots_[ordinal].is_none()) {
        return static_cast<uint16_t>(0);
    }
    return this->slots_[ordinal].as_ref().unwrap().follower_;
}

inline uint64_t PendingTable::sent_term(size_t ordinal) const {
    if (this->slots_[ordinal].is_none()) {
        return static_cast<uint64_t>(0);
    }
    return this->slots_[ordinal].as_ref().unwrap().sent_term_;
}

inline uint64_t PendingTable::sent_round(size_t ordinal) const {
    if (this->slots_[ordinal].is_none()) {
        return static_cast<uint64_t>(0);
    }
    return this->slots_[ordinal].as_ref().unwrap().sent_round_;
}

inline uint64_t PendingTable::sent_end_index(size_t ordinal) const {
    if (this->slots_[ordinal].is_none()) {
        return static_cast<uint64_t>(0);
    }
    return this->slots_[ordinal].as_ref().unwrap().sent_end_index_;
}

inline const rusty::RaftResponsePtr& PendingTable::response(size_t ordinal) const {
    return this->slots_[ordinal].as_ref().unwrap().response_;
}

inline const rusty::RaftCommand& PendingTable::cmd(size_t ordinal) const {
    return this->slots_[ordinal].as_ref().unwrap().cmd_;
}
/*RUSTYCPP:GEN-END id=raft_server.pending_table*/

// Heartbeat quorum evidence belongs to the exact generation and membership
// snapshot that produced it. Slow synchronous followers may reply after the
// HeartbeatLoop has advanced to a later generation, so retain each generation
// until its launched RPCs have either completed or proved a quorum.
//
// The EVIDENCE -- who has voted, who is still outstanding, for which term and
// against how large a config -- is a DSL-owned type. The membership snapshot
// stays C++: it is compared against current_config_, a std::set, which a
// rusty::BTreeSet cannot be compared with. The quorum predicates stay where
// they are in quorum.hpp and are called on this type's accessors, rather than
// being duplicated into it.
#if RUSTYCPP_RUST
pub struct HeartbeatAuthority {
    term_: u64,
    config_size_: usize,
    voters_: rusty::BTreeSet<u16>,
    outstanding_: rusty::BTreeSet<u16>,
}

#[allow(clippy::new_without_default)]
impl HeartbeatAuthority {
    // A generation begins with this site already counted as a voter: a leader
    // is evidence for its own authority.
    pub fn new(term: u64, config_size: usize, self_site: u16) -> HeartbeatAuthority {
        let mut voters = rusty::BTreeSet::new();
        voters.insert(self_site);
        HeartbeatAuthority {
            term_: term,
            config_size_: config_size,
            voters_: voters,
            outstanding_: rusty::BTreeSet::new(),
        }
    }

    pub fn term(&self) -> u64 {
        self.term_
    }

    pub fn config_size(&self) -> usize {
        self.config_size_
    }

    pub fn voter_count(&self) -> usize {
        self.voters_.len()
    }

    // One physical RPC exists per follower per generation, but these stay sets
    // so a future transport cannot double-count a voter.
    pub fn launch(&mut self, site: u16) {
        self.outstanding_.insert(site);
    }

    pub fn retire(&mut self, site: u16) {
        self.outstanding_.remove(&site);
    }

    pub fn record_vote(&mut self, site: u16) {
        self.voters_.insert(site);
    }

    // Every RPC launched in this generation has completed. No later event can
    // add evidence to it.
    pub fn all_completed(&self) -> bool {
        self.outstanding_.is_empty()
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.heartbeat_authority version=1 rust_sha256=24f71afc2d3fa77b204063d16cfc93163fc92971106b3417afb36611f91d44e8*/
struct HeartbeatAuthority;

struct HeartbeatAuthority {
    uint64_t term_;
    size_t config_size_;
    rusty::BTreeSet<uint16_t> voters_;
    rusty::BTreeSet<uint16_t> outstanding_;

    static HeartbeatAuthority new_(uint64_t term, size_t config_size, uint16_t self_site);
    uint64_t term() const;
    size_t config_size() const;
    size_t voter_count() const;
    void launch(uint16_t site);
    void retire(uint16_t site);
    void record_vote(uint16_t site);
    bool all_completed() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};


inline HeartbeatAuthority HeartbeatAuthority::new_(uint64_t term, size_t config_size, uint16_t self_site) {
    auto voters = rusty::BTreeSet<uint16_t>::new_();
    voters.insert(std::move(self_site));
    return HeartbeatAuthority{.term_ = std::move(term), .config_size_ = std::move(config_size), .voters_ = std::move(voters), .outstanding_ = rusty::BTreeSet<uint16_t>::new_()};
}

inline uint64_t HeartbeatAuthority::term() const {
    return this->term_;
}

inline size_t HeartbeatAuthority::config_size() const {
    return this->config_size_;
}

inline size_t HeartbeatAuthority::voter_count() const {
    return rusty::len(this->voters_);
}

inline void HeartbeatAuthority::launch(uint16_t site) {
    this->outstanding_.insert(std::move(site));
}

inline void HeartbeatAuthority::retire(uint16_t site) {
    this->outstanding_.remove(site);
}

inline void HeartbeatAuthority::record_vote(uint16_t site) {
    this->voters_.insert(std::move(site));
}

inline bool HeartbeatAuthority::all_completed() const {
    return rusty::is_empty(this->outstanding_);
}
/*RUSTYCPP:GEN-END id=raft_server.heartbeat_authority*/

}  // namespace janus

// Cross-carrier DSL calls, inline-mode shims.
//
// A DSL body that says `use crate::quorum_hpp::raft_quorum_majority_count`
// emits `using ::quorum_hpp::raft_quorum_majority_count` -- the emitter turns
// the crate module path into a C++ namespace path, and inline mode has no
// type map to rewrite it. These two namespaces supply the names it reaches
// for, exactly as rust_facade_types.h supplies the rusty:: reactor names.
// Aliases only; the definitions stay where they are.
namespace quorum_hpp {
using janus::raft::raft_quorum_majority_count;
using janus::raft::raft_quorum_count_reached;
}  // namespace quorum_hpp

namespace server_h {
using janus::BackoffKind;
using janus::RAFT_SERVER_INVALID_SITE_ID;
using janus::raft_server_read_index_reply_confirms_authority;
using janus::raft_server_read_index_round_can_advance;
using janus::raft_server_log_index_above;
using janus::raft_server_log_entry_is_current_term;
using janus::raft_server_observed_higher_term;
using janus::raft_server_append_acknowledged_through;
using janus::raft_server_log_index_has_successor;
using janus::raft_server_follower_next_index;
// Types cross a carrier boundary exactly as free functions do: the emitter
// writes the name unqualified (both blocks are in namespace janus) and the
// `use crate::server_h::X` on the Rust side emits as this alias.
using janus::RaftConsensusState;
using janus::PeerTable;
using janus::RaftLog;
}  // namespace server_h

namespace janus {

// PHASE 2 binds one of these per slot per poll pass so the loop body keeps the
// field spellings it had when `pending` was a map value. It borrows: the
// carried Command reference is owned by the table's slot, which outlives the
// pass because only this loop releases slots and it does so after its last use.
struct PendingView {
  siteid_t follower_id;
  uint64_t sent_term;
  uint64_t sent_round;
  uint64_t sent_end_index;
  const janus::Command& cmd;
};

// The read-index authority ledger, owned by Rust.
//
// Was std::map<uint64_t, PendingHeartbeatAuthority>. A map bought nothing: the
// generations are few (bounded by rounds with replies still outstanding), they
// are created in ascending round order and scanned in that order, and every
// lookup was by a round id the caller already had. So it is a rusty::Vec with
// the round id as a field -- the same substitution PeerTable made, and for the
// same reason: rusty::BTreeMap's rustc model is not a faithful map.
//
// The membership snapshot is now a rusty::BTreeSet<u16> rather than a
// std::set, which is what lets the whole type be DSL. Its comparison against
// the current membership keeps FULL strength -- it is set equality, not a size
// check -- but it is expressed as length plus containment over a sorted slice
// rather than with `==`. That is deliberate: the rustc facade models BTreeSet
// as a Vec (rusty-rustc/src/lib.rs:817), so a derived `==` there would be
// ORDER-sensitive while the real C++ btree_port `==` is set equality. Using
// only len() and contains(), which are faithful on both sides, keeps the gate
// checking what production does. Same reason the config is admitted member by
// member instead of cloned: the facade's BTreeSet implements neither Clone nor
// PartialEq, and adding them would be adding unfaithful ones.
#if RUSTYCPP_RUST
use crate::quorum_hpp::raft_quorum_majority_count;
use crate::quorum_hpp::raft_quorum_count_reached;
use crate::server_h::raft_server_read_index_reply_confirms_authority;
use crate::server_h::raft_server_read_index_round_can_advance;
use crate::server_h::raft_server_log_index_above;
use crate::server_h::raft_server_log_entry_is_current_term;
use crate::server_h::raft_server_observed_higher_term;
use crate::server_h::raft_server_append_acknowledged_through;
use crate::server_h::raft_server_log_index_has_successor;
use crate::server_h::raft_server_follower_next_index;
use crate::server_h::BackoffKind;
use crate::server_h::RAFT_SERVER_INVALID_SITE_ID;
use crate::server_h::RaftConsensusState;
use crate::server_h::PeerTable;
use crate::server_h::RaftLog;

pub struct AuthorityGeneration {
    round_id_: u64,
    config_: rusty::BTreeSet<u16>,
    evidence_: HeartbeatAuthority,
}

impl AuthorityGeneration {
    pub fn round_id(&self) -> u64 {
        self.round_id_
    }

    pub fn term(&self) -> u64 {
        self.evidence_.term()
    }

    pub fn voter_count(&self) -> usize {
        self.evidence_.voter_count()
    }

    pub fn config_size(&self) -> usize {
        self.evidence_.config_size()
    }

    // Quorum is asked of the generation's OWN config size, not the current
    // one: a delayed reply is evidence against the membership that launched
    // it.
    pub fn has_quorum(&self) -> bool {
        let quorum = raft_quorum_majority_count(self.evidence_.config_size());
        raft_quorum_count_reached(self.evidence_.voter_count(), quorum)
    }

    pub fn all_completed(&self) -> bool {
        self.evidence_.all_completed()
    }

    // Set equality against the launching membership, spelled with len() and
    // contains() so it means the same thing under the Vec-backed rustc model
    // as under the real C++ btree. `sites` is the current config, sorted and
    // duplicate-free, which is what a std::set iteration yields.
    pub fn config_matches(&self, sites: &[u16]) -> bool {
        if self.config_.len() != sites.len() {
            return false;
        }
        let mut i: usize = 0;
        while i < sites.len() {
            if !self.config_.contains(&sites[i]) {
                return false;
            }
            i += 1;
        }
        true
    }
}

// The context one reply carries. Grouped into a value rather than passed as
// seven parameters, which clippy rejects and which reads worse at the call
// site: PHASE 2 is describing one event, not supplying seven unrelated
// arguments.
#[repr(C)]
pub struct AuthorityReply {
    sent_round_: u64,
    follower_: u16,
    sent_term_: u64,
    response_term_: u64,
    current_term_: u64,
    is_leader_: bool,
    response_available_: bool,
}

impl AuthorityReply {
    pub fn new(sent_round: u64, follower: u16, sent_term: u64,
               response_term: u64, current_term: u64, is_leader: bool,
               response_available: bool) -> AuthorityReply {
        AuthorityReply {
            sent_round_: sent_round,
            follower_: follower,
            sent_term_: sent_term,
            response_term_: response_term,
            current_term_: current_term,
            is_leader_: is_leader,
            response_available_: response_available,
        }
    }

    pub fn sent_round(&self) -> u64 { self.sent_round_ }
    pub fn follower(&self) -> u16 { self.follower_ }
    pub fn sent_term(&self) -> u64 { self.sent_term_ }
    pub fn response_term(&self) -> u64 { self.response_term_ }
    pub fn current_term(&self) -> u64 { self.current_term_ }
    pub fn is_leader(&self) -> bool { self.is_leader_ }
    pub fn response_available(&self) -> bool { self.response_available_ }
}

// The outcome of one settlement pass: at most one generation is published.
#[repr(C)]
pub struct AuthorityOutcome {
    confirmed_: bool,
    term_: u64,
    round_id_: u64,
    voter_count_: usize,
    config_size_: usize,
}

impl AuthorityOutcome {
    pub fn confirmed(&self) -> bool { self.confirmed_ }
    pub fn term(&self) -> u64 { self.term_ }
    pub fn round_id(&self) -> u64 { self.round_id_ }
    pub fn voter_count(&self) -> usize { self.voter_count_ }
    pub fn config_size(&self) -> usize { self.config_size_ }
}

pub struct AuthorityLedger {
    generations_: rusty::Vec<AuthorityGeneration>,
}

#[allow(clippy::new_without_default)]
impl AuthorityLedger {
    pub fn new() -> AuthorityLedger {
        AuthorityLedger { generations_: rusty::Vec::new() }
    }

    // Dropped wholesale on leadership loss or a term change, so a prior
    // epoch's evidence can never be counted against the new one.
    pub fn abandon(&mut self) {
        self.generations_.clear();
    }

    // Opens a generation over the membership that launched it. Returns false
    // if this round id is already present, which can only be the deliberately
    // fail-closed UINT64_MAX saturation generation; the caller asserts that.
    pub fn open(&mut self, round_id: u64, config: &[u16],
                evidence: HeartbeatAuthority) -> bool {
        if self.index_of(round_id) < self.generations_.len() {
            return false;
        }
        let mut snapshot = rusty::BTreeSet::new();
        let mut i: usize = 0;
        while i < config.len() {
            snapshot.insert(config[i]);
            i += 1;
        }
        self.generations_.push(AuthorityGeneration {
            round_id_: round_id,
            config_: snapshot,
            evidence_: evidence,
        });
        true
    }

    // Returns generations_.len() when absent. An index, never a reference, so
    // nothing can dangle across an RPC send or a re-entrant callback.
    pub fn index_of(&self, round_id: u64) -> usize {
        let n = self.generations_.len();
        let mut i: usize = 0;
        while i < n {
            if self.generations_[i].round_id_ == round_id {
                return i;
            }
            i += 1;
        }
        n
    }

    pub fn len(&self) -> usize {
        self.generations_.len()
    }

    pub fn is_empty(&self) -> bool {
        self.generations_.is_empty()
    }

    pub fn launch(&mut self, round_id: u64, site: u16) -> bool {
        let index = self.index_of(round_id);
        if index >= self.generations_.len() {
            return false;
        }
        self.generations_[index].evidence_.launch(site);
        true
    }

    pub fn has_quorum(&self, round_id: u64) -> bool {
        let index = self.index_of(round_id);
        if index >= self.generations_.len() {
            return false;
        }
        self.generations_[index].has_quorum()
    }

    // One reply arrives. The RPC is retired unconditionally, and counted as a
    // vote only if it proves this exact generation: same term, a follower that
    // was in the launching membership, and the reply predicate agreeing.
    pub fn record_reply(&mut self, reply: &AuthorityReply) {
        let index = self.index_of(reply.sent_round());
        if index >= self.generations_.len() {
            return;
        }
        self.generations_[index].evidence_.retire(reply.follower());
        let matches_term =
            self.generations_[index].evidence_.term() == reply.sent_term();
        let was_member =
            self.generations_[index].config_.contains(&reply.follower());
        if matches_term && was_member &&
            raft_server_read_index_reply_confirms_authority(
                reply.response_available(), reply.is_leader(),
                reply.sent_term(), reply.response_term(),
                reply.current_term(), reply.sent_round(),
                self.generations_[index].round_id_) {
            self.generations_[index].evidence_.record_vote(reply.follower());
        }
    }

    // Publishes at most one generation and retires every generation that can
    // no longer contribute. Generations are held in ascending round order, and
    // the running confirmation is consulted as it advances, so the highest
    // round reaching quorum wins -- the same outcome the ascending std::map
    // scan produced.
    pub fn settle(&mut self, is_leader: bool, current_term: u64,
                  current_config: &[u16],
                  confirmed_term: u64, confirmed_round: u64)
                  -> AuthorityOutcome {
        let mut outcome = AuthorityOutcome {
            confirmed_: false,
            term_: 0,
            round_id_: 0,
            voter_count_: 0,
            config_size_: 0,
        };
        let mut running_term = confirmed_term;
        let mut running_round = confirmed_round;
        let mut i: usize = 0;
        while i < self.generations_.len() {
            let context_is_current = is_leader &&
                current_term == self.generations_[i].evidence_.term() &&
                self.generations_[i].config_matches(current_config);
            let already_published = running_term ==
                self.generations_[i].evidence_.term() &&
                self.generations_[i].round_id_ <= running_round;
            if !context_is_current || already_published {
                self.generations_.remove(i);
                continue;
            }
            if self.generations_[i].has_quorum() {
                running_term = self.generations_[i].evidence_.term();
                running_round = self.generations_[i].round_id_;
                outcome = AuthorityOutcome {
                    confirmed_: true,
                    term_: running_term,
                    round_id_: running_round,
                    voter_count_: self.generations_[i].evidence_.voter_count(),
                    config_size_: self.generations_[i].evidence_.config_size(),
                };
                self.generations_.remove(i);
                continue;
            }
            if self.generations_[i].all_completed() {
                // Every RPC launched in this generation completed without a
                // quorum. No later event can add evidence to it.
                self.generations_.remove(i);
                continue;
            }
            i += 1;
        }
        outcome
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.authority_ledger version=1 rust_sha256=d56da0ac5eb62c9bf755ab6374c2a96c64349c109ca130b923e0772203f8d372*/
struct AuthorityGeneration;
struct AuthorityReply;
struct AuthorityOutcome;
struct AuthorityLedger;

using ::quorum_hpp::raft_quorum_majority_count;

using ::quorum_hpp::raft_quorum_count_reached;

using ::server_h::raft_server_read_index_reply_confirms_authority;

using ::server_h::raft_server_read_index_round_can_advance;

using ::server_h::raft_server_log_index_above;

using ::server_h::raft_server_log_entry_is_current_term;

using ::server_h::raft_server_observed_higher_term;

using ::server_h::raft_server_append_acknowledged_through;

using ::server_h::raft_server_log_index_has_successor;

using ::server_h::raft_server_follower_next_index;

using ::server_h::BackoffKind;

using ::server_h::RAFT_SERVER_INVALID_SITE_ID;

using ::server_h::RaftConsensusState;

using ::server_h::PeerTable;

using ::server_h::RaftLog;

struct AuthorityGeneration {
    uint64_t round_id_;
    rusty::BTreeSet<uint16_t> config_;
    HeartbeatAuthority evidence_;

    uint64_t round_id() const;
    uint64_t term() const;
    size_t voter_count() const;
    size_t config_size() const;
    bool has_quorum() const;
    bool all_completed() const;
    bool config_matches(std::span<const uint16_t> sites) const;
};

struct AuthorityReply {
    uint64_t sent_round_;
    uint16_t follower_;
    uint64_t sent_term_;
    uint64_t response_term_;
    uint64_t current_term_;
    bool is_leader_;
    bool response_available_;

    static AuthorityReply new_(uint64_t sent_round, uint16_t follower, uint64_t sent_term, uint64_t response_term, uint64_t current_term, bool is_leader, bool response_available);
    uint64_t sent_round() const;
    uint16_t follower() const;
    uint64_t sent_term() const;
    uint64_t response_term() const;
    uint64_t current_term() const;
    bool is_leader() const;
    bool response_available() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct AuthorityOutcome {
    bool confirmed_;
    uint64_t term_;
    uint64_t round_id_;
    size_t voter_count_;
    size_t config_size_;

    bool confirmed() const;
    uint64_t term() const;
    uint64_t round_id() const;
    size_t voter_count() const;
    size_t config_size() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct AuthorityLedger {
    rusty::Vec<AuthorityGeneration> generations_;

    static AuthorityLedger new_();
    void abandon();
    bool open(uint64_t round_id, std::span<const uint16_t> config, HeartbeatAuthority evidence);
    size_t index_of(uint64_t round_id) const;
    size_t len() const;
    bool is_empty() const;
    bool launch(uint64_t round_id, uint16_t site);
    bool has_quorum(uint64_t round_id) const;
    void record_reply(const AuthorityReply& reply);
    AuthorityOutcome settle(bool is_leader, uint64_t current_term, std::span<const uint16_t> current_config, uint64_t confirmed_term, uint64_t confirmed_round);
};


inline uint64_t AuthorityGeneration::round_id() const {
    return this->round_id_;
}

inline uint64_t AuthorityGeneration::term() const {
    return this->evidence_.term();
}

inline size_t AuthorityGeneration::voter_count() const {
    return this->evidence_.voter_count();
}

inline size_t AuthorityGeneration::config_size() const {
    return this->evidence_.config_size();
}

inline bool AuthorityGeneration::has_quorum() const {
    const auto quorum = raft_quorum_majority_count(this->evidence_.config_size());
    return raft_quorum_count_reached(this->evidence_.voter_count(), std::move(quorum));
}

inline bool AuthorityGeneration::all_completed() const {
    return this->evidence_.all_completed();
}

inline bool AuthorityGeneration::config_matches(std::span<const uint16_t> sites) const {
    if (rusty::len(this->config_) != rusty::len(sites)) {
        return false;
    }
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(sites)) {
        if (rusty::detail::rust_not(rusty::contains(this->config_, &sites[i]))) {
            return false;
        }
        i += 1;
    }
    return true;
}

inline AuthorityReply AuthorityReply::new_(uint64_t sent_round, uint16_t follower, uint64_t sent_term, uint64_t response_term, uint64_t current_term, bool is_leader, bool response_available) {
    return AuthorityReply{.sent_round_ = std::move(sent_round), .follower_ = std::move(follower), .sent_term_ = std::move(sent_term), .response_term_ = std::move(response_term), .current_term_ = std::move(current_term), .is_leader_ = std::move(is_leader), .response_available_ = std::move(response_available)};
}

inline uint64_t AuthorityReply::sent_round() const {
    return this->sent_round_;
}

inline uint16_t AuthorityReply::follower() const {
    return this->follower_;
}

inline uint64_t AuthorityReply::sent_term() const {
    return this->sent_term_;
}

inline uint64_t AuthorityReply::response_term() const {
    return this->response_term_;
}

inline uint64_t AuthorityReply::current_term() const {
    return this->current_term_;
}

inline bool AuthorityReply::is_leader() const {
    return this->is_leader_;
}

inline bool AuthorityReply::response_available() const {
    return this->response_available_;
}

inline bool AuthorityOutcome::confirmed() const {
    return this->confirmed_;
}

inline uint64_t AuthorityOutcome::term() const {
    return this->term_;
}

inline uint64_t AuthorityOutcome::round_id() const {
    return this->round_id_;
}

inline size_t AuthorityOutcome::voter_count() const {
    return this->voter_count_;
}

inline size_t AuthorityOutcome::config_size() const {
    return this->config_size_;
}

inline AuthorityLedger AuthorityLedger::new_() {
    return AuthorityLedger{.generations_ = rusty::Vec<AuthorityGeneration>::new_()};
}

inline void AuthorityLedger::abandon() {
    this->generations_.clear();
}

inline bool AuthorityLedger::open(uint64_t round_id, std::span<const uint16_t> config, HeartbeatAuthority evidence) {
    if (this->index_of(std::move(round_id)) < rusty::len(this->generations_)) {
        return false;
    }
    auto snapshot = rusty::BTreeSet<uint16_t>::new_();
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(config)) {
        snapshot.insert(config[i]);
        i += 1;
    }
    this->generations_.push(AuthorityGeneration{.round_id_ = std::move(round_id), .config_ = std::move(snapshot), .evidence_ = std::move(evidence)});
    return true;
}

inline size_t AuthorityLedger::index_of(uint64_t round_id) const {
    auto n = rusty::len(this->generations_);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(n)) {
        if (rusty::detail::deref_if_pointer_like(this->generations_[i].round_id_) == rusty::detail::deref_if_pointer_like(round_id)) {
            return std::move(i);
        }
        i += 1;
    }
    return std::move(n);
}

inline size_t AuthorityLedger::len() const {
    return rusty::len(this->generations_);
}

inline bool AuthorityLedger::is_empty() const {
    return rusty::is_empty(this->generations_);
}

inline bool AuthorityLedger::launch(uint64_t round_id, uint16_t site) {
    const auto index = this->index_of(std::move(round_id));
    if (rusty::detail::deref_if_pointer_like(index) >= rusty::len(this->generations_)) {
        return false;
    }
    this->generations_[index].evidence_.launch(std::move(site));
    return true;
}

inline bool AuthorityLedger::has_quorum(uint64_t round_id) const {
    const auto index = this->index_of(std::move(round_id));
    if (rusty::detail::deref_if_pointer_like(index) >= rusty::len(this->generations_)) {
        return false;
    }
    return this->generations_[index].has_quorum();
}

inline void AuthorityLedger::record_reply(const AuthorityReply& reply) {
    const auto index = this->index_of(reply.sent_round());
    if (rusty::detail::deref_if_pointer_like(index) >= rusty::len(this->generations_)) {
        return;
    }
    this->generations_[index].evidence_.retire(reply.follower());
    const auto matches_term = this->generations_[index].evidence_.term() == reply.sent_term();
    const auto was_member = rusty::contains(this->generations_[index].config_, rusty::addr_of_temp(reply.follower()));
    if ((rusty::detail::deref_if_pointer_like(matches_term) && rusty::detail::deref_if_pointer_like(was_member)) && raft_server_read_index_reply_confirms_authority(reply.response_available(), reply.is_leader(), reply.sent_term(), reply.response_term(), reply.current_term(), reply.sent_round(), this->generations_[index].round_id_)) {
        this->generations_[index].evidence_.record_vote(reply.follower());
    }
}

inline AuthorityOutcome AuthorityLedger::settle(bool is_leader, uint64_t current_term, std::span<const uint16_t> current_config, uint64_t confirmed_term, uint64_t confirmed_round) {
    auto outcome = AuthorityOutcome{.confirmed_ = false, .term_ = static_cast<uint64_t>(0), .round_id_ = static_cast<uint64_t>(0), .voter_count_ = static_cast<size_t>(0), .config_size_ = static_cast<size_t>(0)};
    auto running_term = std::move(confirmed_term);
    auto running_round = std::move(confirmed_round);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(this->generations_)) {
        const auto context_is_current = (rusty::detail::deref_if_pointer_like(is_leader) && (rusty::detail::deref_if_pointer_like(current_term) == this->generations_[i].evidence_.term())) && this->generations_[i].config_matches(current_config);
        const auto already_published = (rusty::detail::deref_if_pointer_like(running_term) == this->generations_[i].evidence_.term()) && (rusty::detail::deref_if_pointer_like(this->generations_[i].round_id_) <= rusty::detail::deref_if_pointer_like(running_round));
        if (rusty::detail::rust_not(context_is_current) || rusty::detail::deref_if_pointer_like(already_published)) {
            this->generations_.remove(std::move(i));
            continue;
        }
        if (this->generations_[i].has_quorum()) {
            running_term = this->generations_[i].evidence_.term();
            running_round = this->generations_[i].round_id_;
            outcome = AuthorityOutcome{.confirmed_ = true, .term_ = std::move(running_term), .round_id_ = std::move(running_round), .voter_count_ = this->generations_[i].evidence_.voter_count(), .config_size_ = this->generations_[i].evidence_.config_size()};
            this->generations_.remove(std::move(i));
            continue;
        }
        if (this->generations_[i].all_completed()) {
            this->generations_.remove(std::move(i));
            continue;
        }
        i += 1;
    }
    return std::move(outcome);
}
/*RUSTYCPP:GEN-END id=raft_server.authority_ledger*/

// @unsafe - Heartbeat loop mutates shared state, performs RPCs, and uses raw pointers.
// ============================================================================
// HEARTBEAT LOOP: the C++ half of the DSL-owned HeartbeatDriver
//
// The loop and the lifecycle are Rust, in the raft_server.heartbeat_driver
// block in server.h. What stays here is the round body -- moved verbatim, not
// rewritten -- plus the prologue and epilogue. Splitting the round into its
// four phases is the next tranche; doing it in the same change as the loop
// extraction would have put the phase boundaries and the loop boundary at
// risk together, and the phases carry twenty inner break/continue statements
// whose meaning depends on exactly which loop encloses them.
// ============================================================================

// The values that outlive a phase but not a round. PHASE 0 establishes all of
// them; PHASE 1, 2 and 3 read them. They were stack locals while the round was
// one function, and naming them is what a phase split costs.
// The round scope, owned by Rust.
//
// Every read and write of this state now goes through a method: the C++ phases
// cannot poke a field. That matters more here than it did for the loops,
// because this is the first Raft type whose `&mut self` is a TRUE statement
// rather than one the model cannot back. HeartbeatRoundState is reachable only
// from the heartbeat fiber -- SendAppendEntries2's completion callback
// captures [response, site_id] and nothing else (commo.cc:50), the
// InstallSnapshot callback aliases RaftServer rather than the round, and
// PHASE 2's pending_rpcs iterator closes before the only suspension point --
// so exclusive mutable access is genuinely exclusive, and a borrow check over
// it is checking something real.
//
// nservers is GONE as a field. It was only ever assigned
// round_config.size(), so it is now derived by nservers(), which removes the
// possibility of the two disagreeing.
#if RUSTYCPP_RUST
pub struct HeartbeatRoundScope {
    term_: u64,
    round_id_: u64,
    config_: rusty::BTreeSet<u16>,
    current_commit_index_: u64,
    authority_inserted_: bool,
}

#[allow(clippy::new_without_default)]
impl HeartbeatRoundScope {
    pub fn new() -> HeartbeatRoundScope {
        HeartbeatRoundScope {
            term_: 0,
            round_id_: 0,
            config_: rusty::BTreeSet::new(),
            current_commit_index_: 0,
            authority_inserted_: false,
        }
    }

    // Opens a round. Term, generation and membership are latched together so
    // no later phase can observe a half-established scope, and the previous
    // round's membership is dropped rather than accumulated.
    pub fn begin(&mut self, term: u64, round_id: u64) {
        self.term_ = term;
        self.round_id_ = round_id;
        self.config_.clear();
        self.current_commit_index_ = 0;
        self.authority_inserted_ = false;
    }

    pub fn admit(&mut self, site: u16) {
        self.config_.insert(site);
    }

    pub fn term(&self) -> u64 {
        self.term_
    }

    pub fn round_id(&self) -> u64 {
        self.round_id_
    }

    // The replica count this round was launched against, membership snapshot
    // included, which is what every quorum decision divides by.
    pub fn nservers(&self) -> usize {
        self.config_.len()
    }

    pub fn is_member(&self, site: u16) -> bool {
        self.config_.contains(&site)
    }

    // The commit index the round puts on the wire. Published by PHASE 0 after
    // it recalculates, read by PHASE 1 when it builds each AppendEntries.
    pub fn publish_commit_index(&mut self, index: u64) {
        self.current_commit_index_ = index;
    }

    pub fn commit_index(&self) -> u64 {
        self.current_commit_index_
    }

    // Whether this round owns a fresh authority generation. False only in the
    // deliberately fail-closed UINT64_MAX saturation case, where PHASE 1 must
    // not record evidence against a reused generation.
    pub fn set_authority_inserted(&mut self, inserted: bool) {
        self.authority_inserted_ = inserted;
    }

    pub fn authority_inserted(&self) -> bool {
        self.authority_inserted_
    }
}

// PHASE 0 of the heartbeat round, which is the whole locked section of
// RaftServer::HeartbeatPhase0.
//
// Everything it decides is now Rust: whether the round runs at all, whether
// the leader epoch changed under the fiber, whether the read-index round may
// advance, which members the round admits, and whether the majority-matched
// index may be committed. Every piece of state it touches was already a DSL
// type -- RaftConsensusState, PeerTable, RaftLog, HeartbeatRoundScope,
// PendingTable, AuthorityLedger -- which is what made the phase convertible
// at all; this is the first body to be assembled out of them rather than
// alongside them.
//
// Three things stay in C++ on purpose, and they are the only three:
//   - taking mtx_, because a std::mutex has no Rust spelling here;
//   - EnqueueCommittedEntries, which is the apply queue, i.e. I/O. It is
//     driven by the range this returns, so the DECISION to commit is Rust
//     and only the hand-off is not;
//   - the Log_debug calls, which must keep their level short-circuit. A DSL
//     body logs through log_line, which evaluates unconditionally, and this
//     runs once per follower per round.
//
// Cross-carrier note: RaftConsensusState, PeerTable and RaftLog live in
// server.h. Referencing a TYPE across carriers works exactly as referencing
// a free function does -- `use crate::server_h::X` on the Rust side, and the
// emitter writes the name unqualified, which resolves because both blocks
// sit in namespace janus. No shim namespace is needed for types.
// PHASE 2's decision core: what one AppendEntries reply means.
//
// PHASE 2 is a polling loop over the in-flight slots. The loop itself, its
// round deadline and its Fiber::sleep stay in C++ -- suspension is the one
// thing genuinely shaped by the fiber runtime. What each reply MEANS is not,
// and that is this.
//
// The wire reply is read out by the caller and arrives here as three scalars.
// That is the same "convert at the edge" split the rest of the file uses: the
// rrr response object never crosses, only what it says.
//
// The caller keeps four things because none of them are decisions:
// LogTermChange and the backoff-rung logging (both pure logging, and both
// need their level short-circuit), PeerOrdinal (a scan over peer_sites_,
// which is C++), and stepDown -- which reaches setIsLeader and the election
// timer, i.e. the reactor. This returns STEP_DOWN and lets the caller do it.
#[repr(C)]
pub struct SentAppend {
    follower_: u16,
    term_: u64,
    round_: u64,
    end_index_: u64,
    // peers.len() when this follower is no longer a peer at all.
    ordinal_: usize,
}

impl SentAppend {
    pub fn new(follower: u16, term: u64, round: u64, end_index: u64, ordinal: usize) -> SentAppend {
        SentAppend { follower_: follower, term_: term, round_: round,
                     end_index_: end_index, ordinal_: ordinal }
    }

    pub fn round(&self) -> u64 {
        self.round_
    }
}

#[repr(C)]
pub struct AppendReply {
    available_: bool,
    status_: bool,
    term_: u64,
    last_log_index_: u64,
}

impl AppendReply {
    pub fn new(available: bool, status: bool, term: u64, last_log_index: u64) -> AppendReply {
        AppendReply { available_: available, status_: status, term_: term,
                      last_log_index_: last_log_index }
    }
}

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum AppendReplyAction {
    // Nothing was learned: the RPC failed, or the reply belongs to a term or
    // a leadership epoch that is no longer current.
    IGNORED = 0,
    // The follower proved a newer term. Term, vote and leader hint have been
    // updated here; the caller must perform the step-down itself.
    STEP_DOWN = 1,
    // Rejected. The backoff ladder ran and reports which rung it took.
    BACKED_OFF = 2,
    // Accepted, and replication progress advanced.
    ACCEPTED = 3,
    // Success that does not cover the payload it was sent. AppendEntries
    // acceptance is atomic, so this proves nothing and must not be counted.
    CONTRADICTORY = 4,
    // The follower has no replication indices: it is not one this server
    // leads. Higher-term evidence was already handled above.
    UNKNOWN_FOLLOWER = 5,
}

#[repr(C)]
pub struct AppendReplyOutcome {
    action_: AppendReplyAction,
    rung_: BackoffKind,
    old_next_: u64,
    new_next_: u64,
    acknowledged_: u64,
    previous_term_: u64,
}

impl AppendReplyOutcome {
    pub fn action(&self) -> AppendReplyAction {
        self.action_
    }

    // BACKED_OFF only.
    pub fn rung(&self) -> BackoffKind {
        self.rung_
    }

    pub fn old_next(&self) -> u64 {
        self.old_next_
    }

    pub fn new_next(&self) -> u64 {
        self.new_next_
    }

    // ACCEPTED only.
    pub fn acknowledged(&self) -> u64 {
        self.acknowledged_
    }

    // STEP_DOWN only: the term this server held before the reply displaced it.
    pub fn previous_term(&self) -> u64 {
        self.previous_term_
    }
}

fn append_reply_nothing(action: AppendReplyAction) -> AppendReplyOutcome {
    AppendReplyOutcome {
        action_: action,
        rung_: BackoffKind::FLOOR,
        old_next_: 0,
        new_next_: 0,
        acknowledged_: 0,
        previous_term_: 0,
    }
}

// Takes the log's tail rather than the log: this decides what a reply
// proves, and the only thing it needs from the log is where the log ends.
// Narrowing the parameter is also what keeps the argument list inside
// clippy's limit without an allow.
pub fn heartbeat_apply_append_reply(
    consensus: &mut RaftConsensusState,
    peers: &mut PeerTable,
    ledger: &mut AuthorityLedger,
    sent: &SentAppend,
    reply: &AppendReply,
    log_last_index: u64,
    is_leader: bool,
) -> AppendReplyOutcome {
    // Retire the RPC and, if it proves this exact generation, count the vote.
    // One physical RPC exists per follower per generation, but the evidence
    // stays a set so a future transport still cannot double-count a voter.
    let evidence = AuthorityReply::new(
        sent.round_,
        sent.follower_,
        sent.term_,
        reply.term_,
        consensus.current_term_,
        is_leader,
        reply.available_,
    );
    ledger.record_reply(&evidence);

    if !reply.available_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }

    // A higher term is authoritative regardless of the accompanying status
    // bit. The responding follower proves a newer term, not its leader.
    if raft_server_observed_higher_term(reply.term_, consensus.current_term_) {
        let previous_term = consensus.current_term_;
        consensus.current_term_ = reply.term_;
        consensus.vote_for_ = u16::MAX;
        // Neither leading nor knowing a leader, so the hint is cleared. The
        // responding follower proved a newer term, not that it is the leader
        // of that term. (With both flags false the shared predicate returns
        // the invalid id whatever ids it is handed, so it is spelled out
        // here rather than called with two arguments that do not matter.)
        consensus.current_leader_id_ = RAFT_SERVER_INVALID_SITE_ID;
        let mut out = append_reply_nothing(AppendReplyAction::STEP_DOWN);
        out.previous_term_ = previous_term;
        return out;
    }

    // A reply from a send term this server has left proves nothing about now.
    if consensus.current_term_ != sent.term_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    // A valid follower processes AppendEntries in the leader's term before
    // replying, so a lower response term cannot prove this send.
    if reply.term_ != sent.term_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    if !is_leader {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    if sent.ordinal_ == peers.len() {
        return append_reply_nothing(AppendReplyAction::UNKNOWN_FOLLOWER);
    }

    if !reply.status_ {
        let old_next = peers.next_index(sent.ordinal_);
        let rung = peers.back_off_after_reject(sent.ordinal_, reply.last_log_index_);
        let new_next = peers.next_index(sent.ordinal_);
        let mut out = append_reply_nothing(AppendReplyAction::BACKED_OFF);
        out.rung_ = rung;
        out.old_next_ = old_next;
        out.new_next_ = new_next;
        return out;
    }

    if reply.last_log_index_ < sent.end_index_ {
        return append_reply_nothing(AppendReplyAction::CONTRADICTORY);
    }

    // Successful responses are monotonic and prove no index beyond the exact
    // payload end. In particular a heartbeat cannot adopt an unknown
    // follower suffix.
    let acknowledged = raft_server_append_acknowledged_through(
        reply.last_log_index_, sent.end_index_, log_last_index);
    peers.accept_through(
        sent.ordinal_,
        acknowledged,
        raft_server_log_index_has_successor(acknowledged),
        raft_server_follower_next_index(acknowledged),
    );
    let mut out = append_reply_nothing(AppendReplyAction::ACCEPTED);
    out.acknowledged_ = acknowledged;
    out
}

// The commit-index advance, which PHASE 0 and PHASE 3 perform identically:
// PHASE 0 before the round's RPCs go out, PHASE 3 after their replies have
// been processed. It was the same fifteen lines twice.
//
// Returns the range the caller must hand to EnqueueCommittedEntries. The
// caller does the enqueue because that is the apply queue, i.e. I/O; the
// decision is here.
#[repr(C)]
pub struct CommitAdvance {
    advanced_: bool,
    from_: u64,
    to_: u64,
}

impl CommitAdvance {
    pub fn advanced(&self) -> bool {
        self.advanced_
    }

    pub fn from_index(&self) -> u64 {
        self.from_
    }

    pub fn to_index(&self) -> u64 {
        self.to_
    }
}

pub fn raft_commit_advance(
    consensus: &mut RaftConsensusState,
    peers: &PeerTable,
    log: &RaftLog,
    nservers: usize,
) -> CommitAdvance {
    // nservers is the value latched in PHASE 0. Reusing it in PHASE 3 is
    // sound only because current_config_ has exactly one write, during
    // Setup, and progress_ is never erased, so the size is invariant across
    // the round. Assert it rather than trusting the phases to stay in step.
    if peers.len() != nservers - 1 {
        panic!("peer table and round membership disagree");
    }
    let candidate_index = peers.majority_match_index(nservers, log.last_index());
    if !raft_server_log_index_above(candidate_index, consensus.commit_index_) {
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    // The candidate is <= last_index() and > commit_index_, so the entry
    // provably exists. This says so rather than leaving a null dereference
    // to express it.
    let candidate = log.get(candidate_index);
    if candidate.is_none() {
        panic!("committable index is absent from the log");
    }
    if !raft_server_log_entry_is_current_term(
        candidate.unwrap().term(),
        consensus.current_term_,
    ) {
        // Raft commits a prior-term entry only via one from the current term.
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    let from = consensus.commit_index_;
    consensus.commit_index_ = candidate_index;
    CommitAdvance { advanced_: true, from_: from, to_: candidate_index }
}

// PHASE 3 of the heartbeat round: the whole locked section. Recomputes the
// commit index now that this round's replies have been processed, then
// publishes read-index authority -- deliberately in that order, because a
// delayed reply is evidence for the exact term, generation and membership
// snapshot that launched it and must never be relabelled as the current
// round.
//
// Both halves were already Rust in their parts: raft_commit_advance above
// and AuthorityLedger::settle. This is the body that joins them.
#[repr(C)]
pub struct Phase3Outcome {
    commit_: CommitAdvance,
    confirmed_: bool,
}

impl Phase3Outcome {
    pub fn commit(&self) -> &CommitAdvance {
        &self.commit_
    }

    // True when a read-index generation reached quorum this round, in which
    // case the confirmed term and round have already been stored.
    pub fn confirmed(&self) -> bool {
        self.confirmed_
    }
}

pub fn heartbeat_phase3_locked(
    consensus: &mut RaftConsensusState,
    peers: &PeerTable,
    log: &RaftLog,
    ledger: &mut AuthorityLedger,
    nservers: usize,
    members: &[u16],
    is_leader: bool,
) -> Phase3Outcome {
    let commit = raft_commit_advance(consensus, peers, log, nservers);
    let outcome = ledger.settle(
        is_leader,
        consensus.current_term_,
        members,
        consensus.read_quorum_confirmed_term_,
        consensus.read_quorum_confirmed_round_,
    );
    let mut confirmed = false;
    if outcome.confirmed() {
        consensus.read_quorum_confirmed_term_ = outcome.term();
        consensus.read_quorum_confirmed_round_ = outcome.round_id();
        confirmed = true;
    }
    Phase3Outcome { commit_: commit, confirmed_: confirmed }
}

#[repr(C)]
pub struct Phase0Outcome {
    restart_: bool,
    commit_advanced_: bool,
    commit_from_: u64,
    commit_to_: u64,
}

impl Phase0Outcome {
    // The round is over before it began -- this server is not the leader.
    // The caller returns, and the driver starts the next round.
    pub fn restart(&self) -> bool {
        self.restart_
    }

    pub fn commit_advanced(&self) -> bool {
        self.commit_advanced_
    }

    pub fn commit_from(&self) -> u64 {
        self.commit_from_
    }

    pub fn commit_to(&self) -> u64 {
        self.commit_to_
    }
}

// TODO(raft-server-struct): remove this allow once RaftServer is itself a DSL
// struct. The ten parameters are exactly the pieces of RaftServer's state that
// PHASE 0 touches; they are separate arguments only because the orphan-impl
// rule forbids `impl RaftServer`, so this cannot yet be a method taking
// &mut self. Grouping them into a carrier struct now would invent a type whose
// only purpose is to be dissolved by that change. Before removing the allow,
// verify the parameter list really has collapsed into self rather than being
// hidden behind a wrapper.
#[allow(clippy::too_many_arguments)]
pub fn heartbeat_phase0_locked(
    consensus: &mut RaftConsensusState,
    peers: &PeerTable,
    log: &RaftLog,
    round: &mut HeartbeatRoundScope,
    pending: &mut PendingTable,
    ledger: &mut AuthorityLedger,
    pending_leader_term: &mut rusty::Option<u64>,
    members: &[u16],
    site_id: u16,
    is_leader: bool,
) -> Phase0Outcome {
    if !is_leader {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term = rusty::None;
        return Phase0Outcome {
            restart_: true,
            commit_advanced_: false,
            commit_from_: 0,
            commit_to_: 0,
        };
    }

    round.begin(consensus.current_term_, consensus.heartbeat_round_);

    // Sized here rather than in the prologue because the round state is the
    // loop's, not the server's. Idempotent: resize only runs when the two
    // tables disagree, so in-flight slots survive every later round.
    if pending.len() != peers.len() {
        pending.resize(peers.len());
    }

    // Leadership may be lost and regained between two observations by this
    // fiber. Never let a prior term's physical RPC occupy a slot or collide
    // with the new leader epoch's round counter reset.
    let epoch_changed = pending_leader_term.is_none()
        || *pending_leader_term.as_ref().unwrap() != round.term();
    if epoch_changed {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term = rusty::Some(round.term());
    }

    if raft_server_read_index_round_can_advance(consensus.heartbeat_round_) {
        consensus.heartbeat_round_ += 1;
    }
    // Saturation is fail-closed for new reads: the round never wraps, so no
    // post-baseline proof can be forged from an old generation. The caller
    // reports it; see round_saturated below.

    let mut i = 0;
    while i < members.len() {
        round.admit(members[i]);
        i += 1;
    }
    if round.nservers() == 0 || !round.is_member(site_id) {
        panic!("heartbeat round admitted no quorum containing this site");
    }
    let advance = raft_commit_advance(consensus, peers, log, round.nservers());
    round.publish_commit_index(consensus.commit_index_);

    Phase0Outcome {
        restart_: false,
        commit_advanced_: advance.advanced(),
        commit_from_: advance.from_index(),
        commit_to_: advance.to_index(),
    }
}

// Whether PHASE 0 declined to advance the read-index generation because the
// counter is saturated. Split out so the caller can log it at ERROR without
// the DSL body paying for an unconditional log_line every round.
pub fn heartbeat_round_saturated(round_counter: u64) -> bool {
    !raft_server_read_index_round_can_advance(round_counter)
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.heartbeat_round_scope version=1 rust_sha256=5bbbdcce5e78a19d60edae8de6dd7eb0b37bd8a2215f5ea17d81e5341bab0212*/
enum class AppendReplyAction : int32_t;
constexpr AppendReplyAction AppendReplyAction_IGNORED();
constexpr AppendReplyAction AppendReplyAction_STEP_DOWN();
constexpr AppendReplyAction AppendReplyAction_BACKED_OFF();
constexpr AppendReplyAction AppendReplyAction_ACCEPTED();
constexpr AppendReplyAction AppendReplyAction_CONTRADICTORY();
constexpr AppendReplyAction AppendReplyAction_UNKNOWN_FOLLOWER();
struct HeartbeatRoundScope;
struct SentAppend;
struct AppendReply;
struct AppendReplyOutcome;
struct CommitAdvance;
struct Phase3Outcome;
struct Phase0Outcome;
AppendReplyOutcome append_reply_nothing(AppendReplyAction action);
bool heartbeat_round_saturated(uint64_t round_counter);

enum class AppendReplyAction : int32_t {
    IGNORED = 0,
    STEP_DOWN = 1,
    BACKED_OFF = 2,
    ACCEPTED = 3,
    CONTRADICTORY = 4,
    UNKNOWN_FOLLOWER = 5
};
inline constexpr AppendReplyAction AppendReplyAction_IGNORED() { return AppendReplyAction::IGNORED; }
inline constexpr AppendReplyAction AppendReplyAction_STEP_DOWN() { return AppendReplyAction::STEP_DOWN; }
inline constexpr AppendReplyAction AppendReplyAction_BACKED_OFF() { return AppendReplyAction::BACKED_OFF; }
inline constexpr AppendReplyAction AppendReplyAction_ACCEPTED() { return AppendReplyAction::ACCEPTED; }
inline constexpr AppendReplyAction AppendReplyAction_CONTRADICTORY() { return AppendReplyAction::CONTRADICTORY; }
inline constexpr AppendReplyAction AppendReplyAction_UNKNOWN_FOLLOWER() { return AppendReplyAction::UNKNOWN_FOLLOWER; }

struct HeartbeatRoundScope {
    uint64_t term_;
    uint64_t round_id_;
    rusty::BTreeSet<uint16_t> config_;
    uint64_t current_commit_index_;
    bool authority_inserted_;

    static HeartbeatRoundScope new_();
    void begin(uint64_t term, uint64_t round_id);
    void admit(uint16_t site);
    uint64_t term() const;
    uint64_t round_id() const;
    size_t nservers() const;
    bool is_member(uint16_t site) const;
    void publish_commit_index(uint64_t index);
    uint64_t commit_index() const;
    void set_authority_inserted(bool inserted);
    bool authority_inserted() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct SentAppend {
    uint16_t follower_;
    uint64_t term_;
    uint64_t round_;
    uint64_t end_index_;
    size_t ordinal_;

    static SentAppend new_(uint16_t follower, uint64_t term, uint64_t round, uint64_t end_index, size_t ordinal);
    uint64_t round() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct AppendReply {
    bool available_;
    bool status_;
    uint64_t term_;
    uint64_t last_log_index_;

    static AppendReply new_(bool available, bool status, uint64_t term, uint64_t last_log_index);
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct AppendReplyOutcome {
    AppendReplyAction action_;
    BackoffKind rung_;
    uint64_t old_next_;
    uint64_t new_next_;
    uint64_t acknowledged_;
    uint64_t previous_term_;

    AppendReplyAction action() const;
    BackoffKind rung() const;
    uint64_t old_next() const;
    uint64_t new_next() const;
    uint64_t acknowledged() const;
    uint64_t previous_term() const;
};

struct CommitAdvance {
    bool advanced_;
    uint64_t from_;
    uint64_t to_;

    bool advanced() const;
    uint64_t from_index() const;
    uint64_t to_index() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct Phase3Outcome {
    CommitAdvance commit_;
    bool confirmed_;

    const CommitAdvance& commit() const;
    bool confirmed() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct Phase0Outcome {
    bool restart_;
    bool commit_advanced_;
    uint64_t commit_from_;
    uint64_t commit_to_;

    bool restart() const;
    bool commit_advanced() const;
    uint64_t commit_from() const;
    uint64_t commit_to() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

AppendReplyOutcome append_reply_nothing(AppendReplyAction action) {
    return AppendReplyOutcome{.action_ = std::move(action), .rung_ = rusty::clone(rusty::clone(BackoffKind::FLOOR)), .old_next_ = static_cast<uint64_t>(0), .new_next_ = static_cast<uint64_t>(0), .acknowledged_ = static_cast<uint64_t>(0), .previous_term_ = static_cast<uint64_t>(0)};
}

AppendReplyOutcome heartbeat_apply_append_reply(RaftConsensusState& consensus, PeerTable& peers, AuthorityLedger& ledger, const SentAppend& sent, const AppendReply& reply, uint64_t log_last_index, bool is_leader) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    const auto evidence = AuthorityReply::new_(sent.round_, sent.follower_, sent.term_, reply.term_, (*consensus_shadow1).current_term_, std::move(is_leader), reply.available_);
    ledger.record_reply(evidence);
    if (!reply.available_) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_IGNORED())));
    }
    if (raft_server_observed_higher_term(reply.term_, (*consensus_shadow1).current_term_)) {
        auto previous_term = (*consensus_shadow1).current_term_;
        (*consensus_shadow1).current_term_ = reply.term_;
        (*consensus_shadow1).vote_for_ = std::numeric_limits<uint16_t>::max();
        (*consensus_shadow1).current_leader_id_ = RAFT_SERVER_INVALID_SITE_ID;
        auto out = append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_STEP_DOWN())));
        out.previous_term_ = std::move(previous_term);
        return std::move(out);
    }
    if (rusty::detail::deref_if_pointer_like((*consensus_shadow1).current_term_) != rusty::detail::deref_if_pointer_like(sent.term_)) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_IGNORED())));
    }
    if (rusty::detail::deref_if_pointer_like(reply.term_) != rusty::detail::deref_if_pointer_like(sent.term_)) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_IGNORED())));
    }
    if (!is_leader) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_IGNORED())));
    }
    if (rusty::detail::deref_if_pointer_like(sent.ordinal_) == rusty::len(peers)) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_UNKNOWN_FOLLOWER())));
    }
    if (!reply.status_) {
        auto old_next = peers.next_index(sent.ordinal_);
        auto rung = peers.back_off_after_reject(sent.ordinal_, reply.last_log_index_);
        auto new_next = peers.next_index(sent.ordinal_);
        auto out = append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_BACKED_OFF())));
        out.rung_ = std::move(rung);
        out.old_next_ = std::move(old_next);
        out.new_next_ = std::move(new_next);
        return std::move(out);
    }
    if (rusty::detail::deref_if_pointer_like(reply.last_log_index_) < rusty::detail::deref_if_pointer_like(sent.end_index_)) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_CONTRADICTORY())));
    }
    auto acknowledged = raft_server_append_acknowledged_through(reply.last_log_index_, sent.end_index_, std::move(log_last_index));
    peers.accept_through(sent.ordinal_, std::move(acknowledged), raft_server_log_index_has_successor(std::move(acknowledged)), raft_server_follower_next_index(std::move(acknowledged)));
    auto out = append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_ACCEPTED())));
    out.acknowledged_ = std::move(acknowledged);
    return std::move(out);
}

CommitAdvance raft_commit_advance(RaftConsensusState& consensus, const PeerTable& peers, const RaftLog& log, size_t nservers) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    if (rusty::len(peers) != (rusty::detail::deref_if_pointer_like(nservers) - 1)) {
        rusty::panic::do_panic(std::format("peer table and round membership disagree"));
    }
    auto candidate_index = peers.majority_match_index(std::move(nservers), log.last_index());
    if (rusty::detail::rust_not(raft_server_log_index_above(std::move(candidate_index), (*consensus_shadow1).commit_index_))) {
        return CommitAdvance{.advanced_ = false, .from_ = static_cast<uint64_t>(0), .to_ = static_cast<uint64_t>(0)};
    }
    auto candidate = log.get(std::move(candidate_index));
    if (candidate.is_none()) {
        rusty::panic::do_panic(std::format("committable index is absent from the log"));
    }
    if (rusty::detail::rust_not(raft_server_log_entry_is_current_term(candidate.unwrap().term(), (*consensus_shadow1).current_term_))) {
        return CommitAdvance{.advanced_ = false, .from_ = static_cast<uint64_t>(0), .to_ = static_cast<uint64_t>(0)};
    }
    auto from = (*consensus_shadow1).commit_index_;
    (*consensus_shadow1).commit_index_ = std::move(candidate_index);
    return CommitAdvance{.advanced_ = true, .from_ = std::move(from), .to_ = std::move(candidate_index)};
}

Phase3Outcome heartbeat_phase3_locked(RaftConsensusState& consensus, const PeerTable& peers, const RaftLog& log, AuthorityLedger& ledger, size_t nservers, std::span<const uint16_t> members, bool is_leader) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    auto commit = raft_commit_advance((*consensus_shadow1), peers, log, std::move(nservers));
    const auto outcome = ledger.settle(std::move(is_leader), (*consensus_shadow1).current_term_, members, (*consensus_shadow1).read_quorum_confirmed_term_, (*consensus_shadow1).read_quorum_confirmed_round_);
    auto confirmed = false;
    if (outcome.confirmed()) {
        (*consensus_shadow1).read_quorum_confirmed_term_ = outcome.term();
        (*consensus_shadow1).read_quorum_confirmed_round_ = outcome.round_id();
        confirmed = true;
    }
    return Phase3Outcome{.commit_ = std::move(commit), .confirmed_ = std::move(confirmed)};
}

Phase0Outcome heartbeat_phase0_locked(RaftConsensusState& consensus, const PeerTable& peers, const RaftLog& log, HeartbeatRoundScope& round, PendingTable& pending, AuthorityLedger& ledger, rusty::Option<uint64_t>& pending_leader_term, std::span<const uint16_t> members, uint16_t site_id, bool is_leader) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    rusty::Option<uint64_t>* pending_leader_term_shadow1 = &pending_leader_term;
    if (!is_leader) {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term_shadow1 = rusty::None;
        return Phase0Outcome{.restart_ = true, .commit_advanced_ = false, .commit_from_ = static_cast<uint64_t>(0), .commit_to_ = static_cast<uint64_t>(0)};
    }
    round.begin((*consensus_shadow1).current_term_, (*consensus_shadow1).heartbeat_round_);
    if (rusty::len(pending) != rusty::len(peers)) {
        pending.resize(rusty::len(peers));
    }
    const auto epoch_changed = ((*pending_leader_term_shadow1)).is_none() || (((*pending_leader_term_shadow1)).as_ref().unwrap() != round.term());
    if (epoch_changed) {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term_shadow1 = rusty::Option<uint64_t>(round.term());
    }
    if (raft_server_read_index_round_can_advance((*consensus_shadow1).heartbeat_round_)) {
        rusty::detail::deref_if_pointer_like((*consensus_shadow1).heartbeat_round_) += 1;
    }
    auto i = 0;
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(members)) {
        round.admit(members[i]);
        rusty::detail::deref_if_pointer_like(i) += 1;
    }
    if ((round.nservers() == static_cast<size_t>(0)) || !round.is_member(std::move(site_id))) {
        rusty::panic::do_panic(std::format("heartbeat round admitted no quorum containing this site"));
    }
    const auto advance = raft_commit_advance((*consensus_shadow1), peers, log, round.nservers());
    round.publish_commit_index((*consensus_shadow1).commit_index_);
    return Phase0Outcome{.restart_ = false, .commit_advanced_ = advance.advanced(), .commit_from_ = advance.from_index(), .commit_to_ = advance.to_index()};
}

bool heartbeat_round_saturated(uint64_t round_counter) {
    return rusty::detail::rust_not(raft_server_read_index_round_can_advance(std::move(round_counter)));
}


inline HeartbeatRoundScope HeartbeatRoundScope::new_() {
    return HeartbeatRoundScope{.term_ = static_cast<uint64_t>(0), .round_id_ = static_cast<uint64_t>(0), .config_ = rusty::BTreeSet<uint16_t>::new_(), .current_commit_index_ = static_cast<uint64_t>(0), .authority_inserted_ = false};
}

inline void HeartbeatRoundScope::begin(uint64_t term, uint64_t round_id) {
    this->term_ = std::move(term);
    this->round_id_ = std::move(round_id);
    this->config_.clear();
    this->current_commit_index_ = static_cast<uint64_t>(0);
    this->authority_inserted_ = false;
}

inline void HeartbeatRoundScope::admit(uint16_t site) {
    this->config_.insert(std::move(site));
}

inline uint64_t HeartbeatRoundScope::term() const {
    return this->term_;
}

inline uint64_t HeartbeatRoundScope::round_id() const {
    return this->round_id_;
}

inline size_t HeartbeatRoundScope::nservers() const {
    return rusty::len(this->config_);
}

inline bool HeartbeatRoundScope::is_member(uint16_t site) const {
    return rusty::contains(this->config_, &site);
}

inline void HeartbeatRoundScope::publish_commit_index(uint64_t index) {
    this->current_commit_index_ = std::move(index);
}

inline uint64_t HeartbeatRoundScope::commit_index() const {
    return this->current_commit_index_;
}

inline void HeartbeatRoundScope::set_authority_inserted(bool inserted) {
    this->authority_inserted_ = std::move(inserted);
}

inline bool HeartbeatRoundScope::authority_inserted() const {
    return this->authority_inserted_;
}

inline SentAppend SentAppend::new_(uint16_t follower, uint64_t term, uint64_t round, uint64_t end_index, size_t ordinal) {
    return SentAppend{.follower_ = std::move(follower), .term_ = std::move(term), .round_ = std::move(round), .end_index_ = std::move(end_index), .ordinal_ = std::move(ordinal)};
}

inline uint64_t SentAppend::round() const {
    return this->round_;
}

inline AppendReply AppendReply::new_(bool available, bool status, uint64_t term, uint64_t last_log_index) {
    return AppendReply{.available_ = std::move(available), .status_ = std::move(status), .term_ = std::move(term), .last_log_index_ = std::move(last_log_index)};
}

inline AppendReplyAction AppendReplyOutcome::action() const {
    return this->action_;
}

inline BackoffKind AppendReplyOutcome::rung() const {
    return this->rung_;
}

inline uint64_t AppendReplyOutcome::old_next() const {
    return this->old_next_;
}

inline uint64_t AppendReplyOutcome::new_next() const {
    return this->new_next_;
}

inline uint64_t AppendReplyOutcome::acknowledged() const {
    return this->acknowledged_;
}

inline uint64_t AppendReplyOutcome::previous_term() const {
    return this->previous_term_;
}

inline bool CommitAdvance::advanced() const {
    return this->advanced_;
}

inline uint64_t CommitAdvance::from_index() const {
    return this->from_;
}

inline uint64_t CommitAdvance::to_index() const {
    return this->to_;
}

inline const CommitAdvance& Phase3Outcome::commit() const {
    return this->commit_;
}

inline bool Phase3Outcome::confirmed() const {
    return this->confirmed_;
}

inline bool Phase0Outcome::restart() const {
    return this->restart_;
}

inline bool Phase0Outcome::commit_advanced() const {
    return this->commit_advanced_;
}

inline uint64_t Phase0Outcome::commit_from() const {
    return this->commit_from_;
}

inline uint64_t Phase0Outcome::commit_to() const {
    return this->commit_to_;
}
/*RUSTYCPP:GEN-END id=raft_server.heartbeat_round_scope*/

// The three loop-carried locals, which outlive a round but not the loop. They
// stay C++ because unique_ptr<PendingAppendEntries> and the wire types inside
// PendingHeartbeatAuthority have no DSL spelling; the Rust driver carries this
// object as an opaque handle and never looks inside it.
struct HeartbeatRoundState {
  // Keep at most one AppendEntries RPC in flight per follower. A synchronous
  // follower may legitimately take longer than one heartbeat interval to
  // persist an entry; retaining its context lets a later round consume that
  // acknowledgement instead of queueing duplicate writes and discarding every
  // late success.
  PendingTable pending_rpcs{PendingTable::new_()};
  AuthorityLedger authority_rounds{AuthorityLedger::new_()};
  // rusty::Option, not std::optional: PHASE 0 is a DSL body now and takes
  // this by &mut, which emits as rusty::Option<uint64_t>&.
  rusty::Option<uint64_t> pending_leader_term{rusty::None};
  // PHASE 0 establishes every field of this each round, so it needs no reset;
  // when PHASE 0 declines the round, phases 1-3 never read it.
  HeartbeatRoundScope scope{HeartbeatRoundScope::new_()};
};

// Rebuilds the ordinal peer tables from current_config_.
//
// MUST BE CALLED WITH mtx_ HELD. Deliberately does not take the lock itself:
// setIsLeader's callers already hold it, and re-acquiring would add one more
// nested acquisition to the 24 that Step C of
// docs/migration/raft/heartbeat-first-conversion-plan.md has to remove.
//
// Rebuilt rather than appended to: the two callers each run once per
// leadership acquisition or loop start, and ordinals must not accumulate
// across terms. The two tables are sized together so an ordinal means the
// same thing in both.
void RaftServer::RebuildPeerTables(uint64_t next_index) {
  const std::set<siteid_t>& replication_targets = current_config_;
  peer_sites_.clear();
  for (const auto peer_id : replication_targets) {
    if (peer_id == site_id_) {
      continue;
    }
    peer_sites_.push_back(peer_id);
  }
  peers_.reset(peer_sites_.size(), next_index);
  const size_t expected = replication_targets.size() -
      static_cast<size_t>(replication_targets.count(site_id_) > 0);
  verify(peers_.len() == expected);
}

// @unsafe - timer allocation, peer-table rebuild under mtx_, atomic stores
void RaftServer::HeartbeatPrologue() {
  heartbeat_loop_running_.store(
      true, rusty::sync::atomic::Ordering::Release);
  {
    // Taken explicitly. setIsLeader does this rebuild holding mtx_ and this
    // did not, which was safe only because both run as fibers on the one poll
    // thread with no suspension in between -- an accident, not a design, and
    // one that Step B's Mutex<T> grouping would have turned into a real
    // inconsistency.
    std::lock_guard<std::mutex> lock(mtx_);
    RebuildPeerTables(1);
  }

  Log_debug("heartbeat loop init from site: {}", site_id_);
  looping_.store(true, rusty::sync::atomic::Ordering::Release);
}

// @safe - acquire load, exactly as the old loop condition read it
bool RaftServer::HeartbeatLooping() const {
  return looping_.load(rusty::sync::atomic::Ordering::Acquire);
}

// @safe - two release stores
void RaftServer::HeartbeatEpilogue() {
  looping_.store(false, rusty::sync::atomic::Ordering::Release);
  heartbeat_loop_running_.store(
      false, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - one heartbeat round: locks, RPC sends, reply polling, commit.
//
// This is the former while-body, unchanged except for its two OUTER-level
// exits, which a function must spell differently from a loop:
//   the wait's `break`            -> return false  (stop looping)
//   PHASE 0's !IsLeader `continue`-> return true   (skip to the next round)
// Both were confirmed to be outer-level by brace depth, with no loop between
// them and the round block. Every other break and continue in here belongs to
// an inner loop and is untouched.
// @unsafe - suspends on the wake gate; false means shutdown, not a timeout
bool RaftServer::HeartbeatWait() {
  return WaitForReplicationOrHeartbeat(heartbeat_interval_us_);
}

// @unsafe - takes mtx_, advances the read-index round, recomputes the commit
// index. Returns false when leadership is not held, which the C++ spelled as
// `continue` and the Rust driver spells as skipping phases 1 to 3.
bool RaftServer::HeartbeatPhase0(HeartbeatRoundState& state,
                                 HeartbeatRoundScope& round) {
  // PHASE 0's decisions are heartbeat_phase0_locked, a DSL body. What is
  // left here is the three things that cannot cross: the mutex, the apply
  // queue, and the debug logging whose level short-circuit must survive.
  auto& pending_rpcs = state.pending_rpcs;
  auto& authority_rounds = state.authority_rounds;
  auto& pending_leader_term = state.pending_leader_term;

  // Sorted, duplicate-free, which is what a std::set iteration yields and
  // what both the round's membership and the ledger's set-equality check
  // expect. Built once and used for both.
  const std::vector<siteid_t> round_members(current_config_.begin(),
                                            current_config_.end());
  {
    std::lock_guard<std::mutex> lock(mtx_);
    const bool leader = IsLeaderLocked();
    if (leader && heartbeat_round_saturated(state_.heartbeat_round_)) {
      Log_error("[READ-INDEX] site={} heartbeat round saturated in term {}",
                site_id_, state_.current_term_);
    }
    if (leader) {
      for (size_t ord = 0; ord < peers_.len(); ord++) {
        Log_debug("[COMMIT-CALC] match_index_[{}] = {}", peer_sites_[ord],
                  peers_.match_index(ord));
      }
    }

    const Phase0Outcome outcome = heartbeat_phase0_locked(
        state_, peers_, raft_log_, round, pending_rpcs, authority_rounds,
        pending_leader_term, round_members, site_id_, leader);

    if (outcome.restart()) {
      // Was `continue`; the Rust driver starts the next round when this
      // returns true.
      return true;
    }
    if (outcome.commit_advanced()) {
      // The apply queue is I/O, so the hand-off stays here; the DECISION to
      // commit was made above.
      EnqueueCommittedEntries(outcome.commit_from(), outcome.commit_to());
    }
  }

  round.set_authority_inserted(authority_rounds.open(
      round.round_id(), round_members,
      HeartbeatAuthority::new_(round.term(), round.nservers(), site_id_)));
  // state_.heartbeat_round_ never wraps. The only possible duplicate is the
  // deliberately fail-closed UINT64_MAX saturation generation, which open()
  // declines rather than overwriting.
  if (!round.authority_inserted()) {
    verify(round.round_id() == UINT64_MAX);
  }
  return true;
}

// @unsafe - builds and sends AppendEntries / InstallSnapshot per follower
void RaftServer::HeartbeatPhase1(HeartbeatRoundState& state,
                                 HeartbeatRoundScope& round) {
  const parid_t partition_id = partition_id_;
  auto& pending_rpcs = state.pending_rpcs;
  auto& authority_rounds = state.authority_rounds;

      // ========================================================================
      // PHASE 1: Send all AppendEntries RPCs in PARALLEL (non-blocking)
      // ========================================================================
      // The cursor is used for ITERATION ONLY; every read and write of a
      // follower's next index below goes through peers_.next_index(ord), which
      // is the same slot (site_id is it->first, so the key provably exists).
      //
      // This is not style. The body calls commo()->SendInstallSnapshot INSIDE
      // the lock_guard scope, and that call's completion callback takes the
      // SAME recursive mutex and writes peers_.next_index(ord). Recursive means
      // a callback that completes synchronously re-enters and mutates the map
      // while a dereferenced cursor into it is live -- the aliasing hazard
      // recorded as OWN-03 in docs/migration/raft/cpp-to-rust-precheck-raft.txt
      // and as the one genuine item of Tranche 5 in cpp-refactor-plan.md.
      // Holding no dereferenced cursor across that call is also what makes the
      // loop expressible in Rust at all: `&mut` into a map cannot be held
      // across a call that takes `&mut` to the same map.
      for (size_t ord = 0; ord < peers_.len(); ord++) {
        const siteid_t site_id = peer_sites_[ord];
        if (site_id == site_id_) {
          continue;
        }
        if (!IsLeader()) {
          break;  // Stop sending if we lost leadership
        }
        if (pending_rpcs.occupied(ord)) {
          continue;
        }

        uint64_t prevLogIndex = 0;
        uint64_t prevLogTerm = 0;
        // migrated from
        // `shared_ptr<Marshallable> cmd = nullptr` to `janus::Command{}`.
        // Empty Command (has_value() == false) signals heartbeat.
        janus::Command cmd{};
        uint64_t cmdLogTerm = 0;
        uint64_t sent_end_index = 0;
        bool skip_follower = false;
        {
          std::lock_guard<std::mutex> lock(mtx_);
          if (peers_.next_index(ord) == 0) {
            Log_warn("[APPEND_ENTRIES] Repairing wrapped next_index for "
                     "follower {} at leader last index {}",
                     site_id, raft_log_.last_index());
            peers_.set_next_index(ord, 
                raft_server_log_index_has_successor(raft_log_.last_index())
                    ? raft_server_follower_next_index(raft_log_.last_index())
                    : raft_log_.last_index());
          }
          prevLogIndex = peers_.next_index(ord) - 1;
          if (prevLogIndex > raft_log_.last_index()) {
            Log_info("[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > raft_log_.last_index() ({}), fixing next_index", prevLogIndex, raft_log_.last_index());
            peers_.set_next_index(ord, 
                raft_server_log_index_has_successor(raft_log_.last_index())
                    ? raft_server_follower_next_index(raft_log_.last_index())
                    : raft_log_.last_index());
            prevLogIndex = peers_.next_index(ord) - 1;
          }
          // Until a payload is selected, this is a heartbeat and proves only
          // the prefix named by prevLogIndex.
          sent_end_index = raft_server_append_sent_end(prevLogIndex, 0);

          if (prevLogIndex > raft_log_.last_index()) {
            Log_info("[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > raft_log_.last_index() ({}), skipping",
                     site_id, prevLogIndex, raft_log_.last_index());
            peers_.set_next_index(ord, 1);
            skip_follower = true;
          } else if (peers_.next_index(ord) < raft_log_.base() && snapshot_manager_) {
            // @unsafe - Follower is too far behind (log compacted), send InstallSnapshot
            Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < raft_log_.base()={}, sending InstallSnapshot",
                     site_id_, site_id, peers_.next_index(ord), raft_log_.base());
            janus::raft::SnapshotMetadata snap_meta;
            std::string snap_data;
            if (snapshot_manager_->LoadLatestSnapshot(&snap_meta, &snap_data)) {
              uint64_t snap_last_idx = snap_meta.last_included_index;
              uint64_t snap_last_term = snap_meta.last_included_term;
              uint64_t send_term = state_.current_term_;
              auto callback_lifetime = async_callback_lifetime_;
              commo()->SendInstallSnapshot(
                  site_id, partition_id_,
                  send_term, site_id_,
                  snap_last_idx, snap_last_term,
                  snap_data,
                  [callback_lifetime, site_id, ord, snap_last_idx, send_term](uint64_t follower_term) {
                    std::lock_guard<std::mutex> lifetime_lock(
                        callback_lifetime->mutex);
                    auto* server = callback_lifetime->server;
                    if (server == nullptr) {
                      return;
                    }
                    // THE LOCK IS TAKEN BELOW THIS CHECK, NOT ABOVE IT, and
                    // that placement is load-bearing.
                    //
                    // This callback runs in TWO contexts. Normally the reactor
                    // invokes it when the reply lands, with mtx_ not held. But
                    // RaftCommo::SendInstallSnapshot invokes it INLINE, on the
                    // caller's stack, when PeerForSite returns null
                    // (commo.cc:167-170) -- and that caller is PHASE 1, which
                    // holds mtx_. A recursive_mutex tolerates the re-entry; a
                    // plain std::mutex would self-deadlock, which is what
                    // blocked demoting it.
                    //
                    // The inline path always passes follower_term == 0, so it
                    // takes the branch below and returns having touched no
                    // state at all. Acquiring after the check means the
                    // synchronous context never reaches the lock, and every
                    // path that does reach it is the asynchronous one. site_id_
                    // is written once during Setup, so reading it for the log
                    // needs no lock.
                    if (!raft_server_install_snapshot_reply_is_available(
                            follower_term)) {
                      Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} snapshot response unavailable; retaining replication indices",
                               server->site_id_, site_id);
                      return;
                    }
                    // @unsafe - callback modifies shared state under lock
                    std::lock_guard<std::mutex> lock(server->mtx_);
                    if (raft_server_observed_higher_term(
                            follower_term, server->state_.current_term_)) {
                      Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} has higher term {} > {}, stepping down",
                               server->site_id_, site_id, follower_term,
                               server->state_.current_term_);
                      const uint64_t previous_term = server->state_.current_term_;
                      server->state_.current_term_ = follower_term;
                      server->state_.vote_for_ = INVALID_SITEID;
                      server->LogTermChange(
                          "InstallSnapshot reply carried newer term",
                          previous_term, server->state_.current_term_, site_id);
                      // A follower's higher term does not identify the leader
                      // of that term. Retire the previous leader hint before
                      // publishing follower state.
                      server->state_.current_leader_id_ =
                          raft_server_leader_hint_after_transition(
                              false, false, server->site_id_, site_id);
                      server->stepDown();
                      server->state_.req_voting_ = false;
                      server->state_.election_in_progress_ = false;
                      return;
                    }
                    if (server->state_.current_term_ != send_term) {
                      Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Term changed since snapshot send, ignoring response",
                               server->site_id_);
                      return;
                    }
                    server->peers_.accept_through(ord,
                        snap_last_idx,
                        raft_server_log_index_has_successor(snap_last_idx),
                        raft_server_log_index_has_successor(snap_last_idx)
                            ? raft_server_follower_next_index(snap_last_idx)
                            : snap_last_idx);
                    Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Updated follower {}: next_index={} match_index={}",
                             server->site_id_, site_id,
                             server->peers_.next_index(ord),
                             server->peers_.match_index(ord));
                  });
              skip_follower = true;  // Skip normal AppendEntries for this follower
            } else {
              Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                       site_id_, site_id);
              skip_follower = true;
            }
          } else {
            verify(prevLogIndex <= raft_log_.last_index());
            if (prevLogIndex == 0) {
              prevLogTerm = 0;
            } else if (prevLogIndex == state_.snapidx_ && state_.snapidx_ > 0) {
              // Keep using snapshot boundary metadata after compaction.
              prevLogTerm = state_.snapterm_;
            } else {
              // Was GetRaftInstance, which default-inserted and therefore
              // can never return null -- so the check below was dead, and a
              // genuinely missing prevLogIndex silently fabricated an empty
              // entry with term 0 and sent prevLogTerm = 0 rather than
              // skipping the follower. FindRaftInstance makes the check live.
              auto instance = FindRaftInstance(prevLogIndex);
              if (!instance) {
                Log_error("[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}",
                          prevLogIndex, site_id);
                skip_follower = true;
              } else {
                prevLogTerm = instance->term();
              }
            }

            if (!skip_follower) {
#ifndef RAFT_BATCH_OPTIMIZATION
              Log_debug("[BATCH_CHECK] site={} follower={} next_index={} raft_log_.base()={} raft_log_.last_index()={}",
                       site_id_, site_id, peers_.next_index(ord), raft_log_.base(), raft_log_.last_index());
              if (peers_.next_index(ord) <= raft_log_.last_index()) {
                if (!raft_server_append_entry_count_fits(prevLogIndex, 1)) {
                  Log_error("[HEARTBEAT-SEND] Log index exhausted after {}, "
                            "skipping follower {}",
                            prevLogIndex, site_id);
                  skip_follower = true;
                } else {
                  const RaftEntry* cur_log =
                      FindRaftInstance(peers_.next_index(ord));
                  if (cur_log == nullptr || !cur_log->cmd().has_value()) {
                    Log_error("[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                              peers_.next_index(ord), site_id);
                    skip_follower = true;
                  } else {
                    const RaftEntry* curInstance = cur_log;
                    // cmd is Command; assign directly from
                    // curInstance->log_ (also Command).
                    cmd = curInstance->cmd();
                    cmdLogTerm = curInstance->term();
                    sent_end_index =
                        raft_server_append_sent_end(prevLogIndex, 1);
                    // 2 step 1: debug log no longer needs the
                    // inner shared_ptr's raw pointer; the kind tag is
                    // a more useful identifier anyway.
                    Log_debug("[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        site_id_, peers_.next_index(ord), site_id, cmd.kind_);
                  }
                }
              }
#endif

#ifdef RAFT_BATCH_OPTIMIZATION
              vector<rusty::Arc<TpcCommitCommand>> batch_buffer_;
              const uint64_t max_batch_entries = GetAppendEntriesBatchMaxEntries();
              const uint64_t batch_start_idx = peers_.next_index(ord);
              Log_debug("[BATCH_CHECK] site={} follower={} next_index={} raft_log_.base()={} raft_log_.last_index()={}",
                       site_id_, site_id, peers_.next_index(ord), raft_log_.base(), raft_log_.last_index());
              if (!raft_server_append_entry_count_fits(prevLogIndex, 1)) {
                Log_error("[HEARTBEAT-BATCH] Log index exhausted after {}, "
                          "skipping follower {}",
                          prevLogIndex, site_id);
                skip_follower = true;
              }
              const uint64_t first_encoded_index = skip_follower
                  ? 0
                  : raft_server_append_sent_end(prevLogIndex, 1);
              if (!skip_follower &&
                  (batch_start_idx != first_encoded_index ||
                   batch_start_idx < raft_log_.base())) {
                Log_error("[HEARTBEAT-BATCH] Non-contiguous source for follower {}: "
                          "prev={} start={} min_active={}; refusing to compress a hole",
                          site_id, prevLogIndex, batch_start_idx,
                          raft_log_.base());
                skip_follower = true;
              } else if (!skip_follower) {
                for (uint64_t idx = batch_start_idx;
                     idx <= raft_log_.last_index() &&
                     batch_buffer_.size() < max_batch_entries;) {
                  const RaftEntry* cur_log = FindRaftInstance(idx);
                  if (cur_log == nullptr || !cur_log->cmd().has_value()) {
                    Log_error("[HEARTBEAT-BATCH] Missing log entry {} for follower {}; "
                              "refusing to compress a hole",
                              idx, site_id);
                    skip_follower = true;
                    break;
                  }
                  const RaftEntry* curInstance = cur_log;
                  // curInstance->cmd() is Command; the
                  // `marshallable_cast<T>(SerializableEnvelope&)`
                  // overload (in serializable_envelope.hpp) handles
                  // this directly.
                  auto curCmd =
                      marshallable_cast<TpcCommitCommand>(curInstance->cmd());
                  if (curCmd.is_none()) {
                    if (batch_buffer_.empty()) {
                      Log_info("[BATCH_SKIP] site={} idx={}: log entry is not "
                               "TpcCommitCommand (kind={}), using raw log",
                               site_id_, idx, curInstance->cmd().kind_);
                      cmd = curInstance->cmd();
                      cmdLogTerm = curInstance->term();
                      sent_end_index =
                          raft_server_append_sent_end(prevLogIndex, 1);
                    } else {
                      Log_info("[BATCH_STOP] site={} idx={}: ending batch before "
                               "non-TpcCommitCommand kind={}",
                               site_id_, idx, curInstance->cmd().kind_);
                    }
                    break;
                  }
                  // STAMP A COPY, NEVER THE STORED ENTRY.
                  //
                  // The batched wire format carries each entry's term inside
                  // its TpcCommitCommand -- the receiver reads it back at
                  // server.cc:4730 to set the entry's term -- and the command
                  // is created with term 0 (raft_worker.cc:741), so something
                  // has to stamp it. This used to const_cast the payload of
                  // the entry in the log and write through it: a mutation
                  // of committed, already-replicated, shared state, performed
                  // lazily at send time.
                  //
                  // It was idempotent, because a committed entry's term never
                  // changes, so it was not a live bug. But it is the only
                  // place in the file that modifies an existing log entry, and
                  // a log that can be modified after commit cannot state its
                  // own invariants -- so it has no spelling in a Rust-owned
                  // RaftLog, whose entries are reachable only as &RaftEntry.
                  //
                  // Copying is cheap and does not touch the payload:
                  // TpcCommitCommand is two scalars, an int, and two Arcs, so
                  // the copy bumps refcounts and leaves the LogEntry bytes
                  // shared.
                  // @unsafe { factory-fresh Arc, uniquely owned mutation window }
                  {
                    auto stamped = rusty::Arc<TpcCommitCommand>::make(
                        *curCmd.as_ref().unwrap());
                    stamped.get_mut().unwrap().term = curInstance->term();
                    batch_buffer_.push_back(std::move(stamped));
                  }
                  if (!raft_server_log_index_has_successor(idx)) {
                    break;
                  }
                  ++idx;
                }
              }
              if (!skip_follower && batch_buffer_.size() > 0) {
                const uint64_t encoded_entry_count =
                    static_cast<uint64_t>(batch_buffer_.size());
                if (!raft_server_append_batch_count_is_valid(
                        prevLogIndex, encoded_entry_count)) {
                  Log_error("[HEARTBEAT-BATCH] Invalid encoded count {} after "
                            "previous index {}; skipping follower {}",
                            encoded_entry_count, prevLogIndex, site_id);
                  skip_follower = true;
                }
              }
              if (!skip_follower && batch_buffer_.size() > 0) {
                // Fill-then-wrap: assemble locally, wrap once complete.
                TpcBatchCommand batch_local;
                batch_local.AddCmds(batch_buffer_);
                auto batch_cmd =
                    rusty::Arc<TpcBatchCommand>::make(std::move(batch_local));
                cmd = std::move(batch_cmd);
                sent_end_index = raft_server_append_sent_end(
                    prevLogIndex,
                    static_cast<uint64_t>(batch_buffer_.size()));
                const uint64_t batch_end_idx = sent_end_index;
                const bool truncated = batch_end_idx < raft_log_.last_index();
                Log_info("[BATCH_SEND] site={} sending batch of {} entries to follower {} "
                         "(from={} to={}{})",
                         site_id_, batch_buffer_.size(), site_id,
                         batch_start_idx, batch_end_idx, truncated ? ", truncated" : "");
              }
#endif
            }
          }
        }
        if (skip_follower) {
          continue;
        }

        // Create pending RPC context
        // Send RPC (non-blocking - just initiates the async call). The
        // response is a shared_ptr the transport's callback also holds, which
        // is what keeps it alive; the table carries it opaquely.
        auto sent_response = commo()->SendAppendEntries2(site_id,
                                              partition_id,
                                              -1,
                                              -1,
                                              IsLeader(),
                                              site_id_,
                                              round.term(),
                                              prevLogIndex,
                                              prevLogTerm,
                                              round.commit_index(),
                                              cmd,
                                              cmdLogTerm);

        pending_rpcs.place(ord, PendingAppend::new_(
            site_id, round.term(), round.round_id(), sent_end_index,
            std::move(sent_response), cmd));
        if (round.authority_inserted() && round.is_member(site_id)) {
          // Was `authority_it->second`, a std::map iterator created in PHASE 0
          // and dereferenced here, after the RPC sends. Nothing between the two
          // points mutates authority_rounds, so the iterator was valid and this
          // is the same element -- but a cursor held across a phase boundary and
          // across a synchronous completion callback is the hazard class commit
          // 4427129a9 removed for next_index_, so look it up by key.
          verify(authority_rounds.launch(round.round_id(), site_id));
        }
      }
}

// @unsafe - polls replies through one round deadline and processes them
void RaftServer::HeartbeatPhase2(HeartbeatRoundState& state,
                                 HeartbeatRoundScope& round) {
  auto& pending_rpcs = state.pending_rpcs;
  auto& authority_rounds = state.authority_rounds;

      // ========================================================================
      // PHASE 2: Poll responses through one SHORT round deadline and process them
      // ========================================================================
      // Do not call wait_timeout on an individual response: that permanently
      // marks its event TIMEOUT and loses a legitimate late persistence reply.
      // Polling also gives every parallel RPC the same bounded round budget.
      constexpr uint64_t RESPONSE_POLL_STEP_US = 1000;
      const uint64_t response_round_timeout_us = std::max<uint64_t>(
          1, std::min<uint64_t>(100000, heartbeat_interval_us_));
      const auto response_deadline =
          std::chrono::steady_clock::now() +
          std::chrono::microseconds(response_round_timeout_us);
      bool stop_response_processing = false;
      bool retry_released_follower = false;
      while (!stop_response_processing) {
        bool waiting_for_current_round = false;

        for (size_t pending_ord = 0; pending_ord < pending_rpcs.len();
             pending_ord++) {
          if (!IsLeader()) {
            stop_response_processing = true;
            break;
          }
          if (!pending_rpcs.occupied(pending_ord)) {
            continue;
          }

          // Bound once per slot per poll pass, not per use: every read below
          // is the same shape it was when `pending` was a map value.
          const PendingView pending{
              pending_rpcs.follower(pending_ord),
              pending_rpcs.sent_term(pending_ord),
              pending_rpcs.sent_round(pending_ord),
              pending_rpcs.sent_end_index(pending_ord),
              pending_rpcs.cmd(pending_ord)};
          auto &resp = *pending_rpcs.response(pending_ord);
          if (!resp.completed.load(std::memory_order_acquire)) {
            if (pending.sent_round == round.round_id()) {
              waiting_for_current_round = true;
            }
            continue;
          }

          bool stepped_down = false;
          {
            std::lock_guard<std::mutex> lock(mtx_);
            // What the reply MEANS is heartbeat_apply_append_reply, a DSL
            // body. It reads the wire response as three scalars -- the rrr
            // object itself never crosses -- and returns what the caller
            // must do about it.
            const bool response_available =
                !(resp.status == false && resp.term == 0 &&
                  resp.last_log_index == 0);
            const size_t resp_ord = PeerOrdinal(pending.follower_id);
            const AppendReplyOutcome outcome = heartbeat_apply_append_reply(
                state_, peers_, authority_rounds,
                SentAppend::new_(pending.follower_id, pending.sent_term,
                                 pending.sent_round, pending.sent_end_index,
                                 resp_ord),
                AppendReply::new_(response_available, resp.status, resp.term,
                                  resp.last_log_index),
                raft_log_.last_index(), IsLeaderLocked());

            switch (outcome.action()) {
              case AppendReplyAction::STEP_DOWN: {
                Log_info(
                    "[STEPDOWN] Site {}: AppendEntries response from follower {} "
                    "carried higher term {} > {}",
                    site_id_, pending.follower_id, resp.term,
                    outcome.previous_term());
                LogTermChange("AppendEntries response carried newer term",
                              outcome.previous_term(), state_.current_term_,
                              pending.follower_id);
                // stepDown reaches setIsLeader and the election timer, so it
                // stays here; the decision to take it was made above.
                stepDown();
                state_.req_voting_ = false;
                state_.election_in_progress_ = false;
                stepped_down = true;
                break;
              }
              case AppendReplyAction::BACKED_OFF: {
                // The five-rung ladder is FollowerProgress::back_off_after_reject;
                // it reports which rung it took so the diagnostics stay as
                // specific as they were when the branches were inline.
                switch (outcome.rung()) {
                  case BackoffKind::FAST:
                    Log_info("[LOG-RECONCILE] Site {}: Fast backoff for "
                             "follower {}: next_index {} -> {} (gap: {}, "
                             "follower reported last: {})",
                             site_id_, pending.follower_id, outcome.old_next(),
                             outcome.new_next(),
                             outcome.old_next() - outcome.new_next(),
                             resp.last_log_index);
                    break;
                  case BackoffKind::TERM_CONFLICT:
                    Log_info("[LOG-RECONCILE] Site {}: Term-conflict backoff "
                             "for follower {}: next_index {} -> {}",
                             site_id_, pending.follower_id, outcome.old_next(),
                             outcome.new_next());
                    break;
                  case BackoffKind::EXPONENTIAL:
                    Log_info("[LOG-RECONCILE] Site {}: Exponential backoff for "
                             "follower {}: next_index {} -> {} (halved)",
                             site_id_, pending.follower_id, outcome.old_next(),
                             outcome.new_next());
                    break;
                  case BackoffKind::LINEAR:
                    Log_debug("[LOG-RECONCILE] Site {}: Linear backoff for "
                              "follower {}: next_index {} -> {}",
                              site_id_, pending.follower_id, outcome.old_next(),
                              outcome.new_next());
                    break;
                  case BackoffKind::FLOOR:
                    break;
                }
                break;
              }
              case AppendReplyAction::ACCEPTED:
                Log_debug(
                    "[APPEND_RPC] Leader {} accepted follower {} proof: "
                    "kind={} reported={} sent_end={} acknowledged={} "
                    "next={} match={}",
                    site_id_, pending.follower_id,
                    pending.cmd.has_value() ? "entries" : "heartbeat",
                    resp.last_log_index, pending.sent_end_index,
                    outcome.acknowledged(), peers_.next_index(resp_ord),
                    peers_.match_index(resp_ord));
                break;
              case AppendReplyAction::CONTRADICTORY:
                Log_warn("[APPEND_RPC] Ignoring contradictory success from "
                         "follower {}: reported_end={} sent_end={}",
                         pending.follower_id, resp.last_log_index,
                         pending.sent_end_index);
                break;
              case AppendReplyAction::UNKNOWN_FOLLOWER:
                Log_debug(
                    "[APPEND_RPC] Ignoring replication response from removed "
                    "follower {}",
                    pending.follower_id);
                break;
              case AppendReplyAction::IGNORED:
                break;
            }
          }

          const bool completed_previous_round = pending.sent_round != round.round_id();
          pending_rpcs.release(pending_ord);
          retry_released_follower =
              retry_released_follower || completed_previous_round;
          if (stepped_down) {
            stop_response_processing = true;
            break;
          }
        }

        const bool current_round_has_authority =
            authority_rounds.has_quorum(round.round_id());
        if (stop_response_processing || !waiting_for_current_round ||
            current_round_has_authority) {
          break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= response_deadline) {
          break;
        }
        const auto remaining_us = std::chrono::duration_cast<
            std::chrono::microseconds>(response_deadline - now).count();
        Fiber::sleep(static_cast<int>(std::min<uint64_t>(
            RESPONSE_POLL_STEP_US,
            static_cast<uint64_t>(std::max<int64_t>(remaining_us, 1)))));
      }
      if (stop_response_processing) {
        pending_rpcs.abandon();
        authority_rounds.abandon();
      } else if (retry_released_follower) {
        // A completion from an older round opened a per-follower slot after
        // Phase 1. Prompt another round instead of waiting a full interval.
        RequestReplication();
      }
}

// @unsafe - recomputes the commit index from the new evidence and publishes
// read-index authority
void RaftServer::HeartbeatPhase3(HeartbeatRoundState& state,
                                 HeartbeatRoundScope& round) {
  // PHASE 3's decisions are heartbeat_phase3_locked, a DSL body: recompute
  // the commit index now that this round's replies are in, then publish
  // read-index authority, in that order. What is left here is the mutex, the
  // apply queue, the replication wake, and the debug logging.
  auto& authority_rounds = state.authority_rounds;
  if (!IsLeader()) {
    return;
  }
  bool commit_advanced_after_send = false;
  {
    std::lock_guard<std::mutex> lock(mtx_);
    const std::vector<siteid_t> settle_members(current_config_.begin(),
                                               current_config_.end());
    const Phase3Outcome outcome = heartbeat_phase3_locked(
        state_, peers_, raft_log_, authority_rounds, round.nservers(),
        settle_members, IsLeaderLocked());

    if (outcome.commit().advanced()) {
      Log_debug("[PHASE3-COMMIT] Advancing state_.commit_index_ {} -> {}",
                outcome.commit().from_index(), outcome.commit().to_index());
      EnqueueCommittedEntries(outcome.commit().from_index(),
                              outcome.commit().to_index());
      commit_advanced_after_send = true;
    }
    if (outcome.confirmed()) {
      Log_debug("[READ-INDEX] site={} confirmed round={} term={}",
                site_id_, state_.read_quorum_confirmed_round_,
                state_.read_quorum_confirmed_term_);
    }
  }

  // The AppendEntries messages for this round carried the old commit index.
  // Latch exactly one prompt follow-up round so followers learn the phase-3
  // commit without waiting for the periodic heartbeat.
  if (commit_advanced_after_send) {
    RequestReplication();
  }
}

// The extern "C" trampolines the DSL block declares. Each casts an opaque
// handle back exactly once, and this is the only place either cast happens.
extern "C" {

// @unsafe { opaque handle cast }
static inline RaftServer* raft_heartbeat_server(rusty::ffi::c_void* server) {
  return static_cast<RaftServer*>(server);
}

// @unsafe { opaque handle cast }
static inline HeartbeatRoundState* raft_heartbeat_state(
    rusty::ffi::c_void* round) {
  return static_cast<HeartbeatRoundState*>(round);
}

void raft_heartbeat_prologue(rusty::ffi::c_void* server) {
  raft_heartbeat_server(server)->HeartbeatPrologue();
}

bool raft_heartbeat_looping(rusty::ffi::c_void* server) {
  return raft_heartbeat_server(server)->HeartbeatLooping();
}

bool raft_heartbeat_wait(rusty::ffi::c_void* server) {
  return raft_heartbeat_server(server)->HeartbeatWait();
}

bool raft_heartbeat_phase0(rusty::ffi::c_void* server,
                           rusty::ffi::c_void* round) {
  HeartbeatRoundState* state = raft_heartbeat_state(round);
  return raft_heartbeat_server(server)->HeartbeatPhase0(*state, state->scope);
}

void raft_heartbeat_phase1(rusty::ffi::c_void* server,
                           rusty::ffi::c_void* round) {
  HeartbeatRoundState* state = raft_heartbeat_state(round);
  raft_heartbeat_server(server)->HeartbeatPhase1(*state, state->scope);
}

void raft_heartbeat_phase2(rusty::ffi::c_void* server,
                           rusty::ffi::c_void* round) {
  HeartbeatRoundState* state = raft_heartbeat_state(round);
  raft_heartbeat_server(server)->HeartbeatPhase2(*state, state->scope);
}

void raft_heartbeat_phase3(rusty::ffi::c_void* server,
                           rusty::ffi::c_void* round) {
  HeartbeatRoundState* state = raft_heartbeat_state(round);
  raft_heartbeat_server(server)->HeartbeatPhase3(*state, state->scope);
}

void raft_heartbeat_epilogue(rusty::ffi::c_void* server) {
  raft_heartbeat_server(server)->HeartbeatEpilogue();
}

}  // extern "C"

// @unsafe - hands two opaque handles to the Rust driver and runs it
void RaftServer::HeartbeatLoop() {
  HeartbeatRoundState round;
  const HeartbeatDriver driver = HeartbeatDriver::new_(
      static_cast<rusty::ffi::c_void*>(this),
      static_cast<rusty::ffi::c_void*>(&round));
  driver.run();
}

// @unsafe - thread join and timer cleanup require manual resource management
RaftServer::~RaftServer() {
  // Make shutdown idempotent for never-started servers and for callers that
  // already completed PrepareForShutdown().  A live server must be prepared
  // on a reactor fiber before its destructor runs.
  stop_.store(true, rusty::sync::atomic::Ordering::Release);
  looping_.store(false, rusty::sync::atomic::Ordering::Release);
  CloseReplicationWakeGate();
  verify(!heartbeat_loop_running_.load(
      rusty::sync::atomic::Ordering::Acquire));
  verify(!election_loop_running_.load(
      rusty::sync::atomic::Ordering::Acquire));

  {
    std::lock_guard<std::mutex> lifetime_lock(async_callback_lifetime_->mutex);
    async_callback_lifetime_->server = nullptr;
  }

  // Stop and join the background apply thread if it was started. The thread
  // captures `this` and walks apply_queue_ / app_next_, so it must finish
  // before any member state is destroyed.
  apply_thread_running_.store(false);
  if (apply_thread_.joinable()) {
    apply_thread_.join();
  }

  Log_info("site par {}, loc {}: prepare {}, accept {}, commit {}",
      partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
}

// @unsafe - Caller holds mtx_; validates the snapshot/log invariant and reads
// the absolute last-log slot without inserting into or otherwise mutating the
// compacted log map.
ballot_t RaftServer::ElectionLastLogTermLocked() const {
  verify(raft_log_.last_index() >= state_.snapidx_);
  if (raft_server_election_last_log_uses_snapshot(raft_log_.last_index(), state_.snapidx_)) {
    return state_.snapterm_;
  }

  const RaftEntry* last_log = FindRaftInstance(raft_log_.last_index());
  verify(last_log != nullptr);
  return last_log->term();
}


bool RaftServer::RequestVoteFromElectionTimer(
    uint64_t expected_generation) {
  return RequestVoteImpl(/*timer_guarded=*/true,
                         expected_generation);
}

bool RaftServer::RequestVoteImpl(bool timer_guarded,
                                 uint64_t expected_generation) {
  // FIX 2: Prevent RequestVote during shutdown
  // The election timer coroutine may fire after ~RaftServer destructor runs,
  // causing a call to the base class TxLogServer::RequestVote() which hits verify(0)
  // Check stop_ flag to avoid this crash during teardown
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    Log_debug("[RAFT-SHUTDOWN] RequestVote called during shutdown (site={}), ignoring to prevent crash", site_id_);
    return false;
  }


  const parid_t par_id = partition_id_;
  const locid_t loc_id = loc_id_;

  slotid_t lst_idx = 0 ;
  ballot_t lst_term = 0 ;
  ballot_t prev_term = 0;
  ballot_t term = 0;
  siteid_t prev_vote_for;
  // @unsafe
  {
  prev_vote_for = INVALID_SITEID;
  }

  {
    std::lock_guard<std::mutex> lock(mtx_);
    if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
      state_.req_voting_ = false;
      return false;
    }
    // RequestVoteImpl is the sole campaign admission point. Its entrants can
    // overlap while one of them is yielding, so callers must not reserve
    // state_.req_voting_ before entering this critical section.
    if (!raft_server_campaign_can_start(
            state_.is_leader_, state_.election_in_progress_)) {
      return false;
    }
    if (timer_guarded) {
      const uint64_t now = Time::now(true);
      const uint64_t elapsed = now - state_.last_heartbeat_time_;
      if (!raft_server_timer_campaign_is_current(
              state_.is_leader_, expected_generation,
              state_.election_timer_generation_, elapsed,
              state_.election_timeout_us_)) {
        return false;
      }
    }

    // A campaign owns a fresh, latched timeout. If it loses without hearing
    // from a leader, the next campaign waits for this complete interval rather
    // than immediately reusing the already-expired follower deadline.
    resetTimerLocked("starting election campaign");
    prev_term = state_.current_term_;
    prev_vote_for = state_.vote_for_;
    auto prev_local_term = state_.current_term_;
    state_.current_term_++ ;
    state_.vote_for_ = site_id_;  // Vote for ourselves when starting election
    // A candidate has no elected leader evidence in its new term. In
    // particular, it must not redirect clients to the leader from the term it
    // just left.
    state_.current_leader_id_ = raft_server_leader_hint_after_transition(
        false, false, site_id_, state_.current_leader_id_);

    // Atomically publish ownership of state_.req_voting_ and the election term before
    // broadcasting so no second caller can campaign concurrently.
    state_.election_in_progress_ = true;
    state_.election_term_ = state_.current_term_;
    state_.req_voting_ = true;
    term = state_.current_term_;

    LogTermChange("starting election", prev_local_term, state_.current_term_);
    lst_idx = raft_log_.last_index();
    lst_term = ElectionLastLogTermLocked();
  }

#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server {} (loc {}) starting election term {}->{} lastLogIdx={} lastLogTerm={} prev_vote_for={}",
           site_id_, loc_id, prev_term, term, lst_idx, lst_term, prev_vote_for);
#endif
  shared_ptr<RaftVoteQuorumEvent> sp_quorum;
  // @unsafe
  {
  // The candidate id on the wire is a GLOBAL site id, not the per-partition
  // locale id. Everything downstream treats it that way: RaftCommo skips
  // itself by comparing against peer->site_id() (commo.cc:121-124), the
  // receiver admits a candidate only if current_config_ contains it and
  // current_config_ is filled from Config::SitesByPartitionId()'s site.id
  // (server.cc:1565-1571, 2939-2942), and this candidate has just recorded
  // state_.vote_for_ = site_id_ above, which the grant path compares against can_id.
  //
  // Passing loc_id_ here was correct only for partition 0. Config::LoadSiteYML
  // increments site_id globally across replica-group rows while resetting
  // locale_id to 0 at the top of each row (config.cc:336-366), so with three
  // replicas per group site_id == 3 * partition + locale and the two id spaces
  // coincide only when partition == 0. For every partition above 0 the
  // candidate advertised 0, 1 or 2 while current_config_ held {3p, 3p+1,
  // 3p+2}: every vote was rejected as a non-voter, the self-skip never
  // matched so a candidate also RequestVoted its own listener, and the term
  // counter ran away. It compiled silently because locid_t is uint32_t and
  // siteid_t is uint16_t (constants.h:15,18), so the call narrowed.
  sp_quorum = commo()->BroadcastVote(
      par_id, lst_idx, lst_term, site_id_, term);
  sp_quorum->wait_timeout(1000000);
  }
  std::unique_lock<std::mutex> lock1(mtx_);
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    state_.election_in_progress_ = false;
    state_.req_voting_ = false;
    return false;
  }
  // A higher term dominates every election outcome, including TIMEOUT and a
  // concurrently completed YES quorum. FeedResponse publishes this maximum
  // before its wakeup, so snapshot it only after reacquiring Raft state.
  const int64_t observed_response_term = sp_quorum->Term();
  const ElectionCompletionAction completion_action =
      static_cast<ElectionCompletionAction>(
          raft_server_election_completion_action(
              state_.election_in_progress_, state_.election_term_, term, state_.current_term_,
              observed_response_term));
  if (completion_action ==
      ElectionCompletionAction::ADVANCE_HIGHER_TERM) {
    const uint64_t previous_term = state_.current_term_;
    state_.current_term_ = static_cast<uint64_t>(observed_response_term);
    state_.vote_for_ = INVALID_SITEID;
    state_.current_leader_id_ = raft_server_leader_hint_after_transition(
        false, false, site_id_, state_.current_leader_id_);

    if (state_.is_leader_) {
      stepDown();
    } else {
      setIsLeader(false);
    }
    state_.election_in_progress_ = false;
    state_.req_voting_ = false;

    LogTermChange("observed higher term from RequestVote replies",
                  previous_term, state_.current_term_);
    return false;
  }

  // An accepted leader RPC can cancel this campaign while BroadcastVote is
  // yielding, and another campaign can then begin before this result arrives.
  // Only the exact active term owns role changes and election bookkeeping.
  // A strictly higher response term was handled above because that evidence
  // globally supersedes even a newer local campaign.
  if (completion_action == ElectionCompletionAction::IGNORE_STALE) {
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} ignoring stale election result: "
             "result_term={} local_term={} election_term={} active={}",
             site_id_, term, state_.current_term_, state_.election_term_,
             state_.election_in_progress_);
#endif
    return false;
  }
  verify(completion_action == ElectionCompletionAction::APPLY_CURRENT);
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}",
           site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get(), sp_quorum->Term(), sp_quorum->q().timeouted_.get());
#endif
  if (sp_quorum->yes()) {
    verify(state_.current_term_ >= term);

    state_.election_in_progress_ = false;
    state_.req_voting_ = false;

    if (stop_.load(rusty::sync::atomic::Ordering::Acquire) ||
        state_.current_term_ != term) {
      state_.req_voting_ = false;
      return false;
    }

    // become a leader
    setIsLeader(true) ;
    // verify(state_.current_term_ == term); // [Jetpack] Comment this since in failure recovery test this will fail after experiment end.
    Log_debug("site {} became leader for term {}", site_id_, term);

#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get());
#endif

    if(IsLeaderLocked()) {
      Log_debug("vote accepted {} curterm {}", loc_id, state_.current_term_);
  		state_.req_voting_ = false ;
			return true;
    } else {
      Log_debug("vote rejected {} curterm {}, do rollback", loc_id, state_.current_term_);
      setIsLeader(false) ;
    	return false;
		}
  } else if (sp_quorum->no()) {
    // become a follower
    Log_debug("site {} requestvote rejected", site_id_);
    setIsLeader(false) ;
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} lost election term {} (yes={} no={}) highest_term={}",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get(), sp_quorum->Term());
#endif
    if (state_.election_in_progress_ && state_.election_term_ == term) {
      state_.election_in_progress_ = false;
    }
  	state_.req_voting_ = false ;
		return false;
  } else {
    Log_debug("vote timeout {}", loc_id);
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get());
#endif
    if (state_.election_in_progress_ && state_.election_term_ == term) {
      state_.election_in_progress_ = false;
    }
  	state_.req_voting_ = false ;
		return false;
  }
}

// @unsafe - calls @safe doVote, external calls marked @external [safe]
void RaftServer::OnRequestVote(const slotid_t& lst_log_idx,
                               const ballot_t& lst_log_term,
                               const siteid_t& can_id,
                               const ballot_t& can_term,
                               ballot_t *reply_term,
                               bool_t *vote_granted) {
  std::lock_guard<std::mutex> lock(mtx_);
  Log_debug("raft receives vote from candidate: {:x}", can_id);

  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    *reply_term = state_.current_term_;
    *vote_granted = false;
    Log_debug("[RAFT-SHUTDOWN] Site {} rejecting RequestVote from {}",
              site_id_, can_id);
    return;
  }

  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool candidate_is_current_voter =
      can_id != invalid && can_id != site_id_ &&
      current_config_.count(can_id) != 0;
  if (can_term < 0 || lst_log_term < 0 ||
      !candidate_is_current_voter) {
    *reply_term = static_cast<ballot_t>(state_.current_term_);
    *vote_granted = false;
    Log_warn("[RAFT_VOTE] Site {} rejected malformed/non-voter candidate {} "
             "term {} last_log_term {} (voter={})",
             site_id_, can_id, can_term, lst_log_term,
             candidate_is_current_voter);
    return;
  }

  uint64_t cur_term = state_.current_term_ ;
  if( can_term < cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;
    return ;
  }

  // has voted to a machine in the same term, vote no
  // CRITICAL FIX: Only reject if we already voted for someone else in this term
  // Standard Raft allows voting for the SAME candidate multiple times (idempotent)
  // and allows voting if we haven't voted yet in this term
  // @unsafe
  {
  if( can_term == cur_term && state_.vote_for_ != INVALID_SITEID && state_.vote_for_ != can_id )
  {
    Log_debug("site {} vote NO for {} (already voted for {} in term {})",
              site_id_, can_id, state_.vote_for_, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;
    return ;
  }
  }

  // Every grant, including an idempotent retry, must still carry an up-to-date
  // candidate log. This is defensive against damaged/legacy persistent state
  // and is the Raft RequestVote rule in its direct form.
  verify(raft_log_.last_index() >= state_.snapidx_);
  const slotid_t lstoff = raft_log_.last_index() - state_.snapidx_;
  const ballot_t curlstterm = ElectionLastLogTermLocked();
  const slotid_t curlstidx = raft_log_.last_index();
  const bool candidate_log_is_current =
      raft_server_candidate_log_is_at_least(
          lst_log_term, curlstterm, lst_log_idx, curlstidx);

  // If we already voted for this same candidate in this term, vote YES again
  // only when the retry still satisfies log freshness.
  if (raft_server_vote_is_idempotent(
          static_cast<uint64_t>(can_term), cur_term, state_.vote_for_, can_id) &&
      candidate_log_is_current)
  {
    Log_debug("site {} vote YES for {} (already voted for them in term {}, idempotent)",
              site_id_, can_id, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true) ;
    return ;
  }

  // lstoff starts from 1
  Log_debug("vote for lstoff {}, curlstterm {}, curlstidx {}", lstoff, curlstterm, curlstidx  );


  // Snapshot-aware offset invariant.
  verify(lstoff + state_.snapidx_ == raft_log_.last_index());

  if (candidate_log_is_current)
  {
    Log_debug("site {} vote for request vote from {}, lastidx {}, lastterm {}", site_id_, can_id, curlstidx, curlstterm);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;

}

// ============================================================================
// ELECTION TIMER: the C++ half of the DSL-owned ElectionTimerLoop
//
// The loop itself -- the while, the campaign branch, the vote wait -- is Rust,
// in the raft_server.election_timer block in server.h. What is left here is
// the set of operations that cannot cross the boundary. Read them as the
// bodies of the lock scopes and external calls that used to be inline in the
// fiber lambda; nothing about the lock discipline changed.
// ============================================================================

// @safe - acquire load, exactly as the inline loop condition read it
bool RaftServer::ElectionLoopStopped() const {
  return stop_.load(rusty::sync::atomic::Ordering::Acquire);
}

// @unsafe - takes mtx_ to read state_.req_voting_
bool RaftServer::ElectionLoopVoting() {
  std::lock_guard<std::mutex> lock(mtx_);
  return state_.req_voting_;
}

// @unsafe - RandomGenerator is external
uint64_t RaftServer::ElectionLoopRandomDelay() const {
  return RandomGenerator::rand(heartbeat_interval_us_ * 2,
                               heartbeat_interval_us_ * 4);
}

// @unsafe - suspends this fiber; unlike a plain Fiber::sleep this is
// interrupted by shutdown, so false means stop rather than timed out.
bool RaftServer::ElectionLoopWait(uint64_t timeout_us) {
  return WaitForElectionTimeoutOrShutdown(timeout_us);
}

// @unsafe - takes mtx_ and reads the whole election cluster in one scope, so
// the Rust loop can branch on copies after the lock is released.
ElectionTick RaftServer::ElectionLoopGather() {
  std::lock_guard<std::mutex> lock(mtx_);
  const uint64_t time_now = Time::now(true);
  const uint64_t heartbeat_time = state_.last_heartbeat_time_;
  const uint64_t time_elapsed = time_now - heartbeat_time;
  const uint64_t election_timeout = state_.election_timeout_us_;
  return ElectionTick::new_(
      time_elapsed, election_timeout, heartbeat_time,
      state_.election_timer_generation_, state_.current_term_,
      static_cast<uint16_t>(state_.vote_for_),
      raft_server_election_timeout_has_fired(state_.is_leader_, time_elapsed,
                                             election_timeout));
}

// @unsafe { rrr logging macro }
void RaftServer::ElectionLoopLogStart() const {
  Log_debug("start timer for election");
}

// @unsafe { rrr logging macro }
void RaftServer::ElectionLoopLogFired(const ElectionTick& tick) const {
  Log_info("[ELECTION_TIMER] Site {}: TIMEOUT FIRED - starting election (elapsed={} > timeout={})",
           site_id_, tick.time_elapsed(), tick.election_timeout());
  Log_info("[ELECTION_START] Site {}: TRIGGERING REQUESTVOTE - time_elapsed={} > timeout={} last_hb={} current_term={} vote_for={}",
           site_id_, tick.time_elapsed(), tick.election_timeout(),
           tick.heartbeat_time(), tick.term(), tick.vote_for());
}

// @unsafe - dispatches through the vtable; the Rust loop re-checks stop_
// immediately before calling, because a collapsed vtable after destruction is
// the hazard this guards.
void RaftServer::ElectionLoopRequestVote(uint64_t generation) {
  RequestVoteFromElectionTimer(generation);
}

// @safe - release store on an atomic
void RaftServer::ElectionLoopSetRunning(bool running) {
  election_loop_running_.store(running, rusty::sync::atomic::Ordering::Release);
}

// The extern "C" trampolines the DSL block declares. Each casts the opaque
// handle back exactly once. This is the only place the cast happens, which is
// what makes "Rust never dereferences the server" a checkable property rather
// than a convention.
extern "C" {

// @unsafe { opaque handle cast }
static inline RaftServer* raft_election_server(rusty::ffi::c_void* server) {
  return static_cast<RaftServer*>(server);
}

bool raft_election_stopped(rusty::ffi::c_void* server) {
  return raft_election_server(server)->ElectionLoopStopped();
}

bool raft_election_is_voting(rusty::ffi::c_void* server) {
  return raft_election_server(server)->ElectionLoopVoting();
}

uint64_t raft_election_random_delay(rusty::ffi::c_void* server) {
  return raft_election_server(server)->ElectionLoopRandomDelay();
}

bool raft_election_wait(rusty::ffi::c_void* server, uint64_t timeout_us) {
  return raft_election_server(server)->ElectionLoopWait(timeout_us);
}

ElectionTick raft_election_gather(rusty::ffi::c_void* server) {
  return raft_election_server(server)->ElectionLoopGather();
}

void raft_election_log_start(rusty::ffi::c_void* server) {
  raft_election_server(server)->ElectionLoopLogStart();
}

void raft_election_log_fired(rusty::ffi::c_void* server,
                             const ElectionTick& tick) {
  raft_election_server(server)->ElectionLoopLogFired(tick);
}

void raft_election_request_vote(rusty::ffi::c_void* server,
                                uint64_t generation) {
  raft_election_server(server)->ElectionLoopRequestVote(generation);
}

void raft_election_set_running(rusty::ffi::c_void* server, bool running) {
  raft_election_server(server)->ElectionLoopSetRunning(running);
}

}  // extern "C"

// @unsafe - Calls undeclared Fiber::create_run()
void RaftServer::StartElectionTimer() {
  ElectionLoopSetRunning(true);
  // @unsafe
  { resetTimer("start election timer"); }

  // Everything that used to be in this lambda now lives in the Rust loop. The
  // lambda captures the loop by value -- it is two words, an opaque pointer
  // and an interval -- so nothing here outlives the fiber.
  const ElectionTimerLoop loop = ElectionTimerLoop::new_(
      static_cast<rusty::ffi::c_void*>(this),
      static_cast<uint64_t>(wait_int_));
  Fiber::create_run([loop]() { loop.run(); });
}

// @unsafe - external calls marked @external [safe], pointer ops in @unsafe blocks
RaftStartResult RaftServer::StartImpl(const janus::Command& cmd,
                                      uint64_t *index,
                                      uint64_t *term,
                                      slotid_t slot_id,
                                      ballot_t ballot) {
  {
  std::lock_guard<std::mutex> lock(mtx_);

  if (!IsLeaderLocked()) {
    // @unsafe
    {
    *index = 0;
    *term = 0;
    }
    return RaftStartResult::REJECTED;
  }
  const RaftStartResult append_result =
      SetLocalAppend(cmd, term, index, slot_id, ballot);
  verify(raft_server_start_was_appended(append_result));
  // SetLocalAppend returns the old raft_log_.last_index() value, but Start returns the
  // index of the newly appended instance
  // @unsafe
  {
  verify(raft_log_.last_index() == (*index) + 1);
  *index = raft_log_.last_index();
  Log_debug("Start(): ldr={} index={} term={}", loc_id_, *index, *term);
  }
  }

  // Publish after releasing mtx_: the wake path never nests the gate's owner
  // mutex below Raft state, and every successful direct Start caller gets the
  // same prompt replication behavior.
  RequestReplication();
  return RaftStartResult::APPENDED;
}

RaftStartResult RaftServer::Start(const janus::Command& cmd,
                                  uint64_t *index,
                                  uint64_t *term,
                                  slotid_t slot_id,
                                  ballot_t ballot) {
  return StartImpl(cmd, index, term, slot_id, ballot);
}

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server
 * should we exclude the execution of this function for leader? */
// @unsafe - external calls marked @external [safe], output pointer writes in @unsafe blocks
void RaftServer::OnAppendEntries(const slotid_t slot_id,
                                 const ballot_t ballot,
                                 const uint64_t leaderCurrentTerm,
                                 const siteid_t leaderSiteId,
                                 const uint64_t leaderPrevLogIndex,
                                 const uint64_t leaderPrevLogTerm,
                                 const uint64_t leaderCommitIndex,
                                 const janus::Command& cmd,
                                 const uint64_t leaderNextLogTerm, // disabled in batched version (term recorded in the TpcCommitCommand)
                                 uint64_t *followerAppendOK,
                                 uint64_t *followerCurrentTerm,
                                 uint64_t *followerLastLogIndex) {
  std::unique_lock<std::mutex> lock(mtx_);

  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    *followerAppendOK = 0;
    *followerCurrentTerm = state_.current_term_;
    *followerLastLogIndex = raft_log_.last_index();
    return;
  }

  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool leader_has_higher_term =
      raft_server_observed_higher_term(leaderCurrentTerm, state_.current_term_);
  const bool leader_term_is_stale =
      raft_server_vote_term_is_stale(leaderCurrentTerm, state_.current_term_);
  const bool sender_is_current_voter =
      leaderSiteId != invalid && leaderSiteId != site_id_ &&
      current_config_.count(leaderSiteId) != 0;
  const bool sender_is_self = leaderSiteId == site_id_;
  const bool has_known_leader = state_.current_leader_id_ != invalid;
  const bool known_leader_matches_sender =
      state_.current_leader_id_ == leaderSiteId;
  if (!sender_is_current_voter || leader_term_is_stale ||
      !raft_server_leader_rpc_sender_is_authoritative(
          leader_has_higher_term, state_.is_leader_, sender_is_self,
          has_known_leader, known_leader_matches_sender)) {
    Log_warn("[APPEND_REJECT] Site {} rejecting unauthoritative "
             "AppendEntries sender {} term {} (local_term={} leader={} "
             "known_leader={} voter={})",
             site_id_, leaderSiteId, leaderCurrentTerm, state_.current_term_,
             state_.is_leader_, state_.current_leader_id_, sender_is_current_voter);
    *followerAppendOK = 0;
    *followerCurrentTerm = state_.current_term_;
    *followerLastLogIndex = raft_log_.last_index();
    return;
  }

  // Validate the encoded entry count before touching any log slot. Empty
  // TpcBatchCommand payloads and additions that would wrap the absolute Raft
  // index are protocol rejections, not zero-entry heartbeats.
  bool append_payload_valid = true;
  uint64_t encoded_entry_count = cmd.has_value() ? 1 : 0;
#ifdef RAFT_BATCH_OPTIMIZATION
  if (cmd.has_value() && raft_server_append_command_is_batch(
          cmd.kind_, TpcBatchCommand::static_kind())) {
    const auto batch = marshallable_cast<TpcBatchCommand>(cmd);
    if (batch.is_none()) {
      append_payload_valid = false;
    } else {
      encoded_entry_count =
          static_cast<uint64_t>(batch.as_ref().unwrap()->cmds_.size());
      append_payload_valid = raft_server_append_batch_count_is_valid(
          leaderPrevLogIndex, encoded_entry_count);
    }
  } else if (cmd.has_value()) {
    append_payload_valid = raft_server_append_entry_count_fits(
        leaderPrevLogIndex, encoded_entry_count);
  }
#else
  append_payload_valid = raft_server_append_entry_count_fits(
      leaderPrevLogIndex, encoded_entry_count);
#endif

  bool term_ok = raft_server_append_term_is_acceptable(
      leaderCurrentTerm, this->state_.current_term_);
  const bool compacted_prefix_miss =
      (leaderPrevLogIndex != 0 &&
       leaderPrevLogIndex < raft_log_.base() &&
       leaderPrevLogIndex != state_.snapidx_);
  bool index_ok = (leaderPrevLogIndex <= raft_log_.last_index()) && !compacted_prefix_miss;
  uint64_t local_prev_term = 0;
  if (leaderPrevLogIndex == 0) {
      local_prev_term = 0;
  } else if (leaderPrevLogIndex == state_.snapidx_) {
      // Snapshot boundary is still valid even when log entries are compacted.
      local_prev_term = state_.snapterm_;
  } else if (leaderPrevLogIndex <= raft_log_.last_index() && !compacted_prefix_miss) {
      // THE LOG-MATCHING CHECK. A follower legitimately may not hold
      // leaderPrevLogIndex -- discovering that is the whole point, and what
      // drives the leader's backtracking.
      //
      // This used GetRaftInstance, which default-inserted, so an absent entry
      // was FABRICATED with term 0 and then compared against
      // leaderPrevLogTerm. The `prev_instance ? ... : 0` below was dead for
      // the same reason: that function cannot return null. Within the
      // <= last_log_index_ guard and the compacted-prefix check a gap should
      // not arise, so this is not a known live divergence -- but the
      // correctness of the check rested on "there are no gaps", while the
      // function used to perform it was the only thing able to create one.
      //
      // FindRaftInstance does not insert, so absence now falls through to
      // local_prev_term = 0 without mutating the log, and the mismatch is
      // reported rather than manufactured.
      auto prev_instance = FindRaftInstance(leaderPrevLogIndex);
      local_prev_term = prev_instance ? prev_instance->term() : 0;
  }
  bool prev_term_ok = (leaderPrevLogIndex == 0 || local_prev_term == leaderPrevLogTerm);

  // Only log rejections or when cmd is present (actual log entries)
  if (!term_ok || !index_ok || !prev_term_ok || cmd.has_value()) {
  }

  // CRITICAL FIX: Reset timer if we hear from a current-term leader, even if log conflicts
  // This prevents followers with divergent logs from constantly starting elections
  // while the leader is trying to repair their log via backtracking
  if (term_ok) {
      if (raft_server_observed_higher_term(
              leaderCurrentTerm, this->state_.current_term_)) {
          auto prev_term = state_.current_term_;
          state_.current_term_ = leaderCurrentTerm;
          state_.vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          // Publish the accepted leader before a possible leader-change
          // callback observes the follower transition.
          state_.current_leader_id_ = raft_server_leader_hint_after_transition(
              false, true, site_id_, leaderSiteId);

          LogTermChange("AppendEntries leader term is newer", prev_term, state_.current_term_, leaderSiteId);
          Log_debug("server {}, set to be follower", loc_id_ ) ;
          if (state_.is_leader_) {
            // Use the central transition so no leadership state survives an
            // accepted competing leader epoch.
            stepDown();
          } else {
            setIsLeader(false);
          }
          state_.req_voting_ = false;
          state_.election_in_progress_ = false;
      }
      // Refresh the validated leader hint for current-term contact too. A
      // higher-term sender was already published before its role transition.
      state_.current_leader_id_ = raft_server_leader_hint_after_transition(
          false, true, site_id_, leaderSiteId);
      // @unsafe
      { resetTimerLocked("AppendEntries from current-term leader"); }
  }

  if (raft_server_append_is_acceptable(term_ok, index_ok, prev_term_ok) &&
      append_payload_valid) {
      Log_debug("refresh timer on appendentry");

      // Any accepted leader RPC establishes follower state even when it is in
      // our current term. Cancel an outstanding election before its delayed
      // result can promote this server after the accepted AppendEntries.
      if (state_.is_leader_) {
        stepDown();
      } else {
        setIsLeader(false);
      }
      state_.req_voting_ = false;
      state_.election_in_progress_ = false;

      // ==================================================================
      // SPECULATIVE REPLICATION: Append to memory and respond immediately.
      // ==================================================================

      // Decode the complete wire payload before mutating the local log.
      std::vector<std::pair<slotid_t, RaftEntry>> incoming_entries;
      const uint64_t old_last_log_index = raft_log_.last_index();
      const uint64_t accepted_through = cmd.has_value()
          ? raft_server_append_sent_end(
                leaderPrevLogIndex, encoded_entry_count)
          : raft_server_append_sent_end(leaderPrevLogIndex, 0);

      if (cmd.has_value()) {
#ifndef RAFT_BATCH_OPTIMIZATION
        incoming_entries.push_back(
            {accepted_through, RaftEntry::new_(leaderNextLogTerm, cmd)});
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
        if (raft_server_append_command_is_batch(
                cmd.kind_, TpcBatchCommand::static_kind())) {
          const auto cmds = marshallable_cast<TpcBatchCommand>(cmd);
          verify(cmds.is_some());
          uint64_t cnt = 0;
          for (const rusty::Arc<TpcCommitCommand>& c : cmds.unwrap()->cmds_) {
            ++cnt;
            const uint64_t index = raft_server_append_sent_end(
                leaderPrevLogIndex, cnt);
            incoming_entries.push_back(
                {index, RaftEntry::new_(c->term, c.clone())});
          }
        } else {
          // Batch optimization is a wire optimization, not a restriction on
          // the Raft log's command type. Application commands travel as one
          // raw entry with their explicit wire term.
          incoming_entries.push_back(
              {accepted_through, RaftEntry::new_(leaderNextLogTerm, cmd)});
        }
#endif
        verify(incoming_entries.size() == encoded_entry_count);
      }

      // Raft's conflict rule is deliberately narrower than "replace through
      // the RPC end".  Concurrent RPCs can complete out of order: if an older
      // payload is already identical through its end, the follower must keep
      // any newer suffix it has since accepted.  Only the first missing or
      // term-conflicting payload slot starts an overwrite.
      bool have_first_write = false;
      bool truncate_suffix = false;
      uint64_t first_write_index = 0;
      for (const auto& [index, incoming] : incoming_entries) {
        const RaftEntry* local = FindRaftInstance(index);
        const bool local_exists =
            local != nullptr && local->cmd().has_value();
        const uint64_t local_term = local_exists ? local->term() : 0;
        if (raft_server_append_entry_conflicts(
                local_exists, local_term, incoming.term())) {
          have_first_write = true;
          first_write_index = index;
          truncate_suffix = index <= old_last_log_index;
          break;
        }
      }

      if (truncate_suffix &&
          first_write_index <= std::max(state_.commit_index_, state_.execute_index_)) {
        // A legitimate leader can never conflict with a committed entry.  Do
        // not let malformed or internally inconsistent input rewrite applied
        // state; reject it before memory changes.
        Log_error("[APPEND_REJECT] Site {} refusing conflict at committed "
                  "index {} (state_.commit_index_={}, state_.execute_index_={}, oldLast={})",
                  site_id_, first_write_index, state_.commit_index_, state_.execute_index_,
                  old_last_log_index);
        *followerAppendOK = 0;
        *followerCurrentTerm = this->state_.current_term_;
        *followerLastLogIndex = raft_log_.last_index();
        return;
      }

      if (have_first_write) {
        // Raft's conflict rule, as two operations that cannot leave a hole:
        // drop the divergent suffix, then re-append in index order.
        // truncate_from is a no-op when first_write_index is already past the
        // tail, which is the ordinary extend case (truncate_suffix false).
        raft_log_.truncate_from(first_write_index);
        for (auto& [index, incoming] : incoming_entries) {
          if (index >= first_write_index) {
            const uint64_t appended = raft_log_.append(std::move(incoming));
            verify(appended == index);
          }
        }
      }
      // Was an assignment to raft_log_.last_index() computed by
      // raft_server_append_result_last_index. The container reaches exactly
      // that value on its own: truncate_from + append leaves the tail at
      // accepted_through when anything was written, and untouched otherwise.
      verify(raft_log_.last_index() == raft_server_append_result_last_index(
          old_last_log_index, accepted_through, truncate_suffix));

      // Advance commit index and enqueue committed entries for background apply.
      const uint64_t follower_commit_candidate =
          raft_server_commit_index_clamp(
              leaderCommitIndex, accepted_through);
      if (raft_server_log_index_above(
              follower_commit_candidate, state_.commit_index_)) {
        auto old_commit = state_.commit_index_;
        state_.commit_index_ = follower_commit_candidate;
        verify(raft_log_.last_index() >= state_.commit_index_);
        EnqueueCommittedEntries(old_commit, state_.commit_index_);
      }

      // @unsafe
      {
      *followerAppendOK = 1;
      *followerCurrentTerm = this->state_.current_term_;
      // On success this field is the inclusive end proved by this call, not
      // the follower's possibly longer and divergent local suffix. Rejections
      // below retain local raft_log_.last_index() as a backoff hint.
      *followerLastLogIndex = accepted_through;
      }
    }
    else {
        Log_info("[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} payload_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})",
                 site_id_, leaderSiteId, term_ok, index_ok, prev_term_ok,
                 append_payload_valid, leaderCurrentTerm, state_.current_term_,
                 leaderPrevLogIndex, raft_log_.last_index(), local_prev_term);
        // @unsafe
        {
        *followerAppendOK = 0;
        *followerCurrentTerm = this->state_.current_term_;
        *followerLastLogIndex = raft_log_.last_index();
        }
    }

/*if (rand() % 1000 == 0) {
	usleep(25*1000);
}*/

    lock.unlock();
}


// @unsafe - Stores callback for later invocation
void RaftServer::RegisterLeaderChangeCallback(std::function<void(bool)> cb) {
  leader_change_cb_ = std::move(cb);
}

// ============================================================================
// InstallSnapshot RPC Handler
// ============================================================================

// @unsafe - Modifies log state, snapshot metadata, calls snapshot_manager_
void RaftServer::OnInstallSnapshot(const uint64_t term,
                                    const uint64_t leader_id,
                                    const uint64_t last_included_index,
                                    const uint64_t last_included_term,
                                    const std::string& data,
                                    uint64_t* term_out) {
  // Snapshot state-machine replacement must not overlap entry application or
  // recovery replay. The global order is apply gate -> Raft state -> queue.
  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::mutex> lock(mtx_);

  // @unsafe
  { *term_out = 0; }

  try {

  // ============================================================================
  // Edge Case 0: Server shutting down
  // ============================================================================
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Ignoring InstallSnapshot - server shutting down", site_id_);
    return;
  }

  // ============================================================================
  // Edge Case 1: Stale term - reject
  // ============================================================================
  if (term < state_.current_term_) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Rejecting InstallSnapshot from leader {} "
             "(leader_term={} < my_term={})",
             site_id_, leader_id, term, state_.current_term_);
    *term_out = state_.current_term_;
    return;
  }

  // A leader cannot have committed an entry from a term that has not happened
  // yet. This is a malformed snapshot boundary, not usable Raft leader
  // evidence. Reject it with the unavailable sentinel before authenticating
  // the sender, stepping down, resetting the timer, or touching payload state.
  if (!raft_server_snapshot_term_is_valid(last_included_term, term)) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Rejecting impossible snapshot "
              "boundary term {} from leader {} in term {}",
              site_id_, last_included_term, leader_id, term);
    return;
  }

  if (leader_id > static_cast<uint64_t>(
                      std::numeric_limits<siteid_t>::max())) {
    Log_warn("[INSTALL-SNAPSHOT] Site {} rejected unrepresentable leader "
             "identity {} in term {}",
             site_id_, leader_id, term);
    return;
  }
  const siteid_t leader_site = static_cast<siteid_t>(leader_id);
  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool sender_is_current_voter =
      leader_site != invalid && leader_site != site_id_ &&
      current_config_.count(leader_site) != 0;
  const bool leader_has_higher_term =
      raft_server_observed_higher_term(term, state_.current_term_);
  const bool sender_is_self = leader_site == site_id_;
  const bool has_known_leader = state_.current_leader_id_ != invalid;
  const bool known_leader_matches_sender =
      state_.current_leader_id_ == leader_site;
  if (!sender_is_current_voter ||
      !raft_server_leader_rpc_sender_is_authoritative(
          leader_has_higher_term, state_.is_leader_, sender_is_self,
          has_known_leader, known_leader_matches_sender)) {
    Log_warn("[INSTALL-SNAPSHOT] Site {} rejected unauthoritative leader {} "
             "in term {} (local_term={} leader={} known_leader={} voter={})",
             site_id_, leader_id, term, state_.current_term_, state_.is_leader_,
             state_.current_leader_id_, sender_is_current_voter);
    return;
  }

  // ============================================================================
  // Edge Case 2: Higher or equal term - accept as legitimate leader
  // ============================================================================
  const uint64_t previous_term = state_.current_term_;
  if (leader_has_higher_term) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Leader {} has higher term ({} > {}) - updating",
             site_id_, leader_id, term, state_.current_term_);
    state_.current_term_ = term;
    // @unsafe
    {
    state_.vote_for_ = INVALID_SITEID;
    }
  }

  // InstallSnapshot comes from a known leader. Publish its identity before a
  // possible leader-to-follower callback observes the role transition.
  state_.current_leader_id_ = raft_server_leader_hint_after_transition(
      false, true, site_id_, leader_site);

  // Any accepted leader RPC, including one in our current term, establishes
  // follower state. Cancel the outstanding election as well as leadership;
  // RequestVote's delayed-success path revalidates this ownership before it
  // can promote the server again.
  if (state_.is_leader_) {
    stepDown();
  } else {
    setIsLeader(false);
  }
  state_.req_voting_ = false;
  state_.election_in_progress_ = false;

  if (leader_has_higher_term) {
    LogTermChange("InstallSnapshot carried newer term", previous_term,
                  state_.current_term_, leader_site);
  }

  // Reset election timer (legitimate leader contact)
  resetTimerLocked("received InstallSnapshot");
  // From here, state_.current_term_ denotes an accepted current-term leader contact.
  // Individual install failures overwrite this with zero so the caller never
  // advances match/next on an unavailable boundary.
  *term_out = state_.current_term_;

  // A current-term leader may retry a snapshot after this follower has already
  // committed, applied, or snapshotted through its boundary. Acknowledge that
  // leader contact but do not roll any local snapshot/log/application state
  // backward and do not install the stale payload.
  uint64_t local_progress_index = state_.commit_index_;
  local_progress_index = std::max(local_progress_index, state_.execute_index_);
  local_progress_index = std::max(local_progress_index, GetAppliedIndex());
  local_progress_index = std::max(local_progress_index, state_.snapidx_);
  if (last_included_index == state_.snapidx_ && state_.snapidx_ != 0 &&
      last_included_term != state_.snapterm_) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: rejecting snapshot boundary "
              "({}, {}) that conflicts with local snapshot ({}, {})",
              site_id_, last_included_index, last_included_term,
              state_.snapidx_, state_.snapterm_);
    *term_out = 0;
    return;
  }
  if (raft_server_snapshot_is_stale(
          last_included_index, local_progress_index)) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Snapshot index {} is already covered "
             "(commit={} execute={} applied={} snapidx={}); acknowledging no-op",
             site_id_, last_included_index, state_.commit_index_, state_.execute_index_,
             GetAppliedIndex(), state_.snapidx_);
    return;
  }
  if (!raft_server_log_index_has_successor(last_included_index)) {
    // raft_log_.base() requires S + 1. Reaching UINT64_MAX exhausts the Raft
    // log index space, so reject the payload without wrapping the value.
    Log_error("[INSTALL-SNAPSHOT] Site {}: Cannot install terminal snapshot "
              "index {}; no successor index is representable",
              site_id_, last_included_index);
    // The leader callback uses zero as an unavailable/failed response and
    // therefore leaves match_index/next_index unchanged.
    // @unsafe
    { *term_out = 0; }
    return;
  }

  if (!snapshot_manager_) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Cannot install snapshot at index {} "
              "without configured snapshot storage",
              site_id_, last_included_index);
    // @unsafe
    { *term_out = 0; }
    return;
  }

  // Complete every fallible observation used by the retention decision before
  // the application loader can replace external state. Use find(), not
  // PutRaftInstance(), and require a decoded command so a synthesized empty
  // RaftEntry can never prove the snapshot boundary.
  const RaftEntry* boundary = FindRaftInstance(last_included_index);
  const bool has_boundary =
      boundary != nullptr && boundary->cmd().has_value();
  const ballot_t local_boundary_term =
      has_boundary ? boundary->term() : 0;
  const bool retain_suffix = raft_server_snapshot_boundary_matches(
      has_boundary, local_boundary_term, last_included_term);
  const slotid_t previous_last_log_index = raft_log_.last_index();

  // Fully validate and stage the exact state-machine image before changing
  // either recovery point. The owned transaction's destructor aborts
  // this private staging image, so rejection leaves the live state machine,
  // latest Raft snapshot, and reconstruction log untouched.
  Log_info("[INSTALL-SNAPSHOT] Site {}: Preparing state machine snapshot ({} bytes)",
           site_id_, data.size());
  auto prepared_state_machine = PrepareStateMachineSnapshotLocked(
      data, last_included_index, last_included_term);
  if (prepared_state_machine == nullptr) {
    *term_out = 0;
    return;
  }

  // ============================================================================
  // Save snapshot data via snapshot_manager_
  // ============================================================================
  // @unsafe { snapshot_manager_ I/O operations }
  const bool saved = snapshot_manager_->TakeSnapshot(
      last_included_index, last_included_term,
      data.data(), data.size());
  if (!saved) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              site_id_, last_included_index, last_included_term);
    // The transaction has not committed, so its destructor discards only the
    // private staging image. The old live state machine and log remain usable.
    { *term_out = 0; }
    return;
  }
  Log_info("[INSTALL-SNAPSHOT] Site {}: Snapshot saved at index={} term={}",
           site_id_, last_included_index, last_included_term);

  // SnapshotManager is now the authority for this boundary. Publish the staged
  // application image only afterward.
  if (!prepared_state_machine->Commit()) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to commit prepared state "
              "machine snapshot at index={} term={}; failing stop",
              site_id_, last_included_index, last_included_term);
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    apply_thread_running_.store(false);
    *term_out = 0;
    return;
  }
  Log_info("[INSTALL-SNAPSHOT] Site {}: State machine committed at index={} "
           "after Raft snapshot publication",
           site_id_, last_included_index);

  // ============================================================================
  // Update snapshot metadata
  // ============================================================================
  state_.snapidx_ = last_included_index;
  state_.snapterm_ = last_included_term;
  snapshot_trigger_index_.store(
      state_.snapidx_, rusty::sync::atomic::Ordering::Release);

  // ============================================================================
  // Reconcile in-memory log and queued application work
  // ============================================================================
  if (retain_suffix) {
    raft_log_.compact_through(last_included_index);
  } else {
    raft_log_.reset(last_included_index + 1);
  }

  size_t purged_apply_entries = 0;
  {
    std::lock_guard<std::mutex> queue_lock(apply_queue_mtx_);
    if (retain_suffix) {
      auto queued = apply_queue_.begin();
      while (queued != apply_queue_.end()) {
        if (raft_server_log_index_at_or_below(
                queued->index, last_included_index)) {
          queued = apply_queue_.erase(queued);
          purged_apply_entries++;
        } else {
          ++queued;
        }
      }
    } else {
      // Also invalidates an entry that the apply thread popped before this
      // queue clear. It rechecks the captured epoch while holding the outer
      // state-machine gate before invoking the callback.
      apply_queue_epoch_++;
      purged_apply_entries = apply_queue_.size();
      apply_queue_.clear();
    }
  }

  // Update raft_log_.base() to reflect compacted log


  // ============================================================================
  // Advance state_.commit_index_ and state_.execute_index_
  // ============================================================================
  state_.commit_index_ = last_included_index;
  verify(state_.commit_index_ <= raft_log_.last_index());

  // Publish application only after the state machine has finished loading the
  // snapshot. Acquire waiters must never observe the covered indices early.
  PublishAppliedIndexLocked(last_included_index);

  Log_info("[INSTALL-SNAPSHOT] Site {}: Installed snapshot from leader {} "
           "(snapidx={}, snapterm={}, state_.commit_index_={}, state_.execute_index_={}, "
           "raft_log_.last_index()={}, retain_suffix={}, purged_apply={})",
           site_id_, leader_id, state_.snapidx_, state_.snapterm_, state_.commit_index_, state_.execute_index_,
           raft_log_.last_index(), retain_suffix, purged_apply_entries);
  } catch (const std::exception& error) {
    Log_error("[INSTALL-SNAPSHOT] Site {} threw while installing snapshot: {}",
              site_id_, error.what());
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    apply_thread_running_.store(false);
    *term_out = 0;
  } catch (...) {
    Log_error("[INSTALL-SNAPSHOT] Site {} threw while installing snapshot",
              site_id_);
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    apply_thread_running_.store(false);
    *term_out = 0;
  }
}

// @unsafe - Calls Setup if not already initialized
void RaftServer::EnsureSetup() {
  if (heartbeat_setup_) {
    return;
  }
  heartbeat_setup_ = true;
  Setup();
}

// ============================================================================
// stepDown - Central leader step-down function
// ============================================================================

void RaftServer::stepDown() {
  // Must be called with mtx_ held (caller's responsibility)
  // Most callers already hold the lock

  Log_info("[SPEC-RAFT] Site {}: Stepping down as leader (term={})",
           site_id_, state_.current_term_);

  // Transition to follower state
  // This handles the leadership-change callback, timer resets, etc.
  setIsLeader(false);

  // A late higher-term response can arrive after this server has already
  // entered a new candidacy. Demotion is terminal for that election as well
  // as for the old leadership epoch.
  state_.req_voting_ = false;
  state_.election_in_progress_ = false;

  // Reset election timer
  // Important: Give other servers time to elect a new leader
  // stepDown takes no lock of its own; every caller holds mtx_.
  resetTimerLocked("stepDown");

  Log_info("[SPEC-RAFT] Site {}: Step-down complete, now follower", site_id_);

}

// ============================================================================
// MEMBERSHIP CONFIGURATION
// ============================================================================




} // namespace janus
