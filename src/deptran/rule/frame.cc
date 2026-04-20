
#include "../__dep__.h"
#include "../command.h"
#include "../command_marshaler.h"
#include "../communicator.h"
#include "../rcc/row.h"
#include "frame.h"
#include "coordinator.h"
#include "tx.h"

namespace janus {

REG_FRAME(MODE_RULE, vector<string>({"rule"}), RuleFrame);

Coordinator *RuleFrame::CreateCoordinator(cooid_t coo_id,
                                           Config *config,
                                           int benchmark,
                                           rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                           uint32_t id,
                                           shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  auto *coord = new CoordinatorRule(coo_id,
                                     benchmark,
                                     std::move(client_status),
                                     id);
  coord->txn_reg_ = txn_reg;
  coord->frame_ = this;
  return coord;
}

// Executor *JanusFrame::CreateExecutor(uint64_t, TxLogServer *sched) {
//   verify(0);
//   return nullptr;
// }

// TxLogServer *JanusFrame::CreateScheduler() {
//   TxLogServer *sched = new SchedulerJanus();
//   sched->frame_ = this;
//   return sched;
// }

// vector<rrr::Service *>
// JanusFrame::CreateRpcServices(uint32_t site_id,
//                               TxLogServer *sched,
//                               rusty::Arc<rrr::PollThreadWorker> poll_thread_worker,
//                               ServerControlServiceImpl *scsi) {
//   return Frame::CreateRpcServices(site_id, sched, poll_thread_worker, scsi);
// }


} // namespace janus
