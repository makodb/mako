#pragma once
#include <rusty/arc.hpp>

#include "__dep__.h"
#include "config.h"
#include "communicator.h"
#include "procedure.h"

namespace janus {

class ClientControlServiceImpl;
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
  int32_t mode;
  bool batch_start;
  uint32_t id;
  uint32_t duration;
  int outbound;  // Jetpack: track outbound requests
  ClientControlServiceImpl *ccsi{nullptr};
  int32_t n_concurrent_;
  map<cooid_t, bool> n_pause_concurrent_{};  // Jetpack: pause tracking
  rrr::Mutex finish_mutex{};
  rrr::CondVar finish_cond{};
  bool forward_requests_to_leader_ = false;

  // coordinators_{mutex, cond} synchronization currently only used for open clients
  std::mutex request_gen_mutex{};
  std::mutex coordinator_mutex{};
  vector<Coordinator*> free_coordinators_{};
  vector<Coordinator*> created_coordinators_{};
  Coordinator* fail_ctrl_coo_{nullptr};  // Jetpack: failover coordinator
  std::shared_ptr<TimeoutEvent> timeout_event;
  std::shared_ptr<NEvent> n_event;
  std::shared_ptr<AndEvent> and_event;

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

  // Jetpack: Latency statistics
  Distribution cli2cli_[10];
  int go_to_jetpack_fastpath_cnt_ = 0;
  vector<std::pair<double, double>> commit_time_;
  Frequency frequency_;
#ifdef LATENCY_DEBUG
  Distribution client2leader_, client2test_point_, client2leader_send_;
#endif

  // Jetpack: Leader tracking for Raft
  locid_t cur_leader_{0};
  bool failover_wait_leader_{false};
  bool failover_trigger_loc{false};
  bool failover_pause_start{false};

  OneArmedBandit one_armed_bandit_;  // Jetpack: fast path prediction

 public:
  // Merged constructor: Jetpack failover params + mako-dev PollThread type
  ClientWorker(uint32_t id,
               Config::SiteInfo& site_info,
               Config* config,
               ClientControlServiceImpl* ccsi,
               rusty::Option<rusty::Arc<PollThread>> poll_thread_worker = rusty::None,
               bool* volatile failover = nullptr,
               volatile bool* failover_server_quit = nullptr,
               volatile locid_t* failover_server_idx = nullptr,
               volatile double* total_throughput = nullptr);
  ClientWorker() = delete;
  ~ClientWorker();
  void retrive_statistic();  // Jetpack: statistics collection
  // This is called from a different thread.
  void Work();
  Coordinator* FindOrCreateCoordinator();
  void FailoverPreprocess(Coordinator* coo);  // Jetpack: failover handling
  void DispatchRequest(Coordinator *coo, bool void_request=false);  // Jetpack: extended signature
  void SearchLeader(Coordinator* coo);  // Jetpack: leader discovery
  void Pause(locid_t locid);  // Jetpack: pause server
  void Resume(locid_t locid);  // Jetpack: resume server
  Coordinator* CreateFailCtrlCoordinator();  // Jetpack: create failover coordinator
  void AcceptForwardedRequest(TxRequest &request, TxReply* txn_reply, rrr::DeferredReply* defer);

 protected:
  Coordinator* CreateCoordinator(uint16_t offset_id);
  void RequestDone(Coordinator* coo, TxReply &txn_reply);
  void ForwardRequestDone(Coordinator* coo, TxReply* output, rrr::DeferredReply* defer, TxReply &txn_reply);
};
} // namespace janus
