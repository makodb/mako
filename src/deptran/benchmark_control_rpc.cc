#include "__dep__.h"
#include "benchmark_control_rpc.h"

#include "srpc/srpc.hpp"

namespace janus {
std::vector<ServerControlServiceImpl *> ServerControlServiceImpl::scsi_s{};
std::atomic<bool> ServerControlServiceImpl::closing_{false};

void ServerControlServiceImpl::shutdown_wrapper(int sig) {
  if (closing_.load()) {
    return;
  }
  for (auto s : scsi_s) {
    if (s != nullptr) {
      s->do_shutdown();
    }
  }
}

void ServerControlServiceImpl::set_sig_handler() const {
  struct sigaction sact;
  sigemptyset(&sact.sa_mask);
  sact.sa_flags = 0;
  sact.sa_handler = shutdown_wrapper;
  sigaction(SIGALRM, &sact, NULL);
}

void ServerControlServiceImpl::do_shutdown() const {
  Log_info("Shutdown Server Control Service");
  status_->set_shutdown();
}

void ServerControlServiceImpl::server_shutdown(
    const ServerControlService::RpcServerShutdownRequest& rpc_req,
    ServerControlService::RpcServerShutdownResponse& rpc_resp,
    srpc::DeferredReply defer) const {
  (void)rpc_req;
  (void)rpc_resp;
  do_shutdown();
  defer.reply();
}

void ServerControlServiceImpl::server_ready(
    const ServerControlService::RpcServerReadyRequest& rpc_req,
    ServerControlService::RpcServerReadyResponse& rpc_resp,
    srpc::DeferredReply defer) const {
  (void)rpc_req;
  rpc_resp.res = status_->is_ready() ? 1 : 0;
  defer.reply();
}

void ServerControlServiceImpl::server_heart_beat(
    const ServerControlService::RpcServerHeartBeatRequest& rpc_req,
    ServerControlService::RpcServerHeartBeatResponse& rpc_resp,
    srpc::DeferredReply defer) const {
  (void)rpc_req;
  (void)rpc_resp;
  std::call_once(sig_handler_once_, [this] { set_sig_handler(); });
  if (!closing_.load()) {
    alarm(timeout_);
  }
  defer.reply();
}

// removed 3rd `Recorder *recorder` ctor
// parameter (and its `if (recorder) { ... set_recorder(recorder); }`
// body) — every caller passed nullptr.
ServerControlServiceImpl::ServerControlServiceImpl(rusty::Arc<ServerStatus> status,
                                                   unsigned int timeout) :
        status_(std::move(status)),
        timeout_(timeout) {
  scsi_s.push_back(this);
}

// Each heartbeat re-arms an alarm whose handler calls do_shutdown on every
// registered service. Services are destroyed as the workers shut down, and
// the alarm the last heartbeat armed went off after that, on a destroyed
// service, crashing the process on its way out. So the first destructor
// cancels the alarm and keeps heartbeats from arming it again, and each
// takes itself off the list (in place, as the handler may be reading it).
ServerControlServiceImpl::~ServerControlServiceImpl() {
  closing_.store(true);
  alarm(0);
  for (auto& s : scsi_s) {
    if (s == this) {
      s = nullptr;
    }
  }
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
