#include <cmath>
#include "client_worker.h"
#include "frame.h"
#include "procedure.h"
#include "coordinator.h"
#include "../bench/rw/workload.h"
#include "benchmark_control_rpc.h"

namespace janus {

ClientWorker::~ClientWorker() {
  // Log_info("~ClientWorker()");
  if (tx_generator_) {
    delete tx_generator_;
  }
  for (auto c : created_coordinators_) {
    delete c;
  }
//  dispatch_pool_->release();

  // Shutdown PollThread if we own it (mako-dev Option pattern)
  if (poll_thread_worker_.is_some()) {
    poll_thread_worker_.as_ref().unwrap()->shutdown();
  }
}

void ClientWorker::retrive_statistic() {
  verify(0); // No longer need since date are recorded directly to ClientWorker instead of Coordinator
}

void ClientWorker::ForwardRequestDone(Coordinator* coo,
                                      TxReply* output,
                                      DeferredReply* defer,
                                      TxReply& txn_reply) {
  verify(coo != nullptr);
  verify(output != nullptr);

  *output = txn_reply;

  bool have_more_time = timer_->elapsed() < duration;
  if (have_more_time && config_->client_type_ == Config::Open) {
    std::lock_guard<std::mutex> lock(coordinator_mutex);
    coo->forward_status_ = NONE;
    free_coordinators_.push_back(coo);
  } else if (!have_more_time) {
    Log_debug("times up. stop.");
    Log_debug("n_concurrent_ = %d", n_concurrent_);
    n_concurrent_--;
    if (n_concurrent_ == 0) {
      Log_debug("all coordinators finished... signal done");
    } else {
      Log_debug("waiting for %d more coordinators to finish", n_concurrent_);
    }
  }

  defer->reply();
}

void ClientWorker::RequestDone(Coordinator* coo, TxReply& txn_reply) {
  verify(0);
  verify(coo != nullptr);

  // Jetpack: Handle WRONG_LEADER response for Raft
  if (txn_reply.res_ == WRONG_LEADER) {
    Log_info("[CLIENT_VIEW] Received WRONG_LEADER response for tx_id: 0x%lx", txn_reply.tx_id_);
    if (txn_reply.sp_view_data_ != nullptr) {
      auto view_data = txn_reply.sp_view_data_;
      Log_info("[CLIENT_VIEW] Extracted view data from response: %s",
               view_data->ToString().c_str());
      commo_->UpdatePartitionView(view_data->partition_id_, view_data);
    } else {
      Log_info("[CLIENT_VIEW] No view data in WRONG_LEADER response for tx_id: 0x%lx", txn_reply.tx_id_);
    }
  }

  if (txn_reply.res_ == SUCCESS)
    success++;
  num_txn++;
  num_try.fetch_add(txn_reply.n_try_);

  bool have_more_time = timer_->elapsed() < duration;
  Log_debug("received callback from tx_id %" PRIx64, txn_reply.tx_id_);
  Log_debug("elapsed: %2.2f; duration: %d", timer_->elapsed(), duration);
  if (have_more_time && config_->client_type_ == Config::Open) {
    std::lock_guard<std::mutex> lock(coordinator_mutex);
    free_coordinators_.push_back(coo);
  } else if (have_more_time && config_->client_type_ == Config::Closed) {
    Log_debug("there is still time to issue another request. continue.");
    Coroutine::CreateRun([this,coo]() {
      DispatchRequest(coo);
    });
  } else if (!have_more_time) {
    Log_debug("times up. stop.");
    Log_debug("n_concurrent_ = %d", n_concurrent_);
    if (coo->offset_ == 0) {
      *failover_server_quit_ = true;
    }
    n_concurrent_--;
    n_pause_concurrent_[coo->coo_id_] = true;
    verify(n_concurrent_ >= 0);
    if (n_concurrent_ == 0) {
      Log_debug("all coordinators finished... signal done");
    } else {
      Log_debug("waiting for %d more coordinators to finish", n_concurrent_);
      Log_debug("transactions they are processing:");
      for (auto c : created_coordinators_) {
        if (c->ongoing_tx_id_ > 0) {
          Log_debug("\t %" PRIx64, c->ongoing_tx_id_);
        }
      }
    }
  } else {
    verify(0);
  }
}

Coordinator* ClientWorker::FindOrCreateCoordinator() {
  std::lock_guard<std::mutex> lock(coordinator_mutex);

  Coordinator* coo = nullptr;

  if (!free_coordinators_.empty()) {
    coo = dynamic_cast<Coordinator*>(free_coordinators_.back());
    free_coordinators_.pop_back();
  } else {
    if (created_coordinators_.size() == UINT16_MAX) {
      return nullptr;
    }
    verify(created_coordinators_.size() <= UINT16_MAX);
    coo = CreateCoordinator(created_coordinators_.size());
  }

  verify(!coo->_inuse_);
  coo->_inuse_ = true;
  return coo;
}

Coordinator* ClientWorker::CreateFailCtrlCoordinator() {
  cooid_t coo_id = cli_id_;
  uint64_t offset_id = 1000000; // TODO temp value
  coo_id = (coo_id << 16) + offset_id;
  auto coo = frame_->CreateCoordinator(coo_id,
                                       config_,
                                       benchmark,
                                       ccsi,
                                       id,
                                       txn_reg_);
  coo->frame_ = frame_;
  coo->loc_id_ = my_site_.locale_id;
  coo->commo_ = commo_;
  coo->forward_status_ = forward_requests_to_leader_ ? FORWARD_TO_LEADER : NONE;
  coo->offset_ = offset_id;
  Log_debug("coordinator %d created at site %d: forward %d",
            coo->coo_id_,
            this->my_site_.id,
            coo->forward_status_);
  return coo;
}

Coordinator* ClientWorker::CreateCoordinator(uint16_t offset_id) {
  cooid_t coo_id = cli_id_;
  coo_id = (coo_id << 16) + offset_id;
  auto coo = frame_->CreateCoordinator(coo_id,
                                       config_,
                                       benchmark,
                                       ccsi,
                                       id,
                                       txn_reg_);
  coo->frame_ = frame_;
  coo->loc_id_ = my_site_.locale_id;
  coo->commo_ = commo_;
  coo->forward_status_ = forward_requests_to_leader_ ? FORWARD_TO_LEADER : NONE;
  coo->offset_ = offset_id;
  coo->client_worker_ = this;
  Log_debug("coordinator %d created at site %d: forward %d",
            coo->coo_id_,
            this->my_site_.id,
            coo->forward_status_);
  created_coordinators_.push_back(coo);
  n_pause_concurrent_[coo_id] = false;
  return coo;
}

void ClientWorker::Work() {
  auto work_start_time = std::chrono::steady_clock::now();
  Log_debug("%s: %d", __FUNCTION__, this->cli_id_);
  txn_reg_ = std::make_shared<TxnRegistry>();
  verify(config_ != nullptr);
  Workload* workload = Workload::CreateWorkload(config_);
  workload->txn_reg_ = txn_reg_;
  workload->RegisterPrecedures();

  commo_->WaitConnectClientLeaders();
  if (ccsi) {
    ccsi->wait_for_start(id);
  }
  Log_debug("after wait for start");

  // Jetpack: Failover testing job
  bool failover = Config::GetConfig()->get_failover();
  if (failover) {
    auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([this]() {
      int run_int = Config::GetConfig()->get_failover_run_interval() * pow(10, 6);
      int stop_int = Config::GetConfig()->get_failover_stop_interval() * pow(10, 6);
      int wait_int = 50 * pow(10, 3);

      if (!fail_ctrl_coo_) {
        fail_ctrl_coo_ = CreateFailCtrlCoordinator();
      }
      locid_t idx = 0;
      while (!*failover_server_quit_) {
        auto r = Reactor::CreateSpEvent<NeverEvent>();
        r->Wait(run_int);
        *failover_trigger_ = true;
        while (*failover_trigger_) {
          auto e = Reactor::CreateSpEvent<NeverEvent>();
          e->Wait(wait_int);
          if (*failover_server_quit_) return;
        }
        Pause(idx);
        *failover_trigger_ = true;
        Log_info("server %d paused for failover test", idx);
        auto s = Reactor::CreateSpEvent<NeverEvent>();
        s->Wait(stop_int);
        while (*failover_trigger_) {
          auto e = Reactor::CreateSpEvent<NeverEvent>();
          e->Wait(wait_int);
          if (*failover_server_quit_) return;
        }
        Resume(idx);
        Log_info("server %d resumed for failover test", idx);
        idx = cur_leader_;
      }
    }));
    auto arc_job_base = rusty::Arc<Job>(arc_job);
    poll_thread_worker_.as_ref().unwrap()->add(arc_job_base);
  }

