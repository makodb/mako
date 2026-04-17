

#include "server.h"
#include "../paxos_worker.h"
#include "exec.h"
#include "paxos/commo.h"

namespace janus {

shared_ptr<ElectionState> es = ElectionState::instance();

void PaxosServer::OnForward(shared_ptr<Marshallable> &cmd,
                            uint64_t dep_id,
                            uint64_t* coro_id,
                            rusty::Function<void()> cb){
  // NOTE: Mako doesn't use this - it uses OnForwardToLearner instead.
  // Empty stub for RPC interface compatibility.
  verify(0); // Should never be called in Mako
}

void PaxosServer::OnPrepare(slotid_t slot_id,
                            ballot_t ballot,
                            ballot_t *max_ballot,
                            uint64_t* coro_id,
                            rusty::Function<void()> cb) {

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler receives prepare for slot_id: %llx",
            slot_id);
  auto instance = GetInstance(slot_id);
  verify(ballot != instance->max_ballot_seen_);
  if (instance->max_ballot_seen_ < ballot) {
    instance->max_ballot_seen_ = ballot;
    PersistLogEntry(slot_id, *instance);  // Phase 1.4: persist ballot update
  } else {
    // TODO if accepted anything, return;
    verify(0);
  }
  auto coro_opt = Fiber::current_coroutine();
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
                           shared_ptr<Marshallable> &cmd,
                           ballot_t *max_ballot,
                           uint64_t* coro_id,
                           rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  //Log_info("multi-paxos scheduler accept for slot_id: %llx", slot_id);
  auto instance = GetInstance(slot_id);
  
  //TODO: might need to optimize this. we can vote yes on duplicates at least for now
  //verify(instance->max_ballot_accepted_ < ballot);
  
  if (instance->max_ballot_seen_ <= ballot) {
    instance->max_ballot_seen_ = ballot;
    instance->max_ballot_accepted_ = ballot;
    PersistLogEntry(slot_id, *instance);  // Phase 1.4: persist accept
  } else {
    // TODO
    verify(0);
  }

