#include "raft_worker.h"
#include "server.h"
#include "commo.h"
#include "service.h"
#include "../config.h"
#include "../paxos_worker.h"  // Reuse LogEntry marshalling for raw log payloads
#include "../paxos/commo.h"   // PaxosStatus enum reused by Mako watermark callbacks
#include "../classic/tpc_command.h"  // TpcCommitCommand for batch optimization
#include "../procedure.h"            // VecPieceData and SimpleCommand
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>

namespace janus {

// Constructor wires up default state; actual services spin up in Setup* methods.
RaftWorker::RaftWorker() = default;

// Destructor stops background work and tears down poll workers the helper owns.
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

// SetupBase creates the Raft scheduler and registers leader-change notifications.
void RaftWorker::SetupBase() {
  auto config = Config::GetConfig();

  // Create Raft frame (uses "raft" protocol)
  rep_frame_ = Frame::GetFrame(config->replica_proto_);

  rep_frame_->site_info_ = site_info_;

  // Create RaftServer instance
  rep_sched_ = rep_frame_->CreateScheduler();

  rep_sched_->loc_id_ = site_info_->locale_id;
  rep_sched_->site_id_ = site_info_->id;  // CRITICAL: Set site_id!
  rep_sched_->partition_id_ = site_info_->partition_id_;

  if (auto raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
    raft_server->RegisterLeaderChangeCallback([this](bool leader) {
      {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        is_leader = leader ? 1 : 0;
      }
      uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
      NotifyRaftLeaderChange(par_id, leader);
    });
  }

  this->tot_num = config->get_tot_req();
}

// SetupService starts the gRPC-style RPC server that receives AppendEntries, etc.
void RaftWorker::SetupService() {
  // Create RPC server and register Raft service
  std::string bind_addr = site_info_->GetBindAddress();

  // Create poll thread worker
  svr_poll_thread_worker_ = rusty::Some(rrr::PollThread::create());

  // Register Raft services (returns vector)
  // Use as_ref().unwrap() to borrow without consuming the Option
  auto& poll_worker = svr_poll_thread_worker_.as_ref().unwrap();
  if (rep_frame_ != nullptr) {
    services_ = rep_frame_->CreateRpcServices(site_info_->id,
                                              rep_sched_,
                                              poll_worker,
                                              scsi_);
  }

  // Create thread pool
  uint32_t num_threads = 1;
  thread_pool_g = new base::ThreadPool(num_threads);

  // Create RPC server
  rpc_server_ = new rrr::Server(poll_worker, thread_pool_g);

  // Register all services
  for (auto service : services_) {
    rpc_server_->reg_service(*service);
  }

  // Start RPC server
  int ret = rpc_server_->start(bind_addr.c_str());
  if (ret != 0) {
    Log_fatal("Raft server launch failed at %s", bind_addr.c_str());
  }
}

// SetupCommo attaches a communicator so Raft can send RPCs to its peers.
void RaftWorker::SetupCommo() {
  // Create Raft communicator
  verify(rep_frame_ != nullptr);
  verify(rep_sched_ != nullptr);

  rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_);

  rep_sched_->commo_ = rep_commo_;
}

// SetupHeartbeat brings up the control-plane RPC service used for heartbeats.
void RaftWorker::SetupHeartbeat() {
  auto config = Config::GetConfig();
  bool hb = config->do_heart_beat();

  if (!hb) {
    return;
  }

  // Setup heartbeat/control RPC server
  // ServerControlServiceImpl constructor takes (timeout, recorder)
  scsi_ = new ServerControlServiceImpl(5, nullptr);
  svr_hb_poll_thread_worker_g = rusty::Some(rrr::PollThread::create());
  hb_thread_pool_g = new base::ThreadPool(1);
  hb_rpc_server_ = new rrr::Server(svr_hb_poll_thread_worker_g.as_ref().unwrap(), hb_thread_pool_g);
  hb_rpc_server_->reg_service(*scsi_);

  auto port = site_info_->port + CtrlPortDelta;
  std::string addr_port = site_info_->GetHostAddr(CtrlPortDelta);

  hb_rpc_server_->start(addr_port.c_str());
}

