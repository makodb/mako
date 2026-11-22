#pragma once
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
// DEPRECATED: Multi-Paxos replication is deprecated and no longer tested; Raft (src/deptran/raft/) is the supported engine. See src/deptran/paxos/DEPRECATED.md.
#include <deptran/communicator.h>
#include "../frame.h"
#include "../constants.h"
#include "commo.h"

namespace janus {

class BulkCoordinatorMultiPaxos;
class CoordinatorMultiPaxos;

class MultiPaxosFrame : public Frame {
 public:
  MultiPaxosFrame() = default;
  MultiPaxosCommo *commo_ = nullptr;
  CoordinatorMultiPaxos *CreateCoordinator();
  BulkCoordinatorMultiPaxos *CreateBulkCoordinator();
  TxLogServer *CreateScheduler() override;
  Communicator *CreateCommo(
      rusty::Option<rusty::Arc<srpc::PollThread>> poll = rusty::None) override;
  std::vector<srpc::ServiceProxy> CreateRpcServices(
      uint32_t site_id,
      TxLogServer *rep_sched,
      rusty::Arc<srpc::PollThread> poll_thread_worker) override;
};

} // namespace janus