  auto coro_opt = Fiber::current_coroutine();
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
                           shared_ptr<Marshallable> &cmd) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler decide for slot: %lx", slot_id);
  auto instance = GetInstance(slot_id);
  instance->committed_cmd_ = cmd;
  bool slot_advanced = false;
  if (slot_id > max_committed_slot_) {
    max_committed_slot_ = slot_id;
    slot_advanced = true;
  }
  PersistLogEntry(slot_id, *instance);  // Phase 1.4: persist commit
  if (slot_advanced) {
    PersistMaxCommitted();  // Phase 1.4: persist max_committed_slot
  }
  verify(slot_id > max_executed_slot_);
  // This prevents the log entry from being applied twice
  if (in_applying_logs_) {
    return;
  }
  in_applying_logs_ = true;
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
    auto next_instance = GetInstance(id);
    if (next_instance->committed_cmd_) {
      app_next_(slot_id,next_instance->committed_cmd_);
      Log_info("apply multi-paxos par:%d loc:%d executed slot %lx now", partition_id_, loc_id_, id);
      max_executed_slot_++;
      n_commit_++;
    } else {
      break;
    }
  }
  in_applying_logs_ = false;
  FreeSlots();
}
// marker:ansh change the args to accomodate objects
// marker:ansh add a suitable reply at bottom
void PaxosServer::OnBulkPrepare(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {


  auto bp_log = marshallable_cast<BulkPrepareLog>(cmd);
  verify(bp_log != nullptr);
  es->state_lock();
  if(bp_log->epoch < es->cur_epoch){
    //es->state_unlock();
    *valid = 0;
    *ballot = es->cur_epoch;
    es->state_unlock();
    cb();
    return;
  }

  if(bp_log->epoch == es->cur_epoch && bp_log->leader_id != es->machine_id){
    //es->state_unlock();
    *valid = 0;
    *ballot = es->cur_epoch;
    es->state_unlock();
    cb();
    return;
  }

  /* acquire all other server locks one by one */
  Log_info("Paxos workers size %d %d %d", pxs_workers_g.size(), bp_log->leader_id, bp_log->epoch, es->cur_epoch);
  for(int i = 0; i < bp_log->min_prepared_slots.size(); i++){
    //if(pxs_workers_g[i])
    //	Log_info("cast successfull %d", i);
    PaxosServer* ps = (PaxosServer*)(pxs_workers_g[i]->rep_sched_);
    ps->mtx_.lock();
  }

  /*verify possibility before modification*/
  for(int i = 0; i < bp_log->min_prepared_slots.size(); i++){
    slotid_t slot_id_min = bp_log->min_prepared_slots[i].second;
    PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    BulkPrepare* bp = &ps->bulk_prepares[make_pair(ps->cur_min_prepared_slot_, ps->max_possible_slot_)];
    if(ps->bulk_prepares.size() != 0 && bp->seen_ballot > bp_log->epoch){
      verify(0); // should not happen, should have been caught bp_log->epoch.
    } else{
      // debug
      //if(slot_id_min <= ps->max_committed_slot_){
      //  verify(0); // marker:ansh to handle. // handle later
      //}
    }
  }

  for(int i = 0; i < bp_log->min_prepared_slots.size(); i++){
    slotid_t slot_id_min = bp_log->min_prepared_slots[i].second;
    PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    BulkPrepare* bp = &ps->bulk_prepares[make_pair(ps->cur_min_prepared_slot_, ps->max_possible_slot_)];
    ps->bulk_prepares.erase(make_pair(ps->cur_min_prepared_slot_, ps->max_possible_slot_));
    bp->seen_ballot = bp_log->epoch;
    bp->leader_id = bp_log->leader_id;
    ps->bulk_prepares[make_pair(slot_id_min, max_possible_slot_)] = *bp;
    ps->cur_min_prepared_slot_ = slot_id_min;
    ps->cur_epoch = bp_log->epoch;
    ps->PersistEpoch();  // Phase 1.4: persist epoch update
    // ps->clear_accepted_entries(); // pending bulk-prepare-return
  }

unlock_and_return:

  /* change election state holder */
  if(es->machine_id != bp_log->leader_id)
    es->set_state(0);
  es->set_leader(bp_log->leader_id);
  es->set_lastseen();
  Log_info("Leader set to %d", bp_log->leader_id);
  es->set_epoch(bp_log->epoch);

  for(int i = 0; i < bp_log->min_prepared_slots.size(); i++){
    PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    ps->mtx_.unlock();
  }
  *ballot = es->cur_epoch;
  es->state_unlock();
  Log_debug("BulkPrepare: Terminating RPC here");
  *valid = 1;
  cb();
}

void PaxosServer::OnHeartbeat(shared_ptr<Marshallable> &cmd,
                              i32* ballot,
                              i32* valid,
                              rusty::Function<void()> cb){

  auto hb_log = marshallable_cast<HeartBeatLog>(cmd);
  verify(hb_log != nullptr);
  es->state_lock();
  if(hb_log->epoch < es->cur_epoch){
    es->state_unlock();
    *valid = 0;
    *ballot = es->cur_epoch;
    cb();
    return;
  }
  if(hb_log->leader_id == 1 && es->machine_id == 2)
  Log_debug("OnHeartbeat: received heartbeat from machine is %d %d", hb_log->leader_id, es->leader_id);
  if(hb_log->epoch == es->cur_epoch){
    if(hb_log->leader_id != es->leader_id){
      Log_info("Req leader is %d while machine leader is %d", hb_log->leader_id, es->leader_id);
      es->state_unlock();
      verify(0); // should not happen, means there are two leaders with different in the same epoch.
    } else if(hb_log->leader_id == es->leader_id){
      if(hb_log->leader_id != es->machine_id)
        es->set_state(0);
      es->set_epoch(hb_log->epoch);
      es->set_lastseen();
      es->state_unlock();
      *valid = 1;
       cb();
       return;
    } else{
      es->set_lastseen();
      es->state_unlock();
      *valid = 1;
      cb();
      return;
    }
  } else{
    // in this case reply needs to be that it needs a prepare.
    *valid = 2 + es->machine_id;    // hacky way.
    es->set_state(0);
    es->set_epoch(hb_log->epoch);
    for(int i = 0; i < pxs_workers_g.size(); i++){
      PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
      ps->mtx_.lock();
      ps->cur_epoch = hb_log->epoch;
      ps->leader_id = hb_log->leader_id;
      ps->mtx_.unlock();
    }
    es->set_lastseen();
    es->set_leader(hb_log->leader_id);
    es->state_unlock();
    *valid = 1; 
    cb();
    return;
  }
}

void PaxosServer::OnBulkPrepare2(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               shared_ptr<BulkPaxosCmd> ret_cmd,
                               rusty::Function<void()> cb){
  //pthread_setname_np(pthread_self(), "Follower server thread");
  auto bcmd = marshallable_cast<PaxosPrepCmd>(cmd);
  verify(bcmd != nullptr);
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  int req_leader = bcmd->leader_id;
  if(req_leader == 1 && es->machine_id != 1)
    //Log_info("Received paxos Prepare for slot %d ballot %d machine %d",cur_slot, cur_b, req_leader);
    *valid = 1;
  //cb();
  //return;
  auto rbcmd = make_shared<BulkPaxosCmd>();
  Log_debug("Received paxos Prepare for slot %d ballot %d machine %d",cur_slot, cur_b, req_leader);
  //es->state_lock();
  // mtx_.lock();
  if(cur_b < cur_epoch){
    *ballot = cur_epoch;
    //es->state_unlock();
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

  // mtx_.lock();
  max_touched_slot = max(max_touched_slot, cur_slot);
  if(cur_b > cur_epoch){
    // mtx_.unlock();
    // es->state_lock();
    // es->set_epoch(cur_b);
    // es->set_leader(req_leader); // marker:ansh send leader in every request.
    // for(int i = 0; i < pxs_workers_g.size(); i++){
    //   PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    //   ps->mtx_.lock();
    //   ps->cur_epoch = cur_b;
    //   ps->leader_id = req_leader;
    //   ps->mtx_.unlock();
    // }
    //es->state_unlock();
  } else{
    //mtx_.unlock();
    if(req_leader != es->leader_id){
      Log_info("Req leader is %d and prev leader is %d", req_leader, es->leader_id);
      verify(0); //more than one leader in a term, should not send prepare if not leader.
    }
  }

  //mtx_.lock();
  auto instance = GetInstance(cur_slot);
  Log_debug("OnBulkPrepare2: Checks successfull preparing response for slot %d %d", cur_slot, partition_id_);
  if(!instance || !instance->accepted_cmd_){
    //mtx_.unlock();
    *valid = 2;
    *ballot = cur_b;
    //*ret_cmd = *bcmd;
    // ret_cmd->ballots.push_back(bcmd->ballots[0]);
    // ret_cmd->slots.push_back(bcmd->slots[0]);
    // ret_cmd->cmds.push_back(bcmd->cmds[0]);
    //Log_info("OnBulkPrepare2: the kind_ of the response object is");
    //es->state_unlock();
    cb();
    //es->state_unlock();
    return;
  }
  //es->state_unlock();
  Log_debug("OnBulkPrepare2: instance found, Preparing response");
  ret_cmd->ballots.push_back(instance->max_ballot_accepted_);
  ret_cmd->slots.push_back(cur_slot);
  ret_cmd->cmds.push_back(make_shared<MarshallDeputy>(instance->accepted_cmd_));
  // mtx_.unlock();
  cb();
}

void PaxosServer::OnSyncLog(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               shared_ptr<SyncLogResponse> ret_cmd,
                               rusty::Function<void()> cb){
  // auto xx = (int32_t)ret_cmd->missing_slots.size();
  // Log_info("received a OnSyncLog,xxx: %d",xx);
  // for(int i = 0; i < ret_cmd->missing_slots.size(); i++){
  //    Log_info("yy: %d", (int32_t)ret_cmd->missing_slots[i].size());
  //    for(int j = 0; j < ret_cmd->missing_slots[i].size(); j++){
  //       Log_info("yy2 a OnSyncLog,xxx: %d",j);
  //    }
  // }
  //cb();
  auto bcmd = marshallable_cast<SyncLogRequest>(cmd);
  verify(bcmd != nullptr);
  es->state_lock();
  if(bcmd->epoch < es->cur_epoch){
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
    ret_cmd->missing_slots.push_back(vector<slotid_t>{});
    PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
    auto bp_cmd = make_shared<BulkPaxosCmd>();
    ps->mtx_.lock();

    for(int j = bcmd->sync_commit_slot[i]; j <= ps->max_committed_slot_; j++){
      auto inst = ps->GetInstance(j);
      if(inst->committed_cmd_){
        bp_cmd->slots.push_back(j);
        bp_cmd->ballots.push_back(inst->max_ballot_accepted_);
        auto temp_cmd = inst->committed_cmd_;
      	MarshallDeputy md(temp_cmd);
      	auto shrd_ptr = make_shared<MarshallDeputy>(md);
        bp_cmd->cmds.push_back(shrd_ptr);
      }
    }
    //Log_info("The partition %d, sync commit is %d; max executed-committed slot is [%d-%d] on follower", i, bcmd->sync_commit_slot[i], ps->max_executed_slot_, ps->max_committed_slot_);
    for(int j = ps->max_executed_slot_; j < bcmd->sync_commit_slot[i]; j++){
      auto inst = ps->GetInstance(j);
      if(!inst->committed_cmd_){
        ret_cmd->missing_slots[i].push_back(j);
      }
    }
    //Log_info("The partition %d has missing slots size %d", i, ret_cmd->missing_slots[i].size());
    auto sp_marshallable = wrap_typed_marshallable(bp_cmd);
    MarshallDeputy bp_md_cmd(sp_marshallable);
    auto bp_sp_md = make_shared<MarshallDeputy>(bp_md_cmd);
    ret_cmd->sync_data.push_back(bp_sp_md);
    ps->mtx_.unlock();
  }
  //cb();
}

void PaxosServer::OnBulkAccept(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {
  auto bcmd = marshallable_cast<BulkPaxosCmd>(cmd);
  verify(bcmd != nullptr);
  *valid = 1;
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  int req_leader = bcmd->leader_id;
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

  // Phase 1.4: collect entries for batch persistence
  std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries_to_persist;

  //Log_info("multi-paxos scheduler accept for slot: %ld, par_id: %d", cur_slot, partition_id_);
  for(int i = 0; i < bcmd->slots.size(); i++){
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
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
        //Log_info("InAccept insert an instance, par_id:%d, epoch:%d, slot_id:%d",partition_id_, cur_epoch, slot_id);
        //verify(instance->max_ballot_accepted_ < ballot_id);
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->accepted_cmd_ = bcmd->cmds[i].get()->inner();
        max_accepted_slot_ = slot_id;
        n_accept_++;
        *valid &= 1;
	      *ballot = ballot_id;
        entries_to_persist.emplace_back(slot_id, instance);  // Phase 1.4
      }
  }
  // Phase 1.4: batch persist all accepted entries
  PersistLogEntries(entries_to_persist);
  if(req_leader != 0)
	Log_debug("multi-paxos scheduler accept for slot: %ld, par_id: %d", cur_slot, partition_id_);
  cb();
}

void PaxosServer::OnSyncCommit(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  //mtx_.lock();
  //Log_info("here");
  //Log_info("multi-paxos scheduler decide for slot: %ld", bcmd->slots.size());
  auto bcmd = marshallable_cast<BulkPaxosCmd>(cmd);
  verify(bcmd != nullptr);
  *valid = 1;
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  //Log_info("multi-paxos scheduler decide for slot: %ld", cur_slot);
  int req_leader = bcmd->leader_id;
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
  // Phase 1.4: collect entries for batch persistence
  std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries_to_persist;
  for(int i = 0; i < bcmd->slots.size(); i++){
      //break;
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
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
        instance->committed_cmd_ = bcmd->cmds[i].get()->inner();
        *valid &= 1;
        if (slot_id > max_committed_slot_) {
            max_committed_slot_ = slot_id;
        }
        entries_to_persist.emplace_back(slot_id, instance);  // Phase 1.4
      }
  }
  // Phase 1.4: batch persist all committed entries
  PersistLogEntries(entries_to_persist);
  PersistMaxCommitted();
  //es->state_unlock();
  if(*valid == 0){
    cb();
    return;
  }
  //mtx_.lock();
  //Log_info("The commit batch size is %d", bcmd->slots.size());
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
      //break;
      auto next_instance = GetInstance(id);
      if (next_instance->committed_cmd_) {
          //app_next_(*next_instance->committed_cmd_);
	        commit_exec.push_back(std::make_pair(id,next_instance));
	        //Log_info("multi-paxos par:%d loc:%d executed slot %lld now", partition_id_, loc_id_, id);
          max_executed_slot_++;
          n_commit_++;
      } else {
          break;
      }
   } 
  //mtx_.unlock();
  //Log_info("Committing %d", commit_exec.size());
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

void PaxosServer::OnBulkCommit(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb) {
  auto bcmd = marshallable_cast<PaxosPrepCmd>(cmd);
  verify(bcmd != nullptr);
  *valid = 1;
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  int req_leader = bcmd->leader_id;
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
  // Phase 1.4: collect entries for batch persistence
  std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>> entries_to_persist;
  for(int i = 0; i < bcmd->slots.size(); i++){
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
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
          Log_info("max_ballot_accepted_: %d, recevied: %d", instance->max_ballot_accepted_, ballot_id);
        }
        //verify(instance->max_ballot_accepted_ == ballot_id); //todo: for correctness, if a new commit comes, sync accept.
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->committed_cmd_ = instance->accepted_cmd_;
        *valid &= 1;
        if (slot_id > max_committed_slot_) {
            max_committed_slot_ = slot_id;
        }
        entries_to_persist.emplace_back(slot_id, instance);  // Phase 1.4
      }
  }
  // Phase 1.4: batch persist all committed entries
  PersistLogEntries(entries_to_persist);
  PersistMaxCommitted();
  if(*valid == 0){
    cb();
    return;
  }
  slotid_t tmpx = max_executed_slot_ + 1;
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
      auto next_instance = GetInstance(id);
      if (next_instance->committed_cmd_) {
          commit_exec.push_back(std::make_pair(id,next_instance));
          max_executed_slot_++;
          n_commit_++;
      }else{
        //Log_info("wait for the id:%d, par_id:%d, max:%d", id, partition_id_, max_committed_slot_);
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
                                    shared_ptr<Marshallable> &cmd,
                                    rusty::Function<void()> cb) {
  //Log_info("received slot:%d",slot);
  max_committed_slot_learner_ = slot;
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  int status=app_next_(slot,cmd);
  cb();
  if (status==janus::PaxosStatus::STATUS_NOOPS){// if noops
    Log_info("Noops received on the learner side");
  }
}

