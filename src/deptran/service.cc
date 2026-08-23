#include "__dep__.h"
#include "classic/scheduler.h"
#include "classic/tpc_command.h"
#include "classic/tx.h"
#include "command.h"
#include "command_marshaler.h"
#include "communicator.h"
#include "config.h"
#include "coordinator.h"
#include "procedure.h"
#include "rrr/misc/serializable.hpp"  // wrap_serializable_aliased
#include "service.h"
#include "scheduler.h"

namespace janus {
void ClassicServiceImpl::ReElect(const ClassicService::RpcReElectRequest& req, ClassicService::RpcReElectResponse& resp, rrr::DeferredReply defer) {
  (void)req;
  this->ReElect(&resp.success, std::move(defer));
}

void ClassicServiceImpl::Dispatch(const ClassicService::RpcDispatchRequest& req, ClassicService::RpcDispatchResponse& resp, rrr::DeferredReply defer) {
  this->Dispatch(req.tid, req.dep_id, req.cmd, &resp.res, &resp.output, &resp.coro_id, &resp.view_data, std::move(defer));
}

void ClassicServiceImpl::FailoverPauseSocketOut(const ClassicService::RpcFailoverPauseSocketOutRequest& req, ClassicService::RpcFailoverPauseSocketOutResponse& resp, rrr::DeferredReply defer) {
  (void)req;
  this->FailoverPauseSocketOut(&resp.res, std::move(defer));
}

void ClassicServiceImpl::FailoverResumeSocketOut(const ClassicService::RpcFailoverResumeSocketOutRequest& req, ClassicService::RpcFailoverResumeSocketOutResponse& resp, rrr::DeferredReply defer) {
  (void)req;
  this->FailoverResumeSocketOut(&resp.res, std::move(defer));
}

void ClassicServiceImpl::SimpleCmd(const ClassicService::RpcSimpleCmdRequest& req, ClassicService::RpcSimpleCmdResponse& resp, rrr::DeferredReply defer) {
  this->SimpleCmd(req.cmd, &resp.res, std::move(defer));
}

void ClassicServiceImpl::IsLeader(const ClassicService::RpcIsLeaderRequest& req, ClassicService::RpcIsLeaderResponse& resp, rrr::DeferredReply defer) {
  this->IsLeader(req.cur_pause, &resp.is_leader, std::move(defer));
}

void ClassicServiceImpl::Prepare(const ClassicService::RpcPrepareRequest& req, ClassicService::RpcPrepareResponse& resp, rrr::DeferredReply defer) {
  this->Prepare(req.tid, req.sids, req.dep_id, &resp.res, &resp.slow, &resp.coro_id, std::move(defer));
}

void ClassicServiceImpl::Commit(const ClassicService::RpcCommitRequest& req, ClassicService::RpcCommitResponse& resp, rrr::DeferredReply defer) {
  this->Commit(req.tid, req.dep_id, &resp.res, &resp.slow, &resp.coro_id, &resp.view_data, std::move(defer));
}

void ClassicServiceImpl::Abort(const ClassicService::RpcAbortRequest& req, ClassicService::RpcAbortResponse& resp, rrr::DeferredReply defer) {
  this->Abort(req.tid, req.dep_id, &resp.res, &resp.slow, &resp.coro_id, &resp.view_data, std::move(defer));
}

void ClassicServiceImpl::EarlyAbort(const ClassicService::RpcEarlyAbortRequest& req, ClassicService::RpcEarlyAbortResponse& resp, rrr::DeferredReply defer) {
  this->EarlyAbort(req.tid, &resp.res, std::move(defer));
}

void ClassicServiceImpl::rpc_null(const ClassicService::RpcRpcNullRequest& req, ClassicService::RpcRpcNullResponse& resp, rrr::DeferredReply defer) {
  (void)req;
  (void)resp;
  this->rpc_null(std::move(defer));
}

void ClassicServiceImpl::UpgradeEpoch(const ClassicService::RpcUpgradeEpochRequest& req, ClassicService::RpcUpgradeEpochResponse& resp, rrr::DeferredReply defer) {
  this->UpgradeEpoch(req.curr_epoch, &resp.res, std::move(defer));
}

void ClassicServiceImpl::TruncateEpoch(const ClassicService::RpcTruncateEpochRequest& req, ClassicService::RpcTruncateEpochResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->TruncateEpoch(req.old_epoch, std::move(defer));
}

void ClassicServiceImpl::JetpackBeginRecovery(const ClassicService::RpcJetpackBeginRecoveryRequest& req, ClassicService::RpcJetpackBeginRecoveryResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->JetpackBeginRecovery(req.old_view, req.new_view, req.new_view_id, std::move(defer));
}

void ClassicServiceImpl::JetpackPullIdSet(const ClassicService::RpcJetpackPullIdSetRequest& req, ClassicService::RpcJetpackPullIdSetResponse& resp, rrr::DeferredReply defer) {
  this->JetpackPullIdSet(req.jepoch, req.oepoch, &resp.ok, &resp.reply_jepoch, &resp.reply_oepoch, &resp.reply_old_view, &resp.reply_new_view, &resp.id_set, std::move(defer));
}

void ClassicServiceImpl::JetpackPullCmd(const ClassicService::RpcJetpackPullCmdRequest& req, ClassicService::RpcJetpackPullCmdResponse& resp, rrr::DeferredReply defer) {
  this->JetpackPullCmd(req.jepoch, req.oepoch, req.key_batch, &resp.ok, &resp.reply_jepoch, &resp.reply_oepoch, &resp.reply_old_view, &resp.reply_new_view, &resp.cmd_batch, std::move(defer));
}

void ClassicServiceImpl::JetpackRecordCmd(const ClassicService::RpcJetpackRecordCmdRequest& req, ClassicService::RpcJetpackRecordCmdResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->JetpackRecordCmd(req.jepoch, req.oepoch, req.sid, req.rid, req.cmd_batch, std::move(defer));
}

void ClassicServiceImpl::JetpackPrepare(const ClassicService::RpcJetpackPrepareRequest& req, ClassicService::RpcJetpackPrepareResponse& resp, rrr::DeferredReply defer) {
  this->JetpackPrepare(req.jepoch, req.oepoch, req.max_seen_ballot, &resp.ok, &resp.reply_jepoch, &resp.reply_oepoch, &resp.reply_old_view, &resp.reply_new_view, &resp.reply_max_seen_ballot, &resp.accepted_ballot, &resp.replied_sid, &resp.replied_set_size, std::move(defer));
}

void ClassicServiceImpl::JetpackAccept(const ClassicService::RpcJetpackAcceptRequest& req, ClassicService::RpcJetpackAcceptResponse& resp, rrr::DeferredReply defer) {
  this->JetpackAccept(req.jepoch, req.oepoch, req.max_seen_ballot, req.sid, req.set_size, &resp.ok, &resp.reply_jepoch, &resp.reply_oepoch, &resp.reply_old_view, &resp.reply_new_view, &resp.reply_max_seen_ballot, std::move(defer));
}

void ClassicServiceImpl::JetpackCommit(const ClassicService::RpcJetpackCommitRequest& req, ClassicService::RpcJetpackCommitResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->JetpackCommit(req.jepoch, req.oepoch, req.sid, req.set_size, std::move(defer));
}

void ClassicServiceImpl::JetpackPullRecSetIns(const ClassicService::RpcJetpackPullRecSetInsRequest& req, ClassicService::RpcJetpackPullRecSetInsResponse& resp, rrr::DeferredReply defer) {
  this->JetpackPullRecSetIns(req.jepoch, req.oepoch, req.sid, req.rid, &resp.ok, &resp.reply_jepoch, &resp.reply_oepoch, &resp.reply_old_view, &resp.reply_new_view, &resp.cmd, std::move(defer));
}

void ClassicServiceImpl::JetpackFinishRecovery(const ClassicService::RpcJetpackFinishRecoveryRequest& req, ClassicService::RpcJetpackFinishRecoveryResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->JetpackFinishRecovery(req.oepoch, std::move(defer));
}

void ClassicServiceImpl::MsgString(const ClassicService::RpcMsgStringRequest& req, ClassicService::RpcMsgStringResponse& resp, rrr::DeferredReply defer) {
  this->MsgString(req.arg, &resp.ret, std::move(defer));
}

void ClassicServiceImpl::MsgMarshall(const ClassicService::RpcMsgMarshallRequest& req, ClassicService::RpcMsgMarshallResponse& resp, rrr::DeferredReply defer) {
  this->MsgMarshall(req.arg, &resp.ret, std::move(defer));
}

ClassicServiceImpl::ClassicServiceImpl(TxLogServer* sched,
                                       rusty::Arc<rrr::PollThread> poll_thread_worker)
    : dtxn_sched_(sched), poll_thread_worker_(poll_thread_worker) {

#ifdef PIECE_COUNT
  piece_count_timer_.start();
  piece_count_prepare_fail_ = 0;
  piece_count_prepare_success_ = 0;
#endif

  // removed `if (do_logging()) { verify(0);
  // ... }` block — body was a `verify(0)`-only TODO shell with the
  // `recorder_ = new Recorder(path);` and `poll_thread_worker->add`
  // calls already commented out; `Service::recorder_` field gone.

}

void ClassicServiceImpl::ReElect(bool_t* success,
																 rrr::DeferredReply defer) {
	for(int i = 0; i < 100000; i++) Log_info("loop loop loop");
	*success = dtxn_sched()->RequestVote();
	defer.reply();
}

void ClassicServiceImpl::Dispatch(const i64& cmd_id,
																	const DepId& dep_id,
                                  const janus::Command& md,
                                  int32_t* res,
                                  TxnOutput* output,
                                  uint64_t* coro_id,
                                  janus::Command* view_data,
                                  rrr::DeferredReply defer) {
  // usleep(20000);

#ifdef LATENCY_LOG_DEBUG
  Log_info("!!!!!!!!!!!!! cmd {} enter ClassicServiceImpl::Dispatch (after client RPC) at loc_id {}", cmd_id, dtxn_sched()->loc_id_);
#endif

#ifdef FULL_LOG_DEBUG
  Log_info("[Jetpack] cmd<{}, {}> entered ClassicServiceImpl::Dispatch", SimpleRWCommand::GetCmdID(md).first, SimpleRWCommand::GetCmdID(md).second);
#endif

  Log_debug("The server side receives a message from the client worker");

#ifdef PIECE_COUNT
  piece_count_key_t piece_count_key =
      (piece_count_key_t){header.t_type, header.p_type};
  std::map<piece_count_key_t, uint64_t>::iterator pc_it =
      piece_count_.find(piece_count_key);

  if (pc_it == piece_count_.end())
      piece_count_[piece_count_key] = 1;
  else
      piece_count_[piece_count_key]++;
  piece_count_tid_.insert(header.tid);
#endif
#ifndef ZERO_OVERHEAD
  dtxn_sched()->OriginalPathUnexecutedCmdConflictPlaceHolder(md);
#endif

  // Check if this is a recovery command
  bool is_recovery = false;
  if (md.has_value() && md.kind_ == VecPieceData::static_kind()) {
    const auto vec_piece_data = marshallable_cast<VecPieceData>(md);
    if (vec_piece_data.is_some() && vec_piece_data.unwrap()->is_recovery_command_) {
      is_recovery = true;
    }
  }

  // TxLogServer::Dispatch still takes std::shared_ptr<ViewData>&
  // (rrr boundary, scheduler.h); convert to the Arc-based envelope
  // at the edge below.
  std::shared_ptr<ViewData> view;
  // TxLogServer::Dispatch now takes janus::Command;
  // pass `md` (RPC param) directly.
  *res = dtxn_sched()->Dispatch(cmd_id, md, *output, view);

  // Set the view data in the output parameter
  if (view) {
    // @unsafe { boundary copy: shared_ptr<ViewData> out-param -> Arc envelope }
    *view_data = rusty::Arc<ViewData>::make(*view);
  } else {
    // Initialize with empty view data if not set
    *view_data = rusty::Arc<ViewData>::make();
  }
  
  auto coro_opt = Fiber::current_fiber();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id.get();
  }
  defer.reply();
  // }, __FILE__, cmd_id);
  // auto func = [cmd_id, sp, output, dep_id, res, coro_id, this, defer]() {
  //   *res = SUCCESS;
  //   auto sched = (SchedulerClassic*) dtxn_sched_;
  //   if (!sched->Dispatch(cmd_id, dep_id, sp, *output)) {
  //     *res = REJECT;
  //   }
  //   auto coro_opt = Fiber::current_fiber();
  //   if (coro_opt.is_some()) {
  //     *coro_id = coro_opt.unwrap()->id.get();
  //   }
  //   defer.reply();
  // };

