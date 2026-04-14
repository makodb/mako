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
class MenciusServer;
class MenciusServiceImpl : public MenciusService {
 public:
  MenciusServer* sched_;
  MenciusServiceImpl(TxLogServer* sched);

  // Defer handlers (preferred for async RPC completion).
  void Prepare(const uint64_t& slot,
               const ballot_t& ballot,
               ballot_t* max_ballot,
               uint64_t* coro_id,
               rrr::DeferredReply defer);

  void Suggest(const uint64_t& slot,
               const uint64_t& time,
               const ballot_t& ballot,
               const uint64_t& sender,
               const std::vector<uint64_t>& skip_commits,
               const std::vector<uint64_t>& skip_potentials,
               const MarshallDeputy& cmd,
               ballot_t* max_ballot,
               uint64_t* coro_id,
               rrr::DeferredReply defer);

  void Decide(const uint64_t& slot,
              const ballot_t& ballot,
              const MarshallDeputy& cmd,
              rrr::DeferredReply defer);


  // BEGIN typed-rpc-decls (MenciusServiceImpl)
  // Typed RPC interface overrides (new API).
  void Prepare(const MenciusService::RpcPrepareRequest& req, MenciusService::RpcPrepareResponse& resp, rrr::DeferredReply defer) override;
  void Suggest(const MenciusService::RpcSuggestRequest& req, MenciusService::RpcSuggestResponse& resp, rrr::DeferredReply defer) override;
  void Decide(const MenciusService::RpcDecideRequest& req, MenciusService::RpcDecideResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (MenciusServiceImpl)
};

} // namespace janus
