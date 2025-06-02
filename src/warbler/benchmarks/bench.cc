#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <utility>
#include <string>

#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <sys/sysinfo.h>

#include "bench.h"

#include "../counter.h"
#include "../scopedperf.hh"
#include "../allocator.h"
#include "sto/Transaction.hh"
#include "lib/configuration.h"
#include "common.h"
#include "lib/fasttransport.h"
#include "deptran/s_main.h"
#include "benchmarks/sto/sync_util.hh"
#include <chrono>
#include <thread>

#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
extern "C" void malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque, const char *opts);
extern "C" int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif
#ifdef USE_TCMALLOC
#include <google/heap-profiler.h>
#endif

using namespace std;
using namespace util;

size_t nthreads = 1;  // number of TPC-C worker threads per shard
size_t num_erpc_server = 1; // number of eRPC servers
double scale_factor = 1.0; // number of warehouses (same as nthreads) per shard
size_t nshards = 1;
size_t shardIndex = 0;
string cluster = "localhost";
int clusterRole = 0;
size_t workload_type = 1; // 0. simpleShards (debug), 1. tpcc
transport::Configuration *config=nullptr; // config file
volatile bool running = true;
volatile int control_mode = 0; // 0--> default, 1--> pausing db worker threads, 2 --> resume db worker threads
int verbose = 0;
uint64_t txn_flags = 0;
uint64_t runtime = 30;
volatile int runtime_plus = 0; // pre-undetermined running time
uint64_t ops_per_worker = 0;
int run_mode = RUNMODE_TIME;
int enable_parallel_loading = false;
int pin_cpus = 0;
int slow_exit = 0;
int retry_aborted_transaction = 0;
int no_reset_counters = 0;
int backoff_aborted_transaction = 0;
int use_hashtable = 0;

// par_id ==> shardClient
std::unordered_map<int, srolis::ShardClient*> shardClientAll;
// par_id ==> txn
std::unordered_map<int, Transaction*> shardTxnAll;


static void arr2str(vector<uint64_t> arr) {
  cerr << "[";
  for (size_t i = 0; i < arr.size(); i++) {
    cerr << arr[i] << " ";
  }
  cerr << "]";
  cerr << endl;
}

template <typename T>
static void
delete_pointers(const vector<T *> &pts)
{
  for (size_t i = 0; i < pts.size(); i++)
    delete pts[i];
}

template <typename T>
static vector<T>
elemwise_sum(const vector<T> &a, const vector<T> &b)
{
  INVARIANT(a.size() == b.size());
  vector<T> ret(a.size());
  for (size_t i = 0; i < a.size(); i++)
    ret[i] = a[i] + b[i];
  return ret;
}

template <typename K, typename V>
static void
map_agg(map<K, V> &agg, const map<K, V> &m)
{
  for (typename map<K, V>::const_iterator it = m.begin();
       it != m.end(); ++it)
    agg[it->first] += it->second;
}

// returns <free_bytes, total_bytes>
static pair<uint64_t, uint64_t>
get_system_memory_info()
{
  struct sysinfo inf;
  sysinfo(&inf);
  return make_pair(inf.mem_unit * inf.freeram, inf.mem_unit * inf.totalram);
}

static bool
clear_file(const char *name)
{
  ofstream ofs(name);
  ofs.close();
  return true;
}

uint64_t getEpochInms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static void
write_cb(void *p, const char *s) UNUSED;
static void
write_cb(void *p, const char *s)
{
  const char *f = "jemalloc.stats";
  static bool s_clear_file UNUSED = clear_file(f);
  ofstream ofs(f, ofstream::app);
  ofs << s;
  ofs.flush();
  ofs.close();
}

static event_avg_counter evt_avg_abort_spins("avg_abort_spins");

