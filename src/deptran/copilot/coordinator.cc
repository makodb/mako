#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"
#include "server.h"
#include "frame.h"
#include "rrr/misc/serializable.hpp"  // wrap_serializable

// #define DO_FINALIZE

namespace janus {

const char* indicator[] = {"COPILOT", "PILOT"};

bool FreeDangling(Communicator* comm, vector<std::pair<uint16_t, rrr::i64> > &dangling) {
  for (auto &dang : dangling) {
    auto it = comm->rpc_clients_.find(dang.first);
    if (it != comm->rpc_clients_.end()) {
      it->second->handle_free(dang.second);
    }
  }

  return true;
}

CoordinatorCopilot::CoordinatorCopilot(uint32_t coo_id,
                                       int32_t benchmark,
                                       rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                       uint32_t thread_id)
  : Coordinator(coo_id, benchmark, std::move(client_status), thread_id) {}

CoordinatorCopilot::~CoordinatorCopilot() {
  // Log_debug("copilot coord {} destroyed", (int)coo_id_);
}

inline ballot_t CoordinatorCopilot::makeUniqueBallot(ballot_t ballot) {
  /**
   * ballot format:
   * 63           8 7        0
   * ballot number | server id
   */
  return ballot << 8 | loc_id_;
}

inline ballot_t CoordinatorCopilot::pickGreaterBallot(ballot_t ballot) {
  return makeUniqueBallot((ballot >> 8) + 1);
}

void CoordinatorCopilot::Submit(const janus::Command& cmd,
                                rusty::Function<void()> func,
                                rusty::Function<void()> exe_callback) {
  verify(IsPilot() || IsCopilot());  // only pilot or copilot can initiate command submission
  done_ = false;
  std::lock_guard<std::recursive_mutex> lock(mtx_);
#ifdef FULL_LOG_DEBUG
  Log_info("cmd<{}, {}> entered site {} CoordinatorCopilot::Submit", SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second, loc_id_);
#endif
  // cmd_now_ is now janus::Command.
  verify(!cmd_now_.has_value());

  // removed `begin = Time::now(true);` —
  // the `begin` field was used only to compute `fac` / `ac`, both
  // of which were dead state.

  cmd_now_ = cmd;
  auto slot_and_dep = sch_->PickInitSlotAndDep();
  // TODO: check if this is correct, whether we can always set initial ballot as 0
  curr_ballot_ = makeUniqueBallot(0);
  is_pilot_ = IsPilot() ? YES : NO;
  slot_id_ = slot_and_dep.first;
  dep_ = slot_and_dep.second;
  verify(cmd_now_.has_value());
  commit_callback_ = std::move(func);
  GotoNextPhase();
}

void CoordinatorCopilot::Prepare() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
start_prepare:
  static_cast<CopilotFrame*>(frame_)->n_prepare_++;
  current_phase_ = Phase::PREPARE;
  ballot_t new_ballot = pickGreaterBallot(curr_ballot_);
  int n_fastac = 0;

  auto sq_quorum = commo()->BroadcastPrepare(par_id_,
                                             is_pilot_, slot_id_,
                                             new_ballot);
  Log_debug(
      "Copilot coordinator {} broadcast PREPARE for"
      "partition: {}, {} slot: {} ballot {}", coo_id_,
      par_id_, indicator[is_pilot_], slot_id_, new_ballot);
  // sq_quorum->id_ = dep_id_;

