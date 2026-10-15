#include "commo.h"

#include <algorithm>
#include <chrono>

#include "../paxos_worker.h"   // BulkPaxosCmd
#include "../rcc_rpc.h"

namespace janus {

MultiPaxosCommo::MultiPaxosCommo(
    rusty::Option<rusty::Arc<srpc::PollThread>> poll)
    : Communicator(std::move(poll)) {}

// A Paxos message has to reach every live replica: a dropped accept leaves
// its slot short of a quorum, and a dropped decide leaves it uncommitted on
// that replica; since a replica applies a partition's slots in order,
// either one stalls the partition there for good. The channel refuses a
// frame while its outbound buffer is past its high-water mark (4 MiB by
// default), which one large log entry (the TPC-C loader writes tens of MB
// at a time) is enough to reach, and the client reports that as
// CLIENT_ERR_IO without sending. So a refused message is queued, every
// later message to that peer queues behind it (a replica needs a
// partition's slots in order, and an accept must not be overtaken by its
// decide), and one job on this communicator's poll thread sends the queue
// in order as the buffer drains. A message that fails some other way (the
// peer is gone) is dropped, as before.
namespace {

// What a queued message counts against its peer's backlog besides its log.
constexpr size_t kMessageOverheadBytes = 64;
// p1's backlog to the learner. The learner does not vote, so past this p1
// drops entries rather than holding them; the learner records the hole and
// fetches those entries from the acceptors when it takes over
// (catch_up_learner).
constexpr size_t kMaxLearnerBacklogBytes = 256u << 20;
// Entries of at most kMaxControlEntryBytes are control records (Mako's end
// signal is empty, its benchmark marker 2 bytes, a no-op 8 bytes; a
// transaction entry is at least 18). They count against the backlog too,
// but are dropped only past an extra reserve, so a full queue drops data
// before them.
constexpr size_t kMaxControlEntryBytes = 8;
constexpr size_t kControlReserveBytes = 16u << 20;

// Bytes of log carried by a BulkPaxosCmd, for the warnings.
size_t LogBytes(const janus::Command& cmd) {
  size_t bytes = 0;
  const auto bp = marshallable_cast<BulkPaxosCmd>(cmd);
  if (bp.is_some()) {
    for (const auto& c : bp.unwrap()->cmds) {
      const auto le = marshallable_cast<LogEntry>(*c);
      if (le.is_some()) bytes += le.unwrap()->length;
    }
  }
  return bytes;
}

// The first and last slot an accept (BulkPaxosCmd) or decide (PaxosPrepCmd)
// carries; {0, 0} for anything else.
std::pair<slotid_t, slotid_t> SlotRange(const janus::Command& cmd) {
  std::vector<slotid_t> slots;
  if (const auto bp = marshallable_cast<BulkPaxosCmd>(cmd); bp.is_some()) {
    slots = bp.unwrap()->slots;
  } else if (const auto pp = marshallable_cast<PaxosPrepCmd>(cmd); pp.is_some()) {
    slots = pp.unwrap()->slots;
  }
  if (slots.empty()) {
    return {0, 0};
  }
  const auto [lo, hi] = std::minmax_element(slots.begin(), slots.end());
  return {*lo, *hi};
}

}  // namespace

void MultiPaxosCommo::SendInOrder(const std::shared_ptr<RpcPeer>& peer,
                                  parid_t par_id,
                                  const char* what,
                                  size_t log_bytes,
                                  std::function<int32_t(srpc::Client*)> send,
                                  std::function<void()> before_queue,
                                  size_t max_backlog_bytes,
                                  slotid_t first_slot,
                                  slotid_t last_slot) {
  if (log_bytes > kMaxLogBytesPerMessage) {
    // Submit refuses such entries, so this only guards other callers.
    Log_error("{} to site {} (partition {}) carries {} log bytes, more than one RPC frame "
              "holds; not sent", what, peer->site_id(), par_id, log_bytes);
    return;
  }
  std::shared_ptr<PeerSendQueue> queue = QueueFor(peer->site_id());
  slotid_t repair_from = 0, repair_to = 0;
  bool repair = false;
  {
    std::lock_guard<std::mutex> lock(queue->mu);
    // A peer that fell behind and has since drained gets the slots it
    // missed, once this lock is released.
    repair = TakeRepair(*queue, &repair_from, &repair_to);
    SendOrQueueLocked(peer, par_id, queue, what, log_bytes, std::move(send),
                      std::move(before_queue), max_backlog_bytes, first_slot, last_slot);
  }
  if (repair) {
    repair_(peer->site_id(), repair_from, repair_to);
  }
}

void MultiPaxosCommo::SendOrQueueLocked(const std::shared_ptr<RpcPeer>& peer,
                                        parid_t par_id,
                                        const std::shared_ptr<PeerSendQueue>& queue,
                                        const char* what,
                                        size_t log_bytes,
                                        std::function<int32_t(srpc::Client*)> send,
                                        std::function<void()> before_queue,
                                        size_t max_backlog_bytes,
                                        slotid_t first_slot,
                                        slotid_t last_slot) {
  if (queue->pending.empty()) {
    int32_t err = 0;
    peer->WithClient([&](srpc::Client* client) { err = send(client); });
    if (err == 0) {
      return;
    }
    if (err != srpc::CLIENT_ERR_IO) {
      // A peer that is down refuses every message, thousands a second.
      if (queue->not_sent++ % 1000 == 0) {
        Log_warn("{} to site {} (partition {}, {} log bytes) not sent: error {} "
                 "({} not sent so far)", what, peer->site_id(), par_id, log_bytes, err,
                 queue->not_sent);
      }
      return;
    }
  }
  const size_t bytes = log_bytes + kMessageOverheadBytes;
  // An accept or decide is dropped only once the queue is already past the
  // limit, never into a shorter one: a message as large as the limit would
  // otherwise be dropped for every site at once, and its round could never
  // reach a quorum. A learner forward is dropped if it would pass the limit.
  const size_t backlog = queue->queued_bytes.load();
  const bool full = first_slot > 0 ? backlog >= max_backlog_bytes
                                   : backlog + bytes > max_backlog_bytes;
  if (max_backlog_bytes > 0 && full) {
    if (queue->dropped++ % 1000 == 0) {
      Log_warn("{} to site {} (partition {}) dropped: {} bytes already wait for that peer "
               "({} dropped so far)", what, peer->site_id(), par_id,
               queue->queued_bytes.load(), queue->dropped);
    }
    if (first_slot > 0) {
      queue->behind_from = queue->behind ? std::min(queue->behind_from, first_slot) : first_slot;
      queue->behind_to = queue->behind ? std::max(queue->behind_to, last_slot) : last_slot;
      queue->behind = true;
    }
    return;
  }
  if (before_queue) {
    before_queue();
  }
  queue->pending.push_back(PeerSendQueue::Message{what, log_bytes, bytes, std::move(send)});
  queue->queued_bytes += bytes;
  if (queue->draining) {
    return;
  }
  queue->draining = true;
  auto poll = PollThread();
  verify(poll.is_some());
  auto self = this;
  auto job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([self, peer, par_id, queue]() {
    self->DrainSendQueue(peer, par_id, queue);
  }));
  poll.unwrap()->add(rusty::Arc<Job>(job));
}

