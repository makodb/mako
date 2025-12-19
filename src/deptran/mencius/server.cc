

#include "server.h"
// #include "paxos_worker.h"
#include "exec.h"
#include "../RW_command.h"

namespace janus {

void MenciusServer::OnPrepare(slotid_t slot_id,
                            ballot_t ballot,
                            ballot_t *max_ballot,
                            uint64_t* coro_id,
                            const function<void()> &cb) {

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("mencius scheduler receives prepare for slot_id: %llx",
            slot_id);
  auto instance = GetInstance(slot_id);
  verify(ballot != instance->max_ballot_seen_);
  if (instance->max_ballot_seen_ < ballot) {
    instance->max_ballot_seen_ = ballot;
  } else {
    // TODO if accepted anything, return;
    verify(0);
  }
  auto coro_opt = Coroutine::CurrentCoroutine();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id;
  }
  *max_ballot = instance->max_ballot_seen_;
  n_prepare_++;
  WAN_WAIT
  cb();
}


void MenciusServer::OnSuggest(const slotid_t slot_id,
		                       const uint64_t time,
                           const ballot_t ballot,
                           const uint64_t sender,
                           const std::vector<uint64_t>& skip_commits, 
                           const std::vector<uint64_t>& skip_potentials,
                           shared_ptr<Marshallable> &cmd,
                           ballot_t *max_ballot,
                           uint64_t* coro_id,
                           const function<void()> &cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  //Log_info("mencius scheduler suggest for slot_id: %llu", slot_id);
  auto instance = GetInstance(slot_id);

  //TODO: might need to optimize this. we can vote yes on duplicates at least for now
  //verify(instance->max_ballot_suggested_ < ballot);
  
  if (instance->max_ballot_seen_ <= ballot) {
    instance->max_ballot_seen_ = ballot;
    instance->max_ballot_suggested_ = ballot;
    instance->cmd_ = cmd;
    max_active_slot_ = std::max(max_active_slot_, slot_id);
  } else {
    // TODO
    verify(0);
  }

  auto coro_opt = Coroutine::CurrentCoroutine();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id;
  }
  *max_ballot = instance->max_ballot_seen_;
  n_suggest_++;
  WAN_WAIT
  if (cb) {
    cb();
  }
}

void MenciusServer::OnCommit(const slotid_t slot_id,
                           const ballot_t ballot,
                           shared_ptr<Marshallable> &cmd,
                           bool is_skip) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  //Log_info("mencius scheduler decide for slot: %d on loc_id_:%d", slot_id, this->loc_id_);
  // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd);
  // Log_info("OnCommit loc_id_=%d cmd_id=<%d, %d>", loc_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second);
  auto instance = GetInstance(slot_id);
  instance->committed_cmd_ = cmd;
  instance->is_skip = true;
  if (instance->is_skip){
    instance->committed_cmd_->kind_ = MarshallDeputy::CMD_TPC_COMMIT;
  }
  if (slot_id > max_committed_slot_) {
    max_committed_slot_ = slot_id;
  }
  verify(slot_id > max_executed_slot_);
  // This prevents the log entry from being applied twice
  if (in_applying_logs_) {
    return;
  }
  in_applying_logs_ = true;

  // auto next_instance = GetInstance(slot_id);
  // app_next_(*next_instance->committed_cmd_);
  
#ifdef JETPACK_DEDUPLICATE_OPTIMIZATION
  // deduplicate optimization: Accepted duplicated cmd can be ignored
  for (slotid_t id = max_committed_slot_; id < max_active_slot_; id++) {
    auto next_instance = GetInstance(id);
    if (next_instance->cmd_ && witness_.has_appeared(next_instance->cmd_)) {
      max_committed_slot_++;
    }
  }
#endif

  slotid_t tmp_max_executed_slot_ = max_executed_slot_;
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
    auto next_instance = GetInstance(id);
    if (next_instance->committed_cmd_) {
      if (!next_instance->executed_){
        RuleWitnessGC(next_instance->committed_cmd_);
        app_next_(id, next_instance->committed_cmd_);
        next_instance->executed_ = true;

        SimpleRWCommand parsed_cmd = SimpleRWCommand(next_instance->committed_cmd_);
        c_mutex.lock();
        unexecuted_keys_[parsed_cmd.key_] -= 1;
        // Log_info("[-1] cmd %d %d cnt %d", parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second, unexecuted_keys_[parsed_cmd.key_].load());
        verify(unexecuted_keys_[parsed_cmd.key_]>=0);
        c_mutex.unlock();
      }
      Log_debug("mencius par:%d loc:%d executed slot %lx now", partition_id_, loc_id_, id);
      max_executed_slot_++;
      n_commit_++;
    } else {
      break;
    }
  }

  //apply the entry out of order if there is no conflict
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
    auto next_instance = GetInstance(id);
    if (next_instance->committed_cmd_) {
      SimpleRWCommand parsed_cmd = SimpleRWCommand(next_instance->committed_cmd_);
      if ((!next_instance->executed_) && (unexecuted_keys_[parsed_cmd.key_]==1)){
        RuleWitnessGC(next_instance->committed_cmd_);
        app_next_(id, next_instance->committed_cmd_);
        next_instance->executed_ = true;
        
        c_mutex.lock();
        unexecuted_keys_[parsed_cmd.key_] -= 1;
        // Log_info("[-1]] cmd %d %d cnt %d", parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second, unexecuted_keys_[parsed_cmd.key_].load());
        verify(unexecuted_keys_[parsed_cmd.key_]>=0);
        c_mutex.unlock();
      } else {
        // Log_info("out-of-order execute fail since %d %d", !next_instance->executed_, unexecuted_keys_[parsed_cmd.key_]==0);
      }
    }
  }

  // TODO should support snapshot for freeing memory.
  // for now just free anything 1000 slots before.
  int i = min_active_slot_;
  // std::lock_guard<std::mutex> guard(g_mutex);
  {
    while (i + 1000 < max_executed_slot_) {
      logs_.erase(i);
      i++;
    }
  }
  min_active_slot_ = i;

  // logs_.erase(slot_id);

  in_applying_logs_ = false;
}

void MenciusServer::Setup() {
  SimpleRWCommand::SetZeroTime();
  Log_info("Setup this=%p, this->loc_id_=%d, this->commo_==%p", 
        (void*)this, this->loc_id_, (void*)this->commo_);
}

#ifdef ZERO_OVERHEAD
bool MenciusServer::ConflictWithOriginalUnexecutedLog(const shared_ptr<Marshallable>& cmd) {
  return false;
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  for (slotid_t id = max_executed_slot_ + 1; id <= max_active_slot_; id++) {
    auto next_instance = GetInstance(id);
    // check next_instance->executed_ since Mencius have out-of-order execution
    if (next_instance->committed_cmd_ && !next_instance->executed_ && SimpleRWCommand::Conflict(next_instance->committed_cmd_, cmd))
      return true;
  }
  return false;
}
#endif

} // namespace janus
