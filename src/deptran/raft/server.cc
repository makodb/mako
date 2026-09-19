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

// @unsafe { writes to stderr and aborts }
void RaftCheckedMutex::ReportReentry() {
  std::fprintf(
      stderr,
      "\n[RAFT-FATAL] re-entrant acquisition of RaftServer::mtx_ on one "
      "thread.\n"
      "  This is a deadlock. mtx_ is a plain mutex, not a recursive one, so "
      "the\n"
      "  thread would wait forever for a lock only it can release.\n"
      "  The usual cause is a state-machine snapshot callback that calls "
      "back\n"
      "  into the same RaftServer. Both callbacks registered through\n"
      "  SetStateMachineSnapshotCallbacks run with mtx_ held and must not "
      "reach\n"
      "  any method that takes it -- GetAppliedIndex, GetSnapshotIndex, "
      "IsLeader,\n"
      "  Start, and so on. See the contract on that method.\n\n");
  std::abort();
}

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
  // The body is Rust (RaftServerBase::InitializeSnapshotManagerLocked). What
  // stays here is the catch-all, which spanned the whole original body:
  // exceptions have no DSL spelling, and this one turns a throwing recovery
  // into a fail-stop rather than a half-restored replica.
  try {
    return InitializeSnapshotManagerLocked();
  } catch (const std::exception& error) {
    Log_error("[RAFT-SNAPSHOT] Site {} recovery threw: {}", site_id_,
              error.what());
  } catch (...) {
    Log_error("[RAFT-SNAPSHOT] Site {} recovery threw an unknown exception",
              site_id_);
  }
  FailStop();
  return false;
}

std::shared_ptr<janus::raft::SnapshotManager>
RaftServer::GetSnapshotManager() {
  std::lock_guard<RaftCheckedMutex> lock(mtx_);
  return snapshot_manager_;
}


// ============================================================================

// ===========================================================================
// THE KERNEL BRIDGE
//
// RaftServerBase (server.h) is a DSL struct, so its method bodies are Rust.
// Three things such a body cannot do, and this is where each one lands:
//
//   1. Look inside an opaque C++ field. peer_sites_ is a std::vector and
//      current_config_ a std::set; Rust can hold and move them but not
//      iterate them.
//   2. Call a RaftServer method that has not converted yet. These kernels
//      take RaftServerBase* and static_cast down. That is well defined here
//      and not a widening of any contract: RaftServerBase is never
//      instantiated on its own, so every such pointer really does point at a
//      RaftServer. It is the same downcast the existing raft_* trampolines
//      already do from void*, spelled with one less erasure.
//   3. Compile conditionally. #[cfg] is DROPPED SILENTLY inside a DSL block
//      (scripts/raft_dsl.sh rejects one for that reason), so anything behind
//      an #ifdef has to keep its preprocessor guard on this side.
//
// Every kernel here is expected to disappear as its reason does: (1) when the
// field converts, (2) when the callee converts, (3) never -- conditional
// compilation has no Rust spelling in this dialect.
// ===========================================================================
extern "C" {

// (1) opaque-field access

// std::function's bool conversion, and its call.
bool raft_leader_change_cb_is_set(const RaftServerBase* self) {
  return static_cast<bool>(self->leader_change_cb_);
}
void raft_fire_leader_change(RaftServerBase* self, bool is_leader) {
  self->leader_change_cb_(is_leader);
}

// (1) the campaign broadcast, and the reply quorum read back under mtx_.
//
// Two kernels, not one, and deliberately: the broadcast SUSPENDS this fiber
// in wait_timeout, so it must run with mtx_ released, while the quorum's
// highest observed term must be sampled only after mtx_ is reacquired --
// FeedResponse publishes it before its wakeup. Reading the term inside the
// broadcast kernel would lose exactly that ordering.
rusty::RaftVoteQuorumPtr raft_broadcast_vote_and_wait(
    RaftServerBase* self, uint32_t par_id, uint64_t last_log_index,
    int64_t last_log_term, uint16_t self_site_id, int64_t term) {
  rusty::RaftVoteQuorumPtr quorum =
      static_cast<RaftServer*>(self)->commo()->BroadcastVote(
          par_id, last_log_index, last_log_term, self_site_id, term);
  quorum->wait_timeout(1000000);
  return quorum;
}

RaftVoteOutcome raft_vote_quorum_snapshot(
    const rusty::RaftVoteQuorumPtr* quorum) {
  RaftVoteQuorumEvent& event = **quorum;
  RaftVoteOutcome outcome{};
  outcome.term_ = event.Term();
  outcome.yes_ = event.yes();
  outcome.no_ = event.no();
  outcome.n_voted_yes_ = event.q().n_voted_yes_.get();
  outcome.n_voted_no_ = event.q().n_voted_no_.get();
  outcome.timeouted_ = event.q().timeouted_.get();
  return outcome;
}

// (1) more opaque-field access

bool raft_snapshot_manager_has_latest(
    const rusty::RaftSnapshotManagerPtr* manager) {
  return (*manager)->GetLatestSnapshot().is_some();
}

bool raft_command_has_value(const rusty::RaftCommand* cmd) {
  return cmd->has_value();
}

// std::thread join.
void raft_apply_thread_join(RaftServerBase* self) {
  if (self->apply_thread_.joinable()) {
    self->apply_thread_.join();
  }
}

// (2) more downcalls into RaftServer methods that have not converted

void raft_commo_set_network_enabled(RaftServerBase* self, bool enabled) {
  static_cast<RaftServer*>(self)->commo()->SetNetworkEnabled(enabled);
}
// The gate's allocation, through the in-place seam. See the declaration in
// server.h for why the DSL cannot spell this one.
rusty::Arc<ReplicationWakeGate> raft_new_replication_wake_gate() {
  return rusty::Arc<ReplicationWakeGate>::make_with(
      []() { return ReplicationWakeGate::new_(); });
}

// The reactor half of RaftServerBase::RequestReplication: the DSL decided to
// wake, this builds the owner-thread job. No downcast -- the gate is a base
// field now.
void raft_queue_replication_wake(RaftServerBase* self) {
  QueueReplicationWake(self->replication_wake_gate_);
}

// The reactor half of RaftServerBase::CloseReplicationWakeGate, likewise.
void raft_queue_replication_shutdown_wake(RaftServerBase* self) {
  QueueReplicationShutdownWake(self->replication_wake_gate_);
}
RaftStartResult raft_set_local_append(RaftServerBase* self,
                                      const rusty::RaftCommand* cmd,
                                      uint64_t* term, uint64_t* index,
                                      uint64_t slot_id, int64_t ballot) {
  return static_cast<RaftServer*>(self)->SetLocalAppend(*cmd, term, index,
                                                        slot_id, ballot);
}
// Spawns the election-timer fiber. The lambda captures the loop by value --
// two words -- so nothing here outlives the fiber.
void raft_spawn_election_timer(RaftServerBase* self, uint64_t wait_int_us) {
  const ElectionTimerLoop loop = ElectionTimerLoop::new_(self, wait_int_us);
  Fiber::create_run([loop]() { loop.run(); });
}

// (3) more conditionally compiled or otherwise unspellable regions

// SetupInternal under the try/catch the C++ Setup wrapped it in. Exceptions
// have no DSL spelling in this dialect, and this one exists to turn a throwing
// setup into a failed startup rather than a crash.
bool raft_setup_internal_guarded(RaftServerBase* self) {
  try {
    return self->SetupInternal();
  } catch (const std::exception& error) {
    Log_error("[RAFT-STARTUP] Site {} setup threw: {}", self->site_id_,
              error.what());
  } catch (...) {
    Log_error("[RAFT-STARTUP] Site {} setup threw an unknown exception",
              self->site_id_);
  }
  return false;
}

// The shutdown barrier's yield. A reactor fiber sleeps as a fiber; production
// shutdown runs on a native worker thread, where the owner PollThread is
// still live and a short native sleep lets it drain both loop fibers.
void raft_shutdown_barrier_yield() {
  if (Fiber::current_fiber().is_some()) {
    Fiber::sleep(1000);
  } else {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// (1)/(3) snapshot recovery: the environment switches, the manager, and the
// snapshot bytes.

bool raft_prepare_snapshot_cb_is_set(const RaftServerBase* self) {
  return static_cast<bool>(self->prepare_sm_snapshot_cb_);
}

bool raft_env_snapshots_enabled() {
  const char* raw = std::getenv("MAKO_RAFT_SNAPSHOTS");
  return raw != nullptr &&
         (strcmp(raw, "1") == 0 || strcmp(raw, "true") == 0);
}

// 0 unset, 1 parsed into *out, 2 present but unparseable (logged here,
// where the raw string is).
int raft_env_snapshot_interval(uint64_t* out) {
  const char* raw = std::getenv("MAKO_RAFT_SNAPSHOT_INTERVAL");
  if (raw == nullptr || raw[0] == '\0') {
    return 0;
  }
  try {
    *out = std::stoull(raw);
  } catch (const std::exception& error) {
    Log_error("[RAFT-SNAPSHOT] Invalid snapshot interval '{}': {}", raw,
              error.what());
    return 2;
  }
  return 1;
}

// Memory-only Raft has no on-disk snapshot store. A manager injected through
// SetSnapshotManager() before Setup keeps the latest snapshot it holds;
// otherwise start from an empty in-memory manager.
void raft_snapshot_recovery_pick_manager(
    RaftServerBase* self, rusty::RaftSnapshotManagerPtr* out) {
  if (self->snapshot_manager_) {
    *out = self->snapshot_manager_;
    return;
  }
  *out = std::make_shared<janus::raft::MemorySnapshotManager>();
}

bool raft_snapshot_manager_latest(
    const rusty::RaftSnapshotManagerPtr* manager, uint64_t* index,
    uint64_t* term) {
  const auto latest = (*manager)->GetLatestSnapshot();
  if (latest.is_none()) {
    return false;
  }
  const auto discovered = latest.unwrap();
  *index = discovered.last_included_index;
  *term = discovered.last_included_term;
  return true;
}

bool raft_snapshot_manager_load(const rusty::RaftSnapshotManagerPtr* manager,
                                rusty::RaftByteString* data, uint64_t* index,
                                uint64_t* term, uint64_t* size_bytes) {
  janus::raft::SnapshotMetadata metadata;
  if (!(*manager)->LoadLatestSnapshot(&metadata, data)) {
    return false;
  }
  *index = metadata.last_included_index;
  *term = metadata.last_included_term;
  *size_bytes = metadata.size_bytes;
  return true;
}

bool raft_load_state_machine_snapshot(RaftServerBase* self,
                                      const rusty::RaftByteString* data,
                                      uint64_t last_included_index,
                                      uint64_t last_included_term) {
  return static_cast<RaftServer*>(self)->LoadStateMachineSnapshotLocked(
      *data, last_included_index, last_included_term);
}

// (1)/(3) OnInstallSnapshot's staging transaction.

// Validate and stage the exact state-machine image, publish the Raft
// snapshot, then commit the staged image -- in that order, because
// SnapshotManager is the authority for the boundary and the application
// image must not become visible before it.
//
// The staging transaction's destructor aborts a private image, so any
// rejection here leaves the live state machine, the latest Raft snapshot and
// the reconstruction log untouched.
//
// Returns 0 prepare-rejected, 1 save-failed, 2 commit-failed (the caller
// fails stop), 3 installed. CALLER MUST HOLD state_machine_apply_mtx_ then
// mtx_.
int raft_install_snapshot_payload(RaftServerBase* self,
                                  uint64_t last_included_index,
                                  uint64_t last_included_term,
                                  const rusty::RaftByteString* data) {
  RaftServer* const server = static_cast<RaftServer*>(self);
  Log_info("[INSTALL-SNAPSHOT] Site {}: Preparing state machine snapshot ({} bytes)",
           self->site_id_, data->size());
  auto prepared_state_machine = server->PrepareStateMachineSnapshotLocked(
      *data, last_included_index, last_included_term);
  if (prepared_state_machine == nullptr) {
    return 0;
  }

  const bool saved = self->snapshot_manager_->TakeSnapshot(
      last_included_index, last_included_term, data->data(), data->size());
  if (!saved) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              self->site_id_, last_included_index, last_included_term);
    // The transaction has not committed, so its destructor discards only the
    // private staging image; the old live state machine and log remain usable.
    return 1;
  }
  Log_info("[INSTALL-SNAPSHOT] Site {}: Snapshot saved at index={} term={}",
           self->site_id_, last_included_index, last_included_term);

  if (!prepared_state_machine->Commit()) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to commit prepared state "
              "machine snapshot at index={} term={}; failing stop",
              self->site_id_, last_included_index, last_included_term);
    return 2;
  }
  Log_info("[INSTALL-SNAPSHOT] Site {}: State machine committed at index={} "
           "after Raft snapshot publication",
           self->site_id_, last_included_index);
  return 3;
}

// (2) the apply thread's callback invocation.

// The learner callback, under the catch-all the C++ wrapped it in. An
// internal no-op is consumed without reaching the application. Returns false
// when the callback threw, which fails the server stop.
bool raft_apply_invoke(RaftServerBase* self, uint64_t id) {
  try {
    if (!raft_server_command_is_internal_noop(
            self->pending_apply_command_.kind_,
            TpcNoopCommand::static_kind())) {
      self->app_next_(id, self->pending_apply_command_);
    }
  } catch (const std::exception& error) {
    Log_error("[RAFT-APPLY] Site {} callback failed at slot {}: {}",
              self->site_id_, id, error.what());
    return false;
  } catch (...) {
    Log_error("[RAFT-APPLY] Site {} callback failed at slot {}",
              self->site_id_, id);
    return false;
  }
  return true;
}

uint64_t raft_monotonic_now_secs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void raft_thread_sleep_ms(uint64_t millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

// (1)/(3) Setup's environment overrides, membership load, and fiber spawns.

// std::getenv + std::stoull + the catch. Returns 0 when unset, 1 with the
// parsed value in *out, 2 when the value is present but unparseable (the
// diagnostic is logged here, where the raw string is).
int raft_env_heartbeat_interval_us(uint64_t* out) {
  const char* raw = std::getenv("MAKO_RAFT_HEARTBEAT_INTERVAL_US");
  if (raw == nullptr || raw[0] == '\0') {
    return 0;
  }
  try {
    *out = std::stoull(raw);
  } catch (const std::exception& error) {
    Log_error("[RAFT] Invalid heartbeat interval '{}': {}", raw, error.what());
    return 2;
  }
  return 1;
}

int raft_env_log_retention_window(uint64_t* out) {
  const char* raw = std::getenv("MAKO_RAFT_LOG_RETENTION_WINDOW");
  if (raw == nullptr || raw[0] == '\0') {
    return 0;
  }
  try {
    *out = std::stoull(raw);
  } catch (const std::exception& error) {
    Log_error("[RAFT] Invalid log retention window '{}': {}", raw,
              error.what());
    return 2;
  }
  return 1;
}

// Binds the wake gate to the communicator's PollThread before HeartbeatLoop
// can publish its owner-thread-only IntEvent. The communicator always
// retains the PollThread it created or was given.
bool raft_bind_replication_poll(RaftServerBase* self) {
  RaftServer* const server = static_cast<RaftServer*>(self);
  rusty::Option<rusty::Arc<rrr::PollThread>> replication_poll = rusty::None;
  if (server->commo() != nullptr) {
    replication_poll = server->commo()->PollThread();
  }
  if (replication_poll.is_none()) {
    return false;
  }
  server->BindReplicationWakeOwner(replication_poll.unwrap());
  return true;
}

bool raft_initialize_snapshot_manager(RaftServerBase* self) {
  return static_cast<RaftServer*>(self)->InitializeSnapshotManager();
}

// The fixed replica set for this partition's lifetime; memory-only Raft has
// no membership change. Returns how many replicas were loaded.
//
// A std::set used to hold this and be mirrored into config_members_. The set
// is gone; the sort and the dedup it provided are done here explicitly,
// because the round membership and the authority ledger's set-equality check
// both depend on config_members_ being sorted and duplicate-free.
uint64_t raft_load_current_config(RaftServerBase* self) {
  auto config = Config::GetConfig();
  auto replicas = config->SitesByPartitionId(self->partition_id_);
  std::set<siteid_t> sorted_unique;
  for (auto& site : replicas) {
    sorted_unique.insert(site.id);
  }
  self->config_members_.clear();
  for (const siteid_t site : sorted_unique) {
    self->config_members_.push(site);
  }
  return sorted_unique.size();
}

void raft_start_apply_thread(RaftServerBase* self) {
  static_cast<RaftServer*>(self)->StartApplyThread();
}

// Forward-declared because the emitter writes definitions in source order and
// the heartbeat block is further down this file; the fiber spawn is up here
// with the other spawns.
void heartbeat_loop_body(RaftServerBase* server);

void raft_spawn_heartbeat_loop(RaftServerBase* self) {
  // The loop is Rust end to end now (heartbeat_loop_body); this is only the
  // fiber spawn, which has no DSL spelling.
  Fiber::create_run([self]() { heartbeat_loop_body(self); });
}

void raft_spawn_election_timer_fiber(RaftServerBase* self) {
  RaftServer* const server = static_cast<RaftServer*>(self);
  Fiber::create_run([server]() { server->StartElectionTimer(); });
}

// CreateSnapshotLocked's state-machine checkpoint and its persistence: a
// std::string built by a std::function that may throw, an #ifdef fallback,
// and snapshot_manager_ I/O. CALLER MUST HOLD mtx_.
bool raft_snapshot_serialize_and_save(RaftServerBase* self,
                                      uint64_t snap_index,
                                      int64_t snap_term) {
  // Production may compact only behind a real state-machine checkpoint.
  // RaftLab has no application state and uses a strict 16-byte index+term
  // marker instead.
  std::string state_data;
  if (self->create_sm_snapshot_cb_) {
    try {
      state_data = self->create_sm_snapshot_cb_(snap_index);
    } catch (const std::exception& error) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback threw: {}",
                self->site_id_, error.what());
      return false;
    } catch (...) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback threw",
                self->site_id_);
      return false;
    }
    if (state_data.empty()) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback "
                "returned an empty checkpoint; retaining the log",
                self->site_id_);
      return false;
    }
    Log_info("[RAFT-SNAPSHOT] Site {}: State machine snapshot callback produced {} bytes",
             self->site_id_, state_data.size());
  } else {
#ifdef RAFT_TEST_CORO
    // Fallback: 8 bytes execute_index_ + 8 bytes term.
    state_data.resize(sizeof(uint64_t) * 2);
    char* ptr = state_data.data();
    std::memcpy(ptr, &snap_index, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    std::memcpy(ptr, &snap_term, sizeof(uint64_t));
#else
    Log_error("[RAFT-SNAPSHOT] Site {} has no state-machine snapshot callback; "
              "production compaction is disabled",
              self->site_id_);
    return false;
#endif
  }

  const bool saved = self->snapshot_manager_->TakeSnapshot(
      snap_index, snap_term, state_data.data(), state_data.size());
  if (!saved) {
    Log_error("[RAFT-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              self->site_id_, snap_index, snap_term);
    return false;
  }
  return true;
}

// (misc) a monotonic clock and a fiber sleep, for PHASE 2's poll deadline.
uint64_t raft_monotonic_now_us() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
void raft_fiber_sleep_us(uint64_t micros) {
  Fiber::sleep(static_cast<int>(micros < 1 ? 1 : micros));
}

// (misc) the env-tunable election-timeout knobs. One call returning the set,
// rather than four getters: GetElectionTimeout needs them together to make
// one decision, and a boundary crossing should be a unit of work.
RaftElectionTimeouts raft_election_timeouts() {
  RaftElectionTimeouts knobs{};
  knobs.grace_period_us_ = GetPreferredLeaderGracePeriodUs();
  knobs.preferred_us_ = GetPreferredElectionTimeoutUs();
  knobs.non_preferred_grace_us_ = GetNonPreferredGraceElectionTimeoutUs();
  knobs.non_preferred_steady_us_ = GetNonPreferredSteadyElectionTimeoutUs();
  return knobs;
}

// (3) conditionally compiled regions

// RAFT_LEADER_ELECTION_DEBUG only.
void raft_log_set_is_leader_entry(const RaftServerBase* self,
                                  bool prev_is_leader,
                                  bool new_is_leader) {
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site {} (loc {}) term {}: prev_is_leader={} new_is_leader={}",
           self->site_id_, self->loc_id_, self->state_.current_term_,
           prev_is_leader, new_is_leader);
#else
  (void)self;
  (void)prev_is_leader;
  (void)new_is_leader;
#endif
}

