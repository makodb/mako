#ifndef _MAKO_COMMON_H_
#define _MAKO_COMMON_H_

#include <fstream>
#include <sstream>
#include <vector>
#include <utility>
#include <string>
#include <set>
#include "rocksdb_persistence_fwd.h"

#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#endif

#include "masstree/config.h"

#include "allocator.h"
#include "stats_server.h"
#include "util.h"

#include "benchmarks/bench.h"
#include "sto/sync_util.hh"
#include "storage/mbta_wrapper.hh"
#include "benchmarks/common.h"
#include "benchmarks/common2.h"
#include "benchmarks/benchmark_config.h"
#include "benchmarks/rpc_setup.h"

// Runtime replication switching - unified interface
#include "deptran/replication_helper.h"

// Cluster-config runtime bootstrap (shard-0 config service + watcher).
#include "cluster_bootstrap.h"

#include "lib/configuration.h"
#include "lib/fasttransport.h"
#include "lib/multi_transport_manager.h"
#include "lib/common.h"
#include "lib/server.h"
#include "lib/rust_wrapper.h"

// Initialize Rust wrapper: communicate with rust-based redis client
/*
static void initialize_rust_wrapper()
{
  RustWrapper* g_rust_wrapper = new RustWrapper();
  if (!g_rust_wrapper->init()) {
      std::cerr << "Failed to initialize rust wrapper!" << std::endl;
      delete g_rust_wrapper;
      std::quick_exit( EXIT_SUCCESS );
  }
  std::cout << "Successfully initialized rust wrapper!" << std::endl;
  g_rust_wrapper->start_polling();
}*/

static void print_system_info()
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  const unsigned long ncpus = coreid::num_cpus_online();
  cerr << "Database Benchmark:"                           << endl;
  cerr << "  pid: " << getpid()                           << endl;
  cerr << "settings:"                                     << endl;
  cerr << "  num-cpus    : " << ncpus                     << endl;
  cerr << "  num-threads : " << benchConfig.getNthreads()  << endl;
  cerr << "  shardIndex  : " << benchConfig.getShardIndex()<< endl;
  cerr << "  paxos_proc_name  : " << benchConfig.getPaxosProcName() << endl;
  cerr << "  nshards     : " << benchConfig.getNshards()   << endl;
  cerr << "  is_micro    : " << benchConfig.getIsMicro()   << endl;
  cerr << "  is_replicated : " << benchConfig.getIsReplicated()   << endl;
#ifdef USE_VARINT_ENCODING
  cerr << "  var-encode  : yes"                           << endl;
#else
  cerr << "  var-encode  : no"                            << endl;
#endif

#ifdef USE_JEMALLOC
  cerr << "  allocator   : jemalloc"                      << endl;
#elif defined USE_TCMALLOC
  cerr << "  allocator   : tcmalloc"                      << endl;
#elif defined USE_FLOW
  cerr << "  allocator   : flow"                          << endl;
#else
  cerr << "  allocator   : libc"                          << endl;
#endif
  cerr << "system properties:" << endl;

#ifdef TUPLE_PREFETCH
  cerr << "  tuple_prefetch          : yes" << endl;
#else
  cerr << "  tuple_prefetch          : no" << endl;
#endif

#ifdef BTREE_NODE_PREFETCH
  cerr << "  btree_node_prefetch     : yes" << endl;
#else
  cerr << "  btree_node_prefetch     : no" << endl;
#endif
}

// Global multi-transport manager (for multi-shard mode)
static mako::MultiTransportManager* g_multi_transport_manager = nullptr;

// Initialize database for a specific shard (multi-shard mode)
// This allows creating isolated database instances for each shard
static abstract_db* initShardDB(int shard_idx, bool is_leader, const std::string& cluster_role) {
  auto& benchConfig = BenchmarkConfig::getInstance();

  Notice("Initializing database for shard %d (cluster: %s, leader: %d)",
         shard_idx, cluster_role.c_str(), is_leader);

  // Create and initialize database instance for this shard
  abstract_db *db = new mbta_wrapper;
  db->init();

  return db;
}

// Initialize and start transports for multi-shard mode.
//
// In multi-shard single-process mode the per-shard "server" FastTransports
// bind base_port + 0 (e.g. 31000, 31100). Worker ShardClients bind
// base_port + par_id (0..warehouses-1) on the same shard, so par_id=0 of
// each shard collides with the manager and panics with AddressInUse.
// ctx->transport is never read anywhere, so the manager's transports are
// unused; skip binding entirely until cross-shard RPC is actually wired
// through the manager instead of through per-worker ShardClients.
static bool initMultiShardTransports(const std::vector<int>& local_shard_indices) {
  if (!BenchmarkConfig::getInstance().getConfig()) {
    Warning("Cannot initialize multi-shard transports: no configuration");
    return false;
  }
  Notice("Skipping MultiTransportManager init for %zu shards "
         "(per-shard transports unused in single-process mode)",
         local_shard_indices.size());
  return true;
}

// Stop multi-shard transports (cleanup)
static void stopMultiShardTransports() {
  if (g_multi_transport_manager) {
    Notice("Stopping MultiTransportManager");
    g_multi_transport_manager->StopAll();
    delete g_multi_transport_manager;
    g_multi_transport_manager = nullptr;
  }
}

