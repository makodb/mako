#include "service.h"

namespace janus {

MongodbServiceImpl::MongodbServiceImpl(TxLogServer *sched)
  : sched_((MongodbServer*)sched) {

}

void MongodbServiceImpl::Commit(const MongodbService::RpcCommitRequest& rpc_req,
                                MongodbService::RpcCommitResponse& rpc_resp,
                                rrr::DeferredReply defer) {
  (void)rpc_resp;
  sched_->RuleWitnessGC(const_cast<janus::Command&>(rpc_req.cmd).inner());
  defer.reply();
}

} // namespace janus;
