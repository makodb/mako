#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include <unistd.h>
#include <mako.hh>

#include "tpcc_sharding.h"

import std;

using namespace std;
using namespace util;


static void parse_command_line_args(int argc,
                                    char **argv,
                                    int &is_micro,
                                    int &is_replicated,
                                    int &startup_timeout_sec,
                                    bool &startup_timeout_explicit,
                                    string& site_name,
                                    vector<string>& paxos_config_file,
                                    string& local_shards_str,
                                    string& replication_type)
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
      {"replication"                , required_argument , 0                          , 'R'} ,
      {"startup-timeout-sec"        , required_argument , 0                          , 'T'} ,
      {"runtime"                    , required_argument , 0                          , 'u'} ,
      {"storage-engine"             , required_argument , 0                          , 'E'} ,
      {"slow-exit"                  , no_argument       , 0                          , 'x'} ,
      {"is-micro"                   , no_argument       , &is_micro                  ,   1} ,
      {"is-replicated"              , no_argument       , &is_replicated             ,   1} ,
      {0, 0, 0, 0}
    };
    int option_index = 0;
    int c = getopt_long(argc, argv, "t:g:q:F:P:N:L:C:Y:S:R:T:u:E:x", long_options, &option_index);
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
      auto transportConfig = std::make_unique<transport::Configuration>(optarg);
      benchConfig.setNshards(transportConfig->nshards);
      benchConfig.setOwnedConfig(std::move(transportConfig));
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

    case 'R':
      replication_type = string(optarg);
      break;

    case 'T': {
      char* endptr = nullptr;
      long parsed = strtol(optarg, &endptr, 10);
      ALWAYS_ASSERT(endptr != optarg && *endptr == '\0');
      ALWAYS_ASSERT(parsed >= 0 && parsed <= 86400);
      startup_timeout_sec = static_cast<int>(parsed);
      startup_timeout_explicit = true;
      }
      break;

    case 'u': {
      char* endptr = nullptr;
      unsigned long parsed = strtoul(optarg, &endptr, 10);
      ALWAYS_ASSERT(endptr != optarg && *endptr == '\0');
      ALWAYS_ASSERT(parsed > 0 && parsed <= 86400);
      BenchmarkConfig::getInstance().setRuntime(parsed);
      }
      break;

    case 'E': {
      const string engine(optarg);
      ALWAYS_ASSERT(engine == "cpp" || engine == "rust");
      BenchmarkConfig::getInstance().setStorageEngine(engine);
      }
      break;

    case 'x':
      BenchmarkConfig::getInstance().setSlowExit(1);
      break;

    case '?':
      exit(1);

    default:
      abort();
    }
  }
}

static bool parse_local_shards(const string& local_shards_str,
                               int shard_count,
                               vector<int>& shard_indices,
                               string& error) {
  shard_indices.clear();
  if (local_shards_str.empty()) {
    error = "the shard list is empty";
    return false;
  }
  if (shard_count <= 0) {
    error = "the shard configuration contains no shards";
    return false;
  }

  set<int> seen;
  size_t begin = 0;
  while (begin <= local_shards_str.size()) {
    const size_t end = local_shards_str.find(',', begin);
    const size_t token_end = end == string::npos ? local_shards_str.size() : end;
    const string_view token(local_shards_str.data() + begin, token_end - begin);
    if (token.empty()) {
      error = "the shard list contains an empty entry";
      return false;
    }

    int shard_idx = -1;
    const auto parsed = from_chars(token.data(), token.data() + token.size(), shard_idx);
    if (parsed.ec != errc() || parsed.ptr != token.data() + token.size()) {
      error = "invalid shard index '" + string(token) + "'";
      return false;
    }
    if (shard_idx < 0 || shard_idx >= shard_count) {
      error = "shard index " + to_string(shard_idx) + " is outside [0, " +
              to_string(shard_count) + ")";
      return false;
    }
    if (!seen.insert(shard_idx).second) {
      error = "duplicate shard index " + to_string(shard_idx);
      return false;
    }
    shard_indices.push_back(shard_idx);

    if (end == string::npos) {
      break;
    }
    begin = end + 1;
  }

  return true;
}