std::shared_ptr<MultiPaxosCommo::PeerSendQueue> MultiPaxosCommo::QueueFor(siteid_t site) {
  std::lock_guard<std::mutex> lock(send_queues_mu_);
  auto& slot = send_queues_[site];
  if (!slot) {
    slot = std::make_shared<PeerSendQueue>();
  }
  return slot;
}

bool MultiPaxosCommo::TakeRepair(PeerSendQueue& queue, slotid_t* from, slotid_t* to) {
  if (!queue.behind || !repair_ || queue.queued_bytes.load() >= kRepairLowWaterBytes) {
    return false;
  }
  *from = queue.behind_from;
  *to = queue.behind_to;
  queue.behind = false;
  return true;
}

void MultiPaxosCommo::SetRepair(std::function<void(siteid_t, slotid_t, slotid_t)> repair) {
  repair_ = std::move(repair);
}

void MultiPaxosCommo::MarkBehind(siteid_t site, slotid_t from, slotid_t to) {
  auto queue = QueueFor(site);
  std::lock_guard<std::mutex> lock(queue->mu);
  queue->behind_from = queue->behind ? std::min(queue->behind_from, from) : from;
  queue->behind_to = queue->behind ? std::max(queue->behind_to, to) : to;
  queue->behind = true;
}

