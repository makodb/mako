#include "server_worker.h"
#include "service.h"
#include "benchmark_control_rpc.h"
#include "sharding.h"
#include "scheduler.h"
#include "frame.h"
#include "communicator.h"
#include "raft/server.h"

#include <gperftools/profiler.h>

namespace janus {

void ServerWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  int n_io_threads = 1;
//  svr_hb_poll_thread_worker_g = new rrr::PollThread(n_io_threads);
  svr_hb_poll_thread_worker_g = svr_poll_thread_worker_;
//  hb_thread_pool_g = new rrr::ThreadPool(1);
  hb_thread_pool_g = svr_thread_pool_;
  hb_rpc_server_ = new rrr::Server(rusty::Some(svr_hb_poll_thread_worker_g.as_ref().unwrap().clone()));

  // Create shared status and pass clone to service
  server_status_ = rusty::Some(rusty::Arc<ServerStatus>::make());
  hb_rpc_server_->reg_service(rusty::make_box<ServerControlServiceImpl>(server_status_.as_ref().unwrap().clone(), timeout));

  auto port = this->site_info_->port + ServerWorker::CtrlPortDelta;
  std::string addr_port = std::string("0.0.0.0:") +
      std::to_string(port);
  hb_rpc_server_->start(addr_port.c_str());
  if (hb_rpc_server_ != nullptr) {
    // Log_info("notify ready to control script for %s", bind_addr.c_str());
    server_status_.as_ref().unwrap()->set_ready();
  }
  Log_info("heartbeat setup for %s on %s",
           this->site_info_->name.c_str(), addr_port.c_str());
}

void ServerWorker::SetupBase() {
  auto config = Config::GetConfig();
  Log_info("tx_proto_=%d replica_proto_=%d", config->tx_proto_, config->replica_proto_);

#ifdef RAFT_TEST_CORO
  // In test mode, only initialize replication frame services
  if (config->IsReplicated()) {
    rep_frame_ = Frame::GetFrame(config->replica_proto_);
    rep_frame_->site_info_ = site_info_;
    rep_sched_ = rep_frame_->CreateScheduler();
    rep_sched_->loc_id_ = site_info_->locale_id;
    rep_sched_->site_id_ = site_info_->id;
    rep_sched_->rep_frame_ = rep_frame_;
    
    // Start election timer after site_id_ is properly initialized
    // if (RaftServer* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
    //   raft_server->StartElectionTimerForTest();
    // }
  }
#else
  // Normal mode - initialize both transaction and replication services
  tx_frame_ = Frame::GetFrame(config->tx_proto_);
  tx_frame_->site_info_ = site_info_;
  // this needs to be done before poping table
  sharding_ = tx_frame_->CreateSharding(Config::GetConfig()->sharding_);
  sharding_->BuildTableInfoPtr();
  verify(tx_reg_ == nullptr);
  tx_reg_ = std::make_shared<TxnRegistry>();
  tx_sched_ = tx_frame_->CreateScheduler();
  tx_sched_->txn_reg_ = tx_reg_;
  tx_sched_->SetPartitionId(site_info_->partition_id_);
  tx_sched_->loc_id_ = site_info_->locale_id;
  tx_sched_->site_id_ = site_info_->id;
  sharding_->tx_sched_ = tx_sched_;

  if (config->IsReplicated() &&
      config->replica_proto_ != config->tx_proto_) {
    rep_frame_ = Frame::GetFrame(config->replica_proto_);
    rep_frame_->site_info_ = site_info_;
    rep_sched_ = rep_frame_->CreateScheduler();
    rep_sched_->txn_reg_ = tx_reg_;
    rep_sched_->loc_id_ = site_info_->locale_id;
    rep_sched_->site_id_ = site_info_->id;
#ifdef CPU_PROFILE_SEVER
    if (rep_sched_->site_id_ == 0) {
      ProfilerStart("server.prof");
    }
#endif
    rep_sched_->tx_sched_ = tx_sched_;
    rep_sched_->rep_frame_ = rep_frame_;

    tx_sched_->rep_frame_ = rep_frame_;
    tx_sched_->rep_sched_ = rep_sched_;
  }
  // add callbacks to execute commands to rep_sched_
  if (rep_sched_ && tx_sched_) {
    rep_sched_->RegLearnerAction(std::bind(
        static_cast<int(TxLogServer::*)(int, shared_ptr<Marshallable>)>(&TxLogServer::Next),
        tx_sched_,
        std::placeholders::_1,
        std::placeholders::_2));
  }
#endif
}

