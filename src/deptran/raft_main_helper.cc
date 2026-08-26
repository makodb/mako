#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <type_traits>

#include "replication_helper.h"

#include "__dep__.h"
#include "config.h"
#include "frame.h"
#include "raft/raft_worker.h"
#include "raft/service.h"
#include "paxos_worker.h"  // ElectionState definition lives here
#include <rusty/rusty.hpp>


import std;

using namespace janus;

namespace janus {
vector<shared_ptr<RaftWorker>> raft_workers_g = {};
std::function<void(int)> leader_callback_{};
std::mutex raft_global_callback_mutex;
}

using janus::raft_workers_g;

// ============================================================================
// Raft Implementation Namespace
// ============================================================================
namespace raft_impl {

#if RUSTYCPP_RUST
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(u8)]
pub enum RaftGroupMode {
    kSingleGroup = 0,
    kPerPartitionGroup = 1,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_main.group_mode version=1 rust_sha256=cf63b52398c20221967e157653e2031f5a744c2bc6c31f8bbabc264671f87431*/
enum class RaftGroupMode : uint8_t;
constexpr RaftGroupMode RaftGroupMode_kSingleGroup();
constexpr RaftGroupMode RaftGroupMode_kPerPartitionGroup();

enum class RaftGroupMode : uint8_t {
    kSingleGroup = 0,
    kPerPartitionGroup = 1
};
inline constexpr RaftGroupMode RaftGroupMode_kSingleGroup() { return RaftGroupMode::kSingleGroup; }
inline constexpr RaftGroupMode RaftGroupMode_kPerPartitionGroup() { return RaftGroupMode::kPerPartitionGroup; }
/*RUSTYCPP:GEN-END id=raft_main.group_mode*/

static_assert(std::is_same_v<
              std::underlying_type_t<RaftGroupMode>, uint8_t>);
static_assert(std::is_trivially_copyable_v<RaftGroupMode>);
static_assert(sizeof(RaftGroupMode) == sizeof(uint8_t));
static_assert(alignof(RaftGroupMode) == alignof(uint8_t));
static_assert(static_cast<uint8_t>(RaftGroupMode::kSingleGroup) == 0);
static_assert(static_cast<uint8_t>(RaftGroupMode::kPerPartitionGroup) == 1);
static_assert(RaftGroupMode{} == RaftGroupMode::kSingleGroup);

#if defined(RAFT_DEFAULT_SINGLE_GROUP)
constexpr RaftGroupMode kDefaultRaftGroupMode = RaftGroupMode::kSingleGroup;
#else
constexpr RaftGroupMode kDefaultRaftGroupMode = RaftGroupMode::kPerPartitionGroup;
#endif

static RaftGroupMode raft_group_mode_g = kDefaultRaftGroupMode;
// File-scope storage for local site infos and stub servers.
static std::vector<Config::SiteInfo*> all_site_infos_g;
static std::vector<rrr::Server*> stub_rpc_servers_g;
static std::vector<rusty::Arc<PollThread>> stub_poll_threads_g;
static std::unordered_map<uint32_t, std::shared_ptr<RaftWorker>> workers_by_partition_g;

// leader_replay_cb / follower_replay_cb cache watermark callbacks across role changes.
std::map<int, std::function<int(const char*&, int, int, int,
    std::queue<std::tuple<int, int, int, int, const char *>> &)>> leader_replay_cb;
std::map<int, std::function<int(const char*&, int, int, int,
    std::queue<std::tuple<int, int, int, int, const char *>> &)>> follower_replay_cb;
std::map<uint32_t, std::function<void(const char*, int)>> leader_apply_cb;
std::map<uint32_t, std::function<void(const char*, int)>> follower_apply_cb;
std::map<uint32_t, std::function<void(const char*&, int, int)>>
    leader_partition_apply_cb;
std::map<uint32_t, std::function<void(const char*&, int, int)>>
    follower_partition_apply_cb;

shared_ptr<ElectionState> es = ElectionState::instance();

// send_no_ops_for_mark serialises a Raft NO-OP entry so leader/followers sync watermarks.
void send_no_ops_for_mark(int epoch) {
  std::string log = "no-ops:" + std::to_string(epoch);
  std::vector<std::pair<uint32_t, std::shared_ptr<RaftWorker>>> workers;
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    workers.reserve(workers_by_partition_g.size());
    for (const auto& kv : workers_by_partition_g) {
      workers.push_back(kv);
    }
  }
  for (const auto& kv : workers) {
    uint32_t par_id = kv.first;
    if (!kv.second) {
      continue;
    }
    add_log_to_nc(log.c_str(), static_cast<int>(log.size()), par_id, 1);
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

enum class RaftLaunchState {
  kNotStarted,
  kStarting,
  kStarted,
  kFailedAfterStart,
};
std::mutex raft_launch_state_mutex;
RaftLaunchState raft_launch_state = RaftLaunchState::kNotStarted;

bool launch_can_preflight() {
  std::lock_guard<std::mutex> guard(raft_launch_state_mutex);
  return raft_launch_state == RaftLaunchState::kNotStarted;
}

bool claim_launch() {
  std::lock_guard<std::mutex> guard(raft_launch_state_mutex);
  if (raft_launch_state != RaftLaunchState::kNotStarted) {
    return false;
  }
  raft_launch_state = RaftLaunchState::kStarting;
  return true;
}

void finish_launch(bool succeeded) {
  std::lock_guard<std::mutex> guard(raft_launch_state_mutex);
  raft_launch_state = succeeded ? RaftLaunchState::kStarted
                                : RaftLaunchState::kFailedAfterStart;
}

#if RUSTYCPP_RUST
#[allow(dead_code)]
fn equals_ignore_case(lhs: &str, rhs: &str) -> bool {
    if lhs.len() != rhs.len() {
        return false;
    }
    let lhs_bytes = lhs.as_bytes();
    let rhs_bytes = rhs.as_bytes();
    let mut i: usize = 0;
    while i < lhs_bytes.len() {
        if unsafe { tolower(lhs_bytes[i] as i32) } !=
            unsafe { tolower(rhs_bytes[i] as i32) } {
            return false;
        }
        i += 1;
    }
    true
}

unsafe extern "C" {
    fn tolower(value: i32) -> i32;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_main.argument_casefold version=1 rust_sha256=aab3bef549cca60d628e6d6caf76e12a44624e86ed820f1f5bd092f9414321f4*/
bool equals_ignore_case(std::string_view lhs, std::string_view rhs);

extern "C" {
    int32_t tolower(int32_t value);
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    if (rusty::len(lhs) != rusty::len(rhs)) {
        return false;
    }
    const auto lhs_bytes = rusty::as_bytes(lhs);
    const auto rhs_bytes = rusty::as_bytes(rhs);
    size_t i = static_cast<size_t>(0);
    while (rusty::detail::deref_if_pointer_like(i) < rusty::len(lhs_bytes)) {
        if (tolower(static_cast<int32_t>(lhs_bytes[i])) != tolower(static_cast<int32_t>(rhs_bytes[i]))) {
            return false;
        }
        i += 1;
    }
    return true;
}
/*RUSTYCPP:GEN-END id=raft_main.argument_casefold*/

std::optional<RaftGroupMode> parse_raft_group_mode_from_args(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i] ? argv[i] : "");
    constexpr std::string_view kPrefix = "--raft-groups=";
    if (arg.rfind(kPrefix, 0) == 0) {
      std::string_view value = arg.substr(kPrefix.size());
      if (equals_ignore_case(value, "single")) {
        return RaftGroupMode::kSingleGroup;
      }
      if (equals_ignore_case(value, "multi") || equals_ignore_case(value, "per-partition")) {
        return RaftGroupMode::kPerPartitionGroup;
      }
    }
    if (arg == "--raft-groups" && i + 1 < argc) {
      std::string_view value(argv[++i] ? argv[i] : "");
      if (equals_ignore_case(value, "single")) {
        return RaftGroupMode::kSingleGroup;
      }
      if (equals_ignore_case(value, "multi") || equals_ignore_case(value, "per-partition")) {
        return RaftGroupMode::kPerPartitionGroup;
      }
    }
  }
  return std::nullopt;
}

