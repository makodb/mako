#pragma once

#include <rusty/arc.hpp>
#include "../__dep__.h"
#include "../coordinator.h"
#include "../benchmark_control_rpc.h"
#include "../server_status.h"
#include "../frame.h"
#include "../scheduler.h"
#include "../communicator.h"
#include "../config.h"
#include "server.h"
#include <condition_variable>
#include <deque>
#include <thread>

// @external: {
//   Log_info: [safe, (...) -> void],
//   Log_debug: [safe, (...) -> void],
//   Log_warn: [safe, (...) -> void],
//   Log_error: [safe, (...) -> void],
//   Log_fatal: [safe, (...) -> void],
//   verify: [safe, (bool) -> void],
//   Config::GetConfig: [safe, () -> Config*],
//   Frame::GetFrame: [safe, (int) -> Frame*],
//   std::make_shared: [safe, (...) -> shared_ptr<T>],
//   dynamic_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>],
//   static_pointer_cast: [safe, (shared_ptr<T>) -> shared_ptr<U>],
//   dynamic_cast: [safe, (T*) -> U*],
//   rrr::PollThread::create: [safe, () -> Arc<PollThread>],
//   rusty::make_box: [safe, (...) -> Box<T>],
//   std::this_thread::sleep_for: [safe, (duration) -> void],
//   std::max: [safe, (T, T) -> T],
//   malloc: [unsafe, (size_t) -> void*],
//   memcpy: [unsafe, (void*, const void*, size_t) -> void*]
// }

namespace janus {

// Runtime replication switching - always declare raft functions
extern std::function<void(int)> leader_callback_;
// @unsafe - uses raw global std::function, unbounded callback invocation
void raft_handle_leader_change(uint32_t partition_id, bool is_leader);
// @unsafe - uses raw global std::function, unbounded callback invocation
void NotifyRaftLeaderChange(uint32_t partition_id, bool is_leader);

// @unsafe - class contains raw pointers and manual memory management
class RaftWorker {
private:
  // Callbacks for log application
  std::function<void(const char*, int)> callback_ = nullptr;
  std::function<void(const char*&, int, int)> callback_par_id_ = nullptr;

  // RAFT CHANGE: Store separate callbacks for leader and follower roles
  // The Next() method will choose which to call based on current leadership
  std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char*>>&)>
    leader_callback_par_id_return_ = nullptr;
  std::function<int(const char*&, int, int, int, std::queue<std::tuple<int, int, int, int, const char*>>&)>
    follower_callback_par_id_return_ = nullptr;

  std::mutex finish_mutex_{};
  std::condition_variable finish_cond_{};
  std::mutex condition_mutex_;
  struct PendingLog {
    std::string payload;
    uint32_t par_id;
  };
  std::deque<PendingLog> submit_queue_;
  std::mutex submit_mutex_;
  std::condition_variable submit_cv_;
  std::atomic<bool> submit_thread_stop_{false};
  bool submit_thread_started_{false};
  std::thread submit_thread_;
  int batch_limit_ = 1;

public:
  // Statistics
  std::atomic<int> n_current{0};   // Current in-flight requests
  std::atomic<int> n_submit{0};    // Total submitted
  std::atomic<int> n_tot{0};       // Total processed
  std::atomic<int> submit_num{0};  // For microbench
  int tot_num = 0;
  int submit_tot_sec_ = 0;
  int submit_tot_usec_ = 0;

  // Configuration
  Config::SiteInfo* site_info_ = nullptr;

  // Raft protocol components
  Frame* rep_frame_ = nullptr;
  TxLogServer* rep_sched_ = nullptr;      // Points to RaftServer
  Communicator* rep_commo_ = nullptr;

  // RPC infrastructure
  rusty::Option<rusty::Arc<PollThread>> svr_poll_thread_worker_;
  // Services are now owned by rpc_server_ via reg_service()
  rrr::Server* rpc_server_ = nullptr;
  base::ThreadPool* thread_pool_g = nullptr;

