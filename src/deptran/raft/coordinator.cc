
#include "../__dep__.h"
#include "../constants.h"
#include "coordinator.h"
#include "commo.h"
#include "../tpc_command.h"
#include "../procedure.h"
#include "../config.h"
#include "server.h"
#include <std_annotation.hpp>
#include <external_annotations.hpp>
// @external: {
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   Log_warn: [safe, (...) -> void]
//   Log_error: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   std::lock_guard: [safe, (...) -> owned]
//   std::make_shared: [safe, (...) -> owned]
//   std::dynamic_pointer_cast: [safe, (...) -> owned]
//   Config::GetConfig: [safe, () -> *]
//   Reactor::create_sp_event: [safe, (...) -> owned]
//   operator bool: [safe, (&'a) -> bool]
//   janus::View::View: [safe, (...) -> owned]
// }

namespace janus {

// @safe
CoordinatorRaft::CoordinatorRaft(uint32_t coo_id,
                                             int32_t benchmark,
                                             rusty::Option<rusty::Arc<ClientStatus>> client_status,
                                             uint32_t thread_id)
    : Coordinator(coo_id, benchmark, std::move(client_status), thread_id),
      slot_hint_(rusty::Arc<rusty::Cell<slotid_t>>::make(0)) {
}

// @safe - raw pointer svr_ is bounded (set in constructor, outlives this object)
bool CoordinatorRaft::IsLeader() {
   // @unsafe
   {
   return this->svr_->IsLeader() ;
   }
}

// @unsafe - external calls marked @external [safe], pointer ops in @unsafe blocks
void CoordinatorRaft::Submit(const janus::Command& cmd_env,
                                   rusty::Function<void()> func,
                                   rusty::Function<void()> exe_callback) {
  if (!IsLeader()) {
    // verify(0);
    auto config = Config::GetConfig();
    // @unsafe
    {
    auto& site = config->SiteById(svr_->site_id_);
    Log_info("[WRONG_LEADER] Submit to server {} (loc_id {}) which is not leader (currentTerm={}, commitIndex={}, lastLogIndex={})",
             svr_->site_id_, loc_id_, svr_->currentTerm, svr_->commitIndex, svr_->lastLogIndex);
    Log_info("[WRONG_LEADER] Server {} site info: host={} locale_id={} partition={}", svr_->site_id_, site.host.c_str(), site.locale_id, site.partition_id_);
    }

    // Handle WRONG_LEADER case
    if (cmd_env.kind_ == TpcCommitCommand::static_kind()) {
      const auto tpc_cmd = marshallable_cast<TpcCommitCommand>(cmd_env);
      if (tpc_cmd.is_some()) {
        // Set WRONG_LEADER error code
        // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
        { auto& mut_cmd = *const_cast<TpcCommitCommand*>(tpc_cmd.unwrap().get()); mut_cmd.ret_ = WRONG_LEADER; }

        // Use the last view published by this Raft server.
        View current_view;
        // @unsafe
        {
        current_view = svr_->GetCurrentView();

        Log_info("[WRONG_LEADER] Server {} retrieving view: {}",
                 svr_->site_id_, current_view.ToString().c_str());
        }

        // If view is empty or stale, use current server state to construct view
        if (current_view.IsEmpty()) {
          // For Raft, we need to determine who the current leader is
          // This might need to be tracked separately or obtained from Raft state
          // @unsafe
          {
          int n_replicas = Config::GetConfig()->GetPartitionSize(par_id_);
          current_view = View(n_replicas,
                            -1,  // Unknown leader for now
                            svr_->currentTerm);
          }
          Log_info("[WRONG_LEADER] View was empty, created new view with unknown leader: {}",
                   current_view.ToString().c_str());
        }

        // Attach view data to the command for propagation back to client
        // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
        {
          auto& mut_cmd = *const_cast<TpcCommitCommand*>(tpc_cmd.unwrap().get());
          mut_cmd.sp_view_data_ = rusty::Option<rusty::Arc<ViewData>>(
              rusty::Arc<ViewData>::make(current_view, par_id_));
        }
        Log_info("[WRONG_LEADER] Attached view data to response for partition {}: {}",
                 par_id_, tpc_cmd.unwrap()->sp_view_data_.unwrap()->ToString().c_str());
      }
    }

    // Still call the callback to signal completion, but with error status
    func();
    // [Jetpack] Even wrong leader, need a reply to call callback function to update view to avoid wrong leader again next time.
    // Pass 0 as log index since we're not actually committing (WRONG_LEADER error path)
    // @unsafe
    {
    svr_->app_next_(0, cmd_env);
    }
    return;
  } else {
    // Log_info("[YYYYY] Submit to loc_id {}, which is leader. Command kind={}, is_recovery={}",
    //          loc_id_, cmd_env.has_value() ? cmd_env.kind_ : -1, SimpleRWCommand(cmd_env.inner_marshallable()).IsRecoveryCommand());
  }
	std::lock_guard<std::recursive_mutex> lock(mtx_);

  verify(!in_submission_);
  verify(!cmd_.has_value());
//  verify(cmd.self_cmd_ != nullptr);
  in_submission_ = true;
  cmd_ = cmd_env;
  verify(cmd_.has_value());
  commit_callback_ = std::move(func);
  GotoNextPhase();
}

// @unsafe - external calls marked @external [safe], address-of ops in @unsafe blocks
void CoordinatorRaft::AppendEntries() {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    verify(!in_append_entries);
    // verify(this->svr_->IsLeader()); TODO del it yidawu
    in_append_entries = true;
    uint64_t index = 0, term = 0;
    bool ok;
    // @unsafe
    {
    ok = this->svr_->Start(cmd_, &index, &term);
    }
    verify(ok);
    // @unsafe
    {
    std::lock_guard<std::recursive_mutex> lock(svr_->ready_for_replication_mtx_);
    if (svr_->ready_for_replication_.is_some())
      svr_->ready_for_replication_.as_ref().unwrap()->set(1);
    }

    // @unsafe
    {
    while (this->svr_->commitIndex < index) {
      create_sp_timeout_event(1000)->wait();
      if (this->svr_->currentTerm != term) {
        Log_info("Term changed during AppendEntries: expected {}, got {}. Leader changed.",
                 term, this->svr_->currentTerm);
        // The command may or may not be committed by the new leader
        // Mark as not committed and let higher layers retry
        committed_ = false;
        in_append_entries = false;
        return;
      }
    }
    }

    committed_ = true;
}

// @unsafe - calls Log_warn (non-borrow-checked I/O), mutex, callback
void CoordinatorRaft::Commit() {
  // @unsafe { Log_warn is not borrow-checked }
  Log_warn("[RAFT] CoordinatorRaft::Commit called but not expected in Raft mode");
  // @unsafe
  {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  commit_callback_();
  }
  verify(phase_ == Phase::COMMIT);
  GotoNextPhase();
}

// @safe
void CoordinatorRaft::LeaderLearn() {
    // @unsafe
    {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    commit_callback_();
    }
    verify(phase_ == Phase::COMMIT);
    // @unsafe
    { GotoNextPhase(); }
}

// @unsafe - calls @safe AppendEntries and @safe LeaderLearn
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
        // TODO: non-leader should forward to leader
        // @unsafe { Log_warn is not borrow-checked }
        Log_warn("[RAFT] CoordinatorRaft::GotoNextPhase: non-leader path not yet implemented, skipping to COMMIT");
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
      // @unsafe { Log_error is not borrow-checked }
      Log_error("[RAFT] CoordinatorRaft::GotoNextPhase: unexpected phase {}", current_phase);
      break;
  }
}

} // namespace janus
