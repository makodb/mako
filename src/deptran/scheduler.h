#pragma once

#include <functional>
#include <utility>

#include "constants.h"
#include "mako_commands.h"

namespace janus {

class Communicator;

// The replication engine interface.
//
// WHAT THIS USED TO BE, and why it changed. Until the Tranche 6 work in
// docs/migration/raft/cpp-refactor-plan.md, TxLogServer was not an interface
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
}  // namespace rusty

namespace janus {

// @interface - pure virtuals and a virtual destructor only; no state.
//
// The three methods are the ONLY things a worker does through a base pointer,
// enumerated from the call sites rather than guessed: raft_worker.cc:288-290,
// 374,444 and paxos_worker.cc:50-51,180,553. Everything else the workers want,
// they dynamic_cast for.
//
// locid_t/parid_t/siteid_t are #defines for uint32_t/uint32_t/uint16_t
// (constants.h:13-18), so the emitted uint32_t/uint16_t below are
// byte-identical to the previous hand-written signatures after preprocessing,
// and both engines' overrides still match.
#if RUSTYCPP_RUST
pub trait TxLogServer {
    fn set_site_identity(&mut self, loc_id: u32, site_id: u16, partition_id: u32);
    fn set_commo(&mut self, commo: *mut rusty::Communicator);
    fn reg_learner_action(&mut self, learner_action: rusty::LearnerAction);
}
#endif
/*RUSTYCPP:GEN-BEGIN id=deptran_scheduler.tx_log_server version=1 rust_sha256=477f1bdffafaade87ca2ea1a2b24182fff8d75a520bead0e63b7303c23eca4b2*/
class TxLogServer;

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
