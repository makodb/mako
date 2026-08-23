#include "../constants.h"
#include "../tx.h"
#include "../procedure.h"
#include "../coordinator.h"
#include "rrr/misc/serializable.hpp"  // serializable_cast
#include "scheduler.h"
#include "tpc_command.h"
#include "tx.h"


namespace janus {

void SchedulerClassic::MergeCommands(vector<shared_ptr<TxPieceData>>& ops,
                                     const janus::Command& cmd2) {

  verify(0);
//  auto& sp_v2 = marshallable_cast<VecPieceData>(cmd2)->sp_vec_piece_data_;
//  verify(sp_v2);
//  for (auto& cmd: *sp_v2) {
//    verify(std::all_of(sp_v1->begin(), sp_v1->end(), [&cmd] (TxPieceData& d) {
//      return (cmd.root_id_ == d.root_id_) && (cmd.inn_id_ != d.inn_id_);
//    }));
//    sp_v1->push_back(cmd);
//  }
}

bool SchedulerClassic::ExecutePiece(Tx& tx,
                                    TxPieceData& piece_data,
                                    TxnOutput& ret_output) {
  auto roottype = piece_data.root_type_;
  auto subtype = piece_data.type_;
  TxnPieceDef& piece_def = txn_reg_->get(roottype, subtype);
  int ret_code;
  auto& conflicts = piece_def.conflicts_;
  piece_data.input.Aggregate(tx.ws_);
// TODO enable this verify
  piece_data.input.VerifyReady();
  piece_def.proc_handler_(tx,
                          piece_data,
                          &ret_code,
                          ret_output[piece_data.inn_id()]);
  tx.ws_.insert(ret_output[piece_data.inn_id()]);
  return true;
}

bool SchedulerClassic::DispatchPiece(Tx& tx,
                                     TxPieceData& piece_data,
                                     TxnOutput& ret_output) {
  TxnPieceDef
      & piece_def = txn_reg_->get(piece_data.root_type_, piece_data.type_);
  auto& conflicts = piece_def.conflicts_;
//  auto id = piece_data.inn_id();
  // Two phase locking won't pass these
//  verify(!tx.inuse);
//  tx.inuse = true;

	/*struct timespec begin, end;
	clock_gettime(CLOCK_MONOTONIC, &begin);*/
  for (auto& c: conflicts) {
    vector<Value> pkeys;
    for (int i = 0; i < c.primary_keys.size(); i++) {
      pkeys.push_back(piece_data.input.at(c.primary_keys[i]));
    }
    auto row = tx.Query(tx.GetTable(c.table), pkeys, c.row_context_id);
    verify(row != nullptr);
    for (auto col_id : c.columns) {
      if (!Guard(tx, row, col_id)) {
        tx.inuse = false;
//        auto reactor = Reactor::get_reactor();
//        auto sz = reactor->fibers_.size();
//        verify(sz > 0);
        auto id = piece_data.inn_id();
        ret_output[id] = {}; // the client uses this to identify ack.
        return false; // abort
      }
    }
  }
	/*clock_gettime(CLOCK_MONOTONIC, &end);
	Log_info("time of dispatch: {}", end.tv_nsec-begin.tv_nsec);*/
//  tx.inuse = false;
  return true;
}

bool SchedulerClassic::Dispatch(cmdid_t cmd_id,
                                struct DepId dep_id,
                                const janus::Command& cmd_env,
                                TxnOutput& ret_output) {
#ifdef FULL_LOG_DEBUG
  Log_info("cmd<{}, {}> entered SchedulerClassic::Dispatch", SimpleRWCommand::GetCmdID(cmd_env).first, SimpleRWCommand::GetCmdID(cmd_env).second);
#endif

  const auto vec_piece_data = marshallable_cast<VecPieceData>(cmd_env);
  verify(vec_piece_data.is_some());
  auto sp_vec_piece = vec_piece_data.unwrap()->sp_vec_piece_data_;
  verify(sp_vec_piece);
  // auto tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(cmd_id));
  auto tx = dynamic_pointer_cast<TxClassic>(GetTx(cmd_id));
  verify(tx != nullptr);
//  MergeCommands(tx.cmd_, cmd);
  Log_debug("{}: received dispatch for tx id: {:x}", site_id_, tx->tid_);
//  verify(partition_id_ == piece_data.partition_id_);
  // pre-proces
  // TODO separate pre-process and process/commit
  // TODO support user-customized pre-process.
// for debug purpose
//  bool b1 = false, b2 = false;
//  for (auto& piece_data : *sp_vec_piece) {
//    if (piece_data.inn_id_ == 200) b1 = true;
//    if (piece_data.inn_id_ == 205) b2 = true;
//  }
//  verify(b1 == b2);
  verify(cmd_env.has_value());
  // 2 step 1: identity check via Command::operator==.
  if (!tx->cmd_.has_value()) {
    tx->cmd_ = cmd_env;
  } else if (tx->cmd_ != cmd_env) {
    const auto present_vec_piece_data =
        marshallable_cast<VecPieceData>(tx->cmd_);
    verify(present_vec_piece_data.is_some());
    auto present_cmd = present_vec_piece_data.unwrap()->sp_vec_piece_data_;
    verify(present_cmd);
    for (auto& sp_piece_data : *sp_vec_piece) {
      present_cmd->push_back(sp_piece_data);
    }
  } else {
    // do nothing
//    verify(0);
  }

	struct timespec begin, end;
	//clock_gettime(CLOCK_MONOTONIC, &begin);
  bool ret = true;
  for (const auto& sp_piece_data : *sp_vec_piece) {
    verify(sp_piece_data);
    ret = DispatchPiece(*tx, *sp_piece_data, ret_output);
    if (!ret) {
      break;
    }
  }
	/*clock_gettime(CLOCK_MONOTONIC, &end);
	Log_info("time of dispatch2: {}", end.tv_nsec-begin.tv_nsec);*/
  // TODO reimplement this.
  if (tx->fully_dispatched_->value_.get() == 0) {
    tx->fully_dispatched_->set(1);
  }
  return ret;
}

// On prepare with replication
//   1. dispatch the whole transaction to others.
//   2. use a paxos command to commit the prepare request.
//   3. after that, run the function to prepare.
//   0. an non-optimized version would be.
//      dispatch the transaction command with paxos instance
bool SchedulerClassic::OnPrepare(cmdid_t tx_id,
                                 const std::vector<i32>& sids,
                                 struct DepId dep_id,
																 bool& null_cmd) {
  auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(tx_id));
  verify(sp_tx);
	/*if(sp_tx->cmd_ == NULL){
		null_cmd = true;
		return false;
	}*/
  Log_debug("{}: at site {}, tx: {:x}", __FUNCTION__, this->site_id_, tx_id);
  if (Config::GetConfig()->IsReplicated()) {
    // fill the payload on a LOCAL, then freeze it into a shared Arc —
    // rusty::Arc payloads are const-view after construction.
    TpcPrepareCommand prepare_cmd_local{};
    // dropped tautological `kMarshallKind == static_kind()` verify.
    prepare_cmd_local.tx_id_ = tx_id;
    prepare_cmd_local.cmd_ = sp_tx->cmd_;
    auto sp_prepare_cmd =
        rusty::Arc<TpcPrepareCommand>::make(std::move(prepare_cmd_local));
    sp_tx->is_leader_hint_ = true;
		
		struct timespec begin, end;
		//clock_gettime(CLOCK_MONOTONIC, &begin);
    //Log_info("This is dep_id: {}", dep_id);
    // here, we need to let the paxos coordinator know what request we are working with
    // thsi could be the transaction id or we can add a new id
    auto coo = CreateRepCoord(dep_id.id);
		
		/*clock_gettime(CLOCK_MONOTONIC, &end);
		Log_info("time of prepare on server: {}", end.tv_nsec-begin.tv_nsec);*/
    //Log_info("The locale id: {}", coo->loc_id_);
    coo->Submit(std::move(sp_prepare_cmd));
    sp_tx->prepare_result->wait();
		slow_ = coo->slow_;
//    Log_debug("finished prepare command replication");
    return sp_tx->prepare_result->get();
  } else {
    // collapsed `else if (do_logging()) {
    // string log; get_prepare_log(tx_id, sids, &log); }` branch into
    // the else — the disk-logging path was a no-op (built `log` and
    // discarded it; `Recorder::submit` was already commented out and
    // the field is gone since Phases 4e-33..4e-36).  `do_logging`,
    // `get_prepare_log`, and `Config::log_path()` also removed in
    // this phase.
    return DoPrepare(tx_id);
  }
}

