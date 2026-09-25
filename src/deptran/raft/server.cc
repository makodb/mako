#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
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
#ifdef RAFT_TEST_CORO
#include "application_log.h"       // EncodeApplicationLog, for the lab Start payload
#include "../replication_log_entry.h"  // LogEntry, which mako_commands.h only forward-declares
#endif
#include "rust_facade_types.h"
#include "memory_snapshot_manager.hpp"
#include "quorum.hpp"

import std;
import rusty;   // rusty::BTreeSet is a btree_port C++20 module, not a header

// @external: {
//   srpc::RandomGenerator::rand_double: [safe, (double, double) -> double]
//   srpc::RandomGenerator::rand: [safe, (int, int) -> int]
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
//   srpc::Fiber::create_run: [safe, (...) -> owned]
//   srpc::Fiber::sleep: [safe, (int) -> void]
//   Reactor::create_sp_event: [safe, (...) -> owned]
//   Config::GetConfig: [safe, () -> *]
//   janus::TpcBatchCommand::AddCmds: [safe, (&'a mut, &'a mut) -> void]
//   std::this_thread::sleep_for: [safe, (...) -> void]
//   std::thread::joinable: [safe, (&'a) -> bool]
//   std::thread::join: [safe, (&'a mut) -> void]
//   std::thread::detach: [safe, (&'a mut) -> void]
//   srpc::IntEvent::set: [safe, (&'a mut, int) -> void]
//   srpc::IntEvent::wait: [safe, (&'a, int) -> void]
//   srpc::Event::wait: [safe, (&'a, int) -> void]
//   srpc::EventStatus::TIMEOUT: [safe, () -> int]
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
// File-local: shared by raft_load_state_machine_snapshot and
// raft_install_snapshot_payload, and by nothing else.
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

// The communicator, resolved from the server that owns it.
//
// WHY THE SERVER NO LONGER CARRIES THE POINTER. `commo_: *mut
// rusty::Communicator` was the ONE field of RaftServerBase's forty-eight that
// is not `Send` (src/deptran/raft/src/server_h.rs), and that single raw
// pointer is what stopped the Raft server satisfying `trait Service: Send +
// Sync`, which the Rust srpc lane requires of anything it dispatches to.
// Keeping it and asserting `unsafe impl Send` would have converted "probably
// fine on one poll thread" into a guarantee -- janus::Communicator holds
// `peers_` and `partition_peers_` as UNGUARDED std::maps (communicator.h:92-94).
// So the pointer stays on this side, where it always belonged, and Rust asks
// for it by server identity.
//
// THIS IS CHEAPER THAN WHAT IT REPLACES, AND THAT IS NOT INCIDENTAL. The old
// commo_of ran a `dynamic_cast` on EVERY call -- an `__dynamic_cast` per
// AppendEntries send. The cast now runs once per server, at bind time, and a
// send does one acquire load plus a hash find.
//
// WHY THE READ PATH TAKES NO LOCK. The obvious shape -- one std::mutex around
// one table -- would not have been an improvement: an uncontended mutex costs
// about what a single-inheritance `__dynamic_cast` costs, and in multi-shard
// single-process mode several Raft servers share a process, so their send
// paths would contend on one lock that the dynamic_cast never had. So the
// table is published, never mutated: writers build a new one and swap the
// pointer, readers walk whatever they loaded.
//
// THE RETIRED TABLE IS DELIBERATELY NOT FREED. A reader may still be walking
// it -- that is the point of publishing instead of mutating -- and reclaiming
// it safely would need epoch tracking to buy nothing. A write happens twice
// per server lifetime (set_commo, and raft_server_delete), so this retains a
// few hundred bytes per server per process run.
using CommoTable = std::unordered_map<const void*, RaftCommo*>;

static std::atomic<const CommoTable*>& server_commo_table() {
  static std::atomic<const CommoTable*> published{new CommoTable()};
  return published;
}

// Serialises writers against each other, never against a reader.
static std::mutex& server_commo_write_mutex() {
  static std::mutex mtx;
  return mtx;
}

