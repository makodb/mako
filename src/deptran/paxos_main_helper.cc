
#include <rusty/thread.hpp>
#include "__dep__.h"
#include "frame.h"
#include "paxos_worker.h"
#include "benchmark_control_rpc.h"
#include "concurrentqueue.h"
#include "sys/time.h"
#ifdef CPU_PROFILE
# include <gperftools/profiler.h>
#endif // ifdef CPU_PROFILE
#include "config.h"
#include "s_main.h"
#include "paxos/server.h"
#include <time.h>

import std;

using namespace janus;

// ============================================================================
// Paxos Implementation Namespace
// ============================================================================
namespace paxos_impl {

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
        // Submit on the caller's thread: it can wait for a free accept
        // round, and rounds finish on the poll thread.
        worker->IncSubmit();
        worker->Submit(log, len, par_id);
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

// Run fn on a Paxos worker's poll thread and wait for it to finish.
static void run_on_poll_thread(const shared_ptr<PaxosWorker>& pw, std::function<void()> fn) {
  auto done = std::make_shared<std::pair<std::mutex, std::condition_variable>>();
  auto finished = std::make_shared<bool>(false);
  auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([fn, done, finished]() {
    fn();
    std::lock_guard<std::mutex> lock(done->first);
    *finished = true;
    done->second.notify_all();
  }));
  pw->GetPollThread()->add(rusty::Arc<Job>(arc_job));
  std::unique_lock<std::mutex> lock(done->first);
  done->second.wait(lock, [&] { return *finished; });
}

using SlotEntries = std::vector<std::pair<slotid_t, rusty::Arc<janus::Command>>>;

// Applies a learner's entries for partition i, in slot order, through the
// partition's replay callback on that partition's thread, where forwarded
// entries are applied. Slots it has applied already are skipped, since the
// replay callback is not idempotent. Returns how many it applied.
static size_t apply_on_learner(size_t i, const SlotEntries& entries) {
  auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
  size_t applied = 0;
  run_on_poll_thread(pxs_workers_g[i], [ps, &entries, &applied] {
    std::lock_guard<std::recursive_mutex> lock(ps->mtx_);
    for (auto& e : entries) {
      if (ps->LearnerHasApplied(e.first)) {
        continue;
      }
      ps->app_next_(static_cast<int>(e.first), *e.second);
      ps->NoteLearnerApplied(e.first);
      applied++;
    }
  });
  return applied;
}

// apply_on_learner for every partition's entries, all partitions at once.
// The takeover waits for this, and the other shards wait for the takeover.
static size_t apply_on_learner_all(const std::vector<SlotEntries>& per) {
  std::vector<size_t> applied(per.size(), 0);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < per.size(); i++) {
    if (per[i].empty()) continue;
    threads.emplace_back([i, &per, &applied] { applied[i] = apply_on_learner(i, per[i]); });
  }
  for (auto& t : threads) {
    t.join();
  }
  size_t total = 0;
  for (size_t a : applied) {
    total += a;
  }
  return total;
}