// Raft only commits prior-term entries after committing one from the current
// term, so a new leader appends an internal no-op: old client submissions
// then resolve even when every client is blocked on the former leader. The
// apply paths consume this protocol entry without invoking the application
// state machine.
//
// Compiled out under RAFT_TEST_CORO, where the lab harness drives the log
// directly and an unexpected extra entry would fail its index assertions.
// CALLER MUST HOLD mtx_.
void raft_append_leader_noop(RaftServerBase* self) {
#ifndef RAFT_TEST_CORO
  RaftServer* const server = static_cast<RaftServer*>(self);
  uint64_t noop_previous_index = 0;
  uint64_t noop_term = 0;
  auto noop = rusty::Arc<TpcNoopCommand>::make();
  const RaftStartResult noop_result = server->SetLocalAppend(
      janus::Command::pack_aliased<TpcNoopCommand>(std::move(noop)),
      &noop_term, &noop_previous_index);
  verify(raft_server_start_was_appended(noop_result));
  verify(noop_term == self->state_.current_term_);
  verify(self->state_.raft_log_.last_index() == noop_previous_index + 1);
  Log_info("[RAFT-NOOP] Site {} appended leader no-op at index {} term {}",
           self->site_id_, self->state_.raft_log_.last_index(),
           self->state_.current_term_);
  server->RequestReplication();
#else
  (void)self;
#endif
}

}  // extern "C"

RaftServer::RaftServer() {
  // The two members RaftServerBase's generated constructor leaves at their
  // zero value, because a DSL constructor can spell neither of them:
  // std::make_shared, and a macro whose value depends on RAFT_TEST.
  // replication_wake_gate_ is NOT among them any more: the gate's DSL block
  // moved into server.h, so the generated constructor builds the Arc through
  // rusty::Arc<ReplicationWakeGate>::make_with, which is what this
  // member-initialiser used to do by hand.
  async_callback_lifetime_ = std::make_shared<AsyncCallbackLifetime>();
  heartbeat_interval_us_ = HEARTBEAT_INTERVAL;

  async_callback_lifetime_->server = this;
  // Keep the immutable kind-4 compatibility factory registered as soon as a
  // Raft server exists so a legacy payload relayed by a peer still decodes.
  EnsureLegacyRaftLogPayloadRegistered();
#ifdef RAFT_TEST_CORO
  setIsLeader(false);
#endif
  stop_.store(false, rusty::sync::atomic::Ordering::Release);
}

// @unsafe - the reactor's event factory. The three wait methods on
// RaftServerBase are Rust; this is the only step in them that is not, because
// create_sp_int_event has no DSL spelling.
extern "C" rusty::Arc<rrr::IntEvent> raft_create_int_event() {
  return create_sp_int_event(1);
}