// init all threads (single-shard mode, backward compatible)
static abstract_db* initWithDB() {
  auto& benchConfig = BenchmarkConfig::getInstance();

  //initialize_rust_wrapper();

  // initialize the numa allocator
  size_t numa_memory = mako::parse_memory_spec("1G");
  if (numa_memory > 0) {
    const size_t maxpercpu = util::iceil(
        numa_memory / benchConfig.getNthreads(), ::allocator::GetHugepageSize());
    numa_memory = maxpercpu * benchConfig.getNthreads();
    ::allocator::Initialize(benchConfig.getNthreads(), maxpercpu);
  }

  // Print system information
  print_system_info();

  sync_util::sync_logger::Init(benchConfig.getShardIndex(), benchConfig.getNshards(),
                               benchConfig.getNthreads(),
                               benchConfig.getLeaderConfig()==1, /* is leader */
                               benchConfig.getCluster(),
                               benchConfig.getConfig());

  abstract_db *db = new mbta_wrapper; // on the leader replica
  db->init() ;
  return db;
}

static void register_paxos_follower_callback(TSharedThreadPoolMbta& replicated_db, int thread_id)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
  //transport::ShardAddress addr = config->shard(shardIndex, mako::LEARNER_CENTER);
  register_for_follower_par_id_return([&,thread_id](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>> & un_replay_logs_) {
    auto& benchConfig = BenchmarkConfig::getInstance();
    //Warning("receive a register_for_follower_par_id_return, par_id:%d, slot_id:%d,len:%d",par_id, slot_id,len);
    int status = mako::PaxosStatus::STATUS_INIT;
    uint32_t timestamp = 0;  // Track timestamp for return value encoding
    abstract_db * db = replicated_db.getDBWrapper(par_id)->getDB () ;
    // SINGLE-RAFT FIX: getDB() calls TThread::set_id(par_id), changing the
    // thread ID per partition. But the STO Transaction object caches threadid_
    // at creation time. In single-Raft, all partitions share one thread, so
    // the thread ID changes with each par_id. Sync Transaction's threadid_.
    Sto::update_threadid();
    bool noops = false;

    if (len==mako::ADVANCER_MARKER_NUM) { // start a advancer
      status = mako::PaxosStatus::STATUS_REPLAY_DONE;
      if (par_id==0 && benchConfig.getNshards() > 1){
        std::cout << "we can start a advancer" << std::endl;
        sync_util::sync_logger::start_advancer();
      }
      return status;
    }

    // ending of Paxos group
    if (len==0) {
      Warning("Recieved a zero length log");
      status = mako::PaxosStatus::STATUS_ENDING;
      // update the timestamp for this Paxos stream so that not blocking other Paxos streams
      uint32_t min_so_far = numeric_limits<uint32_t>::max();
      sync_util::sync_logger::local_timestamp_[par_id].store(min_so_far, memory_order_release) ;
#ifndef DISABLE_DISK
      sync_util::sync_logger::disk_timestamp_[par_id].store(min_so_far, memory_order_release) ;
#endif
      benchConfig.incrementEndReceived();
    }

    // deal with Paxos log
    if (len>0) {
      if (isNoops(log,len)!=-1) {
        Warning("receive a noops, par_id:%d on follower_callback_,%s",par_id,log);
        if (par_id==0) set_epoch(isNoops(log,len));
        noops=true;
        status = mako::PaxosStatus::STATUS_NOOPS;
      }

      if (noops) {
        sync_util::sync_logger::noops_cnt++;
        while (1) { // check if all threads receive noops
          if (sync_util::sync_logger::noops_cnt.load(memory_order_acquire)==benchConfig.getNthreads()) {
            break ;
          }
          sleep(0);
          break;
        }
        Warning("phase-1,par_id:%d DONE",par_id);
        if (par_id==0) {
          uint32_t local_w = sync_util::sync_logger::computeLocal();
          //Warning("update %s in phase-1 on port:%d", ("noops_phase_"+std::to_string(shardIndex)).c_str(), config->mports[clusterRole]);
          mako::NFSSync::set_key("noops_phase_"+std::to_string(benchConfig.getShardIndex()), 
                                     std::to_string(local_w).c_str(),
                                     benchConfig.getConfig()->shard(0, benchConfig.getClusterRole()).host.c_str(),
                                     benchConfig.getConfig()->mports[benchConfig.getClusterRole()]);


          //Warning("We update local_watermark[%d]=%llu",shardIndex, local_w);
          // update the history stable timestamp
          // TODO: replay inside the function
          sync_util::sync_logger::update_stable_timestamp(get_epoch()-1, sync_util::sync_logger::retrieveShardW()/10);
        }
      }else{
        CommitInfo commit_info = get_latest_commit_info((char *) log, len);
        timestamp = commit_info.timestamp;  // Store for return value encoding
        sync_util::sync_logger::local_timestamp_[par_id].store(commit_info.timestamp, memory_order_release) ;
#ifndef DISABLE_DISK
        // Followers don't persist to disk, so immediately update disk_timestamp_ too
        sync_util::sync_logger::disk_timestamp_[par_id].store(commit_info.timestamp, memory_order_release) ;
#endif
        uint32_t w = sync_util::sync_logger::retrieveW();

        // SINGLE-RAFT FIX: During loading phase (before noops), bypass safety_check.
        // Loading entries are deterministic committed data that don't need watermark
        // ordering protection. In single-Raft, loading tail entries can arrive after
        // ADVANCER_MARKERs (which set worker_running=true and reset watermark), causing
        // them to fail safety_check and enter un_replay_logs where drain replay stalls.
        bool loading_phase = sync_util::sync_logger::noops_cnt.load(memory_order_acquire) == 0;
        if (loading_phase || sync_util::sync_logger::safety_check(commit_info.timestamp, w)) {
          benchConfig.incrementReplayBatch();
          treplay_in_same_thread_opt_mbta_v2(par_id, (char*)log, len, db, benchConfig.getNthreads());
          status = mako::PaxosStatus::STATUS_REPLAY_DONE;
        } else {
          status = mako::PaxosStatus::STATUS_SAFETY_FAIL;
        }
      }
    }

    // wait for vectorized watermark computed from other partition servers
    if (noops) {
      for (int i=0; i<benchConfig.getNthreads(); i++) {
        if (i!=benchConfig.getShardIndex() && par_id==0) {
          mako::NFSSync::wait_for_key("noops_phase_"+std::to_string(i), 
                                          benchConfig.getConfig()->shard(0, benchConfig.getClusterRole()).host.c_str(), benchConfig.getConfig()->mports[benchConfig.getClusterRole()]);
          std::string local_w = mako::NFSSync::get_key("noops_phase_"+std::to_string(i), 
                                                          benchConfig.getConfig()->shard(0, benchConfig.getClusterRole()).host.c_str(), 
                                                          benchConfig.getConfig()->mports[benchConfig.getClusterRole()]);

          //Warning("We update local_watermark[%d]=%s (others)",i, local_w.c_str());
          // In single timestamp system, use max value from all shards
          uint32_t new_watermark = std::stoull(local_w);
          uint32_t current = sync_util::sync_logger::single_watermark_.load(memory_order_acquire);
          if (new_watermark > current) {
              sync_util::sync_logger::single_watermark_.store(new_watermark, memory_order_release);
          }
        }
      }
    }
    auto w = sync_util::sync_logger::retrieveW();

    while (un_replay_logs_.size() > 0) {
        auto it = un_replay_logs_.front() ;
        if (sync_util::sync_logger::safety_check(std::get<0>(it), w)) {
          //Warning("replay-2 par_id:%d, slot_id:%d,un_replay_logs_:%d", par_id, std::get<1>(it),un_replay_logs_.size());
          benchConfig.incrementReplayBatch();
          auto nums = treplay_in_same_thread_opt_mbta_v2(par_id, (char *) std::get<4>(it), std::get<3>(it), db, benchConfig.getNthreads());
          un_replay_logs_.pop() ;
          free((char*)std::get<4>(it));
        } else {
          if (noops){
            un_replay_logs_.pop() ; // TODOs: should compare each transactions one by one
            Warning("no-ops pop a log, par_id:%d,slot_id:%d", par_id,std::get<1>(it));
            free((char*)std::get<4>(it));
          }else{
            break ;
          }
        }
    }

    // wait for all worker threads replay DONE
    if (noops){
      sync_util::sync_logger::noops_cnt_hole++ ;
      while (1) {
        if (sync_util::sync_logger::noops_cnt_hole.load(memory_order_acquire)==benchConfig.getNthreads()) {
          break ;
        } else {
          sleep(0);
          break;
        }
      }

      Warning("phase-3,par_id:%d DONE",par_id);

      if (par_id==0) {
        sync_util::sync_logger::reset();
      }
    }
    // Return timestamp * 10 + status (for safety check compatibility)
    return static_cast<int>(timestamp * 10 + status);
  }, 
  thread_id);
}



