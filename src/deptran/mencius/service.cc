
#include "service.h"
#include "server.h"
#include "../RW_command.h"

namespace janus {

MenciusServiceImpl::MenciusServiceImpl(TxLogServer *sched)
    : sched_((MenciusServer*)sched) {

}

void MenciusServiceImpl::Prepare(const MenciusService::RpcPrepareRequest& req, MenciusService::RpcPrepareResponse& resp, rrr::DeferredReply defer) {
  this->Prepare(req.slot, req.ballot, &resp.max_ballot, &resp.coro_id, std::move(defer));
}

void MenciusServiceImpl::Suggest(const MenciusService::RpcSuggestRequest& req, MenciusService::RpcSuggestResponse& resp, rrr::DeferredReply defer) {
  this->Suggest(req.slot, req.time, req.ballot, req.sender, req.skip_commits, req.skip_potentials, req.cmd, &resp.max_ballot, &resp.coro_id, std::move(defer));
}

void MenciusServiceImpl::Decide(const MenciusService::RpcDecideRequest& req, MenciusService::RpcDecideResponse& resp, rrr::DeferredReply defer) {
  (void)resp;
  this->Decide(req.slot, req.ballot, req.cmd, std::move(defer));
}

void MenciusServiceImpl::Prepare(const uint64_t& slot,
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

void MenciusServiceImpl::Suggest(const uint64_t& slot,
                                 const uint64_t& time,
                                 const ballot_t& ballot,
                                 const uint64_t& sender,
                                 const std::vector<uint64_t>& skip_commits,
                                 const std::vector<uint64_t>& skip_potentials,
                                 const MarshallDeputy& cmd,
                                 ballot_t* max_ballot,
                                 uint64_t* coro_id,
                                 rrr::DeferredReply defer) {
  verify(sched_ != nullptr);

  sched_->g_mutex.lock();
  int n = Config::GetConfig()->GetPartitionSize(sched_->partition_id_);
  if (skip_potentials.size() > 100) {
    sched_->skip_potentials_recd[(slot - 1) % n].clear();
    for (auto x: skip_potentials) {
      sched_->skip_potentials_recd[(slot - 1) % n].insert(x);
    }
  }
  for (auto x: skip_commits) {
    auto cmd_ptr = std::make_shared<TpcCommitCommand>();
    MarshallDeputy md(cmd_ptr);
    md.kind_ = MarshallDeputy::CMD_TPC_COMMIT;
    sched_->OnCommit(x, 100, md.inner(), true);
  }
  sched_->g_mutex.unlock();

  Fiber::create_run([this,
                     slot,
                     time,
                     ballot,
                     sender,
                     skip_commits,
                     skip_potentials,
                     cmd,
                     max_ballot,
                     coro_id,
                     defer = std::move(defer)]() mutable {
    sched_->OnSuggest(
        slot,
        time,
        ballot,
        sender,
        skip_commits,
        skip_potentials,
        const_cast<MarshallDeputy&>(cmd).inner(),
        max_ballot,
        coro_id,
        [defer = std::move(defer)]() mutable { defer.reply(); });
  });
}

void MenciusServiceImpl::Decide(const uint64_t& slot,
                                const ballot_t& ballot,
                                const MarshallDeputy& cmd,
                                rrr::DeferredReply defer) {
  verify(sched_ != nullptr);
  auto x = cmd.inner();

  SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd.inner());
  sched_->c_mutex.lock();
  sched_->unexecuted_keys_[parsed_cmd.key_] += 1;
  sched_->c_mutex.unlock();

  sched_->OnCommit(slot, ballot, x);
  defer.reply();
}


} // namespace janus;