// A learner gets the log from p1, which forwards each entry as it applies
// it, so the learner holds only what p1 has applied and forwarded so far,
// minus any entry p1 committed before its connection to the learner was up
// (p1 skips those). When p1 runs behind (on a loaded host, by thousands of
// entries, including rows the TPC-C loader wrote), the learner would take
// over without data the acceptors had committed, and the takeover below
// moves its slots past that data, so it is never applied. Before leading,
// it therefore fetches from the acceptors every entry they committed from
// its first unapplied slot on, in replies of at most kCatchUpReplyBytes,
// and applies them (apply_on_learner_all). Replay writes a key only over an
// older version, so entries it already has, or entries applied out of
// order, leave the same state. Returns the
// highest committed slot seen per partition. If an acceptor rejects the
// request because it is in a later epoch, *higher_epoch is set to that
// epoch: another replica has taken over.
static std::vector<slotid_t> catch_up_learner(int epoch, ballot_t* higher_epoch) {
  constexpr int64_t kCatchUpReplyBytes = 16 * 1024 * 1024;
  const size_t n = pxs_workers_g.size();
  std::vector<slotid_t> next(n), high(n);
  for (size_t i = 0; i < n; i++) {
    auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    std::lock_guard<std::recursive_mutex> lock(ps->mtx_);
    high[i] = ps->max_committed_slot_learner_;
    next[i] = ps->learner_applied_prefix_ + 1;
    Log_info("learner catch-up: partition {} applied every slot up to {}, {} more runs up to {}",
             i, ps->learner_applied_prefix_, ps->learner_applied_runs_.size(), high[i]);
  }
  size_t applied = 0;
  int rounds = 0;
  for (;;) {
    auto req = make_shared<SyncLogRequest>();
    req->leader_id = es->machine_id;
    req->epoch = epoch;
    req->sync_commit_slot = next;
    req->max_reply_bytes = kCatchUpReplyBytes;
    map<pair<int, slotid_t>, rusty::Arc<janus::Command>> got;
    bool ok = false;
    ballot_t rejected = 0;
    run_on_poll_thread(pxs_workers_g.back(), [&] {
      ok = pxs_workers_g.back()->FetchCommitted(req, &got, &rejected);
    });
    if (!ok) {
      if (rejected > epoch) {
        *higher_epoch = rejected;
        Log_warn("learner catch-up: an acceptor is in epoch {}, past this learner's {}",
                 rejected, epoch);
      } else {
        Log_warn("learner catch-up: the acceptors did not answer; taking over with what was forwarded");
      }
      break;
    }
    // The map is ordered by (partition, slot), so each partition's entries
    // come out in slot order.
    std::vector<SlotEntries> per(n);
    for (auto& kv : got) {
      const size_t par = static_cast<size_t>(kv.first.first);
      if (par < n && kv.first.second >= next[par]) {
        per[par].emplace_back(kv.first.second, kv.second.clone());
      }
    }
    applied += apply_on_learner_all(per);
    bool progress = false;
    for (size_t i = 0; i < n; i++) {
      if (per[i].empty()) continue;
      next[i] = per[i].back().first + 1;
      high[i] = std::max(high[i], per[i].back().first);
      progress = true;
    }
    rounds++;
    if (!progress) break;
  }
  Log_info("learner catch-up: applied {} committed entries from the acceptors in {} rounds",
           applied, rounds);
  return high;
}

