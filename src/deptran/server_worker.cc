#include "server_worker.h"
#include "benchmark_control_rpc.h"
#include "frame.h"
#include "communicator.h"
#include "raft/frame.h"

namespace janus {

void ServerWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  svr_hb_poll_thread_worker_g = svr_poll_thread_worker_.clone();
  hb_rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(svr_hb_poll_thread_worker_g.as_ref().unwrap().clone())));

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
  rep_sched_ = dynamic_cast<RaftServer*>(rep_frame_->CreateScheduler());
  verify(rep_sched_ != nullptr);
  rep_sched_->partition_id_ = site_info_->partition_id_;
  rep_sched_->loc_id_ = site_info_->locale_id;
  rep_sched_->site_id_ = site_info_->id;
}

void ServerWorker::SetupService() {
  Log_info("enter {} for {} @ {}", __FUNCTION__,
           this->site_info_->name.c_str(),
           site_info_->GetBindAddress().c_str());

  int ret;
  std::string bind_addr = site_info_->GetBindAddress();

  // init rrr::PollThread
  svr_poll_thread_worker_ = rusty::Some(PollThread::create());
//  svr_thread_pool_ = new rrr::ThreadPool(1);

  // Use as_ref().unwrap() to borrow without consuming the Option
  auto& poll_worker = svr_poll_thread_worker_.as_ref().unwrap();

  // init rrr::Server first (before registering services)
  rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(poll_worker.clone())));

  // Create and register replication services (ownership transferred to
  // rpc_server_).
  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service_proxy(std::move(svc));
    }
  }

  // start rpc server
  Log_debug("starting server at {}", bind_addr.c_str());
  ret = rpc_server_->start(reinterpret_cast<const int8_t*>(bind_addr.c_str()));
  if (ret != 0) {
    Log_fatal("server launch failed.");
  }

  Log_info("Server {} ready at {}",
           site_info_->name.c_str(),
           bind_addr.c_str());

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
  verify(svr_poll_thread_worker_.is_some());
  if (rep_frame_) {
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_.clone());
    verify(rep_commo_ != nullptr);
    rep_sched_->commo_ = rep_commo_;
    verify(rep_sched_->commo_ != nullptr);
  }

  Reactor::get_reactor()->server_id_.set(site_info_->id);
//  svr_thread_pool_ = new rrr::ThreadPool(1);
  auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_(
    [this]() {
      if (rep_sched_) {
        rep_sched_->EnsureSetup();
      }
    }
  ));
  // Cast OneTimeJob to Job base class for PollThread
  auto arc_job_base = rusty::Arc<Job>(arc_job);
  svr_poll_thread_worker_.as_ref().unwrap()->add(arc_job_base);

  // Keep the coroutine scheduler alive for the embedded lab cluster.
  if (rep_sched_->site_id_ == 0) {
    Reactor::get_reactor()->run_loop(true, true);
  }
}

void ServerWorker::ShutDown() {
  Log_debug("deleting rpc_server_ (services owned by server)");

  // Services are now owned by rpc_server_ and will be deleted with it
  delete rpc_server_;
  rpc_server_ = nullptr;

  // Stop worker poll threads during explicit shutdown so test-mode runs don't
  // leave background reconnect/election activity alive until global destructors.
  if (svr_poll_thread_worker_.is_some()) {
    Log_info("Shutting down server poll thread in ServerWorker::ShutDown()");
    svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
    svr_poll_thread_worker_ = rusty::None;
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    Log_info("Shutting down heartbeat poll thread in ServerWorker::ShutDown()");
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
    svr_hb_poll_thread_worker_g = rusty::None;
  }

  // RaftTestConfig::Kill/Restart can replace/delete server objects
  // independently of ServerWorker, so rep_sched_ may be stale here. Skip
  // manual deletion to avoid double-free/use-after-free on shutdown.
  Log_info("Skipping replication scheduler delete in RAFT_TEST_CORO shutdown");
  rep_sched_ = nullptr;
  Log_info("ServerWorker shutdown complete.");
}

ServerWorker::~ServerWorker() {
  // Shutdown PollThreads if we own them
  if (svr_poll_thread_worker_.is_some()) {
    svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
  if (svr_hb_poll_thread_worker_g.is_some()) {
    svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
  }
}


} // namespace janus