  sq_quorum->wait();
#ifdef DO_FINALIZE
  sq_quorum->finalize(finalize_timeout_us,
                      std::bind(FreeDangling, commo(), std::placeholders::_1));
#endif
  // sq_quorum->log();
  auto curr_ins = sch_->GetInstance(slot_id_, is_pilot_);
  if(!curr_ins)
    return;
  /**
   * The recovery value picking procedure is complex and its
   * full details appear in our accompanying technical report
   */
  direct_commit_ = false;
  if (sq_quorum->q().committed_seen_.get()) {
    /**
     * If any of the PrepareOk messages indicate an entry is committed,
     * the pilot short-circuits waiting and commits that entry with the
     * same command and dependency.
     */
    auto& slct_cmd = sq_quorum->GetCmds(Status::COMMITED)[0];
    cmd_now_ = slct_cmd.cmd;
    dep_ = slct_cmd.dep_id;
    direct_commit_ = true;
  } else if (sq_quorum->GetCmds(Status::ACCEPTED).size() > 0) {
    /**
     * There are one or more replies r with accepted as their
     * progress. Then pick r’s command and dependency.
     */
    auto& slct_cmd = sq_quorum->GetCmds(Status::ACCEPTED)[0];
    cmd_now_ = slct_cmd.cmd;
    dep_ = slct_cmd.dep_id;
  } else if ((n_fastac = sq_quorum->GetCmds(Status::FAST_ACCEPTED_EQ).size()) > 0) {
    if (n_fastac < (maxFail() + 1) / 2) {
      /**
       * There are < [f+1]/2 replies r 2 S with fast-accepted as their
       * progress. Then pick no-op with an empty dependency.
       */
      cmd_now_ = janus::Command::pack_aliased(rusty::Arc<TpcNoopCommand>::make());  // no-op
      dep_ = 0;
    } else if (n_fastac >= maxFail()) {
      /**
       * There are >= f replies r 2 S with fast-accepted as their progress.
       * Then pick r's comand and dependency
       */
      auto& slct_cmd = sq_quorum->GetCmds(Status::FAST_ACCEPTED_EQ)[0];
      cmd_now_ = slct_cmd.cmd;
      dep_ = slct_cmd.dep_id;
    } else {
      /**
       * The remaining case is when there are in the range of [ [f+1]/2, f) replies r 2 S
       * with fast-accepted as their progress
       * 
       * In 3-replica(f=1) this case won't happen [1,1)
       */
      cmd_now_ = curr_ins->cmd;
      dep_ = curr_ins->dep_id;
    }
  } else if (sq_quorum->GetCmds(Status::NOT_ACCEPTED).size() >= maxFail() + 1) {
    cmd_now_ = janus::Command::pack_aliased(rusty::Arc<TpcNoopCommand>::make());  // no-op
    dep_ = 0;
  } else {
    // retry with higher ballot number
    sq_quorum->Show();
    Log_warn("{} : {} Prepare failed, retry",
              indicator[is_pilot_], slot_id_);
    goto start_prepare;
  }

  if (GET_STATUS(curr_ins->status) >= Status::COMMITED) {
    // instance already committed, end fast-takeover in advance
    current_phase_ = Phase::COMMIT;
  } else {
    GotoNextPhase();
  }
}

void CoordinatorCopilot::FastAccept() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  static_cast<CopilotFrame*>(frame_)->n_fast_accept_++;
  if ((static_cast<CopilotFrame*>(frame_)->n_fast_accept_ & 0xfff) == 0)
      Log_info("fast/reg/total {}/{}/{}", static_cast<CopilotFrame*>(frame_)->n_fast_path_,
        static_cast<CopilotFrame*>(frame_)->n_regular_path_,
        static_cast<CopilotFrame*>(frame_)->n_fast_accept_);
  Log_debug(
      "Copilot coordinator {} broadcast FAST_ACCEPT, "
      "partition: {}, {} : {} -> {}",
      coo_id_, par_id_, indicator[is_pilot_], slot_id_, dep_);
      // marshallable_cast<TpcCommitCommand>(cmd_now_)->tx_id_);
  // removed `begin = Time::now(true);` —
  // see companion comment in CoordinatorCopilot::Submit.
  // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd_now_);
  // Log_info("FastAccept loc_id_={} is_pilot_={} slot_id_={} cmd<{}, {}> dep_={}", loc_id_, is_pilot_, slot_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second, dep_);
  // BroadcastFastAccept now takes janus::Command.
  auto sq_quorum = commo()->BroadcastFastAccept(par_id_,
                                                is_pilot_, slot_id_,
                                                curr_ballot_,
                                                dep_,
                                                cmd_now_);
  // sq_quorum->id_ = dep_id_;
  // Log_debug("current coroutine's dep_id: {}", Fiber::current_fiber()->dep_id_);

  sq_quorum->wait();
#ifdef FULL_LOG_DEBUG
  // GetCmdID still takes shared_ptr<Marshallable>.
  Log_info("cmd<{}, {}> site {} Finish commo()->BroadcastFastAccept->wait()", SimpleRWCommand::GetCmdID(cmd_now_).first, SimpleRWCommand::GetCmdID(cmd_now_).second, loc_id_);
#endif
#ifdef COPILOT_TIME_DEBUG
  struct timeval tp;
  gettimeofday(&tp, NULL);
  // marshallable_cast<T>(Command&) overload handles cmd_now_ directly.
  Log_info("[2+] [tx={}] FastAccept quorum finish {:.3f}", marshallable_cast<TpcBatchCommand>(cmd_now_).unwrap()->cmds_.at(0)->tx_id_, tp.tv_sec * 1000 + tp.tv_usec / 1000.0);
#endif
  // removed `fac = Time::now(true) - begin;`
  // — `fac` was a timing counter that nothing read.
#ifdef DO_FINALIZE
  sq_quorum->finalize(finalize_timeout_us,
                      std::bind(FreeDangling, commo(), std::placeholders::_1));
