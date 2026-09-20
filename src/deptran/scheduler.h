#pragma once

#include <functional>
#include <string>
#include <utility>

#include "constants.h"
#include "mako_commands.h"

namespace janus {

class Communicator;

// The replication engine interface.
//
// WHAT THIS USED TO BE, and why it changed. Until the Tranche 6 work
// (docs/migration/raft/conversion-log.md section 1), TxLogServer was not an interface
// at all: it was six public data members (loc_id_, site_id_, app_next_,
// commo_, partition_id_, mtx_), one non-virtual method that assigned one of
// them, and a virtual destructor -- ZERO behavioural virtuals. RaftServer and
// PaxosServer inherited those fields and read them as their own.
//
// That is implementation inheritance, and implementation inheritance has no
// Rust spelling (precheck B7, gate G4). It is the reason RaftServer could not
// be moved into the DSL at all: `#[cpp_inherit]` lets a DSL-owned type inherit
// a C++ BASE, but only an interface-shaped one -- it cannot absorb a base's
// data members.
//
// So the data moved down into the two concrete servers, which now declare the
// same five fields under the same names (so no body anywhere had to change),
// and what remains here is a genuine interface: pure virtuals covering exactly
// what a worker does through a base pointer, and nothing else.
//
// THE MUTEX WAS NOT A MISTAKE, so do not read its removal as a correction of
// one. It was added in April 2016 (6651e5b78, "lock guard in multipaxos sched")
// to a base that was genuinely stateful -- it owned dtxns_, mdb_txns_,
// executors_, mdb_txn_mgr_, mode_ and recorder_ -- alongside the lock_guards
// that commit added to MultiPaxosSched's handlers. The lock lived with the data
// it protected, in the class that owned it. Correct.
//
// What changed is everything around it: transaction execution, MemDB ownership,
// epoch management and the retired Jetpack plane all left this hierarchy, and
// the mutex outlived every member it was introduced to guard. By then it was a
// lock in a class with no state, shared by declaration between two derived
// types whose state is mutually unrelated.
//
// The recursion is residue of the same decay: recursive_mutex supports a
// handler locking on entry and then calling other locking methods, which is
// what Raft's 24 nested re-acquisitions still are. With the lock owned by the
// type whose state it guards, a plain non-recursive Mutex<RaftState> becomes
// feasible.
//
// THE MUTEX IS GONE FROM HERE TOO, deliberately. `mtx_` was a
// std::recursive_mutex shared by both engines by inheritance -- 48 acquisitions
// in Raft, 4 in Paxos. Each server now owns its own, which is what lets Raft
// replace its recursive mutex with a single Mutex<RaftState> without touching
// Paxos (Tranche 5).
//
// The learner callback, at namespace scope rather than as a member typedef of
// TxLogServer: a DSL `pub trait` cannot declare a nested type, and nothing
// outside this header ever spelled it `TxLogServer::LearnerAction`.
using LearnerAction = std::function<int(int, Command)>;

}  // namespace janus

// Inline-mode type map for the interface below, the same mechanism
// src/deptran/raft/rust_facade_types.h uses and for the same reason: the
// emitter carries a Rust path into C++ verbatim, and inline mode has no
// `--type-map` to rewrite it. The canonical Rust names these two as
// `rusty::Communicator` and `rusty::LearnerAction` (modelled opaquely in
// src/rrr/rusty-rustc/src/lib.rs); these aliases are the C++ half.
//
// Aliases, deliberately, rather than a `use rusty::*;` glob in the DSL block.
// The glob emits `using namespace rusty;` INSIDE `namespace janus`, and this
// header is included by every replication translation unit -- pulling all of
// rusty into janus name lookup that widely is not worth saving two lines.
namespace rusty {
using Communicator = ::janus::Communicator;
using LearnerAction = ::janus::LearnerAction;
// Named by RaftSpecific's signatures below. They are Raft's, and they sit in
// the shared header only because RaftSpecific does (see the DSL block for
// why); the Rust side models them as opaque carriers in
// src/rrr/rusty-rustc/src/lib.rs, with the layout pinned in raft/server.h.
using RaftCommand = ::janus::Command;
using RaftLeaderChangeCb = ::std::function<void(bool)>;
using RaftByteString = ::std::string;
}  // namespace rusty

