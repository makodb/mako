
#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"

namespace janus {

MultiPaxosCommo::MultiPaxosCommo(rusty::Option<rusty::Arc<PollThread>> poll)
  : Communicator(poll) {
}

void MultiPaxosCommo::BroadcastPrepare(parid_t par_id,
                                       slotid_t slot_id,
                                       ballot_t ballot,
                                       const function<void(Future*)>& cb) {
  verify(0);
  // auto proxies = rpc_par_proxies_[par_id];
  // for (auto& p : proxies) {
  //   if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
  //   auto proxy = (MultiPaxosProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = cb;
  //   Future::safe_release(proxy->async_Prepare(slot_id, ballot, fuattr));
  // }
}

shared_ptr<PaxosPrepareQuorumEvent>
MultiPaxosCommo::BroadcastPrepare(parid_t par_id,
                                  slotid_t slot_id,
                                  ballot_t ballot) {
  verify(0);
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  auto e = Reactor::CreateSpEvent<PaxosPrepareQuorumEvent>(n, n); //marker:ansh debug
  // auto proxies = rpc_par_proxies_[par_id];
  // int cur_batch_idx = current_proxy_batch_idx;
  // current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  // for (int i=0;i<n+1;i++) {
  //   auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
  //   if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
  //   auto proxy = (MultiPaxosProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = [e, ballot](Future* fu) {
  //     ballot_t b = 0;
  //     fu->get_reply() >> b;
  //     e->FeedResponse(b==ballot);
  //     // TODO add max accepted value.
  //   };
  //   Future::safe_release(proxy->async_Prepare(slot_id, ballot, fuattr));
  // }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastAccept(parid_t par_id,
                                 slotid_t slot_id,
                                 ballot_t ballot,
                                 shared_ptr<Marshallable> cmd) {
  verify(0);                               
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
//  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, /2n/2+1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
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
  //     fu->get_reply() >> b;
  //     e->FeedResponse(b==ballot);
  //   };
  //   MarshallDeputy md(cmd);
  //   auto f = proxy->async_Accept(slot_id, ballot, md, fuattr);
  //   Future::safe_release(f);
  // }
  return e;
}

void MultiPaxosCommo::BroadcastAccept(parid_t par_id,
                                      slotid_t slot_id,
                                      ballot_t ballot,
                                      shared_ptr<Marshallable> cmd,
                                      const function<void(Future*)>& cb) {
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
  //   fuattr.callback = cb;
  //   MarshallDeputy md(cmd);
  //   auto f = proxy->async_Accept(slot_id, ballot, md, fuattr);
  //   Future::safe_release(f);
  // }
}

/**
 * @brief forward the committed log the learner 
 * Within the same data center
 */
void MultiPaxosCommo::ForwardToLearner(parid_t par_id,
                                       uint64_t slot,
                                       ballot_t ballot,
                                       shared_ptr<Marshallable> cmd,
                                       const std::function<void(uint64_t, ballot_t)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;

  Log_info("ForwardToLearner: par_id=%d, slot=%lu, n=%d, proxies.size=%zu, batch_idx=%d",
           par_id, slot, n, proxies.size(), cur_batch_idx);

  //auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(1,1);
  int sent_count = 0;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    int site_role = Config::GetConfig()->SiteById(p.first).role;
    Log_debug("ForwardToLearner: site_id=%d, role=%d", p.first, site_role);
    if (site_role!=2) continue;
     auto proxy = (MultiPaxosProxy*) p.second;
     FutureAttr fuattr;
     fuattr.callback = [/*e, */cb] (rusty::Arc<Future> fu) {
        if (fu->get_error_code()!=0) {
          Log_info("received an error message6");
          return;
        }
        uint64_t slot;
        ballot_t ballot;
        // if the learner is killed at this moment, throw an error
        // in datacenter failover, keep learners are alive
        fu->get_reply() >> slot >> ballot;
        cb(slot, ballot);
        //e->FeedResponse(1);
      };
     MarshallDeputy md(cmd);
     Log_info("ForwardToLearner: SENDING to learner site_id=%d, slot=%lu", p.first, slot);
     auto fu_result = proxy->async_ForwardToLearnerServer(par_id, slot, ballot, md, fuattr);
     sent_count++;
     // Arc auto-released

    // auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    // if (Config::GetConfig()->SiteById(p.first).role!=2) continue;
    //  auto proxy = (MultiPaxosProxy*) p.second;
    //  MarshallDeputy md(cmd);
    //  uint64_t *slotr;
    //  ballot_t *ballotr;
    //  proxy->ForwardToLearnerServer(par_id, slot, ballot, md, slotr, ballotr);
    //  cb(*slotr, *ballotr);
  }
  //e->Wait();
}

