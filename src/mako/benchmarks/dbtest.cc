#include <iostream>
#include <thread>
#include <mako.hh>

using namespace std;
using namespace util;


static void parse_command_line_args(int argc,
                                    char **argv,
                                    int &is_micro,
                                    int &is_replicated,
                                    string& site_name,
                                    vector<string>& paxos_config_file,
                                    string& local_shards_str)
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
      {"local-shards"               , required_argument , 0                          , 'L'} ,
      {"cpu-limit"                  , required_argument , 0                          , 'C'} ,
      {"throttle-cycle"             , required_argument , 0                          , 'Y'} ,
      {"sync-dir"                   , required_argument , 0                          , 'S'} ,
      {"is-micro"                   , no_argument       , &is_micro                  ,   1} ,
      {"is-replicated"              , no_argument       , &is_replicated             ,   1} ,
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "t:g:q:F:P:N:L:C:Y:S:", long_options, &option_index);
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
      config.setPaxosProcName(string(optarg));
      }
      break;

    case 'L':
      local_shards_str = string(optarg);
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

    case 'C': {
      auto& config = BenchmarkConfig::getInstance();
      config.setCpuLimitPercent(strtod(optarg, NULL));
      ALWAYS_ASSERT(config.getCpuLimitPercent() >= 0.0 && config.getCpuLimitPercent() <= 100.0);
      }
      break;

    case 'Y': {
      auto& config = BenchmarkConfig::getInstance();
      config.setThrottleCycleMs(strtoul(optarg, NULL, 10));
      ALWAYS_ASSERT(config.getThrottleCycleMs() > 0);
      }
      break;

    case 'S': {
      auto& config = BenchmarkConfig::getInstance();
      config.setNfsSyncDir(string(optarg));
      }
      break;

    case '?':
      exit(1);

    default:
      abort();
    }
  }
}

static vector<int> parse_local_shards(const string& local_shards_str) {
  vector<int> shard_indices;
  if (local_shards_str.empty()) {
    return shard_indices;
  }

  // Parse comma-separated list: "0,1,2"
  stringstream ss(local_shards_str);
  string token;
  while (getline(ss, token, ',')) {
    int shard_idx = stoi(token);
    shard_indices.push_back(shard_idx);
  }

  return shard_indices;
}

static void handle_new_config_format(const string& site_name)
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
    benchConfig.setPaxosProcName(mako::LOCALHOST_CENTER);
  } else if (site->replica_idx == 1) {
    benchConfig.setPaxosProcName(mako::P1_CENTER);
  } else if (site->replica_idx == 2) {
    benchConfig.setPaxosProcName(mako::P2_CENTER);
  } else {
    benchConfig.setPaxosProcName(mako::LEARNER_CENTER);
  }

  Notice("Site %s: shard=%d, replica_idx=%d, is_leader=%d, cluster=%s",
         site_name.c_str(), site->shard_id, site->replica_idx, site->is_leader, benchConfig.getCluster().c_str());
}

static void run_workers(abstract_db* db)
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  bench_runner *r = start_workers_tpcc(benchConfig.getLeaderConfig(), db, benchConfig.getNthreads());
  start_workers_tpcc(benchConfig.getLeaderConfig(), db, benchConfig.getNthreads(), false, 1, r);
  delete db;
}