void
bench_worker::run()
{
  // this is only reserved for leader cluster
  // on other alive leader servers
  register_fasttransport_for_bench([&, control_mode](int control, int value) {
    Warning("receive a control in register_fasttransport_for_bench: %d, EpochInms: %llu", control, getEpochInms());
    switch (control) {
#if defined(FAIL_NEW_VERSION)
      case 0: {
        control_mode = 4;
        // If a transaction sent to a failed shard, we put it into the queue
        sync_util::sync_logger::failed_shard_index = value%10;
        sync_util::sync_logger::failed_shard_ts = value/10;
        sync_util::sync_logger::setShardWBlind(value/10, value%10);

        string log = "no-ops:" + to_string(get_epoch());
        for(int i = 0; i < nthreads; i++){
          add_log_to_nc(log.c_str(), log.size(), i);
        }

        set_epoch();
        runtime_plus = runtime; // another runtime

        // Unpaused previous blocked threads if any
        for (int par_id=0;par_id<nthreads;par_id++){
           shardClientAll[par_id]->setBlocking(false);
        }
        break;
      }
      case 1: {
        // Update its local FVW
        vector<uint32_t> fvw(nshards);
        for (int i=0; i<nshards; i++) {
          // it should get shard-0 from the learner
          int clusterRoleLocal = srolis::LOCALHOST_CENTER_INT;
          if (i==0) 
            clusterRoleLocal = srolis::LEARNER_CENTER_INT;
          std::string w_i = srolis::Memcached::get_key("fvw_"+std::to_string(i), 
                                                      config->shard(0, clusterRoleLocal).host.c_str(), 
                                                      config->mports[clusterRoleLocal]);
          std::cout<<"get fvw, " << clusterRoleLocal << ", fvw_"+std::to_string(i)<<":"<<w_i<<std::endl;
          fvw[i] = std::stoi(w_i);
        }

        sync_util::sync_logger::update_stable_timestamp_vec(get_epoch()-1, fvw);
        control_mode = 5; // We can move to the next epoch, and re-execute transactions in the queue
        break;
      }
#else
      case 0: {
        // 1. pause database worker threads and abort all current transactions if any
        control_mode = 1;
        // for (int par_id=0;par_id<nthreads;par_id++){
        //   shardClientAll[par_id]->setBreakTimeout(true);
        // }
        // 3. config update
        sync_util::sync_logger::failed_shard_index = value%10;
        sync_util::sync_logger::failed_shard_ts = value/10;
        break;
      }
      case 1: { // receive a PREPARE
        // commit local transactions only during the PREPARE phase;
        // commit all buffers in the Paxos stream-0
        control_mode = 3;
      }
#endif
      case 2: { // receive a COMMIT
        // Once a new epoch is received, we can unblock the blocked worker threads
        // for (int par_id=0;par_id<nthreads;par_id++){
        //   shardClientAll[par_id]->setBreakTimeout(false);
        // }
        // resume the database worker threads, update it after the new workers are created;
        // 4. issue no-ops within the old epoch
        string log = "no-ops:" + to_string(get_epoch());
        for(int i = 0; i < nthreads; i++){
          add_log_to_nc(log.c_str(), log.size(), i);
        }
        // 2. increase the epoch
        set_epoch();
        control_mode = 2;
        runtime_plus = runtime; // another runtime
        break;
      }
      case 3: { // terminate
        running=false;
        runtime_plus=0;
        break;
      }
    }
    return 0;
  });

  #ifdef USE_JEMALLOC
  // std::cout << "we are using jemalloc? " << (mallctl != nullptr) << std::endl;
  #endif
  #ifndef JEMALLOC_NO_RENAME
  std::cout << "No JEMALLOC_NO_RENAME" << std::endl;
  #endif
  // XXX(stephentu): so many nasty hacks here. should actually
  // fix some of this stuff one day
  if (set_core_id)
    coreid::set_core_id(worker_id); // cringe
  {
    scoped_rcu_region r; // register this thread in rcu region
  }
  scoped_db_thread_ctx ctx(db, false);
  on_run_setup();
  shardClientAll[TThread::getPartitionID()]=TThread::sclient;

  const workload_desc_vec workload = get_workload();
  //    i (0-5): local commits: A
  //    i (5-10): local aborts: B
  //    i+10: local commits latency - nano - Ta
  //    i+15: local abort latency - nano - Tb
  //    i+20: remote shard commits: C
  //    i+25: remote shard aborts: D
  //    i+30: remote shard commits - nano - Tc
  //    i+35: remote shard aborts - nano - Td
  txn_counts.resize(40);
  barrier_a->count_down();
  barrier_b->wait_for();
  while (running && (run_mode != RUNMODE_OPS || ntxn_commits < ops_per_worker)) {
    double d = r.next_uniform();
    for (size_t i = 0; i < workload.size(); i++) {
      if ((i + 1) == workload.size() || d < workload[i].frequency) {
      retry:
        timer t(true);  // nano counter
        const unsigned long old_seed = r.get_seed();
        const auto ret = workload[i].fn(this);
        // if (control_mode==1){
        //   std::cout<<"one transaction2\n";
        // }
        auto tl = t.lap_nano();
        if (likely(ret.first)) {
          ++ntxn_commits;
          if (ret.second % 10 == 1)
            latency_numer_us_remote += tl/1000.0;
          else
            latency_numer_us += tl/1000.0;
          backoff_shifts >>= 1;
        } else {
          ++ntxn_aborts;
          if (false && retry_aborted_transaction && running) { // don't retry
            if (backoff_aborted_transaction) {
              if (backoff_shifts < 63)
                backoff_shifts++;
              uint64_t spins = 1UL << backoff_shifts;
              spins *= 100; // XXX: tuned pretty arbitrarily
              evt_avg_abort_spins.offer(spins);
              while (spins) {
                nop_pause();
                spins--;
              }
            }
            r.set_seed(old_seed);
            goto retry;
          }
        }
        size_delta += ret.second/10; // should be zero on abort
        if (ret.second % 10 == 1) {  // remote 
          if (ret.first){ // commit
            txn_counts[i+20]++;
            txn_counts[i+30]+=tl;
          } else { // abort
            txn_counts[i+25]++;
            txn_counts[i+35]+=tl;
          }
        } else { // local
          if (ret.first){ // commit
            txn_counts[i]++; // txn_counts aren't used to compute throughput (is
                           // just an informative number to print to the console
                           // in verbose mode)
            txn_counts[i+10]+=tl;
          }else { // abort
            txn_counts[i+5]++; // txn_counts aren't used to compute throughput (is
                           // just an informative number to print to the console
                           // in verbose mode)
            txn_counts[i+15]+=tl;
          }
        }
        break;
      }
      d -= workload[i].frequency;
    }
  }
#if defined(COCO)
  shardTxnAll[TThread::getPartitionID()]=TThread::txn;
#endif
  std::cout<<"jump out the while loop"<<std::endl;
  // clockid_t cid;
  // int s;
  // s = pthread_getcpuclockid(pthread_self(), &cid);
  // pclock((char*)("[CPU_TIME] Database worker thread CPU time lock, id: " + std::to_string(TThread::id()) + ": ").c_str(), cid);
  TThread::sclient->statistics();
  sleep(1); // ensure all worker threads finish execution
}