void RaftServer::StartApplyThread() {
  apply_thread_running_.store(true, rusty::sync::atomic::Ordering::SeqCst);
  // The loop is Rust (RaftServerBase::ApplyThreadLoop). Keep the thread
  // JOINABLE so the destructor can await it: detaching causes
  // use-after-free, because the thread captures `this` and keeps running
  // after ~RaftServer has destroyed the server, which shows up as an empty
  // std::function invocation the next time it pulls from apply_queue_.
  apply_thread_ = std::thread([this]() { this->ApplyThreadLoop(); });
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
using janus::raft_server_leader_hint_after_transition;
using janus::raft_server_vote_term_is_stale;
using janus::raft_server_leader_rpc_sender_is_authoritative;
using janus::raft_server_append_term_is_acceptable;
using janus::raft_server_append_is_acceptable;
using janus::raft_server_append_sent_end;
using janus::raft_server_append_entry_count_fits;
using janus::raft_server_append_batch_count_is_valid;
using janus::raft_server_append_entry_conflicts;
using janus::raft_server_append_result_last_index;
using janus::raft_server_commit_index_clamp;
using janus::raft_server_candidate_log_is_at_least;
using janus::raft_server_vote_is_idempotent;
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
using janus::RaftEntry;
using janus::RaftServerBase;
using janus::RaftLockGuard;
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
use crate::server_h::raft_server_leader_hint_after_transition;
use crate::server_h::raft_server_vote_term_is_stale;
use crate::server_h::raft_server_leader_rpc_sender_is_authoritative;
use crate::server_h::raft_server_append_term_is_acceptable;
use crate::server_h::raft_server_append_is_acceptable;
use crate::server_h::raft_server_append_sent_end;
use crate::server_h::raft_server_append_entry_count_fits;
use crate::server_h::raft_server_append_batch_count_is_valid;
use crate::server_h::raft_server_append_entry_conflicts;
use crate::server_h::raft_server_append_result_last_index;
use crate::server_h::raft_server_commit_index_clamp;
use crate::server_h::raft_server_candidate_log_is_at_least;
use crate::server_h::raft_server_vote_is_idempotent;
use crate::server_h::BackoffKind;
use crate::server_h::RAFT_SERVER_INVALID_SITE_ID;
use crate::server_h::RaftConsensusState;

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
/*RUSTYCPP:GEN-BEGIN id=raft_server.authority_ledger version=1 rust_sha256=db0c4e65b82655da5fad7d5b5960b6ff8b2ed2457ea2cffe1cdaf3de52f2e29a*/
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

using ::server_h::raft_server_leader_hint_after_transition;

using ::server_h::raft_server_vote_term_is_stale;

using ::server_h::raft_server_leader_rpc_sender_is_authoritative;

using ::server_h::raft_server_append_term_is_acceptable;

using ::server_h::raft_server_append_is_acceptable;

using ::server_h::raft_server_append_sent_end;

using ::server_h::raft_server_append_entry_count_fits;

using ::server_h::raft_server_append_batch_count_is_valid;

using ::server_h::raft_server_append_entry_conflicts;

using ::server_h::raft_server_append_result_last_index;

using ::server_h::raft_server_commit_index_clamp;

using ::server_h::raft_server_candidate_log_is_at_least;

using ::server_h::raft_server_vote_is_idempotent;

using ::server_h::BackoffKind;

using ::server_h::RAFT_SERVER_INVALID_SITE_ID;

using ::server_h::RaftConsensusState;

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

// ============================================================================
// THE HEARTBEAT LOOP
//
// All of it: the round scope, the commit rule, the four phases, the state
// they carry between them, and the driver that sequences them. The C++ that
// remains is the fiber spawn in raft_spawn_heartbeat_loop and the kernels
// each phase calls for work with no DSL spelling -- RPC sends, marshalling,
// the snapshot manager.
//
// Read it in protocol order: the round scope and the commit rule both PHASE 0
// and PHASE 3 apply, then PHASE 0 through PHASE 3, then the driver, then the
// two inbound RPC bodies.
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
use crate::server_h::RaftServerBase;
use crate::server_h::RaftEntry;
use crate::server_h::RaftLockGuard;
// Every C++ kernel this carrier calls, in one place. improper_ctypes is
// allowed because each of these passes an opaque handle by pointer and
// nothing is laid out across the boundary -- the same case as the allow
// on server.h's bridge block.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_monotonic_now_us() -> u64;
    fn raft_fiber_sleep_us(micros: u64);
    fn raft_append_response_read(response: *const rusty::RaftResponsePtr)
        -> AppendRespView;
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
    fn raft_verify(condition: bool);
    fn raft_snapshot_manager_is_set(
        manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_phase1_load_and_send_snapshot(server: *mut RaftServerBase,
                                          site_id: u16, ord: usize) -> bool;
    fn raft_batch_optimization_enabled() -> bool;
    fn raft_append_entries_batch_max() -> u64;
    // The wire kind of a command, for the diagnostics that report why an
    // entry could not be batched. It takes the COMMAND, not (server, index):
    // the lookup is Rust, and only reading inside the opaque payload is not.
    fn raft_command_kind(cmd: *const rusty::RaftCommand) -> i32;
    fn raft_batch_try_push(server: *mut RaftServerBase, index: u64) -> bool;
    fn raft_batch_finalize(server: *mut RaftServerBase,
                           cmd_out: *mut rusty::RaftCommand);

    fn raft_phase1_send_append(server: *mut RaftServerBase, site_id: u16,
                               partition_id: u32, is_leader: bool, term: u64,
                               prev_log_index: u64, prev_log_term: u64,
                               commit_index: u64,
                               cmd: *const rusty::RaftCommand,
                               cmd_log_term: u64) -> rusty::RaftResponsePtr;
    fn raft_ae_decode_payload(server: *mut RaftServerBase,
                              cmd: *const core::ffi::c_void,
                              leader_prev_log_index: u64,
                              leader_next_log_term: u64) -> bool;
    fn raft_ae_apply_incoming(server: *mut RaftServerBase,
                              cmd: *const core::ffi::c_void,
                              leader_prev_log_index: u64,
                              leader_next_log_term: u64,
                              first_write_index: u64);
    fn raft_do_vote(server: *mut RaftServerBase,
                    lst_log_idx: u64,
                    lst_log_term: i64,
                    can_id: u16,
                    can_term: i64,
                    reply_term: &mut i64,
                    vote_granted: &mut i8,
                    vote: bool);
}

// ==========================================================================
// THE ROUND SCOPE, AND THE COMMIT RULE BOTH PHASE 0 AND PHASE 3 APPLY
// ==========================================================================

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

// Same aliasing note as heartbeat_phase3_locked: peers and log are reached
// through `consensus` because both are its fields.
pub fn raft_commit_advance(
    consensus: &mut RaftConsensusState,
    nservers: usize,
) -> CommitAdvance {
    // nservers is the value latched in PHASE 0. Reusing it in PHASE 3 is
    // sound only because current_config_ has exactly one write, during
    // Setup, and progress_ is never erased, so the size is invariant across
    // the round. Assert it rather than trusting the phases to stay in step.
    if consensus.peers_.len() != nservers - 1 {
        panic!("peer table and round membership disagree");
    }
    let candidate_index = consensus.peers_
        .majority_match_index(nservers, consensus.raft_log_.last_index());
    if !raft_server_log_index_above(candidate_index, consensus.commit_index_) {
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    // The candidate is <= last_index() and > commit_index_, so the entry
    // provably exists. This says so rather than leaving a null dereference
    // to express it.
    let candidate = consensus.raft_log_.get(candidate_index);
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

// ==========================================================================
// PHASE 0 -- advance the read-index round, recompute the commit index
// ==========================================================================

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
// Same aliasing note as heartbeat_phase3_locked.
pub fn heartbeat_phase0_locked(
    consensus: &mut RaftConsensusState,
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
    if pending.len() != consensus.peers_.len() {
        pending.resize(consensus.peers_.len());
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
    let advance = raft_commit_advance(consensus, round.nservers());
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

// ==========================================================================
// PHASE 0 and PHASE 3, formerly RaftServer::HeartbeatPhase0/3.
//
// Their decisions were already DSL bodies (heartbeat_phase0_locked and
// heartbeat_phase3_locked). What was left in C++ was the mutex, the apply
// queue, the replication wake and the debug logging -- all of which a DSL
// body can now express, so the C++ halves are gone.
//
// The round members no longer come from a std::vector rebuilt out of
// current_config_ every round; they are server.config_members_, filled once
// during Setup. Same contents, sorted and duplicate-free, which is what both
// the round's membership and the ledger's set-equality check expect.
// ==========================================================================
pub fn heartbeat_phase0_body(server: &mut RaftServerBase,
                             pending_rpcs: &mut PendingTable,
                             authority_rounds: &mut AuthorityLedger,
                             pending_leader_term: &mut rusty::Option<u64>,
                             round: &mut HeartbeatRoundScope) -> bool {
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let leader: bool = server.IsLeaderLocked();
        if leader && heartbeat_round_saturated(server.state_.heartbeat_round_) {
            rusty::raft_log_error_2(
                "[READ-INDEX] site={} heartbeat round saturated in term {}",
                server.site_id_, server.state_.current_term_);
        }
        if leader {
            let mut ord: usize = 0;
            while ord < server.state_.peers_.len() {
                rusty::raft_log_debug_2(
                    "[COMMIT-CALC] match_index_[{}] = {}",
                    server.peer_site_at(ord),
                    server.state_.peers_.match_index(ord));
                ord += 1;
            }
        }

        let site_id: u16 = server.site_id_;
        let members: rusty::Vec<u16> = server.config_members_.clone();
        let outcome: Phase0Outcome = heartbeat_phase0_locked(
            &mut server.state_, round, pending_rpcs, authority_rounds,
            pending_leader_term, &members, site_id, leader);

        if outcome.restart() {
            // Was `continue`; the Rust driver starts the next round when
            // this returns true.
            return true;
        }
        if outcome.commit_advanced() {
            // The apply queue is I/O, so the hand-off stays a call; the
            // DECISION to commit was made above.
            server.EnqueueCommittedEntries(outcome.commit_from(),
                                           outcome.commit_to());
        }
    }

    let members: rusty::Vec<u16> = server.config_members_.clone();
    let opened: bool = authority_rounds.open(
        round.round_id(), &members,
        HeartbeatAuthority::new(round.term(), round.nservers(),
                                server.site_id_));
    round.set_authority_inserted(opened);
    // heartbeat_round_ never wraps. The only possible duplicate is the
    // deliberately fail-closed UINT64_MAX saturation generation, which
    // open() declines rather than overwriting.
    if !round.authority_inserted() {
        unsafe {
            raft_verify(round.round_id() == u64::MAX);
        }
    }
    true
}

// ==========================================================================
// PHASE 1 -- choose each follower's payload and send it
// ==========================================================================

// Chooses what this AppendEntries carries: nothing (a heartbeat), one raw
// entry, or a TpcBatchCommand. Returns true when the follower must be
// skipped. CALLER MUST HOLD mtx_.
//
// The two arms were an #ifdef RAFT_BATCH_OPTIMIZATION / #ifndef pair. They
// are an if/else on a compile-time-constant function now, because the
// preprocessor has no DSL spelling; the compiler folds the dead arm exactly
// as the preprocessor removed it.
#[allow(clippy::too_many_arguments)]
pub fn heartbeat_phase1_select_payload(server: &mut RaftServerBase,
                                       ord: usize, site_id: u16,
                                       prev_log_index: u64,
                                       cmd: &mut rusty::RaftCommand,
                                       cmd_log_term: &mut u64,
                                       sent_end_index: &mut u64) -> bool {
    let mut skip_follower: bool = false;

    if !unsafe { raft_batch_optimization_enabled() } {
        rusty::raft_log_debug_5(
            "[BATCH_CHECK] site={} follower={} next_index={} state_.raft_log_.base()={} state_.raft_log_.last_index()={}",
            server.site_id_, site_id, server.state_.peers_.next_index(ord),
            server.state_.raft_log_.base(),
            server.state_.raft_log_.last_index());
        if server.state_.peers_.next_index(ord)
            <= server.state_.raft_log_.last_index()
        {
            if !raft_server_append_entry_count_fits(prev_log_index, 1) {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-SEND] Log index exhausted after {}, skipping follower {}",
                    prev_log_index, site_id);
                skip_follower = true;
            } else {
                let next: u64 = server.state_.peers_.next_index(ord);
                let slot = server.state_.raft_log_.get(next);
                let usable: bool = slot.is_some()
                    && unsafe {
                        raft_command_has_value(
                            slot.unwrap().cmd() as *const rusty::RaftCommand)
                    };
                if !usable {
                    rusty::raft_log_error_2(
                        "[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                        next, site_id);
                    skip_follower = true;
                } else {
                    let entry: &RaftEntry = slot.unwrap();
                    *cmd_log_term = entry.term() as u64;
                    // Copying a Command is a refcount bump on its inner Arc,
                    // which is what the kernel this replaces did.
                    *cmd = entry.cmd().clone();
                    *sent_end_index =
                        raft_server_append_sent_end(prev_log_index, 1);
                    // The kind tag identifies the payload better than the
                    // inner shared_ptr's raw address ever did.
                    let kind: i32 = unsafe {
                        raft_command_kind(
                            entry.cmd() as *const rusty::RaftCommand)
                    };
                    rusty::raft_log_debug_4(
                        "[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        server.site_id_, next, site_id, kind);
                }
            }
        }
        return skip_follower;
    }

    // A fresh buffer per follower, as the C++ local was.
    server.batch_buffer_.clear();
    let max_batch_entries: u64 = unsafe { raft_append_entries_batch_max() };
    let batch_start_idx: u64 = server.state_.peers_.next_index(ord);
    rusty::raft_log_debug_5(
        "[BATCH_CHECK] site={} follower={} next_index={} state_.raft_log_.base()={} state_.raft_log_.last_index()={}",
        server.site_id_, site_id, server.state_.peers_.next_index(ord),
        server.state_.raft_log_.base(), server.state_.raft_log_.last_index());
    if !raft_server_append_entry_count_fits(prev_log_index, 1) {
        rusty::raft_log_error_2(
            "[HEARTBEAT-BATCH] Log index exhausted after {}, skipping follower {}",
            prev_log_index, site_id);
        skip_follower = true;
    }
    let first_encoded_index: u64 = if skip_follower {
        0
    } else {
        raft_server_append_sent_end(prev_log_index, 1)
    };
    if !skip_follower
        && (batch_start_idx != first_encoded_index
            || batch_start_idx < server.state_.raft_log_.base())
    {
        rusty::raft_log_error_4(
            "[HEARTBEAT-BATCH] Non-contiguous source for follower {}: prev={} start={} min_active={}; refusing to compress a hole",
            site_id, prev_log_index, batch_start_idx,
            server.state_.raft_log_.base());
        skip_follower = true;
    } else if !skip_follower {
        let mut idx: u64 = batch_start_idx;
        while idx <= server.state_.raft_log_.last_index()
            && (server.batch_buffer_.len() as u64) < max_batch_entries
        {
            let entry = server.state_.raft_log_.get(idx);
            let usable: bool = entry.is_some()
                && unsafe {
                    raft_command_has_value(
                        entry.unwrap().cmd() as *const rusty::RaftCommand)
                };
            if !usable {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-BATCH] Missing log entry {} for follower {}; refusing to compress a hole",
                    idx, site_id);
                skip_follower = true;
                break;
            }
            let entry_term: i64 = entry.unwrap().term();
            if !unsafe { raft_batch_try_push(server as *mut RaftServerBase, idx) }
            {
                // Looked up again rather than held across the push above:
                // `entry` borrows the log, and handing the push a *mut to the
                // server ends that borrow. This branch is the rare one -- an
                // entry that is not a TpcCommitCommand -- so the second
                // lookup costs nothing on the batching path.
                let slot = server.state_.raft_log_.get(idx);
                let kind: i32 = unsafe {
                    raft_command_kind(
                        slot.unwrap().cmd() as *const rusty::RaftCommand)
                };
                let batched: u64 = server.batch_buffer_.len() as u64;
                if batched == 0 {
                    rusty::raft_log_info_3(
                        "[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log",
                        server.site_id_, idx, kind);
                    *cmd = slot.unwrap().cmd().clone();
                    *cmd_log_term = entry_term as u64;
                    *sent_end_index =
                        raft_server_append_sent_end(prev_log_index, 1);
                } else {
                    rusty::raft_log_info_3(
                        "[BATCH_STOP] site={} idx={}: ending batch before non-TpcCommitCommand kind={}",
                        server.site_id_, idx, kind);
                }
                break;
            }
            if !raft_server_log_index_has_successor(idx) {
                break;
            }
            idx += 1;
        }
    }

    let encoded_entry_count: u64 = server.batch_buffer_.len() as u64;
    if !skip_follower && encoded_entry_count > 0
        && !raft_server_append_batch_count_is_valid(prev_log_index,
                                                    encoded_entry_count)
    {
        rusty::raft_log_error_3(
            "[HEARTBEAT-BATCH] Invalid encoded count {} after previous index {}; skipping follower {}",
            encoded_entry_count, prev_log_index, site_id);
        skip_follower = true;
    }
    if !skip_follower && encoded_entry_count > 0 {
        unsafe {
            raft_batch_finalize(server as *mut RaftServerBase,
                                cmd as *mut rusty::RaftCommand);
        }
        *sent_end_index = raft_server_append_sent_end(prev_log_index,
                                                      encoded_entry_count);
        let batch_end_idx: u64 = *sent_end_index;
        let truncated: bool =
            batch_end_idx < server.state_.raft_log_.last_index();
        rusty::raft_log_info_6(
            "[BATCH_SEND] site={} sending batch of {} entries to follower {} (from={} to={}{})",
            server.site_id_, encoded_entry_count, site_id, batch_start_idx,
            batch_end_idx, if truncated { ", truncated" } else { "" });
    }
    skip_follower
}

// ==========================================================================
// PHASE 1: send every AppendEntries RPC in parallel, non-blocking.
//
// Formerly RaftServer::HeartbeatPhase1. The ordinal is used for ITERATION
// ONLY; every read and write of a follower's next index goes through
// state_.peers_.next_index(ord).
//
// That is not style. The body reaches commo()->SendInstallSnapshot inside
// the lock scope, and that call's completion callback takes the SAME mutex
// and writes next_index -- so a callback that completes synchronously
// re-enters and mutates the table while a dereferenced cursor into it is
// live. Holding no dereferenced cursor across that call is also what makes
// the loop expressible in Rust at all: a `&mut` into a table cannot be held
// across a call that takes `&mut` to the same table.
// ==========================================================================
// The zero-initialisation of prev_log_index and sent_end_index is the C++
// original's, kept so the emitted C++ declares initialised locals rather
// than uninitialised ones. `is_none` then `unwrap` is likewise the shape the
// original had; rewriting it as a match would obscure the correspondence.
#[allow(unused_assignments, clippy::unnecessary_unwrap)]
pub fn heartbeat_phase1_body(server: &mut RaftServerBase,
                             pending_rpcs: &mut PendingTable,
                             authority_rounds: &mut AuthorityLedger,
                             round: &HeartbeatRoundScope) {
    let partition_id: u32 = server.partition_id_;
    let mut ord: usize = 0;
    while ord < server.state_.peers_.len() {
        let site_id: u16 = server.peer_site_at(ord);
        if site_id == server.site_id_ {
            ord += 1;
            continue;
        }
        if !server.IsLeader() {
            break;  // Stop sending if we lost leadership.
        }
        if pending_rpcs.occupied(ord) {
            ord += 1;
            continue;
        }

        let mut prev_log_index: u64 = 0;
        let mut prev_log_term: u64 = 0;
        // An empty Command (has_value() == false) signals a heartbeat.
        let mut cmd: rusty::RaftCommand = Default::default();
        let mut cmd_log_term: u64 = 0;
        let mut sent_end_index: u64 = 0;
        let mut skip_follower: bool = false;
        {
            let _lock = RaftLockGuard::new(&mut server.mtx_);
            if server.state_.peers_.next_index(ord) == 0 {
                rusty::raft_log_warn_2(
                    "[APPEND_ENTRIES] Repairing wrapped next_index for follower {} at leader last index {}",
                    site_id, server.state_.raft_log_.last_index());
                let last: u64 = server.state_.raft_log_.last_index();
                let repaired: u64 = if raft_server_log_index_has_successor(last) {
                    raft_server_follower_next_index(last)
                } else {
                    last
                };
                server.state_.peers_.set_next_index(ord, repaired);
            }
            prev_log_index = server.state_.peers_.next_index(ord) - 1;
            if prev_log_index > server.state_.raft_log_.last_index() {
                rusty::raft_log_info_2(
                    "[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > state_.raft_log_.last_index() ({}), fixing next_index",
                    prev_log_index, server.state_.raft_log_.last_index());
                let last: u64 = server.state_.raft_log_.last_index();
                let repaired: u64 = if raft_server_log_index_has_successor(last) {
                    raft_server_follower_next_index(last)
                } else {
                    last
                };
                server.state_.peers_.set_next_index(ord, repaired);
                prev_log_index = server.state_.peers_.next_index(ord) - 1;
            }
            // Until a payload is selected this is a heartbeat, and proves
            // only the prefix named by prev_log_index.
            sent_end_index = raft_server_append_sent_end(prev_log_index, 0);

            let snapshot_configured: bool = unsafe {
                raft_snapshot_manager_is_set(&server.snapshot_manager_)
            };
            if prev_log_index > server.state_.raft_log_.last_index() {
                rusty::raft_log_info_3(
                    "[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > state_.raft_log_.last_index() ({}), skipping",
                    site_id, prev_log_index,
                    server.state_.raft_log_.last_index());
                server.state_.peers_.set_next_index(ord, 1);
                skip_follower = true;
            } else if server.state_.peers_.next_index(ord)
                < server.state_.raft_log_.base()
                && snapshot_configured
            {
                // The follower is behind the log's base, so send it a
                // snapshot instead of entries it can no longer be given.
                rusty::raft_log_info_4(
                    "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < state_.raft_log_.base()={}, sending InstallSnapshot",
                    server.site_id_, site_id,
                    server.state_.peers_.next_index(ord),
                    server.state_.raft_log_.base());
                let sent: bool = unsafe {
                    raft_phase1_load_and_send_snapshot(
                        server as *mut RaftServerBase, site_id, ord)
                };
                if !sent {
                    rusty::raft_log_warn_2(
                        "[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                        server.site_id_, site_id);
                }
                // Both outcomes skip the normal AppendEntries for this
                // follower: sent, or failed to load.
                skip_follower = true;
            } else {
                unsafe {
                    raft_verify(
                        prev_log_index
                            <= server.state_.raft_log_.last_index());
                }
                if prev_log_index == 0 {
                    prev_log_term = 0;
                } else if prev_log_index == server.state_.snapidx_
                    && server.state_.snapidx_ > 0
                {
                    // Keep using snapshot boundary metadata after compaction.
                    prev_log_term = server.state_.snapterm_ as u64;
                } else {
                    // Was GetRaftInstance, which default-inserted and so
                    // could never return null -- the check below was dead,
                    // and a genuinely missing prevLogIndex silently
                    // fabricated an empty entry with term 0 and sent
                    // prevLogTerm = 0 rather than skipping the follower.
                    let instance = server.state_.raft_log_.get(prev_log_index);
                    if instance.is_none() {
                        rusty::raft_log_error_2(
                            "[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}",
                            prev_log_index, site_id);
                        skip_follower = true;
                    } else {
                        prev_log_term = instance.unwrap().term() as u64;
                    }
                }

                if !skip_follower {
                    skip_follower = heartbeat_phase1_select_payload(
                        server, ord, site_id, prev_log_index, &mut cmd,
                        &mut cmd_log_term, &mut sent_end_index);
                }
            }
        }
        if skip_follower {
            ord += 1;
            continue;
        }

        let is_leader: bool = server.IsLeader();
        let sent_response: rusty::RaftResponsePtr = unsafe {
            raft_phase1_send_append(
                server as *mut RaftServerBase, site_id, partition_id,
                is_leader, round.term(), prev_log_index, prev_log_term,
                round.commit_index(),
                &cmd as *const rusty::RaftCommand, cmd_log_term)
        };

        pending_rpcs.place(ord, PendingAppend::new(
            site_id, round.term(), round.round_id(), sent_end_index,
            sent_response, cmd));
        if round.authority_inserted() && round.is_member(site_id) {
            // Was a std::map iterator created in PHASE 0 and dereferenced
            // here, after the RPC sends. Nothing between the two points
            // mutates authority_rounds, so it was valid -- but a cursor held
            // across a phase boundary and across a synchronous completion
            // callback is the hazard class this file removed for next_index,
            // so look it up by key.
            unsafe {
                raft_verify(
                    authority_rounds.launch(round.round_id(), site_id));
            }
        }
        ord += 1;
    }
}

// ==========================================================================
// PHASE 2 -- poll the replies through one round deadline and apply them
// ==========================================================================

#[repr(C)]
pub struct AppendRespView {
    pub completed_: bool,
    pub status_: bool,
    pub term_: u64,
    pub last_log_index_: u64,
}

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
// `peers` is reached through `consensus` rather than passed alongside it.
// The C++ call site passed `state_` and `state_.peers_` as two arguments --
// two mutable borrows of overlapping state, which only compiled because the
// caller was C++. A Rust caller cannot spell that, and PHASE 2 is a Rust
// caller now.
pub fn heartbeat_apply_append_reply(
    consensus: &mut RaftConsensusState,
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
    if sent.ordinal_ == consensus.peers_.len() {
        return append_reply_nothing(AppendReplyAction::UNKNOWN_FOLLOWER);
    }

    if !reply.status_ {
        let old_next = consensus.peers_.next_index(sent.ordinal_);
        let rung = consensus.peers_
            .back_off_after_reject(sent.ordinal_, reply.last_log_index_);
        let new_next = consensus.peers_.next_index(sent.ordinal_);
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
    consensus.peers_.accept_through(
        sent.ordinal_,
        acknowledged,
        raft_server_log_index_has_successor(acknowledged),
        raft_server_follower_next_index(acknowledged),
    );
    let mut out = append_reply_nothing(AppendReplyAction::ACCEPTED);
    out.acknowledged_ = acknowledged;
    out
}

// ==========================================================================
// PHASE 2: poll responses through one SHORT round deadline and process them.
//
// Formerly RaftServer::HeartbeatPhase2. Never call wait_timeout on an
// individual response: that permanently marks its event TIMEOUT and loses a
// legitimate late persistence reply. Polling also gives every parallel RPC
// the same bounded round budget.
//
// The pieces of HeartbeatRoundState are passed separately rather than the
// struct itself, because that struct is hand-written C++ declared after this
// block; its three members are all DSL types declared in it.
// ==========================================================================
#[allow(clippy::too_many_arguments, clippy::manual_clamp)]
pub fn heartbeat_phase2_body(server: &mut RaftServerBase,
                             pending_rpcs: &mut PendingTable,
                             authority_rounds: &mut AuthorityLedger,
                             round: &HeartbeatRoundScope) {
    const RESPONSE_POLL_STEP_US: u64 = 1000;
    // max(1, min(100000, heartbeat_interval_us_)). Spelled out rather than
    // with clamp: this lowers to C++, where uint64_t has no such member.
    let response_round_timeout_us: u64 =
        if server.heartbeat_interval_us_ > 100000 {
            100000
        } else if server.heartbeat_interval_us_ < 1 {
            1
        } else {
            server.heartbeat_interval_us_
        };
    let response_deadline_us: u64 =
        unsafe { raft_monotonic_now_us() } + response_round_timeout_us;
    let mut stop_response_processing: bool = false;
    let mut retry_released_follower: bool = false;

    while !stop_response_processing {
        let mut waiting_for_current_round: bool = false;
        let mut pending_ord: usize = 0;
        while pending_ord < pending_rpcs.len() {
            if !server.IsLeader() {
                stop_response_processing = true;
                break;
            }
            if !pending_rpcs.occupied(pending_ord) {
                pending_ord += 1;
                continue;
            }

            // Bound once per slot per poll pass, not per use: every read
            // below is the same shape it was when this was a map value.
            let follower_id: u16 = pending_rpcs.follower(pending_ord);
            let sent_term: u64 = pending_rpcs.sent_term(pending_ord);
            let sent_round: u64 = pending_rpcs.sent_round(pending_ord);
            let sent_end_index: u64 = pending_rpcs.sent_end_index(pending_ord);
            let cmd_has_value: bool = unsafe {
                raft_command_has_value(
                    pending_rpcs.cmd(pending_ord) as *const rusty::RaftCommand)
            };
            let resp: AppendRespView = unsafe {
                raft_append_response_read(
                    pending_rpcs.response(pending_ord)
                        as *const rusty::RaftResponsePtr)
            };
            if !resp.completed_ {
                if sent_round == round.round_id() {
                    waiting_for_current_round = true;
                }
                pending_ord += 1;
                continue;
            }

            let mut stepped_down: bool = false;
            {
                let _lock = RaftLockGuard::new(&mut server.mtx_);
                // What the reply MEANS is heartbeat_apply_append_reply. It
                // reads the wire response as three scalars -- the rrr object
                // itself never crosses -- and returns what to do about it.
                let response_available: bool =
                    !(!resp.status_ && resp.term_ == 0
                      && resp.last_log_index_ == 0);
                let resp_ord: usize = server.PeerOrdinal(follower_id);
                let log_last_index: u64 = server.state_.raft_log_.last_index();
                let is_leader: bool = server.IsLeaderLocked();
                let outcome: AppendReplyOutcome = heartbeat_apply_append_reply(
                    &mut server.state_,
                    authority_rounds,
                    &SentAppend::new(follower_id, sent_term, sent_round,
                                     sent_end_index, resp_ord),
                    &AppendReply::new(response_available, resp.status_,
                                      resp.term_, resp.last_log_index_),
                    log_last_index,
                    is_leader);

                let action: AppendReplyAction = outcome.action();
                if action == AppendReplyAction::STEP_DOWN {
                    rusty::raft_log_info_4(
                        "[STEPDOWN] Site {}: AppendEntries response from follower {} carried higher term {} > {}",
                        server.site_id_, follower_id, resp.term_,
                        outcome.previous_term());
                    server.LogTermChange(
                        "AppendEntries response carried newer term",
                        outcome.previous_term(), server.state_.current_term_,
                        follower_id);
                    // stepDown reaches setIsLeader and the election timer, so
                    // it stays here; the decision to take it was made above.
                    server.stepDown();
                    server.state_.req_voting_ = false;
                    server.state_.election_in_progress_ = false;
                    stepped_down = true;
                } else if action == AppendReplyAction::BACKED_OFF {
                    // The five-rung ladder is
                    // FollowerProgress::back_off_after_reject; it reports
                    // which rung it took so the diagnostics stay as specific
                    // as they were when the branches were inline.
                    let rung: BackoffKind = outcome.rung();
                    if rung == BackoffKind::FAST {
                        rusty::raft_log_info_6(
                            "[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next(),
                            outcome.old_next() - outcome.new_next(),
                            resp.last_log_index_);
                    } else if rung == BackoffKind::TERM_CONFLICT {
                        rusty::raft_log_info_4(
                            "[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    } else if rung == BackoffKind::EXPONENTIAL {
                        rusty::raft_log_info_4(
                            "[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    } else if rung == BackoffKind::LINEAR {
                        rusty::raft_log_debug_4(
                            "[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    }
                    // BackoffKind::FLOOR logs nothing, as before.
                } else if action == AppendReplyAction::ACCEPTED {
                    rusty::raft_log_debug_8(
                        "[APPEND_RPC] Leader {} accepted follower {} proof: kind={} reported={} sent_end={} acknowledged={} next={} match={}",
                        server.site_id_, follower_id,
                        if cmd_has_value { "entries" } else { "heartbeat" },
                        resp.last_log_index_, sent_end_index,
                        outcome.acknowledged(),
                        server.state_.peers_.next_index(resp_ord),
                        server.state_.peers_.match_index(resp_ord));
                } else if action == AppendReplyAction::CONTRADICTORY {
                    rusty::raft_log_warn_3(
                        "[APPEND_RPC] Ignoring contradictory success from follower {}: reported_end={} sent_end={}",
                        follower_id, resp.last_log_index_, sent_end_index);
                } else if action == AppendReplyAction::UNKNOWN_FOLLOWER {
                    rusty::raft_log_debug_1(
                        "[APPEND_RPC] Ignoring replication response from removed follower {}",
                        follower_id);
                }
                // AppendReplyAction::IGNORED does nothing, as before.
            }

            let completed_previous_round: bool =
                sent_round != round.round_id();
            pending_rpcs.release(pending_ord);
            retry_released_follower =
                retry_released_follower || completed_previous_round;
            if stepped_down {
                stop_response_processing = true;
                break;
            }
            pending_ord += 1;
        }

        let current_round_has_authority: bool =
            authority_rounds.has_quorum(round.round_id());
        if stop_response_processing || !waiting_for_current_round
            || current_round_has_authority
        {
            break;
        }
        let now_us: u64 = unsafe { raft_monotonic_now_us() };
        if now_us >= response_deadline_us {
            break;
        }
        let remaining_us: u64 = response_deadline_us - now_us;
        let step_us: u64 = if remaining_us < RESPONSE_POLL_STEP_US {
            remaining_us
        } else {
            RESPONSE_POLL_STEP_US
        };
        unsafe {
            raft_fiber_sleep_us(step_us);
        }
    }

    if stop_response_processing {
        pending_rpcs.abandon();
        authority_rounds.abandon();
    } else if retry_released_follower {
        // A completion from an older round opened a per-follower slot after
        // PHASE 1. Prompt another round instead of waiting a full interval.
        server.RequestReplication();
    }
}

// ==========================================================================
// PHASE 3 -- recompute the commit index and publish read-index authority
// ==========================================================================

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

// peers and log are reached through `consensus`, for the same reason
// heartbeat_apply_append_reply's are: the C++ call site passed `state_`,
// `state_.peers_` and `state_.raft_log_` as three arguments, which is one
// mutable borrow overlapping two shared ones. A Rust caller cannot spell it.
pub fn heartbeat_phase3_locked(
    consensus: &mut RaftConsensusState,
    ledger: &mut AuthorityLedger,
    nservers: usize,
    members: &[u16],
    is_leader: bool,
) -> Phase3Outcome {
    let commit = raft_commit_advance(consensus, nservers);
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

pub fn heartbeat_phase3_body(server: &mut RaftServerBase,
                             authority_rounds: &mut AuthorityLedger,
                             round: &HeartbeatRoundScope) {
    if !server.IsLeader() {
        return;
    }
    let mut commit_advanced_after_send: bool = false;
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let members: rusty::Vec<u16> = server.config_members_.clone();
        let nservers: usize = round.nservers();
        let is_leader: bool = server.IsLeaderLocked();
        let outcome: Phase3Outcome = heartbeat_phase3_locked(
            &mut server.state_, authority_rounds, nservers, &members,
            is_leader);

        if outcome.commit().advanced() {
            rusty::raft_log_debug_2(
                "[PHASE3-COMMIT] Advancing state_.commit_index_ {} -> {}",
                outcome.commit().from_index(), outcome.commit().to_index());
            server.EnqueueCommittedEntries(outcome.commit().from_index(),
                                           outcome.commit().to_index());
            commit_advanced_after_send = true;
        }
        if outcome.confirmed() {
            rusty::raft_log_debug_3(
                "[READ-INDEX] site={} confirmed round={} term={}",
                server.site_id_, server.state_.read_quorum_confirmed_round_,
                server.state_.read_quorum_confirmed_term_);
        }
    }

    // The AppendEntries messages for this round carried the OLD commit
    // index. Latch exactly one prompt follow-up round so followers learn the
    // phase-3 commit without waiting out the periodic heartbeat.
    if commit_advanced_after_send {
        server.RequestReplication();
    }
}

// ==========================================================================
// THE ROUND DRIVER
//
// Sequences the four phases above. It is here rather than in server.h --
// where it used to be, beside RaftServerBase -- because a DSL block can only
// call what precedes it, and the phase bodies are in this file. Moving it
// deleted the four `raft_heartbeat_phase*` trampolines and the round-state
// cast they existed to perform.
// ==========================================================================

// Everything one round carries between its phases. This was a hand-written
// C++ struct whose four members were already DSL types -- it existed only
// because the DSL has no default member initialisers, which `fn new` solves.
// Being C++ was not free: the phases could not be called with it, so the
// driver passed it as `*mut c_void` and four kernels cast it back and split
// it into members.
pub struct HeartbeatRoundState {
    // At most one AppendEntries in flight per follower. A synchronous
    // follower may legitimately take longer than one heartbeat interval to
    // persist an entry; retaining its context lets a later round consume that
    // acknowledgement instead of queueing duplicate writes and discarding
    // every late success.
    pending_rpcs_: PendingTable,
    authority_rounds_: AuthorityLedger,
    pending_leader_term_: rusty::Option<u64>,
    // PHASE 0 establishes every field of this each round, so it needs no
    // reset; when PHASE 0 declines the round, phases 1-3 never read it.
    scope_: HeartbeatRoundScope,
}

#[allow(clippy::new_without_default)]
impl HeartbeatRoundState {
    pub fn new() -> HeartbeatRoundState {
        HeartbeatRoundState {
            pending_rpcs_: PendingTable::new(),
            authority_rounds_: AuthorityLedger::new(),
            pending_leader_term_: rusty::None,
            scope_: HeartbeatRoundScope::new(),
        }
    }
}

// The driver OWNS its round state rather than pointing at one the caller
// stack-allocated, which is what the `*mut c_void` handle used to be.
pub struct HeartbeatDriver {
    server_: *mut RaftServerBase,
    round_: HeartbeatRoundState,
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl HeartbeatDriver {
    pub fn new(server: *mut RaftServerBase) -> HeartbeatDriver {
        HeartbeatDriver { server_: server, round_: HeartbeatRoundState::new() }
    }

    // decide -> emit -> collect -> decide, which is the shape the C++ already
    // had as four comment-delimited phases. It is now the shape of the Rust
    // that sequences them, and the phases are called directly.
    pub fn run(&mut self) {
        let server: &mut RaftServerBase = unsafe { &mut *self.server_ };
        server.HeartbeatPrologue();
        while server.HeartbeatLooping() {
            // The wake gate returns false on shutdown rather than on timeout.
            if !server.HeartbeatWait() {
                break;
            }
            // PHASE 0 declines the round when leadership is not held. The C++
            // spelled that `continue`.
            if !heartbeat_phase0_body(server,
                                      &mut self.round_.pending_rpcs_,
                                      &mut self.round_.authority_rounds_,
                                      &mut self.round_.pending_leader_term_,
                                      &mut self.round_.scope_) {
                continue;
            }
            heartbeat_phase1_body(server, &mut self.round_.pending_rpcs_,
                                  &mut self.round_.authority_rounds_,
                                  &self.round_.scope_);
            heartbeat_phase2_body(server, &mut self.round_.pending_rpcs_,
                                  &mut self.round_.authority_rounds_,
                                  &self.round_.scope_);
            heartbeat_phase3_body(server, &mut self.round_.authority_rounds_,
                                  &self.round_.scope_);
        }
        server.HeartbeatEpilogue();
    }
}

// The whole loop, so the C++ side is one fiber spawn rather than a method.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn heartbeat_loop_body(server: *mut RaftServerBase) {
    let mut driver = HeartbeatDriver::new(server);
    driver.run();
}

// ==========================================================================
// INBOUND RequestVote
// ==========================================================================

// OnRequestVote's whole body, as Rust. The caller holds mtx_ for the
// duration, exactly as the C++ did -- the lock stays in C++ because
// RaftCheckedMutex is a C++ type and because moving lock/unlock into a Rust
// body would lose RAII across this function's many early returns.
//
// Two things reach back into unconverted C++ through trampolines, which is
// the same mechanism HeartbeatDriver has used since it landed: doVote, which
// writes the reply and can step the term forward, and
// ElectionLastLogTermLocked, which consults the snapshot boundary. Neither
// is a blocker -- each becomes an ordinary call once its own body converts,
// and the trampoline is deleted then.
//
// The caller also passes `candidate_is_current_voter` rather than this
// reading current_config_: that member is a std::set that has not moved into
// the state struct, and computing the predicate on the C++ side keeps the
// rejection log line's level short-circuit where it belongs.
/// # Safety
///
/// `server` must be a live `RaftServer*`, and the caller must hold that
/// server's `mtx_` for the whole call. Both hold at the only call site,
/// `RaftServer::OnRequestVote`, which takes the lock and passes `this`.
///
/// The handle is not dereferenced here. It is forwarded to the two
/// trampolines below, which cast it back exactly once each.
#[allow(clippy::too_many_arguments)]
// TODO(raft-server-struct): the argument list collapses into &mut self when
// RaftServer is itself a DSL struct; these are its fields, passed separately
// only because the orphan-impl rule forbids `impl RaftServer` today.
pub unsafe fn raft_on_request_vote(
    server: &mut RaftServerBase,
    stopped: bool,
    candidate_is_current_voter: bool,
    lst_log_idx: u64,
    lst_log_term: i64,
    can_id: u16,
    can_term: i64,
    reply_term: &mut i64,
    vote_granted: &mut i8,
) {
    if stopped {
        *reply_term = server.state_.current_term_ as i64;
        *vote_granted = 0;
        return;
    }

    if can_term < 0 || lst_log_term < 0 || !candidate_is_current_voter {
        *reply_term = server.state_.current_term_ as i64;
        *vote_granted = 0;
        return;
    }

    let cur_term = server.state_.current_term_;
    // UNSIGNED, deliberately. The C++ this replaces was
    // `if (can_term < cur_term)` with can_term an int64_t and cur_term a
    // uint64_t, and C++'s usual arithmetic conversions make that an UNSIGNED
    // comparison. Writing it as `can_term < cur_term as i64` instead -- the
    // obvious-looking translation -- is a different function: once
    // current_term_ passes INT64_MAX the cast goes negative, a non-negative
    // can_term is never below it, this rejection is skipped, and the
    // fall-through can GRANT a vote to a candidate whose term is far below
    // ours. current_term_ is a u64 taken straight off the wire with no
    // clamp, so that state is reachable from a peer.
    //
    // can_term >= 0 is already guaranteed above, so the cast to u64 is the
    // faithful spelling.
    if (can_term as u64) < cur_term {
        unsafe {
            raft_do_vote(server as *mut RaftServerBase, lst_log_idx, lst_log_term, can_id, can_term,
                         reply_term, vote_granted, false)
        };
        return;
    }

    // Already voted for someone ELSE this term. Raft allows re-granting to
    // the same candidate, which is why the identity is compared and not just
    // the presence of a vote.
    // u64 here too, for the same reason and so the two comparisons cannot
    // drift apart. Equality happens to be unaffected by the signedness, but
    // relying on that is how the bug above got written.
    if (can_term as u64) == cur_term
        && server.state_.vote_for_ != RAFT_SERVER_INVALID_SITE_ID
        && server.state_.vote_for_ != can_id
    {
        unsafe {
            raft_do_vote(server as *mut RaftServerBase, lst_log_idx, lst_log_term, can_id, can_term,
                         reply_term, vote_granted, false)
        };
        return;
    }

    // Every grant, including an idempotent retry, must still carry an
    // up-to-date candidate log. Defensive against damaged or legacy
    // persistent state, and the RequestVote rule in its direct form.
    if server.state_.raft_log_.last_index() < server.state_.snapidx_ {
        panic!("last log index is below the snapshot boundary");
    }
    let lstoff = server.state_.raft_log_.last_index() - server.state_.snapidx_;
    let curlstterm = server.ElectionLastLogTermLocked();
    let curlstidx = server.state_.raft_log_.last_index();
    let candidate_log_is_current = raft_server_candidate_log_is_at_least(
        lst_log_term, curlstterm, lst_log_idx, curlstidx);

    if raft_server_vote_is_idempotent(can_term as u64, cur_term,
                                      server.state_.vote_for_, can_id)
        && candidate_log_is_current
    {
        unsafe {
            raft_do_vote(server as *mut RaftServerBase, lst_log_idx, lst_log_term, can_id, can_term,
                         reply_term, vote_granted, true)
        };
        return;
    }

    // Snapshot-aware offset invariant.
    if lstoff + server.state_.snapidx_ != server.state_.raft_log_.last_index() {
        panic!("snapshot offset invariant violated");
    }

    let grant = candidate_log_is_current;
    unsafe {
        raft_do_vote(server as *mut RaftServerBase, lst_log_idx, lst_log_term, can_id, can_term,
                     reply_term, vote_granted, grant)
    };
}

// ==========================================================================
// The two inbound RPC bodies, formerly RaftServer::OnRequestVote and
// RaftServer::OnAppendEntries. Both keep a one-line C++ entry point, because
// the rrr service layer calls them by name on RaftServer.
// ==========================================================================
#[allow(clippy::too_many_arguments)]
pub fn on_request_vote_body(server: &mut RaftServerBase, lst_log_idx: u64,
                            lst_log_term: i64, can_id: u16, can_term: i64,
                            reply_term: &mut i64, vote_granted: &mut i8) {
    let _lock = RaftLockGuard::new(&mut server.mtx_);
    rusty::raft_log_debug_1("raft receives vote from candidate: {:x}", can_id);

    let stopped: bool =
        server.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    if stopped {
        rusty::raft_log_debug_2(
            "[RAFT-SHUTDOWN] Site {} rejecting RequestVote from {}",
            server.site_id_, can_id);
    }

    let candidate_is_current_voter: bool =
        can_id != RAFT_SERVER_INVALID_SITE_ID && can_id != server.site_id_
            && server.IsConfigMember(can_id);
    if !stopped
        && (can_term < 0 || lst_log_term < 0 || !candidate_is_current_voter)
    {
        rusty::raft_log_warn_5(
            "[RAFT_VOTE] Site {} rejected malformed/non-voter candidate {} term {} last_log_term {} (voter={})",
            server.site_id_, can_id, can_term, lst_log_term,
            candidate_is_current_voter);
    }

    unsafe {
        raft_on_request_vote(server, stopped, candidate_is_current_voter,
                             lst_log_idx, lst_log_term, can_id, can_term,
                             reply_term, vote_granted);
    }
}

// ==========================================================================
// INBOUND AppendEntries
// ==========================================================================

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
// OnAppendEntries' body, as Rust. The caller holds mtx_ throughout.
//
// The wire payload never crosses. C++ decodes it once -- it is the only side
// that can, since janus::Command is opaque here -- and hands over three
// scalars plus the incoming entries' TERMS. Rust makes every protocol
// decision from those, and when it decides to append it calls back through
// raft_ae_apply_incoming, which builds the entries C++-side. That ordering
// keeps the original's laziness: entries are constructed only for a payload
// that is actually being written, not for one about to be rejected, which
// matters because backtracking rejects are common during log repair.
//
// AppendReport is diagnostics only. The caller acts on nothing in it; it
// exists so the two rejection log lines can keep their level short-circuit
// and still name which check failed.
#[repr(C)]
pub struct AppendReport {
    accepted_: bool,
    term_ok_: bool,
    index_ok_: bool,
    prev_term_ok_: bool,
    refused_committed_conflict_: bool,
    unauthoritative_: bool,
    conflict_index_: u64,
    local_prev_term_: u64,
}

impl AppendReport {
    pub fn accepted(&self) -> bool {
        self.accepted_
    }

    pub fn term_ok(&self) -> bool {
        self.term_ok_
    }

    pub fn index_ok(&self) -> bool {
        self.index_ok_
    }

    pub fn prev_term_ok(&self) -> bool {
        self.prev_term_ok_
    }

    // The append was refused because it would rewrite an entry at or below
    // commit_index_/execute_index_. A legitimate leader never does this.
    pub fn refused_committed_conflict(&self) -> bool {
        self.refused_committed_conflict_
    }

    // Rejected at the authoritative-sender gate, before term_ok, index_ok or
    // prev_term_ok were ever evaluated. The caller needs this to pick the
    // right log line: reporting those three as "failed" when they were never
    // computed is a lie, and it hides a distinct failure mode behind the
    // generic one.
    pub fn unauthoritative(&self) -> bool {
        self.unauthoritative_
    }

    pub fn conflict_index(&self) -> u64 {
        self.conflict_index_
    }

    pub fn local_prev_term(&self) -> u64 {
        self.local_prev_term_
    }
}

/// # Safety
///
/// `server` must be a live `RaftServer*` and `cmd` a live `janus::Command*`
/// that outlives the call, and the caller must hold that server's `mtx_`
/// throughout. All three hold at the only call site,
/// `RaftServer::OnAppendEntries`. Neither handle is dereferenced here; both
/// are forwarded to trampolines that cast back exactly once.
// unnecessary_unwrap: the per-slot lookup below uses is_some()/unwrap() with
// an explicit `&RaftEntry` binding rather than `if let`. `if let` is the
// better Rust and it transpiles, but the emitter renders the binding with a
// dot where the C++ needs an arrow, and an inferred binding COPIES the entry.
#[allow(clippy::too_many_arguments, clippy::unnecessary_unwrap)]
// TODO(raft-server-struct): collapses into &mut self once RaftServer is a
// DSL struct; these are its fields and its RPC arguments.
pub unsafe fn raft_on_append_entries(
    server: &mut RaftServerBase,
    cmd: *const core::ffi::c_void,
    stopped: bool,
    sender_is_current_voter: bool,
    has_cmd: bool,
    leader_current_term: u64,
    leader_site_id: u16,
    leader_prev_log_index: u64,
    leader_prev_log_term: u64,
    leader_commit_index: u64,
    leader_next_log_term: u64,
    follower_append_ok: &mut u64,
    follower_current_term: &mut u64,
    follower_last_log_index: &mut u64,
) -> AppendReport {
    let mut report = AppendReport {
        accepted_: false,
        term_ok_: false,
        index_ok_: false,
        prev_term_ok_: false,
        refused_committed_conflict_: false,
        unauthoritative_: false,
        conflict_index_: 0,
        local_prev_term_: 0,
    };

    if stopped {
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    let leader_has_higher_term =
        raft_server_observed_higher_term(leader_current_term, server.state_.current_term_);
    let leader_term_is_stale =
        raft_server_vote_term_is_stale(leader_current_term, server.state_.current_term_);
    let sender_is_self = leader_site_id == server.state_.site_id_;
    let has_known_leader = server.state_.current_leader_id_ != RAFT_SERVER_INVALID_SITE_ID;
    let known_leader_matches_sender = server.state_.current_leader_id_ == leader_site_id;
    if !sender_is_current_voter
        || leader_term_is_stale
        || !raft_server_leader_rpc_sender_is_authoritative(
            leader_has_higher_term,
            server.state_.is_leader_,
            sender_is_self,
            has_known_leader,
            known_leader_matches_sender,
        )
    {
        report.unauthoritative_ = true;
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    // Decode the wire payload HERE, not before the gates. The original did
    // the marshallable_cast and the count validation at exactly this point,
    // after a stopped server and an unauthoritative sender had already
    // returned. Hoisting it above them would make every rejected
    // AppendEntries pay a dynamic cast and N refcount bumps, on a path a
    // remote peer drives -- and backtracking rejects are common during log
    // repair.
    // The kernel fills server.decoded_terms_, one term per encoded entry, so
    // its length IS the decoded count -- which is why the out-param it used
    // to report that count through is gone with it.
    let append_payload_valid = unsafe {
        raft_ae_decode_payload(server as *mut RaftServerBase, cmd,
                               leader_prev_log_index, leader_next_log_term)
    };
    let decoded_count: u64 = server.decoded_terms_.len() as u64;

    let term_ok =
        raft_server_append_term_is_acceptable(leader_current_term, server.state_.current_term_);
    let compacted_prefix_miss = leader_prev_log_index != 0
        && leader_prev_log_index < server.state_.raft_log_.base()
        && leader_prev_log_index != server.state_.snapidx_;
    let index_ok =
        leader_prev_log_index <= server.state_.raft_log_.last_index() && !compacted_prefix_miss;

    // THE LOG-MATCHING CHECK. A follower legitimately may not hold
    // leaderPrevLogIndex -- discovering that is the point, and what drives
    // the leader's backtracking. An absent entry falls through to term 0 and
    // the mismatch is reported rather than manufactured.
    let mut local_prev_term: u64 = 0;
    if leader_prev_log_index == 0 {
        local_prev_term = 0;
    } else if leader_prev_log_index == server.state_.snapidx_ {
        // The snapshot boundary is still valid when entries are compacted.
        local_prev_term = server.state_.snapterm_ as u64;
    } else if leader_prev_log_index <= server.state_.raft_log_.last_index()
        && !compacted_prefix_miss
        && server.state_.raft_log_.holds(leader_prev_log_index)
    {
        local_prev_term =
            server.state_.raft_log_.get(leader_prev_log_index).unwrap().term() as u64;
    }
    let prev_term_ok = leader_prev_log_index == 0 || local_prev_term == leader_prev_log_term;

    report.term_ok_ = term_ok;
    report.index_ok_ = index_ok;
    report.prev_term_ok_ = prev_term_ok;
    report.local_prev_term_ = local_prev_term;

    // Reset the timer for any current-term leader, even when the log
    // conflicts, so a follower being repaired by backtracking does not keep
    // starting elections.
    if term_ok {
        if raft_server_observed_higher_term(leader_current_term, server.state_.current_term_) {
            let prev_term = server.state_.current_term_;
            server.state_.current_term_ = leader_current_term;
            server.state_.vote_for_ = RAFT_SERVER_INVALID_SITE_ID;
            // Publish the accepted leader before any leader-change callback
            // can observe the follower transition.
            server.state_.current_leader_id_ = raft_server_leader_hint_after_transition(
                false, true, server.state_.site_id_, leader_site_id);
            let now_term: u64 = server.state_.current_term_;
            server.LogTermChange("AppendEntries leader term is newer",
                                 prev_term, now_term, leader_site_id);
            if server.state_.is_leader_ {
                // The central transition, so no leadership state survives an
                // accepted competing leader epoch.
                server.stepDown();
            } else {
                server.setIsLeader(false);
            }
            server.state_.req_voting_ = false;
            server.state_.election_in_progress_ = false;
        }
        // Refresh the hint for current-term contact too; a higher-term sender
        // was already published above, before its role transition.
        server.state_.current_leader_id_ = raft_server_leader_hint_after_transition(
            false, true, server.state_.site_id_, leader_site_id);
        server.resetTimerLocked("AppendEntries from current-term leader");
    }

    if !(raft_server_append_is_acceptable(term_ok, index_ok, prev_term_ok)
         && append_payload_valid)
    {
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    // Any accepted leader RPC establishes follower state even in our current
    // term. Cancel an outstanding election before its delayed result can
    // promote this server after the accepted AppendEntries.
    if server.state_.is_leader_ {
        server.stepDown();
    } else {
        server.setIsLeader(false);
    }
    server.state_.req_voting_ = false;
    server.state_.election_in_progress_ = false;

    let old_last_log_index = server.state_.raft_log_.last_index();
    let count = if has_cmd { decoded_count } else { 0 };
    let accepted_through = raft_server_append_sent_end(leader_prev_log_index, count);

    // Raft's conflict rule is deliberately narrower than "replace through the
    // RPC end". Concurrent RPCs can complete out of order: if an older
    // payload is already identical through its end, the follower must keep
    // any newer suffix it has since accepted. Only the first missing or
    // term-conflicting slot starts an overwrite.
    let mut have_first_write = false;
    let mut truncate_suffix = false;
    let mut first_write_index: u64 = 0;
    let mut i: u64 = 0;
    while i < decoded_count {
        let index = leader_prev_log_index + i + 1;
        // ONE lookup per entry, as the original had. The lookup itself is
        // Rust -- the log is a Rust type -- and only "does this slot hold a
        // payload" crosses, because janus::Command is opaque here.
        let mut local_exists: bool = false;
        let mut local_term: i64 = 0;
        let slot = server.state_.raft_log_.get(index);
        if slot.is_some() {
            let entry: &RaftEntry = slot.unwrap();
            local_exists = unsafe {
                raft_command_has_value(entry.cmd() as *const rusty::RaftCommand)
            };
            if local_exists {
                local_term = entry.term();
            }
        }
        // In bounds by construction: decoded_count IS decoded_terms_.len(),
        // the loop condition is i < decoded_count, and nothing in the body
        // touches the vector. The kernel this replaces needed a verify only
        // because i arrived across the language boundary.
        let incoming_term: i64 = server.decoded_terms_[i as usize];
        if raft_server_append_entry_conflicts(local_exists, local_term as u64,
                                              incoming_term as u64) {
            have_first_write = true;
            first_write_index = index;
            truncate_suffix = index <= old_last_log_index;
            break;
        }
        i += 1;
    }

    if truncate_suffix
        && first_write_index <= (if server.state_.commit_index_ > server.state_.execute_index_ {
               server.state_.commit_index_
           } else {
               server.state_.execute_index_
           })
    {
        // A legitimate leader never conflicts with a committed entry. Do not
        // let malformed or internally inconsistent input rewrite applied
        // state; reject before memory changes.
        report.refused_committed_conflict_ = true;
        report.conflict_index_ = first_write_index;
        *follower_append_ok = 0;
        *follower_current_term = server.state_.current_term_;
        *follower_last_log_index = server.state_.raft_log_.last_index();
        return report;
    }

    if have_first_write {
        // Two operations that cannot leave a hole: drop the divergent suffix,
        // then re-append in index order. truncate_from is a no-op when
        // first_write_index is already past the tail, the ordinary extend
        // case. The append itself is C++ because it needs the wire payload.
        server.state_.raft_log_.truncate_from(first_write_index);
        unsafe {
            raft_ae_apply_incoming(server as *mut RaftServerBase, cmd,
                                   leader_prev_log_index,
                                   leader_next_log_term, first_write_index)
        };
    }
    if server.state_.raft_log_.last_index()
        != raft_server_append_result_last_index(old_last_log_index, accepted_through,
                                                truncate_suffix)
    {
        panic!("append left the log tail somewhere the result rule did not predict");
    }

    let follower_commit_candidate =
        raft_server_commit_index_clamp(leader_commit_index, accepted_through);
    if raft_server_log_index_above(follower_commit_candidate, server.state_.commit_index_) {
        let old_commit = server.state_.commit_index_;
        server.state_.commit_index_ = follower_commit_candidate;
        if server.state_.raft_log_.last_index() < server.state_.commit_index_ {
            panic!("commit index advanced past the log tail");
        }
        let new_commit: u64 = server.state_.commit_index_;
        server.EnqueueCommittedEntries(old_commit, new_commit);
    }

    *follower_append_ok = 1;
    *follower_current_term = server.state_.current_term_;
    // The inclusive end PROVED by this call, not the follower's possibly
    // longer and divergent local suffix. Rejections above report the local
    // tail instead, as a backoff hint.
    *follower_last_log_index = accepted_through;
    report.accepted_ = true;
    report
}

// `cmd` is an opaque handle to the caller's janus::Command; it is passed
// straight through to raft_on_append_entries, never dereferenced here.
#[allow(clippy::too_many_arguments, clippy::not_unsafe_ptr_arg_deref)]
pub fn on_append_entries_body(server: &mut RaftServerBase,
                              leader_current_term: u64, leader_site_id: u16,
                              leader_prev_log_index: u64,
                              leader_prev_log_term: u64,
                              leader_commit_index: u64,
                              cmd: *const core::ffi::c_void,
                              cmd_has_value: bool,
                              leader_next_log_term: u64,
                              follower_append_ok: &mut u64,
                              follower_current_term: &mut u64,
                              follower_last_log_index: &mut u64) {
    let _lock = RaftLockGuard::new(&mut server.mtx_);

    let stopped: bool =
        server.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    let sender_is_current_voter: bool =
        leader_site_id != RAFT_SERVER_INVALID_SITE_ID
            && leader_site_id != server.site_id_
            && server.IsConfigMember(leader_site_id);

    // NO DECODE HERE. raft_on_append_entries calls raft_ae_decode_payload
    // itself, after the stopped check and the authoritative gate -- which is
    // where the original did this work. Decoding up front would make every
    // rejected AppendEntries pay a dynamic cast and N refcount bumps on a
    // path a remote peer drives.
    let report: AppendReport = unsafe {
        raft_on_append_entries(
            server, cmd, stopped, sender_is_current_voter,
            cmd_has_value, leader_current_term, leader_site_id,
            leader_prev_log_index, leader_prev_log_term, leader_commit_index,
            leader_next_log_term, follower_append_ok, follower_current_term,
            follower_last_log_index)
    };

    if !stopped && !report.accepted() {
        if report.refused_committed_conflict() {
            // A legitimate leader can never conflict with a committed entry.
            rusty::raft_log_error_4(
                "[APPEND_REJECT] Site {} refusing conflict at committed index {} (commit_index={}, execute_index={})",
                server.site_id_, report.conflict_index(),
                server.state_.commit_index_, server.state_.execute_index_);
        } else if report.unauthoritative() {
            // Dispatch on WHICH gate rejected, not on
            // sender_is_current_voter. A stale-term or non-authoritative
            // sender that IS a current voter would otherwise land in the
            // branch below and be reported with term_ok/index_ok/
            // prev_term_ok all "failed" -- three checks never evaluated on
            // that path.
            rusty::raft_log_warn_6(
                "[APPEND_REJECT] Site {} rejecting unauthoritative AppendEntries sender {} term {} (local_term={} leader={} voter={})",
                server.site_id_, leader_site_id, leader_current_term,
                server.state_.current_term_,
                server.state_.current_leader_id_, sender_is_current_voter);
        } else {
            rusty::raft_log_info_10(
                "[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})",
                server.site_id_, leader_site_id, report.term_ok(),
                report.index_ok(), report.prev_term_ok(), leader_current_term,
                server.state_.current_term_, leader_prev_log_index,
                server.state_.raft_log_.last_index(),
                report.local_prev_term());
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_server.heartbeat_round_scope version=1 rust_sha256=84747caecf64528ab3ebc6977d307fe8745e3d3b59033de19f94bdb0968dc699*/
enum class AppendReplyAction : int32_t;
constexpr AppendReplyAction AppendReplyAction_IGNORED();
constexpr AppendReplyAction AppendReplyAction_STEP_DOWN();
constexpr AppendReplyAction AppendReplyAction_BACKED_OFF();
constexpr AppendReplyAction AppendReplyAction_ACCEPTED();
constexpr AppendReplyAction AppendReplyAction_CONTRADICTORY();
constexpr AppendReplyAction AppendReplyAction_UNKNOWN_FOLLOWER();
struct HeartbeatRoundScope;
struct CommitAdvance;
struct Phase0Outcome;
struct AppendRespView;
struct SentAppend;
struct AppendReply;
struct AppendReplyOutcome;
struct Phase3Outcome;
struct HeartbeatRoundState;
struct HeartbeatDriver;
struct AppendReport;
bool heartbeat_round_saturated(uint64_t round_counter);
AppendReplyOutcome append_reply_nothing(AppendReplyAction action);

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

using ::server_h::RaftServerBase;

using ::server_h::RaftEntry;

using ::server_h::RaftLockGuard;

extern "C" {
    uint64_t raft_monotonic_now_us();
    void raft_fiber_sleep_us(uint64_t micros);
    AppendRespView raft_append_response_read(const rusty::RaftResponsePtr* response);
    bool raft_command_has_value(const rusty::RaftCommand* cmd);
    void raft_verify(bool condition);
    bool raft_snapshot_manager_is_set(const rusty::RaftSnapshotManagerPtr* manager);
    bool raft_phase1_load_and_send_snapshot(server_h::RaftServerBase* server, uint16_t site_id, size_t ord);
    bool raft_batch_optimization_enabled();
    uint64_t raft_append_entries_batch_max();
    int32_t raft_command_kind(const rusty::RaftCommand* cmd);
    bool raft_batch_try_push(server_h::RaftServerBase* server, uint64_t index);
    void raft_batch_finalize(server_h::RaftServerBase* server, rusty::RaftCommand* cmd_out);
    rusty::RaftResponsePtr raft_phase1_send_append(server_h::RaftServerBase* server, uint16_t site_id, uint32_t partition_id, bool is_leader, uint64_t term, uint64_t prev_log_index, uint64_t prev_log_term, uint64_t commit_index, const rusty::RaftCommand* cmd, uint64_t cmd_log_term);
    bool raft_ae_decode_payload(server_h::RaftServerBase* server, const rusty::ffi::c_void* cmd, uint64_t leader_prev_log_index, uint64_t leader_next_log_term);
    void raft_ae_apply_incoming(server_h::RaftServerBase* server, const rusty::ffi::c_void* cmd, uint64_t leader_prev_log_index, uint64_t leader_next_log_term, uint64_t first_write_index);
    void raft_do_vote(server_h::RaftServerBase* server, uint64_t lst_log_idx, int64_t lst_log_term, uint16_t can_id, int64_t can_term, int64_t& reply_term, int8_t& vote_granted, bool vote);
}

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

struct AppendRespView {
    bool completed_;
    bool status_;
    uint64_t term_;
    uint64_t last_log_index_;
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

struct Phase3Outcome {
    CommitAdvance commit_;
    bool confirmed_;

    const CommitAdvance& commit() const;
    bool confirmed() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

struct HeartbeatRoundState {
    PendingTable pending_rpcs_;
    AuthorityLedger authority_rounds_;
    rusty::Option<uint64_t> pending_leader_term_;
    HeartbeatRoundScope scope_;

    static HeartbeatRoundState new_();
};

struct HeartbeatDriver {
    server_h::RaftServerBase* server_;
    HeartbeatRoundState round_;

    static HeartbeatDriver new_(server_h::RaftServerBase* server);
    void run();
};

struct AppendReport {
    bool accepted_;
    bool term_ok_;
    bool index_ok_;
    bool prev_term_ok_;
    bool refused_committed_conflict_;
    bool unauthoritative_;
    uint64_t conflict_index_;
    uint64_t local_prev_term_;

    bool accepted() const;
    bool term_ok() const;
    bool index_ok() const;
    bool prev_term_ok() const;
    bool refused_committed_conflict() const;
    bool unauthoritative() const;
    uint64_t conflict_index() const;
    uint64_t local_prev_term() const;
    // Rust derives Send/Sync from the field types; C++ cannot see them.
    static constexpr bool is_send = true;
    static constexpr bool is_sync = true;
};

CommitAdvance raft_commit_advance(RaftConsensusState& consensus, size_t nservers) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    if (rusty::len((*consensus_shadow1).peers_) != (rusty::detail::deref_if_pointer_like(nservers) - 1)) {
        rusty::panic::do_panic(std::format("peer table and round membership disagree"));
    }
    auto candidate_index = (*consensus_shadow1).peers_.majority_match_index(std::move(nservers), (*consensus_shadow1).raft_log_.last_index());
    if (rusty::detail::rust_not(raft_server_log_index_above(std::move(candidate_index), (*consensus_shadow1).commit_index_))) {
        return CommitAdvance{.advanced_ = false, .from_ = static_cast<uint64_t>(0), .to_ = static_cast<uint64_t>(0)};
    }
    auto candidate = (*consensus_shadow1).raft_log_.get(std::move(candidate_index));
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

Phase0Outcome heartbeat_phase0_locked(RaftConsensusState& consensus, HeartbeatRoundScope& round, PendingTable& pending, AuthorityLedger& ledger, rusty::Option<uint64_t>& pending_leader_term, std::span<const uint16_t> members, uint16_t site_id, bool is_leader) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    rusty::Option<uint64_t>* pending_leader_term_shadow1 = &pending_leader_term;
    if (!is_leader) {
        pending.abandon();
        ledger.abandon();
        *pending_leader_term_shadow1 = rusty::None;
        return Phase0Outcome{.restart_ = true, .commit_advanced_ = false, .commit_from_ = static_cast<uint64_t>(0), .commit_to_ = static_cast<uint64_t>(0)};
    }
    round.begin((*consensus_shadow1).current_term_, (*consensus_shadow1).heartbeat_round_);
    if (rusty::len(pending) != rusty::len((*consensus_shadow1).peers_)) {
        pending.resize(rusty::len((*consensus_shadow1).peers_));
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
    const auto advance = raft_commit_advance((*consensus_shadow1), round.nservers());
    round.publish_commit_index((*consensus_shadow1).commit_index_);
    return Phase0Outcome{.restart_ = false, .commit_advanced_ = advance.advanced(), .commit_from_ = advance.from_index(), .commit_to_ = advance.to_index()};
}

bool heartbeat_round_saturated(uint64_t round_counter) {
    return rusty::detail::rust_not(raft_server_read_index_round_can_advance(std::move(round_counter)));
}

bool heartbeat_phase0_body(server_h::RaftServerBase& server, PendingTable& pending_rpcs, AuthorityLedger& authority_rounds, rusty::Option<uint64_t>& pending_leader_term, HeartbeatRoundScope& round) {
    {
        const auto _lock = RaftLockGuard::new_(&server.mtx_);
        bool leader = server.IsLeaderLocked();
        if (rusty::detail::deref_if_pointer_like(leader) && heartbeat_round_saturated([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.heartbeat_round_); }) { return (__r.heartbeat_round_); } else if constexpr (requires { (__r.heartbeat_round__field); }) { return (__r.heartbeat_round__field); } else if constexpr (requires { ((*__r).heartbeat_round_); }) { return ((*__r).heartbeat_round_); } else { return ((*__r).heartbeat_round__field); } }(server.state_))) {
            rusty::raft_log_error_2("[READ-INDEX] site={} heartbeat round saturated in term {}", server.site_id_, [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }(server.state_));
        }
        if (leader) {
            size_t ord = static_cast<size_t>(0);
            while (rusty::detail::deref_if_pointer_like(ord) < rusty::len([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_))) {
                rusty::raft_log_debug_2("[COMMIT-CALC] match_index_[{}] = {}", server.peer_site_at(std::move(ord)), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).match_index(std::move(ord)));
                ord += 1;
            }
        }
        uint16_t site_id = server.site_id_;
        rusty::Vec<uint16_t> members = rusty::clone(server.config_members_);
        const Phase0Outcome outcome = heartbeat_phase0_locked(rusty::detail::deref_if_pointer_like(server.state_), round, pending_rpcs, authority_rounds, pending_leader_term, std::span<const uint16_t>(rusty::as_slice(members)), std::move(site_id), std::move(leader));
        if (outcome.restart()) {
            return true;
        }
        if (outcome.commit_advanced()) {
            server.EnqueueCommittedEntries(outcome.commit_from(), outcome.commit_to());
        }
    }
    rusty::Vec<uint16_t> members = rusty::clone(server.config_members_);
    bool opened = authority_rounds.open(round.round_id(), members, HeartbeatAuthority::new_(round.term(), round.nservers(), server.site_id_));
    round.set_authority_inserted(std::move(opened));
    if (!round.authority_inserted()) {
        // @unsafe
        {
            raft_verify(round.round_id() == rusty::detail::deref_if_pointer_like(std::numeric_limits<uint64_t>::max()));
        }
    }
    return true;
}

bool heartbeat_phase1_select_payload(server_h::RaftServerBase& server, size_t ord, uint16_t site_id, uint64_t prev_log_index, rusty::RaftCommand& cmd, uint64_t& cmd_log_term, uint64_t& sent_end_index) {
    rusty::RaftCommand* cmd_shadow1 = &cmd;
    uint64_t* cmd_log_term_shadow1 = &cmd_log_term;
    uint64_t* sent_end_index_shadow1 = &sent_end_index;
    bool skip_follower = false;
    if (!raft_batch_optimization_enabled()) {
        rusty::raft_log_debug_5("[BATCH_CHECK] site={} follower={} next_index={} state_.raft_log_.base()={} state_.raft_log_.last_index()={}", server.site_id_, std::move(site_id), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).base(), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index());
        if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)) <= [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index()) {
            if (rusty::detail::rust_not(raft_server_append_entry_count_fits(std::move(prev_log_index), 1))) {
                rusty::raft_log_error_2("[HEARTBEAT-SEND] Log index exhausted after {}, skipping follower {}", std::move(prev_log_index), std::move(site_id));
                skip_follower = true;
            } else {
                const uint64_t next = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord));
                auto slot = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).get(std::move(next));
                const bool usable = slot.is_some() && raft_command_has_value(rusty::detail::ptr_cast<const rusty::RaftCommand*>(slot.unwrap().cmd()));
                if (!usable) {
                    rusty::raft_log_error_2("[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}", std::move(next), std::move(site_id));
                    skip_follower = true;
                } else {
                    const server_h::RaftEntry& entry = slot.unwrap();
                    *cmd_log_term_shadow1 = static_cast<uint64_t>(entry.term());
                    *cmd_shadow1 = rusty::clone(entry.cmd());
                    *sent_end_index_shadow1 = raft_server_append_sent_end(std::move(prev_log_index), 1);
                    const int32_t kind = raft_command_kind(rusty::detail::ptr_cast<const rusty::RaftCommand*>(entry.cmd()));
                    rusty::raft_log_debug_4("[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}", server.site_id_, std::move(next), std::move(site_id), std::move(kind));
                }
            }
        }
        return std::move(skip_follower);
    }
    server.batch_buffer_.clear();
    const uint64_t max_batch_entries = raft_append_entries_batch_max();
    uint64_t batch_start_idx = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord));
    rusty::raft_log_debug_5("[BATCH_CHECK] site={} follower={} next_index={} state_.raft_log_.base()={} state_.raft_log_.last_index()={}", server.site_id_, std::move(site_id), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).base(), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index());
    if (rusty::detail::rust_not(raft_server_append_entry_count_fits(std::move(prev_log_index), 1))) {
        rusty::raft_log_error_2("[HEARTBEAT-BATCH] Log index exhausted after {}, skipping follower {}", std::move(prev_log_index), std::move(site_id));
        skip_follower = true;
    }
    const uint64_t first_encoded_index = (skip_follower ? static_cast<uint64_t>(0) : raft_server_append_sent_end(std::move(prev_log_index), 1));
    if (!skip_follower && (((rusty::detail::deref_if_pointer_like(batch_start_idx) != rusty::detail::deref_if_pointer_like(first_encoded_index)) || (rusty::detail::deref_if_pointer_like(batch_start_idx) < [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).base())))) {
        rusty::raft_log_error_4("[HEARTBEAT-BATCH] Non-contiguous source for follower {}: prev={} start={} min_active={}; refusing to compress a hole", std::move(site_id), std::move(prev_log_index), std::move(batch_start_idx), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).base());
        skip_follower = true;
    } else if (!skip_follower) {
        uint64_t idx = batch_start_idx;
        while ((rusty::detail::deref_if_pointer_like(idx) <= [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index()) && (((static_cast<uint64_t>(rusty::len(server.batch_buffer_)))) < rusty::detail::deref_if_pointer_like(max_batch_entries))) {
            auto entry = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).get(std::move(idx));
            const bool usable = entry.is_some() && raft_command_has_value(rusty::detail::ptr_cast<const rusty::RaftCommand*>(entry.unwrap().cmd()));
            if (!usable) {
                rusty::raft_log_error_2("[HEARTBEAT-BATCH] Missing log entry {} for follower {}; refusing to compress a hole", std::move(idx), std::move(site_id));
                skip_follower = true;
                break;
            }
            const int64_t entry_term = entry.unwrap().term();
            if (!raft_batch_try_push(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(idx))) {
                auto slot = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).get(std::move(idx));
                const int32_t kind = raft_command_kind(rusty::detail::ptr_cast<const rusty::RaftCommand*>(slot.unwrap().cmd()));
                const uint64_t batched = static_cast<uint64_t>(rusty::len(server.batch_buffer_));
                if (rusty::detail::deref_if_pointer_like(batched) == static_cast<uint64_t>(0)) {
                    rusty::raft_log_info_3("[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log", server.site_id_, std::move(idx), std::move(kind));
                    *cmd_shadow1 = rusty::clone(slot.unwrap().cmd());
                    *cmd_log_term_shadow1 = static_cast<uint64_t>(entry_term);
                    *sent_end_index_shadow1 = raft_server_append_sent_end(std::move(prev_log_index), 1);
                } else {
                    rusty::raft_log_info_3("[BATCH_STOP] site={} idx={}: ending batch before non-TpcCommitCommand kind={}", server.site_id_, std::move(idx), std::move(kind));
                }
                break;
            }
            if (rusty::detail::rust_not(raft_server_log_index_has_successor(std::move(idx)))) {
                break;
            }
            idx += 1;
        }
    }
    const uint64_t encoded_entry_count = static_cast<uint64_t>(rusty::len(server.batch_buffer_));
    if ((!skip_follower && (rusty::detail::deref_if_pointer_like(encoded_entry_count) > 0)) && rusty::detail::rust_not(raft_server_append_batch_count_is_valid(std::move(prev_log_index), std::move(encoded_entry_count)))) {
        rusty::raft_log_error_3("[HEARTBEAT-BATCH] Invalid encoded count {} after previous index {}; skipping follower {}", std::move(encoded_entry_count), std::move(prev_log_index), std::move(site_id));
        skip_follower = true;
    }
    if (!skip_follower && (rusty::detail::deref_if_pointer_like(encoded_entry_count) > 0)) {
        // @unsafe
        {
            raft_batch_finalize(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), static_cast<rusty::RaftCommand*>(rusty::detail::ptr_or_addr((*cmd_shadow1))));
        }
        *sent_end_index_shadow1 = raft_server_append_sent_end(std::move(prev_log_index), std::move(encoded_entry_count));
        const uint64_t batch_end_idx = *sent_end_index_shadow1;
        const bool truncated = rusty::detail::deref_if_pointer_like(batch_end_idx) < [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index();
        rusty::raft_log_info_6("[BATCH_SEND] site={} sending batch of {} entries to follower {} (from={} to={}{})", server.site_id_, std::move(encoded_entry_count), std::move(site_id), std::move(batch_start_idx), std::move(batch_end_idx), (truncated ? ", truncated" : ""));
    }
    return std::move(skip_follower);
}

void heartbeat_phase1_body(server_h::RaftServerBase& server, PendingTable& pending_rpcs, AuthorityLedger& authority_rounds, const HeartbeatRoundScope& round) {
    uint32_t partition_id = server.partition_id_;
    size_t ord = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(ord) < rusty::len([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_))) {
        uint16_t site_id = server.peer_site_at(std::move(ord));
        if (rusty::detail::deref_if_pointer_like(site_id) == rusty::detail::deref_if_pointer_like(server.site_id_)) {
            ord += 1;
            continue;
        }
        if (rusty::detail::rust_not(server.IsLeader())) {
            break;
        }
        if (pending_rpcs.occupied(std::move(ord))) {
            ord += 1;
            continue;
        }
        uint64_t prev_log_index = static_cast<uint64_t>(0);
        uint64_t prev_log_term = static_cast<uint64_t>(0);
        rusty::RaftCommand cmd = rusty::default_like<rusty::RaftCommand>();
        uint64_t cmd_log_term = static_cast<uint64_t>(0);
        uint64_t sent_end_index = static_cast<uint64_t>(0);
        bool skip_follower = false;
        {
            const auto _lock = RaftLockGuard::new_(&server.mtx_);
            if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)) == 0) {
                rusty::raft_log_warn_2("[APPEND_ENTRIES] Repairing wrapped next_index for follower {} at leader last index {}", std::move(site_id), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index());
                const uint64_t last = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index();
                const uint64_t repaired = (raft_server_log_index_has_successor(std::move(last)) ? raft_server_follower_next_index(std::move(last)) : last);
                [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).set_next_index(std::move(ord), std::move(repaired));
            }
            prev_log_index = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)) - static_cast<uint64_t>(1);
            if (rusty::detail::deref_if_pointer_like(prev_log_index) > [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index()) {
                rusty::raft_log_info_2("[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > state_.raft_log_.last_index() ({}), fixing next_index", std::move(prev_log_index), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index());
                const uint64_t last = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index();
                const uint64_t repaired = (raft_server_log_index_has_successor(std::move(last)) ? raft_server_follower_next_index(std::move(last)) : last);
                [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).set_next_index(std::move(ord), std::move(repaired));
                prev_log_index = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)) - static_cast<uint64_t>(1);
            }
            sent_end_index = raft_server_append_sent_end(std::move(prev_log_index), 0);
            const bool snapshot_configured = raft_snapshot_manager_is_set(&server.snapshot_manager_);
            if (rusty::detail::deref_if_pointer_like(prev_log_index) > [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index()) {
                rusty::raft_log_info_3("[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > state_.raft_log_.last_index() ({}), skipping", std::move(site_id), std::move(prev_log_index), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index());
                [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).set_next_index(std::move(ord), 1);
                skip_follower = true;
            } else if (([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)) < [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).base()) && rusty::detail::deref_if_pointer_like(snapshot_configured)) {
                rusty::raft_log_info_4("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < state_.raft_log_.base()={}, sending InstallSnapshot", server.site_id_, std::move(site_id), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }(server.state_).next_index(std::move(ord)), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).base());
                const bool sent = raft_phase1_load_and_send_snapshot(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(site_id), std::move(ord));
                if (!sent) {
                    rusty::raft_log_warn_2("[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping", server.site_id_, std::move(site_id));
                }
                skip_follower = true;
            } else {
                // @unsafe
                {
                    raft_verify(rusty::detail::deref_if_pointer_like(prev_log_index) <= [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index());
                }
                if (rusty::detail::deref_if_pointer_like(prev_log_index) == static_cast<uint64_t>(0)) {
                    prev_log_term = static_cast<uint64_t>(0);
                } else if ((rusty::detail::deref_if_pointer_like(prev_log_index) == rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }(server.state_))) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }(server.state_)) > 0)) {
                    prev_log_term = static_cast<uint64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapterm_); }) { return (__r.snapterm_); } else if constexpr (requires { (__r.snapterm__field); }) { return (__r.snapterm__field); } else if constexpr (requires { ((*__r).snapterm_); }) { return ((*__r).snapterm_); } else { return ((*__r).snapterm__field); } }(server.state_));
                } else {
                    auto instance = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).get(std::move(prev_log_index));
                    if (instance.is_none()) {
                        rusty::raft_log_error_2("[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}", std::move(prev_log_index), std::move(site_id));
                        skip_follower = true;
                    } else {
                        prev_log_term = static_cast<uint64_t>(instance.unwrap().term());
                    }
                }
                if (!skip_follower) {
                    skip_follower = heartbeat_phase1_select_payload(server, std::move(ord), std::move(site_id), std::move(prev_log_index), cmd, cmd_log_term, sent_end_index);
                }
            }
        }
        if (skip_follower) {
            ord += 1;
            continue;
        }
        bool is_leader = server.IsLeader();
        rusty::RaftResponsePtr sent_response = raft_phase1_send_append(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(site_id), std::move(partition_id), std::move(is_leader), round.term(), std::move(prev_log_index), std::move(prev_log_term), round.commit_index(), static_cast<const rusty::RaftCommand*>(&cmd), std::move(cmd_log_term));
        pending_rpcs.place(std::move(ord), PendingAppend::new_(std::move(site_id), round.term(), round.round_id(), std::move(sent_end_index), std::move(sent_response), std::move(cmd)));
        if (round.authority_inserted() && round.is_member(std::move(site_id))) {
            // @unsafe
            {
                raft_verify(authority_rounds.launch(round.round_id(), std::move(site_id)));
            }
        }
        ord += 1;
    }
}

