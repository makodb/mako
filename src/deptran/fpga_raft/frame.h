#pragma once

#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "commo.h"
#include "server.h"
#include <rusty/box.hpp>

namespace janus {

class FpgaRaftFrame : public Frame {
 private:
  slotid_t slot_hint_ = 1;
 public:
  FpgaRaftFrame(int mode);
  FpgaRaftCommo *commo_ = nullptr;
  /* TODO: have another class for common data */
  FpgaRaftServer *sch_ = nullptr;
  // removed `CreateExecutor` override —
  // `FpgaRaftExecutor` class deleted; the virtual is never called.
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>()) override;
  vector<srpc::ServiceProxy> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<srpc::PollThread> poll_thread_worker) override;
};

} // namespace janus
