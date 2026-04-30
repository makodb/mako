
#include "service.h"
#include "server.h"
#include "../paxos_worker.h"

namespace janus {
void MultiPaxosServiceImpl::Forward(const MultiPaxosService::RpcForwardRequest& req, MultiPaxosService::RpcForwardResponse& resp, rrr::DeferredReply defer) {
  this->Forward(req.cmd, req.dep_id, &resp.coro_id, std::move(defer));
}

void MultiPaxosServiceImpl::Prepare(const MultiPaxosService::RpcPrepareRequest& req, MultiPaxosService::RpcPrepareResponse& resp, rrr::DeferredReply defer) {
  this->Prepare(req.slot, req.ballot, &resp.max_ballot, &resp.coro_id, std::move(defer));
}

void MultiPaxosServiceImpl::Accept(const MultiPaxosService::RpcAcceptRequest& req, MultiPaxosService::RpcAcceptResponse& resp, rrr::DeferredReply defer) {
  this->Accept(req.slot, req.time, req.ballot, req.cmd, &resp.max_ballot, &resp.coro_id, std::move(defer));
}

void MultiPaxosServiceImpl::Decide(const MultiPaxosService::RpcDecideRequest& req, MultiPaxosService::RpcDecideResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->Decide(req.slot, req.ballot, req.cmd, std::move(defer));
}

// Workstream N Phase 4e-26: removed `BulkPrepare` and `Heartbeat`
// typed-rpc overrides (and matching N-arg overloads further below).

// Workstream N Phase 4e-27: removed `BulkPrepare2` typed-rpc override
// (and matching N-arg overload further below).

void MultiPaxosServiceImpl::BulkAccept(const MultiPaxosService::RpcBulkAcceptRequest& req, MultiPaxosService::RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) {
  this->BulkAccept(req.cmd, &resp.ballot, &resp.val, std::move(defer));
}

void MultiPaxosServiceImpl::BulkDecide(const MultiPaxosService::RpcBulkDecideRequest& req, MultiPaxosService::RpcBulkDecideResponse& resp, rrr::DeferredReply defer) {
  this->BulkDecide(req.cmd, &resp.ballot, &resp.val, std::move(defer));
}

void MultiPaxosServiceImpl::SyncLog(const MultiPaxosService::RpcSyncLogRequest& req, MultiPaxosService::RpcSyncLogResponse& resp, rrr::DeferredReply defer) {
  this->SyncLog(req.cmd, &resp.ballot, &resp.val, &resp.ret, std::move(defer));
}

void MultiPaxosServiceImpl::SyncCommit(const MultiPaxosService::RpcSyncCommitRequest& req, MultiPaxosService::RpcSyncCommitResponse& resp, rrr::DeferredReply defer) {
  this->SyncCommit(req.cmd, &resp.ballot, &resp.val, std::move(defer));
}

// Workstream N Phase 4e-26: removed `SyncNoOps` typed-rpc override
// (and matching N-arg overload further below).

void MultiPaxosServiceImpl::ForwardToLearnerServer(const MultiPaxosService::RpcForwardToLearnerServerRequest& req, MultiPaxosService::RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) {
  this->ForwardToLearnerServer(req.par_id, req.slot, req.ballot, req.cmd, &resp.ret_slot, &resp.ret_ballot, std::move(defer));
}

MultiPaxosServiceImpl::MultiPaxosServiceImpl(TxLogServer *sched)
    : sched_((PaxosServer*)sched) {

}

void MultiPaxosServiceImpl::Forward(const MarshallDeputy& md_cmd,
                                    const uint64_t& dep_id,
                                    uint64_t* coro_id,
                                    rrr::DeferredReply defer) {
  // NOTE: Original Mako leaves this empty - Mako uses ForwardToLearner instead
}

void MultiPaxosServiceImpl::Prepare(const uint64_t& slot,
                                    const ballot_t& ballot,
                                    ballot_t* max_ballot,
                                    uint64_t* coro_id,
                                    rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  sched_->OnPrepare(slot,
                    ballot,
                    max_ballot,
                    coro_id,
                    [defer = std::move(defer)]() mutable { defer.reply(); });
}

void MultiPaxosServiceImpl::Accept(const uint64_t& slot,
		                   const uint64_t& time,
                                   const ballot_t& ballot,
                                   const MarshallDeputy& md_cmd,
                                   ballot_t* max_ballot,
                                   uint64_t* coro_id,
                                   rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  auto start = chrono::system_clock::now();

  time_t tstart = chrono::system_clock::to_time_t(start);
  tm * date = localtime(&tstart);
  date->tm_hour = 0;
  date->tm_min = 0;
  date->tm_sec = 0;
  auto midn = chrono::system_clock::from_time_t(std::mktime(date));

  auto hours = chrono::duration_cast<chrono::hours>(start-midn);
  auto minutes = chrono::duration_cast<chrono::minutes>(start-midn);
  auto seconds = chrono::duration_cast<chrono::seconds>(start-midn);

  auto start_ = chrono::duration_cast<chrono::microseconds>(start-midn-hours-minutes).count();
  //Log_info("Duration of RPC is: %d", start_-time);

  auto coro = Fiber::create_run([&] () {
    sched_->OnAccept(slot,
		     time,
                     ballot,
                     const_cast<MarshallDeputy&>(md_cmd).inner(),
                     max_ballot,
                     coro_id,
                     [defer = std::move(defer)]() mutable { defer.reply(); });

  });

  auto end = chrono::system_clock::now();
  auto duration = chrono::duration_cast<chrono::microseconds>(end-start);
  //Log_info("Duration of Accept() at Follower's side is: %d", duration.count());
  //Log_info("coro id on service side: %d", coro->id);
}

void MultiPaxosServiceImpl::Decide(const uint64_t& slot,
                                   const ballot_t& ballot,
                                   const MarshallDeputy& md_cmd,
                                   rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  auto x = md_cmd.inner();
  sched_->OnCommit(slot, ballot,x);
  defer.reply();
}


// Workstream N Phase 4e-26: removed `BulkPrepare(MarshallDeputy, ...)`
// and `Heartbeat(MarshallDeputy, ...)` N-arg overloads — only callers
// were the now-deleted typed-rpc shims; the corresponding
// `PaxosServer::OnBulkPrepare` / `OnHeartbeat` impls are also gone.

// Workstream N Phase 4e-27: removed `BulkPrepare2(MarshallDeputy, ...)`
// N-arg overload — only caller was the deleted typed-rpc shim above;
// the corresponding `PaxosServer::OnBulkPrepare2` impl is also gone.

void MultiPaxosServiceImpl::BulkAccept(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  Fiber::create_run([&] () {
    sched_->OnBulkAccept(const_cast<MarshallDeputy&>(md_cmd).inner(),
                         ballot,
                         valid,
                        [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

void MultiPaxosServiceImpl::BulkDecide(const MarshallDeputy& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  // Log_info("BulkDecide RPC handler called");
  Fiber::create_run([&] () {
    // Log_info("BulkDecide coroutine executing, calling OnBulkCommit");
    sched_->OnBulkCommit(const_cast<MarshallDeputy&>(md_cmd).inner(),
                         ballot,
                         valid,
                         [defer = std::move(defer)]() mutable { defer.reply(); });
    // Log_info("BulkDecide coroutine finished");
    //defer.reply();
  });
  // Log_info("BulkDecide RPC handler returning");
}

void MultiPaxosServiceImpl::SyncLog(const MarshallDeputy& md_cmd,
                                     i32* ballot,
                                     i32* valid,
                                     MarshallDeputy* ret,
                                     rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  ret->set_marshallable(std::make_shared<SyncLogResponse>());
  auto response = marshallable_cast<SyncLogResponse>(ret);
  Fiber::create_run([&] () {
    sched_->OnSyncLog(const_cast<MarshallDeputy&>(md_cmd).inner(),
                      ballot,
                      valid,
                      response,
                      [defer = std::move(defer)]() mutable { defer.reply(); });
  //  auto rpx = dynamic_pointer_cast<SyncLogResponse>(ret->sp_data_);
  //   auto xx = (int32_t)rpx->missing_slots.size();
  //   Log_info("received a OnSyncLog2,xxx: %d",xx);
  //   for(int i = 0; i < rpx->missing_slots.size(); i++){
  //      Log_info("yy2: %d", (int32_t)rpx->missing_slots[i].size());
  //      for(int j = 0; j < rpx->missing_slots[i].size(); j++){
  //         Log_info("yy2 a OnSyncLog2,xxx: %d",j);
  //      }
  //   }
    defer.reply();
  });

}

void MultiPaxosServiceImpl::SyncCommit(const MarshallDeputy& md_cmd,
                                     i32* ballot,
                                     i32* valid,
                                     rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  Fiber::create_run([&] () {
    sched_->OnSyncCommit(const_cast<MarshallDeputy&>(md_cmd).inner(),
                         ballot,
                         valid,
                         [defer = std::move(defer)]() mutable { defer.reply(); });
    //defer.reply();
  });
}

// Workstream N Phase 4e-26: removed `SyncNoOps(MarshallDeputy, ...)`
// N-arg overload — only caller was the deleted typed-rpc shim above.

void MultiPaxosServiceImpl::ForwardToLearnerServer(const rrr::i32& par_id,
                                                   const uint64_t& slot, 
                                                   const ballot_t& ballot, /* slot and ballot from the leader */
                                                   const MarshallDeputy& cmd, 
                                                   uint64_t* ret_slot, ballot_t* ret_ballot, rrr::DeferredReply defer) {
    verify(sched_ != nullptr);
    *ret_slot = slot;
    *ret_ballot = ballot;
    Fiber::create_run([&] () {
      sched_->OnForwardToLearner(par_id, slot, ballot, const_cast<MarshallDeputy&>(cmd).inner(),
                               [defer = std::move(defer)]() mutable { defer.reply(); });
    });
}


} // namespace janus;