void MultiPaxosCommo::DrainSendQueue(std::shared_ptr<RpcPeer> peer,
                                     parid_t par_id,
                                     std::shared_ptr<PeerSendQueue> queue) {
  // A loopback or LAN socket drains megabytes per millisecond, so a small
  // message is retried every millisecond; a longer backoff would leave the
  // connection idle while thousands of small entries wait behind a large one.
  // The client serializes the whole request before the channel refuses it,
  // though, and this poll thread also writes the buffer out, so a refused
  // multi-MB entry waits several times as long as the refused attempt took.
  // Otherwise the thread spends its time re-serializing the entry instead of
  // draining the buffer it waits for.
  constexpr uint64_t kMinRetryUs = 1000;
  constexpr uint64_t kRetryCostFactor = 4;
  uint64_t waited_us = 0;
  for (;;) {
    std::unique_lock<std::mutex> lock(queue->mu);
    slotid_t repair_from = 0, repair_to = 0;
    if (TakeRepair(*queue, &repair_from, &repair_to)) {
      // Still draining, so what the repair sends queues behind this.
      lock.unlock();
      repair_(peer->site_id(), repair_from, repair_to);
      continue;
    }
    if (queue->pending.empty()) {
      queue->draining = false;
      return;
    }
    const char* what = queue->pending.front().what;
    const size_t log_bytes = queue->pending.front().log_bytes;
    int32_t err = 0;
    const auto attempt_start = std::chrono::steady_clock::now();
    peer->WithClient([&](srpc::Client* client) { err = queue->pending.front().send(client); });
    if (err == srpc::CLIENT_ERR_IO) {
      const uint64_t attempt_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - attempt_start).count());
      const size_t queued = queue->pending.size();
      lock.unlock();
      // Wait the way a quorum wait does, so this poll thread's reactor keeps
      // running and drains the buffer.
      const uint64_t wait_us = std::max(kMinRetryUs, kRetryCostFactor * attempt_us);
      srpc::create_sp_timeout_event(wait_us)->wait();
      waited_us += wait_us;
      if (waited_us >= 10 * 1000 * 1000) {
        Log_warn("{} to site {} (partition {}, {} log bytes) still waits for its connection "
                 "to drain; {} messages queued", what, peer->site_id(), par_id, log_bytes, queued);
        waited_us = 0;
      }
      continue;
    }
    if (err != 0 && queue->not_sent++ % 1000 == 0) {
      Log_warn("{} to site {} (partition {}, {} log bytes) not sent: error {} "
               "({} not sent so far)", what, peer->site_id(), par_id, log_bytes, err,
               queue->not_sent);
    }
    queue->queued_bytes -= queue->pending.front().bytes;
    queue->pending.pop_front();
    waited_us = 0;
  }
}

size_t MultiPaxosCommo::DropAt(parid_t par_id, siteid_t site, int quorum) {
  if (site == self_site_) {
    return 0;
  }
  return PeersWithRoom(par_id, site) >= quorum ? kDropBacklogBytes : 0;
}

