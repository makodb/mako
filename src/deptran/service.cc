#include "__dep__.h"
#include "benchmark_control_rpc.h"
#include "stats_registry.h"
#include "classic/scheduler.h"
#include "classic/tpc_command.h"
#include "classic/tx.h"
#include "command.h"
#include "command_marshaler.h"
#include "communicator.h"
#include "config.h"
#include "coordinator.h"
#include "janus/scheduler.h"
#include "procedure.h"
#include "rcc/dep_graph.h"
#include "rrr/misc/serializable.hpp"  // wrap_serializable_aliased
#include "service.h"
#include "rcc/server.h"
#include "scheduler.h"
#include "tapir/scheduler.h"
#include "../bench/rw/workload.h" //<copilot+ kv debug>

namespace janus {
void ClassicServiceImpl::ReElect(const ClassicService::RpcReElectRequest& req, ClassicService::RpcReElectResponse& resp, rrr::DeferredReply defer) {
  (void)req;
  this->ReElect(&resp.success, std::move(defer));
}

void ClassicServiceImpl::RuleSpeculativeExecute(const ClassicService::RpcRuleSpeculativeExecuteRequest& req, ClassicService::RpcRuleSpeculativeExecuteResponse& resp, rrr::DeferredReply defer) {
  this->RuleSpeculativeExecute(req.md, &resp.accepted, &resp.result, &resp.is_leader, std::move(defer));
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

void ClassicServiceImpl::IsFPGALeader(const ClassicService::RpcIsFPGALeaderRequest& req, ClassicService::RpcIsFPGALeaderResponse& resp, rrr::DeferredReply defer) {
  this->IsFPGALeader(req.cur_pause, &resp.is_leader, std::move(defer));
}

void ClassicServiceImpl::Prepare(const ClassicService::RpcPrepareRequest& req, ClassicService::RpcPrepareResponse& resp, rrr::DeferredReply defer) {
  this->Prepare(req.tid, req.sids, req.dep_id, &resp.res, &resp.slow, &resp.coro_id, std::move(defer));
}

void ClassicServiceImpl::Commit(const ClassicService::RpcCommitRequest& req, ClassicService::RpcCommitResponse& resp, rrr::DeferredReply defer) {
  this->Commit(req.tid, req.dep_id, &resp.res, &resp.slow, &resp.coro_id, &resp.profile, &resp.view_data, std::move(defer));
}

void ClassicServiceImpl::Abort(const ClassicService::RpcAbortRequest& req, ClassicService::RpcAbortResponse& resp, rrr::DeferredReply defer) {
  this->Abort(req.tid, req.dep_id, &resp.res, &resp.slow, &resp.coro_id, &resp.profile, &resp.view_data, std::move(defer));
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

void ClassicServiceImpl::TapirAccept(const ClassicService::RpcTapirAcceptRequest& req, ClassicService::RpcTapirAcceptResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->TapirAccept(req.cmd_id, req.ballot, req.decision, std::move(defer));
}

void ClassicServiceImpl::TapirFastAccept(const ClassicService::RpcTapirFastAcceptRequest& req, ClassicService::RpcTapirFastAcceptResponse& resp, rrr::DeferredReply defer) {
  this->TapirFastAccept(req.cmd_id, req.txn_cmds, &resp.res, std::move(defer));
}

void ClassicServiceImpl::TapirDecide(const ClassicService::RpcTapirDecideRequest& req, ClassicService::RpcTapirDecideResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->TapirDecide(req.cmd_id, req.commit, std::move(defer));
}

void ClassicServiceImpl::RccDispatch(const ClassicService::RpcRccDispatchRequest& req, ClassicService::RpcRccDispatchResponse& resp, rrr::DeferredReply defer) {
  this->RccDispatch(req.cmd, &resp.res, &resp.output, &resp.md_graph, std::move(defer));
}

void ClassicServiceImpl::RccFinish(const ClassicService::RpcRccFinishRequest& req, ClassicService::RpcRccFinishResponse& resp, rrr::DeferredReply defer) {
  this->RccFinish(req.id, req.md_graph, &resp.outputs, std::move(defer));
}

void ClassicServiceImpl::RccInquire(const ClassicService::RpcRccInquireRequest& req, ClassicService::RpcRccInquireResponse& resp, rrr::DeferredReply defer) {
  this->RccInquire(req.txn_id, req.rank, &resp.out_0, std::move(defer));
}

void ClassicServiceImpl::RccDispatchRo(const ClassicService::RpcRccDispatchRoRequest& req, ClassicService::RpcRccDispatchRoResponse& resp, rrr::DeferredReply defer) {
  this->RccDispatchRo(req.cmd, &resp.output, std::move(defer));
}

void ClassicServiceImpl::RccInquireValidation(const ClassicService::RpcRccInquireValidationRequest& req, ClassicService::RpcRccInquireValidationResponse& resp, rrr::DeferredReply defer) {
  this->RccInquireValidation(req.tx_id, req.rank, &resp.res, std::move(defer));
}

void ClassicServiceImpl::RccNotifyGlobalValidation(const ClassicService::RpcRccNotifyGlobalValidationRequest& req, ClassicService::RpcRccNotifyGlobalValidationResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->RccNotifyGlobalValidation(req.tx_id, req.rank, req.res, std::move(defer));
}

void ClassicServiceImpl::JanusDispatch(const ClassicService::RpcJanusDispatchRequest& req, ClassicService::RpcJanusDispatchResponse& resp, rrr::DeferredReply defer) {
  this->JanusDispatch(req.cmd, &resp.res, &resp.output, &resp.ret_graph, std::move(defer));
}

void ClassicServiceImpl::JanusCommit(const ClassicService::RpcJanusCommitRequest& req, ClassicService::RpcJanusCommitResponse& resp, rrr::DeferredReply defer) {
  this->JanusCommit(req.id, req.rank, req.need_validation, req.graph, &resp.res, &resp.output, std::move(defer));
}

void ClassicServiceImpl::RccCommit(const ClassicService::RpcRccCommitRequest& req, ClassicService::RpcRccCommitResponse& resp, rrr::DeferredReply defer) {
  this->RccCommit(req.id, req.rank, req.need_validation, req.parents, &resp.res, &resp.output, std::move(defer));
}

void ClassicServiceImpl::JanusCommitWoGraph(const ClassicService::RpcJanusCommitWoGraphRequest& req, ClassicService::RpcJanusCommitWoGraphResponse& resp, rrr::DeferredReply defer) {
  this->JanusCommitWoGraph(req.id, req.rank, req.need_validation, &resp.res, &resp.output, std::move(defer));
}

void ClassicServiceImpl::JanusInquire(const ClassicService::RpcJanusInquireRequest& req, ClassicService::RpcJanusInquireResponse& resp, rrr::DeferredReply defer) {
  this->JanusInquire(req.epoch, req.txn_id, &resp.ret_graph, std::move(defer));
}

void ClassicServiceImpl::RccPreAccept(const ClassicService::RpcRccPreAcceptRequest& req, ClassicService::RpcRccPreAcceptResponse& resp, rrr::DeferredReply defer) {
  this->RccPreAccept(req.txn_id, req.rank, req.cmd, &resp.res, &resp.x, std::move(defer));
}

void ClassicServiceImpl::JanusPreAccept(const ClassicService::RpcJanusPreAcceptRequest& req, ClassicService::RpcJanusPreAcceptResponse& resp, rrr::DeferredReply defer) {
  this->JanusPreAccept(req.txn_id, req.rank, req.cmd, req.graph, &resp.res, &resp.ret_graph, std::move(defer));
}

void ClassicServiceImpl::JanusPreAcceptWoGraph(const ClassicService::RpcJanusPreAcceptWoGraphRequest& req, ClassicService::RpcJanusPreAcceptWoGraphResponse& resp, rrr::DeferredReply defer) {
  this->JanusPreAcceptWoGraph(req.txn_id, req.rank, req.cmd, &resp.res, &resp.ret_graph, std::move(defer));
}

void ClassicServiceImpl::RccAccept(const ClassicService::RpcRccAcceptRequest& req, ClassicService::RpcRccAcceptResponse& resp, rrr::DeferredReply defer) {
  this->RccAccept(req.txn_id, req.rank, req.ballot, req.p, &resp.res, std::move(defer));
}

void ClassicServiceImpl::JanusAccept(const ClassicService::RpcJanusAcceptRequest& req, ClassicService::RpcJanusAcceptResponse& resp, rrr::DeferredReply defer) {
  this->JanusAccept(req.txn_id, req.rank, req.ballot, req.graph, &resp.res, std::move(defer));
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

  this->RegisterStats();
}

void ClassicServiceImpl::ReElect(bool_t* success,
																 rrr::DeferredReply defer) {
	for(int i = 0; i < 100000; i++) Log_info("loop loop loop");
	*success = dtxn_sched()->RequestVote();
	defer.reply();
}

void ClassicServiceImpl::RuleSpeculativeExecute(const janus::Command& md,
                                                bool_t* accepted,
                                                int32_t* result,
                                                bool_t* is_leader,
                                                rrr::DeferredReply defer) {
  dtxn_sched()->OnRuleSpeculativeExecute(md, accepted, result, is_leader);
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

#ifdef COPILOT_TIME_DEBUG
  struct timeval tp;
  gettimeofday(&tp, NULL);
  Log_info("[Jetpack] [C+] Received Dispatch {:.3f}", tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif

  Log_debug("The server side receives a message from the client worker");
  // Log_info("[copilot+] [1+] enter ClassicServiceImpl::Dispatch");

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
  // Log_info("[copilot+] [1-] exit ClassicServiceImpl::Dispatch");
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
      auto e = Reactor::create_sp_event<NeverEvent>();
      e->wait_timeout(wait_int);
    }
    clt_cnt_--;
    while (clt_cnt_.load() != 0) {
      auto e = Reactor::create_sp_event<NeverEvent>();
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

void ClassicServiceImpl::IsFPGALeader(
    const locid_t& can_id, bool_t* is_leader, rrr::DeferredReply defer) {
  auto sched = (SchedulerClassic*)dtxn_sched_;
  *is_leader = sched->IsFPGALeader();
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
																Profiling* profile,
                                janus::Command* view_data,
                                rrr::DeferredReply defer) {
  //std::lock_guard<std::mutex> guard(mtx_);
  auto sched = (SchedulerClassic*) dtxn_sched_;
  int ret = sched->OnCommit(tid, dep_id, SUCCESS);

  auto result = rrr::CPUInfo::cpu_stat();  // cpu_stat() returns rusty::Vec<double>
  *profile = {result[0], result[1], result[2], result[3]};
  //*profile = {0.0, 0.0, 0.0, 0.0};
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
															 Profiling* profile,
                               janus::Command* view_data,
                               rrr::DeferredReply defer) {
  Log_debug("get abort_txn: tid: {}", tid);
  //std::lock_guard<std::mutex> guard(mtx_);
  auto sched = (SchedulerClassic*) dtxn_sched_;
  sched->OnCommit(tid, dep_id, REJECT);
  auto result = rrr::CPUInfo::cpu_stat();  // cpu_stat() returns rusty::Vec<double>
  *profile = {result[0], result[1], result[2]};
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

void ClassicServiceImpl::TapirAccept(const cmdid_t& cmd_id,
                                     const ballot_t& ballot,
                                     const int32_t& decision,
                                     rrr::DeferredReply defer) {
  verify(0);
}

void ClassicServiceImpl::TapirFastAccept(const txid_t& tx_id,
                                         const vector<SimpleCommand>& txn_cmds,
                                         rrr::i32* res,
                                         rrr::DeferredReply defer) {
  SchedulerTapir* sched = (SchedulerTapir*) dtxn_sched_;
  *res = sched->OnFastAccept(tx_id, txn_cmds);
  defer.reply();
}

void ClassicServiceImpl::TapirDecide(const cmdid_t& cmd_id,
                                     const rrr::i32& decision,
                                     rrr::DeferredReply defer) {
  SchedulerTapir* sched = (SchedulerTapir*) dtxn_sched_;
  sched->OnDecide(cmd_id, decision, [defer = std::move(defer)]() mutable { defer.reply(); });
}

void ClassicServiceImpl::RccDispatch(const vector<SimpleCommand>& cmd,
                                     int32_t* res,
                                     TxnOutput* output,
                                     rrr::AnyMessage* p_md_graph,
                                     rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(this->mtx_);
  RccServer* sched = (RccServer*) dtxn_sched_;
  auto p = rusty::Arc<RccGraph>::make();
  // graph reply rides directly as `AnyMessage` (aliased: OnDispatch's
  // fills through the shared payload stay visible to the packed reply).
  *p_md_graph = rrr::AnyMessage::pack(p.clone());
  *res = sched->OnDispatch(cmd, output, std::move(p));
  defer.reply();
}

void ClassicServiceImpl::RccFinish(const cmdid_t& cmd_id,
                                   const rrr::AnyMessage& md_graph,
                                   TxnOutput* output,
                                   rrr::DeferredReply defer) {
  // graph rides directly as AnyMessage.
  const auto sp_graph = md_graph.unpack<RccGraph>();
  verify(sp_graph.is_some());
  const RccGraph& graph = *sp_graph.unwrap();
  verify(graph.size() > 0);
  verify(0);
//  std::lock_guard<std::mutex> guard(mtx_);
  RccServer* sched = (RccServer*) dtxn_sched_;
//  sched->OnCommit(cmd_id, RANK_UNDEFINED, graph, output, [defer = std::move(defer)]() mutable { defer.reply(); });

  stat_sz_gra_commit_.sample(graph.size());
}

void ClassicServiceImpl::RccInquire(const txnid_t& tid,
                                    const int32_t& rank,
                                    map<txid_t, parent_set_t>* ret,
                                    rrr::DeferredReply defer) {
//  verify(IS_MODE_RCC || IS_MODE_RO6);
//  std::lock_guard<std::mutex> guard(mtx_);
  RccServer* p_sched = (RccServer*) dtxn_sched_;
//  *p_md_graph = std::make_shared<RccGraph>();
//  p_sched->OnInquire(epoch,
//                     tid,
//                     dynamic_pointer_cast<RccGraph>(p_md_graph->sp_data_));
  p_sched->OnInquire(tid, rank, ret);
  defer.reply();
}


void ClassicServiceImpl::RccDispatchRo(const SimpleCommand& cmd,
                                       map<int32_t, Value>* output,
                                       rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  verify(0);
  auto tx = dtxn_sched_->GetOrCreateTx(cmd.root_id_, true);
  auto dtxn = dynamic_pointer_cast<RccTx>(tx);
  dtxn->start_ro(cmd, *output, [defer = std::move(defer)]() mutable { defer.reply(); });
}

void ClassicServiceImpl::RccInquireValidation(
    const txid_t& txid,
    const int32_t& rank,
    int32_t* ret,
    rrr::DeferredReply defer) {
  auto* s = (RccServer*) dtxn_sched_;
  *ret = s->OnInquireValidation(txid, rank);
  defer.reply();
}

void ClassicServiceImpl::RccNotifyGlobalValidation(
    const txid_t& txid, const int32_t& rank, const int32_t& res, rrr::DeferredReply defer) {
  auto* s = (RccServer*) dtxn_sched_;
  s->OnNotifyGlobalValidation(txid, rank, res);
  defer.reply();
}

void ClassicServiceImpl::JanusDispatch(const vector<SimpleCommand>& cmd,
                                       int32_t* p_res,
                                       TxnOutput* p_output,
                                       rrr::AnyMessage* p_md_res_graph,
                                       rrr::DeferredReply defer) {
//    std::lock_guard<std::mutex> guard(this->mtx_); // TODO remove the lock.
    auto sp_graph = rusty::Arc<RccGraph>::make();
    auto* sched = (SchedulerJanus*) dtxn_sched_;
    *p_res = sched->OnDispatch(cmd, p_output, sp_graph.clone());
    if (sp_graph->size() <= 1) {
      // graph reply rides directly as AnyMessage.
      *p_md_res_graph =
          rrr::AnyMessage::pack(rusty::Arc<EmptyGraph>::make());
    } else {
      *p_md_res_graph = rrr::AnyMessage::pack(std::move(sp_graph));
    }
    verify(!p_md_res_graph->type_name_.empty());
    defer.reply();
}

void ClassicServiceImpl::JanusCommit(const cmdid_t& cmd_id,
                                     const rank_t& rank,
                                     const int32_t& need_validation,
                                     const rrr::AnyMessage& graph,
                                     int32_t* res,
                                     TxnOutput* output,
                                     rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  verify(0);
  auto sp_graph = graph.unpack<RccGraph>();
  auto p_sched = (RccServer*) dtxn_sched_;
  // last use — unwrap() intentionally moves the Arc out.
  *res = p_sched->OnCommit(cmd_id, rank, need_validation, sp_graph.unwrap(), output);
  defer.reply();
}

void ClassicServiceImpl::RccCommit(const cmdid_t& cmd_id,
                                   const rank_t& rank,
                                   const int32_t& need_validation,
                                   const parent_set_t& parents,
                                   int32_t* res,
                                   TxnOutput* output,
                                   rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  auto p_sched = (RccServer*) dtxn_sched_;
  *res = p_sched->OnCommit(cmd_id, rank, need_validation, parents, output);
  defer.reply();
}

void ClassicServiceImpl::JanusCommitWoGraph(const cmdid_t& cmd_id,
                                            const rank_t& rank,
                                            const int32_t& need_validation,
                                            int32_t* res,
                                            TxnOutput* output,
                                            rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  verify(0);
  auto sched = (SchedulerJanus*) dtxn_sched_;
  *res = sched->OnCommit(cmd_id, rank, need_validation,
                         rusty::Arc<RccGraph>::make(), output);
  defer.reply();
}

void ClassicServiceImpl::JanusInquire(const epoch_t& epoch,
                                      const cmdid_t& tid,
                                      rrr::AnyMessage* p_md_graph,
                                      rrr::DeferredReply defer) {
  verify(0);
//  std::lock_guard<std::mutex> guard(mtx_);
//  *p_md_graph = std::make_shared<RccGraph>();
//  auto p_sched = (SchedulerJanus*) dtxn_sched_;
//  p_sched->OnInquire(epoch,
//                     tid,
//                     dynamic_pointer_cast<RccGraph>(p_md_graph->sp_data_));
//  defer.reply();
}

void ClassicServiceImpl::RccPreAccept(const cmdid_t& txnid,
                                      const rank_t& rank,
                                      const vector<SimpleCommand>& cmds,
                                      int32_t* res,
                                      parent_set_t* res_parents,
                                      rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  auto sched = (RccServer*) dtxn_sched_;
  *res = sched->OnPreAccept(txnid, rank, cmds, *res_parents);
  defer.reply();
}

void ClassicServiceImpl::JanusPreAccept(const cmdid_t& txnid,
                                        const rank_t& rank,
                                        const vector<SimpleCommand>& cmds,
                                        const rrr::AnyMessage& md_graph,
                                        int32_t* res,
                                        rrr::AnyMessage* p_md_res_graph,
                                        rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  auto ret_sp_graph = rusty::Arc<RccGraph>::make();
  // aliased: OnPreAccept's fills through the shared payload stay
  // visible to the packed reply.
  *p_md_res_graph = rrr::AnyMessage::pack(ret_sp_graph.clone());
  auto sp_graph = md_graph.unpack<RccGraph>();
  verify(sp_graph.is_some());
  auto sched = (SchedulerJanus*) dtxn_sched_;
  // last use — unwrap() intentionally moves the Arc out.
  *res = sched->OnPreAccept(txnid, rank, cmds, sp_graph.unwrap(),
                            std::move(ret_sp_graph));
  defer.reply();
}

void ClassicServiceImpl::JanusPreAcceptWoGraph(const cmdid_t& txnid,
                                               const rank_t& rank,
                                               const vector<SimpleCommand>& cmds,
                                               int32_t* res,
                                               rrr::AnyMessage* res_graph,
                                               rrr::DeferredReply defer) {
//  std::lock_guard<std::mutex> guard(mtx_);
  auto sp_ret_graph = rusty::Arc<RccGraph>::make();
  // aliased: OnPreAccept's fills through the shared payload stay
  // visible to the packed reply.
  *res_graph = rrr::AnyMessage::pack(sp_ret_graph.clone());
  auto* p_sched = (SchedulerJanus*) dtxn_sched_;
  *res = p_sched->OnPreAccept(txnid, rank, cmds,
                              rusty::Arc<RccGraph>::make(),
                              std::move(sp_ret_graph));
  defer.reply();
}

void ClassicServiceImpl::RccAccept(const cmdid_t& txnid,
                                   const rank_t& rank,
                                   const ballot_t& ballot,
                                   const parent_set_t& parents,
                                   int32_t* res,
                                   rrr::DeferredReply defer) {
  auto sched = (RccServer*) dtxn_sched_;
  *res = sched->OnAccept(txnid, rank, ballot, parents);
  defer.reply();
}

void ClassicServiceImpl::JanusAccept(const cmdid_t& txnid,
                                     const int32_t& rank,
                                     const ballot_t& ballot,
                                     const rrr::AnyMessage& md_graph,
                                     int32_t* res,
                                     rrr::DeferredReply defer) {
  // graph rides directly as AnyMessage.
  auto graph = md_graph.unpack<RccGraph>();
  verify(graph.is_some());
  auto sched = (SchedulerJanus*) dtxn_sched_;
  // last use — unwrap() intentionally moves the Arc out.
  sched->OnAccept(txnid, rank, ballot, graph.unwrap(), res);
  defer.reply();
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
  *cmd_batch = rusty::Arc<KeyCmdBatchData>::make(std::move(batch_result));
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
  *cmd = rusty::Arc<TpcCommitCommand>::make();
  dtxn_sched()->OnJetpackPullRecSetIns(jepoch, oepoch, sid, rid, ok, reply_jepoch, reply_oepoch, reply_old_view, reply_new_view);
  defer.reply();
}

 void ClassicServiceImpl::JetpackFinishRecovery(const epoch_t& oepoch,
                                                rrr::DeferredReply defer) {
  dtxn_sched()->OnJetpackFinishRecovery(oepoch);
  defer.reply();
}


void ClassicServiceImpl::RegisterStats() {
  auto& registry = StatsRegistry::instance();
  // removed `registry.set_recorder(recorder_);`
  // — `Service::recorder_` field is gone; `StatsRegistry::set_recorder`
  // method removed in this phase too.
  registry.set_stat(StatsRegistry::STAT_SZ_SCC, &stat_sz_scc_);
  registry.set_stat(StatsRegistry::STAT_SZ_GRAPH_START, &stat_sz_gra_start_);
  registry.set_stat(StatsRegistry::STAT_SZ_GRAPH_COMMIT, &stat_sz_gra_commit_);
  registry.set_stat(StatsRegistry::STAT_SZ_GRAPH_ASK, &stat_sz_gra_ask_);
  registry.set_stat(StatsRegistry::STAT_N_ASK, &stat_n_ask_);
  registry.set_stat(StatsRegistry::STAT_RO6_SZ_VECTOR, &stat_ro6_sz_vector_);
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
