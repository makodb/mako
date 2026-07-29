#include "../__dep__.h"
#include "../constants.h"
#include "frame.h"
#include "server.h"
#include "service.h"
#include "coordinator.h"

namespace janus {

REG_FRAME(MODE_COPILOT, vector<string>({"copilot"}), CopilotFrame);

CopilotFrame::CopilotFrame(int mode) : Frame(mode) {
}

CopilotFrame::~CopilotFrame() {
  Log_info(
      "server {}, "
      "[FAST_ACCEPT] {} (fast {} regular {}) "
      "[ACCEPT] {} "
      "[COMMIT] {} "
      "[PREPARE] {}",
      site_info_->id, n_fast_accept_, n_fast_path_, n_regular_path_, n_accept_, n_commit_, n_prepare_);
}

Coordinator *CopilotFrame::CreateCoordinator(cooid_t coo_id,
                                            Config *config,
                                            int benchmark,
                                            rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                            uint32_t id,
                                            shared_ptr<TxnRegistry> txn_reg) {
  verify(config != nullptr);
  // TODO: pool used coordinator to avoid creating every time
  auto coord = new CoordinatorCopilot(coo_id, benchmark, std::move(client_status), id);

  setupCoordinator(coord, config);  

  Log_debug("create new copilot coord, coo_id: {}", (int)coord->coo_id_);
  return coord;
}

TxLogServer *CopilotFrame::CreateScheduler() {
  if (sch_ == nullptr) {
    sch_ = new CopilotServer(this);
  } else {
    verify(0);
  }

  Log_debug("create copilot sched loc: {}", this->site_info_->locale_id);
  return sch_;
}

Communicator *CopilotFrame::CreateCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
  if (commo_ == nullptr) {
    commo_ = new CopilotCommo(std::move(poll_thread_worker));
  }

  return commo_;
}

vector<rrr::ServiceProxy>
CopilotFrame::CreateRpcServices(uint32_t site_id,
                                TxLogServer *rep_sched,
                                rusty::Arc<rrr::PollThread> poll_thread_worker) {
  auto config = Config::GetConfig();
  auto result = std::vector<rrr::ServiceProxy>();
  switch (config->replica_proto_) {
    case MODE_COPILOT:
      result.push_back(rrr::make_service_proxy_from_typed_box(rusty::make_box<CopilotServiceImpl>(rep_sched)));
      break;
    default:
      break;
  }

  return result;
}

void CopilotFrame::setupCoordinator(CoordinatorCopilot *coord, Config *config) {
  coord->frame_ = this;
  
  verify(commo_ != nullptr);
  coord->commo_ = commo_;

  verify(sch_ != nullptr);
  coord->sch_ = sch_;
  // removed
  // `coord->slot_hint_ = &slot_hint_;` — the
  // `CoordinatorCopilot::slot_hint_` field had no readers anywhere
  // and was deleted in the same commit.
  // coord->slot_id_ = slot_hint_++;
  coord->n_replica_ = config->GetPartitionSize(site_info_->partition_id_);
  coord->loc_id_ = site_info_->locale_id;
  verify(coord->n_replica_ != 0);
}

} // namespace janus

