
#include "service.h"
#include "server.h"

namespace janus {

FpgaRaftServiceImpl::FpgaRaftServiceImpl(TxLogServer *sched)
    : sched_((FpgaRaftServer*)sched) {
	struct timespec curr_time;
	clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
	srand(curr_time.tv_nsec);
}

void FpgaRaftServiceImpl::Heartbeat(const FpgaRaftService::RpcHeartbeatRequest& req, FpgaRaftService::RpcHeartbeatResponse& resp, srpc::DeferredReply defer) {
  this->Heartbeat(req.leaderPrevLogIndex, req.dep_id, &resp.followerPrevLogIndex, std::move(defer));
}

// removed `Forward` typed-rpc override
// (and matching N-arg overload further below).

void FpgaRaftServiceImpl::Vote(const FpgaRaftService::RpcVoteRequest& req, FpgaRaftService::RpcVoteResponse& resp, srpc::DeferredReply defer) {
  this->Vote(req.lst_log_idx, req.lst_log_term, req.par_id, req.cur_term, &resp.max_ballot, &resp.vote_granted, std::move(defer));
}

void FpgaRaftServiceImpl::Vote2FPGA(const FpgaRaftService::RpcVote2FPGARequest& req, FpgaRaftService::RpcVote2FPGAResponse& resp, srpc::DeferredReply defer) {
  this->Vote2FPGA(req.lst_log_idx, req.lst_log_term, req.par_id, req.cur_term, &resp.max_ballot, &resp.vote_granted, std::move(defer));
}

void FpgaRaftServiceImpl::AppendEntries2(const FpgaRaftService::RpcAppendEntries2Request& req, FpgaRaftService::RpcAppendEntries2Response& resp, srpc::DeferredReply defer) {
  this->AppendEntries2(req.slot, req.ballot, req.leaderCurrentTerm, req.leaderPrevLogIndex, req.leaderPrevLogTerm, req.leaderCommitIndex, req.dep_id, req.cmd, &resp.followerAppendOK, &resp.followerCurrentTerm, &resp.followerLastLogIndex, std::move(defer));
}

void FpgaRaftServiceImpl::AppendEntries(const FpgaRaftService::RpcAppendEntriesRequest& req, FpgaRaftService::RpcAppendEntriesResponse& resp, srpc::DeferredReply defer) {
  this->AppendEntries(req.slot, req.ballot, req.leaderCurrentTerm, req.leaderPrevLogIndex, req.leaderPrevLogTerm, req.leaderCommitIndex, req.dep_id, req.cmd, &resp.followerAppendOK, &resp.followerCurrentTerm, &resp.followerLastLogIndex, std::move(defer));
}

void FpgaRaftServiceImpl::Decide(const FpgaRaftService::RpcDecideRequest& req, FpgaRaftService::RpcDecideResponse& resp, srpc::DeferredReply defer) {
  (void)resp;
  this->Decide(req.slot, req.ballot, req.dep_id, req.cmd, std::move(defer));
}

void FpgaRaftServiceImpl::Heartbeat(const uint64_t& leaderPrevLogIndex,
                                    const DepId& dep_id,
                                    uint64_t* followerPrevLogIndex,
                                    srpc::DeferredReply defer) {
  (void)leaderPrevLogIndex;
  (void)dep_id;
  verify(sched_ != nullptr);
  *followerPrevLogIndex = sched_->lastLogIndex;
  defer.reply();
}

// removed `Forward(janus::Command, ...)`
// N-arg overload — only caller was the deleted typed-rpc shim
// above; the receiver `FpgaRaftServer::OnForward` is also gone.

void FpgaRaftServiceImpl::Vote(const uint64_t& lst_log_idx,
                               const ballot_t& lst_log_term,
                               const parid_t& par_id,
                               const ballot_t& cur_term,
                               ballot_t* max_ballot,
                               bool_t* vote_granted,
                               srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  sched_->OnVote(lst_log_idx,
                 lst_log_term,
                 par_id,
                 cur_term,
                 max_ballot,
                 vote_granted,
                 [defer = std::move(defer)]() mutable { defer.reply(); });
}

void FpgaRaftServiceImpl::Vote2FPGA(const uint64_t& lst_log_idx,
                                    const ballot_t& lst_log_term,
                                    const parid_t& par_id,
                                    const ballot_t& cur_term,
                                    ballot_t* max_ballot,
                                    bool_t* vote_granted,
                                    srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  sched_->OnVote2FPGA(lst_log_idx,
                      lst_log_term,
                      par_id,
                      cur_term,
                      max_ballot,
                      vote_granted,
                      [defer = std::move(defer)]() mutable { defer.reply(); });
}

void FpgaRaftServiceImpl::AppendEntries2(const uint64_t& slot,
                                         const ballot_t& ballot,
                                         const uint64_t& leaderCurrentTerm,
                                         const uint64_t& leaderPrevLogIndex,
                                         const uint64_t& leaderPrevLogTerm,
                                         const uint64_t& leaderCommitIndex,
                                         const DepId& dep_id,
                                         const janus::Command& cmd,
                                         uint64_t* followerAppendOK,
                                         uint64_t* followerCurrentTerm,
                                         uint64_t* followerLastLogIndex,
                                         srpc::DeferredReply defer) {
  (void)slot;
  (void)ballot;
  (void)leaderCurrentTerm;
  (void)leaderPrevLogIndex;
  (void)leaderPrevLogTerm;
  (void)leaderCommitIndex;
  (void)dep_id;
  (void)cmd;
  verify(sched_ != nullptr);
  *followerAppendOK = 1;
  *followerCurrentTerm = 0;
  *followerLastLogIndex = sched_->lastLogIndex;
  defer.reply();
}

void FpgaRaftServiceImpl::AppendEntries(const uint64_t& slot,
                                        const ballot_t& ballot,
                                        const uint64_t& leaderCurrentTerm,
                                        const uint64_t& leaderPrevLogIndex,
                                        const uint64_t& leaderPrevLogTerm,
                                        const uint64_t& leaderCommitIndex,
                                        const DepId& dep_id,
                                        const janus::Command& cmd,
                                        uint64_t* followerAppendOK,
                                        uint64_t* followerCurrentTerm,
                                        uint64_t* followerLastLogIndex,
                                        srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  Fiber::create_run([this,
                     slot,
                     ballot,
                     leaderCurrentTerm,
                     leaderPrevLogIndex,
                     leaderPrevLogTerm,
                     leaderCommitIndex,
                     dep_id,
                     cmd,
                     followerAppendOK,
                     followerCurrentTerm,
                     followerLastLogIndex,
                     defer = std::move(defer)]() mutable {
    sched_->OnAppendEntries(
        slot,
        ballot,
        leaderCurrentTerm,
        leaderPrevLogIndex,
        leaderPrevLogTerm,
        leaderCommitIndex,
        dep_id,
        cmd,
        followerAppendOK,
        followerCurrentTerm,
        followerLastLogIndex,
        [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

void FpgaRaftServiceImpl::Decide(const uint64_t& slot,
                                 const ballot_t& ballot,
                                 const DepId& dep_id,
                                 const janus::Command& cmd,
                                 srpc::DeferredReply defer) {
  verify(sched_ != nullptr);
  (void)dep_id;
  Fiber::create_run([this, slot, ballot, cmd, defer = std::move(defer)]() mutable {
    sched_->OnCommit(slot,
                     ballot,
                     cmd);
    defer.reply();
  });
}

} // namespace janus;
