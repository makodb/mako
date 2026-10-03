#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../tpc_command.h"
#include "../view.h"
#include "commo.h"
#include "raft_kernel_pods.h"
#include "snapshot_callbacks.h"   // PreparedStateMachineSnapshotInstall   // kernel result PODs, global C declarations
#include <deque>
#include <exception>
#include <condition_variable>
#include <memory>
#include <thread>
#include <atomic>
#include <rusty/move.hpp>   // rusty::clone in the generated FollowerProgress
#include <rusty/box.hpp>
#include <rusty/arc.hpp>
#include <rusty/condvar.hpp>
#include <rusty/mutex.hpp>   // rusty::Mutex, the startup gate
#include <rusty/num.hpp>
#include <rusty/array.hpp>   // rusty::len / rusty::is_empty in PeerTable
#include <rusty/ffi.hpp>   // rusty::ffi::c_void, the election loop opaque handle
// The rusty:: aliases for the srpc reactor types. server.cc has included this
// since the wake gate landed; the election timer block below is the first DSL
// in a HEADER to name one, so it must be visible here too. It MUST stay at
// global scope: included inside `namespace janus` it declares `janus::rusty`,
// which then shadows ::rusty for every lookup in the file. Its own ordering
// rule -- after the header that imports srpc.reactor -- is satisfied by
// commo.h above.
import rusty;   // rusty::Vec is a vec_port C++20 module, not a header
#include "rust_facade_types.h"
#include <rusty/rusty.hpp>   // rusty::to_string_view, emitted for &str parameters
#include <rusty/option.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/thread.hpp>
#include <rusty/vecdeque.hpp>   // rusty::VecDeque, RaftServerBase's apply queue
#include <type_traits>
#include <utility>
#include "snapshot_manager.hpp"
#include <mutex>   // std::mutex mtx_, declared below

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   Log_fatal: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Config::GetConfig: [safe, () -> Config*],
//   Reactor::create_sp_event: [safe, () -> rusty::Arc<IntEvent>],
//   Fiber::create_run: [safe, (...) -> void],
//   Fiber::sleep: [safe, (int) -> void],
//   RandomGenerator::rand_double: [safe, (double, double) -> double],
//   RandomGenerator::rand: [safe, (int, int) -> int],
//   Time::now: [safe, () -> uint64_t],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>],
//   strcmp: [safe, (const char*, const char*) -> int],
//   std::sort: [safe, (...) -> void],
//   std::max: [safe, (T, T) -> T],
//   std::min: [safe, (T, T) -> T],
//   std::stoull: [safe, (const string&) -> uint64_t],
//   std::stoll: [safe, (const string&) -> int64_t],
//   std::this_thread::sleep_for: [safe, (duration) -> void]
// }

namespace janus {

// PreparedStateMachineSnapshotInstall: raft/snapshot_callbacks.h.

#define INVALID_SITEID  ((siteid_t)-1)

static_assert(std::is_same_v<int, int32_t>);

}  // namespace janus

// The DSL blocks below name `crate::scheduler_h::{TxLogServer, RaftSpecific,
// RaftStartResult}` -- the Rust module path the raft crate extracts
// src/deptran/scheduler.h to -- so the emitted C++ says `::scheduler_h::X`.
// This is the same shim server.cc opens for its own cross-carrier
// references. It sits above the first block because RaftStartResult is used
// from the first block on.
namespace scheduler_h {
using ::janus::TxLogServer;
using ::janus::RaftSpecific;
using ::janus::RaftStartResult;
}  // namespace scheduler_h