int SchedulerClassic::PrepareReplicated(TpcPrepareCommand& prepare_cmd) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // TODO verify it is the same leader, error if not.
  // TODO and return the prepare callback here.
  auto tx_id = prepare_cmd.tx_id_;
  auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(tx_id));
  // prepare_cmd.cmd_ and sp_tx->cmd_ are both Command;
  // direct assignment.
  if (!sp_tx->cmd_.has_value())
    sp_tx->cmd_ = prepare_cmd.cmd_;
  if (!sp_tx->is_leader_hint_) {
    return 0;
  }
  // else: is the leader.
  sp_tx->prepare_result->set(DoPrepare(sp_tx->tid_));
  Log_debug("prepare request replicated and executed for {:x}, result: {:x}, sid: {:x}",
      sp_tx->tid_, sp_tx->prepare_result->get(), (int)this->site_id_);
  Log_debug("triggering prepare replication callback {:x}", sp_tx->tid_);
  return 0;
}

int SchedulerClassic::OnEarlyAbort(txnid_t tx_id) {
  auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(tx_id));
  DoAbort(*sp_tx);
  return 0;
}

int SchedulerClassic::OnCommit(txnid_t tx_id,
															 struct DepId dep_id,
															 int commit_or_abort) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("{}: at site {}, tx: {:x}",
            __FUNCTION__, this->site_id_, tx_id);
  Log_debug("Coordinator invokes Submit to submit a request to a specific protocol");
  // auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(tx_id));
  auto sp_tx = dynamic_pointer_cast<TxClassic>(GetTx(tx_id));
  verify(sp_tx != nullptr);

  // TODO maybe change inuse to an event?
