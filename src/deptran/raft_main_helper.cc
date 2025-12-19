#include "raft_main_helper.h"

#ifdef MAKO_USE_RAFT

#include "__dep__.h"
#include "config.h"
#include "frame.h"
#include "raft/raft_worker.h"
#include "client_worker.h"
#include "paxos_worker.h"  // ElectionState definition lives here

#include <algorithm>
#include <iostream>

using namespace janus;

namespace janus {
vector<unique_ptr<ClientWorker>> client_workers_storage = {};
vector<shared_ptr<RaftWorker>> raft_workers_g = {};
std::function<void(int)> leader_callback_{};
}

vector<unique_ptr<janus::ClientWorker>>& client_workers_g = janus::client_workers_storage;
using janus::raft_workers_g;

// leader_replay_cb / follower_replay_cb cache watermark callbacks across role changes.
std::map<int, std::function<int(const char*&, int, int, int,
    std::queue<std::tuple<int, int, int, int, const char *>> &)>> leader_replay_cb;
std::map<int, std::function<int(const char*&, int, int, int,
    std::queue<std::tuple<int, int, int, int, const char *>> &)>> follower_replay_cb;

shared_ptr<ElectionState> es = ElectionState::instance();

// send_no_ops_for_mark serialises a Raft NO-OP entry so leader/followers sync watermarks.
void send_no_ops_for_mark(int epoch) {
  std::string log = "no-ops:" + std::to_string(epoch);
  for (auto& worker : raft_workers_g) {
    if (!worker || !worker->site_info_) {
      continue;
    }
    add_log_to_nc(log.c_str(), static_cast<int>(log.size()),
                  worker->site_info_->partition_id_, 1);
  }
}

// send_no_ops_to_all_workers keeps compatibility with Paxos helper call sites.
void send_no_ops_to_all_workers(int epoch) {
  send_no_ops_for_mark(epoch);
}

namespace {

constexpr std::chrono::milliseconds kLeaderWaitTimeout(5000);
std::mutex leader_wait_mutex;
std::condition_variable leader_wait_cv;

void log_wait_outcome(uint32_t par_id, bool success, std::chrono::milliseconds waited) {
  Log_info("[RAFT-WAIT-LEADERSHIP] par_id=%u status=%s waited_ms=%lld",
           par_id,
           success ? "leader" : "still_follower",
           static_cast<long long>(waited.count()));
}

// wait_for_local_leadership blocks until the local Raft node becomes leader for par_id.
bool wait_for_local_leadership(RaftWorker* worker,
                               uint32_t par_id,
                               std::chrono::milliseconds timeout) {
  if (!worker) {
    return false;
  }
  Log_info("[RAFT-WAIT-LEADERSHIP] par_id=%u initial_leader=%s timeout_ms=%lld",
           par_id,
           worker->IsLeader(par_id) ? "true" : "false",
           static_cast<long long>(timeout.count()));
  auto deadline = std::chrono::steady_clock::now() + timeout;
  auto start = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(leader_wait_mutex);
  while (true) {
    lock.unlock();
    if (worker->IsLeader(par_id)) {
      log_wait_outcome(par_id,
                       true,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start));
      return true;
    }
    lock.lock();
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    if (leader_wait_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      break;
    }
  }
  lock.unlock();
  bool now_leader = worker->IsLeader(par_id);
  log_wait_outcome(par_id,
                   now_leader,
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start));
  return now_leader;
}

// check_current_path is kept for parity with Paxos helper logging.
void check_current_path() {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != nullptr) {
    (void)cwd;
  }
}

