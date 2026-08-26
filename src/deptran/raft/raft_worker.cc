#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <utility>

#include <rusty/slice.hpp>

#include "raft_worker.h"
#include "server.h"
#include "commo.h"
#include "service.h"
#include "application_log.h"
#include "../config.h"
#include "../legacy_raft_log_payload.h"
#include "../paxos/commo.h"   // PaxosStatus enum reused by Mako watermark callbacks
#include "../replication_log_entry.h"
#include "../tpc_command.h"  // TpcCommitCommand for batch optimization

import std;

// @external: {
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   Log_warn: [safe, (...) -> void]
//   Log_error: [safe, (...) -> void]
//   Log_fatal: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   std::lock_guard: [safe, (...) -> owned]
//   std::make_shared: [safe, (...) -> owned]
//   std::dynamic_pointer_cast: [safe, (...) -> owned]
//   std::static_pointer_cast: [safe, (...) -> owned]
//   Config::GetConfig: [safe, () -> *]
//   janus::Config::get_tot_req: [safe, (&'a) -> int]
//   janus::Frame::CreateScheduler: [safe, (&'a mut) -> *]
//   janus::TxLogServer::RegLearnerAction: [safe, (&'a mut, ...) -> void]
//   rusty::Option::clone: [safe, (&'a) -> owned]
//   memcpy: [safe, (void*, const void*, size_t) -> void*]
//   std::string::assign: [safe, (&'a mut, ...) -> &'a mut]
//   std::this_thread::sleep_for: [safe, (...) -> void]
//   std::thread::joinable: [safe, (&'a) -> bool]
//   std::thread::join: [safe, (&'a mut) -> void]
//   std::condition_variable::notify_all: [safe, (&'a mut) -> void]
//   std::condition_variable::notify_one: [safe, (&'a mut) -> void]
//   std::condition_variable::wait: [safe, (&'a mut, ...) -> void]
//   std::bind: [safe, (...) -> owned]
// }

