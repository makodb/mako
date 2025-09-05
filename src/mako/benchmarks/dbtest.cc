#include <iostream>
#include <mako.hh>

using namespace std;
using namespace util;


static void parse_command_line_args(int argc, 
                                    char **argv, 
                                    string& paxos_proc_name,
                                    int &is_micro,
                                    int &is_replicated,
                                    string& site_name,
                                    vector<string>& paxos_config_file)
{
  while (1) {
    static struct option long_options[] =
    {
      {"num-threads"                , required_argument , 0                          , 't'} ,
      {"shard-index"                , required_argument , 0                          , 'g'} ,
      {"shard-config"               , required_argument , 0                          , 'q'} ,
      {"paxos-config"               , required_argument , 0                          , 'F'} ,
      {"paxos-proc-name"            , required_argument , 0                          , 'P'} ,
      {"site-name"                  , required_argument , 0                          , 'N'} ,
      {"is-micro"                   , no_argument       , &is_micro                  ,   1} ,
      {"is-replicated"              , no_argument       , &is_replicated             ,   1} ,
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "t:g:q:F:P:N:", long_options, &option_index);
    if (c == -1)
      break;

    switch (c) {
    case 0:
      if (long_options[option_index].flag != 0)
        break;
      abort();
      break;

    case 't': {
      auto& config = BenchmarkConfig::getInstance();
      config.setNthreads(strtoul(optarg, NULL, 10));
      config.setScaleFactor(strtod(optarg, NULL));
      ALWAYS_ASSERT(config.getNthreads() > 0);
      }
      break;

    case 'g': {
      auto& config = BenchmarkConfig::getInstance();
      config.setShardIndex(strtoul(optarg, NULL, 10));
      ALWAYS_ASSERT(config.getShardIndex() >= 0);
      }
      break;

    case 'N':
      site_name = string(optarg);
      break;

    case 'P': {
      auto& config = BenchmarkConfig::getInstance();
      paxos_proc_name = string(optarg);
      config.setCluster(paxos_proc_name);
      config.setClusterRole(mako::convertCluster(paxos_proc_name));
      }
      break;
    
    case 'q': {
      auto& benchConfig = BenchmarkConfig::getInstance();
      transport::Configuration* transportConfig = new transport::Configuration(optarg);
      benchConfig.setConfig(transportConfig);
      benchConfig.setNshards(transportConfig->nshards);
      }
      break;

    case 'F':
      paxos_config_file.push_back(optarg);
      break;

    case '?':
      exit(1);

    default:
      abort();
    }
  }
}

static void handle_new_config_format(const string& site_name,
                                    string& paxos_proc_name)
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  auto site = benchConfig.getConfig()->GetSiteByName(site_name);
  if (!site) {
    cerr << "[ERROR] Site " << site_name << " not found in configuration" << endl;
    exit(1);
  }
  
  // Set shard index from site
  benchConfig.setShardIndex(site->shard_id);
  
  // Set cluster role for compatibility
  if (site->is_leader) {
    benchConfig.setCluster(mako::LOCALHOST_CENTER);
    benchConfig.setClusterRole(mako::LOCALHOST_CENTER_INT);
    paxos_proc_name = mako::LOCALHOST_CENTER;
  } else if (site->replica_idx == 1) {
    benchConfig.setCluster(mako::P1_CENTER);
    benchConfig.setClusterRole(mako::P1_CENTER_INT);
    paxos_proc_name = mako::P1_CENTER;
  } else if (site->replica_idx == 2) {
    benchConfig.setCluster(mako::P2_CENTER);
    benchConfig.setClusterRole(mako::P2_CENTER_INT);
    paxos_proc_name = mako::P2_CENTER;
  } else {
    benchConfig.setCluster(mako::LEARNER_CENTER);
    benchConfig.setClusterRole(mako::LEARNER_CENTER_INT);
    paxos_proc_name = mako::LEARNER_CENTER;
  }
  
  Notice("Site %s: shard=%d, replica_idx=%d, is_leader=%d, cluster=%s", 
         site_name.c_str(), site->shard_id, site->replica_idx, site->is_leader, benchConfig.getCluster().c_str());
}



