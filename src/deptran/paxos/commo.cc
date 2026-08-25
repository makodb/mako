
#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"

namespace janus {

MultiPaxosCommo::MultiPaxosCommo(rusty::Option<rusty::Arc<PollThread>> poll)
  : Communicator(std::move(poll)) {
}

// removed `MultiPaxosCommo::SendForward` —
// never called from anywhere in the tree; was a stub for an
// unwired-up Jetpack forward-to-leader path.  The corresponding
// `Forward` RPC declaration in rcc_rpc.rpc and the
// `MultiPaxosServiceImpl::Forward` empty handler are also gone.

// removed deprecated callback-style
// `void MultiPaxosCommo::BroadcastPrepare(parid_t, slotid_t, ballot_t,
// callback)` — body had `verify(0);` and was mostly commented out;
// no live callers anywhere.

// removed `MultiPaxosCommo::BroadcastPrepare`
// (parid, slot, ballot) — body was a `verify(0);` shell with the
// real implementation commented out.  Only call site was the
// now-deleted `CoordinatorMultiPaxos::Prepare()`.

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastAccept(parid_t par_id,
                                 slotid_t slot_id,
                                 ballot_t ballot,
                                 const janus::Command& cmd) {
  verify(0);
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
//  auto e = reactor_create_sp_event<PaxosAcceptQuorumEvent>(n, /2n/2+1);
  auto e = std::make_shared<PaxosAcceptQuorumEvent>(n, n);
  // auto proxies = rpc_par_proxies_[par_id];
  // vector<Future*> fus;
  // int cur_batch_idx = current_proxy_batch_idx;
  // current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  // for (int i=0;i<n+1;i++) {
  //   auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
  //   if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
  //   auto proxy = (MultiPaxosProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = [e, ballot] (Future* fu) {
  //     ballot_t b = 0;
  //     srpc::deserialize_from(fu->get_reply(), b);
  //     e->FeedResponse(b==ballot);
  //   };
  //   janus::Command md(cmd);
  //   auto f = proxy->async_Accept(slot_id, ballot, md, fuattr);
  //   Future::safe_release(f);
  // }
  return e;
}

// removed deprecated callback-style
// `void MultiPaxosCommo::BroadcastAccept(parid_t, slotid_t, ballot_t,
// cmd, callback)` — body had `verify(0);` and was mostly commented
// out; no live callers anywhere.

/**
 * @brief forward the committed log the learner
 * Within the same data center
 */
void MultiPaxosCommo::ForwardToLearner(parid_t par_id,
                                       uint64_t slot,
                                       ballot_t ballot,
                                       const janus::Command& cmd,
                                       const std::function<void(uint64_t, ballot_t)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;

  // Log_info("ForwardToLearner: par_id={}, slot={}, n={}, proxies.size={}, batch_idx={}",
  //          par_id, slot, n, proxies.size(), cur_batch_idx);

  //auto e = reactor_create_sp_event<PaxosAcceptQuorumEvent>(1,1);
  int sent_count = 0;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    int site_role = Config::GetConfig()->SiteById(p.first).role;
    Log_debug("ForwardToLearner: site_id={}, role={}", p.first, site_role);
    if (site_role!=2) continue;
     auto proxy = (MultiPaxosProxy*) p.second;
     FutureAttr fuattr;
     fuattr.callback = srpc::FutureCallback::from_callable([/*e, */cb] (rusty::Arc<Future> fu) {
        if (fu->get_error_code()!=0) {
          Log_info("received an error message6");
          return;
        }
        uint64_t slot;
        ballot_t ballot;
        // if the learner is killed at this moment, throw an error
        // in datacenter failover, keep learners are alive
        srpc::deserialize_from(fu->get_reply(), slot);
        srpc::deserialize_from(fu->get_reply(), ballot);
        cb(slot, ballot);
        //e->FeedResponse(1);
	      });
	     janus::Command md(cmd);
	     //Log_info("ForwardToLearner: SENDING to learner site_id={}, slot={}", p.first, slot);
       MultiPaxosProxy::RpcForwardToLearnerServerRequest req;
       req.par_id = par_id;
       req.slot = slot;
       req.ballot = ballot;
       req.cmd = md;
	     auto fu_result = proxy->async_ForwardToLearnerServer(req, fuattr);
	     sent_count++;
	     // Arc auto-released

    // auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    // if (Config::GetConfig()->SiteById(p.first).role!=2) continue;
    //  auto proxy = (MultiPaxosProxy*) p.second;
    //  janus::Command md(cmd);
    //  uint64_t *slotr;
    //  ballot_t *ballotr;
    //  proxy->ForwardToLearnerServer(par_id, slot, ballot, md, slotr, ballotr);
    //  cb(*slotr, *ballotr);
  }
  //e->wait();
}

void MultiPaxosCommo::BroadcastDecide(const parid_t par_id,
                                      const slotid_t slot_id,
                                      const ballot_t ballot,
                                      const janus::Command& cmd) {
  verify(0);
  // int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  // auto proxies = rpc_par_proxies_[par_id];
  // vector<Future*> fus;
  // int cur_batch_idx = current_proxy_batch_idx;
  // current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  // for (int i=0;i<n+1;i++) {
  //   auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
  //   if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
  //   auto proxy = (MultiPaxosProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = [](Future* fu) {};
  //   janus::Command md(cmd);
  //   auto f = proxy->async_Decide(slot_id, ballot, md, fuattr);
  //   Future::safe_release(f);
  // }
}