namespace janus {

// @interface - pure virtuals and a virtual destructor only; no state.
//
// TxLogServer's three methods are the ONLY things a worker does through a
// base pointer common to both engines, enumerated from the call sites rather
// than guessed: raft_worker.cc:288-290, 374,444 and paxos_worker.cc:50-51,
// 180,553.
//
// RaftSpecific is what the Raft workers and the Raft RPC service reach beyond
// that. It is declared HERE, in the engine-shared header, and not in
// raft/server.h, for one reason: the transpiler emits a supertrait as a C++
// base (`class RaftSpecific : public TxLogServer`) only when both traits are
// declared in the same carrier. RaftServerBase implements it with
// `#[cpp_inherit]`, so the struct's one C++ base is RaftSpecific and, through
// it, TxLogServer.
//
// locid_t/parid_t/siteid_t/slotid_t/ballot_t/bool_t are #defines for
// uint32_t/uint32_t/uint16_t/uint64_t/int64_t/int8_t (constants.h), so the
// fixed-width types below are byte-identical, after preprocessing, to the
// hand-written signatures they replaced.
#if RUSTYCPP_RUST
pub trait TxLogServer {
    fn set_site_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32);
    fn set_commo(&mut self, commo: *mut rusty::Communicator);
    fn reg_learner_action(&mut self, learner_action: rusty::LearnerAction);
}

// Submission admission result for the RaftWorker interface.  Memory-only Raft
// either rejects a command (not leader) or appends it; there is no durable
// append whose outcome could be unknown.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum RaftStartResult {
    REJECTED = 0,
    APPENDED = 1,
}

