#include "commo.h"

namespace janus
{

vector<std::pair<siteid_t, ClassicProxy*>>
CommunicatorRule::LeaderProxyForPartition(parid_t par_id, int idx) const {
  if (idx > -1) { // Mencius
    auto it = rpc_par_proxies_.find(par_id);
    auto& partition_proxies = it->second;
    verify(partition_proxies.size()>idx);
    vector<std::pair<siteid_t, ClassicProxy*>> ret;
    ret.push_back(it->second.at(idx));
    return ret;
  }

  // First check if we have updated view information
  locid_t view_leader = GetLeaderForPartition(par_id);
  if (view_leader > 0) {
    // We have a leader from the view, find the proxy for it
    auto it = rpc_par_proxies_.find(par_id);
    if (it != rpc_par_proxies_.end()) {
      auto& partition_proxies = it->second;
      auto config = Config::GetConfig();
      
      // Find the proxy for this leader locale_id
      auto proxy_it = std::find_if(
          partition_proxies.begin(),
          partition_proxies.end(),
          [config, view_leader](const std::pair<siteid_t, ClassicProxy*>& p) {
            verify(p.second != nullptr);
            auto& site = config->SiteById(p.first);
            return site.locale_id == view_leader;
          });
      
      if (proxy_it != partition_proxies.end()) {
        vector<std::pair<siteid_t, ClassicProxy*>> ret;
        ret.push_back(*proxy_it);
        return ret;
      }
    }
  }

  // Fall back to static leader approach
  auto leader_cache =
      const_cast<map<parid_t, vector<SiteProxyPair>>&>(this->jetpack_leader_cache_);

  vector<int> leader_ids = LeadersForPartition(par_id);

  auto leader_it = leader_cache.find(par_id);
  if (leader_it != leader_cache.end()) {
    return leader_it->second;
  } else {
    auto it = rpc_par_proxies_.find(par_id);
    verify(it != rpc_par_proxies_.end());
    auto& partition_proxies = it->second;
    auto config = Config::GetConfig();
    vector<std::pair<siteid_t, ClassicProxy*>> cache;
    for (auto leader_id: leader_ids) {
      auto proxy_it = std::find_if(
          partition_proxies.begin(),
          partition_proxies.end(),
          [config, leader_id](const std::pair<siteid_t, ClassicProxy*>& p) {
            verify(p.second != nullptr);
            auto& site = config->SiteById(p.first);
            return site.locale_id == leader_id;
          });
      if (proxy_it == partition_proxies.end()) {
        Log_fatal("could not find leader for partition %d", par_id);
      } else {
        cache.push_back(*proxy_it);
        Log_debug("leader site for parition %d is %d", par_id, proxy_it->first);
      }
      verify(proxy_it->second != nullptr);
    }
    leader_cache[par_id] = cache;
    return cache;
  }
}


// SiteProxyPair CommunicatorRule::FindSiteProxyPair(parid_t par_id, int replica_id) const {
//   auto it  = rpc_par_proxies_.find(par_id);
//   verify(it != rpc_par_proxies_.end());
//   auto& partition_proxies = it->second;
//   auto config = Config::GetConfig();
//   auto proxy_pair =
//       std::find_if(partition_proxies.begin(), partition_proxies.end(),
//                    [config, replica_id](const std::pair<siteid_t, ClassicProxy*>& p) {
//                      verify(p.second != nullptr);
//                      auto& site = config->SiteById(p.first);
//                      return site.locale_id == replica_id;
//                    });
//   if (proxy_pair == partition_proxies.end())
//     Log_fatal("couldn't find replica %d for partition %d", replica_id, par_id);
//   verify(proxy_pair->second);
//   return *proxy_pair;
// }

std::vector<int> CommunicatorRule::LeadersForPartition(parid_t par_id) const {
  std::vector<int> leaders;
  auto config = Config::GetConfig();
  switch (config->replica_proto_) {
    case MODE_RAFT:
    case MODE_FPGA_RAFT:
    case MODE_MONGODB:
      leaders.push_back(0);
      break;
    case MODE_COPILOT:
      leaders.push_back(0);
      leaders.push_back(1);
      break;
    case MODE_MENCIUS:
      for (int replica_id = 0; replica_id < config->GetPartitionSize(par_id); replica_id++)
        leaders.push_back(replica_id);
      break;
    default:
      Log_fatal("Rule mode do not support for this replica protocol now");
      break;
  }
  return leaders;
}

// std::vector<SiteProxyPair>
// CommunicatorRule::LeaderProxyForPartition(parid_t par_id) const {
//   /**
//    * ad-hoc. No leader election. fixed leader(id=0) for raft; fixed pilot(id=0) and copilot(id=1) for copilot.
//    */
//   std::vector<SiteProxyPair> proxy_pairs;
//   for (auto leader_id: LeadersForPartition(par_id))
//     proxy_pairs.push_back(FindSiteProxyPair(par_id, leader_id));
//   return proxy_pairs;  
// }

shared_ptr<RuleSpeculativeExecuteQuorumEvent>
CommunicatorRule::BroadcastRuleSpeculativeExecute(shared_ptr<vector<shared_ptr<SimpleCommand>>> vec_piece_data) {
  verify(!vec_piece_data->empty());
  auto par_id = vec_piece_data->at(0)->PartitionId();
  
  shared_ptr<VecPieceData> sp_vpd(new VecPieceData);
  sp_vpd->sp_vec_piece_data_ = vec_piece_data;
  MarshallDeputy md(sp_vpd);

  int n = Config::GetConfig()->GetPartitionSize(par_id);
  int n_leaders = Config::GetConfig()->get_num_leaders(par_id);
  auto e = Reactor::CreateSpEvent<RuleSpeculativeExecuteQuorumEvent>(n, SimpleRWCommand::RuleSuperMajority(n), n_leaders);
  WAN_WAIT;
  for (auto& pair : rpc_par_proxies_[par_id]) {
    rrr::FutureAttr fuattr;
    fuattr.callback =
        [e, this](rusty::Arc<Future> fu) {
          if (fu->get_error_code() != 0) {
            Log_info("Get a error message in reply");
            return;
          }
          bool_t accepted;
          value_t result;
          bool_t is_leader;
          fu->get_reply() >> accepted >> result >> is_leader;
          e->FeedResponse(accepted, result, is_leader);
        };
    
    DepId di;
    di.str = "dep";
    di.id = Communicator::global_id++;
    
    auto proxy = pair.second;

    // Record Time
    struct timeval tp;
    gettimeofday(&tp, NULL);
    sp_vpd->time_sent_from_client_ = tp.tv_sec * 1000 + tp.tv_usec / 1000.0;
    
    auto future = proxy->async_RuleSpeculativeExecute(md, fuattr);
    Future::safe_release(future);
  }

  e->Wait();

  return e;
}


void CommunicatorRule::BroadcastDispatch(
    bool fastpath_broadcast_mode,
    shared_ptr<vector<shared_ptr<TxPieceData>>> sp_vec_piece,
    Coordinator* coo,
    const function<void(int, TxnOutput&)> & callback) {

  Log_debug("Do a dispatch on client worker");
  cmdid_t cmd_id = sp_vec_piece->at(0)->root_id_;
  verify(!sp_vec_piece->empty());
  auto par_id = sp_vec_piece->at(0)->PartitionId();

  rrr::FutureAttr fuattr;
  fuattr.callback =
      [coo, this, callback, par_id](rusty::Arc<Future> fu) {
        if (fu->get_error_code() != 0) {
          Log_info("Get a error message in reply");
          return;
        }
        int32_t ret;
        TxnOutput outputs;
        uint64_t coro_id = 0;
        MarshallDeputy view_md;
        fu->get_reply() >> ret >> outputs >> coro_id >> view_md;
        
        // Handle WRONG_LEADER response with view data
        if (ret == WRONG_LEADER && view_md.sp_data_ != nullptr) {
          auto sp_view_data = dynamic_pointer_cast<ViewData>(view_md.sp_data_);
          if (sp_view_data) {
            UpdatePartitionView(par_id, sp_view_data);
          }
        }
        
        callback(ret, outputs);
      };
  
  shared_ptr<VecPieceData> sp_vpd(new VecPieceData);
  sp_vpd->sp_vec_piece_data_ = sp_vec_piece;

  // Record Time
  sp_vpd->time_sent_from_client_ = SimpleRWCommand::GetCurrentMsTime();

  MarshallDeputy md(sp_vpd); // ????

	DepId di;
	di.str = "dep";
	di.id = Communicator::global_id++;
  

  WAN_WAIT;

  vector<std::pair<siteid_t, ClassicProxy*>> pair_leader_proxies;

  if (fastpath_broadcast_mode) {
    pair_leader_proxies = LeaderProxyForPartition(par_id);
  } else {
    std::pair<siteid_t, ClassicProxy*> pair_leader_proxy;
    if (Config::GetConfig()->replica_proto_==MODE_MENCIUS) {
      // The logic here is: Mencius have multiple proposor, if the client is co-locate with a proposer, it give all commands to this proposor.
      // If not, round-robin with all proposors.
      auto server_infos = Config::GetConfig()->GetMyServers();
      if (server_infos.size() == 1) {
        int n = rpc_par_proxies_.find(par_id)->second.size();
        pair_leader_proxy = Communicator::LeaderProxyForPartition(par_id, server_infos[0].id);
      } else {
        int n = rpc_par_proxies_.find(par_id)->second.size();
        pair_leader_proxy = Communicator::LeaderProxyForPartition(par_id, rand() % n);
      }
    } else {
      pair_leader_proxy = Communicator::LeaderProxyForPartition(par_id);
    }
    pair_leader_proxies.push_back(pair_leader_proxy);
  }


  

  for (auto pair_leader_proxy: pair_leader_proxies) {
    auto proxy = pair_leader_proxy.second;
    auto future = proxy->async_Dispatch(cmd_id, di, md, fuattr);
    Future::safe_release(future);
  }
  
}


} // namespace janus
