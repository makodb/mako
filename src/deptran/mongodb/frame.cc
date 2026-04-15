#include "frame.h"
#include "coordinator.h"
#include "server.h"
#include "commo.h"
#include "service.h"

namespace janus {

REG_FRAME(MODE_MONGODB, vector<string>({"mongodb"}), MongodbFrame);

MongodbFrame::MongodbFrame(int mode) : Frame(mode) {
}

Coordinator *MongodbFrame::CreateCoordinator(cooid_t coo_id,
                                                Config *config,
                                                int benchmark,
                                                rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                                uint32_t id,
                                                shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  CoordinatorMongodb *coo;
  coo = new CoordinatorMongodb(coo_id,
                                benchmark,
                                std::move(client_status),
                                id);
  coo->frame_ = this;
  verify(commo_ != nullptr);
  coo->commo_ = commo_;
  coo->loc_id_ = this->site_info_->locale_id;
  Log_debug("create new mongodb coord, coo_id: %d", (int) coo->coo_id_);
  return coo;
}

TxLogServer *MongodbFrame::CreateScheduler() {
  TxLogServer *sch = nullptr;
  sch = new MongodbServer();
  sch->frame_ = this;
  return sch;
}

Communicator *MongodbFrame::CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
  if (commo_ == nullptr) {
    commo_ = new MongodbCommo(poll_thread_worker);
  }
  return commo_;
}

vector<rrr::ServiceProxy>
MongodbFrame::CreateRpcServices(uint32_t site_id,
                                   TxLogServer *rep_sched,
                                   rusty::Arc<rrr::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<rrr::ServiceProxy>();
  switch (config->replica_proto_) {
    case MODE_MONGODB:result.push_back(rrr::make_service_proxy_from_typed_box(rusty::make_box<MongodbServiceImpl>(rep_sched)));
    default:break;
  }
  return result;
}


}