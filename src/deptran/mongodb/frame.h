#pragma once

#include "../frame.h"

namespace janus {

class MongodbFrame : public Frame {
 public:
  MongodbFrame(int mode);
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

}