#include "../procedure.h"
#include "../rcc/tx.h"
#include "../rcc/graph_marshaler.h"
#include "commo.h"
#include "__dep__.h"

namespace janus {

void JanusCommo::SendDispatch(vector<TxPieceData>& cmd,
                              const function<void(int res,
                                                  TxnOutput& cmd,
                                                  const RccGraph& graph)>& callback) {
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
        rrr::AnyMessage am;
        rrr::deserialize_from(fu->get_reply(), res, output, am);
        // graph reply rides directly as
        // an `AnyMessage`, no `janus::Command` wrapper.
        if (am.is_a<EmptyGraph>()) {
          RccGraph rgraph;
          auto v = rgraph.CreateV(tid);
          RccTx& info = *v;
//          info.partition_.insert(par_id);
          verify(rgraph.vertex_index().size() > 0);
          callback(res, output, rgraph);
        } else if (auto sp_graph = am.unpack<RccGraph>()) {
          callback(res, output, *sp_graph.as_ref().unwrap());
        } else {
          verify(0);
        }
      };
  fuattr.callback = cb;
  auto proxy = NearestProxyForPartition(cmd[0].PartitionId()).second;
  Log_debug("dispatch to {}", cmd[0].PartitionId());
//  verify(cmd.type_ > 0);
//  verify(cmd.root_type_ > 0);
  ClassicProxy::RpcJanusDispatchRequest req;
  req.cmd = cmd;
  auto fu_result = proxy->async_JanusDispatch(req, fuattr);
  // Arc auto-released
}

void JanusCommo::SendHandoutRo(SimpleCommand& cmd,
                               const function<void(int res,
                                                   SimpleCommand& cmd,
                                                   map<int,
                                                       mdb::version_t>& vers)>&) {
  verify(0);
}

void JanusCommo::SendInquire(parid_t pid,
                             epoch_t epoch,
                             txnid_t tid,
                             const function<void(const RccGraph& graph)>& callback) {
  FutureAttr fuattr;
  function<void(rusty::Arc<Future>)> cb = [callback](rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    rrr::AnyMessage am;
    rrr::deserialize_from(fu->get_reply(), am);
    const auto sp_graph = am.unpack<RccGraph>();
    verify(sp_graph.is_some());
    callback(*sp_graph.unwrap());
  };
  fuattr.callback = cb;
  // TODO fix.
  auto proxy = NearestProxyForPartition(pid).second;
  ClassicProxy::RpcJanusInquireRequest req;
  req.epoch = epoch;
  req.txn_id = tid;
  auto fu_result = proxy->async_JanusInquire(req, fuattr);
  // Arc auto-released
}


void JanusCommo::BroadcastPreAccept(
    parid_t par_id,
    txnid_t txn_id,
    ballot_t ballot,
    vector<TxPieceData>& cmds,
    shared_ptr<RccGraph> sp_graph,
    const function<void(int, shared_ptr<RccGraph>)>& callback) {
  verify(rpc_par_proxies_.find(par_id) != rpc_par_proxies_.end());

  bool skip_graph = IsGraphOrphan(*sp_graph, txn_id);
  verify(0);
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
      rrr::AnyMessage am;
      rrr::deserialize_from(fu->get_reply(), res, am);
      const auto sp_graph = am.unpack<RccGraph>();
      verify(sp_graph.is_some());
      callback(res, std::make_shared<RccGraph>(*sp_graph.unwrap()));
    };
    verify(txn_id > 0);
    if (skip_graph) {
      ClassicProxy::RpcJanusPreAcceptWoGraphRequest req;
      req.txn_id = txn_id;
      req.rank = RANK_UNDEFINED;
      req.cmd = cmds;
      auto fu_result = proxy->async_JanusPreAcceptWoGraph(req, fuattr);
      // Arc auto-released
    } else {
      // graph field is now `AnyMessage`
      // directly (not wrapped in `janus::Command`).
      auto sp_graph_copy = rusty::Arc<RccGraph>::make(*sp_graph);
      ClassicProxy::RpcJanusPreAcceptRequest req;
      req.txn_id = txn_id;
      req.rank = RANK_UNDEFINED;
      req.cmd = cmds;
      req.graph = rrr::AnyMessage::pack(std::move(sp_graph_copy));
      auto fu_result = proxy->async_JanusPreAccept(req, fuattr);
      // Arc auto-released
    }
  }
}