std::map<std::string, abstract_ordered_index *>
bench_runner::get_open_tables() {
    return open_tables;
}

void
bench_runner::stop() { // invoke inside run function; stop all ShardClient instances
  Warning("stop all erpc clients. set stop=false");
  for (int par_id=0;par_id<nthreads;par_id++){
   shardClientAll[par_id]->stop();
  }
}

void
bench_runner::run()
{
  // load data
  const vector<bench_loader *> loaders = make_loaders();

  if (f_mode==0) { // f_mode==0 is normal to load data
  {
    // spin_barrier b(loaders.size());
    const pair<uint64_t, uint64_t> mem_info_before = get_system_memory_info();
    {
      scoped_timer t("dataloading", verbose);
      size_t N=loaders.size();
      Warning("# of loaders size:%d",N);
      for (int batch=0;batch<(N/nthreads)+1;batch++){
        for (int j=0;j<nthreads;j++){
          int i=batch*nthreads+j;
          if (i<N){
            //Warning("start thread:%d",i);
            loaders.at(i)->start();
          } 
        }
        for (int j=0;j<nthreads;j++){
          int i=batch*nthreads+j;
          if (i<N){
            loaders.at(i)->join();
            //Warning("start thread-(DONE):%d",i);
          } 
        }
      }
    }
    
    const pair<uint64_t, uint64_t> mem_info_after = get_system_memory_info();
    const int64_t delta = int64_t(mem_info_before.first) - int64_t(mem_info_after.first); // free mem
    const double delta_mb = double(delta)/1048576.0;
    if (verbose)
      cerr << "DB size: " << delta_mb << " MB" << endl;
  }

  db->do_txn_epoch_sync(); // also waits for worker threads to be persisted
  {
    const auto persisted_info = db->get_ntxn_persisted();
    if (get<0>(persisted_info) != get<1>(persisted_info))
      cerr << "ERROR: " << persisted_info << endl;
    //ALWAYS_ASSERT(get<0>(persisted_info) == get<1>(persisted_info));
    if (verbose)
      cerr << persisted_info << " txns persisted in loading phase" << endl;
  }
  db->reset_ntxn_persisted();

  if (!no_reset_counters) {
    event_counter::reset_all_counters(); // XXX: for now - we really should have a before/after loading
    PERF_EXPR(scopedperf::perfsum_base::resetall());
  }
  {
    const auto persisted_info = db->get_ntxn_persisted();
    if (get<0>(persisted_info) != 0 ||
        get<1>(persisted_info) != 0 ||
        get<2>(persisted_info) != 0.0) {
      cerr << persisted_info << endl;
      ALWAYS_ASSERT(false);
    }
  }
  } // end of f_mode==0

  Warning("# of shards:%d",SHARDS);
  Warning("# of nthreads:%d",nthreads);
  Warning("# of MERGE_KEYS_GROUPS:%d",MERGE_KEYS_GROUPS);
  map<string, size_t> table_sizes_before;
  if (verbose) {
    for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
         it != open_tables.end(); ++it) {
      scoped_rcu_region guard;
      const size_t s = it->second->size();
      //cerr << "table " << it->first << " size " << s << endl;
      table_sizes_before[it->first] = s;
    }
    cerr << "starting benchmark..." << endl;
  }

  const pair<uint64_t, uint64_t> mem_info_before = get_system_memory_info();

  if (f_mode == 0) {
    std::cout << "--------------Finish loading data and wait for others completing load phase ------------" << std::endl;
    srolis::Memcached::set_key("load_phase_"+std::to_string(shardIndex), "DONE", config->shard(0, clusterRole).host.c_str(), config->mports[clusterRole]);

    // wait for all other shards to complete
    for (int i=0; i<config->nshards; i++) {
      if (i!=shardIndex) {
        srolis::Memcached::wait_for_key("load_phase_"+std::to_string(i), config->shard(0, clusterRole).host.c_str(), config->mports[clusterRole]);
      }
    }

#if defined(PAXOS_LIB_ENABLED)
    string log = "aa";
    for(int i=0;i<nthreads;i++)
      add_log_to_nc(log.c_str(), log.size(), i); // notify others start a advancer
#endif
  }
  const vector<bench_worker *> workers = make_workers();
  ALWAYS_ASSERT(!workers.empty());
  Transaction::clear_stats();
  int idx=0;
  for (vector<bench_worker *>::const_iterator it = workers.begin();
       it != workers.end(); ++it) {
        int core_id = shardIndex * 64 + idx;
        idx++;
        (*it)->startBind(core_id);
  }
  //TThread::in_loading_phase = false;

  barrier_a.wait_for(); // wait for all threads to start up
  timer t, t_nosync;  // timing starts
  barrier_b.count_down(); // bombs away!
  std::vector<std::pair<uint64_t, uint32_t>> samplingTPUT;
  if (run_mode == RUNMODE_TIME) {
    Warning("start the running time, runTime:%d", runtime);
    int interval = 10; // 10 ms
    int repeats = 1000/interval;
    int runtime_loop = runtime * repeats;
    while (runtime_loop>0) {
      if (shardIndex==0 &&
            runtime * repeats - runtime_loop >= repeats * 5 &&
            cluster.compare("localhost")==0) { // 5 seconds, kill it on leader{0}
        uint32_t aa = getEpochInms();
        //Panic("STOP current process! tt:%llu", aa);
      }
      if (runtime_loop % repeats == 0) 
        Warning("runtime time left:%d ms, bool:%d",runtime_loop * interval, runtime_loop>0);
      if (runtime_loop % repeats == 0) 
        std::cout<<"flush"<<std::flush;
      runtime_loop--;
      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      uint32_t n_commits = 0 ;
      for (size_t j = 0; j < nthreads; j++) { n_commits += workers[j]->get_ntxn_commits(); }
      samplingTPUT.push_back({getEpochInms(), n_commits});
      cerr << "Time: " << getEpochInms() << ", n_comits: " << n_commits << endl;
    }
    Warning("runtime_plus:%d",runtime_plus);
    runtime_loop = runtime_plus * repeats;
    while (runtime_loop>0 && runtime_plus>0) { // runtime_plus can be used to terminate the process
      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      if (runtime_loop % repeats == 0) 
        Warning("runtime time left:%d ms, bool:%d",runtime_loop * interval, runtime_loop>0);
      runtime_loop--;
      uint32_t n_commits = 0 ;
      for (size_t j = 0; j < nthreads; j++) { n_commits += workers[j]->get_ntxn_commits(); }
      samplingTPUT.push_back({getEpochInms(), n_commits});
      cerr << "Time: " << getEpochInms() << ", n_comits: " << n_commits << endl;
    }
  }
  // notify other leaders to shutdown as well
  // if it is the learner ==> it's the new leader and so it should be terminated faster than others
  // the eRPC client has to be used in the same thread that created it, so we can't use client_control
  if (cluster.compare("learner")==0) {
   sync_util::sync_logger::client_control2(3, shardIndex);
  }
  if (run_mode == RUNMODE_TIME) {
    running = false;  // stop database worker threads
  }
  stop(); // stop erpc clients
  __sync_synchronize();
  for (size_t i = 0; i < nthreads; i++)
     workers[i]->join();
  const unsigned long elapsed_nosync = t_nosync.lap()-1e6; // take 1 second off due to sleep(1) within bench_worker::run()
  db->do_txn_finish(); // waits for all worker txns to persist
  //  usleep(100000);
  size_t n_commits = 0;
  size_t n_aborts = 0;
  uint64_t latency_numer_us = 0;
  uint64_t latency_numer_us_remote = 0;
  cerr << "--- n_commits per partition ---" << endl;
  for (size_t i = 0; i < nthreads; i++) {
    n_commits += workers[i]->get_ntxn_commits();
    cerr << " par_id: " << i << ", n_commits: " << workers[i]->get_ntxn_commits() << endl;
    n_aborts += workers[i]->get_ntxn_aborts();
    latency_numer_us += workers[i]->get_latency_numer_us();
    latency_numer_us_remote += workers[i]->get_latency_numer_us_remote();
  }

