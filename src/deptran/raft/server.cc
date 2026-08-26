#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <rusty/slice.hpp>
#include <rusty/mutex.hpp>
#include <rusty/sync/atomic.hpp>

#include "server.h"
// #include "paxos_worker.h"
#include "frame.h"
#include "../legacy_raft_log_payload.h"
#include "../tpc_command.h"
#include "file_snapshot_manager.hpp"
#include "quorum.hpp"
#include "replicated_db.h"

import std;

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

// @unsafe - Small reactor bridge.  Cross-thread callers touch only rusty
// atomics and mutex-protected handles; the IntEvent operations themselves are
// confined to the bound PollThread owner.
class ReplicationWakeGate final {
 public:
  // @safe - Initializes Rust-style synchronization primitives.
  ReplicationWakeGate()
      : owner_(rusty::None),
        waiter_(rusty::None),
        election_waiter_(rusty::None),
        pending_(false),
        waiter_armed_(false),
        election_waiter_armed_(false),
        wake_job_queued_(false),
        shutdown_job_queued_(false),
        accepting_(true) {}

  // @unsafe - Mutex unwrap is the audited RustyCpp lock boundary.
  void BindOwner(rusty::Arc<rrr::PollThread> owner) const {
    auto guard = owner_.lock().unwrap();
    *guard = rusty::Some(std::move(owner));
    accepting_.store(true, rusty::sync::atomic::Ordering::Release);
  }

  // @safe - Publishes level-triggered work from any thread.
  bool Publish() const {
    pending_.store(true, rusty::sync::atomic::Ordering::Release);
    return accepting_.load(rusty::sync::atomic::Ordering::Acquire);
  }

  // @safe - Begins shutdown while retaining a pending level for an armed wait.
  void Close() const {
    accepting_.store(false, rusty::sync::atomic::Ordering::Release);
    pending_.store(true, rusty::sync::atomic::Ordering::Release);
  }

  // @unsafe - Mutex unwrap is the audited RustyCpp lock boundary.
  void ClearOwner() const {
    auto guard = owner_.lock().unwrap();
    *guard = rusty::None;
  }

  // @unsafe - Atomically reserves one owner job, then clones the owner handle
  // under its RustyCpp mutex.  No RaftServer lock is acquired here.
  rusty::Option<rusty::Arc<rrr::PollThread>> ReserveWakeOwner() const {
    if (!waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire)) {
      return rusty::None;
    }

    auto guard = owner_.lock().unwrap();
    // Reserve while holding owner_: Close may clear the stored owner only
    // after QueueReplicationWake returns.  Thus a concurrent publisher either
    // leaves a queued-job reservation backed by an owner clone, or Close gets
    // to enqueue the shutdown wake itself.
    if (wake_job_queued_.swap(
            true, rusty::sync::atomic::Ordering::AcqRel)) {
      return rusty::None;
    }
    if (guard->is_none()) {
      wake_job_queued_.store(
          false, rusty::sync::atomic::Ordering::Release);
      return rusty::None;
    }
    return guard->clone();
  }

  // @unsafe - Reserves the one shutdown job while holding owner_, closing the
  // race between a concurrent publisher's owner clone and ClearOwner().
  rusty::Option<rusty::Arc<rrr::PollThread>>
  ReserveShutdownWakeOwner() const {
    if (!waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire) &&
        !election_waiter_armed_.load(
            rusty::sync::atomic::Ordering::Acquire)) {
      return rusty::None;
    }

    auto guard = owner_.lock().unwrap();
    if (shutdown_job_queued_.swap(
            true, rusty::sync::atomic::Ordering::AcqRel)) {
      return rusty::None;
    }
    if (guard->is_none()) {
      shutdown_job_queued_.store(
          false, rusty::sync::atomic::Ordering::Release);
      return rusty::None;
    }
    return guard->clone();
  }

  // @unsafe - Runs only on the bound PollThread.  Clone the event before set:
  // set may make the heartbeat fiber runnable, and that fiber clears waiter_.
  void WakeOnOwner() const {
    if (!waiter_armed_.load(rusty::sync::atomic::Ordering::Acquire)) {
      wake_job_queued_.store(
          false, rusty::sync::atomic::Ordering::Release);
      return;
    }

    rusty::Option<rusty::Arc<IntEvent>> waiter = rusty::None;
    {
      auto guard = waiter_.lock().unwrap();
      waiter = guard->clone();
    }
    if (waiter.is_some()) {
      waiter.as_ref().unwrap()->set(1);
    }
  }

  // @unsafe - Runs only on the bound PollThread.  Shutdown has a separate job
  // reservation so a normal replication wake cannot mask the election wake.
  void WakeShutdownOnOwner() const {
    rusty::Option<rusty::Arc<IntEvent>> heartbeat_waiter = rusty::None;
    rusty::Option<rusty::Arc<IntEvent>> election_waiter = rusty::None;
    {
      auto guard = waiter_.lock().unwrap();
      heartbeat_waiter = guard->clone();
    }
    {
      auto guard = election_waiter_.lock().unwrap();
      election_waiter = guard->clone();
    }
    if (heartbeat_waiter.is_some()) {
      heartbeat_waiter.as_ref().unwrap()->set(1);
    }
    if (election_waiter.is_some()) {
      election_waiter.as_ref().unwrap()->set(1);
    }
  }

  // @unsafe - Must run on the bound PollThread.  The two pending exchanges
  // bracket waiter publication, so a publisher can never fall between the
  // fast-path check and arming the IntEvent.
  bool WaitForWork(uint64_t timeout_us) const {
    if (!accepting_.load(rusty::sync::atomic::Ordering::Acquire)) {
      return false;
    }
    if (pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel)) {
      return accepting_.load(rusty::sync::atomic::Ordering::Acquire);
    }

    auto waiter = create_sp_int_event(1);
    waiter->set(0);
    {
      auto guard = waiter_.lock().unwrap();
      *guard = rusty::Some(waiter.clone());
    }
    waiter_armed_.store(true, rusty::sync::atomic::Ordering::Release);

    if (pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel)) {
      DisarmWaiter();
      return accepting_.load(rusty::sync::atomic::Ordering::Acquire);
    }

    waiter->wait_timeout(timeout_us);
    // Consume the work represented by this completed wait before disarming.
    // A publisher that races after this exchange either sees the armed waiter
    // (and queues/wakes it) or sees it disarmed; in both cases its pending bit
    // remains latched for the next round. Disarming first would leave a window
    // where a publisher sees waiter_armed_ == false and this exchange erases
    // its otherwise-unqueued notification.
    pending_.swap(false, rusty::sync::atomic::Ordering::AcqRel);
    DisarmWaiter();
    return accepting_.load(rusty::sync::atomic::Ordering::Acquire);
  }

  // @unsafe - Owner-thread-only interruptible election delay.  Close is
  // rechecked after publishing the waiter to prevent a lost shutdown wake.
  bool WaitForElectionTimeout(uint64_t timeout_us) const {
    if (!accepting_.load(rusty::sync::atomic::Ordering::Acquire)) {
      return false;
    }

    auto waiter = create_sp_int_event(1);
    waiter->set(0);
    {
      auto guard = election_waiter_.lock().unwrap();
      *guard = rusty::Some(waiter.clone());
    }
    election_waiter_armed_.store(
        true, rusty::sync::atomic::Ordering::Release);

    if (!accepting_.load(rusty::sync::atomic::Ordering::Acquire)) {
      DisarmElectionWaiter();
      return false;
    }

    waiter->wait_timeout(timeout_us);
    DisarmElectionWaiter();
    return accepting_.load(rusty::sync::atomic::Ordering::Acquire);
  }

 private:
  // @unsafe - Owner-thread-only waiter teardown under its RustyCpp mutex.
  void DisarmWaiter() const {
    waiter_armed_.store(false, rusty::sync::atomic::Ordering::Release);
    {
      auto guard = waiter_.lock().unwrap();
      *guard = rusty::None;
    }
    // Keep the reservation set through IntEvent::set and until the owner
    // disarms.  A saturated submit burst therefore queues at most one wake job
    // for this wait rather than one job per entry.
    wake_job_queued_.store(
        false, rusty::sync::atomic::Ordering::Release);
  }

  // @unsafe - Owner-thread-only election waiter teardown.
  void DisarmElectionWaiter() const {
    election_waiter_armed_.store(
        false, rusty::sync::atomic::Ordering::Release);
    auto guard = election_waiter_.lock().unwrap();
    *guard = rusty::None;
  }

  rusty::Mutex<rusty::Option<rusty::Arc<rrr::PollThread>>> owner_;
  rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>> waiter_;
  rusty::Mutex<rusty::Option<rusty::Arc<IntEvent>>> election_waiter_;
  rusty::sync::atomic::AtomicBool pending_;
  rusty::sync::atomic::AtomicBool waiter_armed_;
  rusty::sync::atomic::AtomicBool election_waiter_armed_;
  rusty::sync::atomic::AtomicBool wake_job_queued_;
  rusty::sync::atomic::AtomicBool shutdown_job_queued_;
  rusty::sync::atomic::AtomicBool accepting_;
};

namespace {

constexpr uint64_t kInstallSnapshotCallbackDrainBit = uint64_t{1} << 63;
constexpr uint64_t kInstallSnapshotCallbackCountMask =
    ~kInstallSnapshotCallbackDrainBit;

static_assert(raft_server_callback_gate_is_open(
    0, kInstallSnapshotCallbackDrainBit));
static_assert(!raft_server_callback_gate_is_open(
    kInstallSnapshotCallbackDrainBit,
    kInstallSnapshotCallbackDrainBit));
static_assert(raft_server_callback_gate_count(
                  kInstallSnapshotCallbackDrainBit | 3,
                  kInstallSnapshotCallbackCountMask) == 3);

}  // namespace

// @unsafe - Independently owned lifetime gate for fire-and-forget snapshot
// futures. A callback increments the packed borrower count before loading the
// raw server pointer. Close linearizes against that increment, clears the
// pointer, and lets shutdown wait only for callbacks that can touch the server.
class InstallSnapshotCallbackGate final {
 public:
  // @unsafe - Publishes a server pointer whose lifetime is drained by Close.
  explicit InstallSnapshotCallbackGate(RaftServer* server)
      : server_(server), state_(0) {}

  // @unsafe - Returns a borrowed pointer protected by one packed-state count.
  // The caller must invoke Release after its final server access.
  RaftServer* TryAcquire() const {
    auto admitted = state_.fetch_update(
        rusty::sync::atomic::Ordering::AcqRel,
        rusty::sync::atomic::Ordering::Acquire,
        [](uint64_t state) -> rusty::Option<uint64_t> {
          if (!raft_server_callback_gate_is_open(
                  state, kInstallSnapshotCallbackDrainBit)) {
            return rusty::None;
          }
          verify(raft_server_callback_gate_count(
                     state, kInstallSnapshotCallbackCountMask) <
                 kInstallSnapshotCallbackCountMask);
          return rusty::Some(state + 1);
        });
    if (admitted.is_err()) {
      return nullptr;
    }

    RaftServer* server =
        server_.load(rusty::sync::atomic::Ordering::Acquire);
    if (server == nullptr) {
      Release();
    }
    return server;
  }

  // @unsafe - Release-publishes completion of all server accesses.
  void Release() const {
    const uint64_t previous = state_.fetch_sub(
        1, rusty::sync::atomic::Ordering::Release);
    verify(raft_server_callback_gate_count(
               previous, kInstallSnapshotCallbackCountMask) > 0);
  }

  // @safe - Closes future admission before clearing the raw pointer.
  void Close() const {
    state_.fetch_or(kInstallSnapshotCallbackDrainBit,
                    rusty::sync::atomic::Ordering::AcqRel);
    server_.store(nullptr, rusty::sync::atomic::Ordering::Release);
  }

  // @safe - Acquire-load used by the shutdown completion barrier.
  uint64_t ActiveCallbacks() const {
    return raft_server_callback_gate_count(
        state_.load(rusty::sync::atomic::Ordering::Acquire),
        kInstallSnapshotCallbackCountMask);
  }

 private:
  rusty::sync::atomic::AtomicPtr<RaftServer> server_;
  rusty::sync::atomic::AtomicU64 state_;
};

// @unsafe - Stack RAII proof for one callback invocation. The owned gate Arc
// remains valid even when the future itself outlives RaftServer.
class InstallSnapshotCallbackLease final {
 public:
  explicit InstallSnapshotCallbackLease(
      rusty::Arc<InstallSnapshotCallbackGate> gate)
      : gate_(std::move(gate)), server_(gate_->TryAcquire()) {}

  ~InstallSnapshotCallbackLease() {
    if (server_ != nullptr) {
      server_ = nullptr;
      gate_->Release();
    }
  }

  InstallSnapshotCallbackLease(
      const InstallSnapshotCallbackLease&) = delete;
  InstallSnapshotCallbackLease& operator=(
      const InstallSnapshotCallbackLease&) = delete;

  // @safe - Valid only for this RAII object's lexical lifetime.
  RaftServer* get() const { return server_; }

 private:
  rusty::Arc<InstallSnapshotCallbackGate> gate_;
  RaftServer* server_;
};

