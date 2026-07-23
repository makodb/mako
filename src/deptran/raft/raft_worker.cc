#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "raft_worker.h"
#include "server.h"
#include "commo.h"
#include "service.h"
#include "../config.h"
#include "../paxos_worker.h"  // Reuse LogEntry marshalling for raw log payloads
#include "../paxos/commo.h"   // PaxosStatus enum reused by Mako watermark callbacks
#include "../classic/tpc_command.h"  // TpcCommitCommand for batch optimization
#include "../procedure.h"            // VecPieceData and SimpleCommand
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>

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

// @safe - scalar queue/partition/callback decisions only. Thread creation,
// condition variables, callback invocation, malloc/memcpy, and Raft submission
// stay in the hand-written C++ worker body.
#if RUSTYCPP_RUST
pub fn raft_worker_should_start_submit_thread(started: bool) -> bool {
    !started
}

pub fn raft_worker_should_stop_submit_thread(started: bool) -> bool {
    started
}

pub fn raft_worker_should_enqueue(started: bool,
                                  stop_requested: bool) -> bool {
    started && !stop_requested
}

pub fn raft_worker_batch_limit(batch_size: i32) -> i32 {
    if batch_size < 1 {
        1
    } else {
        batch_size
    }
}

pub fn raft_worker_wait_for_submit(submitted: i32, total: i32) -> bool {
    submitted < total
}

pub fn raft_worker_queue_has_work(queue_empty: bool) -> bool {
    !queue_empty
}

pub fn raft_worker_submit_loop_should_wake(stop_requested: bool,
                                           queue_empty: bool) -> bool {
    stop_requested || !queue_empty
}

pub fn raft_worker_submit_loop_should_stop(stop_requested: bool,
                                           queue_empty: bool) -> bool {
    stop_requested && queue_empty
}

pub fn raft_worker_submit_loop_should_take(queue_empty: bool,
                                           batch_size: i32,
                                           limit: i32) -> bool {
    !queue_empty && batch_size < limit
}

pub fn raft_worker_partition_matches(handles_all_partitions: bool,
                                     worker_partition: u32,
                                     requested_partition: u32) -> bool {
    handles_all_partitions || worker_partition == requested_partition
}

pub fn raft_worker_can_register_callback(has_scheduler: bool) -> bool {
    has_scheduler
}

pub fn raft_worker_leader_flag_from_bool(is_leader: bool) -> i32 {
    if is_leader {
        1
    } else {
        0
    }
}

pub fn raft_worker_is_leader_flag_set(is_leader: i32) -> bool {
    is_leader != 0
}

pub fn raft_worker_should_notify_default_partition(has_registered_partitions: bool) -> bool {
    !has_registered_partitions
}

pub fn raft_worker_partition_callback_available(found: bool,
                                                has_callback: bool) -> bool {
    found && has_callback
}

pub fn raft_worker_global_callback_available(has_callback: bool) -> bool {
    has_callback
}

pub fn raft_worker_has_command_payload(has_value: bool) -> bool {
    has_value
}

pub fn raft_worker_should_buffer_unreplayed(status: i32,
                                            safety_fail_status: i32,
                                            len: i32) -> bool {
    status == safety_fail_status && len > 0
}

pub fn raft_worker_pending_log_is_current(request_epoch: i32,
                                          current_epoch: i32) -> bool {
    request_epoch == current_epoch
}

pub fn raft_worker_leadership_changed(previous_is_leader: bool,
                                      current_is_leader: bool) -> bool {
    previous_is_leader != current_is_leader
}

