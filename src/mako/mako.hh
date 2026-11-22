#ifndef _MAKO_COMMON_H_
#define _MAKO_COMMON_H_

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <utility>
#include <string>
#include <set>
#include "rocksdb_persistence_fwd.h"

#include <getopt.h>
#include <stdio.h>
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
#if defined(MAKO_RUST_STO_TPCC)
#include "storage/rust_sto_tpcc_wrapper.hh"
#endif
#include "benchmarks/common.h"
#include "benchmarks/common2.h"
#include "benchmarks/benchmark_config.h"
#include "benchmarks/rpc_setup.h"

// Runtime replication switching - unified interface
#include "deptran/replication_helper.h"
#include "deptran/config.h"
// deptran/config.h pulls in deptran/constants.h, whose object-like SUCCESS
// macro would rewrite mako::ErrorCode::SUCCESS in any Mako header first
// included after this point (the Rust line's simpleTransactionRep build broke
// that way). Nothing reachable from here uses the deptran macro, so drop it.
#ifdef SUCCESS
#undef SUCCESS
#endif

// Cluster-config runtime bootstrap (shard-0 config service + watcher).
#include "cluster_bootstrap.h"

#include "lib/configuration.h"
#include "lib/fasttransport.h"
#include "lib/common.h"
#include "lib/server.h"
#include "lib/rust_wrapper.h"

import rusty;

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
  cerr << "  storage_engine : " << benchConfig.getStorageEngine() << endl;
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

// @unsafe - Construct the configured backend at the native ownership boundary.
static abstract_db* make_benchmark_db() {
  const auto &engine = BenchmarkConfig::getInstance().getStorageEngine();
#if defined(MAKO_RUST_STO_TPCC)
  if (engine == "rust")
    return new rust_sto_tpcc_wrapper;
#endif
  if (engine != "cpp")
    throw std::runtime_error("unsupported storage engine: " + engine);
  return new mbta_wrapper;
}

// Initialize database for a specific shard (multi-shard mode)
// This allows creating isolated database instances for each shard
static abstract_db* initShardDB(int shard_idx, bool is_leader, const std::string& cluster_role) {
  auto& benchConfig = BenchmarkConfig::getInstance();

  Notice("Initializing database for shard %d (cluster: %s, leader: %d)",
         shard_idx, cluster_role.c_str(), is_leader);

  // Create and initialize database instance for this shard
  abstract_db *db = make_benchmark_db();
  db->init();

  return db;
}

// @unsafe - Per-shard RPC servers are started after the table catalogs exist.
// They bind W+5+alpha, never the worker's base_port+partition listener.
static bool initMultiShardTransports(const std::vector<int>& local_shard_indices) {
  auto& cfg = BenchmarkConfig::getInstance();
  if (!cfg.getConfig() || local_shard_indices.empty())
    return false;
  static bool initialized = false;
  if (initialized)
    return true;
  initialized = true;
  const int streams = cfg.getNshards() * cfg.getNthreads();
  sync_util::sync_logger::local_timestamp_ =
      std::vector<std::atomic<uint32_t>>(streams);
#ifndef DISABLE_DISK
  sync_util::sync_logger::disk_timestamp_ =
      std::vector<std::atomic<uint32_t>>(streams);
#endif
  sync_util::sync_logger::Init(
      cfg.getShardIndex(), cfg.getNshards(), streams,
      cfg.getLeaderConfig(), cfg.getCluster(), cfg.getConfig());
  for (int shard = 0; shard < cfg.getNshards(); ++shard) {
    if (std::find(local_shard_indices.begin(), local_shard_indices.end(), shard)
        != local_shard_indices.end())
      continue;
    for (int partition = 0; partition < cfg.getNthreads(); ++partition) {
      const int slot = shard * cfg.getNthreads() + partition;
      sync_util::sync_logger::local_timestamp_[slot].store(UINT32_MAX);
#ifndef DISABLE_DISK
      sync_util::sync_logger::disk_timestamp_[slot].store(UINT32_MAX);
#endif
    }
  }
  return true;
}


static constexpr const char *kAllocatorMemoryEnvironment =
    "MAKO_TPCC_ALLOCATOR_MEMORY";

