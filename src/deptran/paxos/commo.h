#pragma once
#include <rusty/arc.hpp>

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"
#include <chrono>
#include <ctime>

namespace janus {

// Paxos status codes used for encoding with timestamps
// Duplicated in ./src/mako/lib/common.h 
enum PaxosStatus {
    STATUS_NORMAL = 0,          // Normal/default status
    STATUS_INIT = 1,            // Init/initialization
    STATUS_ENDING = 2,          // Ending of Paxos group
    STATUS_SAFETY_FAIL = 3,     // Can't pass safety check
    STATUS_REPLAY_DONE = 4,     // Complete replay/replay done
    STATUS_NOOPS = 5            // No-ops
};


class TxData;

class MultiPaxosCommo : public Communicator {
 public:
  MultiPaxosCommo() = delete;
  MultiPaxosCommo(rusty::Option<rusty::Arc<PollThread>> poll = rusty::None);

  int proxy_batch_size = 1 ;
  int current_proxy_batch_idx = 0;
  bool is_broadcast_syncLog = false;

  // Workstream N Phase 4e-30: removed `BroadcastPrepare(parid, slot,
  // ballot)` declaration — only call site was the now-deleted
  // `CoordinatorMultiPaxos::Prepare()`; body was a `verify(0)` shell.
  // Workstream N Phase 4e-12: removed deprecated callback-style
  // `void BroadcastPrepare(parid_t, slotid_t, ballot_t, callback)` —
  // body had `verify(0);` and was mostly commented out; no live
  // callers anywhere.
  shared_ptr<PaxosAcceptQuorumEvent>
  BroadcastAccept(parid_t par_id,
                  slotid_t slot_id,
                  ballot_t ballot,
                  shared_ptr<Marshallable> cmd);
  // Workstream N Phase 4e-12: removed deprecated callback-style
  // `void BroadcastAccept(parid_t, slotid_t, ballot_t, cmd,
  // callback)` — same shape as the deprecated BroadcastPrepare.
  void ForwardToLearner(parid_t par_id,
                        uint64_t slot,
                        ballot_t ballot,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(uint64_t, ballot_t)>& cb);
  void BroadcastDecide(const parid_t par_id,
                       const slotid_t slot_id,
                       const ballot_t ballot,
                       const shared_ptr<Marshallable> cmd);
  // Workstream N Phase 4e-26: removed `BroadcastBulkPrepare`,
  // `BroadcastHeartBeat`, `BroadcastSyncNoOps` — became dead in
  // Phase 4e-25 when the matching `PaxosWorker::SendHeartBeat` /
  // `SendBulkPrepare` / `SendSyncNoOpLog` senders went away.

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncLog(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(shared_ptr<MarshallDeputy>, ballot_t, int)>& cb) override;


  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncCommit(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(ballot_t, int)>& cb) override;

  shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastBulkAccept(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(ballot_t, int)>& cb);
  shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastBulkDecide(parid_t par_id,
                           const shared_ptr<Marshallable> cmd,
                           const std::function<void(ballot_t, int)>& cb);

  // Workstream N Phase 4e-27: removed `BroadcastPrepare2` declaration
  // — only call site was the now-deleted
  // `BulkCoordinatorMultiPaxos::Prepare()`; the body was already a
  // `verify(0)`-then-commented-out shell.

  // Workstream N Phase 4e-38: removed `SendForward(parid, follower_id,
  // dep_id, cmd)` declaration — never called from anywhere in the
  // tree; the only candidate caller would have been a Jetpack
  // forward-to-leader path that was never wired up.
};

} // namespace janus
