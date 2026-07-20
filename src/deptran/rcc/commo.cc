
#include "commo.h"
#include "dep_graph.h"
#include "../procedure.h"
#include "graph_marshaler.h"
#include "../service.h"

namespace janus {

void RccCommo::SendDispatch(vector<SimpleCommand> &cmd,
                            const function<void(int res,
                                                TxnOutput&,
                                                RccGraph&)>& callback) {
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
        // graph field rides directly as AnyMessage.
        if (am.is_a<EmptyGraph>()) {
          RccGraph rgraph;
          auto v = rgraph.CreateV(tid);
          RccTx& info = *v;
//          info.partition_.insert(par_id);
          verify(rgraph.vertex_index().size() > 0);
          callback(res, output, rgraph);
        } else if (auto sp_graph = am.unpack<RccGraph>()) {
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
  // Arc auto-released
}

void RccCommo::SendHandoutRo(SimpleCommand &cmd,
                             const function<void(int res,
                                                 SimpleCommand& cmd,
                                                 map<int, mdb::version_t>& vers)>&) {
  verify(0);
}

void RccCommo::SendFinish(parid_t pid,
                          txnid_t tid,
                          shared_ptr<RccGraph> graph,
                          const function<void(TxnOutput& output)> &callback) {
  FutureAttr fuattr;
  function<void(rusty::Arc<Future>)> cb = [callback] (rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    map<innid_t, map<int32_t, Value>> outputs;
    rrr::deserialize_from(fu->get_reply(), outputs);
    callback(outputs);
  };
  fuattr.callback = cb;
  auto proxy = NearestProxyForPartition(pid).second;
  // graph field is `AnyMessage` directly.
  auto sp_graph = std::make_shared<RccGraph>(*graph);
  ClassicProxy::RpcRccFinishRequest req;
  req.id = tid;
  req.md_graph = rrr::AnyMessage::pack(sp_graph);
  auto fu_result = proxy->async_RccFinish(req, fuattr);
  // Arc auto-released
}


shared_ptr<map<txid_t, parent_set_t>>
RccCommo::Inquire(parid_t pid, txnid_t tid, rank_t rank) {
  auto ret = std::make_shared<map<txid_t, parent_set_t>>();
  auto ev = Reactor::create_sp_event<IntEvent>();
  FutureAttr fuattr;
  function<void(rusty::Arc<Future>)> cb = [ret, &ev] (rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
//    janus::Command md;
    rrr::deserialize_from(fu->get_reply(), *ret);
    ev->set(1);
  };
  fuattr.callback = cb;
  auto proxy = (ClassicProxy*)NearestProxyForPartition(pid).second;
  ClassicProxy::RpcRccInquireRequest req;
  req.txn_id = tid;
  req.rank = rank;
  auto fu_result = proxy->async_RccInquire(req, fuattr);
  // Arc auto-released
//  ev->wait_timeout(60*1000*1000);
//  verify(ev->status_ != EventStatus::TIMEOUT);
  ev->wait();
  return ret;
}

void RccCommo::SendInquire(parid_t pid,
                           epoch_t epoch,
                           txnid_t tid,
                           const function<void(RccGraph& graph)>& callback) {
  FutureAttr fuattr;
  function<void(rusty::Arc<Future>)> cb = [callback] (rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    rrr::AnyMessage am;
    rrr::deserialize_from(fu->get_reply(), am);
    auto sp_graph = am.unpack<RccGraph>();
    verify(sp_graph);
    callback(*sp_graph);
  };
  fuattr.callback = cb;
  auto proxy = (ClassicProxy*)NearestProxyForPartition(pid).second;
  ClassicProxy::RpcJanusInquireRequest req;
  req.epoch = epoch;
  req.txn_id = tid;
  auto fu_result = proxy->async_JanusInquire(req, fuattr);
  // Arc auto-released
}

void RccCommo::BroadcastCommit(parid_t par_id,
                               txnid_t cmd_id,
                               rank_t rank,
                               bool need_validation,
                               shared_ptr<RccGraph> graph,
                               const function<void(int32_t, TxnOutput&)>& callback) {
  verify(0);
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
                        rrr::deserialize_from(fu->get_reply(), res, output);
                        callback(res, output);
                      };
    verify(cmd_id > 0);
    if (skip_graph) {
      ClassicProxy::RpcJanusCommitWoGraphRequest req;
      req.id = cmd_id;
      req.rank = RANK_UNDEFINED;
      req.need_validation = need_validation;
      auto fu_result = proxy->async_JanusCommitWoGraph(req, fuattr);
      // Arc auto-released
    } else {
      // graph field is `AnyMessage` directly.
      auto sp_graph = std::make_shared<RccGraph>(*graph);
      ClassicProxy::RpcJanusCommitRequest req;
      req.id = cmd_id;
      req.rank = RANK_UNDEFINED;
      req.need_validation = need_validation;
      req.graph = rrr::AnyMessage::pack(sp_graph);
      auto fu_result = proxy->async_JanusCommit(req, fuattr);
      // Arc auto-released
    }
  }
}

bool RccCommo::IsGraphOrphan(RccGraph& graph, txnid_t cmd_id) {
  if (graph.size() == 1) {
    auto v = graph.FindV(cmd_id);
    verify(v);
    return true;
  } else {
    return false;
  }
}

void RccCommo::BroadcastValidation(txid_t id, set<parid_t> pars, int result) {
  for (auto partition_id : pars) {
    for (auto& pair : rpc_par_proxies_[partition_id]) {
      auto proxy = pair.second;
      FutureAttr fuattr;
      fuattr.callback = [] (rusty::Arc<Future> fu) {
      };
      int rank = RANK_D;
      verify(0);
      ClassicProxy::RpcRccNotifyGlobalValidationRequest req;
      req.tx_id = id;
      req.rank = rank;
      req.res = result;
      auto fu_result = proxy->async_RccNotifyGlobalValidation(req, fuattr);
      // Arc auto-released
    }
  }
}

} // namespace janus