static void register_paxos_leader_callback(vector<pair<uint32_t, uint32_t>>& advanceWatermarkTracker, int thread_id)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
  register_for_leader_par_id_return([&,thread_id](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>> & un_replay_logs_) {
    auto& benchConfig = BenchmarkConfig::getInstance();
    //Warning("receive a register_for_leader_par_id_return, par_id:%d, slot_id:%d,len:%d",par_id, slot_id,len);
    int status = mako::PaxosStatus::STATUS_NORMAL;
    uint32_t timestamp = 0;  // Track timestamp for return value encoding
    bool noops = false;

    if (len==mako::ADVANCER_MARKER_NUM) { // start a advancer
      status = mako::PaxosStatus::STATUS_REPLAY_DONE;
      if (par_id==0){
        std::cout << "we can start a advancer" << std::endl;
        sync_util::sync_logger::start_advancer();
      }
      return status; 
    }

    if (len==0) {
      status = mako::PaxosStatus::STATUS_ENDING;
      Warning("Recieved a zero length log");
      uint32_t min_so_far = numeric_limits<uint32_t>::max();
      sync_util::sync_logger::local_timestamp_[par_id].store(min_so_far, memory_order_release) ;
#ifndef DISABLE_DISK
      sync_util::sync_logger::disk_timestamp_[par_id].store(min_so_far, memory_order_release) ;
#endif
      benchConfig.incrementEndReceivedLeader();
    }

    if (len>0) {
      if (isNoops(log,len)!=-1) {
        //Warning("receive a noops, par_id:%d on leader_callback_,log:%s",par_id,log);
        noops=true;
        status = mako::PaxosStatus::STATUS_NOOPS;
      }

      if (noops) {
        sync_util::sync_logger::noops_cnt++;
        auto& benchConfig = BenchmarkConfig::getInstance();
        while (1) { // check if all threads receive noops
          if (sync_util::sync_logger::noops_cnt.load(memory_order_acquire)==benchConfig.getNthreads()) {
            break ;
          }
          sleep(0);
          break;
        }
        Warning("phase-1,par_id:%d DONE",par_id);
        if (par_id==0) {
          uint32_t local_w = sync_util::sync_logger::computeLocal();
          mako::NFSSync::set_key("noops_phase_"+std::to_string(benchConfig.getShardIndex()), 
                                        std::to_string(local_w).c_str(),
                                        benchConfig.getConfig()->shard(0, benchConfig.getClusterRole()).host.c_str(), 
                                        benchConfig.getConfig()->mports[benchConfig.getClusterRole()]);
          sync_util::sync_logger::update_stable_timestamp(get_epoch()-1, sync_util::sync_logger::retrieveShardW()/10);
#if defined(FAIL_NEW_VERSION)
          mako::NFSSync::set_key("fvw_"+std::to_string(benchConfig.getShardIndex()), 
                                     std::to_string(local_w).c_str(),
                                     benchConfig.getConfig()->shard(0, benchConfig.getClusterRole()).host.c_str(),
                                     benchConfig.getConfig()->mports[benchConfig.getClusterRole()]);
          std::cout<<"set fvw, " << benchConfig.getClusterRole() << ", fvw_"+std::to_string(benchConfig.getShardIndex())<<":"<<local_w<<std::endl;
#endif
          sync_util::sync_logger::reset(); 
        }
      }else {
        CommitInfo commit_info = get_latest_commit_info((char *) log, len);
        timestamp = commit_info.timestamp;  // Store for return value encoding
        
        uint32_t end_time = mako::getCurrentTimeMillis();
        //Warning("In register_for_leader_par_id_return, par_id:%d, slot_id:%d, len:%d, st: %llu, et: %llu, latency: %llu",
        //       par_id, slot_id, len, commit_info.latency_tracker, end_time, end_time-commit_info.latency_tracker);
        sync_util::sync_logger::local_timestamp_[par_id].store(commit_info.timestamp, memory_order_release) ;

#if defined(TRACKING_LATENCY)
        if (par_id==4){
          uint32_t vw = sync_util::sync_logger::computeLocal();
          //Warning("Update here: %llu, before:%llu",vw/10,vw);
          advanceWatermarkTracker.push_back(std::make_pair(vw/10 /* actual watermark */, mako::getCurrentTimeMillis()));
        }
#endif
      }
    }
    // Return timestamp * 10 + status (for safety check compatibility)
    return static_cast<int>(timestamp * 10 + status);
  },
  thread_id);
}

