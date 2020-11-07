

#include "server.h"
// #include "paxos_worker.h"
#include "exec.h"

namespace janus {

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

void PaxosServer::OnBulkAccept(shared_ptr<Marshallable> &cmd,
                               i32* valid,
                               const function<void()> &cb) {
  //Log_info("here accept");
  auto bcmd = dynamic_pointer_cast<BulkPaxosCmd>(cmd);
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  *valid = 1;
  //Log_info("multi-paxos scheduler decide for slot: %lx", bcmd->slots.size());
  for(int i = 0; i < bcmd->slots.size(); i++){
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
      auto instance = GetInstance(slot_id);
      verify(instance->max_ballot_accepted_ < ballot_id);
      if (instance->max_ballot_seen_ <= ballot_id) {
          instance->max_ballot_seen_ = ballot_id;
          instance->max_ballot_accepted_ = ballot_id;
	        n_accept_++;
          *valid &= 1;
      } else {
          *valid &= 0;
          // TODO
          verify(0);
      }

  }
  cb();
}

void PaxosServer::OnBulkCommit(shared_ptr<Marshallable> &cmd,
                               i32* valid,
                               const function<void()> &cb) {
  //Log_info("here");
  auto bcmd = dynamic_pointer_cast<BulkPaxosCmd>(cmd);
  vector<shared_ptr<PaxosData>> commit_exec;
  mtx_.lock();
  //Log_info("here");
  //Log_info("multi-paxos scheduler decide for slot: %lx", bcmd->slots.size());
  for(int i = 0; i < bcmd->slots.size(); i++){
      slotid_t slot_id = bcmd->slots[i];
      ballot_t ballot_id = bcmd->ballots[i];
      auto instance = GetInstance(slot_id);
      //verify(bcmd->cmds[i].get()->sp_data_.get() != nullptr);
      instance->committed_cmd_ = bcmd->cmds[i].get()->sp_data_;
      auto x = dynamic_pointer_cast<LogEntry>(instance->committed_cmd_);
      //read_log(x.get()->operation_test.get(), x.get()->length, "server");
      //Log_info("length of log received %d", x.get()->length);
      //Log_info("length is %d slot id %d ballot id %d use count of cmd is %d", x.get()->length, slot_id, ballot_id, instance->committed_cmd_.use_count());
      if (slot_id > max_committed_slot_) {
          max_committed_slot_ = slot_id;
      }
  }
  //Log_info("The commit batch size is %d", bcmd->slots.size());
  for (slotid_t id = max_executed_slot_ + 1; id <= max_committed_slot_; id++) {
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
  for(int i = 0; i < commit_exec.size(); i++){
      app_next_(*commit_exec[i]->committed_cmd_);
  }
  mtx_.lock();
  FreeSlots();
  mtx_.unlock();
  //cb();
}

} // namespace janus