namespace janus {


static_assert(std::is_same_v<std::underlying_type_t<RaftStartResult>, int>);
static_assert(std::is_trivially_copyable_v<RaftStartResult>);
static_assert(sizeof(RaftStartResult) == sizeof(int32_t));
static_assert(alignof(RaftStartResult) == alignof(int32_t));
static_assert(static_cast<int32_t>(RaftStartResult::REJECTED) == 0);
static_assert(static_cast<int32_t>(RaftStartResult::APPENDED) == 1);
static_assert(RaftStartResult{} == RaftStartResult::REJECTED);


// Pure scalar Raft decisions. Stateful sequencing, locks, persistence,
// callbacks, logging, and pointer access remain at their existing C++ call
// sites. `const fn` makes the generated C++ constexpr/implicitly inline.

// The sentinel belongs to Rust as RAFT_SERVER_INVALID_SITE_ID (server_h.rs),
// which is 65535; pin the C++ macro to it.
static_assert(static_cast<uint16_t>(INVALID_SITEID) == 65535);
// The predicates' compile-time tests live next to the predicates themselves
// as `const _: () = assert!(...)` items, which the emitter lowers back to
// static_asserts. What remains here is C++'s own.

// One log entry, owned by Rust.
//
// Two fields: one scalar and one opaque carrier. A grep for the Paxos entry's
// field names -- max_ballot_seen_, max_ballot_accepted_, accepted_cmd_,
// committed_cmd_ -- appears to find them here too, but every hit is PaxosData
// (paxos/server.h:18), a separate struct that happens to share the names.
//
// Each of the two is read at 13 sites in
// server.cc. The payload crosses as rusty::RaftCommand -- held, moved, handed
// to a kernel, never dereferenced from Rust -- which is the same opaque carry
// PendingTable already uses for this exact type.
//
// The payload's internal storage is a shared_ptr<Marshallable>; calls to APIs
// that take one go through cmd().inner_marshallable().
//
// Deliberately NO method returning &mut: an entry is written when it is
// constructed and never afterwards. That is what makes "modify an existing
// entry" unspellable rather than merely discouraged. A caller that wants a
// different term stamp takes a copy.

// The Raft log itself, owned by Rust.
//
// Fixed-size blocks plus the index of the first live entry, not a map.
//
// WHY DENSE. The log is contiguous by construction, and always has been. Only
// two paths insert. SetLocalAppend writes at last_log_index_ + 1. The
// AppendEntries overwrite loop writes a run whose first index is the first
// entry that is missing or term-conflicting -- which is either one past the
// local tail, or an index the same statement has just truncated back to.
// Every erase is a whole prefix or a whole suffix.
//
// That is an argument, not a measurement, so the conversion asserts it rather
// than assuming it: RaftLog::append returns the index it wrote and both call
// sites verify() it is the index they intended, so a gap aborts instead of
// appearing.
//
// WHY IT MATTERS. The log's extents are base() and base() + len() - 1,
// derived from the container rather than stored beside it. There is no
// separate min_active_slot_ or last_log_index_ to advance by hand at each
// write site, so there is nothing that can disagree with the container.
//
// WHY BLOCKS AND NOT ONE VECTOR. A single growing vector measures 2.0 points
// of saturation throughput worse than a map, and blows the tail out --
// p99 +20%, p999 +25%, max +52%. The mechanism is the
// doubling reallocation, which copies the whole log while holding mtx_, so
// the whole pipeline stalls for as long as the memcpy takes. Pre-reserving
// the vector recovered those 2 points and pushed p99 and max BELOW the map
// baseline, which is what identified it. Blocks get the same result without
// having to guess a capacity: a full block is never touched again.
//
// rusty::Vec specifically: it re-exports std::vec::Vec on the rustc side and
// is the real vec_port on the C++ side, so both are faithful. See PeerTable's
// note above for why rusty::BTreeMap is not an option.

#ifdef RAFT_TEST_CORO
#define HEARTBEAT_INTERVAL 100000
#else
#define HEARTBEAT_INTERVAL 5000
#endif


// @unsafe - inherits from non-@interface TxLogServer (individual methods are @safe)
// Per-follower replication progress.
//
// next_index_ and match_index_ are ONE map of a value type rather than two
// maps keyed identically. That removes the "find both, check both" dance at
// the reply site and makes the index arithmetic a method rather than five
// inline branches; the two indices cannot go out of step because there is
// only one entry to find.
//
// The map itself stays C++: rusty::BTreeMap's rustc facade has no new(), no
// remove() and no mutable get, so converting the container would cost more
// facade work than this step is worth. The VALUE is what carries the logic.

// The consensus state that mtx_ guards, owned by Rust.
//
// mtx_ guards its sibling members by CONVENTION: nothing in the C++ says which
// fields it covers, which is why a census found 24 members touched on both
// sides of it. Rust's Mutex<T> guards by OWNERSHIP -- it contains the data and
// lock() is the only way to reach it -- so the conversion needs the guarded
// fields gathered into one type first. This is that type, starting with the
// members measured to be touched ONLY under the lock.
//
// A plain member behind mtx_, not a rusty::Mutex<RaftCore>: that
// would require mtx_ to be non-recursive, since rusty::Mutex::lock() hands
// out a reference to the guarded data.
//
// Fields are public because the kernels reach them as core.field.

// The election timer loop, owned by Rust.
//
// This is the first loop in Raft whose control flow -- not merely its
// arithmetic -- lives in the DSL. The Rust body runs the whole `while` that
// used to sit inside StartElectionTimer's fiber lambda; C++ keeps only the
// eight kernels below, each of which is an operation that genuinely cannot
// cross: a lock, a private member read, an srpc logging macro, or a call into
// another RaftServer method.
//
// ON THE OPAQUE HANDLE. ElectionTimerLoop carries the server as
// `*mut core::ffi::c_void`, not as a pointer to a modelled RaftServer. That is
// deliberate and it is the whole safety argument. A Rust type that modelled
// RaftServer's fields would typecheck against a hand-written model while the
// apply thread and the submit edge mutate the same members concurrently,
// producing a green borrow check over an untrue premise. `c_void` makes that
// structurally impossible: Rust cannot dereference it, so every read of server
// state is forced through a kernel that takes the lock the way the old inline
// code did. The handle is carried, moved and handed back -- never followed.


// A std::mutex that remembers which thread holds it, so re-entering it
// aborts with a message instead of hanging.
//
// WHY THIS IS WORTH AN ATOMIC PER ACQUISITION. mtx_ is non-recursive, so
// re-entering it is a self-deadlock -- the thread waits for a lock only it
// can release. A static walk finds no function inside RaftServer that holds
// mtx_ and reaches another taking it.
//
// The gap is the two application-provided callbacks. CreateSnapshotLocked
// and PrepareStateMachineSnapshotLocked invoke embedder code with mtx_
// held, so a callback that calls back into this server deadlocks. No test
// can catch it: the only in-tree callbacks are pure. Without this, the
// symptom is a silent hang with no stack and no log line. With it, the
// symptom names the bug.
//
// Satisfies BasicLockable and Lockable, so std::lock_guard and
// std::unique_lock work on it unchanged.
class RaftCheckedMutex {
 public:
  void lock() {
    const std::thread::id self = std::this_thread::get_id();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ReportReentry();
    }
    inner_.lock();
    owner_.store(self, std::memory_order_relaxed);
  }