namespace {

// @unsafe - Thread-safe PollThread::add bridge.  The queued closure captures
// only the gate Arc, never a RaftServer pointer.
void QueueReplicationWake(
    const rusty::Arc<ReplicationWakeGate>& replication_wake_gate) {
  auto owner = replication_wake_gate->ReserveWakeOwner();
  if (owner.is_none()) {
    return;
  }

  auto gate_for_job = replication_wake_gate.clone();
  auto wake_job = rusty::Arc<OneTimeJob>::new_(
      OneTimeJob::new_([gate_for_job]() {
        gate_for_job->WakeOnOwner();
      }));
  owner.as_ref().unwrap()->add(rusty::Arc<Job>(wake_job));
}

// @unsafe - Thread-safe shutdown bridge.  The queued closure captures only
// the gate Arc, never the RaftServer whose loops it wakes.
void QueueReplicationShutdownWake(
    const rusty::Arc<ReplicationWakeGate>& replication_wake_gate) {
  auto owner = replication_wake_gate->ReserveShutdownWakeOwner();
  if (owner.is_none()) {
    return;
  }

  auto gate_for_job = replication_wake_gate.clone();
  auto wake_job = rusty::Arc<OneTimeJob>::new_(
      OneTimeJob::new_([gate_for_job]() {
        gate_for_job->WakeShutdownOnOwner();
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

// ============================================================================
// LOG PERSISTENCE IMPLEMENTATION
// ============================================================================

// @safe - Sticky epoch health is owned by a Rusty atomic. A successful latest
// operation is still non-durable if an earlier concurrent operation already
// marked the epoch unhealthy.
bool RaftServer::RecordPersistenceResult(bool succeeded,
                                         const char* operation) {
  if (succeeded) {
    return persistence_healthy_.load(
        rusty::sync::atomic::Ordering::Acquire);
  }

  const bool was_healthy = persistence_healthy_.swap(
      false, rusty::sync::atomic::Ordering::AcqRel);
  if (was_healthy) {
    Log_error("[RAFT-PERSISTENCE] Site {} storage operation '{}' failed; "
              "disabling durable acknowledgements until restart/recovery",
              site_id_, operation ? operation : "unknown");
  }
  return false;
}

// @unsafe - Callers first close every path that can register a new async
// persistence worker. Moving the vector under its mutex gives this function
// sole ownership of all thread handles; joins happen after releasing the mutex
// so a completing worker cannot deadlock against registration/pruning.
void RaftServer::DrainAsyncPersistenceThreads() {
  std::vector<std::pair<std::thread, rusty::Arc<AtomicFlag>>> threads_to_join;
  {
    std::lock_guard<std::mutex> lock(async_threads_mtx_);
    threads_to_join = std::move(async_threads_);
  }
  for (auto& [thread, done_flag] : threads_to_join) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

// @unsafe - Caller holds mtx_, which is the acceptance-order sequencer for
// AppendEntries and InstallSnapshot.
uint64_t RaftServer::ReserveLogPersistenceTicketLocked() {
  const uint64_t ticket = next_log_persistence_ticket_.load(
      rusty::sync::atomic::Ordering::Relaxed);
  verify(ticket != UINT64_MAX);
  next_log_persistence_ticket_.store(
      ticket + 1, rusty::sync::atomic::Ordering::Release);
  return ticket;
}

// @safe - Waits without holding mtx_. Snapshot installation intentionally
// keeps mtx_ while waiting because append persistence workers never acquire it.
void RaftServer::WaitForLogPersistenceTicket(uint64_t ticket) {
  auto serving = serving_log_persistence_ticket_.lock().unwrap();
  serving = log_persistence_ticket_cv_.wait_while(
      std::move(serving),
      [ticket](uint64_t& serving_ticket) {
        return !raft_server_persistence_ticket_is_ready(
            serving_ticket, ticket);
      }).unwrap();
}

// @safe - Publishes completion to the next accepted storage action.
void RaftServer::CompleteLogPersistenceTicket(uint64_t ticket) {
  auto serving = serving_log_persistence_ticket_.lock().unwrap();
  verify(raft_server_persistence_ticket_is_ready(*serving, ticket));
  verify(ticket != UINT64_MAX);
  *serving = ticket + 1;
  log_persistence_ticket_cv_.notify_all();
}

// @safe - Caller first closes admission under mtx_. The target is therefore a
// stable one-past-last reservation, and reaching it drains sync and async work.
void RaftServer::DrainLogPersistenceSequence() {
  const uint64_t target = next_log_persistence_ticket_.load(
      rusty::sync::atomic::Ordering::Acquire);
  auto serving = serving_log_persistence_ticket_.lock().unwrap();
  serving = log_persistence_ticket_cv_.wait_while(
      std::move(serving),
      [target](uint64_t& serving_ticket) {
        return serving_ticket != target;
      }).unwrap();
}

// @unsafe - Uses LogStorage API
bool RaftServer::PersistTermAndVoteToLogStorage(uint64_t term,
                                                siteid_t voted_for) {
  if (!log_storage_ ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  const bool succeeded = raft_server_write_and_sync(
      *log_storage_, [term, voted_for](raft::LogStorage& storage) {
        return storage.set_metadata_batch({
            {META_TERM, std::to_string(term)},
            {META_VOTE_FOR,
             std::to_string(static_cast<int64_t>(voted_for))},
        });
      });
  return RecordPersistenceResult(succeeded, "term+vote write/sync");
}

// @unsafe - Uses LogStorage API
bool RaftServer::PersistVoteToLogStorage(siteid_t voted_for) {
  if (!log_storage_ ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  const bool succeeded = raft_server_write_and_sync(
      *log_storage_, [voted_for](raft::LogStorage& storage) {
        return storage.set_metadata(
            META_VOTE_FOR,
            std::to_string(static_cast<int64_t>(voted_for)));
      });
  return RecordPersistenceResult(succeeded, "vote write/sync");
}

// @unsafe - Uses LogStorage API. Recovery hints deliberately retain their
// historical no-fsync behavior; they are not durable ACK boundaries.
bool RaftServer::PersistCommitIndexToLogStorage(
    uint64_t commit_index,
    uint64_t spec_commit_index,
    uint64_t secured_log_index) {
  if (!log_storage_ ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  const bool storage_ready = log_storage_->is_open();
  bool writes_succeeded = false;
  if (storage_ready) {
    writes_succeeded = log_storage_->set_metadata_batch({
        {META_COMMIT_INDEX, std::to_string(commit_index)},
        {META_SPEC_COMMIT_INDEX, std::to_string(spec_commit_index)},
        {META_SECURED_LOG_INDEX, std::to_string(secured_log_index)},
    });
  }
  return RecordPersistenceResult(
      storage_ready && writes_succeeded, "commit-index metadata write");
}

// @unsafe - Uses LogStorage API. These recovery hints remain unsynced. Caller
// already owns the accepted-order persistence ticket.
bool RaftServer::PersistSpeculativeIndicesSnapshotToLogStorage(
    uint64_t spec_commit_index,
    uint64_t secured_log_index) {
  if (!log_storage_ ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  const bool storage_ready = log_storage_->is_open();
  bool writes_succeeded = false;
  if (storage_ready) {
    writes_succeeded = log_storage_->set_metadata_batch({
        {META_SPEC_COMMIT_INDEX, std::to_string(spec_commit_index)},
        {META_SECURED_LOG_INDEX, std::to_string(secured_log_index)},
    });
  }
  return RecordPersistenceResult(
      storage_ready && writes_succeeded,
      "speculative-index metadata write");
}

// @unsafe - Uses LogStorage API
bool RaftServer::PersistLogEntryToLogStorage(slotid_t slot_id,
                                             const RaftData& data) {
  if (!log_storage_ ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  janus::raft::LogEntry entry(slot_id, data.term);
  entry.command = data.log_;
  entry.max_ballot_seen = data.max_ballot_seen_;
  entry.max_ballot_accepted = data.max_ballot_accepted_;
  entry.committed = (slot_id <= commitIndex);

  const bool succeeded = raft_server_write_and_sync(
      *log_storage_, [&entry](raft::LogStorage& storage) {
        return storage.put(entry);
      });
  return RecordPersistenceResult(succeeded, "log-entry write/sync");
}

// @unsafe - Uses LogStorage API
bool RaftServer::PersistLogEntriesToLogStorage(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries) {
  if (!log_storage_ || entries.empty() ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  std::vector<janus::raft::LogEntry> log_entries;
  log_entries.reserve(entries.size());

  for (const auto& [slot_id, data] : entries) {
    janus::raft::LogEntry entry(slot_id, data->term);
    entry.command = data->log_;
    entry.max_ballot_seen = data->max_ballot_seen_;
    entry.max_ballot_accepted = data->max_ballot_accepted_;
    entry.committed = (slot_id <= commitIndex);
    log_entries.push_back(entry);
  }

  const bool succeeded = raft_server_write_and_sync(
      *log_storage_, [&log_entries](raft::LogStorage& storage) {
        return storage.put_batch(log_entries);
      });
  return RecordPersistenceResult(succeeded, "log-batch write/sync");
}

// @unsafe - Applies suffix removal before the replacement batch and reports
// durability only after both operations cross one final sync boundary. A crash
// between the two unsynced storage operations can leave a shorter log, which is
// safe because no durable acknowledgement has been sent and the leader retries.
bool RaftServer::PersistFollowerAppendToLogStorage(
    const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries,
    const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>&
        matching_entries_to_verify,
    uint64_t committed_index,
    bool truncate_suffix,
    slotid_t truncate_first,
    slotid_t truncate_last) {
  if (!log_storage_ ||
      (entries.empty() && matching_entries_to_verify.empty()) ||
      !persistence_healthy_.load(
          rusty::sync::atomic::Ordering::Acquire)) {
    return false;
  }

  std::vector<janus::raft::LogEntry> log_entries;
  log_entries.reserve(entries.size());
  for (const auto& [slot_id, data] : entries) {
    janus::raft::LogEntry entry(slot_id, data->term);
    entry.command = data->log_;
    entry.max_ballot_seen = data->max_ballot_seen_;
    entry.max_ballot_accepted = data->max_ballot_accepted_;
    entry.committed = (slot_id <= committed_index);
    log_entries.push_back(std::move(entry));
  }

  const bool succeeded = raft_server_write_and_sync(
      *log_storage_,
      [&log_entries, &matching_entries_to_verify, truncate_suffix,
       truncate_first, truncate_last](
          raft::LogStorage& storage) {
        bool suffix_removed = true;
        if (truncate_suffix) {
          verify(truncate_first <= truncate_last);
          if (truncate_last != UINT64_MAX) {
            suffix_removed = storage.remove_range(
                truncate_first, truncate_last + 1);
          } else {
            // LogStorage ranges are half-open. Split the terminal key so the
            // UINT64_MAX endpoint cannot wrap to zero.
            if (truncate_first < UINT64_MAX) {
              suffix_removed = storage.remove_range(
                  truncate_first, UINT64_MAX);
            }
            const auto terminal = storage.get(UINT64_MAX);
            const bool terminal_removed =
                terminal.is_none() || storage.remove(UINT64_MAX);
            suffix_removed = suffix_removed && terminal_removed;
          }
        }

        // Evaluate the replacement write even if removal failed. The sticky
        // persistence-health bit suppresses durability either way, while the
        // best-effort replacement leaves the follower easier to repair.
        const bool entries_written =
            log_entries.empty() || storage.put_batch(log_entries);

        // A fully matching retry performs no rewrite, but it still needs a
        // fresh durable proof when an earlier response/notification was lost.
        // Verify the exact boundary terms before sync; same index+term is the
        // Raft log-identity criterion.
        bool matching_entries_present = true;
        for (const auto& [index, expected] : matching_entries_to_verify) {
          const auto stored = storage.get(index);
          if (stored.is_none() || stored.as_ref().unwrap().term != expected->term) {
            matching_entries_present = false;
            break;
          }
        }
        return suffix_removed && entries_written &&
               matching_entries_present;
      });
  return RecordPersistenceResult(
      succeeded, "follower suffix replacement write/sync");
}

// @unsafe - Ordered term/vote wrapper. mtx_ is recursive because every normal
// caller already holds it while publishing the corresponding in-memory state.
bool RaftServer::PersistState(uint64_t term,
                              siteid_t voted_for,
                              const char* reason) {
  std::unique_lock<std::recursive_mutex> lock(mtx_);
  if (!HasConfiguredStorage()) return false;
  const uint64_t ticket = ReserveLogPersistenceTicketLocked();
  const bool persisted = ExecuteLogPersistenceTicket(
      ticket, "ordered term+vote persistence",
      [this, term, voted_for]() {
        return PersistTermAndVoteToLogStorage(term, voted_for);
      });
  if (persisted) {
    Log_debug("[RAFT-PERSISTENCE] Persisted: term={} votedFor={} ({})",
              term, voted_for, reason);
  }
  return persisted;
}

// @unsafe - Ordered leader-local log wrapper.
bool RaftServer::PersistLogEntry(slotid_t slot_id,
                                 const RaftData& entry,
                                 const char* reason) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!HasConfiguredStorage()) return false;
  const uint64_t ticket = ReserveLogPersistenceTicketLocked();
  const bool persisted = ExecuteLogPersistenceTicket(
      ticket, "ordered leader log-entry persistence",
      [this, slot_id, &entry]() {
        return PersistLogEntryToLogStorage(slot_id, entry);
      });
  if (persisted) {
    Log_debug("[RAFT-PERSISTENCE] Persisted log: slot={} ({})",
              slot_id, reason);
  }
  return persisted;
}

// @unsafe - Ordered leader commit/speculative metadata wrapper.
bool RaftServer::PersistCommitIndex(uint64_t commit_index,
                                    const char* reason) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!HasConfiguredStorage()) return false;
  const uint64_t spec_commit_index = specCommitIndex_;
  const uint64_t secured_log_index = securedLogIndex_;
  const uint64_t ticket = ReserveLogPersistenceTicketLocked();
  const bool persisted = ExecuteLogPersistenceTicket(
      ticket, "ordered commit-index metadata persistence",
      [this, commit_index, spec_commit_index, secured_log_index]() {
        return PersistCommitIndexToLogStorage(
            commit_index, spec_commit_index, secured_log_index);
      });
  if (!persisted) {
    Log_warn("[RAFT-PERSISTENCE] Failed commit metadata: index={} ({})",
             commit_index, reason);
  }
  return persisted;
}

// @unsafe - Ordered current speculative metadata wrapper.
bool RaftServer::PersistSpeculativeIndicesToLogStorage() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!HasConfiguredStorage()) return false;
  const uint64_t spec_commit_index = specCommitIndex_;
  const uint64_t secured_log_index = securedLogIndex_;
  const uint64_t ticket = ReserveLogPersistenceTicketLocked();
  return ExecuteLogPersistenceTicket(
      ticket, "ordered speculative-index metadata persistence",
      [this, spec_commit_index, secured_log_index]() {
        return PersistSpeculativeIndicesSnapshotToLogStorage(
            spec_commit_index, secured_log_index);
      });
}

// @unsafe - Caller holds mtx_. Uses snapshot metadata or a non-mutating map
// lookup so an ACK check cannot recreate a compacted entry.
bool RaftServer::PersistedAppendContextIsCurrentLocked(
    uint64_t accepted_term,
    siteid_t accepted_leader,
    slotid_t boundary_index,
    ballot_t boundary_term) const {
  if (!raft_server_persisted_reply_context_is_current(
          stop_.load(rusty::sync::atomic::Ordering::Acquire),
          is_leader_, currentTerm, accepted_term,
          current_leader_id_, accepted_leader)) {
    return false;
  }

  if (boundary_index == 0) {
    return boundary_term == 0;
  }
  if (boundary_index == snapidx_) {
    return snapterm_ == boundary_term;
  }
  const auto boundary = raft_logs_.find(boundary_index);
  return boundary != raft_logs_.end() && boundary->second != nullptr &&
         boundary->second->log_.has_value() &&
         boundary->second->term == boundary_term;
}

// @unsafe - Recovers state from LogStorage
bool RaftServer::RecoverFromStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return true;  // No storage configured, nothing to recover
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Recovery is transactional with respect to the live Raft object. Storage
  // reads, numeric parsing, command allocation, and range validation all
  // complete in locals; only the final no-throw map swap publishes anything.
  // This is important both for corrupt media and for a repeated recovery call:
  // neither case may merge a partial range into an older in-memory suffix.
  try {
    uint64_t recovered_term = 0;
    uint64_t recovered_commit_index = 0;
    uint64_t recovered_spec_commit_index = 0;
    uint64_t recovered_secured_log_index = 0;
    uint64_t recovered_vote_value =
        static_cast<uint64_t>(INVALID_SITEID);

    auto parse_metadata =
        [this](const char* key, uint64_t default_value,
               uint64_t* parsed_value, bool* was_present) {
          auto stored = log_storage_->get_metadata(key);
          if (stored.is_none()) {
            *parsed_value = default_value;
            *was_present = false;
            return true;
          }

          const std::string text = stored.unwrap();
          uint64_t value = 0;
          const char* begin = text.data();
          const char* end = begin + text.size();
          const auto parsed = std::from_chars(begin, end, value, 10);
          if (text.empty() || parsed.ec != std::errc{} ||
              parsed.ptr != end) {
            Log_error("[RAFT-RECOVERY] Site {} metadata '{}' is not a "
                      "canonical uint64 decimal ({} bytes)",
                      site_id_, key, text.size());
            return false;
          }

          *parsed_value = value;
          *was_present = true;
          return true;
        };

    bool term_present = false;
    bool vote_present = false;
    bool commit_present = false;
    bool spec_present = false;
    bool secured_present = false;
    if (!parse_metadata(META_TERM, 0, &recovered_term, &term_present) ||
        !parse_metadata(META_VOTE_FOR,
                        static_cast<uint64_t>(INVALID_SITEID),
                        &recovered_vote_value, &vote_present) ||
        !parse_metadata(META_COMMIT_INDEX, 0, &recovered_commit_index,
                        &commit_present) ||
        !parse_metadata(META_SPEC_COMMIT_INDEX, 0,
                        &recovered_spec_commit_index, &spec_present) ||
        !parse_metadata(META_SECURED_LOG_INDEX, 0,
                        &recovered_secured_log_index, &secured_present)) {
      return false;
    }

    if (recovered_vote_value >
        static_cast<uint64_t>(std::numeric_limits<siteid_t>::max())) {
      Log_error("[RAFT-RECOVERY] Site {} recovered vote {} is outside the "
                "site-id representation",
                site_id_, recovered_vote_value);
      return false;
    }
    const siteid_t recovered_vote_for =
        static_cast<siteid_t>(recovered_vote_value);
    if (recovered_vote_for != INVALID_SITEID && recovered_term == 0) {
      Log_error("[RAFT-RECOVERY] Site {} recovered a real vote {} in term 0",
                site_id_, recovered_vote_for);
      return false;
    }

    // commitIndex is written before the two speculative hints. A crash between
    // those independent metadata writes can therefore leave an older
    // specCommitIndex. Raising that hint is safe and preserves the committed
    // prefix; unlike the old implementation, committed progress is never
    // clamped down merely because its snapshot-covered log prefix is absent.
    if (recovered_spec_commit_index < recovered_commit_index) {
      Log_warn("[RAFT-RECOVERY] Site {} reconciling stale specCommitIndex "
               "{} -> {} (commitIndex)",
               site_id_, recovered_spec_commit_index,
               recovered_commit_index);
      recovered_spec_commit_index = recovered_commit_index;
    }
    if (recovered_secured_log_index > recovered_spec_commit_index) {
      Log_error("[RAFT-RECOVERY] Site {} invalid progress ordering: "
                "securedLogIndex={} specCommitIndex={}",
                site_id_, recovered_secured_log_index,
                recovered_spec_commit_index);
      return false;
    }

    const size_t stored_entry_count = log_storage_->size();
    const slotid_t recovered_first_index =
        log_storage_->get_first_index();
    const slotid_t recovered_last_index =
        log_storage_->get_last_index();
    std::map<slotid_t, std::shared_ptr<RaftData>> recovered_logs;

    if (stored_entry_count == 0) {
      if (recovered_first_index != 0 || recovered_last_index != 0) {
        Log_error("[RAFT-RECOVERY] Site {} empty storage reported range "
                  "{}..{}",
                  site_id_, recovered_first_index, recovered_last_index);
        return false;
      }

      if (recovered_commit_index != 0 ||
          recovered_spec_commit_index != 0 ||
          recovered_secured_log_index != 0) {
        Log_info("[RAFT-RECOVERY] Site {} deferring empty-log progress "
                 "validation to snapshot recovery: commit={} spec={} "
                 "secured={}",
                 site_id_, recovered_commit_index,
                 recovered_spec_commit_index,
                 recovered_secured_log_index);
      }
    } else {
      if (recovered_first_index == 0 || recovered_last_index == 0 ||
          recovered_first_index > recovered_last_index) {
        Log_error("[RAFT-RECOVERY] Site {} invalid non-empty storage range "
                  "{}..{} (count={})",
                  site_id_, recovered_first_index, recovered_last_index,
                  stored_entry_count);
        return false;
      }
      if (recovered_last_index == UINT64_MAX) {
        Log_error("[RAFT-RECOVERY] Site {} terminal log index UINT64_MAX "
                  "cannot be represented by the half-open range API",
                  site_id_);
        return false;
      }

      const uint64_t expected_entry_count =
          recovered_last_index - recovered_first_index + 1;
      if (expected_entry_count >
              static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
          stored_entry_count != static_cast<size_t>(expected_entry_count)) {
        Log_error("[RAFT-RECOVERY] Site {} sparse/ambiguous storage range "
                  "{}..{}: expected {} entries, backend reports {}",
                  site_id_, recovered_first_index, recovered_last_index,
                  expected_entry_count, stored_entry_count);
        return false;
      }

      const auto entries = log_storage_->get_range(
          recovered_first_index, recovered_last_index + 1);
      if (entries.size() != stored_entry_count || entries.empty() ||
          entries.front().slot_id != recovered_first_index ||
          entries.back().slot_id != recovered_last_index) {
        Log_error("[RAFT-RECOVERY] Site {} range read did not reproduce "
                  "storage endpoints {}..{} (reported={} returned={})",
                  site_id_, recovered_first_index, recovered_last_index,
                  stored_entry_count, entries.size());
        return false;
      }

      slotid_t expected_index = recovered_first_index;
      for (const auto& entry : entries) {
        if (entry.slot_id != expected_index) {
          Log_error("[RAFT-RECOVERY] Site {} non-contiguous log: expected "
                    "slot {}, recovered {}",
                    site_id_, expected_index, entry.slot_id);
          return false;
        }
        if (entry.term > recovered_term) {
          Log_error("[RAFT-RECOVERY] Site {} log slot {} has future term {} "
                    "above recovered currentTerm {}",
                    site_id_, entry.slot_id, entry.term, recovered_term);
          return false;
        }
        if (!entry.command.has_value()) {
          Log_error("[RAFT-RECOVERY] Site {} log slot {} has no command",
                    site_id_, entry.slot_id);
          return false;
        }

        auto data = std::make_shared<RaftData>();
        data->term = entry.term;
        // LogEntry::command and RaftData::log_ are both janus::Command;
        // the copy retains the decoded shared envelope.
        data->log_ = entry.command;
        data->max_ballot_seen_ = entry.max_ballot_seen;
        data->max_ballot_accepted_ = entry.max_ballot_accepted;
        data->slot_id = entry.slot_id;
        const auto inserted = recovered_logs.emplace(entry.slot_id, data);
        if (!inserted.second) {
          Log_error("[RAFT-RECOVERY] Site {} duplicate log slot {}",
                    site_id_, entry.slot_id);
          return false;
        }
        ++expected_index;
      }

      if (recovered_commit_index > recovered_last_index ||
          recovered_spec_commit_index > recovered_last_index ||
          recovered_secured_log_index > recovered_last_index) {
        Log_error("[RAFT-RECOVERY] Site {} progress exceeds non-empty log: "
                  "commit={} spec={} secured={} last={}",
                  site_id_, recovered_commit_index,
                  recovered_spec_commit_index,
                  recovered_secured_log_index, recovered_last_index);
        return false;
      }

      if (recovered_first_index > 1) {
        Log_info("[RAFT-RECOVERY] Site {} deferring compacted prefix before "
                 "slot {} to snapshot recovery",
                 site_id_, recovered_first_index);
      }
    }

    // Log before publication because formatting may allocate. After this
    // point only a noexcept map swap and scalar assignments remain, so a false
    // return can never describe a partially published recovery.
    Log_info("[RAFT-RECOVERY] Site {}: Validated term={} vote_for={} "
             "firstLogIndex={} lastLogIndex={} commitIndex={} "
             "specCommitIndex={} securedLogIndex={} entries={} "
             "metadata_present=[term:{} vote:{} commit:{} spec:{} secured:{}]",
             site_id_, recovered_term, recovered_vote_for,
             recovered_first_index, recovered_last_index,
             recovered_commit_index, recovered_spec_commit_index,
             recovered_secured_log_index, recovered_logs.size(), term_present,
             vote_present, commit_present, spec_present, secured_present);

    // std::map::swap with the default allocator is noexcept. Publish the fully
    // validated map and its scalar description while mtx_ excludes all
    // observers. No old slot survives a repeated recovery call.
    raft_logs_.swap(recovered_logs);
    currentTerm = recovered_term;
    vote_for_ = recovered_vote_for;
    commitIndex = recovered_commit_index;
    specCommitIndex_ = recovered_spec_commit_index;
    securedLogIndex_ = recovered_secured_log_index;
    lastLogIndex = recovered_last_index;
    min_active_slot_ =
        recovered_first_index == 0 ? 1 : recovered_first_index;
    return true;
  } catch (const std::exception& error) {
    Log_error("[RAFT-RECOVERY] Site {} storage recovery threw: {}",
              site_id_, error.what());
  } catch (...) {
    Log_error("[RAFT-RECOVERY] Site {} storage recovery threw an unknown "
              "exception",
              site_id_);
  }
  return false;
}

// @unsafe - Replays committed entries (callbacks wrapped in @unsafe blocks)
bool RaftServer::ReplayCommittedEntries() {
  // Startup replays synchronously before the background apply thread and RPC
  // admission begin. The same gate also keeps explicit test recovery from
  // overlapping a concurrently installed state-machine snapshot.
  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t end = commitIndex;
  if (!app_next_) {
    Log_error("[RAFT-REPLAY] Site {}: No state-machine callback registered",
              site_id_);
    return false;
  }
  if (executeIndex > end) {
    Log_error("[RAFT-REPLAY] Site {}: applied index {} is ahead of commit {}",
              site_id_, executeIndex, end);
    return false;
  }
  if (executeIndex == end) {
    Log_info("[RAFT-REPLAY] Site {}: No entries to replay "
             "(executeIndex={} == commitIndex={})",
             site_id_, executeIndex, commitIndex);
    return true;
  }
  if (!raft_server_log_index_has_successor(executeIndex)) {
    Log_error("[RAFT-REPLAY] Site {}: applied index {} has no successor",
              site_id_, executeIndex);
    return false;
  }
  const slotid_t start = executeIndex + 1;
  Log_info("[RAFT-REPLAY] Site {}: Replaying entries {}..{}", site_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start;; id++) {
    const auto recovered = raft_logs_.find(id);
    const auto instance =
        recovered == raft_logs_.end() ? nullptr : recovered->second;
    if (instance != nullptr && instance->log_.has_value()) {
      // @unsafe
      try {
        if (!raft_server_command_is_internal_noop(
                instance->log_.kind_, TpcNoopCommand::static_kind())) {
          app_next_(id, instance->log_);
        }
      } catch (const std::exception& error) {
        Log_error("[RAFT-REPLAY] Site {}: state-machine callback threw at "
                  "slot {}: {}",
                  site_id_, id, error.what());
        return false;
      } catch (...) {
        Log_error("[RAFT-REPLAY] Site {}: state-machine callback threw at "
                  "slot {}",
                  site_id_, id);
        return false;
      }
      PublishAppliedIndex(id);
      replayed++;
    } else {
      Log_error("[RAFT-REPLAY] Site {}: Missing log entry at committed slot {}",
                site_id_, id);
      return false;
    }
    if (id == end) {
      break;
    }
  }

  Log_info("[RAFT-REPLAY] Site {}: Replayed {} entries, executeIndex now {}",
           site_id_, replayed, executeIndex);

  // Log uncommitted entries status
  size_t uncommitted = GetUncommittedCount();
  if (uncommitted > 0) {
    Log_info("[RAFT-RECOVERY] Site {}: {} uncommitted entries (lastLogIndex={}, commitIndex={}) - will be resolved by consensus",
             site_id_, uncommitted, lastLogIndex, commitIndex);
  }
  return executeIndex == end;
}

// @safe - Read-only accessor
size_t RaftServer::GetUncommittedCount() const {
  if (lastLogIndex > commitIndex) {
    return lastLogIndex - commitIndex;
  }
  return 0;
}

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
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    const bool has_orphaned_compacted_suffix =
        snapidx_ == 0 && !raft_logs_.empty() &&
        raft_logs_.begin()->first > 1;
    const bool has_uncovered_empty_progress =
        snapidx_ == 0 && raft_logs_.empty() &&
        (commitIndex != 0 || specCommitIndex_ != 0 ||
         securedLogIndex_ != 0);
    if (has_orphaned_compacted_suffix || has_uncovered_empty_progress) {
      Log_error("[RAFT-SNAPSHOT] Site {} has recovered progress without its "
                "covering snapshot (first={} commit={} spec={} secured={}); "
                "snapshots are disabled",
                site_id_,
                raft_logs_.empty() ? 0 : raft_logs_.begin()->first,
                commitIndex, specCommitIndex_, securedLogIndex_);
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

  // Build snapshot config
  janus::raft::SnapshotConfig snap_config;
  // @unsafe { getenv is not borrow-checked }
  const char* custom_path = std::getenv("MAKO_RAFT_SNAPSHOT_PATH");
  if (custom_path && custom_path[0] != '\0') {
    snap_config.storage_path = std::string(custom_path) + "/raft_snap_" +
                               std::to_string(site_id_) + "_partition_" +
                               std::to_string(partition_id_);
  } else {
    snap_config = janus::raft::SnapshotConfig::for_replica(partition_id_, loc_id_);
  }

  // Check for custom snapshot interval
  const char* interval_str = std::getenv("MAKO_RAFT_SNAPSHOT_INTERVAL");  // @unsafe
  if (interval_str && interval_str[0] != '\0') {
    try {
      snap_config.snapshot_interval = std::stoull(interval_str);
    } catch (const std::exception& error) {
      Log_error("[RAFT-SNAPSHOT] Invalid snapshot interval '{}': {}",
                interval_str, error.what());
      return false;
    }
    SetSnapshotThreshold(snap_config.snapshot_interval);
  }

  auto manager =
      std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
  if (!manager->IsStorageReady()) {
    Log_error("[RAFT-SNAPSHOT] Snapshot storage is unavailable for site {} at {}",
              site_id_, snap_config.storage_path.c_str());
    return false;
  }

  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::recursive_mutex> lock(mtx_);

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
        snapidx_ == 0 && !raft_logs_.empty() &&
        raft_logs_.begin()->first > 1;
    const bool has_uncovered_empty_progress =
        snapidx_ == 0 && raft_logs_.empty() &&
        (commitIndex != 0 || specCommitIndex_ != 0 ||
         securedLogIndex_ != 0);
    if (snapidx_ != 0 || has_orphaned_compacted_suffix ||
        has_uncovered_empty_progress) {
      return fail_recovery(
          "empty snapshot manager cannot cover the compacted live/persistent log");
    }
    snapshot_manager_ = manager;
    snapshot_manager_configured_.store(
        true, rusty::sync::atomic::Ordering::Release);
    Log_info("[RAFT-SNAPSHOT] Initialized empty manager for site {} partition {}: path={} interval={}",
             site_id_, partition_id_, snap_config.storage_path.c_str(),
             snap_config.snapshot_interval);
    return true;
  }

  const auto discovered = latest.unwrap();
  janus::raft::SnapshotMetadata metadata;
  std::string snapshot_data;
  if (!manager->LoadLatestSnapshot(&metadata, &snapshot_data)) {
    return fail_recovery("latest snapshot bytes failed checksum/format validation");
  }
  if (metadata.last_included_index != discovered.last_included_index ||
      metadata.last_included_term != discovered.last_included_term) {
    return fail_recovery(
        "snapshot filename metadata does not match its verified header");
  }
  if (metadata.last_included_index == 0 ||
      !raft_server_log_index_has_successor(metadata.last_included_index)) {
    return fail_recovery("snapshot boundary is outside the recoverable log range");
  }
  if (metadata.last_included_index < snapidx_ ||
      (metadata.last_included_index == snapidx_ && snapidx_ != 0 &&
       metadata.last_included_term != snapterm_)) {
    return fail_recovery("snapshot manager would move the live boundary backward or change its term");
  }
  if (prepare_sm_snapshot_cb_ &&
      GetAppliedIndex() > metadata.last_included_index) {
    return fail_recovery(
        "refusing to rewind a live state machine to an older snapshot");
  }

  const uint64_t recovered_snapshot_index = metadata.last_included_index;
  const uint64_t recovered_snapshot_term = metadata.last_included_term;
  const uint64_t previous_snapshot_index = snapidx_;
  const uint64_t previous_snapshot_term = snapterm_;
  const uint64_t previous_last_log_index = lastLogIndex;
  const uint64_t previous_min_active_slot = min_active_slot_;

  // A durable snapshot can become visible before the persistent-log cleanup
  // that follows it.  Reconstruct Figure 13's suffix decision from the old
  // boundary when it is still present, or from an already-compacted storage
  // shape.  A live reinitialization may use its exact existing snapshot tuple
  // as the same proof (Test66); a fresh persistence-off restart has no suffix.
  const auto boundary = raft_logs_.find(recovered_snapshot_index);
  const bool has_boundary =
      boundary != raft_logs_.end() && boundary->second != nullptr &&
      boundary->second->log_.has_value();
  const uint64_t local_boundary_term =
      has_boundary ? boundary->second->term : 0;
  const bool boundary_matches = raft_server_snapshot_boundary_matches(
      has_boundary, local_boundary_term, recovered_snapshot_term);
  const bool has_recovered_suffix = raft_server_log_index_above(
      previous_last_log_index, recovered_snapshot_index);

  slotid_t persistent_first_index = 0;
  slotid_t persistent_last_index = 0;
  if (log_storage_) {
    persistent_first_index = log_storage_->get_first_index();
    persistent_last_index = log_storage_->get_last_index();
  }
  const bool storage_compaction_proves_suffix =
      persistent_first_index != 0 &&
      persistent_first_index == recovered_snapshot_index + 1;
  const bool live_snapshot_proves_suffix =
      previous_snapshot_index == recovered_snapshot_index &&
      previous_snapshot_term == recovered_snapshot_term &&
      previous_min_active_slot == recovered_snapshot_index + 1;
  const bool retain_suffix =
      raft_server_snapshot_recovery_retains_suffix(
          has_recovered_suffix, has_boundary, boundary_matches,
          storage_compaction_proves_suffix,
          live_snapshot_proves_suffix);

  if (raft_server_snapshot_recovery_has_unproven_gap(
          has_recovered_suffix, has_boundary,
          storage_compaction_proves_suffix,
          live_snapshot_proves_suffix)) {
    return fail_recovery(
        "persistent suffix has no snapshot boundary or compaction proof");
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

  snapidx_ = recovered_snapshot_index;
  snapterm_ = recovered_snapshot_term;
  if (retain_suffix) {
    raft_logs_.erase(raft_logs_.begin(), raft_logs_.upper_bound(snapidx_));
    lastLogIndex = std::max(previous_last_log_index, snapidx_);
  } else {
    raft_logs_.clear();
    lastLogIndex = snapidx_;
  }
  commitIndex = raft_server_snapshot_progress_clamp(
      commitIndex, snapidx_, lastLogIndex);
  specCommitIndex_ = raft_server_snapshot_progress_clamp(
      specCommitIndex_, commitIndex, lastLogIndex);
  securedLogIndex_ = raft_server_commit_index_clamp(
      securedLogIndex_, specCommitIndex_);
  min_active_slot_ = std::max(min_active_slot_, snapidx_ + 1);

  if (currentTerm < snapterm_) {
    Log_warn("[RAFT-SNAPSHOT] Site {} advancing recovered term {} -> {} "
             "to cover snapshot boundary",
             site_id_, currentTerm, snapterm_);
    currentTerm = snapterm_;
    vote_for_ = INVALID_SITEID;
  }

  verify(securedLogIndex_ <= specCommitIndex_);
  verify(commitIndex <= specCommitIndex_);
  verify(specCommitIndex_ <= lastLogIndex);

  // Finish a snapshot/log crash window before publishing the recovered state
  // to waiters or runtime loops.  remove_range is one atomic RocksDB batch; on
  // a subsequent restart either the old boundary remains available for a term
  // comparison or the first surviving entry is strictly above the snapshot.
  if (HasConfiguredStorage()) {
    const slotid_t remove_through_index =
        retain_suffix ? snapidx_ : persistent_last_index;
    const bool storage_reconciled = raft_server_write_and_sync(
        *log_storage_,
        [this, persistent_first_index, remove_through_index](
            raft::LogStorage& storage) {
          bool removal_succeeded = true;
          if (persistent_first_index != 0) {
            if (raft_server_log_index_has_successor(remove_through_index)) {
              const slotid_t remove_end = remove_through_index + 1;
              if (persistent_first_index < remove_end) {
                removal_succeeded = storage.remove_range(
                    persistent_first_index, remove_end);
              }
            } else {
              if (persistent_first_index < UINT64_MAX) {
                removal_succeeded = storage.remove_range(
                    persistent_first_index, UINT64_MAX);
              }
              const bool terminal_removed =
                  storage.get(UINT64_MAX).is_none() ||
                  storage.remove(UINT64_MAX);
              removal_succeeded =
                  removal_succeeded && terminal_removed;
            }
          }

          const bool metadata_written = storage.set_metadata_batch({
              {META_TERM, std::to_string(currentTerm)},
              {META_VOTE_FOR,
               std::to_string(static_cast<int64_t>(vote_for_))},
              {META_COMMIT_INDEX, std::to_string(commitIndex)},
              {META_SPEC_COMMIT_INDEX, std::to_string(specCommitIndex_)},
              {META_SECURED_LOG_INDEX, std::to_string(securedLogIndex_)},
          });
          return removal_succeeded && metadata_written;
        });
    if (!RecordPersistenceResult(
            storage_reconciled,
            "snapshot startup log/term/progress reconciliation")) {
      return fail_recovery(
          "could not reconcile recovered snapshot with persistent log");
    }
  }

  snapshot_manager_ = manager;
  snapshot_manager_configured_.store(
      true, rusty::sync::atomic::Ordering::Release);
  snapshot_trigger_index_.store(
      snapidx_, rusty::sync::atomic::Ordering::Release);

  if (snapidx_ > GetAppliedIndex()) {
    PublishAppliedIndex(snapidx_);
  }

  Log_info("[RAFT-SNAPSHOT] Restored snapshot for site {}: index={} term={} "
           "size={} commit={} last={} min_active={} retain_suffix={}",
           site_id_, snapidx_, snapterm_, metadata.size_bytes,
           commitIndex, lastLogIndex, min_active_slot_, retain_suffix);

  Log_info("[RAFT-SNAPSHOT] Initialized for site {} partition {}: path={} interval={}",
           site_id_, partition_id_, snap_config.storage_path.c_str(),
           snap_config.snapshot_interval);
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
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  snapshot_manager_ = std::move(manager);
  snapshot_manager_configured_.store(
      snapshot_manager_ != nullptr,
      rusty::sync::atomic::Ordering::Release);
}

std::shared_ptr<janus::raft::SnapshotManager>
RaftServer::GetSnapshotManager() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return snapshot_manager_;
}

void RaftServer::SetSnapshotThreshold(uint64_t threshold) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  snapshot_threshold_ = threshold;
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
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return snapidx_;
}

// @unsafe - Returns the snapshot boundary term under mtx_.
uint64_t RaftServer::GetSnapshotTerm() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return snapterm_;
}

// @unsafe - Log compaction (storage operations wrapped in @unsafe blocks)
size_t RaftServer::CompactLog(slotid_t up_to_index) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Compaction is safe only through the prefix represented by both committed
  // state and the installed/local snapshot boundary.
  const slotid_t requested_index = up_to_index;
  up_to_index = raft_server_compaction_safe_index(
      up_to_index, commitIndex, snapidx_);
  if (up_to_index != requested_index) {
    Log_warn("[RAFT-COMPACT] Site {}: Clamped compaction {} -> {} "
             "(commitIndex={}, snapidx={})",
             site_id_, requested_index, up_to_index, commitIndex, snapidx_);
  }

  // A tracked coordinator has not yet captured whether its exact entry
  // committed or was superseded. Keep every such slot available for the
  // identity check; GetSubmissionProgress erases the exact (index, term)
  // registration after returning a terminal snapshot.
  if (!active_submissions_.empty()) {
    const slotid_t protected_index = active_submissions_.begin()->first;
    if (up_to_index >= protected_index) {
      if (protected_index <= 1) {
        Log_info("[RAFT-COMPACT] Site {} retaining log for active submission "
                 "at index {}", site_id_, protected_index);
        return 0;
      }
      const slotid_t unclamped_index = up_to_index;
      up_to_index = protected_index - 1;
      Log_info("[RAFT-COMPACT] Site {} clamped compaction {} -> {} for "
               "active submission at index {}",
               site_id_, unclamped_index, up_to_index, protected_index);
    }
  }
  if (!raft_server_log_index_has_successor(up_to_index)) {
    Log_error("[RAFT-COMPACT] Site {}: Refusing terminal compaction index {}; "
              "the exclusive storage bound and min_active_slot would wrap",
              site_id_, up_to_index);
    return 0;
  }

  slotid_t persisted_first = 0;
  size_t removed_storage = 0;
  if (HasConfiguredStorage()) {
    const uint64_t ticket = ReserveLogPersistenceTicketLocked();
    const bool persistent_compaction_succeeded = ExecuteLogPersistenceTicket(
        ticket, "snapshot log compaction write/sync",
        [this, up_to_index, &persisted_first, &removed_storage]() {
          if (!HasDurableStorage()) {
            return RecordPersistenceResult(
                false, "snapshot log compaction unavailable storage");
          }
          if (log_storage_->empty()) {
            return true;
          }
          persisted_first = log_storage_->get_first_index();
          if (persisted_first == 0 || up_to_index < persisted_first) {
            return true;
          }
          const bool succeeded = raft_server_write_and_sync(
              *log_storage_,
              [persisted_first, up_to_index](raft::LogStorage& storage) {
                return storage.remove_range(
                    persisted_first, up_to_index + 1);
              });
          if (succeeded) {
            removed_storage = static_cast<size_t>(
                up_to_index - persisted_first + 1);
          }
          return RecordPersistenceResult(
              succeeded, "snapshot log compaction write/sync");
        });
    if (!persistent_compaction_succeeded) {
      Log_error("[RAFT-COMPACT] Site {}: Persistent compaction through {} "
                "failed; retaining the in-memory log and min_active_slot={}",
                site_id_, up_to_index, min_active_slot_);
      return 0;
    }
  }

  size_t removed_memory = 0;
  auto it = raft_logs_.begin();
  while (it != raft_logs_.end() && it->first <= up_to_index) {
    it = raft_logs_.erase(it);
    removed_memory++;
  }

  // up_to_index was proven to have a representable successor above.
  if (up_to_index + 1 > min_active_slot_) {
    min_active_slot_ = up_to_index + 1;
  }

  if (HasConfiguredStorage()) {
    Log_info("[RAFT-COMPACT] Site {}: Compacted through {} "
             "(storage_first={}, storage={}, memory={})",
             site_id_, up_to_index, persisted_first,
             removed_storage, removed_memory);
    return removed_storage != 0 ? removed_storage : removed_memory;
  }

  Log_info("[RAFT-COMPACT] Site {}: Compacted in-memory entries through {} "
           "(memory={})",
           site_id_, up_to_index, removed_memory);
  return removed_memory;
}

uint64_t RaftServer::SetStateMachineSnapshotCallbacks(
    std::function<std::string(uint64_t)> create_cb,
    std::function<std::unique_ptr<PreparedStateMachineSnapshotInstall>(
        const std::string&, uint64_t)> prepare_cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (next_snapshot_callback_owner_token_ == 0) {
    next_snapshot_callback_owner_token_ = 1;
  }
  const uint64_t owner_token = next_snapshot_callback_owner_token_++;
  create_sm_snapshot_cb_ = std::move(create_cb);
  prepare_sm_snapshot_cb_ = std::move(prepare_cb);
  snapshot_callback_owner_token_ = owner_token;
  return owner_token;
}

bool RaftServer::ClearStateMachineSnapshotCallbacks(
    uint64_t callback_owner_token) {
  if (callback_owner_token == 0) {
    return false;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (snapshot_callback_owner_token_ != callback_owner_token) {
    return false;
  }

  create_sm_snapshot_cb_ = {};
  prepare_sm_snapshot_cb_ = {};
  snapshot_callback_owner_token_ = 0;
  return true;
}

// @unsafe - Serializes state-machine bytes with their applied index. The lock
// order matches replay and InstallSnapshot: apply gate before Raft state.
void RaftServer::CreateSnapshot() {
  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  (void)CreateSnapshotLocked();
}

// @unsafe - Slow path for the atomic apply-thread hint. Rechecking the
// canonical fields under both locks makes stale hints harmless.
void RaftServer::MaybeCreateSnapshot() {
  std::lock_guard<std::mutex> apply_lock(state_machine_apply_mtx_);
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (!snapshot_manager_ ||
      !raft_server_snapshot_is_due(
          snapidx_, executeIndex, snapshot_threshold_)) {
    return;
  }
  (void)CreateSnapshotLocked();
}

// @unsafe - Caller holds state_machine_apply_mtx_ then mtx_. This keeps the
// callback's serialized bytes, executeIndex, and boundary term in one applied
// state-machine epoch.
bool RaftServer::CreateSnapshotLocked() {

  if (!snapshot_manager_) {
    Log_debug("[RAFT-SNAPSHOT] Site {}: No snapshot manager, skipping CreateSnapshot",
              site_id_);
    return false;
  }

  slotid_t snap_index = executeIndex;
  if (snap_index == 0) {
    Log_debug("[RAFT-SNAPSHOT] Site {}: executeIndex is 0, nothing to snapshot",
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
  if (raft_server_snapshot_term_uses_boundary(snap_index, snapidx_)) {
    // The boundary entry is intentionally absent after compaction. Its term is
    // carried by snapshot metadata; do not recreate the entry or rewind
    // min_active_slot_ through GetRaftInstance().
    snap_term = snapterm_;
  } else {
    const auto instance = raft_logs_.find(snap_index);
    if (instance != raft_logs_.end() && instance->second != nullptr) {
      snap_term = instance->second->term;
    } else {
      // A missing historical term cannot be inferred from currentTerm: doing
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
    // Fallback: 8 bytes executeIndex + 8 bytes term
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
  slotid_t old_snapidx = snapidx_;
  snapidx_ = snap_index;
  snapterm_ = snap_term;
  snapshot_trigger_index_.store(
      snapidx_, rusty::sync::atomic::Ordering::Release);

  Log_info("[RAFT-SNAPSHOT] Site {}: Snapshot saved at index={} term={} (prev snapidx={})",
           site_id_, snap_index, snap_term, old_snapidx);

  // Compact the log up to the snapshot index
  size_t compacted = CompactLog(snap_index);
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
  : timer_(rusty::Box<Timer>::make(Timer())),  // Initialize Box in member initializer list
    replication_wake_gate_(rusty::Arc<ReplicationWakeGate>::make()),
    install_snapshot_callback_gate_(
        rusty::Arc<InstallSnapshotCallbackGate>::make(this))
{
  async_callback_lifetime_->server = this;
  // RocksDB recovery deserializes polymorphic commands during Setup().  Make
  // the immutable kind-4 compatibility factory available as soon as a Raft
  // server exists, before any storage backend can be opened or replayed.
  EnsureLegacyRaftLogPayloadRegistered();
#ifdef RAFT_TEST_CORO
  setIsLeader(false);
#endif
  stop_.store(false, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - Binds the gate before the owner starts HeartbeatLoop.
void RaftServer::BindReplicationWakeOwner(
    rusty::Arc<rrr::PollThread> owner) {
  replication_wake_gate_->BindOwner(std::move(owner));
}

// @unsafe - Any-thread publication followed by a gate-only PollThread job.
void RaftServer::RequestReplication() {
  if (!replication_wake_gate_->Publish()) {
    return;
  }
  QueueReplicationWake(replication_wake_gate_);
}

// @unsafe - Cooperatively yields the current RPC fiber, or the native caller
// when Start is invoked directly by a test/application thread. Synchronous
// persistence holds the Raft lock across leader-local disk work; async and
// persistence-off request paths do not take this scheduler hop.
void RaftServer::YieldAfterSynchronousLocalAppend() {
  if (!HasConfiguredStorage() || async_persistence_) {
    return;
  }
  auto current_fiber = Fiber::current_fiber();
  if (current_fiber.is_some()) {
    // Raw Fiber::yield_ parks until an external resume. A one-microsecond
    // reactor sleep both yields now and guarantees this caller is requeued.
    Fiber::sleep(1);
  } else {
    std::this_thread::yield();
  }
}

// @unsafe - Called only by HeartbeatLoop on its bound PollThread.
bool RaftServer::WaitForReplicationOrHeartbeat(uint64_t timeout_us) {
  return replication_wake_gate_->WaitForWork(timeout_us);
}

// @unsafe - Called only by the election fiber on the bound PollThread.
bool RaftServer::WaitForElectionTimeoutOrShutdown(uint64_t timeout_us) {
  return replication_wake_gate_->WaitForElectionTimeout(timeout_us);
}

// @unsafe - Close ordering is intentional: make new submissions inert, queue
// one owner-thread wake for an armed waiter, then drop the owner's gate handle.
void RaftServer::CloseReplicationWakeGate() {
  replication_wake_gate_->Close();
  QueueReplicationShutdownWake(replication_wake_gate_);
  replication_wake_gate_->ClearOwner();
}

// @unsafe - Reactor-fiber completion barrier used before deleting a live
// RaftServer.  Both runtime loops publish their running state with Release.
void RaftServer::PrepareForShutdown() {
  {
    // Linearize admission closure with every RPC/local mutation that reserves
    // an accepted-order persistence ticket.
    std::lock_guard<std::recursive_mutex> admission_lock(mtx_);
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
  }
  // Outbound snapshot futures may outlive HeartbeatLoop. Close their
  // independently owned pointer gate before waiting on any runtime activity;
  // late completions then observe nullptr without capturing this server.
  install_snapshot_callback_gate_->Close();
  CloseReplicationWakeGate();

  while (heartbeat_loop_running_.load(
             rusty::sync::atomic::Ordering::Acquire) ||
         election_loop_running_.load(
             rusty::sync::atomic::Ordering::Acquire) ||
         transfer_election_jobs_.load(
             rusty::sync::atomic::Ordering::Acquire) != 0 ||
         install_snapshot_callback_gate_->ActiveCallbacks() != 0) {
    if (Fiber::current_fiber().is_some()) {
      Fiber::sleep(1000);
    } else {
      // Production shutdown runs from a native worker thread rather than a
      // reactor fiber.  The owner PollThread remains live until this barrier
      // completes, so a short native sleep lets it drain both loop fibers.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  // Applying an entry can trigger snapshot compaction and reserve a persistence
  // ticket. Stop and join that producer before taking the stable drain target.
  apply_thread_running_.store(false);
  if (apply_thread_.joinable()) {
    apply_thread_.join();
  }

  // Admission is closed and every loop/apply producer is quiescent. Drain both
  // synchronous RPC handlers and async workers through the common sequence.
  DrainLogPersistenceSequence();

  // Persistence workers retain both `this` and commo(). Test Kill disconnects
  // and rewrites communicator proxy maps immediately after this method, so the
  // shutdown barrier must join them here rather than deferring the join to the
  // destructor.
  DrainAsyncPersistenceThreads();

  // This native monitor is not owned by the reactor. Join it explicitly only
  // after the Raft loops are quiescent and without holding mtx_.
  StopLeadershipTransferMonitoring();
}

// @unsafe - Election timeout calculation (Time::now and RandomGenerator::rand marked safe via @external)
uint64_t RaftServer::GetElectionTimeout() {
  // Keep the configured identity stable for the entire decision. This method
  // is also called from paths that already own mtx_, hence the recursive lock.
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  uint64_t current_time = Time::now(true);
  const uint64_t grace_period_us = GetPreferredLeaderGracePeriodUs();
  bool in_grace_period = (current_time - startup_timestamp_) < grace_period_us;
  uint64_t randomized_timeout = 0;
  const bool preferred_leader_configured =
      IsPreferredLeaderConfigured(preferred_leader_site_id_);
  const bool storage_configured = HasConfiguredStorage();
  // Apply one common shift to every role in a configured preferred-leader
  // cluster. This retains both each range's jitter and the intended separation
  // between preferred, steady non-preferred, and startup-grace deadlines. The
  // grace range is environment-tunable, so include both endpoints when finding
  // the true common minimum even if an operator supplies them in reverse.
  uint64_t randomized_minimum = kNonPreferredSteadyElectionMinUs;
  if (storage_configured && preferred_leader_configured) {
    randomized_minimum = std::min(
        kPreferredElectionMinUs,
        std::min(GetNonPreferredGraceElectionMinUs(),
                 GetNonPreferredGraceElectionMaxUs()));
  }

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

  // Leader-local log and commit-metadata persistence remains synchronous under
  // mtx_ in both persistence modes; follower async workers and completion
  // callbacks add further contention. Keep the follower deadline at least
  // twenty heartbeat periods so a healthy but storage-bound leader is not
  // displaced between batches. Shift the entire randomized range so its jitter
  // is retained rather than collapsing every timer onto the floor. With the
  // production 5ms heartbeat, this floor remains below every existing
  // randomized timeout.
  return raft_server_effective_election_timeout(
      randomized_timeout, randomized_minimum, heartbeat_interval_us_,
      storage_configured);
}

// Enqueue newly committed entries for the background apply thread.
// Called from OnAppendEntries (already under mtx_) when commitIndex advances.
void RaftServer::EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit) {
  // apply_queue_ now holds Command — direct copy from
  // RaftData::log_ (also Command after prep2).
  std::vector<std::pair<slotid_t, Command>> batch;
  slotid_t first_missing = 0;
  for (slotid_t id = old_commit + 1; id <= new_commit; id++) {
    auto it = raft_logs_.find(id);
    if (it != raft_logs_.end() && it->second && it->second->log_.has_value()) {
      batch.emplace_back(id, it->second->log_);
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
void RaftServer::PublishAppliedIndex(uint64_t index) {
  // Every caller owns the state-machine apply gate. Taking the Raft mutex
  // here completes the documented apply-gate -> Raft-state lock order and
  // keeps the legacy executeIndex field synchronized with consensus readers.
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  const uint64_t published = GetAppliedIndex();
  if (raft_server_log_index_above(published, index)) {
    Log_warn("[RAFT-APPLY] Site {} refusing to move applied index backward "
             "from {} to {}",
             site_id_, published, index);
    return;
  }
  executeIndex = index;
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
          Log_info("[APPLY-THREAD] Site {}: applied {} entries, executeIndex={} queue_remaining={}",
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

        // Route periodic cleanup through the snapshot-aware, persistence-ordered
        // compactor. It will retain any prefix not yet covered by a snapshot.
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
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            commit_index_snapshot = commitIndex;
          }
          Log_info("[APPLY-THREAD] Site {}: IDLE executeIndex={} commitIndex={} queue_size={} applied_total={}",
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
  // Keep every handler fail-closed until recovery, snapshot loading, and the
  // committed state-machine suffix have all completed.
  rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);

  // Record startup time for grace period logic
  startup_timestamp_ = Time::now(true);

  // ========== INITIALIZE PERSISTENCE (LogStorage + RecoveryManager) ==========
  const char* persistence_flag = std::getenv("MAKO_RAFT_PERSISTENCE");
  bool should_enable = (persistence_flag &&
                       (strcmp(persistence_flag, "1") == 0 ||
                        strcmp(persistence_flag, "true") == 0));

  if (should_enable) {
    // Check if async persistence is requested (default: sync)
    const char* async_flag = std::getenv("MAKO_RAFT_ASYNC_PERSISTENCE");
    async_persistence_ = (async_flag &&
                         (strcmp(async_flag, "1") == 0 ||
                          strcmp(async_flag, "true") == 0));

    Log_info("[RAFT-PERSISTENCE] Initializing LogStorage for site {} partition {} (mode={})",
             site_id_, partition_id_, async_persistence_ ? "async" : "sync");

    // Create RecoveryConfig
    raft::RecoveryConfig config;
    std::string base_path = "/tmp";
    const char* custom_path = std::getenv("MAKO_RAFT_PERSISTENCE_PATH");
    if (custom_path && custom_path[0] != '\0') {
      base_path = custom_path;
    }
    config.storage_path = base_path + "/raft_" + std::to_string(site_id_) +
                         "_partition_" + std::to_string(partition_id_);

    // Create RecoveryManager and storage
    raft::RecoveryManager manager(config);
    auto storage = manager.create_storage();

    if (!storage) {
      Log_error("[RAFT-PERSISTENCE] Failed to create configured LogStorage; "
                "site {} will not start",
                site_id_);
      stop_.store(true, rusty::sync::atomic::Ordering::Release);
      looping_.store(false, rusty::sync::atomic::Ordering::Release);
      return false;
    } else {
      // Use RecoveryManager to orchestrate recovery
      auto result = manager.recover(
        [this](std::shared_ptr<janus::raft::LogStorage> s) { SetLogStorage(s); },
        [this]() { return RecoverFromStorage(); },
        [this](raft::RecoveryResult& r) {
          r.recovered_term = currentTerm;
          r.recovered_entries = raft_logs_.size();
        }
      );

      if (result.success) {
        Log_info("[RAFT-PERSISTENCE] Recovery complete: mode={} term={} entries={} time={}ms",
                 static_cast<int>(result.mode), result.recovered_term,
                 result.recovered_entries, result.recovery_time_ms);
      } else {
        Log_error("[RAFT-PERSISTENCE] Recovery failed: {}", result.error_message.c_str());
        stop_.store(true, rusty::sync::atomic::Ordering::Release);
        looping_.store(false, rusty::sync::atomic::Ordering::Release);
        return false;
      }
    }
  } else {
    Log_info("[RAFT-PERSISTENCE] Disabled (set MAKO_RAFT_PERSISTENCE=1 to enable)");
  }

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

  // ========== INITIALIZE REPLICATED DB (optional) ==========
  // Construct/register the state-machine loader before snapshot discovery.
  // Recovery must restore the verified checkpoint bytes before it publishes
  // the covered applied index or starts any runtime loop.
  // @unsafe { std::getenv, std::make_shared, RegLearnerAction, Log_info }
  {
    const char* rdb_flag = std::getenv("MAKO_REPLICATED_DB");
    if (rdb_flag && (strcmp(rdb_flag, "1") == 0 || strcmp(rdb_flag, "true") == 0)) {
      std::string db_path;
      const char* custom_path = std::getenv("MAKO_REPLICATED_DB_PATH");
      if (custom_path && custom_path[0] != '\0') {
        db_path = std::string(custom_path) + "/replicated_db_" + std::to_string(site_id_);
      } else {
        db_path = "/tmp/mako_replicated_db_" + std::to_string(site_id_);
      }
      replicated_db_ = std::make_shared<ReplicatedDB>(this, db_path);
      if (!replicated_db_->IsOpen()) {
        Log_error("[RAFT-REPLICATED-DB] Failed to initialize site {} at {}",
                  site_id_, db_path.c_str());
        stop_.store(true, rusty::sync::atomic::Ordering::Release);
        looping_.store(false, rusty::sync::atomic::Ordering::Release);
        return false;
      }

      // Validate the recovered application's own durable marker before a
      // snapshot loader is allowed to replace that database. Checking only
      // after InitializeSnapshotManager() is too late: an older checkpoint
      // could erase the evidence that the application was ahead of Raft's
      // recovered commit boundary.
      const uint64_t recovered_application_index =
          replicated_db_->GetLastAppliedIndex();
      if (recovered_application_index > commitIndex) {
        Log_error("[RAFT-RECOVERY] Site {} state machine is ahead of durable "
                  "Raft commit before snapshot restore: applied={} commit={}; "
                  "refusing unsafe startup",
                  site_id_, recovered_application_index, commitIndex);
        stop_.store(true, rusty::sync::atomic::Ordering::Release);
        looping_.store(false, rusty::sync::atomic::Ordering::Release);
        return false;
      }

      // Register apply callback so committed Raft entries are applied to RocksDB
      RegLearnerAction([this](slotid_t slot, janus::Command md) -> int {
        if (replicated_db_ && !replicated_db_->ApplyEntry(slot, md)) {
          throw std::runtime_error("ReplicatedDB atomic apply failed");
        }
        return 0;
      });

      Log_info("[RAFT-REPLICATED-DB] Initialized for site {} at path {}",
               site_id_, db_path.c_str());
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
  if (replicated_db_ &&
      replicated_db_->GetLastAppliedIndex() > commitIndex) {
    Log_error("[RAFT-RECOVERY] Site {} state machine is ahead of durable Raft "
              "commit: applied={} commit={}; refusing unsafe startup",
              site_id_, replicated_db_->GetLastAppliedIndex(), commitIndex);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    return false;
  }

  // ========== INITIALIZE MEMBERSHIP CONFIGURATION ==========
  // Populate current_config_ from the static partition configuration.
  // This gives us the initial set of replicas; AddServer/RemoveServer will
  // modify it dynamically at runtime.
  {
    auto config = Config::GetConfig();
    auto replicas = config->SitesByPartitionId(partition_id_);
    for (auto& site : replicas) {
      current_config_.insert(site.id);
    }
    Log_info("[RAFT-CONFIG] Initialized current_config_ for site {} partition {} with {} replicas",
             site_id_, partition_id_, current_config_.size());
  }

  // Rebuild application state synchronously while RPC admission remains
  // closed. A ready replica must never advertise a committed index whose
  // state-machine effect is still queued or whose log entry is missing.
  const bool replayed = ReplayCommittedEntries();
  if (!replayed || executeIndex != commitIndex) {
    Log_error("[RAFT-RECOVERY] Site {} failed committed replay: "
              "executeIndex={} commitIndex={}",
              site_id_, executeIndex, commitIndex);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    return false;
  }

  StartApplyThread();
  rpc_ready_.store(true, rusty::sync::atomic::Ordering::Release);

#ifdef RAFT_TEST_CORO
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
#endif

#ifndef RAFT_TEST_CORO
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
#endif

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
  std::lock_guard<std::recursive_mutex> lock(mtx_);
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
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  if (is_leader_) {
    return site_id_;
  }
  return current_leader_id_;
}

RaftSubmissionProgress RaftServer::GetSubmissionProgress(
    uint64_t index, uint64_t appended_term) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  const std::pair<slotid_t, ballot_t> submission{
      static_cast<slotid_t>(index),
      static_cast<ballot_t>(appended_term)};
  const auto snapshot_resolution =
      resolved_submissions_.Consume(submission);
  if (snapshot_resolution.first) {
    return snapshot_resolution.second;
  }

  bool entry_matches = false;
  const auto entry = raft_logs_.find(index);
  if (entry != raft_logs_.end() && entry->second != nullptr) {
    entry_matches =
        static_cast<uint64_t>(entry->second->term) == appended_term;
  }
  const bool entry_known_conflict =
      !entry_matches && index >= min_active_slot_;

  // If the submitted slot disappeared behind a committed entry from a newer
  // term, the newer prefix conflicts with the submitted entry's original log
  // and makes its reappearance impossible even when commitIndex is below the
  // submitted slot (for example, a new leader truncates a long private tail).
  uint64_t committed_term = 0;
  if (!entry_matches && commitIndex > 0) {
    if (commitIndex == snapidx_ && snapidx_ > 0) {
      committed_term = snapterm_;
    } else {
      const auto committed_entry = raft_logs_.find(commitIndex);
      if (committed_entry != raft_logs_.end() &&
          committed_entry->second != nullptr) {
        committed_term =
            static_cast<uint64_t>(committed_entry->second->term);
      }
    }
  }
  const bool committed_newer_prefix = committed_term > appended_term;

  // A higher currentTerm cannot tell us whether this old-term entry will be
  // retained (Raft Figure 8). Keep the submission pending until the committed
  // prefix crosses its exact slot, then resolve it by entry identity.
  const RaftSubmissionProgress progress{
      /*committed=*/raft_server_submission_is_committed(
          commitIndex, index, entry_matches),
      /*superseded=*/raft_server_submission_is_superseded(
          commitIndex, index, entry_known_conflict,
          committed_newer_prefix),
      /*indeterminate=*/false};
  if (progress.committed || progress.superseded || progress.indeterminate) {
    active_submissions_.erase(submission);
  }
  return progress;
}

// @unsafe - Caller holds mtx_. Moves every covered tracked submission into a
// one-shot terminal ledger before InstallSnapshot destroys its local identity.
void RaftServer::ResolveSnapshotCoveredSubmissionsLocked(
    slotid_t last_included_index,
    ballot_t last_included_term,
    bool snapshot_prefix_matches) {
  size_t resolved_count = 0;
  auto active = active_submissions_.begin();
  while (active != active_submissions_.end() &&
         raft_server_snapshot_resolves_submission(
             last_included_index, active->first)) {
    const auto submission = *active;
    const auto local_entry = raft_logs_.find(submission.first);
    const bool local_entry_exists =
        local_entry != raft_logs_.end() &&
        local_entry->second != nullptr &&
        local_entry->second->log_.has_value();
    const bool local_entry_matches =
        local_entry_exists &&
        local_entry->second->term == submission.second;
    const bool local_entry_known_conflict =
        local_entry_exists && !local_entry_matches;
    const bool local_commit_crossed = commitIndex >= submission.first;

    // A matching boundary proves that the local and incoming prefixes are
    // identical. A divergent snapshot proves only its exact boundary entry;
    // older covered identities remain ambiguous unless the local committed
    // prefix had already made their outcome definitive. Missing local entries
    // are likewise not evidence of a conflict.
    const bool committed = raft_server_snapshot_submission_is_committed(
        last_included_index, last_included_term,
        submission.first, submission.second,
        local_entry_matches, local_commit_crossed,
        snapshot_prefix_matches);
    const bool superseded = raft_server_snapshot_submission_is_superseded(
        last_included_index, last_included_term,
        submission.first, submission.second,
        local_entry_known_conflict, local_commit_crossed,
        snapshot_prefix_matches);
    const RaftSubmissionProgress progress{
        /*committed=*/committed,
        /*superseded=*/superseded,
        /*indeterminate=*/raft_server_snapshot_submission_is_indeterminate(
            last_included_index, submission.first, committed, superseded)};
    verify(resolved_submissions_.Record(submission, progress));
    active = active_submissions_.erase(active);
    resolved_count++;
  }

  if (resolved_count != 0) {
    Log_debug("[INSTALL-SNAPSHOT] Site {} preserved {} covered submission "
              "outcome(s) through index {}",
              site_id_, resolved_count, last_included_index);
  }
}

// @unsafe - Caller holds mtx_; Config remains live for the server lifetime.
int RaftServer::LeaderSiteToLocaleLocked(siteid_t leader_site) const {
  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool self_is_member = current_config_.count(site_id_) != 0;
  const int self_locale =
      self_is_member &&
              loc_id_ <= static_cast<locid_t>(std::numeric_limits<int>::max())
          ? static_cast<int>(loc_id_)
          : -1;
  int mapped_locale = -1;

  if (leader_site != invalid && leader_site != site_id_ &&
      current_config_.count(leader_site) != 0) {
    const Config* config = Config::GetConfig();
    if (static_cast<size_t>(leader_site) < config->sites_.size()) {
      const Config::SiteInfo& site = config->sites_[leader_site];
      if (site.id == leader_site && site.partition_id_ == partition_id_ &&
          site.locale_id <=
              static_cast<uint32_t>(std::numeric_limits<int>::max())) {
        mapped_locale = static_cast<int>(site.locale_id);
      }
    }
  }

  return raft_server_view_leader_locale(
      leader_site, site_id_, self_locale, mapped_locale, invalid);
}

// @unsafe - Caller holds mtx_; Config remains live for the server lifetime.
siteid_t RaftServer::LeaderLocaleToSiteLocked(int leader_locale) const {
  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool self_is_member = current_config_.count(site_id_) != 0;
  const int self_locale =
      self_is_member &&
              loc_id_ <= static_cast<locid_t>(std::numeric_limits<int>::max())
          ? static_cast<int>(loc_id_)
          : -1;
  siteid_t mapped_site = invalid;

  if (leader_locale >= 0 && leader_locale != self_locale) {
    const Config* config = Config::GetConfig();
    for (siteid_t candidate_id : current_config_) {
      if (candidate_id == invalid ||
          static_cast<size_t>(candidate_id) >= config->sites_.size()) {
        continue;
      }
      const Config::SiteInfo& candidate = config->sites_[candidate_id];
      if (candidate.id != candidate_id ||
          candidate.partition_id_ != partition_id_ ||
          candidate.locale_id != static_cast<uint32_t>(leader_locale)) {
        continue;
      }
      if (mapped_site != invalid && mapped_site != candidate_id) {
        Log_error("[RAFT_VIEW] Partition {} has ambiguous locale {} at sites "
                  "{} and {}",
                  partition_id_, leader_locale, mapped_site, candidate_id);
        mapped_site = invalid;
        break;
      }
      mapped_site = candidate_id;
    }
  }

  return raft_server_recovery_leader_site(
      leader_locale, self_locale,
      self_is_member ? site_id_ : invalid, mapped_site, invalid);
}

rusty::Arc<ViewData> RaftServer::GetCurrentViewData() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  const siteid_t hint = is_leader_ ? site_id_ : current_leader_id_;
  const int leader = LeaderSiteToLocaleLocked(hint);
  const int replicas = static_cast<int>(current_config_.size());
  return rusty::Arc<ViewData>::make(
      View(replicas, leader, currentTerm), partition_id_);
}

bool RaftServer::ValidateRecoveryView(const ViewData& incoming_view_data,
                                      bool allow_empty) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  const View& incoming_view = incoming_view_data.GetView();
  const int expected_replicas =
      current_config_.size() <=
              static_cast<size_t>(std::numeric_limits<int>::max())
          ? static_cast<int>(current_config_.size())
          : -1;
  if (!raft_server_recovery_view_shape_is_valid(
          incoming_view_data.partition_id_, partition_id_,
          incoming_view.n_, expected_replicas,
          static_cast<uint64_t>(incoming_view.leaders_.size()),
          allow_empty)) {
    Log_warn("[RAFT_VIEW] Site {} rejected malformed recovery view envelope "
             "for partition {}: incoming_partition={} replicas={} "
             "expected_replicas={} leaders={} allow_empty={}",
             site_id_, partition_id_, incoming_view_data.partition_id_,
             incoming_view.n_, expected_replicas,
             incoming_view.leaders_.size(), allow_empty);
    return false;
  }

  if (incoming_view.IsEmpty()) {
    return true;
  }

  const int leader_locale = incoming_view.GetLeader();
  // A historical old view predates the current membership and may name a
  // removed member. Its envelope and scalar shape are still validated above,
  // but only the incoming new view is authoritative enough to require current
  // membership mapping.
  if (allow_empty) {
    // Locale IDs come from the static partition catalogue and can be sparse
    // after a membership change (for example {0,2} with n=2). A historical
    // view must not be range-checked against its replica count.
    return leader_locale >= 0;
  }

  const siteid_t mapped_leader = LeaderLocaleToSiteLocked(leader_locale);
  if (mapped_leader == static_cast<siteid_t>(INVALID_SITEID) ||
      current_config_.count(mapped_leader) == 0) {
    Log_warn("[RAFT_VIEW] Site {} rejected unmappable recovery leader locale "
             "{} for partition {}",
             site_id_, leader_locale, partition_id_);
    return false;
  }
  return true;
}

bool RaftServer::ObserveRecoveryView(const ViewData& incoming_view_data) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  if (!ValidateRecoveryView(incoming_view_data, /*allow_empty=*/false)) {
    return false;
  }

  const View& incoming_view = incoming_view_data.GetView();
  const int leader_locale = incoming_view.GetLeader();
  const epoch_t incoming_view_id = incoming_view.view_id_;

  const epoch_t local_view_id = static_cast<epoch_t>(currentTerm);
  const bool term_matches = raft_server_recovery_view_matches_term(
      incoming_view_id, local_view_id);
  if (!term_matches) {
    Log_warn("[RAFT_VIEW] Site {} rejected non-current recovery view {} "
             "while local term is {}",
             site_id_, incoming_view_id, currentTerm);
    return false;
  }

  const siteid_t leader_site = LeaderLocaleToSiteLocked(leader_locale);
  if (leader_site == static_cast<siteid_t>(INVALID_SITEID)) {
    Log_warn("[RAFT_VIEW] Site {} rejected unmappable recovery leader locale "
             "{} for partition {}",
             site_id_, leader_locale, partition_id_);
    return false;
  }

  const bool leader_is_self = leader_site == site_id_;
  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool has_known_leader = current_leader_id_ != invalid;
  const bool known_leader_matches_view = current_leader_id_ == leader_site;
  if (!raft_server_recovery_view_matches_role(
          term_matches, is_leader_, leader_is_self,
          has_known_leader, known_leader_matches_view)) {
    Log_warn("[RAFT_VIEW] Site {} rejected recovery leader site {} (locale "
             "{}) inconsistent with local role {} and known leader {} in "
             "term {}",
             site_id_, leader_site, leader_locale,
             is_leader_ ? "leader" : "follower/candidate",
             current_leader_id_, currentTerm);
    return false;
  }

  // A same-term recovery view is routing evidence, not a Raft term-change
  // mechanism. Leaders may only reaffirm self. Followers/candidates may only
  // observe a remote leader, which also cancels an in-flight local election.
  current_leader_id_ = leader_site;
  if (!is_leader_) {
    setIsLeader(false);
    req_voting_ = false;
    election_in_progress_ = false;
    earlyDurableVoters_.clear();
    resetTimer("accepted same-term recovery leader");
  }
  return true;
}

bool RaftServer::RecoveryOperationIsCurrent(epoch_t operation_epoch,
                                            const View& accepted_view) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  const epoch_t local_term = static_cast<epoch_t>(currentTerm);
  if (operation_epoch != local_term ||
      accepted_view.view_id_ != operation_epoch ||
      accepted_view.leaders_.size() != 1) {
    return false;
  }

  const siteid_t accepted_leader =
      LeaderLocaleToSiteLocked(accepted_view.GetLeader());
  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const siteid_t authoritative_leader =
      is_leader_ ? site_id_ : current_leader_id_;
  return accepted_leader != invalid && authoritative_leader != invalid &&
      current_config_.count(accepted_leader) != 0 &&
      accepted_leader == authoritative_leader;
}

// @unsafe - Leadership state transition (callbacks and logging wrapped in @unsafe blocks)
void RaftServer::setIsLeader(bool isLeader) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  bool prev_is_leader = is_leader_;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site {} (loc {}) term {}: prev_is_leader={} new_is_leader={}",
           site_id_, loc_id_, currentTerm, prev_is_leader, isLeader);
#endif

  if (isLeader && !prev_is_leader) {
    // Leadership publication is an accepted-order barrier: no suffix write,
    // vote, or metadata mutation from the prior follower/candidate epoch may
    // remain outstanding when is_leader_ becomes observable.
    const uint64_t publication_term = currentTerm;
    DrainLogPersistenceSequence();
    if (stop_.load(rusty::sync::atomic::Ordering::Acquire) ||
        currentTerm != publication_term) {
      Log_warn("[RAFT_STATE] Site {} suppressing stale leadership publication "
               "for term {} (current={}, stopping={})",
               site_id_, publication_term, currentTerm,
               stop_.load(rusty::sync::atomic::Ordering::Acquire));
      return;
    }
  }

  if (isLeader) {
    // A heartbeat proof belongs to exactly one leadership term. Reset the
    // local generation before publishing this server as leader so delayed or
    // historical acknowledgements can never authorize a new-term read.
    heartbeat_round_ = 0;
    read_quorum_confirmed_term_ = 0;
    read_quorum_confirmed_round_ = 0;
  }

  if (isLeader && failover_) {
    std::set<siteid_t> replication_targets = current_config_;
    replication_targets.insert(learners_.begin(), learners_.end());
    for (const auto peer_id : replication_targets) {
      if (peer_id == site_id_) {
        continue;
      }
      match_index_[peer_id] = 0;
      next_index_[peer_id] = lastLogIndex + 1;
      Log_debug("loc_id_={} match_index_[{}]={}, next_index_[{}]={}",
                loc_id_, peer_id, match_index_[peer_id],
                peer_id, next_index_[peer_id]);
    }
    const size_t expected = replication_targets.size() -
        static_cast<size_t>(replication_targets.count(site_id_) > 0);
    verify(match_index_.size() == expected);
    verify(next_index_.size() == expected);
  }


  // This 2 lines MUST put BEFORE is_leader_ = isLeader ! otherwise they will become 0, and new view will without leader
  bool become_new_leader = isLeader && (!is_leader_);
  bool become_new_follower = (!isLeader) && is_leader_;

  // Update the leader state after view handling
  is_leader_ = isLeader;

  // Becoming leader establishes self as the known leader. Becoming a follower
  // deliberately preserves a hint learned from AppendEntries/InstallSnapshot;
  // transitions without a known leader clear it at their call sites.
  current_leader_id_ = raft_server_leader_hint_after_transition(
      isLeader,
      !isLeader && current_leader_id_ != INVALID_SITEID,
      site_id_, current_leader_id_, static_cast<siteid_t>(INVALID_SITEID));

  // Only log on actual transitions, not no-op calls
  if (become_new_leader || become_new_follower) {
    Log_info("RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}", site_id_, become_new_leader, become_new_follower, isLeader);
  }

  // Only update view when transitioning from non-leader to leader
  if (become_new_leader) {
    Log_info("[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}",
             site_id_, currentTerm, prev_is_leader, become_new_leader);

#ifndef RAFT_TEST_CORO
    // Raft only commits prior-term entries after committing an entry from the
    // current term. Append one internal no-op before publishing the new view,
    // so old client submissions resolve even when every client is blocked on
    // the former leader. The apply paths consume this protocol entry without
    // invoking the application state machine.
    uint64_t noop_previous_index = 0;
    uint64_t noop_term = 0;
    auto noop = rusty::Arc<TpcNoopCommand>::make();
    const RaftStartResult noop_result = SetLocalAppend(
        janus::Command::pack_aliased<TpcNoopCommand>(std::move(noop)),
        &noop_term, &noop_previous_index);
    if (!raft_server_start_was_appended(noop_result)) {
      Log_error("[RAFT-NOOP] Site {} failed to append the leader no-op for "
                "term {}; leadership publication aborted",
                site_id_, currentTerm);
      return;
    }
    verify(noop_term == currentTerm);
    verify(lastLogIndex == noop_previous_index + 1);
    Log_info("[RAFT-NOOP] Site {} appended leader no-op at index {} term {}",
             site_id_, lastLogIndex, currentTerm);
    RequestReplication();
#endif

    // ============================================================================
    // LEADERSHIP TRANSFER: Clear transfer flags when becoming leader
    // ============================================================================
    // If we just became leader, any previous transfer is now complete
    transferring_leadership_ = false;

    // Only update view if we have enough information (not during initialization)
    if (partition_id_ != 0xFFFFFFFF && site_id_ != INVALID_SITEID) {
      const View old_view = current_view_;
      const int n_replicas = static_cast<int>(current_config_.size());
      current_view_ = View(n_replicas, site_id_, currentTerm);
      Log_info("[RAFT_VIEW] Server {} became leader for partition {}, term={}, old_view={}, new_view={}", 
               site_id_, partition_id_, currentTerm, 
               old_view.ToString().c_str(), current_view_.ToString().c_str());
    }

    // ============================================================================
    // LEADERSHIP TRANSFER: Start monitoring if non-preferred leader
    // ============================================================================
    // If we just became a non-preferred leader, start monitoring for transfer
    // opportunity. This ensures that after failover/elections, non-preferred
    // leaders will transfer back to preferred leaders when they catch up.
    if (!AmIPreferredLeader() &&
        looping_.load(rusty::sync::atomic::Ordering::Acquire)) {
      Log_info("[LEADERSHIP-TRANSFER] Site {}: Became non-preferred leader, starting transfer monitoring",
               site_id_);
      StartLeadershipTransferMonitoring();
    }
  } else if (become_new_follower) {
    Log_info("[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}",
             site_id_, currentTerm, prev_is_leader, become_new_follower);

    // ============================================================================
    // CRITICAL FIX: Reset election timer when becoming follower
    // ============================================================================
    // This prevents instant elections after recovery/resume. When a node resumes
    // from SIGSTOP/pause, last_heartbeat_time_ is stale (from before pause).
    // Resetting it here ensures the election timer counts from NOW, giving the
    // current leader time to send heartbeats before this node starts an election.
    // This is standard Raft behavior: followers reset their timer when stepping down.
    resetTimer("became follower");
    Log_info("[RAFT_TIMER] Site {} reset election timer when becoming follower (last_hb now={})",
             site_id_, last_heartbeat_time_);

    // When transitioning from leader to non-leader
    Log_info("[RAFT_VIEW] Server {} stepping down as leader for partition {}", site_id_, partition_id_);

    // ============================================================================
    // LEADERSHIP TRANSFER: Stop monitoring when stepping down
    // ============================================================================
    // The single monitor stays alive but idle while this server is a follower.
    // Keeping it joinable until final shutdown avoids both a detached `this`
    // capture and joining while setIsLeader's recursive Raft lock is held.

    // View will be updated when we learn about the new leader
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

// @unsafe - Caller holds mtx_. Direct map lookup is deliberately non-mutating:
// GetRaftInstance() would recreate a compacted entry and corrupt the boundary.
bool RaftServer::HasCommittedEntryInCurrentTermLocked() const {
  uint64_t committed_term = 0;
  bool term_available = false;

  if (commitIndex == snapidx_ && snapidx_ > 0) {
    committed_term = snapterm_;
    term_available = true;
  } else {
    const auto committed = raft_logs_.find(commitIndex);
    if (committed != raft_logs_.end() && committed->second != nullptr) {
      committed_term = committed->second->term;
      term_available = true;
    }
  }

  return term_available &&
      raft_server_read_index_has_current_term_commit(
          commitIndex, committed_term, currentTerm);
}

// @unsafe - Implements the quorum-confirmed ReadIndex protocol without a wire
// change. A local heartbeat generation ties each response to a send that began
// after this read's baseline; historical match/vote state is never sufficient.
bool RaftServer::ReadIndex(uint64_t timeout_us) {
  constexpr uint64_t kWaitStepUs = 100;
  const auto started_at = std::chrono::steady_clock::now();
  const bool running_in_fiber = Fiber::current_fiber().is_some();

  auto elapsed_us = [&]() -> uint64_t {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started_at).count();
    return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
  };

  auto wait_for_progress = [&]() -> bool {
    uint64_t wait_us = kWaitStepUs;
    if (timeout_us != 0) {
      const uint64_t elapsed = elapsed_us();
      if (raft_server_read_index_deadline_expired(timeout_us, elapsed)) {
        return false;
      }
      wait_us = std::min(wait_us, timeout_us - elapsed);
    }
    if (running_in_fiber) {
      Fiber::sleep(static_cast<int>(wait_us));
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
    }
    return true;
  };

  std::unique_lock<std::recursive_mutex> lock(mtx_);
  const bool disconnected = disconnected_.load(std::memory_order_acquire);
  if (!raft_server_read_index_local_state_allows(
          IsLeader(), disconnected)) {
    Log_debug("[READ-INDEX] site={} rejecting read (leader={}, "
              "disconnected={})",
              site_id_, IsLeader(), disconnected);
    return false;
  }
  if (raft_server_read_index_deadline_expired(timeout_us, elapsed_us())) {
    Log_warn("[READ-INDEX] site={} deadline expired before quorum request",
             site_id_);
    return false;
  }

  const uint64_t request_term = currentTerm;
  const uint64_t baseline_round = heartbeat_round_;
  const std::set<siteid_t> request_config = current_config_;
  if (request_config.empty() || request_config.count(site_id_) == 0) {
    Log_warn("[READ-INDEX] site={} rejecting read with invalid membership",
             site_id_);
    return false;
  }

  lock.unlock();
  RequestReplication();

  uint64_t read_index = 0;
  for (;;) {
    lock.lock();
    if (raft_server_read_index_deadline_expired(timeout_us, elapsed_us())) {
      Log_warn("[READ-INDEX] site={} timed out waiting for a fresh quorum "
               "after round={} term={}",
               site_id_, baseline_round, request_term);
      return false;
    }
    if (!raft_server_read_index_local_state_allows(
            IsLeader(), disconnected_) ||
        currentTerm != request_term || current_config_ != request_config) {
      Log_debug("[READ-INDEX] site={} lost authority while waiting for quorum",
                site_id_);
      return false;
    }

    if (raft_server_read_index_quorum_is_fresh(
            request_term, baseline_round,
            read_quorum_confirmed_term_,
            read_quorum_confirmed_round_)) {
      if (!HasCommittedEntryInCurrentTermLocked()) {
        Log_warn("[READ-INDEX] site={} has no current-term committed entry "
                 "at commitIndex={} term={}; rejecting read",
                 site_id_, commitIndex, currentTerm);
        return false;
      }
      read_index = commitIndex;
      lock.unlock();
      break;
    }
    lock.unlock();

    if (!wait_for_progress()) {
      Log_warn("[READ-INDEX] site={} timed out waiting for a fresh quorum "
               "after round={} term={}",
               site_id_, baseline_round, request_term);
      return false;
    }
  }

  for (;;) {
    lock.lock();
    if (raft_server_read_index_deadline_expired(timeout_us, elapsed_us())) {
      Log_warn("[READ-INDEX] site={} timed out waiting for appliedIndex to "
               "reach readIndex={} (applied={})",
               site_id_, read_index, GetAppliedIndex());
      return false;
    }
    if (!raft_server_read_index_local_state_allows(
            IsLeader(), disconnected_) ||
        currentTerm != request_term || current_config_ != request_config) {
      Log_debug("[READ-INDEX] site={} lost authority while waiting for apply",
                site_id_);
      return false;
    }

    const uint64_t applied_index = GetAppliedIndex();
    if (applied_index >= read_index) {
      Log_debug("[READ-INDEX] site={} applied={} >= readIndex={} after {} us",
                site_id_, applied_index, read_index, elapsed_us());
      return true;
    }
    lock.unlock();

    if (!wait_for_progress()) {
      Log_warn("[READ-INDEX] site={} timed out waiting for appliedIndex to "
               "reach readIndex={} (applied={})",
               site_id_, read_index, GetAppliedIndex());
      return false;
    }
  }
}

// @unsafe - Applies committed logs (callbacks wrapped in @unsafe blocks)
void RaftServer::applyLogs() {
  // Log commit state for debugging
  Log_info("[APPLY-LOGS] site={} commitIndex={} executeIndex={}",
           site_id_, commitIndex, executeIndex);

  // Only mark pending if there's actually new work to apply
  if (executeIndex < commitIndex) {
    apply_pending_.store(true, std::memory_order_release);
  }

  // If already applying, return - the current apply loop will pick up our work
  if (in_applying_logs_) {
    return;
  }

  in_applying_logs_ = true;

  // Keep applying logs until no more pending work arrives
  // This ensures we never drop work even under heavy load
  do {
    // Clear the pending flag before processing
    apply_pending_.store(false, std::memory_order_release);

    // Apply all committed logs
    for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
      auto next_instance = GetRaftInstance(id);
      if (next_instance && next_instance->log_.has_value()) {
        // @unsafe
        {
        if (!raft_server_command_is_internal_noop(
                next_instance->log_.kind_, TpcNoopCommand::static_kind())) {
          Log_info("[APPLY-LOGS] site={} applying index={}", site_id_, id);
          app_next_(id, next_instance->log_);  // Pass both id and log (signature requires 2 args)
        }
        PublishAppliedIndex(id);
        }
      } else {
        Log_info("[APPLY-LOGS] site={} SKIP index={} (no instance or log)", site_id_, id);
        break;
      }
    }

    // Check if new work arrived while we were applying
    // If so, loop again to process it
  } while (apply_pending_.load(std::memory_order_acquire));

  in_applying_logs_ = false;

  // Legacy apply path uses the same race-free trigger mirrors. The current
  // queue-based runtime does not call applyLogs(), but keeping this path
  // aligned prevents a future caller from reintroducing unlocked config reads.
  if (snapshot_manager_configured_.load(
          rusty::sync::atomic::Ordering::Acquire) &&
      raft_server_snapshot_is_due(
          snapshot_trigger_index_.load(
              rusty::sync::atomic::Ordering::Acquire),
          GetAppliedIndex(),
          snapshot_trigger_threshold_.load(
              rusty::sync::atomic::Ordering::Acquire))) {
    MaybeCreateSnapshot();
  }

  // Route legacy cleanup through the same snapshot-aware, ordered compactor.
  const slotid_t cutoff = raft_server_retention_cutoff(
      executeIndex, log_retention_window_);
  if (cutoff > 0) {
    CompactLog(cutoff - 1);
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
struct PendingAppendEntries {
  siteid_t follower_id;
  shared_ptr<AppendEntriesResponse> response;  // shared_ptr ensures callback memory safety
  // migrated from
  // `shared_ptr<Marshallable>` to `janus::Command`.  Empty Command
  // (has_value() == false) signals heartbeat.
  janus::Command cmd;
  uint64_t sent_term;  // term when RPC was sent
  uint64_t sent_round;  // local heartbeat round; never sent on the wire
  // Inclusive end of the exact prefix proved by this RPC's wire payload.
  // A heartbeat proves only prevLogIndex; raw and batched payloads extend it
  // by their encoded entry count.
  uint64_t sent_end_index;
};

// ReadIndex evidence belongs to the exact heartbeat generation and membership
// snapshot that produced it. Slow synchronous followers may reply after the
// HeartbeatLoop has advanced to a later generation, so retain each generation
// until its launched RPCs have either completed or proved a quorum.
struct PendingHeartbeatAuthority {
  uint64_t term;
  std::set<siteid_t> config;
  std::set<siteid_t> voters;
  std::set<siteid_t> outstanding;
};

// @unsafe - Heartbeat loop mutates shared state, performs RPCs, and uses raw pointers.
void RaftServer::HeartbeatLoop() {
  heartbeat_loop_running_.store(
      true, rusty::sync::atomic::Ordering::Release);
  // @unsafe
  {
  auto hb_timer = new Timer();
  hb_timer->start();
  }

  parid_t partition_id = partition_id_;
  std::set<siteid_t> replication_targets = current_config_;
  replication_targets.insert(learners_.begin(), learners_.end());
  for (const auto peer_id : replication_targets) {
    if (peer_id == site_id_) {
      continue;
    }
    match_index_[peer_id] = 0;
    next_index_[peer_id] = 1;
  }
  const size_t expected = replication_targets.size() -
      static_cast<size_t>(replication_targets.count(site_id_) > 0);
  verify(match_index_.size() == expected);
  verify(next_index_.size() == expected);

  Log_debug("heartbeat loop init from site: {}", site_id_);
  looping_.store(true, rusty::sync::atomic::Ordering::Release);
  // Keep at most one AppendEntries RPC in flight per follower. A synchronous
  // follower may legitimately take longer than one heartbeat interval to
  // persist an entry; retaining its context lets a later round consume that
  // acknowledgement instead of queueing duplicate writes and discarding every
  // late success.
  std::map<siteid_t, std::unique_ptr<PendingAppendEntries>> pending_rpcs;
  std::map<uint64_t, PendingHeartbeatAuthority> authority_rounds;
  std::optional<uint64_t> pending_leader_term;
  while (looping_.load(rusty::sync::atomic::Ordering::Acquire)) {
    uint64_t term = 0;
    uint64_t round_id = 0;
    size_t nservers = 0;
    std::set<siteid_t> round_config;
    {
      if (!WaitForReplicationOrHeartbeat(heartbeat_interval_us_)) {
        break;
      }

      // ========================================================================
      // PHASE 0: Calculate commit index ONCE per heartbeat round (not per-follower)
      // ========================================================================
      uint64_t current_commit_index = 0;
      uint64_t current_last_log_index = 0;
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        if (!IsLeader()) {
          pending_rpcs.clear();
          authority_rounds.clear();
          pending_leader_term.reset();
          continue;
        }

        term = currentTerm;
        if (!pending_leader_term.has_value() ||
            *pending_leader_term != term) {
          // Leadership may be lost and regained between two observations by
          // this fiber. Never let a prior term's physical RPC occupy a slot or
          // collide with the new leader epoch's round counter reset.
          pending_rpcs.clear();
          authority_rounds.clear();
          pending_leader_term = term;
        }
        if (raft_server_read_index_round_can_advance(heartbeat_round_)) {
          ++heartbeat_round_;
        } else {
          // Saturation is fail-closed for new reads: the round never wraps, so
          // no post-baseline proof can be forged from an old generation.
          Log_error("[READ-INDEX] site={} heartbeat round saturated in term {}",
                    site_id_, currentTerm);
        }
        round_id = heartbeat_round_;
        round_config = current_config_;
        nservers = round_config.size();
        verify(nservers > 0 && round_config.count(site_id_) == 1);

        std::vector<uint64_t> matchedIndices{};
        for (auto it = match_index_.begin(); it != match_index_.end(); it++) {
          // Exclude learners from quorum calculation
          if (learners_.count(it->first) > 0) continue;
          matchedIndices.push_back(it->second);
          Log_debug("[COMMIT-CALC] match_index_[{}] = {}", it->first, it->second);
        }
        Log_debug("[COMMIT-CALC] nservers={}, matchedIndices.size()={}", nservers, matchedIndices.size());
        verify(matchedIndices.size() == nservers - 1);
        std::sort(matchedIndices.begin(), matchedIndices.end());
        uint64_t newCommitIndex = matchedIndices[(nservers - 1) / 2];
        Log_debug("[COMMIT-CALC] newCommitIndex={} (median at index {}), currentCommitIndex={}", newCommitIndex, (nservers - 1) / 2, commitIndex);

        if (newCommitIndex > lastLogIndex) {
          newCommitIndex = lastLogIndex;
        }

        if (newCommitIndex > commitIndex && (GetRaftInstance(newCommitIndex)->term == currentTerm)) {
          uint64_t old_commit = commitIndex;
          Log_debug("newCommitIndex {}", newCommitIndex);
          commitIndex = newCommitIndex;
          PersistCommitIndex(commitIndex, "HeartbeatLoop: leader commit");
          EnqueueCommittedEntries(old_commit, commitIndex);
        }
        current_commit_index = commitIndex;
        current_last_log_index = lastLogIndex;
      }

      auto [authority_it, authority_inserted] = authority_rounds.emplace(
          round_id, PendingHeartbeatAuthority{
                        term, round_config, {site_id_}, {}});
      // heartbeat_round_ never wraps. The only possible duplicate is the
      // deliberately fail-closed UINT64_MAX saturation generation.
      if (!authority_inserted) {
        verify(round_id == UINT64_MAX);
        authority_rounds.erase(authority_it);
      }

      // ========================================================================
      // PHASE 1: Send all AppendEntries RPCs in PARALLEL (non-blocking)
      // ========================================================================
      for (auto it = next_index_.begin(); it != next_index_.end(); it++) {
        auto site_id = it->first;
        if (site_id == site_id_) {
          continue;
        }
        if (!IsLeader()) {
          break;  // Stop sending if we lost leadership
        }
        if (pending_rpcs.count(site_id) > 0) {
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
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          if (it->second == 0) {
            Log_warn("[APPEND_ENTRIES] Repairing wrapped next_index for "
                     "follower {} at leader last index {}",
                     site_id, lastLogIndex);
            it->second = raft_server_log_index_has_successor(lastLogIndex)
                ? raft_server_follower_next_index(lastLogIndex)
                : lastLogIndex;
          }
          prevLogIndex = it->second - 1;
          if (prevLogIndex > lastLogIndex) {
            Log_info("[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > lastLogIndex ({}), fixing next_index", prevLogIndex, lastLogIndex);
            it->second = raft_server_log_index_has_successor(lastLogIndex)
                ? raft_server_follower_next_index(lastLogIndex)
                : lastLogIndex;
            prevLogIndex = it->second - 1;
          }
          // Until a payload is selected, this is a heartbeat and proves only
          // the prefix named by prevLogIndex.
          sent_end_index = raft_server_append_sent_end(prevLogIndex, 0);

          if (prevLogIndex > lastLogIndex) {
            Log_info("[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > lastLogIndex ({}), skipping",
                     site_id, prevLogIndex, lastLogIndex);
            it->second = 1;
            skip_follower = true;
          } else if (it->second < min_active_slot_ && snapshot_manager_) {
            // @unsafe - Follower is too far behind (log compacted), send InstallSnapshot
            Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < min_active_slot_={}, sending InstallSnapshot",
                     site_id_, site_id, it->second, min_active_slot_);
            janus::raft::SnapshotMetadata snap_meta;
            std::string snap_data;
            if (snapshot_manager_->LoadLatestSnapshot(&snap_meta, &snap_data)) {
              uint64_t snap_last_idx = snap_meta.last_included_index;
              uint64_t snap_last_term = snap_meta.last_included_term;
              uint64_t send_term = currentTerm;
              auto callback_lifetime = async_callback_lifetime_;
              commo()->SendInstallSnapshot(
                  site_id, partition_id_,
                  send_term, site_id_,
                  snap_last_idx, snap_last_term,
                  snap_data,
                  [callback_lifetime, site_id, snap_last_idx, send_term](uint64_t follower_term) {
                    std::lock_guard<std::mutex> lifetime_lock(
                        callback_lifetime->mutex);
                    auto* server = callback_lifetime->server;
                    if (server == nullptr) {
                      return;
                    }
                    // @unsafe - callback modifies shared state under lock
                    std::lock_guard<std::recursive_mutex> lock(server->mtx_);
                    if (!raft_server_install_snapshot_reply_is_available(
                            follower_term)) {
                      Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} snapshot response unavailable; retaining replication indices",
                               server->site_id_, site_id);
                      return;
                    }
                    if (raft_server_observed_higher_term(
                            follower_term, server->currentTerm)) {
                      Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} has higher term {} > {}, stepping down",
                               server->site_id_, site_id, follower_term,
                               server->currentTerm);
                      const uint64_t previous_term = server->currentTerm;
                      server->currentTerm = follower_term;
                      server->vote_for_ = INVALID_SITEID;
                      const bool has_configured_storage =
                          server->HasConfiguredStorage();
                      const bool persistence_succeeded =
                          !has_configured_storage || server->PersistState(
                              server->currentTerm, server->vote_for_,
                              "InstallSnapshot reply carried newer term");
                      if (!raft_server_term_advance_is_durable(
                              has_configured_storage,
                              persistence_succeeded)) {
                        Log_error("[HEARTBEAT-SNAPSHOT] Site {} could not "
                                  "durably record response term {}; failing "
                                  "stop",
                                  server->site_id_, server->currentTerm);
                        server->current_leader_id_ =
                            static_cast<siteid_t>(INVALID_SITEID);
                        server->stepDown(StepDownReason::HigherTerm);
                        server->rpc_ready_.store(
                            false, rusty::sync::atomic::Ordering::Release);
                        server->stop_.store(
                            true, rusty::sync::atomic::Ordering::Release);
                        server->looping_.store(
                            false, rusty::sync::atomic::Ordering::Release);
                        server->apply_thread_running_.store(false);
                        return;
                      }
                      server->LogTermChange(
                          "InstallSnapshot reply carried newer term",
                          previous_term, server->currentTerm, site_id);
                      // A follower's higher term does not identify the leader
                      // of that term. Retire the previous leader hint before
                      // publishing follower state.
                      server->current_leader_id_ =
                          raft_server_leader_hint_after_transition(
                              false, false, server->site_id_, site_id,
                              static_cast<siteid_t>(INVALID_SITEID));
                      server->stepDown(StepDownReason::HigherTerm);
                      server->req_voting_ = false;
                      server->election_in_progress_ = false;
                      server->earlyDurableVoters_.clear();
                      return;
                    }
                    if (server->currentTerm != send_term) {
                      Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Term changed since snapshot send, ignoring response",
                               server->site_id_);
                      return;
                    }
                    server->match_index_[site_id] = std::max(
                        server->match_index_[site_id], snap_last_idx);
                    server->next_index_[site_id] = std::max(
                        server->next_index_[site_id],
                        raft_server_log_index_has_successor(snap_last_idx)
                            ? raft_server_follower_next_index(snap_last_idx)
                            : snap_last_idx);
                    Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Updated follower {}: next_index={} match_index={}",
                             server->site_id_, site_id,
                             server->next_index_[site_id],
                             server->match_index_[site_id]);
                  });
              skip_follower = true;  // Skip normal AppendEntries for this follower
            } else {
              Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                       site_id_, site_id);
              skip_follower = true;
            }
          } else {
            verify(prevLogIndex <= lastLogIndex);
            if (prevLogIndex == 0) {
              prevLogTerm = 0;
            } else if (prevLogIndex == snapidx_ && snapidx_ > 0) {
              // Keep using snapshot boundary metadata after compaction.
              prevLogTerm = snapterm_;
            } else {
              auto instance = GetRaftInstance(prevLogIndex);
              if (!instance) {
                Log_error("[HEARTBEAT-SEND] [CRITICAL] GetRaftInstance({}) returned NULL! Skipping follower {}",
                          prevLogIndex, site_id);
                skip_follower = true;
              } else {
                prevLogTerm = instance->term;
              }
            }

            if (!skip_follower) {
#ifndef RAFT_BATCH_OPTIMIZATION
              Log_debug("[BATCH_CHECK] site={} follower={} next_index={} min_active_slot_={} lastLogIndex={}",
                       site_id_, site_id, it->second, min_active_slot_, lastLogIndex);
              if (it->second <= lastLogIndex) {
                if (!raft_server_append_entry_count_fits(prevLogIndex, 1)) {
                  Log_error("[HEARTBEAT-SEND] Log index exhausted after {}, "
                            "skipping follower {}",
                            prevLogIndex, site_id);
                  skip_follower = true;
                } else {
                  auto cur_log = raft_logs_.find(it->second);
                  if (cur_log == raft_logs_.end() || !cur_log->second ||
                      !cur_log->second->log_.has_value()) {
                    Log_error("[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                              it->second, site_id);
                    skip_follower = true;
                  } else {
                    const auto& curInstance = cur_log->second;
                    // cmd is Command; assign directly from
                    // curInstance->log_ (also Command).
                    cmd = curInstance->log_;
                    cmdLogTerm = curInstance->term;
                    sent_end_index =
                        raft_server_append_sent_end(prevLogIndex, 1);
                    // 2 step 1: debug log no longer needs the
                    // inner shared_ptr's raw pointer; the kind tag is
                    // a more useful identifier anyway.
                    Log_debug("[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        site_id_, it->second, site_id, cmd.kind_);
                  }
                }
              }
#endif

#ifdef RAFT_BATCH_OPTIMIZATION
              vector<rusty::Arc<TpcCommitCommand>> batch_buffer_;
              const uint64_t max_batch_entries = GetAppendEntriesBatchMaxEntries();
              const uint64_t batch_start_idx = it->second;
              Log_debug("[BATCH_CHECK] site={} follower={} next_index={} min_active_slot_={} lastLogIndex={}",
                       site_id_, site_id, it->second, min_active_slot_, lastLogIndex);
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
                   batch_start_idx < min_active_slot_)) {
                Log_error("[HEARTBEAT-BATCH] Non-contiguous source for follower {}: "
                          "prev={} start={} min_active={}; refusing to compress a hole",
                          site_id, prevLogIndex, batch_start_idx,
                          min_active_slot_);
                skip_follower = true;
              } else if (!skip_follower) {
                for (uint64_t idx = batch_start_idx;
                     idx <= lastLogIndex &&
                     batch_buffer_.size() < max_batch_entries;) {
                  auto cur_log = raft_logs_.find(idx);
                  if (cur_log == raft_logs_.end() || !cur_log->second ||
                      !cur_log->second->log_.has_value()) {
                    Log_error("[HEARTBEAT-BATCH] Missing log entry {} for follower {}; "
                              "refusing to compress a hole",
                              idx, site_id);
                    skip_follower = true;
                    break;
                  }
                  const auto& curInstance = cur_log->second;
                  // curInstance->log_ is Command; the
                  // `marshallable_cast<T>(SerializableEnvelope&)`
                  // overload (in serializable_envelope.hpp) handles
                  // this directly.
                  auto curCmd =
                      marshallable_cast<TpcCommitCommand>(curInstance->log_);
                  if (curCmd.is_none()) {
                    if (batch_buffer_.empty()) {
                      Log_info("[BATCH_SKIP] site={} idx={}: log entry is not "
                               "TpcCommitCommand (kind={}), using raw log",
                               site_id_, idx, curInstance->log_.kind_);
                      cmd = curInstance->log_;
                      cmdLogTerm = curInstance->term;
                      sent_end_index =
                          raft_server_append_sent_end(prevLogIndex, 1);
                    } else {
                      Log_info("[BATCH_STOP] site={} idx={}: ending batch before "
                               "non-TpcCommitCommand kind={}",
                               site_id_, idx, curInstance->log_.kind_);
                    }
                    break;
                  }
                  // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
                  { auto& mut_cmd = *const_cast<TpcCommitCommand*>(curCmd.as_ref().unwrap().get()); mut_cmd.term = curInstance->term; }
                  batch_buffer_.push_back(curCmd.unwrap());
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
                const bool truncated = batch_end_idx < lastLogIndex;
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
        auto pending = std::make_unique<PendingAppendEntries>();
        pending->follower_id = site_id;
        pending->cmd = cmd;
        pending->sent_term = term;
        pending->sent_round = round_id;
        pending->sent_end_index = sent_end_index;

        // Send RPC (non-blocking - just initiates the async call)
        // Response is allocated with shared_ptr - callback captures it to ensure memory validity
        pending->response = commo()->SendAppendEntries2(site_id,
                                              partition_id,
                                              -1,
                                              -1,
                                              IsLeader(),
                                              site_id_,
                                              term,
                                              prevLogIndex,
                                              prevLogTerm,
                                              current_commit_index,
                                              cmd,
                                              cmdLogTerm);

        pending_rpcs.emplace(site_id, std::move(pending));
        if (authority_inserted && round_config.count(site_id) > 0) {
          authority_it->second.outstanding.insert(site_id);
        }
      }

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

        for (auto pending_it = pending_rpcs.begin();
             pending_it != pending_rpcs.end();) {
          if (!IsLeader()) {
            stop_response_processing = true;
            break;
          }

          auto &pending = *pending_it->second;
          auto &resp = *pending.response;
          if (!resp.completed.load(std::memory_order_acquire)) {
            if (pending.sent_round == round_id) {
              waiting_for_current_round = true;
            }
            ++pending_it;
            continue;
          }

          bool stepped_down = false;
          {
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            const bool response_available =
                !(resp.status == false && resp.term == 0 &&
                  resp.last_log_index == 0);
            auto pending_authority = authority_rounds.find(pending.sent_round);
            if (pending_authority != authority_rounds.end()) {
              auto &authority = pending_authority->second;
              authority.outstanding.erase(pending.follower_id);
              if (authority.term == pending.sent_term &&
                  authority.config.count(pending.follower_id) > 0 &&
                  raft_server_read_index_reply_confirms_authority(
                      response_available, IsLeader(), pending.sent_term,
                      resp.term, currentTerm, pending.sent_round,
                      pending_authority->first)) {
                // One physical RPC exists per follower in a generation. Keep
                // a set so a future transport implementation still cannot
                // double-count one voter.
                authority.voters.insert(pending.follower_id);
              }
            }

            if (!response_available) {
              // RPC failed or no response - do nothing
            } else if (raft_server_observed_higher_term(resp.term,
                                                        currentTerm)) {
              // A higher term is authoritative regardless of the accompanying
              // status bit. Persist it before abandoning this leadership epoch.
              const uint64_t previous_term = currentTerm;
              Log_info(
                  "[STEPDOWN] Site {}: AppendEntries response from follower {} "
                  "carried higher term {} > {}",
                  site_id_, pending.follower_id, resp.term, currentTerm);
              currentTerm = resp.term;
              vote_for_ = INVALID_SITEID;
              const bool has_configured_storage = HasConfiguredStorage();
              const bool persistence_succeeded =
                  !has_configured_storage ||
                  PersistState(currentTerm, vote_for_,
                               "AppendEntries response carried newer term");
              LogTermChange("AppendEntries response carried newer term",
                            previous_term, currentTerm, pending.follower_id);
              // The responding follower proves a newer term, not its leader.
              current_leader_id_ = raft_server_leader_hint_after_transition(
                  false, false, site_id_, pending.follower_id,
                  static_cast<siteid_t>(INVALID_SITEID));
              stepDown(StepDownReason::HigherTerm);
              req_voting_ = false;
              election_in_progress_ = false;
              earlyDurableVoters_.clear();
              if (!raft_server_term_advance_is_durable(has_configured_storage,
                                                       persistence_succeeded)) {
                Log_error("[STEPDOWN] Site {} could not durably record "
                          "AppendEntries response term {}; failing stop",
                          site_id_, currentTerm);
                rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
                stop_.store(true, rusty::sync::atomic::Ordering::Release);
                looping_.store(false, rusty::sync::atomic::Ordering::Release);
                apply_thread_running_.store(false);
              }
              stepped_down = true;
            } else if (currentTerm != pending.sent_term) {
              Log_debug("[APPEND_RPC] Ignoring follower {} response from stale "
                        "send term {} (current={})",
                        pending.follower_id, pending.sent_term, currentTerm);
            } else if (resp.term != pending.sent_term) {
              // A valid follower processes AppendEntries in the leader's term
              // before replying. A lower response term cannot prove this send.
              Log_warn("[APPEND_RPC] Ignoring follower {} response term {} for "
                       "send/current term {}",
                       pending.follower_id, resp.term, pending.sent_term);
            } else if (!IsLeader()) {
              Log_debug("[APPEND_RPC] Ignoring follower {} response after "
                        "same-term leadership ended",
                        pending.follower_id);
            } else {
              auto next_index_it = next_index_.find(pending.follower_id);
              auto match_index_it = match_index_.find(pending.follower_id);
              if (next_index_it == next_index_.end() ||
                  match_index_it == match_index_.end()) {
                // A membership change removed this target while its RPC was
                // in flight. Higher-term evidence was handled above; only the
                // replication-index mutation is target-dependent.
                Log_debug(
                    "[APPEND_RPC] Ignoring replication response from removed "
                    "follower {}",
                    pending.follower_id);
              } else {
                auto &next_index = next_index_it->second;
                auto &match_index = match_index_it->second;

                if (resp.status == 0) {
                  // case 2: AppendEntries rejected - log inconsistency
                  if (resp.last_log_index > 0 &&
                      (resp.last_log_index + 1) < next_index) {
                    uint64_t old_next = next_index;
                    next_index = resp.last_log_index + 1;
                    Log_info("[LOG-RECONCILE] Site {}: Fast backoff for "
                             "follower {}: next_index {} -> {} (gap: {}, "
                             "follower reported last: {})",
                             site_id_, pending.follower_id, old_next,
                             next_index, old_next - next_index,
                             resp.last_log_index);
                  } else if (resp.last_log_index > 0 &&
                             (resp.last_log_index + 1) == next_index &&
                             next_index > 1) {
                    // Follower has prevLogIndex but still rejected, which
                    // indicates a term conflict. Step one slot further back so
                    // the next AppendEntries can overwrite conflict.
                    uint64_t old_next = next_index;
                    next_index--;
                    Log_info("[LOG-RECONCILE] Site {}: Term-conflict backoff "
                             "for follower {}: next_index {} -> {}",
                             site_id_, pending.follower_id, old_next,
                             next_index);
                  } else if (next_index > 10) {
                    uint64_t old_next = next_index;
                    next_index = next_index / 2;
                    Log_info("[LOG-RECONCILE] Site {}: Exponential backoff for "
                             "follower {}: next_index {} -> {} (halved)",
                             site_id_, pending.follower_id, old_next,
                             next_index);
                  } else if (next_index > 1) {
                    next_index--;
                    Log_debug("[LOG-RECONCILE] Site {}: Linear backoff for "
                              "follower {}: next_index {} -> {}",
                              site_id_, pending.follower_id, next_index + 1,
                              next_index);
                  } else {
                    next_index = 1;
                  }
                } else if (resp.last_log_index < pending.sent_end_index) {
                  // AppendEntries acceptance is atomic: success must cover
                  // every encoded entry. Do not turn a contradictory reply into
                  // a partial proof for speculative or durable accounting.
                  Log_warn("[APPEND_RPC] Ignoring contradictory success from "
                           "follower "
                           "{}: reported_end={} sent_end={}",
                           pending.follower_id, resp.last_log_index,
                           pending.sent_end_index);
                } else {
                  // case 3: AppendEntries accepted
                  verify(resp.status == true);

                  const uint64_t acknowledged_through =
                      raft_server_append_acknowledged_through(
                          resp.last_log_index, pending.sent_end_index,
                          lastLogIndex);
                  const bool voter_response =
                      current_config_.count(pending.follower_id) > 0 &&
                      learners_.count(pending.follower_id) == 0;

                  // ==================================================================
                  // SPECULATIVE REPLICATION: Track acknowledgement strength.
                  // A durable acknowledgement also implies the in-memory
                  // acknowledgement used to advance specCommitIndex_.  Sync
                  // followers report Durable on this response; async followers
                  // report Memory here and send AppendEntriesDurable after
                  // fsync.
                  // ==================================================================
                  const bool durable_ack =
                      raft_server_ack_is_durable(resp.ack_type);
                  if (voter_response &&
                      (raft_server_ack_is_memory(resp.ack_type) ||
                       durable_ack)) {
                    // The follower may have an unknown divergent suffix and the
                    // leader may append while this RPC is in flight. Credit
                    // only the prefix this exact wire payload proved. Indices
                    // through the speculative commit point are never scanned
                    // again this term; starting at its successor keeps repeated
                    // heartbeats O(tail).
                    if (raft_server_log_index_has_successor(specCommitIndex_) &&
                        acknowledged_through > specCommitIndex_) {
                      for (uint64_t idx = specCommitIndex_ + 1;
                           idx <= acknowledged_through; ++idx) {
                        memoryAcks_[idx].insert(pending.follower_id);
                        if (idx == acknowledged_through) {
                          break;
                        }
                      }
                    }
                    Log_debug(
                        "[SPEC-RAFT] Memory ack from follower {} through {} "
                        "(reported={} sent_end={})",
                        pending.follower_id, acknowledged_through,
                        resp.last_log_index, pending.sent_end_index);
                  }
                  if (voter_response && durable_ack &&
                      raft_server_persistence_can_report_durable(
                          HasDurableStorage())) {
                    const uint64_t durable_through = acknowledged_through;
                    if (durable_through > securedLogIndex_) {
                      for (uint64_t idx = securedLogIndex_ + 1;
                           idx <= durable_through; ++idx) {
                        durableAcks_[idx].insert(pending.follower_id);
                        if (idx == durable_through) {
                          break;
                        }
                      }
                    }
                    Log_debug(
                        "[SPEC-RAFT] Durable ack from follower {} for index {}",
                        pending.follower_id, durable_through);
                  }

                  // Successful responses are monotonic and prove no index
                  // beyond the exact payload end. In particular, a heartbeat
                  // cannot adopt an unknown follower suffix.
                  match_index = std::max(match_index, acknowledged_through);
                  if (raft_server_log_index_has_successor(
                          acknowledged_through)) {
                    next_index = std::max(
                        next_index,
                        raft_server_follower_next_index(acknowledged_through));
                  }
                  Log_debug(
                      "[APPEND_RPC] Leader {} accepted follower {} proof: "
                      "kind={} reported={} sent_end={} acknowledged={} "
                      "next={} match={}",
                      site_id_, pending.follower_id,
                      pending.cmd.has_value() ? "entries" : "heartbeat",
                      resp.last_log_index, pending.sent_end_index,
                      acknowledged_through, next_index, match_index);
                }
              }
            }
          }

          const bool completed_previous_round = pending.sent_round != round_id;
          pending_it = pending_rpcs.erase(pending_it);
          retry_released_follower =
              retry_released_follower || completed_previous_round;
          if (stepped_down) {
            stop_response_processing = true;
            break;
          }
        }

        bool current_round_has_authority = false;
        const auto current_authority = authority_rounds.find(round_id);
        if (current_authority != authority_rounds.end()) {
          const auto& authority = current_authority->second;
          current_round_has_authority =
              raft::raft_quorum_count_reached(
                  authority.voters.size(),
                  raft::raft_quorum_majority_count(
                      authority.config.size()));
        }
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
        pending_rpcs.clear();
        authority_rounds.clear();
      } else if (retry_released_follower) {
        // A completion from an older round opened a per-follower slot after
        // Phase 1. Prompt another round instead of waiting a full interval.
        RequestReplication();
      }

      // ========================================================================
      // PHASE 3: Recalculate commit index after all responses processed
      // ========================================================================
      // This ensures commits happen promptly after replication, matching the
      // original behavior where commit index was recalculated after each response.
      if (IsLeader()) {
        bool commit_advanced_after_send = false;
        {
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          std::vector<uint64_t> finalMatchedIndices{};
          for (auto it = match_index_.begin(); it != match_index_.end(); it++) {
            // Exclude learners from quorum calculation
            if (learners_.count(it->first) > 0) continue;
            finalMatchedIndices.push_back(it->second);
          }
          std::sort(finalMatchedIndices.begin(), finalMatchedIndices.end());
          uint64_t finalCommitIndex = finalMatchedIndices[(nservers - 1) / 2];
          if (raft_server_log_index_above(finalCommitIndex, lastLogIndex)) {
            finalCommitIndex =
                raft_server_commit_index_clamp(finalCommitIndex, lastLogIndex);
          }
          if (raft_server_log_index_above(finalCommitIndex, commitIndex) &&
              raft_server_log_entry_is_current_term(
                  GetRaftInstance(finalCommitIndex)->term, currentTerm)) {
            uint64_t old_commit = commitIndex;
            Log_debug("[PHASE3-COMMIT] Advancing commitIndex {} -> {}", commitIndex, finalCommitIndex);
            commitIndex = finalCommitIndex;
            PersistCommitIndex(commitIndex, "HeartbeatLoop: post-response commit");
            EnqueueCommittedEntries(old_commit, commitIndex);
            commit_advanced_after_send = true;
          }

          // ==================================================================
          // LEARNER CATCH-UP: Check if any learners are caught up and promote
          // ==================================================================
          CheckAndPromoteLearners();

        // ==================================================================
        // SPECULATIVE REPLICATION: Update specCommitIndex based on memory acks
        // ==================================================================
        size_t quorum = GetQuorumSize();

        // Find the highest index with memory ack quorum
        // Leader's own entry counts as a memory ack
        uint64_t newSpecCommitIndex = specCommitIndex_;
        for (uint64_t idx = specCommitIndex_ + 1; idx <= lastLogIndex; ++idx) {
          // Check if we have quorum for this index
          auto it = memoryAcks_.find(idx);
          size_t ack_count = 0;
          if (it != memoryAcks_.end()) {
            ack_count = it->second.size();
          }
          // Leader's own log counts as an ack (we have the entry)
          ack_count += 1;  // +1 for leader's own entry

          if (ack_count >= quorum) {
            // Verify the entry is from current term
            auto instance = GetRaftInstance(idx);
            if (instance && instance->term == currentTerm) {
              newSpecCommitIndex = idx;
            }
          } else {
            // Stop at first index without quorum (monotonic advance)
            break;
          }
        }

        if (newSpecCommitIndex > specCommitIndex_) {
          uint64_t oldSpecCommitIndex = specCommitIndex_;
          Log_info("[SPEC-RAFT] Site {}: Advancing specCommitIndex {} -> {}",
                   site_id_, specCommitIndex_, newSpecCommitIndex);
          specCommitIndex_ = newSpecCommitIndex;

          // Persist updated speculative indices
          PersistSpeculativeIndicesToLogStorage();

          // Notify clients with SPECULATIVE status for newly committed entries
          if (lastSpecNotifiedIndex_ < newSpecCommitIndex) {
            uint64_t notifyFrom = std::max(lastSpecNotifiedIndex_, oldSpecCommitIndex);
            NotifyCallbacks(notifyFrom, newSpecCommitIndex, CommitStatus::SPECULATIVE);
            lastSpecNotifiedIndex_ = newSpecCommitIndex;
          }
        }

        // A durable quorum may already have arrived before the memory replies
        // above advanced specCommitIndex_.  Re-evaluate after every phase so no
        // additional durable message is required to make progress.
        MaybeAdvanceSecuredLogIndex();

          // Verify invariants
          VerifySpeculativeInvariants();

          // Publish authority only after commit recalculation. A delayed reply
          // remains evidence for the exact term, generation, and membership
          // snapshot that launched it; never relabel it as the current round.
          for (auto authority_it = authority_rounds.begin();
               authority_it != authority_rounds.end();) {
            auto& authority = authority_it->second;
            const bool context_is_current =
                IsLeader() && currentTerm == authority.term &&
                current_config_ == authority.config;
            if (!context_is_current ||
                (read_quorum_confirmed_term_ == authority.term &&
                 authority_it->first <= read_quorum_confirmed_round_)) {
              authority_it = authority_rounds.erase(authority_it);
              continue;
            }

            const size_t authority_quorum =
                raft::raft_quorum_majority_count(authority.config.size());
            if (raft::raft_quorum_count_reached(
                    authority.voters.size(), authority_quorum)) {
              read_quorum_confirmed_term_ = authority.term;
              read_quorum_confirmed_round_ = authority_it->first;
              Log_debug("[READ-INDEX] site={} confirmed round={} term={} "
                        "with {}/{} voters",
                        site_id_, authority_it->first, authority.term,
                        authority.voters.size(), authority.config.size());
              authority_it = authority_rounds.erase(authority_it);
            } else if (authority.outstanding.empty()) {
              // Every RPC launched in this generation completed without a
              // quorum. No later event can add evidence to it.
              authority_it = authority_rounds.erase(authority_it);
            } else {
              ++authority_it;
            }
          }
        }

        // The AppendEntries messages for this round carried the old commit
        // index.  Latch exactly one prompt follow-up round so followers learn
        // the phase-3 commit without waiting for the periodic heartbeat.
        if (commit_advanced_after_send) {
          RequestReplication();
        }
      }
    }

    // ============================================================================
    // LEADERSHIP TRANSFER: Check if we should transfer to preferred replica
    // ============================================================================
    // ============================================================================
    // LEADERSHIP TRANSFER: Handled by Monitor Thread
    // ============================================================================
    // Leadership transfer is now handled by StartLeadershipTransferMonitoring() thread,
    // not here in HeartbeatLoop. This prevents race conditions and double-triggering.
    // The monitor thread is started in setIsLeader() when becoming a non-preferred leader.
    //
    // REMOVED: The piggybacked check that was here to avoid race with monitor thread.
	}
  looping_.store(false, rusty::sync::atomic::Ordering::Release);
  heartbeat_loop_running_.store(
      false, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - thread join and timer cleanup require manual resource management
RaftServer::~RaftServer() {
  // Make shutdown idempotent for never-started servers and for callers that
  // already completed PrepareForShutdown().  A live server must be prepared
  // on a reactor fiber before its destructor runs.
  stop_.store(true, rusty::sync::atomic::Ordering::Release);
  looping_.store(false, rusty::sync::atomic::Ordering::Release);
  install_snapshot_callback_gate_->Close();
  CloseReplicationWakeGate();
  verify(!heartbeat_loop_running_.load(
      rusty::sync::atomic::Ordering::Acquire));
  verify(!election_loop_running_.load(
      rusty::sync::atomic::Ordering::Acquire));
  verify(transfer_election_jobs_.load(
             rusty::sync::atomic::Ordering::Acquire) == 0);
  verify(install_snapshot_callback_gate_->ActiveCallbacks() == 0);

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

  // Stop leadership transfer monitoring thread if running
  StopLeadershipTransferMonitoring();

  // ReplicatedDB's destructor clears its snapshot callbacks through raft_.
  // Destroy it explicitly while this server, its mutex, and callback owner
  // token are still alive, after every apply/heartbeat/election user is gone.
  replicated_db_.reset();

  // Idempotent for never-prepared servers; normal shutdown already drained
  // these before communicator teardown in PrepareForShutdown().
  {
    std::lock_guard<std::recursive_mutex> registration_barrier(mtx_);
  }
  DrainLogPersistenceSequence();
  DrainAsyncPersistenceThreads();

  Log_info("site par {}, loc {}: prepare {}, accept {}, commit {}",
      partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
}

// @unsafe - Caller holds mtx_; validates the snapshot/log invariant and reads
// the absolute last-log slot without inserting into or otherwise mutating the
// compacted log map.
ballot_t RaftServer::ElectionLastLogTermLocked() const {
  verify(lastLogIndex >= snapidx_);
  if (raft_server_election_last_log_uses_snapshot(lastLogIndex, snapidx_)) {
    return snapterm_;
  }

  auto last_log = raft_logs_.find(lastLogIndex);
  verify(last_log != raft_logs_.end());
  verify(last_log->second != nullptr);
  return last_log->second->term;
}

// @unsafe - external calls marked @external [safe], mutex/pointer ops in @unsafe blocks
bool RaftServer::RequestVote() {
  return RequestVoteImpl(/*timer_guarded=*/false,
                         /*expected_generation=*/0);
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

  // for(int i = 0; i < 1000; i++) Log_info("not calling the wrong method");

  const parid_t par_id = partition_id_;
  const locid_t loc_id = loc_id_;

  slotid_t lst_idx = 0 ;
  ballot_t lst_term = 0 ;
  ballot_t prev_term = 0;
  ballot_t term = 0;
  siteid_t prev_vote_for;
  bool local_vote_persisted = false;
  // @unsafe
  {
  prev_vote_for = INVALID_SITEID;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
      req_voting_ = false;
      return false;
    }
    // RequestVoteImpl is the sole campaign admission point. Leadership
    // transfer and timer fibers can overlap while either one is yielding, so
    // callers must not reserve req_voting_ before entering this critical
    // section.
    if (!raft_server_campaign_can_start(
            is_leader_, election_in_progress_)) {
      return false;
    }
    if (timer_guarded) {
      const uint64_t now = Time::now(true);
      const uint64_t elapsed = now - last_heartbeat_time_;
      if (accepted_sync_append_persistence_ != 0 ||
          !raft_server_timer_campaign_is_current(
              is_leader_, expected_generation,
              election_timer_generation_, elapsed,
              election_timeout_us_)) {
        return false;
      }
    }

    // A campaign owns a fresh, latched timeout. If it loses without hearing
    // from a leader, the next campaign waits for this complete interval rather
    // than immediately reusing the already-expired follower deadline.
    resetTimer("starting election campaign");
    prev_term = currentTerm;
    prev_vote_for = vote_for_;
    auto prev_local_term = currentTerm;
    currentTerm++ ;
    vote_for_ = site_id_;  // Vote for ourselves when starting election
    // A candidate has no elected leader evidence in its new term. In
    // particular, it must not redirect clients to the leader from the term it
    // just left.
    current_leader_id_ = raft_server_leader_hint_after_transition(
        false, false, site_id_, current_leader_id_,
        static_cast<siteid_t>(INVALID_SITEID));

    // CRITICAL: Persist term and vote BEFORE sending RequestVote RPCs
    local_vote_persisted = PersistState(
        currentTerm, vote_for_, "RequestVote: starting election");
    if (HasConfiguredStorage() && !local_vote_persisted) {
      // A candidate's self vote participates in the election quorum.  If its
      // term/vote is only volatile, a restart could vote for a different
      // candidate in this same term and permit two leaders.  Do not send any
      // RequestVote RPC after crossing that failed durability boundary.
      Log_error("[RAFT-PERSISTENCE] Site {} could not durably record its "
                "self vote for term {}; failing stop before broadcast",
                site_id_, currentTerm);
      rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
      stop_.store(true, rusty::sync::atomic::Ordering::Release);
      looping_.store(false, rusty::sync::atomic::Ordering::Release);
      apply_thread_running_.store(false);
      election_in_progress_ = false;
      earlyDurableVoters_.clear();
      req_voting_ = false;
      return false;
    }

    // VoteDurable is a separate RPC in async mode and can overtake the
    // ordinary Vote response. Atomically publish ownership of req_voting_ and
    // the election term before broadcasting so no second caller can campaign
    // concurrently and OnVoteDurable can retain an early notification.
    election_in_progress_ = true;
    election_term_ = currentTerm;
    req_voting_ = true;
    term = currentTerm;
    earlyDurableVoters_.clear();

    LogTermChange("starting election", prev_local_term, currentTerm);
    // PersistState() already called above - no need for duplicate persistence
    lst_idx = lastLogIndex;
    lst_term = ElectionLastLogTermLocked();
  }

#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server {} (loc {}) starting election term {}->{} lastLogIdx={} lastLogTerm={} prev_vote_for={}",
           site_id_, loc_id, prev_term, term, lst_idx, lst_term, prev_vote_for);
#endif
  shared_ptr<RaftVoteQuorumEvent> sp_quorum;
  // @unsafe
  {
  sp_quorum = commo()->BroadcastVote(
      par_id, lst_idx, lst_term, loc_id, term);
  sp_quorum->wait_timeout(1000000);
  }
  std::unique_lock<std::recursive_mutex> lock1(mtx_);
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    election_in_progress_ = false;
    earlyDurableVoters_.clear();
    req_voting_ = false;
    return false;
  }
  // A higher term dominates every election outcome, including TIMEOUT and a
  // concurrently completed YES quorum. FeedResponse publishes this maximum
  // before its wakeup, so snapshot it only after reacquiring Raft state.
  const int64_t observed_response_term = sp_quorum->Term();
  const ElectionCompletionAction completion_action =
      static_cast<ElectionCompletionAction>(
          raft_server_election_completion_action(
              election_in_progress_, election_term_, term, currentTerm,
              observed_response_term));
  if (completion_action ==
      ElectionCompletionAction::ADVANCE_HIGHER_TERM) {
    const uint64_t previous_term = currentTerm;
    currentTerm = static_cast<uint64_t>(observed_response_term);
    vote_for_ = INVALID_SITEID;
    current_leader_id_ = raft_server_leader_hint_after_transition(
        false, false, site_id_, current_leader_id_,
        static_cast<siteid_t>(INVALID_SITEID));

    const bool has_configured_storage = HasConfiguredStorage();
    const bool persistence_succeeded =
        !has_configured_storage ||
        PersistState(currentTerm, vote_for_,
                     "RequestVote: observed higher response term");

    if (is_leader_) {
      stepDown(StepDownReason::HigherTerm);
    } else {
      setIsLeader(false);
    }
    election_in_progress_ = false;
    earlyDurableVoters_.clear();
    req_voting_ = false;

    if (!raft_server_term_advance_is_durable(
            has_configured_storage, persistence_succeeded)) {
      Log_error("[RAFT-PERSISTENCE] Site {} could not durably record "
                "RequestVote response term {}; failing stop",
                site_id_, currentTerm);
      rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
      stop_.store(true, rusty::sync::atomic::Ordering::Release);
      looping_.store(false, rusty::sync::atomic::Ordering::Release);
      apply_thread_running_.store(false);
      return false;
    }

    LogTermChange("observed higher term from RequestVote replies",
                  previous_term, currentTerm);
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
             site_id_, term, currentTerm, election_term_,
             election_in_progress_);
#endif
    return false;
  }
  verify(completion_action == ElectionCompletionAction::APPLY_CURRENT);
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}",
           site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get(), sp_quorum->Term(), sp_quorum->q().timeouted_.get());
#endif
  if (sp_quorum->yes()) {
    verify(currentTerm >= term);

    // =========================================================================
    // SPECULATIVE VOTING: Initialize specVoters from vote responses
    // =========================================================================
    // These are memory votes - not yet durable
    specVoters_ = sp_quorum->GetSpecVoters();
    specVoters_.insert(site_id_);  // Add self vote

    // A durable vote necessarily implies that the same follower granted its
    // in-memory vote, even if the separate durable RPC arrived first.
    specVoters_.insert(earlyDurableVoters_.begin(),
                       earlyDurableVoters_.end());

    // Persistence-off has no durable voters.  Sync Vote replies crossed the
    // persistence boundary before returning, while async followers are counted
    // only after their VoteDurable notification (the local vote was persisted
    // synchronously before BroadcastVote).
    durableVoters_ = raft_server_initial_durable_voters(
        HasDurableStorage(), async_persistence_, local_vote_persisted,
        site_id_, specVoters_, earlyDurableVoters_);
    election_in_progress_ = false;
    earlyDurableVoters_.clear();
    req_voting_ = false;

    // Reset commit indices
    specCommitIndex_ = commitIndex;
    securedLogIndex_ = commitIndex;

    // Persist updated speculative indices
    PersistSpeculativeIndicesToLogStorage();

    // Clear ack tracking maps for new term
    memoryAcks_.clear();
    durableAcks_.clear();

    // Sync persistence can make the leader secured immediately.  Async mode
    // remains unsecured until enough VoteDurable notifications are recorded.
    securedLeader_ = HasDurableStorage() &&
        raft::raft_quorum_count_reached(
            durableVoters_.size(), GetQuorumSize());

    Log_info("[SPEC-RAFT] Site {}: Won election term {} - specVoters={} durableVoters={}",
             site_id_, term, specVoters_.size(), durableVoters_.size());
    // =========================================================================

    if (stop_.load(rusty::sync::atomic::Ordering::Acquire) ||
        currentTerm != term) {
      req_voting_ = false;
      return false;
    }

    // become a leader
    setIsLeader(true) ;
    // verify(currentTerm == term); // [Jetpack] Comment this since in failure recovery test this will fail after experiment end.
    Log_debug("site {} became leader for term {}", site_id_, term);

#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get());
#endif

    if(IsLeader()) {
	  	//for(int i = 0; i < 100; i++) Log_info("wait wait wait");
      Log_debug("vote accepted {} curterm {}", loc_id, currentTerm);
  		req_voting_ = false ;
			return true;
    } else {
      Log_debug("vote rejected {} curterm {}, do rollback", loc_id, currentTerm);
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
    if (election_in_progress_ && election_term_ == term) {
      election_in_progress_ = false;
      earlyDurableVoters_.clear();
    }
  	req_voting_ = false ;
		return false;
  } else {
    Log_debug("vote timeout {}", loc_id);
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get());
#endif
    if (election_in_progress_ && election_term_ == term) {
      election_in_progress_ = false;
      earlyDurableVoters_.clear();
    }
  	req_voting_ = false ;
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
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("raft receives vote from candidate: {:x}", can_id);

  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    *reply_term = currentTerm;
    *vote_granted = false;
    Log_debug("[RAFT-SHUTDOWN] Site {} rejecting RequestVote from {}",
              site_id_, can_id);
    return;
  }

  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool candidate_is_current_voter =
      can_id != invalid && can_id != site_id_ &&
      current_config_.count(can_id) != 0 && learners_.count(can_id) == 0;
  if (can_term < 0 || lst_log_term < 0 ||
      !candidate_is_current_voter) {
    *reply_term = static_cast<ballot_t>(currentTerm);
    *vote_granted = false;
    Log_warn("[RAFT_VOTE] Site {} rejected malformed/non-voter candidate {} "
             "term {} last_log_term {} (voter={})",
             site_id_, can_id, can_term, lst_log_term,
             candidate_is_current_voter);
    return;
  }

  uint64_t cur_term = currentTerm ;
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
  if( can_term == cur_term && vote_for_ != INVALID_SITEID && vote_for_ != can_id )
  {
    Log_debug("site {} vote NO for {} (already voted for {} in term {})",
              site_id_, can_id, vote_for_, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;
    return ;
  }
  }

  // Every grant, including an idempotent retry, must still carry an up-to-date
  // candidate log. This is defensive against damaged/legacy persistent state
  // and is the Raft RequestVote rule in its direct form.
  verify(lastLogIndex >= snapidx_);
  const slotid_t lstoff = lastLogIndex - snapidx_;
  const ballot_t curlstterm = ElectionLastLogTermLocked();
  const slotid_t curlstidx = lastLogIndex;
  const bool candidate_log_is_current =
      raft_server_candidate_log_is_at_least(
          lst_log_term, curlstterm, lst_log_idx, curlstidx);

  // If we already voted for this same candidate in this term, vote YES again
  // only when the retry still satisfies log freshness.
  if (raft_server_vote_is_idempotent(
          static_cast<uint64_t>(can_term), cur_term, vote_for_, can_id) &&
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
  verify(lstoff + snapidx_ == lastLogIndex);

  if (candidate_log_is_current)
  {
    Log_debug("site {} vote for request vote from {}, lastidx {}, lastterm {}", site_id_, can_id, curlstidx, curlstterm);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;

}

// ============================================================================
// VoteDurable RPC Handler - Speculative Voting Protocol
// ============================================================================

void RaftServer::OnVoteDurable(const ballot_t& term,
                                const siteid_t& voter_id,
                                bool_t* acknowledged) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Shutdown closes persistence-ticket admission while holding mtx_.  Reject
  // late durable notifications so they cannot enqueue speculative metadata
  // after PrepareForShutdown has selected the sequence it will drain.
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    *acknowledged = false;
    return;
  }

  // A server running with persistence disabled cannot validate or advertise
  // durable state.  In particular, an unexpected/stale durable RPC must not be
  // able to make an in-memory-only leader emit DURABLE callbacks.
  if (!raft_server_persistence_can_report_durable(HasDurableStorage())) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring VoteDurable from {} - persistence disabled",
              site_id_, voter_id);
    *acknowledged = false;
    return;
  }

