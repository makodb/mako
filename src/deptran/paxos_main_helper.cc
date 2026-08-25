
#include <rusty/thread.hpp>
#include "__dep__.h"
#include "frame.h"
#include "paxos_worker.h"
#include "client_worker.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "benchmark_control_rpc.h"
#include "server_worker.h"
#include "concurrentqueue.h"
#include "sys/time.h"
#ifdef CPU_PROFILE
# include <gperftools/profiler.h>
#endif // ifdef CPU_PROFILE
#include "config.h"
#include "s_main.h"
#include "paxos/server.h"
#include "network_client/network_impl.h"
#include <time.h>

import std;

using namespace janus;
using namespace network_client;

// ============================================================================
// Paxos Implementation Namespace
// ============================================================================
namespace paxos_impl {

// removed
//   `std::vector<shared_ptr<network_client::NetworkClientServiceImpl>>
//    nc_services = {};`
// — never populated anywhere (only `vector::push_back` etc. would
// add elements).  Reads at the now-deleted `nc_get_*_requests`
// getter functions accessed `nc_services[par_id]` which would have
// been UB on the empty vector.  The live `nc_setup_server` /
// `nc_start_server` create their own `NetworkClientServiceImpl`
// instances inside an `srpc::Server` and never touch this global.
// removed
//   `std::vector<shared_ptr<pthread_t>> nc_service_pthreads = {};`
// — declared but never written or read anywhere in the codebase.
// end of network client

vector<unique_ptr<ClientWorker>> client_workers_g = {};
//vector<shared_ptr<PaxosWorker>> pxs_workers_g = {};
//static vector<shared_ptr<Coordinator>> bulk_coord_g = {};
//static vector<pair<string, pair<int,uint32_t>>> submit_loggers(10000000);
typedef std::chrono::high_resolution_clock::time_point tp;
typedef pair<const char*, pair<int,tp>> queue_entry;
typedef pair<const char*, pair<int,int>> queue_entry_par;
// removed
//   `static moodycamel::ConcurrentQueue<queue_entry_par> submit_queue;`
// — `add_log` enqueued into it but the only dequeue happened in
// the now-deleted `submit_logger` (which was itself only called from
// the dead `PollSubmitLog` thread function — no `pthread_create`
// for it survived the Phase 4e-16 cleanup).
// removed
//   `static std::queue<queue_entry_par> submit_queue_nc;`
// — only used inside the now-deleted `PollSubQNc` function.
// removed `static srpc::SpinLock l_;` —
// declared but no `lock()` / `unlock()` calls anywhere in the file
// or codebase.
// removed `static atomic<int> producer{0};`
// — only read inside the now-deleted `PollSubmitLog`'s
// `while(producer >= 0)` loop guard; no writers anywhere.
// 16 already removed the `consumer` half of
// the original `static atomic<int> producer{0}, consumer{0};`
// pair for the same reason.
static atomic<int> submit_tot{0};
// removed `pthread_t submit_poll_th_;` —
// referenced only in commented-out `pthread_create(...)` /
// `pthread_detach(...)` lines.
// removed `const int len = 5;` and
// `static std::map<std::string,long double> timer;`.  Both fed only
// the now-deleted `microbench_paxos` / `microbench_paxos_queue`
// driver functions and `add_time()` reporting helper.
function<void(int)> leader_callback_{};

std::map<int, std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)>> leader_replay_cb;
// std::map<int, std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)>> follower_replay_cb{};


shared_ptr<ElectionState> es = ElectionState::instance();
// removed `const bool is_datacenter_failure = false;`
// and `const bool is_fail_new_impl = true;` — both were hard-coded
// constants with no command-line / YAML override path, gating dead
// branches in `setup2()` (the `is_datacenter_failure` branch and its
// `heartbeatBackground2` / `heartbeatMonitor3` thread fns) and in
// `heartbeatMonitor2()` (the `else` branch of `if(is_fail_new_impl)`).
// All gated-off code removed alongside the constants.

int get_epoch(){
  int x;
  //pxs_workers_g.back()->election_state_lock.lock();
  x = pxs_workers_g.back()->cur_epoch;
  //pxs_workers_g.back()->election_state_lock.unlock();
  return x;
}