static void run_workers_multi_shard(const vector<int>& shard_indices)
{
  auto& benchConfig = BenchmarkConfig::getInstance();

  Notice("Starting multi-shard workers for %zu shards", shard_indices.size());

  // Initialize multi-shard barrier for thread synchronization
  // This ensures all shard workers complete thread_init() before any start transactions
  benchConfig.initMultiShardBarrier(shard_indices.size());

  // Phase 1: Create and initialize bench_runners for all shards
  vector<bench_runner*> runners;
  for (int shard_idx : shard_indices) {
    ShardContext* ctx = benchConfig.getShardContext(shard_idx);
    if (!ctx) {
      cerr << "[ERROR] ShardContext not found for shard " << shard_idx << endl;
      continue;
    }

    Notice("Creating bench_runner for shard %d", shard_idx);

    // Bind to this shard's SiloRuntime before creating the runner
    // Note: Use operator->() or get() instead of get_mut() because get_mut()
    // returns null when there are multiple Arc references (which is expected
    // when using a shared runtime across shards)
    if (!ctx->runtime.get()) {
      cerr << "[ERROR] Runtime is null for shard " << shard_idx << endl;
      continue;
    }
    // Cast away const since BindToCurrentThread modifies thread-local state, not the runtime itself
    const_cast<SiloRuntime*>(ctx->runtime.get())->BindToCurrentThread();

    // IMPORTANT: Set thread-local shard index BEFORE creating the runner.
    // This ensures getShardIndex() returns the correct value during runner
    // initialization, especially in OpenTablesForTablespaceRemote which
    // uses getShardIndex() to determine which tables are local vs remote.
    BenchmarkConfig::setThreadLocalShardIndex(shard_idx);

    // Create the runner with shard_index
    bench_runner *r = start_workers_tpcc_shard(
        benchConfig.getLeaderConfig(),
        ctx->db,
        benchConfig.getNthreads(),
        shard_idx);

    // Clear thread-local shard index after runner creation
    BenchmarkConfig::clearThreadLocalShardIndex();

    runners.push_back(r);
    Notice("Created bench_runner for shard %d", shard_idx);
  }

  // Phase 1.5: Wire up cross-shard tables for local access (multi-shard mode only)
  // Each runner needs tables from all OTHER local shards
  Notice("Wiring up cross-shard tables for %zu runners", runners.size());
  for (size_t i = 0; i < runners.size(); i++) {
    // Wire up tables from all other shards
    for (size_t j = 0; j < runners.size(); j++) {
      if (i == j) continue;  // Skip self
      int source_shard = shard_indices[j];

      // Wire up tables from source_shard into target_runner's remote_partitions
      wireup_cross_shard_tables_tpcc(runners[i], source_shard, runners[j]);
    }
  }
  Notice("Cross-shard table wiring completed");

  // Phase 2: Run all shards in parallel using threads
  // Each shard runs its workers independently
  vector<thread> shard_threads;

  for (size_t i = 0; i < runners.size(); i++) {
    int shard_idx = shard_indices[i];
    bench_runner* runner = runners[i];

    shard_threads.emplace_back([shard_idx, runner, &benchConfig]() {
      ShardContext* ctx = benchConfig.getShardContext(shard_idx);
      if (!ctx) return;

      Notice("Running workers for shard %d in thread", shard_idx);

      // Set thread-local shard index for sync operations
      BenchmarkConfig::setThreadLocalShardIndex(shard_idx);

      // Bind this thread to the shard's runtime
      // Note: Use get() and const_cast because get_mut() returns null with shared ownership
      const_cast<SiloRuntime*>(ctx->runtime.get())->BindToCurrentThread();

      // Start the runner (this blocks until workers complete)
      start_workers_tpcc_shard(
          benchConfig.getLeaderConfig(),
          ctx->db,
          benchConfig.getNthreads(),
          shard_idx,
          false,
          1,  // run=1 to actually start
          runner);

      Notice("Workers completed for shard %d", shard_idx);

      // Clear thread-local shard index
      BenchmarkConfig::clearThreadLocalShardIndex();
    });
  }

  // Wait for all shard threads to complete
  for (auto& t : shard_threads) {
    t.join();
  }

  // Cleanup
  for (size_t i = 0; i < runners.size(); i++) {
    int shard_idx = shard_indices[i];
    ShardContext* ctx = benchConfig.getShardContext(shard_idx);
    if (ctx && ctx->db) {
      delete ctx->db;
      ctx->db = nullptr;
    }
  }

  Notice("Multi-shard workers completed");
}

