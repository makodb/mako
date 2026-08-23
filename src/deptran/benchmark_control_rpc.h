#ifndef BENCHMARK_CTRL_H_
#define BENCHMARK_CTRL_H_

#include "rcc_rpc.h"
#include "server_status.h"
#include <rusty/function.hpp>
#include <rusty/box.hpp>
#include <rusty/arc.hpp>

#include <time.h>
#include <vector>
#include <sys/time.h>
#ifdef __APPLE__ // for OS X
#include <mach/clock.h>
#include <mach/mach.h>
#endif

namespace janus {

void clock_gettime(struct timespec *time);
double timespec2ms(struct timespec time);
bool operator<(const struct timespec &lhs, const struct timespec &rhs);

class ServerControlServiceImpl: public ServerControlService {
 private:
  // Shared status - owned externally, service just observes/modifies
  rusty::Arc<ServerStatus> status_;

  unsigned int timeout_;
  bool sig_handler_set_;

  static std::vector<ServerControlServiceImpl*> scsi_s;

  static void shutdown_wrapper(int sig);

  void set_sig_handler();

 public:
  // Internal shutdown without RPC reply
  void do_shutdown();

  // removed 3rd `Recorder *recorder = nullptr`
  // ctor parameter — every caller passed nullptr; the constructor's
  // `if (recorder) { StatsRegistry::set_recorder(recorder); }` body
  // never fired.  `StatsRegistry::set_recorder` is also gone.
  ServerControlServiceImpl(rusty::Arc<ServerStatus> status,
                           unsigned int timeout = 5);
  ~ServerControlServiceImpl();

  // Movable but not copyable (Arc is movable and clonable)
  ServerControlServiceImpl(ServerControlServiceImpl&&) = default;
  ServerControlServiceImpl& operator=(ServerControlServiceImpl&&) = default;
  ServerControlServiceImpl(const ServerControlServiceImpl&) = delete;
  ServerControlServiceImpl& operator=(const ServerControlServiceImpl&) = delete;

  // BEGIN typed-rpc-decls (ServerControlServiceImpl)
  // Typed RPC interface overrides (new API).
  void server_shutdown(const ServerControlService::RpcServerShutdownRequest& req, ServerControlService::RpcServerShutdownResponse& resp, rrr::DeferredReply defer) override;
  void server_ready(const ServerControlService::RpcServerReadyRequest& req, ServerControlService::RpcServerReadyResponse& resp, rrr::DeferredReply defer) override;
  void server_heart_beat(const ServerControlService::RpcServerHeartBeatRequest& req, ServerControlService::RpcServerHeartBeatResponse& resp, rrr::DeferredReply defer) override;
  // END typed-rpc-decls (ServerControlServiceImpl)
};

}

#endif
