

#include "server.h"
#include "../paxos_worker.h"
#include "exec.h"

namespace janus {

shared_ptr<ElectionState> es = ElectionState::instance();

void PaxosServer::OnPrepare(slotid_t slot_id,
                            ballot_t ballot,
                            ballot_t *max_ballot,
                            const function<void()> &cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler receives prepare for slot_id: %llx",
            slot_id);
  auto instance = GetInstance(slot_id);
  verify(ballot != instance->max_ballot_seen_);
  if (instance->max_ballot_seen_ < ballot) {
    instance->max_ballot_seen_ = ballot;
  } else {
    // TODO if accepted anything, return;
    verify(0);
  }
  *max_ballot = instance->max_ballot_seen_;
  n_prepare_++;
  cb();
}

void PaxosServer::OnAccept(const slotid_t slot_id,
                           const ballot_t ballot,
                           shared_ptr<Marshallable> &cmd,
                           ballot_t *max_ballot,
                           const function<void()> &cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler accept for slot_id: %llx", slot_id);
  auto instance = GetInstance(slot_id);
  verify(instance->max_ballot_accepted_ < ballot);
  if (instance->max_ballot_seen_ <= ballot) {
    instance->max_ballot_seen_ = ballot;
    instance->max_ballot_accepted_ = ballot;
  } else {
    // TODO
    verify(0);
  }
  *max_ballot = instance->max_ballot_seen_;
  n_accept_++;
  cb();
}