void set_epoch(int v) {
  auto x = get_epoch();
  if (v==-1) {
    es->set_epoch();
  } else {
    es->set_epoch(v);
  }
  for(int i = 0; i < pxs_workers_g.size(); i++){
    pxs_workers_g[i]->cur_epoch = es->get_epoch();
  }
}

void check_current_path() {
    auto path = std::filesystem::current_path();
    Log_info("PWD : {}", path.string().c_str());
}

void server_launch_worker(vector<Config::SiteInfo>& server_sites) {
    int i = 0;
    vector<std::thread> service_setup_ths;
    for (auto& site_info : server_sites) {
        int thread_index = i++;
        auto site_info_for_thread = site_info;
        service_setup_ths.push_back(std::thread([site_info_for_thread, thread_index]() mutable {
            Log_info("launching site: {:x}, bind address {}",
                     site_info_for_thread.id,
                     site_info_for_thread.GetBindAddress().c_str());
            auto& worker = pxs_workers_g[thread_index];
            worker->SetupService();
        }));
    }

    Log_info("waiting for server service setup threads.");
    for (auto& th : service_setup_ths) {
        th.join();
    }
    Log_info("done waiting for server service setup threads.");

    i = 0;
    vector<std::thread> commo_setup_ths;
    for (auto& site_info : server_sites) {
        int thread_index = i++;
        auto site_info_for_thread = site_info;
        commo_setup_ths.push_back(std::thread([site_info_for_thread, thread_index]() mutable {
            auto& worker = pxs_workers_g[thread_index];
            worker->SetupCommo();
            worker->InitQueueRead();
            Log_info("site {} launched!", (int)site_info_for_thread.id);
        }));
    }

    Log_info("waiting for server communicator setup threads.");
    for (auto& th : commo_setup_ths) {
        th.join();
    }
    Log_info("done waiting for server communicator setup threads.");

    for (auto& worker : pxs_workers_g) {
        // setup communication between controller script - log data collection on the run.py
        worker->SetupHeartbeat();
    }
    Log_info("server workers' communicators setup");
}

// removed `char* message[200];` global +
// `microbench_paxos()` driver — never called from production paths
// (only the now-deleted dispatcher in `replication_helper.cc`
// referenced it; nothing referenced the dispatcher either).
//
// removed long-commented-out
// `remove_from_submitq()` helper that called
// `submit_queue.try_dequeue` — `submit_queue` is gone.

void add_log_without_queue(const char* log, int len, uint32_t par_id){
  char* nlog = (char*)log;
  //Log_info("invoke add_log_without_queue:len:{}, par_id:{}",len,par_id);
  for (auto& worker : pxs_workers_g) {  // submit a transaction
    if (worker->site_info_->partition_id_ == par_id){
        // for the same partition, protect it with mutex
        std::unique_lock<std::mutex> lock(worker->condition_mutex);
        {
    	    worker->IncSubmit();
          worker->Submit(log,len, par_id);
          break;
        }
      }
    }
}


// removed dead `wait` / `count_free` /
// `submit_logger()` / `PollSubmitLog()` chain.  `PollSubmitLog`
// was a `pthread_create` worker entry point that polled
// `submit_queue` and called `submit_logger`, which in turn
// dequeued and routed to `add_log_without_queue`.  No surviving
// `pthread_create(..., PollSubmitLog, ...)` call site referenced
// it — Phase 4e-16's commented-out `// pthread_create(...,
// PollSubQNc, nullptr)` left both polling threads as orphans —
// so `PollSubmitLog` was never started.  Without a live thread
// dequeueing, `submit_queue.enqueue(...)` in `add_log` (now also
// removed) was leaking entries.

map<string, string> getHosts(std::string filename) {
    map<string, string> proc_host_map_;
    YAML::Node config = YAML::LoadFile(filename);

    if (config["host"]) {
        auto node = config["host"];
        for (auto it = node.begin(); it != node.end(); it++) {
            auto proc_name = it->first.as<string>();
            auto host_name = it->second.as<string>();
            proc_host_map_[proc_name] = host_name ;
        }
    } else {
        std::cout << "there is no host attribute in the XML: " << filename << std::endl;
        exit(1) ;
    }
    return proc_host_map_;
}

