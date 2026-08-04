
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "communicator.h"
#include "coordinator.h"
#include "classic/coordinator.h"
#include "rcc/graph.h"
#include "rcc/graph_marshaler.h"
#include "command.h"
#include "command_marshaler.h"
#include "classic/tpc_command.h"
#include "procedure.h"
#include "rcc_rpc.h"
#include <rusty/vec.hpp>
#include "RW_command.h"

import std;
import rusty;  // names rusty::Vec directly (clients_.insert below); <rusty/vec.hpp> is an empty shim

namespace janus {

// Jetpack feature: Global partition view tracking
std::map<parid_t, View> Communicator::partition_views_;
std::mutex Communicator::partition_views_mutex_;

/************************RULE begin*********************************/

void RuleSpeculativeExecuteQuorumEvent::FeedResponse(bool y, value_t result, bool is_leader) {
  if (y) {
    if (has_result_) {
      verify(result == result_);
    } else {
      has_result_ = true;
      result_ = result;
    }
    if (is_leader)
      q().n_leader_yes_.set(q().n_leader_yes_.get() + 1);
    vote_yes();
  } else {
    if (is_leader)
      q().n_leader_no_.set(q().n_leader_no_.get() + 1);
    vote_no();
  }
}

value_t RuleSpeculativeExecuteQuorumEvent::GetResult() {
  return result_;
}

/************************RULE end*********************************/

uint64_t Communicator::global_id = 0;

// Use mako-dev's PollThread type (correct architecture)
Communicator::Communicator(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) {
  Log_info("setup paxos communicator");
  vector<string> addrs;
  if (poll_thread_worker.is_none()) {
    rpc_poll_ = rusty::Some(PollThread::create());
    owns_poll_thread_ = true;  // We created this poll thread, we own it
  } else {
    rpc_poll_ = rusty::Some(poll_thread_worker.as_ref().unwrap().clone());
    owns_poll_thread_ = false;  // Passed in, don't shutdown on destruction
  }
  auto config = Config::GetConfig();
  // create more client per server
  int proxy_batch_size = 1 ;
  vector<parid_t> partitions = config->GetAllPartitionIds();
  for (auto& par_id : partitions) {
    auto site_infos = config->SitesByPartitionId(par_id);
    vector<std::pair<siteid_t, ClassicProxy*>> proxies;
    for (int i=0; i<proxy_batch_size; i++) {
      for (auto& si : site_infos) {
        auto result = ConnectToSite(si, std::chrono::milliseconds
            (CONNECT_TIMEOUT_MS));
        verify(result.first == SUCCESS);
        proxies.push_back(std::make_pair(si.id, result.second));
      }
    }
    rpc_par_proxies_.insert(std::make_pair(par_id, proxies));

  }
  client_leaders_connected_.store(false);
  if (config->forwarding_enabled_) {
    threads.push_back(std::thread(&Communicator::ConnectClientLeaders, this));
  } else {
    client_leaders_connected_.store(true);
  }
}

void Communicator::ConnectClientLeaders() {
  auto config = Config::GetConfig();
  if (config->forwarding_enabled_) {
    Log_info("{}: connect to client sites", __FUNCTION__);
    auto client_leaders = config->SitesByLocaleId(0, Config::CLIENT);
    for (Config::SiteInfo leader_site_info : client_leaders) {
      verify(leader_site_info.locale_id == 0);
      Log_info("client @ leader {}", leader_site_info.id);
      auto result = ConnectToClientSite(leader_site_info,
                                        std::chrono::milliseconds
                                            (CONNECT_TIMEOUT_MS));
      verify(result.first == SUCCESS);
      verify(result.second != nullptr);
      Log_info("connected to client leader site: {}, {}, {}",
               leader_site_info.id,
               leader_site_info.locale_id,
               (void*)result.second);
      client_leaders_.push_back(std::make_pair(leader_site_info.id,
                                               result.second));
    }
  }
  client_leaders_connected_.store(true);
}

void Communicator::WaitConnectClientLeaders() {
  bool connected;
  do {
    connected = client_leaders_connected_.load();
  } while (!connected);
  Log_info("Done waiting to connect to client leaders.");
}

// removed `Communicator::ResetProfiles()`
// — reset all of the now-deleted CPU-utilization / RPC-latency
// profiling fields (`index`, `total`, `window`, `window_time`,
// `total_time`, `window_avg`, `total_avg`). Its only callers were
// inside `if(false && ...)` short-circuited re-elect branches in
// `classic/coordinator.cc:494, 675`, both of which were removed
// alongside.

Communicator::~Communicator() {
  verify(rpc_clients_.size() > 0);
  for (auto& pair : rpc_clients_) {
    auto& rpc_cli = pair.second;
    rpc_cli->close();
    // shared_ptr handles cleanup automatically
  }
  rpc_clients_.clear();

  // Only shutdown PollThread if we created it (owns_poll_thread_ == true)
  // If it was passed in (shared with RPC server), we must NOT shutdown it
  // or the server will stop accepting new connections
  if (rpc_poll_.is_some() && owns_poll_thread_) {
    Log_info("[COMMUNICATOR] Shutting down owned poll thread");
    rpc_poll_.as_ref().unwrap()->shutdown();
  } else if (rpc_poll_.is_some()) {
    Log_info("[COMMUNICATOR] Not shutting down shared poll thread (owns_poll_thread_=false)");
  }
}

bool Communicator::EnsureClientConnected(siteid_t site_id) {
  auto it = rpc_clients_.find(site_id);
  if (it == rpc_clients_.end()) {
    Log_error("Communicator: no client for site {}", site_id);
    return false;
  }

  auto& client = it->second;

  // Check if already connected
  if (client->connected()) {
    return true;
  }

  // Try to reconnect if in FAILED or DISCONNECTED state
  auto state = client->connection_state();
  if (state == rrr::ConnectionState::FAILED ||
      state == rrr::ConnectionState::DISCONNECTED) {
    Log_info("Communicator: site {} in state {}, attempting reconnect",
             site_id, rrr::connection_state_to_string(state));

    if (client->try_reconnect_if_needed()) {
      Log_info("Communicator: reconnected to site {} successfully", site_id);
      return true;
    } else {
      Log_warn("Communicator: reconnect to site {} failed", site_id);
      return false;
    }
  }

  // CONNECTING or other transient state - can't help immediately
  Log_debug("Communicator: site {} in transient state {}",
            site_id, rrr::connection_state_to_string(state));
  return false;
}

std::pair<siteid_t, ClassicProxy*>
Communicator::RandomProxyForPartition(parid_t par_id) const {
  auto it = rpc_par_proxies_.find(par_id);
  verify(it != rpc_par_proxies_.end());
  auto& par_proxies = it->second;
  int index = rrr::RandomGenerator::rand(0, par_proxies.size() - 1);
  return par_proxies[index];
}

// for most protocol, e.g., Paxos or Raft, the client always 
//      tries to issue the request to the fixed leader (the first one) (idx is -1 by default)
// but, for Mencius, it uses round robin to rotate the leader (idx > -1)
// @param idx: get the index of servers as the leader
std::pair<siteid_t, ClassicProxy*>
Communicator::LeaderProxyForPartition(parid_t par_id, int idx) const {
  
  if (idx > -1) { // Mencius
    auto it = rpc_par_proxies_.find(par_id);
    auto& partition_proxies = it->second;
    verify(partition_proxies.size()>idx);
    return it->second.at(idx);
  }
  
  // Check if we have a dynamic leader callback
  if (leader_callback_) {
    locid_t dynamic_leader = leader_callback_(par_id);
    
    if (dynamic_leader >= 0) {
      // Find the proxy for this leader
      auto it = rpc_par_proxies_.find(par_id);
      if (it != rpc_par_proxies_.end()) {
        auto& partition_proxies = it->second;
        auto config = Config::GetConfig();
        
        
        auto proxy_it = std::find_if(
            partition_proxies.begin(),
            partition_proxies.end(),
            [config, dynamic_leader](const std::pair<siteid_t, ClassicProxy*>& p) {
              verify(p.second != nullptr);
              auto& site = config->SiteById(p.first);
              return site.locale_id == dynamic_leader;
            });
        if (proxy_it != partition_proxies.end()) {
          // Update cache and return
          const_cast<Communicator*>(this)->leader_cache_[par_id] = *proxy_it;
          return *proxy_it;
        } else {
        }
      }
    } else {
    }
  }

  // If no dynamic leader, first check partition views for updated leader info
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
        // Update cache and return
        const_cast<Communicator*>(this)->leader_cache_[par_id] = *proxy_it;
        return *proxy_it;
      } else {
      }
    }
  }
  
  // Check the leader cache
  auto leader_cache =
      const_cast<map<parid_t, SiteProxyPair>&>(this->leader_cache_);
  auto leader_it = leader_cache.find(par_id);
  if (leader_it != leader_cache.end()) {
    return leader_it->second;
  } else {
    auto it = rpc_par_proxies_.find(par_id);
    verify(it != rpc_par_proxies_.end());
    auto& partition_proxies = it->second;
    auto config = Config::GetConfig();
    auto proxy_it = std::find_if(
        partition_proxies.begin(),
        partition_proxies.end(),
        [config](const std::pair<siteid_t, ClassicProxy*>& p) {
          verify(p.second != nullptr);
          auto& site = config->SiteById(p.first);
          return site.locale_id == 0;
        });
    if (proxy_it == partition_proxies.end()) {
      Log_fatal("could not find leader for partition {}", par_id);
    } else {
      leader_cache[par_id] = *proxy_it;
    }
    verify(proxy_it->second != nullptr);
    return *proxy_it;
  }
}

