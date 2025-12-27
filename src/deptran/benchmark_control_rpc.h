#ifndef BENCHMARK_CTRL_H_
#define BENCHMARK_CTRL_H_

#include "rcc/dep_graph.h"
#include "rcc_rpc.h" // before this one include all the custom data structures.
#include "server_status.h"
#include "client_status.h"
#include <rusty/function.hpp>
#include <rusty/mutex.hpp>
#include <rusty/condvar.hpp>
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
  void server_shutdown(rrr::DeferredReply defer) override;
  void server_ready(i32 *res, rrr::DeferredReply defer) override;
  void server_heart_beat_with_data(ServerResponse *res, rrr::DeferredReply defer) override;
  void server_heart_beat(rrr::DeferredReply defer) override;

  ServerControlServiceImpl(rusty::Arc<ServerStatus> status,
                           unsigned int timeout = 5,
                           Recorder *recorder = nullptr);
  ~ServerControlServiceImpl();

  // Movable but not copyable (Arc is movable and clonable)
  ServerControlServiceImpl(ServerControlServiceImpl&&) = default;
  ServerControlServiceImpl& operator=(ServerControlServiceImpl&&) = default;
  ServerControlServiceImpl(const ServerControlServiceImpl&) = delete;
  ServerControlServiceImpl& operator=(const ServerControlServiceImpl&) = delete;

  void do_statistics(const char *key, int64_t value_delta);
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
  // RPC handlers
  void client_get_txn_names(std::map<i32, std::string> *txn_names, rrr::DeferredReply defer) override;
  void client_shutdown(rrr::DeferredReply defer) override;
  void client_force_stop(rrr::DeferredReply defer) override;
  void client_response(const DepId& dep_id, ClientResponse *res, rrr::DeferredReply defer) override;
  void client_ready_block(i32 *res, rrr::DeferredReply defer) override;
  void client_ready(i32 *res, rrr::DeferredReply defer) override;
  void client_start(rrr::DeferredReply defer) override;
  void DispatchTxn(const TxDispatchRequest& req, TxReply* txn_reply, rrr::DeferredReply defer) override;

  // Constructor takes Arc<ClientStatus> - shared state managed externally
  ClientControlServiceImpl(rusty::Arc<ClientStatus> status);
  ~ClientControlServiceImpl();

  // Movable but not copyable (Arc is movable and clonable)
  ClientControlServiceImpl(ClientControlServiceImpl&&) = default;
  ClientControlServiceImpl& operator=(ClientControlServiceImpl&&) = default;
  ClientControlServiceImpl(const ClientControlServiceImpl&) = delete;
  ClientControlServiceImpl& operator=(const ClientControlServiceImpl&) = delete;
};

}

#endif
