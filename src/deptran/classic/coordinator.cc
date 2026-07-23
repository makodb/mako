
/**
 * What shoud we do to change this to asynchronous?
 * 1. Fisrt we need to have a queue to hold all transaction requests.
 * 2. pop next request, send start request for each piece until there is no
 *available left.
 *          in the callback, send the next piece of start request.
 *          if responses to all start requests are collected.
 *              send the finish request
 *                  in callback, remove it from queue.
 *
 */

#include "coordinator.h"
#include "../frame.h"
#include "../benchmark_control_rpc.h"

namespace janus {

CoordinatorClassic::CoordinatorClassic(uint32_t coo_id,
                                       int benchmark,
                                       rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                       uint32_t thread_id)
    : Coordinator(coo_id,
                  benchmark,
                  std::move(client_status),
                  thread_id) {
  verify(commo_ == nullptr);
}

Communicator* CoordinatorClassic::commo() {
  if (commo_ == nullptr) {
    verify(0);
    commo_ = new Communicator;
  }
  verify(commo_ != nullptr);
  return commo_;
}

void CoordinatorClassic::ForwardTxnRequest(TxRequest& req) {
  auto comm = commo();
  comm->SendForwardTxnRequest(
      req,
      this,
      std::bind(&CoordinatorClassic::ForwardTxRequestAck,
                this,
                std::placeholders::_1
      ));
}

void CoordinatorClassic::ForwardTxRequestAck(const TxReply& txn_reply) {
  Log_info("{}: {}", __FUNCTION__, txn_reply.res_);
  committed_ = (txn_reply.res_ == REJECT) ? false : true;
  aborted_ = !committed_;
  phase_ = Phase::COMMIT;
  GotoNextPhase();
}

void CoordinatorClassic::DoTxAsync(TxRequest& req) {
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  // cmd already RWChopper type if RW workload
  // also copy data by (1) ws_init_ = req.input_; (2) ws_ = req.input_;
  TxData* cmd = frame_->CreateTxnCommand(req, txn_reg_);
  verify(txn_reg_ != nullptr);
  cmd->root_id_ = this->next_txn_id();
  cmd->id_ = cmd->root_id_;
  ongoing_tx_id_ = cmd->id_;
  cmd->client_id_ = req.client_id_;
  cmd->cmd_id_in_client_ = req.cmd_id_in_client_;
  Log_debug("assigning tx id: {:x}", ongoing_tx_id_);
  cmd->timestamp_ = GenerateTimestamp();
  cmd_ = cmd;
  n_retry_ = 0;
  Reset(); // In case of reuse.

  Log_debug("do one request txn_id: {}", cmd_->id_);
  auto config = Config::GetConfig();
  bool not_forwarding = forward_status_ != PROCESS_FORWARD_REQUEST;

  if (client_status_.is_some() && not_forwarding) {
    client_status_.as_ref().unwrap()->txn_start_one(thread_id_, cmd->type_);
  }
  if (config->forwarding_enabled_ && forward_status_ == FORWARD_TO_LEADER) {
    Log_info("forward to leader: {}; cooid: {}",
             (int)forward_status_,
             this->coo_id_);
    verify(0); // not supported yet for the new open closed loop.
    ForwardTxnRequest(req);
  } else {
    Log_debug("start txn!!! : {}", (int)forward_status_);
    // this GotoNextPhase is in none/coordinator.cc, coz this is CoordinatorNone instance
    // class CoordinatorNone : public CoordinatorClassic { }
    Fiber::create_run([this]() {
        // Log_info("Start CoroutineID {} {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id);
        GotoNextPhase();
      }, __FILE__, __LINE__
    );
  }
}


void CoordinatorClassic::GotoNextPhase() {
  //Log_info("We're moving along: {}", phase_ % 4);
  int n_phase = 4;
  int current_phase = phase_ % n_phase;
  phase_++;
	bool first = true;
  //Log_info("Current phase is {}", current_phase);
  //Log_info("aborted and committed: {}, {}", aborted_, committed_);
  switch (current_phase) {
    case Phase::INIT_END:
			if (n_retry_ > 0) Log_info("dispatching after restart");
      //Log_info("Dispatching for some reason: {:x}, {}", this, phase_);
      verify(phase_ % n_phase == Phase::DISPATCH);

			/*while(commo()->paused){
				if(first){
					commo()->count_lock_.lock();
					commo()->total_++;
					commo()->qe->n_voted_yes_.set(commo()->qe->n_voted_yes_.get() + 1);
					commo()->count_lock_.unlock();
					Log_info("is it ready: {}", commo()->qe->is_ready());
					commo()->qe->test();
					first = false;
				}
				Log_info("total: {}", commo()->total_);
				auto t = Reactor::create_sp_event<TimeoutEvent>(0.1*1000*1000);
				t->wait_timeout(0.1*1000*1000);
			}*/
			DispatchAsync(true);
      break;
      //break;
    case Phase::DISPATCH:
      //Log_info("Preparing for some reason: {:x}, {}", this, phase_);
      verify(phase_ % n_phase == Phase::PREPARE);
      verify(!committed_);
      if (!aborted_) {
				phase_++;
        Prepare();
      } else {
        phase_++;
        Log_info("Aborting for some reason: {}", n_retry_);
        EarlyAbort();
				break;
      }
      //break;
    case Phase::PREPARE:
      //Log_info("Committing for some reason: {:x}, {}", this, phase_);
      verify(phase_ % n_phase == Phase::COMMIT);
      phase_++;
      Commit();
      //break;
    case Phase::COMMIT:
      verify(phase_ % n_phase == Phase::INIT_END);
      verify(committed_ != aborted_);
      if (committed_){
        //Log_info("Finishing for some reason");
        //phase_++;
        End();
      }
      else if (aborted_) {
        Log_info("Restarting for some reason: {}", n_retry_);
        //phase_++;
        Restart();
      } else
        verify(0);
      break;
    default:
      verify(0);
  }
}

void CoordinatorClassic::Reset() {
  Coordinator::Reset();
  // removed `site_prepare_[i] = 0;` reset
  // loop and `n_prepare_req_ = 0;` write — both fields are gone.
  n_dispatch_ = 0;
  n_dispatch_ack_ = 0;
  n_prepare_ack_ = 0;
  n_finish_req_ = 0;
  n_finish_ack_ = 0;
  dispatch_acks_.clear();
  committed_ = false;
  aborted_ = false;
  // removed `repeat_ = false;` — the
  // `repeat_` field was deleted alongside (always-false dead state).
}

void CoordinatorClassic::Restart() {
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  verify(aborted_);
  n_retry_++;
	verify(n_retry_ < 5);
  cmd_->root_id_ = this->next_txn_id();
  cmd_->id_ = cmd_->root_id_;
  ongoing_tx_id_ = cmd_->root_id_;
  Log_debug("assigning tx_id: {:x}", ongoing_tx_id_);
  TxData* txn = (TxData*) cmd_;
  double last_latency = txn->last_attempt_latency();
  if (client_status_.is_some())
    client_status_.as_ref().unwrap()->txn_retry_one(this->thread_id_, txn->type_, last_latency);
  auto& max_retry = Config::GetConfig()->max_retry_;
  if (n_retry_ > max_retry && max_retry >= 0) {
    if (client_status_.is_some())
      client_status_.as_ref().unwrap()->txn_give_up_one(this->thread_id_, txn->type_);
    End();
  } else {
    Log_info("retry count {}, max_retry: {}, this coord: {}", n_retry_, max_retry, (void*)this);
    Reset();
    txn->Reset();
    //could be a problem or maybe not???
    GotoNextPhase();
  }
}

void CoordinatorClassic::DispatchAsync() {
  Log_debug("commo Broadcast to the server on client worker");
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  auto txn = (TxData*) cmd_;

  int cnt = 0;
  auto n_pd = Config::GetConfig()->n_parallel_dispatch_;
  n_pd = 100;
  ReadyPiecesData cmds_by_par;
  cmds_by_par = txn->GetReadyPiecesData(n_pd); // TODO setting n_pd larger than 1 will cause 2pl to wait forever
  Log_debug("Dispatch for tx_id: {:x}", txn->root_id_);
  for (auto& pair: cmds_by_par) {
    const parid_t& par_id = pair.first;
    auto& cmds = pair.second;
    n_dispatch_ += cmds.size();
    cnt += cmds.size();
    auto sp_vec_piece = std::make_shared<vector<shared_ptr<TxPieceData>>>();
    for (auto c: cmds) {
      c->id_ = next_pie_id();
      dispatch_acks_[c->inn_id_] = false;
      sp_vec_piece->push_back(c);
    }

    commo()->BroadcastDispatch(sp_vec_piece,
                               this,
                               std::bind(&CoordinatorClassic::DispatchAck,
                                         this,
                                         phase_,
                                         -1, 
                                         std::placeholders::_1,
                                         std::placeholders::_2));
  }

  Log_debug("Dispatch cnt: {} for tx_id: {:x}", cnt, txn->root_id_);
}

// removed `CoordinatorClassic::DispatchSync`
// (~36 LOC) — never called externally; was the synchronous twin of
// `DispatchAsync` and the only call site of
// `Communicator::SyncBroadcastDispatch` (left for follow-up).

// not used
void CoordinatorClassic::DispatchAsync(bool last) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  auto txn = (TxData*) cmd_;

