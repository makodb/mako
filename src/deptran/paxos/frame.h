#pragma once
#include <rusty/arc.hpp>

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
  Executor *CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 ClientControlServiceImpl *ccsi,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  Coordinator *CreateBulkCoordinator(Config *config, int benchmark);
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None) override;
  vector<rrr::Service *> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<rrr::PollThread> poll_thread_worker,
                                           ServerControlServiceImpl *scsi) override;
};

} // namespace janus
