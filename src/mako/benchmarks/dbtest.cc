#include <iostream>
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
      {"is-micro"                   , no_argument       , &is_micro                  ,   1} ,
      {"is-replicated"              , no_argument       , &is_replicated             ,   1} ,
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "t:g:q:F:P:N:L:", long_options, &option_index);
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

    for (int shard_idx : benchConfig.getConfig()->local_shard_indices) {
      ShardContext ctx;
      ctx.shard_index = shard_idx;
      ctx.cluster_role = benchConfig.getCluster();

      // Initialize database for this shard
      bool is_leader = benchConfig.getLeaderConfig();
      ctx.db = initShardDB(shard_idx, is_leader, ctx.cluster_role);

      // Store shard context
      benchConfig.addShardContext(shard_idx, ctx);

      Notice("Initialized ShardContext for shard %d", shard_idx);
    }

    // Initialize and start transports for all local shards
    if (!initMultiShardTransports(benchConfig.getConfig()->local_shard_indices)) {
      cerr << "[ERROR] Failed to initialize multi-shard transports" << endl;
      return 1;
    }

    // TODO: For now, we'll use the first shard's db for leader/follower operations
    // Full multi-shard worker thread support will be added later
    ShardContext* first_shard = benchConfig.getShardContext(
        benchConfig.getConfig()->local_shard_indices[0]);

    if (first_shard && benchConfig.getLeaderConfig()) {
      Notice("Running workers on first shard (shard %d) - full multi-shard support pending",
             first_shard->shard_index);
      run_workers(first_shard->db);
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