namespace janus {

// Pure worker decisions over copied scalar values. Thread lifecycle,
// condition-variable waits, queue mutation, callbacks, allocation, and Raft
// submission stay in the existing C++ control flow. `const fn` makes the
// generated helpers constexpr and therefore implicitly inline.
#if RUSTYCPP_RUST
pub const fn raft_worker_should_start_submit_thread(started: bool) -> bool {
    !started
}

pub const fn raft_worker_should_stop_submit_thread(started: bool) -> bool {
    started
}

pub const fn raft_worker_should_enqueue(started: bool) -> bool {
    started
}

pub const fn raft_worker_batch_limit(batch_size: i32) -> i32 {
    if batch_size < 1 {
        1
    } else {
        batch_size
    }
}

pub const fn raft_worker_wait_for_submit(submitted: i32, total: i32) -> bool {
    submitted < total
}

pub const fn raft_worker_queue_has_work(queue_empty: bool) -> bool {
    !queue_empty
}

pub const fn raft_worker_submit_loop_should_wake(queue_empty: bool) -> bool {
    !queue_empty
}

pub const fn raft_worker_submit_loop_should_stop(queue_empty: bool) -> bool {
    queue_empty
}

pub const fn raft_worker_submit_loop_should_take(batch_size: i32,
                                                  limit: i32) -> bool {
    batch_size < limit
}

pub const fn raft_worker_partition_matches(worker_partition: u32,
                                            requested_partition: u32) -> bool {
    worker_partition == requested_partition
}

pub const fn raft_worker_scheduler_available(has_scheduler: bool) -> bool {
    has_scheduler
}

pub const fn raft_worker_leader_flag_from_bool(is_leader: bool) -> i32 {
    if is_leader {
        1
    } else {
        0
    }
}

pub const fn raft_worker_should_notify_default_partition(
    callback_partitions_empty: bool,
) -> bool {
    callback_partitions_empty
}

pub const fn raft_worker_has_command_payload(has_value: bool) -> bool {
    has_value
}

pub const fn raft_worker_callback_available(has_callback: bool) -> bool {
    has_callback
}

pub const fn raft_worker_should_buffer_unreplayed(status: i32,
                                                   safety_fail_status: i32,
                                                   len: i32) -> bool {
    status == safety_fail_status && len > 0
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_worker.scalar_decisions version=1 rust_sha256=5bfebc26fae75ef372bc70340d3e47e1fa56c0dc3d00c6510fc30988d1bfc79e*/
constexpr bool raft_worker_should_start_submit_thread(bool started);
constexpr bool raft_worker_should_stop_submit_thread(bool started);
constexpr bool raft_worker_should_enqueue(bool started);
constexpr int32_t raft_worker_batch_limit(int32_t batch_size);
constexpr bool raft_worker_wait_for_submit(int32_t submitted, int32_t total);
constexpr bool raft_worker_queue_has_work(bool queue_empty);
constexpr bool raft_worker_submit_loop_should_wake(bool queue_empty);
constexpr bool raft_worker_submit_loop_should_stop(bool queue_empty);
constexpr bool raft_worker_submit_loop_should_take(int32_t batch_size, int32_t limit);
constexpr bool raft_worker_partition_matches(uint32_t worker_partition, uint32_t requested_partition);
constexpr bool raft_worker_scheduler_available(bool has_scheduler);
constexpr int32_t raft_worker_leader_flag_from_bool(bool is_leader);
constexpr bool raft_worker_should_notify_default_partition(bool callback_partitions_empty);
constexpr bool raft_worker_has_command_payload(bool has_value);
constexpr bool raft_worker_callback_available(bool has_callback);
constexpr bool raft_worker_should_buffer_unreplayed(int32_t status, int32_t safety_fail_status, int32_t len);
constexpr bool raft_worker_should_start_submit_thread(bool started) {
    return !started;
}
constexpr bool raft_worker_should_stop_submit_thread(bool started) {
    return std::move(started);
}
constexpr bool raft_worker_should_enqueue(bool started) {
    return std::move(started);
}
constexpr int32_t raft_worker_batch_limit(int32_t batch_size) {
    if (rusty::detail::deref_if_pointer_like(batch_size) < 1) {
        return static_cast<int32_t>(1);
    } else {
        return std::move(batch_size);
    }
}
constexpr bool raft_worker_wait_for_submit(int32_t submitted, int32_t total) {
    return rusty::detail::deref_if_pointer_like(submitted) < rusty::detail::deref_if_pointer_like(total);
}
constexpr bool raft_worker_queue_has_work(bool queue_empty) {
    return !queue_empty;
}
constexpr bool raft_worker_submit_loop_should_wake(bool queue_empty) {
    return !queue_empty;
}
constexpr bool raft_worker_submit_loop_should_stop(bool queue_empty) {
    return std::move(queue_empty);
}
constexpr bool raft_worker_submit_loop_should_take(int32_t batch_size, int32_t limit) {
    return rusty::detail::deref_if_pointer_like(batch_size) < rusty::detail::deref_if_pointer_like(limit);
}
constexpr bool raft_worker_partition_matches(uint32_t worker_partition, uint32_t requested_partition) {
    return rusty::detail::deref_if_pointer_like(worker_partition) == rusty::detail::deref_if_pointer_like(requested_partition);
}
constexpr bool raft_worker_scheduler_available(bool has_scheduler) {
    return std::move(has_scheduler);
}
constexpr int32_t raft_worker_leader_flag_from_bool(bool is_leader) {
    if (is_leader) {
        return static_cast<int32_t>(1);
    } else {
        return static_cast<int32_t>(0);
    }
}
constexpr bool raft_worker_should_notify_default_partition(bool callback_partitions_empty) {
    return std::move(callback_partitions_empty);
}
constexpr bool raft_worker_has_command_payload(bool has_value) {
    return std::move(has_value);
}
constexpr bool raft_worker_callback_available(bool has_callback) {
    return std::move(has_callback);
}
constexpr bool raft_worker_should_buffer_unreplayed(int32_t status, int32_t safety_fail_status, int32_t len) {
    return (rusty::detail::deref_if_pointer_like(status) == rusty::detail::deref_if_pointer_like(safety_fail_status)) && (rusty::detail::deref_if_pointer_like(len) > 0);
}
/*RUSTYCPP:GEN-END id=raft_worker.scalar_decisions*/

static_assert(raft_worker_should_start_submit_thread(false));
static_assert(!raft_worker_should_start_submit_thread(true));
static_assert(!raft_worker_should_stop_submit_thread(false));
static_assert(raft_worker_should_stop_submit_thread(true));
static_assert(!raft_worker_should_enqueue(false));
static_assert(raft_worker_should_enqueue(true));
static_assert(raft_worker_batch_limit(-1) == 1);
static_assert(raft_worker_batch_limit(0) == 1);
static_assert(raft_worker_batch_limit(1) == 1);
static_assert(raft_worker_batch_limit(8) == 8);
static_assert(raft_worker_wait_for_submit(2, 3));
static_assert(!raft_worker_wait_for_submit(3, 3));
static_assert(!raft_worker_queue_has_work(true));
static_assert(raft_worker_queue_has_work(false));
static_assert(!raft_worker_submit_loop_should_wake(true));
static_assert(raft_worker_submit_loop_should_wake(false));
static_assert(!raft_worker_submit_loop_should_stop(false));
static_assert(raft_worker_submit_loop_should_stop(true));
static_assert(raft_worker_submit_loop_should_take(1, 2));
static_assert(!raft_worker_submit_loop_should_take(2, 2));
static_assert(raft_worker_partition_matches(4, 4));
static_assert(!raft_worker_partition_matches(4, 5));
static_assert(!raft_worker_scheduler_available(false));
static_assert(raft_worker_scheduler_available(true));
static_assert(raft_worker_leader_flag_from_bool(false) == 0);
static_assert(raft_worker_leader_flag_from_bool(true) == 1);
static_assert(raft_worker_should_notify_default_partition(true));
static_assert(!raft_worker_should_notify_default_partition(false));
static_assert(!raft_worker_has_command_payload(false));
static_assert(raft_worker_has_command_payload(true));
static_assert(!raft_worker_callback_available(false));
static_assert(raft_worker_callback_available(true));
static_assert(!raft_worker_should_buffer_unreplayed(7, 7, 0));
static_assert(raft_worker_should_buffer_unreplayed(7, 7, 1));
static_assert(!raft_worker_should_buffer_unreplayed(6, 7, 1));

// @safe
RaftWorker::RaftWorker() = default;

// @unsafe - cleanup operations are bounded
RaftWorker::~RaftWorker() {
  StopSubmitThread();

  // Shutdown PollThreadWorkers if we own them
  if (svr_poll_thread_worker_.is_some()) {
    svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
  }
}

// @safe - external calls marked @external [safe], dynamic_cast is bounded
void RaftWorker::SetupBase() {
  auto config = Config::GetConfig();

  // Create Raft frame (uses "raft" protocol)
  // @unsafe
  { // config-> dereference, Frame::GetFrame returns raw pointer
    rep_frame_ = Frame::GetFrame(config->replica_proto_);
  }

  // @unsafe
  { // rep_frame_-> pointer dereference
    rep_frame_->site_info_ = site_info_;
  }

  // Create RaftServer instance
  // @unsafe
  { // rep_frame_-> pointer dereference, Frame::CreateScheduler
    rep_sched_ = rep_frame_->CreateScheduler();
  }

  // @unsafe
  { // rep_sched_-> and site_info_-> pointer dereferences
    rep_sched_->loc_id_ = site_info_->locale_id;
    rep_sched_->site_id_ = site_info_->id;  // CRITICAL: Set site_id!
    rep_sched_->partition_id_ = site_info_->partition_id_;
  }

  if (auto raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
    raft_server->RegisterLeaderChangeCallback([this](bool leader) {
      {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        is_leader = raft_worker_leader_flag_from_bool(leader);
      }
      // Notify all partitions that currently have callback registrations.
      std::set<uint32_t> par_ids;
      {
        std::lock_guard<std::mutex> callback_guard(
            callback_registry_mutex_);
        for (const auto& kv : leader_callbacks_by_partition_) {
          par_ids.insert(kv.first);
        }
        for (const auto& kv : follower_callbacks_by_partition_) {
          par_ids.insert(kv.first);
        }
      }
      if (raft_worker_should_notify_default_partition(par_ids.empty())) {
        uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
        NotifyRaftLeaderChange(par_id, leader);
      } else {
        for (uint32_t pid : par_ids) {
          NotifyRaftLeaderChange(pid, leader);
        }
      }
    });
  }

  // @unsafe
  { // config-> pointer dereference, Config::get_tot_req
    this->tot_num = config->get_tot_req();
  }
}

// @unsafe - uses new to allocate raw pointers (manual memory management)
void RaftWorker::SetupService() {
  // Create RPC server and register Raft service
  std::string bind_addr = site_info_->GetBindAddress();

  // Create poll thread worker
  svr_poll_thread_worker_ = rusty::Some(rrr::PollThread::create());

  // Use as_ref().unwrap() to borrow without consuming the Option
  auto& poll_worker = svr_poll_thread_worker_.as_ref().unwrap();

  // Create RPC server first (before registering services)
  rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(poll_worker.clone())));
  rpc_server_->set_admission_ready(false);