std::optional<RaftGroupMode> parse_raft_group_mode_from_env() {
  const char* env = std::getenv("MAKO_RAFT_GROUP_MODE");
  if (!env) {
    return std::nullopt;
  }
  std::string_view value(env);
  if (equals_ignore_case(value, "single")) {
    return RaftGroupMode::kSingleGroup;
  }
  if (equals_ignore_case(value, "multi") || equals_ignore_case(value, "per-partition")) {
    return RaftGroupMode::kPerPartitionGroup;
  }
  Log_warn("[RAFT-SETUP] Ignoring invalid MAKO_RAFT_GROUP_MODE={} (expected single|multi)",
           env);
  return std::nullopt;
}

void configure_raft_group_mode(int argc, char* argv[]) {
  raft_group_mode_g = kDefaultRaftGroupMode;
  if (auto from_env = parse_raft_group_mode_from_env(); from_env.has_value()) {
    raft_group_mode_g = *from_env;
  }
  if (auto from_args = parse_raft_group_mode_from_args(argc, argv); from_args.has_value()) {
    raft_group_mode_g = *from_args;
  }
  Log_info("[RAFT-SETUP] raft group mode: {}",
           raft_group_mode_g == RaftGroupMode::kSingleGroup ? "single" : "multi");
}

#if RUSTYCPP_RUST
#[allow(dead_code)]
fn is_raft_group_mode_arg(arg: &str) -> bool {
    arg == "--raft-groups" || arg.starts_with("--raft-groups=")
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_main.group_mode_argument_predicate version=1 rust_sha256=a70c0881db3d75205a090a1ddfb8894ed2e796293ef72dcee1361bedd884da53*/
bool is_raft_group_mode_arg(std::string_view arg);

bool is_raft_group_mode_arg(std::string_view arg) {
    return (rusty::detail::deref_if_pointer_like(rusty::to_string_view(arg)) == std::string_view("--raft-groups")) || rusty::starts_with(arg, "--raft-groups=");
}
/*RUSTYCPP:GEN-END id=raft_main.group_mode_argument_predicate*/

void build_config_argv_without_raft_group_mode(
    int argc,
    char* argv[],
    std::vector<std::string>& argv_storage,
    std::vector<char*>& argv_filtered) {
  argv_storage.clear();
  argv_filtered.clear();
  argv_storage.reserve(static_cast<size_t>(argc));
  argv_filtered.reserve(static_cast<size_t>(argc));

  for (int i = 0; i < argc; ++i) {
    std::string_view arg(argv[i] ? argv[i] : "");
    if (arg == "--raft-groups" && i + 1 < argc) {
      ++i;  // Skip value token too.
      continue;
    }
    if (is_raft_group_mode_arg(arg)) {
      continue;
    }
    argv_storage.emplace_back(arg);
  }

  for (auto& arg : argv_storage) {
    argv_filtered.push_back(const_cast<char*>(arg.c_str()));
  }
}

void log_wait_outcome(uint32_t par_id, bool success, std::chrono::milliseconds waited) {
  Log_info("[RAFT-WAIT-LEADERSHIP] par_id={} status={} waited_ms={}",
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
  Log_info("[RAFT-WAIT-LEADERSHIP] par_id={} initial_leader={} timeout_ms={}",
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

// SINGLE-RAFT: Create stub RPC servers on extra partition ports.
// Remote replicas' Communicators expect to connect to all partition ports.
// Stub servers register the same RaftServiceImpl pointing to the single RaftServer.
void create_stub_servers() {
  if (raft_group_mode_g != RaftGroupMode::kSingleGroup) {
    return;
  }
  if (all_site_infos_g.size() <= 1) {
    Log_info("[SINGLE-RAFT] Only 1 site, no stub servers needed");
    return;
  }

  auto& worker = raft_workers_g[0];
  auto* rep_sched = dynamic_cast<RaftServer*>(worker->rep_sched_);
  verify(rep_sched != nullptr);

  for (size_t i = 1; i < all_site_infos_g.size(); i++) {
    auto* site_info = all_site_infos_g[i];
    std::string bind_addr = site_info->GetBindAddress();

    // Create a PollThread for this stub
    auto poll_thread = rrr::PollThread::create();
    stub_poll_threads_g.push_back(poll_thread);

    // Create RPC server
    auto* rpc_server = new rrr::Server(rrr::Server::new_(rusty::Some(poll_thread.clone())));
    rpc_server->set_admission_ready(false);

    // Register RaftServiceImpl pointing to the single RaftServer
    rpc_server->reg_service_typed(rusty::make_box<RaftServiceImpl>(rep_sched, poll_thread.clone()));

    // Bind to the site's port
    int ret = rpc_server->start(reinterpret_cast<const int8_t*>(bind_addr.c_str()));
    if (ret != 0) {
      Log_fatal("[SINGLE-RAFT] Stub server failed to bind at {}", bind_addr.c_str());
    }

    stub_rpc_servers_g.push_back(rpc_server);
    Log_info("[SINGLE-RAFT] Created stub server on {} for site {} (partition {})",
             bind_addr.c_str(), site_info->id, site_info->partition_id_);
  }

  Log_info("[SINGLE-RAFT] Created {} stub servers", stub_rpc_servers_g.size());
}

// SINGLE-RAFT: Shutdown and clean up stub servers
void destroy_stub_servers() {
  for (auto* server : stub_rpc_servers_g) {
    if (server) {
      delete server;
    }
  }
  stub_rpc_servers_g.clear();

  for (auto& pt : stub_poll_threads_g) {
    pt->shutdown();
  }
  stub_poll_threads_g.clear();

  Log_info("[SINGLE-RAFT] Destroyed stub servers");
}

// server_launch_worker finishes wiring RPC/commo threads and starts batching loops.
bool server_launch_worker(std::vector<Config::SiteInfo>& server_sites) {
  if (server_sites.empty()) {
    return true;
  }
  if (!launch_can_preflight()) {
    Log_error("[RAFT-LAUNCH] setup2() cannot be repeated after launch has started");
    return false;
  }

  if (raft_group_mode_g == RaftGroupMode::kSingleGroup) {
    Log_info("[SINGLE-RAFT] server_sites.size()={} raft_workers_g.size()={}",
             server_sites.size(), raft_workers_g.size());

    if (raft_workers_g.empty() || !raft_workers_g[0]) {
      Log_error("[SINGLE-RAFT] No worker to launch!");
      return false;
    }

    auto& worker = raft_workers_g[0];
    std::vector<uint32_t> partition_ids;
    partition_ids.reserve(all_site_infos_g.size());
    for (const auto* site : all_site_infos_g) {
      if (site != nullptr) {
        partition_ids.push_back(site->partition_id_);
      }
    }
    if (!worker->PrepareForStartup(partition_ids)) {
      Log_error("[SINGLE-RAFT] Application callbacks are incomplete; startup remains closed");
      return false;
    }
    if (!claim_launch()) {
      Log_error("[SINGLE-RAFT] Another setup2() launch won the startup race");
      return false;
    }

    worker->SetupService();
    create_stub_servers();
    worker->SetupCommo();

    if (auto raft_server = worker->GetRaftServer()) {
      auto poll_worker_opt = worker->GetPollThreadWorker();
      if (poll_worker_opt.is_some()) {
        auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([raft_server]() {
          Log_info("[RAFTPOLL] EnsureSetup executing (site={} par={})",
                   raft_server->site_id_, raft_server->partition_id_);
          raft_server->EnsureSetup();
        }));
        Log_info("[RAFTPOLL] Queueing EnsureSetup job for single worker");
        poll_worker_opt.unwrap()->add(rusty::Arc<Job>(arc_job));
      } else {
        raft_server->EnsureSetup();
      }
    }

    if (!worker->WaitForStartup()) {
      Log_error("single-group Raft startup failed");
      finish_launch(false);
      return false;
    }
    worker->SetRpcAdmissionReady(true);
    for (auto* stub_server : stub_rpc_servers_g) {
      if (stub_server != nullptr) {
        stub_server->set_admission_ready(true);
      }
    }

    worker->StartSubmitThread();
    worker->SetupHeartbeat();
    finish_launch(true);
    return true;
  }

  Log_info("[RAFT-LAUNCH] server_sites.size()={} raft_workers_g.size()={}",
           server_sites.size(), raft_workers_g.size());
  if (server_sites.size() != raft_workers_g.size()) {
    Log_error("[RAFT-LAUNCH] Site/worker count mismatch: {} sites, {} workers",
              server_sites.size(), raft_workers_g.size());
    return false;
  }

  // Preflight every state-machine callback before opening even one listener.
  // This makes startup all-or-nothing with respect to application readiness
  // and leaves callers free to register missing callbacks and retry setup2().
  for (const auto& worker : raft_workers_g) {
    if (!worker || worker->site_info_ == nullptr ||
        !worker->PrepareForStartup(
            {worker->site_info_->partition_id_})) {
      Log_error("[RAFT-LAUNCH] Application callbacks are incomplete");
      return false;
    }
  }
  if (!claim_launch()) {
    Log_error("[RAFT-LAUNCH] Another setup2() launch won the startup race");
    return false;
  }

  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->SetupService();
    }
  }

  for (size_t i = 0; i < raft_workers_g.size(); ++i) {
    auto& worker = raft_workers_g[i];
    if (!worker) continue;

    worker->SetupCommo();

    if (auto raft_server = worker->GetRaftServer()) {
      auto poll_worker_opt = worker->GetPollThreadWorker();
      if (poll_worker_opt.is_some()) {
        auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_([raft_server]() {
          Log_info("[RAFTPOLL] EnsureSetup executing (site={} par={})",
                   raft_server->site_id_, raft_server->partition_id_);
          raft_server->EnsureSetup();
        }));
        poll_worker_opt.unwrap()->add(rusty::Arc<Job>(arc_job));
      } else {
        raft_server->EnsureSetup();
      }
    }

  }

  // Recovery may complete in any order, but no application listener opens
  // until every worker has crossed the startup success barrier.
  for (size_t i = 0; i < raft_workers_g.size(); ++i) {
    auto& worker = raft_workers_g[i];
    if (!worker || !worker->WaitForStartup()) {
      Log_error("Raft startup failed for worker {}", i);
      finish_launch(false);
      return false;
    }
  }

  for (auto& worker : raft_workers_g) {
    worker->SetRpcAdmissionReady(true);
  }
  for (auto& worker : raft_workers_g) {
    worker->StartSubmitThread();
  }

  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->SetupHeartbeat();
    }
  }
  finish_launch(true);
  return true;
}