pub fn raft_worker_should_count_submission(server_available: bool,
                                           appended: bool) -> bool {
    server_available && appended
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_worker.small_helpers version=1 rust_sha256=d5ef3893ed056f764577b488ab5bfeec982e49d24f96254ba805626c97d8b994*/
bool raft_worker_should_start_submit_thread(bool started);
bool raft_worker_should_stop_submit_thread(bool started);
bool raft_worker_should_enqueue(bool started, bool stop_requested);
int32_t raft_worker_batch_limit(int32_t batch_size);
bool raft_worker_wait_for_submit(int32_t submitted, int32_t total);
bool raft_worker_queue_has_work(bool queue_empty);
bool raft_worker_submit_loop_should_wake(bool stop_requested, bool queue_empty);
bool raft_worker_submit_loop_should_stop(bool stop_requested, bool queue_empty);
bool raft_worker_submit_loop_should_take(bool queue_empty, int32_t batch_size, int32_t limit);
bool raft_worker_partition_matches(bool handles_all_partitions, uint32_t worker_partition, uint32_t requested_partition);
bool raft_worker_can_register_callback(bool has_scheduler);
int32_t raft_worker_leader_flag_from_bool(bool is_leader);
bool raft_worker_is_leader_flag_set(int32_t is_leader);
bool raft_worker_should_notify_default_partition(bool has_registered_partitions);
bool raft_worker_partition_callback_available(bool found, bool has_callback);
bool raft_worker_global_callback_available(bool has_callback);
bool raft_worker_has_command_payload(bool has_value);
bool raft_worker_should_buffer_unreplayed(int32_t status, int32_t safety_fail_status, int32_t len);
bool raft_worker_pending_log_is_current(int32_t request_epoch, int32_t current_epoch);
bool raft_worker_leadership_changed(bool previous_is_leader, bool current_is_leader);
bool raft_worker_should_count_submission(bool server_available, bool appended);

bool raft_worker_should_start_submit_thread(bool started) {
    return !started;
}

bool raft_worker_should_stop_submit_thread(bool started) {
    return std::move(started);
}

bool raft_worker_should_enqueue(bool started, bool stop_requested) {
    return rusty::detail::deref_if_pointer_like(started) && !stop_requested;
}

int32_t raft_worker_batch_limit(int32_t batch_size) {
    if (rusty::detail::deref_if_pointer_like(batch_size) < 1) {
        return static_cast<int32_t>(1);
    } else {
        return std::move(batch_size);
    }
}

bool raft_worker_wait_for_submit(int32_t submitted, int32_t total) {
    return rusty::detail::deref_if_pointer_like(submitted) < rusty::detail::deref_if_pointer_like(total);
}

bool raft_worker_queue_has_work(bool queue_empty) {
    return !queue_empty;
}

bool raft_worker_submit_loop_should_wake(bool stop_requested, bool queue_empty) {
    return rusty::detail::deref_if_pointer_like(stop_requested) || !queue_empty;
}

bool raft_worker_submit_loop_should_stop(bool stop_requested, bool queue_empty) {
    return rusty::detail::deref_if_pointer_like(stop_requested) && rusty::detail::deref_if_pointer_like(queue_empty);
}

bool raft_worker_submit_loop_should_take(bool queue_empty, int32_t batch_size, int32_t limit) {
    return !queue_empty && (rusty::detail::deref_if_pointer_like(batch_size) < rusty::detail::deref_if_pointer_like(limit));
}

bool raft_worker_partition_matches(bool handles_all_partitions, uint32_t worker_partition, uint32_t requested_partition) {
    return rusty::detail::deref_if_pointer_like(handles_all_partitions) || (rusty::detail::deref_if_pointer_like(worker_partition) == rusty::detail::deref_if_pointer_like(requested_partition));
}

bool raft_worker_can_register_callback(bool has_scheduler) {
    return std::move(has_scheduler);
}

int32_t raft_worker_leader_flag_from_bool(bool is_leader) {
    if (is_leader) {
        return static_cast<int32_t>(1);
    } else {
        return static_cast<int32_t>(0);
    }
}

bool raft_worker_is_leader_flag_set(int32_t is_leader) {
    return rusty::detail::deref_if_pointer_like(is_leader) != static_cast<int32_t>(0);
}

bool raft_worker_should_notify_default_partition(bool has_registered_partitions) {
    return !has_registered_partitions;
}

bool raft_worker_partition_callback_available(bool found, bool has_callback) {
    return rusty::detail::deref_if_pointer_like(found) && rusty::detail::deref_if_pointer_like(has_callback);
}

bool raft_worker_global_callback_available(bool has_callback) {
    return std::move(has_callback);
}

bool raft_worker_has_command_payload(bool has_value) {
    return std::move(has_value);
}

bool raft_worker_should_buffer_unreplayed(int32_t status, int32_t safety_fail_status, int32_t len) {
    return (rusty::detail::deref_if_pointer_like(status) == rusty::detail::deref_if_pointer_like(safety_fail_status)) && (rusty::detail::deref_if_pointer_like(len) > 0);
}

bool raft_worker_pending_log_is_current(int32_t request_epoch, int32_t current_epoch) {
    return rusty::detail::deref_if_pointer_like(request_epoch) == rusty::detail::deref_if_pointer_like(current_epoch);
}

bool raft_worker_leadership_changed(bool previous_is_leader, bool current_is_leader) {
    return rusty::detail::deref_if_pointer_like(previous_is_leader) != rusty::detail::deref_if_pointer_like(current_is_leader);
}

bool raft_worker_should_count_submission(bool server_available, bool appended) {
    return rusty::detail::deref_if_pointer_like(server_available) && rusty::detail::deref_if_pointer_like(appended);
}
/*RUSTYCPP:GEN-END id=raft_worker.small_helpers*/

// @safe
RaftWorker::RaftWorker()
    : state_core_(RaftWorkerStateCore::new_()) {}

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
      bool leadership_changed = false;
      {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        bool previous_leader = raft_worker_is_leader_flag_set(state_core_.is_leader());
        leadership_changed = raft_worker_leadership_changed(previous_leader, leader);
        state_core_.set_is_leader(raft_worker_leader_flag_from_bool(leader));
      }
      if (!leadership_changed) {
        return;
      }
      // Notify all partitions that currently have callback registrations.
      std::set<uint32_t> par_ids;
      for (const auto& kv : leader_callbacks_by_partition_) {
        par_ids.insert(kv.first);
      }
      for (const auto& kv : follower_callbacks_by_partition_) {
        par_ids.insert(kv.first);
      }
      if (raft_worker_should_notify_default_partition(!par_ids.empty())) {
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

// @unsafe - creates owned RPC server and registers legacy service proxies
void RaftWorker::SetupService() {
  // Create RPC server and register Raft service
  std::string bind_addr = site_info_->GetBindAddress();

  // Create poll thread worker
  svr_poll_thread_worker_ = rusty::Some(rrr::PollThread::create());

  // Use as_ref().unwrap() to borrow without consuming the Option
  auto& poll_worker = svr_poll_thread_worker_.as_ref().unwrap();

  // Create RPC server first (before registering services)
  rpc_server_ = std::make_unique<rrr::Server>(rusty::Some(poll_worker.clone()));

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
  int ret = rpc_server_->start(bind_addr.c_str());
  if (ret != 0) {
    Log_fatal("Raft server launch failed at %s", bind_addr.c_str());
  }
}

// @unsafe - borrows/creates RaftFrame-owned communicator and wires it into the
// borrowed RaftServer.
void RaftWorker::SetupCommo() {
  // Borrow/create the RaftFrame-owned communicator.
  verify(rep_frame_ != nullptr);
  verify(rep_sched_ != nullptr);

  // @unsafe
  { // rep_frame_-> pointer dereference, Option::clone
    // CreateCommo returns a borrowed pointer to RaftFrame::commo_; RaftWorker
    // must not delete it. Use clone() to preserve svr_poll_thread_worker_ for
    // later use by GetPollThreadWorker().
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_.clone());
  }

  // @unsafe
  { // rep_sched_-> pointer dereference
    rep_sched_->commo_ = rep_commo_;
  }
}

// @unsafe - creates owned heartbeat/control RPC server
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
  hb_rpc_server_ = std::make_unique<rrr::Server>(
      rusty::Some(svr_hb_poll_thread_worker_g.as_ref().unwrap().clone()));

  // Create shared status and pass clone to service
  server_status_ = rusty::Some(rusty::Arc<ServerStatus>::make());
  hb_rpc_server_->reg_service(rusty::make_box<ServerControlServiceImpl>(server_status_.as_ref().unwrap().clone(), 5));

  auto port = site_info_->port + CtrlPortDelta;
  std::string addr_port = site_info_->GetHostAddr(CtrlPortDelta);

  hb_rpc_server_->start(addr_port.c_str());
}

