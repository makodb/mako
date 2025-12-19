#pragma once

#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "commo.h"
#include "server.h"

namespace janus {

class FpgaRaftFrame : public Frame {
 private:
  slotid_t slot_hint_ = 1;
 public:
  FpgaRaftFrame(int mode);
  FpgaRaftCommo *commo_ = nullptr;
  /* TODO: have another class for common data */
  FpgaRaftServer *sch_ = nullptr;
  Executor *CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 ClientControlServiceImpl *ccsi,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>()) override;
  vector<rrr::Service *> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<rrr::PollThread> poll_thread_worker,
                                           ServerControlServiceImpl *scsi) override;
};

} // namespace janus