  // auto sched = (SchedulerClassic*) dtxn_sched_;
  // auto tx = dynamic_pointer_cast<TxClassic>(sched->GetOrCreateTx(cmd_id));
	// func();
}


void ClassicServiceImpl::FailoverPauseSocketOut(
    rrr::i32* res, rrr::DeferredReply defer) {
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! ClassicServiceImpl::FailoverPauseSocketOut");
#endif
  if (pause && clt_cnt_ == 0) {
    // TODO temp solution yidawu
    auto client_infos = Config::GetConfig()->GetMyClients();
    unordered_set<string> clt_set;
    for (auto c : client_infos) {
      clt_set.insert(c.host);
    }

    clt_cnt_.store(clt_set.size());
  }

  Fiber::create_run([&]() {
    // TODO: yidawu need to test with multi clients in diff machines
    int wait_int = 50 * 1000; // 50ms
    while (clt_cnt_.load() == 0) {
      auto e = create_sp_never_event();
      e->wait_timeout(wait_int);
    }
    clt_cnt_--;
    while (clt_cnt_.load() != 0) {
      auto e = create_sp_never_event();
      e->wait_timeout(wait_int);
    }
    dtxn_sched_->rep_sched_->Pause();
    // pause() not implemented in PollThreadWorker;
    *res = SUCCESS;
    defer.reply();
  });
}