  // Heartbeat/control RPC
  rusty::Option<rusty::Arc<PollThread>> svr_hb_poll_thread_worker_g;
  rusty::Option<rusty::Arc<ServerStatus>> server_status_;
  rrr::Server* hb_rpc_server_ = nullptr;
  base::ThreadPool* hb_thread_pool_g = nullptr;

  // Queue for unreplayed logs (follower only)
  std::queue<std::tuple<int, int, int, int, const char*>> un_replay_logs_;

  // Leadership state
  int cur_epoch = 0;
  int is_leader = 0;
  std::recursive_mutex election_state_lock;

  // Constants
  static const uint32_t CtrlPortDelta = 10000;

  // Constructor & Destructor
  // @safe
  RaftWorker();
  // @safe - cleanup operations are bounded
  ~RaftWorker();

  // Setup methods
  // @unsafe - uses raw pointers, dynamic_cast
  void SetupBase();
  // @unsafe - uses new, raw pointers
  void SetupService();
  // @unsafe - uses raw pointers
  void SetupCommo();
  // @unsafe - uses new, raw pointers
  void SetupHeartbeat();

  // Shutdown
  // @unsafe - uses delete on raw pointers (manual memory management)
  void ShutDown();
  // @safe - bounded pointer dereferences
  void WaitForShutdown();
  // @safe - std::thread creation is bounded
  void StartSubmitThread();
  // @safe - mutex/condvar operations are bounded
  void StopSubmitThread();
  // @safe - raw pointer parameter is bounded (length-delimited)
  void EnqueueLog(const char* log, int len, uint32_t par_id, int batch_size);
  // @safe
  bool HasSubmitThread() const { return submit_thread_started_; }

  // Leadership & Partition queries
  // @unsafe - uses raw pointers, dynamic_cast
  bool IsLeader(uint32_t par_id);
  // @unsafe - uses raw pointers
  bool IsPartition(uint32_t par_id);

  // Log submission (called from Mako)
  // @unsafe - uses raw pointers, shared_ptr, dynamic_cast
  void Submit(const char* log, int len, uint32_t par_id);
  // @safe
  void IncSubmit();
  // @safe - mutex/condvar operations are bounded
  void WaitForSubmit();

  // Callback registration (Mako watermark integration)
  // @safe - stores callback for later invocation
  void register_apply_callback(std::function<void(const char*, int)> cb);
  // @safe - stores callback for later invocation
  void register_apply_callback_par_id(std::function<void(const char*&, int, int)> cb);

  // RAFT CHANGE: Separate registration for leader and follower callbacks
  // @safe - stores callback for later invocation
  void register_leader_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );
  // @safe - stores callback for later invocation
  void register_follower_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );

  // Legacy method for compatibility (deprecated - use leader/follower specific methods)
  // @safe - delegates to register_follower_callback_par_id_return
  void register_apply_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );

  // Application callback (called from RaftServer::applyLogs)
  // @unsafe - uses shared_ptr, dynamic_pointer_cast, raw pointers, malloc/memcpy
  int Next(int slot, shared_ptr<Marshallable> cmd);

  // @safe
  rusty::Option<rusty::Arc<PollThread>> GetPollThreadWorker() {
    // @unsafe
    { // Option::clone on Arc<PollThread>
      return svr_poll_thread_worker_.clone();
    }
  }

  // @unsafe - uses dynamic_cast, returns raw pointer
  RaftServer* GetRaftServer() {
    return dynamic_cast<RaftServer*>(rep_sched_);
  }

  // @unsafe - uses std::make_shared, raw pointers
  std::shared_ptr<TpcCommitCommand> CreateRaftLogCommand(
      const char* log_entry,
      int length,
      txnid_t tx_id);

private:
  // @safe - mutex/condvar operations are bounded
  void SubmitLoop();
};

extern vector<shared_ptr<RaftWorker>> raft_workers_g;

} // namespace janus
