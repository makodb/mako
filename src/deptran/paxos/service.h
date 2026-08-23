#pragma once

#include "__dep__.h"
#include "constants.h"
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
  // removed `Forward(janus::Command, ...)`
  // declaration — paired with its typed-rpc override (also removed
  // below); body was empty and the matching `MultiPaxos::Forward`
  // RPC was dropped from rcc_rpc.rpc.

  void Prepare(const uint64_t& slot,
               const ballot_t& ballot,
               ballot_t* max_ballot,
               uint64_t* coro_id,
               rrr::DeferredReply defer);

  void Accept(const uint64_t& slot,
	      const uint64_t& time,
              const ballot_t& ballot,
              const janus::Command& cmd,
              ballot_t* max_ballot,
              uint64_t* coro_id,
              rrr::DeferredReply defer);

  void Decide(const uint64_t& slot,
              const ballot_t& ballot,
              const janus::Command& cmd,
              rrr::DeferredReply defer);

  void BulkDecide(const janus::Command& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  void BulkAccept(const janus::Command& cmd,
                  i32* ballot,
                  i32* valid,
                  rrr::DeferredReply defer);

  // removed `BulkPrepare(janus::Command, ...)`
  // and `Heartbeat(janus::Command, ...)` declarations — paired with
  // their typed-rpc overrides (also removed below); both became dead
  // when their senders went away in Phase 4e-25.

  // removed `BulkPrepare2(janus::Command, ...)`
  // declaration — paired with its typed-rpc override (also removed
  // below).

  void SyncLog(const janus::Command& md_cmd,
                     i32* ballot,
                     i32* valid,
                     janus::Command* ret,
                     rrr::DeferredReply defer);

  void SyncCommit(const janus::Command& md_cmd,
                     i32* ballot,
                     i32* valid,
                     rrr::DeferredReply defer);

  // removed `SyncNoOps(janus::Command, ...)`
  // declaration — paired with its typed-rpc override (also removed
  // below).

  void ForwardToLearnerServer(const rrr::i32& par_id, const uint64_t& slot, const ballot_t& ballot, const janus::Command& cmd, uint64_t* ret_slot, ballot_t* ret_ballot, rrr::DeferredReply defer);


  // BEGIN typed-rpc-decls (MultiPaxosServiceImpl)
  // Typed RPC interface overrides (new API).
  // removed `Forward` typed-rpc override —
  // the matching abstract base class virtual is gone (rcc_rpc.rpc
  // updated; rcc_rpc.h regenerated) and the body was empty.
  void Prepare(const MultiPaxosService::RpcPrepareRequest& req, MultiPaxosService::RpcPrepareResponse& resp, rrr::DeferredReply defer) override;
  void Accept(const MultiPaxosService::RpcAcceptRequest& req, MultiPaxosService::RpcAcceptResponse& resp, rrr::DeferredReply defer) override;
  void Decide(const MultiPaxosService::RpcDecideRequest& req, MultiPaxosService::RpcDecideResponse& resp, rrr::DeferredReply defer) override;
  // removed `BulkPrepare`, `Heartbeat`,
  // `SyncNoOps` typed-rpc overrides — the matching abstract base
  // class virtuals are gone (rcc_rpc.rpc updated; rcc_rpc.h
  // regenerated) and no senders remain.
  // removed `BulkPrepare2` typed-rpc
  // override — the matching `MultiPaxos::BulkPrepare2` RPC was
  // dropped from rcc_rpc.rpc; no senders remain.
  void BulkAccept(const MultiPaxosService::RpcBulkAcceptRequest& req, MultiPaxosService::RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) override;
  void BulkDecide(const MultiPaxosService::RpcBulkDecideRequest& req, MultiPaxosService::RpcBulkDecideResponse& resp, rrr::DeferredReply defer) override;
  void SyncLog(const MultiPaxosService::RpcSyncLogRequest& req, MultiPaxosService::RpcSyncLogResponse& resp, rrr::DeferredReply defer) override;
  void SyncCommit(const MultiPaxosService::RpcSyncCommitRequest& req, MultiPaxosService::RpcSyncCommitResponse& resp, rrr::DeferredReply defer) override;
  void ForwardToLearnerServer(const MultiPaxosService::RpcForwardToLearnerServerRequest& req, MultiPaxosService::RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (MultiPaxosServiceImpl)
};

} // namespace janus