int get_outstanding_logs(uint32_t par_id) {
    for (auto& worker : pxs_workers_g) {
        if (worker->site_info_->partition_id_ == par_id){
            auto ps = dynamic_cast<PaxosServer*>(worker->rep_sched_);
            return (int)worker->n_tot - (int)ps->n_commit_ ;
        }
    }
    return -1;
}


std::vector<std::string> setup(int argc, char* argv[]) {
    vector<string> retVector;
    check_current_path();
    Log_info("starting process {}", getpid());

    int ret = Config::CreateConfig(argc, argv);
    if (ret != SUCCESS) {
        Log_fatal("Read config failed");
        return retVector;
    }

    auto server_infos = Config::GetConfig()->GetMyServers();
    Log_info("server_infos, number of sites: {}, proc_name: {}", server_infos.size(), Config::GetConfig()->proc_name_.c_str());
    for (int i = server_infos.size()-1; i >=0; i--) {
      retVector.push_back(Config::GetConfig()->SiteById(server_infos[i].id).name) ;
      PaxosWorker* worker = new PaxosWorker();
      pxs_workers_g.push_back(std::shared_ptr<PaxosWorker>(worker));
      pxs_workers_g.back()->site_info_ = const_cast<Config::SiteInfo*>(&(Config::GetConfig()->SiteById(server_infos[i].id)));
      Log_info("partition id of each Paxos group is {}, site-name: {}, site-id: {}", pxs_workers_g.back()->site_info_->partition_id_, server_infos[i].name.c_str(), server_infos[i].id);
      // setup frame and scheduler
      pxs_workers_g.back()->SetupBase();
    }
    reverse(pxs_workers_g.begin(), pxs_workers_g.end());
    es->machine_id = pxs_workers_g.back()->site_info_->locale_id;
    Log_info("running machine-id: {}", es->machine_id);
    return retVector;
}

int shutdown_paxos() {
    // kill the election thread
    es->running = false;

    // removed `timer` print loop — fed only
    // by the now-deleted `add_time()` helper.
    for (auto& worker : pxs_workers_g) {
        worker->WaitForShutdown();
    }
    Log_info("all server workers have shut down.");

    fflush(stderr);
    fflush(stdout);

    // Add a small delay to allow network operations to complete gracefully
    // This prevents crashes during cleanup when connections are still active
    usleep(100 * 1000); // 100ms delay

    for (auto& worker : pxs_workers_g) {
        worker->ShutDown();
    }

    // Another small delay before clearing workers
    usleep(50 * 1000); // 50ms delay

    pxs_workers_g.clear();

    RandomGenerator::destroy();
    Config::DestroyConfig();

    Log_info("exit process.");

    return 0;
}

void register_leader_election_callback(std::function<void(int)> cb){
  leader_callback_ = cb;
}

void register_for_follower(std::function<void(const char*, int)> cb, uint32_t par_id) {
    for (auto& worker : pxs_workers_g) {
        if (worker->IsPartition(par_id) && !worker->IsLeader(par_id)) {
            worker->register_apply_callback(cb);
        }
    }
}

void register_for_follower_par_id(std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
    for (auto& worker : pxs_workers_g) {
        if (worker->IsPartition(par_id) && !worker->IsLeader(par_id)) {
            worker->register_apply_callback_par_id(cb);
        }
    }
}

void register_for_follower_par_id_return(std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)> cb, 
                                                                             uint32_t par_id) {
    // follower_replay_cb[par_id] = cb;
    if(es->machine_id != 0){
      for (auto& worker : pxs_workers_g) {
        if(worker->IsPartition(par_id))
          worker->register_apply_callback_par_id_return(cb);
      }
    }
}

void register_for_leader(std::function<void(const char*, int)> cb, uint32_t par_id) {
    for (auto& worker : pxs_workers_g) {
        if (worker->IsLeader(par_id)) {
            worker->register_apply_callback(cb);
        }
    }
}

void register_for_leader_par_id(std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
    for (auto& worker : pxs_workers_g) {
        if (worker->IsLeader(par_id)) {
            worker->register_apply_callback_par_id(cb);
        }
    }
}

