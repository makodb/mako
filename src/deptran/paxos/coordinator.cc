
#include <stdint.h>

#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"
#include "paxos_worker.h"

import std;

namespace janus {

std::shared_ptr<ElectionState> es_cc = ElectionState::instance();

MultiPaxosCommo* CoordinatorMultiPaxos::commo() {
  verify(commo_ != nullptr);
  auto* result = dynamic_cast<MultiPaxosCommo*>(commo_);
  verify(result != nullptr);
  return result;
}

void CoordinatorMultiPaxos::Submit(const janus::Command& cmd,
                                   rusty::Function<void()> func) {
  if (!IsLeader()) {
    //change back to fatal
    Log_info("i am not the leader; partition {}; locale {}", par_id_, loc_id_);
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(!in_submission_);
  // cmd_ is now janus::Command; null check via has_value.
  verify(!cmd_.has_value());
//  verify(cmd.self_cmd_ != nullptr);
  in_submission_ = true;
  cmd_ = cmd;
  verify(cmd_.has_value());
  commit_callback_ = std::move(func);
  GotoNextPhase();
}

void BulkCoordinatorMultiPaxos::BulkSubmit(const janus::Command& cmd,
                                           rusty::Function<void()> func) {
    verify(!in_submission_);
    in_submission_ = true;
    cmd_ = cmd;
    commit_callback_ = std::move(func);
    GotoNextPhase();
}

// removed `CoordinatorMultiPaxos::PickBallot()`
// — only call site was the now-deleted `Prepare()`.
// removed `CoordinatorMultiPaxos::Prepare()`
// (~50 LOC) — body started with `verify(0); // for debug;`, and the
// only place the method could have been reached was via
// `GotoNextPhase()`'s `Phase::PREPARE` case which is itself
// unreachable: `INIT_END` skips the prepare phase via `phase_++` and
// jumps directly to `Accept`.  The only use site of the matching
// `BroadcastPrepare(parid, slot, ballot)` commo method (which was
// also a `verify(0)` shell) — both removed in this phase.

void CoordinatorMultiPaxos::Accept() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(!in_accept_);
  in_accept_ = true;
  Log_debug("multi-paxos coordinator broadcasts accept, "
                "par_id_: {:x}, slot_id: {:x}",
            par_id_, slot_id_);
  auto sp_quorum = commo()->BroadcastAccept(par_id_, slot_id_, curr_ballot_, cmd_);
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
// `BroadcastAccept(..., AcceptAck-callback)` legacy + companion
// `AcceptAck(phase_t, Future*)` body.  The shape was the
// callback-style RPC dispatch that pre-dated the
// `commo()->BroadcastAccept(...) -> sp_quorum->wait()` / `yes()`
// pattern used today.  No live code; no surviving caller.
}

void CoordinatorMultiPaxos::Commit() {
  //std::lock_guard<std::recursive_mutex> lock(mtx_);
  commit_callback_();
  Log_debug("multi-paxos broadcast commit for partition: {}, slot {}",
            (int) par_id_, (int) slot_id_);
  commo()->BroadcastDecide(par_id_, slot_id_, curr_ballot_, cmd_);
  verify(phase_ == Phase::COMMIT);
  GotoNextPhase();
}

void CoordinatorMultiPaxos::GotoNextPhase() {
  int n_phase = 4;
  int current_phase = phase_ % n_phase;
  //Log_info("Current phase is {}", current_phase);
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
        // dropped commented-out
        // `//Forward();` and stale TODO breadcrumbs — `Forward()`
        // was never defined and the non-leader branch is
        // `verify(0)`-guarded anyway.
        verify(0);
        Log_info("The local id is {}", this->loc_id_);
      }
    case Phase::ACCEPT:
      verify(phase_ % n_phase == Phase::COMMIT);
      if (committed_) {
        Commit();
      }
      else{
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
    if (!IsLeader())
    verify(1);

    if(current_phase == Phase::INIT_END){
      if(phase_ > 3){
        break;
      }
      //Prepare();  // not necessary for the prepare phase
      if(!in_submission_){
        break;
      }
      phase_++;// need to do this because Phase::Dispatch = 1
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

// removed `BulkCoordinatorMultiPaxos::Prepare()`
// (~70 LOC) — never called.  `GotoNextPhase` already skipped this
// phase via a `// Prepare();` comment + `phase_++` workaround.  The
// matching `BroadcastPrepare2`, `MultiPaxosServiceImpl::BulkPrepare2`,
// and `OnBulkPrepare2` chain is removed in this phase.

void BulkCoordinatorMultiPaxos::Accept() {
    in_accept_ = true;
    // cmd_ is Command; marshallable_cast<T>(Command&)
    // overload handles the cast.  BroadcastBulkAccept now also takes
    // const Command& (per prep6t), so cmd_ flows through directly.
    const auto cmd_temp1 = marshallable_cast<BulkPaxosCmd>(cmd_);
    verify(cmd_temp1.is_some());
    if(!in_submission_){
      return;
    }
    auto ess_cc = es_cc;
    auto sp_quorum = commo()->BroadcastBulkAccept(
        par_id_, cmd_, [](ballot_t, int) {});
    
  // auto strt = std::chrono::high_resolution_clock::now();
  // auto endt2 = std::chrono::high_resolution_clock::now();
  sp_quorum->wait();
  // auto endt3 = std::chrono::high_resolution_clock::now();
  // Log_info("Wan_wait: {}, {}", 
  //         std::chrono::duration_cast<std::chrono::milliseconds>(endt2 - strt).count(),
  //         std::chrono::duration_cast<std::chrono::milliseconds>(endt3 - endt2).count());

    sp_quorum->wait();
    if (sp_quorum->yes()) {
	      if(ess_cc->machine_id == 0)
			Log_debug("Accept: slot {}  is committed, partition id {}", cmd_temp1.unwrap()->slots[0], par_id_);
        committed_ = true;
    } else if (sp_quorum->no()) {
        in_submission_ = false;
        Log_info("can't reach quorum on Accept phase");
        return;
    } else {
        verify(0);
    }
    in_accept_ = false;
}

void BulkCoordinatorMultiPaxos::Commit() {
    // Log_info("BulkCoordinatorMultiPaxos::Commit() called, in_submission_={}", (int)in_submission_);
    if(!in_submission_){
      // Log_info("BulkCoordinatorMultiPaxos::Commit() returning early because in_submission_=false");
      return;
    }
    in_commit_ = true;

    // cmd_ is Command; marshallable_cast<T>(Command&)
    // overload handles the cast.
    const auto cmd_temp1 = marshallable_cast<BulkPaxosCmd>(cmd_);
    verify(cmd_temp1.is_some());
    bool terminal_batch = false;
    for (const auto& command : cmd_temp1.unwrap()->cmds) {
      const auto entry = marshallable_cast<LogEntry>(*command);
      if (entry.is_some() && entry.unwrap()->length == 0) {
        terminal_batch = true;
        break;
      }
    }
    // Fill-then-wrap: build the payload as a local, wrap once complete.
    PaxosPrepCmd prep_cmd;
    prep_cmd.slots = cmd_temp1.unwrap()->slots;
    prep_cmd.ballots = cmd_temp1.unwrap()->ballots;
    prep_cmd.leader_id = cmd_temp1.unwrap()->leader_id;
    auto commit_cmd = rusty::Arc<PaxosPrepCmd>::make(std::move(prep_cmd));

    auto ess_cc = es_cc;
    // Log_info("About to call BroadcastBulkDecide from Commit()");
    auto sp_quorum = commo()->BroadcastBulkDecide(
        par_id_, std::move(commit_cmd),
        [ess_cc](ballot_t ballot, int valid) {
          if (!valid) {
            ess_cc->step_down(ballot);
          }
        });
    if (terminal_batch) {
      // Normal batches keep their asynchronous decide path. For END, make a
      // bounded best-effort wait for the existing Paxos quorum before the
      // submit is reported complete. Do not require every replica: a missing
      // follower must not turn graceful shutdown into a liveness failure.
      constexpr uint64_t kTerminalDecideTimeoutUs = 30'000'000;
      sp_quorum->wait_timeout(kTerminalDecideTimeoutUs);
      if (!sp_quorum->yes()) {
        Log_warn("terminal BulkDecide did not reach quorum for partition {} "
                 "within 30s (yes={}, no={}, required={}); continuing shutdown",
                 par_id_, sp_quorum->q().n_voted_yes_.get(),
                 sp_quorum->q().n_voted_no_.get(), sp_quorum->q().quorum_);
      }
    }
    // Log_info("Called BroadcastBulkDecide from Commit()");
    // it's not necessary to wait for a majority of commits
  //   sp_quorum->wait();
  //   if (sp_quorum->yes()) {
	// //Log_info("Commit: some stuff is committed");
  //   } else if (sp_quorum->no()) {
  //     in_submission_ = false;
  //     return;
  //   } else {
  //     verify(0);
  //   }
    in_commit_ = false;
    //verify(phase_ == Phase::COMMIT);
    commit_callback_();
    // Log_info("BulkCoordinatorMultiPaxos::Commit() finished");
    //GotoNextPhase();
}

} // namespace janus