  // Create and register Raft services (ownership transferred to rpc_server_)
  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service_proxy(std::move(svc));
    }
  }

  // Start RPC server
  int ret = rpc_server_->start(reinterpret_cast<const int8_t*>(bind_addr.c_str()));
  if (ret != 0) {
    Log_fatal("Raft server launch failed at {}", bind_addr.c_str());
  }
}

// @safe - pointer dereferences are bounded, external calls marked @external
void RaftWorker::SetupCommo() {
  // Create Raft communicator
  verify(rep_frame_ != nullptr);
  verify(rep_sched_ != nullptr);

  // @unsafe
  { // rep_frame_-> pointer dereference, Option::clone
    // Use clone() to preserve svr_poll_thread_worker_ for later use by GetPollThreadWorker()
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_.clone());
  }

  // @unsafe
  { // rep_sched_-> pointer dereference
    rep_sched_->commo_ = rep_commo_;
  }
}

// @unsafe - synchronizes legacy std::function callbacks with Raft startup.
bool RaftWorker::PrepareForStartup(
    const std::vector<uint32_t>& partition_ids) {
  const char* replicated_db_flag = std::getenv("MAKO_REPLICATED_DB");
  if (replicated_db_flag != nullptr &&
      (strcmp(replicated_db_flag, "1") == 0 ||
       strcmp(replicated_db_flag, "true") == 0)) {
    // SetupInternal constructs ReplicatedDB and installs its atomic apply
    // callback before snapshot discovery or committed replay. Do not install
    // the standalone helper trampoline over that built-in state machine.
    return true;
  }

  auto learner = std::bind(&RaftWorker::Next, this,
                           std::placeholders::_1,
                           std::placeholders::_2);
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);

  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_error("[RAFT-STARTUP] Cannot bind application callback: scheduler is unavailable");
    return false;
  }

  std::set<uint32_t> unique_partitions(partition_ids.begin(),
                                       partition_ids.end());
  if (unique_partitions.empty()) {
    Log_error("[RAFT-STARTUP] Cannot bind application callback without a partition");
    return false;
  }

  const auto has_role_callback = [this](uint32_t par_id, bool leader) {
    const auto& watermark_callbacks =
        leader ? leader_callbacks_by_partition_
               : follower_callbacks_by_partition_;
    const auto& global_watermark_callback =
        leader ? leader_callback_par_id_return_
               : follower_callback_par_id_return_;
    const auto& simple_callbacks =
        leader ? leader_apply_callbacks_by_partition_
               : follower_apply_callbacks_by_partition_;
    const auto& partition_callbacks =
        leader ? leader_partition_apply_callbacks_by_partition_
               : follower_partition_apply_callbacks_by_partition_;

    const auto watermark_it = watermark_callbacks.find(par_id);
    const auto simple_it = simple_callbacks.find(par_id);
    const auto partition_it = partition_callbacks.find(par_id);
    return (watermark_it != watermark_callbacks.end() &&
            static_cast<bool>(watermark_it->second)) ||
           static_cast<bool>(global_watermark_callback) ||
           (simple_it != simple_callbacks.end() &&
            static_cast<bool>(simple_it->second)) ||
           (partition_it != partition_callbacks.end() &&
            static_cast<bool>(partition_it->second)) ||
           static_cast<bool>(callback_) ||
           static_cast<bool>(callback_par_id_);
  };

  for (uint32_t par_id : unique_partitions) {
    const bool has_leader = has_role_callback(par_id, true);
    const bool has_follower = has_role_callback(par_id, false);
    if (!has_leader || !has_follower) {
      Log_error("[RAFT-STARTUP] Partition {} is missing its {}{} application callback; refusing recovery",
                par_id,
                has_leader ? "" : "leader",
                !has_leader && !has_follower
                    ? "+follower"
                    : (has_follower ? "" : "follower"));
      return false;
    }
  }

  if (!learner_callback_bound_) {
    // This is the only helper-side write to TxLogServer::app_next_. It occurs
    // before EnsureSetup(), so recovery observes either the complete callback
    // registry or no learner at all.
    rep_sched_->RegLearnerAction(std::move(learner));
    learner_callback_bound_ = true;
  }
  return true;
}

