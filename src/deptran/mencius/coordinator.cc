
#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"
#include "server_worker.h"

namespace janus {

CoordinatorMencius::CoordinatorMencius(uint32_t coo_id,
                                             int32_t benchmark,
                                             rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                             uint32_t thread_id)
    : Coordinator(coo_id, benchmark, std::move(client_status), thread_id) {
}

void CoordinatorMencius::Submit(const janus::Command& cmd,
                                   rusty::Function<void()> func,
                                   rusty::Function<void()> exe_callback) {
#ifdef LATENCY_LOG_DEBUG
  Log_info("Time of cmd <{}, {}> arrive svr {} Submit: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
  if (!IsLeader(slot_id_)) {
    //change back to fatal
    Log_info("i am not the leader; site {}; locale {}, slot_id:{}",
              frame_->site_info_->id, loc_id_, slot_id_);
    verify(0);
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(!in_submission_);
  // cmd_ is now janus::Command.
  verify(!cmd_.has_value());
//  verify(cmd.self_cmd_ != nullptr);
  in_submission_ = true;
  cmd_ = cmd;
  verify(cmd_.has_value());
  commit_callback_ = std::move(func);
  GotoNextPhase();
}

// removed `CoordinatorMencius::PickBallot()`
// — only call site was the now-deleted `Prepare()`.
// removed `CoordinatorMencius::Prepare()`
// (~67 LOC) — body started with `verify(0); // for debug;`, and the
// only path that could reach it was via `GotoNextPhase()`'s
// `Phase::PREPARE` case which is unreachable: `INIT_END` jumps
// directly to `Suggest`.  Companion `BroadcastPrepare(parid, slot,
// ballot)` shell on `MenciusCommo` removed in this phase.

void CoordinatorMencius::Suggest() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(!in_suggest);
  in_suggest = true;
  // Log_info("mencius coordinator broadcasts Suggest, "
  //               "par_id_: %lx, slot_id: %llx",
  //           par_id_, slot_id_);
  auto start = chrono::system_clock::now();
  commo()->svr_workers_g = svr_workers_g;
  auto sp_quorum = commo()->BroadcastSuggest(par_id_, slot_id_, curr_ballot_, cmd_);
  sp_quorum->q().id_.set(dep_id_);
	//Log_info("current coroutine's dep_id: {}", Fiber::current_fiber()->dep_id_);
  //Log_info("Suggest(): dep_id:{}, slot_id:{}, site: {}", dep_id_, slot_id_, frame_->site_info_->id);

  sp_quorum->wait();
  // auto end = chrono::system_clock::now();
  // auto duration = chrono::duration_cast<chrono::microseconds>(end-start);
  //auto duration_ready = chrono::duration_cast<chrono::microseconds>(end-sp_quorum->ready_time);
  //Log_info("Duration of Wait() in Suggest() is: {}", duration.count());
  //Log_info("Duration after Ready to end of Wait() is: {}", duration_ready.count());
  sp_quorum->log();
  if (sp_quorum->yes()) {
    committed_ = true;
  } else if (sp_quorum->no()) {
    // TODO process the case: failed to get a majority.
    verify(0);
  } else {
    // TODO process timeout.
    verify(0);
  }
// removed ~30 LOC of commented-out
// `BroadcastSuggest(..., SuggestAck-callback)` legacy + companion
// `SuggestAck(phase_t, Future*)` body — same shape as the also-gone
// AcceptAck variant in paxos/coordinator.cc.
}

void CoordinatorMencius::Commit() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  commit_callback_();
  Log_debug("mencius broadcast commit for partition: {}, slot {}",
            (int) par_id_, (int) slot_id_);
  commo()->BroadcastDecide(par_id_, slot_id_, curr_ballot_, cmd_);
  verify(phase_ == Phase::COMMIT);
  GotoNextPhase();
}

void CoordinatorMencius::GotoNextPhase() {
  int n_phase = 4;
  int current_phase = phase_ % n_phase;
  //Log_info("Current phase is {}", current_phase);
  phase_++;
  switch (current_phase) {
    case Phase::INIT_END:
      if (IsLeader(slot_id_)) {
        phase_++; // skip prepare phase for "leader"
        verify(phase_ % n_phase == Phase::SUGGEST);
        Suggest();
        phase_++;
        verify(phase_ % n_phase == Phase::COMMIT);
      } else {
        // TODO
        verify(0);
        Log_info("The local id is {}", this->loc_id_);
        //Next steps: Find the leader, call submit, wait for the reply
      }
    case Phase::SUGGEST:
      verify(phase_ % n_phase == Phase::COMMIT);
      if (committed_) {
        Commit();
      }
      else{
        verify(0);
      }
      break;
    case Phase::PREPARE:
      verify(phase_ % n_phase == Phase::SUGGEST);
      Suggest();
      break;
    case Phase::COMMIT:
      // do nothing.
      break;
    default:
      verify(0);
  }
}

} // namespace janus