// server_launch_worker finishes wiring RPC/commo threads and starts batching loops.
void server_launch_worker(std::vector<Config::SiteInfo>& server_sites) {
  if (server_sites.empty()) {
    return;
  }

  Log_info("[RAFT-LAUNCH] server_sites.size()=%zu raft_workers_g.size()=%zu",
           server_sites.size(), raft_workers_g.size());
  for (size_t i = 0; i < server_sites.size(); ++i) {
    Log_info("[RAFT-LAUNCH] server_sites[%zu]: id=%d locale_id=%u proc_name=%s partition_id=%d",
             i, server_sites[i].id, server_sites[i].locale_id,
             server_sites[i].proc_name.c_str(), server_sites[i].partition_id_);
  }

  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->SetupService();
    }
  }

  for (size_t i = 0; i < server_sites.size() && i < raft_workers_g.size(); ++i) {
    auto& worker = raft_workers_g[i];
    if (!worker) {
      continue;
    }

    worker->SetupCommo();

    // CRITICAL: Call RaftServer::Setup() AFTER SetupCommo()
    // Setup() needs commo_ to be initialized for HeartbeatLoop
    if (auto raft_server = worker->GetRaftServer()) {
      auto poll_worker_opt = worker->GetPollThreadWorker();
      if (poll_worker_opt.is_some()) {
        auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([raft_server]() {
          Log_info("[RAFTPOLL] EnsureSetup executing (site=%d par=%d)",
                   raft_server->site_id_,
                   raft_server->partition_id_);
          raft_server->EnsureSetup();
        }));
        Log_info("[RAFTPOLL] Queueing EnsureSetup job for worker index %zu", i);
        poll_worker_opt.unwrap()->add(rusty::Arc<Job>(arc_job));
      } else {
        Log_info("[RAFTPOLL] No poll worker for index %zu; calling EnsureSetup inline", i);
        raft_server->EnsureSetup();
      }
    } else {
      Log_error("[LAUNCH-WORKER] Worker %zu: GetRaftServer() returned null!", i);
    }

    worker->StartSubmitThread();
  }

  for (auto& worker : raft_workers_g) {
    if (worker) {
      Log_info("[RAFT-HEARTBEAT-SETUP] About to call SetupHeartbeat() for worker: site_id=%d, port=%d, heartbeat_port=%d",
               worker->site_info_ ? worker->site_info_->id : -1,
               worker->site_info_ ? worker->site_info_->port : -1,
               worker->site_info_ ? (worker->site_info_->port + 10000) : -1);
      worker->SetupHeartbeat();
      Log_info("[RAFT-HEARTBEAT-SETUP] SetupHeartbeat() completed for site_id=%d",
               worker->site_info_ ? worker->site_info_->id : -1);
    }
  }
}

// find_worker returns the worker responsible for the requested partition.
RaftWorker* find_worker(uint32_t par_id) {
  for (auto& worker : raft_workers_g) {
    if (worker && worker->IsPartition(par_id)) {
      return worker.get();
    }
  }
  return nullptr;
}

// enqueue_to_worker increments bookkeeping and drops the payload into the worker queue.
void enqueue_to_worker(RaftWorker* worker,
                       const char* log,
                       int len,
                       uint32_t par_id,
                       int batch_size) {
  if (!worker) {
    return;
  }
  worker->IncSubmit();
  if (worker->HasSubmitThread()) {
    worker->EnqueueLog(log, len, par_id, batch_size);
  } else {
    worker->Submit(log, len, par_id);
  }
}

// RAFT CHANGE: Register BOTH leader and follower callbacks
// RaftWorker::Next() will dynamically choose which to call based on current leadership
void apply_callbacks_for_partition(uint32_t par_id) {
  auto* worker = find_worker(par_id);
  if (!worker || !worker->site_info_) {
    return;
  }

  // Register leader callback if available
  auto leader_it = leader_replay_cb.find(par_id);
  if (leader_it != leader_replay_cb.end()) {
    worker->register_leader_callback_par_id_return(leader_it->second);
    Log_info("[RAFT-APPLY-CB] Registered leader callback for partition %u", par_id);
  }

  // Register follower callback if available
  auto follower_it = follower_replay_cb.find(par_id);
  if (follower_it != follower_replay_cb.end()) {
    worker->register_follower_callback_par_id_return(follower_it->second);
    Log_info("[RAFT-APPLY-CB] Registered follower callback for partition %u", par_id);
  }
}

}  // namespace

