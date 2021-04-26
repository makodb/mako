
#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"
#include "paxos_worker.h"

namespace janus {

shared_ptr<ElectionState> es = ElectionState::instance();

CoordinatorMultiPaxos::CoordinatorMultiPaxos(uint32_t coo_id,
                                             int32_t benchmark,
                                             ClientControlServiceImpl* ccsi,
                                            uint32_t thread_id)
    : Coordinator(coo_id, benchmark, ccsi, thread_id) {
}

BulkCoordinatorMultiPaxos::BulkCoordinatorMultiPaxos(uint32_t coo_id,
                                             int32_t benchmark,
                                             ClientControlServiceImpl* ccsi,
                                             uint32_t thread_id)
  : CoordinatorMultiPaxos(coo_id, benchmark, ccsi, thread_id) {
}

void CoordinatorMultiPaxos::Submit(shared_ptr<Marshallable>& cmd,
                                   const function<void()>& func,
                                   const function<void()>& exe_callback) {
  if (!IsLeader()) {
    Log_fatal("i am not the leader; site %d; locale %d",
              frame_->site_info_->id, loc_id_);
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(!in_submission_);
  verify(cmd_ == nullptr);
//  verify(cmd.self_cmd_ != nullptr);
  in_submission_ = true;
  cmd_ = cmd;
  verify(cmd_->kind_ != MarshallDeputy::UNKNOWN);
  commit_callback_ = func;
  GotoNextPhase();
}

void BulkCoordinatorMultiPaxos::BulkSubmit(shared_ptr<Marshallable>& cmd,
                                       const function<void()>& func,
                                       const function<void()>& exe_callback) {
    if (!IsLeader()) {
        Log_fatal("i am not the leader; site %d; locale %d",
                  frame_->site_info_->id, loc_id_);
    }
    //std::lock_guard<std::recursive_mutex> lock(mtx_);
    verify(!in_submission_);
    in_submission_ = true;
    cmd_ = cmd;
    commit_callback_ = func;
    GotoNextPhase();
}

ballot_t CoordinatorMultiPaxos::PickBallot() {
  return curr_ballot_ + 1;
}

void CoordinatorMultiPaxos::Prepare() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(0); // for debug;
  verify(!in_prepare_);
  in_prepare_ = true;
  curr_ballot_ = PickBallot();
  verify(slot_id_ > 0);
  Log_debug("multi-paxos coordinator broadcasts prepare, "
                "par_id_: %lx, slot_id: %llx",
            par_id_,
            slot_id_);
  verify(n_prepare_ack_ == 0);
  int n_replica = Config::GetConfig()->GetPartitionSize(par_id_);
  auto sp_quorum = commo()->BroadcastPrepare(par_id_, slot_id_, curr_ballot_);
  sp_quorum->Wait();
  if (sp_quorum->Yes()) {
    verify(!sp_quorum->HasAcceptedValue());
    // TODO use the previously accepted value.

  } else if (sp_quorum->No()) {
    // TODO restart prepare?
    verify(0);
  } else {
    // TODO timeout
    verify(0);
  }
//  commo()->BroadcastPrepare(par_id_,
//                            slot_id_,
//                            curr_ballot_,
//                            std::bind(&CoordinatorMultiPaxos::PrepareAck,
//                                      this,
//                                      phase_,
//                                      std::placeholders::_1));
//}
//
//void CoordinatorMultiPaxos::PrepareAck(phase_t phase, Future* fu) {
//  std::lock_guard<std::recursive_mutex> lock(mtx_);
//  if (phase_ != phase) return;
//  ballot_t max_ballot;
//  fu->get_reply() >> max_ballot;
//  if (max_ballot == curr_ballot_) {
//    n_prepare_ack_++;
//    verify(n_prepare_ack_ <= n_replica_);
//    if (n_prepare_ack_ >= GetQuorum()) {
//      GotoNextPhase();
//    }
//  } else {
//    if (max_ballot > curr_ballot_) {
//      curr_ballot_ = max_ballot + 1;
//      Log_debug("%s: saw greater ballot increment to %d",
//                __FUNCTION__, curr_ballot_);
//      phase_ = Phase::INIT_END;
//      GotoNextPhase();
//    } else {
////       max_ballot < curr_ballot ignore
//    }
//  }
}

void CoordinatorMultiPaxos::Accept() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(!in_accept);
  in_accept = true;
  Log_debug("multi-paxos coordinator broadcasts accept, "
                "par_id_: %lx, slot_id: %llx",
            par_id_, slot_id_);
  auto sp_quorum = commo()->BroadcastAccept(par_id_, slot_id_, curr_ballot_, cmd_);
  sp_quorum->Wait();
  if (sp_quorum->Yes()) {
    committed_ = true;
  } else if (sp_quorum->No()) {
    // TODO process the case: failed to get a majority.
    verify(0);
  } else {
    // TODO process timeout.
    verify(0);
  }
//  commo()->BroadcastAccept(par_id_,
//                           slot_id_,
//                           curr_ballot_,
//                           cmd_,
//                           std::bind(&CoordinatorMultiPaxos::AcceptAck,
//                                     this,
//                                     phase_,
//                                     std::placeholders::_1));
//}
//
//void CoordinatorMultiPaxos::AcceptAck(phase_t phase, Future* fu) {
//  std::lock_guard<std::recursive_mutex> lock(mtx_);
//  if (phase_ > phase) return;
//  ballot_t max_ballot;
//  fu->get_reply() >> max_ballot;
//  if (max_ballot == curr_ballot_) {
//    n_finish_ack_++;
//    if (n_finish_ack_ >= GetQuorum()) {
//      committed_ = true;
//      GotoNextPhase();
//    }
//  } else {
//    if (max_ballot > curr_ballot_) {
//      curr_ballot_ = max_ballot + 1;
//      Log_debug("%s: saw greater ballot increment to %d",
//                __FUNCTION__, curr_ballot_);
//      phase_ = Phase::INIT_END;
//      GotoNextPhase();
//    } else {
//      // max_ballot < curr_ballot ignore
//    }
//  }
}