// A value the old leader got accepted by a quorum is chosen even if no
// replica heard its decide. The leader learns a slot is decided as soon as
// its own decide reaches it, and may then release the slot's transactions,
// while the acceptors still hold the value only as accepted; catch-up asks
// for committed entries only, and the takeover would give those slots to
// new entries. So, after catch-up, the learner asks both acceptors (with
// the leader gone, every accept quorum includes one of them) for every
// value they accepted past the committed slots and adopts, per slot, the
// one accepted at the highest ballot, as a Paxos prepare phase does. A
// value only one acceptor accepted may not have been chosen; choosing it
// now is safe. Each acceptor first promises lead_epoch, the epoch the
// learner will lead in, and rejects lower ballots from then on, so the old
// leader, if it was only paused, cannot get a value accepted after it was
// read here. Replies stop at kRecoveryReplyBytes, so a slot is settled
// only once both acceptors have reported every slot up to it. The adopted
// values are proposed again at the slots they were accepted at, which
// follow the committed ones without a gap: the leader sends each acceptor
// its accepts in slot order, so neither can hold a slot past one that
// both lack, unless a send failed outright. Should that happen, adoption
// stops at the gap and the slots past it go to new entries. Returns the
// adopted values per partition, in slot order; *higher_epoch as in
// catch_up_learner. *min_executed gets, per partition, the last slot the
// acceptor furthest behind has applied.
static std::vector<SlotEntries> recover_accepted(int epoch, ballot_t lead_epoch,
                                                 const std::vector<slotid_t>& committed_high,
                                                 std::vector<slotid_t>* min_executed,
                                                 ballot_t* higher_epoch) {
  constexpr int64_t kRecoveryReplyBytes = 16 * 1024 * 1024;
  constexpr ballot_t kCommitted = std::numeric_limits<ballot_t>::max();
  constexpr slotid_t kAll = std::numeric_limits<slotid_t>::max();
  const size_t n = pxs_workers_g.size();
  std::vector<std::map<slotid_t, std::pair<ballot_t, rusty::Arc<janus::Command>>>> best(n);
  std::vector<slotid_t> next(n);
  std::vector<bool> done(n, false);
  for (size_t i = 0; i < n; i++) {
    next[i] = committed_high[i] + 1;
  }
  for (;;) {
    auto req = make_shared<SyncLogRequest>();
    req->leader_id = es->machine_id;
    req->epoch = epoch;
    req->sync_commit_slot = next;
    req->max_reply_bytes = kRecoveryReplyBytes;
    req->include_accepted = 1;
    req->promise_epoch = lead_epoch;
    vector<rusty::Arc<SyncLogResponse>> responses;
    bool ok = false;
    ballot_t rejected = 0;
    run_on_poll_thread(pxs_workers_g.back(), [&] {
      ok = pxs_workers_g.back()->FetchSyncLog(req, &responses, &rejected);
    });
    if (!ok) {
      if (rejected > epoch) {
        *higher_epoch = rejected;
      } else {
        Log_warn("learner recovery: the acceptors did not answer; adopting only the slots settled so far");
      }
      break;
    }
    for (const auto& r : responses) {
      for (size_t i = 0; i < n && i < r->executed.size(); i++) {
        (*min_executed)[i] = std::min((*min_executed)[i], r->executed[i]);
      }
    }
    bool progress = false;
    for (size_t i = 0; i < n; i++) {
      if (done[i]) continue;
      struct Reported { slotid_t slot; ballot_t ballot; rusty::Arc<janus::Command> cmd; };
      std::vector<Reported> reported;
      slotid_t settled = kAll, reported_last = next[i] - 1;
      for (const auto& r : responses) {
        slotid_t last = next[i] - 1;
        auto take = [&](const vector<rusty::Arc<janus::Command>>& data, bool committed) {
          if (i >= data.size()) return;
          const auto bp = marshallable_cast<BulkPaxosCmd>(*data[i]);
          if (bp.is_none()) return;
          for (size_t k = 0; k < bp.unwrap()->slots.size(); k++) {
            const slotid_t slot = bp.unwrap()->slots[k];
            if (slot < next[i]) continue;
            last = std::max(last, slot);
            reported.push_back({slot, committed ? kCommitted : bp.unwrap()->ballots[k],
                                bp.unwrap()->cmds[k].clone()});
          }
        };
        take(r->sync_data, true);
        take(r->accepted_data, false);
        reported_last = std::max(reported_last, last);
        if (i >= r->complete.size() || r->complete[i] == 0) {
          settled = std::min(settled, last);
        }
      }
      for (auto& v : reported) {
        if (v.slot > settled) continue;
        auto it = best[i].find(v.slot);
        if (it == best[i].end() || it->second.first < v.ballot) {
          best[i].insert_or_assign(v.slot, std::make_pair(v.ballot, std::move(v.cmd)));
        }
      }
      if (settled == kAll) {
        // Later requests start past this partition's slots, so its
        // entries do not use up the next replies' bytes.
        done[i] = true;
        next[i] = reported_last + 1;
        progress = true;
      } else if (settled >= next[i]) {
        next[i] = settled + 1;
        progress = true;
      }
    }
    if (std::all_of(done.begin(), done.end(), [](bool d) { return d; })) break;
    if (!progress) {
      Log_warn("learner recovery: the acceptors' replies settled no slot; adopting only the slots settled so far");
      break;
    }
  }
  std::vector<SlotEntries> adopted(n);
  for (size_t i = 0; i < n; i++) {
    for (auto& kv : best[i]) {
      if (kv.first != committed_high[i] + 1 + static_cast<slotid_t>(adopted[i].size())) {
        Log_warn("learner recovery: partition {}: no acceptor holds slot {}; not adopting the {} values past it",
                 i, committed_high[i] + 1 + adopted[i].size(), best[i].size() - adopted[i].size());
        break;
      }
      adopted[i].emplace_back(kv.first, kv.second.second.clone());
    }
    if (!adopted[i].empty()) {
      Log_info("learner recovery: partition {} adopts {} accepted values in slots {}-{}",
               i, adopted[i].size(), adopted[i].front().first, adopted[i].back().first);
    }
  }
  return adopted;
}