void register_for_leader_par_id_return(std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char *>> &)> cb, 
                                       uint32_t par_id) {
    leader_replay_cb[par_id] = cb;
    if(es->machine_id == 0){
      for (auto& worker : pxs_workers_g) {
        if(worker->IsPartition(par_id))
          worker->register_apply_callback_par_id_return(cb);
      }
    }
}

void submit(const char* log, int len, uint32_t par_id) {
    for (auto& worker : pxs_workers_g) {  // submit a transaction
        if (!worker->IsLeader(par_id)) continue;
        // removed
        // `verify(worker->submit_pool != nullptr);` — `submit_pool`
        // was always nullptr (assignment was commented out in
        // `SetupBase`), so this verify would have always fired had
        // this branch ever been reached.  Field + class deleted in
        // this phase.
        string log_str;
        std::copy(log, log + len, std::back_inserter(log_str));
        worker->IncSubmit();

        auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([&worker,log_str,len,par_id] () {
            worker->Submit(log_str.data(),len, par_id);
        }));
        auto arc_job_base = rusty::Arc<Job>(arc_job);
        worker->GetPollThread()->add(arc_job_base);
        submit_tot++;
    }
}
// removed `add_time(key, value, denom)`
// helper — the only call site was inside the also-deleted
// `microbench_paxos_queue` driver, and the `timer` map it accumulated
// into is gone (only reader was the `shutdown_paxos` print loop).
//
// removed dead `static tp firstTime;`,
// `static tp endTime;`, `static bool debug = false;` — declared
// but no production reader or writer anywhere.  The only `debug`
// reference was a `// marker:ansh for debug` line comment inside
// the also-dead `electionMonitor` (deleted alongside).
void add_log_to_nc(const char* log, int len, uint32_t par_id, int batch_size) {
  // Find the worker for this partition by iterating, don't assume index == partition_id
  for (auto& worker : pxs_workers_g) {
    if (worker->site_info_->partition_id_ != par_id) {
      continue;
    }

    // Check if this worker is the leader for this partition
    if(!worker->is_leader){
      if(es->machine_id != 0)
        Log_info("Did not find to be leader, len: {},par_id:{}",len,par_id);
      return;
    }

    // Submit the log
    add_log_without_queue((char*)log, len, par_id);
    return;
  }

  // If we get here, no worker found for this partition
  Log_error("add_log_to_nc: no worker found for par_id {}", par_id);
}

// removed `void* PollSubQNc(void*)` — body
// started with `Log_error("exit branch"); exit(1);`, making everything
// after unreachable.  The only reference to the function (a
// `pthread_create(..., PollSubQNc, ...)` at line 939) was already
// commented out.  `submit_queue_nc` and `submit_poll_th_` went away
// alongside.

// removed `createBulkPrepare(epoch, machine_id)`
// — only call site was inside the now-deleted `send_bulk_prep`.
// removed `createHeartBeat(epoch, machine_id)`
// — only call site was inside the deleted `electionMonitor` thread fn
//.  No surviving caller.

shared_ptr<SyncLogRequest> createSyncLog(int epoch, int machine_id){
  auto syncLog = make_shared<SyncLogRequest>();
  syncLog->leader_id = machine_id;
  syncLog->epoch = epoch;
  for(int i = 0; i < pxs_workers_g.size(); i++){
    auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    ps->mtx_.lock();
    slotid_t min_slot = ps->max_committed_slot_+1;
    ps->mtx_.unlock();
    syncLog->sync_commit_slot.push_back(min_slot);
  }
  return syncLog;
}

// removed `createSyncNoOpLog(epoch, machine_id)`
// — only call site was inside the now-deleted `send_no_ops_to_all_workers`
//.  No surviving caller; the matching
// `PaxosWorker::SendSyncNoOpLog` is also removed in this phase.

// removed `send_no_ops_to_all_workers(epoch)`
// — only call site was inside the now-deleted `stuff_todo_leader_election`.
// The commented-out `// send_no_ops_to_all_workers(epoch)` line in
// `stuff_todo_learner_upgrade` is unaffected.

/*
change state to 1,
set epochs for workers
send synch rpc to followers.
*/