void PaxosServer::OnSyncNoOps(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               rusty::Function<void()> cb){

  auto bcmd = marshallable_cast<SyncNoOpRequest>(cmd);
  verify(bcmd != nullptr);
  *valid = 1;
  ballot_t cur_b = bcmd->epoch;
  int req_leader = bcmd->leader_id;
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

  for(int i = 0; i < pxs_workers_g.size(); i++){
    PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
  //   ps->mtx_.lock();
  //   if(bcmd->sync_slots[i] <= ps->max_executed_slot_){
  //     Log_info("The sync slot is %d for partition %d and committed slot is %d", bcmd->sync_slots[i], i, ps->max_executed_slot_);
  //     verify(0);
  //   }
  //   Log_info("NoOps sync slot is %d for partition %d", bcmd->sync_slots[i], i);
  //   for(int j = bcmd->sync_slots[i]; j <= ps->max_committed_slot_; j++){
  //     auto instance = ps->GetInstance(j);
  //     if(instance->committed_cmd_)
	//       continue;
  //     instance->committed_cmd_ = make_shared<LogEntry>();
  //     instance->is_no_op = true;
  //     instance->max_ballot_accepted_ = cur_b;
  //   }
  //   for (slotid_t id = ps->max_executed_slot_ + 1; id <= ps->max_committed_slot_; id++) {
  //     auto next_instance = ps->GetInstance(id);
  //     if (next_instance->committed_cmd_ && !next_instance->is_no_op) {
  //         ps->app_next_(next_instance->committed_cmd_);
  //         ps->max_executed_slot_++;
  //         ps->n_commit_++;
  //     } else {
  //         verify(0);
  //     }
  //   }
    ps->max_committed_slot_ = bcmd->sync_slots[i];
    ps->max_executed_slot_ = bcmd->sync_slots[i];
    ps->cur_open_slot_ = bcmd->sync_slots[i]+1;
    ps->mtx_.unlock();
  }

  pxs_workers_g.back()->cur_epoch = cur_b;

  *valid = 1;
  cb();

}