ClientSiteProxyPair
Communicator::ConnectToClientSite(Config::SiteInfo& site,
                                  std::chrono::milliseconds timeout) {
  auto config = Config::GetConfig();
  char addr[1024];
  snprintf(addr, sizeof(addr), "%s:%d", site.host.c_str(), site.port);

  auto start = std::chrono::steady_clock::now();
  auto rpc_cli = rrr::Client::create(rpc_poll_.as_ref().unwrap());
  double elapsed;
  int attempt = 0;
  do {
    Log_debug("connect to client site: {} (attempt {})", addr, attempt++);
    auto connect_result = rpc_cli->connect(reinterpret_cast<const int8_t*>(addr), false);
    if (connect_result == SUCCESS) {
      // Arc::get() returns const T*, but proxy doesn't mutate client
      ClientControlProxy* rpc_proxy = new ClientControlProxy(const_cast<rrr::Client*>(rpc_cli.get()));
      rpc_clients_.insert(std::make_pair(site.id, rpc_cli));
      Log_debug("connect to client site: {} success!", addr);
      return std::make_pair(SUCCESS, rpc_proxy);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(CONNECT_SLEEP_MS));
    }
    auto end = std::chrono::steady_clock::now();
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
        .count();
  } while (elapsed < timeout.count());
  Log_info("timeout connecting to client {}", addr);
  rpc_cli->close();
  // Arc handles cleanup automatically
  return std::make_pair(FAILURE, nullptr);
}

std::pair<int, ClassicProxy*>
Communicator::ConnectToSite(Config::SiteInfo& site,
                            std::chrono::milliseconds timeout) {
  string addr = site.GetHostAddr();
  auto start = std::chrono::steady_clock::now();
  auto rpc_cli = rrr::Client::create(rpc_poll_.as_ref().unwrap());
  double elapsed;
  int attempt = 0;
  do {
    Log_debug("connect to site: {} (attempt {})", addr.c_str(), attempt++);
    auto connect_result = rpc_cli->connect(reinterpret_cast<const int8_t*>(addr.c_str()), false);
    if (connect_result == SUCCESS) {
      // Arc::get() returns const T*, but proxy doesn't mutate client
      ClassicProxy* rpc_proxy = new ClassicProxy(const_cast<rrr::Client*>(rpc_cli.get()));
      rpc_clients_.insert(std::make_pair(site.id, rpc_cli));
      rpc_proxies_.insert(std::make_pair(site.id, rpc_proxy));

      // Keep a host-scoped reference to the connection through PollableProxy.
      auto conn_opt = rpc_cli->connection();
      if (conn_opt.is_some()) {
        // Client::host() now returns rusty::String (post-DSL migration);
        // rrr::reactor_clients_th_ is keyed by std::string — convert once.
        std::string host_key = rpc_cli->host().to_string();
        if (!rrr::reactor_clients_th_.contains_key(host_key)) {
          rrr::reactor_clients_th_.insert(host_key, rusty::Vec<rrr::PollableProxy>{});
        }
        auto conn_proxy = rrr::make_pollable_proxy_from_typed_arc(conn_opt.as_ref().unwrap().clone());
        rrr::reactor_clients_th_.get(host_key).unwrap().push(std::move(conn_proxy));
      }
      Log_info("connect to site: {} success!", addr.c_str());
      return std::make_pair(SUCCESS, rpc_proxy);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(CONNECT_SLEEP_MS));
    }
    auto end = std::chrono::steady_clock::now();
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
        .count();
  } while (elapsed < timeout.count());
  Log_info("timeout connecting to {}", addr.c_str());
  rpc_cli->close();
  // Arc handles cleanup automatically
  return std::make_pair(FAILURE, nullptr);
}

bool Communicator::ReconnectToSite(siteid_t site_id, parid_t par_id) {
  Log_info("[RECONNECT] Attempting to reconnect to site {} for partition {}", site_id, par_id);

  auto config = Config::GetConfig();
  Config::SiteInfo* site_info = nullptr;

  // Find the site info
  for (auto& site : config->sites_) {
    if (site.id == site_id) {
      site_info = &site;
      break;
    }
  }

  if (!site_info) {
    Log_error("[RECONNECT] Could not find site info for site {}", site_id);
    return false;
  }

  // Close old connection if exists
  auto old_client_it = rpc_clients_.find(site_id);
  if (old_client_it != rpc_clients_.end()) {
    Log_info("[RECONNECT] Closing old connection to site {}", site_id);
    old_client_it->second->close();
    rpc_clients_.erase(old_client_it);
  }

  // Delete old proxy if exists
  auto old_proxy_it = rpc_proxies_.find(site_id);
  ClassicProxy* old_proxy = nullptr;
  if (old_proxy_it != rpc_proxies_.end()) {
    old_proxy = old_proxy_it->second;
    rpc_proxies_.erase(old_proxy_it);
  }

  // Create new connection
  auto result = ConnectToSite(*site_info, std::chrono::milliseconds(CONNECT_TIMEOUT_MS));
  if (result.first != SUCCESS) {
    Log_error("[RECONNECT] Failed to reconnect to site {}", site_id);
    return false;
  }

  ClassicProxy* new_proxy = result.second;
  Log_info("[RECONNECT] Successfully reconnected to site {}, new proxy={}", site_id, (void*)new_proxy);

  // Update rpc_par_proxies_ with new proxy
  auto par_it = rpc_par_proxies_.find(par_id);
  if (par_it != rpc_par_proxies_.end()) {
    for (auto& pair : par_it->second) {
      if (pair.first == site_id) {
        Log_info("[RECONNECT] Updating proxy in rpc_par_proxies_ for site {}: old={} new={}",
                 site_id, (void*)pair.second, (void*)new_proxy);
        pair.second = new_proxy;
        break;
      }
    }
  }

  // Clean up old proxy after updating references
  if (old_proxy) {
    delete old_proxy;
  }

  return true;
}

std::pair<siteid_t, ClassicProxy*>
Communicator::NearestProxyForPartition(parid_t par_id) const {
  // TODO Fix me.
  auto it = rpc_par_proxies_.find(par_id);
  verify(it != rpc_par_proxies_.end());
  auto& partition_proxies = it->second;
  verify(partition_proxies.size() > loc_id_);
  int index = loc_id_;
  return partition_proxies[index];
};

void Communicator::Pause() {
  for (auto it = rpc_clients_.begin(); it != rpc_clients_.end(); it++) {
    it->second->pause();
  }
}

void Communicator::Resume() {
  for (auto it = rpc_clients_.begin(); it != rpc_clients_.end(); it++) {
    it->second->resume();
  }
}

