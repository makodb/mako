
#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"

#include "server.h"
#include "../RW_command.h"

namespace janus {

CoordinatorFpgaRaft::CoordinatorFpgaRaft(uint32_t coo_id,
                                             int32_t benchmark,
                                             rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                             uint32_t thread_id)
    : Coordinator(coo_id, benchmark, std::move(client_status), thread_id) {
}

CoordinatorFpgaRaft::~CoordinatorFpgaRaft() {
  // Log_info("coordinator loc_id_={}, client2leader_ 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", loc_id_, client2leader_.pct50(), client2leader_.pct90(), client2leader_.pct99());
  // Log_info("coordinator loc_id_={}, client2test_point_ 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", loc_id_, client2test_point_.pct50(), client2test_point_.pct90(), client2test_point_.pct99());
  // Log_info("coordinator loc_id_={}, client2leader_send_ 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", loc_id_, client2leader_send_.pct50(), client2leader_send_.pct90(), client2leader_send_.pct99());
}

bool CoordinatorFpgaRaft::IsLeader() {
   return this->sch_->IsLeader() ;
}

bool CoordinatorFpgaRaft::IsFPGALeader() {
   return this->sch_->IsFPGALeader() ;
}

// removed `CoordinatorFpgaRaft::Forward`
// — body started with `verify(0); // TODO delete it` and the only
// upstream caller would have been a forwarding-from-follower path
// that was never wired up.  Companion `FpgaRaftCommo::SendForward`,
// `FpgaRaftForwardQuorumEvent`, and the FpgaRaft::Forward RPC
// handler chain (`FpgaRaftServiceImpl::Forward` +
// `FpgaRaftServer::OnForward`) are also gone in this phase.


void CoordinatorFpgaRaft::Submit(const janus::Command& cmd,
                                   rusty::Function<void()> func,
                                   rusty::Function<void()> exe_callback) {
#ifdef LATENCY_LOG_DEBUG
  Log_info("Time of cmd <{}, {}> arrive svr {} Submit: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd).first, SimpleRWCommand::GetCmdID(cmd).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
  // client2leader_.append(SimpleRWCommand::GetCommandMsTimeElaps(cmd));
  if (!IsLeader()) {
    // removed `Forward(cmd, ...)` call —
    // `Forward` method deleted (was `verify(0)`-tagged dead code).
    // Treat non-leader submission as a hard failure for now —
    // production behavior was already a crash via the dead Forward.
    verify(0); // not the leader; non-leader submission unsupported
    return ;
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

void CoordinatorFpgaRaft::AppendEntries() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    verify(!in_append_entries);
    // verify(this->sch_->IsLeader()); TODO del it yidawu
    in_append_entries = true;
    Log_debug("fpga-raft coordinator broadcasts append entries, "
                  "par_id_: {:x}, slot_id: {:x}, lastLogIndex: {}",
              par_id_, slot_id_, this->sch_->lastLogIndex);
    /* Should we use slot_id instead of lastLogIndex and balot instead of term? */
    uint64_t prevLogIndex = this->sch_->lastLogIndex;

    /*this->sch_->lastLogIndex += 1;
    auto instance = this->sch_->GetFpgaRaftInstance(this->sch_->lastLogIndex);

    instance->log_ = cmd_;
    instance->term = this->sch_->currentTerm;*/

    /* TODO: get prevLogTerm based on the logs */
    uint64_t prevLogTerm = this->sch_->currentTerm;
		this->sch_->SetLocalAppend(cmd_, &prevLogTerm, &prevLogIndex, slot_id_, curr_ballot_) ;

#ifdef LATENCY_LOG_DEBUG
    Log_info("Time of cmd <{}, {}> arrive svr {} Before BroadcastAppendEntries: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd_).first, SimpleRWCommand::GetCmdID(cmd_).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
    auto sp_quorum = commo()->BroadcastAppendEntries(par_id_,
                                                     this->sch_->site_id_,
                                                     slot_id_,
                                                     dep_id_,
                                                     curr_ballot_,
                                                     this->sch_->IsLeader(),
                                                     this->sch_->currentTerm,
                                                     prevLogIndex,
                                                     prevLogTerm,
                                                     /* ents, */
                                                     this->sch_->commitIndex,
                                                     cmd_);

		struct timespec start_;
		clock_gettime(CLOCK_MONOTONIC, &start_);
    sp_quorum->wait();
		struct timespec end_;
		clock_gettime(CLOCK_MONOTONIC, &end_);

		// quorum_events_.push_back(sp_quorum);
		// Log_info("time of Wait(): {}", (end_.tv_sec-start_.tv_sec)*1000000000 + end_.tv_nsec-start_.tv_nsec);
		slow_ = sp_quorum->is_slow();
		
		long leader_time;
		std::vector<long> follower_times {};

		int total_ob = 0;
		int avg_ob = 0;
		//Log_info("begin_index: {}", commo()->begin_index);
		if (commo()->begin_index >= 1000) {
			if (commo()->ob_index < 100) {
				commo()->outbounds[commo()->ob_index] = commo()->outbound;
				commo()->ob_index++;
			} else {
				for (int i = 0; i < 99; i++) {
					commo()->outbounds[i] = commo()->outbounds[i+1];
					total_ob += commo()->outbounds[i];
				}
				commo()->outbounds[99] = commo()->outbound;
				total_ob += commo()->outbounds[99];
			}
			commo()->begin_index = 0;
		} else {
			commo()->begin_index++;
		}
		avg_ob = total_ob/100;

		for (auto it = commo()->rpc_clients_.begin(); it != commo()->rpc_clients_.end(); it++) {
			if (avg_ob > 0 && it->second->time() > 0) Log_info("time for {} is: {}", it->first, it->second->time()/avg_ob);
			if (it->first != loc_id_) {
				follower_times.push_back(it->second->time());
			}
		}
		if (avg_ob > 0 && !slow_) {
			Log_debug("number of rpcs: {}", avg_ob);
			Log_debug("{} and {}", follower_times[0]/avg_ob, follower_times[1]/avg_ob);
			slow_ = follower_times[0]/avg_ob > 80000 && follower_times[1]/avg_ob > 80000;
		}

		//Log_info("slow?: {}", slow_);
    if (sp_quorum->yes()) {
        minIndex = sp_quorum->minIndex;
				//Log_info("{} vs {}", minIndex, this->sch_->commitIndex);
        verify(minIndex >= this->sch_->commitIndex) ;
        committed_ = true;
        Log_debug("fpga-raft append commited loc:{} minindex:{}", loc_id_, minIndex ) ;
    }
    else if (sp_quorum->no()) {
        verify(0);
        // removed `Forward(cmd_,
        // commit_callback_)` call inside this `verify(0)`-guarded
        // unreachable branch — `Forward` method deleted.
    }
    else {
        verify(0);
    }
}

void CoordinatorFpgaRaft::Commit() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // Log_info("About to commit slot {} <{}, {}> key {}", slot_id_,
  //   SimpleRWCommand::GetCmdID(cmd_).first, SimpleRWCommand::GetCmdID(cmd_).second, SimpleRWCommand::GetKey(cmd_));
  commit_callback_();
  Log_debug("fpga-raft broadcast commit for partition: {}, slot {}",
            (int) par_id_, (int) slot_id_);
#ifdef LATENCY_LOG_DEBUG
  // GetCmdID still takes shared_ptr<Marshallable>.
  Log_info("Time of cmd <{}, {}> arrive svr {} Before BroadcastDecide: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd_).first, SimpleRWCommand::GetCmdID(cmd_).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
  commo()->BroadcastDecide(par_id_, slot_id_, dep_id_, curr_ballot_, cmd_);
  verify(phase_ == Phase::COMMIT);
  GotoNextPhase();
}

void CoordinatorFpgaRaft::LeaderLearn() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    commit_callback_();
    uint64_t prevCommitIndex = this->sch_->commitIndex;
    verify(minIndex >= prevCommitIndex);
    this->sch_->commitIndex = std::max(this->sch_->commitIndex, minIndex);
    Log_debug("fpga-raft commit for partition: {}, slot {}, commit {} minIndex {} in loc:{}",
      (int) par_id_, (int) slot_id_, sch_->commitIndex, minIndex, loc_id_);

    /* if (prevCommitIndex < this->sch_->commitIndex) { */
    /*     auto instance = this->sch_->GetFpgaRaftInstance(this->sch_->commitIndex); */
    /*     this->sch_->app_next_(*instance->log_); */
    /* } */
#ifdef LATENCY_LOG_DEBUG
    // GetCmdID still takes shared_ptr<Marshallable>.
  Log_info("Time of cmd <{}, {}> arrive svr {} Before BroadcastDecide: {:.2f}ms", SimpleRWCommand::GetCmdID(cmd_).first, SimpleRWCommand::GetCmdID(cmd_).second, loc_id_, SimpleRWCommand::GetMsTimeElaps());
#endif
    commo()->BroadcastDecide(par_id_, slot_id_, dep_id_, curr_ballot_, cmd_);
    verify(phase_ == Phase::COMMIT);
    GotoNextPhase();
}

void CoordinatorFpgaRaft::GotoNextPhase() {
  int n_phase = 4;
  int current_phase = phase_ % n_phase;
  phase_++;
  switch (current_phase) {
    case Phase::INIT_END:
      if (IsLeader()) {
        phase_++; // skip prepare phase for "leader"
        verify(phase_ % n_phase == Phase::ACCEPT);
        AppendEntries();
        phase_++;
        verify(phase_ % n_phase == Phase::COMMIT);
      } else {
        // TODO
        verify(0);
        // removed `Forward(cmd_,
        // commit_callback_)` and `phase_ = Phase::COMMIT;` inside
        // this `verify(0)`-guarded unreachable branch — `Forward`
        // method deleted.
      }
    case Phase::ACCEPT:
      verify(phase_ % n_phase == Phase::COMMIT);
      if (committed_) {
        LeaderLearn();
      } else {
        // verify(0);
        // Forward(cmd_,commit_callback_) ;
        phase_ = Phase::COMMIT;
      }
      break;
    case Phase::PREPARE:
      verify(phase_ % n_phase == Phase::ACCEPT);
      AppendEntries();
      break;
    case Phase::COMMIT:
      // do nothing.
      break;
    default:
      verify(0);
  }
}

} // namespace janus