// ============================================================================
// LOG PERSISTENCE IMPLEMENTATION (Phase 1.4)
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

  // Prefer committed_cmd_ if available, otherwise accepted_cmd_
  if (data.committed_cmd_) {
    entry.command = data.committed_cmd_;
    entry.committed = true;
  } else if (data.accepted_cmd_) {
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

    if (data->committed_cmd_) {
      entry.command = data->committed_cmd_;
      entry.committed = true;
    } else if (data->accepted_cmd_) {
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
    Log_info("Paxos recovery: cur_epoch = %lu", cur_epoch);
  }

  // Recover max_committed_slot
  auto max_committed_opt = log_storage_->get_metadata(META_MAX_COMMITTED);  // @unsafe
  if (max_committed_opt.is_some()) {
    max_committed_slot_ = std::stoull(max_committed_opt.unwrap());
    Log_info("Paxos recovery: max_committed_slot = %lu", max_committed_slot_);
  }

  // Recover max_executed_slot
  auto max_executed_opt = log_storage_->get_metadata(META_MAX_EXECUTED);  // @unsafe
  if (max_executed_opt.is_some()) {
    max_executed_slot_ = std::stoull(max_executed_opt.unwrap());
    Log_info("Paxos recovery: max_executed_slot = %lu", max_executed_slot_);
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

      if (entry.committed) {
        paxos_data->committed_cmd_ = entry.command;
      } else {
        paxos_data->accepted_cmd_ = entry.command;
      }
    }

    Log_info("Paxos recovery: recovered %lu log entries (slots %lu to %lu)",
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
    Log_warn("[PAXOS-REPLAY] Site par %d loc %d: No app_next_ callback, skipping replay",
             partition_id_, loc_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = max_executed_slot_ + 1;
  slotid_t end = max_committed_slot_;

  if (start > end) {
    Log_info("[PAXOS-REPLAY] Site par %d loc %d: No entries to replay (max_executed=%lu >= max_committed=%lu)",
             partition_id_, loc_id_, max_executed_slot_, max_committed_slot_);
    return;
  }

  Log_info("[PAXOS-REPLAY] Site par %d loc %d: Replaying entries %lu..%lu",
           partition_id_, loc_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto it = logs_.find(id);
    if (it != logs_.end() && it->second && it->second->committed_cmd_) {
      app_next_(id, it->second->committed_cmd_);
      max_executed_slot_ = id;
      replayed++;
    } else {
      Log_warn("[PAXOS-REPLAY] Site par %d loc %d: Missing committed entry at slot %lu, stopping replay",
               partition_id_, loc_id_, id);
      break;
    }
  }

  Log_info("[PAXOS-REPLAY] Site par %d loc %d: Replayed %zu entries, max_executed now %lu",
           partition_id_, loc_id_, replayed, max_executed_slot_);

  // Phase 2.3: Log uncommitted entries status
  size_t uncommitted = GetUncommittedCount();
  if (uncommitted > 0) {
    Log_info("[PAXOS-RECOVERY] Site par %d loc %d: %zu uncommitted entries (max_accepted=%lu, max_committed=%lu) - will be resolved by consensus",
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
    Log_debug("[PAXOS-COMPACT] Site par %d loc %d: No log storage, skipping compaction",
              partition_id_, loc_id_);
    return 0;
  }

  // Safety check: don't compact beyond committed index
  if (up_to_index > max_committed_slot_) {
    Log_warn("[PAXOS-COMPACT] Site par %d loc %d: Cannot compact beyond max_committed (%lu > %lu)",
             partition_id_, loc_id_, up_to_index, max_committed_slot_);
    up_to_index = max_committed_slot_;
  }

  // Get current first slot
  slotid_t first_slot = log_storage_->get_first_index();
  if (first_slot == 0 || log_storage_->empty()) {
    Log_debug("[PAXOS-COMPACT] Site par %d loc %d: Log is empty, nothing to compact",
              partition_id_, loc_id_);
    return 0;
  }

  // Nothing to compact if up_to_index is before first slot
  if (up_to_index < first_slot) {
    Log_debug("[PAXOS-COMPACT] Site par %d loc %d: up_to_index %lu < first_slot %lu, nothing to compact",
              partition_id_, loc_id_, up_to_index, first_slot);
    return 0;
  }

  // Remove entries from storage
  size_t to_remove = up_to_index - first_slot + 1;
  if (log_storage_->remove_range(first_slot, up_to_index + 1)) {
    Log_info("[PAXOS-COMPACT] Site par %d loc %d: Compacted %zu entries [%lu..%lu]",
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
    Log_error("[PAXOS-COMPACT] Site par %d loc %d: Failed to compact log entries",
              partition_id_, loc_id_);
    return 0;
  }
}

} // namespace janus