AppendReplyOutcome append_reply_nothing(AppendReplyAction action) {
    return AppendReplyOutcome{.action_ = std::move(action), .rung_ = rusty::clone(rusty::clone(BackoffKind::FLOOR)), .old_next_ = static_cast<uint64_t>(0), .new_next_ = static_cast<uint64_t>(0), .acknowledged_ = static_cast<uint64_t>(0), .previous_term_ = static_cast<uint64_t>(0)};
}

AppendReplyOutcome heartbeat_apply_append_reply(RaftConsensusState& consensus, AuthorityLedger& ledger, const SentAppend& sent, const AppendReply& reply, uint64_t log_last_index, bool is_leader) {
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
    if (rusty::detail::deref_if_pointer_like(sent.ordinal_) == rusty::len((*consensus_shadow1).peers_)) {
        return append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_UNKNOWN_FOLLOWER())));
    }
    if (!reply.status_) {
        auto old_next = (*consensus_shadow1).peers_.next_index(sent.ordinal_);
        auto rung = (*consensus_shadow1).peers_.back_off_after_reject(sent.ordinal_, reply.last_log_index_);
        auto new_next = (*consensus_shadow1).peers_.next_index(sent.ordinal_);
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
    (*consensus_shadow1).peers_.accept_through(sent.ordinal_, std::move(acknowledged), raft_server_log_index_has_successor(std::move(acknowledged)), raft_server_follower_next_index(std::move(acknowledged)));
    auto out = append_reply_nothing(rusty::clone(rusty::clone(AppendReplyAction_ACCEPTED())));
    out.acknowledged_ = std::move(acknowledged);
    return std::move(out);
}

