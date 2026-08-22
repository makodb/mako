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
//   Reactor::create_sp_event: [safe, () -> rusty::Arc<IntEvent>],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>]
// }

namespace janus {

class RaftCommo;
class RaftServer;
// @unsafe - inherits from non-@interface Coordinator
class CoordinatorRaft : public Coordinator {
 public:
//  static ballot_t next_slot_s;
  RaftServer* svr_ = nullptr;
 private:
  enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3, FORWARD = 4 };
  const int32_t n_phase_ = 4;

  // @unsafe - C-style cast on raw pointer
  RaftCommo *commo() {
    // TODO fix this.
    verify(commo_ != nullptr);
    // @unsafe
    { return (RaftCommo *) commo_; }
  }
  bool in_submission_ = false; // debug;
  // removed `in_prepare_` and `in_accept`
  // debug-guard fields — neither was ever written or read in the
  // raft path (the comparable guards on the paxos coordinator are
  // used; CoordinatorRaft just had the shape
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
  // Safe shared mutable counter - shares ownership with RaftFrame
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_;
  // The former `cmt_idx_` field was never written or read.

  // @safe
  uint32_t n_replica() {
    verify(n_replica_ > 0);
    return n_replica_;
  }

  // @unsafe - raw pointer dereference svr_->
  bool IsLeader() ;

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
    return n_replica() / 2 + 1;
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