// The committed entries in slots from[i]..to[i] of each partition, fetched
// from the acceptors (each slot from whichever has it) in replies of at most
// kReplyBytes. The learner applied these slots from p1's forwards and kept
// no copy. A slot neither acceptor reports ends its partition's range.
static std::vector<SlotEntries> fetch_committed_range(int epoch, const std::vector<slotid_t>& from,
                                                      const std::vector<slotid_t>& to) {
  constexpr int64_t kReplyBytes = 16 * 1024 * 1024;
  const size_t n = pxs_workers_g.size();
  std::vector<SlotEntries> got(n);
  std::vector<slotid_t> next = from;
  for (;;) {
    bool wanted = false;
    for (size_t i = 0; i < n; i++) {
      wanted = wanted || next[i] <= to[i];
    }
    if (!wanted) break;
    auto req = make_shared<SyncLogRequest>();
    req->leader_id = es->machine_id;
    req->epoch = epoch;
    req->sync_commit_slot = next;
    for (size_t i = 0; i < n; i++) {
      req->sync_commit_slot[i] = std::min(next[i], to[i] + 1);
    }
    req->max_reply_bytes = kReplyBytes;
    map<pair<int, slotid_t>, rusty::Arc<janus::Command>> m;
    bool ok = false;
    ballot_t rejected = 0;
    run_on_poll_thread(pxs_workers_g.back(), [&] {
      ok = pxs_workers_g.back()->FetchCommitted(req, &m, &rejected);
    });
    if (!ok) {
      Log_warn("learner takeover: could not fetch the slots an acceptor is missing");
      break;
    }
    bool progress = false;
    for (auto& kv : m) {  // ordered by (partition, slot)
      const size_t i = static_cast<size_t>(kv.first.first);
      const slotid_t slot = kv.first.second;
      if (i >= n || slot != next[i] || slot > to[i]) continue;
      got[i].emplace_back(slot, kv.second.clone());
      next[i]++;
      progress = true;
    }
    if (!progress) {
      for (size_t i = 0; i < n; i++) {
        if (next[i] <= to[i]) {
          Log_warn("learner takeover: partition {}: no acceptor reports committed slot {}; "
                   "an acceptor behind it stays behind", i, next[i]);
        }
      }
      break;
    }
  }
  return got;
}

// Proposes partition i's entries again in this leader's epoch, each at its
// own slot (Submit with an explicit slot, not the next open one).
static void propose_again(size_t i, const SlotEntries& entries) {
  auto& pw = pxs_workers_g[i];
  for (auto& e : entries) {
    const auto entry = marshallable_cast<LogEntry>(*e.second);
    verify(entry.is_some());
    std::unique_lock<std::mutex> lock(pw->condition_mutex);
    pw->IncSubmit();
    pw->Submit(entry.unwrap()->log_entry.data(), entry.unwrap()->length,
               pw->site_info_->partition_id_, e.first);
  }
}

