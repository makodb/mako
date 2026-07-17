#pragma once

#include "../__dep__.h"
#include "../coordinator.h"
#include "../frame.h"
#include <rusty/arc.hpp>
#include <rusty/cell.hpp>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Config::GetConfig: [safe, () -> Config*],
//   Reactor::create_sp_event: [safe, () -> shared_ptr<IntEvent>],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>]
// }

namespace janus {

class RaftCommo;
class RaftServer;

// @safe - pure CoordinatorRaft phase/quorum helpers over copied scalar
// values. The CoordinatorRaft class keeps inherited phase_ state, raw
// server/commo back-pointers, and command orchestration in C++.
#if RUSTYCPP_RUST
pub fn coordinator_raft_phase_value(phase: i32, n_phase: i32) -> i32 {
    phase % n_phase
}

pub fn coordinator_raft_phase_is_prepare(phase: i32) -> bool {
    phase == 1
}

pub fn coordinator_raft_phase_is_accept(phase: i32) -> bool {
    phase == 2
}

pub fn coordinator_raft_phase_is_commit(phase: i32) -> bool {
    phase == 3
}

pub fn coordinator_raft_majority_count(n_replica: u32) -> u32 {
    (n_replica / 2) + 1
}
#endif
/*RUSTYCPP:GEN-BEGIN id=coordinator.1 version=1 rust_sha256=db86be189ce372c2ac19ed914ec6d050a6bd9cee73e201ecd6d08d0f79da48fa*/
inline int32_t coordinator_raft_phase_value(int32_t phase, int32_t n_phase);
inline bool coordinator_raft_phase_is_prepare(int32_t phase);
inline bool coordinator_raft_phase_is_accept(int32_t phase);
inline bool coordinator_raft_phase_is_commit(int32_t phase);
inline uint32_t coordinator_raft_majority_count(uint32_t n_replica);

inline int32_t coordinator_raft_phase_value(int32_t phase, int32_t n_phase) {
    return phase % n_phase;
}

inline bool coordinator_raft_phase_is_prepare(int32_t phase) {
    return phase == 1;
}

inline bool coordinator_raft_phase_is_accept(int32_t phase) {
    return phase == 2;
}

inline bool coordinator_raft_phase_is_commit(int32_t phase) {
    return phase == 3;
}

inline uint32_t coordinator_raft_majority_count(uint32_t n_replica) {
    return (n_replica / 2) + 1;
}
/*RUSTYCPP:GEN-END id=coordinator.1*/

// @unsafe - inherits from non-@interface Coordinator and keeps borrowed raw
// back-pointers into RaftFrame/RaftServer state.
class CoordinatorRaft : public Coordinator {
 public:
//  static ballot_t next_slot_s;
  // @unsafe - borrowed RaftServer back-pointer set by RaftFrame; CoordinatorRaft
  // does not own or delete it.
  RaftServer* svr_ = nullptr;
 private:
  enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3, FORWARD = 4 };
  const int32_t n_phase_ = 4;

  // @unsafe - C-style cast on borrowed Coordinator::commo_ pointer. The
  // communicator is owned by RaftFrame, not this coordinator.
  RaftCommo *commo() {
    // TODO fix this.
    verify(commo_ != nullptr);
    // @unsafe
    { return (RaftCommo *) commo_; }
  }
  bool in_submission_ = false; // debug;
  // removed `in_prepare_` and `in_accept`
  // debug-guard fields — neither was ever written or read in the
  // raft path (the comparable guards on the paxos / mencius
  // coordinators ARE used; CoordinatorRaft just had the shape
  // copied over).
  bool in_append_entries = false; // debug
  uint64_t minIndex = 0;
 public:
  // migrated from
  // `shared_ptr<Marshallable>` to `janus::Command`.
  Command cmd_{};
  CoordinatorRaft(uint32_t coo_id,
                        int32_t benchmark,
                        rusty::Option<rusty::Arc<ClientStatus>> client_status,
                        uint32_t thread_id);
  ballot_t curr_ballot_ = 1; // TODO
  uint32_t n_replica_ = 0;   // TODO
  slotid_t slot_id_ = 0;
  // Safe shared mutable counter - shares ownership with RaftFrame.
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_;
  // removed `uint64_t cmt_idx_ = 0;` —
  // declared but never written or read on CoordinatorRaft (the
  // sibling field on CoordinatorFpgaRaft IS live; this one was a
  // copy-paste that never got wired up).

  // @safe
  uint32_t n_replica() {
    verify(n_replica_ > 0);
    return n_replica_;
  }

  // @unsafe - raw pointer dereference svr_->
  bool IsLeader() ;
  // @unsafe - raw pointer dereference svr_->
  bool IsFPGALeader() ;

  // @unsafe - calls Log_warn (non-borrow-checked I/O), Uses Arc<Cell<T>> for safe shared mutable access
  slotid_t GetNextSlot() {
    // @unsafe { Log_warn is not borrow-checked }
    Log_warn("[RAFT] CoordinatorRaft::GetNextSlot called but not implemented for Raft");
    slot_id_ = slot_hint_->get();
    slot_hint_->set(slot_hint_->get() + 1);
    return 0;
  }

  // @safe
  uint32_t GetQuorum() {
    return coordinator_raft_majority_count(n_replica());
  }

  // @safe
  void DoTxAsync(TxRequest &req) override {}

  // @unsafe - raw pointer dereferences, shared_ptr, complex logic
  void Submit(const janus::Command& cmd,
              rusty::Function<void()> func = {},
              rusty::Function<void()> exe_callback = {}) override;

  // @unsafe - raw pointer dereference svr_->, address-of &
  void AppendEntries();
  // @safe - mutex and callback operations are bounded
  void Commit();
  // @safe - mutex and callback operations are bounded
  void LeaderLearn();

  // @safe
  void Reset() override {}
  // @unsafe - calls Log_warn (non-borrow-checked I/O)
  void Restart() override {
    // @unsafe { Log_warn is not borrow-checked }
    Log_warn("[RAFT] CoordinatorRaft::Restart called but not implemented for Raft");
  }

  // @unsafe - calls AppendEntries which is @unsafe
  void GotoNextPhase();
};

} //namespace janus
