#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"

namespace janus {

class TxLogServer;
class CopilotServer;

class CopilotServiceImpl : public CopilotService {
  CopilotServer* sched_;
 public:
  CopilotServiceImpl(TxLogServer *sched);

  // Defer handlers (preferred for async RPC completion).
  // removed `Forward(janus::Command, ...)`
  // declaration — paired with its typed-rpc override (also removed
  // below); matching `Copilot::Forward` RPC was dropped from
  // rcc_rpc.rpc and `CopilotServer::OnForward` is also gone.

  void Prepare(const uint8_t& is_pilot,
               const uint64_t& slot,
               const ballot_t& ballot,
               const DepId& dep_id,
               janus::Command* ret_cmd,
               ballot_t* max_ballot,
               uint64_t* dep,
               status_t* status,
               srpc::DeferredReply defer);

  void FastAccept(const uint8_t& is_pilot,
                  const uint64_t& slot,
                  const ballot_t& ballot,
                  const uint64_t& dep,
                  const janus::Command& cmd,
                  const DepId& dep_id,
                  ballot_t* max_ballot,
                  uint64_t* ret_dep,
                  srpc::DeferredReply defer);

  void Accept(const uint8_t& is_pilot,
              const uint64_t& slot,
              const ballot_t& ballot,
              const uint64_t& dep,
              const janus::Command& cmd,
              const DepId& dep_id,
              ballot_t* max_ballot,
              srpc::DeferredReply defer);

  void Commit(const uint8_t& is_pilot,
              const uint64_t& slot,
              const uint64_t& dep,
              const janus::Command& cmd,
              srpc::DeferredReply defer);

  // BEGIN typed-rpc-decls (CopilotServiceImpl)
  // Typed RPC interface overrides (new API).
  // removed `Forward` typed-rpc override —
  // matching abstract base class virtual is gone (rcc_rpc.rpc updated).
  void Prepare(const CopilotService::RpcPrepareRequest& req, CopilotService::RpcPrepareResponse& resp, srpc::DeferredReply defer) override;
  void FastAccept(const CopilotService::RpcFastAcceptRequest& req, CopilotService::RpcFastAcceptResponse& resp, srpc::DeferredReply defer) override;
  void Accept(const CopilotService::RpcAcceptRequest& req, CopilotService::RpcAcceptResponse& resp, srpc::DeferredReply defer) override;
  void Commit(const CopilotService::RpcCommitRequest& req, CopilotService::RpcCommitResponse& resp, srpc::DeferredReply defer) override;
  // END typed-rpc-decls (CopilotServiceImpl)
};

} // namespace janus