  bool try_lock() {
    const std::thread::id self = std::this_thread::get_id();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ReportReentry();
    }
    if (!inner_.try_lock()) {
      return false;
    }
    owner_.store(self, std::memory_order_relaxed);
    return true;
  }

  void unlock() {
    owner_.store(std::thread::id{}, std::memory_order_relaxed);
    inner_.unlock();
  }

 private:
  // @unsafe { writes to stderr and aborts }
  [[noreturn]] static void ReportReentry();

  std::mutex inner_{};
  std::atomic<std::thread::id> owner_{};
};

// If thread::id is not lock-free, std::atomic<> falls back to an internal
// lock and this check would cost far more than intended. Fail the build
// rather than quietly pay for it.
static_assert(std::atomic<std::thread::id>::is_always_lock_free,
              "RaftCheckedMutex assumes a lock-free atomic<thread::id>");

// Lifted out of RaftServer ahead of the struct conversion: a DSL struct
// cannot declare a nested type, and these two are plain data with no reason
// to be nested. AsyncCallbackLifetime keeps a back-pointer, so RaftServer is
// forward-declared above it.
class RaftServer;

// RPC futures can outlive the server during shutdown. Destruction nulls this
// shared gate after waiting for any callback already using it.
struct RaftServerBase;  // defined below, in the DSL block that owns it
struct AsyncCallbackLifetime {
  std::mutex mutex;
  RaftServerBase* server = nullptr;  // the callbacks only call base methods
};