  // Reject stale votes from old terms
  if (term != currentTerm) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring VoteDurable from {} - term mismatch (got {}, current {})",
              site_id_, voter_id, term, currentTerm);
    *acknowledged = false;
    return;
  }

  if (current_config_.count(voter_id) == 0 ||
      learners_.count(voter_id) > 0) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring VoteDurable from non-voter {}",
              site_id_, voter_id);
    *acknowledged = false;
    return;
  }

  // In async mode VoteDurable may overtake the ordinary Vote response and
  // arrive while BroadcastVote is still waiting.  Buffer it for this exact
  // election term; it will be merged atomically if the election succeeds.
  if (!is_leader_) {
    if (raft_server_can_buffer_early_durable_vote(
            HasDurableStorage(), async_persistence_, is_leader_,
            election_in_progress_, term, election_term_)) {
      earlyDurableVoters_.insert(voter_id);
      *acknowledged = true;
      Log_debug("[SPEC-RAFT] Site {}: Buffered early VoteDurable from {} for term {}",
                site_id_, voter_id, term);
      return;
    }
    Log_debug("[SPEC-RAFT] Site {}: Ignoring VoteDurable from {} - not leader",
              site_id_, voter_id);
    *acknowledged = false;
    return;
  }

  // Add voter to durable voters set
  durableVoters_.insert(voter_id);
  *acknowledged = true;

  Log_info("[SPEC-RAFT] Site {}: Received VoteDurable from {} - durableVoters size={}",
           site_id_, voter_id, durableVoters_.size());

  // Check if we've achieved secured leader status
  size_t quorum = GetQuorumSize();
  if (!securedLeader_ && durableVoters_.size() >= quorum) {
    securedLeader_ = true;
    Log_info("[SPEC-RAFT] Site {}: Became SECURED leader with {} durable votes (quorum={})",
             site_id_, durableVoters_.size(), quorum);
    MaybeAdvanceSecuredLogIndex();
  }
}