static size_t tpcc_allocator_memory_bytes() {
  static constexpr const char *kEnvironment = kAllocatorMemoryEnvironment;
  static constexpr const char *kDefault = "1G";
  const char *configured = getenv(kEnvironment);
  const std::string spec = configured == nullptr ? kDefault : configured;

  const bool has_suffix =
      !spec.empty() &&
      (spec.back() == 'K' || spec.back() == 'M' || spec.back() == 'G');
  const size_t digit_count = spec.size() - static_cast<size_t>(has_suffix);
  if (digit_count == 0 || spec.front() == '0') {
    throw std::runtime_error("invalid " + std::string(kEnvironment) + ": " +
                             spec);
  }

  size_t amount = 0;
  for (size_t index = 0; index < digit_count; ++index) {
    const char digit = spec[index];
    if (digit < '0' || digit > '9') {
      throw std::runtime_error("invalid " + std::string(kEnvironment) + ": " +
                               spec);
    }
    const size_t value = static_cast<size_t>(digit - '0');
    if (amount > (std::numeric_limits<size_t>::max() - value) / 10) {
      throw std::runtime_error(std::string(kEnvironment) +
                               " exceeds size_t: " + spec);
    }
    amount = amount * 10 + value;
  }

  size_t multiplier = 1;
  if (has_suffix) {
    multiplier = static_cast<size_t>(1) << (spec.back() == 'G'   ? 30
                                            : spec.back() == 'M' ? 20
                                                                 : 10);
  }
  if (amount > std::numeric_limits<size_t>::max() / multiplier) {
    throw std::runtime_error(std::string(kEnvironment) +
                             " exceeds size_t: " + spec);
  }
  return amount * multiplier;
}

// @unsafe - Configure the selected native runtime before any loader pins an arena.
static void initialize_benchmark_allocator() {
  const auto& benchConfig = BenchmarkConfig::getInstance();
  const size_t numa_memory = tpcc_allocator_memory_bytes();
  if (numa_memory > 0) {
    const size_t nthreads = benchConfig.getNthreads();
    // Validate the rounded per-worker share before native allocator state exists.
    const size_t maxpercpu = ::allocator::CheckedPerWorkerCapacity(
        numa_memory, nthreads, ::allocator::GetHugepageSize(),
        kAllocatorMemoryEnvironment);
    ::allocator::Initialize(nthreads, maxpercpu);
  }
}

// @unsafe - Single-shard startup through native allocation and database bridges.
static abstract_db* initWithDB() {
  auto& benchConfig = BenchmarkConfig::getInstance();

  initialize_benchmark_allocator();

  // Print system information
  print_system_info();

  if (!benchConfig.getIsReplicated()) {
  sync_util::sync_logger::Init(benchConfig.getShardIndex(), benchConfig.getNshards(),
                               benchConfig.getNthreads(),
                               benchConfig.getLeaderConfig()==1, /* is leader */
                               benchConfig.getCluster(),
                               benchConfig.getConfig());
  }

  abstract_db *db = make_benchmark_db(); // on the leader replica
  db->init() ;
  return db;
}

// @unsafe - Replication IDs remain global on the wire; backend TLS is shard-local.
static int select_replication_callback_partition(int partition) {
  auto& cfg = BenchmarkConfig::getInstance();
  const int shard = cfg.usesGlobalReplicationPartitions()
      ? partition / cfg.getNthreads() : cfg.getShardIndex();
  const int local = partition % cfg.getNthreads();
  BenchmarkConfig::setThreadLocalShardIndex(shard);
  TThread::set_shard_index(shard);
  TThread::set_pid(local);
  TThread::set_nshards(cfg.getNshards());
  return cfg.getReplicationWatermarkPartition(shard, local);
}

// @unsafe - Process-owned state outlives every registered replication callback.
// Callbacks publish the atomic terminal bit; the main thread owns peer handshakes.
struct ReplicaDrainState {
  int partition;
  mako::NFSSync::DrainHandshake handshake;
  rusty::Vec<mako::NFSSync::DrainHandshake::Peer> peers;
  rusty::sync::atomic::AtomicBool ended{false};

  // @safe - Reserve the configured peers once, outside transaction processing.
  ReplicaDrainState(int partition_id, size_t peer_count)
      : partition(partition_id),
        peers(rusty::Vec<mako::NFSSync::DrainHandshake::Peer>::with_capacity(peer_count)) {}
};

// @safe - Initialized before callback registration, immutable membership afterward.
static auto &replica_drains() {
  static rusty::Vec<rusty::Box<ReplicaDrainState>> states;
  return states;
}