static void warn_if_replicated_role_may_block() {
  auto& benchConfig = BenchmarkConfig::getInstance();
  if (!benchConfig.getIsReplicated()) {
    return;
  }

  if (benchConfig.getPaxosProcName() != mako::LOCALHOST_CENTER) {
    return;
  }

  Warning("Replicated dbtest started with --paxos-proc-name=%s. "
          "If peer role groups (p1, p2, learner) are not running, startup can wait indefinitely. "
          "Use --startup-timeout-sec=<seconds> or MAKO_STARTUP_TIMEOUT_SEC to fail fast in non-interactive runs. "
          "For local end-to-end runs use examples/test_1shard_replication.sh or "
          "examples/test_2shard_replication.sh.",
          benchConfig.getPaxosProcName().c_str());
}

static bool should_enable_replicated_startup_watchdog()
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  return benchConfig.getIsReplicated() &&
         benchConfig.getPaxosProcName() == mako::LOCALHOST_CENTER;
}

static int resolve_startup_timeout_sec(int startup_timeout_sec, bool startup_timeout_explicit)
{
  int resolved_timeout_sec = startup_timeout_sec;
  bool timeout_configured = startup_timeout_explicit;

  if (!timeout_configured) {
    if (const char* env = getenv("MAKO_STARTUP_TIMEOUT_SEC")) {
      char* endptr = nullptr;
      long parsed = strtol(env, &endptr, 10);
      if (endptr != env && *endptr == '\0' && parsed >= 0 && parsed <= 86400) {
        resolved_timeout_sec = static_cast<int>(parsed);
        timeout_configured = true;
      } else {
        Warning("Invalid MAKO_STARTUP_TIMEOUT_SEC='%s'; ignoring", env);
      }
    }
  }

  if (!timeout_configured &&
      should_enable_replicated_startup_watchdog() &&
      !isatty(STDIN_FILENO)) {
    // In non-interactive/headless runs, avoid indefinite hangs by default.
    resolved_timeout_sec = 120;
    Notice("Non-interactive startup detected; applying default startup timeout (%ds). "
           "Override with --startup-timeout-sec or MAKO_STARTUP_TIMEOUT_SEC.",
           resolved_timeout_sec);
  }

  return resolved_timeout_sec;
}

class replicated_startup_watchdog {
public:
  explicit replicated_startup_watchdog(int startup_timeout_sec)
  {
    if (startup_timeout_sec <= 0 ||
        !should_enable_replicated_startup_watchdog()) {
      return;
    }

    Notice("Enabling replicated startup watchdog (timeout=%ds)", startup_timeout_sec);
    state_ = std::make_shared<state>();
    thread_ = std::thread([startup_timeout_sec, state = state_]() {
      std::unique_lock<std::mutex> lock(state->mutex);
      if (state->condition.wait_for(
              lock,
              std::chrono::seconds(startup_timeout_sec),
              [&state]() { return state->complete; })) {
        return;
      }
      fprintf(stderr,
              "[ERROR] dbtest startup timed out after %d seconds in replicated localhost mode.\n"
              "        Start peer roles (p1, p2, learner) or use examples/test_1shard_replication.sh / examples/test_2shard_replication.sh.\n",
              startup_timeout_sec);
      std::fflush(stderr);
      std::_Exit(2);
    });
  }

  replicated_startup_watchdog(const replicated_startup_watchdog &) = delete;
  replicated_startup_watchdog &operator=(
      const replicated_startup_watchdog &) = delete;

  ~replicated_startup_watchdog()
  {
    complete();
  }

  void complete()
  {
    if (!state_)
      return;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->complete = true;
    }
    state_->condition.notify_all();
    if (thread_.joinable())
      thread_.join();
    state_.reset();
  }

private:
  struct state {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
  };

  std::shared_ptr<state> state_;
  std::thread thread_;
};

static void restore_default_termination_signals()
{
  // FastTransport/libevent installs process-wide SIGTERM/SIGINT handlers.
  // For dbtest CLI usage we want standard process semantics for timeout/docker stop:
  // SIGTERM/SIGINT should terminate the process unless explicitly handled here.
  std::signal(SIGTERM, SIG_DFL);
  std::signal(SIGINT, SIG_DFL);
}