//  verify(!sp_tx->inuse);
//  sp_tx->inuse = true;
//
  //always true
#ifdef FULL_LOG_DEBUG
  // GetCmdID still takes shared_ptr<Marshallable>.
  Log_info("cmd<{}, {}> entered SchedulerClassic::OnCommit, Config::GetConfig()->IsReplicated()={}",
    SimpleRWCommand::GetCmdID(sp_tx->cmd_).first, SimpleRWCommand::GetCmdID(sp_tx->cmd_).second, Config::GetConfig()->IsReplicated());
#endif
  if (Config::GetConfig()->IsReplicated()) {
    // fill the payload on a LOCAL, then freeze it into the shared Arc
    // BEFORE Submit — the replication coordinator writes WRONG_LEADER
    // back through this same shared payload, and the `cmd->ret_` read
    // below must observe it through the SAME object.
    TpcCommitCommand commit_cmd_local{};
    commit_cmd_local.tx_id_ = tx_id;
    commit_cmd_local.ret_ = commit_or_abort;
    commit_cmd_local.cmd_ = sp_tx->cmd_;
    auto cmd = rusty::Arc<TpcCommitCommand>::make(std::move(commit_cmd_local));
    sp_tx->is_leader_hint_ = true;
    shared_ptr<Coordinator> coo{CreateRepCoord(dep_id.id)};
    coo->svr_workers_g = svr_workers_g;

    const auto commit_vec_piece = marshallable_cast<VecPieceData>(cmd->cmd_);
    verify(commit_vec_piece.is_some());
    double client_ms = commit_vec_piece.unwrap()->time_sent_from_client_;
    struct timeval tp;
    gettimeofday(&tp, NULL);
    double start_ms = tp.tv_sec * 1000 + tp.tv_usec / 1000.0;
    cli2tx.append(start_ms - client_ms);

    // Coordinator::Submit takes Command (prep6o);
    // 2 step 4: rusty::Arc<TpcCommitCommand> auto-converts
    // through Command's templated Arc<T> ctor; clone() keeps `cmd`
    // alive for the post-Submit ret_ readback.
    coo->Submit(cmd.clone());
    
    sp_tx->commit_result->wait();

    // Check if Submit failed due to WRONG_LEADER
    if (cmd->ret_ == WRONG_LEADER)
      return WRONG_LEADER;

    gettimeofday(&tp, NULL);
    double finish_ms = tp.tv_sec * 1000 + tp.tv_usec / 1000.0;
    tx2tx.append(finish_ms - start_ms);
    
		slow_ = coo->slow_;
  } else {
    if (commit_or_abort == SUCCESS) {
      DoCommit(*sp_tx);
    } else if (commit_or_abort == REJECT) {
      DoAbort(*sp_tx);
    } else {
      verify(0);
    }
  }
  return 0;
}

