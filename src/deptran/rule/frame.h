#pragma once

#include "../frame.h"

namespace janus {

// [Jetpack] Note: This can only be created as Tx Frame, but not Replica Frame

class RuleFrame : public Frame {
 public:
  RuleFrame(int mode = MODE_RULE) : Frame(mode) {}
//   Executor *CreateExecutor(cmdid_t, TxLogServer *sched) override;
  Coordinator *CreateCoordinator(cooid_t coo_id,
                                 Config *config,
                                 int benchmark,
                                 rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                 uint32_t id,
                                 shared_ptr<TxnRegistry> txn_reg) override;
//   TxLogServer *CreateScheduler() override;
//   vector<srpc::Service *> CreateRpcServices(uint32_t site_id,
//                                            TxLogServer *dtxn_sched,
//                                            rusty::Arc<srpc::PollThreadWorker> poll_thread_worker,
//                                            ServerControlServiceImpl *scsi) override;
//   mdb::Row *CreateRow(const mdb::Schema *schema,
//                       vector<Value> &row_data) override;

//   shared_ptr<Tx> CreateTx(epoch_t epoch, txnid_t tid,
//                           bool ro, TxLogServer *mgr) override;

//   Communicator *CreateCommo(rusty::Arc<srpc::PollThreadWorker> poll_thread_worker = rusty::Arc<srpc::PollThreadWorker>()) override;
};

} // namespace janus