int MultiPaxosCommo::Quorum(parid_t par_id) {
  const int n = Config::GetConfig()->GetPartitionSize(par_id) - 1;
  return (n % 2 == 0) ? n / 2 : (n / 2 + 1);
}

int MultiPaxosCommo::PeersWithRoom(parid_t par_id, siteid_t except) {
  int with_room = 0;
  for (const auto& peer : PeersForPartition(par_id)) {
    if (peer->site_id() == except ||
        Config::GetConfig()->SiteById(peer->site_id()).role == 2) {
      continue;
    }
    std::lock_guard<std::mutex> lock(send_queues_mu_);
    const auto it = send_queues_.find(peer->site_id());
    if (it == send_queues_.end() || it->second->queued_bytes.load() < kRoomBytes) {
      with_room++;
    }
  }
  return with_room;
}

void MultiPaxosCommo::SendRepair(parid_t par_id, siteid_t site, const janus::Command& accept,
                                 const janus::Command& decide, size_t log_bytes,
                                 slotid_t from, slotid_t to) {
  auto peer = PeerForSite(par_id, site);
  if (!peer) {
    return;
  }
  // The replies change nothing here: a leader that lost its epoch learns it
  // from its own rounds.
  auto ignore = [](rusty::Arc<Future>) {};
  auto accept_req = std::make_shared<MultiPaxosProxy::RpcBulkAcceptRequest>();
  accept_req->cmd = accept;
  auto accept_attr = std::make_shared<FutureAttr>();
  accept_attr->callback = srpc::FutureCallback::from_callable(ignore);
  SendInOrder(peer, par_id, "BulkAccept (repair)", log_bytes,
              [accept_req, accept_attr](srpc::Client* client) -> int32_t {
                MultiPaxosProxy proxy(client);
                auto result = proxy.async_BulkAccept(*accept_req, *accept_attr);
                if (result.is_err()) return result.unwrap_err();
                Future::safe_release(result.unwrap().raw_future());
                return 0;
              },
              nullptr, kDropBacklogBytes, from, to);
  auto decide_req = std::make_shared<MultiPaxosProxy::RpcBulkDecideRequest>();
  decide_req->cmd = decide;
  auto decide_attr = std::make_shared<FutureAttr>();
  decide_attr->callback = srpc::FutureCallback::from_callable(ignore);
  SendInOrder(peer, par_id, "BulkDecide (repair)", 0,
              [decide_req, decide_attr](srpc::Client* client) -> int32_t {
                MultiPaxosProxy proxy(client);
                auto result = proxy.async_BulkDecide(*decide_req, *decide_attr);
                if (result.is_err()) return result.unwrap_err();
                Future::safe_release(result.unwrap().raw_future());
                return 0;
              },
              nullptr, kDropBacklogBytes, from, to);
}

shared_ptr<PaxosAcceptQuorumEvent> MultiPaxosCommo::BroadcastAccept(
    parid_t par_id,
    slotid_t slot_id,
    ballot_t ballot,
    const janus::Command& cmd) {
  (void)par_id;
  (void)slot_id;
  (void)ballot;
  (void)cmd;
  verify(0);
  return std::make_shared<PaxosAcceptQuorumEvent>(1, 1);
}

