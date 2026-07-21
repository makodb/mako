

#include "server.h"
#include "../paxos_worker.h"
#include "exec.h"
#include "paxos/commo.h"

namespace janus {

shared_ptr<ElectionState> es = ElectionState::instance();

// removed `PaxosServer::OnForward` —
// body was `verify(0); // Should never be called in Mako`.  The
// `MultiPaxosServiceImpl::Forward(janus::Command, ...)` handler
// already has an empty body (Mako uses `OnForwardToLearner` via
// `ForwardToLearnerServer` RPC instead), so this method was
// genuinely unreachable.

void PaxosServer::OnPrepare(slotid_t slot_id,
                            ballot_t ballot,
                            ballot_t *max_ballot,
                            uint64_t* coro_id,
                            rusty::Function<void()> cb) {

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler receives prepare for slot_id: {:x}",
            slot_id);
  auto instance = GetInstance(slot_id);
  verify(ballot != instance->max_ballot_seen_);
  if (instance->max_ballot_seen_ < ballot) {
    instance->max_ballot_seen_ = ballot;
    PersistLogEntry(slot_id, *instance);  // persist ballot update
  } else {
    // TODO if accepted anything, return;
    verify(0);
  }
  auto coro_opt = Fiber::current_fiber();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id;
  }
  *max_ballot = instance->max_ballot_seen_;
  n_prepare_++;
  WAN_WAIT
  cb();
}


void PaxosServer::OnAccept(const slotid_t slot_id,
		           const uint64_t time,
                           const ballot_t ballot,
                           const janus::Command& cmd,
                           ballot_t *max_ballot,
                           uint64_t* coro_id,
                           rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  //Log_info("multi-paxos scheduler accept for slot_id: {:x}", slot_id);
  auto instance = GetInstance(slot_id);
  
  //TODO: might need to optimize this. we can vote yes on duplicates at least for now
  //verify(instance->max_ballot_accepted_ < ballot);
  
  if (instance->max_ballot_seen_ <= ballot) {
    instance->max_ballot_seen_ = ballot;
    instance->max_ballot_accepted_ = ballot;
    PersistLogEntry(slot_id, *instance);  // persist accept
  } else {
    // TODO
    verify(0);
  }

  auto coro_opt = Fiber::current_fiber();
  if (coro_opt.is_some()) {
    *coro_id = coro_opt.unwrap()->id;
  }
  *max_ballot = instance->max_ballot_seen_;
  n_accept_++;
  WAN_WAIT
  cb();
}

void PaxosServer::OnCommit(const slotid_t slot_id,
                           const ballot_t ballot,
                           const janus::Command& cmd) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler decide for slot: {:x}", slot_id);
  auto instance = GetInstance(slot_id);
  // cmd is Command; PaxosData::committed_cmd_ is Command;
  // direct copy.
  instance->committed_cmd_ = cmd;
  bool slot_advanced = false;
  if (slot_id > max_committed_slot_) {
    max_committed_slot_ = slot_id;
    slot_advanced = true;
  }
  PersistLogEntry(slot_id, *instance);  // persist commit
  if (slot_advanced) {
    PersistMaxCommitted();  // persist max_committed_slot
  }
  verify(slot_id > max_executed_slot_);
  // This prevents the log entry from being applied twice
  if (in_applying_logs_) {
    return;
  }
  in_applying_logs_ = true;
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
    auto next_instance = GetInstance(id);
    // PaxosData::committed_cmd_ is Command;
    // app_next_ takes Command directly.
    if (next_instance->committed_cmd_.has_value()) {
      app_next_(slot_id,next_instance->committed_cmd_);
      Log_info("apply multi-paxos par:{} loc:{} executed slot {:x} now", partition_id_, loc_id_, id);
      max_executed_slot_++;
      n_commit_++;
    } else {
      break;
    }
  }
  in_applying_logs_ = false;
  FreeSlots();
}
// removed `PaxosServer::OnBulkPrepare`
// (~85 LOC) and `PaxosServer::OnHeartbeat` (~58 LOC) — only callers
// were the now-deleted `MultiPaxosServiceImpl::BulkPrepare` /
// `Heartbeat` handlers in paxos/service.cc.

// removed `PaxosServer::OnBulkPrepare2`
// (~85 LOC) — only caller was the now-deleted
// `MultiPaxosServiceImpl::BulkPrepare2` handler.