static void setup_paxos_leader_callbacks(vector<pair<uint32_t, uint32_t>>& advanceWatermarkTracker)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
  for (int i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
    register_paxos_leader_callback(advanceWatermarkTracker, i);
  }
}

static void setup_paxos_follower_callbacks(TSharedThreadPoolMbta& replicated_db)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
    for (int i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++) {
      register_paxos_follower_callback(replicated_db, i);
    }
}

static void run_latency_tracking()
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  int leader_config = benchConfig.getLeaderConfig();
#if defined(TRACKING_LATENCY)
  if(leader_config) {
        const auto& advanceWatermarkTracker = benchConfig.getAdvanceWatermarkTracker();
        uint32_t latency_ts = 0;
        std::map<uint32_t, uint32_t> ordered(sample_transaction_tracker.begin(),
                                                           sample_transaction_tracker.end());
        int valid_cnt = 0;
        std::vector<float> latencyVector ;
        for (auto it: ordered) {  // cid => updated time
            int i = 0;
            for (; i < advanceWatermarkTracker.size(); i++) { // G => updated time
                if (advanceWatermarkTracker[i].first >= it.first) break;
            }
            if (i < advanceWatermarkTracker.size() && advanceWatermarkTracker[i].first >= it.first) {
                latency_ts += advanceWatermarkTracker[i].second - it.second;
                latencyVector.emplace_back(advanceWatermarkTracker[i].second - it.second) ;
                valid_cnt++;
                // std::cout << "Transaction: " << it.first << " takes "
                //          << (advanceWatermarkTracker[i].second - it.second) << " ms" 
                //          << ", end_time: " << advanceWatermarkTracker[i].second 
                //          << ", st_time: " << it.second << std::endl;
            } else { // incur only for last several transactions

            }
        }
        if (latencyVector.size() > 0) {
            std::cout << "averaged latency: " << latency_ts / valid_cnt << std::endl;
            std::sort (latencyVector.begin(), latencyVector.end());
            std::cout << "10% latency: " << latencyVector[(int)(valid_cnt *0.1)]  << std::endl;
            std::cout << "50% latency: " << latencyVector[(int)(valid_cnt *0.5)]  << std::endl;
            std::cout << "90% latency: " << latencyVector[(int)(valid_cnt *0.9)]  << std::endl;
            std::cout << "95% latency: " << latencyVector[(int)(valid_cnt *0.95)]  << std::endl;
            std::cout << "99% latency: " << latencyVector[(int)(valid_cnt *0.99)]  << std::endl;
        }
  }
