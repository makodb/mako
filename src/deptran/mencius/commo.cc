
#include "commo.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "../procedure.h"
#include "../command_marshaler.h"
#include "../rcc_rpc.h"
#include "server_worker.h"
#include "server.h"
#include <mutex>

namespace janus {

MenciusCommo::MenciusCommo(rusty::Option<rusty::Arc<PollThread>> poll) : Communicator(poll) {
//  verify(poll != nullptr);
}

void MenciusCommo::BroadcastPrepare(parid_t par_id,
                                       slotid_t slot_id,
                                       ballot_t ballot,
                                       const function<void(rusty::Arc<Future>)>& cb) {
  verify(0); // deprecated function
  auto proxies = rpc_par_proxies_[par_id];
  auto leader_id = LeaderProxyForPartition(par_id).first;

  WAN_WAIT
  for (auto& p : proxies) {
    auto proxy = (MenciusProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = cb;
    Future::safe_release(proxy->async_Prepare(slot_id, ballot, fuattr));
  }
}

shared_ptr<MenciusPrepareQuorumEvent>
MenciusCommo::BroadcastPrepare(parid_t par_id,
                                  slotid_t slot_id,
                                  ballot_t ballot) {
  verify(0);
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<MenciusPrepareQuorumEvent>(n, n/2+1);
  auto src_coroid = e->GetCoroId();
  auto proxies = rpc_par_proxies_[par_id];
  auto leader_id = LeaderProxyForPartition(par_id).first;

  WAN_WAIT
  for (auto& p : proxies) {
    auto proxy = (MenciusProxy*) p.second;
    auto follower_id = p.first;
    // e->add_dep(leader_id, src_coroid, follower_id, -1);

    FutureAttr fuattr;
    fuattr.callback = [e, ballot, leader_id, src_coroid, follower_id](rusty::Arc<Future> fu) {
      if (fu->get_error_code() != 0) {
        Log_info("Get a error message in reply");
        return;
      }
      ballot_t b = 0;
      uint64_t coro_id = 0;
      fu->get_reply() >> b >> coro_id;
      e->FeedResponse(b==ballot);
      // e->deps[leader_id][src_coroid][follower_id].erase(-1);
      // e->deps[leader_id][src_coroid][follower_id].insert(coro_id);
      // TODO add max accepted value.
    };
    Future::safe_release(proxy->async_Prepare(slot_id, ballot, fuattr));
  }
  return e;
}

shared_ptr<MenciusSuggestQuorumEvent>
MenciusCommo::BroadcastSuggest(parid_t par_id,
                                 slotid_t slot_id,
                                 ballot_t ballot,
                                 shared_ptr<Marshallable> cmd) {
  //Log_info("invoke BroadcastSuggest, slot_id:%d", slot_id);
  int n = Config::GetConfig()->GetPartitionSize(par_id);
  auto e = Reactor::CreateSpEvent<MenciusSuggestQuorumEvent>(n, n/2+1);
//  auto e = Reactor::CreateSpEvent<MenciusSuggestQuorumEvent>(n, n);

  auto src_coroid = e->GetCoroId();
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
      fu->get_reply() >> b >> coro_id;
      e->FeedResponse(b==ballot);
      // auto end = chrono::system_clock::now();
      // auto duration = chrono::duration_cast<chrono::microseconds>(end-start2).count();
      //Log_info("The duration of Suggest() for %d is: %d", follower_id, duration); // 20029
      // e->deps[leader_id][src_coroid][follower_id].erase(-1);
      // e->deps[leader_id][src_coroid][follower_id].insert(coro_id);
    };
    MarshallDeputy md(cmd);
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
    auto f = proxy->async_Suggest(slot_id, start_, ballot, sender, skip_commits, skip_potentials, md, fuattr);
    auto end1 = chrono::system_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end1-start1).count();
    Future::safe_release(f);
  }
  return e;
}

void MenciusCommo::BroadcastSuggest(parid_t par_id,
                                      slotid_t slot_id,
                                      ballot_t ballot,
                                      shared_ptr<Marshallable> cmd,
                                      const function<void(rusty::Arc<Future>)>& cb) {
  verify(0); // deprecated function
  // auto proxies = rpc_par_proxies_[par_id];
  // auto leader_id = LeaderProxyForPartition(par_id).first;
  // vector<Future*> fus;
  // for (auto& p : proxies) {
  //   auto proxy = (MenciusProxy*) p.second;
  //   FutureAttr fuattr;
  //   fuattr.callback = cb;
  //   MarshallDeputy md(cmd);
  //   uint64_t time = 0; // compiles the code
  //   std::vector<uint64_t> skip_commits(1);
  //   skip_commits.push_back(100);
  //   std::vector<uint64_t> skip_potentials(1);
  //   skip_potentials.push_back(200);
  //   auto f = proxy->async_Suggest(slot_id, time,ballot, skip_commits, skip_potentials, md, fuattr);
  //   Future::safe_release(f);
  // }
}

void MenciusCommo::BroadcastDecide(const parid_t par_id,
                                      const slotid_t slot_id,
                                      const ballot_t ballot,
                                      const shared_ptr<Marshallable> cmd) {
  //Log_info("invoke BroadcastDecide, slot_id:%d", slot_id);
  auto proxies = rpc_par_proxies_[par_id];
  int n = proxies.size();
  auto leader_id = LeaderProxyForPartition(par_id, (slot_id-1)%n).first;
  vector<rusty::Arc<Future>> fus;

  WAN_WAIT
  for (auto& p : proxies) {
    auto proxy = (MenciusProxy*) p.second;
    FutureAttr fuattr;
    fuattr.callback = [](rusty::Arc<Future> fu) {};
    MarshallDeputy md(cmd);
    auto f = proxy->async_Decide(slot_id, ballot, md, fuattr);
    //sp_quorum_event->add_dep(leader_id, p.first);
    Future::safe_release(f);
  }
}

} // namespace janus