// @safe - RaftServer owns the startup completion synchronization.
bool RaftWorker::WaitForStartup() {
  auto* raft_server = GetRaftServer();
  if (raft_server == nullptr) {
    return false;
  }
  if (!raft_server->WaitForStartup()) {
    Log_error("[RAFT-STARTUP] Site {} failed; keeping RPC admission closed",
              site_info_ ? site_info_->id : 0);
    return false;
  }
  return true;
}

// @safe - rrr::Server owns the shared atomic admission flag.
void RaftWorker::SetRpcAdmissionReady(bool ready) {
  if (rpc_server_ != nullptr) {
    rpc_server_->set_admission_ready(ready);
  }
}

// @safe - Compatibility helper for non-orchestrated callers.
bool RaftWorker::FinishStartup() {
  if (rpc_server_ == nullptr || !WaitForStartup()) {
    return false;
  }
  SetRpcAdmissionReady(true);
  return true;
}

// @unsafe - uses new to allocate raw pointers (manual memory management)
void RaftWorker::SetupHeartbeat() {
  auto config = Config::GetConfig();
  bool hb = config->do_heart_beat();

  if (!hb) {
    return;
  }

  // Setup heartbeat/control RPC server
  // ServerControlServiceImpl ctor 3rd
  // `Recorder*` parameter removed; updated call site to 2 args.
  svr_hb_poll_thread_worker_g = rusty::Some(rrr::PollThread::create());
  hb_rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(svr_hb_poll_thread_worker_g.as_ref().unwrap().clone())));

  // Create shared status and pass clone to service
  server_status_ = rusty::Some(rusty::Arc<ServerStatus>::make());
  hb_rpc_server_->reg_service_typed(rusty::make_box<ServerControlServiceImpl>(server_status_.as_ref().unwrap().clone(), 5));

  auto port = site_info_->port + CtrlPortDelta;
  std::string addr_port = site_info_->GetHostAddr(CtrlPortDelta);

  hb_rpc_server_->start(reinterpret_cast<const int8_t*>(addr_port.c_str()));
  if (auto* raft_server = GetRaftServer();
      raft_server != nullptr && raft_server->IsRpcReady() &&
      server_status_.is_some()) {
    server_status_.as_ref().unwrap()->set_ready();
  }
}