void PaxosServer::OnSyncLog(const janus::Command& cmd_env,
                               i32* ballot,
                               i32* valid,
                               SyncLogResponse& ret_cmd){
  // marshallable_cast works on Command directly via
  // Envelope overload — drop the boundary lift.
  const auto bcmd = marshallable_cast<SyncLogRequest>(cmd_env);
  verify(bcmd.is_some());
  es->state_lock();
  if(bcmd.unwrap()->epoch < es->cur_epoch){
    //es->state_unlock();
    *valid = 0;
    *ballot = es->cur_epoch;
    es->state_unlock();
    //cb();
    return;
  }
  es->state_unlock();
  *valid = 1;
  for(int i = 0; i < pxs_workers_g.size(); i++){
    ret_cmd.missing_slots.push_back(vector<slotid_t>{});
    PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    BulkPaxosCmd bp_cmd;
    ps->mtx_.lock();

    for(int j = bcmd.unwrap()->sync_commit_slot[i]; j <= ps->max_committed_slot_; j++){
      auto inst = ps->GetInstance(j);
      // committed_cmd_ is Command; the temp_cmd
      // copy + janus::Command(Command) wrapping below relies on
      // Command's copy ctor.
      if(inst->committed_cmd_.has_value()){
        bp_cmd.slots.push_back(j);
        bp_cmd.ballots.push_back(inst->max_ballot_accepted_);
        auto temp_cmd = inst->committed_cmd_;
      	janus::Command md(temp_cmd);
      	auto shrd_ptr = rusty::Arc<janus::Command>::make(md);
        bp_cmd.cmds.push_back(std::move(shrd_ptr));
      }
    }
    //Log_info("The partition {}, sync commit is {}; max executed-committed slot is [{}-{}] on follower", i, bcmd->sync_commit_slot[i], ps->max_executed_slot_, ps->max_committed_slot_);
    for(int j = ps->max_executed_slot_; j < bcmd.unwrap()->sync_commit_slot[i]; j++){
      auto inst = ps->GetInstance(j);
      if(!inst->committed_cmd_.has_value()){
        ret_cmd.missing_slots[i].push_back(j);
      }
    }
    //Log_info("The partition {} has missing slots size {}", i, ret_cmd->missing_slots[i].size());
    ret_cmd.sync_data.push_back(rusty::Arc<janus::Command>::make(
        rusty::Arc<BulkPaxosCmd>::make(std::move(bp_cmd))));
    ps->mtx_.unlock();
  }
}

