#include "service.h"
#include "server.h"

namespace janus {

CopilotServiceImpl::CopilotServiceImpl(TxLogServer *sched)
    : sched_((CopilotServer *)sched) {
}

void CopilotServiceImpl::Forward(const MarshallDeputy& cmd,
                                 rrr::DeferredReply defer) {
  verify(sched_);
  auto coro = Fiber::create_run([&]() {
    sched_->OnForward(const_cast<MarshallDeputy&>(cmd).sp_data_,
                      [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

void CopilotServiceImpl::Prepare(const uint8_t& is_pilot,
                                 const uint64_t& slot,
                                 const ballot_t& ballot,
                                 const struct DepId& dep_id,
                                 MarshallDeputy* ret_cmd,
                                 ballot_t* max_ballot,
                                 uint64_t* dep,
                                 status_t* status,
                                 rrr::DeferredReply defer) {
  verify(sched_);
  sched_->OnPrepare(is_pilot, slot,
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
                                    const MarshallDeputy& cmd,
                                    const struct DepId& dep_id,
                                    ballot_t* max_ballot,
                                    uint64_t* ret_dep,
                                    rrr::DeferredReply defer) {
  verify(sched_);

#ifdef COPILOT_TIME_DEBUG
  struct timeval tp;
  gettimeofday(&tp, NULL);
  Log_info("[1+] [tx=%d] on FastAccept %.3f", dynamic_pointer_cast<TpcBatchCommand>(const_cast<MarshallDeputy&>(cmd).sp_data_)->cmds_.at(0)->tx_id_, tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif

  // auto coro = Fiber::create_run([&]() {
    sched_->OnFastAccept(is_pilot, slot,
                         ballot,
                         dep,
                         const_cast<MarshallDeputy&>(cmd).sp_data_,
                         dep_id,
                         max_ballot,
                         ret_dep,
                         [defer = std::move(defer)]() mutable { defer.reply(); });
  // });
}

void CopilotServiceImpl::Accept(const uint8_t& is_pilot,
                                const uint64_t& slot,
                                const ballot_t& ballot,
                                const uint64_t& dep,
                                const MarshallDeputy& cmd,
                                const struct DepId& dep_id,
                                ballot_t* max_ballot,
                                rrr::DeferredReply defer) {
  verify(sched_);

  // auto coro = Fiber::create_run([&]() {
    sched_->OnAccept(is_pilot, slot,
                     ballot,
                     dep,
                     const_cast<MarshallDeputy&>(cmd).sp_data_,
                     dep_id,
                     max_ballot,
                     [defer = std::move(defer)]() mutable { defer.reply(); });
  // });
}

void CopilotServiceImpl::Commit(const uint8_t& is_pilot,
                                const uint64_t& slot,
                                const uint64_t& dep,
                                const MarshallDeputy& cmd,
                                rrr::DeferredReply defer) {
  verify(sched_);
  // Fiber::create_run([&]() {
    sched_->OnCommit(is_pilot, slot, dep,
                     const_cast<MarshallDeputy&>(cmd).sp_data_);
    defer.reply();
  // });
}

} // namespace janus