void ServerWorker::PopTable() {
#ifdef RAFT_TEST_CORO
  // In test mode, we don't need to populate tables since we're only testing Raft
  Log_info("Skipping table population in test mode");
  return;
#else
  // populate table
  int ret = 0;
  // get all tables
  std::vector<std::string> table_names;

  Log_info("start data population for site %d", site_info_->id);
  ret = sharding_->GetTableNames(site_info_->partition_id_, table_names);
  verify(ret > 0);

  for (auto table_name : table_names) {
    mdb::Schema* schema = new mdb::Schema();
    mdb::symbol_t symbol;
    sharding_->init_schema(table_name, schema, &symbol);
    mdb::Table* tb;

    switch (symbol) {
      case mdb::TBL_SORTED:
        tb = new mdb::SortedTable(table_name, schema);
        break;
      case mdb::TBL_UNSORTED:
        tb = new mdb::UnsortedTable(table_name, schema);
        break;
      case mdb::TBL_SNAPSHOT:
        tb = new mdb::SnapshotTable(table_name, schema);
        break;
      default:
        verify(0);
    }
    tx_sched_->reg_table(table_name, tb);
  }
  verify(sharding_);
  sharding_->PopulateTables(site_info_->partition_id_);
  Log_info("data populated for site: %x, partition: %x",
           site_info_->id, site_info_->partition_id_);
  verify(ret > 0);
#endif
}

void ServerWorker::RegisterWorkload() {
#ifdef RAFT_TEST_CORO
  // In test mode, we don't need to register workload since we're only testing Raft
  Log_info("Skipping workload registration in test mode");
  return;
#else
  Workload* workload = Workload::CreateWorkload(Config::GetConfig());
  verify(tx_reg_ != nullptr);
  verify(sharding_ != nullptr);
  workload->sss_ = sharding_;
  workload->txn_reg_ = tx_reg_;
  workload->RegisterPrecedures();
#endif
}

void ServerWorker::SetupService() {
  Log_info("enter %s for %s @ %s", __FUNCTION__,
           this->site_info_->name.c_str(),
           site_info_->GetBindAddress().c_str());

  int ret;
  auto config = Config::GetConfig();
  // set running mode and initialize transaction manager.
  std::string bind_addr = site_info_->GetBindAddress();

  // init rrr::PollThread
  svr_poll_thread_worker_ = rusty::Some(PollThread::create());
//  svr_thread_pool_ = new rrr::ThreadPool(1);

  // Use as_ref().unwrap() to borrow without consuming the Option
  auto& poll_worker = svr_poll_thread_worker_.as_ref().unwrap();

  // init rrr::Server first (before registering services)
  rpc_server_ = new rrr::Server(rusty::Some(poll_worker.clone()));

  // Create and register services (ownership transferred to rpc_server_)
#ifdef RAFT_TEST_CORO
  // In test mode, only initialize replication services
  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service(std::move(svc));
    }
  }
#else
  if (tx_frame_ != nullptr) {
    auto services = tx_frame_->CreateRpcServices(site_info_->id,
                                                  tx_sched_,
                                                  poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service(std::move(svc));
    }
  }

  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service(std::move(svc));
    }
  }
#endif

//  auto& alarm = TimeoutALock::get_alarm_s();
//  ServerWorker::svr_poll_thread_worker_->add(&alarm);

  uint32_t num_threads = 1;
//  thread_pool_g = new base::ThreadPool(num_threads);

  // start rpc server
  Log_debug("starting server at %s", bind_addr.c_str());
  ret = rpc_server_->start(bind_addr.c_str());
  if (ret != 0) {
    Log_fatal("server launch failed.");
  }

  Log_info("Server %s ready at %s",
           site_info_->name.c_str(),
           bind_addr.c_str());

}