// ============================================================================
// AppendEntriesDurable RPC Handler - Speculative Commit Protocol
// ============================================================================

void RaftServer::OnAppendEntriesDurable(const ballot_t& term,
                                         const siteid_t& follower_id,
                                         const uint64_t& lastLogIndex,
                                         bool_t* acknowledged) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    *acknowledged = false;
    return;
  }

  if (!raft_server_persistence_can_report_durable(HasDurableStorage())) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring AppendEntriesDurable from {} - persistence disabled",
              site_id_, follower_id);
    *acknowledged = false;
    return;
  }

  // Reject stale acks from old terms
  if (term != currentTerm) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring AppendEntriesDurable from {} - term mismatch (got {}, current {})",
              site_id_, follower_id, term, currentTerm);
    *acknowledged = false;
    return;
  }

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring AppendEntriesDurable from {} - not leader",
              site_id_, follower_id);
    *acknowledged = false;
    return;
  }

  // Learners replicate for catch-up but are not members of either quorum.
  // Also reject removed/unknown sites so stale durable notifications cannot
  // re-enter acknowledgement maps after a configuration change.
  if (current_config_.count(follower_id) == 0 ||
      learners_.count(follower_id) > 0) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring AppendEntriesDurable from "
              "non-voter {}",
              site_id_, follower_id);
    *acknowledged = false;
    return;
  }

  // Add follower to durable acks for all relevant indices up to lastLogIndex
  // We track this for all indices since the follower has durably persisted everything up to lastLogIndex
  const uint64_t durable_through = std::min(lastLogIndex, this->lastLogIndex);
  if (durable_through > securedLogIndex_) {
    for (uint64_t idx = securedLogIndex_ + 1; idx <= durable_through; ++idx) {
      durableAcks_[idx].insert(follower_id);
      if (idx == durable_through) {
        break;
      }
    }
  }
  *acknowledged = true;

  Log_info("[SPEC-RAFT] Site {}: Received AppendEntriesDurable from {} for index={}",
           site_id_, follower_id, lastLogIndex);

  MaybeAdvanceSecuredLogIndex();

  // Verify invariants in debug mode
  VerifySpeculativeInvariants();
}

