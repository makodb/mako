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
  bool in_prepare_ = false; // debug
  bool in_accept = false; // debug
  bool in_append_entries = false; // debug
  uint64_t minIndex = 0;
 public:
  shared_ptr<Marshallable> cmd_{nullptr};
  CoordinatorRaft(uint32_t coo_id,
                        int32_t benchmark,
                        rusty::Option<rusty::Arc<ClientStatus>> client_status,
                        uint32_t thread_id);
  ballot_t curr_ballot_ = 1; // TODO
  uint32_t n_replica_ = 0;   // TODO
  slotid_t slot_id_ = 0;
  // Safe shared mutable counter - shares ownership with RaftFrame
  rusty::Arc<rusty::Cell<slotid_t>> slot_hint_;
  uint64_t cmt_idx_ = 0 ;

  // @safe
  uint32_t n_replica() {
    verify(n_replica_ > 0);
    return n_replica_;
  }

  // @unsafe - raw pointer dereference svr_->
  bool IsLeader() ;
  // @unsafe - raw pointer dereference svr_->
  bool IsFPGALeader() ;

  // @safe - Uses Arc<Cell<T>> for safe shared mutable access
  slotid_t GetNextSlot() {
    verify(0);
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
  void Submit(shared_ptr<Marshallable> &cmd,
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
  // @safe
  void Restart() override { verify(0); }

  // @unsafe - calls AppendEntries which is @unsafe
  void GotoNextPhase();
};

} //namespace janus
