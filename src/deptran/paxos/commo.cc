#include "commo.h"

#include "../rcc_rpc.h"
#include "../paxos_worker.h"

namespace janus {

namespace {

// @unsafe - Snapshot the legacy peer under its lock, then let Rust wait for
// admission with no Client/peer lock held. No accepted request is resubmitted.
template <typename Write>
srpc::FutureResult request_replication(const RpcPeer& peer, i32 rpc_id,
                                      const FutureAttr& attr, Write&& write) {
  auto connection = peer.WithClient([](srpc::Client* client) {
    return client->connection();
  });
  if (connection.is_none())
    return srpc::FutureResult::Err(srpc::CLIENT_ERR_NOT_CONNECTED);
  return connection.unwrap()->request_with_admission(
      rpc_id, attr, 30'000'000, std::forward<Write>(write));
}

}  // namespace

MultiPaxosCommo::MultiPaxosCommo(
    rusty::Option<rusty::Arc<srpc::PollThread>> poll)
    : Communicator(std::move(poll)) {}

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

// @unsafe - Bridge legacy Paxos delivery into a separately owned SRPC fiber.
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
    // its delivery batch. Only this child fiber may suspend for queue space.
    Fiber::create_run([peer, req = std::move(req)] {
      FutureAttr attr;
      attr.callback = srpc::FutureCallback::from_callable(
          [](rusty::Arc<Future> future) {
            if (future->get_error_code() != 0)
              Log_error("learner forwarding failed: {}", future->get_error_code());
          });
      auto result = request_replication(
          *peer, MultiPaxosService::FORWARDTOLEARNERSERVER, attr,
          [&req](srpc::BinaryWriteArchive& archive) {
            srpc::Serialize_::serialize(req.par_id, archive);
            srpc::Serialize_::serialize(req.slot, archive);
            srpc::Serialize_::serialize(req.ballot, archive);
            srpc::Serialize_::serialize(req.cmd, archive);
          });
      if (result.is_ok())
        Future::safe_release(result.unwrap());
      else
        Log_error("learner forwarding admission failed: {}", result.unwrap_err());
    });
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

    FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
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
    auto result = request_replication(
        *peer, MultiPaxosService::BULKACCEPT, attr,
        [&cmd](srpc::BinaryWriteArchive& archive) {
          srpc::Serialize_::serialize(cmd, archive);
        });
    if (result.is_ok()) {
      Future::safe_release(result.unwrap());
    } else {
      Log_error("BulkAccept admission failed: {}", result.unwrap_err());
      event->FeedResponse(false);
    }
  }
  return event;
}

// @unsafe - The accept quorum already chose this value; dispatch each decision
// after its accept was admitted, preserving per-connection TCP ordering.
void MultiPaxosCommo::BroadcastBulkDecide(
    parid_t par_id, const janus::Command& cmd) {

  for (const auto& peer : PeersForPartition(par_id)) {
    if (Config::GetConfig()->SiteById(peer->site_id()).role == 2) {
      continue;
    }

    FutureAttr attr;
    attr.callback = srpc::FutureCallback::from_callable(
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

    auto result = request_replication(
        *peer, MultiPaxosService::BULKDECIDE, attr,
        [&cmd](srpc::BinaryWriteArchive& archive) {
          srpc::Serialize_::serialize(cmd, archive);
        });
    if (result.is_ok())
      Future::safe_release(result.unwrap());
    else
      Log_error("BulkDecide admission failed: {}", result.unwrap_err());
  }
}

}  // namespace janus