std::shared_ptr<RaftWorker> find_worker(uint32_t par_id) {
  std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
  auto it = workers_by_partition_g.find(par_id);
  if (it == workers_by_partition_g.end() || !it->second) {
    return {};
  }
  return it->second;
}

std::vector<std::shared_ptr<RaftWorker>> snapshot_workers() {
  std::vector<std::shared_ptr<RaftWorker>> workers;
  std::set<RaftWorker*> seen;
  std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
  workers.reserve(workers_by_partition_g.size());
  for (const auto& kv : workers_by_partition_g) {
    if (kv.second && seen.insert(kv.second.get()).second) {
      workers.push_back(kv.second);
    }
  }
  return workers;
}

// enqueue_to_worker increments bookkeeping and drops the payload into the worker queue.
void enqueue_to_worker(const std::shared_ptr<RaftWorker>& worker,
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

void apply_callbacks_for_partition(uint32_t par_id) {
  std::shared_ptr<RaftWorker> worker;
  watermark_callback_t leader_callback;
  watermark_callback_t follower_callback;
  std::function<void(const char*, int)> leader_apply_callback;
  std::function<void(const char*, int)> follower_apply_callback;
  std::function<void(const char*&, int, int)> leader_partition_callback;
  std::function<void(const char*&, int, int)> follower_partition_callback;
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    auto worker_it = workers_by_partition_g.find(par_id);
    if (worker_it != workers_by_partition_g.end()) {
      worker = worker_it->second;
    }
    auto leader_it = leader_replay_cb.find(par_id);
    if (leader_it != leader_replay_cb.end()) {
      leader_callback = leader_it->second;
    }
    auto follower_it = follower_replay_cb.find(par_id);
    if (follower_it != follower_replay_cb.end()) {
      follower_callback = follower_it->second;
    }
    auto leader_apply_it = leader_apply_cb.find(par_id);
    if (leader_apply_it != leader_apply_cb.end()) {
      leader_apply_callback = leader_apply_it->second;
    }
    auto follower_apply_it = follower_apply_cb.find(par_id);
    if (follower_apply_it != follower_apply_cb.end()) {
      follower_apply_callback = follower_apply_it->second;
    }
    auto leader_partition_it = leader_partition_apply_cb.find(par_id);
    if (leader_partition_it != leader_partition_apply_cb.end()) {
      leader_partition_callback = leader_partition_it->second;
    }
    auto follower_partition_it = follower_partition_apply_cb.find(par_id);
    if (follower_partition_it != follower_partition_apply_cb.end()) {
      follower_partition_callback = follower_partition_it->second;
    }
  }
  if (!worker) {
    return;
  }
  if (leader_callback) {
    worker->register_leader_callback_for_partition(
        par_id, std::move(leader_callback));
  }
  if (follower_callback) {
    worker->register_follower_callback_for_partition(
        par_id, std::move(follower_callback));
  }
  if (leader_apply_callback) {
    worker->register_leader_apply_callback_for_partition(
        par_id, std::move(leader_apply_callback));
  }
  if (follower_apply_callback) {
    worker->register_follower_apply_callback_for_partition(
        par_id, std::move(follower_apply_callback));
  }
  if (leader_partition_callback) {
    worker->register_leader_partition_apply_callback_for_partition(
        par_id, std::move(leader_partition_callback));
  }
  if (follower_partition_callback) {
    worker->register_follower_partition_apply_callback_for_partition(
        par_id, std::move(follower_partition_callback));
  }
}

}  // anonymous namespace

