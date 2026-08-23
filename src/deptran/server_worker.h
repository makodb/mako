#pragma once
#include <rusty/arc.hpp>

#include "__dep__.h"
#include "marshal-value.h"
#include "command.h"
#include "procedure.h"
#include "command_marshaler.h"
#include "rcc_rpc.h"
#include "service.h"
#include "sharding.h"
#include "tx.h"
#include "workload.h"
#include "config.h"
#include "server_status.h"

namespace janus {

class Communicator;
class Frame;
class ServerWorker {
 public:
  rusty::Option<rusty::Arc<rrr::PollThread>> svr_poll_thread_worker_;
  // Services are now owned by rpc_server_ via reg_service()
  rrr::Server *rpc_server_ = nullptr;

  rusty::Option<rusty::Arc<rrr::PollThread>> svr_hb_poll_thread_worker_g;
  rusty::Option<rusty::Arc<ServerStatus>> server_status_;
  rrr::Server *hb_rpc_server_ = nullptr;

  Frame* tx_frame_ = nullptr;
  Frame* rep_frame_ = nullptr;
  Config::SiteInfo *site_info_ = nullptr;
  Sharding *sharding_ = nullptr;
  TxLogServer *tx_sched_ = nullptr;
  TxLogServer *rep_sched_ = nullptr;
  shared_ptr<TxnRegistry> tx_reg_{nullptr};

  Communicator *tx_commo_ = nullptr;
  Communicator *rep_commo_ = nullptr;

  bool launched_{false};

  // Default constructor
  ServerWorker() = default;

  // No copy - ServerWorker owns resources
  ServerWorker(const ServerWorker&) = delete;
  ServerWorker& operator=(const ServerWorker&) = delete;

  // Move operations - required for std::vector
  ServerWorker(ServerWorker&& other) noexcept = default;
  ServerWorker& operator=(ServerWorker&& other) noexcept = default;

  ~ServerWorker(); // Destructor to cleanup resources
  int DbChecksum(); // Jetpack: Database checksum for validation

  void SetupHeartbeat();
  void PopTable();
  void SetupBase();
  void SetupService();
  void SetupCommo();
  void RegisterWorkload();
  void ShutDown();
  void Pause();
  void Resume();

  // Initialize recovery for replication servers
  void InitializeRecovery(uint32_t partition_id, uint32_t locale_id);

  static const uint32_t CtrlPortDelta = 10000;
  void WaitForShutdown();
};

} // namespace janus
