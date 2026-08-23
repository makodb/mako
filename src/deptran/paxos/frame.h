#pragma once
#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "commo.h"

namespace janus {

class MultiPaxosFrame : public Frame {
 private:
  slotid_t slot_hint_ = 1;
 public:
  MultiPaxosFrame() = default;
  MultiPaxosCommo *commo_ = nullptr;
  Coordinator *CreateCoordinator(cooid_t coo_id);
  Coordinator *CreateBulkCoordinator();
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(
      rusty::Option<rusty::Arc<rrr::PollThread>> poll = rusty::None) override;
  std::vector<rrr::ServiceProxy> CreateRpcServices(
      uint32_t site_id,
      TxLogServer *rep_sched,
      rusty::Arc<rrr::PollThread> poll_thread_worker) override;
};

} // namespace janus
