#include "../procedure.h"
#include "../rcc/tx.h"
#include "../rcc/graph_marshaler.h"
#include "commo.h"
#include "../../rrr/misc/marshal.hpp"

namespace janus {

void TroadCommo::SendDispatch(vector<TxPieceData>& cmd,
                              const function<void(int res,
                                                  TxnOutput& cmd,
                                                  RccGraph& graph)>& callback) {
  rrr::FutureAttr fuattr;
  auto tid = cmd[0].root_id_;
  auto par_id = cmd[0].partition_id_;
  std::function<void(rusty::Arc<Future>)> cb =
      [callback, tid, par_id](rusty::Arc<Future> fu) {
        if (fu->get_error_code() != 0) {
          Log_info("Get a error message in reply");
          return;
        }
        int res;
        TxnOutput output;
        MarshallDeputy md;
        fu->get_reply() >> res >> output >> md;
        if (md.kind_ == MarshallDeputy::EMPTY_GRAPH) {
          RccGraph rgraph;
//          auto v = rgraph.CreateV(tid);
//          RccTx& info = *v;
//          info.partition_.insert(par_id);
//          verify(rgraph.vertex_index().size() > 0);
          callback(res, output, rgraph);
        } else if (md.kind_ == MarshallDeputy::RCC_GRAPH) {
          auto sp_graph = marshallable_cast<RccGraph>(md);
          verify(sp_graph);
          callback(res, output, *sp_graph);
        } else {
          verify(0);
        }
      };
  fuattr.callback = cb;
  auto proxy = NearestProxyForPartition(cmd[0].PartitionId()).second;
  Log_debug("dispatch to %ld", cmd[0].PartitionId());
//  verify(cmd.type_ > 0);
//  verify(cmd.root_type_ > 0);
  ClassicProxy::RpcJanusDispatchRequest req;
  req.cmd = cmd;
  auto fu_result = proxy->async_JanusDispatch(req, fuattr);
  if (fu_result.is_ok()) {
    Future::safe_release(fu_result.unwrap().raw_future());
  }
}

void TroadCommo::SendHandoutRo(SimpleCommand& cmd,
                               const function<void(int res,
                                                   SimpleCommand& cmd,
                                                   map<int,
                                                       mdb::version_t>& vers)>&) {
  verify(0);
}

void TroadCommo::SendInquire(parid_t pid,
                             epoch_t epoch,
                             txnid_t tid,
                             const function<void(RccGraph& graph)>& callback) {
  FutureAttr fuattr;
  function<void(rusty::Arc<Future>)> cb = [callback](rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    MarshallDeputy md;
    fu->get_reply() >> md;
    verify(0);
//    auto graph = dynamic_cast<RccGraph&>(*md.inner());
//    callback(graph);
  };
  fuattr.callback = cb;
  // TODO fix.
  auto proxy = NearestProxyForPartition(pid).second;
  ClassicProxy::RpcJanusInquireRequest req;
  req.epoch = epoch;
  req.txn_id = tid;
  auto fu_result = proxy->async_JanusInquire(req, fuattr);
  if (fu_result.is_ok()) {
    Future::safe_release(fu_result.unwrap().raw_future());
  }
}


void TroadCommo::BroadcastPreAccept(
    parid_t par_id,
    txnid_t txn_id,
    rank_t rank,
    ballot_t ballot,
    vector<TxPieceData>& cmds,
    shared_ptr<RccGraph> sp_graph,
    const function<void(int, shared_ptr<RccGraph>)>& callback) {
  verify(0); // deprecated
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());