void MultiPaxosCommo::ForwardToLearner(
    parid_t par_id,
    uint64_t slot,
    ballot_t ballot,
    const janus::Command& cmd,
    const std::function<void(uint64_t, ballot_t)>& cb) {
  for (const auto& peer : PeersForPartition(par_id)) {
    const auto site_id = peer->site_id();
    const int role = Config::GetConfig()->SiteById(site_id).role;
    Log_debug("ForwardToLearner: site_id={}, role={}", site_id, role);
    if (role != 2) {
      continue;
    }

    FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
        [cb](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error forwarding to learner");
            return;
          }
          uint64_t returned_slot = 0;
          ballot_t returned_ballot = 0;
          srpc::deserialize_from(future->get_reply(), returned_slot);
          srpc::deserialize_from(future->get_reply(), returned_ballot);
          cb(returned_slot, returned_ballot);
        });

    auto req = std::make_shared<MultiPaxosProxy::RpcForwardToLearnerServerRequest>();
    req->par_id = par_id;
    req->slot = slot;
    req->ballot = ballot;
    req->cmd = cmd;
    auto shared_attr = std::make_shared<FutureAttr>(std::move(attr));
    const auto entry = marshallable_cast<LogEntry>(cmd);
    const size_t entry_bytes = entry.is_some() ? entry.unwrap()->length : 0;
    SendInOrder(peer, par_id, "ForwardToLearner", entry_bytes,
                [req, shared_attr](srpc::Client* client) -> int32_t {
                  MultiPaxosProxy proxy(client);
                  auto result = proxy.async_ForwardToLearnerServer(*req, *shared_attr);
                  if (result.is_err()) return result.unwrap_err();
                  Future::safe_release(result.unwrap().raw_future());
                  return 0;
                },
                [req]() {
                  // cmd shares p1's log entry, and p1 replays it right after
                  // forwarding it; the replay edits the bytes in place (it
                  // clears a delete's flag bit in the table id). A forward
                  // that waits is serialized later, so it keeps a copy of
                  // the entry as it is now.
                  const auto entry = marshallable_cast<LogEntry>(req->cmd);
                  if (entry.is_some()) {
                    req->cmd = janus::Command(rusty::Arc<LogEntry>::make(*entry.unwrap()));
                  }
                },
                entry_bytes <= kMaxControlEntryBytes
                    ? kMaxLearnerBacklogBytes + kControlReserveBytes
                    : kMaxLearnerBacklogBytes);
  }
}

void MultiPaxosCommo::BroadcastDecide(
    parid_t par_id,
    slotid_t slot_id,
    ballot_t ballot,
    const janus::Command& cmd) {
  (void)par_id;
  (void)slot_id;
  (void)ballot;
  (void)cmd;
  verify(0);
}

shared_ptr<PaxosAcceptQuorumEvent> MultiPaxosCommo::BroadcastSyncLog(
    parid_t par_id,
    const janus::Command& cmd,
    const std::function<void(shared_ptr<janus::Command>, ballot_t, int)>& cb) {
  Log_info("invoke BroadcastSyncLog to prepare for the failover");
  const int n = Config::GetConfig()->GetPartitionSize(par_id) - 1;
  const int quorum = (n % 2 == 0) ? n / 2 : (n / 2 + 1);
  auto event = std::make_shared<PaxosAcceptQuorumEvent>(n, quorum);

  for (const auto& peer : PeersForPartition(par_id)) {
    const int role = Config::GetConfig()->SiteById(peer->site_id()).role;
    if (role == 2 || role == 0) {
      continue;
    }

    FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
        [event, cb](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error from SyncLog");
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          janus::Command value;
          srpc::deserialize_from(future->get_reply(), ballot);
          srpc::deserialize_from(future->get_reply(), valid);
          srpc::deserialize_from(future->get_reply(), value);
          cb(std::make_shared<janus::Command>(value), ballot, valid);
          event->FeedResponse(valid);
        });

    verify(cmd.has_value());
    MultiPaxosProxy::RpcSyncLogRequest req;
    req.cmd = cmd;
    peer->WithClient([&](srpc::Client* client) {
      MultiPaxosProxy proxy(client);
      auto result = proxy.async_SyncLog(req, attr);
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    });
  }
  return event;
}