#if defined(TRACKING_ROLLBACK)
  vector<uint32_t> fvw;

  if (sync_util::sync_logger::hist_timestamp_vec.find(0) != sync_util::sync_logger::hist_timestamp_vec.end()) { 
    auto w = sync_util::sync_logger::hist_timestamp_vec[0][0];
    // w -> timestamp * 10 + epoch
    cerr << "--- failed shard: " << w << endl;
    // time in ms; <tput; good tput>; we do this analysis only up to the failure dection point
    unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> merged_rollback_tracker;
    for (size_t i = 0; i < nthreads; i++) {
      Transaction * txn = shardTxnAll[i];
      unordered_map<uint64_t, vector<uint64_t>> rt = txn->rollbacks_tracker;

      for(const auto& it : rt){
        //cerr << "Key: " << it.first << ", Value: ";
        //arr2str(it.second);
        int c = 0;
        for (const auto& vv : it.second) {
          if (vv <= w / 10) c++;
        }
        merged_rollback_tracker[it.first] = std::make_pair(std::get<0>(merged_rollback_tracker[it.first])+it.second.size(),
                                                           std::get<1>(merged_rollback_tracker[it.first])+c);
      }
    }
    std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> vec(merged_rollback_tracker.begin(), merged_rollback_tracker.end());
    std::sort(vec.begin(), vec.end(), [](const auto& a, const auto& b) {
      return a.first < b.first;
    });

    for (const auto& entry : vec) {
        std::cout << "Key: " << entry.first
                  << ", Value: (" << entry.second.first << ", " << entry.second.second << ")\n";
    }
  }
