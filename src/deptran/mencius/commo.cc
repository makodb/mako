
#include <stdint.h>
#include <time.h>

#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include "server_worker.h"
#include "server.h"

import std;

namespace janus {

MenciusCommo::MenciusCommo(rusty::Option<rusty::Arc<PollThread>> poll) : Communicator(std::move(poll)) {
//  verify(poll != nullptr);
}

// removed deprecated callback-style
// `void MenciusCommo::BroadcastPrepare(parid_t, slotid_t, ballot_t,
// callback)`.  See companion comment in commo.h.

// removed `MenciusCommo::BroadcastPrepare`
// (parid, slot, ballot) — body was a `verify(0);` shell.  Only call
// site was the now-deleted `CoordinatorMencius::Prepare()`.

shared_ptr<MenciusSuggestQuorumEvent>
MenciusCommo::BroadcastSuggest(parid_t par_id,
                                 slotid_t slot_id,
                                 ballot_t ballot,
                                 const janus::Command& cmd) {
  //Log_info("invoke BroadcastSuggest, slot_id:{}", slot_id);
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = std::make_shared<MenciusSuggestQuorumEvent>(n, n/2+1);
//  auto e = Reactor::create_sp_event<MenciusSuggestQuorumEvent>(n, n);

  auto src_coroid = e->get_fiber_id();
  auto proxies = rpc_par_proxies_[par_id];
  auto leader_id = LeaderProxyForPartition(par_id, (slot_id-1)%n).first;
  vector<rusty::Arc<Future>> fus;
  auto start = chrono::system_clock::now();

  std::vector<ServerWorker>* svr_workers = static_cast<std::vector<ServerWorker>*>(svr_workers_g);
  auto ms = dynamic_cast<MenciusServer*>(svr_workers->at((slot_id-1)%(svr_workers->size())).rep_sched_);
  auto skip_potentials_recd = ms->skip_potentials_recd;
  auto logs_ = ms->logs_;

  // from skip_potentials_recd (received by ServerWorker) to compute the committed SKIP entries (as well alpha)
  std::vector<uint64_t> skip_commits;
  ms->g_mutex.lock();
  {
    int id = ms->max_executed_slot_ + 1;
    while (true) {
      int cnt = 0;
      if ((id-1)%n==(slot_id-1)%n){
        for (int i=0;i<n;i++){
          if(ms->skip_potentials_recd.find(i)!=ms->skip_potentials_recd.end() 
              && ms->skip_potentials_recd.at(i).find(id)!=ms->skip_potentials_recd.at(i).end())
            cnt++;
        }
        if (cnt>=(n/2+1)){
          skip_commits.push_back(id);
        }else{
          break;
        }
      }else{
        id+=1;
      }
    }
  }
  ms->g_mutex.unlock();
  // the customized alpha
  int alpha = 10;
  if (skip_commits.size()<alpha){
    skip_commits.clear();
  }

  // from logs_ to compute potential SKIP entries => skip_potentials
  std::vector<uint64_t> skip_potentials;
  ms->g_mutex.lock();
  {
    for (slotid_t id = ms->max_executed_slot_ + 1; id <= ms->max_committed_slot_; id++) {
      auto& sp_instance = logs_[id];
      if(!sp_instance){ // not committed yet
        skip_potentials.push_back(id);
      }
    }
  }
  ms->g_mutex.unlock();

  WAN_WAIT
  for (auto& p : proxies) {
    auto proxy = (MenciusProxy*) p.second;
    auto follower_id = p.first;

    if (p.first == loc_id_) {
        auto start_ = 0;
        uint64_t sender = loc_id_;
        
        ballot_t b = 0;
        uint64_t coro_id = 0;

        static_cast<MenciusServer *>(rep_sched_)->OnSuggest(
          slot_id, start_, ballot, sender, skip_commits, skip_potentials, cmd, &b, &coro_id, nullptr);

        e->FeedResponse(b==ballot);
        continue;
    }

    // e->add_dep(leader_id, src_coroid, follower_id, -1);

    FutureAttr fuattr;
    // auto start2 = chrono::system_clock::now();
    auto start2 = 0;
    fuattr.callback = [e, start2, ballot, leader_id, src_coroid, follower_id] (rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      ballot_t b = 0;
      uint64_t coro_id = 0;
      rrr::deserialize_from(fu->get_reply(), b, coro_id);
      e->FeedResponse(b==ballot);
      // auto end = chrono::system_clock::now();
      // auto duration = chrono::duration_cast<chrono::microseconds>(end-start2).count();
      //Log_info("The duration of Suggest() for {} is: {}", follower_id, duration); // 20029
      // e->deps[leader_id][src_coroid][follower_id].erase(-1);
      // e->deps[leader_id][src_coroid][follower_id].insert(coro_id);
    };
    auto start1 = chrono::system_clock::now();
    uint64_t sender = loc_id_;
    // time_t tstart = chrono::system_clock::to_time_t(start);
    // tm * date = localtime(&tstart);
    // date->tm_hour = 0;
    // date->tm_min = 0;
    // date->tm_sec = 0;
    // auto midn = std::chrono::system_clock::from_time_t(std::mktime(date));

    // auto hours = chrono::duration_cast<chrono::hours>(start-midn);
    // auto minutes = chrono::duration_cast<chrono::minutes>(start-midn);

    // auto start_ = chrono::duration_cast<chrono::microseconds>(start-midn-hours-minutes).count();
    auto start_ = 0;
    MenciusProxy::RpcSuggestRequest req{};
    req.slot = slot_id;
    req.time = start_;
    req.ballot = ballot;
    req.sender = sender;
    req.skip_commits = skip_commits;
    req.skip_potentials = skip_potentials;
    req.cmd = cmd;
    auto f = proxy->async_Suggest(req, fuattr);
    auto end1 = chrono::system_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end1-start1).count();
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
  return e;
}

// removed deprecated callback-style
// `void MenciusCommo::BroadcastSuggest(parid_t, slotid_t, ballot_t,
// cmd, callback)`.  Body had `verify(0);` and was mostly already
// commented out.  See companion comment in commo.h.

void MenciusCommo::BroadcastDecide(const parid_t par_id,
                                      const slotid_t slot_id,
                                      const ballot_t ballot,
                                      const janus::Command& cmd) {
  //Log_info("invoke BroadcastDecide, slot_id:{}", slot_id);
  auto proxies = rpc_par_proxies_[par_id];
  int n = proxies.size();
  auto leader_id = LeaderProxyForPartition(par_id, (slot_id-1)%n).first;
  vector<rusty::Arc<Future>> fus;

  WAN_WAIT
  for (auto& p : proxies) {
    auto proxy = (MenciusProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](rusty::Arc<Future> fu) {};
    MenciusProxy::RpcDecideRequest req{};
    req.slot = slot_id;
    req.ballot = ballot;
    req.cmd = cmd;
    auto f = proxy->async_Decide(req, fuattr);
    //sp_quorum_event->add_dep(leader_id, p.first);
    if (f.is_ok()) {
      Future::safe_release(f.unwrap().raw_future());
    }
  }
}

} // namespace janus
