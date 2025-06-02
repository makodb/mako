#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <utility>
#include <string>
#include <set>

#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sysinfo.h>

#include "../allocator.h"
#include "../stats_server.h"
#include "bench.h"
#include "sto/sync_util.hh"
#include "mbta_wrapper.hh"
#include "deptran/s_main.h"
#include "benchmarks/common.h"
#include "common2.h"
#include "common3.h"
#include "lib/common.h"
#include "lib/configuration.h"
#include "lib/fasttransport.h"
#include "lib/common.h"
#include "lib/server.h"

//#define PAXOS_LIB_ENABLED 1
using namespace std;
using namespace util;

INIT_SYNC_UTIL_VARS

static size_t
parse_memory_spec(const string &s)
{
  string x(s);
  size_t mult = 1;
  if (x.back() == 'G') {
    mult = static_cast<size_t>(1) << 30;
    x.pop_back();
  } else if (x.back() == 'M') {
    mult = static_cast<size_t>(1) << 20;
    x.pop_back();
  } else if (x.back() == 'K') {
    mult = static_cast<size_t>(1) << 10;
    x.pop_back();
  }
  return strtoul(x.c_str(), nullptr, 10) * mult;
}

int
main(int argc, char **argv)
{
  abstract_db *db = NULL;
  //void (*test_fn)(abstract_db *, int argc, char **argv) = NULL;
  string bench_type = "tpcc";
  string db_type = "mbta";
  char *curdir = get_current_dir_name();
  string basedir = curdir;
  string bench_opts;
  size_t numa_memory = 0;
  free(curdir);
  int saw_run_spec = 0;
  int nofsync = 0;
  int do_compress = 0;
  int fake_writes = 0;
  int disable_gc = 0;
  int disable_snapshots = 0;
  vector<string> logfiles;
  vector<vector<unsigned>> assignments;
  string stats_server_sockfile;

  int leader_config = 0;
  //int kSTOBatchSize = 1000;
  int kPaxosBatchSize = 50000;
  vector<string> paxos_config_file{};
  string paxos_proc_name = srolis::LOCALHOST_CENTER;
  static std::atomic<int> end_received(0);
  static std::atomic<int> end_received_leader(0);

  // tracking vectorized watermark
  std::vector<std::pair<uint32_t, uint32_t>> advanceWatermarkTracker;  // local watermark -> time

  while (1) {
    static struct option long_options[] =
    {
      {"verbose"                    , no_argument       , &verbose                   , 1}   ,
      {"parallel-loading"           , no_argument       , &enable_parallel_loading   , 1}   ,
      {"pin-cpus"                   , no_argument       , &pin_cpus                  , 1}   ,
      {"slow-exit"                  , no_argument       , &slow_exit                 , 1}   ,
      {"retry-aborted-transactions" , no_argument       , &retry_aborted_transaction , 1}   ,
      {"backoff-aborted-transactions" , no_argument     , &backoff_aborted_transaction , 1}   ,
      {"bench"                      , required_argument , 0                          , 'b'} ,
      {"scale-factor"               , required_argument , 0                          , 's'} ,
      {"num-threads"                , required_argument , 0                          , 't'} ,
      {"num-erpc-server"            , required_argument , 0                          , 'e'} ,
      {"shard-index"                , required_argument , 0                          , 'g'} ,
      {"shard-config"               , required_argument , 0                          , 'q'} ,
      {"db-type"                    , required_argument , 0                          , 'd'} ,
      {"basedir"                    , required_argument , 0                          , 'B'} ,
      {"txn-flags"                  , required_argument , 0                          , 'f'} ,
      {"runtime"                    , required_argument , 0                          , 'r'} ,
      {"ops-per-worker"             , required_argument , 0                          , 'n'} ,
      {"bench-opts"                 , required_argument , 0                          , 'o'} ,
      {"numa-memory"                , required_argument , 0                          , 'm'} , // implies --pin-cpus
      {"logfile"                    , required_argument , 0                          , 'l'} ,
      {"assignment"                 , required_argument , 0                          , 'a'} ,
      {"log-nofsync"                , no_argument       , &nofsync                   , 1}   ,
      {"log-compress"               , no_argument       , &do_compress               , 1}   ,
      {"log-fake-writes"            , no_argument       , &fake_writes               , 1}   ,
      {"disable-gc"                 , no_argument       , &disable_gc                , 1}   ,
      {"disable-snapshots"          , no_argument       , &disable_snapshots         , 1}   ,
      {"stats-server-sockfile"      , required_argument , 0                          , 'x'} ,
      {"no-reset-counters"          , no_argument       , &no_reset_counters         , 1}   ,
      {"use-hashtable"		          , no_argument	,     &use_hashtable	             , 1}   ,
      {"paxos-config"               , required_argument , 0                          , 'F'} ,
      //{"sto-batch-size"             , optional_argument , 0                          , 'S'},
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "b:s:t:d:B:f:r:n:o:m:l:a:x:g:q:F:P:S", long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 0:
      if (long_options[option_index].flag != 0)
        break;
      abort();
      break;

    case 'b':
      bench_type = optarg;
      break;

    case 's':
      scale_factor = strtod(optarg, NULL);
      ALWAYS_ASSERT(scale_factor > 0.0);
      break;

    // case 'S':
    //   kSTOBatchSize = strtoul(optarg, NULL, 10);
    //   break;

    case 't':
      nthreads = strtoul(optarg, NULL, 10);
      ALWAYS_ASSERT(nthreads > 0);
      break;

    case 'g':
      shardIndex = strtoul(optarg, NULL, 10);
      ALWAYS_ASSERT(shardIndex >= 0);
      break;

    case 'P':
      paxos_proc_name = string(optarg);
      cluster = paxos_proc_name;
      clusterRole = srolis::convertCluster(cluster);
      if (cluster.compare(srolis::LOCALHOST_CENTER) == 0) leader_config = 1;
      break;
    
    case 'q':
      config = new transport::Configuration(optarg);
      nshards = config->nshards;
      break;

    case 'd':
      db_type = optarg;
      break;

    case 'B':
      basedir = optarg;
      break;

    case 'f':
      txn_flags = strtoul(optarg, NULL, 10);
      break;

    case 'F':
      paxos_config_file.push_back(optarg);
      break;

    case 'r':
      ALWAYS_ASSERT(!saw_run_spec);
      saw_run_spec = 1;
      runtime = strtoul(optarg, NULL, 10);
      ALWAYS_ASSERT(runtime > 0);
      run_mode = RUNMODE_TIME;
      break;

    case 'n':
      ALWAYS_ASSERT(!saw_run_spec);
      saw_run_spec = 1;
      ops_per_worker = strtoul(optarg, NULL, 10);
      ALWAYS_ASSERT(ops_per_worker > 0);
      run_mode = RUNMODE_OPS;

    case 'o':
      bench_opts = optarg;
      break;

    case 'm':
      {
        pin_cpus = 1;
        const size_t m = parse_memory_spec(optarg);
        ALWAYS_ASSERT(m > 0);
        numa_memory = m;
      }
      break;

    case 'l':
      logfiles.emplace_back(optarg);
      break;

    case 'a':
      assignments.emplace_back(
          ParseCSVString<unsigned, RangeAwareParser<unsigned>>(optarg));
      break;

    case 'x':
      stats_server_sockfile = optarg;
      break;

    case 'e':
      num_erpc_server = strtoul(optarg, NULL, 10);
      break;

    case '?':
      /* getopt_long already printed an error message. */
      exit(1);

    default:
      abort();
    }
  }

  verbose = 1;

  if (bench_type == "tpcc") {
    workload_type = 1;
  }

  if (do_compress && logfiles.empty()) {
    cerr << "[ERROR] --log-compress specified without logging enabled" << endl;
    return 1;
  }

  if (fake_writes && logfiles.empty()) {
    cerr << "[ERROR] --log-fake-writes specified without logging enabled" << endl;
    return 1;
  }

  if (nofsync && logfiles.empty()) {
    cerr << "[ERROR] --log-nofsync specified without logging enabled" << endl;
    return 1;
  }

  if (fake_writes && nofsync) {
    cerr << "[WARNING] --log-nofsync has no effect with --log-fake-writes enabled" << endl;
  }

#ifndef ENABLE_EVENT_COUNTERS
  if (!stats_server_sockfile.empty()) {
    cerr << "[WARNING] --stats-server-sockfile with no event counters enabled is useless" << endl;
  }
#endif

  // initialize the numa allocator
  if (numa_memory > 0) {
    const size_t maxpercpu = util::iceil(
        numa_memory / nthreads, ::allocator::GetHugepageSize());
    numa_memory = maxpercpu * nthreads;
    ::allocator::Initialize(nthreads, maxpercpu);
  }

  const set<string> can_persist({"ndb-proto2"});
  if (!logfiles.empty() && !can_persist.count(db_type)) {
    cerr << "[ERROR] benchmark " << db_type
         << " does not have persistence implemented" << endl;
    return 1;
  }

#ifdef PROTO2_CAN_DISABLE_GC
  const set<string> has_gc({"ndb-proto1", "ndb-proto2"});
  if (disable_gc && !has_gc.count(db_type)) {
    cerr << "[ERROR] benchmark " << db_type
         << " does not have gc to disable" << endl;
    return 1;
  }
#else
  if (disable_gc) {
    cerr << "[ERROR] macro PROTO2_CAN_DISABLE_GC was not set, cannot disable gc" << endl;
    return 1;
  }
#endif

#ifdef PROTO2_CAN_DISABLE_SNAPSHOTS
  const set<string> has_snapshots({"ndb-proto2"});
  if (disable_snapshots && !has_snapshots.count(db_type)) {
    cerr << "[ERROR] benchmark " << db_type
         << " does not have snapshots to disable" << endl;
    return 1;
  }
#else
  if (disable_snapshots) {
    cerr << "[ERROR] macro PROTO2_CAN_DISABLE_SNAPSHOTS was not set, cannot disable snapshots" << endl;
    return 1;
  }
#endif

  if (db_type == "mbta") {
    db = new mbta_wrapper; // on the leader replica
  } else
    ALWAYS_ASSERT(false);

#ifdef DEBUG
  cerr << "WARNING: benchmark built in DEBUG mode!!!" << endl;
#endif

#ifdef CHECK_INVARIANTS
  cerr << "WARNING: invariant checking is enabled - should disable for benchmark" << endl;
#ifdef PARANOID_CHECKING
  cerr << "  *** Paranoid checking is enabled ***" << endl;
#endif
#endif

  if (verbose) {
    const unsigned long ncpus = coreid::num_cpus_online();
    cerr << "Database Benchmark:"                           << endl;
    cerr << "  pid: " << getpid()                           << endl;
    cerr << "settings:"                                     << endl;
    cerr << "  par-loading : " << enable_parallel_loading   << endl;
    cerr << "  pin-cpus    : " << pin_cpus                  << endl;
    cerr << "  slow-exit   : " << slow_exit                 << endl;
    cerr << "  retry-txns  : " << retry_aborted_transaction << endl;
    cerr << "  backoff-txns: " << backoff_aborted_transaction << endl;
    cerr << "  bench       : " << bench_type                << endl;
    cerr << "  scale       : " << scale_factor              << endl;
    cerr << "  num-cpus    : " << ncpus                     << endl;
    cerr << "  num-threads : " << nthreads                  << endl;
    cerr << "  shardIndex  : " << shardIndex                << endl;
    cerr << "  paxos_proc_name  : " << paxos_proc_name      << endl;
    cerr << "  nshards     : " << nshards                   << endl;
    cerr << "  db-type     : " << db_type                   << endl;
    cerr << "  basedir     : " << basedir                   << endl;
    cerr << "  txn-flags   : " << hexify(txn_flags)         << endl;
    if (run_mode == RUNMODE_TIME)
      cerr << "  runtime     : " << runtime                 << endl;
    else
      cerr << "  ops/worker  : " << ops_per_worker          << endl;
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
    if (numa_memory > 0) {
      cerr << "  numa-memory : " << numa_memory             << endl;
    } else {
      cerr << "  numa-memory : disabled"                    << endl;
    }
    cerr << "  logfiles : " << logfiles                     << endl;
    cerr << "  assignments : " << assignments               << endl;
    cerr << "  disable-gc : " << disable_gc                 << endl;
    cerr << "  disable-snapshots : " << disable_snapshots   << endl;
    cerr << "  stats-server-sockfile: " << stats_server_sockfile << endl;

    cerr << "system properties:" << endl;
    //cerr << "  btree_internal_node_size: " << concurrent_btree::InternalNodeSize() << endl;
    //cerr << "  btree_leaf_node_size    : " << concurrent_btree::LeafNodeSize() << endl;

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

  sync_util::sync_logger::Init(shardIndex, nshards, nthreads, 
                               leader_config==1, /* is leader */ 
                               cluster,
                               config);
  
  if (leader_config && !stats_server_sockfile.empty()) {
    stats_server *srvr = new stats_server(stats_server_sockfile);
    thread(&stats_server::serve_forever, srvr).detach();
  }

  int argc_paxos = 18;
  int k = 0;
  char *argv_paxos[argc_paxos];
  if (paxos_config_file.size() < 2) {
      cerr << "no enough paxos config files" << endl;
      return 1;
  }

  argv_paxos[0] = (char *) "";
  argv_paxos[1] = (char *) "-b";
  argv_paxos[2] = (char *) "-d";
  argv_paxos[3] = (char *) "60";
  argv_paxos[4] = (char *) "-f";
  argv_paxos[5] = (char *) paxos_config_file[k++].c_str();
  argv_paxos[6] = (char *) "-f";
  argv_paxos[7] = (char *) paxos_config_file[k++].c_str();
  argv_paxos[8] = (char *) "-t";
  argv_paxos[9] = (char *) "30";
  argv_paxos[10] = (char *) "-T";
  argv_paxos[11] = (char *) "100000";
  argv_paxos[12] = (char *) "-n";
  argv_paxos[13] = (char *) "32";
  argv_paxos[14] = (char *) "-P";
  argv_paxos[15] = (char *) paxos_proc_name.c_str();
  argv_paxos[16] = (char *) "-A";
  argv_paxos[17] = new char[20];
  memset(argv_paxos[17], '\0', 20);
  sprintf(argv_paxos[17], "%d", kPaxosBatchSize);

  TSharedThreadPoolMbta tpool_mbta (nthreads+1);
  if (!leader_config) { // initialize tables on follower replicas
    abstract_db * db = tpool_mbta.getDBWrapper(nthreads)->getDB () ;
    // pre-initialize all tables to avoid table creation data race
    if (likely(workload_type == 1)) { // tpcc or microbenchmark, table_ids [1,11*nthreads+1] at most 
      for (int i=0;i<((size_t)scale_factor)*11+1;i++) {
        db->open_index(i+1);
      }
    }
  }

  // Invoke get_epoch function
  register_sync_util([&]() {
#if defined(PAXOS_LIB_ENABLED)
     return get_epoch();
#else
    return 0;
#endif
  });

  // rpc client
  register_sync_util_sc([&]() {
#if defined(FAIL_NEW_VERSION)
     return 0; // get_epoch();
#else
    return 0;
#endif
  });

  // rpc server
  register_sync_util_ss([&]() {
#if defined(FAIL_NEW_VERSION)
     return 0; // get_epoch();
#else
    return 0;
#endif
  });

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
        for(int i = 0; i < nthreads; i++){
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

  
  register_leader_election_callback([&](int control) { // communicate with third party: Paxos
    // happens on the learner for case 0 and case 2, 3
    uint32_t aa = srolis::getCurrentTimeMillis();
    Warning("Receive a control command:%d, current ms: %llu", control, aa);
    switch (control) {
#if defined(FAIL_NEW_VERSION)
      case 0: {
        std::cout<<"Implement a new fail recovery!"<<std::endl;
        sync_util::sync_logger::exchange_running = false;
        sync_util::sync_logger::failed_shard_index = shardIndex;
        sync_util::sync_logger::client_control(0, shardIndex); // in bench.cc register_fasttransport_for_bench
        break;
      }
      case 2: {
        // Wait for FVW in the old epoch (w * 10 + epoch); this is very important in our new implementation
        vector<uint32_t> fvw(nshards);
        for (int i=0; i<nshards; i++) {
          int clusterRoleLocal = srolis::LOCALHOST_CENTER_INT;
          if (i==0) 
            clusterRoleLocal = srolis::LEARNER_CENTER_INT;
          srolis::Memcached::wait_for_key("fvw_"+std::to_string(i), 
                                          config->shard(0, clusterRoleLocal).host.c_str(), config->mports[clusterRole]);
          std::string w_i = srolis::Memcached::get_key("fvw_"+std::to_string(i), 
                                                      config->shard(0, clusterRoleLocal).host.c_str(), 
                                                      config->mports[clusterRoleLocal]);
          std::cout<<"get fvw, " << clusterRoleLocal << ", fvw_"+std::to_string(i)<<":"<<w_i<<std::endl;
          fvw[i] = std::stoi(w_i);
        }

        sync_util::sync_logger::update_stable_timestamp_vec(get_epoch()-1, fvw);
        sync_util::sync_logger::client_control(1, shardIndex);

        // Start transactions in new epoch
        std::lock_guard<std::mutex> lk((sync_util::sync_logger::m));
        sync_util::sync_logger::toLeader = true ;
        std::cout << "notify a new leader is elected!\n" ;
        //sync_util::sync_logger::worker_running = true;
        sync_util::sync_logger::cv.notify_one();
        auto x0 = std::chrono::high_resolution_clock::now() ;
        break;
      }
#else
      // for the partial datacenter failure
      case 0: {
        // 0. stop exchange client + server on the new leader (learner)
        sync_util::sync_logger::exchange_running = false;
        // 1. issue a control command to all other leader partition servers to
        //    1.1 pause other servers DB threads 
        //    1.2 config update 
        //    1.3 issue no-ops within the old epoch
        //    1.4 start the controller
        sync_util::sync_logger::failed_shard_index = shardIndex;

        auto x0 = std::chrono::high_resolution_clock::now() ;
        sync_util::sync_logger::client_control(0, shardIndex); // in bench.cc register_fasttransport_for_bench
        auto x1 = std::chrono::high_resolution_clock::now() ;
        printf("first connection:%d\n",
            std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count());
        break;
      }
      case 2: {// notify that you're the new leader; PREPARE
         sync_util::sync_logger::client_control(1, shardIndex);
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
        sync_util::sync_logger::client_control(2, shardIndex);
        auto x1 = std::chrono::high_resolution_clock::now() ;
        printf("second connection:%d\n",
            std::chrono::duration_cast<std::chrono::microseconds>(x1-x0).count());
        break;
      }
      // for the datacenter failure, triggered on p1
      case 4: {
        // send a message to all p1 follower nodes within the same datacenter
        sync_util::sync_logger::client_control(4, shardIndex);
        break;
      }
#endif
    }
  });

#if defined(PAXOS_LIB_ENABLED)
  //StringAllocator::setSTOBatchSize(kSTOBatchSize);
 
  std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
  if (ret.empty()) {
    return -1;
  }

  for (int i = 0; i < nthreads; i++) {
    //transport::ShardAddress addr = config->shard(shardIndex, srolis::LEARNER_CENTER);
    register_for_follower_par_id_return([&,i](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<std::vector<uint32_t>, int, int, int, const char *>> & un_replay_logs_) {
      //Warning("receive a register_for_follower_par_id_return, par_id:%d, slot_id:%d,len:%d",par_id, slot_id,len);
      // status: 1 => default, 2 => ending of Paxos group, 3 => fail to safety check
      //         4 => replay DONE for this log, 5 => noops
      int status = 1;
      std::vector<uint32_t> latest_commit_id_v(nshards+1,0);
      abstract_db * db = tpool_mbta.getDBWrapper(par_id)->getDB () ;
      bool noops = false;

      if (len==2) { // start a advancer
        status = 4;
        if (par_id==0){
          std::cout << "we can start a advancer" << std::endl;
          sync_util::sync_logger::start_advancer();
        }
        latest_commit_id_v[0] = latest_commit_id_v[0]*10+status;
        return latest_commit_id_v; 
      }

      // ending of Paxos group
      if (len==0) {
        Warning("Recieved a zero length log");
        status = 2;
        // update the timestamp for this Paxos stream so that not blocking other Paxos streams
        uint32_t min_so_far = numeric_limits<uint32_t>::max();
        sync_util::sync_logger::local_timestamp_[par_id].store(min_so_far, memory_order_release) ;
        end_received+=1;
      }

      // deal with Paxos log
      if (len>0) {
        if (isNoops(log,len)!=-1) {
          Warning("receive a noops, par_id:%d on follower_callback_,%s",par_id,log);
          if (par_id==0) set_epoch(isNoops(log,len));
          noops=true;
          status=5;
        }

        if (noops) {
          sync_util::sync_logger::noops_cnt++;
          while (1) { // check if all threads receive noops
            if (sync_util::sync_logger::noops_cnt.load(memory_order_acquire)==nthreads) {
              break ;
            }
            sleep(0);
            break;
          }
          Warning("phase-1,par_id:%d DONE",par_id);
          if (par_id==0) {
            uint32_t local_w = sync_util::sync_logger::computeLocal();
            //Warning("update %s in phase-1 on port:%d", ("noops_phase_"+std::to_string(shardIndex)).c_str(), config->mports[clusterRole]);
            srolis::Memcached::set_key("noops_phase_"+std::to_string(shardIndex), 
                                       std::to_string(local_w).c_str(),
                                       config->shard(0, clusterRole).host.c_str(),
                                       config->mports[clusterRole]);


            //Warning("We update local_watermark[%d]=%llu",shardIndex, local_w);
            // update the history stable timestamp
            // TODO: replay inside the function
            sync_util::sync_logger::update_stable_timestamp(get_epoch()-1, sync_util::sync_logger::retrieveShardW()/10);
          }
        }else{
          get_latest_commit_id ((char *) log, len, nshards).swap(latest_commit_id_v);
          //ALWAYS_ASSERT(latest_commit_id_v[shardIndex]>=0);
          //Warning("par_id:%d, slot_id:%d, timestamp:%llu",par_id, slot_id, latest_commit_id_v[shardIndex]);
          sync_util::sync_logger::local_timestamp_[par_id].store(latest_commit_id_v[shardIndex], memory_order_release) ;
          auto w = sync_util::sync_logger::retrieveW();
          // Have to be the vectorized watermark comparison
          if (sync_util::sync_logger::safety_check(latest_commit_id_v, w)) { // pass safety check
            treplay_in_same_thread_opt_mbta_v2(par_id, (char*)log, len, db, nshards);
            //Warning("replay par_id:%d,slot_id:%d,un_replay_logs_:%d", par_id, slot_id,un_replay_logs_.size());
            status = 4;
          } else {
            status = 3;
          }
        }
      }

      // wait for vectorized watermark computed from other partition servers
      if (noops) {
        for (int i=0; i<nshards; i++) {
          if (i!=shardIndex && par_id==0) {
            srolis::Memcached::wait_for_key("noops_phase_"+std::to_string(i), 
                                            config->shard(0, clusterRole).host.c_str(), config->mports[clusterRole]);
            std::string local_w = srolis::Memcached::get_key("noops_phase_"+std::to_string(i), 
                                                            config->shard(0, clusterRole).host.c_str(), 
                                                            config->mports[clusterRole]);

            //Warning("We update local_watermark[%d]=%s (others)",i, local_w.c_str());
            sync_util::sync_logger::vectorized_w_[i].store(std::stoull(local_w), memory_order_release);
          }
        }
        
        // (TODO) server (!=0) wait for watermark computed from all other partition servers
      }
      auto w = sync_util::sync_logger::retrieveW(); 

      while (un_replay_logs_.size() > 0) {
          auto it = un_replay_logs_.front() ;
          if (sync_util::sync_logger::safety_check(std::get<0>(it), w)) {
            //Warning("replay-2 par_id:%d, slot_id:%d,un_replay_logs_:%d", par_id, std::get<1>(it),un_replay_logs_.size());
            auto nums = treplay_in_same_thread_opt_mbta_v2(par_id, (char *) std::get<4>(it), std::get<3>(it), db, nshards);
            un_replay_logs_.pop() ;
            free((char*)std::get<4>(it));
          } else {
            if (noops){
              un_replay_logs_.pop() ; // SWH (TODO): should compare each transactions one by one
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
          if (sync_util::sync_logger::noops_cnt_hole.load(memory_order_acquire)==nthreads) {
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
      if (status==5) { // for no-ops, there is no timestamp obtained 
        latest_commit_id_v[0]=0;
      }

      latest_commit_id_v[0] = latest_commit_id_v[0]*10+status;
      return latest_commit_id_v;
    }, 
    i);

    register_for_leader_par_id_return([&,i](const char*& log, int len, int par_id, int slot_id, std::queue<std::tuple<std::vector<uint32_t>, int, int, int, const char *>> & un_replay_logs_) {
      //Warning("receive a register_for_leader_par_id_return, par_id:%d, slot_id:%d,len:%d",par_id, slot_id,len);
      int status = 0;
      std::vector<uint32_t> latest_commit_id_v(nshards+1,0);
      bool noops = false;

      if (len==2) { // start a advancer
        status = 4;
        if (par_id==0){
          std::cout << "we can start a advancer" << std::endl;
          sync_util::sync_logger::start_advancer();
        }
        latest_commit_id_v[0] = latest_commit_id_v[0]*10+status;
        return latest_commit_id_v; 
      }

      if (len==0) {
        status = 2;
        Warning("Recieved a zero length log");
        uint32_t min_so_far = numeric_limits<uint32_t>::max();
        sync_util::sync_logger::local_timestamp_[par_id].store(min_so_far, memory_order_release) ;
        end_received_leader++;
      }

      if (len>0) {
        if (isNoops(log,len)!=-1) {
          //Warning("receive a noops, par_id:%d on leader_callback_,log:%s",par_id,log);
          noops=true;
          status=5;
        }

        if (noops) {
          sync_util::sync_logger::noops_cnt++;
          while (1) { // check if all threads receive noops
            if (sync_util::sync_logger::noops_cnt.load(memory_order_acquire)==nthreads) {
              break ;
            }
            sleep(0);
            break;
          }
          Warning("phase-1,par_id:%d DONE",par_id);
          if (par_id==0) {
            uint32_t local_w = sync_util::sync_logger::computeLocal();
            srolis::Memcached::set_key("noops_phase_"+std::to_string(shardIndex), 
                                          std::to_string(local_w).c_str(),
                                          config->shard(0, clusterRole).host.c_str(), 
                                          config->mports[clusterRole]);
            sync_util::sync_logger::update_stable_timestamp(get_epoch()-1, sync_util::sync_logger::retrieveShardW()/10);
#if defined(FAIL_NEW_VERSION)
            srolis::Memcached::set_key("fvw_"+std::to_string(shardIndex), 
                                       std::to_string(local_w).c_str(),
                                       config->shard(0, clusterRole).host.c_str(),
                                       config->mports[clusterRole]);
            std::cout<<"set fvw, " << clusterRole << ", fvw_"+std::to_string(shardIndex)<<":"<<local_w<<std::endl;
#endif
            sync_util::sync_logger::reset(); 
          }
        }else {
          get_latest_commit_id ((char *) log, len, nshards).swap(latest_commit_id_v);
          uint32_t end_time = srolis::getCurrentTimeMillis();
          //Warning("In register_for_leader_par_id_return, par_id:%d, slot_id:%d, len:%d, st: %llu, et: %llu, latency: %llu",
          //       par_id, slot_id, len, latest_commit_id_v[nshards], end_time, end_time-latest_commit_id_v[nshards]);
          sync_util::sync_logger::local_timestamp_[par_id].store(latest_commit_id_v[shardIndex], memory_order_release) ;
  
  #if defined(TRACKING_LATENCY)
          if (par_id==4){
            uint32_t vw = sync_util::sync_logger::computeLocal();
            //Warning("Update here: %llu, before:%llu",vw/10,vw);
            advanceWatermarkTracker.push_back(std::make_pair(vw/10 /* actual watermark */, srolis::getCurrentTimeMillis()));
          }
  #endif
        }
      }

      if (status==5) { // for no-ops, there is no timestamp obtained 
        latest_commit_id_v[0]=0;
      }
      latest_commit_id_v[0] = latest_commit_id_v[0]*10+status;
      return latest_commit_id_v;
    },
    i);
  }

  int ret2 = setup2(0, shardIndex);
  sleep(3); // ensure that all get started
#endif // END OF PAXOS_LIB_ENABLED

  if (leader_config) { // leader cluster
    if (bench_type == "tpcc") {
      bench_runner *r = start_workers_tpcc(leader_config, db, nthreads);
      start_workers_tpcc(leader_config, db, nthreads, false, 1, r);
    }
    delete db;
  } else if (cluster.compare(srolis::LEARNER_CENTER)==0) { // learner cluster
    if (bench_type == "tpcc") {
      abstract_db * db = tpool_mbta.getDBWrapper(nthreads)->getDB () ;
      bench_runner *r = start_workers_tpcc(1, db, nthreads, true);
      modeMonitor(db, nthreads, r) ;
    }
  }

#if defined(TRACKING_LATENCY)
  if(leader_config) {
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

  if (!leader_config) { // learner or follower cluster
    bool isLearner = cluster.compare(srolis::LEARNER_CENTER)==0 ;
    // if (bench_type == "tpcc") {
    //    abstract_db *tdb = NULL;
    //    tdb = tpool_mbta.getDBWrapper(nthreads)->getDB ();
    //    modeMonitor(tdb, nthreads) ;
    // }

    // in case, the Paxos streams on other side is terminated, 
    // not need for all no-ops for the final termination
    while (!(end_received.load() > 0 ||end_received_leader.load() > 0)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (isLearner)
        Notice("learner is waiting for being ended: %d/%zu, noops_cnt:%d\n", end_received.load(), nthreads, sync_util::sync_logger::noops_cnt.load());
      else
        Notice("follower is waiting for being ended: %d/%zu, noops_cnt:%d\n", end_received.load(), nthreads, sync_util::sync_logger::noops_cnt.load());
      //if (end_received.load() > 0) {std::quick_exit( EXIT_SUCCESS );}
    }
  }

#if defined(PAXOS_LIB_ENABLED)
  std::this_thread::sleep_for(2s);
  pre_shutdown_step();
  shutdown_paxos();
#endif

  sync_util::sync_logger::shutdown();
  std::quick_exit( EXIT_SUCCESS );

  return 0;
}