// @unsafe - Mutates acknowledgement maps/indices, persists metadata, and
// invokes client callbacks while the caller holds mtx_.
void RaftServer::MaybeAdvanceSecuredLogIndex() {
  // Caller holds mtx_.  Re-running this method is intentional: durable ACKs,
  // speculative quorum, and durable vote quorum are independent messages and
  // may arrive in any order.
  if (!is_leader_ || !securedLeader_ ||
      !raft_server_persistence_can_report_durable(HasDurableStorage())) {
    return;
  }

  const uint64_t new_secured_index =
      raft_server_highest_contiguous_secured_index(
          securedLogIndex_, specCommitIndex_, lastLogIndex, GetQuorumSize(),
          durableAcks_);
  if (!raft_server_log_index_above(new_secured_index, securedLogIndex_)) {
    return;
  }

  const uint64_t old_secured_index = securedLogIndex_;
  Log_info("[SPEC-RAFT] Site {}: Advancing securedLogIndex {} -> {}",
           site_id_, old_secured_index, new_secured_index);
  securedLogIndex_ = new_secured_index;
  PersistSpeculativeIndicesToLogStorage();

  if (raft_server_log_index_above(new_secured_index,
                                  lastDurableNotifiedIndex_)) {
    const uint64_t notify_from =
        std::max(lastDurableNotifiedIndex_, old_secured_index);
    NotifyCallbacks(notify_from, new_secured_index, CommitStatus::DURABLE);
    lastDurableNotifiedIndex_ = new_secured_index;
  }
}