void SchedulerClassic::DoCommit(Tx& tx_box) {
#ifdef DB_CHECKSUM
  // ApplyToDatabase now takes Command directly.
  ApplyToDatabase(tx_box.cmd_);
#endif
  auto mdb_txn = RemoveMTxn(tx_box.tid_);
  verify(mdb_txn == tx_box.mdb_txn_);
  mdb_txn->commit();
  tx_box.mdb_txn_ = nullptr;
  delete mdb_txn; // TODO remove this
}

void SchedulerClassic::DoAbort(Tx& tx_box) {
  auto mdb_txn = RemoveMTxn(tx_box.tid_);
  verify(mdb_txn == tx_box.mdb_txn_);
  mdb_txn->abort();
  delete mdb_txn; // TODO remove this
  tx_box.mdb_txn_ = nullptr;
}

int SchedulerClassic::CommitReplicated(TpcCommitCommand& tpc_commit_cmd) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  auto tx_id = tpc_commit_cmd.tx_id_;
  // Log_info("[EXECUTION] CommitReplicated called for tx_id: {} (This is actual execution)", tx_id);
  auto sp_tx = dynamic_pointer_cast<TxClassic>(GetOrCreateTx(tx_id));
  // Replicated logs can deliver a command more than once. Keep commit
  // application idempotent after the first result becomes visible.
  if (sp_tx->commit_result->is_ready())
    return 0;
  int commit_or_abort = tpc_commit_cmd.ret_;
  // both fields are Command; direct assignment.
  if (!sp_tx->cmd_.has_value())
    sp_tx->cmd_ = tpc_commit_cmd.cmd_;
  if (!sp_tx->is_leader_hint_) {
    if (commit_or_abort == REJECT) {
      sp_tx->commit_result->set(1);
      return 0;
    } else {
      verify(sp_tx->cmd_.has_value());
      unique_ptr<TxnOutput> out = std::make_unique<TxnOutput>();
			DepId di = { "dep", 0 };
      // Dispatch now takes janus::Command directly.
      SchedulerClassic::Dispatch(sp_tx->tid_, di, sp_tx->cmd_, *out);
      DoPrepare(sp_tx->tid_);
    }
  }
  if (commit_or_abort == SUCCESS) {
    // Log_info("[SUCCESS] Scheduler received SUCCESS for tx_id: {}", tx_id);
    sp_tx->committed_ = true;
    DoCommit(*sp_tx);
    // Track recovered transactions
    if (in_state_machine_recovery_) {
      transactions_recovered_++;
    }
  } else if (commit_or_abort == REJECT) {
    Log_info("[REJECT] Scheduler received REJECT for tx_id: {}", tx_id);
    sp_tx->aborted_ = true;
    DoAbort(*sp_tx);
  } else if (commit_or_abort == WRONG_LEADER) {
    // Handle WRONG_LEADER case - don't commit or abort, just return the error
    Log_info("[WRONG_LEADER] Scheduler received WRONG_LEADER for tx_id: {}", tx_id);
    sp_tx->aborted_ = true;  // Mark as aborted to clean up resources
    // The view information is in tpc_commit_cmd.sp_view_data_
    // It will be propagated to client through the coordinator
    if (tpc_commit_cmd.sp_view_data_.is_some()) {
      Log_info("[WRONG_LEADER] View data available in scheduler: {}",
               tpc_commit_cmd.sp_view_data_.as_ref().unwrap()->ToString().c_str());
      sp_tx->sp_view_data_ = tpc_commit_cmd.sp_view_data_;
    } else {
      Log_info("[WRONG_LEADER] No view data available in scheduler for tx_id: {}", tx_id);
    }
  } else {
    verify(0);
  }
  // if (sp_tx->is_leader_hint_) {
  //   // mostly for debug
  //   sp_tx->commit_result->set(1);
  // }