// @unsafe - uses delete, raw pointers, Option<Arc<PollThread>>
void RaftWorker::ShutDown() {
  Log_info("[RAFT-WORKER-SHUTDOWN] entering");

  // Stop the native producer and drain its bounded queue while Raft is still
  // live. StopSubmitThread's final drain calls Submit() synchronously, so doing
  // this after PrepareForShutdown would append into a quiesced server.
  StopSubmitThread();

  // Raft's heartbeat and election fibers are owned by the server PollThread.
  // First close RPC admission and drain every handler that borrowed the raw
  // server pointer. Then quiesce the runtime loops while their owner is still
  // able to run the gate's shutdown wake job; deleting the scheduler after
  // stopping the PollThread would leave those fibers suspended with raw
  // references to the server.
  if (auto* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
    RaftServiceImpl::UpdateServer(site_info_->id, nullptr);
    raft_server->PrepareForShutdown();
  }

  // rrr::Server::~Server schedules its listener-close job on the PollThread.
  // Keep both owner threads alive until their servers and the Raft scheduler
  // have released every connection and callback.
  if (rpc_server_) {
    Log_info("[RAFT-WORKER-SHUTDOWN] deleting rpc_server_");
    delete rpc_server_;
    rpc_server_ = nullptr;
  }

  if (hb_rpc_server_) {
    delete hb_rpc_server_;  // Server destructor cleans up owned services
    hb_rpc_server_ = nullptr;
    server_status_ = rusty::None;
  }

  // Services are now owned by rpc_server_ and deleted with it

  if (rep_sched_) {
    delete rep_sched_;
    rep_sched_ = nullptr;
  }

  // Shutdown poll threads only after every owner that can enqueue work onto
  // them has been destroyed.
  Log_info("[RAFT-WORKER-SHUTDOWN] shutting down poll threads");
  if (svr_poll_thread_worker_.is_some()) {
    auto& poll_thread = svr_poll_thread_worker_.as_ref().unwrap();
    Log_info("[RAFT-WORKER-SHUTDOWN] calling shutdown on svr_poll_thread_worker_={}", (void*)poll_thread.get());
    poll_thread->shutdown();
    Log_info("[RAFT-WORKER-SHUTDOWN] svr_poll_thread_worker_ shutdown returned");
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    auto& poll_thread = svr_hb_poll_thread_worker_g.as_ref().unwrap();
    Log_info("[RAFT-WORKER-SHUTDOWN] calling shutdown on svr_hb_poll_thread_worker_g={}", (void*)poll_thread.get());
    poll_thread->shutdown();
    Log_info("[RAFT-WORKER-SHUTDOWN] svr_hb_poll_thread_worker_g shutdown returned");
  }
  Log_info("[RAFT-WORKER-SHUTDOWN] poll threads shutdown complete");
}

// @unsafe
void RaftWorker::WaitForShutdown() {
  StopSubmitThread();

  if (hb_rpc_server_) {
    // @unsafe
    { // hb_rpc_server_-> raw pointer dereference
      hb_rpc_server_->do_shutdown();
      hb_rpc_server_->wait_for_shutdown();
    }
  }
}

// @unsafe - pointer dereferences are bounded, dynamic_cast marked @external [safe]
bool RaftWorker::IsLeader(uint32_t par_id) {
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);

  if (!handles_all_partitions_) {
    // @unsafe
    { // rep_frame_->site_info_-> pointer dereference chain
      if (!raft_worker_partition_matches(
              rep_frame_->site_info_->partition_id_, par_id)) {
        return false;
      }
    }
  }

  // @unsafe
  { // GetRaftServer uses dynamic_cast on raw pointer, raft_server-> pointer dereference
    auto raft_server = GetRaftServer();
    if (raft_server) {
      return raft_server->IsLeader();
    }
  }

  return false;
}

// @unsafe - uses raw pointers, dynamic_cast
siteid_t RaftWorker::GetLeaderHint() {
  // @unsafe
  { // GetRaftServer uses dynamic_cast on raw pointer
    auto raft_server = GetRaftServer();
    if (raft_server) {
      return raft_server->GetLeaderHint();
    }
  }
  return INVALID_SITEID;
}

// @unsafe - pointer dereferences are bounded
bool RaftWorker::IsPartition(uint32_t par_id) {
  if (handles_all_partitions_) {
    return true;
  }
  // Multi-group mode: each worker owns exactly one partition.
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);
  // @unsafe
  { // rep_frame_->site_info_-> pointer dereference chain
    return raft_worker_partition_matches(
        rep_frame_->site_info_->partition_id_, par_id);
  }
}

// @unsafe
void RaftWorker::StartSubmitThread() {
  if (!raft_worker_should_start_submit_thread(submit_thread_started_)) {
    return;
  }
  submit_thread_stop_ = false;
  submit_thread_started_ = true;
  // @unsafe
  { // 'this' pointer passed to std::thread constructor
    submit_thread_ = std::thread(&RaftWorker::SubmitLoop, this);
  }
}

// @unsafe
void RaftWorker::StopSubmitThread() {
  if (!raft_worker_should_stop_submit_thread(submit_thread_started_)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    submit_thread_stop_ = true;
  }
  submit_cv_.notify_all();
  if (submit_thread_.joinable()) {
    submit_thread_.join();
  }
  submit_thread_started_ = false;
  submit_thread_stop_ = false;

  std::deque<PendingLog> remaining;
  {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    remaining.swap(submit_queue_);
  }
  for (auto& entry : remaining) {
    Submit(entry.payload.data(), static_cast<int>(entry.payload.size()), entry.par_id);
  }
}

// @unsafe
void RaftWorker::EnqueueLog(const char* log, int len, uint32_t par_id, int batch_size) {
  if (!raft_worker_should_enqueue(submit_thread_started_)) {
    // @unsafe
    { // const char* propagation to Submit
      Submit(log, len, par_id);
    }
    return;
  }

  PendingLog entry;
  // @unsafe
  { // std::string::assign from raw const char* pointer
    entry.payload.assign(log, len);
  }
  entry.par_id = par_id;

  {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    batch_limit_ = raft_worker_batch_limit(batch_size);
    submit_queue_.push_back(std::move(entry));
  }
  submit_cv_.notify_one();
}