// @unsafe - Calls undeclared Fiber::create_run()
void RaftServer::StartElectionTimer() {
  election_loop_running_.store(
      true, rusty::sync::atomic::Ordering::Release);
  // @unsafe
  { resetTimer("start election timer"); }

  Fiber::create_run([this]() {
    Log_debug("start timer for election") ;

    while (!stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
      // Sleep for a portion of the timeout before checking.  Unlike a plain
      // Fiber::sleep, this owner-thread event is interrupted by shutdown.
      const uint64_t election_delay = RandomGenerator::rand(
          heartbeat_interval_us_ * 2, heartbeat_interval_us_ * 4);
      if (!WaitForElectionTimeoutOrShutdown(election_delay)) {
        break;
      }

      // Retry NotifyRestart for any PENDING peers
      // This handles the case where a peer was partitioned when we restarted
      auto c = commo();
      if (c != nullptr && c->HasPendingNotifyRestart()) {
        Log_debug("[NOTIFY-RESTART-RETRY] Site {}: retrying for pending peers", site_id_);
        c->RetryPendingNotifyRestart();
      }

      bool timeout_fired = false;
      uint64_t time_elapsed = 0;
      uint64_t election_timeout = 0;
      uint64_t heartbeat_time = 0;
      uint64_t timer_generation = 0;
      uint64_t timer_term = 0;
      siteid_t timer_vote_for = INVALID_SITEID;
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        const uint64_t time_now = Time::now(true);
        heartbeat_time = last_heartbeat_time_;
        time_elapsed = time_now - heartbeat_time;
        election_timeout = election_timeout_us_;
        timer_generation = election_timer_generation_;
        timer_term = currentTerm;
        timer_vote_for = vote_for_;
        timeout_fired = accepted_sync_append_persistence_ == 0 &&
            raft_server_election_timeout_has_fired(
                is_leader_, time_elapsed, election_timeout);
      }

      if (timeout_fired) {
        Log_info("[ELECTION_TIMER] Site {}: TIMEOUT FIRED - starting election (elapsed={} > timeout={})",
                 site_id_, time_elapsed, election_timeout);

        Log_info("[ELECTION_START] Site {}: TRIGGERING REQUESTVOTE - time_elapsed={} > timeout={} last_hb={} current_term={} vote_for={}",
                 site_id_, time_elapsed, election_timeout, heartbeat_time,
                 timer_term, timer_vote_for);
        // CRITICAL: Check stop_ before calling RequestVote() to prevent
        // calling through collapsed vtable after object destruction
        if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) break;
        RequestVoteFromElectionTimer(timer_generation);
        for (;;) {
          bool voting = false;
          {
            std::lock_guard<std::recursive_mutex> lock(mtx_);
            voting = req_voting_;
          }
          if (!voting) {
            break;
          }
          Fiber::sleep(wait_int_);
          if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) break;
        }
      }
    }
    election_loop_running_.store(
        false, rusty::sync::atomic::Ordering::Release);
  });
}

// @unsafe - external calls marked @external [safe], pointer ops in @unsafe blocks
RaftStartResult RaftServer::StartImpl(const janus::Command& cmd,
                                      uint64_t *index,
                                      uint64_t *term,
                                      bool track_resolution,
                                      slotid_t slot_id,
                                      ballot_t ballot) {
  {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // #ifndef RAFT_TEST_CORO
  // if (!heartbeat_setup_) {
  //   heartbeat_setup_ = true;
  //   if (heartbeat_) {
  //     Log_debug("starting heartbeat loop at site {}", site_id_);
  //     Fiber::create_run([this](){
  //       this->HeartbeatLoop(); 
  //     });
  //     // Start election timeout loop
  //     Log_info("!!!!!!! if (failover_)");
  //     if (failover_) {
  //       Fiber::create_run([this](){
  //         StartElectionTimer(); 
  //       });
  //     }
  //   }
  // }
  // #endif
  if (!IsLeader()) {
    // @unsafe
    {
    *index = 0;
    *term = 0;
    }
    return RaftStartResult::REJECTED;
  }
  const RaftStartResult append_result =
      SetLocalAppend(cmd, term, index, slot_id, ballot);
  if (!raft_server_start_was_appended(append_result)) {
    if (track_resolution &&
        raft_server_start_is_indeterminate(append_result)) {
      const std::pair<slotid_t, ballot_t> submission{
          static_cast<slotid_t>(*index),
          static_cast<ballot_t>(*term)};
      const RaftSubmissionProgress unknown{
          /*committed=*/false,
          /*superseded=*/false,
          /*indeterminate=*/true};
      verify(resolved_submissions_.Record(submission, unknown));
      // Let CoordinatorRaft consume the explicit terminal UNKNOWN outcome.
      // The tri-state return also prevents every other public caller from
      // mistaking a possibly durable command for an ordinary rejection.
    }
    if (raft_server_start_was_rejected(append_result)) {
      *index = 0;
      *term = 0;
    }
    return append_result;
  }
  // SetLocalAppend returns the old lastLogIndex value, but Start returns the
  // index of the newly appended instance
  // @unsafe
  {
  verify(lastLogIndex == (*index) + 1);
  *index = lastLogIndex;
  if (track_resolution) {
    active_submissions_.insert(
        {static_cast<slotid_t>(*index), static_cast<ballot_t>(*term)});
  }
  Log_debug("Start(): ldr={} index={} term={}", loc_id_, *index, *term);
  }
  }

  // Publish after releasing mtx_: the wake path never nests the gate's owner
  // mutex below Raft state, and every successful direct Start caller gets the
  // same prompt replication behavior.
  RequestReplication();
  YieldAfterSynchronousLocalAppend();
  return RaftStartResult::APPENDED;
}

RaftStartResult RaftServer::Start(const janus::Command& cmd,
                                  uint64_t *index,
                                  uint64_t *term,
                                  slotid_t slot_id,
                                  ballot_t ballot) {
  return StartImpl(cmd, index, term, /*track_resolution=*/false,
                   slot_id, ballot);
}

RaftStartResult RaftServer::StartTracked(const janus::Command& cmd,
                                         uint64_t *index,
                                         uint64_t *term,
                                         slotid_t slot_id,
                                         ballot_t ballot) {
  return StartImpl(cmd, index, term, /*track_resolution=*/true,
                   slot_id, ballot);
}

// @unsafe - Append and callback registration share one Raft-state critical
// section. Replication is published exactly once, after releasing mtx_.
RaftStartResult RaftServer::StartWithCallback(
    const janus::Command& cmd,
    uint64_t* index,
    uint64_t* term,
    std::function<void(CommitStatus)> callback,
    uint64_t* callback_token,
    slotid_t slot_id,
    ballot_t ballot) {
  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    if (callback_token != nullptr) {
      *callback_token = 0;
    }
    if (!IsLeader()) {
      *index = 0;
      *term = 0;
      return RaftStartResult::REJECTED;
    }

    const RaftStartResult append_result =
        SetLocalAppend(cmd, term, index, slot_id, ballot);
    if (!raft_server_start_was_appended(append_result)) {
      if (raft_server_start_was_rejected(append_result)) {
        *index = 0;
        *term = 0;
      }
      return append_result;
    }
    verify(lastLogIndex == (*index) + 1);
    *index = lastLogIndex;

    const uint64_t token =
        RegisterCommitCallbackLocked(*index, std::move(callback));
    if (callback_token != nullptr) {
      *callback_token = token;
    }
    Log_debug("StartWithCallback(): ldr={} index={} term={} token={}",
              loc_id_, *index, *term, token);
  }

  RequestReplication();
  YieldAfterSynchronousLocalAppend();
  return RaftStartResult::APPENDED;
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
                                 uint64_t *followerLastLogIndex,
                                 uint64_t *followerAckType,
                                 bool trigger_election_now) {
  std::unique_lock<std::recursive_mutex> lock(mtx_);

  // The ordinary response is memory-only unless this exact call successfully
  // writes and syncs new log entries in synchronous persistence mode.
  // @unsafe
  {
    *followerAckType = raft_server_follower_append_ack_type(
        HasDurableStorage(), async_persistence_,
        /*persistence_succeeded=*/false);
  }

  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    *followerAppendOK = 0;
    *followerCurrentTerm = currentTerm;
    *followerLastLogIndex = lastLogIndex;
    return;
  }

  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool leader_has_higher_term =
      raft_server_observed_higher_term(leaderCurrentTerm, currentTerm);
  const bool leader_term_is_stale =
      raft_server_vote_term_is_stale(leaderCurrentTerm, currentTerm);
  const bool sender_is_current_voter =
      leaderSiteId != invalid && leaderSiteId != site_id_ &&
      current_config_.count(leaderSiteId) != 0 &&
      learners_.count(leaderSiteId) == 0;
  const bool sender_is_self = leaderSiteId == site_id_;
  const bool has_known_leader = current_leader_id_ != invalid;
  const bool known_leader_matches_sender =
      current_leader_id_ == leaderSiteId;
  if (!sender_is_current_voter || leader_term_is_stale ||
      !raft_server_leader_rpc_sender_is_authoritative(
          leader_has_higher_term, is_leader_, sender_is_self,
          has_known_leader, known_leader_matches_sender)) {
    Log_warn("[APPEND_REJECT] Site {} rejecting unauthoritative "
             "AppendEntries sender {} term {} (local_term={} leader={} "
             "known_leader={} voter={})",
             site_id_, leaderSiteId, leaderCurrentTerm, currentTerm,
             is_leader_, current_leader_id_, sender_is_current_voter);
    *followerAppendOK = 0;
    *followerCurrentTerm = currentTerm;
    *followerLastLogIndex = lastLogIndex;
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
      leaderCurrentTerm, this->currentTerm);
  const bool compacted_prefix_miss =
      (leaderPrevLogIndex != 0 &&
       leaderPrevLogIndex < min_active_slot_ &&
       leaderPrevLogIndex != snapidx_);
  bool index_ok = (leaderPrevLogIndex <= this->lastLogIndex) && !compacted_prefix_miss;
  uint64_t local_prev_term = 0;
  if (leaderPrevLogIndex == 0) {
      local_prev_term = 0;
  } else if (leaderPrevLogIndex == snapidx_) {
      // Snapshot boundary is still valid even when log entries are compacted.
      local_prev_term = snapterm_;
  } else if (leaderPrevLogIndex <= this->lastLogIndex && !compacted_prefix_miss) {
      auto prev_instance = GetRaftInstance(leaderPrevLogIndex);
      local_prev_term = prev_instance ? prev_instance->term : 0;
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
              leaderCurrentTerm, this->currentTerm)) {
          auto prev_term = currentTerm;
          currentTerm = leaderCurrentTerm;
          vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          // Publish the accepted leader before a possible leader-change
          // callback observes the follower transition.
          current_leader_id_ = raft_server_leader_hint_after_transition(
              false, true, site_id_, leaderSiteId, invalid);

          // CRITICAL: Persist term before accepting any entries from new leader
          if (HasConfiguredStorage() &&
              !PersistState(currentTerm, vote_for_,
                            "OnAppendEntries: new leader term")) {
            Log_error("[APPEND_REJECT] Site {} could not durably record "
                      "higher term {}; failing stop after follower transition",
                      site_id_, currentTerm);
            if (is_leader_) {
              stepDown(StepDownReason::HigherTerm);
            } else {
              setIsLeader(false);
            }
            req_voting_ = false;
            election_in_progress_ = false;
            earlyDurableVoters_.clear();
            rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
            stop_.store(true, rusty::sync::atomic::Ordering::Release);
            looping_.store(false, rusty::sync::atomic::Ordering::Release);
            apply_thread_running_.store(false);
            *followerAppendOK = 0;
            *followerCurrentTerm = currentTerm;
            *followerLastLogIndex = lastLogIndex;
            return;
          }

          LogTermChange("AppendEntries leader term is newer", prev_term, currentTerm, leaderSiteId);
          Log_debug("server {}, set to be follower", loc_id_ ) ;
          if (is_leader_) {
            // Use the central transition so pending callbacks and speculative
            // state cannot survive an accepted competing leader epoch.
            stepDown(StepDownReason::HigherTerm);
          } else {
            setIsLeader(false);
          }
          req_voting_ = false;
          election_in_progress_ = false;
          earlyDurableVoters_.clear();
          // PersistState() already called above - no need for duplicate persistence
      }
      // Refresh the validated leader hint for current-term contact too. A
      // higher-term sender was already published before its role transition.
      current_leader_id_ = raft_server_leader_hint_after_transition(
          false, true, site_id_, leaderSiteId, invalid);
      // @unsafe
      { resetTimer("AppendEntries from current-term leader"); }
  }

  if (raft_server_append_is_acceptable(term_ok, index_ok, prev_term_ok) &&
      append_payload_valid) {
      Log_debug("refresh timer on appendentry");

      // Any accepted leader RPC establishes follower state even when it is in
      // our current term. Cancel an outstanding election before its delayed
      // result can promote this server after the accepted AppendEntries.
      if (is_leader_) {
        stepDown(StepDownReason::HigherTerm);
      } else {
        setIsLeader(false);
      }
      req_voting_ = false;
      election_in_progress_ = false;
      earlyDurableVoters_.clear();

      // // Update follower's view to track the current leader
      // if (!IsLeader() && leaderSiteId != INVALID_SITEID) {
      //     int n_replicas = Config::GetConfig()->GetPartitionSize(partition_id_);
      //     Log_info("[RAFT_VIEW_FOLLOWER] Server {} observed leader change {}->{} term={} prev_term={}",
      //              site_id_, prev_leader, leaderSiteId, leaderCurrentTerm, currentTerm);
      // }

      // ==================================================================
      // SPECULATIVE REPLICATION: Append to memory, respond immediately,
      // then persist asynchronously and send AppendEntriesDurable
      // ==================================================================

      // Decode the complete wire payload before mutating the local log.  This
      // also gives the persistence path an immutable copy whose lifetime is
      // independent of later map erasure.
      std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>
          incoming_entries;
      const uint64_t old_last_log_index = lastLogIndex;
      const uint64_t accepted_through = cmd.has_value()
          ? raft_server_append_sent_end(
                leaderPrevLogIndex, encoded_entry_count)
          : raft_server_append_sent_end(leaderPrevLogIndex, 0);
      const uint64_t log_index_for_durable_ack = cmd.has_value()
          ? accepted_through
          : 0;

      if (cmd.has_value()) {
#ifndef RAFT_BATCH_OPTIMIZATION
        auto incoming = std::make_shared<RaftData>();
        incoming->log_ = cmd;
        incoming->term = leaderNextLogTerm;
        incoming_entries.push_back({accepted_through, incoming});
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
            auto incoming = std::make_shared<RaftData>();
            incoming->log_ = c.clone();
            incoming->term = c->term;
            incoming_entries.push_back({index, std::move(incoming)});
          }
        } else {
          // Batch optimization is a wire optimization, not a restriction on
          // the Raft log's command type. ReplicatedDB and future application
          // commands travel as one raw entry with their explicit wire term.
          auto incoming = std::make_shared<RaftData>();
          incoming->log_ = cmd;
          incoming->term = leaderNextLogTerm;
          incoming_entries.push_back({accepted_through,
                                      std::move(incoming)});
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
      uint64_t truncate_suffix_first = 0;
      uint64_t truncate_suffix_last = 0;
      for (const auto& [index, incoming] : incoming_entries) {
        const auto local = raft_logs_.find(index);
        const bool local_exists =
            local != raft_logs_.end() && local->second != nullptr &&
            local->second->log_.has_value();
        const uint64_t local_term = local_exists ? local->second->term : 0;
        if (raft_server_append_entry_conflicts(
                local_exists, local_term, incoming->term)) {
          have_first_write = true;
          first_write_index = index;
          truncate_suffix = index <= old_last_log_index;
          break;
        }
      }

      if (truncate_suffix &&
          first_write_index <= std::max(commitIndex, executeIndex)) {
        // A legitimate leader can never conflict with a committed entry.  Do
        // not let malformed or internally inconsistent input rewrite applied
        // state; reject it before either memory or storage changes.
        Log_error("[APPEND_REJECT] Site {} refusing conflict at committed "
                  "index {} (commitIndex={}, executeIndex={}, oldLast={})",
                  site_id_, first_write_index, commitIndex, executeIndex,
                  old_last_log_index);
        *followerAppendOK = 0;
        *followerCurrentTerm = this->currentTerm;
        *followerLastLogIndex = this->lastLogIndex;
        return;
      }

      if (have_first_write) {
        if (truncate_suffix) {
          truncate_suffix_first = first_write_index;
          truncate_suffix_last = old_last_log_index;
          raft_logs_.erase(raft_logs_.lower_bound(first_write_index),
                           raft_logs_.end());
        }
        for (const auto& [index, incoming] : incoming_entries) {
          if (index >= first_write_index) {
            raft_logs_[index] = incoming;
          }
        }
      }
      lastLogIndex = raft_server_append_result_last_index(
          old_last_log_index, accepted_through, truncate_suffix);
      if (truncate_suffix) {
        specCommitIndex_ = raft_server_snapshot_progress_clamp(
            specCommitIndex_, commitIndex, lastLogIndex);
        securedLogIndex_ = raft_server_commit_index_clamp(
            securedLogIndex_, specCommitIndex_);
        memoryAcks_.erase(memoryAcks_.lower_bound(first_write_index),
                          memoryAcks_.end());
        durableAcks_.erase(durableAcks_.lower_bound(first_write_index),
                           durableAcks_.end());
      }

      // Rewrite only the first changed slot and its wire suffix. A fully
      // matching retry performs a term verification plus sync below, rather
      // than rewriting or truncating any entry.
      std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>
          entries_to_persist;
      if (have_first_write) {
        for (const auto& entry : incoming_entries) {
          if (entry.first >= first_write_index) {
            entries_to_persist.push_back(entry);
          }
        }
      }

      // Advance commit index and enqueue committed entries for background apply.
      const uint64_t follower_commit_candidate =
          raft_server_commit_index_clamp(
              leaderCommitIndex, accepted_through);
      bool follower_commit_advanced = false;
      if (raft_server_log_index_above(
              follower_commit_candidate, commitIndex)) {
        auto old_commit = commitIndex;
        commitIndex = follower_commit_candidate;
        specCommitIndex_ = std::max(specCommitIndex_, commitIndex);
        verify(lastLogIndex >= commitIndex);
        EnqueueCommittedEntries(old_commit, commitIndex);
        follower_commit_advanced = true;
      }

      // @unsafe
      {
      *followerAppendOK = 1;
      *followerCurrentTerm = this->currentTerm;
      // On success this field is the inclusive end proved by this call, not
      // the follower's possibly longer and divergent local suffix. Rejections
      // below retain local lastLogIndex as a backoff hint.
      *followerLastLogIndex = accepted_through;
      }

      // Capture state needed for async persistence thread
      ballot_t term_copy = currentTerm;
      siteid_t follower_id_copy = site_id_;
      siteid_t leader_id_copy = leaderSiteId;
      parid_t par_id_copy = partition_id_;
      uint64_t commit_index_copy = commitIndex;
      uint64_t spec_commit_index_copy = specCommitIndex_;
      uint64_t secured_log_index_copy = securedLogIndex_;
      const bool has_append_payload = cmd.has_value();
      const ballot_t accepted_boundary_term = has_append_payload
          ? incoming_entries.back().second->term
          : leaderPrevLogTerm;
      std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>
          matching_entries_to_verify;
      if (has_append_payload && !have_first_write) {
        matching_entries_to_verify = incoming_entries;
      }
      const bool has_ordered_persistence_work =
          HasConfiguredStorage() &&
          (has_append_payload || follower_commit_advanced);
      const bool queue_async_persistence =
          raft_server_async_persistence_should_queue(
              async_persistence_, HasConfiguredStorage(),
              has_ordered_persistence_work);

      // ==================================================================
      // PERSISTENCE: Either async (speculative) or sync (traditional)
      // ==================================================================
      if (queue_async_persistence) {
        // Allocate and register a non-joinable placeholder before reserving the
        // ticket. Thread/vector allocation failure can then never strand a FIFO
        // slot or destroy a live untracked std::thread.
        std::lock_guard<std::mutex> lk(async_threads_mtx_);
        async_threads_.erase(
          std::remove_if(async_threads_.begin(), async_threads_.end(),
            [](auto& entry) {
              if (entry.second->get()) {
                if (entry.first.joinable()) entry.first.join();
                return true;
              }
              return false;
            }),
          async_threads_.end());
        auto done = rusty::Arc<AtomicFlag>::make(false);
        async_threads_.emplace_back(std::thread{}, done);
        const uint64_t log_persistence_ticket =
            ReserveLogPersistenceTicketLocked();
        try {
          async_threads_.back().first = std::thread(
            [this, entries = std::move(entries_to_persist),
             matching = std::move(matching_entries_to_verify),
             has_append_payload, log_index_for_durable_ack,
             accepted_boundary_term, term_copy, follower_id_copy,
             leader_id_copy, par_id_copy, commit_index_copy,
             spec_commit_index_copy, secured_log_index_copy,
             truncate_suffix, truncate_suffix_first,
             truncate_suffix_last, log_persistence_ticket, done]() {
              try {
                const bool persistence_succeeded =
                    ExecuteLogPersistenceTicket(
                        log_persistence_ticket,
                        "ordered async follower append persistence",
                        [this, &entries, &matching, has_append_payload,
                         commit_index_copy, spec_commit_index_copy,
                         secured_log_index_copy, truncate_suffix,
                         truncate_suffix_first, truncate_suffix_last]() {
                          bool append_persisted = true;
                          if (has_append_payload) {
                            append_persisted =
                                PersistFollowerAppendToLogStorage(
                                    entries, matching, commit_index_copy,
                                    truncate_suffix, truncate_suffix_first,
                                    truncate_suffix_last);
                          }
                          const bool metadata_persisted =
                              PersistCommitIndexToLogStorage(
                                  commit_index_copy,
                                  spec_commit_index_copy,
                                  secured_log_index_copy);
                          return append_persisted && metadata_persisted;
                        });

                // Revalidate after ticket completion. Later accepted work may
                // have changed the term, leader, or exact boundary while the
                // storage operation waited for its turn.
                if (has_append_payload && persistence_succeeded) {
                  std::lock_guard<std::recursive_mutex> state_lock(mtx_);
                  if (PersistedAppendContextIsCurrentLocked(
                          term_copy, leader_id_copy,
                          log_index_for_durable_ack,
                          accepted_boundary_term) &&
                      HasDurableStorage()) {
                    auto c = commo();
                    if (c != nullptr) {
                      c->SendAppendEntriesDurable(
                          leader_id_copy, par_id_copy, term_copy,
                          follower_id_copy, log_index_for_durable_ack);
                    }
                  }
                }
              } catch (const std::exception& error) {
                Log_error("[RAFT-PERSISTENCE] Site {} async append worker "
                          "threw after launch: {}", site_id_, error.what());
              } catch (...) {
                Log_error("[RAFT-PERSISTENCE] Site {} async append worker "
                          "threw after launch", site_id_);
              }
              done->set(true);
            });
        } catch (const std::exception& error) {
          ExecuteLogPersistenceTicket(
              log_persistence_ticket,
              "async append thread launch failure",
              [this]() {
                return RecordPersistenceResult(
                    false, "async append thread launch failure");
              });
          async_threads_.pop_back();
          Log_error("[RAFT-PERSISTENCE] Site {} could not launch async "
                    "append worker: {}", site_id_, error.what());
        } catch (...) {
          ExecuteLogPersistenceTicket(
              log_persistence_ticket,
              "async append thread launch failure",
              [this]() {
                return RecordPersistenceResult(
                    false, "async append thread launch failure");
              });
          async_threads_.pop_back();
          Log_error("[RAFT-PERSISTENCE] Site {} could not launch async "
                    "append worker", site_id_);
        }
        lock.unlock();
      } else {
        // Release Raft state while ordered synchronous storage work waits on
        // an earlier accepted async/sync operation.
        bool persistence_succeeded = false;
        if (has_ordered_persistence_work) {
          const uint64_t log_persistence_ticket =
              ReserveLogPersistenceTicketLocked();
          verify(accepted_sync_append_persistence_ != UINT64_MAX);
          ++accepted_sync_append_persistence_;
          lock.unlock();
          persistence_succeeded = ExecuteLogPersistenceTicket(
              log_persistence_ticket,
              "ordered synchronous follower append persistence",
              [this, &entries_to_persist, &matching_entries_to_verify,
               has_append_payload, commit_index_copy,
               spec_commit_index_copy, secured_log_index_copy,
               truncate_suffix, truncate_suffix_first,
               truncate_suffix_last]() {
                bool append_persisted = true;
                if (has_append_payload) {
                  append_persisted = PersistFollowerAppendToLogStorage(
                      entries_to_persist, matching_entries_to_verify,
                      commit_index_copy, truncate_suffix,
                      truncate_suffix_first, truncate_suffix_last);
                }
                const bool metadata_persisted =
                    PersistCommitIndexToLogStorage(
                        commit_index_copy, spec_commit_index_copy,
                        secured_log_index_copy);
                return append_persisted && metadata_persisted;
              });
          lock.lock();
          verify(accepted_sync_append_persistence_ > 0);
          --accepted_sync_append_persistence_;

          const bool persistence_epoch_is_current =
              raft_server_persisted_reply_context_is_current(
                  stop_.load(rusty::sync::atomic::Ordering::Acquire),
                  is_leader_, currentTerm, term_copy,
                  current_leader_id_, leader_id_copy);
          const bool persistence_boundary_is_current =
              PersistedAppendContextIsCurrentLocked(
                  term_copy, leader_id_copy, accepted_through,
                  accepted_boundary_term);
          if (persistence_succeeded && persistence_epoch_is_current) {
            // The first reset records receipt. A slow synchronous storage
            // boundary can legitimately exceed the randomized election
            // timeout, so restart the full timeout after the accepted handler
            // finishes as a single-threaded Raft event loop would.
            resetTimer("completed synchronous AppendEntries persistence");
          }

          // In synchronous persistence mode an ordinary success is itself a
          // replication proof: the leader advances match_index and counts even
          // a Memory-classified ACK toward speculative commit.  Therefore a
          // configured-storage failure must reject the whole RPC, including a
          // heartbeat whose only ordered mutation was commit metadata.
          if (!persistence_succeeded || !persistence_boundary_is_current) {
            *followerAppendOK = 0;
            *followerCurrentTerm = currentTerm;
            *followerLastLogIndex = lastLogIndex;
            *followerAckType = raft_server_follower_append_ack_type(
                HasDurableStorage(), async_persistence_, false);
          } else if (has_append_payload) {
            *followerAckType = raft_server_follower_append_ack_type(
                HasDurableStorage(), async_persistence_,
                persistence_succeeded);
          }
        }
      }

      // Async persistence releases the mutex for the memory reply. Sync and
      // persistence-off paths still own it here.
      if (!lock.owns_lock()) {
        lock.lock();
      }

    }
    else {
        Log_info("[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} payload_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})",
                 site_id_, leaderSiteId, term_ok, index_ok, prev_term_ok,
                 append_payload_valid, leaderCurrentTerm, currentTerm,
                 leaderPrevLogIndex, lastLogIndex, local_prev_term);
        // @unsafe
        {
        *followerAppendOK = 0;
        *followerCurrentTerm = this->currentTerm;
        *followerLastLogIndex = this->lastLogIndex;
        }
    }

