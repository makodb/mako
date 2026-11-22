#pragma once

#include <rusty/arc.hpp>

#include "../__dep__.h"
#include "../communicator.h"
#include "../constants.h"
#include "../replication_quorum.h"
#include "replication_outbox.h"

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

  // @safe - Quorum counters and readiness use synchronized interior mutation.
  void FeedResponse(bool accepted) const {
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
      rusty::Option<rusty::Arc<srpc::PollThread>> poll = rusty::None);

  shared_ptr<PaxosAcceptQuorumEvent> BroadcastAccept(
      parid_t par_id,
      slotid_t slot_id,
      ballot_t ballot,
      const janus::Command& cmd);

  void ForwardToLearner(
      parid_t par_id,
      uint64_t slot,
      ballot_t ballot,
      const janus::Command& cmd);

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

  rusty::Arc<PaxosAcceptQuorumEvent> BroadcastBulkAccept(
      parid_t par_id,
      const janus::Command& cmd);

  void BroadcastBulkDecide(parid_t par_id, const janus::Command& cmd);

  // BroadcastBulkAccept, BroadcastBulkDecide and ForwardToLearner queue
  // their requests in the destination's ReplicationOutbox
  // (replication_outbox.h) and return without waiting for channel space.

 private:
  // @unsafe - Owner poll thread only (verified): append to the destination's
  // outbox, creating it on first use. Never suspends the caller.
  void Send(const rusty::CommoPeerPtr& peer, ReplicationMessage message);

  struct OutboxSlot {
    siteid_t site_id;
    ReplicationOutbox outbox;
  };
  // The poll thread every caller of Send runs on: coordinator jobs, the
  // service handlers that apply and forward commits, and this communicator's
  // clients. Recorded at construction; outboxes_ and the outboxes are not
  // synchronized.
  const rusty::Arc<srpc::PollThread> owner_;
  // At most one per peer site, created on first use.
  rusty::Vec<OutboxSlot> outboxes_;
};

}  // namespace janus
