#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>

#include <iomanip>
#if defined(__linux__)
#include <sys/sysinfo.h>
#else
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

#include "bench.h"
#include "tpcc.h"

#include "../counter.h"
#include "../scopedperf.hh"
#include "../allocator.h"
#include "sto/Transaction.hh"
#include "lib/configuration.h"
#include "common.h"
#include "lib/fasttransport.h"
#include "deptran/s_main.h"
#include "sto/sync_util.hh"
#include "rpc_setup.h"
#include "cpu_throttler.h"

#ifdef USE_JEMALLOC
#include <jemalloc/jemalloc.h>
extern "C" void malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque, const char *opts);
extern "C" int mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif
#ifdef USE_TCMALLOC
#include <google/heap-profiler.h>
#endif

import std;

using namespace std;
using namespace util;

// par_id ==> shardClient
std::unordered_map<int, mako::ShardClient*> shardClientAll;
std::mutex shardClientAllMutex;
// par_id ==> txn
std::unordered_map<int, Transaction*> shardTxnAll;

namespace {

// Multi-shard runs execute one bench_runner per shard concurrently.  Build the
// machine record off-stream, then publish the complete, short line with one
// write(2).  POSIX guarantees that writes no larger than 512 bytes cannot
// interleave on a pipe, which is the CTest and comparison-runner capture path.
// A chained iostream insertion is only synchronized per insertion and allowed
// two shard records (and stderr diagnostics) to corrupt each other.
void emit_tpcc_result_line(std::string line) {
  // The human-readable statistics immediately before this record do not all
  // end in a newline.  Delimit the machine record in the same atomic write so
  // a concurrent shard cannot leave the prefix attached to diagnostic text.
  line.insert(line.begin(), '\n');
  line.push_back('\n');
  ALWAYS_ASSERT(line.size() <= 512);

  std::lock_guard<std::mutex> lock(mako::benchmark_output_mutex());
  std::cout.flush();

  ssize_t written;
  do {
    written = write(STDOUT_FILENO, line.data(), line.size());
  } while (written < 0 && errno == EINTR);
  ALWAYS_ASSERT(written == static_cast<ssize_t>(line.size()));
}

class scoped_shard_client_registration {
public:
  scoped_shard_client_registration(int partition_id,
                                    mako::ShardClient *client)
      : partition_id_(partition_id), client_(client) {
    ALWAYS_ASSERT(client_ != nullptr);
    std::lock_guard<std::mutex> lock(shardClientAllMutex);
    const auto [entry, inserted] =
        shardClientAll.emplace(partition_id_, client_);
    ALWAYS_ASSERT(inserted || entry->second == client_);
  }

  ~scoped_shard_client_registration() {
    std::lock_guard<std::mutex> lock(shardClientAllMutex);
    const auto entry = shardClientAll.find(partition_id_);
    if (entry != shardClientAll.end() && entry->second == client_)
      shardClientAll.erase(entry);
  }

private:
  int partition_id_;
  mako::ShardClient *client_;
};

} // namespace


static void arr2str(vector<uint64_t> arr) {
  mako::benchmark_cerr() << "[";
  for (size_t i = 0; i < arr.size(); i++) {
    mako::benchmark_cerr() << arr[i] << " ";
  }
  mako::benchmark_cerr() << "]";
  mako::benchmark_cerr() << endl;
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

static map<string, uint64_t>
clear_and_close_benchmark_indexes(
    abstract_db *db,
    map<string, abstract_ordered_index *> &open_tables)
{
  // The database decides whether open_index transfers ownership or returns a
  // borrowed facade. close_index is the common release operation.
  ALWAYS_ASSERT(db != nullptr);
  map<string, uint64_t> agg_stats;
  for (auto &entry : open_tables) {
    ALWAYS_ASSERT(entry.second != nullptr);
    try {
      map_agg(agg_stats, entry.second->clear());
    } catch (const oi_clear_unsupported &) {
      // Closing the facade is still required and is the only teardown
      // operation implemented by the legacy MassTrans adapter.
    }
    db->close_index(entry.second);
  }
  open_tables.clear();
  return agg_stats;
}

void
bench_runner::clear_and_close_open_tables()
{
  const map<string, uint64_t> agg_stats =
      clear_and_close_benchmark_indexes(db, open_tables);
  if (BenchmarkConfig::getInstance().getVerbose()) {
    for (auto &p : agg_stats)
      mako::benchmark_cerr() << p.first << " : " << p.second << endl;
  }
}

// returns <free_bytes, total_bytes>
static pair<uint64_t, uint64_t>
get_system_memory_info()
{
#if defined(__linux__)
  struct sysinfo inf;
  sysinfo(&inf);
  return make_pair(inf.mem_unit * inf.freeram, inf.mem_unit * inf.totalram);
#else
  uint64_t total = 0;
  size_t total_len = sizeof(total);
  (void)sysctlbyname("hw.memsize", &total, &total_len, nullptr, 0);

  mach_port_t host = mach_host_self();
  vm_size_t page_size = 0;
  (void)host_page_size(host, &page_size);
  vm_statistics64_data_t vmstat;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  uint64_t free_bytes = 0;
  if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vmstat, &count) == KERN_SUCCESS) {
    free_bytes = static_cast<uint64_t>(vmstat.free_count) * static_cast<uint64_t>(page_size);
  }
  return make_pair(free_bytes, total);