void heartbeat_phase2_body(server_h::RaftServerBase& server, PendingTable& pending_rpcs, AuthorityLedger& authority_rounds, const HeartbeatRoundScope& round) {
    server_h::RaftServerBase* server_shadow1 = &server;
    static constexpr uint64_t RESPONSE_POLL_STEP_US = static_cast<uint64_t>(1000);
    const uint64_t response_round_timeout_us = (rusty::detail::deref_if_pointer_like((*server_shadow1).heartbeat_interval_us_) > 100000 ? static_cast<uint64_t>(100000) : (rusty::detail::deref_if_pointer_like((*server_shadow1).heartbeat_interval_us_) < 1 ? static_cast<uint64_t>(1) : (*server_shadow1).heartbeat_interval_us_));
    const uint64_t response_deadline_us = raft_monotonic_now_us() + rusty::detail::deref_if_pointer_like(response_round_timeout_us);
    bool stop_response_processing = false;
    bool retry_released_follower = false;
    while (!stop_response_processing) {
        bool waiting_for_current_round = false;
        size_t pending_ord = static_cast<size_t>(0);
        while (rusty::detail::deref_if_pointer_like(pending_ord) < rusty::len(pending_rpcs)) {
            if (rusty::detail::rust_not(((*server_shadow1)).IsLeader())) {
                stop_response_processing = true;
                break;
            }
            if (rusty::detail::rust_not(pending_rpcs.occupied(std::move(pending_ord)))) {
                pending_ord += 1;
                continue;
            }
            uint16_t follower_id = pending_rpcs.follower(std::move(pending_ord));
            uint64_t sent_term = pending_rpcs.sent_term(std::move(pending_ord));
            uint64_t sent_round = pending_rpcs.sent_round(std::move(pending_ord));
            uint64_t sent_end_index = pending_rpcs.sent_end_index(std::move(pending_ord));
            const bool cmd_has_value = raft_command_has_value(rusty::detail::ptr_cast<const rusty::RaftCommand*>(pending_rpcs.cmd(std::move(pending_ord))));
            const AppendRespView resp = raft_append_response_read(rusty::detail::ptr_cast<const rusty::RaftResponsePtr*>(pending_rpcs.response(std::move(pending_ord))));
            if (!resp.completed_) {
                if (rusty::detail::deref_if_pointer_like(sent_round) == round.round_id()) {
                    waiting_for_current_round = true;
                }
                pending_ord += 1;
                continue;
            }
            bool stepped_down = false;
            {
                const auto _lock = RaftLockGuard::new_(&(*server_shadow1).mtx_);
                bool response_available = !((!resp.status_ && (rusty::detail::deref_if_pointer_like(resp.term_) == static_cast<uint64_t>(0))) && (rusty::detail::deref_if_pointer_like(resp.last_log_index_) == static_cast<uint64_t>(0)));
                size_t resp_ord = ((*server_shadow1)).PeerOrdinal(std::move(follower_id));
                uint64_t log_last_index = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index();
                bool is_leader = ((*server_shadow1)).IsLeaderLocked();
                const AppendReplyOutcome outcome = heartbeat_apply_append_reply(rusty::detail::deref_if_pointer_like((*server_shadow1).state_), authority_rounds, SentAppend::new_(std::move(follower_id), std::move(sent_term), std::move(sent_round), std::move(sent_end_index), std::move(resp_ord)), AppendReply::new_(std::move(response_available), std::move(resp.status_), std::move(resp.term_), std::move(resp.last_log_index_)), std::move(log_last_index), std::move(is_leader));
                const AppendReplyAction action = outcome.action();
                if (rusty::detail::deref_if_pointer_like(action) == rusty::clone(AppendReplyAction_STEP_DOWN())) {
                    rusty::raft_log_info_4("[STEPDOWN] Site {}: AppendEntries response from follower {} carried higher term {} > {}", (*server_shadow1).site_id_, std::move(follower_id), std::move(resp.term_), outcome.previous_term());
                    ((*server_shadow1)).LogTermChange("AppendEntries response carried newer term", outcome.previous_term(), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_), std::move(follower_id));
                    ((*server_shadow1)).stepDown();
                    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.req_voting_); }) { return (__r.req_voting_); } else if constexpr (requires { (__r.req_voting__field); }) { return (__r.req_voting__field); } else if constexpr (requires { ((*__r).req_voting_); }) { return ((*__r).req_voting_); } else { return ((*__r).req_voting__field); } }((*server_shadow1).state_) = false;
                    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.election_in_progress_); }) { return (__r.election_in_progress_); } else if constexpr (requires { (__r.election_in_progress__field); }) { return (__r.election_in_progress__field); } else if constexpr (requires { ((*__r).election_in_progress_); }) { return ((*__r).election_in_progress_); } else { return ((*__r).election_in_progress__field); } }((*server_shadow1).state_) = false;
                    stepped_down = true;
                } else if (rusty::detail::deref_if_pointer_like(action) == rusty::clone(AppendReplyAction_BACKED_OFF())) {
                    const BackoffKind rung = outcome.rung();
                    if (rusty::detail::deref_if_pointer_like(rung) == rusty::clone(BackoffKind::FAST)) {
                        rusty::raft_log_info_6("[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})", (*server_shadow1).site_id_, std::move(follower_id), outcome.old_next(), outcome.new_next(), outcome.old_next() - outcome.new_next(), std::move(resp.last_log_index_));
                    } else if (rusty::detail::deref_if_pointer_like(rung) == rusty::clone(BackoffKind::TERM_CONFLICT)) {
                        rusty::raft_log_info_4("[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}", (*server_shadow1).site_id_, std::move(follower_id), outcome.old_next(), outcome.new_next());
                    } else if (rusty::detail::deref_if_pointer_like(rung) == rusty::clone(BackoffKind::EXPONENTIAL)) {
                        rusty::raft_log_info_4("[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)", (*server_shadow1).site_id_, std::move(follower_id), outcome.old_next(), outcome.new_next());
                    } else if (rusty::detail::deref_if_pointer_like(rung) == rusty::clone(BackoffKind::LINEAR)) {
                        rusty::raft_log_debug_4("[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}", (*server_shadow1).site_id_, std::move(follower_id), outcome.old_next(), outcome.new_next());
                    }
                } else if (rusty::detail::deref_if_pointer_like(action) == rusty::clone(AppendReplyAction_ACCEPTED())) {
                    rusty::raft_log_debug_8("[APPEND_RPC] Leader {} accepted follower {} proof: kind={} reported={} sent_end={} acknowledged={} next={} match={}", (*server_shadow1).site_id_, std::move(follower_id), (cmd_has_value ? "entries" : "heartbeat"), std::move(resp.last_log_index_), std::move(sent_end_index), outcome.acknowledged(), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }((*server_shadow1).state_).next_index(std::move(resp_ord)), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.peers_); }) { return (__r.peers_); } else if constexpr (requires { (__r.peers__field); }) { return (__r.peers__field); } else if constexpr (requires { ((*__r).peers_); }) { return ((*__r).peers_); } else { return ((*__r).peers__field); } }((*server_shadow1).state_).match_index(std::move(resp_ord)));
                } else if (rusty::detail::deref_if_pointer_like(action) == rusty::clone(AppendReplyAction_CONTRADICTORY())) {
                    rusty::raft_log_warn_3("[APPEND_RPC] Ignoring contradictory success from follower {}: reported_end={} sent_end={}", std::move(follower_id), std::move(resp.last_log_index_), std::move(sent_end_index));
                } else if (rusty::detail::deref_if_pointer_like(action) == rusty::clone(AppendReplyAction_UNKNOWN_FOLLOWER())) {
                    rusty::raft_log_debug_1("[APPEND_RPC] Ignoring replication response from removed follower {}", std::move(follower_id));
                }
            }
            const bool completed_previous_round = rusty::detail::deref_if_pointer_like(sent_round) != round.round_id();
            pending_rpcs.release(std::move(pending_ord));
            retry_released_follower = rusty::detail::deref_if_pointer_like(retry_released_follower) || rusty::detail::deref_if_pointer_like(completed_previous_round);
            if (stepped_down) {
                stop_response_processing = true;
                break;
            }
            pending_ord += 1;
        }
        const bool current_round_has_authority = authority_rounds.has_quorum(round.round_id());
        if ((rusty::detail::deref_if_pointer_like(stop_response_processing) || !waiting_for_current_round) || rusty::detail::deref_if_pointer_like(current_round_has_authority)) {
            break;
        }
        const uint64_t now_us = raft_monotonic_now_us();
        if (rusty::detail::deref_if_pointer_like(now_us) >= rusty::detail::deref_if_pointer_like(response_deadline_us)) {
            break;
        }
        const uint64_t remaining_us = rusty::detail::deref_if_pointer_like(response_deadline_us) - rusty::detail::deref_if_pointer_like(now_us);
        uint64_t step_us = (rusty::detail::deref_if_pointer_like(remaining_us) < rusty::detail::deref_if_pointer_like(RESPONSE_POLL_STEP_US) ? remaining_us : RESPONSE_POLL_STEP_US);
        // @unsafe
        {
            raft_fiber_sleep_us(std::move(step_us));
        }
    }
    if (stop_response_processing) {
        pending_rpcs.abandon();
        authority_rounds.abandon();
    } else if (retry_released_follower) {
        ((*server_shadow1)).RequestReplication();
    }
}