  // Jetpack: Main transaction dispatch loop
  for (uint32_t n_tx = 0; n_tx < n_concurrent_; n_tx++) {
    auto arc_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([this, n_tx] () {
      auto beg_time = Time::now();
      auto end_time = beg_time + duration * pow(10, 6);
#ifdef DB_CHECKSUM
      auto read_end_time = end_time + 20 * pow(10, 6);
#endif
#ifdef COPILOT_DEBUG
      end_time = beg_time + duration * 5 * pow(10, 2);
#endif
      while (true) {
        auto cur_time = Time::now();
#ifndef DB_CHECKSUM
        if (cur_time > end_time) {
#endif
#ifdef DB_CHECKSUM
        if (cur_time > read_end_time) {
#endif
          break;
        }
        n_tx_issued_++;
        while (true) {
          auto n_undone_tx = n_tx_issued_ - sp_n_tx_done_.value_;
          if (n_undone_tx % 1000 == 0) {
            Log_debug("unfinished tx %d", n_undone_tx);
          }
          if (config_->client_max_undone_ > 0
              && n_undone_tx > config_->client_max_undone_) {
            Reactor::CreateSpEvent<NeverEvent>()->Wait(pow(10, 4));
          } else {
            break;
          }
        }
        num_txn++;
        auto coo = FindOrCreateCoordinator();
        coo->cli_id_ = cli_id_;
        verify(!coo->sp_ev_commit_);
        verify(!coo->sp_ev_done_);
        coo->sp_ev_commit_ = Reactor::CreateSpEvent<IntEvent>();
        coo->sp_ev_done_ = Reactor::CreateSpEvent<IntEvent>();

        Log_debug("Dispatching request for %d", n_tx);
        this->outbound++;

        bool first = true;
        while(coo->commo_->paused) {
          if(first) {
            coo->commo_->count_lock_.lock();
            coo->commo_->total_ = this->outbound;
            coo->commo_->qe->n_voted_yes_ = this->outbound;
            coo->commo_->count_lock_.unlock();
            Log_info("is it ready: %d", coo->commo_->qe->IsReady());
            coo->commo_->qe->Test();
            first = false;
          }
          Log_info("total: %d", coo->commo_->total_);
          auto t = Reactor::CreateSpEvent<TimeoutEvent>(0.1*1000*1000);
          t->Wait();
        }
#ifdef DB_CHECKSUM
        if (cur_time > end_time)
          this->DispatchRequest(coo, true);
        else
#endif
          this->DispatchRequest(coo);
        if (config_->client_type_ == Config::Closed) {
          auto ev = coo->sp_ev_commit_;
#if 1
          char txid[20];
          sprintf(txid, "%" PRIx64 "|", coo->ongoing_tx_id_);
          ev->wait_place_ = std::string(txid);
#endif
          Wait_recordplace(ev, Wait(600*1000*1000));
          this->outbound--;
          verify(ev->status_ != Event::TIMEOUT);
        } else {
          auto sp_event = Reactor::CreateSpEvent<NeverEvent>();
          Wait_recordplace(sp_event, Wait(pow(10, 6)));
        }
        Coroutine::CreateRun([this, coo](){
          verify(coo->_inuse_);
          auto ev = coo->sp_ev_done_;
          Wait_recordplace(ev, Wait());
          verify(coo->coo_id_ > 0);
          verify(coo->_inuse_);
          verify(coo->coo_id_ > 0);
          verify(ev->status_ != Event::TIMEOUT);
          if (coo->committed_) {
            success++;
          }
          sp_n_tx_done_.Set(sp_n_tx_done_.value_+1);
          num_try.fetch_add(coo->n_retry_);
          coo->sp_ev_done_.reset();
          coo->sp_ev_commit_.reset();
          free_coordinators_.push_back(coo);
          coo->_inuse_ = false;
          n_pause_concurrent_[coo->coo_id_] = true;
        }, __FILE__, __LINE__);
      }
      n_ceased_client_.Set(n_ceased_client_.value_+1);
    }));
    auto arc_job_base = rusty::Arc<Job>(arc_job);
    poll_thread_worker_.as_ref().unwrap()->add(arc_job_base);
  }