// @unsafe - Announce every hosted stream before admitting benchmark work.
static void initialize_replica_drains() {
  const auto &config = BenchmarkConfig::getInstance();
  auto *replication = janus::Config::GetConfig();
  const auto sites = replication->GetMyServers();
  if (sites.empty())
    Panic("replication drain requires at least one hosted stream");
  auto &states = replica_drains();
  states = rusty::Vec<rusty::Box<ReplicaDrainState>>::with_capacity(sites.size());
  for (const auto &site : sites) {
    const auto members = replication->SiteIdsByPartitionId(site.partition_id_);
    auto state = rusty::make_box<ReplicaDrainState>(site.partition_id_, members.size() - 1);
    const int shard = config.usesGlobalReplicationPartitions()
        ? site.partition_id_ / config.getNthreads() : config.getShardIndex();
    state->handshake.announce(mako::NFSSync::drain_prefix(config) + "replica_" +
        std::to_string(shard) + "_" +
        std::to_string(site.partition_id_ % config.getNthreads()) + "_",
        std::to_string(site.id));
    for (int member : members) {
      if (member != site.id) {
        mako::NFSSync::DrainHandshake::Peer peer;
        state->handshake.bind(peer, std::to_string(member));
        state->peers.push(std::move(peer));
      }
    }
    states.push(std::move(state));
  }
}

// @safe - Borrowed only by callbacks; state survives replication worker shutdown.
static const ReplicaDrainState &replica_drain_for_partition(int partition) {
  for (const auto &state : replica_drains()) {
    if (state->partition == partition)
      return *state;
  }
  Panic("missing replica drain state for partition %d", partition);
}

