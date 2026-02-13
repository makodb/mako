
#include "../../rrr/misc/marshal.hpp"
#include "service.h"
#include "server.h"

// @external: {
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   clock_gettime: [safe, (...) -> int]
//   srand: [safe, (...) -> void]
//   rrr::Fiber::create_run: [safe, (...) -> owned]
// }

namespace janus {

// @safe - C-style cast in @unsafe block, clock_gettime/srand marked @external [safe]
RaftServiceImpl::RaftServiceImpl(TxLogServer *sched)
    // @unsafe
    : svr_((RaftServer*)sched) {
	struct timespec curr_time;
	clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
	srand(curr_time.tv_nsec);
}

// @safe - svr_ pointer is bounded (set in constructor), external calls marked @external
void RaftServiceImpl::HandleVote(const uint64_t& lst_log_idx,
                                    const ballot_t& lst_log_term,
                                    const siteid_t& can_id,
                                    const ballot_t& can_term,
                                    ballot_t* reply_term,
                                    bool_t *vote_granted,
                                    rrr::DeferredReply defer) {
  verify(svr_ != nullptr);
  svr_->OnRequestVote(lst_log_idx,lst_log_term, can_id, can_term,
                    reply_term, vote_granted,
                    [defer = std::move(defer)]() mutable { defer.reply(); });
}

// @safe - svr_ pointer is bounded, Fiber::create_run marked @external [safe]
void RaftServiceImpl::HandleAppendEntries(const uint64_t& slot,
                                        const ballot_t& ballot,
                                        const uint64_t& leaderCurrentTerm,
                                        const siteid_t& leaderSiteId,
                                        const uint64_t& leaderPrevLogIndex,
                                        const uint64_t& leaderPrevLogTerm,
                                        const uint64_t& leaderCommitIndex,
                                        const MarshallDeputy& md_cmd,
                                        const uint64_t& leaderNextLogTerm,
                                        uint64_t *followerAppendOK,
                                        uint64_t *followerCurrentTerm,
                                        uint64_t *followerLastLogIndex,
                                        rrr::DeferredReply defer) {
  verify(svr_ != nullptr);

  Fiber::create_run([=, defer = std::move(defer)]() mutable {
    svr_->OnAppendEntries(slot,
                            ballot,
                            leaderCurrentTerm,
                            leaderSiteId,
                            leaderPrevLogIndex,
                            leaderPrevLogTerm,
                            leaderCommitIndex,
                            const_cast<MarshallDeputy&>(md_cmd).sp_data_,
                            leaderNextLogTerm,
                            followerAppendOK,
                            followerCurrentTerm,
                            followerLastLogIndex,
                            [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

// @safe - svr_ pointer is bounded, Fiber::create_run marked @external [safe]
void RaftServiceImpl::HandleEmptyAppendEntries(const uint64_t& slot,
                                             const ballot_t& ballot,
                                             const uint64_t& leaderCurrentTerm,
                                             const siteid_t& leaderSiteId,
                                             const uint64_t& leaderPrevLogIndex,
                                             const uint64_t& leaderPrevLogTerm,
                                             const uint64_t& leaderCommitIndex,
                                             const bool_t& trigger_election_now,
                                             uint64_t *followerAppendOK,
                                             uint64_t *followerCurrentTerm,
                                             uint64_t *followerLastLogIndex,
                                             rrr::DeferredReply defer) {
  std::shared_ptr<Marshallable> cmd = nullptr;
  Fiber::create_run([=, defer = std::move(defer)]() mutable {
    svr_->OnAppendEntries(slot,
                            ballot,
                            leaderCurrentTerm,
                            leaderSiteId,
                            leaderPrevLogIndex,
                            leaderPrevLogTerm,
                            leaderCommitIndex,
                            cmd,
                            0,
                            followerAppendOK,
                            followerCurrentTerm,
                            followerLastLogIndex,
                            [defer = std::move(defer)]() mutable { defer.reply(); },
                            trigger_election_now);
  });
}

// @safe - svr_ pointer is bounded, external calls marked @external [safe]
void RaftServiceImpl::HandleTimeoutNow(const uint64_t& leaderTerm,
                                        const siteid_t& leaderSiteId,
                                        uint64_t* followerTerm,
                                        bool_t* success,
                                        rrr::DeferredReply defer) {
  verify(svr_ != nullptr);
  svr_->OnTimeoutNow(leaderTerm, leaderSiteId, followerTerm, success,
                     [defer = std::move(defer)]() mutable { defer.reply(); });
}

} // namespace janus;
