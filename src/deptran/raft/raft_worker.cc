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
#include "../tpc_command.h"  // TpcCommitCommand for batch optimization
#include "../procedure.h"            // VecPieceData and SimpleCommand

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
        is_leader = leader ? 1 : 0;
      }
      // Notify all partitions that currently have callback registrations.
      std::set<uint32_t> par_ids;
      for (const auto& kv : leader_callbacks_by_partition_) {
        par_ids.insert(kv.first);
      }
      for (const auto& kv : follower_callbacks_by_partition_) {
        par_ids.insert(kv.first);
      }
      if (par_ids.empty()) {
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
}

// @unsafe - uses delete, raw pointers, Option<Arc<PollThread>>
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

  StopSubmitThread();

  if (rep_sched_) {
    delete rep_sched_;
    rep_sched_ = nullptr;
  }

  // IMPORTANT: Shutdown poll threads AFTER servers are destroyed.
  // Server::~Server() enqueues remove commands to the poll thread; keeping the
  // poll thread alive allows it to drain those commands and drop the final
  // references so sconns_ctr_ reaches zero.
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
      if (rep_frame_->site_info_->partition_id_ != par_id) {
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
    return rep_frame_->site_info_->partition_id_ == par_id;
  }
}

// @unsafe
void RaftWorker::StartSubmitThread() {
  if (submit_thread_started_) {
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
  if (!submit_thread_started_) {
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
  if (!submit_thread_started_) {
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
    batch_limit_ = std::max(batch_size, 1);
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

  auto vpd = rusty::Arc<VecPieceData>::make();
  // @unsafe - unique-owner mutation window (factory-fresh Arc).
  vpd.get_mut().unwrap().sp_vec_piece_data_ =
      std::make_shared<vector<shared_ptr<SimpleCommand>>>();

  auto simple_cmd = std::make_shared<SimpleCommand>();

  simple_cmd->input.values_ = std::make_shared<map<int32_t, Value>>();
  (*simple_cmd->input.values_)[0] = Value(std::string(log_entry, length));
  simple_cmd->input.keys_.insert(0);
  // Store the partition id so callback routing is always explicit.
  simple_cmd->partition_id_ = par_id;

  vpd->sp_vec_piece_data_->push_back(simple_cmd);

  auto tpc_cmd = rusty::Arc<TpcCommitCommand>::make();
  // @unsafe - unique-owner mutation window (factory-fresh Arc).
  {
    auto& mut_cmd = tpc_cmd.get_mut().unwrap();
    mut_cmd.tx_id_ = tx_id;
    mut_cmd.cmd_ = std::move(vpd);
  }

  Log_debug("[RAFT-LOG-CMD] Created TpcCommitCommand tx_id={} with {} bytes (Mako/test payload)",
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

  // Use the production helper to create proper TpcCommitCommand{cmd_=VecPieceData}
  auto tpc_cmd = CreateRaftLogCommand(log_entry, length, tx_id, par_id);

  uint64_t index = 0;
  uint64_t term = 0;
  bool appended = raft_server->Start(std::move(tpc_cmd), &index, &term);
  if (!appended) {
    return;
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
  while (n_submit < tot_num) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  lock.unlock();

  while (true) {
    {
      std::lock_guard<std::mutex> qlock(submit_mutex_);
      if (submit_queue_.empty()) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// @unsafe
void RaftWorker::register_apply_callback(std::function<void(const char*, int)> cb) {
  // @unsafe
  { this->callback_ = cb; }

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
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
    std::function<void(const char*&, int, int)> cb) {
  // @unsafe
  { this->callback_par_id_ = cb; }

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
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
void RaftWorker::register_leader_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  // @unsafe
  { this->leader_callback_par_id_return_ = cb; }

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition {}",
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

  Log_info("[RAFT-CALLBACK] Registered leader callback for partition {}",
           site_info_ ? site_info_->partition_id_ : -1);
}

// @unsafe
void RaftWorker::register_follower_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  // @unsafe
  { this->follower_callback_par_id_return_ = cb; }

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition {}",
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
int RaftWorker::Next(int slot_id, janus::Command md) {
  int status = -1;

  // @unsafe
  { // null check on Command envelope
    if (!md.has_value()) {
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
  const auto tpc_cmd = marshallable_cast<TpcCommitCommand>(md);
  // tpc_cmd is Option<Arc<TpcCommitCommand>>; the payload's cmd_ is
  // Command; has_value() for null
  // check; marshallable_cast<T>(Command&) overload handles the cast.
  if (tpc_cmd.is_some() && tpc_cmd.unwrap()->cmd_.has_value()) {
    // Extract VecPieceData that contains the raw bytes
    const auto vpd = marshallable_cast<VecPieceData>(tpc_cmd.unwrap()->cmd_);
    verify(vpd.is_some());
    if (vpd.is_some() && vpd.unwrap()->sp_vec_piece_data_ && !vpd.unwrap()->sp_vec_piece_data_->empty()) {
      // Get the first SimpleCommand
      auto simple_cmd = (*vpd.unwrap()->sp_vec_piece_data_)[0];
      if (simple_cmd && simple_cmd->input.values_ && !simple_cmd->input.values_->empty()) {
        // Extract the raw bytes stored as STR value
        auto& first_val = simple_cmd->input.values_->begin()->second;
        if (first_val.get_kind() == Value::STR) {
          const std::string& payload = first_val.get_str();
          log = payload.c_str();
          len = static_cast<int>(payload.size());
          Log_debug("[RAFT-CALLBACK] Extracted log from VecPieceData (tx_id={}): len={}",
                    tpc_cmd.unwrap()->tx_id_, len);
        } else {
          Log_error("[RAFT-CALLBACK] VecPieceData value is not STR type for slot {}", slot_id);
          return status;
        }
      } else {
        Log_error("[RAFT-CALLBACK] VecPieceData SimpleCommand has no values for slot {}", slot_id);
        return status;
      }
    } else {
      Log_error("[RAFT-CALLBACK] TpcCommitCommand.cmd_ is not VecPieceData for slot {}", slot_id);
      return status;
    }
  } else {
    Log_error("[RAFT-CALLBACK] Command is not TpcCommitCommand for partition {}, slot {}",
              site_info_ ? site_info_->partition_id_ : -1, slot_id);
    return status;
  }

  // Extract par_id from the committed entry's SimpleCommand::partition_id_.
  uint32_t par_id = 0;
  if (tpc_cmd.is_some() && tpc_cmd.unwrap()->cmd_.has_value()) {
    const auto vpd_inner = marshallable_cast<VecPieceData>(tpc_cmd.unwrap()->cmd_);
    verify(vpd_inner.is_some());
    if (vpd_inner.is_some() && vpd_inner.unwrap()->sp_vec_piece_data_ && !vpd_inner.unwrap()->sp_vec_piece_data_->empty()) {
      par_id = (*vpd_inner.unwrap()->sp_vec_piece_data_)[0]->partition_id_;
    }
  }

  bool am_leader = IsLeader(par_id);

  // Route to per-partition callback maps first, fall back to global callbacks
  watermark_callback_t* active_callback_ptr = nullptr;
  auto& cb_map = am_leader ? leader_callbacks_by_partition_ : follower_callbacks_by_partition_;
  auto cb_it = cb_map.find(par_id);
  if (cb_it != cb_map.end() && cb_it->second) {
    active_callback_ptr = &cb_it->second;
  } else {
    auto& global_cb = am_leader ? leader_callback_par_id_return_ : follower_callback_par_id_return_;
    if (global_cb) {
      active_callback_ptr = &global_cb;
    }
  }

  if (!active_callback_ptr) {
    Log_error("[RAFT-CALLBACK] No {} callback registered for partition {}",
              am_leader ? "leader" : "follower", par_id);
    return status;
  }

  Log_debug("[RAFT-CALLBACK] Applying log at slot {} par_id {} using {} callback",
            slot_id, par_id, am_leader ? "LEADER" : "FOLLOWER");

  auto& un_replay_queue = un_replay_logs_by_partition_[par_id];

  int encoded_value = (*active_callback_ptr)(
      log, len, par_id, slot_id, un_replay_queue);

  status = encoded_value % 10;
  uint32_t timestamp = encoded_value / 10;

  if (status == janus::PaxosStatus::STATUS_SAFETY_FAIL && len > 0) {
      char* dest = static_cast<char*>(malloc(len));
      verify(dest != nullptr);
      memcpy(dest, log, len);
      un_replay_queue.push(std::make_tuple(timestamp, slot_id, status, len,
                                           static_cast<const char*>(dest)));
  }

  Log_debug("Raft applied log at slot {}: status={}, timestamp={}, role={}, par_id={}",
            slot_id, status, timestamp, am_leader ? "leader" : "follower", par_id);

  return status;
  } // end @unsafe block for const char* log
}

// SINGLE-RAFT: Per-partition callback registration methods
// @unsafe
void RaftWorker::register_leader_callback_for_partition(uint32_t par_id, watermark_callback_t cb) {
  leader_callbacks_by_partition_[par_id] = cb;

  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition {}", par_id);
    return;
  }

  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
  }

  Log_info("[SINGLE-RAFT] Registered leader callback for partition {}", par_id);
}

// @unsafe
void RaftWorker::register_follower_callback_for_partition(uint32_t par_id, watermark_callback_t cb) {
  follower_callbacks_by_partition_[par_id] = cb;

  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition {}", par_id);
    return;
  }

  // @unsafe
  { // rep_sched_-> raw pointer dereference, RegLearnerAction
    rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                           this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
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
        return submit_thread_stop_ || !submit_queue_.empty();
      }
    });
    bool should_stop = false;
    // @unsafe
    { // operator bool on std::atomic<bool>
      should_stop = submit_thread_stop_ && submit_queue_.empty();
    }
    if (should_stop) {
      break;
    }

    int limit = std::max(batch_limit_, 1);
    std::vector<PendingLog> batch;
    batch.reserve(limit);
    while (!submit_queue_.empty() && static_cast<int>(batch.size()) < limit) {
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