#endif



  const auto persisted_info = db->get_ntxn_persisted();

  const unsigned long elapsed = t.lap()-1e6; // lap() must come after do_txn_finish(),
                                         // because do_txn_finish() potentially
                                         // waits a bit

  // various sanity checks
  ALWAYS_ASSERT(get<0>(persisted_info) == get<1>(persisted_info));
  // not == b/c persisted_info does not count read-only txns
  ALWAYS_ASSERT(n_commits >= get<1>(persisted_info));

  const double elapsed_nosync_sec = double(elapsed_nosync) / 1000000.0;
  const double agg_nosync_throughput = double(n_commits) / elapsed_nosync_sec;
  const double avg_nosync_per_core_throughput = agg_nosync_throughput / double(workers.size());

  const double elapsed_sec = double(elapsed) / 1000000.0;
  const double agg_throughput = double(n_commits) / elapsed_sec;
  const double avg_per_core_throughput = agg_throughput / double(workers.size());

  const double agg_abort_rate = double(n_aborts) / elapsed_sec;
  const double avg_per_core_abort_rate = agg_abort_rate / double(workers.size());

  // we can use n_commits here, because we explicitly wait for all txns
  // run to be durable
  const double agg_persist_throughput = double(n_commits) / elapsed_sec;
  const double avg_per_core_persist_throughput =
    agg_persist_throughput / double(workers.size());

  // XXX(stephentu): latency currently doesn't account for read-only txns
  const double avg_latency_us =
    double(latency_numer_us) / double(n_commits);
  const double avg_latency_ms = avg_latency_us / 1000.0;
  const double avg_persist_latency_ms =
    get<2>(persisted_info) / 1000.0;

  map<string, size_t> agg_txn_counts = workers[0]->get_txn_counts();
  //cerr << "[breakdown] TPUT worker-0: " << format_list(agg_txn_counts.begin(), agg_txn_counts.end()) << endl << endl;
  ssize_t size_delta = workers[0]->get_size_delta();
  for (size_t i = 1; i < workers.size(); i++) {
    map_agg(agg_txn_counts, workers[i]->get_txn_counts());
    size_delta += workers[i]->get_size_delta();
    map<string, size_t> tmp = workers[i]->get_txn_counts();
    //cerr << "[breakdown] TPUT worker-" << i << ": " << format_list(tmp.begin(), tmp.end()) << endl << endl;
  }

  if (verbose) {
    const pair<uint64_t, uint64_t> mem_info_after = get_system_memory_info();
    const int64_t delta = int64_t(mem_info_before.first) - int64_t(mem_info_after.first); // free mem
    const double delta_mb = double(delta)/1048576.0;
    const double size_delta_mb = double(size_delta)/1048576.0;
    map<string, counter_data> ctrs = event_counter::get_all_counters();

    // cerr << "--- table statistics ---" << endl;
    // for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
    //      it != open_tables.end(); ++it) {
    //   scoped_rcu_region guard;
    //   const size_t s = it->second->size();
    //   const ssize_t delta = ssize_t(s) - ssize_t(table_sizes_before[it->first]);
    //   cerr << "table " << it->first << " size " << it->second->size();
    //   if (delta < 0)
    //     cerr << " (" << delta << " records)" << endl;
    //   else
    //     cerr << " (+" << delta << " records)" << endl;
    // }
#ifdef ENABLE_BENCH_TXN_COUNTERS
    cerr << "--- txn counter statistics ---" << endl;
    {
      // take from thread 0 for now
      abstract_db::txn_counter_map agg = workers[0]->get_local_txn_counters();
      for (auto &p : agg) {
        cerr << p.first << ":" << endl;
        for (auto &q : p.second)
          cerr << "  " << q.first << " : " << q.second << endl;
      }
    }
#endif
    cerr << "--- benchmark statistics ---" << endl;
    cerr << "runtime: " << elapsed_sec << " sec" << endl;
    cerr << "memory delta: " << delta_mb  << " MB" << endl;
    cerr << "n_commits: " << n_commits << endl;
    cerr << "latency_numer_us: " << latency_numer_us << endl;
    cerr << "latency_numer_us_remote: " << latency_numer_us_remote << endl;
    cerr << "memory delta rate: " << (delta_mb / elapsed_sec)  << " MB/sec" << endl;
    cerr << "logical memory delta: " << size_delta_mb << " MB" << endl;
    cerr << "logical memory delta rate: " << (size_delta_mb / elapsed_sec) << " MB/sec" << endl;
    cerr << "agg_nosync_throughput: " << agg_nosync_throughput << " ops/sec" << endl;
    cerr << "avg_nosync_per_core_throughput: " << avg_nosync_per_core_throughput << " ops/sec/core" << endl;
    cerr << "agg_throughput: " << agg_throughput << " ops/sec" << endl;
    cerr << "avg_per_core_throughput: " << avg_per_core_throughput << " ops/sec/core" << endl;
    cerr << "agg_persist_throughput: " << agg_persist_throughput << " ops/sec" << endl;
    cerr << "avg_per_core_persist_throughput: " << avg_per_core_persist_throughput << " ops/sec/core" << endl;
    cerr << "avg_latency: " << avg_latency_ms << " ms" << endl;
    cerr << "avg_persist_latency: " << avg_persist_latency_ms << " ms" << endl;
    cerr << "agg_abort_rate: " << agg_abort_rate << " aborts/sec" << endl;
    cerr << "avg_per_core_abort_rate: " << avg_per_core_abort_rate << " aborts/sec/core" << endl;
    cerr << "txn breakdown: " << format_list(agg_txn_counts.begin(), agg_txn_counts.end()) << endl;

    string txn_w1[] = {"NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel"};
    string txn_ratio[] = {"NewOrder", "Payment"};
    for (int i=0;i<sizeof(txn_w1)/sizeof(txn_w1[0]); i++) {
      if (agg_txn_counts.find(txn_w1[i]+"_Local")!=agg_txn_counts.end()) {
        cerr << "  " << txn_w1[i] << "_local_commit_latency: " << agg_txn_counts[txn_w1[i]+"_Local_NANO"] / (agg_txn_counts[txn_w1[i]+"_Local"] + 0.0) / 1000000.0 << " ms" << endl;
        cerr << "  " << txn_w1[i] << "_local_abort_latency: " << agg_txn_counts[txn_w1[i]+"_Local_NANO_abort"] / (agg_txn_counts[txn_w1[i]+"_Local_abort"] + 0.0) / 1000000.0 << " ms" << endl;
        cerr << "  " << txn_w1[i] << "_local_abort_ratio: " << agg_txn_counts[txn_w1[i]+"_Local_abort"] / (agg_txn_counts[txn_w1[i]+"_Local"] + agg_txn_counts[txn_w1[i]+"_Local_abort"] + 0.0) << endl;
      }
    }

    for (int i=0;i<sizeof(txn_ratio)/sizeof(txn_ratio[0]); i++) {
      if (agg_txn_counts.find(txn_ratio[i]+"_Local")!=agg_txn_counts.end() 
          && agg_txn_counts.find(txn_ratio[i]+"_Remote")!=agg_txn_counts.end()) {
        cerr << "  " << txn_ratio[i] << "_remote_ratio: " << 100*(agg_txn_counts[txn_ratio[i]+"_Remote"]+agg_txn_counts[txn_ratio[i]+"_Remote_abort"]) / (agg_txn_counts[txn_ratio[i]+"_Local"]+agg_txn_counts[txn_ratio[i]+"_Local_abort"]+agg_txn_counts[txn_ratio[i]+"_Remote"]+agg_txn_counts[txn_ratio[i]+"_Remote_abort"] + 0.0) << " %"<< endl;
        cerr << "  " << txn_ratio[i] << "_remote_abort_ratio: " << 100*agg_txn_counts[txn_ratio[i]+"_Remote_abort"] / (agg_txn_counts[txn_ratio[i]+"_Remote_abort"] + agg_txn_counts[txn_ratio[i]+"_Remote"] + 0.0) << " %" << endl;
        cerr << "  " << txn_w1[i] << "_remote_commit_latency: " << agg_txn_counts[txn_w1[i]+"_Remote_NANO"] / (agg_txn_counts[txn_w1[i]+"_Remote"] + 0.0) / 1000000.0 << " ms" << endl;
        cerr << "  " << txn_w1[i] << "_remote_abort_latency: " << agg_txn_counts[txn_w1[i]+"_Remote_NANO_abort"] / (agg_txn_counts[txn_w1[i]+"_Remote_abort"] + 0.0) / 1000000.0 << " ms" << endl;
      }
    }

    cerr << "--- system counters (for benchmark) ---" << endl;
    for (map<string, counter_data>::iterator it = ctrs.begin();
         it != ctrs.end(); ++it)
      cerr << it->first << ": " << it->second << endl;
    cerr << "--- perf counters (if enabled, for benchmark) ---" << endl;
    PERF_EXPR(scopedperf::perfsum_base::printall());
    cerr << "--- allocator stats ---" << endl;
    ::allocator::DumpStats();
    cerr << "---------------------------------------" << endl;

#ifdef USE_JEMALLOC
    // cerr << "dumping heap profile..." << endl;
    // mallctl("prof.dump", NULL, NULL, NULL, 0);
    // cerr << "printing jemalloc stats..." << endl;
    // malloc_stats_print(write_cb, NULL, "");
#endif
#ifdef USE_TCMALLOC
    HeapProfilerDump("before-exit");
#endif
  }

  cerr << "--- system counters for n_comits ---" << endl;