// Called once per server from raft_bind_commo, before any send. The
// dynamic_cast is here and only here.
static void bind_commo_for(const void* server, rusty::Communicator* commo) {
  auto* communicator = dynamic_cast<RaftCommo*>(commo);
  verify(communicator != nullptr);
  std::lock_guard<std::mutex> guard(server_commo_write_mutex());
  auto* next = new CommoTable(
      *server_commo_table().load(std::memory_order_acquire));
  (*next)[server] = communicator;
  server_commo_table().store(next, std::memory_order_release);
}

static void unbind_commo_for(const void* server) {
  std::lock_guard<std::mutex> guard(server_commo_write_mutex());
  const CommoTable* current =
      server_commo_table().load(std::memory_order_acquire);
  if (current->find(server) == current->end()) {
    return;  // never bound, or unbound twice: nothing to publish.
  }
  auto* next = new CommoTable(*current);
  next->erase(server);
  server_commo_table().store(next, std::memory_order_release);
}

static RaftCommo* commo_of(const void* server) {
  const CommoTable* table =
      server_commo_table().load(std::memory_order_acquire);
  const auto it = table->find(server);
  verify(it != table->end());
  return it->second;
}

extern "C" {

// (0) the binding. A kernel, not an export: RaftServerBase::set_commo calls
// it, because the dynamic_cast that validates the communicator has no Rust
// spelling. The cast now runs ONCE per server instead of once per send.
void raft_bind_commo(RaftServerBase* s, rusty::Communicator* commo) {
  bind_commo_for(s, commo);
}

// Called from raft_server_delete, after Shutdown and before the free, so the
// key is dropped while it is still a live pointer. Shutdown reaches no kernel
// that resolves the communicator -- it stops the loops, joins the apply
// thread and logs -- so releasing the row ahead of it cannot strand a send.
void raft_unbind_commo(RaftServerBase* s) {
  unbind_commo_for(s);
}

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
    RaftServerBase* self, uint32_t par_id, uint64_t last_log_index,
    int64_t last_log_term, uint16_t self_site_id, int64_t term,
    rusty::RaftVoteQuorumPtr* out) {
  construct_into(out, commo_of(self)->BroadcastVote(
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

void raft_commo_set_network_enabled(RaftServerBase* self, bool enabled) {
  commo_of(self)->SetNetworkEnabled(enabled);
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
// cannot touch, so the bump has to happen on this side and the result is
// constructed into a slot Rust owns.
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

#ifdef RAFT_TEST_CORO
// What the lab harness needs from the payload system.
//
// A committed command's identity lives in the C++ payload registry --
// PayloadMember<MakoCommands, T>::KIND plus SerializableRegistry::reg<T> --
// so Rust can HOLD a janus::Command but cannot be a member of the set, and
// cannot build the std::function the learner interface takes.
//
// The harness does not need either capability in general. It needs one
// integer out of a committed command, one command built, and one callback
// installed. These are narrow on purpose: a Rust-side payload registry is a
// far larger change than the harness justifies.
//
// RAFT_TEST_CORO-guarded, so no production build sees them.

// The tx_id of a committed TpcCommitCommand, or -1 when the payload is
// anything else. That integer is the value the lab's agreement oracle
// compares: a command is "committed at index i on replica r" when r's apply
// callback saw this tx_id at slot i.
int64_t raft_lab_commit_tx_id(const rusty::RaftCommand* cmd) {
  const auto commit_cmd = marshallable_cast<TpcCommitCommand>(*cmd);
  if (commit_cmd.is_none()) {
    return -1;
  }
  return static_cast<int64_t>(commit_cmd.unwrap()->tx_id_);
}

// Wrap a Rust function as a LearnerAction. The std::function owns only the
// raw fn pointer, so there is nothing to keep alive on the Rust side and no
// lifetime to get wrong; the command is handed over as a borrowed pointer for
// the duration of the call and is not retained.
// `ctx` is the replica's locale id, captured the way the C++ lambda captures
// `svr` -- the callback has to know which replica applied.
void raft_lab_make_learner_action(
    uint64_t ctx,
    int32_t (*apply)(uint64_t ctx, uint64_t slot, const rusty::RaftCommand* cmd),
    rusty::LearnerAction* out) {
  construct_into(out, janus::LearnerAction(
      [ctx, apply](int slot, janus::Command md) -> int {
        return apply(ctx, static_cast<uint64_t>(slot),
                     reinterpret_cast<const rusty::RaftCommand*>(&md));
      }));
}

// The encode direction of the same registry problem. The lab's Start()
// appends a TpcCommitCommand carrying `tx_id` and an empty application log --
// transcribed from testconf.cc's Start, which built exactly this -- and
// building one needs PayloadMember<MakoCommands, TpcCommitCommand>::KIND.
// Rust holds the result as an opaque rusty::RaftCommand and drops it through
// the raft_destroy_command it already has.
void raft_lab_make_commit_command(int64_t tx_id, rusty::RaftCommand* out) {
  auto cmdptr = rusty::Arc<TpcCommitCommand>::make();
  LogEntry raw_log;
  verify(raft::EncodeApplicationLog(nullptr, 0, 0, &raw_log.log_entry));
  raw_log.length = static_cast<int>(raw_log.log_entry.size());
  {
    auto& mut_cmd = cmdptr.get_mut().unwrap();
    mut_cmd.tx_id_ = static_cast<int32_t>(tx_id);
    mut_cmd.cmd_ = rusty::Arc<LogEntry>::make(rusty::move(raw_log));
  }
  construct_into(out,
      janus::Command::pack_aliased<TpcCommitCommand>(rusty::move(cmdptr)));
}

// --- The snapshot manager, for the Rust lab harness ------------------------
//
// Everything a case compares across a snapshot operation, in one struct so a
// before/after comparison is one call. Declared here rather than in server.h's
// inline block because it is lab-only: no production build sees it.
struct RaftLabSnapshotProbe {
  bool present;
  uint64_t last_included_index;
  uint64_t last_included_term;
  uint64_t timestamp_ms;
  uint64_t size_bytes;
  // FNV-1a over the checksum STRING (SnapshotMetadata::checksum is a
  // std::string) and over the payload. A digest compares as well as a copy
  // for a before/after assertion and marshals nothing into Rust.
  uint64_t checksum_digest;
  uint64_t data_digest;
  uint64_t count;
};

namespace {
uint64_t lab_fnv1a(const std::string& bytes) {
  uint64_t digest = 0xcbf29ce484222325ull;
  for (const char byte : bytes) {
    digest ^= static_cast<unsigned char>(byte);
    digest *= 0x100000001b3ull;
  }
  return digest;
}
}  // namespace
//
// janus::raft::SnapshotManager is a C++ interface held as a shared_ptr, and
// the cases that rotate one (54-60, 69) need to create, seed and inspect it.
// Rust holds the shared_ptr as the opaque rusty::RaftSnapshotManagerPtr it
// already uses for the server's own field, so these are the verbs it cannot
// spell -- the same reason the production snapshot kernels next to them exist.

void raft_lab_new_snapshot_manager(rusty::RaftSnapshotManagerPtr* out) {
  construct_into(out, std::make_shared<janus::raft::MemorySnapshotManager>());
}

uint64_t raft_lab_snapshot_delete_all(
    const rusty::RaftSnapshotManagerPtr* manager) {
  if (!*manager) {
    return 0;
  }
  return static_cast<uint64_t>((*manager)->DeleteAllSnapshots());
}

// One call for everything a case compares across an operation: the metadata,
// a digest of the payload, and how many snapshots the manager holds. Test 58
// asserts five metadata fields, the bytes and the count are all unchanged; a
// digest says that as well as a copy would and does not marshal a std::string
// into Rust.
void raft_lab_snapshot_probe(const rusty::RaftSnapshotManagerPtr* manager,
                             RaftLabSnapshotProbe* out) {
  *out = RaftLabSnapshotProbe{};
  if (!*manager) {
    return;
  }
  janus::raft::SnapshotMetadata metadata;
  std::string data;
  if (!(*manager)->LoadLatestSnapshot(&metadata, &data)) {
    return;
  }
  out->present = true;
  out->last_included_index = metadata.last_included_index;
  out->last_included_term = metadata.last_included_term;
  out->timestamp_ms = metadata.timestamp_ms;
  out->size_bytes = static_cast<uint64_t>(metadata.size_bytes);
  out->checksum_digest = lab_fnv1a(metadata.checksum);
  out->data_digest = lab_fnv1a(data);
  out->count = static_cast<uint64_t>((*manager)->ListSnapshots().size());
}

// --- The state-machine snapshot callbacks, for the Rust lab harness --------
//
// Both are std::functions, one of which returns a
// std::unique_ptr<PreparedStateMachineSnapshotInstall> -- a C++ interface with
// a vtable, which Rust cannot implement. So the PROBES are C++ and the test
// LOGIC that installs them and reads their verdict is Rust.
//
// One probe is live at a time, which is all tests 58 and 60 need, so their
// observations are file statics rather than a handle Rust would have to own.

namespace {

std::atomic<bool> lab_reject_prepare_called{false};

// Test 60's oracle: a prepared install whose Commit succeeds only once the
// EXACT incoming snapshot is readable from the manager. That makes the
// Prepare -> TakeSnapshot -> Commit ordering a witnessed fact rather than an
// assumption. Anonymous-namespace, so it does not collide with test.cc's own
// copy while both harnesses exist.
class LabPublicationProbe final : public PreparedStateMachineSnapshotInstall {
 public:
  LabPublicationProbe(std::shared_ptr<janus::raft::SnapshotManager> manager,
                      uint64_t expected_index, uint64_t expected_term,
                      std::string expected_data,
                      std::atomic<bool>* commit_called,
                      std::atomic<bool>* commit_saw_published,
                      std::atomic<bool>* aborted_before_commit)
      : manager_(std::move(manager)),
        expected_index_(expected_index),
        expected_term_(expected_term),
        expected_data_(std::move(expected_data)),
        commit_called_(commit_called),
        commit_saw_published_(commit_saw_published),
        aborted_before_commit_(aborted_before_commit) {}

  ~LabPublicationProbe() override {
    if (!commit_attempted_ && aborted_before_commit_ != nullptr) {
      aborted_before_commit_->store(true, std::memory_order_release);
    }
  }

  bool Commit() override {
    if (commit_attempted_) {
      return false;
    }
    commit_attempted_ = true;
    janus::raft::SnapshotMetadata metadata;
    std::string data;
    const bool published = manager_ != nullptr &&
        manager_->LoadLatestSnapshot(&metadata, &data) &&
        metadata.last_included_index == expected_index_ &&
        metadata.last_included_term == expected_term_ &&
        data == expected_data_;
    if (commit_saw_published_ != nullptr) {
      commit_saw_published_->store(published, std::memory_order_release);
    }
    if (commit_called_ != nullptr) {
      commit_called_->store(true, std::memory_order_release);
    }
    return published;
  }

 private:
  std::shared_ptr<janus::raft::SnapshotManager> manager_;
  uint64_t expected_index_ = 0;
  uint64_t expected_term_ = 0;
  std::string expected_data_;
  std::atomic<bool>* commit_called_ = nullptr;
  std::atomic<bool>* commit_saw_published_ = nullptr;
  std::atomic<bool>* aborted_before_commit_ = nullptr;
  bool commit_attempted_ = false;
};

struct LabProbeState {
  std::shared_ptr<janus::raft::SnapshotManager> manager;
  std::atomic<bool> prepare_called{false};
  std::atomic<bool> prepare_saw_old_manager{false};
  std::atomic<bool> commit_called{false};
  std::atomic<bool> commit_saw_published{false};
  std::atomic<bool> aborted_before_commit{false};
};

LabProbeState lab_probe_state;

}  // namespace

// Test 58: a Prepare that refuses. A clean rejection must not publish bytes,
// compact the log, or fail-stop a healthy follower.
void raft_lab_make_reject_prepare_cbs(rusty::RaftCreateSnapshotCb* create_out,
                                      rusty::RaftPrepareSnapshotCb* prepare_out) {
  lab_reject_prepare_called.store(false, std::memory_order_release);
  construct_into(create_out, [](uint64_t) { return std::string(); });
  construct_into(prepare_out,
      [](const std::string&, uint64_t)
          -> std::unique_ptr<PreparedStateMachineSnapshotInstall> {
        lab_reject_prepare_called.store(true, std::memory_order_release);
        return nullptr;
      });
}

bool raft_lab_reject_prepare_called() {
  return lab_reject_prepare_called.load(std::memory_order_acquire);
}

// Test 60: the publication-ordering oracle. Commit succeeds only once the
// exact incoming snapshot is readable from the manager, so the flags below
// witness Prepare -> TakeSnapshot -> Commit rather than assuming it.
void raft_lab_make_probe_cbs(const rusty::RaftSnapshotManagerPtr* manager,
                             rusty::RaftCreateSnapshotCb* create_out,
                             rusty::RaftPrepareSnapshotCb* prepare_out) {
  lab_probe_state.manager = *manager;
  lab_probe_state.prepare_called.store(false, std::memory_order_release);
  lab_probe_state.prepare_saw_old_manager.store(false, std::memory_order_release);
  lab_probe_state.commit_called.store(false, std::memory_order_release);
  lab_probe_state.commit_saw_published.store(false, std::memory_order_release);
  lab_probe_state.aborted_before_commit.store(false, std::memory_order_release);
  construct_into(create_out, [](uint64_t) { return std::string(); });
  construct_into(prepare_out,
      [](const std::string& incoming_data, uint64_t incoming_index)
          -> std::unique_ptr<PreparedStateMachineSnapshotInstall> {
        constexpr size_t kMarkerSize = sizeof(uint64_t) * 2;
        if (incoming_data.size() != kMarkerSize) {
          return nullptr;
        }
        uint64_t marker_index = 0;
        uint64_t marker_term = 0;
        std::memcpy(&marker_index, incoming_data.data(),
                    sizeof(marker_index));
        std::memcpy(&marker_term,
                    incoming_data.data() + sizeof(marker_index),
                    sizeof(marker_term));
        if (marker_index != incoming_index) {
          return nullptr;
        }
        auto manager = lab_probe_state.manager;
        auto previous = manager->GetLatestSnapshot();
        const bool still_old = previous.is_none() ||
            previous.unwrap().last_included_index < incoming_index;
        lab_probe_state.prepare_saw_old_manager.store(
            still_old, std::memory_order_release);
        lab_probe_state.prepare_called.store(
            true, std::memory_order_release);
        return std::make_unique<LabPublicationProbe>(
            manager, incoming_index, marker_term, incoming_data,
            &lab_probe_state.commit_called,
            &lab_probe_state.commit_saw_published,
            &lab_probe_state.aborted_before_commit);
      });
}

// bit 0 prepare ran, 1 it saw the old manager, 2 commit ran, 3 commit saw the
// snapshot published, 4 a probe was dropped without committing.
uint32_t raft_lab_probe_flags() {
  uint32_t flags = 0;
  if (lab_probe_state.prepare_called.load(std::memory_order_acquire)) flags |= 1;
  if (lab_probe_state.prepare_saw_old_manager.load(std::memory_order_acquire)) flags |= 2;
  if (lab_probe_state.commit_called.load(std::memory_order_acquire)) flags |= 4;
  if (lab_probe_state.commit_saw_published.load(std::memory_order_acquire)) flags |= 8;
  if (lab_probe_state.aborted_before_commit.load(std::memory_order_acquire)) flags |= 16;
  return flags;
}

void raft_lab_probe_release() {
  lab_probe_state.manager.reset();
}

// Copy one manager's latest checkpoint into another. What
// InstallAndSeedSnapshotManager does when rotating a manager on a replica
// that has ALREADY compacted: a boundary means nothing without its exact
// bytes, so the checkpoint is copied rather than regenerated.
bool raft_lab_snapshot_copy_latest(const rusty::RaftSnapshotManagerPtr* src,
                                   const rusty::RaftSnapshotManagerPtr* dst) {
  if (!*src || !*dst) {
    return false;
  }
  janus::raft::SnapshotMetadata metadata;
  std::string data;
  if (!(*src)->LoadLatestSnapshot(&metadata, &data)) {
    return false;
  }
  return (*dst)->TakeSnapshot(metadata.last_included_index,
                              metadata.last_included_term,
                              data.data(), data.size());
}

// A std::string from Rust bytes, for the snapshot payloads test 58 and 59
// hand to OnInstallSnapshot. The carrier is opaque on the Rust side, so this
// is the only way to make one.
void raft_lab_byte_string_from(rusty::RaftByteString* out,
                               const char* data, size_t size) {
  construct_into(out, std::string(data, size));
}

#endif
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
void raft_noop_command_into(rusty::RaftCommand* dst) {
  auto noop = rusty::Arc<TpcNoopCommand>::make();
  construct_into(dst, janus::Command::pack_aliased<TpcNoopCommand>(std::move(noop)));
}
// Spawns the election-timer fiber. The lambda captures the loop by value --
// two words -- so nothing here outlives the fiber.
// ============================================================================
// FIBER-HOSTED RUST
//
// The three spawn kernels below run Rust-authored bodies on srpc fibers, and
// those bodies suspend mid-frame: ReplicationWakeGate::finish_wait_for_work
// and ::wait_for_election_timeout (IntEvent::wait_timeout), heartbeat phase
// 2's response-collection poll (raft_fiber_sleep_us), PrepareForShutdown's
// barrier yield. C++ keeps the scheduling; Rust keeps the loops. The
// alternative -- C++ owning every wait, Rust returning before each yield --
// would turn phase 2 into a resumable state machine and change nothing but
// risk in the protocol's timing.
//
// What makes a Rust frame on a fiber stack sound, and what must stay true:
//  1. A fiber is a stack switch on ONE OS thread (srpc_fiber.c); every site
//     has one PollThread, so a suspended frame resumes on the thread it
//     left. Rust's thread_local! is per OS thread and therefore stable.
//  2. No unwinding may cross the assembly switch. raft_catch is the only
//     catch on these paths and it catches on the C++ side; the raft crate
//     builds with panic = "abort" (Cargo.toml) so a Rust panic can never
//     try.
//  3. The stack budget is srpc's kDefaultStackBytes (1 MiB,
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
// CALLER MUST HOLD state_machine_apply_mtx_ then mtx_.
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
// which is why this needs no try/catch: std::stoull throws on malformed
// input, and a Rust digit loop returns Err.
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

// Binds the wake gate to the communicator's PollThread, which must happen
// before HeartbeatLoop publishes its owner-thread-only IntEvent. The
// communicator retains the PollThread it created or was given, so the
// binding outlives this call.
bool raft_bind_replication_poll(RaftServerBase* self) {
  // commo_of verifies the communicator is set, exactly as the
  // RaftServer::commo() it replaces did, so the null test that used to sit
  // here could never be false.
  rusty::Option<rusty::Arc<srpc::PollThread>> replication_poll =
      commo_of(self)->PollThread();
  if (replication_poll.is_none()) {
    return false;
  }
  rusty::Arc<srpc::PollThread> owner = replication_poll.unwrap();
  raft_server_bind_replication_wake_owner(self, &owner);
  return true;
}

// Initializes the snapshot manager and restores the exact state-machine bytes
// before publishing any recovered snapshot boundary. C++ for the exception
// boundary around the recovery: a throwing restore must fail the server, not
// leave a boundary advertised with no bytes behind it.
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
  // The loop itself is heartbeat_loop_body, in Rust; this is only the fiber
  // spawn, which has no DSL spelling.
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
// The Rust logger's two kernels. The runtime facade formats the
// line itself and asks two things of srpc's logger (module srpc.logging, the
// transpiled src/srpc/base/logging.rs): is the level on, and here is a line.
// Levels are srpc's: ERROR 1, WARN 2, INFO 3, DEBUG 4. Line 0 and a null file,
// as srpc_log.h's Log_* templates pass. Unused by the transpiled build, whose
// Rust bodies log through the C++ templates in rust_log_shims.h.
bool raft_log_enabled(int32_t level) { return level <= srpc::Log::level_now(); }
void raft_log_line(int32_t level, const uint8_t* text, size_t len) {
  srpc::log_line(level, 0, nullptr,
                std::string(reinterpret_cast<const char*>(text), len));
}

// The small kernels declared in server.h.
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

// The carriers' destructors, behind the Rust `Drop` impls in the
// runtime facade (src/srpc/rusty-rustc). Each runs the C++ destructor in
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

// PHASE 2 binds one of these per slot per poll pass, so the loop body names
// fields rather than indexing. It borrows: the carried Command reference is
// owned by the table's slot, which outlives the pass because only this loop
// releases slots and it does so after its last use.
struct PendingView {
  siteid_t follower_id;
  uint64_t sent_term;
  uint64_t sent_round;
  uint64_t sent_end_index;
  const janus::Command& cmd;
};

// The read-index authority ledger, owned by Rust.
//
// A rusty::Vec with the round id as a field, not a map keyed by round id. A
// map would buy nothing: the generations are few (bounded by rounds with
// replies still outstanding), they are created in ascending round order and
// scanned in that order, and every lookup has a round id the caller already
// holds. PeerTable is a Vec for the same reason, plus one more: rusty::BTreeMap's
// rustc model is not a faithful map.
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

// The round scope, owned by Rust: the values that outlive a phase but not a
// round. PHASE 0 establishes all of them; PHASE 1, 2 and 3 read them.
//
// Every read and write goes through a method, so the C++ phases cannot poke a
// field. The `&mut self` on those methods is a true exclusivity claim rather
// than a formality, because HeartbeatRoundState is reachable only
// from the heartbeat fiber -- SendAppendEntries2's completion callback
// captures [response, site_id] and nothing else (commo.cc:50), the
// InstallSnapshot callback aliases RaftServer rather than the round, and
// PHASE 2's pending_rpcs iterator closes before the only suspension point --
// so exclusive mutable access is genuinely exclusive, and a borrow check over
// it is checking something real.
//
// There is deliberately no nservers field: it would only ever equal
// round_config.size(), so nservers() derives it and the two cannot
// disagree.

// ==========================================================================
// PHASE 1's two unspellable regions.
//
// The loop around them is heartbeat_phase1_body, in Rust. These two are
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
    RaftServerBase* self,
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
  commo_of(self)->SendInstallSnapshot(
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
        // callback_lifetime->mutex: NOT acquired here. Acquiring it
        // unconditionally would make the inline path take it while holding
        // mtx_ -- the exact inverse of the documented order
        // (callback_lifetime->mutex -> state_machine_apply_mtx_ -> mtx_ ->
        // apply_queue_), against the asynchronous path below, which takes it
        // and then mtx_. That is an ABBA pair. It would be latent rather than
        // live, because both contexts run on the one poll thread and a fiber
        // blocking on a std::mutex blocks that thread, so the two halves
        // cannot be in flight at once -- but a total order one call site
        // inverts is not a total order, and a suspension inside a critical
        // section under the non-recursive mtx_ is exactly what would make it
        // reachable.
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
    RaftServerBase* self, uint16_t self_site_id, uint16_t site_id,
    uint32_t partition_id,
    bool is_leader, uint64_t term, uint64_t prev_log_index,
    uint64_t prev_log_term, uint64_t commit_index,
    const rusty::RaftCommand* cmd, uint64_t cmd_log_term,
    rusty::RaftResponsePtr* out) {
  construct_into(out, commo_of(self)->SendAppendEntries2(
                          site_id, partition_id, -1, -1, is_leader, self_site_id, term,
                          prev_log_index, prev_log_term, commit_index, *cmd, cmd_log_term));
}

}  // extern "C"

// AppendRespView is generated by the block above, so this kernel has to sit
// below it rather than with the rest of the bridge.
extern "C" {
// The three scalars of an srpc AppendEntries reply. The response object is a
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
// methods on RaftServerBase, Rust in server_h.rs; the first two reach their
// bodies in server_cc.rs directly. What is left here is OnInstallSnapshot's
// std::mutex and its catch -- the one place an embedder throw becomes
// FailStop -- which are C++ by nature.
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
