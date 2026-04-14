#include "service.h"

namespace janus {

MongodbServiceImpl::MongodbServiceImpl(TxLogServer *sched)
  : sched_((MongodbServer*)sched) {

}

void MongodbServiceImpl::Commit(const MongodbService::RpcCommitRequest& rpc_req,
                                MongodbService::RpcCommitResponse& rpc_resp,
                                rrr::DeferredReply defer) {
  (void)rpc_resp;
  sched_->RuleWitnessGC(const_cast<MarshallDeputy&>(rpc_req.cmd).sp_data_);
  defer.reply();
}

} // namespace janus;