void PaxosServer::OnBulkAccept(const janus::Command& cmd_env,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {
  // marshallable_cast works on Command directly.
  const auto bcmd = marshallable_cast<BulkPaxosCmd>(cmd_env);
  verify(bcmd.is_some());
  *valid = 1;
  ballot_t cur_b = bcmd.unwrap()->ballots[0];
  slotid_t cur_slot = bcmd.unwrap()->slots[0];
  int req_leader = bcmd.unwrap()->leader_id;
  if(req_leader == 1 && es->machine_id != 1)
        Log_debug("Accept Received from new leader");
  // mtx_.lock();
  if(cur_b < cur_epoch){  // the leader is potentially changed
    *ballot = cur_epoch;
    *valid = 0;
    // mtx_.unlock();
    cb();
    return;
  }
  // mtx_.unlock();
  // es->state_lock();
  // es->set_lastseen();
  // if(req_leader != es->machine_id)
  //   es->set_state(0);
  // es->state_unlock();

  // collect entries for batch persistence
  std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries_to_persist;

  //Log_info("multi-paxos scheduler accept for slot: {}, par_id: {}", cur_slot, partition_id_);
  for(int i = 0; i < bcmd.unwrap()->slots.size(); i++){
      slotid_t slot_id = bcmd.unwrap()->slots[i];
      ballot_t ballot_id = bcmd.unwrap()->ballots[i];
      // mtx_.lock();
      if(cur_epoch > ballot_id){
        *valid = 0;
        *ballot = cur_epoch;
        // mtx_.unlock();
        break;
      } else{
        // if(cur_epoch < ballot_id){
        //   // mtx_.unlock();
        //   for(int i = 0; i < pxs_workers_g.size(); i++){
        //     PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
        //     ps->mtx_.lock();
        //     ps->cur_epoch = ballot_id;
        //     ps->leader_id = req_leader;
        //     // ps->mtx_.unlock();
        //   }
        // } else{
        //   //mtx_.unlock();
        // }
        // es->state_lock();
        // es->set_leader(req_leader);
        // es->state_unlock();
        auto instance = GetInstance(slot_id);
        //Log_info("InAccept insert an instance, par_id:{}, epoch:{}, slot_id:{}",partition_id_, cur_epoch, slot_id);
        //verify(instance->max_ballot_accepted_ < ballot_id);
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->accepted_cmd_ = *bcmd.unwrap()->cmds[i];
        max_accepted_slot_ = slot_id;
        n_accept_++;
        *valid &= 1;
	      *ballot = ballot_id;
        entries_to_persist.emplace_back(slot_id, instance);  
      }
  }
  // batch persist all accepted entries
  PersistLogEntries(entries_to_persist);
  if(req_leader != 0)
	Log_debug("multi-paxos scheduler accept for slot: {}, par_id: {}", cur_slot, partition_id_);
  cb();
}

void PaxosServer::OnSyncCommit(const janus::Command& cmd_env,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  //mtx_.lock();
  //Log_info("here");
  //Log_info("multi-paxos scheduler decide for slot: {}", bcmd->slots.size());
  // marshallable_cast works on Command directly.
  const auto bcmd = marshallable_cast<BulkPaxosCmd>(cmd_env);
  verify(bcmd.is_some());
  *valid = 1;
  ballot_t cur_b = bcmd.unwrap()->ballots[0];
  slotid_t cur_slot = bcmd.unwrap()->slots[0];
  //Log_info("multi-paxos scheduler decide for slot: {}", cur_slot);
  int req_leader = bcmd.unwrap()->leader_id;
  //es->state_lock();
  mtx_.lock();
  if(cur_b < cur_epoch){
    *ballot = cur_epoch;
    //es->state_unlock();
    *valid = 0;
    mtx_.unlock();
    cb();
    return;
  }
  mtx_.unlock();
  es->state_lock();
  es->set_lastseen();
  if(req_leader != es->machine_id)
  es->set_state(0);
  es->state_unlock();
  vector<std::pair<int,shared_ptr<PaxosData>>> commit_exec;
  // collect entries for batch persistence
  std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries_to_persist;
  for(int i = 0; i < bcmd.unwrap()->slots.size(); i++){
      //break;
      slotid_t slot_id = bcmd.unwrap()->slots[i];
      ballot_t ballot_id = bcmd.unwrap()->ballots[i];
      mtx_.lock();
      if(cur_epoch > ballot_id){
        *valid = 0;
        *ballot = cur_epoch;
        mtx_.unlock();
        break;
      } else{
        if(cur_epoch < ballot_id){
          mtx_.unlock();
          for(int i = 0; i < pxs_workers_g.size(); i++){
            PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
            ps->mtx_.lock();
            ps->cur_epoch = ballot_id;
            ps->leader_id = req_leader;
            ps->mtx_.unlock();
          }
        } else{
          mtx_.unlock();
        }
        es->state_lock();
        es->set_leader(req_leader);
        es->state_unlock();

        auto instance = GetInstance(slot_id);
        verify(instance->max_ballot_accepted_ <= ballot_id);
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->committed_cmd_ = *bcmd.unwrap()->cmds[i];
        *valid &= 1;
        if (slot_id > max_committed_slot_) {
            max_committed_slot_ = slot_id;
        }
        entries_to_persist.emplace_back(slot_id, instance);  
      }
  }
  // batch persist all committed entries
  PersistLogEntries(entries_to_persist);
  PersistMaxCommitted();
  //es->state_unlock();
  if(*valid == 0){
    cb();
    return;
  }
  //mtx_.lock();
  //Log_info("The commit batch size is {}", bcmd->slots.size());
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
      //break;
      auto next_instance = GetInstance(id);
      if (next_instance->committed_cmd_.has_value()) {
          //app_next_(*next_instance->committed_cmd_);
	        commit_exec.push_back(std::make_pair(id,next_instance));
	        //Log_info("multi-paxos par:{} loc:{} executed slot {} now", partition_id_, loc_id_, id);
          max_executed_slot_++;
          n_commit_++;
      } else {
          break;
      }
   }
  //mtx_.unlock();
  //Log_info("Committing {}", commit_exec.size());
  for(int i = 0; i < commit_exec.size(); i++){
      //auto x = new PaxosData();
      app_next_(commit_exec[i].first,commit_exec[i].second->committed_cmd_);
  }

  *valid = 1;
  //cb();

  //mtx_.lock();
  //FreeSlots();
  //mtx_.unlock();
  cb();
}