// ---------------------------------------------------------------------------
// C++ spellings for RaftServerBase's field types.
//
// Inline mode has no --type-map, so a foreign type reaches C++ under the exact
// path the Rust spells -- the mechanism src/deptran/raft/rust_facade_types.h
// documents at length. These pairs belong in that header and cannot go there:
// every one aliases a type server.h itself declares, and rust_facade_types.h
// is included before those declarations exist.
//
// The Rust half is src/srpc/rusty-rustc/src/lib.rs, where each name is an
// opaque struct of the C++ size and alignment (the layout pins below). Rust
// can hold and move one, default-construct those whose C++ type has an empty
// state, and copy the Arc handles through their Clone; it cannot look inside.
// Each alias below is EXACTLY the type the hand-written member used, so this
// changes where the members are declared and nothing else about them.
// ---------------------------------------------------------------------------
}  // namespace janus

namespace rusty {
using RaftCheckedMutex = ::janus::RaftCheckedMutex;
using RaftAsyncCallbackLifetimePtr =
    ::std::shared_ptr<::janus::AsyncCallbackLifetime>;
#if MAKO_RAFT_LANE_RUST
// The guard (plan N1/N4). On the Rust lane the carrier holds an
// Arc<SnapshotStore> (raft-rt, rt/src/snapshot.rs), not a shared_ptr, so it is
// an opaque, copy-deleted 16-byte struct here: HOST C++ that dereferenced or
// copied it as a shared_ptr fails to compile instead of misreading an Arc.
// Every access goes through the SEAM kernels in lane_kernels.h; destruction
// through raft_destroy_snapshot_manager_ptr, which leaves it all-zero.
struct RaftRustSnapshotStorePtr;
}  // namespace rusty
extern "C" void raft_destroy_snapshot_manager_ptr(rusty::RaftRustSnapshotStorePtr* p);
namespace rusty {
struct alignas(8) RaftRustSnapshotStorePtr {
  unsigned char bytes_[16] = {};
  RaftRustSnapshotStorePtr() = default;
  RaftRustSnapshotStorePtr(const RaftRustSnapshotStorePtr&) = delete;
  RaftRustSnapshotStorePtr& operator=(const RaftRustSnapshotStorePtr&) = delete;
  ~RaftRustSnapshotStorePtr() { raft_destroy_snapshot_manager_ptr(this); }
};
using RaftSnapshotManagerPtr = RaftRustSnapshotStorePtr;
#else
using RaftSnapshotManagerPtr =
    ::std::shared_ptr<::janus::raft::SnapshotManager>;
#endif
// RaftCreateSnapshotCb and RaftPrepareSnapshotCb are aliased in scheduler.h,
// where RaftSpecific names them; their layout pins stay below.
using RaftStdMutex = ::std::mutex;
using RaftStdThread = ::std::thread;
using RaftVoteQuorumPtr = ::std::shared_ptr<::janus::RaftVoteQuorumEvent>;
// RaftLeaderChangeCb, RaftByteString and RaftCommand are aliased in
// scheduler.h, where RaftSpecific names them; their layout pins stay here.
// The batch buffer's ELEMENT. The buffer itself is a rusty::Vec owned by
// Rust; only what is inside each Arc stays opaque, because it is a wire type
// the marshalling layer owns.
using RaftTpcCommitPtr = ::rusty::Arc<::janus::TpcCommitCommand>;
using RaftIntEventPtr = ::rusty::Arc<::srpc::IntEvent>;
using RaftPollThreadPtr = ::rusty::Arc<::srpc::PollThread>;

// ---------------------------------------------------------------------------
// LAYOUT PINS. Each of these types has a rustc-side model in
// src/srpc/rusty-rustc/src/lib.rs that exists so a DSL body can NAME the field.
// The models carry the real size and alignment of the C++ type, because
// rustc compiles RaftServerBase: a model that understated a carrier's size
// would put every field after it at the wrong offset.
//
// These assertions are what keep the two halves honest. If a
// libc++ release changes one of these, the build stops here with the new
// number instead of silently reintroducing the divergence -- update the
// matching model, do not relax the assertion.
//
// What these pins do NOT cover, recorded so the remaining gap is not mistaken
// for zero: the rusty containers RaftServerBase also holds (rusty::Vec,
// rusty::VecDeque, rusty::Mutex, rusty::Condvar, rusty::Arc) have their own
// C++/Rust size differences inside the rusty runtime -- rusty::Vec is 48
// bytes in C++ against std::vec::Vec's 24 in Rust. Those are a rusty-cpp
// question, not a Raft one, and the struct's total size still diverges
// because of them.
// ---------------------------------------------------------------------------
static_assert(sizeof(RaftCheckedMutex) == 48 && alignof(RaftCheckedMutex) == 8);
static_assert(sizeof(RaftAsyncCallbackLifetimePtr) == 16 &&
              alignof(RaftAsyncCallbackLifetimePtr) == 8);
static_assert(sizeof(RaftSnapshotManagerPtr) == 16 &&
              alignof(RaftSnapshotManagerPtr) == 8);
static_assert(sizeof(RaftCreateSnapshotCb) == 48 &&
              alignof(RaftCreateSnapshotCb) == 16);
static_assert(sizeof(RaftPrepareSnapshotCb) == 48 &&
              alignof(RaftPrepareSnapshotCb) == 16);
static_assert(sizeof(RaftStdMutex) == 40 && alignof(RaftStdMutex) == 8);
static_assert(sizeof(RaftLeaderChangeCb) == 48 &&
              alignof(RaftLeaderChangeCb) == 16);
static_assert(sizeof(RaftStdThread) == 8 && alignof(RaftStdThread) == 8);
static_assert(sizeof(RaftVoteQuorumPtr) == 16 &&
              alignof(RaftVoteQuorumPtr) == 8);
static_assert(sizeof(RaftByteString) == 24 && alignof(RaftByteString) == 8);
static_assert(sizeof(LearnerAction) == 48 && alignof(LearnerAction) == 16);
static_assert(sizeof(RaftTpcCommitPtr) == 8 && alignof(RaftTpcCommitPtr) == 8);
static_assert(sizeof(RaftIntEventPtr) == 8 && alignof(RaftIntEventPtr) == 8);
static_assert(sizeof(RaftPollThreadPtr) == 8 && alignof(RaftPollThreadPtr) == 8);
// The two carriers declared in rust_facade_types.h rather than here.
static_assert(sizeof(RaftCommand) == 24 && alignof(RaftCommand) == 8);
static_assert(sizeof(RaftResponsePtr) == 16 && alignof(RaftResponsePtr) == 8);
}  // namespace rusty


