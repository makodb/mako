#pragma once

// The state-machine snapshot callback types an embedder registers with a Raft
// server (RaftSpecific::SetStateMachineSnapshotCallbacks, or the replication
// helper's register_snapshot_callbacks_for_partition). A header of its own so
// an embedder can implement them without including raft/server.h.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace janus {

// PreparedStateMachineSnapshotInstall is an owned, abort-on-destruction
// transaction. Prepare callbacks must fully validate and durably stage an
// incoming state-machine image without changing the live state machine.
// Commit() may publish the staged image only after Raft has durably published
// the matching snapshot bytes.
// @unsafe - Abstract C++ ownership boundary for filesystem-backed state machines.
class PreparedStateMachineSnapshotInstall {
 public:
  virtual ~PreparedStateMachineSnapshotInstall() = default;

  // @unsafe - Atomically publishes the already-validated staged image.
  virtual bool Commit() = 0;
};

// create(applied_index) returns the state machine's image at that index, or
// "" to refuse. prepare(image, index) validates and stages it, returning
// nullptr to refuse.
using RaftCreateSnapshotFn = std::function<std::string(uint64_t)>;
using RaftPrepareSnapshotFn = std::function<
    std::unique_ptr<PreparedStateMachineSnapshotInstall>(const std::string&, uint64_t)>;

}  // namespace janus