void PaxosServer::OnBulkCommit(const janus::Command& cmd_env,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {
  // marshallable_cast works on Command directly.
  const auto bcmd = marshallable_cast<PaxosPrepCmd>(cmd_env);
  verify(bcmd.is_some());
  *valid = 1;
  ballot_t cur_b = bcmd.unwrap()->ballots[0];
  slotid_t cur_slot = bcmd.unwrap()->slots[0];
  int req_leader = bcmd.unwrap()->leader_id;
  // mtx_.lock();
  if(cur_b < cur_epoch){
    *ballot = cur_epoch;
    *valid = 0;
    // mtx_.unlock();
    cb();
    return;
  }
  // mtx_.unlock();
  // es->state_lock();
  // es->set_lastseen();
  // if(req_leader != es->machine_id)
  // es->set_state(0);
  // es->state_unlock();
  vector<std::pair<int,shared_ptr<PaxosData>>> commit_exec;
  // collect entries for batch persistence
  std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries_to_persist;
  for(int i = 0; i < bcmd.unwrap()->slots.size(); i++){
      slotid_t slot_id = bcmd.unwrap()->slots[i];
      ballot_t ballot_id = bcmd.unwrap()->ballots[i];
      // mtx_.lock();
      if(cur_epoch > ballot_id){
        *valid = 0;
        *ballot = cur_epoch;
        // mtx_.unlock();
        break;
      } else{
        // if(cur_epoch < ballot_id){
        //   // mtx_.unlock();
        //   for(int i = 0; i < pxs_workers_g.size(); i++){
        //     PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
        //     ps->mtx_.lock();
        //     ps->cur_epoch = ballot_id;
        //     ps->leader_id = req_leader;
        //     ps->mtx_.unlock();
        //   }
        // } else{
        //   mtx_.unlock();
        // }
        // es->state_lock();
        // es->set_leader(req_leader);
        // es->state_unlock();

        auto instance = GetInstance(slot_id);
        if (instance->max_ballot_accepted_ != ballot_id){
          Log_info("max_ballot_accepted_: {}, recevied: {}", instance->max_ballot_accepted_, ballot_id);
        }
        //verify(instance->max_ballot_accepted_ == ballot_id); //todo: for correctness, if a new commit comes, sync accept.
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->committed_cmd_ = instance->accepted_cmd_;
        *valid &= 1;
        if (slot_id > max_committed_slot_) {
            max_committed_slot_ = slot_id;
        }
        entries_to_persist.emplace_back(slot_id, instance);  
      }
  }
  // batch persist all committed entries
  PersistLogEntries(entries_to_persist);
  PersistMaxCommitted();
  if(*valid == 0){
    cb();
    return;
  }
  slotid_t tmpx = max_executed_slot_ + 1;
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
      auto next_instance = GetInstance(id);
      if (next_instance->committed_cmd_.has_value()) {
          commit_exec.push_back(std::make_pair(id,next_instance));
          max_executed_slot_++;
          n_commit_++;
      }else{
        //Log_info("wait for the id:{}, par_id:{}, max:{}", id, partition_id_, max_committed_slot_);
        // if (max_committed_slot_ - tmpx>20){
        //   max_executed_slot_++;
        //   n_commit_++;
        // }
        break;
      }
   }
  for(int i = 0; i < commit_exec.size(); i++){
      app_next_(commit_exec[i].first,commit_exec[i].second->committed_cmd_);
  }

  *valid = 1;
  cb();
}

void PaxosServer::OnForwardToLearner(const rrr::i32& par_id,
                                    const uint64_t& slot,
                                    const ballot_t& ballot,
                                    const janus::Command& cmd,
                                    rusty::Function<void()> cb) {
  //Log_info("received slot:{}",slot);
  max_committed_slot_learner_ = slot;
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // app_next_ takes janus::Command directly.
  int status=app_next_(slot,cmd);
  cb();
  if (status==janus::PaxosStatus::STATUS_NOOPS){// if noops
    Log_info("Noops received on the learner side");
  }
}

// removed `PaxosServer::OnSyncNoOps`
// (~60 LOC) — only caller was the now-deleted
// `MultiPaxosServiceImpl::SyncNoOps` handler.

// ============================================================================
// LOG PERSISTENCE IMPLEMENTATION
// ============================================================================