  // Jetpack: Wait for completion job
  auto finish_job = rusty::Arc<OneTimeJob>::new_(OneTimeJob([this](){
    Log_info("wait for all virtual clients to stop issuing new requests.");
    n_ceased_client_.WaitUntilGreaterOrEqualThan(n_concurrent_,
                                                 (duration+500)*1000000);
    Log_info("wait for all outstanding requests to finish.");
    sp_n_tx_done_.WaitUntilGreaterOrEqualThan(n_tx_issued_);
    *failover_server_quit_ = true;
    all_done_ = 1;
  }));
  auto finish_job_base = rusty::Arc<Job>(finish_job);
  poll_thread_worker_.as_ref().unwrap()->add(finish_job_base);

  // Force stop if there is no progress in 5 seconds
  int prev_done = 0;
  while (all_done_ == 0) {
    auto current_time = std::chrono::steady_clock::now();
    auto elapsed_time = std::chrono::duration_cast<std::chrono::seconds>(current_time - work_start_time).count();
    if (prev_done == (int) sp_n_tx_done_.value_ && (prev_done > 0 || elapsed_time > 30)) {
      break;
    }
    prev_done = (int) sp_n_tx_done_.value_;
    Log_info("wait for finish... n_ceased_clients: %d,"
              "n_issued: %d, n_done: %d, n_created_coordinator: %d, client_id: %d",
              (int) n_ceased_client_.value_, (int) n_tx_issued_,
              (int) sp_n_tx_done_.value_, (int) created_coordinators_.size(), cli_id_);
    sleep(5);
  }