#endif
}

static void wait_for_termination()
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  bool isLearner = benchConfig.getCluster().compare(mako::LEARNER_CENTER)==0 ;

  // Timeout while waiting for end signal from leader.
  // Docker runs can be noticeably slower than local runs, especially during
  // RocksDB-heavy startup/load phases. Keep a conservative default and allow
  // override via MAKO_FOLLOWER_END_WAIT_SECONDS.
  int max_wait_seconds = 180;
  if (const char* env = getenv("MAKO_FOLLOWER_END_WAIT_SECONDS")) {
    char* endptr = nullptr;
    long parsed = strtol(env, &endptr, 10);
    if (endptr != env && *endptr == '\0' && parsed > 0 && parsed <= 3600) {
      max_wait_seconds = static_cast<int>(parsed);
    } else {
      Warning("Invalid MAKO_FOLLOWER_END_WAIT_SECONDS='%s'; using default %d",
              env, max_wait_seconds);
    }
  }
  int wait_count = 0;

  // in case, the Paxos streams on other side is terminated,
  // not need for all no-ops for the final termination
  while (!(benchConfig.getEndReceived() > 0 || benchConfig.getEndReceivedLeader() > 0)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    wait_count++;

    if (isLearner)
      Notice("learner is waiting for being ended: %d/%zu, noops_cnt:%d, replay_batch:%d, wait_time:%ds\n",
             benchConfig.getEndReceived(), benchConfig.getNthreads(),
             sync_util::sync_logger::noops_cnt.load(), benchConfig.getReplayBatch(), wait_count);
    else
      Notice("follower is waiting for being ended: %d/%zu, noops_cnt:%d, replay_batch:%d, wait_time:%ds\n",
             benchConfig.getEndReceived(), benchConfig.getNthreads(),
             sync_util::sync_logger::noops_cnt.load(), benchConfig.getReplayBatch(), wait_count);

    // Timeout check: exit gracefully if we've waited too long
    if (wait_count >= max_wait_seconds) {
      Warning("%s timed out waiting for end signal after %d seconds - exiting gracefully",
              isLearner ? "Learner" : "Follower", max_wait_seconds);
      break;
    }
    //if (benchConfig.getEndReceived() > 0) {std::quick_exit( EXIT_SUCCESS );}
  }

  // Drain the replay backlog before tearing down. The loop above exits on
  // the FIRST partition's END marker, but each partition replays
  // synchronously inside its own delivery callback — the other partitions'
  // batch tails may still be undelivered (on throttled CI runners,
  // hundreds of batches), and watermark-parked entries drain on later
  // deliveries. Also keep printing the counter: the CI harness reads
  // "replay_batch:N" from this log, and going silent here made it
  // snapshot a stale mid-lag value (the shard1Replication mako-dev flake).
  // Exit once the counter stops advancing for a few seconds (drained or
  // genuinely stuck) or the overall budget runs out.
  int drain_stall_limit = 5;
  if (const char* env = getenv("MAKO_FOLLOWER_DRAIN_STALL_SECONDS")) {
    char* endptr = nullptr;
    long parsed = strtol(env, &endptr, 10);
    if (endptr != env && *endptr == '\0' && parsed > 0 && parsed <= 600) {
      drain_stall_limit = static_cast<int>(parsed);
    } else {
      Warning("Invalid MAKO_FOLLOWER_DRAIN_STALL_SECONDS='%s'; using default %d",
              env, drain_stall_limit);
    }
  }
  int drain_max_seconds = 120;
  if (const char* env = getenv("MAKO_FOLLOWER_DRAIN_MAX_SECONDS")) {
    char* endptr = nullptr;
    long parsed = strtol(env, &endptr, 10);
    if (endptr != env && *endptr == '\0' && parsed > 0 && parsed <= 3600) {
      drain_max_seconds = static_cast<int>(parsed);
    } else {
      Warning("Invalid MAKO_FOLLOWER_DRAIN_MAX_SECONDS='%s'; using default %d",
              env, drain_max_seconds);
    }
  }
  int last_replay_batch = benchConfig.getReplayBatch();
  int drain_stall = 0;
  for (int drain_time = 1; drain_time <= drain_max_seconds; drain_time++) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    int rb = benchConfig.getReplayBatch();
    if (rb != last_replay_batch) {
      last_replay_batch = rb;
      drain_stall = 0;
    } else {
      drain_stall++;
    }
    Notice("%s draining replay backlog: %d/%zu ended, replay_batch:%d, stall:%ds, drain_time:%ds\n",
           isLearner ? "learner" : "follower",
           benchConfig.getEndReceived(), benchConfig.getNthreads(),
           rb, drain_stall, drain_time);
    if (drain_stall >= drain_stall_limit) {
      Notice("%s replay drained: replay_batch:%d stable for %ds after %ds\n",
             isLearner ? "learner" : "follower", rb, drain_stall, drain_time);
      break;
    }
  }

  // Track and report latency if configured if tracked
  run_latency_tracking();
}

static void setup_sync_util_callbacks()
{
  // Invoke get_epoch function
  register_sync_util([&]() {
    return BenchmarkConfig::getInstance().getIsReplicated()? get_epoch(): 0;
  });

  // rpc client
  register_sync_util_sc([&]() {
    return BenchmarkConfig::getInstance().getIsReplicated()? get_epoch(): 0;
  });

  // rpc server
  register_sync_util_ss([&]() {
    return BenchmarkConfig::getInstance().getIsReplicated()? get_epoch(): 0;
  });
}