  int cnt = 0;
  auto n_pd = Config::GetConfig()->n_parallel_dispatch_;
  n_pd = 100;
  auto cmds_by_par = txn->GetReadyPiecesData(n_pd); // TODO setting n_pd larger than 1 will cause 2pl to wait forever
  Log_debug("Dispatch for tx_id: {:x}", txn->root_id_);
  
  for (auto& pair: cmds_by_par){
    auto& cmds = pair.second;
    n_dispatch_ += cmds.size();
  }
  
  sp_int_event = commo()->BroadcastDispatch(cmds_by_par, this, txn);
  phase_t phase = phase_;

  sp_int_event->wait_timeout(txn_timeout_);

  // Check for timeout
  if (sp_int_event->status_.get() == EventStatus::TIMEOUT) {
    Log_warn("Transaction {}: DispatchAsync timed out after {} us",
             (unsigned long)cmd_->id_, (unsigned long)txn_timeout_);
    aborted_ = true;
    tx_data().reply_.timed_out_ = true;
    tx_data().reply_.res_ = TXN_TIMEOUT;
  }

	debug_cnt--;

  if(phase != phase) verify(0);
  /*if(txn->HasMoreUnsentPiece()){
    DispatchAsync(true);
  }*/if(last && AllDispatchAcked()){
    GotoNextPhase();
  } else if (last && aborted_) {
		GotoNextPhase();
	}
  //Log_debug("Dispatch cnt: {} for tx_id: {:x}", cnt, txn->root_id_);
}