// removed `MultiPaxosCommo::BroadcastBulkPrepare`
// — became dead in Phase 4e-25 when the only sender
// (`PaxosWorker::SendBulkPrepare`) went away.  The body was already a
// `verify(0)`-then-commented-out shell.

// removed `MultiPaxosCommo::BroadcastPrepare2`
// — only call site was the now-deleted
// `BulkCoordinatorMultiPaxos::Prepare()`; body was a
// `verify(0)`-then-commented-out shell.

// removed `MultiPaxosCommo::BroadcastHeartBeat`
// — became dead in Phase 4e-25 when the only sender
// (`PaxosWorker::SendHeartBeat`) went away.

// Distant data centers
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastSyncLog(parid_t par_id,
                                  const janus::Command& cmd,
                                  const std::function<void(shared_ptr<janus::Command>, ballot_t, int)>& cb) {
  is_broadcast_syncLog = true;
  Log_info("invoke BroadcastSyncLog to prepare for the failover");
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = std::make_shared<PaxosAcceptQuorumEvent>(n, k);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue;
    if (Config::GetConfig()->SiteById(p.first).role==0) continue;
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = srpc::FutureCallback::from_callable([e, cb] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message3");
        return;
      }
      i32 valid;
      i32 ballot;
      janus::Command response_val;
      srpc::deserialize_from(fu->get_reply(), ballot);
      srpc::deserialize_from(fu->get_reply(), valid);
      srpc::deserialize_from(fu->get_reply(), response_val);
      auto sp_md = make_shared<janus::Command>(response_val);
      cb(sp_md, ballot, valid);
      e->FeedResponse(valid);
    });
    verify(cmd.has_value());
    janus::Command md(cmd);
    MultiPaxosProxy::RpcSyncLogRequest req;
    req.cmd = md;
    auto fu_result = proxy->async_SyncLog(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

// removed `MultiPaxosCommo::BroadcastSyncNoOps`
// — became dead in Phase 4e-25 when the only sender
// (`PaxosWorker::SendSyncNoOpLog`) went away.

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastSyncCommit(parid_t par_id,
                                  const janus::Command& cmd,
                                  const std::function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = std::make_shared<PaxosAcceptQuorumEvent>(1, 1);
  e->FeedResponse(1);
  // auto proxies = rpc_par_proxies_[par_id];
  // vector<Future*> fus;
  // int cur_batch_idx = current_proxy_batch_idx;
  // current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  // for (int i=0;i<n+1;i++) {
  //   auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
  //   if (Config::GetConfig()->SiteById(p.first).role==2) continue;
  //   if (Config::GetConfig()->SiteById(p.first).role==0) continue;
  //   auto proxy = (MultiPaxosProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = [e, cb] (Future* fu) {
  //     i32 valid;
  //     i32 ballot;
  //     srpc::deserialize_from(fu->get_reply(), ballot);
  //     srpc::deserialize_from(fu->get_reply(), valid);
  //     cb(ballot, valid);
  //     e->FeedResponse(valid);
  //   };
  //   verify(cmd != nullptr);
  //   janus::Command md(cmd);
  //   auto f = proxy->async_SyncCommit(md, fuattr);
  //   Future::safe_release(f);
  // }
  return e;
}

// Distant data center
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkAccept(parid_t par_id,
                                 const janus::Command& cmd,
                                 const function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = std::make_shared<PaxosAcceptQuorumEvent>(n, k); //marker:debug
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  //Log_info("cur_batch_idx:{}",cur_batch_idx);
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue;
    auto proxy = (MultiPaxosProxy*) p.second;  // a Proxy pool for the concurrent request
    FutureAttr fuattr;
    int st = p.first;
    fuattr.callback = srpc::FutureCallback::from_callable([e, cb, st] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message2");
        return;
      }
      i32 valid;
      i32 ballot;
      srpc::deserialize_from(fu->get_reply(), ballot);
      srpc::deserialize_from(fu->get_reply(), valid);
       // it's possible during failure because the client can receive reponse even the distant server shutdowns
      if (!valid)
        Log_debug("Accept invalid response received from {} site", st);
      cb(ballot, valid);
      e->FeedResponse(valid);
    });
    verify(cmd.has_value());
    janus::Command md(cmd);
    MultiPaxosProxy::RpcBulkAcceptRequest req;
    req.cmd = md;
    auto fu_result = proxy->async_BulkAccept(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

// Distant data center
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkDecide(parid_t par_id,
                                     const janus::Command& cmd,
                                     const function<void(ballot_t, int)>& cb){
  auto proxies = rpc_par_proxies_[par_id];
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = std::make_shared<PaxosAcceptQuorumEvent>(n, k); //marker:debug
  vector<rusty::Arc<Future>> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue;
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = srpc::FutureCallback::from_callable([e, cb] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message");
        return;
      }
      i32 valid;
      i32 ballot;
      srpc::deserialize_from(fu->get_reply(), ballot);
      srpc::deserialize_from(fu->get_reply(), valid);
      cb(ballot, valid);
      e->FeedResponse(valid);
    });
    janus::Command md(cmd);
    MultiPaxosProxy::RpcBulkDecideRequest req;
    req.cmd = md;
    auto fu_result = proxy->async_BulkDecide(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

} // namespace janus