static void setup_transport_callbacks()
{
  // happens on the elected follower-p1, to be the new leader datacenter
  register_fasttransport_for_dbtest([&](int control, int value) {
    Warning("receive a control in register_fasttransport_for_dbtest: %d", control);
    switch (control) {
      case 4: {
        // 1. stop the exchange server on p1 datacenter
        // 2. increase the epoch
        // 3. add no-ops
        // 4. sync the logs
        // 5. start the worker threads 
        // change the membership
        upgrade_p1_to_leader();

        string log = "no-ops:" + to_string(get_epoch());
        auto& benchConfig = BenchmarkConfig::getInstance();
        for(int i = 0; i < benchConfig.getNthreads(); i++){
          add_log_to_nc(log.c_str(), log.size(), i);
        }

        // start the worker threads
        std::lock_guard<std::mutex> lk((sync_util::sync_logger::m));
        sync_util::sync_logger::toLeader = true ;
        std::cout << "notify a new leader is elected!\n" ;
        //sync_util::sync_logger::worker_running = true;
        sync_util::sync_logger::cv.notify_one();

        // terminate the exchange-watermark server
        sync_util::sync_logger::exchange_running = false;
        break;
      }
    }
    return 0;
  });
}

static void setup_leader_election_callbacks()
{
  register_leader_election_callback([&](int control) { // communicate with third party: Paxos
    // happens on the learner for case 0 and case 2, 3
    uint32_t aa = mako::getCurrentTimeMillis();
    Warning("Receive a control command:%d, current ms: %llu", control, aa);
    switch (control) {
#if defined(FAIL_NEW_VERSION) && !defined(MAKO_USE_RAFT)
      case 0: {
        if (janus::is_using_raft()) {
          // Raft: Leader stepped down - no action needed, Raft handles internally
          break;
        }
        std::cout<<"Implement a new fail recovery!"<<std::endl;
        sync_util::sync_logger::exchange_running = false;
        auto& benchConfig = BenchmarkConfig::getInstance();
        sync_util::sync_logger::failed_shard_index = benchConfig.getShardIndex();
        sync_util::sync_logger::client_control(0, benchConfig.getShardIndex()); // in bench.cc register_fasttransport_for_bench
        break;
      }
      case 2: {
        if (janus::is_using_raft()) {
          // Raft: Became leader - no action needed, Raft handles internally
          break;
        }
        // Wait for FVW in the old epoch (w * 10 + epoch); this is very important in our new implementation
        // Single timestamp system: collect all shard watermarks and use maximum
        uint32_t max_watermark = 0;
        auto& benchConfig = BenchmarkConfig::getInstance();
        for (int i=0; i<benchConfig.getNshards(); i++) {
          int clusterRoleLocal = mako::LOCALHOST_CENTER_INT;
          if (i==0)
            clusterRoleLocal = mako::LEARNER_CENTER_INT;
          mako::NFSSync::wait_for_key("fvw_"+std::to_string(i),
                                          benchConfig.getConfig()->shard(0, clusterRoleLocal).host.c_str(), benchConfig.getConfig()->mports[benchConfig.getClusterRole()]);
          std::string w_i = mako::NFSSync::get_key("fvw_"+std::to_string(i),
                                                      benchConfig.getConfig()->shard(0, clusterRoleLocal).host.c_str(),
                                                      benchConfig.getConfig()->mports[clusterRoleLocal]);
          std::cout<<"get fvw, " << clusterRoleLocal << ", fvw_"+std::to_string(i)<<":"<<w_i<<std::endl;
          uint32_t watermark = std::stoi(w_i);
          max_watermark = std::max(max_watermark, watermark);
        }

        sync_util::sync_logger::update_stable_timestamp(get_epoch()-1, max_watermark);
        sync_util::sync_logger::client_control(1, benchConfig.getShardIndex());

        // Start transactions in new epoch
        std::lock_guard<std::mutex> lk((sync_util::sync_logger::m));
        sync_util::sync_logger::toLeader = true ;
        std::cout << "notify a new leader is elected!\n" ;
        //sync_util::sync_logger::worker_running = true;
        sync_util::sync_logger::cv.notify_one();
        auto x0 = std::chrono::high_resolution_clock::now() ;
        break;
      }
#endif
#if !defined(FAIL_NEW_VERSION)
      // for the partial datacenter failure
      case 0: {
        if (janus::is_using_raft()) {
          // Raft: Leader stepped down - no action needed, Raft handles internally
          break;
        }
        // Paxos: 0. stop exchange client + server on the new leader (learner)
        sync_util::sync_logger::exchange_running = false;
        // 1. issue a control command to all other leader partition servers to
        //    1.1 pause other servers DB threads
        //    1.2 config update
        //    1.3 issue no-ops within the old epoch
        //    1.4 start the controller
        auto& benchConfig = BenchmarkConfig::getInstance();
        sync_util::sync_logger::failed_shard_index = benchConfig.getShardIndex();

        auto x0 = std::chrono::high_resolution_clock::now() ;
        sync_util::sync_logger::client_control(0, benchConfig.getShardIndex()); // in bench.cc register_fasttransport_for_bench
        auto x1 = std::chrono::high_resolution_clock::now() ;
        printf("first connection:%d\n",
            std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count());
        break;
      }
      case 2: {// notify that you're the new leader; PREPARE
        if (janus::is_using_raft()) {
          // Raft: Became leader - no action needed, Raft handles internally
          break;
        }
        // Paxos:
        auto& benchConfig = BenchmarkConfig::getInstance();
        sync_util::sync_logger::client_control(1, benchConfig.getShardIndex());
        // wait for Paxos logs replicated
        auto x0 = std::chrono::high_resolution_clock::now() ;
        WAN_WAIT_TIME;
        auto x1 = std::chrono::high_resolution_clock::now() ;
        printf("replicated:%d\n",
            std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count());
        break;
      }
      case 3: {  // COMMIT
        std::lock_guard<std::mutex> lk((sync_util::sync_logger::m));
        sync_util::sync_logger::toLeader = true ;
        std::cout << "notify a new leader is elected!\n" ;
        //sync_util::sync_logger::worker_running = true;
        sync_util::sync_logger::cv.notify_one();
        auto x0 = std::chrono::high_resolution_clock::now() ;
        auto& benchConfig = BenchmarkConfig::getInstance();
        sync_util::sync_logger::client_control(2, benchConfig.getShardIndex());
        auto x1 = std::chrono::high_resolution_clock::now() ;
        printf("second connection:%d\n",
            std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count());
        break;
      }
      // for the datacenter failure, triggered on p1
      case 4: {
        // send a message to all p1 follower nodes within the same datacenter
        auto& benchConfig = BenchmarkConfig::getInstance();
        sync_util::sync_logger::client_control(4, benchConfig.getShardIndex());
        break;
      }
#endif
    }
  });
}