rusty::Arc<QuorumEvent> Communicator::SendReelect(){
	//paused = true;
	//sleep(10);
	int total = rpc_par_proxies_[0].size() - 1;
  rusty::Arc<QuorumEvent> e = create_sp_quorum_event(total, 1);
	auto pair_leader_proxy = LeaderProxyForPartition(0);
	int new_leader = (pair_leader_proxy.first + 1) % total;

	for(auto& pair: rpc_par_proxies_[0]){
		rrr::FutureAttr fuattr;
		int id = pair.first;
		if(id != 1) continue;
			fuattr.callback =
				[e, this, id] (rusty::Arc<Future> fu) {
        if (fu->get_error_code() != 0) {
          Log_info("Get a error message in reply");
          return;
        }
				bool_t success = false;
				rrr::deserialize_from(fu->get_reply(), success);

					if(success){
						e->vote_yes();
						this->SetNewLeaderProxy(0, id);
					}
				};
			ClassicProxy::RpcReElectRequest req;
			auto f = pair.second->async_ReElect(req, fuattr);
			if (f.is_ok()) {
				Future::safe_release(f.unwrap().raw_future());
			}
		}
		return e;

}

void Communicator::BroadcastDispatch(
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
        janus::Command view_md;
        rrr::deserialize_from(fu->get_reply(), ret, outputs, coro_id, view_md);
        
        // Handle WRONG_LEADER response with view data
        if (ret == WRONG_LEADER && view_md.has_value()) {
          const auto sp_view_data = marshallable_cast<ViewData>(view_md);
          if (sp_view_data.is_some()) {
            UpdatePartitionView(par_id, *sp_view_data.unwrap());
          }
        }
        callback(ret, outputs);
      };
  
  std::pair<siteid_t, ClassicProxy*> pair_leader_proxy;
  if (Config::GetConfig()->replica_proto_==MODE_MENCIUS) {
    // The logic here is: Mencius have multiple proposor, if the client is co-locate with a proposer, it give all commands to this proposor.
    // If not, round-robin with all proposors.
    auto server_infos = Config::GetConfig()->GetMyServers();
    if (server_infos.size() == 1) {
      int n = rpc_par_proxies_.find(par_id)->second.size();
      pair_leader_proxy = LeaderProxyForPartition(par_id, server_infos[0].id);
    } else {
      int n = rpc_par_proxies_.find(par_id)->second.size();
      pair_leader_proxy = LeaderProxyForPartition(par_id, coo->coo_id_ % n);
    }
  } else {
    pair_leader_proxy = LeaderProxyForPartition(par_id);
  }
  
  SetLeaderCache(par_id, pair_leader_proxy);
  Log_debug("send dispatch to site {}, par {}",
            pair_leader_proxy.first, par_id);
  auto proxy = pair_leader_proxy.second;
  // Fill-then-wrap: build the payload locally, wrap once complete.
  VecPieceData vpd;
  vpd.sp_vec_piece_data_ = sp_vec_piece;

  // Record Time
  vpd.time_sent_from_client_ = SimpleRWCommand::GetCurrentMsTime();

  janus::Command md(rusty::Arc<VecPieceData>::make(std::move(vpd)));

  DepId di;
  di.str = "dep";
  di.id = Communicator::global_id++;

#ifdef COPILOT_TIME_DEBUG
  struct timeval tp;
  gettimeofday(&tp, NULL);
  Log_info("[Jetpack] [C-] BroadcastDispatch at Communicator {:.3f}", tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif

  WAN_WAIT;
#ifdef FULL_LOG_DEBUG
  Log_info("[Jetpack] cmd<{}, {}> before async_Dispatch", SimpleRWCommand::GetCmdID(md).first, SimpleRWCommand::GetCmdID(md).second);
#endif
#ifdef LATENCY_LOG_DEBUG
  Log_info("!!!!!!!! Before proxy->async_Dispatch(cmd_id, di, md, fuattr);");
#endif
  ClassicProxy::RpcDispatchRequest dispatch_req;
  dispatch_req.tid = cmd_id;
  dispatch_req.dep_id = di;
  dispatch_req.cmd = md;
  auto future = proxy->async_Dispatch(dispatch_req, fuattr);
  if (future.is_ok()) {
    Future::safe_release(future.unwrap().raw_future());
  }
}

// removed `Communicator::SyncBroadcastDispatch`
// (~78 LOC) — only call site was the now-deleted
// `CoordinatorClassic::DispatchSync`.  This was the
// synchronous (blocking `proxy->Dispatch(req)`) twin of the live
// async `BroadcastDispatch` path; no surviving caller anywhere.

//need to change this code to solve the quorum info in the graphs
//either create another event here or inside the coordinator.
rusty::Arc<IntEvent> Communicator::BroadcastDispatch(
    ReadyPiecesData cmds_by_par,
    Coordinator* coo,
    TxData* txn) {
  int total = cmds_by_par.size();
  //std::shared_ptr<WaitAll> e = create_sp_waitall();
  rusty::Arc<IntEvent> e = create_sp_int_event(1);
	e->value_.set(0);
	e->target_.set(total);
  std::unordered_set<int> leaders{};
  auto src_coroid = e->get_fiber_id();
  coo->coro_id_ = src_coroid;
  Log_info("The size of cmds_by_par is {}", cmds_by_par.size());

  for(auto& pair: cmds_by_par){
    bool first = false;
    auto& cmds = pair.second;
    auto sp_vec_piece = std::make_shared<vector<shared_ptr<TxPieceData>>>();
    for(auto c: cmds){
      c->id_ = coo->next_pie_id();
      coo->dispatch_acks_[c->inn_id_] = false;
      sp_vec_piece->push_back(c);
    }
    cmdid_t cmd_id = sp_vec_piece->at(0)->root_id_;
    verify(sp_vec_piece->size() > 0);
    auto par_id = sp_vec_piece->at(0)->PartitionId();
    auto pair_leader_proxy = LeaderProxyForPartition(par_id);
    auto leader_id = pair_leader_proxy.first;

    phase_t phase = coo->phase_;
    rrr::FutureAttr fuattr;
    fuattr.callback =
        [e, coo, this, phase, txn, src_coroid, leader_id, par_id](rusty::Arc<Future> fu) {
          if (fu->get_error_code() != 0) {
            Log_info("Get a error message in reply");
            return;
          }
          int32_t ret;
          TxnOutput outputs;
          uint64_t coro_id = 0;
          janus::Command view_md;
	  			double cpu = 0.0;
	  			double net = 0.0;
          rrr::deserialize_from(fu->get_reply(), ret, outputs, coro_id, view_md);

          e->value_.set(e->value_.get() + 1);
          if(phase != coo->phase_){
						verify(0);
	    			e->test();
	  			}
          else{
            // Handle WRONG_LEADER response with view data
            if (ret == WRONG_LEADER && view_md.has_value()) {
              const auto sp_view_data = marshallable_cast<ViewData>(view_md);
              if (sp_view_data.is_some()) {
                UpdatePartitionView(par_id, *sp_view_data.unwrap());
              }
              coo->aborted_ = true;
              txn->commit_.store(false);
              e->value_.set(e->target_.get());
              e->test();
              return;
            }
            
            if(ret == REJECT){
              coo->aborted_ = true;
              txn->commit_.store(false);

							e->value_.set(e->target_.get());
							e->test();
							return;
            }
            coo->n_dispatch_ack_ += outputs.size();
            for(auto& pair: outputs){
              const uint32_t& inn_id = pair.first;
              coo->dispatch_acks_[inn_id] = true;
              txn->Merge(pair.first, pair.second);
            }
	  
	    			CoordinatorClassic* classic_coo = (CoordinatorClassic*)coo;
	    			//classic_coo->debug_cnt--;
            if(txn->HasMoreUnsentPiece()){
              classic_coo->DispatchAsync(false);
            }
              //e->add_dep(coo->cli_id_, src_coroid, leader_id, coro_id);
            // removed
            // `coo->ids_.push_back(leader_id);` — the
            // `Coordinator::ids_` vector had no readers anywhere, so
            // the field went away in the same commit.
            e->test();
	  			}
      };
    
    Log_debug("send dispatch to site {}",
              pair_leader_proxy.first);
    auto proxy = pair_leader_proxy.second;
    // Fill-then-wrap: build the payload locally, wrap once complete.
    VecPieceData vpd;
    vpd.sp_vec_piece_data_ = sp_vec_piece;
    janus::Command md(rusty::Arc<VecPieceData>::make(std::move(vpd))); // ????
    CoordinatorClassic* classic_coo = (CoordinatorClassic*) coo;
    //classic_coo->debug_cnt++;

    // removed the `outbound_[src_coroid]` start-time
    // record + the matching `clock_gettime(CLOCK_REALTIME, &start_)` — the
    // recorded start times were only consumed by the dead window-tracking
    // blocks in the Commit / Abort callbacks below, also removed.

		DepId di;
		di.str = "dep";
		di.id = Communicator::global_id++;
    
			ClassicProxy::RpcDispatchRequest dispatch_req;
			dispatch_req.tid = cmd_id;
			dispatch_req.dep_id = di;
			dispatch_req.cmd = md;
			auto future = proxy->async_Dispatch(dispatch_req, fuattr);
    if (future.is_ok()) {
      Future::safe_release(future.unwrap().raw_future());
    }
    if(!broadcasting_to_leaders_only_){
      for (auto& pair : rpc_par_proxies_[par_id]) {
        if (pair.first != pair_leader_proxy.first) {
          //if(first) curr->n_total_++;
          auto follower_id = pair.first;
          rrr::FutureAttr fuattr;
          fuattr.callback =
              [e, coo, this, src_coroid, follower_id](rusty::Arc<Future> fu) {
                if (fu->get_error_code() != 0) {
                  Log_info("Get a error message in reply");
                  return;
                }
                int32_t ret;
                TxnOutput outputs;
                uint64_t coro_id = 0;
                janus::Command view_md;
                rrr::deserialize_from(fu->get_reply(), ret, outputs, coro_id, view_md);
                //e->add_dep(coo->cli_id_, src_coroid, follower_id, coro_id);
                //coo->ids_.push_back(follower_id);
                // do nothing
              };
					DepId di2;
					di2.str = "dep";
					di2.id = Communicator::global_id++;
          
						ClassicProxy::RpcDispatchRequest follower_dispatch_req;
						follower_dispatch_req.tid = cmd_id;
						follower_dispatch_req.dep_id = di2;
						follower_dispatch_req.cmd = md;
						auto follower_future = pair.second->async_Dispatch(follower_dispatch_req, fuattr);
            if (follower_future.is_ok()) {
						  Future::safe_release(follower_future.unwrap().raw_future());
            }
        }
      }
    }
  }
  //probably should modify the data structure here.
  return e;
}