#ifdef LATENCY_LOG_DEBUG
  // Log_info("!!!!!!!!! Before sp_tx->commit_result->set(1);");
#endif
  sp_tx->commit_result->set(1);
  sp_tx->ev_execute_ready_->set(1);
  return 0;
}

bool SchedulerClassic::CheckCommitted(const janus::Command& tpc_commit_cmd) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // 2 step 4: caller passes Command directly; downcast via the
  // SerializableEnvelope `marshallable_cast<T>` overload.
  const auto commit_cmd = marshallable_cast<TpcCommitCommand>(tpc_commit_cmd);
  verify(commit_cmd.is_some());
  auto tx_id = commit_cmd.unwrap()->tx_id_;
  auto sp_tx = dynamic_pointer_cast<TxClassic>(GetTx(tx_id));
  if (!sp_tx)  // it's too old that it's already deleted
    return true;
  return (sp_tx->commit_result->is_ready());
}

int SchedulerClassic::Next(int slot, janus::Command md) {
  if (md.kind_ == TpcPrepareCommand::static_kind()) {
    // TpcPrepareCommand migrated to Serializable.
    auto* c = md.unpack_mut<TpcPrepareCommand>();
    verify(c != nullptr);
    PrepareReplicated(*c);
  } else if (md.kind_ == TpcCommitCommand::static_kind()) {
    const auto c = marshallable_cast<TpcCommitCommand>(md);
    verify(c.is_some());
    // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
    CommitReplicated(*const_cast<TpcCommitCommand*>(c.unwrap().get()));
  } else if (md.kind_ == TpcEmptyCommand::static_kind()) {
    // TpcEmptyCommand is now a Serializable; the apply
    // path's Done() must wake the original sender's Wait() — possible
    // because construction sites use `wrap_serializable_aliased`,
    // which preserves shared_ptr aliasing through the proxy. On the
    // leader, `c` here aliases the sender's instance.
    auto* c = md.unpack_mut<TpcEmptyCommand>();
    verify(c != nullptr);
    c->Done();
  } else if (md.kind_ == TpcBatchCommand::static_kind()) {
    const auto c = marshallable_cast<TpcBatchCommand>(md);
    verify(c.is_some());
    // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
    for (auto& cc : c.unwrap()->cmds_)
      CommitReplicated(*const_cast<TpcCommitCommand*>(cc.get()));
  } else {
    verify(0);
  }
  return -1;
}


} // namespace janus
