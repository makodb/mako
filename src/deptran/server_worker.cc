#include "server_worker.h"
#include "benchmark_control_rpc.h"
#include "frame.h"
#include "communicator.h"
#include "raft/frame.h"
#include "raft/raft_lane.h"

namespace janus {

void ServerWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  svr_hb_poll_thread_worker_g = rusty::Some(PollThread::create());
  hb_rpc_server_ = new srpc::Server(srpc::Server::new_(rusty::Some(svr_hb_poll_thread_worker_g.as_ref().unwrap().clone())));

  // Create shared status and pass clone to service
  server_status_ = rusty::Some(rusty::Arc<ServerStatus>::make());
  hb_rpc_server_->reg_service_typed(rusty::make_box<ServerControlServiceImpl>(server_status_.as_ref().unwrap().clone(), timeout));

  auto port = this->site_info_->port + ServerWorker::CtrlPortDelta;
  std::string addr_port = std::string("0.0.0.0:") +
      std::to_string(port);
  hb_rpc_server_->start(reinterpret_cast<const int8_t*>(addr_port.c_str()));
  if (hb_rpc_server_ != nullptr) {
    // Log_info("notify ready to control script for {}", bind_addr.c_str());
    server_status_.as_ref().unwrap()->set_ready();
  }
  Log_info("heartbeat setup for {} on {}",
           this->site_info_->name.c_str(), addr_port.c_str());
}

void ServerWorker::SetupBase() {
  auto config = Config::GetConfig();
  verify(config->IsReplicated());
  Log_info("replica_proto_={}", config->replica_proto_);

  rep_frame_ = dynamic_cast<RaftFrame*>(Frame::GetFrame(config->replica_proto_));
  verify(rep_frame_ != nullptr);
  rep_frame_->site_info_ = site_info_;
  rep_sched_ = rep_frame_->CreateRaftScheduler();
  verify(rep_sched_ != nullptr);
  // One interface call rather than three field writes, as RaftWorker does.
  rep_sched_->set_site_identity(site_info_->locale_id, site_info_->id,
                                site_info_->partition_id_);

  // RaftLab installs its agreement oracle only after all communicators exist.
  // Keep recovery fail-closed in the meantime: an unexpected replay must stop
  // startup instead of invoking an empty std::function.
  rep_sched_->reg_learner_action(
      [](slotid_t, janus::Command) -> int {
        throw std::runtime_error(
            "RaftLab learner callback not installed yet");
      });
}

void ServerWorker::SetupService() {
  // raft-rt's transport serves Raft on its own poll thread, admission closed
  // until startup.
  std::string bind_addr = site_info_->GetBindAddress();
  rust_transport_ = raft_lane::Serve(rep_sched_, bind_addr);
  rep_frame_->rust_transport_ = rust_transport_;
  Log_info("Server {} ready at {}", site_info_->name.c_str(), bind_addr.c_str());
}

void ServerWorker::WaitForShutdown() {
  Log_debug("{}", __FUNCTION__);
  if (hb_rpc_server_ != nullptr) {
    hb_rpc_server_->wait_for_shutdown();
    delete hb_rpc_server_;  // Server destructor cleans up owned scsi_
    hb_rpc_server_ = nullptr;
    // Arc auto-releases on destruction.
  }
  Log_debug("exit {}", __FUNCTION__);
}

void ServerWorker::SetupCommo() {
  raft_lane::ConnectPeers(rust_transport_);
#ifdef RAFT_TEST_CORO
  RaftFrame::RustLaneLabCommoCreated();
#endif
  auto* sched = rep_sched_;
  raft_lane::Post(rust_transport_, [sched]() { sched->EnsureSetup(); });
  if (!rep_sched_->WaitForStartup()) {
    Log_error("[RAFT-STARTUP] Site {} failed startup; RPC admission remains closed",
              site_info_->id);
    return;
  }
  raft_lane::SetAdmissionReady(rust_transport_, true);
#ifdef RAFT_TEST_CORO
  // Site 0 runs the lab harness on this thread's reactor.
  RaftFrame::RustLaneLabRunIfSite0(site_info_->locale_id);
#endif
}

void ServerWorker::ShutDown() {
  // Close admission and drain, then drop the server, before the scheduler
  // goes.
  if (rust_transport_ != nullptr) {
    raft_lane::Drain(rust_transport_, 5000);
    if (rep_sched_ != nullptr) {
      rep_sched_->PrepareForShutdown();
    }
    raft_lane::CloseServer(rust_transport_);
    if (rep_frame_ != nullptr) {
      rep_frame_->rust_transport_ = nullptr;
    }
  }

  // Stop the heartbeat poll thread during explicit shutdown so test-mode runs
  // don't leave background activity alive until global destructors.
  if (svr_hb_poll_thread_worker_g.is_some()) {
    Log_info("Shutting down heartbeat poll thread in ServerWorker::ShutDown()");
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
    svr_hb_poll_thread_worker_g = rusty::None;
  }

  // The worker is the SOLE owner of the scheduler CreateScheduler() returned:
  // RaftFrame::svr_ is a borrowed back-reference, not a unique_ptr, so this
  // delete is the only one. Matches PaxosWorker (paxos_worker.cc:240) and
  // RaftWorker (raft_worker.cc:564).
  if (rep_sched_ != nullptr) {
    Log_info("Deleting replication scheduler in RAFT_TEST_CORO shutdown");
    // Drop the frame's borrowed back-reference first; see
    // RaftFrame::ReleaseScheduler.
    if (rep_frame_ != nullptr) {
      rep_frame_->ReleaseScheduler();
    }
    // @unsafe { raw delete of an owned pointer at the ownership boundary }
    delete rep_sched_;
    rep_sched_ = nullptr;
  }
  // The poll thread and clients last, after the scheduler is gone.
  if (rust_transport_ != nullptr) {
    raft_lane::Destroy(rust_transport_);
    rust_transport_ = nullptr;
  }
  Log_info("ServerWorker shutdown complete.");
}

ServerWorker::~ServerWorker() {
  // Shut the heartbeat poll thread down if this worker owns it.
  if (svr_hb_poll_thread_worker_g.is_some()) {
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
  }
}


} // namespace janus
