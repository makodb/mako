// Implementation of TPCC setup utilities extracted from tpcc.cc

#include <stdint.h>
#include <stddef.h>

#include "rpc_setup.h"

#include <pthread.h>
#include <unistd.h>

#include "benchmark_config.h"
#include "lib/common.h"
#include "lib/fasttransport.h"
#include "lib/server.h"
#include "deptran/s_main.h"
#include "benchmarks/sto/Interface.hh"
#include "spinbarrier.h"


using namespace std;
using namespace mako;

namespace {

std::mutex g_helper_mu;
std::vector<mako::ShardServer *> g_helper_servers;

static inline size_t NumWarehouses() {
  return (size_t) BenchmarkConfig::getInstance().getScaleFactor();
}

static inline size_t NumWarehousesTotal() {
  auto &cfg = BenchmarkConfig::getInstance();
  return cfg.getNshards() * ((size_t) cfg.getScaleFactor());
}

// Thread entry: server-side helper processing
void helper_server(
  int g_wid,
  std::string cluster,
  int running_shardIndex,
  int num_warehouses,
  transport::Configuration *config,
  abstract_db *db,
  mako::HelperQueue *queue,
  mako::HelperQueue *queue_response,
  std::map<int, abstract_ordered_index *> open_tables,
  spin_barrier *barrier_ready)
{
  scoped_db_thread_ctx ctx(db, true, 1);
  TThread::set_mode(1);
#if defined(DISABLE_MULTI_VERSION)
  TThread::disable_multiversion();
#else
  TThread::enable_multiverison();
#endif
  int shardIdx = (g_wid - 1) / num_warehouses;
  int par_id = (g_wid - 1) % num_warehouses;
  TThread::set_shard_index(running_shardIndex);
  TThread::set_pid(par_id);
  TThread::set_nshards(config->nshards);

  mako::ShardServer *ss = new mako::ShardServer(
    config->configFile, running_shardIndex, shardIdx, par_id);
  ss->Register(db, queue, queue_response, open_tables);
  {
    std::lock_guard<std::mutex> lock(g_helper_mu);
    g_helper_servers.push_back(ss);
  }

  // Signal that this helper is ready before starting the event loop
  if (barrier_ready) {
    barrier_ready->count_down();
  }

  ss->Run(); // event-driven
}

// Thread entry: eRPC server
void erpc_server(
  std::string cluster,
  int running_shardIndex,
  int num_warehouses,
  transport::Configuration *config,
  int alpha,
  std::vector<FastTransport*> &server_transports,
  std::atomic<int> &set_server_transport)
{
  std::string local_uri = config->shard(running_shardIndex, mako::convertCluster(cluster)).host;
  int base = 5;
  int id = num_warehouses + base + alpha;
  server_transports[alpha] = new FastTransport(
    config->configFile,
    local_uri,
    cluster,
    // Handler range is inclusive; 14-16 are the self-contained
    // non-txn write ops (nontxnPut/Insert/Remove, common.h).
    1, 17,
    0, // physPort
    0, // numa node
    running_shardIndex,
    id);

  // Set up helper queues for this server transport
  std::unordered_map<uint16_t, mako::HelperQueue*> local_queue_holders;
  std::unordered_map<uint16_t, mako::HelperQueue*> local_queue_holders_response;

  for (int i = 0; i < (int)NumWarehousesTotal(); i++) {
    if (i / (int)NumWarehouses() == running_shardIndex)
      continue;
    if (i % (int)BenchmarkConfig::getInstance().getNumErpcServer() == alpha) {
      auto *it = new mako::HelperQueue(i, true);
      local_queue_holders[i] = it;
      auto *it_res = new mako::HelperQueue(i, false);
      local_queue_holders_response[i] = it_res;
    }
  }

  server_transports[alpha]->SetHelperQueues(local_queue_holders);
  server_transports[alpha]->SetHelperQueuesResponse(local_queue_holders_response);
  set_server_transport.fetch_add(1);
  server_transports[alpha]->Run();
  Notice("the erpc_server is terminated on shardIdx:%d, alpha:%d!", running_shardIndex, alpha);
}

} // anonymous namespace

