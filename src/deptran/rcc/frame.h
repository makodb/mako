#pragma once
#include <rusty/arc.hpp>
#include <rusty/box.hpp>

#include "../frame.h"
#include "../constants.h"
#include "commo.h"

namespace janus {

class FrameRococo : public Frame {
 public:
  FrameRococo(int m=MODE_RCC) : Frame(MODE_RCC) {}
  Executor *CreateExecutor(cmdid_t, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry>) override;
  TxLogServer *CreateScheduler() override;
  vector<srpc::ServiceProxy> CreateRpcServices(uint32_t site_id,
                                           TxLogServer *dtxn_sched,
                                           rusty::Arc<srpc::PollThread> poll_thread_worker)
  override;
  mdb::Row *CreateRow(const mdb::Schema *schema,
                      vector<Value> &row_data) override;

  shared_ptr<Tx> CreateTx(epoch_t epoch, txnid_t tid,
                          bool ro, TxLogServer *mgr) override;

  Communicator *CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None) override;

};
} // namespace janus