// @unsafe - external calls marked @external [safe]
rusty::Arc<TpcCommitCommand> RaftWorker::CreateRaftLogCommand(
    const char* log_entry,
    int length,
    txnid_t tx_id,
    uint32_t par_id) {

  LogEntry raw_log;
  verify(raft::EncodeApplicationLog(log_entry, length, par_id,
                                    &raw_log.log_entry));
  raw_log.length = static_cast<int>(raw_log.log_entry.size());

  auto tpc_cmd = rusty::Arc<TpcCommitCommand>::make();
  // @unsafe - unique-owner mutation window (factory-fresh Arc).
  {
    auto& mut_cmd = tpc_cmd.get_mut().unwrap();
    mut_cmd.tx_id_ = tx_id;
    mut_cmd.term = 0;
    mut_cmd.cmd_ = rusty::Arc<LogEntry>::make(std::move(raw_log));
  }

  Log_debug("[RAFT-LOG-CMD] Created TpcCommitCommand tx_id={} with {} application bytes",
            tx_id, length);

  return tpc_cmd;
}

// @unsafe - external calls marked @external [safe], pointer ops are bounded
void RaftWorker::Submit(const char* log_entry, int length, uint32_t par_id) {
  // Log_debug("[RAFT-SUBMIT] Enter Submit: par_id={} length={}", par_id, length);

  // Do not pre-check leadership here. RaftServer::Start() performs the
  // authoritative leadership check under server lock; pre-checking can race.

  // @unsafe
  {
  RaftServer* raft_server = GetRaftServer();
  if (!raft_server) {
    Log_error("[RAFT-SUBMIT] RaftServer is null in Submit()");
    return;
  }

  // Use a simple incrementing tx_id (in production this would be a global txn ID)
  static std::atomic<txnid_t> next_tx_id{1};
  txnid_t tx_id = next_tx_id.fetch_add(1);

  // Use the production helper to create TpcCommitCommand{cmd_=LogEntry}.
  auto tpc_cmd = CreateRaftLogCommand(log_entry, length, tx_id, par_id);

  uint64_t index = 0;
  uint64_t term = 0;
  const RaftStartResult start_result =
      raft_server->Start(std::move(tpc_cmd), &index, &term);
  if (raft_server_start_was_rejected(start_result)) {
    return;
  }
  if (raft_server_start_is_indeterminate(start_result)) {
    // This fire-and-forget interface has no channel for commit-outcome-unknown.
    // Continuing would let upstream retry a command that may already be
    // durable, so terminate the replica process at the ambiguity boundary.
    Log_fatal("[RAFT-SUBMIT] Local append outcome is indeterminate for "
              "slot {} term {}; refusing to report rejection",
              index, term);
  }
  }

  n_tot++;
}

// @safe
void RaftWorker::IncSubmit() {
  // @unsafe
  { n_submit++; }
}

// @unsafe
void RaftWorker::WaitForSubmit() {
  std::unique_lock<std::mutex> lock(condition_mutex_);
  // Wait logic - can be enhanced with condition variable if needed
  // For now, simple busy wait
  while (raft_worker_wait_for_submit(n_submit, tot_num)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  lock.unlock();

  while (true) {
    {
      std::lock_guard<std::mutex> qlock(submit_mutex_);
      if (!raft_worker_queue_has_work(submit_queue_.empty())) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// @unsafe
void RaftWorker::register_apply_callback(std::function<void(const char*, int)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  callback_ = std::move(cb);

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip apply callback registration");
    return;
  }

}

// @unsafe
void RaftWorker::register_apply_callback_par_id(
    std::function<void(const char*&, int, int)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  callback_par_id_ = std::move(cb);

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip apply callback registration");
    return;
  }

}

// @unsafe - stores a role-aware legacy callback behind the registry mutex.
void RaftWorker::register_leader_apply_callback_for_partition(
    uint32_t par_id, std::function<void(const char*, int)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  leader_apply_callbacks_by_partition_[par_id] = std::move(cb);
  Log_info("[RAFT-CALLBACK] Registered simple leader callback for partition {}",
           par_id);
}

// @unsafe - stores a role-aware legacy callback behind the registry mutex.
void RaftWorker::register_follower_apply_callback_for_partition(
    uint32_t par_id, std::function<void(const char*, int)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  follower_apply_callbacks_by_partition_[par_id] = std::move(cb);
  Log_info("[RAFT-CALLBACK] Registered simple follower callback for partition {}",
           par_id);
}

// @unsafe - stores a role-aware legacy callback behind the registry mutex.
void RaftWorker::register_leader_partition_apply_callback_for_partition(
    uint32_t par_id,
    std::function<void(const char*&, int, int)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  leader_partition_apply_callbacks_by_partition_[par_id] = std::move(cb);
  Log_info("[RAFT-CALLBACK] Registered partition-aware leader callback for partition {}",
           par_id);
}

// @unsafe - stores a role-aware legacy callback behind the registry mutex.
void RaftWorker::register_follower_partition_apply_callback_for_partition(
    uint32_t par_id,
    std::function<void(const char*&, int, int)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  follower_partition_apply_callbacks_by_partition_[par_id] = std::move(cb);
  Log_info("[RAFT-CALLBACK] Registered partition-aware follower callback for partition {}",
           par_id);
}

// @unsafe
void RaftWorker::register_leader_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  leader_callback_par_id_return_ = std::move(cb);

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition {}",
             site_info_ ? site_info_->partition_id_ : -1);
    return;
  }

  Log_info("[RAFT-CALLBACK] Registered leader callback for partition {}",
           site_info_ ? site_info_->partition_id_ : -1);
}