void JanusCommo::BroadcastAccept(parid_t par_id,
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
      rrr::deserialize_from(fu->get_reply(), res);
      callback(res);
    };
    verify(cmd_id > 0);
    // graph field is `AnyMessage` directly.
    auto sp_graph = rusty::Arc<RccGraph>::make(*graph);
    rank_t rank = RANK_D;
    ClassicProxy::RpcJanusAcceptRequest req;
    req.txn_id = cmd_id;
    req.rank = rank;
    req.ballot = ballot;
    req.graph = rrr::AnyMessage::pack(std::move(sp_graph));
    auto fu_result = proxy->async_JanusAccept(req, fuattr);
    // Arc auto-released
  }
}

void JanusCommo::BroadcastCommit(
    parid_t par_id,
    txnid_t cmd_id,
    rank_t rank,
    bool need_validation,
    shared_ptr<RccGraph> graph,
    const function<void(int32_t, TxnOutput&)>& callback) {
  bool skip_graph = IsGraphOrphan(*graph, cmd_id);
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
      TxnOutput output;
      rrr::deserialize_from(fu->get_reply(), res, output);
      callback(res, output);
    };
    verify(cmd_id > 0);
    if (skip_graph) {
      ClassicProxy::RpcJanusCommitWoGraphRequest req;
      req.id = cmd_id;
      req.rank = 0;
      req.need_validation = need_validation;
      auto fu_result = proxy->async_JanusCommitWoGraph(req, fuattr);
      // Arc auto-released
    } else {
      // graph field is `AnyMessage` directly.
      auto sp_graph = rusty::Arc<RccGraph>::make(*graph);
      ClassicProxy::RpcJanusCommitRequest req;
      req.id = cmd_id;
      req.rank = 0;
      req.need_validation = need_validation;
      req.graph = rrr::AnyMessage::pack(std::move(sp_graph));
      auto fu_result = proxy->async_JanusCommit(req, fuattr);
      // Arc auto-released
    }
  }
}

rusty::Arc<QuorumEvent> JanusCommo::BroadcastInquireValidation(set<parid_t>& pars, txid_t txid) {
  auto e = reactor_create_sp_event<QuorumEvent>(pars.size(), pars.size());
  for (auto par_id : pars) {
    auto proxy = NearestProxyForPartition(par_id).second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      rrr::deserialize_from(fu->get_reply(), res);
      if (res == 1) {
        e->vote_yes();
      } else if (res == -1) {
        e->vote_no();
      } else {
        verify(0);
      }
    };
    verify(0);
    int rank = RANK_D;
    ClassicProxy::RpcRccInquireValidationRequest req;
    req.tx_id = txid;
    req.rank = rank;
    auto fu_result = proxy->async_RccInquireValidation(req, fuattr);
    // Arc auto-released
  }
  return e;
}
void JanusCommo::BroadcastNotifyValidation(txid_t txid, set<parid_t>& pars, int32_t result) {
  for (auto par_id : pars) {
    for (auto pair : rpc_par_proxies_[par_id]) {
      auto proxy = pair.second;
      FutureAttr fuattr;
      fuattr.callback = [](rusty::Arc<Future> fu) {};
      int rank = RANK_D;
      verify(0);
      ClassicProxy::RpcRccNotifyGlobalValidationRequest req;
      req.tx_id = txid;
      req.rank = rank;
      req.res = result;
      auto fu_result = proxy->async_RccNotifyGlobalValidation(req, fuattr);
      // Arc auto-released
    }
  }

}

} // namespace janus