void Communicator::SendStart(SimpleCommand& cmd,
                             int32_t output_size,
                             std::function<void(rusty::Arc<Future> fu)>& callback) {
  verify(0);
}

rusty::Arc<WaitAll>
Communicator::SendPrepare(Coordinator* coo,
                          txnid_t tid,
                          std::vector<int32_t>& sids){
	int32_t res_ = 10;
  TxData* cmd = (TxData*) coo->cmd_;
  auto n = cmd->partition_ids_.size();
  auto e = create_sp_waitall();
  auto phase = coo->phase_;
  int n_total = 1;
  int quorum_id = 0;
  for(auto& partition_id : cmd->partition_ids_){
    auto leader_id = LeaderProxyForPartition(partition_id).first;
    auto site_id = leader_id;
    auto proxies = rpc_par_proxies_[partition_id];
    if(follower_forwarding) n_total = 3;
    auto qe = create_sp_quorum_event(n_total, 1);
    e->add_event(qe);
    auto src_coroid = qe->get_fiber_id();
      
    qe->id_.set(Communicator::global_id);
    qe->par_id_.set(quorum_id++);
    FutureAttr fuattr;
    fuattr.callback = [this, e, qe, src_coroid, site_id, coo, phase, cmd, tid](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
			bool_t slow;
      uint64_t coro_id = 0;
      rrr::deserialize_from(fu->get_reply(), res, slow, coro_id);

			this->slow = slow;
      // qe->add_dep(coo->cli_id_, src_coroid, site_id, coro_id);

      if(phase != coo->phase_){
        return;
      }

      if(res == REJECT){
				Log_info("REJECT in prepare");
        cmd->commit_.store(false);
        coo->aborted_ = true;
      }
      qe->n_voted_yes_.set(qe->n_voted_yes_.get() + 1);
      e->test();
    };
    
    ClassicProxy* proxy = LeaderProxyForPartition(partition_id).second;
    Log_debug("SendPrepare to {} sites gid:{}, tid:{}\n",
              sids.size(),
              partition_id,
              tid);
		DepId di;
		di.str = "dep";
		di.id = Communicator::global_id++;
    
			ClassicProxy::RpcPrepareRequest prepare_req;
			prepare_req.tid = tid;
			prepare_req.sids = sids;
			prepare_req.dep_id = di;
      auto prepare_result = proxy->async_Prepare(prepare_req, fuattr);
      if (prepare_result.is_ok()) {
			  Future::safe_release(prepare_result.unwrap().raw_future());
      }
    if(follower_forwarding){
      for(auto& pair : rpc_par_proxies_[partition_id]){
        if(pair.first != leader_id){
          site_id = pair.first;
          proxy = pair.second;
					
					DepId di2;
					di2.str = "dep";
					di2.id = Communicator::global_id++;
          
						ClassicProxy::RpcPrepareRequest follower_prepare_req;
						follower_prepare_req.tid = tid;
						follower_prepare_req.sids = sids;
						follower_prepare_req.dep_id = di2;
            auto follower_prepare_result = proxy->async_Prepare(follower_prepare_req, fuattr);
            if (follower_prepare_result.is_ok()) {
						  Future::safe_release(follower_prepare_result.unwrap().raw_future());
            }
        }
      }
    }
  }
  return e;
}

/*void Communicator::SendPrepare(groupid_t gid,
                               txnid_t tid,
                               std::vector<int32_t>& sids,
                               const function<void(int)>& callback) {
  FutureAttr fuattr;
  std::function<void(rusty::Arc<Future>)> cb =
      [this, callback](rusty::Arc<Future> fu) {
        int res;
        rrr::deserialize_from(fu->get_reply(), res);
        callback(res);
      };
  fuattr.callback = cb;
  // ClassicProxy* proxy = LeaderProxyForPartition(gid).second;
  auto pair_proxies = PilotProxyForPartition(gid);
  verify(pair_proxies.size() == 2);
  Log_debug("SendPrepare to {} sites gid:{}, tid:{}\n",
            sids.size(),
            gid,
            tid);
  for (auto& p : pair_proxies)
    Future::safe_release(p.second->async_Prepare(tid, sids, fuattr));
}*/

void Communicator::___LogSent(parid_t pid, txnid_t tid) {
  auto value = std::make_pair(pid, tid);
  auto it = phase_three_sent_.find(value);
  if (it != phase_three_sent_.end()) {
    Log_fatal("phase 3 sent exists: {} {:x}", it->first, it->second);
  } else {
    phase_three_sent_.insert(value);
    Log_debug("phase 3 sent: pid: {}; tid: {:x}", value.first, value.second);
  }
}