// setup initialises all RaftWorkers for the current process and disables Jetpack.
std::vector<std::string> setup(int argc, char* argv[]) {
  std::vector<std::string> ret_vector;
  check_current_path();

  if (std::getenv("MAKO_DISABLE_JETPACK") == nullptr) {
    setenv("MAKO_DISABLE_JETPACK", "1", 1);
    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK unset; forcing helper default of 1");
  } else {
    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK=%s", std::getenv("MAKO_DISABLE_JETPACK"));
  }

  int ret = Config::CreateConfig(argc, argv);
  if (ret != SUCCESS) {
    Log_fatal("Read config failed");
    return ret_vector;
  }

  auto server_infos = Config::GetConfig()->GetMyServers();

  for (int i = static_cast<int>(server_infos.size()) - 1; i >= 0; --i) {
    const auto& site = Config::GetConfig()->SiteById(server_infos[i].id);
    ret_vector.push_back(site.name);

    auto worker = std::make_shared<RaftWorker>();
    worker->site_info_ = const_cast<Config::SiteInfo*>(&site);
    worker->SetupBase();
    raft_workers_g.push_back(std::move(worker));
  }

  std::reverse(raft_workers_g.begin(), raft_workers_g.end());
  if (!raft_workers_g.empty() && raft_workers_g.back()->site_info_) {
    es->machine_id = raft_workers_g.back()->site_info_->locale_id;
  }

  return ret_vector;
}

// ============================================================================
// setup2 launches network services and configures preferred leader system
// ============================================================================
// Uses Raft's natural election mechanism with preferred replica bias:
// - Machine_id 0 (localhost) is set as the preferred leader
// - Elections are biased toward the preferred replica
// - If preferred fails, others can take over
// - When preferred recovers, it catches up then reclaims leadership
//
// This provides similar behavior to Paxos fixed leader but with automatic failover.
int setup2(int action, int shardIndex) {
  auto server_infos = Config::GetConfig()->GetMyServers();

  // ============================================================================
  // PREFERRED REPLICA SYSTEM SETUP
  // ============================================================================
  // Configure preferred leader: Use localhost (locale_id==0) as preferred for all partitions
  // This can be changed dynamically later via SetPreferredLeader()
  //
  // IMPORTANT: In multi-partition systems, each partition has its own set of replicas
  // with different site_ids. We must set the preferred leader PER PARTITION.
  //
  // Example with 6 partitions:
  //   Partition 0: sites s101(localhost), s201(p1), s302(p2) → preferred = s101
  //   Partition 1: sites s102(localhost), s202(p1), s303(p2) → preferred = s102
  //   etc.

  auto config = Config::GetConfig();

  // Set preferred leader for each worker based on its partition
  for (auto& worker : raft_workers_g) {
    if (!worker) continue;

    auto raft_server = worker->GetRaftServer();
    if (!raft_server) continue;

    parid_t partition_id = raft_server->partition_id_;
    siteid_t my_site_id = raft_server->site_id_;

    // Get all sites for THIS partition
    auto partition_sites = config->SitesByPartitionId(partition_id);

    // Find the site in THIS partition that has locale_id == 0 (localhost)
    siteid_t preferred_site_id = INVALID_SITEID;
    for (const auto& site : partition_sites) {
      if (site.locale_id == 0) {  // localhost
        preferred_site_id = site.id;
        break;
      }
    }

    // Set the preferred leader for THIS partition
    if (preferred_site_id != INVALID_SITEID) {
      raft_server->SetPreferredLeader(preferred_site_id);
      Log_info("[PREFERRED-REPLICA] Partition %d, Site %d: Set preferred leader to site %d (localhost)",
               partition_id, my_site_id, preferred_site_id);
    } else {
      Log_info("[PREFERRED-REPLICA] Partition %d, Site %d: No preferred leader found (using standard Raft)",
               partition_id, my_site_id);
    }
  }

  Log_info("[RAFT-SETUP] Preferred replica system configured for all partitions (localhost preferred)");

  // Update election state for Paxos compatibility
  if (es->machine_id == 0) {
    es->set_state(0);   // Will become leader via election
    es->set_epoch(0);
    es->set_leader(0);
    Log_info("[RAFT-SETUP] Machine %d: Preferred leader (will win elections naturally)",
             es->machine_id);
  } else {
    es->set_state(0);
    es->set_epoch(0);
    es->set_leader(0);
    Log_info("[RAFT-SETUP] Machine %d: Non-preferred replica (can take over if preferred fails)",
             es->machine_id);
  }

  // Launch workers (normal Raft elections will proceed, biased toward preferred)
  if (!server_infos.empty()) {
    server_launch_worker(server_infos);
  }

  (void)shardIndex;
  (void)action;  // action parameter ignored in preferred replica mode
  return 0;
}