void ClassicServiceImpl::FailoverResumeSocketOut(
    rrr::i32* res, rrr::DeferredReply defer) {
#ifdef FAILOVER_DEBUG
  Log_info("!!!!!!!!!!!!!! ClassicServiceImpl::FailoverResumeSocketOut");
#endif
  if (pause && clt_cnt_ == 0) {
    // TODO temp solution yidawu
    auto client_infos = Config::GetConfig()->GetMyClients();
    unordered_set<string> clt_set;
    for (auto c : client_infos) {
      clt_set.insert(c.host);
    }

    clt_cnt_.store(clt_set.size());
  }

  Fiber::create_run([&]() {
    // resume() not implemented in PollThreadWorker;
    dtxn_sched_->rep_sched_->Resume();
    *res = SUCCESS;
    defer.reply();
  });
}

void ClassicServiceImpl::SimpleCmd(
    const SimpleCommand& cmd, rrr::i32* res, rrr::DeferredReply defer) {
  Fiber::create_run([res, defer = std::move(defer), this]() mutable {
    auto empty_cmd = rusty::Arc<TpcEmptyCommand>::make();
    // aliased wrap preserves event-member aliasing — the
    // apply path's Done() must wake this empty_cmd's Wait() below.
    auto sched = (SchedulerClassic*)dtxn_sched_;
    sched->CreateRepCoord(0)->Submit(
        janus::Command::pack_aliased<TpcEmptyCommand>(empty_cmd.clone()));
    empty_cmd->Wait();
    *res = SUCCESS;
    defer.reply();
  });
}

