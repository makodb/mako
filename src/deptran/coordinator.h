#pragma once
// DEPRECATED: Multi-Paxos replication is deprecated and no longer tested; Raft (src/deptran/raft/) is the supported engine. This header (janus::Coordinator) has only Paxos users, but it is still pulled into Raft translation units through paxos_worker.h. See src/deptran/paxos/DEPRECATED.md.
#include "constants.h"

namespace janus {

class Communicator;

// Common ownership surface for replication coordinators. Protocol-specific
// phase state belongs to the derived coordinator.
class Coordinator {
 public:
  locid_t loc_id_ = static_cast<locid_t>(-1);
  parid_t par_id_ = static_cast<parid_t>(-1);
  Communicator* commo_ = nullptr;

  virtual ~Coordinator() = default;
};

}  // namespace janus
