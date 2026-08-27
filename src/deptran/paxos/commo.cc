#include "commo.h"

#include "../rcc_rpc.h"

namespace janus {

MultiPaxosCommo::MultiPaxosCommo(
    rusty::Option<rusty::Arc<rrr::PollThread>> poll)
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
    attr.callback = rrr::FutureCallback::from_callable(
        [cb](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error forwarding to learner");
            return;
          }
          uint64_t returned_slot = 0;
          ballot_t returned_ballot = 0;
          rrr::deserialize_from(future->get_reply(), returned_slot);
          rrr::deserialize_from(future->get_reply(), returned_ballot);
          cb(returned_slot, returned_ballot);
        });

    MultiPaxosProxy::RpcForwardToLearnerServerRequest req;
    req.par_id = par_id;
    req.slot = slot;
    req.ballot = ballot;
    req.cmd = cmd;
    peer->WithClient([&](rrr::Client* client) {
      MultiPaxosProxy proxy(client);
      auto result = proxy.async_ForwardToLearnerServer(req, attr);
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
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
    attr.callback = rrr::FutureCallback::from_callable(
        [event, cb](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error from SyncLog");
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          janus::Command value;
          rrr::deserialize_from(future->get_reply(), ballot);
          rrr::deserialize_from(future->get_reply(), valid);
          rrr::deserialize_from(future->get_reply(), value);
          cb(std::make_shared<janus::Command>(value), ballot, valid);
          event->FeedResponse(valid);
        });

    verify(cmd.has_value());
    MultiPaxosProxy::RpcSyncLogRequest req;
    req.cmd = cmd;
    peer->WithClient([&](rrr::Client* client) {
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

  for (const auto& peer : PeersForPartition(par_id)) {
    const auto site_id = peer->site_id();
    if (Config::GetConfig()->SiteById(site_id).role == 2) {
      continue;
    }

    FutureAttr attr;
    attr.callback = rrr::FutureCallback::from_callable(
        [event, cb, site_id](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error from BulkAccept");
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          rrr::deserialize_from(future->get_reply(), ballot);
          rrr::deserialize_from(future->get_reply(), valid);
          if (!valid) {
            Log_debug("Accept invalid response received from {} site", site_id);
          }
          cb(ballot, valid);
          event->FeedResponse(valid);
        });

    verify(cmd.has_value());
    MultiPaxosProxy::RpcBulkAcceptRequest req;
    req.cmd = cmd;
    peer->WithClient([&](rrr::Client* client) {
      MultiPaxosProxy proxy(client);
      auto result = proxy.async_BulkAccept(req, attr);
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    });
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

  for (const auto& peer : PeersForPartition(par_id)) {
    if (Config::GetConfig()->SiteById(peer->site_id()).role == 2) {
      continue;
    }

    FutureAttr attr;
    attr.callback = rrr::FutureCallback::from_callable(
        [event, cb](rusty::Arc<Future> future) {
          if (future->get_error_code() != 0) {
            Log_info("received an error from BulkDecide");
            return;
          }
          i32 valid = 0;
          i32 ballot = 0;
          rrr::deserialize_from(future->get_reply(), ballot);
          rrr::deserialize_from(future->get_reply(), valid);
          cb(ballot, valid);
          event->FeedResponse(valid);
        });

    MultiPaxosProxy::RpcBulkDecideRequest req;
    req.cmd = cmd;
    peer->WithClient([&](rrr::Client* client) {
      MultiPaxosProxy proxy(client);
      auto result = proxy.async_BulkDecide(req, attr);
      if (result.is_ok()) {
        Future::safe_release(result.unwrap().raw_future());
      }
    });
  }
  return event;
}

}  // namespace janus