// ShutDown manually deletes RPC state and the underlying scheduler.
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

  if (hb_rpc_server_ && scsi_) {
    delete hb_rpc_server_;
    hb_rpc_server_ = nullptr;
    delete scsi_;
    scsi_ = nullptr;
  }

  if (hb_thread_pool_g) {
    hb_thread_pool_g->release();
    hb_thread_pool_g = nullptr;
  }

  if (thread_pool_g) {
    thread_pool_g->release();
    thread_pool_g = nullptr;
  }

  for (auto service : services_) {
    delete service;
  }
  services_.clear();

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

// WaitForShutdown blocks until control RPC has acknowledged a shutdown request.
void RaftWorker::WaitForShutdown() {
  StopSubmitThread();

  if (hb_rpc_server_ && scsi_) {
    scsi_->server_shutdown(nullptr);
    scsi_->wait_for_shutdown();
  }
}

// IsLeader returns true only when this worker owns the partition and Raft elected it.
bool RaftWorker::IsLeader(uint32_t par_id) {
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);

  // Check if this is the right partition
  if (rep_frame_->site_info_->partition_id_ != par_id) {
    return false;
  }

  // Check if Raft server thinks we're leader
  // Use the public IsLeader() method instead of private is_leader_ field
  auto raft_server = GetRaftServer();
  if (raft_server) {
    return raft_server->IsLeader();
  }

  return false;
}

// IsPartition reports whether the worker maps to the requested partition id.
bool RaftWorker::IsPartition(uint32_t par_id) {
  verify(rep_frame_ != nullptr);
  verify(rep_frame_->site_info_ != nullptr);
  return rep_frame_->site_info_->partition_id_ == par_id;
}

// StartSubmitThread launches a background thread that batches client log submissions.
void RaftWorker::StartSubmitThread() {
  if (submit_thread_started_) {
    return;
  }
  submit_thread_stop_ = false;
  submit_thread_started_ = true;
  submit_thread_ = std::thread(&RaftWorker::SubmitLoop, this);
}

// StopSubmitThread drains any queued work and joins the background thread.
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

// EnqueueLog pushes a log payload into the submit queue so SubmitLoop can process it.
void RaftWorker::EnqueueLog(const char* log, int len, uint32_t par_id, int batch_size) {
  if (!submit_thread_started_) {
    Submit(log, len, par_id);
    return;
  }

  PendingLog entry;
  entry.payload.assign(log, len);
  entry.par_id = par_id;

  {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    batch_limit_ = std::max(batch_size, 1);
    submit_queue_.push_back(std::move(entry));
  }
  submit_cv_.notify_one();
}

// Helper function to create TpcCommitCommand with VecPieceData wrapper for raw bytes
// This is used by BOTH:
// 1. Mako production: raw serialized transaction logs from Masstree
// 2. Tests: simple raw byte payloads
// The structure matches what RAFT_BATCH_OPTIMIZATION and SetLocalAppend expect
std::shared_ptr<TpcCommitCommand> RaftWorker::CreateRaftLogCommand(
    const char* log_entry,
    int length,
    txnid_t tx_id) {

  // Create TpcCommitCommand (outer wrapper required by batch optimization)
  auto tpc_cmd = std::make_shared<TpcCommitCommand>();
  tpc_cmd->tx_id_ = tx_id;

  // Create VecPieceData (inner container that SetLocalAppend expects)
  auto vpd = std::make_shared<VecPieceData>();
  vpd->sp_vec_piece_data_ = std::make_shared<vector<shared_ptr<SimpleCommand>>>();

  // Create SimpleCommand to hold the raw payload (Mako logs or test data)
  auto simple_cmd = std::make_shared<SimpleCommand>();

  // CRITICAL: Store the raw bytes as a STRING value (not i32!)
  // SetLocalAppend iterates over input.values_ expecting i32 key-value pairs
  // for Janus transactions, but Mako sends raw serialized bytes.
  // We store as STR to avoid get_i32() crash, and will add null checks
  // in SetLocalAppend to handle this gracefully.
  // Use key=0 to store the complete serialized log data
  simple_cmd->input.values_ = std::make_shared<map<int32_t, Value>>();
  (*simple_cmd->input.values_)[0] = Value(std::string(log_entry, length));
  simple_cmd->input.keys_.insert(0);

  // Mark partition (required field)
  simple_cmd->partition_id_ = 0;

  // Assemble the structure: TpcCommitCommand → VecPieceData → SimpleCommand
  vpd->sp_vec_piece_data_->push_back(simple_cmd);
  tpc_cmd->cmd_ = vpd;

  Log_debug("[RAFT-LOG-CMD] Created TpcCommitCommand tx_id=%lu with %d bytes (Mako/test payload)",
            tx_id, length);

  return tpc_cmd;
}