int
main(int argc, char **argv)
{
  // Parameters prepared
  int is_micro = 0;  // Flag for micro benchmark mode
  int is_replicated = 0;  // if use Paxos to replicate
  vector<string> paxos_config_file{};
  string site_name = "";  // For new config format
  string local_shards_str = "";  // For multi-shard mode: comma-separated list

  auto& benchConfig = BenchmarkConfig::getInstance();
  // Parse command line arguments
  parse_command_line_args(argc, argv, is_micro, is_replicated, site_name, paxos_config_file, local_shards_str);

  // Handle new configuration format if site name is provided
  if (!site_name.empty() && benchConfig.getConfig() != nullptr) {
    handle_new_config_format(site_name);
  }

  benchConfig.setIsMicro(is_micro);
  benchConfig.setIsReplicated(is_replicated);
  benchConfig.setPaxosConfigFile(paxos_config_file);

  // Parse local shards if specified
  if (!local_shards_str.empty() && benchConfig.getConfig() != nullptr) {
    auto local_shards = parse_local_shards(local_shards_str);
    benchConfig.getConfig()->local_shard_indices = local_shards;
    benchConfig.getConfig()->multi_shard_mode = (local_shards.size() > 1);

    Notice("Multi-shard mode: running %zu shards in this process", local_shards.size());
    for (int shard_idx : local_shards) {
      Notice("  - Shard %d", shard_idx);
    }

    // If multi-shard mode, use first shard as default
    if (!local_shards.empty()) {
      benchConfig.setShardIndex(local_shards[0]);
    }
  }

  init_env();

  // Check if running in multi-shard mode
  if (benchConfig.getConfig() && benchConfig.getConfig()->multi_shard_mode) {
    // Multi-shard mode: initialize database for each local shard
    Notice("Initializing multi-shard mode with %zu local shards",
           benchConfig.getConfig()->local_shard_indices.size());

    // IMPORTANT: In multi-shard single-process mode, all shards must share
    // the same SiloRuntime so that cross-shard local table access works correctly.
    // Otherwise, a transaction from shard 0's worker accessing shard 1's tables
    // would use the wrong transaction context and fail.
    rusty::Arc<SiloRuntime> shared_runtime = SiloRuntime::Create();
    Notice("Created shared SiloRuntime %d for multi-shard mode", shared_runtime->id());

    for (int shard_idx : benchConfig.getConfig()->local_shard_indices) {
      ShardContext ctx;
      ctx.shard_index = shard_idx;
      ctx.cluster_role = benchConfig.getCluster();

      // Use the SHARED runtime for all shards (clone the Arc to share ownership)
      ctx.runtime = shared_runtime.clone();
      Notice("Assigned shared SiloRuntime %d to shard %d", ctx.runtime->id(), shard_idx);

      // IMPORTANT: Set thread-local shard index BEFORE initializing database
      // This ensures preallocate_open_index() uses the correct shard index
      // when marking tables as local vs remote
      BenchmarkConfig::setThreadLocalShardIndex(shard_idx);

      // Initialize database for this shard
      bool is_leader = benchConfig.getLeaderConfig();
      ctx.db = initShardDB(shard_idx, is_leader, ctx.cluster_role);

      // Clear thread-local shard index after DB init
      BenchmarkConfig::clearThreadLocalShardIndex();

      // Store shard context
      benchConfig.addShardContext(shard_idx, ctx);

      Notice("Initialized ShardContext for shard %d", shard_idx);
    }

    // Initialize and start transports for all local shards
    if (!initMultiShardTransports(benchConfig.getConfig()->local_shard_indices)) {
      cerr << "[ERROR] Failed to initialize multi-shard transports" << endl;
      return 1;
    }

    // Run workers on all local shards
    if (benchConfig.getLeaderConfig()) {
      Notice("Running workers on all %zu local shards",
             benchConfig.getConfig()->local_shard_indices.size());
      run_workers_multi_shard(benchConfig.getConfig()->local_shard_indices);
    }
  } else {
    // Single-shard mode: keep existing behavior
    abstract_db * db = initWithDB(); // Some init is required for followers/learners
    // Run worker threads on the leader
    if (benchConfig.getLeaderConfig()) {
      run_workers(db);
    }
  }

  db_close() ;
  return 0;
}
