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

  // The partition's sites a round hears from (all but the learner, the
  // leader included), and how many of them must accept.
  static int Quorum(parid_t par_id);
  // Those sites (but except, if given) whose send queue holds less than
  // kRoomBytes. A leader with fewer than a quorum of them waits before it
  // starts more rounds (PaxosWorker::_Submit); one slow acceptor does not
  // hold it up.
  int PeersWithRoom(parid_t par_id, siteid_t except = -1);
  static constexpr size_t kRoomBytes = 256u << 20;

  // An accept or decide to a site whose queue already holds
  // kDropBacklogBytes is dropped, its slots noted, but only while a quorum
  // of the other sites have room, so every round still reaches a quorum.
  // Once that queue drains below kRepairLowWaterBytes, repair is called with
  // the site and the slots it missed, to send them again
  // (PaxosWorker::RepairPeer). Set once, before any traffic.
  void SetRepair(std::function<void(siteid_t, slotid_t, slotid_t)> repair);
  // This worker's own site, whose messages to itself are never dropped: a
  // leader that lost its own accept could not repair anyone from its log.
  void SetSelf(siteid_t site) { self_site_ = site; }
  // Notes more slots a site needs again (what one repair did not send).
  void MarkBehind(siteid_t site, slotid_t from, slotid_t to);
  // Sends one site the accept and the decide for slots already chosen.
  void SendRepair(parid_t par_id, siteid_t site, const janus::Command& accept,
                  const janus::Command& decide, size_t log_bytes,
                  slotid_t from, slotid_t to);
  static constexpr size_t kDropBacklogBytes = 512u << 20;
  static constexpr size_t kRepairLowWaterBytes = 64u << 20;

  // srpc refuses a frame over 64 MiB with an error the client also reports
  // as CLIENT_ERR_IO, so a message carrying more log than this can never be
  // sent. PaxosWorker::Submit refuses such an entry before it takes a slot.
  static constexpr size_t kMaxLogBytesPerMessage = 60u << 20;

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
    uint64_t not_sent = 0;   // messages the client refused (peer down)
    bool draining = false;   // a DrainSendQueue job is sending them
    // Accepts or decides for these slots were dropped for a full backlog;
    // the peer gets them again once its queue drains.
    bool behind = false;
    slotid_t behind_from = 0;
    slotid_t behind_to = 0;
  };

  // Send one message to peer now, or queue it behind earlier ones; send
  // returns 0 once the message is queued on the connection, or the client's
  // error code. before_queue, if set, runs when the message has to wait, so
  // it can stop sharing data the caller goes on to change. With
  // max_backlog_bytes set, a message that would take the peer's queue past
  // it is dropped instead; one carrying slots (first_slot > 0) marks the
  // peer behind on them.
  void SendInOrder(const std::shared_ptr<RpcPeer>& peer,
                   parid_t par_id,
                   const char* what,
                   size_t log_bytes,
                   std::function<int32_t(srpc::Client*)> send,
                   std::function<void()> before_queue = nullptr,
                   size_t max_backlog_bytes = 0,
                   slotid_t first_slot = 0,
                   slotid_t last_slot = 0);
  // SendInOrder's work, with queue->mu held.
  void SendOrQueueLocked(const std::shared_ptr<RpcPeer>& peer,
                         parid_t par_id,
                         const std::shared_ptr<PeerSendQueue>& queue,
                         const char* what,
                         size_t log_bytes,
                         std::function<int32_t(srpc::Client*)> send,
                         std::function<void()> before_queue,
                         size_t max_backlog_bytes,
                         slotid_t first_slot,
                         slotid_t last_slot);
  // With queue->mu held: whether the peer is behind and its queue has
  // drained enough to send it the slots; if so, takes them.
  bool TakeRepair(PeerSendQueue& queue, slotid_t* from, slotid_t* to);
  std::shared_ptr<PeerSendQueue> QueueFor(siteid_t site);
  // The backlog past which an accept or decide to site is dropped: none for
  // this worker's own site, or unless a quorum of the other sites have room.
  size_t DropAt(parid_t par_id, siteid_t site, int quorum);
  void DrainSendQueue(std::shared_ptr<RpcPeer> peer,
                      parid_t par_id,
                      std::shared_ptr<PeerSendQueue> queue);

  std::mutex send_queues_mu_;
  std::map<siteid_t, std::shared_ptr<PeerSendQueue>> send_queues_;
  std::function<void(siteid_t, slotid_t, slotid_t)> repair_{};
  siteid_t self_site_ = -1;
};

}  // namespace janus