shared_ptr<PaxosAcceptQuorumEvent> MultiPaxosCommo::BroadcastSyncCommit(
    parid_t par_id,
    const janus::Command& cmd,
    const std::function<void(ballot_t, int)>& cb) {
  (void)par_id;
  (void)cmd;
  (void)cb;
  auto event = std::make_shared<PaxosAcceptQuorumEvent>(1, 1);
  event->FeedResponse(true);
  return event;
}

shared_ptr<PaxosAcceptQuorumEvent> MultiPaxosCommo::BroadcastBulkAccept(
    parid_t par_id,
    const janus::Command& cmd,
    const std::function<void(ballot_t, int)>& cb) {
  const int n = Config::GetConfig()->GetPartitionSize(par_id) - 1;
  const int quorum = (n % 2 == 0) ? n / 2 : (n / 2 + 1);
  auto event = std::make_shared<PaxosAcceptQuorumEvent>(n, quorum);

  // A queue too far behind drops the message and notes its slots for repair.
  const auto slots = SlotRange(cmd);
  for (const auto& peer : PeersForPartition(par_id)) {
    const auto site_id = peer->site_id();
    if (Config::GetConfig()->SiteById(site_id).role == 2) {
      continue;
    }

    FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
        [event, cb, site_id](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error from BulkAccept");
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          srpc::deserialize_from(future->get_reply(), ballot);
          srpc::deserialize_from(future->get_reply(), valid);
          if (!valid) {
            Log_debug("Accept invalid response received from {} site", site_id);
          }
          cb(ballot, valid);
          event->FeedResponse(valid);
        });

    verify(cmd.has_value());
    auto req = std::make_shared<MultiPaxosProxy::RpcBulkAcceptRequest>();
    req->cmd = cmd;
    auto shared_attr = std::make_shared<FutureAttr>(std::move(attr));
    SendInOrder(peer, par_id, "BulkAccept", LogBytes(cmd),
                [req, shared_attr](srpc::Client* client) -> int32_t {
                  MultiPaxosProxy proxy(client);
                  auto result = proxy.async_BulkAccept(*req, *shared_attr);
                  if (result.is_err()) return result.unwrap_err();
                  Future::safe_release(result.unwrap().raw_future());
                  return 0;
                },
                nullptr, DropAt(par_id, site_id, quorum), slots.first, slots.second);
  }
  return event;
}

shared_ptr<PaxosAcceptQuorumEvent> MultiPaxosCommo::BroadcastBulkDecide(
    parid_t par_id,
    const janus::Command& cmd,
    const std::function<void(ballot_t, int)>& cb) {
  const int n = Config::GetConfig()->GetPartitionSize(par_id) - 1;
  const int quorum = (n % 2 == 0) ? n / 2 : (n / 2 + 1);
  auto event = std::make_shared<PaxosAcceptQuorumEvent>(n, quorum);
  const auto slots = SlotRange(cmd);

  for (const auto& peer : PeersForPartition(par_id)) {
    if (Config::GetConfig()->SiteById(peer->site_id()).role == 2) {
      continue;
    }

    FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
        [event, cb](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error from BulkDecide");
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          srpc::deserialize_from(future->get_reply(), ballot);
          srpc::deserialize_from(future->get_reply(), valid);
          cb(ballot, valid);
          event->FeedResponse(valid);
        });

    auto req = std::make_shared<MultiPaxosProxy::RpcBulkDecideRequest>();
    req->cmd = cmd;
    auto shared_attr = std::make_shared<FutureAttr>(std::move(attr));
    SendInOrder(peer, par_id, "BulkDecide", 0,
                [req, shared_attr](srpc::Client* client) -> int32_t {
                  MultiPaxosProxy proxy(client);
                  auto result = proxy.async_BulkDecide(*req, *shared_attr);
                  if (result.is_err()) return result.unwrap_err();
                  Future::safe_release(result.unwrap().raw_future());
                  return 0;
                },
                nullptr, DropAt(par_id, peer->site_id(), quorum), slots.first, slots.second);
  }
  return event;
}

}  // namespace janus