void send_sync_logs(int epoch){
  auto pw = pxs_workers_g.back();
  auto syncLog = createSyncLog(epoch, es->machine_id);
  auto ess = es;
  auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([pw, syncLog, ess](){
  int val = pw->SendSyncLog(syncLog);
  if(val == -1){
    ess->stuff_after_election_cond_.notify_all();
  }
 }));
 auto arc_job_base = rusty::Arc<Job>(arc_job);
 pxs_workers_g.back()->GetPollThread()->add(arc_job_base);
 {
   std::unique_lock<std::mutex> lock(es->stuff_after_election_mutex_);
   es->stuff_after_election_cond_.wait(lock);
 }
}

void sync_callbacks_for_new_leader(){
  for(int i = 0; i < pxs_workers_g.size(); i++){
    auto pw = pxs_workers_g[i];
    int partition_id_ = pw->site_info_->partition_id_;
    pw->register_apply_callback_par_id_return(leader_replay_cb[partition_id_]);
  }
}

void send_no_ops_for_mark(int epoch){
  string log = "no-ops:" + to_string(epoch);
  for(int i = 0; i < pxs_workers_g.size(); i++){
    add_log_to_nc(log.c_str(), log.size(), i, 0);
  }
}

void upgrade_p1_to_leader() {
  Config::GetConfig()->UpgradeFromP1ToLeader();
  Log_info("upgrade_p1_to_leader invoke");
  // change the machine_id
  es->machine_id = 0;
  for(int i = 0; i < pxs_workers_g.size(); i++){
    pxs_workers_g[i]->election_state_lock.lock();
    pxs_workers_g[i]->cur_epoch = es->get_epoch();
    pxs_workers_g[i]->is_leader = 1;
    pxs_workers_g[i]->election_state_lock.unlock();
    auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    ps->mtx_.lock();
    ps->max_committed_slot_ = ps->max_committed_slot_learner_+100;
    ps->max_executed_slot_ = ps->max_committed_slot_;
    ps->cur_open_slot_ = ps->max_committed_slot_+1;
    ps->cur_epoch = es->get_epoch();
    ps->mtx_.unlock();
  }
  sync_callbacks_for_new_leader();
}

void stuff_todo_learner_upgrade(){
  Config::GetConfig()->UpgradeFromLearnerToLeader();
  es->state_lock();
  es->set_state(1);
  es->set_epoch(); // increase the epoch number for failover
  for(int i = 0; i < pxs_workers_g.size(); i++){
    pxs_workers_g[i]->election_state_lock.lock();
    pxs_workers_g[i]->cur_epoch = es->get_epoch();
    pxs_workers_g[i]->is_leader = 1;
    pxs_workers_g[i]->election_state_lock.unlock();
    auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    ps->mtx_.lock();
    ps->max_committed_slot_ = ps->max_committed_slot_learner_+100;
    ps->max_executed_slot_ = ps->max_committed_slot_;
    ps->cur_open_slot_ = ps->max_committed_slot_+1;
    ps->cur_epoch = es->get_epoch();
    ps->mtx_.unlock();
  }
  int epoch = es->get_epoch();
  es->state_unlock();
  send_sync_logs(epoch);
  //send_no_ops_to_all_workers(epoch);
  sync_callbacks_for_new_leader(); // switch from follower_callback_ to leader_callback_
  send_no_ops_for_mark(epoch);
  vector<thread> threads;
  //usleep(40*1000);
  for(int i=0; i<pxs_workers_g.size(); i++) {
    Log_info("wait for noops: {}", i);
    pxs_workers_g[i]->WaitForNoops();
    Log_info("wait for noops(DONE),par_id: {}", i);
  }
}

// removed `stuff_todo_leader_election()` —
// no caller anywhere; the `electionMonitor` thread fn
// was its only invoker, and the surviving cluster-internal
// `heartbeatMonitor2` path uses `stuff_todo_learner_upgrade` instead.
// removed `send_bulk_prep(send_epoch)` —
// also referenced only by the deleted `electionMonitor` thread fn.