static void handle_new_config_format(const string& site_name)
{
  auto& benchConfig = BenchmarkConfig::getInstance();
  auto site = benchConfig.getConfig()->GetSiteByName(site_name);
  if (!site) {
    mako::benchmark_cerr() << "[ERROR] Site " << site_name
                           << " not found in configuration" << endl;
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
  if (benchConfig.getSlowExit())
    delete r;
  delete db;
  mako::clear_tpcc_sharding_policy();
}

static bool run_workers_multi_shard(const vector<int>& shard_indices)
{
  auto& benchConfig = BenchmarkConfig::getInstance();

  Notice("Starting multi-shard workers for %zu shards", shard_indices.size());

  // A partial topology cannot complete the configured shard barrier or
  // cross-shard table wiring. Reject it before constructing any runner instead
  // of stalling the smaller worker set until the barrier timeout.
  for (int shard_idx : shard_indices) {
    ShardContext* ctx = benchConfig.getShardContext(shard_idx);
    if (!ctx || !ctx->runtime.get() || !ctx->db) {
      mako::benchmark_cerr() << "[ERROR] Incomplete ShardContext for shard "
                             << shard_idx << endl;
      return false;
    }
  }

  // Initialize multi-shard barrier for thread synchronization
  // This ensures all shard workers complete thread_init() before any start transactions
  benchConfig.initMultiShardBarrier(shard_indices.size());

  // Create and initialize bench_runners for all shards
  vector<bench_runner*> runners;
  vector<int> runner_shard_indices;
  for (int shard_idx : shard_indices) {
    ShardContext* ctx = benchConfig.getShardContext(shard_idx);
    ALWAYS_ASSERT(ctx != nullptr);

    Notice("Creating bench_runner for shard %d", shard_idx);

    // Bind to this shard's SiloRuntime before creating the runner
    // Note: Use operator->() or get() instead of get_mut() because get_mut()
    // returns null when there are multiple Arc references (which is expected
    // when using a shared runtime across shards)
    ALWAYS_ASSERT(ctx->runtime.get() != nullptr);
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
    runner_shard_indices.push_back(shard_idx);
    Notice("Created bench_runner for shard %d", shard_idx);
  }

  // Wire up cross-shard tables for local access (multi-shard mode only)
  // Each runner needs tables from all OTHER local shards
  Notice("Wiring up cross-shard tables for %zu runners", runners.size());
  for (size_t i = 0; i < runners.size(); i++) {
    // Wire up tables from all other shards
    for (size_t j = 0; j < runners.size(); j++) {
      if (i == j) continue;  // Skip self
      int source_shard = runner_shard_indices[j];

      // Wire up tables from source_shard into target_runner's remote_partitions
      wireup_cross_shard_tables_tpcc(runners[i], source_shard, runners[j]);
    }
  }
  Notice("Cross-shard table wiring completed");

  // Run all shards in parallel using threads
  // Each shard runs its workers independently
  vector<thread> shard_threads;

  for (size_t i = 0; i < runners.size(); i++) {
    int shard_idx = runner_shard_indices[i];
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
  if (benchConfig.getSlowExit()) {
    // Every runner's workers, including their cross-shard table pointers, were
    // destroyed before run() returned. Only now is it safe to close the table
    // facades owned by any shard.
    for (bench_runner* runner : runners)
      runner->clear_and_close_open_tables();
    for (bench_runner* runner : runners)
      delete runner;
  }
  for (int shard_idx : shard_indices) {
    ShardContext* ctx = benchConfig.getShardContext(shard_idx);
    if (ctx && ctx->db) {
      delete ctx->db;
      ctx->db = nullptr;
    }
  }
  mako::clear_tpcc_sharding_policy();

  Notice("Multi-shard workers completed");
  return true;
}

int
main(int argc, char **argv)
{
  // Parameters prepared
  int is_micro = 0;  // Flag for micro benchmark mode
  int is_replicated = 0;  // if use Paxos to replicate
  int startup_timeout_sec = 0;  // Optional startup watchdog timeout for replicated localhost mode
  bool startup_timeout_explicit = false;
  vector<string> paxos_config_file{};
  string site_name = "";  // For new config format
  string local_shards_str = "";  // For multi-shard mode: comma-separated list
  string replication_type = "";  // paxos or raft (default: paxos)

  auto& benchConfig = BenchmarkConfig::getInstance();
  // Parse command line arguments
  parse_command_line_args(argc, argv, is_micro, is_replicated, startup_timeout_sec, startup_timeout_explicit,
                          site_name, paxos_config_file, local_shards_str, replication_type);

#if defined(MAKO_RUST_STO_TPCC)
  benchConfig.setEmitTpccResult(true);
#else
  if (benchConfig.getStorageEngine() != "cpp") {
    mako::benchmark_cerr()
        << "[ERROR] This dbtest binary has no Rust STO TPC-C adapter; "
           "use the sto_tpcc_bench target."
        << endl;
    return 2;
  }
#endif

  // Keep dbtest CLI responsive to process-level termination signals (SIGTERM/SIGINT),
  // which are commonly used by timeout/docker stop/script cleanup flows.
  set_fasttransport_signal_handlers_enabled(false);

  // Set replication type before any initialization (default is paxos)
  if (!replication_type.empty()) {
    janus::set_replication_type_from_string(replication_type);
    Notice("Using replication type: %s", replication_type.c_str());
  }

  // Handle new configuration format if site name is provided
  if (!site_name.empty() && benchConfig.getConfig() != nullptr) {
    handle_new_config_format(site_name);
  }

  benchConfig.setIsMicro(is_micro);
  benchConfig.setIsReplicated(is_replicated);
  benchConfig.setPaxosConfigFile(paxos_config_file);

  // Validate the complete local-shard topology before starting a watchdog or
  // constructing any database, runner, or table facade. Duplicate entries
  // would otherwise create multiple runners sharing one ShardContext and make
  // slow-exit teardown close the same table pointers more than once.
  if (!local_shards_str.empty()) {
    if (benchConfig.getConfig() == nullptr) {
      mako::benchmark_cerr()
          << "[ERROR] --local-shards requires --shard-config" << endl;
      return 2;
    }
    vector<int> local_shards;
    string parse_error;
    if (!parse_local_shards(local_shards_str,
                            benchConfig.getConfig()->nshards,
                            local_shards,
                            parse_error)) {
      mako::benchmark_cerr() << "[ERROR] Invalid --local-shards: "
                             << parse_error << endl;
      return 2;
    }
    const size_t configured_shards =
        static_cast<size_t>(benchConfig.getConfig()->nshards);
    if (local_shards.size() > 1 && local_shards.size() != configured_shards) {
      mako::benchmark_cerr()
          << "[ERROR] --local-shards cannot select multiple but not all "
             "configured shards; hybrid local/remote multi-shard topology is "
             "unsupported"
          << endl;
      return 2;
    }
    benchConfig.getConfig()->local_shard_indices = local_shards;
    benchConfig.getConfig()->multi_shard_mode = (local_shards.size() > 1);

    Notice("Multi-shard mode: running %zu shards in this process", local_shards.size());
    for (int shard_idx : local_shards) {
      Notice("  - Shard %d", shard_idx);
    }

    benchConfig.setShardIndex(local_shards.front());
  }

#if defined(MAKO_RUST_STO_TPCC)
  // Keep this capability check ahead of init_env(): the Rust comparison
  // adapter deliberately implements one non-replicated shard and no remote
  // indexes or distributed commit. It also has a finite shared native/C++
  // attachment budget. The wrapper repeats both checks as defense in depth,
  // but CLI misuse must fail cleanly before allocating a database.
  if (benchConfig.getStorageEngine() == "rust" &&
      (benchConfig.getNshards() != 1 || benchConfig.getIsReplicated())) {
    mako::benchmark_cerr()
        << "[ERROR] Rust STO TPC-C comparison supports one non-replicated shard"
        << endl;
    return 2;
  }
  if (benchConfig.getStorageEngine() == "rust") {
    try {
      (void)rust_sto_tpcc_detail::db_config_for_worker_count(
          benchConfig.getNthreads());
    } catch (const std::invalid_argument &error) {
      mako::benchmark_cerr() << "[ERROR] " << error.what() << endl;
      return 2;
    }
  }
#endif

  warn_if_replicated_role_may_block();
  startup_timeout_sec = resolve_startup_timeout_sec(startup_timeout_sec, startup_timeout_explicit);
  replicated_startup_watchdog startup_watchdog(startup_timeout_sec);

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
      mako::benchmark_cerr()
          << "[ERROR] Failed to initialize multi-shard transports" << endl;
      return 1;
    }
    restore_default_termination_signals();
    startup_watchdog.complete();

    // Run workers on all local shards
    if (benchConfig.getLeaderConfig()) {
      Notice("Running workers on all %zu local shards",
             benchConfig.getConfig()->local_shard_indices.size());
      if (!run_workers_multi_shard(benchConfig.getConfig()->local_shard_indices))
        return 1;
    }
  } else {
    // Single-shard mode: keep existing behavior
    abstract_db * db = initWithDB(); // Some init is required for followers/learners
    restore_default_termination_signals();
    startup_watchdog.complete();
    // Run worker threads on the leader
    if (benchConfig.getLeaderConfig()) {
      run_workers(db);
    }
  }

  db_close() ;
  return 0;
}
