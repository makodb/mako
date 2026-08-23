#pragma once

#include <rusty/arc.hpp>

#include "__dep__.h"
#include "config.h"
#include "server_status.h"

namespace janus {

class Communicator;
class Frame;
class TxLogServer;

// ServerWorker is the small embedded-server harness used by the RAFT_TEST
// executable. Production Mako creates RaftWorker/PaxosWorker directly.
class ServerWorker {
 public:
  rusty::Option<rusty::Arc<rrr::PollThread>> svr_poll_thread_worker_;
  // Services are now owned by rpc_server_ via reg_service()
  rrr::Server *rpc_server_ = nullptr;

  rusty::Option<rusty::Arc<rrr::PollThread>> svr_hb_poll_thread_worker_g;
  rusty::Option<rusty::Arc<ServerStatus>> server_status_;
  rrr::Server *hb_rpc_server_ = nullptr;

  Frame* rep_frame_ = nullptr;
  Config::SiteInfo *site_info_ = nullptr;
  TxLogServer *rep_sched_ = nullptr;

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

  void SetupHeartbeat();
  void SetupBase();
  void SetupService();
  void SetupCommo();
  void ShutDown();
  void Pause();
  void Resume();

  static const uint32_t CtrlPortDelta = 10000;
  void WaitForShutdown();
};

} // namespace janus
