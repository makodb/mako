#pragma once

#include "deptran/classic/coordinator.h"
#include "commo.h"

namespace janus {

// This Coordinator should be on Client Side

class CoordinatorRule : public CoordinatorClassic {
 public:
  enum Phase {INIT_END=0, DISPATCHED=1, WAITING_ORIGIN=2};
  bool fast_path_success_{false};
  bool coordinator_success_{false};
  shared_ptr<VecPieceData> sp_vpd_; // cmd

  // CommunicatorRule* commo_;
  // double margin_success_rate_;

  value_t result_;

  CoordinatorRule(uint32_t coo_id,
                  int32_t benchmark,
                  ClientControlServiceImpl *ccsi,
                  uint32_t thread_id);
  ~CoordinatorRule() {
  }
  // CommunicatorRule* commo();
  void GotoNextPhase() override;
  void BroadcastRuleSpeculativeExecute(int cmd_ver);
  void DispatchAsync(bool fastpath_broadcast_mode);
};

} // namespace janus