// Submit hands the log to RaftServer::Start; only leaders should ever accept it.
void RaftWorker::Submit(const char* log_entry, int length, uint32_t par_id) {
  // Log_debug("[RAFT-SUBMIT] Enter Submit: par_id=%d length=%d", par_id, length);

  if (!IsLeader(par_id)) {
    // Log_debug("[RAFT-SUBMIT] Not leader for partition %d, ignoring submit", par_id);
    return;
  }

  auto raft_server = GetRaftServer();
  if (!raft_server) {
    Log_error("[RAFT-SUBMIT] RaftServer is null in Submit()");
    return;
  }

  // Log_debug("[RAFT-SUBMIT] Creating TpcCommitCommand for partition %d", par_id);

  // Use a simple incrementing tx_id (in production this would be a global txn ID)
  static std::atomic<txnid_t> next_tx_id{1};
  txnid_t tx_id = next_tx_id.fetch_add(1);

  // Use the production helper to create proper TpcCommitCommand{cmd_=VecPieceData}
  // This matches the structure expected by RAFT_BATCH_OPTIMIZATION and SetLocalAppend
  auto tpc_cmd = CreateRaftLogCommand(log_entry, length, tx_id);

  // Log_debug("[RAFT-SUBMIT] TpcCommitCommand created: tx_id=%lu, wrapped in VecPieceData", tx_id);

  auto cmd = std::static_pointer_cast<Marshallable>(tpc_cmd);

  uint64_t index = 0;
  uint64_t term = 0;
  // Log_debug("[RAFT-SUBMIT] Calling raft_server->Start() for partition %d", par_id);
  bool appended = raft_server->Start(cmd, &index, &term);
  if (!appended) {
    // Log_debug("[RAFT-SUBMIT] Start() rejected for partition %d (not leader)", par_id);
    return;
  }

  n_tot++;

  // Log_debug("[RAFT-SUBMIT] Start() succeeded for partition %d, index=%lu term=%lu n_tot=%d",
  //          par_id, index, term, n_tot.load());
}

// IncSubmit bumps the total-submitted counter used by WaitForSubmit.
void RaftWorker::IncSubmit() {
  n_submit++;
}

// WaitForSubmit blocks until the helper has pushed every queued entry to Raft.
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

// register_apply_callback installs a simple (log,len) callback for leader-side code.
void RaftWorker::register_apply_callback(std::function<void(const char*, int)> cb) {
  this->callback_ = cb;

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip apply callback registration");
    return;
  }

  rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                         this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));
}

// register_apply_callback_par_id includes the partition id in the callback signature.
void RaftWorker::register_apply_callback_par_id(
    std::function<void(const char*&, int, int)> cb) {
  this->callback_par_id_ = cb;

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip apply callback registration");
    return;
  }

  rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                         this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));
}

// RAFT CHANGE: Register leader-specific callback
void RaftWorker::register_leader_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  this->leader_callback_par_id_return_ = cb;

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip leader callback registration for partition %d",
             site_info_ ? site_info_->partition_id_ : -1);
    return;
  }

  // Register Next() with RaftServer if not already registered
  // Next() will dynamically choose leader vs follower callback
  rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                         this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));

  Log_info("[RAFT-CALLBACK] Registered leader callback for partition %d",
           site_info_ ? site_info_->partition_id_ : -1);
}