// @safe - Helper function accessible from janus:: namespace for leader change handling.
// Calls internal functions to apply callbacks and notify waiting threads.
// No ownership transfer; uses references to internal state.
void handle_leader_change_impl(uint32_t partition_id) {
  apply_callbacks_for_partition(partition_id);
  leader_wait_cv.notify_all();
}

// setup creates raft workers according to caller-selected group mode.
std::vector<std::string> setup(int argc, char* argv[]) {
  std::vector<std::string> ret_vector;
  if (!launch_can_preflight()) {
    Log_error("[RAFT-SETUP] setup() cannot replace a live or partially started Raft lifecycle");
    return ret_vector;
  }
  check_current_path();

  if (std::getenv("MAKO_DISABLE_JETPACK") == nullptr) {
    setenv("MAKO_DISABLE_JETPACK", "1", 1);
    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK unset; forcing helper default of 1");
  } else {
    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK={}", std::getenv("MAKO_DISABLE_JETPACK"));
  }

  configure_raft_group_mode(argc, argv);

  std::vector<std::string> filtered_argv_storage;
  std::vector<char*> filtered_argv;
  build_config_argv_without_raft_group_mode(
      argc, argv, filtered_argv_storage, filtered_argv);

  int ret = Config::CreateConfig(static_cast<int>(filtered_argv.size()),
                                 filtered_argv.data());
  if (ret != SUCCESS) {
    Log_fatal("Read config failed");
    return ret_vector;
  }

  // Verify that replica_proto_ is set to MODE_RAFT via raft.yml config.
  auto config = Config::GetConfig();
  if (config->replica_proto_ != MODE_RAFT) {
    Log_warn("[RAFT-SETUP] replica_proto_={} is not MODE_RAFT ({}). "
             "Make sure to use config/raft.yml with 'ab: raft' setting.",
             config->replica_proto_, MODE_RAFT);
  } else {
    Log_info("[RAFT-SETUP] replica_proto_ correctly set to MODE_RAFT ({})",
             config->replica_proto_);
  }

  auto server_infos = Config::GetConfig()->GetMyServers();

  raft_workers_g.clear();
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    workers_by_partition_g.clear();
  }
  all_site_infos_g.clear();
  // Callback registration is intentionally not cleared here. Callers may
  // register before setup(), and Mako registers its election callback before
  // configuration creates workers. shutdown_paxos() owns lifecycle cleanup.

  // Preserve historical ordering behavior for return vector.
  for (int i = static_cast<int>(server_infos.size()) - 1; i >= 0; --i) {
    const auto& site = Config::GetConfig()->SiteById(server_infos[i].id);
    ret_vector.push_back(site.name);
    all_site_infos_g.push_back(const_cast<Config::SiteInfo*>(&site));
  }
  std::reverse(all_site_infos_g.begin(), all_site_infos_g.end());

  if (raft_group_mode_g == RaftGroupMode::kSingleGroup) {
    if (!all_site_infos_g.empty()) {
      auto worker = std::make_shared<RaftWorker>();
      worker->site_info_ = all_site_infos_g[0];
      worker->handles_all_partitions_ = true;
      worker->SetupBase();
      raft_workers_g.push_back(worker);
      {
        std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
        for (auto* site : all_site_infos_g) {
          workers_by_partition_g[site->partition_id_] = worker;
        }
      }
      for (auto* site : all_site_infos_g) {
        apply_callbacks_for_partition(site->partition_id_);
      }
      Log_info("[SINGLE-RAFT] Created 1 worker for site {} (partition {}), total sites={}",
               all_site_infos_g[0]->id, all_site_infos_g[0]->partition_id_,
               all_site_infos_g.size());
    }

    if (!raft_workers_g.empty() && raft_workers_g[0]->site_info_) {
      es->machine_id = raft_workers_g[0]->site_info_->locale_id;
    }
    return ret_vector;
  }

  // Multi-group mode: create one worker per partition/site.
  for (int i = static_cast<int>(server_infos.size()) - 1; i >= 0; --i) {
    const auto& site = Config::GetConfig()->SiteById(server_infos[i].id);
    auto worker = std::make_shared<RaftWorker>();
    worker->site_info_ = const_cast<Config::SiteInfo*>(&site);
    worker->handles_all_partitions_ = false;
    worker->SetupBase();
    {
      std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
      workers_by_partition_g[site.partition_id_] = worker;
    }
    raft_workers_g.push_back(std::move(worker));
    apply_callbacks_for_partition(site.partition_id_);
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

  if (raft_group_mode_g == RaftGroupMode::kSingleGroup) {
    // Single-group mode: one local worker carries all partitions.
    if (!raft_workers_g.empty() && raft_workers_g[0]) {
      auto raft_server = raft_workers_g[0]->GetRaftServer();
      if (raft_server) {
        parid_t partition_id = raft_server->partition_id_;
        siteid_t my_site_id = raft_server->site_id_;

        auto partition_sites = config->SitesByPartitionId(partition_id);

        siteid_t preferred_site_id = INVALID_SITEID;
        for (const auto& site : partition_sites) {
          if (site.locale_id == 0) {
            preferred_site_id = site.id;
            break;
          }
        }

        if (preferred_site_id != INVALID_SITEID) {
          raft_server->SetPreferredLeader(preferred_site_id);
          Log_info("[SINGLE-RAFT] Partition {}, Site {}: Set preferred leader to site {}",
                   partition_id, my_site_id, preferred_site_id);
        }
      }
    }
    Log_info("[SINGLE-RAFT] Preferred replica system configured (localhost preferred)");
  } else {
    // Multi-group mode: set preferred leader for each partition worker.
    for (auto& worker : raft_workers_g) {
      if (!worker) continue;

      auto raft_server = worker->GetRaftServer();
      if (!raft_server) continue;

      parid_t partition_id = raft_server->partition_id_;
      siteid_t my_site_id = raft_server->site_id_;

      auto partition_sites = config->SitesByPartitionId(partition_id);

      siteid_t preferred_site_id = INVALID_SITEID;
      for (const auto& site : partition_sites) {
        if (site.locale_id == 0) {
          preferred_site_id = site.id;
          break;
        }
      }

      if (preferred_site_id != INVALID_SITEID) {
        raft_server->SetPreferredLeader(preferred_site_id);
        Log_info("[PREFERRED-REPLICA] Partition {}, Site {}: Set preferred leader to site {}",
                 partition_id, my_site_id, preferred_site_id);
      }
    }
    Log_info("[RAFT-SETUP] Preferred replica system configured for all partitions");
  }

  // Update election state for Paxos compatibility
  if (es->machine_id == 0) {
    es->set_state(0);   // Will become leader via election
    es->set_epoch(0);
    es->set_leader(0);
    Log_info("[RAFT-SETUP] Machine {}: Preferred leader (will win elections naturally)",
             es->machine_id);
  } else {
    es->set_state(0);
    es->set_epoch(0);
    es->set_leader(0);
    Log_info("[RAFT-SETUP] Machine {}: Non-preferred replica (can take over if preferred fails)",
             es->machine_id);
  }

  // Launch workers (normal Raft elections will proceed, biased toward preferred)
  if (!server_infos.empty()) {
    if (!server_launch_worker(server_infos)) {
      Log_error("[RAFT-SETUP] Worker launch failed; RPC admission remains closed");
      return -1;
    }
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
  auto worker = find_worker(par_id);
  if (!worker) {
    Log_warn("get_outstanding_logs(): unknown partition {}", par_id);
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

  if (raft_group_mode_g == RaftGroupMode::kSingleGroup) {
    destroy_stub_servers();
  }

  for (auto& worker : raft_workers_g) {
    if (worker) {
      worker->ShutDown();
    }
  }

  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    workers_by_partition_g.clear();
    leader_replay_cb.clear();
    follower_replay_cb.clear();
    leader_apply_cb.clear();
    follower_apply_cb.clear();
    leader_partition_apply_cb.clear();
    follower_partition_apply_cb.clear();
    janus::leader_callback_ = {};
  }
  raft_workers_g.clear();
  all_site_infos_g.clear();
  {
    std::lock_guard<std::mutex> guard(raft_launch_state_mutex);
    raft_launch_state = RaftLaunchState::kNotStarted;
  }
  RandomGenerator::destroy();
  Config::DestroyConfig();

  Log_info("Raft helper shutdown complete.");
  return 0;
}

// removed `microbench_paxos()` Log_warn-only
// stub — both impls (paxos + raft) and the dispatcher in
// `replication_helper.cc` are gone (no callers anywhere in tree).

// register_for_follower installs a lightweight follower callback.
void register_for_follower(std::function<void(const char*, int)> cb,
                           uint32_t par_id) {
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    follower_apply_cb[par_id] = std::move(cb);
  }
  apply_callbacks_for_partition(par_id);
}