bool CoordinatorClassic::AllDispatchAcked() {
  bool ret1 = std::all_of(dispatch_acks_.begin(),
                          dispatch_acks_.end(),
                          [](std::pair<innid_t, bool> pair) {
                            return pair.second;
                          });
  if (ret1){
    verify(n_dispatch_ack_ == n_dispatch_);
  }
  return ret1;
}

void CoordinatorClassic::DispatchAck(phase_t phase,
                                     double dispatch_time,
                                     int res,
                                     TxnOutput& outputs) {
#ifdef LATENCY_LOG_DEBUG
  Log_info("!!!!!!!!!!!! enter CoordinatorClassic::DispatchAck");
#endif
  //Log_info("Is this being called");
  WAN_WAIT
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  if (dispatch_time > 0 && dispatch_duration_3_times_ > Config::GetConfig()->duration_ * 1000 && dispatch_duration_3_times_ < Config::GetConfig()->duration_ * 2 * 1000) {
    client_worker_->cli2cli_[3].append(SimpleRWCommand::GetCurrentMsTime() - dispatch_time);
  }
  if (phase != phase_) return;
  auto* txn = (TxData*) cmd_;
  if (res == REJECT) {
    aborted_ = true;
    txn->commit_.store(false);
    // Log_info("DispatchAck Reject CoroutineID {} {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id);
    GotoNextPhase();
    return;
  } else if (res == WRONG_LEADER) {
    Log_info("[WRONG_LEADER] DispatchAck received WRONG_LEADER for tx_id: {}", txn->id_);
    aborted_ = true;
    txn->commit_.store(false);
    txn->reply_.res_ = WRONG_LEADER;
    // For None mode, we need to check if we can get view data from the transaction
    // The view data should have been set by the scheduler
    if (txn->reply_.sp_view_data_.is_some()) {
      Log_info("[WRONG_LEADER] DispatchAck has view data: {}",
               txn->reply_.sp_view_data_.as_ref().unwrap()->ToString().c_str());
    }
    GotoNextPhase();
    return;
  }
  n_dispatch_ack_ += outputs.size();
  /*if (aborted_) {
    if (n_dispatch_ack_ == n_dispatch_) {
      GotoNextPhase();
      return;
    }
  }*/

  for (auto& pair : outputs) {
    const innid_t& inn_id = pair.first;
    verify(!dispatch_acks_.at(inn_id));
    dispatch_acks_[inn_id] = true;
    Log_debug("get start ack {}/{} for cmd_id: {:x}, inn_id: {}",
              n_dispatch_ack_, n_dispatch_, cmd_->id_, inn_id);
    txn->Merge(pair.first, pair.second);
  }
  if (txn->HasMoreUnsentPiece()) {
    Log_debug("command has more sub-cmd, cmd_id: {:x},"
                  " n_started_: {}, n_pieces: {}",
              txn->id_, txn->n_pieces_dispatched_, txn->GetNPieceAll());
    DispatchAsync();
  } else if (AllDispatchAcked()) {
    Log_debug("receive all start acks, txn_id: {:x}; START PREPARE",
              txn->id_);
    dispatch_ack_ = true;
    // Log_info("CoordinatorRule coo_id={} thread_id={} cmd_ver_={} cmd_ver={} current_phase={} [End of DispatchAck]", coo_id_, thread_id_, cmd_ver_, cmd_ver, phase % 3);
    if (phase != phase_) {
      // Log_info("AllDispatchAcked Failed CoroutineID {} {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id);
      return;
    }
    // Log_info("AllDispatchAcked Successed CoroutineID {} {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id);
    GotoNextPhase();
  }
}

