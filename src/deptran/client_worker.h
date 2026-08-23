#pragma once
#include <rusty/arc.hpp>
#include <rusty/option.hpp>
#include <mutex>
#include <condition_variable>

#include "__dep__.h"
#include "config.h"
#include "communicator.h"
#include "procedure.h"
#include "scheduler.h"
#include "client_status.h"

namespace janus {
class Workload;
class CoordinatorBase;
class Frame;
class Coordinator;
class TxnRegistry;
class TxReply;

class ClientWorker {
 public:
  // Merged: Use mako-dev's Option<Arc<PollThread>> type for memory safety
  rusty::Option<rusty::Arc<PollThread>> poll_thread_worker_;
  Frame* frame_{nullptr};
  Communicator* commo_{nullptr};
  cliid_t cli_id_;
  int32_t benchmark;
  bool batch_start;
  uint32_t id;
  uint32_t duration;
  int outbound;  // Jetpack: track outbound requests
  // Shared client status for synchronization and statistics
  rusty::Option<rusty::Arc<ClientStatus>> client_status_;
  int32_t n_concurrent_;
  map<cooid_t, bool> n_pause_concurrent_{};  // Jetpack: pause tracking
  std::mutex finish_mutex{};
  std::condition_variable finish_cond{};
  bool forward_requests_to_leader_ = false;

  // coordinators_{mutex, cond} synchronization currently only used for open clients
  std::mutex request_gen_mutex{};
  std::mutex coordinator_mutex{};
  vector<Coordinator*> free_coordinators_{};
  vector<Coordinator*> created_coordinators_{};
  Coordinator* fail_ctrl_coo_{nullptr};  // Jetpack: failover coordinator

  std::atomic<uint32_t> num_txn, success, num_try;
  int all_done_{0};  // Jetpack: completion flag
  int64_t n_tx_issued_{0};  // Jetpack: transaction counter
  SharedIntEvent n_ceased_client_{};  // Jetpack: client shutdown tracking
  SharedIntEvent sp_n_tx_done_{};  // Jetpack: done transaction counter
  Workload * tx_generator_{nullptr};
  Timer *timer_{nullptr};
  shared_ptr<TxnRegistry> txn_reg_{nullptr};
  Config* config_{nullptr};
  Config::SiteInfo& my_site_;
  vector<string> servers_;

  // Jetpack: Failover control pointers
  bool* volatile failover_trigger_;
  volatile bool* failover_server_quit_;
  volatile locid_t* failover_server_idx_;
  volatile double* total_throughput_;

  // Latency statistics
  Distribution request_latency_;
  vector<std::pair<double, double>> commit_time_;
#ifdef LATENCY_DEBUG
  Distribution client2leader_, client2test_point_, client2leader_send_;
#endif

  // Jetpack: Leader tracking for Raft
  locid_t cur_leader_{0};
  bool failover_wait_leader_{false};
  bool failover_trigger_loc{false};
  bool failover_pause_start{false};

 public:
  // Merged constructor: Jetpack failover params + mako-dev PollThread type
  // Takes Arc<ClientStatus> for synchronization and statistics instead of raw pointer
  ClientWorker(uint32_t id,
               Config::SiteInfo& site_info,
               Config* config,
               rusty::Option<rusty::Arc<ClientStatus>> client_status,
               rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::None,
               bool* volatile failover = nullptr,
               volatile bool* failover_server_quit = nullptr,
               volatile locid_t* failover_server_idx = nullptr,
               volatile double* total_throughput = nullptr);
  ClientWorker() = delete;
  ~ClientWorker();
  // removed `void retrive_statistic();`
  // declaration — body was `verify(0); // No longer need...`; only
  // call site (`s_main.cc:242`) was already commented out.
  // This is called from a different thread.
  void Work();
  Coordinator* FindOrCreateCoordinator();
  void FailoverPreprocess(Coordinator* coo);  // Jetpack: failover handling
  void DispatchRequest(Coordinator *coo, bool void_request=false);  // Jetpack: extended signature
  void SearchLeader(Coordinator* coo);  // Jetpack: leader discovery
  void Pause(locid_t locid);  // Jetpack: pause server
  void Resume(locid_t locid);  // Jetpack: resume server
  Coordinator* CreateFailCtrlCoordinator();  // Jetpack: create failover coordinator
  void AcceptForwardedRequest(TxRequest request, TxReply* txn_reply);

 protected:
  Coordinator* CreateCoordinator(uint16_t offset_id);
  void RequestDone(Coordinator* coo, TxReply &txn_reply);
  void ForwardRequestDone(Coordinator* coo, TxReply* output, rusty::Function<void()> reply_cb, TxReply &txn_reply);
};
} // namespace janus