// removed dead `void* electionMonitor(void*)`
// (~46 lines) and `void* heartbeatMonitor(void*)` (~29 lines)
// pthread thread functions.  Both were referenced only inside
// commented-out `// Pthread_create(...)` lines further down in
// `setup2()` (around line 910/913 of the pre-cleanup file); no
// surviving call site started either thread.  The live election
// path is `setup2()`'s `Pthread_create(..., heartbeatBackground{,2}, ...)`
// + `Pthread_create(..., heartbeatMonitor{2,3}, ...)` set, which
// stays.  Both deleted functions referenced live state
// (`es->state_lock()`, `send_bulk_prep`, `stuff_todo_leader_election`,
// `pxs_workers_g.back()->GetPollThread()`, `createHeartBeat`,
// `OneTimeJob`, `leader_callback_`), but nothing on the live path
// invokes them.

void* heartbeatBackground(void* arg) {
  auto poll_arc = PollThread::create();
  auto rpc_cli = srpc::Client::create(poll_arc);
  auto site_leader = Config::GetConfig()->LeaderSiteByPartitionId(0);
  // get the leader's host + port
  auto port = site_leader.port + PaxosWorker::CtrlPortDelta;
  std::string addr_port = site_leader.GetHostAddr(PaxosWorker::CtrlPortDelta);
  Log_info("start a heartbeatBackground, addr:{}",addr_port.c_str());
  while (rpc_cli->connect(reinterpret_cast<const int8_t*>(addr_port.c_str()), true)!=0) {
     usleep(100 * 1000); // retry to connect
  }

  // Arc::get() returns const T*, but proxy doesn't mutate client
  ServerControlProxy *client_proxy = new ServerControlProxy(const_cast<srpc::Client*>(rpc_cli.get()));
  while (es->running) {
    ServerControlProxy::RpcServerHeartBeatRequest req;
    auto connected = client_proxy->server_heart_beat(req);
    if (connected.is_ok()){
      es->set_heartbeat_seen();
    }
    std::this_thread::sleep_for(10ms); // CAN'T run it too fast, otherwise error!
  }
  Log_info("heartbeatBackground is ended!");
  return nullptr;
}

// removed `void* heartbeatBackground2(void*)`
// — referenced only inside the now-deleted `if (is_datacenter_failure)`
// branch of `setup2()` (`Pthread_create(..., heartbeatBackground2, ...)`)
// and the constant gating that branch was hard-coded `false`.

// learner maintains heartbeat with the leader (connect to the first PaxosWorker::SetupHeartbeat())
void* heartbeatMonitor2(void* arg) { // happens on the learner
  time_t st = time(NULL);
  std::this_thread::sleep_for(std::chrono::seconds(5)); // ensure heartbeatBackground get started

  while (es->running) {
    auto duration2 = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - es->heartbeat_seen);
    WAN_WAIT_TIME(5); // 5ms is far enough within the same datacenter, otherwise, several seconds across data-center
    auto xx1 = std::chrono::high_resolution_clock::now() ;
    if (duration2.count()/1000.0/1000.0 > 1000) { // timeout: 1s
     Log_info("the time for the heartbeat: {:f} ms", duration2.count()/1000.0/1000.0);
     time_t end = time (NULL);
     if (end - st > 35) {
       Log_info("Let's stop it automatically without failover!!!");
       std::quick_exit( EXIT_SUCCESS );
     }

     Log_info("trigger an new leader: {:f} ms, {} sec", duration2.count()/1000.0/1000.0, (int)(end - st));

     // collapsed `if (is_fail_new_impl) {...}
     // else {...}` (the constant was hard-coded `true`); the dead else
     // branch ran a stale 4-step `leader_callback_(0/2/3)` sequence
     // with chrono timing instrumentation.
     leader_callback_(0); // call register_leader_election_callback in dbtest.cc
     stuff_todo_learner_upgrade();
     leader_callback_(2);
     std::this_thread::sleep_for(std::chrono::seconds(100000));
     break;
    }
  }
  return nullptr;
}

// removed `void* heartbeatMonitor3(void*)`
// — paired with the deleted `heartbeatBackground2`; referenced only
// inside the same dead `if (is_datacenter_failure)` branch.

