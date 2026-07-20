#include "commo.h"

namespace janus
{

std::vector<SiteProxyPair>
CommunicatorNoneCopilot::PilotProxyForPartition(parid_t par_id) const {
  /**
   * ad-hoc. No leader election. fixed pilot(id=0) and copilot(id=1)
   */
  auto it  = rpc_par_proxies_.find(par_id);
  verify(it != rpc_par_proxies_.end());
  auto& partition_proxies = it->second;
  auto config = Config::GetConfig();
  auto pilot_it =
      std::find_if(partition_proxies.begin(), partition_proxies.end(),
                   [config](const std::pair<siteid_t, ClassicProxy*>& p) {
                     verify(p.second != nullptr);
                     auto& site = config->SiteById(p.first);
                     return site.locale_id == 0;
                   });
  if (pilot_it == partition_proxies.end())
    Log_fatal("couldn't find pilot for partition %d", par_id);
  verify(pilot_it->second);

  auto copilot_it =
      std::find_if(partition_proxies.begin(), partition_proxies.end(),
                   [config](const std::pair<siteid_t, ClassicProxy*>& p) {
                     verify(p.second != nullptr);
                     auto& site = config->SiteById(p.first);
                     return site.locale_id == 1;
                   });
  if (copilot_it == partition_proxies.end())
    Log_fatal("couldn't find copilot for partition %d", par_id);
  verify(copilot_it->second);

  return { *pilot_it, *copilot_it };  
}

void CommunicatorNoneCopilot::BroadcastDispatch(shared_ptr<vector<shared_ptr<SimpleCommand>>> sp_vec_piece,
                                                Coordinator *coo,
                                                const std::function<void(int res, TxnOutput &)> &callback) {
  WAN_WAIT
  cmdid_t cmd_id = sp_vec_piece->at(0)->root_id_;
  verify(!sp_vec_piece->empty());
  auto par_id = sp_vec_piece->at(0)->PartitionId();
  rrr::FutureAttr fuattr;
  fuattr.callback = [coo, this, callback, par_id](rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    int32_t ret;
    TxnOutput outputs;
    uint64_t coro_id = 0;
    janus::Command view_md;
    rrr::deserialize_from(fu->get_reply(), ret, outputs, coro_id, view_md);
    n_pending_rpc_[0]--;
    verify(n_pending_rpc_[0] >= 0);
    dispatch_quota.set(dispatch_quota.value_ + 1);
    
    // Handle WRONG_LEADER response with view data
    if (ret == WRONG_LEADER && view_md.has_value()) {
      const auto sp_view_data = marshallable_cast<ViewData>(view_md);
      if (sp_view_data.is_some()) {
        UpdatePartitionView(par_id, *sp_view_data.unwrap());
      }
    }

    callback(ret, outputs);
  };
  // auto pair_leader_proxy = LeaderProxyForPartition(par_id);
  // Log_debug("send dispatch to site %ld",
  //           pair_leader_proxy.first);
  // auto proxy = pair_leader_proxy.second;
  auto pair_proxies = PilotProxyForPartition(par_id);
  verify(pair_proxies.size() == 2);
  Log_debug("send dispatch to site %d, %d", pair_proxies[0].first,
            pair_proxies[1].first);
  VecPieceData vpd;
  vpd.sp_vec_piece_data_ = sp_vec_piece;
  janus::Command md(rusty::Arc<VecPieceData>::make(std::move(vpd)));

  struct DepId di;
  di.id = cmd_id;
  di.str = __func__;

  dispatch_quota.wait_until_gte(0, /*timeout=*/0);

  bool send = false;

  if (n_pending_rpc_[0] < max_pending_rpc_) {
    // if (true) {
    ClassicProxy::RpcDispatchRequest req0;
    req0.tid = cmd_id;
    req0.dep_id = di;
    req0.cmd = md;
    auto future = pair_proxies[0].second->async_Dispatch(req0, fuattr);
    if (future.is_ok()) {
      Future::safe_release(future.unwrap().raw_future());
    }
    n_pending_rpc_[0]++;
    dispatch_quota.set(dispatch_quota.value_ - 1);
    send = true;
  }

  rrr::FutureAttr fu2;
  fu2.callback = [coo, this, callback, par_id](rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    int32_t ret;
    TxnOutput outputs;
    uint64_t coro_id = 0;
    janus::Command view_md;
    rrr::deserialize_from(fu->get_reply(), ret, outputs, coro_id, view_md);
    n_pending_rpc_[1]--;
    verify(n_pending_rpc_[1] >= 0);
    dispatch_quota.set(dispatch_quota.value_ + 1);
    
    // Handle WRONG_LEADER response with view data
    if (ret == WRONG_LEADER && view_md.has_value()) {
      const auto sp_view_data = marshallable_cast<ViewData>(view_md);
      if (sp_view_data.is_some()) {
        UpdatePartitionView(par_id, *sp_view_data.unwrap());
      }
    }

    callback(ret, outputs);
  };

  if (n_pending_rpc_[1] < max_pending_rpc_) {
    ClassicProxy::RpcDispatchRequest req1;
    req1.tid = cmd_id;
    req1.dep_id = di;
    req1.cmd = md;
    auto future = pair_proxies[1].second->async_Dispatch(req1, fu2);
    if (future.is_ok()) {
      Future::safe_release(future.unwrap().raw_future());
    }
    n_pending_rpc_[1]++;
    dispatch_quota.set(dispatch_quota.value_ - 1);
    send = true;
  }

  verify(send);
}
    
} // namespace janus
