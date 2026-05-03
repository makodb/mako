#include "service.h"

namespace janus {

MongodbServiceImpl::MongodbServiceImpl(TxLogServer *sched)
  : sched_((MongodbServer*)sched) {

}

void MongodbServiceImpl::Commit(const MongodbService::RpcCommitRequest& rpc_req,
                                MongodbService::RpcCommitResponse& rpc_resp,
                                rrr::DeferredReply defer) {
  (void)rpc_resp;
  // L10f-prep6h: RuleWitnessGC now takes janus::Command directly.
  sched_->RuleWitnessGC(rpc_req.cmd);
  defer.reply();
}

} // namespace janus;