#if defined(COCO)
  for (int i = 0; i < samplingTPUT.size(); i++) {
    cerr << "Time: " << samplingTPUT[i].first << ", n_comits: " << samplingTPUT[i].second << endl;
  }
  std::cout<<"DONE"<<std::endl;
#endif

  cout.flush();

  for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
       it != open_tables.end(); ++it) {
    //it->second->print_stats();
  }

  if (!slow_exit)
    return;

  map<string, uint64_t> agg_stats;
  for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
       it != open_tables.end(); ++it) {
    map_agg(agg_stats, it->second->clear());
    delete it->second;
  }
  if (verbose) {
    for (auto &p : agg_stats)
      cerr << p.first << " : " << p.second << endl;

  }
  open_tables.clear();

  delete_pointers(loaders);
  delete_pointers(workers);
}

template <typename K, typename V>
struct map_maxer {
  typedef map<K, V> map_type;
  void
  operator()(map_type &agg, const map_type &m) const
  {
    for (typename map_type::const_iterator it = m.begin();
        it != m.end(); ++it)
      agg[it->first] = std::max(agg[it->first], it->second);
  }
};

//template <typename KOuter, typename KInner, typename VInner>
//struct map_maxer<KOuter, map<KInner, VInner>> {
//  typedef map<KInner, VInner> inner_map_type;
//  typedef map<KOuter, inner_map_type> map_type;
//};