void MultiPaxosCommo::BroadcastDecide(const parid_t par_id,
                                      const slotid_t slot_id,
                                      const ballot_t ballot,
                                      const shared_ptr<Marshallable> cmd) {
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
  //   MarshallDeputy md(cmd);
  //   auto f = proxy->async_Decide(slot_id, ballot, md, fuattr);
  //   Future::safe_release(f);
  // }
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkPrepare(parid_t par_id,
                                      shared_ptr<Marshallable> cmd,
                                      function<void(ballot_t, int)> cb) {
  verify(0);
  //Log_info("BroadcastBulkPrepare: i am here");
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, k); // marker:debug
  //Log_info("BroadcastBulkPrepare: i am here partition size %d", n);
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
  //     fu->get_reply() >> ballot >> valid;
  //     //Log_info("Received response %d %d", ballot, valid);
  //     cb(ballot, valid);
  //     e->FeedResponse(valid);
  //   };
  //   verify(cmd != nullptr);
  //   MarshallDeputy md(cmd);
  //   auto f = proxy->async_BulkPrepare(md, fuattr);
  //   Future::safe_release(f);
  // }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastPrepare2(parid_t par_id,
                                 shared_ptr<Marshallable> cmd,
                                 const std::function<void(MarshallDeputy, ballot_t, int)>& cb) {
  verify(0);
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, k); //marker:debug
  // auto proxies = rpc_par_proxies_[par_id];
  // vector<Future*> fus;
  // //Log_info("paxos commo bulkaccept: length proxies %d", proxies.size());
  // int cur_batch_idx = current_proxy_batch_idx;
  // current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  // for (int i=0;i<n+1;i++) {
  //   auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
  //   if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
  //   auto proxy = (MultiPaxosProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = [e, cb] (Future* fu) {
  //     i32 valid;
  //     i32 ballot;
  //     MarshallDeputy response_val;
  //     fu->get_reply() >> ballot >> valid >> response_val;
  //     //Log_info("BroadcastPrepare2: received response: %d %d", ballot, valid);
  //     cb(response_val, ballot, valid);
  //     e->FeedResponse(valid);
  //   };
  //   verify(cmd != nullptr);
  //   MarshallDeputy md(cmd);
  //   auto f = proxy->async_BulkPrepare2(md, fuattr);
  //   Future::safe_release(f);
  // }
  return e;
}

// Within the same data center
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastHeartBeat(parid_t par_id,
                                    shared_ptr<Marshallable> cmd,
                                    const function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, k);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message5");
        return;
      }
      i32 valid;
      i32 ballot;
      fu->get_reply() >> ballot >> valid;
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto fu_result = proxy->async_Heartbeat(md, fuattr);
    // Arc auto-released
  }
  return e;
}

// Distant data centers
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastSyncLog(parid_t par_id,
                                  shared_ptr<Marshallable> cmd,
                                  const std::function<void(shared_ptr<MarshallDeputy>, ballot_t, int)>& cb) {
  is_broadcast_syncLog = true;
  Log_info("invoke BroadcastSyncLog to prepare for the failover");
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, k);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue; 
    if (Config::GetConfig()->SiteById(p.first).role==0) continue;
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message3");
        return;
      }
      i32 valid;
      i32 ballot;
      MarshallDeputy response_val;
      fu->get_reply() >> ballot >> valid >> response_val;
      auto sp_md = make_shared<MarshallDeputy>(response_val);
      cb(sp_md, ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto fu_result = proxy->async_SyncLog(md, fuattr);
    // Arc auto-released
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastSyncNoOps(parid_t par_id,
                                  shared_ptr<Marshallable> cmd,
                                  const std::function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  // not old leader, not new leader(old learner)
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n-1, n-1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue;
    if (Config::GetConfig()->SiteById(p.first).role==0) continue; // ??? why skip itself
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message4");
        return;
      }
      i32 valid;
      i32 ballot;
      fu->get_reply() >> ballot >> valid;
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto fu_result = proxy->async_SyncNoOps(md, fuattr);
    // Arc auto-released
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastSyncCommit(parid_t par_id,
                                  shared_ptr<Marshallable> cmd,
                                  const std::function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(1, 1);
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
  //     fu->get_reply() >> ballot >> valid;
  //     cb(ballot, valid);
  //     e->FeedResponse(valid);
  //   };
  //   verify(cmd != nullptr);
  //   MarshallDeputy md(cmd);
  //   auto f = proxy->async_SyncCommit(md, fuattr);
  //   Future::safe_release(f);
  // }
  return e;
}

// Distant data center
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkAccept(parid_t par_id,
                                 shared_ptr<Marshallable> cmd,
                                 const function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, k); //marker:debug
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  //Log_info("cur_batch_idx:%d",cur_batch_idx);
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue;
    auto proxy = (MultiPaxosProxy*) p.second;  // a Proxy pool for the concurrent request
    FutureAttr fuattr;
    int st = p.first;
    fuattr.callback = [e, cb, st] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message2");
        return;
      }
      i32 valid;
      i32 ballot;
      fu->get_reply() >> ballot >> valid;
       // it's possible during failure because the client can receive reponse even the distant server shutdowns
      if (!valid)
        Log_debug("Accept invalid response received from %d site", st);
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto fu_result = proxy->async_BulkAccept(md, fuattr);
    // Arc auto-released
  }
  return e;
}

// Distant data center
shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkDecide(parid_t par_id, 
                                     shared_ptr<Marshallable> cmd,
                                     const function<void(ballot_t, int)>& cb){
  auto proxies = rpc_par_proxies_[par_id];
  int n = Config::GetConfig()->GetPartitionSize(par_id)-1;
  int k = (n%2 == 0) ? n/2 : (n/2 + 1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, k); //marker:debug 
  vector<Future*> fus;
  int cur_batch_idx = current_proxy_batch_idx;
  current_proxy_batch_idx=(current_proxy_batch_idx+1)%proxy_batch_size;
  for (int i=0;i<n+1;i++) {
    auto p = proxies.at(cur_batch_idx*(Config::GetConfig()->GetPartitionSize(par_id)) + i);
    if (Config::GetConfig()->SiteById(p.first).role==2) continue;
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (rusty::Arc<Future> fu) {
      if (fu->get_error_code()!=0) {
        Log_info("received an error message");
        return;
      }
      i32 valid;
      i32 ballot;
      fu->get_reply() >> ballot >> valid;
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    MarshallDeputy md(cmd);
    auto fu_result = proxy->async_BulkDecide(md, fuattr);
    // Arc auto-released
  }
  return e;
}

} // namespace janus
