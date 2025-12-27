#pragma once

#include "../__dep__.h"
#include "../frame.h"
#include "../constants.h"
#include "commo.h"
#include <rusty/box.hpp>

namespace janus {

class FrameCarousel : public Frame {
 public:
  FrameCarousel(int m=MODE_CAROUSEL) : Frame(MODE_CAROUSEL) {}
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry>) override;
  TxLogServer *CreateScheduler() override;
  mdb::Row *CreateRow(const mdb::Schema *schema,
                      vector<Value> &row_data) override;
  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::Option<rusty::Arc<PollThread>>()) override;

  shared_ptr<Tx> CreateTx(epoch_t epoch, txnid_t tid,
                          bool ro, TxLogServer *mgr) override;

  vector<rusty::Box<rrr::Service>> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<rrr::PollThread> poll_thread_worker) override;  
};
} // namespace janus