void mako::setup_helper(
  abstract_db *db,
  const std::map<int, abstract_ordered_index *> &open_tables)
{
  auto &cfg = BenchmarkConfig::getInstance();
  auto &queue_holders = cfg.getQueueHolders();
  auto &queue_holders_response = cfg.getQueueHoldersResponse();

  // Count the number of helper threads that will be created
  int num_helpers = 0;
  for (int i = 0; i < (int)NumWarehousesTotal(); i++) {
    if (i / (int)NumWarehouses() == (int)cfg.getShardIndex())
      continue;
    num_helpers++;
  }

  // Create barrier to wait for all helpers to be ready
  spin_barrier barrier_ready(num_helpers+1);

  for (int i = 0; i < (int)NumWarehousesTotal(); i++) {
    if (i / (int)NumWarehouses() == (int)cfg.getShardIndex())
      continue;

    auto t = std::thread(
      helper_server,
      i + 1,
      cfg.getCluster(),
      (int)cfg.getShardIndex(),
      (int)NumWarehouses(),
      cfg.getConfig(),
      db,
      queue_holders[i],
      queue_holders_response[i],
      open_tables,
      &barrier_ready);
#if defined(__APPLE__)
    // macOS pthread_setname_np() only supports naming the *current* thread.
#else
    pthread_setname_np(t.native_handle(), ("helper_" + std::to_string(i)).c_str());
#endif
    t.detach();
  }

  // Wait for all helper threads to finish initialization before returning
  barrier_ready.count_down();
  barrier_ready.wait_for();
}

void mako::setup_update_table(int table_id, abstract_ordered_index *table)
{
  if (!table)
    return;

  std::lock_guard<std::mutex> lock(g_helper_mu);
  for (auto *server : g_helper_servers) {
    server->UpdateTable(table_id, table);
  }
}

void mako::stop_helper()
{
  auto &cfg = BenchmarkConfig::getInstance();
  auto &queue_holders = cfg.getQueueHolders();
  for (auto &entry : queue_holders) {
    if (entry.second) {
      entry.second->request_stop();
    } 
  }
  {
    std::lock_guard<std::mutex> lock(g_helper_mu);
    g_helper_servers.clear();
  }
}

void mako::initialize_per_thread(abstract_db *db_) {
  scoped_db_thread_ctx ctx(db_, false);
}

void mako::setup_erpc_server()
{
  auto &cfg = BenchmarkConfig::getInstance();
  auto &server_transports = cfg.getServerTransports();
  auto &queue_holders = cfg.getQueueHolders();
  auto &queue_holders_response = cfg.getQueueHoldersResponse();
  auto &set_server_transport = cfg.getServerTransportReadyCounter();

  // Use existing state; server threads will populate queues.
  if (server_transports.size() < cfg.getNumErpcServer())
    server_transports.resize(cfg.getNumErpcServer());
  for (int i = 0; i < (int)cfg.getNumErpcServer(); ++i) {
    auto t = std::thread(
      erpc_server,
      cfg.getCluster(),
      (int)cfg.getShardIndex(),
      (int)NumWarehouses(),
      cfg.getConfig(),
      i,
      std::ref(server_transports),
      std::ref(set_server_transport));
#if defined(__APPLE__)
    // macOS pthread_setname_np() only supports naming the *current* thread.
#else
    pthread_setname_np(t.native_handle(), "erpc_server");
#endif
    t.detach();
  }

  while (set_server_transport.load() < (int)cfg.getNumErpcServer()) {
    sleep(0);
  }

  for (int i = 0; i < (int)NumWarehousesTotal(); i++) {
    if (i / (int)NumWarehouses() == (int)cfg.getShardIndex())
      continue;
    auto idx = i % (int)cfg.getNumErpcServer();
    queue_holders[i] = server_transports[idx]->GetHelperQueue(i);
    queue_holders_response[i] = server_transports[idx]->GetHelperQueueResponse(i);
  }
}

void mako::stop_erpc_server()
{
  auto &cfg = BenchmarkConfig::getInstance();
  auto &server_transports = cfg.getServerTransports();

  // Use actual vector size to avoid out-of-bounds access
  // In multi-shard mode, server_transports may be empty while getNumErpcServer() > 0
  size_t actual_count = server_transports.size();
  std::cerr << "[STOP_SERVER] Stopping " << actual_count << " server transports" << std::endl;

  for (size_t i = 0; i < actual_count; ++i) {
    if (server_transports[i]) {
      std::cerr << "[STOP_SERVER] Stopping server transport " << i << std::endl;
      server_transports[i]->Stop();
      std::cerr << "[STOP_SERVER] Server transport " << i << " stopped" << std::endl;
    }
  }
  std::cerr << "[STOP_SERVER] All server transports stopped" << std::endl;
}

// ============================================================================
// Client TCP Server for RemoteDB connections
// ============================================================================

#include "rocks_interface/client_tcp_server.h"

import std;