void ClassicServiceImpl::IsLeader(
    const locid_t& can_id, bool_t* is_leader, rrr::DeferredReply defer) {
  auto sched = (SchedulerClassic*)dtxn_sched_;
  *is_leader = sched->IsLeader();
  defer.reply();
}

void ClassicServiceImpl::Prepare(const rrr::i64& tid,
                                 const std::vector<i32>& sids,
                                 const DepId& dep_id,
                                 rrr::i32* res,
																 bool_t* slow,
                                 uint64_t* coro_id,
                                 rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  auto sched = (SchedulerClassic*) dtxn_sched_;
  bool null_cmd = false;
  bool ret = sched->OnPrepare(tid, sids, dep_id, null_cmd);
  //Log_info("slow1: {}", sched->slow_);
  *slow = sched->slow_;
  *res = ret ? SUCCESS : REJECT;
  if(null_cmd) *res = REPEAT;
  auto coro_opt = Fiber::current_fiber();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id.get();
  }
  defer.reply();
  //auto coro = Fiber::create_run(func);
  //Log_info("coro id on service side: {}", coro->id);
// TODO move the stat to somewhere else.
#ifdef PIECE_COUNT
  std::map<piece_count_key_t, uint64_t>::iterator pc_it;
  if (*res != SUCCESS)
      piece_count_prepare_fail_++;
  else
      piece_count_prepare_success_++;
  if (piece_count_timer_.elapsed() >= 5.0) {
      Log_info("PIECE_COUNT: txn served: {}", piece_count_tid_.size());
      Log_info("PIECE_COUNT: prepare success: {}, failed: {}",
        piece_count_prepare_success_, piece_count_prepare_fail_);
      for (pc_it = piece_count_.begin(); pc_it != piece_count_.end(); pc_it++)
          Log_info("PIECE_COUNT: t_type: {}, p_type: {}, started: {}",
            pc_it->first.t_type, pc_it->first.p_type, pc_it->second);
      piece_count_timer_.start();
  }