// getHosts reads host bindings from the supplied YAML file.
std::map<std::string, std::string> getHosts(std::string filename) {
  std::map<std::string, std::string> proc_host_map;

  try {
    YAML::Node config = YAML::LoadFile(filename);
    if (config["host"]) {
      auto node = config["host"];
      for (auto it = node.begin(); it != node.end(); ++it) {
        auto proc_name = it->first.as<std::string>();
        auto host_name = it->second.as<std::string>();
        proc_host_map[proc_name] = host_name;
      }
    } else {
      std::cerr << "No host attribute in YAML: " << filename << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "getHosts() failed: " << e.what() << std::endl;
  }

  return proc_host_map;
}

// get_outstanding_logs reports how many Raft slots the local worker still owes.
int get_outstanding_logs(uint32_t par_id) {
  auto* worker = find_worker(par_id);
  if (!worker) {
    Log_warn("get_outstanding_logs(): unknown partition %u", par_id);
    return -1;
  }
  auto* raft_server = worker->GetRaftServer();
  if (!raft_server) {
    return -1;
  }
  return static_cast<int>(worker->n_tot.load()) -
         static_cast<int>(raft_server->commitIndex);
}

// shutdown_paxos drains workers, tears down configs, and mirrors the Paxos helper API.
int shutdown_paxos() {
  es->running = false;

  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->WaitForShutdown();
    }
  }

  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->ShutDown();
    }
  }

  raft_workers_g.clear();
  RandomGenerator::destroy();
  Config::DestroyConfig();

  Log_info("Raft helper shutdown complete.");
  return 0;
}

// microbench_paxos placeholder keeps params aligned with the Paxos helper.
void microbench_paxos() {
  Log_warn("microbench_paxos is not supported for Raft; skipping.");
}

// register_for_follower installs a lightweight follower callback.
void register_for_follower(std::function<void(const char*, int)> cb,
                           uint32_t par_id) {
  for (auto& worker : raft_workers_g) {
    if (worker && worker->IsPartition(par_id) && !worker->IsLeader(par_id)) {
      worker->register_apply_callback(cb);
    }
  }
}

// register_for_follower_par_id wires callbacks that also consume partition id.
void register_for_follower_par_id(
    std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
  for (auto& worker : raft_workers_g) {
    if (worker && worker->IsPartition(par_id) && !worker->IsLeader(par_id)) {
      worker->register_apply_callback_par_id(cb);
    }
  }
}

// register_for_follower_par_id_return stores the full watermark callback then applies it.
void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
  follower_replay_cb[par_id] = cb;
  apply_callbacks_for_partition(par_id);
}

// register_for_leader connects simple leader callbacks to matching workers.
void register_for_leader(std::function<void(const char*, int)> cb,
                         uint32_t par_id) {
  for (auto& worker : raft_workers_g) {
    if (worker && worker->IsLeader(par_id)) {
      worker->register_apply_callback(cb);
    }
  }
}

// register_leader_election_callback saves the external notifier invoked on leadership change.
void register_leader_election_callback(std::function<void(int)> cb) {
  janus::leader_callback_ = std::move(cb);
}

// register_for_leader_par_id registers leader callbacks that want the partition id.
void register_for_leader_par_id(
    std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
  for (auto& worker : raft_workers_g) {
    if (worker && worker->IsLeader(par_id)) {
      worker->register_apply_callback_par_id(cb);
    }
  }
}

// register_for_leader_par_id_return mirrors the Paxos helper and stores callbacks for reuse.
void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
  leader_replay_cb[par_id] = cb;
  apply_callbacks_for_partition(par_id);
}

