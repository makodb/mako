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
// THE MUTEX IS GONE FROM HERE TOO, deliberately. `mtx_` was a
// std::recursive_mutex shared by both engines by inheritance -- 48 acquisitions
// in Raft, 4 in Paxos. Each server now owns its own, which is what lets Raft
// replace its recursive mutex with a single Mutex<RaftState> without touching
// Paxos (Tranche 5).
//
// @interface - pure virtuals and a virtual destructor only; no state.
class TxLogServer {
 public:
  using LearnerAction = std::function<int(int, Command)>;

  TxLogServer() = default;
  virtual ~TxLogServer();

  TxLogServer(const TxLogServer&) = delete;
  TxLogServer& operator=(const TxLogServer&) = delete;

  // The four things a worker does through this pointer. Enumerated from the
  // call sites rather than guessed: raft_worker.cc:288-290,374,444 and
  // paxos_worker.cc:50-51,180,553. Everything else the workers want, they
  // dynamic_cast for.
  virtual void SetSiteIdentity(locid_t loc_id,
                               siteid_t site_id,
                               parid_t partition_id) = 0;
  virtual void SetCommo(Communicator* commo) = 0;
  virtual void RegLearnerAction(LearnerAction learner_action) = 0;
};

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
  void SetSiteIdentity(locid_t loc_id, siteid_t site_id,             \
                       parid_t partition_id) override {              \
    loc_id_ = loc_id;                                                \
    site_id_ = site_id;                                              \
    partition_id_ = partition_id;                                    \
  }                                                                  \
  void SetCommo(Communicator* commo) override { commo_ = commo; }    \
  void RegLearnerAction(LearnerAction learner_action) override {     \
    app_next_ = std::move(learner_action);                           \
  }

}  // namespace janus