rusty::Arc<WaitAll>
Communicator::SendCommit(Coordinator* coo,
                              txnid_t tid) {
#ifdef LOG_LEVEL_AS_DEBUG
//  ___LogSent(pid, tid);
#endif
	TxData* cmd = (TxData*) coo->cmd_;
  int n_total = 1;
  auto n = cmd->GetPartitionIds().size();
  auto e = create_sp_waitall();
  
  for(auto& rp : cmd->partition_ids_){
    auto leader_id = LeaderProxyForPartition(rp).first;
    auto site_id = leader_id;
    auto proxies = rpc_par_proxies_[rp];
    if(follower_forwarding) n_total = 3;
    auto qe = create_sp_quorum_event(n_total, 1);
    qe->id_.set(Communicator::global_id);
    auto src_coroid = qe->get_fiber_id();

    e->add_event(qe);

    coo->n_finish_req_++;
    FutureAttr fuattr;
    auto phase = coo->phase_;
    fuattr.callback = [this, e, qe, src_coroid, site_id, coo, phase, cmd, tid](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
			bool_t slow;
      uint64_t coro_id = 0;
			Profiling profile;
      janus::Command view_md;
      rrr::deserialize_from(fu->get_reply(), res, slow, coro_id, profile, view_md);
			this->slow = slow;
			// removed `cpu = profile.cpu_util;`
			// — the `cpu` field was deleted alongside the rest of the
			// dead CPU / RPC-latency profiling subsystem.
      // Propagate the result status (including WRONG_LEADER) back to the coordinator
      cmd->reply_.res_ = res;
      
      // Extract and attach view data if present
      if (view_md.has_value()) {
        const auto sp_view_data = marshallable_cast<ViewData>(view_md);
        if (sp_view_data.is_some()) {
          cmd->reply_.sp_view_data_ = sp_view_data;
          Log_info("[VIEW_PROPAGATE] Received view data in Commit response for tx_id={}: {}",
                   tid, sp_view_data.unwrap()->ToString().c_str());
        }
      }

      // removed the rolling-window
      // RPC-latency tracking block that read
      // `outbound_[src_coroid]`, computed `curr` in microseconds,
      // and updated `total_time` / `window_time` / `window_avg`
      // / `total_avg` / `total` / `index` / `window[200]`.  The
      // averages were never read outside commented-out
      // `Log_info` lines and `if(false && ...)` short-circuited
      // re-elect branches in `classic/coordinator.cc`.

      // qe->add_dep(coo->cli_id_, src_coroid, site_id, coro_id);

      if(coo->phase_ != phase) return;
      qe->n_voted_yes_.set(qe->n_voted_yes_.get() + 1);
      e->test();
    };

		DepId di;
		di.str = "dep";
		di.id = Communicator::global_id++;
    ClassicProxy* proxy = LeaderProxyForPartition(rp).second;
    Log_debug("SendCommit to {} tid:{}\n", rp, tid);
    ClassicProxy::RpcCommitRequest commit_req;
    commit_req.tid = tid;
    commit_req.dep_id = di;
    auto commit_result = proxy->async_Commit(commit_req, fuattr);
    if (commit_result.is_ok()) {
      Future::safe_release(commit_result.unwrap().raw_future());
    }
    
    if(follower_forwarding){
      for(auto& pair : rpc_par_proxies_[rp]){
        if(pair.first != leader_id){
					DepId di2;
					di2.str = "dep";
					di2.id = Communicator::global_id++;
          
					site_id = pair.first;
          proxy = pair.second;
          ClassicProxy::RpcCommitRequest follower_commit_req;
          follower_commit_req.tid = tid;
          follower_commit_req.dep_id = di2;
          auto follower_commit_result = proxy->async_Commit(follower_commit_req, fuattr);
          if (follower_commit_result.is_ok()) {
            Future::safe_release(follower_commit_result.unwrap().raw_future());
          }
        }
      }
    }

    // removed `coo->site_commit_[rp]++;` —
    // counter was write-only; field gone.

  }
  return e;
}

/*void Communicator::SendCommit(parid_t pid,
                              txnid_t tid,
                              rusty::Function<void()> callback) {
#ifdef LOG_LEVEL_AS_DEBUG
  ___LogSent(pid, tid);
#endif
  FutureAttr fuattr;
  fuattr.callback = [callback](rusty::Arc<Future>) { callback(); };
  auto proxy_pair = LeaderProxyForPartition(pid);
  ClassicProxy* proxy = proxy_pair.second;
  SetLeaderCache(pid, proxy_pair);
  Log_debug("SendCommit to {} tid:{}\n", pid, tid);
  Future::safe_release(proxy->async_Commit(tid, 0, fuattr));
}*/

rusty::Arc<WaitAll>
Communicator::SendAbort(Coordinator* coo,
                              txnid_t tid) {
#ifdef LOG_LEVEL_AS_DEBUG
//  ___LogSent(pid, tid);
#endif
  TxData* cmd = (TxData*) coo->cmd_;
  int n_total = 1;
  auto n = cmd->GetPartitionIds().size();
  auto e = create_sp_waitall();
  for(auto& rp : cmd->partition_ids_){
    auto proxies = rpc_par_proxies_[rp];
    auto leader_id = LeaderProxyForPartition(rp).first;
    auto site_id = leader_id;
    if(follower_forwarding) n_total = 3;
    auto qe = create_sp_quorum_event(n_total, 1);
    qe->id_.set(Communicator::global_id);
    auto src_coroid = qe->get_fiber_id();

    e->add_event(qe);

    coo->n_finish_req_++;
    FutureAttr fuattr;
    auto phase = coo->phase_;
    fuattr.callback = [this, e, qe, coo, src_coroid, site_id, phase, cmd, tid](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int32_t res;
      bool_t slow;
      uint64_t coro_id = 0;
      Profiling profile;
      janus::Command view_md;
      rrr::deserialize_from(fu->get_reply(), res, slow, coro_id, profile, view_md);
      this->slow = slow;

      // Propagate the result status (including WRONG_LEADER) back to the coordinator
      cmd->reply_.res_ = res;

      // Extract and attach view data if present
      if (view_md.has_value()) {
        const auto sp_view_data = marshallable_cast<ViewData>(view_md);
        if (sp_view_data.is_some()) {
          cmd->reply_.sp_view_data_ = sp_view_data;
          Log_info("[VIEW_PROPAGATE] Received view data in Abort response for tx_id={}: {}",
                   tid, sp_view_data.unwrap()->ToString().c_str());
        }
      }

      // removed the CPU-utilization /
      // network-utilization snapshot (`profile.cpu_util` /
      // `profile.tx_util` writes into `this->cpu` / `this->tx`)
      // and the rolling-window RPC-latency tracking block that
      // updated `total_time` / `window_time` / `window` / `total`
      // / `index`.  Same dead-state cleanup as the Commit-callback
      // path above.

      // qe->add_dep(coo->cli_id_, src_coroid, site_id, coro_id);

      if(coo->phase_ != phase) return;
      qe->n_voted_yes_.set(qe->n_voted_yes_.get() + 1);
      e->test();
    };

    DepId di;
    di.str = "dep";
    di.id = Communicator::global_id++;
    ClassicProxy* proxy = LeaderProxyForPartition(rp).second;
    Log_debug("SendAbort to {} tid:{}\n", rp, tid);
    ClassicProxy::RpcAbortRequest abort_req;
    abort_req.tid = tid;
    abort_req.dep_id = di;
    auto abort_result = proxy->async_Abort(abort_req, fuattr);
    if (abort_result.is_ok()) {
      Future::safe_release(abort_result.unwrap().raw_future());
    }

    if(follower_forwarding){
      for(auto& pair : rpc_par_proxies_[rp]){
        if(pair.first != leader_id){
          DepId di2;
          di2.str = "dep";
          di2.id = Communicator::global_id++;

          site_id = pair.first;
          proxy = pair.second;
          ClassicProxy::RpcAbortRequest follower_abort_req;
          follower_abort_req.tid = tid;
          follower_abort_req.dep_id = di2;
          auto follower_abort_result = proxy->async_Abort(follower_abort_req, fuattr);
          if (follower_abort_result.is_ok()) {
            Future::safe_release(follower_abort_result.unwrap().raw_future());
          }
        }
      }

    }
    // removed `coo->site_abort_[rp]++;` —
    // counter was write-only; field gone.
  }
  return e;
}

void Communicator::SendEarlyAbort(parid_t pid,
                                  txnid_t tid) {
#ifdef LOG_LEVEL_AS_DEBUG
  ___LogSent(pid, tid);
#endif
  FutureAttr fuattr;
  fuattr.callback = [](rusty::Arc<Future>) {};
  ClassicProxy* proxy = LeaderProxyForPartition(pid).second;
  Log_debug("SendAbort to {} tid:{}\n", pid, tid);
  ClassicProxy::RpcEarlyAbortRequest early_abort_req;
  early_abort_req.tid = tid;
  auto early_abort_result = proxy->async_EarlyAbort(early_abort_req, fuattr);
  if (early_abort_result.is_ok()) {
    Future::safe_release(early_abort_result.unwrap().raw_future());
  }
}