#endif
  // cout << "fac";
  // sq_quorum->Log();

  fast_path_ = false;
  if (sq_quorum->FastYes()) {
    /**
     * If a pilot gathers a fast quorum, then enough replicas have
     * agreed to its initial dependency that it will always be recovered
     * from any majority quorum of replicas. Thus, it is safe for the
     * pilot to commit this entry on the fast path and continue to execution.
     */
    fast_path_ = true;
    committed_ = true; // fast-path
    static_cast<CopilotFrame*>(frame_)->n_fast_path_++;
    Log_debug("commit on fast path");
  } else {
    if (sq_quorum->yes()) {
#ifdef FULL_LOG_DEBUG
      Log_info("cmd<{}, {}> site {} sq_quorum->yes()", SimpleRWCommand::GetCmdID(cmd_now_).first, SimpleRWCommand::GetCmdID(cmd_now_).second, loc_id_);
#endif
      /**
       * go to accept phase (regular-path):
       * it must use the (f+1)-th dependency to ensure quorum intersection
       * with any command that has already been committed and potentially
       * executed by the other pilot
       */
      dep_ = sq_quorum->GetFinalDep();
      static_cast<CopilotFrame*>(frame_)->n_regular_path_++;
      Log_debug("Final dep: {}, continue on regular path", dep_);
    } else if (sq_quorum->no()) {
      // TODO process the case: failed to get a majority.
      verify(0);
    } else {
      // TODO process timeout.
      verify(0);
    }
  }
  GotoNextPhase();
}

void CoordinatorCopilot::Accept() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  static_cast<CopilotFrame*>(frame_)->n_accept_++;
  verify(current_phase_ == Phase::ACCEPT);
  Log_debug(
      "Copilot coordinator {} broadcast ACCEPT, "
      "partition: {}, {} : {} -> {}",
      coo_id_, par_id_, indicator[is_pilot_], slot_id_, dep_);

  // removed `begin = Time::now(true);` —
  // see companion comment in CoordinatorCopilot::Submit.
  // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd_now_);
  // Log_info("Accept loc_id_={} is_pilot_={} slot_id_={} cmd<{}, {}> dep_={}", loc_id_, is_pilot_, slot_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second, dep_);
  auto sp_quorum = commo()->BroadcastAccept(par_id_,
                                            is_pilot_, slot_id_,
                                            curr_ballot_,
                                            dep_,
                                            cmd_now_);
  // sp_quorum->id_ = dep_id_;
  // Log_debug("current coroutine's dep_id: {}", Fiber::current_fiber()->dep_id_);

  sp_quorum->wait();
#ifdef DO_FINALIZE
  sp_quorum->finalize(finalize_timeout_us,
                      std::bind(FreeDangling, commo(), std::placeholders::_1));
#endif
  // cout << "ac";
  // sp_quorum->Log();
  // if ((static_cast<CopilotFrame*>(frame_)->n_accept_ & 0x3ff) == 0)
  // removed `ac = Time::now(true) - begin;`
  // — `ac` was a timing counter that nothing read.

  if (sp_quorum->yes()) {
    committed_ = true;
  } else if (sp_quorum->no()) {
    /**
     * TODO: process the case: failed to get a majority.
     * An consensus instance with higher ballot is ongoing,
     * abandon this one
     */
  } else {
    // TODO process timeout.
    verify(0);
  }
  GotoNextPhase();
}

void CoordinatorCopilot::Commit() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  static_cast<CopilotFrame*>(frame_)->n_commit_++;
  verify(current_phase_ == Phase::COMMIT);
  commit_callback_();
  Log_debug("Copilot coordinator {} broadcast COMMIT for partition: {}, {} : {} -> {}",
            coo_id_, (int)par_id_, indicator[is_pilot_], slot_id_, dep_);

  // SimpleRWCommand parsed_cmd = SimpleRWCommand(cmd_now_);
  // Log_info("Commit loc_id_={} is_pilot_={} slot_id_={} cmd<{}, {}> dep_={}", loc_id_, is_pilot_, slot_id_, parsed_cmd.cmd_id_.first, parsed_cmd.cmd_id_.second, dep_);
  auto sp_quorum = commo()->BroadcastCommit(par_id_,
                                            is_pilot_, slot_id_,
                                            dep_,
                                            cmd_now_);
  sp_quorum->wait();  // in fact this doesn't wait since it's a fake quorum event
#ifdef DO_FINALIZE
  sp_quorum->finalize(finalize_timeout_us,
                      std::bind(FreeDangling, commo(), std::placeholders::_1));