#endif
}

void ClassicServiceImpl::Commit(const rrr::i64& tid,
                                const DepId& dep_id,
                                rrr::i32* res,
																bool_t* slow,
                                uint64_t* coro_id,
                                janus::Command* view_data,
                                rrr::DeferredReply defer) {
  //std::lock_guard<std::mutex> guard(mtx_);
  auto sched = (SchedulerClassic*) dtxn_sched_;
  int ret = sched->OnCommit(tid, dep_id, SUCCESS);

  //Log_info("slow2: {}", sched->slow_);
  *slow = sched->slow_;
  auto coro_opt = Fiber::current_fiber();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id.get();
  }

  if (ret == WRONG_LEADER) {
    *res = WRONG_LEADER;
    Log_info("[WRONG_LEADER] ServiceImpl::Commit returning WRONG_LEADER for tx_id: {}", tid);
    // removed the
    // `dynamic_cast<TxData*>(sp_tx->cmd_.inner_marshallable().get())`
    // escape hatch.  After L10f-1, TxData no longer inherits
    // Marshallable, so the dynamic_cast always returned nullptr
    // and the entire if-block was dead.  sp_tx->cmd_ in this path
    // is always a dispatched RPC command (VecPieceData or
    // TpcCommitCommand kind), not a TxData.
  } else {
    *res = SUCCESS;
    // Set view data from replication scheduler if available
    if (sched->rep_sched_ != nullptr) {
      *view_data = rusty::Arc<ViewData>::make(sched->rep_sched_->new_view_);
    } else {
      // If no replication scheduler, set an empty ViewData
      *view_data = rusty::Arc<ViewData>::make();
    }
  }

  defer.reply();
}

void ClassicServiceImpl::Abort(const rrr::i64& tid,
                               const DepId& dep_id,
                               rrr::i32* res,
															 bool_t* slow,
                               uint64_t* coro_id,
                               janus::Command* view_data,
                               rrr::DeferredReply defer) {
  Log_debug("get abort_txn: tid: {}", tid);
  //std::lock_guard<std::mutex> guard(mtx_);
  auto sched = (SchedulerClassic*) dtxn_sched_;
  sched->OnCommit(tid, dep_id, REJECT);
  Log_info("slow3: {}", sched->slow_);
  *slow = sched->slow_;
  *res = SUCCESS;
  auto coro_opt = Fiber::current_fiber();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id.get();
  }
  // Set view data from replication scheduler if available
  if (sched->rep_sched_ != nullptr) {
    *view_data = rusty::Arc<ViewData>::make(sched->rep_sched_->new_view_);
  } else {
    // If no replication scheduler, set an empty ViewData
    *view_data = rusty::Arc<ViewData>::make();
  }
  defer.reply();
}