// @unsafe - Uses LogStorage which has non-borrow-checked operations
void PaxosServer::PersistEpoch() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }
  log_storage_->set_metadata(META_EPOCH, std::to_string(cur_epoch));
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage which has non-borrow-checked operations
void PaxosServer::PersistMaxCommitted() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }
  log_storage_->set_metadata(META_MAX_COMMITTED, std::to_string(max_committed_slot_));
  log_storage_->set_metadata(META_MAX_EXECUTED, std::to_string(max_executed_slot_));
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage which has non-borrow-checked operations
void PaxosServer::PersistLogEntry(slotid_t slot_id, const PaxosData& data) {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  janus::raft::LogEntry entry;
  entry.slot_id = slot_id;
  entry.term = cur_epoch;
  entry.max_ballot_seen = data.max_ballot_seen_;
  entry.max_ballot_accepted = data.max_ballot_accepted_;
  entry.is_no_op = data.is_no_op;

  // Prefer committed_cmd_ if available, otherwise accepted_cmd_.
  // PaxosData::*_cmd_ are now Command; LogEntry::command
  // is also Command — direct copy.
  if (data.committed_cmd_.has_value()) {
    entry.command = data.committed_cmd_;
    entry.committed = true;
  } else if (data.accepted_cmd_.has_value()) {
    entry.command = data.accepted_cmd_;
    entry.committed = false;
  }

  log_storage_->put(entry);  // @unsafe
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage which has non-borrow-checked operations
void PaxosServer::PersistLogEntries(
    const std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>>& entries) {
  if (!log_storage_ || !log_storage_->is_open() || entries.empty()) {
    return;
  }

  std::vector<janus::raft::LogEntry> log_entries;
  log_entries.reserve(entries.size());

  for (const auto& [slot_id, data] : entries) {
    janus::raft::LogEntry entry;
    entry.slot_id = slot_id;
    entry.term = cur_epoch;
    entry.max_ballot_seen = data->max_ballot_seen_;
    entry.max_ballot_accepted = data->max_ballot_accepted_;
    entry.is_no_op = data->is_no_op;

    if (data->committed_cmd_.has_value()) {
      entry.command = data->committed_cmd_;
      entry.committed = true;
    } else if (data->accepted_cmd_.has_value()) {
      entry.command = data->accepted_cmd_;
      entry.committed = false;
    }

    log_entries.push_back(std::move(entry));
  }

  log_storage_->put_batch(log_entries);  // @unsafe
  log_storage_->sync();  // @unsafe
}

// @unsafe - Uses LogStorage which has non-borrow-checked operations
bool PaxosServer::RecoverFromStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return true;  // No storage configured, nothing to recover
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Recover epoch
  auto epoch_opt = log_storage_->get_metadata(META_EPOCH);  // @unsafe
  if (epoch_opt.is_some()) {
    cur_epoch = std::stoull(epoch_opt.unwrap());
    Log_info("Paxos recovery: cur_epoch = {}", cur_epoch);
  }

  // Recover max_committed_slot
  auto max_committed_opt = log_storage_->get_metadata(META_MAX_COMMITTED);  // @unsafe
  if (max_committed_opt.is_some()) {
    max_committed_slot_ = std::stoull(max_committed_opt.unwrap());
    Log_info("Paxos recovery: max_committed_slot = {}", max_committed_slot_);
  }

  // Recover max_executed_slot
  auto max_executed_opt = log_storage_->get_metadata(META_MAX_EXECUTED);  // @unsafe
  if (max_executed_opt.is_some()) {
    max_executed_slot_ = std::stoull(max_executed_opt.unwrap());
    Log_info("Paxos recovery: max_executed_slot = {}", max_executed_slot_);
  }

  // Recover log entries
  slotid_t first_index = log_storage_->get_first_index();  // @unsafe
  slotid_t last_index = log_storage_->get_last_index();  // @unsafe

  if (first_index > 0 && last_index >= first_index) {
    auto entries = log_storage_->get_range(first_index, last_index + 1);  // @unsafe

    for (const auto& entry : entries) {
      auto& paxos_data = logs_[entry.slot_id];
      if (!paxos_data) {
        paxos_data = std::make_shared<PaxosData>();
      }

      paxos_data->max_ballot_seen_ = entry.max_ballot_seen;
      paxos_data->max_ballot_accepted_ = entry.max_ballot_accepted;
      paxos_data->is_no_op = entry.is_no_op;

      // prep1+3a: both LogEntry::command and
      // PaxosData::*_cmd_ are janus::Command — direct copy.
      if (entry.committed) {
        paxos_data->committed_cmd_ = entry.command;
      } else {
        paxos_data->accepted_cmd_ = entry.command;
      }
    }

    Log_info("Paxos recovery: recovered {} log entries (slots {} to {})",
             entries.size(), first_index, last_index);
  }

  // Update min_active_slot_ based on recovered data
  if (!logs_.empty()) {
    min_active_slot_ = logs_.begin()->first;
  }

  return true;
}

