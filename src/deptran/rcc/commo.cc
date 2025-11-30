
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
        int res;
        TxnOutput output;
        MarshallDeputy md;
        fu->get_reply() >> res >> output >> md;
        if (md.kind_ == MarshallDeputy::EMPTY_GRAPH) {
          RccGraph rgraph;
          auto v = rgraph.CreateV(tid);
          RccTx& info = *v;
          info.partition_.insert(par_id);
          verify(rgraph.vertex_index().size() > 0);
          callback(res, output, rgraph);
        } else if (md.kind_ == MarshallDeputy::RCC_GRAPH) {
          RccGraph& graph = dynamic_cast<RccGraph&>(*md.sp_data_);
          callback(res, output, graph);
        } else {
          verify(0);
        }
      };
  fuattr.callback = cb;
  auto proxy = NearestProxyForPartition(cmd[0].PartitionId()).second;
  Log_debug("dispatch to %ld", cmd[0].PartitionId());
//  verify(cmd.type_ > 0);
//  verify(cmd.root_type_ > 0);
  auto fu_result = proxy->async_JanusDispatch(cmd, fuattr);
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
    map<innid_t, map<int32_t, Value>> outputs;
    fu->get_reply() >> outputs;
    callback(outputs);
  };
  fuattr.callback = cb;
  auto proxy = NearestProxyForPartition(pid).second;
  // Use shared_ptr directly for MarshallDeputy
  auto sp_graph = std::make_shared<RccGraph>(*graph);
  MarshallDeputy md(sp_graph);
  auto fu_result = proxy->async_RccFinish(tid, md, fuattr);
  // Arc auto-released
}

void RccCommo::SendInquire(parid_t pid,
                           epoch_t epoch,
                           txnid_t tid,
                           const function<void(RccGraph& graph)>& callback) {
  FutureAttr fuattr;
  function<void(rusty::Arc<Future>)> cb = [callback] (rusty::Arc<Future> fu) {
    MarshallDeputy md;
    fu->get_reply() >> md;
    RccGraph& graph = dynamic_cast<RccGraph&>(*md.sp_data_);
    callback(graph);
  };
  fuattr.callback = cb;
  auto proxy = (ClassicProxy*)NearestProxyForPartition(pid).second;
  auto fu_result = proxy->async_RccInquire(epoch, tid, fuattr);
  // Arc auto-released
}

void RccCommo::BroadcastCommit(parid_t par_id,
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
                        int32_t res;
                        TxnOutput output;
                        fu->get_reply() >> res >> output;
                        callback(res, output);
                      };
    verify(cmd_id > 0);
    if (skip_graph) {
      auto fu_result = proxy->async_JanusCommitWoGraph(cmd_id, RANK_UNDEFINED, need_validation, fuattr);
      // Arc auto-released
    } else {
      // Use shared_ptr directly for MarshallDeputy
      auto sp_graph = std::make_shared<RccGraph>(*graph);
      MarshallDeputy md(sp_graph);
      auto fu_result = proxy->async_JanusCommit(cmd_id, RANK_UNDEFINED, need_validation, md, fuattr);
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

} // namespace janus