// to be called after setup 1; needed for multiprocess setup
int setup2(int action, int shardIndex){  // action == 0 is default, action == 1 is forced to be follower

  auto server_infos = Config::GetConfig()->GetMyServers();
  if (server_infos.size() > 0) {
    server_launch_worker(server_infos);
  }
  if(action == 0 && es->machine_id == 0){
    es->set_state(1);
    //es->set_epoch(2);
    es->set_leader(0);
    for(int i = 0; i < pxs_workers_g.size(); i++){
      pxs_workers_g[i]->is_leader = 1;
      //pxs_workers_g[i]->cur_epoch = 2;
    }
  } else{
    es->set_state(0);
    es->set_epoch(0);
    es->set_leader(0);
  }
  // collapsed `if (is_datacenter_failure)
  // {...} else {...}` to just the else-branch (the constant was
  // hard-coded `false`).  The dead if-branch launched
  // `heartbeatBackground2` / `heartbeatMonitor3`, both removed above.
  if (Config::GetConfig()->proc_name_.compare("learner")==0) {
    // Rust-idiomatic: the dropped JoinHandle detaches (std-faithful).
    (void)rusty::thread::spawn([]() { heartbeatBackground(nullptr); });

    (void)rusty::thread::spawn([]() { heartbeatMonitor2(nullptr); });
  }
  // 20 / 4e-19 / 4e-16: cleared a stale
  // commented-out block that referenced now-deleted thread entry
  // points (`PollSubQNc`, `electionMonitor`, `heartbeatMonitor`)
  // and their `Pthread_create` / `pthread_detach` lines.  The live
  // pthread starts (`heartbeatBackground` / `heartbeatMonitor2` above)
  // are unaffected.
  return 0;
}

void add_log(const char* log, int len, uint32_t par_id){
    //read_log(log, len, "silo");
    // removed the `chrono::high_resolution_clock`
    // start/end snapshots, the `paxos_entry = make_pair(...)` packing,
    // and `submit_queue.enqueue(paxos_entry)` — `submit_queue` was a
    // leaked write-only queue (no live dequeue thread; see the
    // file-scope retirement note above).  The `IncSubmit` accounting
    // on the leader worker stays.  The chrono snapshots fed only the
    // commented-out `add_time("enqueue_time", ...)` reporting line.
    for (auto& worker : pxs_workers_g) {
      if (!worker->IsLeader(par_id)) continue;
      worker->IncSubmit();
      break;
    }
    submit_tot++;
}


void worker_info_stats(size_t nthreads) {
    Log_info("# of paxos_workers is {}", pxs_workers_g.size());

    for (size_t par_id=0; par_id<nthreads; par_id++) {
      Log_info("par_id {}", par_id);
      size_t wIdx = 0;
      for (auto& worker : pxs_workers_g) {
          if (worker->IsLeader(par_id)) {
              Log_info("    work_index: {}, par_id: {} - IsLeader", wIdx, par_id);
          } else {
              Log_info("    work_index: {}, par_id: {} - Is not Leader", wIdx, par_id);
          };

          if (worker->IsPartition(par_id)) {
              Log_info("    work_index: {}, par_id: {} - IsPartition", wIdx, par_id);
          } else {
              Log_info("    work_index: {}, par_id: {} - Is not Partition", wIdx, par_id);
          };
          wIdx += 1 ;
      }
    }
}

void wait_for_submit(uint32_t par_id) {
    int total_submits = 0;
    //Log_info("The number of completed submits {}", (int)submit_queue.size_approx());
 
    for (auto& worker : pxs_workers_g) {
        if(!worker->IsPartition(par_id))
          continue;
        worker->election_state_lock.lock();
        if (!worker->is_leader){
          worker->election_state_lock.unlock();
          continue;
        }
        worker->election_state_lock.unlock();
        // removed commented-out
        // `//verify(worker->submit_pool != nullptr);` and
        // `//worker->submit_pool->wait_for_all();` — `submit_pool`
        // field is gone.
	      // dropped `replay_queue.size_approx()`
	      // from this Log_info — `replay_queue` field went away with
	      // the dead `AddReplayEntry` / `StartReplayRead` pair.
	      Log_info("The number of completed submits n_current: {} par_id: {} submit_tot: {}", (int)worker->n_current, par_id, (int)worker->n_tot);
        worker->WaitForSubmit();
        total_submits = worker->n_tot;
    }
    for (auto& worker : pxs_workers_g) {
        if (!worker->IsPartition(par_id)) continue;
	      // dropped `replay_queue.size_approx()`
	      // from this Log_info too.
	      Log_info("Par_id {} [partition], the number of completed submits {}", par_id, (int)worker->n_current);
        worker->n_tot = total_submits;
        worker->WaitForSubmit();
    }
}
void pre_shutdown_step(){
    Log_info("shutdown Server Control Service after task finish total submit {}", (int)submit_tot);
    for (auto& worker : pxs_workers_g) {
        if (worker->hb_rpc_server_ != nullptr) {
            worker->hb_rpc_server_->do_shutdown();
        }
    }
}

