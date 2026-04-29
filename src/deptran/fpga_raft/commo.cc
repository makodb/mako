
#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"

namespace janus {

FpgaRaftCommo::FpgaRaftCommo(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker) : Communicator(std::move(poll_thread_worker)) {
//  verify(poll != nullptr);
}

shared_ptr<FpgaRaftForwardQuorumEvent> FpgaRaftCommo::SendForward(parid_t par_id, 
                                            parid_t self_id, shared_ptr<Marshallable> cmd)
{
    int n = Config::GetConfig()->GetPartitionSize(par_id);
    auto e = Reactor::create_sp_event<FpgaRaftForwardQuorumEvent>(1,1);
    parid_t fid = (self_id + 1 ) % n ;
    if (fid != self_id + 1 )
    {
      // sleep for 2 seconds cos no leader
      int32_t timeout = 2*1000*1000 ;
      auto sp_e = Reactor::create_sp_event<TimeoutEvent>(timeout);
      sp_e->wait();    
    }
    auto proxies = rpc_par_proxies_[par_id];
    WAN_WAIT;
    auto proxy = (FpgaRaftProxy*) proxies[fid].second ;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      uint64_t cmt_idx = 0;
      fu->get_reply() >> cmt_idx;
      e->FeedResponse(cmt_idx);
    };    
    MarshallDeputy md(cmd);
    FpgaRaftProxy::RpcForwardRequest req{};
    req.cmd = md;
    auto f = proxy->async_Forward(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
    return e;
}

void FpgaRaftCommo::BroadcastHeartbeat(parid_t par_id,
																			 uint64_t logIndex) {
	//Log_info("heartbeat for log index: %d", logIndex);
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
  for (auto& p : proxies) {
    if (p.first == this->loc_id_)
        continue;
		auto follower_id = p.first;
    auto proxy = (FpgaRaftProxy*) p.second;
    FutureAttr fuattr;
    
		fuattr.callback = [this, follower_id, logIndex] (rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      uint64_t index = 0;
			
      fu->get_reply() >> index;
			this->matchedIndex[follower_id] = index;
			
			//Log_info("follower_index for %d: %d and leader_index: %d", follower_id, index, logIndex);
			
    };

		DepId di;
		di.str = "hb";
		di.id = -1;
    FpgaRaftProxy::RpcHeartbeatRequest req{};
    req.leaderPrevLogIndex = logIndex;
    req.dep_id = di;
    auto f = proxy->async_Heartbeat(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
}

void FpgaRaftCommo::SendHeartbeat(parid_t par_id,
																	siteid_t site_id,
																  uint64_t logIndex) {
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	// WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first != site_id)
        continue;
		auto follower_id = p.first;
    auto proxy = (FpgaRaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](rusty::Arc<Future> fu) {};
    
		DepId di;
		di.str = "dep";
		di.id = -1;
		
		//Log_info("heartbeat2 for log index: %d", logIndex);
    FpgaRaftProxy::RpcHeartbeatRequest req{};
    req.leaderPrevLogIndex = logIndex;
    req.dep_id = di;
    auto f = proxy->async_Heartbeat(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
}

void FpgaRaftCommo::SendAppendEntriesAgain(siteid_t site_id,
																					 parid_t par_id,
																					 slotid_t slot_id,
																					 ballot_t ballot,
																					 bool isLeader,
																					 uint64_t currentTerm,
																					 uint64_t prevLogIndex,
																					 uint64_t prevLogTerm,
																					 uint64_t commitIndex,
																					 shared_ptr<Marshallable> cmd) {
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
	WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first != site_id)
        continue;
		auto follower_id = p.first;
    auto proxy = (FpgaRaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](rusty::Arc<Future> fu) {};

		MarshallDeputy md(cmd);
		verify(md.inner() != nullptr);

		DepId di;
		di.str = "dep";
		di.id = -1;

		Log_info("heartbeat2 for log index: %d", prevLogIndex);
    FpgaRaftProxy::RpcAppendEntriesRequest req{};
    req.slot = slot_id;
    req.ballot = ballot;
    req.leaderCurrentTerm = currentTerm;
    req.leaderPrevLogIndex = prevLogIndex;
    req.leaderPrevLogTerm = prevLogTerm;
    req.leaderCommitIndex = commitIndex;
    req.dep_id = di;
    req.cmd = md;
    auto f = proxy->async_AppendEntries(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }

}

shared_ptr<FpgaRaftAppendQuorumEvent>
FpgaRaftCommo::BroadcastAppendEntries(parid_t par_id,
                                      siteid_t leader_site_id,
                                      slotid_t slot_id,
                                      i64 dep_id,
                                      ballot_t ballot,
                                      bool isLeader,
                                      uint64_t currentTerm,
                                      uint64_t prevLogIndex,
                                      uint64_t prevLogTerm,
                                      uint64_t commitIndex,
                                      shared_ptr<Marshallable> cmd) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::create_sp_event<FpgaRaftAppendQuorumEvent>(n, n/2 + 1);
  auto proxies = rpc_par_proxies_[par_id];

  unordered_set<std::string> ip_addrs {};
  std::vector<std::shared_ptr<rrr::Client>> clients;

  vector<rusty::Arc<Future>> fus;
  WAN_WAIT;

  for (auto& p : proxies) {
    auto id = p.first;
    auto proxy = (FpgaRaftProxy*) p.second;
    auto cli_it = rpc_clients_.find(id);
    std::string ip = "";
    if (cli_it != rpc_clients_.end()) {
      ip = cli_it->second->host();
			//cli = cli_it->second;
    }
    ip_addrs.insert(ip);
		//clients.push_back(cli);
  }
  //e->clients_ = clients;
  
  for (auto& p : proxies) {
    auto follower_id = p.first;
    auto proxy = (FpgaRaftProxy*) p.second;
    auto cli_it = rpc_clients_.find(follower_id);
    std::string ip = "";
    if (cli_it != rpc_clients_.end()) {
      ip = cli_it->second->host();
    }
	if (p.first == leader_site_id) {
        // fix the 1c1s1p bug
        // Log_info("leader_site_id %d", leader_site_id);
        e->FeedResponse(true, prevLogIndex + 1, ip);
        continue;
    }
    FutureAttr fuattr;
    struct timespec begin;
    clock_gettime(CLOCK_MONOTONIC, &begin);

    fuattr.callback = [this, e, isLeader, currentTerm, follower_id, n, ip, begin] (rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      uint64_t accept = 0;
      uint64_t term = 0;
      uint64_t index = 0;
			
			fu->get_reply() >> accept;
      fu->get_reply() >> term;
      fu->get_reply() >> index;
			
			struct timespec end;
			//clock_gettime(CLOCK_MONOTONIC, &begin);
			this->outbound--;
			//Log_info("reply from server: %s and is_ready: %d", ip.c_str(), e->is_ready());
			clock_gettime(CLOCK_MONOTONIC, &end);
			//Log_info("time of reply on server %d: %ld", follower_id, (end.tv_sec - begin.tv_sec)*1000000000 + end.tv_nsec - begin.tv_nsec);
			
      bool y = ((accept == 1) && (isLeader) && (currentTerm == term));
      e->FeedResponse(y, index, ip);
    };
    MarshallDeputy md(cmd);
		verify(md.inner() != nullptr);
		outbound++;
		DepId di;
		di.str = "dep";
		di.id = dep_id;
    FpgaRaftProxy::RpcAppendEntriesRequest req{};
    req.slot = slot_id;
    req.ballot = ballot;
    req.leaderCurrentTerm = currentTerm;
    req.leaderPrevLogIndex = prevLogIndex;
    req.leaderPrevLogTerm = prevLogTerm;
    req.leaderCommitIndex = commitIndex;
    req.dep_id = di;
    req.cmd = md;
    auto f = proxy->async_AppendEntries(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
  verify(!e->is_ready());
  return e;
}

// Workstream N Phase 4e-11: removed deprecated callback-style
// `void FpgaRaftCommo::BroadcastAppendEntries(... callback)` — body
// had `verify(0); // deprecated function` and no callers.

void FpgaRaftCommo::BroadcastDecide(const parid_t par_id,
                                      const slotid_t slot_id,
																			const i64 dep_id,
                                      const ballot_t ballot,
                                      const shared_ptr<Marshallable> cmd) {
  auto proxies = rpc_par_proxies_[par_id];
  vector<rusty::Arc<Future>> fus;
  for (auto& p : proxies) {
    auto proxy = (FpgaRaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](rusty::Arc<Future> fu) {};
    MarshallDeputy md(cmd);
		DepId di;
		di.str = "dep";
		di.id = dep_id;
    FpgaRaftProxy::RpcDecideRequest req{};
    req.slot = slot_id;
    req.ballot = ballot;
    req.dep_id = di;
    req.cmd = md;
    auto f = proxy->async_Decide(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
}

// Workstream N Phase 4e-11: removed deprecated callback-style
// `void FpgaRaftCommo::BroadcastVote(... callback)` — body had
// `verify(0); // deprecated function` and no callers.

shared_ptr<FpgaRaftVoteQuorumEvent>
FpgaRaftCommo::BroadcastVote(parid_t par_id,
                                    slotid_t lst_log_idx,
                                    ballot_t lst_log_term,
                                    parid_t self_id,
                                    ballot_t cur_term ) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::create_sp_event<FpgaRaftVoteQuorumEvent>(n, n/2);
  auto proxies = rpc_par_proxies_[par_id];
  WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first == this->loc_id_)
        continue;
    auto proxy = (FpgaRaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      ballot_t term = 0;
      bool_t vote = false ;
      fu->get_reply() >> term;
      fu->get_reply() >> vote ;
      e->FeedResponse(vote, term);
      // TODO add max accepted value.
    };
    FpgaRaftProxy::RpcVoteRequest req{};
    req.lst_log_idx = lst_log_idx;
    req.lst_log_term = lst_log_term;
    req.par_id = self_id;
    req.cur_term = cur_term;
    auto f = proxy->async_Vote(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
  return e;
}

// Workstream N Phase 4e-11: removed deprecated callback-style
// `void FpgaRaftCommo::BroadcastVote2FPGA(... callback)` — body had
// `verify(0); // deprecated function` and no callers.

shared_ptr<FpgaRaftVote2FPGAQuorumEvent>
FpgaRaftCommo::BroadcastVote2FPGA(parid_t par_id,
                                    slotid_t lst_log_idx,
                                    ballot_t lst_log_term,
                                    parid_t self_id,
                                    ballot_t cur_term ) {
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::create_sp_event<FpgaRaftVote2FPGAQuorumEvent>(n, n/2);
  auto proxies = rpc_par_proxies_[par_id];
  WAN_WAIT;
  for (auto& p : proxies) {
    if (p.first == this->loc_id_)
        continue;
    auto proxy = (FpgaRaftProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [e](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      ballot_t term = 0;
      bool_t vote = false ;
      fu->get_reply() >> term;
      fu->get_reply() >> vote ;
      e->FeedResponse(vote, term);
    };
    FpgaRaftProxy::RpcVoteRequest req{};
    req.lst_log_idx = lst_log_idx;
    req.lst_log_term = lst_log_term;
    req.par_id = self_id;
    req.cur_term = cur_term;
    auto f = proxy->async_Vote(req, fuattr);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
  return e;
}



} // namespace janus
