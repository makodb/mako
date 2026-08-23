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
  MultiPaxosFrame(int mode);
  MultiPaxosCommo *commo_ = nullptr;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  Coordinator *CreateBulkCoordinator(Config *config, int benchmark);
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None) override;
  vector<rrr::ServiceProxy> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<rrr::PollThread> poll_thread_worker) override;
};

} // namespace janus
