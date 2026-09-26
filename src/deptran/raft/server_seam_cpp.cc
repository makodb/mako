// The C++ lane's runtime seam: the reactor- and socket-facing kernels the
// Raft core calls (src/deptran/raft/src), over the C++ srpc runtime.
//
// The core never names a runtime. It reaches fibers, events, the poll thread
// and the wire only through the extern "C" kernels below, and carries the
// runtime's handles as opaque carriers. This file is those kernels for the
// C++ lanes (MAKO_RAFT_LANE=hybrid|cpp); raft-rt's src/seam.rs is the same
// symbols over the Rust srpc runtime (MAKO_RAFT_LANE=rust). Exactly one of the
// two is linked. Everything that is Mako's rather than a runtime's -- the
// Command payload, the snapshot manager, the embedder callbacks -- stays in
// server.cc, which both lanes link.
//
// Moved here from server.cc unchanged, apart from InstallSnapshot, whose send
// is now raft_lane_send_install_snapshot (below) and whose reply handling is
// server.cc's raft_snapshot_reply_deliver/free.

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <rusty/rusty.hpp>
#include <rusty/mutex.hpp>
#include <rusty/sync/atomic.hpp>

#include "server.h"
#include "commo.h"
#include "../rcc_rpc.h"
#include "rust_facade_types.h"
#include "lane_kernels.h"
#include "quorum.hpp"

import std;
import rusty;

namespace janus {

// Construct-in-place for the out-parameter kernels: destroy whatever the slot
// holds, then construct the new value there (the same helper server.cc has).
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
// `peers_` and `partition_peers_` as UNGUARDED std::maps when this was written;
// stage 3d has since replaced all five of its members with one Rust-authored
// PeerRegistry, so that hazard is gone -- but it was real then.
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

void raft_fiber_sleep_us(uint64_t micros) {
  Fiber::sleep(static_cast<int>(micros < 1 ? 1 : micros));
}

void raft_destroy_response_ptr(rusty::RaftResponsePtr* p) { std::destroy_at(p); }

void raft_destroy_vote_quorum_ptr(rusty::RaftVoteQuorumPtr* p) { std::destroy_at(p); }

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

// The send half of raft_phase1_load_and_send_snapshot (server.cc), which has
// loaded the snapshot and built the reply context. With no peer, RaftCommo
// calls the callback inline with 0, which is what the host's deliver relies
// on to take no lock on that path. The context is freed when the last copy of
// the callback goes -- after it ran, or unrun if the send never left.
void raft_lane_send_install_snapshot(
    RaftServerBase* self, uint16_t site_id, uint32_t partition_id,
    uint64_t term, uint64_t leader_id, uint64_t last_included_index,
    uint64_t last_included_term, const uint8_t* data, size_t len, void* ctx) {
  auto owned = std::shared_ptr<void>(ctx, [](void* c) { raft_snapshot_reply_free(c); });
  commo_of(self)->SendInstallSnapshot(
      site_id, partition_id, term, leader_id, last_included_index,
      last_included_term,
      std::string(reinterpret_cast<const char*>(data), len),
      [owned](uint64_t follower_term) {
        raft_snapshot_reply_deliver(owned.get(), follower_term);
      });
}

}  // extern "C"

}  // namespace janus