namespace janus {

// raft_handle_leader_change re-applies callbacks and notifies any waiters.
void raft_handle_leader_change(uint32_t partition_id, bool is_leader) {
  ::apply_callbacks_for_partition(partition_id);
  Log_info("[RAFT-LEADER-CHANGE] par_id=%u is_leader=%s",
           partition_id,
           is_leader ? "true" : "false");

  ::leader_wait_cv.notify_all();

  // Call the callback for BOTH gaining and losing leadership
  if (leader_callback_) {
    leader_callback_(is_leader ? 1 : 0);  // 1 = became leader, 0 = lost leadership
  }
}

}  // namespace janus

// submit forwards a log to the local leader submit queue (if this node leads).
void submit(const char* log, int len, uint32_t par_id) {
  auto* worker = find_worker(par_id);
  if (!worker) {
    Log_warn("submit(): unknown partition %u", par_id);
    return;
  }
  if (!worker->IsLeader(par_id)) {
    Log_debug("submit(): partition %u not on leader, dropping", par_id);
    return;
  }
  enqueue_to_worker(worker, log, len, par_id, 1);
}

// add_log aliases submit for callers that do not care about batching.
void add_log(const char* log, int len, uint32_t par_id) {
  submit(log, len, par_id);
}

// add_log_without_queue matches the Paxos helper signature but still uses queueing.
void add_log_without_queue(const char* log, int len, uint32_t par_id) {
  submit(log, len, par_id);
}

// add_log_to_nc ensures we are leader (waiting briefly if needed) then enqueues.
void add_log_to_nc(const char* log, int len, uint32_t par_id,
                   int batch_size) {
  // Log_debug("[RAFT-ADD-LOG] par_id=%u len=%d batch=%d", par_id, len, batch_size);
  auto* worker = find_worker(par_id);
  if (!worker) {
    Log_warn("[RAFT-ADD-LOG] no worker found for par_id=%u", par_id);
    return;
  }
  bool leader_for_partition = worker->IsLeader(par_id);
  // Log_debug("[RAFT-ADD-LOG] worker leader=%s n_submit=%d n_tot=%d",
  //          leader_for_partition ? "true" : "false",
  //          worker->n_submit.load(),
  //          worker->n_tot.load());
  if (!leader_for_partition) {
    // Match Paxos behavior: immediately drop if not leader (no waiting)
    // Log_debug("[RAFT-ADD-LOG] partition %u not led here, dropping", par_id);
    return;
  }
  enqueue_to_worker(worker, log, len, par_id, std::max(1, batch_size));
  // Log_debug("[RAFT-ADD-LOG] enqueued par_id=%u len=%d batch=%d", par_id, len, batch_size);
}

// wait_for_submit blocks until the worker drained its submission queue.
void wait_for_submit(uint32_t par_id) {
  auto* worker = find_worker(par_id);
  if (!worker) {
    Log_warn("wait_for_submit(): unknown partition %u", par_id);
    return;
  }
  worker->WaitForSubmit();
}

// microbench_paxos_queue placeholder matches Paxos helper API.
void microbench_paxos_queue() {
  Log_warn("microbench_paxos_queue is not supported for Raft; skipping.");
}

// pre_shutdown_step politely drops control RPC connections before shutdown.
void pre_shutdown_step() {
  Log_info("Raft pre_shutdown_step invoked.");
  for (auto& worker : raft_workers_g) {
    if (!worker) {
      continue;
    }
    if (worker->hb_rpc_server_ && worker->scsi_) {
      worker->scsi_->server_shutdown(nullptr);
    }
    worker->WaitForShutdown();
  }
}

// get_epoch proxies to the shared ElectionState singleton.
int get_epoch() {
  return es ? es->get_epoch() : 0;
}

// set_epoch updates ElectionState and propagates the value to all workers.
void set_epoch(int epoch) {
  if (!es) {
    return;
  }
  if (epoch == -1) {
    es->set_epoch();
  } else {
    es->set_epoch(epoch);
  }
  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->cur_epoch = es->get_epoch();
    }
  }
}