namespace janus {

// The small kernels below are DECLARED here and defined in server.cc. They
// cannot be `extern "C" inline` bodies in the header: such a body is emitted
// only in a translation unit that uses it, and the only caller is Rust.
// @unsafe - the two halves of std::lock_guard<RaftCheckedMutex>, so a DSL
// body can hold mtx_ across a scope. mtx_ is opaque to Rust; these are the
// only operations on it a converted body performs.
extern "C" void raft_mutex_lock(RaftCheckedMutex* mutex);
extern "C" void raft_mutex_unlock(RaftCheckedMutex* mutex);

// @unsafe - the same two halves for a plain std::mutex. RaftServer has two
// left: state_machine_apply_mtx_, which a DSL body has to hold across a
// scope. Neither the startup gate nor the apply queue needs these any more --
// each lives inside a rusty::Mutex that owns what it guards.
extern "C" void raft_std_mutex_lock(std::mutex* mutex);
extern "C" void raft_std_mutex_unlock(std::mutex* mutex);

// @unsafe - Time::now is an srpc clock read; the argument is the
// microsecond-resolution flag every Raft call site already passes.
extern "C" uint64_t raft_time_now_us();

// raft_snapshot_manager_is_set: lane_kernels.h (a SEAM kernel since plan N4).

// @unsafe - RandomGenerator is external.
extern "C" uint64_t raft_random_range_us(uint64_t low, uint64_t high);

// @safe - RAFT_LEADER_ELECTION_DEBUG, as a value a DSL body can branch on.
//
// #[cfg] is dropped silently inside a DSL block, so an #ifdef-guarded log
// cannot be written there directly. Returning the flag instead keeps the log
// AT its call site, where its arguments are, and the compiler folds the
// branch away exactly as the preprocessor would -- the arguments are all
// scalars already in registers, so the #ifdef has nothing else to save.
extern "C" bool raft_election_debug_enabled();


// ReplicationWakeGate.
//
// The two wait entry points are SPLIT rather than moved wholesale, for one
// reason: creating an `IntEvent` calls the reactor factory
// `::srpc::create_sp_int_event`, which the DSL cannot name. The rustc facade
// exposes it only as `rusty::srpc::reactor::create_sp_int_event`, and inline
// mode has no `--type-map` to rewrite that path, so spelling it would require
// a nested `rusty::srpc::reactor` namespace in C++ merely to hold a factory.
// Instead the two wait entry points take the event as a PARAMETER and
// RaftServer creates it -- `WaitForReplicationOrHeartbeat` and
// `WaitForElectionTimeoutOrShutdown` in this file, which are the C++ kernels
// CLAUDE.md describes: the DSL owns the shape, C++ owns the surgery.
//
// The fast path is preserved exactly. `begin_wait_for_work` returns
// Some(answer) when it could decide without a waiter and None when the caller
// must arm one, so the path that decides immediately allocates no event.
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

// ============================================================================
// KERNEL RESULT PODS -- the three values kernels return by value -- are C
// declarations now, in raft_kernel_pods.h (included at the top of this file),
// owned by the canonical src/server_pods_h.rs.
// ============================================================================




// The heartbeat loop, owned by Rust.
//
// Same shape as ElectionTimerLoop above and for the same reason: the outer
// while, the lifecycle and the continuation decision are Rust; everything
// that touches shared RaftServer state is a C++ kernel behind an opaque
// handle. Two handles here rather than one, because the round-carried state
// (the pending-RPC table, the authority generations, the leader-term latch)
// outlives a round but not the loop, and holds unique_ptr and wire types that
// have no DSL spelling. Rust carries it and hands it back; it never looks in.
// The base clause the two `#[cpp_inherit]` impls above must produce. See the
// comment on `impl TxLogServer for RaftServerBase` for why this can fail
// silently without the assertion.

// The C ABI over the struct: every behaviour the shim and the kernels reach,
// as extern "C" functions defined in Rust. The header is generated by
// scripts/raft_gen_exports.py. It opens namespace janus itself, so the
// namespace is closed around the include.
// The server itself is Rust (src/deptran/raft/src/server_h.rs, libraft.a). C++
// holds it as a pointer and reaches it through server_exports.h alone.
struct RaftServerBase;

}  // namespace janus
#include "server_exports.h"
namespace janus {

// The C++ view of the Rust object.
//
// RaftServer does not derive from RaftServerBase: it holds the pointer Rust
// allocated (raft_server_new) and forwards every method to the C ABI in
// server_exports.h. It is what the workers and the RPC service hold, and it
// knows nothing of the struct's layout or methods -- which is what lets the
// struct be a Rust type without this class changing.
// Generated by scripts/raft_gen_exports.py --shim; re-run and diff when the
// interface or the lab surface changes.
class RaftServer : public RaftSpecific {
 public:
  RaftServer() : impl_(raft_server_new()) {}
  // @unsafe - thread join and timer cleanup require manual resource management
  ~RaftServer() { raft_server_delete(impl_); }