/** caller should be thread_safe */
void CoordinatorClassic::Prepare() {
  TxData* cmd = (TxData*) cmd_;
  auto mode = Config::GetConfig()->tx_proto_;
  verify(mode == MODE_OCC || mode == MODE_2PL);
   
  std::vector<i32> sids;
  for (auto& site : cmd->partition_ids_) {
    sids.push_back(site);
  }

  Log_info("send prepare tid: {}",
            cmd_->id_);
  auto phase = phase_;
  
  /*commo()->SendPrepare(partition_id,
                         cmd_->id_,
                         sids,
                         std::bind(&CoordinatorClassic::PrepareAck,
                                   this,
                                   phase_,
                                   std::placeholders::_1));*/

  auto quorum_event = commo()->SendPrepare(this,
                                          cmd_->id_,
                                          sids);

	quorum_event->wait_timeout(txn_timeout_);

  // Check for timeout
  if (quorum_event->status_.get() == EventStatus::TIMEOUT) {
    Log_warn("Transaction {}: Prepare timed out after {} us",
             (unsigned long)cmd_->id_, (unsigned long)txn_timeout_);
    aborted_ = true;
    tx_data().reply_.timed_out_ = true;
    tx_data().reply_.res_ = TXN_TIMEOUT;
  }

	//Log_info("slow inside Prepare is: {}", commo()->slow);
  Log_info("DONE send prepare tid: {}",
            cmd_->id_);
  quorum_event->log();

  if(!aborted_){
    cmd->commit_.store(true);
    committed_ = true;
  }
  // removed empty-body `if(repeat_) {}`
  // — `repeat_` was always false (default-init, never set true);
  // the field and this empty branch went away together.
	if (commo()->slow) {
		Log_info("prep_slow");
		prep_slow = true;
	}
	// removed the dead re-elect branch that
	// referenced the now-deleted `commo()->total / window_avg / cpu /
	// last_cpu / low_util / ResetProfiles()` profiling state.  The
	// branch was already commented out (`//if(...)` then nested
	// `/* ... */` blocks); the live `commo()->slow` / `Log_info` /
	// `prep_slow` write above is unaffected.
}