#endif
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
  auto& benchConfig = BenchmarkConfig::getInstance();
  register_fasttransport_for_bench([](int control, int value) {
    auto& benchConfig = BenchmarkConfig::getInstance();
    Warning("receive a control in register_fasttransport_for_bench: %d, EpochInms: %llu", control, getEpochInms());
    switch (control) {
#if defined(FAIL_NEW_VERSION)
      case 0: {
        // If a transaction sent to a failed shard, we put it into the queue
        sync_util::sync_logger::failed_shard_index.store(
            value % 10, std::memory_order_relaxed);
        sync_util::sync_logger::failed_shard_ts.store(
            value / 10, std::memory_order_relaxed);
        sync_util::sync_logger::setShardWBlind(value/10, value%10);
        benchConfig.setControlMode(4);

        string log = "no-ops:" + to_string(get_epoch());
        for(int i = 0; i < benchConfig.getNthreads(); i++){
          add_log_to_nc(log.c_str(), log.size(), i);
        }

        set_epoch();
        benchConfig.setRuntimePlus(benchConfig.getRuntime()); // another runtime

        // Unpaused previous blocked threads if any
        {
          std::lock_guard<std::mutex> lock(shardClientAllMutex);
          for (auto &[par_id, client] : shardClientAll) {
            if (client != nullptr)
              client->setBlocking(false);
            else
              Warning("ShardClient for par_id=%d is nullptr in setBlocking, skipping", par_id);
          }
        }
        break;
      }
      case 1: {
        // Update its local FVW
        vector<uint32_t> fvw(benchConfig.getNshards());
        for (int i=0; i<benchConfig.getNshards(); i++) {
          // it should get shard-0 from the learner
          int clusterRoleLocal = mako::LOCALHOST_CENTER_INT;
          if (i==0) 
            clusterRoleLocal = mako::LEARNER_CENTER_INT;
          std::string w_i = mako::NFSSync::get_key("fvw_"+std::to_string(i), 
                                                      benchConfig.getConfig()->shard(0, clusterRoleLocal).host.c_str(), 
                                                      benchConfig.getConfig()->mports[clusterRoleLocal]);
          mako::benchmark_cout() << "get fvw, " << clusterRoleLocal
                                 << ", fvw_" + std::to_string(i) << ":"
                                 << w_i << std::endl;
          fvw[i] = std::stoi(w_i);
        }

        sync_util::sync_logger::update_stable_timestamp_vec(get_epoch()-1, fvw);
        benchConfig.setControlMode(5); // We can move to the next epoch, and re-execute transactions in the queue
        break;
      }
#else
      case 0: {
        // 1. pause database worker threads and abort all current transactions if any
        // for (int par_id=0;par_id<nthreads;par_id++){
        //   shardClientAll[par_id]->setBreakTimeout(true);
        // }
        // 3. config update
        sync_util::sync_logger::failed_shard_index.store(
            value % 10, std::memory_order_relaxed);
        sync_util::sync_logger::failed_shard_ts.store(
            value / 10, std::memory_order_relaxed);
        benchConfig.setControlMode(1);
        break;
      }
      case 1: { // receive a PREPARE
        // commit local transactions only during the PREPARE phase;
        // commit all buffers in the Paxos stream-0
        benchConfig.setControlMode(3);
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
        for(int i = 0; i < benchConfig.getNthreads(); i++){
          add_log_to_nc(log.c_str(), log.size(), i);
        }
        // 2. increase the epoch
        set_epoch();
        benchConfig.setControlMode(2);
        benchConfig.setRuntimePlus(benchConfig.getRuntime()); // another runtime
        break;
      }
      case 3: { // terminate
        benchConfig.setRunning(false);
        benchConfig.setRuntimePlus(0);
        break;
      }
    }
    return 0;
  });

  #ifdef USE_JEMALLOC
  // std::cout << "we are using jemalloc? " << (mallctl != nullptr) << std::endl;
  #endif
  #ifndef JEMALLOC_NO_RENAME
  mako::benchmark_cout() << "No JEMALLOC_NO_RENAME" << std::endl;
  #endif

  // Bind this worker thread to the appropriate SiloRuntime FIRST
  // This MUST happen before set_core_id since it needs the correct runtime
  if (benchConfig.getConfig() && benchConfig.getConfig()->multi_shard_mode) {
    // Use this worker's shard index, or fall back to first local shard
    int shard_idx = shard_index_;
    if (shard_idx < 0 && !benchConfig.getConfig()->local_shard_indices.empty()) {
      shard_idx = benchConfig.getConfig()->local_shard_indices[0];
    }
    // IMPORTANT: Set thread-local shard index BEFORE thread_init() is called
    // This ensures TThread::set_shard_index() in thread_init() gets the correct value
    BenchmarkConfig::setThreadLocalShardIndex(shard_idx);

    ShardContext* shard_ctx = benchConfig.getShardContext(shard_idx);
    if (shard_ctx && shard_ctx->runtime.get()) {
      // Use get() and const_cast because get_mut() returns null with shared ownership
      const_cast<SiloRuntime*>(shard_ctx->runtime.get())->BindToCurrentThread();
    }
  } else {
    // Single-shard mode: use global default runtime
    SiloRuntime::Current()->BindToCurrentThread();
  }

  // XXX(stephentu): so many nasty hacks here. should actually
  // fix some of this stuff one day
  // In multi-shard mode, use local worker ID for core assignment
  if (set_core_id)
    coreid::set_core_id(worker_id);

  {
    scoped_rcu_region r; // register this thread in rcu region
  }
  scoped_db_thread_ctx ctx(db, false);
  on_run_setup();
  scoped_shard_client_registration client_registration(
      TThread::getGlobalPartitionID(), TThread::sclient);

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

  // Create CPU throttler for this worker thread
  CpuThrottler throttler(
      benchConfig.getCpuLimitPercent(),
      benchConfig.getThrottleCycleMs()
  );
  if (throttler.is_enabled()) {
    mako::benchmark_cout() << "Worker " << worker_id
                           << " CPU throttling enabled: "
                           << throttler.get_cpu_percent() << "% with "
                           << throttler.get_cycle_ms() << "ms cycle"
                           << std::endl;
  }

  while (benchConfig.isRunning() &&
         (benchConfig.getRunMode() != RUNMODE_OPS ||
          get_ntxn_commits() < benchConfig.getOpsPerWorker())) {
    throttler.begin_work();  // Start work timing
    double d = r.next_uniform();
    for (size_t i = 0; i < workload.size(); i++) {
      if ((i + 1) == workload.size() || d < workload[i].frequency) {
      retry:
        util::timer t(true);  // nano counter
        const unsigned long old_seed = r.get_seed();
        const auto ret = workload[i].fn(this);
        // if (control_mode==1){
        //   std::cout<<"one transaction2\n";
        // }
        auto tl = t.lap_nano();
        if (likely(ret.first)) {
#if defined(COCO)
          ntxn_commits.fetch_add(1, std::memory_order_relaxed);
#else
          ++ntxn_commits;
#endif
          if (ret.second % 10 == 1)
            latency_numer_us_remote += tl/1000.0;
          else
            latency_numer_us += tl/1000.0;
          backoff_shifts >>= 1;
        } else {
          ++ntxn_aborts;
          if (false && benchConfig.getRetryAbortedTransaction() && benchConfig.isRunning()) { // don't retry
            if (benchConfig.getBackoffAbortedTransaction()) {
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
    throttler.end_work();  // End work timing, may sleep if budget exhausted
  }
#if defined(COCO)
  shardTxnAll[TThread::getGlobalPartitionID()]=TThread::txn;
#endif
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
  Warning("stop all rpc clients. set stop=false");
  std::lock_guard<std::mutex> lock(shardClientAllMutex);
  for (auto &[par_id, client] : shardClientAll) {
    if (client != nullptr)
      client->stop();
    else
      Warning("ShardClient for par_id=%d is nullptr, skipping stop()", par_id);
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
      scoped_timer t("dataloading", BenchmarkConfig::getInstance().getVerbose());
      size_t N=loaders.size();
      Warning("# of loaders size:%d",N);
      auto& benchConfig = BenchmarkConfig::getInstance();
      for (int batch=0;batch<(N/benchConfig.getNthreads())+1;batch++){
        for (int j=0;j<benchConfig.getNthreads();j++){
          int i=batch*benchConfig.getNthreads()+j;
          if (i<N){
            //Warning("start thread:%d",i);
            loaders.at(i)->start();
          } 
        }
        for (int j=0;j<benchConfig.getNthreads();j++){
          int i=batch*benchConfig.getNthreads()+j;
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
    if (BenchmarkConfig::getInstance().getVerbose())
      mako::benchmark_cerr() << "DB size: " << delta_mb << " MB" << endl;
  }

  db->do_txn_epoch_sync(); // also waits for worker threads to be persisted
  {
    const auto persisted_info = db->get_ntxn_persisted();
    if (get<0>(persisted_info) != get<1>(persisted_info))
      mako::benchmark_cerr() << "ERROR: " << persisted_info << endl;
    //ALWAYS_ASSERT(get<0>(persisted_info) == get<1>(persisted_info));
    if (BenchmarkConfig::getInstance().getVerbose())
      mako::benchmark_cerr() << persisted_info
                             << " txns persisted in loading phase" << endl;
  }
  db->reset_ntxn_persisted();

  if (!BenchmarkConfig::getInstance().getNoResetCounters()) {
    event_counter::reset_all_counters(); // XXX: for now - we really should have a before/after loading
    PERF_EXPR(scopedperf::perfsum_base::resetall());
  }
  {
    const auto persisted_info = db->get_ntxn_persisted();
    if (get<0>(persisted_info) != 0 ||
        get<1>(persisted_info) != 0 ||
        get<2>(persisted_info) != 0.0) {
      mako::benchmark_cerr() << persisted_info << endl;
      ALWAYS_ASSERT(false);
    }
  }
  } // end of f_mode==0

  Warning("# of nthreads:%d",BenchmarkConfig::getInstance().getNthreads());
  map<string, size_t> table_sizes_before;
  if (BenchmarkConfig::getInstance().getVerbose()) {
    for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
         it != open_tables.end(); ++it) {
      scoped_rcu_region guard;
      const size_t s = it->second->size();
      //cerr << "table " << it->first << " size " << s << endl;
      table_sizes_before[it->first] = s;
    }
    mako::benchmark_cerr() << "starting benchmark..." << endl;
  }

  const pair<uint64_t, uint64_t> mem_info_before = get_system_memory_info();

  if (f_mode == 0) {
    mako::benchmark_cout()
        << "--------------Finish loading data and wait for others completing load phase ------------"
        << std::endl;
    auto& cfg = BenchmarkConfig::getInstance();
    mako::NFSSync::set_key("load_phase_"+std::to_string(cfg.getShardIndex()), "DONE", cfg.getConfig()->shard(0, cfg.getClusterRole()).host.c_str(), cfg.getConfig()->mports[cfg.getClusterRole()]);

    // wait for all other shards to complete
    for (int i=0; i<cfg.getConfig()->nshards; i++) {
      if (i!=cfg.getShardIndex()) {
        mako::NFSSync::wait_for_key("load_phase_"+std::to_string(i), cfg.getConfig()->shard(0, cfg.getClusterRole()).host.c_str(), cfg.getConfig()->mports[cfg.getClusterRole()]);
      }
    }

    if (cfg.getIsReplicated()) {
      std::string log(mako::ADVANCER_MARKER_NUM, 'a');
      mako::benchmark_cout()
          << "[ADVANCER-SEND] Leader sending ADVANCER_MARKER to "
          << BenchmarkConfig::getInstance().getNthreads() << " partitions"
          << std::endl;
      for(int i=0;i<BenchmarkConfig::getInstance().getNthreads();i++) {
        add_log_to_nc(log.c_str(), log.size(), i); // notify others start a advancer
        mako::benchmark_cout()
            << "[ADVANCER-SEND] Sent ADVANCER_MARKER to partition " << i
            << std::endl;
      }
    }

    db->on_load_complete();
  }
  const vector<bench_worker *> workers = make_workers();
  ALWAYS_ASSERT(!workers.empty());
  Transaction::clear_stats();
  int idx=0;
  for (vector<bench_worker *>::const_iterator it = workers.begin();
       it != workers.end(); ++it) {
        int core_id = BenchmarkConfig::getInstance().getShardIndex() * 64 + idx;
        idx++;
        (*it)->startBind(core_id);
  }
  //TThread::in_loading_phase = false;

  barrier_a.wait_for(); // wait for all threads to start up

  // In multi-shard single-process mode, wait for all shard threads to be ready
  // This ensures all shards have completed thread_init() before any start transactions
  BenchmarkConfig::getInstance().waitMultiShardBarrier();

  // These timestamped markers let the comparison runner distinguish the
  // measured transaction interval from database loading and slow process
  // teardown. Keep START before timer construction/worker release and END
  // after workers join and the elapsed interval is captured.
  Warning("TPCC_BENCH_MEASURE_START");
  util::timer t, t_nosync;  // timing starts
  barrier_b.count_down(); // bombs away!
#if defined(COCO)
  std::vector<std::pair<uint64_t, uint32_t>> samplingTPUT;
#endif
  auto& benchConfig = BenchmarkConfig::getInstance();
  if (benchConfig.getRunMode() == RUNMODE_TIME) {
    Warning("start the running time, runTime:%d", benchConfig.getRuntime());
    int interval = 10; // 10 ms
    int repeats = 1000/interval;
    int runtime_loop = benchConfig.getRuntime() * repeats;
    while (runtime_loop>0) {
      if (benchConfig.getShardIndex()==0 &&
            benchConfig.getRuntime() * repeats - runtime_loop >= repeats * 5 &&
            benchConfig.getCluster().compare("localhost")==0) { // 5 seconds, kill it on leader{0}
        uint32_t aa = getEpochInms();
        //Panic("STOP current process! tt:%llu", aa);
      }
      if (runtime_loop % repeats == 0) 
        Warning("runtime time left:%d ms, bool:%d",runtime_loop * interval, runtime_loop>0);
      if (runtime_loop % repeats == 0) 
        mako::benchmark_cout() << std::flush;
      runtime_loop--;
      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
#if defined(COCO)
      uint32_t n_commits = 0;
      for (size_t j = 0; j < benchConfig.getNthreads(); ++j)
        n_commits += workers[j]->get_ntxn_commits();
      samplingTPUT.push_back({getEpochInms(), n_commits});
#endif
      //cerr << "Time: " << getEpochInms() << ", n_commits: " << n_commits << endl;
    }
    Warning("runtime_plus:%d",benchConfig.getRuntimePlus());
    runtime_loop = benchConfig.getRuntimePlus() * repeats;
    while (runtime_loop>0 && benchConfig.getRuntimePlus()>0) { // runtime_plus can be used to terminate the process
      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      if (runtime_loop % repeats == 0) 
        Warning("runtime time left:%d ms, bool:%d",runtime_loop * interval, runtime_loop>0);
      runtime_loop--;
#if defined(COCO)
      uint32_t n_commits = 0;
      for (size_t j = 0; j < benchConfig.getNthreads(); ++j)
        n_commits += workers[j]->get_ntxn_commits();
      samplingTPUT.push_back({getEpochInms(), n_commits});
#endif
      //cerr << "Time: " << getEpochInms() << ", n_commits: " << n_commits << endl;
    }
  }
  // notify other leaders to shutdown as well
  // if it is the learner ==> it's the new leader and so it should be terminated faster than others
  // the RPC client has to be used in the same thread that created it, so we can't use client_control
  if (benchConfig.getCluster().compare("learner")==0) {
   sync_util::sync_logger::client_control2(3, benchConfig.getShardIndex());
  }
  if (benchConfig.getRunMode() == RUNMODE_TIME) {
    mako::benchmark_cerr()
        << "[SHUTDOWN] Setting running=false to stop database worker threads"
        << endl;
    benchConfig.setRunning(false);  // stop database worker threads
  }
  mako::benchmark_cerr()
      << "[SHUTDOWN] Calling first stop() to stop client transports" << endl;
  stop(); // stop rpc clients (unblocks outstanding RPCs)
  mako::benchmark_cerr() << "[SHUTDOWN] First stop() completed" << endl;
  __sync_synchronize();

  mako::benchmark_cerr() << "[SHUTDOWN] Joining "
                         << BenchmarkConfig::getInstance().getNthreads()
                         << " worker threads" << endl;
  for (size_t i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
     mako::benchmark_cerr() << "[SHUTDOWN] Joining worker " << i << endl;
     workers[i]->join();
     mako::benchmark_cerr() << "[SHUTDOWN] Worker " << i << " joined" << endl;
  }
  mako::benchmark_cerr() << "[SHUTDOWN] All workers joined" << endl;

  // Stop server transports AFTER workers exit to ensure they can finish processing
  mako::benchmark_cerr() << "[SHUTDOWN] Calling stop_rpc_server()" << endl;
  mako::stop_rpc_server();
  mako::benchmark_cerr() << "[SHUTDOWN] stop_rpc_server() completed" << endl;

  mako::benchmark_cerr() << "[SHUTDOWN] Calling second stop()" << endl;
  stop(); // ensure transports are torn down after workers exit
  mako::benchmark_cerr() << "[SHUTDOWN] Second stop() completed" << endl;
  const unsigned long elapsed_nosync = t_nosync.lap()-1e6; // take 1 second off due to sleep(1) within bench_worker::run()
  Warning("TPCC_BENCH_MEASURE_END");
  mako::benchmark_cerr() << "[SHUTDOWN] Calling do_txn_finish()" << endl;
  db->do_txn_finish(); // waits for all worker txns to persist
  mako::benchmark_cerr() << "[SHUTDOWN] do_txn_finish() completed" << endl;
  //  usleep(100000);
  size_t n_commits = 0;
  size_t n_aborts = 0;
  uint64_t latency_numer_us = 0;
  uint64_t latency_numer_us_remote = 0;
  mako::benchmark_cerr() << "--- n_commits per partition ---" << endl;
  for (size_t i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
    n_commits += workers[i]->get_ntxn_commits();
    mako::benchmark_cerr() << " par_id: " << i << ", n_commits: "
                           << workers[i]->get_ntxn_commits() << endl;
    n_aborts += workers[i]->get_ntxn_aborts();
    latency_numer_us += workers[i]->get_latency_numer_us();
    latency_numer_us_remote += workers[i]->get_latency_numer_us_remote();
  }

#if defined(TRACKING_ROLLBACK)
  vector<uint32_t> fvw;

  if (sync_util::sync_logger::hist_timestamp_vec.find(0) != sync_util::sync_logger::hist_timestamp_vec.end()) { 
    auto w = sync_util::sync_logger::hist_timestamp_vec[0][0];
    // w -> timestamp * 10 + epoch
    mako::benchmark_cerr() << "--- failed shard: " << w << endl;
    // time in ms; <tput; good tput>; we do this analysis only up to the failure dection point
    unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> merged_rollback_tracker;
    for (size_t i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
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
        mako::benchmark_cout()
            << "Key: " << entry.first << ", Value: (" << entry.second.first
            << ", " << entry.second.second << ")\n";
    }
  }
#endif



  const auto persisted_info = db->get_ntxn_persisted();

  const unsigned long elapsed = t.lap()-1e6; // lap() must come after do_txn_finish(),
                                         // because do_txn_finish() potentially
                                         // waits a bit
  const auto safe_div = [](double numer, double denom) -> double {
    return denom > 0.0 ? numer / denom : 0.0;
  };

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
    safe_div(double(latency_numer_us), double(n_commits));
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

  if (BenchmarkConfig::getInstance().getVerbose()) {
    const pair<uint64_t, uint64_t> mem_info_after = get_system_memory_info();
    const int64_t delta = int64_t(mem_info_before.first) - int64_t(mem_info_after.first); // free mem
    const double delta_mb = double(delta)/1048576.0;
    const double size_delta_mb = double(size_delta)/1048576.0;
    map<string, counter_data> ctrs = event_counter::get_all_counters();
    auto diagnostics = mako::benchmark_cerr();

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
    diagnostics << "--- txn counter statistics ---" << endl;
    {
      // take from thread 0 for now
      abstract_db::txn_counter_map agg = workers[0]->get_local_txn_counters();
      for (auto &p : agg) {
        diagnostics << p.first << ":" << endl;
        for (auto &q : p.second)
          diagnostics << "  " << q.first << " : " << q.second << endl;
      }
    }
#endif
    diagnostics << "--- benchmark statistics ---" << endl;
    diagnostics << "runtime: " << elapsed_sec << " sec" << endl;
    diagnostics << "memory delta: " << delta_mb  << " MB" << endl;
    diagnostics << "n_commits: " << n_commits << endl;
    diagnostics << "latency_numer_us: " << latency_numer_us << endl;
    diagnostics << "latency_numer_us_remote: " << latency_numer_us_remote << endl;
    diagnostics << "memory delta rate: " << (delta_mb / elapsed_sec)  << " MB/sec" << endl;
    diagnostics << "logical memory delta: " << size_delta_mb << " MB" << endl;
    diagnostics << "logical memory delta rate: " << (size_delta_mb / elapsed_sec) << " MB/sec" << endl;
    diagnostics << "agg_nosync_throughput: " << agg_nosync_throughput << " ops/sec" << endl;
    diagnostics << "avg_nosync_per_core_throughput: " << avg_nosync_per_core_throughput << " ops/sec/core" << endl;
    diagnostics << "agg_throughput: " << agg_throughput << " ops/sec" << endl;
    diagnostics << "avg_per_core_throughput: " << avg_per_core_throughput << " ops/sec/core" << endl;
    diagnostics << "agg_persist_throughput: " << agg_persist_throughput << " ops/sec" << endl;
    diagnostics << "avg_per_core_persist_throughput: " << avg_per_core_persist_throughput << " ops/sec/core" << endl;
    diagnostics << "avg_latency: " << avg_latency_ms << " ms" << endl;
    diagnostics << "avg_persist_latency: " << avg_persist_latency_ms << " ms" << endl;
    diagnostics << "agg_abort_rate: " << agg_abort_rate << " aborts/sec" << endl;
    diagnostics << "avg_per_core_abort_rate: " << avg_per_core_abort_rate << " aborts/sec/core" << endl;
    //cerr << "txn breakdown: " << format_list(agg_txn_counts.begin(), agg_txn_counts.end()) << endl;

    string txn_w1[] = {"NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel"};
    string txn_ratio[] = {"NewOrder", "Payment"};
    for (int i=0;i<sizeof(txn_w1)/sizeof(txn_w1[0]); i++) {
      if (agg_txn_counts.find(txn_w1[i]+"_Local")!=agg_txn_counts.end()) {
        const double local_commits = double(agg_txn_counts[txn_w1[i]+"_Local"]);
        const double local_aborts = double(agg_txn_counts[txn_w1[i]+"_Local_abort"]);
        const double local_commit_nano = double(agg_txn_counts[txn_w1[i]+"_Local_NANO"]);
        const double local_abort_nano = double(agg_txn_counts[txn_w1[i]+"_Local_NANO_abort"]);
        diagnostics << "  " << txn_w1[i] << "_local_commit_latency: "
                    << safe_div(local_commit_nano, local_commits) / 1000000.0
                    << " ms" << endl;
        diagnostics << "  " << txn_w1[i] << "_local_abort_latency: "
                    << safe_div(local_abort_nano, local_aborts) / 1000000.0
                    << " ms" << endl;
        diagnostics << "  " << txn_w1[i] << "_local_abort_ratio: "
                    << safe_div(local_aborts, local_commits + local_aborts)
                    << endl;
      }
    }

    for (int i=0;i<sizeof(txn_ratio)/sizeof(txn_ratio[0]); i++) {
      if (agg_txn_counts.find(txn_ratio[i]+"_Local")!=agg_txn_counts.end() 
          && agg_txn_counts.find(txn_ratio[i]+"_Remote")!=agg_txn_counts.end()) {
        const double local_commits = double(agg_txn_counts[txn_ratio[i]+"_Local"]);
        const double local_aborts = double(agg_txn_counts[txn_ratio[i]+"_Local_abort"]);
        const double remote_commits = double(agg_txn_counts[txn_ratio[i]+"_Remote"]);
        const double remote_aborts = double(agg_txn_counts[txn_ratio[i]+"_Remote_abort"]);
        const double remote_commit_nano = double(agg_txn_counts[txn_w1[i]+"_Remote_NANO"]);
        const double remote_abort_nano = double(agg_txn_counts[txn_w1[i]+"_Remote_NANO_abort"]);
        const double remote_total = remote_commits + remote_aborts;
        const double txn_total = local_commits + local_aborts + remote_total;
        diagnostics << "  " << txn_ratio[i] << "_remote_ratio: "
                    << 100 * safe_div(remote_total, txn_total) << " %" << endl;
        diagnostics << "  " << txn_ratio[i] << "_remote_abort_ratio: "
                    << 100 * safe_div(remote_aborts, remote_total) << " %"
                    << endl;
        diagnostics << "  " << txn_w1[i] << "_remote_commit_latency: "
                    << safe_div(remote_commit_nano, remote_commits) / 1000000.0
                    << " ms" << endl;
        diagnostics << "  " << txn_w1[i] << "_remote_abort_latency: "
                    << safe_div(remote_abort_nano, remote_aborts) / 1000000.0
                    << " ms" << endl;
      }
    }

    diagnostics << "--- system counters (for benchmark) ---" << endl;
    for (map<string, counter_data>::iterator it = ctrs.begin();
         it != ctrs.end(); ++it)
      diagnostics << it->first << ": " << it->second << endl;
    diagnostics << "--- perf counters (if enabled, for benchmark) ---" << endl;
    PERF_EXPR(scopedperf::perfsum_base::printall());
    diagnostics << "--- allocator stats ---" << endl;
    ::allocator::DumpStats();
    diagnostics << "---------------------------------------" << endl;

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

  mako::benchmark_cerr() << "--- system counters for n_commits ---" << endl;
#if defined(COCO)
  for (int i = 0; i < samplingTPUT.size(); i++) {
    mako::benchmark_cerr() << "Time: " << samplingTPUT[i].first
                           << ", n_commits: " << samplingTPUT[i].second
                           << endl;
  }
  mako::benchmark_cout() << "DONE" << std::endl;
#endif

  if (BenchmarkConfig::getInstance().getEmitTpccResult()) {
    // Stable machine-readable record consumed by the paired TPC-C runner.
    // The no-sync interval excludes the deliberate one-second worker shutdown
    // sleep and is the actual workload measurement interval.
    std::ostringstream result;
    result << std::setprecision(17) << "TPCC_BENCH_RESULT {"
        << "\"schema_version\":1,"
        << "\"engine\":\"" << BenchmarkConfig::getInstance().getStorageEngine()
        << "\","
        << "\"threads\":" << workers.size() << ","
        << "\"warehouses\":"
        << static_cast<size_t>(
               BenchmarkConfig::getInstance().getScaleFactor())
        << ","
        << "\"configured_seconds\":"
        << BenchmarkConfig::getInstance().getRuntime() << ","
        << "\"measured_seconds\":" << elapsed_nosync_sec << ","
        << "\"commits\":" << n_commits << ","
        << "\"aborts\":" << n_aborts << ","
        << "\"attempts\":" << (n_commits + n_aborts) << ","
        << "\"throughput_txn_s\":" << agg_nosync_throughput << ","
        << "\"mix\":{"
        << "\"NewOrder\":"
        << (agg_txn_counts["NewOrder_Local"] +
            agg_txn_counts["NewOrder_Remote"])
        << ","
        << "\"Payment\":"
        << (agg_txn_counts["Payment_Local"] + agg_txn_counts["Payment_Remote"])
        << ","
        << "\"Delivery\":"
        << (agg_txn_counts["Delivery_Local"] +
            agg_txn_counts["Delivery_Remote"])
        << ","
        << "\"OrderStatus\":"
        << (agg_txn_counts["OrderStatus_Local"] +
            agg_txn_counts["OrderStatus_Remote"])
        << ","
        << "\"StockLevel\":"
        << (agg_txn_counts["StockLevel_Local"] +
            agg_txn_counts["StockLevel_Remote"])
        << "}}";
    emit_tpcc_result_line(std::move(result).str());
  }

  mako::benchmark_cout().flush();

  for (map<string, abstract_ordered_index *>::iterator it = open_tables.begin();
       it != open_tables.end(); ++it) {
    //it->second->print_stats();
  }

  if (!BenchmarkConfig::getInstance().getSlowExit())
    return;

  const auto &config = BenchmarkConfig::getInstance().getConfig();
  if (!config || !config->multi_shard_mode)
    clear_and_close_open_tables();

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
    mako::benchmark_cout() << "[work_id:" << worker_id << "]:"
                           << sampling_remote_calls[i] << std::endl;
}