/*if (rand() % 1000 == 0) {
	usleep(25*1000);
}*/

    // ============================================================================
    // PIGGYBACKED LEADERSHIP TRANSFER: Handle trigger_election_now flag
    // ============================================================================
    // The leader sends trigger_election_now=true to ALL replicas during transfer.
    // How we handle it depends on whether we're the preferred replica or not.
    if (trigger_election_now) {
        if (AmIPreferredLeader()) {
            // I'm the PREFERRED replica - start election (if not already leader)
            if (!IsLeader() &&
                !stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
                Log_info("[PIGGYBACKED-TRANSFER] Site {} (preferred): Received transfer signal from leader {} - will start election after 30ms",
                         site_id_, leaderSiteId);

                // Wait before starting election to allow old leader's heartbeats
                // to reach other replicas. This prevents election storms.
                transfer_election_jobs_.fetch_add(
                    1, rusty::sync::atomic::Ordering::AcqRel);
                Fiber::create_run([this]() {
                    Fiber::sleep(30000);
                    // CRITICAL: Check stop_ before calling RequestVote() to prevent
                    // calling through collapsed vtable after object destruction
                    if (!stop_.load(
                            rusty::sync::atomic::Ordering::Acquire)) {
                      RequestVote();
                    }
                    transfer_election_jobs_.fetch_sub(
                        1, rusty::sync::atomic::Ordering::AcqRel);
                });
            } else {
                Log_info("[PIGGYBACKED-TRANSFER] Site {} (preferred): Received transfer signal but already leader - ignoring",
                         site_id_);
            }
        } else {
            // I'm a NON-PREFERRED replica - just log and do nothing
            Log_info("[PIGGYBACKED-TRANSFER] Site {} (non-preferred): Received transfer signal (preferred={})",
                     site_id_, preferred_leader_site_id_);
        }
    }

    lock.unlock();
}

// @unsafe - Removes command from log (external calls wrapped in @unsafe blocks)
void RaftServer::removeCmd(slotid_t slot) {
  auto it = raft_logs_.find(slot);
  if (it == raft_logs_.end()) {
    return;
  }

  // Committed log replay and callback execution may overlap with log cleanup.
  // Only evict the log entry here; transaction destruction is handled elsewhere.
  raft_logs_.erase(it);
}

// @unsafe - Stores callback for later invocation
void RaftServer::RegisterLeaderChangeCallback(std::function<void(bool)> cb) {
  leader_change_cb_ = std::move(cb);
}

// @unsafe - external calls marked @external [safe], output pointer writes in @unsafe blocks
void RaftServer::OnTimeoutNow(const uint64_t leaderTerm,
                               const siteid_t leaderSiteId,
                               uint64_t *followerTerm,
                               bool_t *success) {
  std::unique_lock<std::recursive_mutex> lock(mtx_);

  // @unsafe
  {
  *followerTerm = currentTerm;
  *success = false;
  }

  // ============================================================================
  // Edge Case 0: Server shutting down
  // ============================================================================
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow - server shutting down", site_id_);
    return;
  }

  // ============================================================================
  // Edge Case 1: Stale TimeoutNow from old term
  // ============================================================================
  if (leaderTerm < currentTerm) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring stale TimeoutNow from leader {} (leader_term={} < my_term={})",
             site_id_, leaderSiteId, leaderTerm, currentTerm);
    return;
  }

  // TimeoutNow identifies a specific current-configuration leader. Reject a
  // malformed/non-member sender before it can advance our durable term or
  // poison a later WRONG_LEADER hint.
  if (leaderSiteId == static_cast<siteid_t>(INVALID_SITEID) ||
      current_config_.count(leaderSiteId) == 0) {
    Log_warn("[TIMEOUT-NOW] Site {}: Rejecting TimeoutNow from non-member {} "
             "in term {}",
             site_id_, leaderSiteId, leaderTerm);
    return;
  }

  const bool leader_has_higher_term =
      raft_server_observed_higher_term(leaderTerm, currentTerm);
  const siteid_t invalid = static_cast<siteid_t>(INVALID_SITEID);
  const bool sender_is_self = leaderSiteId == site_id_;
  const bool has_known_leader = current_leader_id_ != invalid;
  const bool known_leader_matches_sender =
      current_leader_id_ == leaderSiteId;

  if (!raft_server_leader_rpc_sender_is_authoritative(
          leader_has_higher_term, is_leader_, sender_is_self,
          has_known_leader, known_leader_matches_sender)) {
    Log_warn("[TIMEOUT-NOW] Site {}: Rejecting sender {} in term {} "
             "inconsistent with role {} and known leader {}",
             site_id_, leaderSiteId, leaderTerm,
             is_leader_ ? "leader" : "follower/candidate",
             current_leader_id_);
    return;
  }

  // A current-term follower may learn or refresh its leader here. A leader
  // only accepts a different sender as authoritative when the RPC carries a
  // higher term; never leave is_leader_ paired with a remote leader hint.
  if (!leader_has_higher_term && !is_leader_) {
    current_leader_id_ = raft_server_leader_hint_after_transition(
        false, true, site_id_, leaderSiteId,
        static_cast<siteid_t>(INVALID_SITEID));
  }

  // ============================================================================
  // Edge Case 1b: Leader is ahead of us - update term
  // ============================================================================
  if (leader_has_higher_term) {
    Log_info("[TIMEOUT-NOW] Site {}: Leader {} has higher term ({} > {}) - updating term and stepping down",
             site_id_, leaderSiteId, leaderTerm, currentTerm);

    currentTerm = leaderTerm;
    // @unsafe
    {
    vote_for_ = INVALID_SITEID;  // Reset vote for new term
    }

    // CRITICAL: Persist term before responding to TimeoutNow or starting an
    // election. A configured but unhealthy store cannot safely acknowledge a
    // volatile term advance; fail-stop exactly as InstallSnapshot does.
    const bool has_configured_storage = HasConfiguredStorage();
    const bool persistence_succeeded =
        !has_configured_storage ||
        PersistState(currentTerm, vote_for_,
                     "OnTimeoutNow: leader higher term");
    current_leader_id_ = raft_server_leader_hint_after_transition(
        false, true, site_id_, leaderSiteId,
        static_cast<siteid_t>(INVALID_SITEID));
    if (is_leader_) {
      stepDown(StepDownReason::HigherTerm);
    } else {
      setIsLeader(false);
    }
    req_voting_ = false;
    election_in_progress_ = false;
    earlyDurableVoters_.clear();

    if (!raft_server_term_advance_is_durable(
            has_configured_storage, persistence_succeeded)) {
      Log_error("[TIMEOUT-NOW] Site {} could not durably record higher term "
                "{}; failing stop after follower transition",
                site_id_, currentTerm);
      rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
      stop_.store(true, rusty::sync::atomic::Ordering::Release);
      looping_.store(false, rusty::sync::atomic::Ordering::Release);
      apply_thread_running_.store(false);
      *followerTerm = currentTerm;
      return;
    }

    // @unsafe
    { *followerTerm = currentTerm; }
  }

  // ============================================================================
  // Edge Case 2: Already leader
  // ============================================================================
  if (is_leader_) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow from leader {} - already leader in term {}",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already leader (goal achieved)
    return;
  }

  // ============================================================================
  // Edge Case 3: Currently candidate (already in election)
  // ============================================================================
  if (req_voting_) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow from leader {} - already requesting votes (term={})",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already trying to become leader
    return;
  }

  // ============================================================================
  // Edge Case 4: We're transferring leadership (stepping down)
  // ============================================================================
  if (transferring_leadership_) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow from leader {} - currently transferring leadership",
             site_id_, leaderSiteId);
    return;
  }

  // ============================================================================
  // Valid TimeoutNow - Start Election Immediately
  // ============================================================================
  Log_info("[TIMEOUT-NOW] *** Site {}: Received TimeoutNow from leader {} (term={}) - STARTING ELECTION IMMEDIATELY ***",
           site_id_, leaderSiteId, leaderTerm);

  // Start election immediately (bypass random timeout)
  // This will increment term and send RequestVote RPCs
  lock.unlock();
  bool election_started = RequestVote();
  lock.lock();

  *followerTerm = currentTerm;

  if (election_started) {
    *success = true;
    Log_info("[TIMEOUT-NOW] Site {}: Election started successfully (new_term={})",
             site_id_, currentTerm);
  } else {
    *success = false;
    Log_warn("[TIMEOUT-NOW] Site {}: Failed to start election",
             site_id_);
  }
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
  std::lock_guard<std::recursive_mutex> lock(mtx_);

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
  if (term < currentTerm) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Rejecting InstallSnapshot from leader {} "
             "(leader_term={} < my_term={})",
             site_id_, leader_id, term, currentTerm);
    *term_out = currentTerm;
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
      current_config_.count(leader_site) != 0 &&
      learners_.count(leader_site) == 0;
  const bool leader_has_higher_term =
      raft_server_observed_higher_term(term, currentTerm);
  const bool sender_is_self = leader_site == site_id_;
  const bool has_known_leader = current_leader_id_ != invalid;
  const bool known_leader_matches_sender =
      current_leader_id_ == leader_site;
  if (!sender_is_current_voter ||
      !raft_server_leader_rpc_sender_is_authoritative(
          leader_has_higher_term, is_leader_, sender_is_self,
          has_known_leader, known_leader_matches_sender)) {
    Log_warn("[INSTALL-SNAPSHOT] Site {} rejected unauthoritative leader {} "
             "in term {} (local_term={} leader={} known_leader={} voter={})",
             site_id_, leader_id, term, currentTerm, is_leader_,
             current_leader_id_, sender_is_current_voter);
    return;
  }

  // ============================================================================
  // Edge Case 2: Higher or equal term - accept as legitimate leader
  // ============================================================================
  const uint64_t previous_term = currentTerm;
  bool higher_term_is_durable = true;
  if (leader_has_higher_term) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Leader {} has higher term ({} > {}) - updating",
             site_id_, leader_id, term, currentTerm);
    currentTerm = term;
    // @unsafe
    {
    vote_for_ = INVALID_SITEID;
    }
    higher_term_is_durable =
        !HasConfiguredStorage() ||
        PersistState(currentTerm, vote_for_,
                     "OnInstallSnapshot: leader higher term");
  }

  // InstallSnapshot comes from a known leader. Publish its identity before a
  // possible leader-to-follower callback observes the role transition. The
  // transition is required even if the new term cannot be persisted: once the
  // volatile term advanced, fail-stop must not leave leadership, view, or
  // speculative callback state published from the prior epoch.
  current_leader_id_ = raft_server_leader_hint_after_transition(
      false, true, site_id_, leader_site, invalid);

  // Any accepted leader RPC, including one in our current term, establishes
  // follower state. Cancel the outstanding election as well as leadership;
  // RequestVote's delayed-success path revalidates this ownership before it
  // can promote the server again.
  if (is_leader_) {
    stepDown(StepDownReason::HigherTerm);
  } else {
    setIsLeader(false);
  }
  req_voting_ = false;
  election_in_progress_ = false;
  earlyDurableVoters_.clear();

  if (!higher_term_is_durable) {
    Log_error("[INSTALL-SNAPSHOT] Site {} could not durably record higher "
              "term {}; failing stop after follower transition",
              site_id_, currentTerm);
    rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
    stop_.store(true, rusty::sync::atomic::Ordering::Release);
    looping_.store(false, rusty::sync::atomic::Ordering::Release);
    apply_thread_running_.store(false);
    *term_out = 0;
    return;
  }
  if (leader_has_higher_term) {
    LogTermChange("InstallSnapshot carried newer term", previous_term,
                  currentTerm, leader_site);
  }

  // Reset election timer (legitimate leader contact)
  resetTimer("received InstallSnapshot");
  // From here, currentTerm denotes an accepted current-term leader contact.
  // Individual install failures overwrite this with zero so the caller never
  // advances match/next on an unavailable boundary.
  *term_out = currentTerm;

  // A current-term leader may retry a snapshot after this follower has already
  // committed, applied, or snapshotted through its boundary. Acknowledge that
  // leader contact but do not roll any local snapshot/log/application state
  // backward and do not persist the stale payload.
  uint64_t local_progress_index = commitIndex;
  local_progress_index = std::max(local_progress_index, executeIndex);
  local_progress_index = std::max(local_progress_index, GetAppliedIndex());
  local_progress_index = std::max(local_progress_index, snapidx_);
  if (last_included_index == snapidx_ && snapidx_ != 0 &&
      last_included_term != snapterm_) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: rejecting snapshot boundary "
              "({}, {}) that conflicts with local snapshot ({}, {})",
              site_id_, last_included_index, last_included_term,
              snapidx_, snapterm_);
    *term_out = 0;
    return;
  }
  if (raft_server_snapshot_is_stale(
          last_included_index, local_progress_index)) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Snapshot index {} is already covered "
             "(commit={} execute={} applied={} snapidx={}); acknowledging no-op",
             site_id_, last_included_index, commitIndex, executeIndex,
             GetAppliedIndex(), snapidx_);
    return;
  }
  if (!raft_server_log_index_has_successor(last_included_index)) {
    // min_active_slot_ and LogStorage's exclusive removal bound both require
    // S + 1. Reaching UINT64_MAX exhausts the Raft log index space, so reject
    // the payload without wrapping either value.
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

  // Serialize every persistent-log observation and mutation after all earlier
  // accepted AppendEntries work. Holding mtx_ prevents any later append from
  // reserving its ticket until this complete snapshot epoch publishes.
  const bool snapshot_persistence_ordered = HasConfiguredStorage();
  uint64_t snapshot_persistence_ticket = 0;
  if (snapshot_persistence_ordered) {
    snapshot_persistence_ticket = ReserveLogPersistenceTicketLocked();
    WaitForLogPersistenceTicket(snapshot_persistence_ticket);
  }
  LogPersistenceTicketCompletion snapshot_ticket_completion(
      this, snapshot_persistence_ticket, snapshot_persistence_ordered);
  if (snapshot_persistence_ordered && !HasDurableStorage()) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Persistent log is unavailable "
              "or unhealthy",
              site_id_);
    *term_out = 0;
    return;
  }

  // Complete every fallible observation used by the retention decision before
  // the application loader can replace external state. Use find(), not
  // GetRaftInstance(), and require a decoded command so a synthesized empty
  // RaftData can never prove the snapshot boundary.
  const auto boundary = raft_logs_.find(last_included_index);
  const bool has_boundary =
      boundary != raft_logs_.end() && boundary->second != nullptr &&
      boundary->second->log_.has_value();
  const ballot_t local_boundary_term =
      has_boundary ? boundary->second->term : 0;
  const bool retain_suffix = raft_server_snapshot_boundary_matches(
      has_boundary, local_boundary_term, last_included_term);
  const slotid_t previous_last_log_index = lastLogIndex;

  // Capture the persistent range before mutating either state machine. LogStorage
  // remove_range uses [start, end) bounds and backend metadata parsing may throw;
  // the enclosing handler guard converts that into a failed, stopped install.
  slotid_t persistent_first_index = 0;
  slotid_t persistent_last_index = 0;
  if (log_storage_) {
    persistent_first_index = log_storage_->get_first_index();
    persistent_last_index = log_storage_->get_last_index();
  }

  // Fully validate and durably stage the exact state-machine image before
  // changing either recovery point. The owned transaction's destructor aborts
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

  // SnapshotManager is now the durable authority for this boundary. Publish
  // the staged application directory only afterward, closing the crash window
  // where the application could advance without recoverable Raft bytes.
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
           "after durable Raft snapshot publication",
           site_id_, last_included_index);

  // ============================================================================
  // Update snapshot metadata
  // ============================================================================
  // Preserve a terminal result while every covered local identity still
  // exists. A matching snapshot boundary proves the whole retained prefix;
  // a divergent boundary proves only its own entry. Any older outcome that
  // the local committed prefix did not already settle becomes explicitly
  // indeterminate and surface COMMIT_OUTCOME_UNKNOWN rather than a false
  // success, an automatic retry, or an endless wait after the snapshot erases
  // its identity.
  ResolveSnapshotCoveredSubmissionsLocked(
      last_included_index, last_included_term, retain_suffix);

  snapidx_ = last_included_index;
  snapterm_ = last_included_term;
  snapshot_trigger_index_.store(
      snapidx_, rusty::sync::atomic::Ordering::Release);

  // ============================================================================
  // Reconcile in-memory log and queued application work
  // ============================================================================
  if (retain_suffix) {
    raft_logs_.erase(raft_logs_.begin(),
                     raft_logs_.upper_bound(last_included_index));
    lastLogIndex = std::max(previous_last_log_index,
                            last_included_index);
  } else {
    raft_logs_.clear();
    lastLogIndex = last_included_index;
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

  // Update min_active_slot_ to reflect compacted log
  if (last_included_index + 1 > min_active_slot_) {
    min_active_slot_ = last_included_index + 1;
  }

  // ============================================================================
  // Advance commitIndex and executeIndex
  // ============================================================================
  commitIndex = last_included_index;
  const uint64_t previous_spec_commit_index = specCommitIndex_;
  const uint64_t previous_secured_log_index = securedLogIndex_;
  specCommitIndex_ = raft_server_snapshot_progress_clamp(
      specCommitIndex_, commitIndex, lastLogIndex);
  // A memory-committed prefix need not yet have a durable quorum. Snapshot
  // installation may raise commit/speculative progress, but it must not forge
  // secured progress; only clamp an old secured marker to the surviving log.
  securedLogIndex_ = raft_server_commit_index_clamp(
      securedLogIndex_, specCommitIndex_);
  if (specCommitIndex_ != previous_spec_commit_index ||
      securedLogIndex_ != previous_secured_log_index) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Reconciled speculative indices "
             "spec {}->{} secured {}->{} at lastLogIndex={}",
             site_id_, previous_spec_commit_index, specCommitIndex_,
             previous_secured_log_index, securedLogIndex_, lastLogIndex);
  }
  verify(securedLogIndex_ <= specCommitIndex_);
  verify(commitIndex <= specCommitIndex_);
  verify(specCommitIndex_ <= lastLogIndex);
  verify(commitIndex <= lastLogIndex);

  if (snapshot_persistence_ordered) {
    const slotid_t remove_through_index =
        retain_suffix ? last_included_index : persistent_last_index;
    const bool storage_reconciled = raft_server_write_and_sync(
        *log_storage_,
        [this, persistent_first_index, remove_through_index, retain_suffix](
            raft::LogStorage& storage) {
          bool removal_succeeded = true;
          if (persistent_first_index != 0) {
            if (raft_server_log_index_has_successor(remove_through_index)) {
              const slotid_t remove_end = remove_through_index + 1;
              if (persistent_first_index < remove_end) {
                removal_succeeded = storage.remove_range(
                    persistent_first_index, remove_end);
              }
            } else {
              if (persistent_first_index < UINT64_MAX) {
                removal_succeeded = storage.remove_range(
                    persistent_first_index, UINT64_MAX);
              }
              if (storage.get(UINT64_MAX).is_some()) {
                const bool terminal_removed = storage.remove(UINT64_MAX);
                removal_succeeded =
                    terminal_removed && removal_succeeded;
              }
            }
          }

          const bool metadata_written = storage.set_metadata_batch({
              {META_COMMIT_INDEX, std::to_string(commitIndex)},
              {META_SPEC_COMMIT_INDEX, std::to_string(specCommitIndex_)},
              {META_SECURED_LOG_INDEX, std::to_string(securedLogIndex_)},
          });
          if (!removal_succeeded) {
            Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to reconcile "
                      "persistent log range [{}..={}] while retain_suffix={}",
                      site_id_, persistent_first_index,
                      remove_through_index, retain_suffix);
          }
          return removal_succeeded && metadata_written;
        });
    if (!RecordPersistenceResult(
            storage_reconciled, "snapshot log reconciliation write/sync")) {
      // Snapshot bytes and memory may already have advanced. Refuse the ACK
      // and fail stop so this replica cannot vote or advertise stale durable
      // state until restart performs recovery.
      rpc_ready_.store(false, rusty::sync::atomic::Ordering::Release);
      stop_.store(true, rusty::sync::atomic::Ordering::Release);
      looping_.store(false, rusty::sync::atomic::Ordering::Release);
      apply_thread_running_.store(false);
      *term_out = 0;
      return;
    }
  }

  // Publish application only after the state machine has finished loading the
  // snapshot. Acquire waiters must never observe the covered indices early.
  PublishAppliedIndex(last_included_index);

  Log_info("[INSTALL-SNAPSHOT] Site {}: Installed snapshot from leader {} "
           "(snapidx={}, snapterm={}, commitIndex={}, executeIndex={}, "
           "lastLogIndex={}, retain_suffix={}, purged_apply={})",
           site_id_, leader_id, snapidx_, snapterm_, commitIndex, executeIndex,
           lastLogIndex, retain_suffix, purged_apply_entries);
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