void CoordinatorClassic::PrepareAck(phase_t phase, int res) {
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  if (phase != phase_) return;
  TxData* cmd = (TxData*) cmd_;
  n_prepare_ack_++;

  verify(res == SUCCESS || res == REJECT);
  if (res == REJECT) {
    cmd->commit_.store(false);
    aborted_ = true;
//    Log_fatal("2PL prepare failed due to error {}", e);
  }
  Log_debug("tid {:x}; prepare result {}", (int64_t) cmd_->root_id_, res);

  if (n_prepare_ack_ == cmd->partition_ids_.size()) {
    Log_debug("2PL prepare finished for {}", cmd->root_id_);
    if (!aborted_) {
      cmd->commit_.store(true);
      committed_ = true;
    }
    //GotoNextPhase();
  } else {
    // Do nothing.
  }
}

void CoordinatorClassic::EarlyAbort() {
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  tx_data().reply_.res_ = REJECT;
  for (auto& rp : tx_data().partition_ids_) {
    n_finish_req_++;
    Log_debug("send abort for txn_id {:x} to {}", tx_data().id_, rp);
    commo()->SendEarlyAbort(rp, cmd_->id_);
    // removed `site_abort_[rp]++;` — write-only.
  }
  GotoNextPhase();
}

void CoordinatorClassic::Commit() {
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  auto it = dispatch_acks_.begin();
  it->second = true;
  // removed commented-out
  // `// ___TestPhaseThree(cmd_->id_);` — method deleted.
  auto mode = Config::GetConfig()->tx_proto_;
  verify(mode == MODE_OCC || mode == MODE_2PL);
  Log_debug("send out finish request, cmd_id: {:x}, {}", tx_data().id_, n_finish_req_);

  verify(tx_data().commit_.load() == committed_);
  verify(committed_ != aborted_);

  TxData* cmd = (TxData*) cmd_;
  if (committed_) {
    tx_data().reply_.res_ = SUCCESS;
    auto quorum_event = commo()->SendCommit(this,
                                            tx_data().id_);

		Log_info("send commit tid: {}",
            cmd_->id_);
		quorum_event->wait_timeout(txn_timeout_);

    // Check for timeout (best-effort commit to reachable shards)
    if (quorum_event->status_.get() == EventStatus::TIMEOUT) {
      Log_warn("Transaction {}: Commit timed out after {} us (some shards unreachable)",
               (unsigned long)cmd_->id_, (unsigned long)txn_timeout_);
      // Note: committed to reachable shards, unreachable shards missed
      // Don't change result - transaction was committed (just some shards missed)
      tx_data().reply_.timed_out_ = true;
    }

		Log_info("DONE send commit tid: {}",
            cmd_->id_);
    quorum_event->log();
		
    if(cmd->reply_.res_ == REJECT) {
      aborted_ = true;
    } else if(cmd->reply_.res_ == WRONG_LEADER) {
      // Handle WRONG_LEADER response
      Log_info("[WRONG_LEADER] Coordinator received WRONG_LEADER in Commit phase for tx_id: {}", tx_data().id_);
      aborted_ = true;  // Mark as aborted to clean up
      // The view data should be attached to the TpcCommitCommand by the Raft coordinator
      // It will be propagated to the client through the TxReply
      if (cmd->reply_.sp_view_data_.is_some()) {
        Log_info("[WRONG_LEADER] View data attached to reply: {}",
                 cmd->reply_.sp_view_data_.as_ref().unwrap()->ToString().c_str());
      } else {
        Log_info("[WRONG_LEADER] No view data attached to reply for tx_id: {}", tx_data().id_);
      }
    } else {
      committed_ = true;
    }
    /*for (auto& rp : tx_data().partition_ids_) {
      n_finish_req_++;
      Log_debug("send commit for txn_id {:x} to {}", tx_data().id_, rp);
      commo()->SendCommit(rp,
                          tx_data().id_,
                          std::bind(&CoordinatorClassic::CommitAck,
                                    this,
                                    phase_));
      site_commit_[rp]++;
    }*/
  } else if (aborted_) {
    tx_data().reply_.res_ = REJECT;
    auto quorum_event = commo()->SendAbort(this,
                                           tx_data().id_);
		Log_info("send abort tid: {}",
            cmd_->id_);
    quorum_event->wait_timeout(txn_timeout_);

    // Check for timeout (best-effort abort on reachable shards)
    if (quorum_event->status_.get() == EventStatus::TIMEOUT) {
      Log_warn("Transaction {}: Abort timed out after {} us (some shards unreachable)",
               (unsigned long)cmd_->id_, (unsigned long)txn_timeout_);
      // Note: aborted on reachable shards, unreachable shards missed
      tx_data().reply_.timed_out_ = true;
    }

		Log_info("DONE send abort tid: {}",
            cmd_->id_);
    quorum_event->log();

    if(cmd->reply_.res_ == REJECT) aborted_ = true;
    else committed_ = true;
    /*for (auto& rp : tx_data().partition_ids_) {
      n_finish_req_++;
      Log_debug("send abort for txn_id {:x} to {}", tx_data().id_, rp);
      commo()->SendAbort(rp,
                         cmd_->id_,
                         std::bind(&CoordinatorClassic::CommitAck,
                                   this,
                                   phase_));
      //site_abort_[rp]++;
    }*/
  } else {
    verify(0);
  }
	// removed the `if(false && ...)`
	// short-circuited re-elect branch that referenced the now-deleted
	// `commo()->total` / `window_avg` / `cpu` / `last_cpu` /
	// `low_util` / `ResetProfiles()` profiling state.  The branch
	// was unreachable (`false &&` short-circuits before any of the
	// fields are touched).  The live `prep_slow = false;` at the
	// bottom of the function is unaffected.
	prep_slow = false;
}

