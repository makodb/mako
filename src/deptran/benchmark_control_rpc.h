#ifndef BENCHMARK_CTRL_H_
#define BENCHMARK_CTRL_H_

#include "rcc/dep_graph.h"
#include "rcc_rpc.h" // before this one include all the custom data structures.
#include "server_status.h"
#include "client_status.h"
#include <rusty/function.hpp>
#include <rusty/box.hpp>
#include <rusty/arc.hpp>

#include <time.h>
#include <sys/time.h>
#ifdef __APPLE__ // for OS X
#include <mach/clock.h>
#include <mach/mach.h>
#endif

namespace janus {

extern const char S_RES_KEY_N_SCC[];
extern const char S_RES_KEY_N_ASK[];
extern const char S_RES_KEY_START_GRAPH[];
extern const char S_RES_KEY_COMMIT_GRAPH[];
extern const char S_RES_KEY_ASK_GRAPH[];
//extern const char S_RES_KEY_CPU[];

void clock_gettime(struct timespec *time);
double timespec2ms(struct timespec time);
bool operator<(const struct timespec &lhs, const struct timespec &rhs);

class ServerControlServiceImpl: public ServerControlService {
 private:
  // Shared status - owned externally, service just observes/modifies
  rusty::Arc<ServerStatus> status_;

  unsigned int timeout_;
  bool sig_handler_set_;

  static vector<ServerControlServiceImpl*> scsi_s;

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

  void do_statistics(const char *key, int64_t value_delta);

  // BEGIN typed-rpc-decls (ServerControlServiceImpl)
  // Typed RPC interface overrides (new API).
  void server_shutdown(const ServerControlService::RpcServerShutdownRequest& req, ServerControlService::RpcServerShutdownResponse& resp, srpc::DeferredReply defer) override;
  void server_ready(const ServerControlService::RpcServerReadyRequest& req, ServerControlService::RpcServerReadyResponse& resp, srpc::DeferredReply defer) override;
  void server_heart_beat(const ServerControlService::RpcServerHeartBeatRequest& req, ServerControlService::RpcServerHeartBeatResponse& resp, srpc::DeferredReply defer) override;
  void server_heart_beat_with_data(const ServerControlService::RpcServerHeartBeatWithDataRequest& req, ServerControlService::RpcServerHeartBeatWithDataResponse& resp, srpc::DeferredReply defer) override;
  // END typed-rpc-decls (ServerControlServiceImpl)
};

/**
 * ClientControlServiceImpl handles RPC requests for client control.
 * Shared state is managed by ClientStatus, which can be held by both
 * this service and external callers (ClientWorker, Coordinators).
 *
 * This allows using reg_service() with owned Box<Service> while
 * external code can still access the shared state via Arc<ClientStatus>.
 */
class ClientControlServiceImpl: public ClientControlService {
 private:
  // Shared status - owned externally, service just observes/modifies
  rusty::Arc<ClientStatus> status_;

  void LogClientResponse(ClientResponse *res);

 public:
  // Constructor takes Arc<ClientStatus> - shared state managed externally
  ClientControlServiceImpl(rusty::Arc<ClientStatus> status);
  ~ClientControlServiceImpl();

  // Movable but not copyable (Arc is movable and clonable)
  ClientControlServiceImpl(ClientControlServiceImpl&&) = default;
  ClientControlServiceImpl& operator=(ClientControlServiceImpl&&) = default;
  ClientControlServiceImpl(const ClientControlServiceImpl&) = delete;
  ClientControlServiceImpl& operator=(const ClientControlServiceImpl&) = delete;

  // BEGIN typed-rpc-decls (ClientControlServiceImpl)
  // Typed RPC interface overrides (new API).
  void client_shutdown(const ClientControlService::RpcClientShutdownRequest& req, ClientControlService::RpcClientShutdownResponse& resp, srpc::DeferredReply defer) override;
  void client_force_stop(const ClientControlService::RpcClientForceStopRequest& req, ClientControlService::RpcClientForceStopResponse& resp, srpc::DeferredReply defer) override;
  void client_response(const ClientControlService::RpcClientResponseRequest& req, ClientControlService::RpcClientResponseResponse& resp, srpc::DeferredReply defer) override;
  void client_ready_block(const ClientControlService::RpcClientReadyBlockRequest& req, ClientControlService::RpcClientReadyBlockResponse& resp, srpc::DeferredReply defer) override;
  void client_ready(const ClientControlService::RpcClientReadyRequest& req, ClientControlService::RpcClientReadyResponse& resp, srpc::DeferredReply defer) override;
  void client_start(const ClientControlService::RpcClientStartRequest& req, ClientControlService::RpcClientStartResponse& resp, srpc::DeferredReply defer) override;
  void client_get_txn_names(const ClientControlService::RpcClientGetTxnNamesRequest& req, ClientControlService::RpcClientGetTxnNamesResponse& resp, srpc::DeferredReply defer) override;
  void DispatchTxn(const ClientControlService::RpcDispatchTxnRequest& req, ClientControlService::RpcDispatchTxnResponse& resp, srpc::DeferredReply defer) override;
  // END typed-rpc-decls (ClientControlServiceImpl)
};

}

#endif
