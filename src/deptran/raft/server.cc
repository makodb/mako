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


static_assert(std::is_same_v<siteid_t, uint16_t>);
static_assert(static_cast<uint16_t>(INVALID_SITEID) ==
              std::numeric_limits<uint16_t>::max());

}  // namespace

// ===========================================================================
// THE EXCEPTION BOUNDARY
//
// Rust models failure as a VALUE; C++ models it as control flow. The two do
// not compose: a C++ exception unwinding through a Rust frame is undefined
// behaviour, so no DSL body may ever be on the stack when one is thrown.
//
// The rule this file follows, and the only rule that makes the two models
// meet: an exception is converted to a value at the LAST C++ FRAME BEFORE
// RUST, and above that line every failure is a value. This is the one place
// that conversion happens. Seven call sites used to open-code it, each with
// its own catch pair and its own wording.
//
// Two things it deliberately does NOT do:
//
//   - It does not format a diagnostic. `what` is a string literal, so the
//     success path costs nothing; callers that have more to say (a slot id,
//     a snapshot index) log it themselves when this returns false. That
//     matters for raft_apply_invoke, which runs once per applied entry.
//   - It does not decide what failure means. Some callers fail-stop, some
//     return false, some publish a zero term. That belongs at the call site,
//     which is exactly the argument for making the failure a value.
//
// @unsafe { the catch-all is the point }
template <typename Fn>
static bool raft_catch(uint16_t site_id, const char* what, Fn&& fn) {
  try {
    fn();
    return true;
  } catch (const std::exception& error) {
    Log_error("[RAFT-GUARD] Site {}: {} threw: {}", site_id, what,
              error.what());
  } catch (...) {
    Log_error("[RAFT-GUARD] Site {}: {} threw a non-std exception", site_id,
              what);
  }
  return false;
}


// @unsafe - Caller holds the state-machine apply gate followed by mtx_. The
// production callback must validate and stage without changing live state.
// RaftLab has no application state, so it validates a strict index+term marker.
// Was RaftServer::PrepareStateMachineSnapshotLocked; a file-local helper now,
// shared by raft_load_state_machine_snapshot and raft_install_snapshot_payload.
static std::unique_ptr<PreparedStateMachineSnapshotInstall>
prepare_state_machine_snapshot_locked(
    const rusty::RaftPrepareSnapshotCb* prepare_cb, uint16_t site_id,
    const std::string& data,
    uint64_t last_included_index,
    uint64_t last_included_term) {
  if (*prepare_cb) {
    std::unique_ptr<PreparedStateMachineSnapshotInstall> prepared;
    if (!raft_catch(site_id, "state-machine snapshot prepare", [&] {
          prepared = (*prepare_cb)(data, last_included_index);
        })) {
      Log_error("[RAFT-SNAPSHOT] Site {} prepare was for snapshot index={} "
                "term={}",
                site_id, last_included_index, last_included_term);
      return nullptr;
    }
    if (prepared == nullptr) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine prepare rejected "
                "snapshot index={} term={}",
                site_id, last_included_index, last_included_term);
    }
    return prepared;
  }

