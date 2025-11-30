#pragma once
#include <rusty/arc.hpp>

#include "../__dep__.h"
#include "../constants.h"
#include "../communicator.h"

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

  shared_ptr<PaxosPrepareQuorumEvent>
  BroadcastPrepare(parid_t par_id,
                   slotid_t slot_id,
                   ballot_t ballot);
  void BroadcastPrepare(parid_t par_id,
                        slotid_t slot_id,
                        ballot_t ballot,
                        const function<void(Future *fu)> &callback);
  shared_ptr<PaxosAcceptQuorumEvent>
  BroadcastAccept(parid_t par_id,
                  slotid_t slot_id,
                  ballot_t ballot,
                  shared_ptr<Marshallable> cmd);
  void BroadcastAccept(parid_t par_id,
                       slotid_t slot_id,
                       ballot_t ballot,
                       shared_ptr<Marshallable> cmd,
                       const function<void(Future*)> &callback);
  void ForwardToLearner(parid_t par_id,
                        uint64_t slot,
                        ballot_t ballot,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(uint64_t, ballot_t)>& cb);
  void BroadcastDecide(const parid_t par_id,
                       const slotid_t slot_id,
                       const ballot_t ballot,
                       const shared_ptr<Marshallable> cmd);
  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastBulkPrepare(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        std::function<void(ballot_t, int)> cb) override;
  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastHeartBeat(parid_t par_id,
                        shared_ptr<Marshallable> cmd,
                        const std::function<void(ballot_t, int)>& cb) override;

  virtual shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastSyncNoOps(parid_t par_id,
                    shared_ptr<Marshallable> cmd,
                    const std::function<void(ballot_t, int)>& cb) override;

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

  shared_ptr<PaxosAcceptQuorumEvent>
    BroadcastPrepare2(parid_t par_id,
                      const shared_ptr<Marshallable> cmd,
                      const std::function<void(MarshallDeputy, ballot_t, int)>& cb);
};

} // namespace janus