// removed `microbench_paxos_queue()` driver
// — never called from production paths (only the now-deleted
// dispatcher in `replication_helper.cc` referenced it; nothing
// referenced the dispatcher either).

// http://www.cse.cuhk.edu.hk/~ericlo/teaching/os/lab/9-PThread/Pass.html
struct args {
    int port;
    char* server_ip;
    int par_id;
};

static void
nc_pclock(char *msg, clockid_t cid)
{
    struct timespec ts;

    printf("%s", msg);
    if (clock_gettime(cid, &ts) == -1)
        std::cout << "clock_gettime error" << std::endl;
    printf("%4jd.%03ld\n", (intmax_t)ts.tv_sec, ts.tv_nsec / 1000000);
}

void *nc_start_server(void *input) {
    auto poll_arc = PollThread::create();
    srpc::Server *server = new srpc::Server(srpc::Server::new_(rusty::Some(poll_arc)));

    server->reg_service_typed(rusty::make_box<NetworkClientServiceImpl>());
    server->start(reinterpret_cast<const int8_t*>((std::string(((struct args*)input)->server_ip)+std::string(":")+std::to_string(((struct args*)input)->port)).c_str())  );
    // Service is now owned by server
    int c=0;
    while (1) {
      c++;
      sleep(1);
      if (c==40) break;

      // if (track_cputime) {
      //   clockid_t cid;
      //   int s = pthread_getcpuclockid(*ps, &cid);
      //   if (s != 0)
      //       std::cout << "error\n";
      //   nc_pclock("sub threads thread CPU time:   ", cid);
      // }
      
      /*
      std::cout << "received on par_id: " << std::to_string(((struct args*)input)->par_id) << "\n";
      std::cout << "  new_order_counter:" << impl->counter_new_order << "\n"
                << "  counter_payement:" << impl->counter_payement << "\n"
                << "  counter_delivery:" << impl->counter_delivery << "\n"
                << "  counter_order_status:" << impl->counter_order_status << "\n"
                << "  counter_stock_level:" << impl->counter_stock_level << "\n"
                << "  in total:" << (impl->counter_new_order+impl->counter_payement+impl->counter_delivery+impl->counter_order_status+impl->counter_stock_level) << "\n\n" ;
                */
    }
    return nullptr;
}

// setup nthreads servers
void nc_setup_server(int nthreads, std::string host) {
  // std::map<std::string, std::string> hosts = getHosts(filename) ;
  // (char*)hosts["localhost"]
  for (int i=0; i<nthreads; i++) {
    struct args *ps = (struct args *)malloc(sizeof(struct args));
    ps->port=10010+i;
    ps->server_ip=(char*)host.c_str();
    ps->par_id=i;
    pthread_t ph_s;
    pthread_create(&ph_s, NULL, nc_start_server, (void *)ps);
    pthread_detach(ph_s);
    usleep(10 * 1000); // wait for 10ms
  }
}

// removed seven `nc_get_*_requests` getter
// functions (~30 lines):
//   nc_get_new_order_requests, nc_get_payment_requests,
//   nc_get_delivery_requests, nc_get_order_status_requests,
//   nc_get_stock_level_requests, nc_get_read_requests,
//   nc_get_rmw_requests.
// All seven returned `&nc_services[par_id]->{varies}_requests`, but
// `nc_services` was never populated, so any call would have been
// UB on an empty vector.  The only external caller in `nc_main.cc`
// at line 381 was already a single-line `//` comment.  The
// matching dispatchers in `replication_helper.cc` and the seven
// declarations in each of `paxos_impl` / `raft_impl` / global
// namespaces in `replication_helper.h` (21 total) were removed
// alongside, plus the seven Raft-side `Log_warn`-only
// placeholders in `raft_main_helper.cc`.

}  // namespace paxos_impl