#endif
  /**
   * A pilot sets a takeover-timeout when it has a committed
   * command but does not know the final dependencies of all
   * potentially preceding entries, i.e., it has not seen a
   * commit for this entry’s final dependency.
   */
  auto dep_ins = sch_->GetInstance(dep_, REVERSE(is_pilot_));
  int take = 0;
  if (dep_ins && !in_fast_takeover_ && dep_ != 0) {
      // removed `begin = Time::now(true);`
      // — see companion comment in CoordinatorCopilot::Submit.
  // if (false) {
    // auto dep_ins = sch_->GetInstance(dep_, REVERSE(is_pilot_));
    /* It must proceed after all entries before its dependency have committed
     */
    if (!sch_->AllDepsEliminated(REVERSE(is_pilot_), dep_) &&
        !sch_->WaitMaxCommittedGT(REVERSE(is_pilot_), dep_, takeover_timeout_us)) {
      /*
       if timeout but the final dependency is still not committed,
       start takeover for all uncommitted entries
      */
      slotid_t start = sch_->GetMaxCommittedSlot(REVERSE(is_pilot_)) + 1;
      slotid_t end = dep_;
      uint8_t cur_pilot = is_pilot_;
      slotid_t cur_slot = slot_id_;  // save the property of current insatnce , cause initFastTakeover resets the property
      
      Log_info("TAKEOVER on {} for {} from {} to {}", indicator[cur_pilot], cur_slot, start, end);
      for (auto i = start; i <= end; i++) {
        auto ucmit_ins = sch_->GetInstance(i, REVERSE(cur_pilot));
        if (ucmit_ins
            && (GET_TAKEOVER(ucmit_ins->status) == 0) // another coordiator is not taking over this instance
            && GET_STATUS(ucmit_ins->status) < Status::COMMITED
            && !sch_->EliminateNullDep(ucmit_ins)) {
          verify(IsPilot() || IsCopilot());
          take++;
          Log_info(
              "initiate fast-TAKEOVER on {} for slot {} 's dep:"
              " {}, {}, status: {:x}",
              indicator[cur_pilot], cur_slot, indicator[ucmit_ins->is_pilot],
              ucmit_ins->slot_id, ucmit_ins->status);
          // initFastTakeover(ucmit_ins); // Ze: temporarily comment this line for fixing zoo open loop unexpected TAKEOVER problem
        }
      }
    }
    // removed
    //   `uint64_t finish = Time::now(true) - begin;`
    // — `finish` was unused (the only Log_info consumer is
    // commented-out below) and `begin` went away with the other
    // dead timing counters.
  }
  clearStatus();
}

void CoordinatorCopilot::GotoNextPhase() {
  phase_++;
  switch (current_phase_) {
  case Phase::INIT_END:
    current_phase_ = Phase::FAST_ACCEPT;
    if (IsPilot() || IsCopilot()) {
      FastAccept();
    } else {
      // TODO
      verify(0);
    }
    break;
  case Phase::PREPARE:
    if (direct_commit_) {
      current_phase_ = Phase::COMMIT;
      Commit();
    } else {
      current_phase_ = Phase::ACCEPT;
      Accept();
    }
    break;
  case Phase::FAST_ACCEPT:
    if (fast_path_) {
      current_phase_ = Phase::COMMIT;
      Commit();
    } else {
      current_phase_ = Phase::ACCEPT;
      Accept();
    }
    break;
  case Phase::ACCEPT:
    current_phase_ = Phase::COMMIT;
    Commit();
    break;
  default:
    break;
  }
}

void CoordinatorCopilot::initFastTakeover(shared_ptr<CopilotData>& ins) {
  // another coordiator is already taking over this instance
  if (GET_TAKEOVER(ins->status) != 0)
    return;

  if (ins->status >= Status::COMMITED)
    return;
  
  /* When we reach this step, the shared int event should be either READY or TIMEOUT
  thus, update on max_committed_evt should have no effect on it */

  ins->status |= FLAG_TAKEOVER;  // prevent multiple takeover on the same instance
  // reuse current coordinator
  curr_ballot_ = ins->ballot;
  // is_pilot_ = !is_pilot_;
  is_pilot_ = ins->is_pilot;  // takeover another pilot
  slot_id_ = ins->slot_id;
  dep_ = 0; // dependency doesn't matter
  in_fast_takeover_ = true;
  Prepare();
}

inline void CoordinatorCopilot::clearStatus() {
  if (done_)
    return;
  done_ = true;
  curr_ballot_ = 0;
  // Command's reset is via default-construction.
  cmd_now_ = Command{};
  current_phase_ = INIT_END;
  fast_path_ = false;
  direct_commit_ = false;
  in_fast_takeover_ = false;

  is_pilot_ = 0;
  slot_id_ = 0;
  // removed `slot_hint_ = nullptr;` —
  // the field was never read; the frame-side write at frame.cc:85
  // also went away.
  dep_ = 0;
}

} // namespace janus