Phase3Outcome heartbeat_phase3_locked(RaftConsensusState& consensus, AuthorityLedger& ledger, size_t nservers, std::span<const uint16_t> members, bool is_leader) {
    RaftConsensusState* consensus_shadow1 = &consensus;
    auto commit = raft_commit_advance((*consensus_shadow1), std::move(nservers));
    const auto outcome = ledger.settle(std::move(is_leader), (*consensus_shadow1).current_term_, members, (*consensus_shadow1).read_quorum_confirmed_term_, (*consensus_shadow1).read_quorum_confirmed_round_);
    auto confirmed = false;
    if (outcome.confirmed()) {
        (*consensus_shadow1).read_quorum_confirmed_term_ = outcome.term();
        (*consensus_shadow1).read_quorum_confirmed_round_ = outcome.round_id();
        confirmed = true;
    }
    return Phase3Outcome{.commit_ = std::move(commit), .confirmed_ = std::move(confirmed)};
}

void heartbeat_phase3_body(server_h::RaftServerBase& server, AuthorityLedger& authority_rounds, const HeartbeatRoundScope& round) {
    if (rusty::detail::rust_not(server.IsLeader())) {
        return;
    }
    bool commit_advanced_after_send = false;
    {
        const auto _lock = RaftLockGuard::new_(&server.mtx_);
        rusty::Vec<uint16_t> members = rusty::clone(server.config_members_);
        size_t nservers = round.nservers();
        bool is_leader = server.IsLeaderLocked();
        const Phase3Outcome outcome = heartbeat_phase3_locked(rusty::detail::deref_if_pointer_like(server.state_), authority_rounds, std::move(nservers), std::span<const uint16_t>(rusty::as_slice(members)), std::move(is_leader));
        if (outcome.commit().advanced()) {
            rusty::raft_log_debug_2("[PHASE3-COMMIT] Advancing state_.commit_index_ {} -> {}", outcome.commit().from_index(), outcome.commit().to_index());
            server.EnqueueCommittedEntries(outcome.commit().from_index(), outcome.commit().to_index());
            commit_advanced_after_send = true;
        }
        if (outcome.confirmed()) {
            rusty::raft_log_debug_3("[READ-INDEX] site={} confirmed round={} term={}", server.site_id_, [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.read_quorum_confirmed_round_); }) { return (__r.read_quorum_confirmed_round_); } else if constexpr (requires { (__r.read_quorum_confirmed_round__field); }) { return (__r.read_quorum_confirmed_round__field); } else if constexpr (requires { ((*__r).read_quorum_confirmed_round_); }) { return ((*__r).read_quorum_confirmed_round_); } else { return ((*__r).read_quorum_confirmed_round__field); } }(server.state_), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.read_quorum_confirmed_term_); }) { return (__r.read_quorum_confirmed_term_); } else if constexpr (requires { (__r.read_quorum_confirmed_term__field); }) { return (__r.read_quorum_confirmed_term__field); } else if constexpr (requires { ((*__r).read_quorum_confirmed_term_); }) { return ((*__r).read_quorum_confirmed_term_); } else { return ((*__r).read_quorum_confirmed_term__field); } }(server.state_));
        }
    }
    if (commit_advanced_after_send) {
        server.RequestReplication();
    }
}

void heartbeat_loop_body(server_h::RaftServerBase* server) {
    auto driver = HeartbeatDriver::new_(server);
    driver.run();
}

/// # Safety
///
/// `server` must be a live `RaftServer*`, and the caller must hold that
/// server's `mtx_` for the whole call. Both hold at the only call site,
/// `RaftServer::OnRequestVote`, which takes the lock and passes `this`.
///
/// The handle is not dereferenced here. It is forwarded to the two
/// trampolines below, which cast it back exactly once each.
// @unsafe
void raft_on_request_vote(server_h::RaftServerBase& server, bool stopped, bool candidate_is_current_voter, uint64_t lst_log_idx, int64_t lst_log_term, uint16_t can_id, int64_t can_term, int64_t& reply_term, int8_t& vote_granted) {
    int64_t* reply_term_shadow1 = &reply_term;
    int8_t* vote_granted_shadow1 = &vote_granted;
    if (stopped) {
        *reply_term_shadow1 = static_cast<int64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }(server.state_));
        *vote_granted_shadow1 = static_cast<int8_t>(0);
        return;
    }
    if (((rusty::detail::deref_if_pointer_like(can_term) < 0) || (rusty::detail::deref_if_pointer_like(lst_log_term) < 0)) || !candidate_is_current_voter) {
        *reply_term_shadow1 = static_cast<int64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }(server.state_));
        *vote_granted_shadow1 = static_cast<int8_t>(0);
        return;
    }
    const auto cur_term = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }(server.state_);
    if (((static_cast<uint64_t>(can_term))) < rusty::detail::deref_if_pointer_like(cur_term)) {
        // @unsafe
        {
            raft_do_vote(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(lst_log_idx), std::move(lst_log_term), std::move(can_id), std::move(can_term), (*reply_term_shadow1), (*vote_granted_shadow1), false);
        }
        return;
    }
    if (((((static_cast<uint64_t>(can_term))) == rusty::detail::deref_if_pointer_like(cur_term)) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.vote_for_); }) { return (__r.vote_for_); } else if constexpr (requires { (__r.vote_for__field); }) { return (__r.vote_for__field); } else if constexpr (requires { ((*__r).vote_for_); }) { return ((*__r).vote_for_); } else { return ((*__r).vote_for__field); } }(server.state_)) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID))) && (rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.vote_for_); }) { return (__r.vote_for_); } else if constexpr (requires { (__r.vote_for__field); }) { return (__r.vote_for__field); } else if constexpr (requires { ((*__r).vote_for_); }) { return ((*__r).vote_for_); } else { return ((*__r).vote_for__field); } }(server.state_)) != rusty::detail::deref_if_pointer_like(can_id))) {
        // @unsafe
        {
            raft_do_vote(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(lst_log_idx), std::move(lst_log_term), std::move(can_id), std::move(can_term), (*reply_term_shadow1), (*vote_granted_shadow1), false);
        }
        return;
    }
    if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index() < rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }(server.state_))) {
        rusty::panic::do_panic(std::format("last log index is below the snapshot boundary"));
    }
    const auto lstoff = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index() - rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }(server.state_));
    const auto curlstterm = server.ElectionLastLogTermLocked();
    const auto curlstidx = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index();
    auto candidate_log_is_current = raft_server_candidate_log_is_at_least(std::move(lst_log_term), std::move(curlstterm), std::move(lst_log_idx), std::move(curlstidx));
    if (raft_server_vote_is_idempotent(static_cast<uint64_t>(can_term), std::move(cur_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.vote_for_); }) { return (__r.vote_for_); } else if constexpr (requires { (__r.vote_for__field); }) { return (__r.vote_for__field); } else if constexpr (requires { ((*__r).vote_for_); }) { return ((*__r).vote_for_); } else { return ((*__r).vote_for__field); } }(server.state_), std::move(can_id)) && rusty::detail::deref_if_pointer_like(candidate_log_is_current)) {
        // @unsafe
        {
            raft_do_vote(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(lst_log_idx), std::move(lst_log_term), std::move(can_id), std::move(can_term), (*reply_term_shadow1), (*vote_granted_shadow1), true);
        }
        return;
    }
    if ((rusty::detail::deref_if_pointer_like(lstoff) + rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }(server.state_))) != [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index()) {
        rusty::panic::do_panic(std::format("snapshot offset invariant violated"));
    }
    auto grant = std::move(candidate_log_is_current);
    // @unsafe
    {
        raft_do_vote(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr(server)), std::move(lst_log_idx), std::move(lst_log_term), std::move(can_id), std::move(can_term), (*reply_term_shadow1), (*vote_granted_shadow1), std::move(grant));
    }
}

