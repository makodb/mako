#pragma once

#include <atomic>
#include <deque>
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

  // Bytes waiting in the longest queue to one peer. A leader waits for it
  // to drain before it starts more rounds (PaxosWorker::_Submit).
  size_t MaxQueuedBytes();

 private:
  // The messages for one peer that wait behind one the channel refused.
  struct PeerSendQueue {
    std::mutex mu;
    struct Message {
      const char* what;
      size_t log_bytes;   // the log entries it carries, for the warnings
      size_t bytes;       // what it counts against the queue's backlog
      std::function<int32_t(srpc::Client*)> send;
    };
    std::deque<Message> pending;
    std::atomic<size_t> queued_bytes{0};
    uint64_t dropped = 0;    // messages refused for a full backlog
    bool draining = false;   // a DrainSendQueue job is sending them
  };

  // Send one message to peer now, or queue it behind earlier ones; send
  // returns 0 once the message is queued on the connection, or the client's
  // error code. before_queue, if set, runs when the message has to wait, so
  // it can stop sharing data the caller goes on to change. With
  // max_backlog_bytes set, a message that would take the peer's queue past
  // it is dropped instead.
  void SendInOrder(const std::shared_ptr<RpcPeer>& peer,
                   parid_t par_id,
                   const char* what,
                   size_t log_bytes,
                   std::function<int32_t(srpc::Client*)> send,
                   std::function<void()> before_queue = nullptr,
                   size_t max_backlog_bytes = 0);
  void DrainSendQueue(std::shared_ptr<RpcPeer> peer,
                      parid_t par_id,
                      std::shared_ptr<PeerSendQueue> queue);

  std::mutex send_queues_mu_;
  std::map<siteid_t, std::shared_ptr<PeerSendQueue>> send_queues_;
};

}  // namespace janus