// @unsafe - Calls app_next_ callback
void PaxosServer::ReplayCommittedEntries() {
  if (!app_next_) {
    Log_warn("[PAXOS-REPLAY] Site par {} loc {}: No app_next_ callback, skipping replay",
             partition_id_, loc_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = max_executed_slot_ + 1;
  slotid_t end = max_committed_slot_;

  if (start > end) {
    Log_info("[PAXOS-REPLAY] Site par {} loc {}: No entries to replay (max_executed={} >= max_committed={})",
             partition_id_, loc_id_, max_executed_slot_, max_committed_slot_);
    return;
  }

  Log_info("[PAXOS-REPLAY] Site par {} loc {}: Replaying entries {}..{}",
           partition_id_, loc_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto it = logs_.find(id);
    if (it != logs_.end() && it->second && it->second->committed_cmd_.has_value()) {
      app_next_(id, it->second->committed_cmd_);
      max_executed_slot_ = id;
      replayed++;
    } else {
      Log_warn("[PAXOS-REPLAY] Site par {} loc {}: Missing committed entry at slot {}, stopping replay",
               partition_id_, loc_id_, id);
      break;
    }
  }

  Log_info("[PAXOS-REPLAY] Site par {} loc {}: Replayed {} entries, max_executed now {}",
           partition_id_, loc_id_, replayed, max_executed_slot_);

  // Log uncommitted entries status
  size_t uncommitted = GetUncommittedCount();
  if (uncommitted > 0) {
    Log_info("[PAXOS-RECOVERY] Site par {} loc {}: {} uncommitted entries (max_accepted={}, max_committed={}) - will be resolved by consensus",
             partition_id_, loc_id_, uncommitted, max_accepted_slot_, max_committed_slot_);
  }
}

// @safe - Read-only accessor
size_t PaxosServer::GetUncommittedCount() const {
  if (max_accepted_slot_ > max_committed_slot_) {
    return max_accepted_slot_ - max_committed_slot_;
  }
  return 0;
}

// @unsafe - Modifies log storage
size_t PaxosServer::CompactLog(slotid_t up_to_index) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  if (!log_storage_) {
    Log_debug("[PAXOS-COMPACT] Site par {} loc {}: No log storage, skipping compaction",
              partition_id_, loc_id_);
    return 0;
  }

  // Safety check: don't compact beyond committed index
  if (up_to_index > max_committed_slot_) {
    Log_warn("[PAXOS-COMPACT] Site par {} loc {}: Cannot compact beyond max_committed ({} > {})",
             partition_id_, loc_id_, up_to_index, max_committed_slot_);
    up_to_index = max_committed_slot_;
  }

  // Get current first slot
  slotid_t first_slot = log_storage_->get_first_index();
  if (first_slot == 0 || log_storage_->empty()) {
    Log_debug("[PAXOS-COMPACT] Site par {} loc {}: Log is empty, nothing to compact",
              partition_id_, loc_id_);
    return 0;
  }

  // Nothing to compact if up_to_index is before first slot
  if (up_to_index < first_slot) {
    Log_debug("[PAXOS-COMPACT] Site par {} loc {}: up_to_index {} < first_slot {}, nothing to compact",
              partition_id_, loc_id_, up_to_index, first_slot);
    return 0;
  }

  // Remove entries from storage
  size_t to_remove = up_to_index - first_slot + 1;
  if (log_storage_->remove_range(first_slot, up_to_index + 1)) {
    Log_info("[PAXOS-COMPACT] Site par {} loc {}: Compacted {} entries [{}..{}]",
             partition_id_, loc_id_, to_remove, first_slot, up_to_index);

    // Also remove from in-memory logs
    for (slotid_t id = first_slot; id <= up_to_index; ++id) {
      logs_.erase(id);
    }

    // Update min_active_slot_
    if (up_to_index + 1 > min_active_slot_) {
      min_active_slot_ = up_to_index + 1;
    }

    return to_remove;
  } else {
    Log_error("[PAXOS-COMPACT] Site par {} loc {}: Failed to compact log entries",
              partition_id_, loc_id_);
    return 0;
  }
}

} // namespace janus