void stuff_todo_learner_upgrade(){
  // Still a follower of the old epoch here: the replay callbacks are the
  // follower ones, which write entries into the tables.
  ballot_t higher_epoch = 0;
  const std::vector<slotid_t> committed_high = catch_up_learner(es->get_epoch(), &higher_epoch);
  std::vector<SlotEntries> adopted;
  std::vector<slotid_t> min_executed = committed_high;
  // The epoch this learner will lead in (es->set_epoch() below adds one).
  const ballot_t lead_epoch = es->get_epoch() + 1;
  if (higher_epoch == 0) {
    adopted = recover_accepted(es->get_epoch(), lead_epoch, committed_high, &min_executed, &higher_epoch);
  }
  if (higher_epoch > 0) {
    // Another replica already leads in a later epoch; this one stays a
    // learner rather than take over without the acceptors.
    Log_warn("learner takeover: not taking over, the acceptors are in epoch {}", higher_epoch);
    return;
  }
  // An acceptor can fall behind the other (the old leader reached its
  // quorum without it, and what was queued for it died with the leader).
  // Followers apply in order, and nothing else sends it the committed
  // values it lacks (BroadcastSyncCommit sends nothing), so it would wait
  // at its first missing slot for good. Fetch those values to propose
  // again below; the acceptors that have them already ignore the repeat.
  std::vector<slotid_t> repair_from(committed_high.size());
  for (size_t i = 0; i < repair_from.size(); i++) {
    repair_from[i] = min_executed[i] + 1;
  }
  const std::vector<SlotEntries> repair = fetch_committed_range(es->get_epoch(), repair_from, committed_high);
  apply_on_learner_all(repair);  // normally applied already, from p1's forwards
  apply_on_learner_all(adopted);
  Config::GetConfig()->UpgradeFromLearnerToLeader();
  es->state_lock();
  es->set_state(1);
  es->set_epoch(); // increase the epoch number for failover
  if (es->get_epoch() != lead_epoch) {
    Log_warn("learner takeover: leading in epoch {}, but the acceptors were promised {}",
             es->get_epoch(), lead_epoch);
  }
  for(int i = 0; i < pxs_workers_g.size(); i++){
    pxs_workers_g[i]->election_state_lock.lock();
    pxs_workers_g[i]->cur_epoch = es->get_epoch();
    pxs_workers_g[i]->is_leader = 1;
    pxs_workers_g[i]->election_state_lock.unlock();
    auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    ps->mtx_.lock();
    // New slots start right after every slot the acceptors committed. The
    // old +100 guessed at how far p1 had run past its forwards; catch-up
    // now knows, and the followers apply in order, so a gap would leave
    // them waiting for slots nobody will send (BroadcastSyncCommit sends
    // nothing), and they would never apply this leader's entries.
    ps->max_committed_slot_ = std::max(ps->max_committed_slot_learner_, committed_high[i]);
    // The adopted values were applied above as a learner applies them, so
    // when their proposals below commit they are not executed again.
    const slotid_t last_adopted = adopted[i].empty() ? slotid_t{0} : adopted[i].back().first;
    ps->max_executed_slot_ = std::max(ps->max_committed_slot_, last_adopted);
    // The adopted values keep their slots (proposed again below); this
    // leader's own entries start after them.
    ps->cur_open_slot_ = ps->max_executed_slot_ + 1;
    ps->cur_epoch = es->get_epoch();
    ps->took_over_ = true;
    ps->mtx_.unlock();
  }
  int epoch = es->get_epoch();
  es->state_unlock();
  send_sync_logs(epoch);
  //send_no_ops_to_all_workers(epoch);
  sync_callbacks_for_new_leader(); // switch from follower_callback_ to leader_callback_
  // Propose the repaired slots, then the adopted values, again in this
  // epoch, each at its own slot, ahead of this leader's own entries, so
  // every acceptor commits them. Each gets its slot explicitly rather than
  // the next open one: a decide the old leader sent before it died can
  // still reach p1 during recovery and be forwarded here, moving
  // max_committed_slot_learner_ into these slots, and the next open slot
  // would then shift every value onto another value's slot. A value already
  // committed at its slot is proposed again unchanged.
  for (size_t i = 0; i < repair.size(); i++) {
    if (repair[i].empty()) continue;
    propose_again(i, repair[i]);
    Log_info("learner takeover: partition {} proposes {} committed values again in slots {}-{} "
             "for an acceptor that is behind", i, repair[i].size(), repair[i].front().first, repair[i].back().first);
  }
  for (size_t i = 0; i < adopted.size(); i++) {
    if (adopted[i].empty()) continue;
    propose_again(i, adopted[i]);
    auto ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    Log_info("learner takeover: partition {} proposes its {} adopted values again in slots {}-{}; "
             "its own entries start at slot {}",
             i, adopted[i].size(), adopted[i].front().first, adopted[i].back().first, ps->cur_open_slot_);
  }
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

}  // namespace paxos_impl