  if (failover_server_quit_ && !*failover_server_quit_) {
    *failover_server_quit_ = true;
  }

  Log_info("Finish:\nTotal: %u, Commit: %u, Attempts: %u, Running for %u, Throughput: %.2f, Client_id: %d\n",
           num_txn.load(),
           success.load(),
           num_try.load(),
           Config::GetConfig()->get_duration(),
           static_cast<float>(num_txn.load()) / Config::GetConfig()->get_duration(),
           cli_id_);
  *total_throughput_ += static_cast<float>(num_txn.load()) / Config::GetConfig()->get_duration();
  fflush(stderr);
  fflush(stdout);

  if (ccsi) {
    Log_info("%s: wait_for_shutdown at client %d", __FUNCTION__, cli_id_);
    ccsi->wait_for_shutdown();
  }
  delete timer_;
  return;
}

void ClientWorker::AcceptForwardedRequest(TxRequest& request,
                                          TxReply* txn_reply,
                                          rrr::DeferredReply* defer) {
  const char* f = __FUNCTION__;

  Coordinator* coo = nullptr;
  while (coo == nullptr) {
    coo = FindOrCreateCoordinator();
  }
  coo->forward_status_ = PROCESS_FORWARD_REQUEST;

  std::function<void()> task = [=]() {
    TxRequest req(request);
    req.callback_ = std::bind(&ClientWorker::ForwardRequestDone,
                              this,
                              coo,
                              txn_reply,
                              defer,
                              std::placeholders::_1);
    Log_debug("%s: running forwarded request at site %d", f, my_site_.id);
    coo->concurrent = n_concurrent_;
    coo->DoTxAsync(req);
  };
  task();
}