// @unsafe - Final native-thread completion barrier. Callers must not hold mtx_.
void RaftServer::StopLeadershipTransferMonitoring() {
  leadership_monitor_stop_.store(
      true, rusty::sync::atomic::Ordering::Release);
  leadership_monitor_wait_cv_.notify_all();

  rusty::Option<rusty::thread::JoinHandle<rusty::thread::Unit>>
      monitor_to_join = rusty::None;
  {
    auto monitor_guard = leadership_monitor_thread_.lock().unwrap();
    if (monitor_guard->is_some()) {
      leadership_monitor_joining_.store(
          true, rusty::sync::atomic::Ordering::Release);
      monitor_to_join = monitor_guard->take();
    }
  }
  if (monitor_to_join.is_some()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site {}: Joining monitor thread",
              site_id_);
    auto joined = monitor_to_join.take().unwrap().join();
    verify(joined.is_ok());
    leadership_monitor_joining_.store(
        false, rusty::sync::atomic::Ordering::Release);
    leadership_monitor_wait_cv_.notify_all();
    return;
  }

  // Another concurrent shutdown caller may own the taken handle. It must not
  // return to a destructor until that owner has completed the join.
  while (leadership_monitor_joining_.load(
      rusty::sync::atomic::Ordering::Acquire)) {
    if (Fiber::current_fiber().is_some()) {
      Fiber::sleep(1000);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

// @unsafe - Starts one persistent native monitor. It idles across follower and
// preferred-leader periods and is joined only by the final shutdown barrier.
void RaftServer::StartLeadershipTransferMonitoring() {
  auto monitor_guard = leadership_monitor_thread_.lock().unwrap();
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    return;
  }
  if (monitor_guard->is_some()) {
    return;
  }
  leadership_monitor_stop_.store(
      false, rusty::sync::atomic::Ordering::Release);

  Log_info("[LEADERSHIP-TRANSFER] Site {}: Starting leadership transfer monitoring thread",
           site_id_);

  // Launch one monitor for the server lifetime. A fresh stable interval begins
  // each time this server becomes a non-preferred leader.
  *monitor_guard = rusty::Some(rusty::thread::spawn([this]() {
    const uint64_t CHECK_INTERVAL_MS = 1000;  // Check every 1 second
    const uint64_t MIN_STABLE_TIME_US = 500000; // Wait 0.5 seconds (in microseconds) after becoming leader before transferring
    bool was_transfer_candidate = false;
    uint64_t became_transfer_candidate_time = 0;

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Monitor thread started (will check every {}ms)",
             site_id_, CHECK_INTERVAL_MS);

    while (!leadership_monitor_stop_.load(
               rusty::sync::atomic::Ordering::Acquire) &&
           !stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
      {
        auto wait_guard = leadership_monitor_wait_mtx_.lock().unwrap();
        auto wait_result = leadership_monitor_wait_cv_.wait_timeout_us_while(
            std::move(wait_guard), CHECK_INTERVAL_MS * 1000,
            [this](bool&) {
              return !leadership_monitor_stop_.load(
                         rusty::sync::atomic::Ordering::Acquire) &&
                     !stop_.load(
                         rusty::sync::atomic::Ordering::Acquire);
            });
        verify(wait_result.is_ok());
      }

      if (leadership_monitor_stop_.load(
              rusty::sync::atomic::Ordering::Acquire) ||
          stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
        break;
      }

      bool should_transfer = false;

      // Critical section: check shared state with proper locking
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);

        // Check if we should stop monitoring
        if (leadership_monitor_stop_.load(
                rusty::sync::atomic::Ordering::Acquire)) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: Monitor stop requested, exiting", site_id_);
          break;
        }

        // Check if server is shutting down
        if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: Server shutting down, exiting monitor", site_id_);
          break;
        }

        // Stay alive but idle until this server is a non-preferred leader.
        const bool is_transfer_candidate =
            is_leader_ && !AmIPreferredLeader();
        if (!is_transfer_candidate) {
          was_transfer_candidate = false;
          continue;
        }
        if (!was_transfer_candidate) {
          was_transfer_candidate = true;
          became_transfer_candidate_time = Time::now(false);
          continue;
        }

        // Wait for cluster to stabilize after becoming leader
        uint64_t time_as_leader =
            Time::now(false) - became_transfer_candidate_time;
        if (!raft_server_leadership_stable_window_elapsed(
                time_as_leader, MIN_STABLE_TIME_US)) {
          continue;
        }

        // Check if we should transfer leadership
        if (ShouldTransferLeadership()) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: Conditions met, initiating transfer NOW",
                   site_id_);
          should_transfer = true;
        }
      } // End critical section - LOCK RELEASED

      // Call InitiateLeadershipTransfer WITHOUT holding lock to avoid deadlock
      if (should_transfer) {
        InitiateLeadershipTransfer();
        // The transition normally makes this server a follower. Reset here as
        // well so a failed transfer cannot reuse an old stability interval.
        was_transfer_candidate = false;
      }
    }

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Monitor thread exiting", site_id_);
  }));
}

// @unsafe - Calls Setup if not already initialized
void RaftServer::EnsureSetup() {
  if (heartbeat_setup_) {
    return;
  }
  heartbeat_setup_ = true;
  Setup();
}

// @unsafe - Checks conditions for leadership transfer (mutex and map access marked safe via @external)
bool RaftServer::ShouldTransferLeadership() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Must be leader
  if (!is_leader_) {
    return false;
  }

  // Must not be preferred (preferred leaders don't transfer)
  if (AmIPreferredLeader()) {
    return false;
  }

  // Must have a preferred leader configured
  // @unsafe
  {
  if (preferred_leader_site_id_ == INVALID_SITEID) {
    return false;
  }
  }

  // Already transferring
  if (transferring_leadership_) {
    return false;
  }

  // Check if preferred replica is in our peer list
  auto it = match_index_.find(preferred_leader_site_id_);
  if (it == match_index_.end()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site {}: Preferred replica {} not in peer list",
              site_id_, preferred_leader_site_id_);
    return false;
  }

  // Check if preferred replica is caught up
  slotid_t preferred_match_index = it->second;
  bool is_caught_up = (preferred_match_index >= commitIndex);

  if (!is_caught_up) {
    Log_debug("[LEADERSHIP-TRANSFER] Site {}: Preferred replica {} not caught up (match={}, commit={})",
              site_id_, preferred_leader_site_id_, preferred_match_index, commitIndex);
    return false;
  }

  Log_info("[LEADERSHIP-TRANSFER] Site {}: Preferred replica {} is caught up! Ready to transfer",
           site_id_, preferred_leader_site_id_);
  return true;
}

// @unsafe - Initiates leadership transfer (RPC calls wrapped in @unsafe blocks)
void RaftServer::InitiateLeadershipTransfer() {
  // Check if server is shutting down
  if (stop_.load(rusty::sync::atomic::Ordering::Acquire)) {
    Log_info("[LEADERSHIP-TRANSFER] Site {}: Aborting transfer - server shutting down", site_id_);
    return;
  }

  siteid_t target_site_id;
  parid_t par_id;
  uint64_t current_term_snapshot;

  // ============================================================================
  // PIGGYBACKED LEADERSHIP TRANSFER (Approach 2)
  // ============================================================================

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // Conditions may have changed after the monitor released mtx_. Revalidate
    // before publishing transfer heartbeats or changing transfer state.
    if (stop_.load(rusty::sync::atomic::Ordering::Acquire) ||
        !ShouldTransferLeadership()) {
      return;
    }

    target_site_id = preferred_leader_site_id_;
    par_id = partition_id_;
    current_term_snapshot = currentTerm;

    // Mark transfer as in progress - this will suppress elections on non-preferred replicas
    transferring_leadership_ = true;
    leadership_transfer_start_time_ = Time::now(false);

    Log_info("[LEADERSHIP-TRANSFER] Site {} (partition {}): Starting transfer to site {}",
             site_id_, partition_id_, target_site_id);

    // Send heartbeats to ALL replicas
    for (auto& kv : match_index_) {
      siteid_t peer_site_id = kv.first;

      if (peer_site_id == site_id_) {
        continue;
      }

      slotid_t slot = commitIndex;
      ballot_t ballot = 0;
      uint64_t prevLogIndex = next_index_[peer_site_id] - 1;
      uint64_t prevLogTerm = 0;

      if (prevLogIndex > 0 && prevLogIndex < logs_.size()) {
        prevLogTerm = logs_[prevLogIndex]->term;
      }

      // Send trigger_election_now=true to ALL replicas during transfer:
      // - Preferred replica: Will start election
      // - Non-preferred replicas: Will activate election suppression
      bool trigger_election = true;  // Signal transfer to ALL replicas

      // @unsafe
      {
      commo()->SendAppendEntries(
        peer_site_id,
        partition_id_,
        slot,
        ballot,
        true,
        site_id_,
        currentTerm,
        prevLogIndex,
        prevLogTerm,
        commitIndex,
        janus::Command{},
        0,
        trigger_election
      );
      }
    }
  }

  // Sleep briefly to ensure the RPC library has time to send the packets.
  // Note: The preferred replica will wait 30ms before starting election,
  // so this sleep is just to ensure packet transmission, not to delay step-down.
  // We will likely step down earlier when we receive RequestVote from preferred replica.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // ============================================================================
  // Step Down from Leadership Immediately
  // ============================================================================
  // With piggybacked approach, we step down immediately after sending the message.
  // The preferred replica will:
  // 1. Reset its election timeout (from the heartbeat)
  // 2. Start election immediately (from the trigger_election_now flag)
  // 3. Win the election (since it's caught up and has all committed entries)
  //
  // Other replicas will:
  // 1. Reset their election timeouts (from normal heartbeats)
  // 2. Not start elections (timers reset)
  // 3. Vote for preferred replica when it requests votes
  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    // A term/preference/leadership change, or new commits beyond the target's
    // match index, invalidates the decision made before the send delay.
    auto target_match = match_index_.find(target_site_id);
    const bool target_still_caught_up =
        target_match != match_index_.end() &&
        raft_server_preferred_replica_is_caught_up(
            target_match->second, commitIndex);
    if (stop_.load(rusty::sync::atomic::Ordering::Acquire) ||
        !is_leader_ || currentTerm != current_term_snapshot ||
        preferred_leader_site_id_ != target_site_id ||
        !target_still_caught_up) {
      transferring_leadership_ = false;
      Log_info("[LEADERSHIP-TRANSFER] Site {}: Transfer conditions changed; "
               "remaining leader", site_id_);
      return;
    }

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Stepping down from leadership (current_term={})",
             site_id_, currentTerm);

    // The preferred target is only a candidate until it wins the transfer
    // election. Do not advertise it (or retain self) as an elected leader.
    current_leader_id_ = raft_server_leader_hint_after_transition(
        false, false, site_id_, target_site_id,
        static_cast<siteid_t>(INVALID_SITEID));

    // Become follower - this stops heartbeats and allows new leader to emerge
    setIsLeader(false);

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Leadership transfer complete - now follower",
             site_id_);
  }
}

// ============================================================================
// SPECULATIVE REPLICATION STATE
// ============================================================================

void RaftServer::ResetSpeculativeState() {
  // Note: caller must hold mtx_ lock

  if (is_leader_) {
    // On becoming leader: initialize with self votes
    specVoters_.clear();
    specVoters_.insert(site_id_);  // voted for self
    durableVoters_.clear();
    if (raft_server_persistence_can_report_durable(HasDurableStorage())) {
      durableVoters_.insert(site_id_);
    }

    // Reset commit indices to current commitIndex (from previous term)
    securedLogIndex_ = commitIndex;
    specCommitIndex_ = commitIndex;

    securedLeader_ = raft::raft_quorum_count_reached(
        durableVoters_.size(), GetQuorumSize());

    Log_info("[SPEC-RAFT] Site {}: Reset speculative state as new leader - "
             "specVoters={{{}}} durableVoters={{{}}} securedLogIndex={} specCommitIndex={}",
             site_id_, site_id_, site_id_, securedLogIndex_, specCommitIndex_);
  } else {
    // On stepping down: clear all speculative state
    specVoters_.clear();
    durableVoters_.clear();
    securedLogIndex_ = 0;
    specCommitIndex_ = 0;
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site {}: Cleared speculative state (stepped down)",
             site_id_);
  }

  // Persist the updated speculative indices
  PersistSpeculativeIndicesToLogStorage();

  // Clear ack tracking maps
  memoryAcks_.clear();
  durableAcks_.clear();
  election_in_progress_ = false;
  earlyDurableVoters_.clear();

  // Reset callback notification tracking
  // Note: We don't clear pendingCallbacks_ here because:
  // - On becoming leader: there shouldn't be any pending callbacks yet
  // - On stepping down: NotifyRollback() handles clearing after notification
  lastSpecNotifiedIndex_ = commitIndex;  // Don't re-notify already-committed entries
  lastDurableNotifiedIndex_ = commitIndex;
}

void RaftServer::VerifySpeculativeInvariants() const {
  // Memory commit and speculative commit can advance before disk quorum. The
  // durable boundary is therefore not a floor for commitIndex; both progress
  // paths are independently bounded by specCommitIndex and the local log.
  if (securedLogIndex_ > specCommitIndex_) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: securedLogIndex ({}) > specCommitIndex ({})",
              securedLogIndex_, specCommitIndex_);
    verify(securedLogIndex_ <= specCommitIndex_);
  }

  if (commitIndex > specCommitIndex_) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: commitIndex ({}) > "
              "specCommitIndex ({})",
              commitIndex, specCommitIndex_);
    verify(commitIndex <= specCommitIndex_);
  }

  if (specCommitIndex_ > lastLogIndex) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: specCommitIndex ({}) > lastLogIndex ({})",
              specCommitIndex_, lastLogIndex);
    verify(specCommitIndex_ <= lastLogIndex);
  }

  // Note: durableVoters ⊆ specVoters is NOT strictly enforced after crashes.
  // A crashed node loses its memory vote but keeps its durable vote on disk.
  // This is expected behavior, not an invariant violation.
  //
  // Key insight: |durableVoters| >= quorum is sufficient for securedLeader = true.
  // Once durable quorum is reached, specVoters quorum is no longer required.
  // See docs/dev/phase6_relax_invariant_plan.md for full safety argument.

  Log_debug("[SPEC-RAFT] Site {}: Invariants OK - securedLogIndex={} specCommitIndex={} lastLogIndex={}",
            site_id_, securedLogIndex_, specCommitIndex_, lastLogIndex);
}

// ============================================================================
// OnPeerRestart - Handle speculative state invalidation on peer restart
// ============================================================================

void RaftServer::OnPeerRestart(siteid_t restarted_site_id) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring peer restart from {} - not leader",
              site_id_, restarted_site_id);
    return;
  }

  Log_info("[SPEC-RAFT] Site {}: Handling peer restart from site {}",
           site_id_, restarted_site_id);

  // Remove from specVoters (their memory vote is no longer reliable)
  size_t removed_from_voters = specVoters_.erase(restarted_site_id);
  if (removed_from_voters > 0) {
    Log_info("[SPEC-RAFT] Site {}: Removed site {} from specVoters (now size={})",
             site_id_, restarted_site_id, specVoters_.size());
  }

  // Remove from memoryAcks for unsecured entries only
  // Entries at or below securedLogIndex are already durably committed,
  // so removing the restarted server doesn't affect their status
  size_t entries_affected = 0;
  for (auto& entry : memoryAcks_) {
    uint64_t idx = entry.first;
    if (idx > securedLogIndex_) {
      if (entry.second.erase(restarted_site_id) > 0) {
        entries_affected++;
      }
    }
  }
  if (entries_affected > 0) {
    Log_info("[SPEC-RAFT] Site {}: Removed site {} from memoryAcks for {} unsecured entries",
             site_id_, restarted_site_id, entries_affected);
  }

  // Note: We don't remove from durableVoters or durableAcks because:
  // 1. durableVoters represents votes that were persisted to disk BEFORE the crash
  //    - If the vote was durable, it survives the crash
  //    - If it wasn't durable, it was never in durableVoters
  // 2. durableAcks represents entries that were persisted to disk
  //    - Same logic: durable acks survive crashes by definition

  // Check if we need to become secured or step down
  // Relaxed invariant - durableVoters and specVoters are independent after crashes
  if (!securedLeader_ && is_leader_) {
    size_t quorum = GetQuorumSize();

    // Check if durable quorum is sufficient for secured status.  The set
    // explicitly contains every persisted vote (including self only when local
    // persistence is enabled), so no implicit +1 is needed.
    size_t durable_vote_count = durableVoters_.size();
    if (HasDurableStorage() && raft_server_should_become_secured(
            securedLeader_, durable_vote_count, quorum)) {
      // We have durable quorum - become secured leader
      // Safety: durableVoters have votedFor=us on disk, can't vote for others in this term
      securedLeader_ = true;
      Log_info("[SPEC-RAFT] Site {}: Became secured via durable quorum ({}/{}) "
               "despite spec quorum loss (specVoters={})",
               site_id_, durable_vote_count, quorum, specVoters_.size());
      MaybeAdvanceSecuredLogIndex();
    } else {
      // No durable quorum yet - check speculative quorum
      // Note: site_id_ is already in specVoters_ (inserted by ResetSpeculativeState
      // or RequestElection), so no +1 needed.
      size_t vote_count = specVoters_.size();
      if (vote_count < quorum) {
        // No durable quorum AND no speculative quorum - must step down
        Log_info("[SPEC-RAFT] Site {}: Lost both spec quorum ({}/{}) and durable quorum ({}/{}) - stepping down",
                 site_id_, vote_count, quorum, durable_vote_count, quorum);
        stepDown(StepDownReason::UnsecuredFailure);
        return;  // Don't verify invariants after stepping down
      }
    }
  }

  VerifySpeculativeInvariants();
}

// ============================================================================
// stepDown - Central leader step-down function
// ============================================================================

static const char* StepDownReasonToString(StepDownReason reason) {
  switch (reason) {
    case StepDownReason::UnsecuredFailure: return "UnsecuredFailure";
    case StepDownReason::SecuredFailure: return "SecuredFailure";
    case StepDownReason::HigherTerm: return "HigherTerm";
    default: return "Unknown";
  }
}

void RaftServer::stepDown(StepDownReason reason) {
  // Must be called with mtx_ held (caller's responsibility)
  // Most callers already hold the lock

  Log_info("[SPEC-RAFT] Site {}: Stepping down as leader (reason={}, term={}, "
           "securedLeader={}, specVoters={}, durableVoters={})",
           site_id_, StepDownReasonToString(reason), currentTerm,
           securedLeader_, specVoters_.size(), durableVoters_.size());

  // Quorum-loss step-down has no replacement leader evidence. Higher-term
  // callers distinguish known leader RPCs from term-only responses before
  // entering this central transition.
  if (reason != StepDownReason::HigherTerm) {
    current_leader_id_ = raft_server_leader_hint_after_transition(
        false, false, site_id_, current_leader_id_,
        static_cast<siteid_t>(INVALID_SITEID));
  }

  // Transition to follower state
  // This handles view updates, callback notifications, etc.
  setIsLeader(false);

  // Notify clients before resetting speculative state.  The rollback range is
  // defined by the leader's pre-step-down secured/log bounds; resetting first
  // destroys those bounds and can silently strand pending callbacks.
  NotifyRollback(reason);

  // Clear speculative state only after the notification.  is_leader_ is now
  // false, so ResetSpeculativeState() takes its follower branch rather than
  // accidentally reinitializing this server as a new leader.
  ResetSpeculativeState();

  // A late higher-term response can arrive after this server has already
  // entered a new candidacy. Demotion is terminal for that election as well
  // as for the old leadership epoch.
  req_voting_ = false;
  election_in_progress_ = false;
  earlyDurableVoters_.clear();

  // Reset election timer
  // Important: Give other servers time to elect a new leader
  resetTimer("stepDown");

  Log_info("[SPEC-RAFT] Site {}: Step-down complete, now follower", site_id_);

}

// ============================================================================
// CLIENT NOTIFICATION CALLBACKS
// ============================================================================

// @unsafe - Caller holds mtx_; callback may run synchronously under that lock.
uint64_t RaftServer::RegisterCommitCallbackLocked(
    uint64_t index, std::function<void(CommitStatus)> callback) {
  // Zero is reserved for "no registration". Wrapping is unreachable in any
  // practical server lifetime, but still skip the sentinel deterministically.
  if (nextCommitCallbackToken_ == 0) {
    nextCommitCallbackToken_ = 1;
  }
  const uint64_t token = nextCommitCallbackToken_++;

  // If already speculatively committed, invoke immediately
  if (index <= specCommitIndex_) {
    Log_debug("[SPEC-CALLBACK] Index {} already spec-committed, notifying SPECULATIVE",
              index);
    callback(CommitStatus::SPECULATIVE);
  }

  // If already durably committed, invoke immediately
  if (securedLeader_ && HasDurableStorage() &&
      raft_server_log_index_at_or_below(index, securedLogIndex_)) {
    Log_debug("[SPEC-CALLBACK] Index {} already durable-committed, notifying DURABLE",
              index);
    callback(CommitStatus::DURABLE);
    return token;  // No need to track - already fully committed
  }

  // Store callback for future notification
  pendingCallbacks_[index] = {token, std::move(callback)};
  Log_debug("[SPEC-CALLBACK] Registered callback for index {} token {}",
            index, token);
  return token;
}

uint64_t RaftServer::RegisterCommitCallback(
    uint64_t index, std::function<void(CommitStatus)> callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return RegisterCommitCallbackLocked(index, std::move(callback));
}

bool RaftServer::UnregisterCommitCallback(
    uint64_t index, uint64_t callback_token) {
  if (callback_token == 0) {
    return false;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  auto it = pendingCallbacks_.find(index);
  if (it == pendingCallbacks_.end() ||
      it->second.token != callback_token) {
    return false;
  }

  pendingCallbacks_.erase(it);
  Log_debug("[SPEC-CALLBACK] Unregistered callback for index {} token {}",
            index, callback_token);
  return true;
}

void RaftServer::NotifyCallbacks(uint64_t from, uint64_t to, CommitStatus status) {
  // Note: Caller must hold mtx_
  // Notify callbacks for indices in (from, to]

  for (uint64_t idx = from + 1; idx <= to; ++idx) {
    auto it = pendingCallbacks_.find(idx);
    if (it != pendingCallbacks_.end()) {
      Log_debug("[SPEC-CALLBACK] Notifying index {} with status {}",
                idx, static_cast<int>(status));
      const uint64_t callback_token = it->second.token;
      auto callback = it->second.callback;
      callback(status);

      // If DURABLE, remove callback (fully committed)
      if (raft_server_commit_status_is_durable(status)) {
        // The callback may re-enter cancellation through the recursive mutex.
        // Re-find and match the token before erasing to avoid invalid iterators
        // or deleting a replacement registration.
        auto current = pendingCallbacks_.find(idx);
        if (current != pendingCallbacks_.end() &&
            current->second.token == callback_token) {
          pendingCallbacks_.erase(current);
        }
      }
    }
  }
}

// @unsafe - Invokes callbacks, clears pendingCallbacks_
void RaftServer::NotifyRollback(StepDownReason reason) {
  // Note: Caller must hold mtx_
  // Notify pending callbacks based on step-down reason

  Log_info("[SPEC-CALLBACK] NotifyRollback reason={}, pending={}, "
           "commitIndex={}, specCommitIndex={}, securedLogIndex={}, lastLogIndex={}",
           StepDownReasonToString(reason), pendingCallbacks_.size(),
           commitIndex, specCommitIndex_, securedLogIndex_, lastLogIndex);

  switch (reason) {
    case StepDownReason::UnsecuredFailure:
      // Lost speculative quorum while unsecured leader.
      // Entries above securedLogIndex_ have no durable-quorum guarantee.  This
      // includes entries already exposed as SPECULATIVE (commitIndex advances
      // on the same memory quorum) and locally appended entries that have not
      // reached a memory quorum yet.
      Log_info("[SPEC-CALLBACK] UnsecuredFailure: rolling back entries ({}, {}]",
               securedLogIndex_, lastLogIndex);
      NotifyCallbacks(securedLogIndex_, lastLogIndex, CommitStatus::ROLLEDBACK);  // @unsafe
      break;

    case StepDownReason::SecuredFailure:
      // Lost quorum after becoming secured.  Entries through
      // securedLogIndex_ are durable; every pending local entry above that
      // boundary needs a terminal notification, whether or not it reached
      // SPECULATIVE status.
      Log_info("[SPEC-CALLBACK] SecuredFailure: rolling back entries ({}, {}]",
               securedLogIndex_, lastLogIndex);
      NotifyCallbacks(securedLogIndex_, lastLogIndex, CommitStatus::ROLLEDBACK);  // @unsafe
      break;

    case StepDownReason::HigherTerm:
      // Saw higher term from another server. Entries may still be
      // valid under the new leader - don't send rollback notifications.
      Log_info("[SPEC-CALLBACK] HigherTerm step-down - no automatic rollback");
      break;
  }

  // Clear ALL pending callbacks regardless of reason (we're no longer leader)
  pendingCallbacks_.clear();

  // Reset notification tracking
  lastSpecNotifiedIndex_ = 0;
  lastDurableNotifiedIndex_ = 0;
}

// ============================================================================
// MEMBERSHIP CHANGE IMPLEMENTATION
// ============================================================================

// @safe - Read-only computation on member field
size_t RaftServer::GetQuorumSize() const {
  size_t config_size = 0;
  // @unsafe
  { config_size = current_config_.size(); }
  return config_size / 2 + 1;
}

// @safe - Read-only accessor
// @lifetime: (&'a) -> &'a
const std::set<siteid_t>& RaftServer::GetCurrentConfig() const {
  return current_config_;
}

std::set<siteid_t> RaftServer::GetCurrentConfigSnapshot() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  return current_config_;
}

// @unsafe - Modifies config state
void RaftServer::OnAddServer(const uint64_t term,
                             const uint64_t new_server_id,
                             const std::string& addr,
                             bool_t* success,
                             std::string* error_msg,
                             uint64_t* leader_hint) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // @unsafe
  {
    *leader_hint = (current_leader_id_ != INVALID_SITEID) ? current_leader_id_ : 0;
  }

  // Check if this server is the leader
  if (!IsLeader()) {
    // @unsafe
    {
      *success = false;
      *error_msg = "not leader";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: not leader (site {})", site_id_);
    return;
  }

  // Check if a config change is already pending
  if (config_change_pending_) {
    // @unsafe
    {
      *success = false;
      *error_msg = "config change already pending";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: config change pending (site {})", site_id_);
    return;
  }

  // Check if server is already in config or is already a learner
  if (current_config_.count(static_cast<siteid_t>(new_server_id)) > 0) {
    // @unsafe
    {
      *success = false;
      *error_msg = "server already in config";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: server {} already in config (site {})",
             new_server_id, site_id_);
    return;
  }

  if (learners_.count(static_cast<siteid_t>(new_server_id)) > 0) {
    // @unsafe
    {
      *success = false;
      *error_msg = "server already a learner (catch-up in progress)";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: server {} already a learner (site {})",
             new_server_id, site_id_);
    return;
  }

  // Add server as a learner first. It will receive log entries via HeartbeatLoop
  // (through next_index_/match_index_) but will NOT count towards quorum.
  // Once caught up (match_index_ within catchup_threshold_ of lastLogIndex),
  // CheckAndPromoteLearners() will promote it to a full member.
  auto sid = static_cast<siteid_t>(new_server_id);
  learners_.insert(sid);
  config_change_pending_ = true;
  pending_config_index_ = lastLogIndex;  // Track where this change happened

  // Initialize replication state so HeartbeatLoop sends entries to this learner
  if (next_index_.find(sid) == next_index_.end()) {
    next_index_[sid] = lastLogIndex + 1;
  }
  if (match_index_.find(sid) == match_index_.end()) {
    match_index_[sid] = 0;
  }

  // @unsafe
  {
    *success = true;
    *error_msg = "";
  }

  Log_info("[RAFT-CONFIG] AddServer: added server {} as learner (site {}), "
           "learners={}, config_size={}, next_index={}",
           new_server_id, site_id_, learners_.size(),
           current_config_.size(), next_index_[sid]);
}

// @unsafe - Modifies config state, logs output
void RaftServer::PromoteLearner(siteid_t id) {
  // Must be called with mtx_ held
  learners_.erase(id);
  current_config_.insert(id);
  config_change_pending_ = false;
  Log_info("[RAFT-CONFIG] Promoted learner {} to full member "
           "(config size={}, quorum={}, learners={})",
           id, current_config_.size(), GetQuorumSize(), learners_.size());
}

// @unsafe - Reads match_index_, calls PromoteLearner
void RaftServer::CheckAndPromoteLearners() {
  // Must be called with mtx_ held
  if (learners_.empty()) {
    return;
  }

  std::vector<siteid_t> to_promote;
  for (auto learner_id : learners_) {
    auto it = match_index_.find(learner_id);
    if (it != match_index_.end() && lastLogIndex > 0) {
      // Learner is caught up if within catchup_threshold_ of leader's log
      if (it->second >= lastLogIndex ||
          (lastLogIndex - it->second) <= catchup_threshold_) {
        to_promote.push_back(learner_id);
      }
    }
  }
  for (auto id : to_promote) {
    PromoteLearner(id);
  }
}

// @unsafe - Modifies config state
void RaftServer::OnRemoveServer(const uint64_t term,
                                const uint64_t server_id,
                                bool_t* success,
                                std::string* error_msg,
                                uint64_t* leader_hint) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // @unsafe
  {
    *leader_hint = (current_leader_id_ != INVALID_SITEID) ? current_leader_id_ : 0;
  }

  // Check if this server is the leader
  if (!IsLeader()) {
    // @unsafe
    {
      *success = false;
      *error_msg = "not leader";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: not leader (site {})", site_id_);
    return;
  }

  // Check if a config change is already pending
  if (config_change_pending_) {
    // @unsafe
    {
      *success = false;
      *error_msg = "config change already pending";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: config change pending (site {})", site_id_);
    return;
  }

  // Check if server is in config
  if (current_config_.count(static_cast<siteid_t>(server_id)) == 0) {
    // @unsafe
    {
      *success = false;
      *error_msg = "server not in config";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: server {} not in config (site {})",
             server_id, site_id_);
    return;
  }

  // Cannot remove the last server
  if (current_config_.size() <= 1) {
    // @unsafe
    {
      *success = false;
      *error_msg = "cannot remove last server";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: cannot remove last server (site {})", site_id_);
    return;
  }

  // TODO: In the future, this should append a configuration change entry to the
  // Raft log and only take effect when committed. For now, we apply the change
  // directly in memory.

  // Apply config change immediately
  current_config_.erase(static_cast<siteid_t>(server_id));
  config_change_pending_ = true;
  pending_config_index_ = lastLogIndex;  // Track where this change happened

  // @unsafe
  {
    *success = true;
    *error_msg = "";
  }

  Log_info("[RAFT-CONFIG] RemoveServer: removed server {} from config (site {}), new config size={}, quorum={}",
           server_id, site_id_, current_config_.size(), GetQuorumSize());
}

} // namespace janus
