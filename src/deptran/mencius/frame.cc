#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "exec.h"
#include "coordinator.h"
#include "server.h"
#include "service.h"
#include "commo.h"
#include "config.h"

namespace janus {

REG_FRAME(MODE_MENCIUS, vector<string>({"mencius"}), MenciusFrame);

MenciusFrame::MenciusFrame(int mode) : Frame(mode) {

}

Executor *MenciusFrame::CreateExecutor(cmdid_t cmd_id, TxLogServer *sched) {
  Executor *exec = new MenciusExecutor(cmd_id, sched);
  return exec;
}

Coordinator *MenciusFrame::CreateCoordinator(cooid_t coo_id,
                                                Config *config,
                                                int benchmark,
                                                ClientControlServiceImpl *ccsi,
                                                uint32_t id,
                                                shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  CoordinatorMencius *coo;
  coo = new CoordinatorMencius(coo_id,
                                  benchmark,
                                  ccsi,
                                  id);
  coo->frame_ = this;
  verify(commo_ != nullptr);
  coo->commo_ = commo_;
  slot_id_ = slot_hint_ + site_info_->id;
  coo->slot_hint_ = &slot_id_;
  coo->slot_id_ = slot_id_;
  coo->n_replica_ = config->GetPartitionSize(site_info_->partition_id_);
  slot_hint_ += coo->n_replica_;
  coo->loc_id_ = this->site_info_->locale_id;
  verify(coo->n_replica_ != 0); // TODO
  return coo;
}

TxLogServer *MenciusFrame::CreateScheduler() {
  TxLogServer *sch = nullptr;
  sch = new MenciusServer();
  sch->frame_ = this;
  return sch;
}

Communicator *MenciusFrame::CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll) {
  // We only have 1 instance of MenciusFrame object that is returned from
  // GetFrame method. MenciusCommo currently seems ok to share among the
  // clients of this method.
  if (commo_ == nullptr) {
    commo_ = new MenciusCommo(poll);
  }
  return commo_;
}

vector<rrr::Service *>
MenciusFrame::CreateRpcServices(uint32_t site_id,
                                   TxLogServer *rep_sched,
                                   rusty::Arc<rrr::PollThread> poll_thread_worker,
                                   ServerControlServiceImpl *scsi) {
  auto config = Config::GetConfig();
  auto result = std::vector<Service *>();
  switch (config->replica_proto_) {
    case MODE_MENCIUS:result.push_back(new MenciusServiceImpl(rep_sched));
    default:break;
  }
  return result;
}

} // namespace janus;