void on_request_vote_body(server_h::RaftServerBase& server, uint64_t lst_log_idx, int64_t lst_log_term, uint16_t can_id, int64_t can_term, int64_t& reply_term, int8_t& vote_granted) {
    const auto _lock = RaftLockGuard::new_(&server.mtx_);
    rusty::raft_log_debug_1("raft receives vote from candidate: {:x}", std::move(can_id));
    bool stopped = server.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    if (stopped) {
        rusty::raft_log_debug_2("[RAFT-SHUTDOWN] Site {} rejecting RequestVote from {}", server.site_id_, std::move(can_id));
    }
    bool candidate_is_current_voter = ((rusty::detail::deref_if_pointer_like(can_id) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID)) && (rusty::detail::deref_if_pointer_like(can_id) != rusty::detail::deref_if_pointer_like(server.site_id_))) && server.IsConfigMember(std::move(can_id));
    if (!stopped && ((((rusty::detail::deref_if_pointer_like(can_term) < 0) || (rusty::detail::deref_if_pointer_like(lst_log_term) < 0)) || !candidate_is_current_voter))) {
        rusty::raft_log_warn_5("[RAFT_VOTE] Site {} rejected malformed/non-voter candidate {} term {} last_log_term {} (voter={})", server.site_id_, std::move(can_id), std::move(can_term), std::move(lst_log_term), std::move(candidate_is_current_voter));
    }
    // @unsafe
    {
        raft_on_request_vote(server, std::move(stopped), std::move(candidate_is_current_voter), std::move(lst_log_idx), std::move(lst_log_term), std::move(can_id), std::move(can_term), reply_term, vote_granted);
    }
}

/// # Safety
///
/// `server` must be a live `RaftServer*` and `cmd` a live `janus::Command*`
/// that outlives the call, and the caller must hold that server's `mtx_`
/// throughout. All three hold at the only call site,
/// `RaftServer::OnAppendEntries`. Neither handle is dereferenced here; both
/// are forwarded to trampolines that cast back exactly once.
// @unsafe
AppendReport raft_on_append_entries(server_h::RaftServerBase& server, const rusty::ffi::c_void* cmd, bool stopped, bool sender_is_current_voter, bool has_cmd, uint64_t leader_current_term, uint16_t leader_site_id, uint64_t leader_prev_log_index, uint64_t leader_prev_log_term, uint64_t leader_commit_index, uint64_t leader_next_log_term, uint64_t& follower_append_ok, uint64_t& follower_current_term, uint64_t& follower_last_log_index) {
    server_h::RaftServerBase* server_shadow1 = &server;
    uint64_t* follower_append_ok_shadow1 = &follower_append_ok;
    uint64_t* follower_current_term_shadow1 = &follower_current_term;
    uint64_t* follower_last_log_index_shadow1 = &follower_last_log_index;
    auto report = AppendReport{.accepted_ = false, .term_ok_ = false, .index_ok_ = false, .prev_term_ok_ = false, .refused_committed_conflict_ = false, .unauthoritative_ = false, .conflict_index_ = static_cast<uint64_t>(0), .local_prev_term_ = static_cast<uint64_t>(0)};
    if (stopped) {
        *follower_append_ok_shadow1 = static_cast<uint64_t>(0);
        *follower_current_term_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
        *follower_last_log_index_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index();
        return std::move(report);
    }
    const auto leader_has_higher_term = raft_server_observed_higher_term(std::move(leader_current_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_));
    const auto leader_term_is_stale = raft_server_vote_term_is_stale(std::move(leader_current_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_));
    const auto sender_is_self = rusty::detail::deref_if_pointer_like(leader_site_id) == rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.site_id_); }) { return (__r.site_id_); } else if constexpr (requires { (__r.site_id__field); }) { return (__r.site_id__field); } else if constexpr (requires { ((*__r).site_id_); }) { return ((*__r).site_id_); } else { return ((*__r).site_id__field); } }((*server_shadow1).state_));
    const auto has_known_leader = rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_leader_id_); }) { return (__r.current_leader_id_); } else if constexpr (requires { (__r.current_leader_id__field); }) { return (__r.current_leader_id__field); } else if constexpr (requires { ((*__r).current_leader_id_); }) { return ((*__r).current_leader_id_); } else { return ((*__r).current_leader_id__field); } }((*server_shadow1).state_)) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID);
    const auto known_leader_matches_sender = rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_leader_id_); }) { return (__r.current_leader_id_); } else if constexpr (requires { (__r.current_leader_id__field); }) { return (__r.current_leader_id__field); } else if constexpr (requires { ((*__r).current_leader_id_); }) { return ((*__r).current_leader_id_); } else { return ((*__r).current_leader_id__field); } }((*server_shadow1).state_)) == rusty::detail::deref_if_pointer_like(leader_site_id);
    if ((!sender_is_current_voter || rusty::detail::deref_if_pointer_like(leader_term_is_stale)) || rusty::detail::rust_not(raft_server_leader_rpc_sender_is_authoritative(std::move(leader_has_higher_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.is_leader_); }) { return (__r.is_leader_); } else if constexpr (requires { (__r.is_leader__field); }) { return (__r.is_leader__field); } else if constexpr (requires { ((*__r).is_leader_); }) { return ((*__r).is_leader_); } else { return ((*__r).is_leader__field); } }((*server_shadow1).state_), std::move(sender_is_self), std::move(has_known_leader), std::move(known_leader_matches_sender)))) {
        report.unauthoritative_ = true;
        *follower_append_ok_shadow1 = static_cast<uint64_t>(0);
        *follower_current_term_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
        *follower_last_log_index_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index();
        return std::move(report);
    }
    const auto append_payload_valid = raft_ae_decode_payload(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr((*server_shadow1))), cmd, std::move(leader_prev_log_index), std::move(leader_next_log_term));
    const uint64_t decoded_count = static_cast<uint64_t>(rusty::len((*server_shadow1).decoded_terms_));
    auto term_ok = raft_server_append_term_is_acceptable(std::move(leader_current_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_));
    const auto compacted_prefix_miss = ((rusty::detail::deref_if_pointer_like(leader_prev_log_index) != static_cast<uint64_t>(0)) && (rusty::detail::deref_if_pointer_like(leader_prev_log_index) < [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).base())) && (rusty::detail::deref_if_pointer_like(leader_prev_log_index) != rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }((*server_shadow1).state_)));
    auto index_ok = (rusty::detail::deref_if_pointer_like(leader_prev_log_index) <= [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index()) && rusty::detail::rust_not(compacted_prefix_miss);
    uint64_t local_prev_term = static_cast<uint64_t>(0);
    if (rusty::detail::deref_if_pointer_like(leader_prev_log_index) == static_cast<uint64_t>(0)) {
        local_prev_term = static_cast<uint64_t>(0);
    } else if (rusty::detail::deref_if_pointer_like(leader_prev_log_index) == rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapidx_); }) { return (__r.snapidx_); } else if constexpr (requires { (__r.snapidx__field); }) { return (__r.snapidx__field); } else if constexpr (requires { ((*__r).snapidx_); }) { return ((*__r).snapidx_); } else { return ((*__r).snapidx__field); } }((*server_shadow1).state_))) {
        local_prev_term = static_cast<uint64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.snapterm_); }) { return (__r.snapterm_); } else if constexpr (requires { (__r.snapterm__field); }) { return (__r.snapterm__field); } else if constexpr (requires { ((*__r).snapterm_); }) { return ((*__r).snapterm_); } else { return ((*__r).snapterm__field); } }((*server_shadow1).state_));
    } else if (((rusty::detail::deref_if_pointer_like(leader_prev_log_index) <= [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index()) && rusty::detail::rust_not(compacted_prefix_miss)) && [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).holds(std::move(leader_prev_log_index))) {
        local_prev_term = static_cast<uint64_t>([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).get(std::move(leader_prev_log_index)).unwrap().term());
    }
    auto prev_term_ok = (rusty::detail::deref_if_pointer_like(leader_prev_log_index) == static_cast<uint64_t>(0)) || (rusty::detail::deref_if_pointer_like(local_prev_term) == rusty::detail::deref_if_pointer_like(leader_prev_log_term));
    report.term_ok_ = std::move(term_ok);
    report.index_ok_ = std::move(index_ok);
    report.prev_term_ok_ = std::move(prev_term_ok);
    report.local_prev_term_ = std::move(local_prev_term);
    if (term_ok) {
        if (raft_server_observed_higher_term(std::move(leader_current_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_))) {
            const auto prev_term = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_) = std::move(leader_current_term);
            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.vote_for_); }) { return (__r.vote_for_); } else if constexpr (requires { (__r.vote_for__field); }) { return (__r.vote_for__field); } else if constexpr (requires { ((*__r).vote_for_); }) { return ((*__r).vote_for_); } else { return ((*__r).vote_for__field); } }((*server_shadow1).state_) = RAFT_SERVER_INVALID_SITE_ID;
            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_leader_id_); }) { return (__r.current_leader_id_); } else if constexpr (requires { (__r.current_leader_id__field); }) { return (__r.current_leader_id__field); } else if constexpr (requires { ((*__r).current_leader_id_); }) { return ((*__r).current_leader_id_); } else { return ((*__r).current_leader_id__field); } }((*server_shadow1).state_) = raft_server_leader_hint_after_transition(false, true, [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.site_id_); }) { return (__r.site_id_); } else if constexpr (requires { (__r.site_id__field); }) { return (__r.site_id__field); } else if constexpr (requires { ((*__r).site_id_); }) { return ((*__r).site_id_); } else { return ((*__r).site_id__field); } }((*server_shadow1).state_), std::move(leader_site_id));
            const uint64_t now_term = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
            ((*server_shadow1)).LogTermChange("AppendEntries leader term is newer", std::move(prev_term), std::move(now_term), std::move(leader_site_id));
            if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.is_leader_); }) { return (__r.is_leader_); } else if constexpr (requires { (__r.is_leader__field); }) { return (__r.is_leader__field); } else if constexpr (requires { ((*__r).is_leader_); }) { return ((*__r).is_leader_); } else { return ((*__r).is_leader__field); } }((*server_shadow1).state_)) {
                ((*server_shadow1)).stepDown();
            } else {
                ((*server_shadow1)).setIsLeader(false);
            }
            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.req_voting_); }) { return (__r.req_voting_); } else if constexpr (requires { (__r.req_voting__field); }) { return (__r.req_voting__field); } else if constexpr (requires { ((*__r).req_voting_); }) { return ((*__r).req_voting_); } else { return ((*__r).req_voting__field); } }((*server_shadow1).state_) = false;
            [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.election_in_progress_); }) { return (__r.election_in_progress_); } else if constexpr (requires { (__r.election_in_progress__field); }) { return (__r.election_in_progress__field); } else if constexpr (requires { ((*__r).election_in_progress_); }) { return ((*__r).election_in_progress_); } else { return ((*__r).election_in_progress__field); } }((*server_shadow1).state_) = false;
        }
        [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_leader_id_); }) { return (__r.current_leader_id_); } else if constexpr (requires { (__r.current_leader_id__field); }) { return (__r.current_leader_id__field); } else if constexpr (requires { ((*__r).current_leader_id_); }) { return ((*__r).current_leader_id_); } else { return ((*__r).current_leader_id__field); } }((*server_shadow1).state_) = raft_server_leader_hint_after_transition(false, true, [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.site_id_); }) { return (__r.site_id_); } else if constexpr (requires { (__r.site_id__field); }) { return (__r.site_id__field); } else if constexpr (requires { ((*__r).site_id_); }) { return ((*__r).site_id_); } else { return ((*__r).site_id__field); } }((*server_shadow1).state_), std::move(leader_site_id));
        ((*server_shadow1)).resetTimerLocked("AppendEntries from current-term leader");
    }
    if (!(raft_server_append_is_acceptable(std::move(term_ok), std::move(index_ok), std::move(prev_term_ok)) && rusty::detail::deref_if_pointer_like(append_payload_valid))) {
        *follower_append_ok_shadow1 = static_cast<uint64_t>(0);
        *follower_current_term_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
        *follower_last_log_index_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index();
        return std::move(report);
    }
    if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.is_leader_); }) { return (__r.is_leader_); } else if constexpr (requires { (__r.is_leader__field); }) { return (__r.is_leader__field); } else if constexpr (requires { ((*__r).is_leader_); }) { return ((*__r).is_leader_); } else { return ((*__r).is_leader__field); } }((*server_shadow1).state_)) {
        ((*server_shadow1)).stepDown();
    } else {
        ((*server_shadow1)).setIsLeader(false);
    }
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.req_voting_); }) { return (__r.req_voting_); } else if constexpr (requires { (__r.req_voting__field); }) { return (__r.req_voting__field); } else if constexpr (requires { ((*__r).req_voting_); }) { return ((*__r).req_voting_); } else { return ((*__r).req_voting__field); } }((*server_shadow1).state_) = false;
    [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.election_in_progress_); }) { return (__r.election_in_progress_); } else if constexpr (requires { (__r.election_in_progress__field); }) { return (__r.election_in_progress__field); } else if constexpr (requires { ((*__r).election_in_progress_); }) { return ((*__r).election_in_progress_); } else { return ((*__r).election_in_progress__field); } }((*server_shadow1).state_) = false;
    const auto old_last_log_index = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index();
    const auto count = (has_cmd ? decoded_count : 0);
    auto accepted_through = raft_server_append_sent_end(std::move(leader_prev_log_index), std::move(count));
    auto have_first_write = false;
    auto truncate_suffix = false;
    uint64_t first_write_index = static_cast<uint64_t>(0);
    uint64_t i = static_cast<uint64_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::detail::deref_if_pointer_like(decoded_count)) {
        auto index = (rusty::detail::deref_if_pointer_like(leader_prev_log_index) + rusty::detail::deref_if_pointer_like(i)) + 1;
        bool local_exists = false;
        int64_t local_term = static_cast<int64_t>(0);
        auto slot = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).get(std::move(index));
        if (slot.is_some()) {
            const server_h::RaftEntry& entry = slot.unwrap();
            local_exists = raft_command_has_value(rusty::detail::ptr_cast<const rusty::RaftCommand*>(entry.cmd()));
            if (local_exists) {
                local_term = entry.term();
            }
        }
        const int64_t incoming_term = (*server_shadow1).decoded_terms_[static_cast<size_t>(i)];
        if (raft_server_append_entry_conflicts(std::move(local_exists), static_cast<uint64_t>(local_term), static_cast<uint64_t>(incoming_term))) {
            have_first_write = true;
            first_write_index = std::move(index);
            truncate_suffix = rusty::detail::deref_if_pointer_like(index) <= rusty::detail::deref_if_pointer_like(old_last_log_index);
            break;
        }
        i += 1;
    }
    if (rusty::detail::deref_if_pointer_like(truncate_suffix) && (rusty::detail::deref_if_pointer_like(first_write_index) <= ((rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_)) > rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.execute_index_); }) { return (__r.execute_index_); } else if constexpr (requires { (__r.execute_index__field); }) { return (__r.execute_index__field); } else if constexpr (requires { ((*__r).execute_index_); }) { return ((*__r).execute_index_); } else { return ((*__r).execute_index__field); } }((*server_shadow1).state_)) ? [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_) : [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.execute_index_); }) { return (__r.execute_index_); } else if constexpr (requires { (__r.execute_index__field); }) { return (__r.execute_index__field); } else if constexpr (requires { ((*__r).execute_index_); }) { return ((*__r).execute_index_); } else { return ((*__r).execute_index__field); } }((*server_shadow1).state_))))) {
        report.refused_committed_conflict_ = true;
        report.conflict_index_ = std::move(first_write_index);
        *follower_append_ok_shadow1 = static_cast<uint64_t>(0);
        *follower_current_term_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
        *follower_last_log_index_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index();
        return std::move(report);
    }
    if (have_first_write) {
        [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).truncate_from(std::move(first_write_index));
        // @unsafe
        {
            raft_ae_apply_incoming(static_cast<server_h::RaftServerBase*>(rusty::detail::ptr_or_addr((*server_shadow1))), cmd, std::move(leader_prev_log_index), std::move(leader_next_log_term), std::move(first_write_index));
        }
    }
    if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index() != raft_server_append_result_last_index(std::move(old_last_log_index), std::move(accepted_through), std::move(truncate_suffix))) {
        rusty::panic::do_panic(std::format("append left the log tail somewhere the result rule did not predict"));
    }
    auto follower_commit_candidate = raft_server_commit_index_clamp(std::move(leader_commit_index), std::move(accepted_through));
    if (raft_server_log_index_above(std::move(follower_commit_candidate), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_))) {
        const auto old_commit = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_);
        [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_) = std::move(follower_commit_candidate);
        if ([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }((*server_shadow1).state_).last_index() < rusty::detail::deref_if_pointer_like([&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_))) {
            rusty::panic::do_panic(std::format("commit index advanced past the log tail"));
        }
        const uint64_t new_commit = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }((*server_shadow1).state_);
        ((*server_shadow1)).EnqueueCommittedEntries(std::move(old_commit), std::move(new_commit));
    }
    *follower_append_ok_shadow1 = static_cast<uint64_t>(1);
    *follower_current_term_shadow1 = [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }((*server_shadow1).state_);
    *follower_last_log_index_shadow1 = accepted_through;
    report.accepted_ = true;
    return std::move(report);
}

void on_append_entries_body(server_h::RaftServerBase& server, uint64_t leader_current_term, uint16_t leader_site_id, uint64_t leader_prev_log_index, uint64_t leader_prev_log_term, uint64_t leader_commit_index, const rusty::ffi::c_void* cmd, bool cmd_has_value, uint64_t leader_next_log_term, uint64_t& follower_append_ok, uint64_t& follower_current_term, uint64_t& follower_last_log_index) {
    const auto _lock = RaftLockGuard::new_(&server.mtx_);
    bool stopped = server.stop_.load(rusty::sync::atomic::Ordering::Acquire);
    bool sender_is_current_voter = ((rusty::detail::deref_if_pointer_like(leader_site_id) != rusty::detail::deref_if_pointer_like(RAFT_SERVER_INVALID_SITE_ID)) && (rusty::detail::deref_if_pointer_like(leader_site_id) != rusty::detail::deref_if_pointer_like(server.site_id_))) && server.IsConfigMember(std::move(leader_site_id));
    const AppendReport report = raft_on_append_entries(server, cmd, std::move(stopped), std::move(sender_is_current_voter), std::move(cmd_has_value), std::move(leader_current_term), std::move(leader_site_id), std::move(leader_prev_log_index), std::move(leader_prev_log_term), std::move(leader_commit_index), std::move(leader_next_log_term), follower_append_ok, follower_current_term, follower_last_log_index);
    if (!stopped && !report.accepted()) {
        if (report.refused_committed_conflict()) {
            rusty::raft_log_error_4("[APPEND_REJECT] Site {} refusing conflict at committed index {} (commit_index={}, execute_index={})", server.site_id_, report.conflict_index(), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.commit_index_); }) { return (__r.commit_index_); } else if constexpr (requires { (__r.commit_index__field); }) { return (__r.commit_index__field); } else if constexpr (requires { ((*__r).commit_index_); }) { return ((*__r).commit_index_); } else { return ((*__r).commit_index__field); } }(server.state_), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.execute_index_); }) { return (__r.execute_index_); } else if constexpr (requires { (__r.execute_index__field); }) { return (__r.execute_index__field); } else if constexpr (requires { ((*__r).execute_index_); }) { return ((*__r).execute_index_); } else { return ((*__r).execute_index__field); } }(server.state_));
        } else if (report.unauthoritative()) {
            rusty::raft_log_warn_6("[APPEND_REJECT] Site {} rejecting unauthoritative AppendEntries sender {} term {} (local_term={} leader={} voter={})", server.site_id_, std::move(leader_site_id), std::move(leader_current_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }(server.state_), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_leader_id_); }) { return (__r.current_leader_id_); } else if constexpr (requires { (__r.current_leader_id__field); }) { return (__r.current_leader_id__field); } else if constexpr (requires { ((*__r).current_leader_id_); }) { return ((*__r).current_leader_id_); } else { return ((*__r).current_leader_id__field); } }(server.state_), std::move(sender_is_current_voter));
        } else {
            rusty::raft_log_info_10("[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})", server.site_id_, std::move(leader_site_id), report.term_ok(), report.index_ok(), report.prev_term_ok(), std::move(leader_current_term), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.current_term_); }) { return (__r.current_term_); } else if constexpr (requires { (__r.current_term__field); }) { return (__r.current_term__field); } else if constexpr (requires { ((*__r).current_term_); }) { return ((*__r).current_term_); } else { return ((*__r).current_term__field); } }(server.state_), std::move(leader_prev_log_index), [&](auto&& __r) -> decltype(auto) { if constexpr (requires { (__r.raft_log_); }) { return (__r.raft_log_); } else if constexpr (requires { (__r.raft_log__field); }) { return (__r.raft_log__field); } else if constexpr (requires { ((*__r).raft_log_); }) { return ((*__r).raft_log_); } else { return ((*__r).raft_log__field); } }(server.state_).last_index(), report.local_prev_term());
        }
    }
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