void ClientWorker::FailoverPreprocess(Coordinator* coo) {
  if (*failover_trigger_ || failover_trigger_loc) {
    if (coo->offset_ == 0) {
      failover_wait_leader_ = true;
    }
    n_pause_concurrent_[coo->coo_id_] = true;

    if (coo->offset_ == 0) {
      failover_pause_start = false;
      for (auto it = n_pause_concurrent_.begin(); it != n_pause_concurrent_.end(); it++) {
        while (!it->second) {
          auto sp_e = Reactor::CreateSpEvent<TimeoutEvent>(300 * 1000);
          sp_e->Wait();
        }
      }
      failover_pause_start = true;
    } else {
      while (!failover_pause_start) {
        auto sp_e = Reactor::CreateSpEvent<TimeoutEvent>(300 * 1000);
        sp_e->Wait();
      }
    }

    Log_debug("client worker start dispatch request pause: %d with cur leader %d",
        coo->coo_id_, cur_leader_);
    if (coo->offset_ == 0) {
      failover_trigger_loc = true;
      *failover_trigger_ = false;
    }
    while (!*failover_trigger_) {
      auto sp_e = Reactor::CreateSpEvent<TimeoutEvent>(300 * 1000);
      sp_e->Wait();
      if (*failover_server_quit_) break;
    }
    if (coo->offset_ == 0) {
      SearchLeader(coo);
      *failover_trigger_ = false;
      failover_trigger_loc = false;
      failover_pause_start = false;
      failover_wait_leader_ = false;
    } else {
      while (failover_wait_leader_ && !*failover_server_quit_) {
        auto sp_e = Reactor::CreateSpEvent<TimeoutEvent>(500 * 1000);
        sp_e->Wait();
      }
    }
    n_pause_concurrent_[coo->coo_id_] = false;
    Log_debug("client worker end dispatch request pause: %d with cur leader %d",
        coo->coo_id_, cur_leader_);
  }
}

void ClientWorker::DispatchRequest(Coordinator* coo, bool void_request) {
  const char* f = __FUNCTION__;
  std::function<void()> task = [=]() {
    Log_debug("%s: %d", f, cli_id_);
    TxRequest *req = new TxRequest;
    {
      std::lock_guard<std::mutex> lock(this->request_gen_mutex);
      tx_generator_->GetTxRequest(req, coo->coo_id_);
      req->client_id_ = coo->coo_id_;
      req->cmd_id_in_client_ = coo->cmd_in_client_count++;
#ifdef DB_CHECKSUM
      if (void_request)
        req->tx_type_ = RW_BENCHMARK_R_TXN;
#endif
    }
    req->callback_ = [coo, req, this] (TxReply& reply) {
      // Jetpack: Handle WRONG_LEADER response for Raft
      if (reply.res_ == WRONG_LEADER) {
        Log_info("[CLIENT_VIEW] Received WRONG_LEADER response for tx_id: 0x%lx", reply.tx_id_);
        if (reply.sp_view_data_ != nullptr) {
          auto view_data = reply.sp_view_data_;
          Log_info("[CLIENT_VIEW] Extracted view data from response: %s",
                   view_data->ToString().c_str());
          commo_->UpdatePartitionView(view_data->partition_id_, view_data);
        } else {
          Log_info("[CLIENT_VIEW] No view data in WRONG_LEADER response for tx_id: 0x%lx", reply.tx_id_);
        }
      }

      coo->sp_ev_commit_->Set(1);
      auto& status = coo->sp_ev_done_->status_;
      verify(status == Event::WAIT || status == Event::INIT);
      coo->sp_ev_done_->Set(1);
      delete req;
    };
    coo->DoTxAsync(*req);
  };
  task();
}