// @unsafe - resets owned RPC servers and clears borrowed protocol pointers
void RaftWorker::ShutDown() {
  Log_info("[RAFT-WORKER-SHUTDOWN] entering");

  // Signal poll threads to stop BEFORE deleting servers.
  // This allows Reactor::Loop() to exit, which unblocks server connection cleanup.
  Log_info("[RAFT-WORKER-SHUTDOWN] signaling poll threads to stop");
  if (svr_poll_thread_worker_.is_some()) {
    svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
  }

  if (rpc_server_) {
    Log_info("[RAFT-WORKER-SHUTDOWN] resetting rpc_server_");
    rpc_server_.reset();
  }

  if (hb_rpc_server_) {
    hb_rpc_server_.reset();  // Server destructor cleans up owned services
    server_status_ = rusty::None;
  }

  // Services are owned by rpc_server_ and destroyed with it.

  StopSubmitThread();

  // rep_sched_ is borrowed from RaftFrame::svr_; RaftWorker must not delete it.
  rep_sched_ = nullptr;
  // rep_commo_ is borrowed from RaftFrame::commo_; RaftWorker must not delete it.
  rep_commo_ = nullptr;

  // IMPORTANT: Shutdown poll threads AFTER servers are destroyed.
  // Server::~Server() enqueues remove commands to the poll thread; keeping the
  // poll thread alive allows it to drain those commands and drop the final
  // references so sconns_ctr_ reaches zero.
  Log_info("[RAFT-WORKER-SHUTDOWN] shutting down poll threads");
  if (svr_poll_thread_worker_.is_some()) {
    auto& poll_thread = svr_poll_thread_worker_.as_ref().unwrap();
    Log_info("[RAFT-WORKER-SHUTDOWN] calling shutdown on svr_poll_thread_worker_=%p", poll_thread.get());
    poll_thread->shutdown();
    Log_info("[RAFT-WORKER-SHUTDOWN] svr_poll_thread_worker_ shutdown returned");
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    auto& poll_thread = svr_hb_poll_thread_worker_g.as_ref().unwrap();
    Log_info("[RAFT-WORKER-SHUTDOWN] calling shutdown on svr_hb_poll_thread_worker_g=%p", poll_thread.get());
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

  if (!raft_worker_partition_matches(handles_all_partitions_,
                                     rep_frame_->site_info_->partition_id_,
                                     par_id)) {
    // @unsafe
    { // rep_frame_->site_info_-> pointer dereference chain
      return false;
    }
  }

  // @unsafe
  { // GetRaftServer uses dynamic_cast on raw pointer, raft_server-> pointer dereference
    auto raft_server = GetRaftServer();
    if (raft_server) {
      return raft_server->IsLeader();
    }
  }

  return raft_worker_is_leader_flag_set(state_core_.is_leader());
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
  // Multi-group mode: each worker owns exactly one partition.
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);
  // @unsafe
  { // rep_frame_->site_info_-> pointer dereference chain
    return raft_worker_partition_matches(handles_all_partitions_,
                                         rep_frame_->site_info_->partition_id_,
                                         par_id);
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

  std::deque<RaftWorkerPendingLog> remaining;
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
  if (!raft_worker_should_enqueue(
          submit_thread_started_, submit_thread_stop_.load())) {
    // @unsafe
    { // const char* propagation to Submit
      Submit(log, len, par_id);
    }
    return;
  }

  RaftWorkerPendingLog entry;
  // @unsafe
  { // std::string::assign from raw const char* pointer
    entry.payload.assign(log, len);
  }
  entry.par_id = par_id;
  {
    std::lock_guard<std::recursive_mutex> state_lock(election_state_lock);
    entry.epoch = CurrentEpoch();
  }

  {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    state_core_.set_batch_limit(raft_worker_batch_limit(batch_size));
    submit_queue_.push_back(std::move(entry));
  }
  submit_cv_.notify_one();
}

// @unsafe - external calls marked @external [safe]
std::shared_ptr<TpcCommitCommand> RaftWorker::CreateRaftLogCommand(
    const char* log_entry,
    int length,
    txnid_t tx_id,
    uint32_t par_id) {

  auto tpc_cmd = std::make_shared<TpcCommitCommand>();
  tpc_cmd->tx_id_ = tx_id;

  auto vpd = std::make_shared<VecPieceData>();
  vpd->sp_vec_piece_data_ = std::make_shared<vector<shared_ptr<SimpleCommand>>>();

  auto simple_cmd = std::make_shared<SimpleCommand>();

  simple_cmd->input.values_ = std::make_shared<map<int32_t, Value>>();
  (*simple_cmd->input.values_)[0] = Value(std::string(log_entry, length));
  simple_cmd->input.keys_.insert(0);
  // Store the partition id so callback routing is always explicit.
  simple_cmd->partition_id_ = par_id;

  vpd->sp_vec_piece_data_->push_back(simple_cmd);
  tpc_cmd->cmd_ = vpd;

  Log_debug("[RAFT-LOG-CMD] Created TpcCommitCommand tx_id=%lu with %d bytes (Mako/test payload)",
            tx_id, length);

  return tpc_cmd;
}

// @unsafe - external calls marked @external [safe], pointer ops are bounded
void RaftWorker::Submit(const char* log_entry, int length, uint32_t par_id) {
  // Log_debug("[RAFT-SUBMIT] Enter Submit: par_id=%d length=%d", par_id, length);

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

  // Use the production helper to create proper TpcCommitCommand{cmd_=VecPieceData}
  auto tpc_cmd = CreateRaftLogCommand(log_entry, length, tx_id, par_id);

  uint64_t index = 0;
  uint64_t term = 0;
  bool appended = raft_server->Start(tpc_cmd, &index, &term);
  if (!appended) {
    return;
  }

  if (raft_worker_should_count_submission(true, appended)) {
    n_tot++;
  }
  }
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
  while (raft_worker_wait_for_submit(n_submit.load(), tot_num)) {
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
void RaftWorker::register_apply_callback(rusty::Function<void(const char*, int)> cb) {
  // @unsafe - stores move-only legacy callback for later invocation.
  {
    this->callback_ = std::move(cb);
  }

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_can_register_callback(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip apply callback registration");
    return;
  }

  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }
}

// @unsafe
void RaftWorker::register_apply_callback_par_id(
    rusty::Function<void(const char*&, int, int)> cb) {
  // @unsafe - stores move-only legacy callback for later invocation.
  {
    this->callback_par_id_ = std::move(cb);
  }

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_can_register_callback(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip apply callback registration");
    return;
  }

  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }
}