inline bool CommitAdvance::advanced() const {
    return this->advanced_;
}

inline uint64_t CommitAdvance::from_index() const {
    return this->from_;
}

inline uint64_t CommitAdvance::to_index() const {
    return this->to_;
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

inline const CommitAdvance& Phase3Outcome::commit() const {
    return this->commit_;
}

inline bool Phase3Outcome::confirmed() const {
    return this->confirmed_;
}

inline HeartbeatRoundState HeartbeatRoundState::new_() {
    return HeartbeatRoundState{.pending_rpcs_ = PendingTable::new_(), .authority_rounds_ = AuthorityLedger::new_(), .pending_leader_term_ = rusty::None, .scope_ = HeartbeatRoundScope::new_()};
}

inline HeartbeatDriver HeartbeatDriver::new_(server_h::RaftServerBase* server) {
    return HeartbeatDriver{.server_ = server, .round_ = HeartbeatRoundState::new_()};
}

inline void HeartbeatDriver::run() {
    server_h::RaftServerBase& server = *this->server_;
    server.HeartbeatPrologue();
    while (server.HeartbeatLooping()) {
        if (rusty::detail::rust_not(server.HeartbeatWait())) {
            break;
        }
        if (!heartbeat_phase0_body(server, this->round_.pending_rpcs_, this->round_.authority_rounds_, this->round_.pending_leader_term_, this->round_.scope_)) {
            continue;
        }
        heartbeat_phase1_body(server, this->round_.pending_rpcs_, this->round_.authority_rounds_, this->round_.scope_);
        heartbeat_phase2_body(server, this->round_.pending_rpcs_, this->round_.authority_rounds_, this->round_.scope_);
        heartbeat_phase3_body(server, this->round_.authority_rounds_, this->round_.scope_);
    }
    server.HeartbeatEpilogue();
}

inline bool AppendReport::accepted() const {
    return this->accepted_;
}

inline bool AppendReport::term_ok() const {
    return this->term_ok_;
}

inline bool AppendReport::index_ok() const {
    return this->index_ok_;
}

inline bool AppendReport::prev_term_ok() const {
    return this->prev_term_ok_;
}

inline bool AppendReport::refused_committed_conflict() const {
    return this->refused_committed_conflict_;
}

inline bool AppendReport::unauthoritative() const {
    return this->unauthoritative_;
}

inline uint64_t AppendReport::conflict_index() const {
    return this->conflict_index_;
}

inline uint64_t AppendReport::local_prev_term() const {
    return this->local_prev_term_;
}
/*RUSTYCPP:GEN-END id=raft_server.heartbeat_round_scope*/

// ==========================================================================
// PHASE 1's two unspellable regions.
//
// The loop around them is Rust now (heartbeat_phase1_body). These two are
// not: the first sends an InstallSnapshot whose completion callback is a
// C++ lambda that re-enters the server, and the second selects the
// AppendEntries payload under #ifdef RAFT_BATCH_OPTIMIZATION -- conditional
// compilation, which has no DSL spelling.
// ==========================================================================
extern "C" {

// The follower is behind the log's base, so send it a snapshot instead.
// Returns true in both outcomes -- sent, or failed to load -- because either
// way the normal AppendEntries for this follower is skipped.
// CALLER MUST HOLD mtx_.
// Loads the latest snapshot and sends it. Returns false when there is no
// snapshot to load; the caller logs that and skips the follower either way.
// CALLER MUST HOLD mtx_.
bool raft_phase1_load_and_send_snapshot(RaftServerBase* self,
                                        uint16_t site_id, size_t ord) {
  janus::raft::SnapshotMetadata snap_meta;
  std::string snap_data;
  if (!self->snapshot_manager_->LoadLatestSnapshot(&snap_meta, &snap_data)) {
    return false;
  }
  const uint64_t snap_last_idx = snap_meta.last_included_index;
  const uint64_t snap_last_term = snap_meta.last_included_term;
  const uint64_t send_term = self->state_.current_term_;
  auto callback_lifetime = self->async_callback_lifetime_;
  static_cast<RaftServer*>(self)->commo()->SendInstallSnapshot(
      site_id, self->partition_id_,
      send_term, self->site_id_,
      snap_last_idx, snap_last_term,
      snap_data,
      [callback_lifetime, site_id, ord, snap_last_idx, send_term](
          uint64_t follower_term) {
        std::lock_guard<std::mutex> lifetime_lock(callback_lifetime->mutex);
        auto* server = callback_lifetime->server;
        if (server == nullptr) {
          return;
        }
        // THE LOCK IS TAKEN BELOW THIS CHECK, NOT ABOVE IT, and that
        // placement is load-bearing.
        //
        // This callback runs in TWO contexts. Normally the reactor invokes it
        // when the reply lands, with mtx_ not held. But
        // RaftCommo::SendInstallSnapshot invokes it INLINE, on the caller's
        // stack, when PeerForSite returns null (commo.cc:167-170) -- and that
        // caller is PHASE 1, which holds mtx_. A recursive_mutex tolerates
        // the re-entry; a plain std::mutex would self-deadlock, which is what
        // blocked demoting it.
        //
        // The inline path always passes follower_term == 0, so it takes the
        // branch below and returns having touched no state at all. Acquiring
        // after the check means the synchronous context never reaches the
        // lock, and every path that does reach it is the asynchronous one.
        // site_id_ is written once during Setup, so reading it for the log
        // needs no lock.
        //
        // Everything past this point is Rust
        // (RaftServerBase::InstallSnapshotReplyAccepted), which takes mtx_
        // itself -- so the ordering the comment describes is now expressed by
        // where the call sits rather than by where a guard is declared.
        if (!raft_server_install_snapshot_reply_is_available(follower_term)) {
          Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} snapshot response unavailable; retaining replication indices",
                   server->site_id_, site_id);
          return;
        }
        server->InstallSnapshotReplyAccepted(site_id, ord, snap_last_idx,
                                             send_term, follower_term);
      });
  return true;
}

// RAFT_BATCH_OPTIMIZATION, as a value a DSL body can branch on. Same device
// as raft_election_debug_enabled: the two payload-selection arms were an
// #ifdef/#ifndef pair, and the preprocessor has no DSL spelling, so the
// switch becomes a branch the compiler folds.
bool raft_batch_optimization_enabled() {
#ifdef RAFT_BATCH_OPTIMIZATION
  return true;
#else
  return false;
#endif
}

uint64_t raft_append_entries_batch_max() {
  return GetAppendEntriesBatchMaxEntries();
}

// The wire kind of a command. The lookup that produced it is Rust; this only
// reads a field of the opaque payload.
int32_t raft_command_kind(const rusty::RaftCommand* cmd) {
  return static_cast<int32_t>(cmd->kind_);
}

// Appends the entry at `index` to the batch under assembly. Returns false
// when it is not a TpcCommitCommand, which ends the batch.
//
// STAMP A COPY, NEVER THE STORED ENTRY.
//
// The batched wire format carries each entry's term inside its
// TpcCommitCommand -- the receiver reads it back to set the entry's term --
// and the command is created with term 0 (raft_worker.cc), so something has
// to stamp it. This used to const_cast the payload of the entry in the log
// and write through it: a mutation of committed, already-replicated, shared
// state, performed lazily at send time.
//
// It was idempotent, because a committed entry's term never changes, so it
// was not a live bug. But it is the only place in the file that modifies an
// existing log entry, and a log that can be modified after commit cannot
// state its own invariants -- so it has no spelling in a Rust-owned RaftLog,
// whose entries are reachable only as &RaftEntry.
//
// Copying is cheap and does not touch the payload: TpcCommitCommand is two
// scalars, an int and two Arcs, so the copy bumps refcounts and leaves the
// LogEntry bytes shared.
// @unsafe { factory-fresh Arc, uniquely owned mutation window }
bool raft_batch_try_push(RaftServerBase* self, uint64_t index) {
  const auto found = self->state_.raft_log_.get(index);
  verify(found.is_some());
  const RaftEntry& entry = found.unwrap();
  // marshallable_cast<T>(SerializableEnvelope&) handles a Command directly.
  auto cur_cmd = marshallable_cast<TpcCommitCommand>(entry.cmd());
  if (cur_cmd.is_none()) {
    return false;
  }
  auto stamped = rusty::Arc<TpcCommitCommand>::make(*cur_cmd.as_ref().unwrap());
  stamped.get_mut().unwrap().term = entry.term();
  self->batch_buffer_.push(std::move(stamped));
  return true;
}

// Fill-then-wrap: the buffer is assembled entry by entry above, and wrapped
// once, here, when it is complete.
void raft_batch_finalize(RaftServerBase* self, rusty::RaftCommand* cmd_out) {
  TpcBatchCommand batch_local;
  // AddCmd per element rather than AddCmds, which assigned the whole vector
  // (`cmds_ = cmds`) and so bumped every Arc's refcount. The buffer is a
  // rusty::Vec now and the round is done with it, so each handle MOVES.
  for (rusty::Arc<TpcCommitCommand>& cmd : self->batch_buffer_) {
    batch_local.AddCmd(std::move(cmd));
  }
  auto batch_cmd = rusty::Arc<TpcBatchCommand>::make(std::move(batch_local));
  *cmd_out = std::move(batch_cmd);
}

// The send itself. Non-blocking: it only initiates the async call. The
// response is a shared_ptr the transport's callback also holds, which is
// what keeps it alive; the pending table carries it opaquely.
rusty::RaftResponsePtr raft_phase1_send_append(
    RaftServerBase* self, uint16_t site_id, uint32_t partition_id,
    bool is_leader, uint64_t term, uint64_t prev_log_index,
    uint64_t prev_log_term, uint64_t commit_index,
    const rusty::RaftCommand* cmd, uint64_t cmd_log_term) {
  return static_cast<RaftServer*>(self)->commo()->SendAppendEntries2(
      site_id, partition_id, -1, -1, is_leader, self->site_id_, term,
      prev_log_index, prev_log_term, commit_index, *cmd, cmd_log_term);
}

}  // extern "C"

// AppendRespView is generated by the block above, so this kernel has to sit
// below it rather than with the rest of the bridge.
extern "C" {
// The three scalars of an rrr AppendEntries reply. The response object is a
// shared_ptr the DSL only carries; this reads through it.
AppendRespView raft_append_response_read(
    const rusty::RaftResponsePtr* response) {
  const AppendEntriesResponse& resp = **response;
  AppendRespView view{};
  view.completed_ = resp.completed.load(std::memory_order_acquire);
  view.status_ = resp.status != 0;
  view.term_ = resp.term;
  view.last_log_index_ = resp.last_log_index;
  return view;
}
}  // extern "C"

// The three loop-carried locals, which outlive a round but not the loop. They
// stay C++ because unique_ptr<PendingAppendEntries> and the wire types inside
// PendingHeartbeatAuthority have no DSL spelling; the Rust driver carries this
// object as an opaque handle and never looks inside it.

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
  apply_thread_running_.store(false, rusty::sync::atomic::Ordering::SeqCst);
  if (apply_thread_.joinable()) {
    apply_thread_.join();
  }

  Log_info("site par {}, loc {}: prepare {}, accept {}, commit {}",
      partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
}


// @unsafe - calls @safe doVote, external calls marked @external [safe]
// The one kernel raft_on_request_vote still needs. It is not a forwarder: the
// vote reply is written through two out-params whose C++ types (int64_t& /
// int8_t&) are what the RPC handler already owns.
extern "C" {

void raft_do_vote(RaftServerBase* server,
                  uint64_t lst_log_idx, int64_t lst_log_term,
                  uint16_t can_id, int64_t can_term,
                  int64_t& reply_term, int8_t& vote_granted, bool vote) {
  // @unsafe { RaftServerBase is RaftServer's base; the call needs the derived }
  static_cast<RaftServer*>(server)->doVote(lst_log_idx, lst_log_term, can_id,
                                           can_term, &reply_term, &vote_granted,
                                           vote);
}

}  // extern "C"

// The body is raft_on_request_vote, a DSL function. What stays here is the
// lock, the two log lines that must keep their level short-circuit, and the
// current_config_ membership test -- that member is a std::set which has not
// moved into the state struct.
void RaftServer::OnRequestVote(const slotid_t& lst_log_idx,
                               const ballot_t& lst_log_term,
                               const siteid_t& can_id,
                               const ballot_t& can_term,
                               ballot_t *reply_term,
                               bool_t *vote_granted) {
  // The body is Rust (on_request_vote_body). This entry point remains
  // because the rrr service layer calls it by name on RaftServer.
  on_request_vote_body(*this, lst_log_idx, lst_log_term, can_id, can_term,
                       *reply_term, *vote_granted);
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

// @unsafe - suspends this fiber; unlike a plain Fiber::sleep this is
// interrupted by shutdown, so false means stop rather than timed out.
RaftStartResult RaftServer::Start(const janus::Command& cmd,
                                  uint64_t *index,
                                  uint64_t *term,
                                  slotid_t slot_id,
                                  ballot_t ballot) {
  return StartImpl(&cmd, index, term, slot_id, ballot);
}

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server
 * should we exclude the execution of this function for leader? */
// @unsafe - external calls marked @external [safe], output pointer writes in @unsafe blocks
// The three kernels raft_on_append_entries still needs. None is a forwarder:
// all three decode or build the wire payload, which is a Marshallable
// hierarchy with no Rust spelling.
extern "C" {

// Decodes the wire payload into the Rust-owned decoded_terms_ and reports
// whether the encoded entry count is acceptable. Called from the DSL body
// only AFTER the stopped check and the authoritative-sender gate, which is
// where the original did this work -- a rejected AppendEntries must not pay
// for a dynamic cast it will discard.
//
// The count is NOT reported out of band: one term goes in per encoded entry,
// so decoded_terms_.len() is the count, and the caller reads it there.
bool raft_ae_decode_payload(RaftServerBase* server,
                            const rusty::ffi::c_void* cmd_handle,
                            uint64_t leader_prev_log_index,
                            uint64_t leader_next_log_term) {
  rusty::Vec<int64_t>& terms = server->decoded_terms_;
  terms.clear();
  const janus::Command& cmd =
      *static_cast<const janus::Command*>(static_cast<const void*>(cmd_handle));
  if (!cmd.has_value()) {
    return raft_server_append_entry_count_fits(leader_prev_log_index, 0);
  }
#ifdef RAFT_BATCH_OPTIMIZATION
  if (raft_server_append_command_is_batch(cmd.kind_,
                                          TpcBatchCommand::static_kind())) {
    const auto batch = marshallable_cast<TpcBatchCommand>(cmd);
    if (batch.is_none()) {
      return false;
    }
    const uint64_t count =
        static_cast<uint64_t>(batch.as_ref().unwrap()->cmds_.size());
    for (const rusty::Arc<TpcCommitCommand>& c : batch.unwrap()->cmds_) {
      terms.push(static_cast<int64_t>(c->term));
    }
    return raft_server_append_batch_count_is_valid(leader_prev_log_index,
                                                   count);
  }
#endif
  terms.push(static_cast<int64_t>(leader_next_log_term));
  return raft_server_append_entry_count_fits(leader_prev_log_index, 1);
}

// Builds and appends the incoming entries at or past first_write_index.
// Called only after the Rust body has decided to accept and has truncated
// the divergent suffix, so entries are constructed for a payload that is
// actually being written -- the same laziness the C++ had.
void raft_ae_apply_incoming(RaftServerBase* server,
                            const rusty::ffi::c_void* cmd_handle,
                            uint64_t leader_prev_log_index,
                            uint64_t leader_next_log_term,
                            uint64_t first_write_index) {
  RaftServer* self = static_cast<RaftServer*>(server);
  const janus::Command& cmd =
      *static_cast<const janus::Command*>(static_cast<const void*>(cmd_handle));
  uint64_t cnt = 0;
#ifdef RAFT_BATCH_OPTIMIZATION
  if (raft_server_append_command_is_batch(cmd.kind_,
                                          TpcBatchCommand::static_kind())) {
    const auto cmds = marshallable_cast<TpcBatchCommand>(cmd);
    verify(cmds.is_some());
    for (const rusty::Arc<TpcCommitCommand>& c : cmds.unwrap()->cmds_) {
      ++cnt;
      const uint64_t index =
          raft_server_append_sent_end(leader_prev_log_index, cnt);
      if (index < first_write_index) {
        continue;
      }
      const uint64_t appended =
          self->state_.raft_log_.append(RaftEntry::new_(c->term, c.clone()));
      verify(appended == index);
    }
    return;
  }
#endif
  const uint64_t index = raft_server_append_sent_end(leader_prev_log_index, 1);
  if (index >= first_write_index) {
    const uint64_t appended = self->state_.raft_log_.append(
        RaftEntry::new_(leader_next_log_term, cmd));
    verify(appended == index);
  }
}

}  // extern "C"

// The body is raft_on_append_entries, a DSL function. What stays here is the
// lock, the payload decode -- the only part that must touch janus::Command --
// the current_config_ membership test, and the two rejection log lines,
// which keep their level short-circuit and use the report the DSL returns to
// say which check failed.
void RaftServer::OnAppendEntries(const slotid_t slot_id,
                                 const ballot_t ballot,
                                 const uint64_t leaderCurrentTerm,
                                 const siteid_t leaderSiteId,
                                 const uint64_t leaderPrevLogIndex,
                                 const uint64_t leaderPrevLogTerm,
                                 const uint64_t leaderCommitIndex,
                                 const janus::Command& cmd,
                                 const uint64_t leaderNextLogTerm,
                                 uint64_t *followerAppendOK,
                                 uint64_t *followerCurrentTerm,
                                 uint64_t *followerLastLogIndex) {
  // The body is Rust (on_append_entries_body). This entry point remains
  // because the rrr service layer calls it by name on RaftServer; slot_id
  // and ballot were unused there too.
  (void)slot_id;
  (void)ballot;
  on_append_entries_body(
      *this, leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex,
      leaderPrevLogTerm, leaderCommitIndex,
      static_cast<const rusty::ffi::c_void*>(static_cast<const void*>(&cmd)),
      cmd.has_value(), leaderNextLogTerm, *followerAppendOK,
      *followerCurrentTerm, *followerLastLogIndex);
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
  std::lock_guard<RaftCheckedMutex> lock(mtx_);

  // The body is Rust (RaftServerBase::OnInstallSnapshotLocked). What stays
  // here is the catch-all around it -- exceptions have no DSL spelling --
  // and the rrr service layer's entry point by name on RaftServer.
  try {
    OnInstallSnapshotLocked(term, leader_id, last_included_index,
                            last_included_term, &data, *term_out);
  } catch (const std::exception& error) {
    Log_error("[INSTALL-SNAPSHOT] Site {} threw while installing snapshot: {}",
              site_id_, error.what());
    FailStop();
    *term_out = 0;
  } catch (...) {
    Log_error("[INSTALL-SNAPSHOT] Site {} threw while installing snapshot",
              site_id_);
    FailStop();
    *term_out = 0;
  }
}

// ============================================================================
// stepDown - Central leader step-down function
// ============================================================================

// ============================================================================
// MEMBERSHIP CONFIGURATION
// ============================================================================




} // namespace janus