/*void Communicator::SendAbort(parid_t pid, txnid_t tid,
                             rusty::Function<void()> callback) {
#ifdef LOG_LEVEL_AS_DEBUG
  ___LogSent(pid, tid);
#endif
  FutureAttr fuattr;
  fuattr.callback = [callback](rusty::Arc<Future>) { callback(); };
  // ClassicProxy* proxy = LeaderProxyForPartition(pid).second;
  auto pair_proxies = PilotProxyForPartition(pid);
  Log_debug("SendAbort to {} tid:{}\n", pid, tid);
  for (auto& p : pair_proxies)
    Future::safe_release(p.second->async_Abort(tid, fuattr));
}*/

void Communicator::SendUpgradeEpoch(epoch_t curr_epoch,
                                    const function<void(parid_t,
                                                        siteid_t,
                                                        int32_t&)>& callback) {
  for (auto& pair: rpc_par_proxies_) {
    auto& par_id = pair.first;
    auto& proxies = pair.second;
    for (auto& pair: proxies) {
      FutureAttr fuattr;
      auto& site_id = pair.first;
      function<void(rusty::Arc<Future>)> cb = [callback, par_id, site_id](rusty::Arc<Future> fu) {
        if (fu->get_error_code() != 0) {
          Log_info("Get a error message in reply");
          return;
        }
        int32_t res;
        rrr::deserialize_from(fu->get_reply(), res);
        callback(par_id, site_id, res);
      };
      fuattr.callback = cb;
      auto proxy = (ClassicProxy*) pair.second;
      ClassicProxy::RpcUpgradeEpochRequest req;
      req.curr_epoch = curr_epoch;
      auto fu_result = proxy->async_UpgradeEpoch(req, fuattr);
      // Arc auto-released (fire-and-forget pattern)
    }
  }
}

void Communicator::SendTruncateEpoch(epoch_t old_epoch) {
  for (auto& pair: rpc_par_proxies_) {
    auto& par_id = pair.first;
    auto& proxies = pair.second;
    for (auto& pair: proxies) {
      FutureAttr fuattr;
      fuattr.callback = [](rusty::Arc<Future>) {};
      auto proxy = (ClassicProxy*) pair.second;
      ClassicProxy::RpcTruncateEpochRequest req;
      req.old_epoch = old_epoch;
      auto fu_result = proxy->async_TruncateEpoch(req, fuattr);
      // Arc auto-released (fire-and-forget pattern)
    }
  }
}

void Communicator::SendForwardTxnRequest(
    TxRequest& req,
    Coordinator* coo,
    std::function<void(const TxReply&)> callback) {
  Log_info("{}: {}, {}", __FUNCTION__, coo->coo_id_, coo->par_id_);
  verify(client_leaders_.size() > 0);
  auto idx = rrr::RandomGenerator::rand(0, client_leaders_.size() - 1);
  auto p = client_leaders_[idx];
  auto leader_site_id = p.first;
  auto leader_proxy = p.second;
  Log_debug("{}: send to client site {}", __FUNCTION__, leader_site_id);
  TxDispatchRequest dispatch_request;
  dispatch_request.id = coo->coo_id_;
  for (size_t i = 0; i < req.input_.size(); i++) {
    dispatch_request.input.push_back(req.input_[i]);
  }
  dispatch_request.tx_type = req.tx_type_;

  FutureAttr future;
  future.callback = [callback](rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    TxReply reply;
    rrr::deserialize_from(fu->get_reply(), reply);
    callback(reply);
  };
  ClientControlProxy::RpcDispatchTxnRequest dispatch_rpc_req;
  dispatch_rpc_req.req = dispatch_request;
  auto fu_result = leader_proxy->async_DispatchTxn(dispatch_rpc_req, future);
  // Arc auto-released (fire-and-forget pattern)
}

void Communicator::AddMessageHandler(
    function<bool(const janus::Command&, janus::Command&)> f) {
   msg_marshall_handlers_.push_back(f);
}

shared_ptr<GetLeaderQuorumEvent> Communicator::BroadcastGetLeader(
    parid_t par_id, locid_t cur_pause) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = std::make_shared<GetLeaderQuorumEvent>(n - 1, 1);
  auto proxies = rpc_par_proxies_[par_id];
  WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first == cur_pause) continue;
    auto proxy = p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, p](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      bool_t is_leader = false;
      rrr::deserialize_from(fu->get_reply(), is_leader);
      e->FeedResponse(is_leader, p.first);
    };
    ClassicProxy::RpcIsFPGALeaderRequest req;
    req.cur_pause = par_id;
    auto is_leader_result = proxy->async_IsFPGALeader(req, fuattr);
    if (is_leader_result.is_ok()) {
      Future::safe_release(is_leader_result.unwrap().raw_future());
    }
  }
  return e;
}

rusty::Arc<QuorumEvent> Communicator::FailoverPauseSocketOut(
    parid_t par_id, locid_t loc_id) {
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! enter Communicator::FailoverPauseSocketOut");
#endif
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = create_sp_quorum_event(1, 1);
  auto proxies = rpc_par_proxies_[par_id];
  // sleep(1);
  // WAN_WAIT;
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! after Communicator::FailoverPauseSocketOut WAN_WAIT");
#endif
  for (auto& p : proxies) {
    if (p.first != loc_id) continue;
    auto proxy = p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int res;
      rrr::deserialize_from(fu->get_reply(), res);
      if (res == 0)
        e->vote_yes();
      else
        e->vote_no();
    };
#ifdef FAILOVER_DEBUG
    Log_info("!!!!!!!!!!!! Communicator::FailoverPauseSocketOut");
#endif
    ClassicProxy::RpcFailoverPauseSocketOutRequest req;
    auto pause_result = proxy->async_FailoverPauseSocketOut(req, fuattr);
    if (pause_result.is_ok()) {
      Future::safe_release(pause_result.unwrap().raw_future());
    }
  }
  return e;
}

rusty::Arc<QuorumEvent> Communicator::FailoverResumeSocketOut(
    parid_t par_id, locid_t loc_id) {
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! enter Communicator::FailoverResumeSocketOut");
#endif
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = create_sp_quorum_event(1, 1);
  auto proxies = rpc_par_proxies_[par_id];
  // sleep(1);
  // WAN_WAIT;
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! after Communicator::FailoverResumeSocketOut WAN_WAIT");
#endif
  for (auto& p : proxies) {
    if (p.first != loc_id) continue;
    auto proxy = p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      int res;
      rrr::deserialize_from(fu->get_reply(), res);
      if (res == 0)
        e->vote_yes();
      else
        e->vote_no();
    };
#ifdef FAILOVER_DEBUG
    Log_info("!!!!!!!!!!!! Communicator::FailoverResumeSocketOut");
#endif
    ClassicProxy::RpcFailoverResumeSocketOutRequest req;
    auto resume_result = proxy->async_FailoverResumeSocketOut(req, fuattr);
    if (resume_result.is_ok()) {
      Future::safe_release(resume_result.unwrap().raw_future());
    }
  }
  return e;
}

void Communicator::SetNewLeaderProxy(parid_t par_id, locid_t loc_id) {
  bool found = false;
  auto proxies = rpc_par_proxies_[par_id];
  for (auto& p : proxies) {
    if (p.first == loc_id) {
      leader_cache_[par_id] = p;
      found = true;
      break;
    }
  }

  verify(found);

  /*  auto it = rpc_par_proxies_.find(par_id);
    verify(it != rpc_par_proxies_.end());
    auto& partition_proxies = it->second;
    auto config = Config::GetConfig();
    auto proxy_it = std::find_if(
        partition_proxies.begin(),
        partition_proxies.end(),
        [config, loc_id](const std::pair<siteid_t, ClassicProxy*>& p) {
          verify(p.second != nullptr);
          auto& site = config->SiteById(p.first);
          return site.locale_id == loc_id ;
        });
     verify (proxy_it != partition_proxies.end()) ;
     leader_cache_[par_id] = *proxy_it;*/
  Log_debug("set leader proxy for partition {} is {}", par_id, loc_id);
}

