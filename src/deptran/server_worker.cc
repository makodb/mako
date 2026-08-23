#include "server_worker.h"
#include "benchmark_control_rpc.h"
#include "sharding.h"
#include "scheduler.h"
#include "frame.h"
#include "communicator.h"
#include "raft/server.h"
#include "paxos/server.h"
#include "raft/recovery_manager.hpp"

#include <gperftools/profiler.h>

namespace janus {

void ServerWorker::SetupHeartbeat() {
  bool hb = Config::GetConfig()->do_heart_beat();
  if (!hb) return;
  auto timeout = Config::GetConfig()->get_ctrl_timeout();
  int n_io_threads = 1;
//  svr_hb_poll_thread_worker_g = new rrr::PollThread(n_io_threads);
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
  Log_info("tx_proto_={} replica_proto_={}", config->tx_proto_, config->replica_proto_);

#ifdef RAFT_TEST_CORO
  // In test mode, only initialize replication frame services
  if (config->IsReplicated()) {
    rep_frame_ = Frame::GetFrame(config->replica_proto_);
    rep_frame_->site_info_ = site_info_;
    rep_sched_ = rep_frame_->CreateScheduler();
    rep_sched_->loc_id_ = site_info_->locale_id;
    rep_sched_->site_id_ = site_info_->id;
    rep_sched_->rep_frame_ = rep_frame_;

    // Note: RAFT_TEST_CORO mode uses RaftPersistence (src/deptran/raft/) controlled
    // by MAKO_RAFT_PERSISTENCE env var, not the mako-dev RecoveryManager system.
    // InitializeRecovery() is intentionally not called here.

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

    // Initialize recovery for replication servers
    InitializeRecovery(site_info_->partition_id_, site_info_->locale_id);
  }
  // add callbacks to execute commands to rep_sched_
  if (rep_sched_ && tx_sched_) {
    rep_sched_->RegLearnerAction(std::bind(
        static_cast<int(TxLogServer::*)(int, janus::Command)>(&TxLogServer::Next),
        tx_sched_,
        std::placeholders::_1,
        std::placeholders::_2));

    // Start state machine recovery tracking
    tx_sched_->SetRecoveryMode(true);

    // Replay committed entries after callback is registered
    if (auto* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
      raft_server->ReplayCommittedEntries();
    }
    if (auto* paxos_server = dynamic_cast<PaxosServer*>(rep_sched_)) {
      paxos_server->ReplayCommittedEntries();
    }

    // End state machine recovery tracking
    tx_sched_->SetRecoveryMode(false);
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

  Log_info("start data population for site {}", site_info_->id);
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
  Log_info("data populated for site: {:x}, partition: {:x}",
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
  Log_info("enter {} for {} @ {}", __FUNCTION__,
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
  rpc_server_ = new rrr::Server(rrr::Server::new_(rusty::Some(poll_worker.clone())));

  // Create and register services (ownership transferred to rpc_server_)
#ifdef RAFT_TEST_CORO
  // In test mode, only initialize replication services
  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service_proxy(std::move(svc));
    }
  }
#else
  if (tx_frame_ != nullptr) {
    auto services = tx_frame_->CreateRpcServices(site_info_->id,
                                                  tx_sched_,
                                                  poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service_proxy(std::move(svc));
    }
  }

  if (rep_frame_ != nullptr) {
    auto services = rep_frame_->CreateRpcServices(site_info_->id,
                                                   rep_sched_,
                                                   poll_worker);
    for (auto& svc : services) {
      rpc_server_->reg_service_proxy(std::move(svc));
    }
  }
#endif

//  auto& alarm = TimeoutALock::get_alarm_s();
//  ServerWorker::svr_poll_thread_worker_->add(&alarm);

  uint32_t num_threads = 1;
//  thread_pool_g = new base::ThreadPool(num_threads);

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
    // svr_hb_poll_thread_worker_g automatically released by shared_ptr
    // Arc auto-releases on destruction (hb_thread_pool_g goes out of scope with ServerWorker)

    // removed `for_each_service(...) { if
    // (auto* s = dynamic_cast<ClassicServiceImpl*>(...)) { auto&
    // recorder = s->recorder_; if (recorder) { Log_info(...) } } }`
    // block — `Service::recorder_` field is always nullptr (now
    // gone); the `if (recorder)` branch was unreachable.
  }
  Log_debug("exit {}", __FUNCTION__);
}

void ServerWorker::SetupCommo() {
  verify(svr_poll_thread_worker_.is_some());
  if (tx_frame_) {
    tx_commo_ = tx_frame_->CreateCommo(svr_poll_thread_worker_.clone());
    if (tx_commo_) {
      tx_commo_->loc_id_ = site_info_->locale_id;
    }
    tx_sched_->commo_ = tx_commo_;
  }
  if (rep_frame_) {
    rep_commo_ = rep_frame_->CreateCommo(svr_poll_thread_worker_.clone());
    if (rep_commo_) {
      rep_commo_->loc_id_ = site_info_->locale_id;
    }
    verify(rep_commo_ != nullptr);
    rep_sched_->commo_ = rep_commo_;
    verify(rep_sched_->commo_ != nullptr);
  }

  Reactor::get_reactor()->server_id_.set(site_info_->id);
//  svr_thread_pool_ = new rrr::ThreadPool(1);
  auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob::new_(
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
    Reactor::get_reactor()->run_loop(true, true);
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

  // Modern C++ - smart pointer auto-cleanup
  // svr_poll_thread_worker_ automatically released by shared_ptr

  // In lab test mode, RaftTestConfig::Kill/Restart can replace/delete server
  // objects independently of ServerWorker, so rep_sched_ may be stale here.
  // Skip manual deletion to avoid double-free/use-after-free on shutdown.
#ifdef RAFT_TEST_CORO
  Log_info("Skipping replication scheduler delete in RAFT_TEST_CORO shutdown");
  rep_sched_ = nullptr;
#else
  // Production mode keeps ownership here.
  Log_info("Deleting replication scheduler...");
  if (rep_sched_) delete rep_sched_;
#endif
  Log_info("ServerWorker shutdown complete.");
}

int ServerWorker::DbChecksum() {
  // auto cs = this->tx_sched_->mdb_txn_mgr_->Checksum();
  uint32_t cs = this->tx_sched_->ChecksumXor();
  Log_info("site_id: {} shard_id: {} checksum: {:x}", (int)this->site_info_->id,
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

// Initialize recovery for replication servers
// @unsafe - Uses LogStorage and filesystem operations
void ServerWorker::InitializeRecovery(uint32_t partition_id, uint32_t locale_id) {
  if (!rep_sched_) {
    return;  // No replication scheduler to recover
  }

  // Create recovery config for this replica
  raft::RecoveryConfig config = raft::RecoveryConfig::for_replica(partition_id, locale_id);

  // Create recovery manager
  raft::RecoveryManager recovery_manager(config);

  // Create storage backend
  auto storage = recovery_manager.create_storage();
  if (!storage) {
    Log_error("Failed to create storage for partition {} replica {}", partition_id, locale_id);
    return;
  }

  // Try to recover Raft server
  if (auto* raft_server = dynamic_cast<RaftServer*>(rep_sched_)) {
    auto result = recovery_manager.recover(
        [raft_server, &storage](std::shared_ptr<janus::raft::LogStorage> s) {
          raft_server->SetLogStorage(s);
        },
        [raft_server]() {
          return raft_server->RecoverFromStorage();
        },
        [&storage](raft::RecoveryResult& r) {
          auto term_opt = storage->get_metadata("currentTerm");
          if (term_opt.is_some()) {
            r.recovered_term = std::stoull(term_opt.unwrap());
          }
        });

    if (!result.success) {
      Log_error("Raft recovery failed for partition {} replica {}: {}",
                partition_id, locale_id, result.error_message.c_str());
    } else {
      Log_info("Raft recovery: partition={} replica={} mode={} entries={} term={} time={}ms",
               partition_id, locale_id, static_cast<int>(result.mode),
               result.recovered_entries, result.recovered_term, result.recovery_time_ms);
    }
    return;
  }

  // Try to recover Paxos server
  if (auto* paxos_server = dynamic_cast<PaxosServer*>(rep_sched_)) {
    auto result = recovery_manager.recover(
        [paxos_server, &storage](std::shared_ptr<janus::raft::LogStorage> s) {
          paxos_server->SetLogStorage(s);
        },
        [paxos_server]() {
          return paxos_server->RecoverFromStorage();
        },
        [&storage](raft::RecoveryResult& r) {
          auto epoch_opt = storage->get_metadata("cur_epoch");
          if (epoch_opt.is_some()) {
            r.recovered_epoch = std::stoull(epoch_opt.unwrap());
          }
        });

    if (!result.success) {
      Log_error("Paxos recovery failed for partition {} replica {}: {}",
                partition_id, locale_id, result.error_message.c_str());
    } else {
      Log_info("Paxos recovery: partition={} replica={} mode={} entries={} epoch={} time={}ms",
               partition_id, locale_id, static_cast<int>(result.mode),
               result.recovered_entries, result.recovered_epoch, result.recovery_time_ms);
    }
    return;
  }

  Log_debug("No Raft or Paxos server found for recovery in partition {} replica {}",
            partition_id, locale_id);
}

} // namespace janus