void PaxosServer::OnCommit(const slotid_t slot_id,
                           const ballot_t ballot,
                           shared_ptr<Marshallable> &cmd) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("multi-paxos scheduler decide for slot: %lx", slot_id);
  auto instance = GetInstance(slot_id);
  instance->committed_cmd_ = cmd;
  if (slot_id > max_committed_slot_) {
    max_committed_slot_ = slot_id;
  }
  verify(slot_id > max_executed_slot_);
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
    auto next_instance = GetInstance(id);
    if (next_instance->committed_cmd_) {
      app_next_(*next_instance->committed_cmd_);
      Log_debug("multi-paxos par:%d loc:%d executed slot %lx now", partition_id_, loc_id_, id);
      max_executed_slot_++;
      n_commit_++;
    } else {
      break;
    }
  }
  FreeSlots();
}
// marker:ansh change the args to accomodate objects
// marker:ansh add a suitable reply at bottom
void PaxosServer::OnBulkPrepare(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               const function<void()> &cb) {


  auto bp_log = dynamic_pointer_cast<BulkPrepareLog>(cmd);
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
  Log_debug("Paxos workers size %d %d", pxs_workers_g.size(), bp_log->leader_id);
  for(int i = 0; i < bp_log->min_prepared_slots.size(); i++){
    if(pxs_workers_g[i])
	Log_info("cast successfull %d", i);
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
      if(slot_id_min <= ps->max_committed_slot_){
        verify(0); // marker:ansh to handle. // handle later
      }
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
    // ps->clear_accepted_entries(); // pending bulk-prepare-return
  }

unlock_and_return:

  /* change election state holder */
  if(es->machine_id != bp_log->leader_id)
    es->set_state(0);
  es->set_leader(bp_log->leader_id);
  es->set_lastseen();
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
                              const function<void()> &cb){

  auto hb_log = dynamic_pointer_cast<HeartBeatLog>(cmd);
  es->state_lock();
  if(hb_log->epoch < es->cur_epoch){
    es->state_unlock();
    *valid = 0;
    *ballot = es->cur_epoch;
    cb();
    return;
  }
  Log_debug("OnHeartbeat: received heartbeat from machine is %d %d", hb_log->leader_id, es->leader_id);
  if(hb_log->epoch == es->cur_epoch){
    if(hb_log->leader_id != es->leader_id){
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
    for(int i = 0; i < pxs_workers_g.size()-1; i++){
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
                               const function<void()> &cb){
  pthread_setname_np(pthread_self(), "Follower server thread");
  auto bcmd = dynamic_pointer_cast<BulkPaxosCmd>(cmd);
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  int req_leader = bcmd->leader_id;
  Log_info("Received paxos Prepare for slot %d ballot %d machine %d",cur_slot, cur_b, req_leader);
  *valid = 1;
  //cb();
  //return;
  auto rbcmd = make_shared<BulkPaxosCmd>();
  //Log_info("Received paxos Prepare for slot %d ballot %d machine %d",cur_slot, cur_b, req_leader);
  es->state_lock();
  if(cur_b < es->cur_epoch){
    *ballot = es->cur_epoch;
    es->state_unlock();
    *valid = 0;
    cb();
    return;
  }
  auto instance = GetInstance(cur_slot);
  es->set_lastseen();
  if(req_leader != es->machine_id)
	es->set_state(0);
  if(cur_b > es->cur_epoch){
    es->set_epoch(cur_b);
    es->set_leader(req_leader); // marker:ansh send leader in every request.
    for(int i = 0; i < pxs_workers_g.size()-1; i++){
      PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
      ps->mtx_.lock();
      ps->cur_epoch = cur_b;
      ps->leader_id = req_leader;
      ps->mtx_.unlock();
    }
  } else{
    if(req_leader != es->leader_id){
      verify(0); //more than one leader in a term, should not send prepare if not leader.
    }
  }
  //Log_info("OnBulkPrepare2: Checks successfull preparing response for slot %d %d", cur_slot, partition_id_);
  if(!instance || !instance->accepted_cmd_){
    *ballot = cur_b;
    //*ret_cmd = *bcmd;
    ret_cmd->ballots.push_back(bcmd->ballots[0]);
    ret_cmd->slots.push_back(bcmd->slots[0]);
    ret_cmd->cmds.push_back(bcmd->cmds[0]);
    //Log_info("OnBulkPrepare2: the kind_ of the response object is");
    es->state_unlock();
    cb();
    //es->state_unlock();
    return;
  }
  //es->state_unlock();
  Log_debug("OnBulkPrepare2: instance found, Preparing response");
  ret_cmd->ballots.push_back(instance->max_ballot_accepted_);
  ret_cmd->slots.push_back(cur_slot);
  ret_cmd->cmds.push_back(make_shared<MarshallDeputy>(instance->accepted_cmd_));
  cb();
}

void PaxosServer::OnBulkAccept(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               const function<void()> &cb) {
  //Log_info("here accept");
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  auto bcmd = dynamic_pointer_cast<BulkPaxosCmd>(cmd);
  *valid = 1;
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  int req_leader = bcmd->leader_id;
  //Log_debug("multi-paxos scheduler accept for slot: %lx", bcmd->slots.size());
  es->state_lock();
  if(cur_b < es->cur_epoch){
    *ballot = es->cur_epoch;
    es->state_unlock();
    *valid = 0;
    cb();
    return;
  }
  if(es->machine_id != req_leader)
	es->set_state(0);
  es->set_lastseen();
  //cb();
  //return;
  //Log_info("multi-paxos scheduler accept for slot: %ld, par_id: %d", cur_slot, partition_id_);
  for(int i = 0; i < bcmd->slots.size(); i++){
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
      if(es->cur_epoch > ballot_id){
        *valid = 0;
        *ballot = es->cur_epoch;
        break;
      } else{
        if(es->cur_epoch < ballot_id){
          //Log_info("I am here");
          for(int i = 0; i < pxs_workers_g.size()-1; i++){
            PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
            ps->mtx_.lock();
            ps->cur_epoch = ballot_id;
            ps->leader_id = req_leader;
            ps->mtx_.unlock();
          }
        }
        mtx_.lock();
        es->set_leader(req_leader);
        auto instance = GetInstance(slot_id);
        //verify(instance->max_ballot_accepted_ < ballot_id);
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->accepted_cmd_ = bcmd->cmds[i].get()->sp_data_;
        max_accepted_slot_ = slot_id;
        n_accept_++;
        mtx_.unlock();
        *valid &= 1;
	*ballot = ballot_id;
      }
  }
  Log_info("multi-paxos scheduler accept for slot: %ld, par_id: %d", cur_slot, partition_id_);  
  es->state_unlock();
  cb();
  //Log_info("multi-paxos scheduler accept for slot: %ld, par_id: %d", cur_slot, partition_id_);
}

void PaxosServer::OnBulkCommit(shared_ptr<Marshallable> &cmd,
                               i32* ballot,
                               i32* valid,
                               const function<void()> &cb) {
  //Log_info("here");
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  //mtx_.lock();
  //Log_info("here");
  //Log_info("multi-paxos scheduler decide for slot: %ld", bcmd->slots.size());
  auto bcmd = dynamic_pointer_cast<BulkPaxosCmd>(cmd);
  *valid = 1;
  ballot_t cur_b = bcmd->ballots[0];
  slotid_t cur_slot = bcmd->slots[0];
  //Log_info("multi-paxos scheduler decide for slot: %ld", cur_slot);
  int req_leader = bcmd->leader_id;
  es->state_lock();
  if(cur_b < es->cur_epoch){
    *ballot = es->cur_epoch;
    es->state_unlock();
    *valid = 0;
    cb();
    return;
  }
  if(es->machine_id != req_leader)
	es->set_state(0);
  es->set_lastseen();
  vector<shared_ptr<PaxosData>> commit_exec;
  for(int i = 0; i < bcmd->slots.size(); i++){
      //break;
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
      if(es->cur_epoch > ballot_id){
        *valid = 0;
        *ballot = es->cur_epoch;
        break;
      } else{
        if(es->cur_epoch < ballot_id){
          for(int i = 0; i < pxs_workers_g.size()-1; i++){
            PaxosServer* ps = dynamic_cast<PaxosServer*>(pxs_workers_g[i]->rep_sched_);
            ps->mtx_.lock();
            ps->cur_epoch = ballot_id;
            ps->leader_id = req_leader;
            ps->mtx_.unlock();
          }
        }
        es->set_leader(req_leader);
        mtx_.lock();
        auto instance = GetInstance(slot_id);
        verify(instance->max_ballot_accepted_ <= ballot_id);
        instance->max_ballot_seen_ = ballot_id;
        instance->max_ballot_accepted_ = ballot_id;
        instance->committed_cmd_ = bcmd->cmds[i].get()->sp_data_;
        *valid &= 1;
        if (slot_id > max_committed_slot_) {
            max_committed_slot_ = slot_id;
        }
        mtx_.unlock();
      }
  }
  es->state_unlock();
  if(*valid == 0){
    cb();
    return;
  }
  mtx_.lock();
  //Log_info("The commit batch size is %d", bcmd->slots.size());
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
      //break;
      auto next_instance = GetInstance(id);
      if (next_instance->committed_cmd_) {
          //app_next_(*next_instance->committed_cmd_);
	        commit_exec.push_back(next_instance);
	        //Log_debug("multi-paxos par:%d loc:%d executed slot %lx now", partition_id_, loc_id_, id);
          max_executed_slot_++;
          n_commit_++;
      } else {
          break;
      }
   } 
  mtx_.unlock();
  //Log_info("Committing %d", commit_exec.size());
  for(int i = 0; i < commit_exec.size(); i++){
      //auto x = new PaxosData();
      app_next_(*commit_exec[i]->committed_cmd_);
  }

  *valid = 1;
  //cb();

  mtx_.lock();
  FreeSlots();
  mtx_.unlock();
  cb();
}

} // namespace janus