// @unsafe
void RaftWorker::register_leader_callback_par_id_return(watermark_callback_t cb) {
  // @unsafe
  {
  this->leader_callback_par_id_return_ = std::move(cb);
  }

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_can_register_callback(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition %d",
             site_info_ ? site_info_->partition_id_ : -1);
    return;
  }

  // Register Next() with RaftServer if not already registered
  // Next() will dynamically choose leader vs follower callback
  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }

  Log_info("[RAFT-CALLBACK] Registered leader callback for partition %d",
           site_info_ ? site_info_->partition_id_ : -1);
}

// @unsafe
void RaftWorker::register_follower_callback_par_id_return(watermark_callback_t cb) {
  // @unsafe
  {
    this->follower_callback_par_id_return_ = std::move(cb);
  }

  // Guard against accessing scheduler during shutdown
  if (!raft_worker_can_register_callback(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition %d",
             site_info_ ? site_info_->partition_id_ : -1);
    return;
  }

  // Register Next() with RaftServer if not already registered
  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }

  Log_info("[RAFT-CALLBACK] Registered follower callback for partition %d",
           site_info_ ? site_info_->partition_id_ : -1);
}

// @unsafe - delegates to register_follower_callback_par_id_return
void RaftWorker::register_apply_callback_par_id_return(watermark_callback_t cb) {
  Log_warn("[RAFT-CALLBACK] Using deprecated register_apply_callback_par_id_return - use register_leader/follower_callback_par_id_return instead");
  register_follower_callback_par_id_return(std::move(cb));
}