namespace {
// Global pointer to client TCP server (singleton)
mako::ClientTcpServer* g_client_tcp_server = nullptr;
std::mutex g_client_tcp_server_mu;

// Global pointer to standalone ShardReceiver for single-shard mode
// @unsafe - Requires manual cleanup in stop_client_tcp_server
mako::ShardReceiver* g_standalone_receiver = nullptr;
}  // anonymous namespace

bool mako::setup_client_tcp_server(int port)
{
  std::lock_guard<std::mutex> lock(g_client_tcp_server_mu);

  if (g_client_tcp_server) {
    std::cerr << "[CLIENT_TCP] Server already running" << std::endl;
    return false;
  }

  // Get the first available ShardReceiver from helper servers
  ShardReceiver* receiver = nullptr;
  {
    std::lock_guard<std::mutex> helper_lock(g_helper_mu);
    if (!g_helper_servers.empty()) {
      receiver = g_helper_servers[0]->GetReceiver();
    }
  }

  if (!receiver) {
    std::cerr << "[CLIENT_TCP] No ShardReceiver available - call setup_helper() first" << std::endl;
    return false;
  }

  // Get configuration for worker pool size
  auto& cfg = BenchmarkConfig::getInstance();
  size_t max_clients = cfg.getNthreads();  // One client per worker thread

  // Create and start the TCP server with worker pool
  g_client_tcp_server = new ClientTcpServer(port, max_clients);
  g_client_tcp_server->SetReceiver(receiver);

  if (!g_client_tcp_server->Start()) {
    std::cerr << "[CLIENT_TCP] Failed to start server on port " << port << std::endl;
    delete g_client_tcp_server;
    g_client_tcp_server = nullptr;
    return false;
  }

  std::cerr << "[CLIENT_TCP] Server started on port " << port
            << " (max " << max_clients << " concurrent clients)" << std::endl;
  return true;
}

// @unsafe - Creates ShardReceiver for single-shard mode
bool mako::setup_client_tcp_server(
  abstract_db *db,
  const std::map<int, abstract_ordered_index *> &open_tables,
  int port)
{
  std::lock_guard<std::mutex> lock(g_client_tcp_server_mu);

  if (g_client_tcp_server) {
    std::cerr << "[CLIENT_TCP] Server already running" << std::endl;
    return false;
  }

  // Get configuration for worker pool size
  auto& cfg = BenchmarkConfig::getInstance();
  size_t max_clients = cfg.getNthreads();  // One client per worker thread

  // Create a standalone ShardReceiver for single-shard mode
  // @unsafe { dynamic memory allocation, requires cleanup }
  g_standalone_receiver = new ShardReceiver(cfg.getConfig()->configFile);
  g_standalone_receiver->Register(db, open_tables);

  std::cerr << "[CLIENT_TCP] Created standalone ShardReceiver for single-shard mode" << std::endl;

  // Create and start the TCP server with worker pool
  g_client_tcp_server = new ClientTcpServer(port, max_clients);
  g_client_tcp_server->SetReceiver(g_standalone_receiver);

  if (!g_client_tcp_server->Start()) {
    std::cerr << "[CLIENT_TCP] Failed to start server on port " << port << std::endl;
    delete g_client_tcp_server;
    g_client_tcp_server = nullptr;
    delete g_standalone_receiver;
    g_standalone_receiver = nullptr;
    return false;
  }

  std::cerr << "[CLIENT_TCP] Server started on port " << port
            << " (max " << max_clients << " concurrent clients, single-shard mode)" << std::endl;
  return true;
}

void mako::stop_client_tcp_server()
{
  std::lock_guard<std::mutex> lock(g_client_tcp_server_mu);

  if (g_client_tcp_server) {
    std::cerr << "[CLIENT_TCP] Stopping server..." << std::endl;
    g_client_tcp_server->Stop();
    delete g_client_tcp_server;
    g_client_tcp_server = nullptr;
    std::cerr << "[CLIENT_TCP] Server stopped" << std::endl;
  }

  // Clean up standalone receiver if created (single-shard mode)
  if (g_standalone_receiver) {
    delete g_standalone_receiver;
    g_standalone_receiver = nullptr;
    std::cerr << "[CLIENT_TCP] Standalone ShardReceiver cleaned up" << std::endl;
  }
}

// Get the ShardReceiver instance for the current shard
ShardReceiver* mako::get_shard_receiver()
{
  std::lock_guard<std::mutex> lock(g_helper_mu);
  if (!g_helper_servers.empty()) {
    return g_helper_servers[0]->GetReceiver();
  }
  return nullptr;
}