  // --- TxLogServer and RaftSpecific, forwarded to the C ABI.
  void set_site_identity(uint32_t loc_id, uint16_t site_id, uint32_t partition_id) override { raft_server_set_site_identity(impl_, loc_id, site_id, partition_id); }
  void set_commo(rusty::Communicator* commo) override { raft_server_set_commo(impl_, commo); }
  void reg_learner_action(const rusty::LearnerAction& learner_action) override { raft_server_reg_learner_action(impl_, &learner_action); }
  void EnsureSetup() override { raft_server_ensure_setup(impl_); }
  bool WaitForStartup() override { return raft_server_wait_for_startup(impl_); }
  void PrepareForShutdown() override { raft_server_prepare_for_shutdown(impl_); }
  bool IsLeader() override { return raft_server_is_leader(impl_); }
  uint16_t GetLeaderHint() override { return raft_server_get_leader_hint(impl_); }
  void SetPreferredLeader(uint16_t site_id) override { raft_server_set_preferred_leader(impl_, site_id); }
  void RegisterLeaderChangeCallback(const rusty::RaftLeaderChangeCb& cb) override { raft_server_register_leader_change_callback(impl_, &cb); }
  bool IsRpcReady() const override { return raft_server_is_rpc_ready(impl_); }
  uint16_t SiteId() const override { return raft_server_site_id(impl_); }
  uint32_t PartitionId() const override { return raft_server_partition_id(impl_); }
  uint64_t CommitIndex() const override { return raft_server_commit_index(impl_); }
  RaftStartResult Start(const rusty::RaftCommand& cmd, uint64_t* index, uint64_t* term) override { return raft_server_start(impl_, &cmd, index, term); }
  void ServeVote(uint64_t lst_log_idx, int64_t lst_log_term, uint16_t can_id, int64_t can_term, int64_t* reply_term, int8_t* vote_granted) override { raft_server_serve_vote(impl_, lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted); }
  void ServeAppendEntries(uint64_t leader_current_term, uint16_t leader_site_id, uint64_t leader_prev_log_index, uint64_t leader_prev_log_term, uint64_t leader_commit_index, const rusty::RaftCommand& cmd, uint64_t leader_next_log_term, uint64_t* follower_append_ok, uint64_t* follower_current_term, uint64_t* follower_last_log_index) override { raft_server_serve_append_entries(impl_, leader_current_term, leader_site_id, leader_prev_log_index, leader_prev_log_term, leader_commit_index, &cmd, leader_next_log_term, follower_append_ok, follower_current_term, follower_last_log_index); }
  void ServeInstallSnapshot(uint64_t term, uint64_t leader_id, uint64_t last_included_index, uint64_t last_included_term, const rusty::RaftByteString& data, uint64_t* term_out) override { raft_server_serve_install_snapshot(impl_, term, leader_id, last_included_index, last_included_term, &data, term_out); }
  uint64_t SetStateMachineSnapshotCallbacks(const rusty::RaftCreateSnapshotCb& create_cb, const rusty::RaftPrepareSnapshotCb& prepare_cb) override { return raft_server_set_state_machine_snapshot_callbacks(impl_, &create_cb, &prepare_cb); }

  // The Rust object itself, for the one caller that must hand it to
  // Rust rather than forward a method: the Rust lane's transport binds
  // to it (raft_lane_rust.cc). Not part of RaftSpecific.
  RaftServerBase* impl() const { return impl_; }

 private:
  RaftServerBase* impl_;
};

} // namespace janus
