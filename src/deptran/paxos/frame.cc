#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "coordinator.h"
#include "server.h"
#include "service.h"
#include "commo.h"
#include "config.h"

namespace janus {

Coordinator *MultiPaxosFrame::CreateCoordinator(cooid_t coo_id) {
  auto *config = Config::GetConfig();
  verify(config != nullptr);
  CoordinatorMultiPaxos *coo;
  coo = new CoordinatorMultiPaxos(coo_id,
                                  0,
                                  rusty::None,
                                  0);
  coo->frame_ = this;
  verify(commo_ != nullptr);
  coo->commo_ = commo_;
  coo->slot_hint_ = &slot_hint_;
  coo->slot_id_ = slot_hint_++;
  coo->n_replica_ = config->GetPartitionSize(site_info_->partition_id_);
  coo->loc_id_ = this->site_info_->locale_id;
  verify(coo->n_replica_ != 0); // TODO
  return coo;
}


Coordinator *MultiPaxosFrame::CreateBulkCoordinator() {
    auto *config = Config::GetConfig();
    verify(config != nullptr);
    CoordinatorMultiPaxos *coo;
    coo = new BulkCoordinatorMultiPaxos(0, 0, rusty::None, 0);
    coo->frame_ = this;
    verify(commo_ != nullptr);
    coo->commo_ = commo_;
    coo->slot_hint_ = &slot_hint_;  // add the slot info
    coo->slot_id_ = slot_hint_++;
    coo->n_replica_ = config->GetPartitionSize(site_info_->partition_id_);
    coo->loc_id_ = this->site_info_->locale_id;
    verify(coo->n_replica_ != 0); // TODO
    return coo;
}


TxLogServer *MultiPaxosFrame::CreateScheduler() {
  TxLogServer *sch = nullptr;
  sch = new PaxosServer();
  sch->frame_ = this;
  return sch;
}

Communicator *MultiPaxosFrame::CreateCommo(
    rusty::Option<rusty::Arc<rrr::PollThread>> poll) {
  // We only have 1 instance of MultiPaxosFrame object that is returned from
  // GetFrame method. MultiPaxosCommo currently seems ok to share among the
  // clients of this method.
  if (commo_ == nullptr) {
    commo_ = new MultiPaxosCommo(std::move(poll));
  }
  return commo_;
}

std::vector<rrr::ServiceProxy>
MultiPaxosFrame::CreateRpcServices(uint32_t site_id,
                                   TxLogServer *rep_sched,
                                   rusty::Arc<rrr::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<rrr::ServiceProxy>();
  switch (config->replica_proto_) {
    case MODE_MULTI_PAXOS:result.push_back(rrr::make_service_proxy_from_typed_box(rusty::make_box<MultiPaxosServiceImpl>(rep_sched)));
    default:break;
  }
  return result;
}

} // namespace janus;
