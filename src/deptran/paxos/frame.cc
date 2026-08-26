#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "coordinator.h"
#include "server.h"
#include "service.h"
#include "commo.h"
#include "config.h"

namespace janus {

CoordinatorMultiPaxos *MultiPaxosFrame::CreateCoordinator() {
  auto *coo = new CoordinatorMultiPaxos();
  verify(commo_ != nullptr);
  coo->commo_ = commo_;
  coo->loc_id_ = this->site_info_->locale_id;
  return coo;
}


BulkCoordinatorMultiPaxos *MultiPaxosFrame::CreateBulkCoordinator() {
    auto *coo = new BulkCoordinatorMultiPaxos();
    verify(commo_ != nullptr);
    coo->commo_ = commo_;
    coo->loc_id_ = this->site_info_->locale_id;
    return coo;
}


TxLogServer *MultiPaxosFrame::CreateScheduler() {
  return new PaxosServer();
}

Communicator *MultiPaxosFrame::CreateCommo(
    rusty::Option<rusty::Arc<srpc::PollThread>> poll) {
  // We only have 1 instance of MultiPaxosFrame object that is returned from
  // GetFrame method. MultiPaxosCommo currently seems ok to share among the
  // clients of this method.
  if (commo_ == nullptr) {
    commo_ = new MultiPaxosCommo(std::move(poll));
  }
  return commo_;
}

std::vector<srpc::ServiceProxy>
MultiPaxosFrame::CreateRpcServices(uint32_t site_id,
                                   TxLogServer *rep_sched,
                                   rusty::Arc<srpc::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<srpc::ServiceProxy>();
  switch (config->replica_proto_) {
    case MODE_MULTI_PAXOS: {
      auto* server = dynamic_cast<PaxosServer*>(rep_sched);
      verify(server != nullptr);
      result.push_back(srpc::make_service_proxy_from_typed_box(
          rusty::make_box<MultiPaxosServiceImpl>(server)));
      break;
    }
    default:break;
  }
  return result;
}

} // namespace janus;