// upgrade_p1_to_leader keeps the Paxos helper instrumentation happy under Raft.
void upgrade_p1_to_leader() {
  Log_info("upgrade_p1_to_leader invoked for Raft helper.");
  if (janus::leader_callback_) {
    janus::leader_callback_(0);
  }
}

// worker_info_stats dumps per-partition counters for debugging.
void worker_info_stats(size_t /*worker_id*/) {
  for (auto& worker : raft_workers_g) {
    if (!worker || !worker->site_info_) {
      continue;
    }
    Log_info("partition %u, n_tot=%d, n_current=%d",
             worker->site_info_->partition_id_,
             worker->n_tot.load(),
             worker->n_current.load());
  }
}

// ============================================================================
// PREFERRED REPLICA SYSTEM API IMPLEMENTATION
// ============================================================================

/**
 * Dynamically set the preferred leader for all Raft workers.
 *
 * This allows Mako worker threads to change the preferred leader at runtime.
 * When a new preferred leader is set:
 * 1. All replicas update their voting bias
 * 2. If the new preferred is not currently leader, it starts catch-up monitoring
 * 3. Once caught up, it triggers an election and reclaims leadership
 *
 * @param site_id The site ID of the new preferred leader (or INVALID_SITEID to disable)
 *
 * Example usage:
 *   // Set site 5 as preferred leader
 *   set_preferred_leader(5);
 *
 *   // Disable preference (use standard Raft)
 *   set_preferred_leader(INVALID_SITEID);
 */
void set_preferred_leader(int site_id) {
  siteid_t preferred = static_cast<siteid_t>(site_id);

  Log_info("[PREFERRED-REPLICA-API] Setting preferred leader to site_id=%d", site_id);

  int count = 0;
  for (auto& worker : raft_workers_g) {
    if (!worker) {
      continue;
    }

    auto raft_server = worker->GetRaftServer();
    if (!raft_server) {
      continue;
    }

    raft_server->SetPreferredLeader(preferred);
    count++;

    Log_info("[PREFERRED-REPLICA-API] Updated worker %d: site_id=%d, preferred=%d",
             count, raft_server->site_id_, preferred);
  }

  if (count == 0) {
    Log_warn("[PREFERRED-REPLICA-API] No Raft workers found to update!");
  } else {
    Log_info("[PREFERRED-REPLICA-API] Successfully updated %d Raft workers with preferred_leader=%d",
             count, site_id);
  }
}

// nc_* helpers are unused in Mako+Raft; keep stubs for linkage parity.
void nc_setup_server(int /*port*/, std::string /*ip*/) {
  Log_warn("nc_setup_server not implemented for Raft helper (unused).");
}

std::vector<std::vector<int>>* nc_get_new_order_requests(int /*id*/) {
  Log_warn("nc_get_new_order_requests not implemented for Raft helper.");
  return nullptr;
}

std::vector<std::vector<int>>* nc_get_payment_requests(int /*id*/) {
  Log_warn("nc_get_payment_requests not implemented for Raft helper.");
  return nullptr;
}

std::vector<std::vector<int>>* nc_get_delivery_requests(int /*id*/) {
  Log_warn("nc_get_delivery_requests not implemented for Raft helper.");
  return nullptr;
}

std::vector<std::vector<int>>* nc_get_order_status_requests(int /*id*/) {
  Log_warn("nc_get_order_status_requests not implemented for Raft helper.");
  return nullptr;
}

std::vector<std::vector<int>>* nc_get_stock_level_requests(int /*id*/) {
  Log_warn("nc_get_stock_level_requests not implemented for Raft helper.");
  return nullptr;
}

std::vector<std::vector<int>>* nc_get_read_requests(int /*id*/) {
  Log_warn("nc_get_read_requests not implemented for Raft helper.");
  return nullptr;
}

std::vector<std::vector<int>>* nc_get_rmw_requests(int /*id*/) {
  Log_warn("nc_get_rmw_requests not implemented for Raft helper.");
  return nullptr;
}

#endif  // MAKO_USE_RAFT
