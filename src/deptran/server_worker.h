#pragma once

#include <rusty/arc.hpp>

#include "__dep__.h"
#include "config.h"
#include "server_status.h"

namespace janus {

struct RaftTransport;  // raft-rt

class RaftFrame;
class RaftServer;

// ServerWorker is the small embedded-server harness used by the RAFT_TEST
// executable. Production Mako creates RaftWorker/PaxosWorker directly.
class ServerWorker {
 public:
  rusty::Option<rusty::Arc<srpc::PollThread>> svr_hb_poll_thread_worker_g;
  rusty::Option<rusty::Arc<ServerStatus>> server_status_;
  srpc::Server *hb_rpc_server_ = nullptr;

  RaftFrame* rep_frame_ = nullptr;
  Config::SiteInfo *site_info_ = nullptr;
  RaftServer *rep_sched_ = nullptr;

  // raft-rt's transport (raft/raft_lane.h): the poll thread, the RPC server
  // and the peer clients.
  RaftTransport *rust_transport_ = nullptr;

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

  static const uint32_t CtrlPortDelta = 10000;
  void WaitForShutdown();
};

} // namespace janus
