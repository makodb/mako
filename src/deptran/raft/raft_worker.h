#pragma once

#include <rusty/arc.hpp>
#include "../__dep__.h"
#include "../coordinator.h"
#include "../benchmark_control_rpc.h"
#include "../frame.h"
#include "../scheduler.h"
#include "../communicator.h"
#include "../config.h"
#include "server.h"
#include <condition_variable>
#include <deque>
#include <thread>

namespace janus {

#ifdef MAKO_USE_RAFT
extern std::function<void(int)> leader_callback_;
void raft_handle_leader_change(uint32_t partition_id, bool is_leader);
static inline void NotifyRaftLeaderChange(uint32_t partition_id, bool is_leader) {
  raft_handle_leader_change(partition_id, is_leader);
}
#else
static inline void NotifyRaftLeaderChange(uint32_t, bool) {}
#endif

// RaftWorker bridges raw RaftServer slots to the callback shape Mako expects.
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

  rrr::Mutex finish_mutex_{};
  rrr::CondVar finish_cond_{};
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
  vector<rrr::Service*> services_ = {};
  rrr::Server* rpc_server_ = nullptr;
  base::ThreadPool* thread_pool_g = nullptr;

  // Heartbeat/control RPC
  rusty::Option<rusty::Arc<PollThread>> svr_hb_poll_thread_worker_g;
  ServerControlServiceImpl* scsi_ = nullptr;
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
  RaftWorker();
  ~RaftWorker();

  // Setup methods
  void SetupBase();
  void SetupService();
  void SetupCommo();
  void SetupHeartbeat();

  // Shutdown
  void ShutDown();
  void WaitForShutdown();
  void StartSubmitThread();
  void StopSubmitThread();
  void EnqueueLog(const char* log, int len, uint32_t par_id, int batch_size);
  bool HasSubmitThread() const { return submit_thread_started_; }

  // Leadership & Partition queries
  bool IsLeader(uint32_t par_id);
  bool IsPartition(uint32_t par_id);

  // Log submission (called from Mako)
  void Submit(const char* log, int len, uint32_t par_id);
  void IncSubmit();
  void WaitForSubmit();

  // Callback registration (Mako watermark integration)
  void register_apply_callback(std::function<void(const char*, int)> cb);
  void register_apply_callback_par_id(std::function<void(const char*&, int, int)> cb);

  // RAFT CHANGE: Separate registration for leader and follower callbacks
  void register_leader_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );
  void register_follower_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );

  // Legacy method for compatibility (deprecated - use leader/follower specific methods)
  void register_apply_callback_par_id_return(
    std::function<int(const char*&, int, int, int,
                      std::queue<std::tuple<int, int, int, int, const char*>>&)> cb
  );

  // Application callback (called from RaftServer::applyLogs)
  int Next(int slot, shared_ptr<Marshallable> cmd);

  // Helper methods
  rusty::Option<rusty::Arc<PollThread>> GetPollThreadWorker() {
    return svr_poll_thread_worker_;
  }

  RaftServer* GetRaftServer() {
    return dynamic_cast<RaftServer*>(rep_sched_);
  }

  // Helper to create TpcCommitCommand wrapper for raw byte payloads
  // Used by both Mako production (raw serialized transactions) and tests
  // Wraps raw bytes in VecPieceData structure required by RAFT_BATCH_OPTIMIZATION
  std::shared_ptr<TpcCommitCommand> CreateRaftLogCommand(
      const char* log_entry,
      int length,
      txnid_t tx_id);

private:
  void SubmitLoop();
};

extern vector<shared_ptr<RaftWorker>> raft_workers_g;

} // namespace janus