// @unsafe - external calls marked @external [safe], malloc/memcpy in @unsafe blocks
int RaftWorker::Next(int slot_id, janus::Command md) {
  int status = -1;

  // @unsafe
  { // null check on Command envelope
    if (!raft_worker_has_command_payload(md.has_value())) {
      Log_error("Received null command in Next()");
      return status;
    }
  }

  // @unsafe
  {
  // Extract log payload from TpcCommitCommand{cmd_=VecPieceData}
  // This matches the structure created by CreateRaftLogCommand() helper
  const char* log = nullptr;
  int len = 0;

  // Try TpcCommitCommand (production path with RAFT_BATCH_OPTIMIZATION)
  auto tpc_cmd = marshallable_cast<TpcCommitCommand>(md);
  // tpc_cmd->cmd_ is Command; has_value() for null
  // check; marshallable_cast<T>(Command&) overload handles the cast.
  if (tpc_cmd && tpc_cmd->cmd_.has_value()) {
    // Extract VecPieceData that contains the raw bytes
    auto vpd = marshallable_cast<VecPieceData>(tpc_cmd->cmd_);
    verify(vpd != nullptr);
    if (vpd && vpd->sp_vec_piece_data_ && !vpd->sp_vec_piece_data_->empty()) {
      // Get the first SimpleCommand
      auto simple_cmd = (*vpd->sp_vec_piece_data_)[0];
      if (simple_cmd && simple_cmd->input.values_ && !simple_cmd->input.values_->empty()) {
        // Extract the raw bytes stored as STR value
        auto& first_val = simple_cmd->input.values_->begin()->second;
        if (first_val.get_kind() == Value::STR) {
          const std::string& payload = first_val.get_str();
          log = payload.c_str();
          len = static_cast<int>(payload.size());
          Log_debug("[RAFT-CALLBACK] Extracted log from VecPieceData (tx_id=%lu): len=%d",
                    tpc_cmd->tx_id_, len);
        } else {
          Log_error("[RAFT-CALLBACK] VecPieceData value is not STR type for slot %d", slot_id);
          return status;
        }
      } else {
        Log_error("[RAFT-CALLBACK] VecPieceData SimpleCommand has no values for slot %d", slot_id);
        return status;
      }
    } else {
      Log_error("[RAFT-CALLBACK] TpcCommitCommand.cmd_ is not VecPieceData for slot %d", slot_id);
      return status;
    }
  } else {
    Log_error("[RAFT-CALLBACK] Command is not TpcCommitCommand for partition %d, slot %d",
              site_info_ ? site_info_->partition_id_ : -1, slot_id);
    return status;
  }

  // Extract par_id from the committed entry's SimpleCommand::partition_id_.
  uint32_t par_id = 0;
  if (tpc_cmd && tpc_cmd->cmd_.has_value()) {
    auto vpd_inner = marshallable_cast<VecPieceData>(tpc_cmd->cmd_);
    verify(vpd_inner != nullptr);
    if (vpd_inner && vpd_inner->sp_vec_piece_data_ && !vpd_inner->sp_vec_piece_data_->empty()) {
      par_id = (*vpd_inner->sp_vec_piece_data_)[0]->partition_id_;
    }
  }

  bool am_leader = IsLeader(par_id);

  // Route to per-partition callback maps first, fall back to global callbacks
  watermark_callback_t* active_callback_ptr = nullptr;
  auto& cb_map = am_leader ? leader_callbacks_by_partition_ : follower_callbacks_by_partition_;
  auto cb_it = cb_map.find(par_id);
  if (raft_worker_partition_callback_available(
          cb_it != cb_map.end(),
          cb_it != cb_map.end() && static_cast<bool>(cb_it->second))) {
    active_callback_ptr = &cb_it->second;
  } else {
    auto& global_cb = am_leader ? leader_callback_par_id_return_ : follower_callback_par_id_return_;
    if (raft_worker_global_callback_available(static_cast<bool>(global_cb))) {
      active_callback_ptr = &global_cb;
    }
  }

  if (!active_callback_ptr) {
    Log_error("[RAFT-CALLBACK] No %s callback registered for partition %d",
              am_leader ? "leader" : "follower", par_id);
    return status;
  }

  Log_debug("[RAFT-CALLBACK] Applying log at slot %d par_id %d using %s callback",
            slot_id, par_id, am_leader ? "LEADER" : "FOLLOWER");

  auto& un_replay_queue = un_replay_logs_by_partition_[par_id];

  int encoded_value = (*active_callback_ptr)(
      log, len, par_id, slot_id, un_replay_queue);

  status = encoded_value % 10;
  uint32_t timestamp = encoded_value / 10;

  if (raft_worker_should_buffer_unreplayed(
          status, janus::PaxosStatus::STATUS_SAFETY_FAIL, len)) {
      char* dest = static_cast<char*>(malloc(len));
      verify(dest != nullptr);
      memcpy(dest, log, len);
      un_replay_queue.push(std::make_tuple(timestamp, slot_id, status, len,
                                           static_cast<const char*>(dest)));
  }

  Log_debug("Raft applied log at slot %d: status=%d, timestamp=%u, role=%s, par_id=%d",
            slot_id, status, timestamp, am_leader ? "leader" : "follower", par_id);

  return status;
  } // end @unsafe block for const char* log
}

