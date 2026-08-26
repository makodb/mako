
#include "service.h"
#include "server.h"
#include "../paxos_worker.h"

namespace janus {
void MultiPaxosServiceImpl::BulkAccept(const MultiPaxosService::RpcBulkAcceptRequest& req, MultiPaxosService::RpcBulkAcceptResponse& resp, srpc::DeferredReply defer) {
  this->BulkAccept(req.cmd, &resp.ballot, &resp.val, std::move(defer));
}

void MultiPaxosServiceImpl::BulkDecide(const MultiPaxosService::RpcBulkDecideRequest& req, MultiPaxosService::RpcBulkDecideResponse& resp, srpc::DeferredReply defer) {
  this->BulkDecide(req.cmd, &resp.ballot, &resp.val, std::move(defer));
}

void MultiPaxosServiceImpl::SyncLog(const MultiPaxosService::RpcSyncLogRequest& req, MultiPaxosService::RpcSyncLogResponse& resp, srpc::DeferredReply defer) {
  this->SyncLog(req.cmd, &resp.ballot, &resp.val, &resp.ret, std::move(defer));
}

void MultiPaxosServiceImpl::ForwardToLearnerServer(const MultiPaxosService::RpcForwardToLearnerServerRequest& req, MultiPaxosService::RpcForwardToLearnerServerResponse& resp, srpc::DeferredReply defer) {
  this->ForwardToLearnerServer(req.par_id, req.slot, req.ballot, req.cmd, &resp.ret_slot, &resp.ret_ballot, std::move(defer));
}

MultiPaxosServiceImpl::MultiPaxosServiceImpl(PaxosServer *sched)
    : sched_(sched) {

}

void MultiPaxosServiceImpl::BulkAccept(const janus::Command& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  Fiber::create_run([&] () {
    sched_->OnBulkAccept(md_cmd,
                         ballot,
                         valid,
                        [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

void MultiPaxosServiceImpl::BulkDecide(const janus::Command& md_cmd,
                                       i32* ballot,
                                       i32* valid,
                                       srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  // Log_info("BulkDecide RPC handler called");
  Fiber::create_run([&] () {
    // Log_info("BulkDecide coroutine executing, calling OnBulkCommit");
    sched_->OnBulkCommit(md_cmd,
                         ballot,
                         valid,
                         [defer = std::move(defer)]() mutable { defer.reply(); });
    // Log_info("BulkDecide coroutine finished");
    //defer.reply();
  });
  // Log_info("BulkDecide RPC handler returning");
}

void MultiPaxosServiceImpl::SyncLog(const janus::Command& md_cmd,
                                     i32* ballot,
                                     i32* valid,
                                     janus::Command* ret,
                                     srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  // Default reply payload (the OnSyncLog early-return path keeps it) —
  // byte-identical to the pre-reshape empty pack.
  *ret = rusty::Arc<SyncLogResponse>::make();
  Fiber::create_run([&, defer = std::move(defer)] () mutable {
    // Fill-then-wrap: the response lives on the fiber stack and is
    // packed into *ret only after OnSyncLog completes; the reply then
    // fires explicitly. (Previously the response was packed EMPTY up
    // front and filled through the packed handle, and the reply fired
    // from the dead cb-lambda's DeferredReply destructor.)
    SyncLogResponse response;
    sched_->OnSyncLog(md_cmd, ballot, valid, response);
    if (*valid == 1) {
      *ret = rusty::Arc<SyncLogResponse>::make(std::move(response));
    }
    defer.reply();
  });
}

void MultiPaxosServiceImpl::ForwardToLearnerServer(const srpc::i32& par_id,
                                                   const uint64_t& slot, 
                                                   const ballot_t& ballot, /* slot and ballot from the leader */
                                                   const janus::Command& cmd, 
                                                   uint64_t* ret_slot, ballot_t* ret_ballot, srpc::DeferredReply defer) {
    verify(sched_ != nullptr);
    *ret_slot = slot;
    *ret_ballot = ballot;
    Fiber::create_run([&] () {
      sched_->OnForwardToLearner(par_id, slot, ballot, cmd,
                               [defer = std::move(defer)]() mutable { defer.reply(); });
    });
}


} // namespace janus;