#ifdef ENABLE_BENCH_TXN_COUNTERS
void
bench_worker::measure_txn_counters(void *txn, const char *txn_name)
{
  auto ret = db->get_txn_counters(txn);
  map_maxer<string, uint64_t>()(local_txn_counters[txn_name], ret);
}
#endif

map<string, size_t>
bench_worker::get_txn_counts() const
{
  map<string, size_t> m;
  const workload_desc_vec workload = get_workload();
  for (size_t i = 0; i < workload.size(); i++) {
    m[workload[i].name+"_Local"] = txn_counts[i];
    m[workload[i].name+"_Local_abort"] = txn_counts[i+5];
    m[workload[i].name+"_Local_NANO"] = txn_counts[i+10];
    m[workload[i].name+"_Local_NANO_abort"] = txn_counts[i+15];
    m[workload[i].name+"_Remote"] = txn_counts[i+20];
    m[workload[i].name+"_Remote_abort"] = txn_counts[i+25];
    m[workload[i].name+"_Remote_NANO"] = txn_counts[i+30];
    m[workload[i].name+"_Remote_NANO_abort"] = txn_counts[i+35];
  }
  return m;
}

void
bench_worker::print_stats() const
{
  for (int i=0; i<sampling_remote_calls.size(); i++)
    std::cout << "[work_id:" << worker_id << "]:" << sampling_remote_calls[i] << std::endl;
}