void ServerWorker::WaitForShutdown() {
  Log_debug("%s", __FUNCTION__);
  if (hb_rpc_server_ != nullptr) {
    hb_rpc_server_->wait_for_shutdown();
    delete hb_rpc_server_;  // Server destructor cleans up owned scsi_
    // svr_hb_poll_thread_worker_g automatically released by shared_ptr
    if (hb_thread_pool_g != svr_thread_pool_)
      hb_thread_pool_g->release();

    // Use for_each_service to access services owned by rpc_server_
    if (rpc_server_ != nullptr) {
      rpc_server_->for_each_service([](rrr::Service& service) {
        if (DepTranServiceImpl* s = dynamic_cast<DepTranServiceImpl*>(&service)) {
          auto& recorder = s->recorder_;
          if (recorder) {
            auto n_flush_avg_ = recorder->stat_cnt_.peek().avg_;
            auto sz_flush_avg_ = recorder->stat_sz_.peek().avg_;
            Log::info("Log to disk, average log per flush: %lld,"
                          " average size per flush: %lld",
                      n_flush_avg_, sz_flush_avg_);
          }
        }
      });
    }
  }
  Log_debug("exit %s", __FUNCTION__);
}

void ServerWorker::SetupCommo() {
  verify(svr_poll_thread_worker_.is_some());
  if (tx_frame_) {
    tx_commo_ = tx_frame_->CreateCommo(svr_poll_thread_worker_);
    if (tx_commo_) {
      tx_commo_->loc_id_ = site_info_->locale_id;
    }
    tx_sched_->commo_ = tx_commo_;
  }
  if (rep_frame_) {
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_);
    if (rep_commo_) {
      rep_commo_->loc_id_ = site_info_->locale_id;
    }
    verify(rep_commo_ != nullptr);
    rep_sched_->commo_ = rep_commo_;
    verify(rep_sched_->commo_ != nullptr);
    rep_commo_->rep_sched_ = rep_sched_;
  }

  Reactor::GetReactor()->server_id_ = site_info_->id;
//  svr_thread_pool_ = new rrr::ThreadPool(1);
  auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob(
    [this]() {
      if (rep_sched_) {
        rep_sched_->Setup();
      }
    }
  ));
  // Cast OneTimeJob to Job base class for PollThread
  auto arc_job_base = rusty::Arc<Job>(arc_job);
  svr_poll_thread_worker_.as_ref().unwrap()->add(arc_job_base);

#ifdef RAFT_TEST_CORO
// dead loop this thread for coroutine scheduling 
// TODO, figure out a better approach
  if (rep_sched_->site_id_ == 0) {
    Reactor::GetReactor()->Loop(true, true);
  }
#endif
}

void ServerWorker::Pause() {
  Log_info("!!!!!!!! ServerWorker::Pause()");
  rep_sched_->Pause();
  // pause() not implemented in PollThreadWorker;
}

void ServerWorker::Resume() {
  // resume() not implemented in PollThreadWorker;
  rep_sched_->Resume();
}

void ServerWorker::ShutDown() {
  Log_debug("deleting rpc_server_ (services owned by server)");

  // Merged: mako-dev's cleanup + Jetpack's rep_sched_ deletion
  // Services are now owned by rpc_server_ and will be deleted with it
  delete rpc_server_;
  rpc_server_ = nullptr;

  // Modern C++ - smart pointer auto-cleanup
  // svr_poll_thread_worker_ automatically released by shared_ptr

  // Jetpack: Clean up replication scheduler (raw pointer needs manual deletion)
  Log_info("Deleting replication scheduler...");
  if (rep_sched_) delete rep_sched_;
  Log_info("ServerWorker shutdown complete.");
}

int ServerWorker::DbChecksum() {
  // auto cs = this->tx_sched_->mdb_txn_mgr_->Checksum();
  uint32_t cs = this->tx_sched_->ChecksumXor();
  Log_info("site_id: %d shard_id: %d checksum: %x", (int)this->site_info_->id,
           (int)this->site_info_->partition_id_, (int) cs);
  return cs;
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

