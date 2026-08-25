
#include "command.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "commo.h"
#include "../coordinator.h"
#include "../rcc_rpc.h"
#include "deptran/service.h"

namespace janus {

//void TapirCommo::SendDispatch(vector<SimpleCommand> &cmd,
//                              Coordinator *coo,
//                              const function<void(int,
//                                                  TxnOutput &)>
//                              &callback) {
//  srpc::FutureAttr fuattr;
//  parid_t par_id = cmd[0].PartitionId();
//  auto proxy = (ClassicProxy*)
//      NearestProxyForPartition(cmd[0].PartitionId()).second;
//  function<void(Future*)> cb =
//      [coo, this, callback] (Future *fu) {
//        int32_t res;
//        TxnOutput output;
//        srpc::deserialize_from(fu->get_reply(), res);
//        srpc::deserialize_from(fu->get_reply(), output);
//        callback(res, output);
//      };
//  fuattr.callback = cb;
//  Log_debug("SendStart to {} from {}", cmd[0].PartitionId(), coo->coo_id_);
////  verify(cmd.type_ > 0);
////  verify(cmd.root_type_ > 0);
//  Future::safe_release(proxy->async_Dispatch(cmd, fuattr));
//}

void TapirCommo::BroadcastFastAccept(parid_t par_id,
                                     cmdid_t cmd_id,
                                     vector<SimpleCommand>& cmds,
                                     const function<void(int32_t)>& cb) {
  auto proxies = rpc_par_proxies_[par_id];
  for (auto &p : proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = srpc::FutureCallback::from_callable([cb] (rusty::Arc<Future> fu) {
      int32_t res;
      srpc::deserialize_from(fu->get_reply(), res);
      cb(res);
    });
    ClassicProxy::RpcTapirFastAcceptRequest req;
    req.cmd_id = cmd_id;
    req.txn_cmds = cmds;
    auto fu_result = proxy->async_TapirFastAccept(req, fuattr);
    // Arc auto-released
  }
}

void TapirCommo::BroadcastDecide(parid_t par_id,
                                 cmdid_t cmd_id,
                                 int32_t decision) {
  auto proxies = rpc_par_proxies_[par_id];
  for (auto &p : proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = srpc::FutureCallback::from_callable([] (rusty::Arc<Future> fu) {});
    ClassicProxy::RpcTapirDecideRequest req;
    req.cmd_id = cmd_id;
    req.commit = decision;
    auto fu_result = proxy->async_TapirDecide(req, fuattr);
    // Arc auto-released
  }
}

void TapirCommo::BroadcastAccept(parid_t par_id,
                                 cmdid_t cmd_id,
                                 ballot_t ballot,
                                 int decision,
                                 const function<void(rusty::Arc<Future>)>& callback) {
  auto proxies = rpc_par_proxies_[par_id];
  for (auto &p: proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = srpc::FutureCallback::from_callable(callback);
    ClassicProxy::RpcTapirAcceptRequest req;
    req.cmd_id = cmd_id;
    req.ballot = ballot;
    req.decision = decision;
    auto fu_result = proxy->async_TapirAccept(req, fuattr);
    // Arc auto-released
  }
}

} // namespace janus