static void cleanup_and_shutdown()
{
  if (BenchmarkConfig::getInstance().getIsReplicated()) { 
    std::this_thread::sleep_for(2s);
    pre_shutdown_step();
    shutdown_paxos();
  }

  sync_util::sync_logger::shutdown();
  //std::quick_exit( EXIT_SUCCESS ); // don't exit early
}

// @safe - Scans config files for "ab: raft" to auto-detect replication type.
// Called before setup() so the dispatcher routes to the correct implementation.
// Only sets the type if the current type is still the default (PAXOS) and
// no explicit --replication flag was provided.
static void detect_replication_type_from_config(const vector<string>& config_files) {
  // Don't override explicit CLI setting
  if (janus::is_using_raft()) {
    return;
  }

  for (const auto& file_path : config_files) {
    // @unsafe { file I/O }
    std::ifstream ifs(file_path);
    if (!ifs.is_open()) {
      continue;
    }
    std::string line;
    while (std::getline(ifs, line)) {
      // Look for "ab:" field in YAML - match patterns like "ab: raft", "ab:raft"
      auto pos = line.find("ab:");
      if (pos != std::string::npos) {
        auto value = line.substr(pos + 3);
        // Trim leading whitespace
        auto start = value.find_first_not_of(" \t");
        if (start != std::string::npos) {
          value = value.substr(start);
        }
        // Trim trailing whitespace and comments
        auto end = value.find_first_of(" \t#\r\n");
        if (end != std::string::npos) {
          value = value.substr(0, end);
        }
        if (value == "raft" || value == "fpga_raft") {
          Notice("Auto-detected replication type '%s' from config file: %s",
                 value.c_str(), file_path.c_str());
          janus::set_replication_type(janus::ReplicationType::RAFT);
          return;
        }
      }
    }
  }
}

static char** prepare_paxos_args(const vector<string>& paxos_config_file,
  const string paxos_proc_name, int& argc_out)
{
  // Support variable number of config files (typically 2 or 3)
  // Base args: 14 args + 2 args per config file
  int num_configs = paxos_config_file.size();
  int argc_paxos = 14 + (num_configs * 2);
  argc_out = argc_paxos;

  int kPaxosBatchSize = 50000;
  char **argv_paxos = new char*[argc_paxos];
  int i = 0;

  argv_paxos[i++] = (char *) "";
  argv_paxos[i++] = (char *) "-b";
  argv_paxos[i++] = (char *) "-d";
  argv_paxos[i++] = (char *) "60";

  // Add all config files
  for (int k = 0; k < num_configs; k++) {
    argv_paxos[i++] = (char *) "-f";
    argv_paxos[i++] = (char *) paxos_config_file[k].c_str();
  }

  argv_paxos[i++] = (char *) "-t";
  argv_paxos[i++] = (char *) "30";
  argv_paxos[i++] = (char *) "-T";
  argv_paxos[i++] = (char *) "100000";
  argv_paxos[i++] = (char *) "-n";
  argv_paxos[i++] = (char *) "32";
  argv_paxos[i++] = (char *) "-P";
  argv_paxos[i++] = (char *) paxos_proc_name.c_str();
  argv_paxos[i++] = (char *) "-A";
  argv_paxos[i] = new char[20];
  memset(argv_paxos[i], '\0', 20);
  sprintf(argv_paxos[i], "%d", kPaxosBatchSize);

  return argv_paxos;
}