void Communicator::SendSimpleCmd(groupid_t gid, SimpleCommand& cmd,
    std::vector<int32_t>& sids, const function<void(int)>& callback) {
  FutureAttr fuattr;
  std::function<void(rusty::Arc<Future>)> cb = [this, callback](rusty::Arc<Future> fu) {
    if (fu->get_error_code() != 0) {
      Log_info("Get a error message in reply");
      return;
    }
    int res;
    rrr::deserialize_from(fu->get_reply(), res);
    callback(res);
  };
  fuattr.callback = cb;
  ClassicProxy* proxy = LeaderProxyForPartition(gid).second;
  Log_debug("SendEmptyCmd to {} sites gid:{}\n", sids.size(), gid);
  ClassicProxy::RpcSimpleCmdRequest req;
  req.cmd = cmd;
  auto simple_cmd_result = proxy->async_SimpleCmd(req, fuattr);
  if (simple_cmd_result.is_ok()) {
    Future::safe_release(simple_cmd_result.unwrap().raw_future());
  }
}


rusty::Arc<QuorumEvent> Communicator::JetpackBroadcastBeginRecovery(parid_t par_id, locid_t loc_id,
                                                                const View& old_view, 
                                                                const View& new_view, 
                                                                epoch_t new_view_id) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = create_sp_quorum_event(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;

  janus::Command old_view_deputy = rusty::Arc<ViewData>::make(old_view);
  janus::Command new_view_deputy = rusty::Arc<ViewData>::make(new_view);
  
  for (auto& p : proxies) {
    // TODO: Local call optimization temporarily commented out
    // if (p.first == loc_id) {
    //     e->vote_yes();
    //     continue;
    // }
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      e->vote_yes();
    };
    ClassicProxy::RpcJetpackBeginRecoveryRequest req;
    req.old_view = old_view_deputy;
    req.new_view = new_view_deputy;
    req.new_view_id = new_view_id;
    auto fu_result = proxy->async_JetpackBeginRecovery(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

shared_ptr<JetpackPullIdSetQuorumEvent> Communicator::JetpackBroadcastPullIdSet(parid_t par_id, locid_t loc_id,
                                                                           epoch_t jepoch, epoch_t oepoch) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = std::make_shared<JetpackPullIdSetQuorumEvent>(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    // TODO: Local call optimization temporarily commented out
    // if (p.first == loc_id) {
    //     // Local call - call OnJetpackPullIdSet directly
    //     bool_t ok;
    //     epoch_t reply_jepoch, reply_oepoch;
    //     janus::Command reply_old_view, reply_new_view;
    //     auto id_set = std::make_shared<VecRecData>();
    //     dtxn_sched_->OnJetpackPullIdSet(jepoch, oepoch, &ok, &reply_jepoch, &reply_oepoch, 
    //                                    &reply_old_view, &reply_new_view, id_set);
    //     janus::Command id_set_deputy;
    //     id_set_deputy = id_set;
    //     e->FeedResponse(ok, reply_jepoch, reply_oepoch, id_set_deputy);
    //     continue;
    // }
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      bool_t ok;
      epoch_t reply_jepoch, reply_oepoch;
      janus::Command reply_old_view, reply_new_view, id_set;
      rrr::deserialize_from(fu->get_reply(), ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, id_set);
      e->FeedResponse(ok, reply_jepoch, reply_oepoch, id_set);
    };
    ClassicProxy::RpcJetpackPullIdSetRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    auto fu_result = proxy->async_JetpackPullIdSet(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

shared_ptr<JetpackPullCmdQuorumEvent> Communicator::JetpackBroadcastPullCmd(parid_t par_id, locid_t loc_id, 
                                                                        const std::vector<key_t>& keys, epoch_t jepoch, epoch_t oepoch) {
  // Log_info("[JETPACK-DEBUG] JetpackBroadcastPullCmd called with par_id={}, loc_id={}, key={}", par_id, loc_id, key);
  
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  // Log_info("[JETPACK-DEBUG] Partition size n={}", n);
  
  auto e = std::make_shared<JetpackPullCmdQuorumEvent>(n, n/2+1, keys);
  // if (!e) {
  //   Log_info("[JETPACK-DEBUG] ERROR: Failed to create JetpackPullCmdQuorumEvent!");
  // }
  
  // if (rpc_par_proxies_.find(par_id) == rpc_par_proxies_.end()) {
  //   Log_info("[JETPACK-DEBUG] ERROR: No proxies found for partition {}!", par_id);
  // }
  
  auto proxies = rpc_par_proxies_[par_id];
  // Log_info("[JETPACK-DEBUG] Found {} proxies for partition {}", proxies.size(), par_id);

  vector<rusty::Arc<Future>> fus;
  // Fill-then-wrap: build the payload locally, wrap once complete.
  VecRecData key_batch;
  key_batch.key_data_ = std::make_shared<vector<key_t>>(keys.begin(), keys.end());
  janus::Command key_batch_md = rusty::Arc<VecRecData>::make(std::move(key_batch));
	WAN_WAIT;
  for (auto& p : proxies) {
    // TODO: Local call optimization temporarily commented out
    // if (p.first == loc_id) {
    //     // Local call - call OnJetpackPullCmd directly
    //     bool_t ok;
    //     epoch_t reply_jepoch, reply_oepoch;
    //     janus::Command reply_old_view, reply_new_view;
    //     auto cmd = std::make_shared<TpcCommitCommand>();
    //     dtxn_sched_->OnJetpackPullCmd(jepoch, oepoch, key, &ok, &reply_jepoch, &reply_oepoch, 
    //                                  &reply_old_view, &reply_new_view, cmd);
    //     janus::Command cmd_deputy;
    //     cmd_deputy = cmd;
    //     e->FeedResponse(ok, reply_jepoch, reply_oepoch, cmd_deputy);
    //     continue;
    // }
    auto proxy = (ClassicProxy*) p.second;
    // if (!proxy) {
    //   Log_info("[JETPACK-DEBUG] ERROR: Proxy is NULL for site {}!", p.first);
    // }
    // Log_info("[JETPACK-DEBUG] Sending JetpackPullCmd to site {}", p.first);
    
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      bool_t ok;
      epoch_t reply_jepoch, reply_oepoch;
      janus::Command reply_old_view, reply_new_view, cmd;
      rrr::deserialize_from(fu->get_reply(), ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, cmd);
      e->FeedResponse(ok, reply_jepoch, reply_oepoch, cmd);
    };

    // Log_info("[JETPACK-DEBUG] About to call async_JetpackPullCmd");
    ClassicProxy::RpcJetpackPullCmdRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    req.key_batch = key_batch_md;
    auto fu_result = proxy->async_JetpackPullCmd(req, fuattr);
    // if (!fu) {
    //   Log_info("[JETPACK-DEBUG] ERROR: async_JetpackPullCmd returned NULL Future!");
    // }
    // Log_info("[JETPACK-DEBUG] async_JetpackPullCmd returned Future, adding to list");
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  // Log_info("[JETPACK-DEBUG] JetpackBroadcastPullCmd returning event with {} futures", fus.size());
  return e;
}

rusty::Arc<QuorumEvent> Communicator::JetpackBroadcastRecordCmd(parid_t par_id, locid_t loc_id,
                                                               epoch_t jepoch, epoch_t oepoch,
                                                               int sid, int rid,
                                                               const std::vector<std::pair<key_t, janus::Command>>& cmds) {
  // Log_info("[JETPACK-DEBUG] JetpackBroadcastRecordCmd called: par_id={}, loc_id={}, sid={}, rid={}",
  //          par_id, loc_id, sid, rid);

  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = create_sp_quorum_event(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;

  // Fill-then-wrap: build the payload locally, wrap once complete.
  KeyCmdBatchData batch_data;
  for (const auto& entry : cmds) {
    batch_data.AddEntry(entry.first, entry.second);
  }
  janus::Command cmd_deputy = rusty::Arc<KeyCmdBatchData>::make(std::move(batch_data));
  
  // Log_info("[JETPACK-DEBUG] Broadcasting RecordCmd to {} sites, need {} votes", proxies.size(), n/2+1);
  
  for (auto& p : proxies) {
    // TODO: Local call optimization temporarily commented out
    // if (p.first == loc_id) {
    //     // Local call - call OnJetpackRecordCmd directly
    //     dtxn_sched_->OnJetpackRecordCmd(jepoch, oepoch, sid, rid, cmd);
    //     e->vote_yes();
    //     continue;
    // }
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e, p](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        // Log_info("[JETPACK-DEBUG] RecordCmd error from site {}: error_code={}",
        //          p.first, fu->get_error_code());
        e->vote_no();  // Vote no on error to prevent hanging
        return;
      }
      // Log_info("[JETPACK-DEBUG] RecordCmd success from site {}", p.first);
      e->vote_yes();
    };
    // Log_info("[JETPACK-DEBUG] Sending RecordCmd to site {}", p.first);
    ClassicProxy::RpcJetpackRecordCmdRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    req.sid = sid;
    req.rid = rid;
    req.cmd_batch = cmd_deputy;
    auto fu_result = proxy->async_JetpackRecordCmd(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

shared_ptr<JetpackPrepareQuorumEvent> Communicator::JetpackBroadcastPrepare(parid_t par_id, locid_t loc_id, 
                                                                      epoch_t jepoch, epoch_t oepoch, 
                                                                      ballot_t max_seen_ballot) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = std::make_shared<JetpackPrepareQuorumEvent>(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    // TODO: Local call optimization temporarily commented out
    // if (p.first == loc_id) {
    //     // Local call - call OnJetpackPrepare directly
    //     bool_t ok;
    //     epoch_t reply_jepoch, reply_oepoch;
    //     janus::Command reply_old_view, reply_new_view;
    //     ballot_t accepted_ballot;
    //     int replied_sid, replied_set_size;
    //     dtxn_sched_->OnJetpackPrepare(jepoch, oepoch, max_seen_ballot, &ok, &reply_jepoch, &reply_oepoch,
    //                                  &reply_old_view, &reply_new_view, &accepted_ballot, &replied_sid, &replied_set_size);
    //     e->FeedResponse(ok, reply_jepoch, reply_oepoch, accepted_ballot, replied_sid, replied_set_size, max_seen_ballot);
    //     continue;
    // }
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      bool_t ok;
      epoch_t reply_jepoch, reply_oepoch;
      janus::Command reply_old_view, reply_new_view;
      ballot_t reply_max_seen_ballot;
      ballot_t accepted_ballot;
      int replied_sid, replied_set_size;
      rrr::deserialize_from(fu->get_reply(), ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, reply_max_seen_ballot, accepted_ballot, replied_sid, replied_set_size);
      e->FeedResponse(ok, reply_jepoch, reply_oepoch, accepted_ballot, replied_sid, replied_set_size, reply_max_seen_ballot);
    };
    ClassicProxy::RpcJetpackPrepareRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    req.max_seen_ballot = max_seen_ballot;
    auto fu_result = proxy->async_JetpackPrepare(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

shared_ptr<JetpackAcceptQuorumEvent> Communicator::JetpackBroadcastAccept(parid_t par_id, locid_t loc_id,
                                                                          epoch_t jepoch, epoch_t oepoch, 
                                                                          ballot_t max_seen_ballot, int sid, int set_size) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = std::make_shared<JetpackAcceptQuorumEvent>(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      bool_t ok;
      epoch_t reply_jepoch, reply_oepoch;
      janus::Command reply_old_view, reply_new_view;
      ballot_t reply_max_seen_ballot;
      rrr::deserialize_from(fu->get_reply(), ok);
      rrr::deserialize_from(fu->get_reply(), reply_jepoch);
      rrr::deserialize_from(fu->get_reply(), reply_oepoch);
      rrr::deserialize_from(fu->get_reply(), reply_old_view);
      rrr::deserialize_from(fu->get_reply(), reply_new_view);
      rrr::deserialize_from(fu->get_reply(), reply_max_seen_ballot);
      e->FeedResponse(ok, reply_jepoch, reply_oepoch, reply_max_seen_ballot);
    };
    ClassicProxy::RpcJetpackAcceptRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    req.max_seen_ballot = max_seen_ballot;
    req.sid = sid;
    req.set_size = set_size;
    auto fu_result = proxy->async_JetpackAccept(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

rusty::Arc<QuorumEvent> Communicator::JetpackBroadcastCommit(parid_t par_id, locid_t loc_id, epoch_t jepoch, epoch_t oepoch, int sid, int set_size) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = create_sp_quorum_event(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      e->vote_yes();
    };
    ClassicProxy::RpcJetpackCommitRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    req.sid = sid;
    req.set_size = set_size;
    auto fu_result = proxy->async_JetpackCommit(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

shared_ptr<JetpackPullRecSetInsQuorumEvent> Communicator::JetpackBroadcastPullRecSetIns(parid_t par_id, locid_t loc_id, epoch_t jepoch, epoch_t oepoch, int sid, int rid) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = std::make_shared<JetpackPullRecSetInsQuorumEvent>(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      bool_t ok;
      epoch_t reply_jepoch, reply_oepoch;
      janus::Command reply_old_view, reply_new_view, cmd;
      rrr::deserialize_from(fu->get_reply(), ok);
      rrr::deserialize_from(fu->get_reply(), reply_jepoch);
      rrr::deserialize_from(fu->get_reply(), reply_oepoch);
      rrr::deserialize_from(fu->get_reply(), reply_old_view);
      rrr::deserialize_from(fu->get_reply(), reply_new_view);
      rrr::deserialize_from(fu->get_reply(), cmd);
      e->FeedResponse(ok, reply_jepoch, reply_oepoch, cmd);
    };
    ClassicProxy::RpcJetpackPullRecSetInsRequest req;
    req.jepoch = jepoch;
    req.oepoch = oepoch;
    req.sid = sid;
    req.rid = rid;
    auto fu_result = proxy->async_JetpackPullRecSetIns(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

rusty::Arc<QuorumEvent> Communicator::JetpackBroadcastFinishRecovery(parid_t par_id, locid_t loc_id, epoch_t oepoch) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = create_sp_quorum_event(n, n/2+1);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    auto proxy = (ClassicProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      e->vote_yes();
    };
    ClassicProxy::RpcJetpackFinishRecoveryRequest req;
    req.oepoch = oepoch;
    auto fu_result = proxy->async_JetpackFinishRecovery(req, fuattr);
    if (fu_result.is_ok()) {
      fus.push_back(fu_result.unwrap().raw_future());
    }
  }
  return e;
}

void Communicator::UpdatePartitionView(parid_t partition_id, const ViewData& view_data) {
  View view = view_data.view_;
  
  // Lock the mutex for thread-safe access
  std::lock_guard<std::mutex> lock(partition_views_mutex_);
  
  // Check if we have an existing view
  auto it = partition_views_.find(partition_id);
  if (it != partition_views_.end()) {
    const View& prev_view = it->second;
    Log_info("[VIEW_DEBUG] partition {} view update {} -> {}", partition_id,
             prev_view.ToString().c_str(), view.ToString().c_str());
  } else {
    Log_info("[VIEW_DEBUG] partition {} initial view {}", partition_id, view.ToString().c_str());
  }
  if (it != partition_views_.end()) {
    // Only update if the new view is newer
    if (view.timestamp_ > it->second.timestamp_) {
      partition_views_[partition_id] = view;
    }
  } else {
    // First view for this partition
    partition_views_[partition_id] = view;
  }
  
  // Note: We no longer update leader_cache_ here since each communicator instance
  // should look up the leader from the global partition_views_ when needed
}

View Communicator::GetPartitionView(parid_t partition_id) {
  std::lock_guard<std::mutex> lock(partition_views_mutex_);
  auto it = partition_views_.find(partition_id);
  if (it != partition_views_.end()) {
    return it->second;
  }
  // Return empty view if not found
  return View();
}

locid_t Communicator::GetLeaderForPartition(parid_t partition_id) {
  View view = GetPartitionView(partition_id);
  
  if (!view.IsEmpty()) {
    int leader = view.GetLeader();
    if (leader >= 0) {
      return leader;
    }
  }
  
  // Fall back to static leader if no view or invalid leader
  return 0;
}

} // namespace janus
