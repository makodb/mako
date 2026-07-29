#pragma once

#include "../__dep__.h"
#include "../coordinator.h"
#include "../classic/tpc_command.h"
#include "../frame.h"
#include <chrono>

namespace janus {

class MultiPaxosCommo;
class BulkCoordinatorMultiPaxos;
class CoordinatorMultiPaxos : public Coordinator {
  public:
  enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3 };
  const int32_t n_phase_ = 4;

  MultiPaxosCommo *commo() {
    // TODO fix this.
    verify(commo_ != nullptr);
    return (MultiPaxosCommo *) commo_;
  }
  bool in_submission_ = false; // debug;
  bool in_prepare_ = false; // debug
  bool in_accept = false; // debug
  bool in_commit = false;
  // removed `bool in_forward = false;` —
  // declared but never written or read.
  // polymorphic command field
  // migrated from `shared_ptr<Marshallable>` to `janus::Command`.
  // Boundary calls into commo (which still takes
  // `shared_ptr<Marshallable>`) use `cmd_.inner_marshallable()`.
  Command cmd_{};
  // removed dead `vec_md`
  // field — declared but never written or read anywhere.
  CoordinatorMultiPaxos(uint32_t coo_id,
                        int32_t benchmark,
                        rusty::Option<rusty::Arc<ClientStatus>> client_status,
                        uint32_t thread_id);
  ~CoordinatorMultiPaxos() {
#ifdef LATENCY_DEBUG
    Log_info("coordinator loc_id_={}, client2leader_ 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", loc_id_, client2leader_.pct50(), client2leader_.pct90(), client2leader_.pct99());
    // Log_info("coordinator loc_id_={}, client2test_point_ 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", loc_id_, client2test_point_.pct50(), client2test_point_.pct90(), client2test_point_.pct99());
    Log_info("coordinator loc_id_={}, client2leader_send_ 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", loc_id_, client2leader_send_.pct50(), client2leader_send_.pct90(), client2leader_send_.pct99());
#endif
  }
  ballot_t curr_ballot_ = 1; // TODO
  uint32_t n_replica_ = 0;   // TODO
  slotid_t slot_id_ = 0;
  slotid_t *slot_hint_ = nullptr;

  uint32_t n_replica() {
    verify(n_replica_ > 0);
    return n_replica_;
  }

  bool IsLeader() {
    return this->loc_id_ == 0;
  }

  void assignCmd(const janus::Command& cmd) override {
    cmd_ = cmd;
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
  void Submit(const janus::Command& cmd,
              rusty::Function<void()> func = {},
              rusty::Function<void()> exe_callback = {}) override;

  // removed `PickBallot()` and `Forward()`
  // declarations — `PickBallot()` was used only by the now-deleted
  // `Prepare()`; `Forward()` was declared but never
  // defined (commented-out call site at coordinator.cc:157 also gone).
  void Submit();

  // removed `Prepare()` declaration —
  // the body was `verify(0)`-tagged debug code, and `GotoNextPhase`
  // skips the prepare phase entirely.  The commented-out `PrepareAck`
  // legacy callback signature is also gone now.
  void Accept();
  // removed commented-out
  // `// void AcceptAck(phase_t phase, Future *fu);` legacy decl.
  void Commit();

  void Reset() override {}
  void Restart() override { verify(0); }

  void GotoNextPhase();

  void set_slot(int slot) override {
    slot_id_ = slot;
  }
};

class BulkCoordinatorMultiPaxos : public CoordinatorMultiPaxos {
public:
    // shadow Command field (intentionally hides the
    // base class's `cmd_` for the bulk coordinator's separate state).
    Command cmd_{};
    // removed `Prepare()` declaration — the
    // method was dead (`GotoNextPhase` skips the prepare phase via a
    // `// Prepare();` comment, and no other caller exists).
    void Accept();
    void Commit();
    void GotoNextPhase();
    BulkCoordinatorMultiPaxos(uint32_t coo_id,
                          int32_t benchmark,
                          rusty::Option<rusty::Arc<ClientStatus>> client_status,
                          uint32_t thread_id);
    void BulkSubmit(const janus::Command& cmd,
                    rusty::Function<void()> func = {},
                    rusty::Function<void()> exe_callback = {}) override;
};

} //namespace janus