// RAFT CHANGE: Register follower-specific callback
void RaftWorker::register_follower_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  this->follower_callback_par_id_return_ = cb;

  // Guard against accessing scheduler during shutdown
  if (!rep_sched_) {
    Log_warn("[RAFT-CALLBACK] Scheduler already torn down; skip follower callback registration for partition %d",
             site_info_ ? site_info_->partition_id_ : -1);
    return;
  }

  // Register Next() with RaftServer if not already registered
  rep_sched_->RegLearnerAction(std::bind(&RaftWorker::Next,
                                         this,
                                         std::placeholders::_1,
                                         std::placeholders::_2));

  Log_info("[RAFT-CALLBACK] Registered follower callback for partition %d",
           site_info_ ? site_info_->partition_id_ : -1);
}

// Legacy method for backward compatibility - treats as follower callback
void RaftWorker::register_apply_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb) {
  Log_warn("[RAFT-CALLBACK] Using deprecated register_apply_callback_par_id_return - use register_leader/follower_callback_par_id_return instead");
  register_follower_callback_par_id_return(cb);
}

// Next is invoked for each committed Raft slot; it forwards to Mako and tracks queues.
// RAFT CHANGE: Dynamically choose leader vs follower callback based on current leadership
int RaftWorker::Next(int slot_id, shared_ptr<Marshallable> cmd) {
  int status = -1;

  if (!cmd) {
    Log_error("Received null command in Next()");
    return status;
  }

  // Extract log payload from TpcCommitCommand{cmd_=VecPieceData}
  // This matches the structure created by CreateRaftLogCommand() helper
  const char* log = nullptr;
  int len = 0;

  // Try TpcCommitCommand (production path with RAFT_BATCH_OPTIMIZATION)
  auto tpc_cmd = std::dynamic_pointer_cast<TpcCommitCommand>(cmd);
  if (tpc_cmd && tpc_cmd->cmd_) {
    // Extract VecPieceData that contains the raw bytes
    auto vpd = std::dynamic_pointer_cast<VecPieceData>(tpc_cmd->cmd_);
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

  // RAFT CHANGE: Choose callback based on current leadership status
  uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
  bool am_leader = IsLeader(par_id);

  auto& active_callback = am_leader ? leader_callback_par_id_return_ : follower_callback_par_id_return_;

  if (!active_callback) {
    Log_error("[RAFT-CALLBACK] No %s callback registered for partition %d",
              am_leader ? "leader" : "follower", par_id);
    return status;
  }

  Log_debug("[RAFT-CALLBACK] Applying log at slot %d using %s callback",
            slot_id, am_leader ? "LEADER" : "FOLLOWER");

  int encoded_value = active_callback(
      log,
      len,
      par_id,
      slot_id,
      un_replay_logs_);

  status = encoded_value % 10;
  uint32_t timestamp = encoded_value / 10;

  if (status == janus::PaxosStatus::STATUS_SAFETY_FAIL && len > 0) {
    char* dest = static_cast<char*>(malloc(len));
    verify(dest != nullptr);
    memcpy(dest, log, len);
    un_replay_logs_.push(std::make_tuple(timestamp,
                                         slot_id,
                                         status,
                                         len,
                                         static_cast<const char*>(dest)));
  }

  Log_debug("Raft applied log at slot %d: status=%d, timestamp=%u, role=%s",
            slot_id, status, timestamp, am_leader ? "leader" : "follower");

  return status;
}

// SubmitLoop repeatedly pulls pending logs and calls Submit to hand them to Raft.
void RaftWorker::SubmitLoop() {
  std::unique_lock<std::mutex> lock(submit_mutex_);
  while (true) {
    submit_cv_.wait(lock, [&] {
      return submit_thread_stop_ || !submit_queue_.empty();
    });
    if (submit_thread_stop_ && submit_queue_.empty()) {
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