// @unsafe
void RaftWorker::register_follower_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  follower_callback_par_id_return_ = std::move(cb);

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition {}",
             site_info_ ? site_info_->partition_id_ : -1);
    return;
  }

  Log_info("[RAFT-CALLBACK] Registered follower callback for partition {}",
           site_info_ ? site_info_->partition_id_ : -1);
}

// @unsafe - delegates to register_follower_callback_par_id_return
void RaftWorker::register_apply_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  Log_warn("[RAFT-CALLBACK] Using deprecated register_apply_callback_par_id_return - use register_leader/follower_callback_par_id_return instead");
  register_follower_callback_par_id_return(cb);
}

// @unsafe - external calls marked @external [safe], malloc/memcpy in @unsafe blocks
int RaftWorker::Next(slotid_t slot_id, janus::Command md) {
  int status = -1;

  // The legacy Mako watermark callback still accepts a signed int. Refuse a
  // lossy conversion instead of corrupting its replay queue after INT_MAX.
  if (slot_id > static_cast<slotid_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error("Raft watermark callback index exceeds int");
  }
  const int callback_slot_id = static_cast<int>(slot_id);

  // @unsafe
  { // null check on Command envelope
    if (!raft_worker_has_command_payload(md.has_value())) {
      Log_error("Received null command in Next()");
      throw std::runtime_error("Raft application received null command");
    }
  }

  // @unsafe
  {
  // Extract the application payload from TpcCommitCommand. New entries carry
  // a replication-native LogEntry; the MDB-free LegacyVecPieceData reader
  // remains available solely for logs persisted by older Mako builds.
  const char* log = nullptr;
  int len = 0;
  uint32_t par_id = 0;
  // Owns legacy bytes until both the callback and any safety-failure copy
  // below have finished consuming `log`.
  std::string legacy_payload;

  // Try TpcCommitCommand (production path with RAFT_BATCH_OPTIMIZATION)
  const auto tpc_cmd = marshallable_cast<TpcCommitCommand>(md);
  // tpc_cmd is Option<Arc<TpcCommitCommand>>; the payload's cmd_ is
  // Command; has_value() for null
  // check; marshallable_cast<T>(Command&) overload handles the cast.
  if (!tpc_cmd.is_some() ||
      !raft_worker_has_command_payload(
          tpc_cmd.unwrap()->cmd_.has_value())) {
    Log_error("[RAFT-CALLBACK] Command is not TpcCommitCommand for partition {}, slot {}",
              site_info_ ? site_info_->partition_id_ : -1, slot_id);
    throw std::runtime_error("Unexpected Raft application command kind");
  }

  const auto& inner = tpc_cmd.unwrap()->cmd_;
  if (inner.kind_ == LogEntry::static_kind()) {
    const auto raw_log = marshallable_cast<LogEntry>(inner);
    if (!raw_log.is_some() || raw_log.unwrap()->length < 0 ||
        static_cast<size_t>(raw_log.unwrap()->length) !=
            raw_log.unwrap()->log_entry.size() ||
        !raft::DecodeApplicationLog(raw_log.unwrap()->log_entry, &log, &len,
                                    &par_id)) {
      Log_error("[RAFT-CALLBACK] Invalid application LogEntry for slot {}",
                slot_id);
      return status;
    }
    Log_debug("[RAFT-CALLBACK] Extracted application LogEntry (tx_id={}): len={}",
              tpc_cmd.unwrap()->tx_id_, len);
  } else if (inner.kind_ == LegacyVecPieceData::static_kind()) {
    const auto legacy = marshallable_cast<LegacyVecPieceData>(inner);
    if (!legacy.is_some() ||
        !legacy.unwrap()->TryGetApplicationLog(&legacy_payload, &par_id)) {
      Log_error("[RAFT-CALLBACK] Invalid legacy VecPieceData for slot {}",
                slot_id);
      return status;
    }
    if (legacy_payload.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
      Log_error("[RAFT-CALLBACK] Legacy VecPieceData payload is too large for slot {}",
                slot_id);
      return status;
    }
    log = legacy_payload.data();
    len = static_cast<int>(legacy_payload.size());
    Log_info("[RAFT-CALLBACK] Decoded legacy VecPieceData at slot {}", slot_id);
  } else {
    Log_error("[RAFT-CALLBACK] Unsupported inner command kind {} for slot {}",
              inner.kind_, slot_id);
    return status;
  }

  bool am_leader = IsLeader(par_id);

  // Route to the richest per-partition callback first, then preserve the two
  // legacy void-callback APIs as role-aware fallbacks. Copy under the mutex and
  // invoke outside it so callbacks may safely register replacements.
  watermark_callback_t active_callback;
  std::function<void(const char*&, int, int)> active_partition_callback;
  std::function<void(const char*, int)> active_simple_callback;
  {
    std::lock_guard<std::mutex> guard(callback_registry_mutex_);
    auto& cb_map = am_leader ? leader_callbacks_by_partition_
                             : follower_callbacks_by_partition_;
    auto cb_it = cb_map.find(par_id);
    if (cb_it != cb_map.end() &&
        raft_worker_callback_available(static_cast<bool>(cb_it->second))) {
      active_callback = cb_it->second;
    } else {
      auto& global_cb = am_leader ? leader_callback_par_id_return_
                                  : follower_callback_par_id_return_;
      if (raft_worker_callback_available(static_cast<bool>(global_cb))) {
        active_callback = global_cb;
      }
    }

    if (!active_callback) {
      auto& partition_cb_map =
          am_leader ? leader_partition_apply_callbacks_by_partition_
                    : follower_partition_apply_callbacks_by_partition_;
      auto partition_cb_it = partition_cb_map.find(par_id);
      if (partition_cb_it != partition_cb_map.end() &&
          static_cast<bool>(partition_cb_it->second)) {
        active_partition_callback = partition_cb_it->second;
      } else if (callback_par_id_) {
        active_partition_callback = callback_par_id_;
      }
    }

    if (!active_callback && !active_partition_callback) {
      auto& simple_cb_map =
          am_leader ? leader_apply_callbacks_by_partition_
                    : follower_apply_callbacks_by_partition_;
      auto simple_cb_it = simple_cb_map.find(par_id);
      if (simple_cb_it != simple_cb_map.end() &&
          static_cast<bool>(simple_cb_it->second)) {
        active_simple_callback = simple_cb_it->second;
      } else if (callback_) {
        active_simple_callback = callback_;
      }
    }
  }

  if (!active_callback && !active_partition_callback &&
      !active_simple_callback) {
    Log_error("[RAFT-CALLBACK] No {} callback registered for partition {}",
              am_leader ? "leader" : "follower", par_id);
    throw std::runtime_error("Raft application callback is not registered");
  }

  Log_debug("[RAFT-CALLBACK] Applying log at slot {} par_id {} using {} callback",
            slot_id, par_id, am_leader ? "LEADER" : "FOLLOWER");

  uint32_t timestamp = 0;
  if (active_callback) {
    auto& un_replay_queue = un_replay_logs_by_partition_[par_id];
    const int encoded_value = active_callback(
        log, len, par_id, callback_slot_id, un_replay_queue);
    status = encoded_value % 10;
    timestamp = encoded_value / 10;

    if (raft_worker_should_buffer_unreplayed(
            status, janus::PaxosStatus::STATUS_SAFETY_FAIL, len)) {
      char* dest = static_cast<char*>(malloc(len));
      verify(dest != nullptr);
      memcpy(dest, log, len);
      un_replay_queue.push(std::make_tuple(timestamp, callback_slot_id, status, len,
                                           static_cast<const char*>(dest)));
    }
  } else if (active_partition_callback) {
    active_partition_callback(log, len, static_cast<int>(par_id));
    status = 0;
  } else {
    active_simple_callback(log, len);
    status = 0;
  }

  Log_debug("Raft applied log at slot {}: status={}, timestamp={}, role={}, par_id={}",
            slot_id, status, timestamp, am_leader ? "leader" : "follower", par_id);

  return status;
  } // end @unsafe block for const char* log
}

