
#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"
#include "../classic/tpc_command.h"
#include "../procedure.h"
#include "../config.h"
#include "server.h"
#include <std_annotation.hpp>
#include <external_annotations.hpp>
// External annotations for std library template functions
// @external: {
//   dynamic_pointer_cast: [unsafe, template<T, U>(const std::shared_ptr<U>& ptr) -> std::shared_ptr<T> where ptr: 'a, return: 'a]
//   operator bool: [unsafe, () -> bool]
//   rrr::RandomGenerator::rand_double: [unsafe, (double, double) -> double]
// }

namespace janus {

// @safe
CoordinatorRaft::CoordinatorRaft(uint32_t coo_id,
                                             int32_t benchmark,
                                             ClientControlServiceImpl* ccsi,
                                             uint32_t thread_id)
    : Coordinator(coo_id, benchmark, ccsi, thread_id),
      slot_hint_(rusty::Arc<rusty::Cell<slotid_t>>::make(0)) {
}

// @safe
bool CoordinatorRaft::IsLeader() {
   return this->svr_->IsLeader() ;
}

// @safe - Calls svr_->IsFPGALeader() which is now marked @safe
bool CoordinatorRaft::IsFPGALeader() {
   return this->svr_->IsFPGALeader() ;
}

// @safe
void CoordinatorRaft::Submit(shared_ptr<Marshallable>& cmd,
                                   const function<void()>& func,
                                   const function<void()>& exe_callback) {
  if (!IsLeader()) {
    // verify(0);
    auto config = Config::GetConfig();
    auto& site = config->SiteById(svr_->site_id_);
    Log_info("[WRONG_LEADER] Submit to server %d (loc_id %d) which is not leader (currentTerm=%lu, commitIndex=%lu, lastLogIndex=%lu)",
             svr_->site_id_, loc_id_, svr_->currentTerm, svr_->commitIndex, svr_->lastLogIndex);
    Log_info("[WRONG_LEADER] Server %d site info: host=%s locale_id=%d partition=%d", svr_->site_id_, site.host.c_str(), site.locale_id, site.partition_id_);
    
    // Handle WRONG_LEADER case
    if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT) {
      auto tpc_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
      if (tpc_cmd) {
        // Set WRONG_LEADER error code
        tpc_cmd->ret_ = WRONG_LEADER;
        
        // Get current view from TxLogServer (parent class)
        // The new_view_ contains the most recent view information
        View current_view = svr_->new_view_;
        
        Log_info("[WRONG_LEADER] Server %d retrieving view: %s", 
                 svr_->site_id_, current_view.ToString().c_str());
        
        // If view is empty or stale, use current server state to construct view
        if (current_view.IsEmpty()) {
          // For Raft, we need to determine who the current leader is
          // This might need to be tracked separately or obtained from Raft state
          int n_replicas = Config::GetConfig()->GetPartitionSize(par_id_);
          current_view = View(n_replicas, 
                            -1,  // Unknown leader for now
                            svr_->currentTerm);
          Log_info("[WRONG_LEADER] View was empty, created new view with unknown leader: %s", 
                   current_view.ToString().c_str());
        }
        
        // Attach view data to the command for propagation back to client
        tpc_cmd->sp_view_data_ = std::make_shared<ViewData>(current_view, par_id_);
        Log_info("[WRONG_LEADER] Attached view data to response for partition %d: %s", 
                 par_id_, tpc_cmd->sp_view_data_->ToString().c_str());
      }
    }
    
    // Still call the callback to signal completion, but with error status
    func();
    // [Jetpack] Even wrong leader, need a reply to call callback function to update view to avoid wrong leader again next time.
    // Pass 0 as log index since we're not actually committing (WRONG_LEADER error path)
    svr_->app_next_(0, cmd);
    return;
  } else {
    // Log_info("[YYYYY] Submit to loc_id %d, which is leader. Command kind=%d, is_recovery=%d", 
    //          loc_id_, cmd ? cmd->kind_ : -1, SimpleRWCommand(cmd).IsRecoveryCommand());
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

// @safe - Uses @unsafe blocks for pointer operations
void CoordinatorRaft::AppendEntries() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    verify(!in_append_entries);
    // verify(this->svr_->IsLeader()); TODO del it yidawu
    in_append_entries = true;
    uint64_t index, term;
    bool ok;
    // @unsafe
    {
      ok = this->svr_->Start(cmd_, &index, &term); // address-of for output params
    }
    verify(ok);
    // @unsafe
    {
      if (svr_->ready_for_replication_ != nullptr)
        svr_->ready_for_replication_->Set(1);
    }

    while (this->svr_->commitIndex < index) {
      Reactor::CreateSpEvent<TimeoutEvent>(1000)->Wait();
      if (this->svr_->currentTerm != term) {
        Log_info("Term changed during AppendEntries: expected %lu, got %lu. Leader changed.", 
                 term, this->svr_->currentTerm);
        // The command may or may not be committed by the new leader
        // Mark as not committed and let higher layers retry
        committed_ = false;
        in_append_entries = false;
        return;
      }
    }
    
    committed_ = true;
}

// @safe - Calls unmarked GotoNextPhase()
void CoordinatorRaft::Commit() {
  verify(0);
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  commit_callback_();
  verify(phase_ == Phase::COMMIT);
  GotoNextPhase();
}

// @safe - Calls unmarked GotoNextPhase()
void CoordinatorRaft::LeaderLearn() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    commit_callback_();
    verify(phase_ == Phase::COMMIT);
    GotoNextPhase();
}

// @safe
void CoordinatorRaft::GotoNextPhase() {
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
        // Forward(cmd_,commit_callback_) ;
        phase_ = Phase::COMMIT;
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