// SINGLE-RAFT: Per-partition callback registration methods
// @unsafe
void RaftWorker::register_leader_callback_for_partition(uint32_t par_id, watermark_callback_t cb) {
  leader_callbacks_by_partition_[par_id] = std::move(cb);

  if (!raft_worker_can_register_callback(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition %d", par_id);
    return;
  }

  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }

  Log_info("[SINGLE-RAFT] Registered leader callback for partition %d", par_id);
}

// @unsafe
void RaftWorker::register_follower_callback_for_partition(uint32_t par_id, watermark_callback_t cb) {
  follower_callbacks_by_partition_[par_id] = std::move(cb);

  if (!raft_worker_can_register_callback(rep_sched_ != nullptr)) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition %d", par_id);
    return;
  }

  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }

  Log_info("[SINGLE-RAFT] Registered follower callback for partition %d", par_id);
}

// @unsafe
void RaftWorker::SubmitLoop() {
  std::unique_lock<std::mutex> lock(submit_mutex_);
  while (true) {
    submit_cv_.wait(lock, [&] {
      // @unsafe
      { // operator bool on std::atomic<bool>
        return raft_worker_submit_loop_should_wake(
            submit_thread_stop_.load(), submit_queue_.empty());
      }
    });
    bool should_stop = false;
    // @unsafe
    { // operator bool on std::atomic<bool>
      should_stop = raft_worker_submit_loop_should_stop(
          submit_thread_stop_.load(), submit_queue_.empty());
    }
    if (should_stop) {
      break;
    }

    int limit = raft_worker_batch_limit(state_core_.batch_limit());
    std::vector<RaftWorkerPendingLog> batch;
    batch.reserve(limit);
    while (raft_worker_submit_loop_should_take(
        submit_queue_.empty(), static_cast<int>(batch.size()), limit)) {
      batch.push_back(std::move(submit_queue_.front()));
      submit_queue_.pop_front();
    }
    lock.unlock();

    for (auto& entry : batch) {
      int current_epoch;
      {
        std::lock_guard<std::recursive_mutex> state_lock(election_state_lock);
        current_epoch = CurrentEpoch();
      }
      if (!raft_worker_pending_log_is_current(entry.epoch, current_epoch)) {
        Log_debug("[RAFT-WORKER] Dropping stale queued entry: epoch=%d current_epoch=%d partition=%u",
                  entry.epoch, current_epoch, entry.par_id);
        continue;
      }
      Submit(entry.payload.data(), static_cast<int>(entry.payload.size()), entry.par_id);
    }

    lock.lock();
  }
}

} // namespace janus
