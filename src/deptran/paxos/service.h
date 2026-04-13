#pragma once

#include "__dep__.h"
#include "constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "deptran/procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include <chrono>

class SimpleCommand;
namespace janus {

class TxLogServer;
class PaxosServer;
class MultiPaxosServiceImpl : public MultiPaxosService {
 public:
  PaxosServer* sched_;
  MultiPaxosServiceImpl(TxLogServer* sched);
  void Forward(const MarshallDeputy& cmd,
               const uint64_t& dep_id,
               uint64_t* coro_id,
               rrr::DeferredReply defer);

  void Prepare(const uint64_t& slot,
               const ballot_t& ballot,
               ballot_t* max_ballot,
               uint64_t* coro_id,
               rrr::DeferredReply defer);

  void Accept(const uint64_t& slot,
	      const uint64_t& time,
              const ballot_t& ballot,
              const MarshallDeputy& cmd,
              ballot_t* max_ballot,
              uint64_t* coro_id,
              rrr::DeferredReply defer);

  void Decide(const uint64_t& slot,
              const ballot_t& ballot,
              const MarshallDeputy& cmd,
              rrr::DeferredReply defer);

  void BulkDecide(const MarshallDeputy& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void BulkAccept(const MarshallDeputy& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void BulkPrepare(const MarshallDeputy& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void Heartbeat(const MarshallDeputy& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void BulkPrepare2(const MarshallDeputy& md_cmd,
                     i32* ballot,
                     i32* valid,
                     MarshallDeputy* ret,
                     rrr::DeferredReply defer);

  void SyncLog(const MarshallDeputy& md_cmd,
                     i32* ballot,
                     i32* valid,
                     MarshallDeputy* ret,
                     rrr::DeferredReply defer);

  void SyncCommit(const MarshallDeputy& md_cmd,
                     i32* ballot,
                     i32* valid,
                     rrr::DeferredReply defer);

  void SyncNoOps(const MarshallDeputy& md_cmd,
                 i32* ballot,
                 i32* valid,
                 rrr::DeferredReply defer);

  void ForwardToLearnerServer(const rrr::i32& par_id, const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd, uint64_t* ret_slot, ballot_t* ret_ballot, rrr::DeferredReply defer);


  // BEGIN typed-rpc-decls (MultiPaxosServiceImpl)
  // Typed RPC interface overrides (new API).
  void Forward(const MultiPaxosService::RpcForwardRequest& req, MultiPaxosService::RpcForwardResponse& resp, rrr::DeferredReply defer) override;
  void Prepare(const MultiPaxosService::RpcPrepareRequest& req, MultiPaxosService::RpcPrepareResponse& resp, rrr::DeferredReply defer) override;
  void Accept(const MultiPaxosService::RpcAcceptRequest& req, MultiPaxosService::RpcAcceptResponse& resp, rrr::DeferredReply defer) override;
  void Decide(const MultiPaxosService::RpcDecideRequest& req, MultiPaxosService::RpcDecideResponse& resp, rrr::DeferredReply defer) override;
  void BulkPrepare(const MultiPaxosService::RpcBulkPrepareRequest& req, MultiPaxosService::RpcBulkPrepareResponse& resp, rrr::DeferredReply defer) override;
  void Heartbeat(const MultiPaxosService::RpcHeartbeatRequest& req, MultiPaxosService::RpcHeartbeatResponse& resp, rrr::DeferredReply defer) override;
  void BulkPrepare2(const MultiPaxosService::RpcBulkPrepare2Request& req, MultiPaxosService::RpcBulkPrepare2Response& resp, rrr::DeferredReply defer) override;
  void BulkAccept(const MultiPaxosService::RpcBulkAcceptRequest& req, MultiPaxosService::RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) override;
  void BulkDecide(const MultiPaxosService::RpcBulkDecideRequest& req, MultiPaxosService::RpcBulkDecideResponse& resp, rrr::DeferredReply defer) override;
  void SyncLog(const MultiPaxosService::RpcSyncLogRequest& req, MultiPaxosService::RpcSyncLogResponse& resp, rrr::DeferredReply defer) override;
  void SyncCommit(const MultiPaxosService::RpcSyncCommitRequest& req, MultiPaxosService::RpcSyncCommitResponse& resp, rrr::DeferredReply defer) override;
  void SyncNoOps(const MultiPaxosService::RpcSyncNoOpsRequest& req, MultiPaxosService::RpcSyncNoOpsResponse& resp, rrr::DeferredReply defer) override;
  void ForwardToLearnerServer(const MultiPaxosService::RpcForwardToLearnerServerRequest& req, MultiPaxosService::RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (MultiPaxosServiceImpl)
};

} // namespace janus
