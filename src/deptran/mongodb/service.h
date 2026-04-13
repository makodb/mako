#pragma once

#include "server.h"

namespace janus {

class MongodbServiceImpl: public MongodbService {
 public:
  MongodbServer* sched_;
  MongodbServiceImpl(TxLogServer* sched);


  // BEGIN typed-rpc-decls (MongodbServiceImpl)
  // Typed RPC interface overrides (new API).
  void Commit(const MongodbService::RpcCommitRequest& req, MongodbService::RpcCommitResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (MongodbServiceImpl)
};


} // namespace janus
