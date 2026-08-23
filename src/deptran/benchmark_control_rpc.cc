#include "__dep__.h"
#include "benchmark_control_rpc.h"
#include "stats_registry.h"

#include "rrr/rrr.hpp"

namespace janus {
vector<ServerControlServiceImpl *> ServerControlServiceImpl::scsi_s{};

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
}
