#include "__dep__.h"
#include "command.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "benchmark_control_rpc.h"
#include "client_worker.h"
#include "stats_registry.h"

#include "rrr/rrr.hpp"


extern vector<unique_ptr<janus::ClientWorker>> client_workers_g;

namespace janus {
vector<ServerControlServiceImpl *> ServerControlServiceImpl::scsi_s{};

const char S_RES_KEY_N_SCC[] = "scc";
const char S_RES_KEY_N_ASK[] = "ask";
const char S_RES_KEY_START_GRAPH[] = "start_graph";
const char S_RES_KEY_COMMIT_GRAPH[] = "commit_graph";
const char S_RES_KEY_ASK_GRAPH[] = "ask_graph";

void ServerControlServiceImpl::shutdown_wrapper(int sig) {
  for (auto s : scsi_s) {
    s->do_shutdown();
  }
}

void ServerControlServiceImpl::set_sig_handler() {
  struct sigaction sact;
  sigemptyset(&sact.sa_mask);
  sact.sa_flags = 0;
  sact.sa_handler = shutdown_wrapper;
  sigaction(SIGALRM, &sact, NULL);
  sig_handler_set_ = true;
}

void ServerControlServiceImpl::do_shutdown() {
  Log_info("Shutdown Server Control Service");
  status_->set_shutdown();
}

void ServerControlServiceImpl::server_shutdown(
    const ServerControlService::RpcServerShutdownRequest& rpc_req,
    ServerControlService::RpcServerShutdownResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  (void)rpc_resp;
  do_shutdown();
  defer.reply();
}

void ServerControlServiceImpl::server_ready(
    const ServerControlService::RpcServerReadyRequest& rpc_req,
    ServerControlService::RpcServerReadyResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  rpc_resp.res = status_->is_ready() ? 1 : 0;
  defer.reply();
}

void ServerControlServiceImpl::do_statistics(const char *key,
                                             int64_t value_delta) {
  StatsRegistry::instance().do_statistics(key, value_delta);
}

void ServerControlServiceImpl::server_heart_beat(
    const ServerControlService::RpcServerHeartBeatRequest& rpc_req,
    ServerControlService::RpcServerHeartBeatResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  (void)rpc_resp;
  if (!sig_handler_set_)
    set_sig_handler();
  alarm(timeout_);
  defer.reply();
}

void ServerControlServiceImpl::server_heart_beat_with_data(
    const ServerControlService::RpcServerHeartBeatWithDataRequest& rpc_req,
    ServerControlService::RpcServerHeartBeatWithDataResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  ServerResponse *res = &rpc_resp.res;
  res->cpu_util = 0.0;

  // collapsed `if (recorder) { ... } else
  // {res->r_cnt_sum = 0; ... }` to just the else branch — recorder
  // was always nullptr; field + getter both gone.
  res->r_cnt_sum = 0;
  res->r_cnt_num = 0;
  res->r_sz_sum = 0;
  res->r_sz_num = 0;
  if (!sig_handler_set_)
    set_sig_handler();
  alarm(timeout_);

  // Get statistics from StatsRegistry
  auto& registry = StatsRegistry::instance();
  auto statistics = registry.get_all_statistics();
  for (auto it = statistics.begin(); it != statistics.end(); it++) {
    res->statistics[std::string(it->first)] = it->second;
  }

  auto stats = registry.get_all_stats();
  for (auto &pair : stats) {
    auto &name = pair.first;
    auto &stat = pair.second;
    auto ss = stat->reset();
    verify(ss.sum_ >= 0);
    verify(ss.n_stat_ >= 0);
    Log_info("stat name: {}, value: {}, times: {}",
             name.c_str(), ss.sum_, ss.n_stat_);
    res->statistics[name].value = ss.sum_;
    res->statistics[name].times = ss.n_stat_;
  }

  defer.reply();
}

// removed 3rd `Recorder *recorder` ctor
// parameter (and its `if (recorder) { ... set_recorder(recorder); }`
// body) — every caller passed nullptr.
ServerControlServiceImpl::ServerControlServiceImpl(rusty::Arc<ServerStatus> status,
                                                   unsigned int timeout) :
        status_(std::move(status)),
        timeout_(timeout),
        sig_handler_set_(false) {
  scsi_s.push_back(this);
}

ServerControlServiceImpl::~ServerControlServiceImpl() {
}

bool operator<(const struct timespec &lhs, const struct timespec &rhs) {
  if (lhs.tv_sec < rhs.tv_sec)
    return true;
  else if (lhs.tv_sec == rhs.tv_sec)
    if (lhs.tv_nsec < rhs.tv_nsec)
      return true;
    else
      return false;
  else
    return false;
}

void clock_gettime(struct timespec *time) {
#ifdef __APPLE__ // OS X
  clock_serv_t cclock;
  mach_timespec_t mts;
  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);
  time->tv_sec = mts.tv_sec;
  time->tv_nsec = mts.tv_nsec;
#else
  ::clock_gettime(CLOCK_REALTIME, time);
#endif
}

double timespec2ms(struct timespec time) {
  return time.tv_sec * 1000.0 + time.tv_nsec / 1000000.0;
}

// =============================================================================
// ClientControlServiceImpl - Delegates to Arc<ClientStatus>
// =============================================================================

ClientControlServiceImpl::ClientControlServiceImpl(rusty::Arc<ClientStatus> status)
    : status_(std::move(status)) {
}

ClientControlServiceImpl::~ClientControlServiceImpl() {
  // ClientStatus is managed by Arc, nothing to do here
}