void CoordinatorClassic::CommitAck(phase_t phase) {
  std::lock_guard<std::recursive_mutex> lock(this->mtx_);
  // TODO fix bug: when receiving a reply, the coordinator already frees.
  if (phase != phase_) return;
  TxData* cmd = (TxData*) cmd_;
  n_finish_ack_++;
  Log_info("finish cmd_id_: {}; n_finish_ack_: {}; n_finish_req_: {}",
            cmd_->id_, n_finish_ack_, n_finish_req_);
  verify(cmd->GetPartitionIds().size() == n_finish_req_);
  // Perhaps a bug here?
  if (n_finish_ack_ == cmd->GetPartitionIds().size()) {
    if (cmd->reply_.res_ == REJECT) {
      aborted_ = true;
    } else {
      committed_ = true;
    }
    // GotoNextPhase();
  }
  Log_debug("callback: {}, retry: {}",
            committed_ ? "True" : "False",
            aborted_ ? "True" : "False");
}

void CoordinatorClassic::ReportCommit() {
  auto* tx_data = (TxData*) cmd_;
  TxReply& tx_reply_buf = tx_data->get_reply();
  double last_latency = tx_data->last_attempt_latency();
  tx_data->reply_.res_ = SUCCESS;
  this->Report(tx_reply_buf, last_latency);
  commit_reported_ = true;
}

