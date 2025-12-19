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

  void Prepare(const uint64_t& slot,
               const ballot_t& ballot,
               ballot_t* max_ballot,
               uint64_t* coro_id,
               rrr::DeferredReply* defer) override;

  void Suggest(const uint64_t& slot,
	          const uint64_t& time,
              const ballot_t& ballot,
              const uint64_t& sender,
              const std::vector<uint64_t>& skip_commits, 
              const std::vector<uint64_t>& skip_potentials,
              const MarshallDeputy& cmd,
              ballot_t* max_ballot,
              uint64_t* coro_id,
              rrr::DeferredReply* defer) override;

  void Decide(const uint64_t& slot,
              const ballot_t& ballot,
              const MarshallDeputy& cmd,
              rrr::DeferredReply* defer) override;

};

} // namespace janus
