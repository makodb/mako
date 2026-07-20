#include "service.h"
#include "server.h"

namespace janus {

CopilotServiceImpl::CopilotServiceImpl(TxLogServer *sched)
    : sched_((CopilotServer *)sched) {
}

// removed `Forward` typed-rpc override
// (and matching N-arg overload further below).

void CopilotServiceImpl::Prepare(const CopilotService::RpcPrepareRequest& req, CopilotService::RpcPrepareResponse& resp, rrr::DeferredReply defer) {
  this->Prepare(req.is_pilot, req.slot, req.ballot, req.dep_id, &resp.ret_cmd, &resp.max_ballot, &resp.dep, &resp.status, std::move(defer));
}

void CopilotServiceImpl::FastAccept(const CopilotService::RpcFastAcceptRequest& req, CopilotService::RpcFastAcceptResponse& resp, rrr::DeferredReply defer) {
  this->FastAccept(req.is_pilot, req.slot, req.ballot, req.dep, req.cmd, req.dep_id, &resp.max_ballot, &resp.ret_dep, std::move(defer));
}

void CopilotServiceImpl::Accept(const CopilotService::RpcAcceptRequest& req, CopilotService::RpcAcceptResponse& resp, rrr::DeferredReply defer) {
  this->Accept(req.is_pilot, req.slot, req.ballot, req.dep, req.cmd, req.dep_id, &resp.max_ballot, std::move(defer));
}

void CopilotServiceImpl::Commit(const CopilotService::RpcCommitRequest& req, CopilotService::RpcCommitResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->Commit(req.is_pilot, req.slot, req.dep, req.cmd, std::move(defer));
}

// removed `Forward(janus::Command, ...)`
// N-arg overload — only caller was the deleted typed-rpc shim above;
// the receiver `CopilotServer::OnForward` is also gone.

void CopilotServiceImpl::Prepare(const uint8_t& is_pilot,
                                 const uint64_t& slot,
                                 const ballot_t& ballot,
                                 const DepId& dep_id,
                                 janus::Command* ret_cmd,
                                 ballot_t* max_ballot,
                                 uint64_t* dep,
                                 status_t* status,
                                 rrr::DeferredReply defer) {
  verify(sched_);
  sched_->OnPrepare(is_pilot,
                    slot,
                    ballot,
                    dep_id,
                    ret_cmd,
                    max_ballot,
                    dep,
                    status,
                    [defer = std::move(defer)]() mutable { defer.reply(); });
}

void CopilotServiceImpl::FastAccept(const uint8_t& is_pilot,
                                    const uint64_t& slot,
                                    const ballot_t& ballot,
                                    const uint64_t& dep,
                                    const janus::Command& cmd,
                                    const DepId& dep_id,
                                    ballot_t* max_ballot,
                                    uint64_t* ret_dep,
                                    rrr::DeferredReply defer) {
  verify(sched_);
#ifdef COPILOT_TIME_DEBUG
  struct timeval tp;
  gettimeofday(&tp, NULL);
  Log_info("[1+] [tx=%d] on FastAccept %.3f",
           marshallable_cast<TpcBatchCommand>(cmd).unwrap()->cmds_.at(0)->tx_id_,
           tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif
  sched_->OnFastAccept(is_pilot,
                       slot,
                       ballot,
                       dep,
                       cmd,
                       dep_id,
                       max_ballot,
                       ret_dep,
                       [defer = std::move(defer)]() mutable { defer.reply(); });
}

void CopilotServiceImpl::Accept(const uint8_t& is_pilot,
                                const uint64_t& slot,
                                const ballot_t& ballot,
                                const uint64_t& dep,
                                const janus::Command& cmd,
                                const DepId& dep_id,
                                ballot_t* max_ballot,
                                rrr::DeferredReply defer) {
  verify(sched_);
  sched_->OnAccept(is_pilot,
                   slot,
                   ballot,
                   dep,
                   cmd,
                   dep_id,
                   max_ballot,
                   [defer = std::move(defer)]() mutable { defer.reply(); });
}

void CopilotServiceImpl::Commit(const uint8_t& is_pilot,
                                const uint64_t& slot,
                                const uint64_t& dep,
                                const janus::Command& cmd,
                                rrr::DeferredReply defer) {
  verify(sched_);
  sched_->OnCommit(is_pilot, slot, dep, cmd);
  defer.reply();
}

} // namespace janus