void CoordinatorClassic::End() {
#ifdef LATENCY_LOG_DEBUG
  Log_info("!!!!!!!!! enter CoordinatorClassic::End()");
#endif
  TxData* tx_data = (TxData*) cmd_;
  TxReply& tx_reply_buf = tx_data->get_reply();
  double last_latency = tx_data->last_attempt_latency();
  
  if (committed_) {
    if (!commit_reported_) {
      tx_data->reply_.res_ = SUCCESS;
      this->Report(tx_reply_buf, last_latency
#ifdef TXN_STAT
          , txn
#endif // ifdef TXN_STAT
      );
    }
  } else if (aborted_) {
    // Check if this was actually a WRONG_LEADER case
    if (tx_data->reply_.res_ == WRONG_LEADER) {
      // Keep WRONG_LEADER status (already set in Commit phase)
      Log_info("[WRONG_LEADER] Maintaining WRONG_LEADER status in End() for tx_id: {}", tx_data->id_);
      if (tx_data->reply_.sp_view_data_.is_some()) {
        Log_info("[WRONG_LEADER] View data will be sent to client: {}",
                 tx_data->reply_.sp_view_data_.as_ref().unwrap()->ToString().c_str());
      }
    } else {
      tx_data->reply_.res_ = REJECT;
    }
  } else {
    verify(0);
  }
  tx_reply_buf.tx_id_ = ongoing_tx_id_;
  Log_debug("call reply for tx_id: {:x}", ongoing_tx_id_);
#ifdef FULL_LOG_DEBUG
  Log_info("callback for cmd<{}, {}>", tx_data->client_id_, tx_data->cmd_id_in_client_);
#endif
  tx_data->callback_(tx_reply_buf);
  ongoing_tx_id_ = 0;
  delete tx_data;
}

void CoordinatorClassic::Report(TxReply& txn_reply,
                                double last_latency
#ifdef TXN_STAT
    , TxnChopper *ch
#endif // ifdef TXN_STAT
) {

  bool not_forwarding = forward_status_ != PROCESS_FORWARD_REQUEST;
  if (client_status_.is_some() && not_forwarding) {
    if (txn_reply.res_ == SUCCESS) {
#ifdef TXN_STAT
      txn_stats_[ch->tx_type_].one(ch->proxies_.size(), ch->p_types_);
#endif // ifdef TXN_STAT
      client_status_.as_ref().unwrap()->txn_success_one(thread_id_,
                             txn_reply.txn_type_,
                             txn_reply.start_time_,
                             txn_reply.time_,
                             last_latency,
                             txn_reply.n_try_);
    } else
      client_status_.as_ref().unwrap()->txn_reject_one(thread_id_,
                            txn_reply.txn_type_,
                            txn_reply.start_time_,
                            txn_reply.time_,
                            last_latency,
                            txn_reply.n_try_);
  }
}

// removed `___TestPhaseOne(txnid_t)` and
// `___TestPhaseThree(txnid_t)` test helpers + companion
// `___phase_one_tids_` / `___phase_three_tids_` set fields — only
// references were commented-out call sites in
// `tapir/coordinator.cc:23` and `classic/coordinator.cc:520`.

void CoordinatorClassic::SetNewLeader(parid_t par_id, volatile locid_t* cur_pause) {
  locid_t prev_pause_srv = *cur_pause;
retry:
  Log_debug("start setting a new leader from {}", prev_pause_srv);
  auto e = commo()->BroadcastGetLeader(par_id, prev_pause_srv);
  e->wait();
  if (e->yes()) {
    // assign new leader
    Log_debug("set a new leader {}", e->q().leader_id_.get());
    commo()->SetNewLeaderProxy(par_id, e->q().leader_id_.get());
    if (prev_pause_srv != e->q().leader_id_.get()) {
      *cur_pause = e->q().leader_id_.get();
    }
  } else if (e->no()) {
    auto sp_e = Reactor::create_sp_event<TimeoutEvent>(300 * 1000);
    sp_e->wait();
    // usleep(300 * 1000) ;  // 300 ms
    goto retry;
  } else {
    verify(0);
  }
}

void CoordinatorClassic::FailoverPauseSocketOut(parid_t par_id, locid_t loc_id) {
  Log_info("!!!!!!!!!!! CoordinatorClassic::FailoverPauseSocketOut");
  auto e = commo()->FailoverPauseSocketOut(par_id, loc_id);
  e->wait();
  if (e->no()) {
    verify(0);
  }
};

void CoordinatorClassic::FailoverResumeSocketOut(parid_t par_id, locid_t loc_id) {
  auto e = commo()->FailoverResumeSocketOut(par_id, loc_id);
  e->wait();
  if (e->no()) {
    verify(0);
  }
};

} // namespace janus