// register_for_follower_par_id wires callbacks that also consume partition id.
void register_for_follower_par_id(
    std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    follower_partition_apply_cb[par_id] = std::move(cb);
  }
  apply_callbacks_for_partition(par_id);
}

// register_for_follower_par_id_return stores the full watermark callback then applies it.
void register_for_follower_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    follower_replay_cb[par_id] = std::move(cb);
  }
  apply_callbacks_for_partition(par_id);
}

// register_for_leader connects simple leader callbacks to matching workers.
void register_for_leader(std::function<void(const char*, int)> cb,
                         uint32_t par_id) {
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    leader_apply_cb[par_id] = std::move(cb);
  }
  apply_callbacks_for_partition(par_id);
}

// register_leader_election_callback saves the external notifier invoked on leadership change.
void register_leader_election_callback(std::function<void(int)> cb) {
  std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
  janus::leader_callback_ = std::move(cb);
}

// register_for_leader_par_id registers leader callbacks that want the partition id.
void register_for_leader_par_id(
    std::function<void(const char*&, int, int)> cb, uint32_t par_id) {
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    leader_partition_apply_cb[par_id] = std::move(cb);
  }
  apply_callbacks_for_partition(par_id);
}

// register_for_leader_par_id_return mirrors the Paxos helper and stores callbacks for reuse.
void register_for_leader_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb,
    uint32_t par_id) {
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    leader_replay_cb[par_id] = std::move(cb);
  }
  apply_callbacks_for_partition(par_id);
}

