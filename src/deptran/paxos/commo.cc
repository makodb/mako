#include "commo.h"

#include "../rcc_rpc.h"
#include "../paxos_worker.h"

namespace janus {

namespace {

// @unsafe - Snapshot the legacy peer under its lock, then let Rust wait for
// admission with no Client/peer lock held. No accepted request is resubmitted.
// The outbox pump passes ReplicationOutbox::kNoAdmissionDeadline: congestion on
// an open connection delays a replication request and never fails it.
srpc::FutureResult request_replication(const RpcPeer& peer, i32 rpc_id,
                                      const FutureAttr& attr,
                                      uint64_t admission_timeout_us,
                                      ReplicationWrite& write) {
  auto connection = peer.WithClient([](srpc::Client* client) {
    return client->connection();
  });
  if (connection.is_none())
    return srpc::FutureResult::Err(srpc::CLIENT_ERR_NOT_CONNECTED);
  return connection.unwrap()->request_with_admission(
      rpc_id, attr, admission_timeout_us,
      [&write](srpc::BinaryWriteArchive& archive) { write(archive); });
}

}  // namespace

// @unsafe - Communicator resolved (or created) the poll thread; every Send
// must run on it.
MultiPaxosCommo::MultiPaxosCommo(
    rusty::Option<rusty::Arc<srpc::PollThread>> poll)
    : Communicator(std::move(poll)), owner_(PollThread().unwrap()) {}

// @unsafe - Owner-thread outbox lookup; see ReplicationOutbox.
void MultiPaxosCommo::Send(const rusty::CommoPeerPtr& peer,
                           ReplicationMessage message) {
  // outboxes_ is unsynchronized: check the thread before touching it.
  verify(owner_->is_current_thread());
  const siteid_t site_id = peer->site_id();
  rusty::Option<ReplicationOutbox> found{rusty::None};
  for (const auto& slot : outboxes_) {
    if (slot.site_id == site_id) {
      found = rusty::Some(slot.outbox.clone());
      break;
    }
  }
  if (found.is_none()) {
    ReplicationOutbox created(
        site_id, owner_.clone(),
        [peer](i32 rpc_id, const FutureAttr& attr, uint64_t timeout_us,
               ReplicationWrite& write) {
          return request_replication(*peer, rpc_id, attr, timeout_us, write);
        });
    outboxes_.push(OutboxSlot{site_id, created.clone()});
    found = rusty::Some(std::move(created));
  }
  // Send may run the pump, and so a refusal hook, inline: no reference into
  // outboxes_ is held across it.
  found.unwrap().Send(std::move(message));
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

// @unsafe - Bridge legacy Paxos delivery into the learner's ordered outbox.
void MultiPaxosCommo::ForwardToLearner(
    parid_t par_id, uint64_t slot, ballot_t ballot, const janus::Command& cmd) {
  for (const auto& peer : PeersForPartition(par_id)) {
    if (Config::GetConfig()->SiteById(peer->site_id()).role != 2)
      continue;

    MultiPaxosProxy::RpcForwardToLearnerServerRequest req;
    req.par_id = par_id;
    req.slot = slot;
    req.ballot = ballot;
    req.cmd = cmd;
    // Waiting in app_next_ would let another commit overtake the remainder of
    // its delivery batch, so only the learner's pump fiber waits for queue
    // space; Send never suspends this caller and keeps commit order.
    ReplicationMessage message;
    message.rpc_id = MultiPaxosService::FORWARDTOLEARNERSERVER;
    message.attr.callback = srpc::FutureCallback::from_callable(
        [](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0)
            Log_error("learner forwarding failed: {}", future->get_error_code());
        });
    message.write = [req = std::move(req)](srpc::BinaryWriteArchive& archive) {
      srpc::Serialize_::serialize(req.par_id, archive);
      srpc::Serialize_::serialize(req.slot, archive);
      srpc::Serialize_::serialize(req.ballot, archive);
      srpc::Serialize_::serialize(req.cmd, archive);
    };
    message.refused = [](i32 error) {
      Log_error("learner forwarding admission failed: {}", error);
    };
    Send(peer, std::move(message));
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

// @unsafe - Legacy Paxos fanout; Rust owns bounded channel admission.
rusty::Arc<PaxosAcceptQuorumEvent> MultiPaxosCommo::BroadcastBulkAccept(
    parid_t par_id, const janus::Command& cmd) {
  const int n = Config::GetConfig()->GetPartitionSize(par_id) - 1;
  const int quorum = (n % 2 == 0) ? n / 2 : (n / 2 + 1);
  auto event = rusty::Arc<PaxosAcceptQuorumEvent>::make(n, quorum);

  for (const auto& peer : PeersForPartition(par_id)) {
    const auto site_id = peer->site_id();
    if (Config::GetConfig()->SiteById(site_id).role == 2) {
      continue;
    }

    ReplicationMessage message;
    message.rpc_id = MultiPaxosService::BULKACCEPT;
    message.attr.callback = srpc::FutureCallback::from_callable(
        [event, site_id](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_error("BulkAccept failed: {}", future->get_error_code());
            event->FeedResponse(false);
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          srpc::deserialize_from(future->get_reply(), ballot);
          srpc::deserialize_from(future->get_reply(), valid);
          if (!valid) {
            Log_debug("Accept invalid response received from {} site", site_id);
          }
          event->FeedResponse(valid);
        });

    verify(cmd.has_value());
    message.write = [cmd](srpc::BinaryWriteArchive& archive) {
      srpc::Serialize_::serialize(cmd, archive);
    };
    message.refused = [event](i32 error) {
      Log_error("BulkAccept admission failed: {}", error);
      event->FeedResponse(false);
    };
    // Queued, not awaited: a congested destination cannot hold back the
    // others' votes, so the quorum forms from whichever replicas keep up.
    Send(peer, std::move(message));
  }
  return event;
}

// @unsafe - The accept quorum already chose this value. Each decision joins
// its destination's outbox behind that destination's accept, so it reaches
// every replica after the accept, in per-connection TCP order.
void MultiPaxosCommo::BroadcastBulkDecide(
    parid_t par_id, const janus::Command& cmd) {

  for (const auto& peer : PeersForPartition(par_id)) {
    if (Config::GetConfig()->SiteById(peer->site_id()).role == 2) {
      continue;
    }

    ReplicationMessage message;
    message.rpc_id = MultiPaxosService::BULKDECIDE;
    message.attr.callback = srpc::FutureCallback::from_callable(
        [](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_error("BulkDecide failed: {}", future->get_error_code());
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          srpc::deserialize_from(future->get_reply(), ballot);
          srpc::deserialize_from(future->get_reply(), valid);
          if (!valid)
            ElectionState::instance()->step_down(ballot);
        });
    message.write = [cmd](srpc::BinaryWriteArchive& archive) {
      srpc::Serialize_::serialize(cmd, archive);
    };
    message.refused = [](i32 error) {
      Log_error("BulkDecide admission failed: {}", error);
    };
    Send(peer, std::move(message));
  }
}

}  // namespace janus