void ClassicServiceImpl::EarlyAbort(const rrr::i64& tid,
                                    rrr::i32* res,
                                    rrr::DeferredReply defer) {
  Log_debug("get abort_txn: tid: {}", tid);
//  std::lock_guard<std::mutex> guard(mtx_);
//  const auto& func = [tid, res, defer, this]() {
  auto sched = (SchedulerClassic*) dtxn_sched_;
  sched->OnEarlyAbort(tid);
  *res = SUCCESS;
  defer.reply();
//  };
//  Fiber::create_run(func);
}

void ClassicServiceImpl::rpc_null(rrr::DeferredReply defer) {
  defer.reply();
}

void ClassicServiceImpl::UpgradeEpoch(const uint32_t& curr_epoch,
                                      int32_t* res,
                                      rrr::DeferredReply defer) {
  *res = dtxn_sched()->OnUpgradeEpoch(curr_epoch);
  defer.reply();
}

void ClassicServiceImpl::TruncateEpoch(const uint32_t& old_epoch,
                                       rrr::DeferredReply defer) {
  verify(0);
}

void ClassicServiceImpl::JetpackBeginRecovery(const janus::Command& old_view,
                                              const janus::Command& new_view, 
                                              const epoch_t& new_view_id, 
                                              rrr::DeferredReply defer) {
  dtxn_sched()->OnJetpackBeginRecovery(old_view, new_view, new_view_id);
  defer.reply();
}

void ClassicServiceImpl::JetpackPullIdSet(const epoch_t& jepoch,
                                          const epoch_t& oepoch,
                                          bool_t* ok, 
                                          epoch_t* reply_jepoch, 
                                          epoch_t* reply_oepoch,
                                          janus::Command* reply_old_view,
                                          janus::Command* reply_new_view,
                                          janus::Command* id_set, 
                                          rrr::DeferredReply defer) {
  // Fill-then-wrap: the handler populates a local, which is packed
  // once complete — no mutation through a packed handle.
  VecRecData ret_id_set;
  dtxn_sched()->OnJetpackPullIdSet(jepoch, oepoch, ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, ret_id_set);
  *id_set = rusty::Arc<VecRecData>::make(std::move(ret_id_set));
  defer.reply();
}

void ClassicServiceImpl::JetpackPullCmd(const epoch_t& jepoch,
                                        const epoch_t& oepoch, 
                                        const janus::Command& key_batch, 
                                        bool_t* ok, 
                                        epoch_t* reply_jepoch, 
                                        epoch_t* reply_oepoch,
                                        janus::Command* reply_old_view,
                                        janus::Command* reply_new_view,
                                        janus::Command* cmd_batch, 
                                        rrr::DeferredReply defer) {
  const auto vec_keys = marshallable_cast<VecRecData>(key_batch);
  std::vector<key_t> keys;
  if (vec_keys.is_some() && vec_keys.unwrap()->key_data_) {
    keys.assign(vec_keys.unwrap()->key_data_->begin(),
                vec_keys.unwrap()->key_data_->end());
  }
  // Fill-then-wrap: handler fills a local, packed after completion.
  KeyCmdBatchData batch_result;
  dtxn_sched()->OnJetpackPullCmd(jepoch, oepoch, keys, ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, batch_result);
  *cmd_batch = janus::Command::pack_aliased(rusty::Arc<KeyCmdBatchData>::make(std::move(batch_result)));
  defer.reply();

}