// Method names are the C++ names the workers, the service and the lab tests
// already call; renaming them to snake_case is a mechanical follow-up once
// nothing hand-written calls them.
#[allow(non_snake_case)]
#[allow(clippy::too_many_arguments)]
pub trait RaftSpecific: TxLogServer {
    // Lifecycle, as the worker drives it.
    fn EnsureSetup(&mut self);
    fn WaitForStartup(&mut self) -> bool;
    fn PrepareForShutdown(&mut self);
    // Leadership.
    fn IsLeader(&mut self) -> bool;
    fn GetLeaderHint(&mut self) -> u16;
    fn SetPreferredLeader(&mut self, site_id: u16);
    fn RegisterLeaderChangeCallback(&mut self, cb: rusty::RaftLeaderChangeCb);
    // Admission, as the RPC service checks it before every handler.
    fn IsRpcReady(&self) -> bool;
    fn IsDisconnected(&self) -> bool;
    // Replication entry: the worker's "replicate this command".
    fn Start(&mut self, cmd: &rusty::RaftCommand, index: *mut u64,
             term: *mut u64) -> RaftStartResult;
    // Inbound RPC, as the service decodes it off the wire.
    fn OnRequestVote(&mut self, lst_log_idx: u64, lst_log_term: i64,
                     can_id: u16, can_term: i64, reply_term: *mut i64,
                     vote_granted: *mut i8);
    fn OnAppendEntries(&mut self, leader_current_term: u64,
                       leader_site_id: u16, leader_prev_log_index: u64,
                       leader_prev_log_term: u64, leader_commit_index: u64,
                       cmd: &rusty::RaftCommand, leader_next_log_term: u64,
                       follower_append_ok: *mut u64,
                       follower_current_term: *mut u64,
                       follower_last_log_index: *mut u64);
    fn OnInstallSnapshot(&mut self, term: u64, leader_id: u64,
                         last_included_index: u64, last_included_term: u64,
                         data: &rusty::RaftByteString, term_out: *mut u64);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=deptran_scheduler.tx_log_server version=1 rust_sha256=7c92186db9aa6c20272a1bcc90ac230ec6abfadf737ad311ec653032b5d12699*/
enum class RaftStartResult : int32_t;
constexpr RaftStartResult RaftStartResult_REJECTED();
constexpr RaftStartResult RaftStartResult_APPENDED();
class TxLogServer;
class RaftSpecific;

enum class RaftStartResult : int32_t {
    REJECTED = 0,
    APPENDED = 1
};
inline constexpr RaftStartResult RaftStartResult_REJECTED() { return RaftStartResult::REJECTED; }
inline constexpr RaftStartResult RaftStartResult_APPENDED() { return RaftStartResult::APPENDED; }

class TxLogServer {
public:
    virtual ~TxLogServer() noexcept(false) {}
    virtual void set_site_identity(uint32_t loc_id, uint16_t site_id, uint32_t partition_id) = 0;
    virtual void set_commo(rusty::Communicator* commo) = 0;
    virtual void reg_learner_action(rusty::LearnerAction learner_action) = 0;
    TxLogServer(const TxLogServer&) = delete;
    TxLogServer& operator=(const TxLogServer&) = delete;
    TxLogServer(TxLogServer&&) = delete;
    TxLogServer& operator=(TxLogServer&&) = delete;
protected:
    TxLogServer() = default;
};

template <class U> class TxLogServerAdapter;
template <class U> class TxLogServerAdapterRef;
template <class U> class TxLogServerAdapterRefMut;

class RaftSpecific : public TxLogServer {
public:
    virtual ~RaftSpecific() noexcept(false) {}
    virtual void EnsureSetup() = 0;
    virtual bool WaitForStartup() = 0;
    virtual void PrepareForShutdown() = 0;
    virtual bool IsLeader() = 0;
    virtual uint16_t GetLeaderHint() = 0;
    virtual void SetPreferredLeader(uint16_t site_id) = 0;
    virtual void RegisterLeaderChangeCallback(rusty::RaftLeaderChangeCb cb) = 0;
    virtual bool IsRpcReady() const = 0;
    virtual bool IsDisconnected() const = 0;
    virtual RaftStartResult Start(const rusty::RaftCommand& cmd, uint64_t* index, uint64_t* term) = 0;
    virtual void OnRequestVote(uint64_t lst_log_idx, int64_t lst_log_term, uint16_t can_id, int64_t can_term, int64_t* reply_term, int8_t* vote_granted) = 0;
    virtual void OnAppendEntries(uint64_t leader_current_term, uint16_t leader_site_id, uint64_t leader_prev_log_index, uint64_t leader_prev_log_term, uint64_t leader_commit_index, const rusty::RaftCommand& cmd, uint64_t leader_next_log_term, uint64_t* follower_append_ok, uint64_t* follower_current_term, uint64_t* follower_last_log_index) = 0;
    virtual void OnInstallSnapshot(uint64_t term, uint64_t leader_id, uint64_t last_included_index, uint64_t last_included_term, const rusty::RaftByteString& data, uint64_t* term_out) = 0;
    RaftSpecific(const RaftSpecific&) = delete;
    RaftSpecific& operator=(const RaftSpecific&) = delete;
    RaftSpecific(RaftSpecific&&) = delete;
    RaftSpecific& operator=(RaftSpecific&&) = delete;
protected:
    RaftSpecific() = default;
};

template <class U> class RaftSpecificAdapter;
template <class U> class RaftSpecificAdapterRef;
template <class U> class RaftSpecificAdapterRefMut;
/*RUSTYCPP:GEN-END id=deptran_scheduler.tx_log_server*/

// The five fields that used to sit in TxLogServer, as a macro rather than a
// base class or a member struct.
//
// A member struct would have been cleaner C++, but it would have renamed every
// use: 164 `site_id_`, 20 `partition_id_`, 10 `loc_id_` and 6 `app_next_` in
// Raft alone would all have become `site_.site_id_` and so on -- ~200 edits
// whose only purpose is to satisfy a scoping rule, in the same change that
// moves ownership and locking. Flattening keeps every body byte-identical and
// leaves the fields as plain members of the concrete type, which is also the
// shape the DSL wants: a DSL-owned struct has fields, not an embedded base.
//
// The trade, stated plainly: this is a macro, and a macro is worse to read
// than a struct. It is confined to these two lines and two call sites.
#define TXLOG_SERVER_SITE_FIELDS()                       \
  locid_t loc_id_ = static_cast<locid_t>(-1);            \
  siteid_t site_id_ = static_cast<siteid_t>(-1);         \
  LearnerAction app_next_{};                             \
  Communicator* commo_ = nullptr;                        \
  parid_t partition_id_ = 0;

// The bodies of the three interface methods, identical in both engines.
#define TXLOG_SERVER_SITE_METHODS()                                  \
  void set_site_identity(locid_t loc_id, siteid_t site_id,             \
                       parid_t partition_id) override {              \
    loc_id_ = loc_id;                                                \
    site_id_ = site_id;                                              \
    partition_id_ = partition_id;                                    \
  }                                                                  \
  void set_commo(Communicator* commo) override { commo_ = commo; }    \
  void reg_learner_action(LearnerAction learner_action) override {     \
    app_next_ = std::move(learner_action);                           \
  }

}  // namespace janus