// Note: raft_handle_leader_change is moved to the end of this file,
// outside the raft_impl namespace, so it's in janus:: namespace.

// submit forwards a log to the local leader submit queue (if this node leads).
void submit(const char* log, int len, uint32_t par_id) {
  auto worker = find_worker(par_id);
  if (!worker) {
    Log_warn("submit(): unknown partition {}", par_id);
    return;
  }
  // Do not pre-check leadership here. RaftServer::Start() performs the
  // authoritative leadership check under server lock; pre-checking can race.
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

// @unsafe - checks leadership, enqueues log, returns false with leader hint if not leader
bool add_log_to_nc(const char* log, int len, uint32_t par_id,
                   int batch_size, siteid_t* leader_hint_out /* = nullptr */) {
  // Log_debug("[RAFT-ADD-LOG] par_id={} len={} batch={}", par_id, len, batch_size);
  auto worker = find_worker(par_id);
  if (!worker) {
    Log_warn("[RAFT-ADD-LOG] no worker found for par_id={}", par_id);
    if (leader_hint_out) {
      // @unsafe
      { *leader_hint_out = INVALID_SITEID; }
    }
    return false;
  }
  // Pre-check leadership so callers get an immediate error with a leader hint
  // instead of silently dropping the log deep inside RaftServer::Start().
  if (!worker->IsLeader(par_id)) {
    siteid_t hint = worker->GetLeaderHint();
    // @unsafe
    {
    Log_warn("[RAFT-ADD-LOG] par_id={}: not leader, leader_hint={}", par_id, hint);
    }
    if (leader_hint_out) {
      // @unsafe
      { *leader_hint_out = hint; }
    }
    return false;
  }
  enqueue_to_worker(worker, log, len, par_id, std::max(1, batch_size));
  // Log_debug("[RAFT-ADD-LOG] enqueued par_id={} len={} batch={}", par_id, len, batch_size);
  if (leader_hint_out) {
    // @unsafe
    { *leader_hint_out = INVALID_SITEID; }
  }
  return true;
}

// wait_for_submit blocks until the worker drained its submission queue.
void wait_for_submit(uint32_t par_id) {
  auto worker = find_worker(par_id);
  if (!worker) {
    Log_warn("wait_for_submit(): unknown partition {}", par_id);
    return;
  }
  worker->WaitForSubmit();
}

// removed `microbench_paxos_queue()`
// Log_warn-only stub — counterpart in paxos_impl deleted; no callers.

// pre_shutdown_step politely drops control RPC connections before shutdown.
void pre_shutdown_step() {
  Log_info("Raft pre_shutdown_step invoked.");
  auto workers = snapshot_workers();
  for (auto& worker : workers) {
    if (!worker) {
      continue;
    }
    if (worker->hb_rpc_server_) {
      worker->hb_rpc_server_->do_shutdown();
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
  std::function<void(int)> callback;
  {
    std::lock_guard<std::mutex> guard(janus::raft_global_callback_mutex);
    callback = ::janus::leader_callback_;
  }
  if (callback) {
    callback(0);
  }
}

// worker_info_stats dumps per-partition counters for debugging.
void worker_info_stats(size_t /*worker_id*/) {
  for (auto& worker : raft_workers_g) {
    if (!worker || !worker->site_info_) {
      continue;
    }
    Log_info("partition {}, n_tot={}, n_current={}",
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

  Log_info("[PREFERRED-REPLICA-API] Setting preferred leader to site_id={}", site_id);

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

    Log_info("[PREFERRED-REPLICA-API] Updated worker {}: site_id={}, preferred={}",
             count, raft_server->site_id_, preferred);
  }

  if (count == 0) {
    Log_warn("[PREFERRED-REPLICA-API] No Raft workers found to update!");
  } else {
    Log_info("[PREFERRED-REPLICA-API] Successfully updated {} Raft workers with preferred_leader={}",
             count, site_id);
  }
}

}  // namespace raft_impl

// ============================================================================
// Functions in janus:: namespace (outside raft_impl) for callback compatibility
// @safe - These functions are called by RaftWorker and must be in janus::
// namespace. They delegate to raft_impl and use the global leader_callback_.
// ============================================================================
namespace janus {

// @safe - Called by RaftWorker on leadership changes. Delegates to raft_impl
// helper and invokes the registered callback if present. No ownership transfer.
void raft_handle_leader_change(uint32_t partition_id, bool is_leader) {
  raft_impl::handle_leader_change_impl(partition_id);

  // Call the callback for BOTH gaining and losing leadership
  std::function<void(int)> callback;
  {
    std::lock_guard<std::mutex> guard(raft_global_callback_mutex);
    callback = leader_callback_;
  }
  if (callback) {
    callback(is_leader ? 1 : 0);  // 1 = became leader, 0 = lost leadership
  }
}

// @safe - Runtime dispatch wrapper. Only calls raft_handle_leader_change if
// the runtime replication type is Raft. No-op for Paxos mode.
void NotifyRaftLeaderChange(uint32_t partition_id, bool is_leader) {
  if (is_using_raft()) {
    raft_handle_leader_change(partition_id, is_leader);
  }
}

}  // namespace janus