#ifdef RAFT_TEST_CORO
  constexpr size_t kMarkerSize = sizeof(uint64_t) * 2;
  if (data.size() != kMarkerSize) {
    Log_error("[RAFT-SNAPSHOT] Site {} RaftLab marker has {} bytes, expected {}",
              site_id, data.size(), kMarkerSize);
    return nullptr;
  }

  uint64_t marker_index = 0;
  uint64_t marker_term = 0;
  std::memcpy(&marker_index, data.data(), sizeof(marker_index));
  std::memcpy(&marker_term, data.data() + sizeof(marker_index),
              sizeof(marker_term));
  const bool matches = marker_index == last_included_index &&
                       marker_term == last_included_term;
  if (!matches) {
    Log_error("[RAFT-SNAPSHOT] Site {} RaftLab marker mismatch: "
              "payload=({}, {}) metadata=({}, {})",
              site_id, marker_index, marker_term,
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
            site_id, last_included_index, last_included_term);
  return nullptr;
#endif
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
// The typed communicator. commo_ is the generic Communicator* the TxLogServer
// interface hands every engine; RaftFrame::CreateCommo built it as a
// RaftCommo, and this is the one place that fact is recovered. It replaces the
// RaftServer::commo() method the kernels used to downcast the SERVER to reach.
// @unsafe - dynamic_cast on a pointer the frame owns; verify keeps the old
// abort-if-unset behaviour.
// Construct-in-place for the out-parameter kernels: destroy whatever the slot
// holds, then copy- or move-construct the new value there. Under the
// transpiler the slot is a live default-constructed object; under the runtime
// it is the zero bytes Rust default-constructs, or a value from an earlier
// round. Both are handled by the same two lines, which is why the kernels
// never plain-assign into a slot.
template <typename T, typename V>
static void construct_into(T* dst, V&& value) {
  std::destroy_at(dst);
  new (dst) T(std::forward<V>(value));
}

static RaftCommo* commo_of(rusty::Communicator* commo) {
  auto* communicator = dynamic_cast<RaftCommo*>(commo);
  verify(communicator != nullptr);
  return communicator;
}

extern "C" {

// (1) opaque-field access

// std::function's bool conversion, and its call.
bool raft_leader_change_cb_is_set(const rusty::RaftLeaderChangeCb* cb) {
  return static_cast<bool>(*cb);
}
void raft_fire_leader_change(const rusty::RaftLeaderChangeCb* cb,
                             bool is_leader) {
  (*cb)(is_leader);
}

// (1) the campaign broadcast, and the reply quorum read back under mtx_.
//
// Two kernels, not one, and deliberately: the broadcast SUSPENDS this fiber
// in wait_timeout, so it must run with mtx_ released, while the quorum's
// highest observed term must be sampled only after mtx_ is reacquired --
// FeedResponse publishes it before its wakeup. Reading the term inside the
// broadcast kernel would lose exactly that ordering.
void raft_broadcast_vote_and_wait(
    rusty::Communicator* commo, uint32_t par_id, uint64_t last_log_index,
    int64_t last_log_term, uint16_t self_site_id, int64_t term,
    rusty::RaftVoteQuorumPtr* out) {
  construct_into(out, commo_of(commo)->BroadcastVote(
                          par_id, last_log_index, last_log_term, self_site_id, term));
  (*out)->wait_timeout(1000000);
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
void raft_apply_thread_join(rusty::RaftStdThread* thread) {
  if (thread->joinable()) {
    thread->join();
  }
}

// (2) more downcalls into RaftServer methods that have not converted

void raft_commo_set_network_enabled(rusty::Communicator* commo, bool enabled) {
  commo_of(commo)->SetNetworkEnabled(enabled);
}
// @unsafe - Thread-safe PollThread::add bridge, the one reactor step of the
// wake path. `token` is a Box<GateWakeJob> made raw by
// RaftServerBase::queue_wake_job; the job hands it back to Rust exactly once,
// through the raft_wake_job_run export, which takes the Box and drops it.
// Nothing here names the server or the gate.
void raft_queue_wake_job(const rusty::RaftPollThreadPtr* owner,
                         rusty::ffi::c_void* token) {
  auto wake_job = rusty::Arc<OneTimeJob>::new_(
      OneTimeJob::new_([token]() { raft_wake_job_run(token); }));
  (*owner)->add(rusty::Arc<Job>(wake_job));
}
// A copy of a janus::Command -- a shared_ptr refcount the opaque Rust carrier
// cannot touch -- made here and handed back by value. Was the reason
// SetLocalAppend / raft_set_local_append had to be C++; the append is
// RaftServerBase::AppendLocal now.
void raft_command_clone_into(const rusty::RaftCommand* src,
                             rusty::RaftCommand* dst) {
  construct_into(dst, *src);
}
// D2: the setters' carriers, copied the same way -- a std::function or a
// shared_ptr, into the export's default-constructed slot -- so that no export
// takes a non-trivial C++ object by value.
void raft_learner_action_clone_into(const rusty::LearnerAction* src,
                                    rusty::LearnerAction* dst) {
  construct_into(dst, *src);
}
void raft_leader_change_cb_clone_into(const rusty::RaftLeaderChangeCb* src,
                                      rusty::RaftLeaderChangeCb* dst) {
  construct_into(dst, *src);
}
void raft_snapshot_manager_ptr_clone_into(const rusty::RaftSnapshotManagerPtr* src,
                                          rusty::RaftSnapshotManagerPtr* dst) {
  construct_into(dst, *src);
}
void raft_create_snapshot_cb_clone_into(const rusty::RaftCreateSnapshotCb* src,
                                        rusty::RaftCreateSnapshotCb* dst) {
  construct_into(dst, *src);
}
void raft_prepare_snapshot_cb_clone_into(const rusty::RaftPrepareSnapshotCb* src,
                                         rusty::RaftPrepareSnapshotCb* dst) {
  construct_into(dst, *src);
}
// RAFT_TEST_CORO as a predicate. Conditional compilation has no spelling in
// this dialect, so the flag is read here and the Rust callers branch on it:
// AppendLeaderNoop skips the no-op in lab mode (the suite counts entries),
// ConstructRuntime sets the lab's initial role.
bool raft_lab_mode() {
#ifdef RAFT_TEST_CORO
  return true;
#else
  return false;
#endif
}
void raft_noop_command_into(rusty::RaftCommand* dst) {
  auto noop = rusty::Arc<TpcNoopCommand>::make();
  construct_into(dst, janus::Command::pack_aliased<TpcNoopCommand>(std::move(noop)));
}
// Spawns the election-timer fiber. The lambda captures the loop by value --
// two words -- so nothing here outlives the fiber.
// ============================================================================
// FIBER-HOSTED RUST (plan.md, step E -- decided, not deferred)
//
// The three spawn kernels below run Rust-authored bodies on rrr fibers, and
// those bodies suspend mid-frame: ReplicationWakeGate::finish_wait_for_work
// and ::wait_for_election_timeout (IntEvent::wait_timeout), heartbeat phase
// 2's response-collection poll (raft_fiber_sleep_us), PrepareForShutdown's
// barrier yield. C++ keeps the scheduling; Rust keeps the loops. The
// alternative -- C++ owning every wait, Rust returning before each yield --
// would turn phase 2 into a resumable state machine and change nothing but
// risk in the protocol's timing.
//
// What makes a Rust frame on a fiber stack sound, and what the cutover must
// keep true:
//  1. A fiber is a stack switch on ONE OS thread (srpc_fiber.c); every site
//     has one PollThread, so a suspended frame resumes on the thread it
//     left. Rust's thread_local! is per OS thread and therefore stable.
//  2. No unwinding may cross the assembly switch. raft_catch is the only
//     catch on these paths and it catches on the C++ side; the raft crate
//     builds with panic = "abort" (Cargo.toml) so a Rust panic can never
//     try.
//  3. The stack budget is rrr's kDefaultStackBytes (1 MiB,
//     reactor.rs) with a PROT_NONE guard page below it (srpc_fiber.c:46):
//     an overflow faults at once, it does not corrupt. Rust's own
//     stack-overflow message will not appear, the SIGSEGV will.
// ============================================================================
void raft_spawn_election_timer(RaftServerBase* self, uint64_t wait_int_us) {
  Fiber::create_run([self, wait_int_us]() {
    raft_server_run_election_timer_loop(self, wait_int_us);
  });
}

// (3) more conditionally compiled or otherwise unspellable regions

// SetupInternal under the try/catch the C++ Setup wrapped it in. Exceptions
// have no DSL spelling in this dialect, and this one exists to turn a throwing
// setup into a failed startup rather than a crash.
bool raft_setup_internal_guarded(RaftServerBase* self, uint16_t site_id) {
  bool ok = false;
  if (!raft_catch(site_id, "setup", [&] {
        ok = raft_server_setup_internal(self);
      })) {
    return false;
  }
  return ok;
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

bool raft_prepare_snapshot_cb_is_set(const rusty::RaftPrepareSnapshotCb* cb) {
  return static_cast<bool>(*cb);
}

bool raft_env_snapshots_enabled() {
  const char* raw = std::getenv("MAKO_RAFT_SNAPSHOTS");
  return raw != nullptr &&
         (strcmp(raw, "1") == 0 || strcmp(raw, "true") == 0);
}

// 0 unset, 1 parsed into *out, 2 present but unparseable (logged here,
// where the raw string is).

// Memory-only Raft has no on-disk snapshot store. A manager injected through
// SetSnapshotManager() before Setup keeps the latest snapshot it holds;
// otherwise start from an empty in-memory manager.
void raft_snapshot_recovery_pick_manager(
    const rusty::RaftSnapshotManagerPtr* current,
    rusty::RaftSnapshotManagerPtr* out) {
  if (*current) {
    construct_into(out, *current);
    return;
  }
  construct_into(out, std::make_shared<janus::raft::MemorySnapshotManager>());
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

// Startup helper for a snapshot already held by the manager: prepares and
// immediately commits its state-machine image before publishing recovery.
// Caller holds state_machine_apply_mtx_ then mtx_. Was
// RaftServer::LoadStateMachineSnapshotLocked.
bool raft_load_state_machine_snapshot(
    const rusty::RaftPrepareSnapshotCb* prepare_cb, uint16_t site_id,
    const rusty::RaftByteString* data, uint64_t last_included_index,
    uint64_t last_included_term) {
  auto prepared = prepare_state_machine_snapshot_locked(
      prepare_cb, site_id, *data, last_included_index, last_included_term);
  if (prepared == nullptr) {
    return false;
  }
  bool committed = false;
  if (!raft_catch(site_id, "state-machine snapshot commit", [&] {
        committed = prepared->Commit();
      })) {
    Log_error("[RAFT-SNAPSHOT] Site {} commit was for snapshot index={} "
              "term={}",
              site_id, last_included_index, last_included_term);
    return false;
  }
  return committed;
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
int raft_install_snapshot_payload(
    const rusty::RaftPrepareSnapshotCb* prepare_cb,
    const rusty::RaftSnapshotManagerPtr* snapshot_manager, uint16_t site_id,
    uint64_t last_included_index, uint64_t last_included_term,
    const rusty::RaftByteString* data) {
  Log_info("[INSTALL-SNAPSHOT] Site {}: Preparing state machine snapshot ({} bytes)",
           site_id, data->size());
  auto prepared_state_machine = prepare_state_machine_snapshot_locked(
      prepare_cb, site_id, *data, last_included_index, last_included_term);
  if (prepared_state_machine == nullptr) {
    return 0;
  }

  const bool saved = (*snapshot_manager)->TakeSnapshot(
      last_included_index, last_included_term, data->data(), data->size());
  if (!saved) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              site_id, last_included_index, last_included_term);
    // The transaction has not committed, so its destructor discards only the
    // private staging image; the old live state machine and log remain usable.
    return 1;
  }
  Log_info("[INSTALL-SNAPSHOT] Site {}: Snapshot saved at index={} term={}",
           site_id, last_included_index, last_included_term);

  if (!prepared_state_machine->Commit()) {
    Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to commit prepared state "
              "machine snapshot at index={} term={}; failing stop",
              site_id, last_included_index, last_included_term);
    return 2;
  }
  Log_info("[INSTALL-SNAPSHOT] Site {}: State machine committed at index={} "
           "after Raft snapshot publication",
           site_id, last_included_index);
  return 3;
}

// (2) the apply thread's callback invocation.

// The learner callback, under the catch-all the C++ wrapped it in. An
// internal no-op is consumed without reaching the application. Returns false
// when the callback threw, which fails the server stop.
bool raft_apply_invoke(const rusty::LearnerAction* app_next,
                       const rusty::RaftCommand* pending, uint16_t site_id,
                       uint64_t id) {
  if (!raft_catch(site_id, "apply callback", [&] {
        if (pending->kind_ != TpcNoopCommand::static_kind()) {
          (*app_next)(id, *pending);
        }
      })) {
    Log_error("[RAFT-APPLY] Site {} callback failed at slot {}",
              site_id, id);
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


// getenv, and nothing else. The PARSE is Rust (raft_env_u64 in server.h),
// which is why these three readers no longer need a try/catch: std::stoull
// throws on malformed input, and a Rust digit loop returns Err.
//
// `which` is an i32 rather than a name, so nothing has to carry a Rust &str
// into C++ -- rusty::ffi::CStr is mapped by the transpiler but NOT
// implemented in the C++ runtime, so a DSL body cannot receive a C string.
const char* raft_env_lookup(int32_t which) {
  const char* raw = nullptr;
  switch (which) {
    case 0: raw = std::getenv("MAKO_RAFT_HEARTBEAT_INTERVAL_US"); break;
    case 1: raw = std::getenv("MAKO_RAFT_LOG_RETENTION_WINDOW"); break;
    case 2: raw = std::getenv("MAKO_RAFT_SNAPSHOT_INTERVAL"); break;
    default: verify(false); break;
  }
  if (raw == nullptr || raw[0] == '\0') {
    return nullptr;
  }
  return raw;
}

// Binds the wake gate to the communicator's PollThread before HeartbeatLoop
// can publish its owner-thread-only IntEvent. The communicator always
// retains the PollThread it created or was given.
bool raft_bind_replication_poll(RaftServerBase* self,
                                rusty::Communicator* commo) {
  // commo_of verifies the communicator is set, exactly as the
  // RaftServer::commo() it replaces did, so the null test that used to sit
  // here could never be false.
  rusty::Option<rusty::Arc<rrr::PollThread>> replication_poll =
      commo_of(commo)->PollThread();
  if (replication_poll.is_none()) {
    return false;
  }
  rusty::Arc<rrr::PollThread> owner = replication_poll.unwrap();
  raft_server_bind_replication_wake_owner(self, &owner);
  return true;
}

// Initializes the snapshot manager and restores the exact state-machine bytes
// before publishing any recovered snapshot boundary. C++ for the exception
// boundary around the recovery. Was RaftServer::InitializeSnapshotManager.
bool raft_initialize_snapshot_manager(RaftServerBase* self, uint16_t site_id) {
  bool recovered = false;
  if (!raft_catch(site_id, "snapshot recovery", [&] {
        recovered = raft_server_initialize_snapshot_manager_locked(self);
      })) {
    raft_server_fail_stop(self);
    return false;
  }
  return recovered;
}

// The partition's membership from the static (yaml) config: sorted,
// de-duplicated site ids, read one index at a time by
// RaftServerBase::LoadCurrentConfig into ITS Vec. Startup only, so the set
// is rebuilt per call rather than cached.
static std::set<siteid_t> config_replica_sites(uint32_t partition_id) {
  auto config = Config::GetConfig();
  std::set<siteid_t> sorted_unique;
  for (auto& site : config->SitesByPartitionId(partition_id)) {
    sorted_unique.insert(site.id);
  }
  return sorted_unique;
}
uint64_t raft_config_replica_count(uint32_t partition_id) {
  return static_cast<uint64_t>(config_replica_sites(partition_id).size());
}
uint16_t raft_config_replica_site(uint32_t partition_id, uint64_t i) {
  const auto sites = config_replica_sites(partition_id);
  verify(i < sites.size());
  auto it = sites.begin();
  for (uint64_t k = 0; k < i; ++k) {
    ++it;
  }
  return *it;
}

// The std::thread construction, and nothing else: the running flag and the
// loop body are both Rust. Joinable on purpose -- see StartApplyThread.
void raft_spawn_apply_thread(RaftServerBase* self,
                             rusty::RaftStdThread* thread) {
  *thread = std::thread([self]() { raft_server_apply_thread_loop(self); });
}

// The async-RPC gate's back-pointer, cleared under the gate's own mutex.
// AsyncCallbackLifetime is a hand-written C++ struct holding a std::mutex,
// and the field is a std::shared_ptr to it, so neither half has a DSL
// spelling.
void raft_clear_async_callback_owner(
    const rusty::RaftAsyncCallbackLifetimePtr* lifetime) {
  std::lock_guard<std::mutex> lifetime_lock((*lifetime)->mutex);
  (*lifetime)->server = nullptr;
}

// Forward-declared because the emitter writes definitions in source order and
// Fiber-hosted Rust; the constraints are stated at raft_spawn_election_timer.
void raft_spawn_heartbeat_loop(RaftServerBase* self) {
  // The loop is Rust end to end now (heartbeat_loop_body); this is only the
  // fiber spawn, which has no DSL spelling.
  Fiber::create_run([self]() { raft_server_heartbeat_loop(self); });
}

// Fiber-hosted Rust; the constraints are stated at raft_spawn_election_timer.
void raft_spawn_election_timer_fiber(RaftServerBase* self) {
  Fiber::create_run([self]() { raft_server_start_election_timer(self); });
}

// CreateSnapshotLocked's state-machine checkpoint and its persistence: a
// std::string built by a std::function that may throw, an #ifdef fallback,
// and snapshot_manager_ I/O. CALLER MUST HOLD mtx_.
bool raft_snapshot_serialize_and_save(
    const rusty::RaftCreateSnapshotCb* create_cb,
    const rusty::RaftSnapshotManagerPtr* snapshot_manager, uint16_t site_id,
    uint64_t snap_index, int64_t snap_term) {
  // Production may compact only behind a real state-machine checkpoint.
  // RaftLab has no application state and uses a strict 16-byte index+term
  // marker instead.
  std::string state_data;
  if (*create_cb) {
    if (!raft_catch(site_id, "state-machine snapshot callback", [&] {
          state_data = (*create_cb)(snap_index);
        })) {
      return false;
    }
    if (state_data.empty()) {
      Log_error("[RAFT-SNAPSHOT] Site {} state-machine snapshot callback "
                "returned an empty checkpoint; retaining the log",
                site_id);
      return false;
    }
    Log_info("[RAFT-SNAPSHOT] Site {}: State machine snapshot callback produced {} bytes",
             site_id, state_data.size());
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
              site_id);
    return false;
#endif
  }

  const bool saved = (*snapshot_manager)->TakeSnapshot(
      snap_index, snap_term, state_data.data(), state_data.size());
  if (!saved) {
    Log_error("[RAFT-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              site_id, snap_index, snap_term);
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
void raft_log_set_is_leader_entry(uint16_t site_id, uint32_t loc_id,
                                  uint64_t term, bool prev_is_leader,
                                  bool new_is_leader) {
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site {} (loc {}) term {}: prev_is_leader={} new_is_leader={}",
           site_id, loc_id, term, prev_is_leader, new_is_leader);
#else
  (void)site_id;
  (void)loc_id;
  (void)term;
  (void)prev_is_leader;
  (void)new_is_leader;
#endif
}


// ConstructRuntime's three kernels: what the C++ constructor did that the
// generated constructor could not -- see RaftServerBase::ConstructRuntime.
// F2 slice 1c: the Rust logger's two kernels. The runtime facade formats the
// line itself and asks two things of rrr's logger (module rrr.logging, the
// transpiled src/rrr/base/logging.rs): is the level on, and here is a line.
// Levels are rrr's: ERROR 1, WARN 2, INFO 3, DEBUG 4. Line 0 and a null file,
// as rrr_log.h's Log_* templates pass. Unused by the transpiled build, whose
// Rust bodies log through the C++ templates in rust_log_shims.h.
bool raft_log_enabled(int32_t level) { return level <= rrr::Log::level_now(); }
void raft_log_line(int32_t level, const uint8_t* text, size_t len) {
  rrr::log_line(level, 0, nullptr,
                std::string(reinterpret_cast<const char*>(text), len));
}

// The header's small kernels (F2.6): see their declarations in server.h.
void raft_verify(bool condition) { verify(condition); }
void raft_mutex_lock(RaftCheckedMutex* mutex) {
  mutex->lock();
}
void raft_mutex_unlock(RaftCheckedMutex* mutex) {
  mutex->unlock();
}
void raft_std_mutex_lock(std::mutex* mutex) {
  mutex->lock();
}
void raft_std_mutex_unlock(std::mutex* mutex) {
  mutex->unlock();
}
uint64_t raft_time_now_us() { return Time::now(true); }
bool raft_snapshot_manager_is_set( const rusty::RaftSnapshotManagerPtr* manager) {
  return *manager != nullptr;
}
uint64_t raft_random_range_us(uint64_t low, uint64_t high) {
  return RandomGenerator::rand(low, high);
}
bool raft_election_debug_enabled() {
#ifdef RAFT_LEADER_ELECTION_DEBUG
  return true;
#else
  return false;
#endif
}

// F2 slice 1: the carriers' destructors, behind the Rust `Drop` impls in the
// runtime facade (src/rrr/rusty-rustc). Each runs the C++ destructor in
// place. On a default-constructed carrier -- all zero bytes, the empty state
// of every one of these types -- each is a no-op, which is what lets Rust
// default-construct a slot for an _into kernel and drop it unconditionally.
// Unused by the transpiled build; they are the runtime's, and they compile
// against the real types here so a drift is caught now.
void raft_destroy_command(rusty::RaftCommand* p) { std::destroy_at(p); }
void raft_destroy_response_ptr(rusty::RaftResponsePtr* p) { std::destroy_at(p); }
void raft_destroy_checked_mutex(rusty::RaftCheckedMutex* p) { std::destroy_at(p); }
void raft_destroy_async_callback_lifetime_ptr(rusty::RaftAsyncCallbackLifetimePtr* p) {
  std::destroy_at(p);
}
void raft_destroy_snapshot_manager_ptr(rusty::RaftSnapshotManagerPtr* p) { std::destroy_at(p); }
void raft_destroy_create_snapshot_cb(rusty::RaftCreateSnapshotCb* p) { std::destroy_at(p); }
void raft_destroy_prepare_snapshot_cb(rusty::RaftPrepareSnapshotCb* p) { std::destroy_at(p); }
void raft_destroy_std_mutex(rusty::RaftStdMutex* p) { std::destroy_at(p); }
void raft_destroy_leader_change_cb(rusty::RaftLeaderChangeCb* p) { std::destroy_at(p); }
// A joinable std::thread's destructor terminates the process; Shutdown joins
// the apply thread first, and this says so where it would otherwise be silent.
void raft_destroy_std_thread(rusty::RaftStdThread* p) {
  verify(!p->joinable());
  std::destroy_at(p);
}
void raft_destroy_vote_quorum_ptr(rusty::RaftVoteQuorumPtr* p) { std::destroy_at(p); }
void raft_destroy_byte_string(rusty::RaftByteString* p) { std::destroy_at(p); }
void raft_destroy_tpc_commit_ptr(rusty::RaftTpcCommitPtr* p) { std::destroy_at(p); }
void raft_destroy_learner_action(rusty::LearnerAction* p) { std::destroy_at(p); }
void raft_new_callback_lifetime(RaftServerBase* self,
                                rusty::RaftAsyncCallbackLifetimePtr* out) {
  construct_into(out, std::make_shared<AsyncCallbackLifetime>());
  (*out)->server = self;
}
uint64_t raft_heartbeat_interval_default() { return HEARTBEAT_INTERVAL; }
void raft_ensure_legacy_payload_registered() {
  EnsureLegacyRaftLogPayloadRegistered();
}

}  // extern "C"


// @unsafe - the Arc carriers' construction paths, for the rustc facade
// (rusty-rustc/src/lib.rs: raft_new_int_event and the two Clone impls). Each
// writes into storage the facade owns but has not constructed -- a
// MaybeUninit -- so these placement-new WITHOUT the destroy_at that
// construct_into runs first: there is no live value in the slot to destroy,
// and rusty::Arc has no empty state a zeroed slot could stand for. Under the
// transpiler the DSL reaches them through the C++ halves of the same facade
// functions (server.h, raft_construct_by), so both worlds run this code.
extern "C" void raft_create_int_event_into(rusty::RaftIntEventPtr* out) {
  new (out) rusty::RaftIntEventPtr(create_sp_int_event(1));
}
extern "C" void raft_int_event_clone_into(const rusty::RaftIntEventPtr* src,
                                          rusty::RaftIntEventPtr* dst) {
  new (dst) rusty::RaftIntEventPtr(*src);
}
extern "C" void raft_int_event_set(const rusty::RaftIntEventPtr* event, int32_t value) {
  (*event)->set(value);
}
extern "C" void raft_int_event_wait_timeout(const rusty::RaftIntEventPtr* event,
                                            uint64_t timeout_us) {
  (*event)->wait_timeout(timeout_us);
}
extern "C" void raft_poll_thread_clone_into(const rusty::RaftPollThreadPtr* src,
                                            rusty::RaftPollThreadPtr* dst) {
  new (dst) rusty::RaftPollThreadPtr(*src);
}
extern "C" void raft_destroy_int_event_ptr(rusty::RaftIntEventPtr* p) { std::destroy_at(p); }
extern "C" void raft_destroy_poll_thread_ptr(rusty::RaftPollThreadPtr* p) { std::destroy_at(p); }


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
bool raft_phase1_load_and_send_snapshot(
    rusty::Communicator* commo,
    const rusty::RaftSnapshotManagerPtr* snapshot_manager,
    const rusty::RaftAsyncCallbackLifetimePtr* lifetime,
    uint16_t self_site_id, uint32_t partition_id, uint64_t send_term,
    uint16_t site_id, size_t ord) {
  janus::raft::SnapshotMetadata snap_meta;
  std::string snap_data;
  if (!(*snapshot_manager)->LoadLatestSnapshot(&snap_meta, &snap_data)) {
    return false;
  }
  const uint64_t snap_last_idx = snap_meta.last_included_index;
  const uint64_t snap_last_term = snap_meta.last_included_term;
  auto callback_lifetime = *lifetime;
  commo_of(commo)->SendInstallSnapshot(
      site_id, partition_id,
      send_term, self_site_id,
      snap_last_idx, snap_last_term,
      snap_data,
      [callback_lifetime, site_id, self_site_id, ord, snap_last_idx,
       send_term](uint64_t follower_term) {
        // NEITHER LOCK IS TAKEN ABOVE THIS CHECK, and that is load-bearing
        // for two different reasons.
        //
        // This callback runs in TWO contexts. Normally the reactor invokes it
        // when the reply lands, with mtx_ not held. But
        // RaftCommo::SendInstallSnapshot invokes it INLINE, on the caller's
        // stack, when PeerForSite returns null (commo.cc:167-170) -- and that
        // caller is PHASE 1, which holds mtx_.
        //
        // mtx_: a recursive_mutex tolerated the re-entry; a plain std::mutex
        // would self-deadlock. Everything past this check is Rust
        // (RaftServerBase::InstallSnapshotReplyAccepted), which takes mtx_
        // itself, so the inline path never reaches it.
        //
        // callback_lifetime->mutex: this one USED TO BE ACQUIRED FIRST,
        // unconditionally, which made the inline path take it while holding
        // mtx_ -- the exact inverse of the documented order
        // (callback_lifetime->mutex -> state_machine_apply_mtx_ -> mtx_ ->
        // apply_queue_), against the asynchronous path below which takes it
        // and then mtx_. That is an ABBA pair. It was latent rather than live
        // because both contexts run on the one poll thread and a fiber
        // blocking on a std::mutex blocks that thread, so the two halves
        // cannot be in flight at once -- but a total order an existing call
        // site inverts is not a total order, and the residual risk of the
        // non-recursive mtx_ (a suspension inside a critical section;
        // docs/migration/raft/conversion-log.md section 2) is exactly what
        // would make it reachable.
        //
        // The inline path always passes follower_term == 0, so it takes the
        // branch below and returns having touched no state and taken no lock.
        // self_site_id is captured by value rather than read through the
        // server, so the diagnostic needs no lock either; it is written once
        // during Setup and never changes.
        if (follower_term == 0) {
          Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} snapshot response unavailable; retaining replication indices",
                   self_site_id, site_id);
          return;
        }
        std::lock_guard<std::mutex> lifetime_lock(callback_lifetime->mutex);
        auto* server = callback_lifetime->server;
        if (server == nullptr) {
          return;
        }
        raft_server_install_snapshot_reply_accepted(
            server, site_id, ord, snap_last_idx, send_term, follower_term);
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

// The leader's batch, per entry: is this log entry a TpcCommitCommand, and
// if so a copy of it stamped with its log term, for batch_buffer_ (Rust's).
bool raft_command_is_tpc_commit(const rusty::RaftCommand* cmd) {
  return marshallable_cast<TpcCommitCommand>(*cmd).is_some();
}
void raft_stamped_commit_into(const rusty::RaftCommand* cmd, int64_t term,
                              rusty::RaftTpcCommitPtr* out) {
  auto cur_cmd = marshallable_cast<TpcCommitCommand>(*cmd);
  verify(cur_cmd.is_some());
  // Into unconstructed storage (the facade's MaybeUninit), so placement-new
  // without construct_into's destroy_at: see raft_create_int_event_into.
  new (out) rusty::RaftTpcCommitPtr(
      rusty::Arc<TpcCommitCommand>::make(*cur_cmd.as_ref().unwrap()));
  out->get_mut().unwrap().term = term;
}

// Moves the buffered Arcs into one TpcBatchCommand; the buffer is Rust's, so
// it arrives as (pointer, count) and the caller clears it next round.
void raft_batch_finalize(rusty::RaftTpcCommitPtr* entries, size_t count,
                         rusty::RaftCommand* cmd_out) {
  TpcBatchCommand batch_local;
  for (size_t i = 0; i < count; ++i) {
    batch_local.AddCmd(std::move(entries[i]));
  }
  auto batch_cmd = rusty::Arc<TpcBatchCommand>::make(std::move(batch_local));
  *cmd_out = std::move(batch_cmd);
}

// The send itself. Non-blocking: it only initiates the async call. The
// response is a shared_ptr the transport's callback also holds, which is
// what keeps it alive; the pending table carries it opaquely.
void raft_phase1_send_append(
    rusty::Communicator* commo, uint16_t self_site_id, uint16_t site_id,
    uint32_t partition_id,
    bool is_leader, uint64_t term, uint64_t prev_log_index,
    uint64_t prev_log_term, uint64_t commit_index,
    const rusty::RaftCommand* cmd, uint64_t cmd_log_term,
    rusty::RaftResponsePtr* out) {
  construct_into(out, commo_of(commo)->SendAppendEntries2(
                          site_id, partition_id, -1, -1, is_leader, self_site_id, term,
                          prev_log_index, prev_log_term, commit_index, *cmd, cmd_log_term));
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


// ============================================================================
// THE RPC ENTRY POINTS' C++ HALF
//
// OnRequestVote, OnAppendEntries, OnInstallSnapshot and Start are RaftSpecific
// methods on RaftServerBase, Rust in server_h.rs. The first two reach their
// bodies in server_cc.rs directly (F2.7). What is left here is
// OnInstallSnapshot's std::mutex and its catch -- the one place an embedder
// throw becomes FailStop -- which are C++ by nature.
// ============================================================================

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server
 * should we exclude the execution of this function for leader? */
// @unsafe - external calls marked @external [safe], output pointer writes in @unsafe blocks
// The three kernels raft_on_append_entries still needs. None is a forwarder:
// all three decode or build the wire payload, which is a Marshallable
// hierarchy with no Rust spelling.
extern "C" {

// The AppendEntries payload, read per entry for RaftServerBase::
// AeDecodePayload and AeApplyIncoming. The payload is a janus::Command -- a
// Marshallable the DSL cannot look inside -- laundered as c_void by the
// service forwarder. A batch is opened ONCE (raft_wire_batch: the one
// marshallable_cast the old kernels made) and then indexed.
static const janus::Command& wire_command(const rusty::ffi::c_void* cmd_handle) {
  return *static_cast<const janus::Command*>(
      static_cast<const void*>(cmd_handle));
}
static const TpcBatchCommand& wire_batch(const rusty::ffi::c_void* batch_handle) {
  return *static_cast<const TpcBatchCommand*>(
      static_cast<const void*>(batch_handle));
}
bool raft_wire_is_batch(const rusty::ffi::c_void* cmd_handle) {
#ifdef RAFT_BATCH_OPTIMIZATION
  return wire_command(cmd_handle).kind_ == TpcBatchCommand::static_kind();
#else
  (void)cmd_handle;
  return false;
#endif
}
// The batch behind a payload raft_wire_is_batch accepted, or null when the
// cast fails. Borrowed from the payload: valid exactly as long as it is.
const rusty::ffi::c_void* raft_wire_batch(const rusty::ffi::c_void* cmd_handle) {
  const auto batch = marshallable_cast<TpcBatchCommand>(wire_command(cmd_handle));
  if (batch.is_none()) {
    return nullptr;
  }
  const TpcBatchCommand& borrowed = *batch.as_ref().unwrap();
  return static_cast<const rusty::ffi::c_void*>(
      static_cast<const void*>(&borrowed));
}
uint64_t raft_batch_len(const rusty::ffi::c_void* batch_handle) {
  return static_cast<uint64_t>(wire_batch(batch_handle).cmds_.size());
}
int64_t raft_batch_term_at(const rusty::ffi::c_void* batch_handle, uint64_t i) {
  return static_cast<int64_t>(wire_batch(batch_handle).cmds_[i]->term);
}
// The i-th entry as its own janus::Command: an Arc clone (the refcount the
// carrier cannot touch), wrapped exactly as RaftEntry used to receive it.
void raft_batch_command_into(const rusty::ffi::c_void* batch_handle,
                             uint64_t i, rusty::RaftCommand* dst) {
  construct_into(dst, janus::Command::pack_aliased<TpcCommitCommand>(
                          wire_batch(batch_handle).cmds_[i].clone()));
}
void raft_wire_command_clone_into(const rusty::ffi::c_void* cmd_handle,
                                  rusty::RaftCommand* dst) {
  construct_into(dst, wire_command(cmd_handle));
}


}  // extern "C"



// @unsafe - the InstallSnapshot exception boundary. The two locks it used to
// take are Rust's now (RaftServerBase::OnInstallSnapshot holds them across
// this call); what is left is raft_catch, the one place an embedder throw
// from the snapshot callbacks becomes a false return -- and FailStop, in Rust.
extern "C" bool raft_install_snapshot_guarded(RaftServerBase* self, uint16_t site_id,
                                   uint64_t term, uint64_t leader_id,
                                   uint64_t last_included_index,
                                   uint64_t last_included_term,
                                   const rusty::RaftByteString* data,
                                   uint64_t* term_out) {
  return raft_catch(site_id, "snapshot install", [&] {
    raft_server_on_install_snapshot_locked(self, term, leader_id,
                                           last_included_index,
                                           last_included_term, data, term_out);
  });
}

} // namespace janus