void ClassicServiceImpl::JetpackRecordCmd(const epoch_t& jepoch,
                                          const epoch_t& oepoch,
                                          const int32_t& sid,
                                          const int32_t& rid,
                                          const janus::Command& md, 
                                          rrr::DeferredReply defer) {
  const auto batch = marshallable_cast<KeyCmdBatchData>(md);
  const KeyCmdBatchData empty_batch;
  dtxn_sched()->OnJetpackRecordCmd(jepoch, oepoch, sid, rid,
                                   batch.is_some() ? *batch.unwrap()
                                                   : empty_batch);
  defer.reply();
}

void ClassicServiceImpl::JetpackPrepare(const epoch_t& jepoch, 
                                        const epoch_t& oepoch, 
                                        const ballot_t& max_seen_ballot, 
                                        bool_t* ok, 
                                        epoch_t* reply_jepoch,
                                        epoch_t* reply_oepoch,
                                        janus::Command* reply_old_view,
                                        janus::Command* reply_new_view,
                                        ballot_t* reply_max_seen_ballot,
                                        ballot_t* accepted_ballot, 
                                        int32_t* replied_sid, 
                                        int32_t* replied_set_size, 
                                        rrr::DeferredReply defer) {
  dtxn_sched()->OnJetpackPrepare(jepoch, oepoch, max_seen_ballot, ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, reply_max_seen_ballot, accepted_ballot, replied_sid, replied_set_size);
  defer.reply();
}

void ClassicServiceImpl::JetpackAccept(const epoch_t& jepoch, 
                                       const epoch_t& oepoch, 
                                       const ballot_t& max_seen_ballot, 
                                       const int32_t& sid, 
                                       const int32_t& set_size, 
                                       bool_t* ok, 
                                       epoch_t* reply_jepoch,
                                       epoch_t* reply_oepoch,
                                       janus::Command* reply_old_view,
                                       janus::Command* reply_new_view,
                                       ballot_t* reply_max_seen_ballot,
                                       rrr::DeferredReply defer) {
  dtxn_sched()->OnJetpackAccept(jepoch, oepoch, max_seen_ballot, sid, set_size, ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view, reply_max_seen_ballot);
  defer.reply();
}

void ClassicServiceImpl::JetpackCommit(const epoch_t& jepoch,
                                       const epoch_t& oepoch, 
                                       const int32_t& sid, 
                                       const int32_t& set_size, 
                                       rrr::DeferredReply defer) {
  dtxn_sched()->OnJetpackCommit(jepoch, oepoch, sid, set_size);
  defer.reply();
}

void ClassicServiceImpl::JetpackPullRecSetIns(const epoch_t& jepoch,
                                              const epoch_t& oepoch, 
                                              const int32_t& sid, 
                                              const int32_t& rid, 
                                              bool_t* ok, 
                                              epoch_t* reply_jepoch,
                                              epoch_t* reply_oepoch,
                                              janus::Command* reply_old_view,
                                              janus::Command* reply_new_view,
                                              janus::Command* cmd, 
                                              rrr::DeferredReply defer) {
  *cmd = janus::Command::pack_aliased(rusty::Arc<TpcCommitCommand>::make());
  dtxn_sched()->OnJetpackPullRecSetIns(jepoch, oepoch, sid, rid, ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view);
  defer.reply();
}

 void ClassicServiceImpl::JetpackFinishRecovery(const epoch_t& oepoch,
                                                rrr::DeferredReply defer) {
  dtxn_sched()->OnJetpackFinishRecovery(oepoch);
  defer.reply();
}


void ClassicServiceImpl::MsgString(const string& arg,
                                   string* ret,
                                   rrr::DeferredReply defer) {

  verify(comm_ != nullptr);
  for (auto& f : comm_->msg_string_handlers_) {
    if (f(arg, *ret)) {
      defer.reply();
      return;
    }
  }
  verify(0);
  return;
}

void ClassicServiceImpl::MsgMarshall(const janus::Command& arg,
                                     janus::Command* ret,
                                     rrr::DeferredReply defer) {

  verify(comm_ != nullptr);
  for (auto& f : comm_->msg_marshall_handlers_) {
    if (f(arg, *ret)) {
      defer.reply();
      return;
    }
  }
  verify(0);
  return;
}

} // namespace janus
