#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../command.h"
#include "../rcc_rpc.h"

namespace janus {

class PaxosServer;
class MultiPaxosServiceImpl : public MultiPaxosService {
 public:
  PaxosServer* sched_;
  explicit MultiPaxosServiceImpl(PaxosServer* sched);

  void BulkDecide(const janus::Command& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void BulkAccept(const janus::Command& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void SyncLog(const janus::Command& md_cmd,
                     i32* ballot,
                     i32* valid,
                     janus::Command* ret,
                     rrr::DeferredReply defer);

  void ForwardToLearnerServer(const rrr::i32& par_id, const uint64_t& slot, const ballot_t& ballot, const janus::Command& cmd, uint64_t* ret_slot, ballot_t* ret_ballot, rrr::DeferredReply defer);


  // BEGIN typed-rpc-decls (MultiPaxosServiceImpl)
  void BulkAccept(const MultiPaxosService::RpcBulkAcceptRequest& req, MultiPaxosService::RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) override;
  void BulkDecide(const MultiPaxosService::RpcBulkDecideRequest& req, MultiPaxosService::RpcBulkDecideResponse& resp, rrr::DeferredReply defer) override;
  void SyncLog(const MultiPaxosService::RpcSyncLogRequest& req, MultiPaxosService::RpcSyncLogResponse& resp, rrr::DeferredReply defer) override;
  void ForwardToLearnerServer(const MultiPaxosService::RpcForwardToLearnerServerRequest& req, MultiPaxosService::RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (MultiPaxosServiceImpl)
};

} // namespace janus
