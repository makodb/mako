
#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"

namespace janus {

MultiPaxosCommo::MultiPaxosCommo(PollMgr* poll) : Communicator(poll) {
//  verify(poll != nullptr);
}

void MultiPaxosCommo::BroadcastPrepare(parid_t par_id,
                                       slotid_t slot_id,
                                       ballot_t ballot,
                                       const function<void(Future*)>& cb) {

  auto proxies = rpc_par_proxies_[par_id];
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = cb;
    Future::safe_release(proxy->async_Prepare(slot_id, ballot, fuattr));
  }
}

shared_ptr<PaxosPrepareQuorumEvent>
MultiPaxosCommo::BroadcastPrepare(parid_t par_id,
                                  slotid_t slot_id,
                                  ballot_t ballot) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<PaxosPrepareQuorumEvent>(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, ballot](Future* fu) {
      ballot_t b = 0;
      fu->get_reply() >> b;
      e->FeedResponse(b==ballot);
      // TODO add max accepted value.
    };
    Future::safe_release(proxy->async_Prepare(slot_id, ballot, fuattr));
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastAccept(parid_t par_id,
                                 slotid_t slot_id,
                                 ballot_t ballot,
                                 shared_ptr<Marshallable> cmd) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
//  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n/2+1);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, ballot] (Future* fu) {
      ballot_t b = 0;
      fu->get_reply() >> b;
      e->FeedResponse(b==ballot);
    };
    MarshallDeputy md(cmd);
    auto f = proxy->async_Accept(slot_id, ballot, md, fuattr);
    Future::safe_release(f);
  }
  return e;
}

void MultiPaxosCommo::BroadcastAccept(parid_t par_id,
                                      slotid_t slot_id,
                                      ballot_t ballot,
                                      shared_ptr<Marshallable> cmd,
                                      const function<void(Future*)>& cb) {
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = cb;
    MarshallDeputy md(cmd);
    auto f = proxy->async_Accept(slot_id, ballot, md, fuattr);
    Future::safe_release(f);
  }
//  verify(0);
}

void MultiPaxosCommo::BroadcastDecide(const parid_t par_id,
                                      const slotid_t slot_id,
                                      const ballot_t ballot,
                                      const shared_ptr<Marshallable> cmd) {
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](Future* fu) {};
    MarshallDeputy md(cmd);
    auto f = proxy->async_Decide(slot_id, ballot, md, fuattr);
    Future::safe_release(f);
  }
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkPrepare(parid_t par_id,
                                      shared_ptr<Marshallable> cmd,
                                      const function<void(ballot_t, int)>& cb) override {
  Log_info("BroadcastBulkPrepare: i am here");
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (Future* fu) {
      i32 valid;
      ballot_t ballot;
      fu->get_reply() >> ballot >> valid;
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto f = proxy->async_BulkPrepare(md);
    Future::safe_release(f);
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastPrepare2(parid_t par_id,
                                 shared_ptr<Marshallable> cmd,
                                 const std::function<void(MarshallDeputy, ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  //Log_info("paxos commo bulkaccept: length proxies %d", proxies.size());
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (Future* fu) {
      i32 valid;
      ballot_t ballot;
      MarshallDeputy response_val;
      fu->get_reply() >> ballot >> valid >> response_val;
      cb(response_val, ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto f = proxy->async_BulkPrepare2(md, fuattr);
    Future::safe_release(f);
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastHeartBeat(parid_t par_id,
                                    shared_ptr<Marshallable> cmd,
                                    const function<void(ballot_t, int)>& cb) override {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (Future* fu) {
      i32 valid;
      ballot_t ballot;
      fu->get_reply() >> ballot >> valid;
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto f = proxy->async_Heartbeat(md);
    Future::safe_release(f);
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkAccept(parid_t par_id,
                                 shared_ptr<Marshallable> cmd,
                                 const function<void(ballot_t, int)>& cb) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
  auto proxies = rpc_par_proxies_[par_id];
  vector<Future*> fus;
  //Log_info("paxos commo bulkaccept: length proxies %d", proxies.size());
  for (auto& p : proxies) {
    auto proxy = (MultiPaxosProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, cb] (Future* fu) {
      i32 valid;
      ballot_t ballot;
      fu->get_reply() >> ballot >> valid;
      cb(ballot, valid);
      e->FeedResponse(valid);
    };
    verify(cmd != nullptr);
    MarshallDeputy md(cmd);
    auto f = proxy->async_BulkAccept(md, fuattr);
    Future::safe_release(f);
  }
  return e;
}

shared_ptr<PaxosAcceptQuorumEvent>
MultiPaxosCommo::BroadcastBulkDecide(parid_t par_id, 
                                     shared_ptr<Marshallable> cmd,
                                     const function<void(ballot_t, int)>& cb){
    auto proxies = rpc_par_proxies_[par_id];
    int n = Config::GetConfig()->GetPartitionSize(par_id);
    auto e = Reactor::CreateSpEvent<PaxosAcceptQuorumEvent>(n, n);
    vector<Future*> fus;
    for (auto& p : proxies) {
        auto proxy = (MultiPaxosProxy*) p.second;
        FutureAttr fuattr;
        fuattr.callback = [e, cb] (Future* fu) {
          i32 valid;
          ballot_t ballot;
          fu->get_reply() >> ballot >> valid;
          cb(ballot, valid);
          e->FeedResponse(valid);
        };
        MarshallDeputy md(cmd);
        auto f = proxy->async_BulkDecide(md, fuattr);
        Future::safe_release(f);
    }
    return e;
}

} // namespace janus