// @unsafe - Registered Paxos/Raft callbacks bridge bytes to an affine worker.
static void register_paxos_follower_callback(TSharedThreadPoolMbta& replicated_db, int thread_id)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
  auto &terminal = replica_drain_for_partition(thread_id);
  register_for_follower_par_id_return([&,thread_id](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>> & un_replay_logs_) {
    auto& benchConfig = BenchmarkConfig::getInstance();
    par_id = select_replication_callback_partition(par_id);
    //Warning("receive a register_for_follower_par_id_return, par_id:%d, slot_id:%d,len:%d",par_id, slot_id,len);
    int status = mako::PaxosStatus::STATUS_INIT;
    uint32_t timestamp = 0;  // Track timestamp for return value encoding
    abstract_db * db = replicated_db.getDBWrapper(
        par_id % benchConfig.getNthreads())->getDB();
    bool noops = false;

    if (len==mako::ADVANCER_MARKER_NUM) { // start a advancer
      status = mako::PaxosStatus::STATUS_REPLAY_DONE;
      if (par_id==0 && benchConfig.getNshards() > 1){
        // An apply thread must not format through std::cout: see the note in
        // register_paxos_leader_callback().
        // @unsafe { stdio is not borrow-checked }
        {
          ::fputs("we can start a advancer\n", stdout);
          ::fflush(stdout);
        }
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
        static thread_local mako::ReplayLogView decoded_replay_log;
        if (!mako::parse_replay_log(log, len, decoded_replay_log)) {
          Panic("malformed follower replay log: len=%d", len);
        }
        // @unsafe - Reject a mixed/old topology before replay or watermark effects.
        for (const auto& record : decoded_replay_log.records) {
          if ((record.table_id - 1) / mako::NUM_TABLES_PER_SHARD !=
              benchConfig.getShardIndex())
            Panic("replication stream received a foreign-shard table: %u",
                  unsigned(record.table_id));
        }
        // Floor the selected backend before publishing watermarks or queueing
        // the log. Promotion may occur before a safety-delayed batch is drained.
        db->observe_replay_timestamp(decoded_replay_log.recovered_time_term);
        CommitInfo commit_info{
          decoded_replay_log.latest_time_term,
          decoded_replay_log.latency_tracker};
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
          replay_validated_mbta_v2(decoded_replay_log, db);
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
        // END is submitted only after every cross-shard coordinator drained:
        // no speculative transaction remains unresolved, so finish the queued
        // committed prefix even if the asynchronous watermark is still behind.
        if (len == 0 || sync_util::sync_logger::safety_check(std::get<0>(it), w)) {
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
    if (len == 0 && !terminal.ended.swap(true, rusty::sync::atomic::Ordering::AcqRel))
      benchConfig.incrementEndReceived();
    // Return timestamp * 10 + status (for safety check compatibility)
    return static_cast<int>(timestamp * 10 + status);
  }, 
  thread_id);
}



static void register_paxos_leader_callback(vector<pair<uint32_t, uint32_t>>& advanceWatermarkTracker, int thread_id)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
  auto &terminal = replica_drain_for_partition(thread_id);
  register_for_leader_par_id_return([&,thread_id](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<int, int, int, int, const char *>> & un_replay_logs_) {
    auto& benchConfig = BenchmarkConfig::getInstance();
    par_id = select_replication_callback_partition(par_id);
    //Warning("receive a register_for_leader_par_id_return, par_id:%d, slot_id:%d,len:%d",par_id, slot_id,len);
    int status = mako::PaxosStatus::STATUS_NORMAL;
    uint32_t timestamp = 0;  // Track timestamp for return value encoding
    bool noops = false;

    if (len==mako::ADVANCER_MARKER_NUM) { // start a advancer
      status = mako::PaxosStatus::STATUS_REPLAY_DONE;
      if (par_id==0){
        // This runs on a replication apply thread while benchmark threads
        // print through std::cout, whose formatting state (width, fill) every
        // operator<< rewrites without a lock. One stdio call writes the same
        // bytes and touches none of that state.
        // @unsafe { stdio is not borrow-checked }
        {
          ::fputs("we can start a advancer\n", stdout);
          ::fflush(stdout);
        }
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
      if (!un_replay_logs_.empty())
        Panic("leader ended with unresolved replay for partition %d", thread_id);
      if (!terminal.ended.swap(true, rusty::sync::atomic::Ordering::AcqRel))
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
        static thread_local mako::ReplayLogView decoded_replay_log;
        if (!mako::parse_replay_log(log, len, decoded_replay_log)) {
          Panic("malformed leader replay log: len=%d", len);
        }
        for (const auto& record : decoded_replay_log.records) {
          if ((record.table_id - 1) / mako::NUM_TABLES_PER_SHARD !=
              benchConfig.getShardIndex())
            Panic("replication stream submitted a foreign-shard table: %u",
                  unsigned(record.table_id));
        }
        CommitInfo commit_info{
          decoded_replay_log.latest_time_term,
          decoded_replay_log.latency_tracker};
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
  for (const auto& site : janus::Config::GetConfig()->GetMyServers()) {
    register_paxos_leader_callback(advanceWatermarkTracker, site.partition_id_);
  }
}

static void setup_paxos_follower_callbacks(TSharedThreadPoolMbta& replicated_db)
{
  if (!BenchmarkConfig::getInstance().getIsReplicated()) { return ; }
    for (const auto& site : janus::Config::GetConfig()->GetMyServers()) {
      register_paxos_follower_callback(replicated_db, site.partition_id_);
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

// @unsafe - Keep replication services alive until every hosted stream and peer
// has applied its terminal prefix. Missing peers fail closed, never time out OK.
static void wait_for_termination()
{
  auto& benchConfig = BenchmarkConfig::getInstance();

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
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::seconds(max_wait_seconds);
  auto next_notice = std::chrono::steady_clock::now();
  for (;;) {
    size_t ended = 0;
    size_t total = 0;
    for (const auto &state : replica_drains()) {
      ++total;
      if (state->ended.load(rusty::sync::atomic::Ordering::Acquire))
        ++ended;
    }
    bool all_peers_drained = ended == total;
    if (all_peers_drained) {
      for (auto &state : replica_drains()) {
        for (auto &peer : state->peers) {
          if (!state->handshake.acknowledge(peer))
            all_peers_drained = false;
        }
      }
    }
    if (all_peers_drained) {
      Notice("replication drained: %zu/%zu ended, replay_batch:%d",
             ended, total, benchConfig.getReplayBatch());
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      Panic("replication drain timed out: %zu/%zu ended, replay_batch:%d",
            ended, total, benchConfig.getReplayBatch());
    if (now >= next_notice) {
      Notice("replication waiting: %zu/%zu ended, replay_batch:%d",
             ended, total, benchConfig.getReplayBatch());
      next_notice = now + std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
          if (!add_log_to_nc(log.c_str(), log.size(),
                  benchConfig.getReplicationPartition(benchConfig.getShardIndex(), i)))
            Panic("replication rejected required epoch marker");
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
        sync_util::sync_logger::failed_shard_index.store(
            static_cast<int>(benchConfig.getShardIndex()),
            std::memory_order_relaxed);
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
        sync_util::sync_logger::failed_shard_index.store(
            static_cast<int>(benchConfig.getShardIndex()),
            std::memory_order_relaxed);

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
    pre_shutdown_step();
    shutdown_paxos();
  }
#ifndef DISABLE_DISK
  // @unsafe - Flush and close while RocksDB's background pools are still alive.
  // Their process-exit destructors can run before our singleton destructor.
  mako::RocksDBPersistence::getInstance().shutdown();
#endif

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
        if (value == "raft") {
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

// @unsafe - Initialize storage before registering any replication callbacks.
static abstract_db * init_env() {
  auto& benchConfig = BenchmarkConfig::getInstance();

  // Setup callbacks
  setup_sync_util_callbacks();

  if (BenchmarkConfig::getInstance().getIsReplicated()) {
    // Replay, follower serving and promotion share the actual selected backend.
    // The process-lifetime database outlives callbacks and their TLS workers.
    abstract_db* db = make_benchmark_db();
    db->init();
    ThreadDBWrapperMbta::replay_thread_wrapper_db = db;
    static TSharedThreadPoolMbta replicated_db(benchConfig.getNthreads() + 1);

    // failures handling callbacks
    setup_transport_callbacks();
    setup_leader_election_callbacks();


    // Auto-detect replication type from config files before dispatching setup().
    // This ensures dbtest uses the Raft code path when config says "ab: raft",
    // even if --replication raft wasn't explicitly passed on the command line.
    detect_replication_type_from_config(benchConfig.getPaxosConfigFile());

    // Every peer in a colocated deployment loads the same ordered topology.
    // Ownership is a filter, never a renumbering of partition/site IDs.
    janus::Config::mako_partitions_per_shard_ = benchConfig.getNthreads();
    janus::Config::mako_shard_count_ = benchConfig.getNshards();
    janus::Config::mako_hosted_shards_ =
        benchConfig.getConfig()->multi_shard_mode
        ? benchConfig.getConfig()->local_shard_indices
        : std::vector<int>{static_cast<int>(benchConfig.getShardIndex())};

    int argc_paxos = 0;
    char** argv_paxos = prepare_paxos_args(benchConfig.getPaxosConfigFile(), benchConfig.getPaxosProcName(), argc_paxos);
    std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
    if (ret.empty()) {
      Warning("paxos args errors");
      return db;
    }
    benchConfig.setGlobalReplicationPartitions(
        janus::Config::GetConfig()->GetNumPartition() > benchConfig.getNthreads());
    initialize_replica_drains();

    // Setup Paxos callbacks have to be after setup() is called
    setup_paxos_leader_callbacks(benchConfig.getAdvanceWatermarkTracker());
    setup_paxos_follower_callbacks(replicated_db);

    if (benchConfig.getConfig()->multi_shard_mode) {
      if (!initMultiShardTransports(benchConfig.getConfig()->local_shard_indices))
        throw std::runtime_error("cannot initialize colocated replication");
    } else {
      sync_util::sync_logger::local_timestamp_ =
          std::vector<std::atomic<uint32_t>>(benchConfig.getNthreads());
#ifndef DISABLE_DISK
      sync_util::sync_logger::disk_timestamp_ =
          std::vector<std::atomic<uint32_t>>(benchConfig.getNthreads());
#endif
      sync_util::sync_logger::Init(
          benchConfig.getShardIndex(), benchConfig.getNshards(),
          benchConfig.getNthreads(), benchConfig.getLeaderConfig(),
          benchConfig.getCluster(), benchConfig.getConfig());
    }

    int ret2 = setup2(0, benchConfig.getShardIndex());
    sleep(3); // ensure that all get started

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
    for (const auto& site : janus::Config::GetConfig()->GetMyServers()) {
      if (!add_log_to_nc(endLogInd.c_str(), 0, site.partition_id_))
        throw std::runtime_error("replication rejected required end marker");
    }
  }
}

static void db_close() {
  auto& benchConfig = BenchmarkConfig::getInstance();
  // @unsafe - no native queue, deferred reply, or watcher may outlive the DB.
  janus::ShutdownClusterConfig();
  if (benchConfig.getLeaderConfig() && benchConfig.getIsReplicated()) {
    send_end_signal();
  }

  if (benchConfig.getIsReplicated())
    wait_for_termination();

  mako::stop_all_rpc_servers();

  // Cleanup and shutdown
  if (benchConfig.getIsReplicated())
    cleanup_and_shutdown();
}

#endif