static char** prepare_paxos_args(const vector<string>& paxos_config_file,
                                const string& paxos_proc_name,
                                int kPaxosBatchSize)
{
  int argc_paxos = 18;
  char **argv_paxos = new char*[argc_paxos];
  int k = 0;
  
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

  RustWrapper* g_rust_wrapper = nullptr;
  // start a rust wrapper
  g_rust_wrapper = new RustWrapper();

  abstract_db *sample_db = new mbta_wrapper;
  abstract_ordered_index *customerTable = db->open_index("customer_0", 1, false, false);;

  g_rust_wrapper->db = sample_db;
  g_rust_wrapper->customerTable = customerTable;

  if (!g_rust_wrapper->init()) {
      std::cerr << "Failed to initialize rust wrapper!" << std::endl;
      delete g_rust_wrapper;
      return 1;
  } else {
      std::cout << "Successfully initialized rust wrapper!" << std::endl;
  }
    
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

  
  return argv_paxos;
}



static void run_workers(int leader_config, abstract_db* db, TSharedThreadPoolMbta& tpool_mbta)
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  if (leader_config) { // leader cluster
    bench_runner *r = start_workers_tpcc(leader_config, db, benchConfig.getNthreads());
    start_workers_tpcc(leader_config, db, benchConfig.getNthreads(), false, 1, r);
    delete db;
  } else if (benchConfig.getCluster().compare(mako::LEARNER_CENTER)==0) { // learner cluster
    abstract_db * db = tpool_mbta.getDBWrapper(benchConfig.getNthreads())->getDB () ;
    bench_runner *r = start_workers_tpcc(1, db, benchConfig.getNthreads(), true);
    modeMonitor(db, benchConfig.getNthreads(), r) ;
  }
}


int
main(int argc, char **argv)
{
  int is_micro = 0;  // Flag for micro benchmark mode
  int is_replicated = 0;  // if use Paxos to replicate
  std::string paxos_proc_name = mako::LOCALHOST_CENTER;
  vector<string> paxos_config_file{};
  string site_name = "";  // For new config format

  auto& benchConfig = BenchmarkConfig::getInstance();
  // Parse command line arguments
  parse_command_line_args(argc, argv, paxos_proc_name, is_micro, is_replicated, site_name, paxos_config_file);

  // Handle new configuration format if site name is provided
  if (!site_name.empty() && benchConfig.getConfig() != nullptr) {
    handle_new_config_format(site_name, paxos_proc_name);
  }

  benchConfig.setIsMicro(is_micro);
  benchConfig.setIsReplicated(is_replicated);
  benchConfig.setPaxosProcName(paxos_proc_name);
  benchConfig.setPaxosConfigFile(paxos_config_file);

  abstract_db * db = init();

  TSharedThreadPoolMbta tpool_mbta (benchConfig.getNthreads()+1);
  if (!benchConfig.getLeaderConfig()) { // initialize tables on follower replicas
    abstract_db * db = tpool_mbta.getDBWrapper(benchConfig.getNthreads())->getDB () ;
    // pre-initialize all tables to avoid table creation data race
    for (int i=0;i<((size_t)benchConfig.getScaleFactor())*11+1;i++) {
      db->open_index(i+1);
    }
  }

  // Prepare Paxos arguments
  int kPaxosBatchSize = 50000;
  if (benchConfig.getPaxosConfigFile().size() < 2) {
      cerr << "no enough paxos config files" << endl;
      return 1;
  }
  char** argv_paxos = prepare_paxos_args(benchConfig.getPaxosConfigFile(), paxos_proc_name, kPaxosBatchSize);
  
  // Setup callbacks
  setup_sync_util_callbacks();
  setup_transport_callbacks();
  setup_leader_election_callback();

  if (BenchmarkConfig::getInstance().getIsReplicated()) {
    std::vector<std::string> ret = setup(18, argv_paxos);
    if (ret.empty()) {
      return -1;
    }

    // Setup Paxos callbacks - MUST be after setup() is called
    setup_paxos_callbacks(tpool_mbta, benchConfig.getAdvanceWatermarkTracker());

    int ret2 = setup2(0, benchConfig.getShardIndex());
    sleep(3); // ensure that all get started
  }

  // Run worker threads
  run_workers(benchConfig.getLeaderConfig(), db, tpool_mbta);

  // Track and report latency if configured
  run_latency_tracking();

  // Wait for termination if not a leader
  if (!benchConfig.getLeaderConfig()) {
    wait_for_termination();
  }

  // Cleanup and shutdown
  cleanup_and_shutdown();

  return 0;
}