// SINGLE-RAFT: Per-partition callback registration methods
// @unsafe
void RaftWorker::register_leader_callback_for_partition(uint32_t par_id, watermark_callback_t cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  leader_callbacks_by_partition_[par_id] = std::move(cb);

  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition {}", par_id);
    return;
  }

  Log_info("[SINGLE-RAFT] Registered leader callback for partition {}", par_id);
}

// @unsafe
void RaftWorker::register_follower_callback_for_partition(uint32_t par_id, watermark_callback_t cb) {
  std::lock_guard<std::mutex> guard(callback_registry_mutex_);
  follower_callbacks_by_partition_[par_id] = std::move(cb);

  if (!raft_worker_scheduler_available(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition {}", par_id);
    return;
  }

  Log_info("[SINGLE-RAFT] Registered follower callback for partition {}", par_id);
}

// @unsafe
void RaftWorker::SubmitLoop() {
  std::unique_lock<std::mutex> lock(submit_mutex_);
  while (true) {
    submit_cv_.wait(lock, [&] {
      // @unsafe
      { // operator bool on std::atomic<bool>
        return submit_thread_stop_ ||
               raft_worker_submit_loop_should_wake(submit_queue_.empty());
      }
    });
    bool should_stop = false;
    // @unsafe
    { // operator bool on std::atomic<bool>
      should_stop = submit_thread_stop_ &&
                    raft_worker_submit_loop_should_stop(
                        submit_queue_.empty());
    }
    if (should_stop) {
      break;
    }

    int limit = raft_worker_batch_limit(batch_limit_);
    std::vector<PendingLog> batch;
    batch.reserve(limit);
    while (raft_worker_submit_loop_should_wake(submit_queue_.empty()) &&
           raft_worker_submit_loop_should_take(
               static_cast<int>(batch.size()), limit)) {
      batch.push_back(std::move(submit_queue_.front()));
      submit_queue_.pop_front();
    }
    lock.unlock();

    for (auto& entry : batch) {
      Submit(entry.payload.data(), static_cast<int>(entry.payload.size()), entry.par_id);
    }

    lock.lock();
  }
}

} // namespace janus