static abstract_db * init_env() {
  auto& benchConfig = BenchmarkConfig::getInstance();

  // Setup callbacks
  setup_sync_util_callbacks();

  if (BenchmarkConfig::getInstance().getIsReplicated()) {
    // We need replicated_db keep live for future replay!
    static TSharedThreadPoolMbta replicated_db(benchConfig.getNthreads() + 1);
    abstract_db *db = replicated_db.getDBWrapper(benchConfig.getNthreads())->getDB () ;
    db->init() ;

    // failures handling callbacks
    setup_transport_callbacks();
    setup_leader_election_callbacks();


    // Auto-detect replication type from config files before dispatching setup().
    // This ensures dbtest uses the Raft code path when config says "ab: raft",
    // even if --replication raft wasn't explicitly passed on the command line.
    detect_replication_type_from_config(benchConfig.getPaxosConfigFile());

    int argc_paxos = 0;
    char** argv_paxos = prepare_paxos_args(benchConfig.getPaxosConfigFile(), benchConfig.getPaxosProcName(), argc_paxos);
    std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
    if (ret.empty()) {
      Warning("paxos args errors");
      return db;
    }

    // Setup Paxos callbacks have to be after setup() is called
    setup_paxos_leader_callbacks(benchConfig.getAdvanceWatermarkTracker());
    setup_paxos_follower_callbacks(replicated_db);

    int ret2 = setup2(0, benchConfig.getShardIndex());
    sleep(3); // ensure that all get started

    // Wire the cluster-config read path (shard-0 config service +
    // per-node ConfigWatcher into the routing cache). No-op unless
    // MAKO_CLUSTER_CONFIG=1 and the cluster has >1 shard.
    // @unsafe { RPC I/O, storage index open, background thread }
    janus::BootstrapClusterConfig(db);

#ifndef DISABLE_DISK
    // Initialize RocksDB persistence layer ONLY on the leader
    // Followers and learners don't need RocksDB since they only replay, not generate logs
    if (benchConfig.getIsReplicated() && benchConfig.getLeaderConfig()) {
      auto& persistence = mako::RocksDBPersistence::getInstance();
      // Add username prefix to avoid conflicts when multiple users run on the same server
      std::string username = util::get_current_username();
      std::string db_path = "/tmp/" + username + "_mako_rocksdb_shard" + std::to_string(benchConfig.getShardIndex())
                            + "_leader_pid" + std::to_string(getpid());
      size_t num_partitions = benchConfig.getNthreads();
      size_t num_threads = num_partitions;
      uint32_t shard_id = benchConfig.getShardIndex();
      uint32_t num_shards = benchConfig.getNshards();

      fprintf(stderr, "Leader initializing RocksDB at path: %s with %zu partitions and %zu worker threads\n",
              db_path.c_str(), num_partitions, num_threads);
      if (!persistence.initialize(db_path, num_partitions, num_threads, shard_id, num_shards)) {
          fprintf(stderr, "WARNING: RocksDB initialization failed for %s\n", db_path.c_str());
      } else {
          // Write initial metadata and set epoch
          persistence.writeMetadata(shard_id, num_shards);
          persistence.setEpoch(get_epoch());
      }
    }
#else
    // Disk persistence disabled by DISABLE_DISK flag
    if (benchConfig.getIsReplicated() && benchConfig.getLeaderConfig()) {
      fprintf(stderr, "Disk persistence disabled by DISABLE_DISK flag\n");
    }
#endif

    // start a failure monitor on learners
    if (benchConfig.getCluster().compare(mako::LEARNER_CENTER)==0) { // learner cluster
      abstract_db * db = replicated_db.getDBWrapper(benchConfig.getNthreads())->getDB () ;
      bench_runner *r = start_workers_tpcc(1, db, benchConfig.getNthreads(), true);
      modeMonitor(db, benchConfig.getNthreads(), r) ;
    }
    return db;
  }
  return nullptr;
}

static void send_end_signal() {
  if (BenchmarkConfig::getInstance().getIsReplicated()) {
    Warning("######--------------###### send endLlogs #####---------------######");
    std::string endLogInd = "";
    for (int i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++)
        add_log_to_nc((char *)endLogInd.c_str(), 0, i);

    // vector<std::thread> wait_threads;
    // for (int i = 0; i < BenchmarkConfig::getInstance().getNthreads(); i++)
    // {
    //     wait_threads.push_back(std::thread([i]() {
    //        std::cout << "starting wait for par_id: " << i << std::endl;
    //        wait_for_submit(i); 
    //     }));
    // }
    // for (auto &th : wait_threads)
    // {
    //     th.join();
    // }
  }
}

static void db_close() {
  auto& benchConfig = BenchmarkConfig::getInstance();
  if (benchConfig.getLeaderConfig() && benchConfig.getIsReplicated()) {
    send_end_signal();
    // Give followers/learners time to receive and process the end signal
    // before we start cleanup. This prevents race condition where leader
    // shuts down Paxos before end signal propagates.
    Notice("Leader sent end signal, waiting 3 seconds for propagation...");
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }

  mako::stop_helper();

  // Stop multi-shard transports if running
  stopMultiShardTransports();

  // Wait for termination if not a leader
  if (!benchConfig.getLeaderConfig()) {
    wait_for_termination();
  }

  // Cleanup and shutdown
  if (benchConfig.getIsReplicated())
    cleanup_and_shutdown();
}

#endif