void ClientControlServiceImpl::client_shutdown(
    const ClientControlService::RpcClientShutdownRequest& rpc_req,
    ClientControlService::RpcClientShutdownResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  (void)rpc_resp;
  Log_info("Shutdown Client Control Service");
  status_->set_status(ClientStatus::Status::STOP);
  defer.reply();
}

void ClientControlServiceImpl::client_force_stop(
    const ClientControlService::RpcClientForceStopRequest& rpc_req,
    ClientControlService::RpcClientForceStopResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  (void)rpc_resp;
  unsigned int num_threads = status_->num_threads();
  pthread_t** coo_threads = status_->coo_threads();
  for (unsigned int i = 0; i < num_threads; i++) {
    if (coo_threads[i] != nullptr) {
      pthread_kill(*(coo_threads[i]), SIGALRM);
    }
  }
  defer.reply();
}

void ClientControlServiceImpl::client_response(
    const ClientControlService::RpcClientResponseRequest& rpc_req,
    ClientControlService::RpcClientResponseResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req.dep_id;
  rpc_resp.res.is_finish = status_->collect_response(&rpc_resp.res) ? 1 : 0;
#ifdef LOG_LEVEL_AS_DEBUG
  LogClientResponse(&rpc_resp.res);
#endif
  defer.reply();
}

void ClientControlServiceImpl::client_ready_block(
    const ClientControlService::RpcClientReadyBlockRequest& rpc_req,
    ClientControlService::RpcClientReadyBlockResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  rpc_resp.res = 1;
  if (status_->get_status() != ClientStatus::Status::READY) {
    status_->wait_until_ready_or_stop();
  }
  defer.reply();
}

void ClientControlServiceImpl::client_ready(
    const ClientControlService::RpcClientReadyRequest& rpc_req,
    ClientControlService::RpcClientReadyResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  rpc_resp.res = (status_->get_status() == ClientStatus::Status::READY) ? 1 : 0;
  defer.reply();
}

void ClientControlServiceImpl::client_start(
    const ClientControlService::RpcClientStartRequest& rpc_req,
    ClientControlService::RpcClientStartResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  (void)rpc_resp;
  status_->set_status_run_and_start_timer();
  defer.reply();
}

void ClientControlServiceImpl::client_get_txn_names(
    const ClientControlService::RpcClientGetTxnNamesRequest& rpc_req,
    ClientControlService::RpcClientGetTxnNamesResponse& rpc_resp,
    rrr::DeferredReply defer) {
  (void)rpc_req;
  rpc_resp.txn_names = status_->txn_names();
  defer.reply();
}

const int MAX_LAT_LOG = 25;

void ClientControlServiceImpl::LogClientResponse(ClientResponse *res) {
  Log_debug("__{}__", __FUNCTION__);
  Log_debug("run_sec: {}", res->run_sec);
  Log_debug("run_nsec: {}", res->run_nsec);
  Log_debug("period_sec: {}", res->period_sec);
  Log_debug("period_nsec: {}", res->period_nsec);

  unsigned int num_threads = status_->num_threads();
  auto* txn_info = status_->txn_info();

  for (unsigned int i = 0; i < num_threads; i++) {
    for (auto it = txn_info[i].begin(); it != txn_info[i].end(); it++) {
      Log_debug("{}: start_txn: {}", it->first, res->txn_info[it->first].start_txn);
      Log_debug("{}: total_txn: {}", it->first, res->txn_info[it->first].total_txn);
      Log_debug("{}: total_try: {}", it->first, res->txn_info[it->first].total_try);
      Log_debug("{}: commit_txn: {}", it->first, res->txn_info[it->first].commit_txn);

      char output[1024];
      output[0] = '\0';
      Log_debug("{}: interval_latency: ", it->first);
      auto &interval_lat = res->txn_info[it->first].interval_latency;
      size_t cnt = 0;
      int n = 0;
      for (auto lat_it = interval_lat.begin(); lat_it != interval_lat.end() && n < MAX_LAT_LOG; ++lat_it, ++n) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%0.6f, ", *lat_it);
        if (strlen(buf) + cnt < sizeof(output)) {
          cnt += strlen(buf);
        } else {
          Log_debug("{}", output);
          output[0] = '\0';
          cnt = strlen(buf);
        }
        strcat(output, buf);
      }
      if (strlen(output) > 0) {
        Log_debug("{}", output);
      }
    }
  }
  Log_debug("__End {}__", __FUNCTION__);
}

void ClientControlServiceImpl::DispatchTxn(
    const ClientControlService::RpcDispatchTxnRequest& rpc_req,
    ClientControlService::RpcDispatchTxnResponse& rpc_resp,
    rrr::DeferredReply defer) {
  const TxDispatchRequest& req = rpc_req.req;
  // TODO: fix -- we dont need to do this everytime.
  std::vector<ClientWorker*> locale0_workers;
  for (auto& worker : client_workers_g) {
    Log_debug("{} worker {}; site {}; locale {}", __FUNCTION__, worker->id, worker->my_site_.id, worker->my_site_.locale_id);
    if (worker->my_site_.locale_id == 0)
      locale0_workers.push_back(worker.get());
  }
  verify(locale0_workers.size() > 0);
  auto worker = locale0_workers[rrr::RandomGenerator::rand(0, locale0_workers.size()-1)];
  Log_info("{}: from coo {}; site {}", __FUNCTION__, req.id, worker->my_site_.id);
  verify(worker->my_site_.locale_id == 0);
  TxRequest request;
  size_t i = 0;
  for (auto &v : req.input) {
    request.input_[i] = v;
    i++;
  }
  request.n_try_ = 0;
  request.tx_type_ = req.tx_type;
  worker->AcceptForwardedRequest(std::move(request), &rpc_resp.result);
  defer.reply();
}
}
