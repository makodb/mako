#pragma once

#include <rusty/arc.hpp>

#include "../__dep__.h"
#include "../communicator.h"
#include "../constants.h"
#include "../replication_quorum.h"

namespace janus {

class Command;

// Paxos status codes used for encoding with timestamps.
enum PaxosStatus {
  STATUS_NORMAL = 0,
  STATUS_INIT = 1,
  STATUS_ENDING = 2,
  STATUS_SAFETY_FAIL = 3,
  STATUS_REPLAY_DONE = 4,
  STATUS_NOOPS = 5
};

class PaxosAcceptQuorumEvent : public QuorumEventBase {
 public:
  using QuorumEventBase::QuorumEventBase;

  void FeedResponse(bool accepted) {
    if (accepted) {
      q().n_voted_yes_.set(q().n_voted_yes_.get() + 1);
    } else {
      q().n_voted_no_.set(q().n_voted_no_.get() + 1);
    }
    test();
  }
};

class MultiPaxosCommo : public Communicator {
 public:
  MultiPaxosCommo() = delete;
  explicit MultiPaxosCommo(
      rusty::Option<rusty::Arc<rrr::PollThread>> poll = rusty::None);

  shared_ptr<PaxosAcceptQuorumEvent> BroadcastAccept(
      parid_t par_id,
      slotid_t slot_id,
      ballot_t ballot,
      const janus::Command& cmd);

  void ForwardToLearner(
      parid_t par_id,
      uint64_t slot,
      ballot_t ballot,
      const janus::Command& cmd,
      const std::function<void(uint64_t, ballot_t)>& cb);

  void BroadcastDecide(
      parid_t par_id,
      slotid_t slot_id,
      ballot_t ballot,
      const janus::Command& cmd);

  shared_ptr<PaxosAcceptQuorumEvent> BroadcastSyncLog(
      parid_t par_id,
      const janus::Command& cmd,
      const std::function<void(shared_ptr<janus::Command>, ballot_t, int)>& cb);

  shared_ptr<PaxosAcceptQuorumEvent> BroadcastSyncCommit(
      parid_t par_id,
      const janus::Command& cmd,
      const std::function<void(ballot_t, int)>& cb);

  shared_ptr<PaxosAcceptQuorumEvent> BroadcastBulkAccept(
      parid_t par_id,
      const janus::Command& cmd,
      const std::function<void(ballot_t, int)>& cb);

  shared_ptr<PaxosAcceptQuorumEvent> BroadcastBulkDecide(
      parid_t par_id,
      const janus::Command& cmd,
      const std::function<void(ballot_t, int)>& cb);
};

}  // namespace janus
