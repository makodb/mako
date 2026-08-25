#pragma once

#include "../communicator.h"
#include "../frame.h"
#include "../constants.h"
#include "commo.h"
#include "server.h"
#include <rusty/box.hpp>

namespace janus {

class CoordinatorCopilot;
class CopilotFrame : public Frame {
  CopilotCommo *commo_ = nullptr;
  CopilotServer *sch_ = nullptr;

  slotid_t slot_hint_ = 1;

  void setupCoordinator(CoordinatorCopilot *coord, Config *config);

 public:
  CopilotFrame(int mode);
  virtual ~CopilotFrame();

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

  // Statistic
  uint32_t n_fast_accept_ = 0;
  uint32_t n_fast_path_ = 0;
  uint32_t n_regular_path_ = 0;
  uint32_t n_accept_ = 0;
  uint32_t n_commit_ = 0;
  uint32_t n_prepare_ = 0;

};

} // namespace janus