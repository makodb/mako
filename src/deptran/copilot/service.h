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
  void Forward(const MarshallDeputy& cmd,
               rrr::DeferredReply defer);

  void Prepare(const uint8_t& is_pilot,
               const uint64_t& slot,
               const ballot_t& ballot,
               const DepId& dep_id,
               MarshallDeputy* ret_cmd,
               ballot_t* max_ballot,
               uint64_t* dep,
               status_t* status,
               rrr::DeferredReply defer);

  void FastAccept(const uint8_t& is_pilot,
                  const uint64_t& slot,
                  const ballot_t& ballot,
                  const uint64_t& dep,
                  const MarshallDeputy& cmd,
                  const DepId& dep_id,
                  ballot_t* max_ballot,
                  uint64_t* ret_dep,
                  rrr::DeferredReply defer);

  void Accept(const uint8_t& is_pilot,
              const uint64_t& slot,
              const ballot_t& ballot,
              const uint64_t& dep,
              const MarshallDeputy& cmd,
              const DepId& dep_id,
              ballot_t* max_ballot,
              rrr::DeferredReply defer);

  void Commit(const uint8_t& is_pilot,
              const uint64_t& slot,
              const uint64_t& dep,
              const MarshallDeputy& cmd,
              rrr::DeferredReply defer);

  // BEGIN typed-rpc-decls (CopilotServiceImpl)
  // Typed RPC interface overrides (new API).
  void Forward(const CopilotService::RpcForwardRequest& req, CopilotService::RpcForwardResponse& resp, rrr::DeferredReply defer) override;
  void Prepare(const CopilotService::RpcPrepareRequest& req, CopilotService::RpcPrepareResponse& resp, rrr::DeferredReply defer) override;
  void FastAccept(const CopilotService::RpcFastAcceptRequest& req, CopilotService::RpcFastAcceptResponse& resp, rrr::DeferredReply defer) override;
  void Accept(const CopilotService::RpcAcceptRequest& req, CopilotService::RpcAcceptResponse& resp, rrr::DeferredReply defer) override;
  void Commit(const CopilotService::RpcCommitRequest& req, CopilotService::RpcCommitResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (CopilotServiceImpl)
};

} // namespace janus
