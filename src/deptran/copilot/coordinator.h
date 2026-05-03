#pragma once

#include "../__dep__.h"
#include "../coordinator.h"
#include "../frame.h"

namespace janus {

const uint64_t takeover_timeout_us = 600000;  // TODO: what to set here? // old Copilot number is 15000
const uint64_t finalize_timeout_us = 200000;

class CopilotCommo;
class CopilotServer;
struct CopilotData;
class CoordinatorCopilot : public Coordinator {
  ballot_t curr_ballot_ = 0;

  // Workstream N L10f-prep3c (2026-05-03): cmd_now_ migrated to
  // janus::Command.
  Command cmd_now_{};
  int current_phase_ = 0;

  bool fast_path_ = false;
  bool direct_commit_ = false;
  bool in_fast_takeover_ = false;
  bool done_ = false;

  // Workstream N Phase 4e-13: removed `uint64_t begin / fac / ac /
  // cmt` timing counters.  `cmt` was never written or read anywhere.
  // `begin` was reset 4 times to compute `fac` and `ac`, but those
  // were never read.  All four fields were dead state.

 private:
  CopilotCommo *commo() {
    // TODO: fix this (fix what?)
    verify(commo_);
    return (CopilotCommo *)commo_;
  }
  ballot_t makeUniqueBallot(ballot_t ballot);
  ballot_t pickGreaterBallot(ballot_t ballot);
  void initFastTakeover(shared_ptr<CopilotData>& ins);
  void clearStatus();

  inline int maxFail() {
    return (n_replica_ - 1) / 2;
  }
  inline uint32_t getQuorum() {
    return n_replica_ / 2 + 1;
  }

 public:
  CopilotServer* sch_ = nullptr;
  enum Phase : int { INIT_END = 0, PREPARE, FAST_ACCEPT, ACCEPT, COMMIT };
  CoordinatorCopilot(uint32_t coo_id,
  					         int32_t benchmark,
                     rusty::Option<rusty::Arc<ClientStatus>> client_status,
                     uint32_t thread_id);
  virtual ~CoordinatorCopilot();

  inline bool IsPilot() {
    return loc_id_ == 0;
  }

  inline bool IsCopilot() {
    return loc_id_ == 1;
  }

  void DoTxAsync(TxRequest &req) override {}

  void Submit(const janus::Command& cmd,
              rusty::Function<void()> func = {},
              rusty::Function<void()> exe_callback = {}) override;
  
  // Protocol operations
  void Prepare();
  void FastAccept();
  void Accept();
  void Commit();

  void Restart() override { verify(0); }

  void GotoNextPhase();

 public:
  uint32_t n_replica_ = 0;

  /* info of the current instance being coordinated */
  uint8_t is_pilot_ = 0;
  slotid_t slot_id_ = 0;
  // Workstream N Phase 4e-13: removed `slotid_t *slot_hint_ = nullptr;`
  // — assigned at `frame.cc:85` (`coord->slot_hint_ = &slot_hint_;`),
  // reset to `nullptr` at `coordinator.cc:432`, but never read.  The
  // frame-side write went away in the same commit.
  slotid_t dep_ = 0;
};

} // namespace janus