void ClientWorker::SearchLeader(Coordinator* coo) {
  parid_t par_id = 0;
  coo->SetNewLeader(par_id, failover_server_idx_);
  cur_leader_ = *failover_server_idx_;
  Log_debug("client %d set cur_leader_ %d failover_server_idx_ %d", cli_id_, cur_leader_,
      *failover_server_idx_);
}

// Merged constructor: Jetpack failover params + mako-dev PollThread type
ClientWorker::ClientWorker(
    uint32_t id,
    Config::SiteInfo& site_info,
    Config* config,
    ClientControlServiceImpl* ccsi,
    rusty::Option<rusty::Arc<PollThread>> poll_thread_worker,
    bool* volatile failover_trigger,
    volatile bool* failover_server_quit,
    volatile locid_t* failover_server_idx,
    volatile double* total_throughput) :
    id(id),
    my_site_(site_info),
    config_(config),
    cli_id_(site_info.id),
    benchmark(config->benchmark()),
    mode(config->get_mode()),
    duration(config->get_duration()),
    ccsi(ccsi),
    n_concurrent_(config->get_concurrent_txn()),
    failover_trigger_(failover_trigger),
    failover_server_quit_(failover_server_quit),
    failover_server_idx_(failover_server_idx),
    total_throughput_(total_throughput) {
  Log_info("[Jetpack] launch ClientWorker %d site_info is id=%d locale_id=%d name=%s proc_name=%s host=%s port=%d n_thread=%d partition_id_=%d",
           id, site_info.id, site_info.locale_id, site_info.name.c_str(), site_info.proc_name.c_str(),
           site_info.host.c_str(), site_info.port, site_info.n_thread, site_info.partition_id_);
  // Merged: Use mako-dev's Option<Arc<PollThread>> pattern
  poll_thread_worker_ = poll_thread_worker.is_some() ? std::move(poll_thread_worker) : rusty::Some(PollThread::create());
  // Jetpack: 2-param GetFrame (needs replica_proto_ for Raft)
  frame_ = Frame::GetFrame(config->tx_proto_, config->replica_proto_);
  tx_generator_ = frame_->CreateTxGenerator();
  config->get_all_site_addr(servers_);
  num_txn.store(0);
  success.store(0);
  num_try.store(0);
  commo_ = frame_->CreateCommo(rusty::Some(poll_thread_worker_.as_ref().unwrap().clone()));
  commo_->loc_id_ = my_site_.locale_id;

  // Jetpack: Set up dynamic leader callback for the communicator
  commo_->SetLeaderCallback([this](parid_t par_id) {
    locid_t leader = commo_->GetLeaderForPartition(par_id);
    return leader;
  });

  // Jetpack: MODE_RAFT and MODE_FPGA_RAFT for leader forwarding
  forward_requests_to_leader_ =
      ((config->replica_proto_ == MODE_RAFT || config->replica_proto_ == MODE_FPGA_RAFT) && site_info.locale_id != 0) ? true :
                                                                               false;
  Log_debug("client %d created; forward %d",
            cli_id_,
            forward_requests_to_leader_);
}

void ClientWorker::Pause(locid_t locid) {
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! ClientWorker::Pause %d", locid);
#endif
  fail_ctrl_coo_->FailoverPauseSocketOut(0, locid);
}

void ClientWorker::Resume(locid_t locid) {
  fail_ctrl_coo_->FailoverResumeSocketOut(0, locid);
}

} // namespace janus