  bool skip_graph = IsGraphOrphan(*sp_graph, txn_id);

  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [callback](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      MarshallDeputy md;
      fu->get_reply() >> res >> md;
      auto sp = marshallable_cast<RccGraph>(md);
      verify(sp);
      callback(res, sp);
    };
    verify(txn_id > 0);
    if (skip_graph) {
      ClassicProxy::RpcJanusPreAcceptWoGraphRequest req;
      req.txn_id = txn_id;
      req.rank = rank;
      req.cmd = cmds;
      auto fu_result = proxy->async_JanusPreAcceptWoGraph(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    } else {
      MarshallDeputy md(sp_graph);
      ClassicProxy::RpcJanusPreAcceptRequest req;
      req.txn_id = txn_id;
      req.rank = rank;
      req.cmd = cmds;
      req.graph = md;
      auto fu_result = proxy->async_JanusPreAccept(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    }
  }
}

shared_ptr<PreAcceptQuorumEvent>
TroadCommo::BroadcastPreAccept(
    parid_t par_id,
    txnid_t txn_id,
    rank_t rank,
    ballot_t ballot,
    vector<TxPieceData>& cmds) {
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  auto n = rpc_par_proxies_[par_id].size();
  auto ev = Reactor::create_sp_event<PreAcceptQuorumEvent>(n, n);
  ev->partition_id_ = par_id;
//  WAN_WAIT;
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [ev](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      auto sp = std::make_shared<parent_set_t>();
      fu->get_reply() >> res >> *sp;
//      auto sp = dynamic_pointer_cast<RccGraph>(md.inner());
//      verify(sp);
      if (res == SUCCESS) {
        ev->vote_yes();
      } else if (res == REJECT) {
        verify(0);
        ev->vote_no();
      } else {
        verify(0);
      }
      ev->vec_parents_.push_back(sp);
    };
    verify(txn_id > 0);
    ClassicProxy::RpcRccPreAcceptRequest req;
    req.txn_id = txn_id;
    req.rank = rank;
    req.cmd = cmds;
    auto fu_result = proxy->async_RccPreAccept(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return ev;
}

shared_ptr<PreAcceptQuorumEvent>
TroadCommo::BroadcastPreAccept(
    parid_t par_id,
    txnid_t txn_id,
    rank_t rank,
    ballot_t ballot,
    vector<TxPieceData>& cmds,
    shared_ptr<RccGraph> sp_graph) {
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  auto n = rpc_par_proxies_[par_id].size();
  auto ev = Reactor::create_sp_event<PreAcceptQuorumEvent>(n, n);
  ev->partition_id_ = par_id;
  bool skip_graph = IsGraphOrphan(*sp_graph, txn_id);
//  WAN_WAIT;
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [ev](rusty::Arc<Future> fu) {
      int32_t res;
      MarshallDeputy md;
      fu->get_reply() >> res >> md;
      auto sp = marshallable_cast<RccGraph>(md);
      verify(sp);
      if (res == SUCCESS) {
        ev->vote_yes();
      } else if (res == REJECT) {
        verify(0);
        ev->vote_no();
      } else {
        verify(0);
      }
      // TODO recover
//      ev->graphs_.push_back(sp);
    };
    verify(txn_id > 0);
    if (skip_graph) {
      ClassicProxy::RpcJanusPreAcceptWoGraphRequest req;
      req.txn_id = txn_id;
      req.rank = rank;
      req.cmd = cmds;
      auto fu_result = proxy->async_JanusPreAcceptWoGraph(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    } else {
      MarshallDeputy md(sp_graph);
      ClassicProxy::RpcJanusPreAcceptRequest req;
      req.txn_id = txn_id;
      req.rank = rank;
      req.cmd = cmds;
      req.graph = md;
      auto fu_result = proxy->async_JanusPreAccept(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    }
  }
  return ev;
}

void TroadCommo::BroadcastAccept(parid_t par_id,
                                 txnid_t cmd_id,
                                 ballot_t ballot,
                                 shared_ptr<RccGraph> graph,
                                 const function<void(int)>& callback) {
  verify(0);
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [callback](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      fu->get_reply() >> res;
      callback(res);
    };
    verify(cmd_id > 0);
    MarshallDeputy md(graph);
    rank_t rank = RANK_D;
    verify(0);
    ClassicProxy::RpcJanusAcceptRequest req;
    req.txn_id = cmd_id;
    req.rank = rank;
    req.ballot = ballot;
    req.graph = md;
    auto fu_result = proxy->async_JanusAccept(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
}

shared_ptr<QuorumEvent> TroadCommo::BroadcastAccept(parid_t par_id,
                                                    txnid_t cmd_id,
                                                    rank_t rank,
                                                    ballot_t ballot,
                                                    parent_set_t& parents) {
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  auto n = rpc_par_proxies_[par_id].size();
  auto ev = Reactor::create_sp_event<QuorumEvent>(n, n/2+1);
//  auto ev = Reactor::create_sp_event<QuorumEvent>(n, n/2+1);
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [ev](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      fu->get_reply() >> res;
      if (res == SUCCESS) {
        ev->vote_yes();
      } else if (res == REJECT) {
        ev->vote_no();
      } else {
        verify(0);
      }
    };
    verify(cmd_id > 0);
    verify(rank == RANK_D || rank == RANK_I);
    ClassicProxy::RpcRccAcceptRequest req;
    req.txn_id = cmd_id;
    req.rank = rank;
    req.ballot = ballot;
    req.p = parents;
    auto fu_result = proxy->async_RccAccept(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return ev;
}

shared_ptr<QuorumEvent> TroadCommo::BroadcastAccept(parid_t par_id,
                                                    txnid_t cmd_id,
                                                    ballot_t ballot,
                                                    shared_ptr<RccGraph> graph) {
  verify(0);
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  auto n = rpc_par_proxies_[par_id].size();
  auto ev = Reactor::create_sp_event<QuorumEvent>(n, n/2+1);
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [ev](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      fu->get_reply() >> res;
      if (res == SUCCESS) {
        ev->vote_yes();
      } else if (res == REJECT) {
        ev->vote_no();
      } else {
        verify(0);
      }
    };
    verify(cmd_id > 0);
    MarshallDeputy md(graph);
    rank_t rank = RANK_D;
    verify(0);
    ClassicProxy::RpcJanusAcceptRequest req;
    req.txn_id = cmd_id;
    req.rank = rank;
    req.ballot = ballot;
    req.graph = md;
    auto fu_result = proxy->async_JanusAccept(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return ev;
}

shared_ptr<QuorumEvent> TroadCommo::CollectValidation(txid_t txid, set<parid_t> pars) {
  auto n = pars.size();
  auto ev = Reactor::create_sp_event<QuorumEvent>(n, n);

  for (auto partition_id : pars) {
    auto proxy = NearestProxyForPartition(partition_id).second;
    FutureAttr fuattr;
    fuattr.callback = [ev] (rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int res;
      fu->get_reply() >> res;
      if (res == SUCCESS) {
        ev->vote_yes();
      } else {
        ev->vote_no();
      }
    };
    int rank = RANK_D;
    ClassicProxy::RpcRccInquireValidationRequest req;
    req.tx_id = txid;
    req.rank = rank;
    auto fu_result = proxy->async_RccInquireValidation(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return ev;
}

shared_ptr<QuorumEvent> TroadCommo::BroadcastValidation(CmdData& cmd_, int result) {
  auto n = cmd_.GetPartitionIds().size();
  auto ev = Reactor::create_sp_event<QuorumEvent>(n, n);

  for (auto partition_id : cmd_.GetPartitionIds()) {
    for (auto& pair : rpc_par_proxies_[partition_id]) {
      auto proxy = pair.second;
      FutureAttr fuattr;
      fuattr.callback = [ev] (rusty::Arc<Future> fu) {
        if (fu->get_error_code() != 0) {
          Log_info("Get a error message in reply");
          return;
        }
//        ev->n_voted_yes_++;
      };
      int rank = RANK_D;
      ClassicProxy::RpcRccNotifyGlobalValidationRequest req;
      req.tx_id = cmd_.id_;
      req.rank = rank;
      req.res = result;
      auto fu_result = proxy->async_RccNotifyGlobalValidation(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    }
  }
  return ev;
};

void TroadCommo::BroadcastCommit(
    parid_t par_id,
    txnid_t cmd_id,
    rank_t rank,
    bool need_validation,
    shared_ptr<RccGraph> graph,
    const function<void(int32_t, TxnOutput&)>& callback) {
  bool skip_graph = IsGraphOrphan(*graph, cmd_id);

  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [callback](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
          Log_info("Get a error message in reply");
          return;
        }
      int32_t res;
      TxnOutput output;
      fu->get_reply() >> res >> output;
      callback(res, output);
    };
    verify(cmd_id > 0);
    if (skip_graph) {
      ClassicProxy::RpcJanusCommitWoGraphRequest req;
      req.id = cmd_id;
      req.rank = rank;
      req.need_validation = need_validation;
      auto fu_result = proxy->async_JanusCommitWoGraph(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    } else {
      MarshallDeputy md(graph);
      ClassicProxy::RpcJanusCommitRequest req;
      req.id = cmd_id;
      req.rank = rank;
      req.need_validation = need_validation;
      req.graph = md;
      auto fu_result = proxy->async_JanusCommit(req, fuattr);
      if (fu_result.is_ok()) {
        Future::safe_release(fu_result.unwrap().raw_future());
      }
    }
  }
}

shared_ptr<QuorumEvent>
TroadCommo::BroadcastCommit(parid_t par_id,
                            txnid_t cmd_id,
                            rank_t rank,
                            bool need_validation,
                            parent_set_t& parents) {
//  bool skip_graph = IsGraphOrphan(*graph, cmd_id);
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());
  int n = rpc_par_proxies_[par_id].size();
  auto ev = Reactor::create_sp_event<QuorumEvent>(n, 1);
  for (auto& p : rpc_par_proxies_[par_id]) {
    auto proxy = (p.second);
    verify(proxy != nullptr);
    FutureAttr fuattr;
    fuattr.callback = [ev](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      TxnOutput output;
      fu->get_reply() >> res >> output;
      if (res == SUCCESS) {
        ev->vote_yes();
      } else {
        ev->vote_no();
      }
    };
    verify(cmd_id > 0);
//    if (skip_graph) {
//      Future::safe_release(proxy->async_JanusCommitWoGraph(cmd_id, rank, need_validation, fuattr));
//    } else {
//      MarshallDeputy md(graph);
//      Future::safe_release(proxy->async_JanusCommit(cmd_id, rank, need_validation, md, fuattr));
//    }
    ClassicProxy::RpcRccCommitRequest req;
    req.id = cmd_id;
    req.rank = rank;
    req.need_validation = need_validation;
    req.parents = parents;
    auto fu_result = proxy->async_RccCommit(req, fuattr);
    if (fu_result.is_ok()) {
      Future::safe_release(fu_result.unwrap().raw_future());
    }
  }
  return ev;
}

} // namespace janus
