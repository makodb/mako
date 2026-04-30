#pragma once

#include "../__dep__.h"
#include "../coordinator.h"
#include "../classic/tpc_command.h"
#include "../frame.h"
#include <chrono>

namespace janus {

class MenciusCommo;
class CoordinatorMencius : public Coordinator {
 public:
  enum Phase { INIT_END = 0, PREPARE = 1, SUGGEST = 2, COMMIT = 3 };
  const int32_t n_phase_ = 4;

  MenciusCommo *commo() {
    // TODO fix this.
    verify(commo_ != nullptr);
    return (MenciusCommo *) commo_;
  }
  bool in_submission_ = false; // debug;
  bool in_prepare_ = false; // debug
  bool in_suggest = false; // debug
  // Workstream N Phase 4e-14: removed `bool in_forward = false;` —
  // declared but never written or read.
  shared_ptr<Marshallable> cmd_{nullptr};
  CoordinatorMencius(uint32_t coo_id,
                        int32_t benchmark,
                        rusty::Option<rusty::Arc<ClientStatus>> client_status,
                        uint32_t thread_id);
  ballot_t curr_ballot_ = 1; // TODO
  uint32_t n_replica_ = 0;   // TODO
  slotid_t slot_id_ = 0;
  slotid_t *slot_hint_ = nullptr;

  uint32_t n_replica() {
    verify(n_replica_ > 0);
    return n_replica_;
  }

  bool IsLeader(int slot_id_) {
    int n = n_replica();
    if ((slot_id_-1)%n!=this->loc_id_){
      Log_warn("IsLeader slot_id_:%d, slot_id_: %d, loc_id_:%d", (slot_id_-1)%n, slot_id_, this->loc_id_);
    }
    return (slot_id_-1)%n==this->loc_id_;
  }

  slotid_t GetNextSlot() {
    verify(0);
    verify(slot_hint_ != nullptr);
    slot_id_ = (*slot_hint_)++;
    return 0;
  }

  uint32_t GetQuorum() {
    return n_replica() / 2 + 1;
  }

  void DoTxAsync(TxRequest &req) override {}
  void Submit(shared_ptr<Marshallable> &cmd,
              rusty::Function<void()> func = {},
              rusty::Function<void()> exe_callback = {}) override;

  // Workstream N Phase 4e-31: removed `PickBallot()` declaration —
  // only call site was the now-deleted `Prepare()` (Phase 4e-30).
  void Submit();

  // Workstream N Phase 4e-30: removed `Prepare()` declaration —
  // body was `verify(0)`-tagged debug code; `GotoNextPhase` skips
  // the prepare phase entirely.
  void Suggest();
  void Commit();

  void Reset() override {}
  void Restart() override { verify(0); }

  void GotoNextPhase();
};

} //namespace janus