void CoordinatorMultiPaxos::Commit() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  commit_callback_();
  Log_debug("multi-paxos broadcast commit for partition: %d, slot %d",
            (int) par_id_, (int) slot_id_);
  commo()->BroadcastDecide(par_id_, slot_id_, curr_ballot_, cmd_);
  verify(phase_ == Phase::COMMIT);
  GotoNextPhase();
}

void CoordinatorMultiPaxos::GotoNextPhase() {
  int n_phase = 4;
  int current_phase = phase_ % n_phase;
  phase_++;
  switch (current_phase) {
    case Phase::INIT_END:
      if (IsLeader()) {
        phase_++; // skip prepare phase for "leader"
        verify(phase_ % n_phase == Phase::ACCEPT);
        Accept();
        phase_++;
        verify(phase_ % n_phase == Phase::COMMIT);
      } else {
        // TODO
        verify(0);
      }
    case Phase::ACCEPT:
      verify(phase_ % n_phase == Phase::COMMIT);
      if (committed_) {
        Commit();
      } else {
        verify(0);
      }
      break;
    case Phase::PREPARE:
      verify(phase_ % n_phase == Phase::ACCEPT);
      Accept();
      break;
    case Phase::COMMIT:
      // do nothing.
      break;
    default:
      verify(0);
  }
}

void BulkCoordinatorMultiPaxos::GotoNextPhase() {
  while(true){
    int n_phase = 4;
    int current_phase = phase_ % n_phase;
    phase_++;
    if(current_phase == Phase::INIT_END){
      if(phase_ > 3){
        break;
      }
      Prepare();
      if(!in_submission_){
        break;
      }
      phase_++; // need to do this because Phase::Dispatch = 1
    } else if(current_phase == Phase::ACCEPT){
      Accept();
      if(!in_submission_){
        break;
      }
    } else if(current_phase == Phase::COMMIT){
      Commit();
      if(!in_submission_){
        break;
      }
    }
  }
}

void BulkCoordinatorMultiPaxos::Prepare() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  // in_prepare_ = true;
  // curr_ballot_ = PickBallot();
  // verify(slot_id_ > 0);
  // Log_debug("multi-paxos coordinator broadcasts prepare, "
  //               "par_id_: %lx, slot_id: %llx",
  //           par_id_,
  //           slot_id_);
  // verify(n_prepare_ack_ == 0);
  // int n_replica = Config::GetConfig()->GetPartitionSize(par_id_);
  std::vector<pair<ballot_t, MarshallDeputy>> vec_md;
  auto sp_quorum = commo()->BroadcastPrepare2(par_id_, cmd_, [&vec_md, &es, this](MarshallDeputy md, ballot_t bt, int valid){
    if(!valid){
      es->step_down(bt);
      this->in_submission_ = false;
    } else{
      vec_md.push_back(make_pair(bt, md));
    }
  });
  sp_quorum->Wait();
  if (sp_quorum->Yes()) {
    ballot_t candidate_b = 0;
    MarshallDeputy* candidate_val = nullptr;
    for(int i = 0; i < vec_md.size(); i++){
      if(vec_md[i].first > candidate_b){
        candidate_b = vec_md[i].first;
        candidate_val = &vec_md[i].second;
      }
    }
    if(candidate_val){
      cmd_ = candidate_val->sp_data;
      cmd_->slots = candidate_val->sp_data->slots;
      cmd_->cmds = candidate_val->sp_data->cmds;
    }

  } else if (sp_quorum->No()) {
    // TODO restart prepare?
    // verify(0);
    //.. not a leader anymore, exit.
  } else {
    // TODO timeout
    verify(0);
  }
}

void BulkCoordinatorMultiPaxos::Accept() {
    //std::lock_guard<std::recursive_mutex> lock(mtx_);
    //committed_ = true;
    //return;
    if(!in_submission_){
      return;
    }
    auto sp_quorum = commo()->BroadcastBulkAccept(par_id_, cmd_, [this, &es](ballot_t ballot, int valid){
      if(!valid){
        es->step_down(ballot);
        this->in_submission_ = false;
      }
    });
    sp_quorum->Wait();
    if (sp_quorum->Yes()) {
        committed_ = true;
    } else if (sp_quorum->No()) {
        in_submission_ = false;
        return;
    } else {
        verify(0);
    }
}

void BulkCoordinatorMultiPaxos::Commit() {
    //std::lock_guard<std::recursive_mutex> lock(mtx_);
    //commit_callback_();
    //GotoNextPhase();
    //return;
    if(!in_submission_){
      return;
    }
    auto sp_quorum = commo()->BroadcastBulkDecide(par_id_, cmd_, [this, &es](ballot_t ballot, int valid){
      if(!valid){
        es->step_down(ballot);
        this->in_submission_ = false;
      }
    });
    sp_quorum->Wait();
    if (sp_quorum->Yes()) {
    } else if (sp_quorum->No()) {
      in_submission_ = false;
      return;
    } else {
      verify(0);
    }
    verify(phase_ == Phase::COMMIT);
    commit_callback_();
    GotoNextPhase();
}

} // namespace janus
