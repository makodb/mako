
#include "coordinator.h"
#include "frame.h"
#include "benchmark_control_rpc.h"

namespace janus {

/** thread safe */

void CoordinatorNone::GotoNextPhase() {
  Log_debug("GoToNextPhase on client side");
  // uint64_t coroutine_id = Coroutine::CurrentCoroutine()->id;
  // uint64_t coroutine_global_id = Coroutine::CurrentCoroutine()->global_id;
  // Log_info("Enter GotoNextPhase CoroutineID %d %d phase_ = %d", Coroutine::CurrentCoroutine()->id, Coroutine::CurrentCoroutine()->global_id, phase_);
  int n_phase = 2;
  // int judgement_phase = phase_;
  switch (phase_++ % n_phase) {
    case Phase::INIT_END:
      // Log_info("Enter switch CoroutineID %d %d phase_ = %d", Coroutine::CurrentCoroutine()->id, Coroutine::CurrentCoroutine()->global_id, phase_);
      verify(phase_ % n_phase == Phase::DISPATCH);
      dispatch_time_ = SimpleRWCommand::GetCurrentMsTime();
      dispatch_duration_3_times_ = (dispatch_time_ - created_time_) * 3;
      DispatchAsync();
      break;
    case Phase::DISPATCH:
      // Log_info("Enter switch CoroutineID %d %d phase_ = %d", Coroutine::CurrentCoroutine()->id, Coroutine::CurrentCoroutine()->global_id, phase_);
      committed_ = true;
      verify(phase_ % n_phase == Phase::INIT_END);
      if (dispatch_duration_3_times_ > Config::GetConfig()->duration_ * 1000 && dispatch_duration_3_times_ < Config::GetConfig()->duration_ * 2 * 1000) {
        client_worker_->cli2cli_[3].append(SimpleRWCommand::GetCurrentMsTime() - dispatch_time_);
        client_worker_->cli2cli_[4].append(SimpleRWCommand::GetCurrentMsTime() - dispatch_time_);
        client_worker_->cli2cli_[5].append(SimpleRWCommand::GetCurrentMsTime() - dispatch_time_);
      }
      // Log_info("End");
      client_worker_->commit_time_.push_back(std::make_pair(dispatch_time_ - created_time_, SimpleRWCommand::GetCurrentMsTime() - dispatch_time_));
      End();
      break;
    default:
      verify(0);
  }
}

} // namespace janus
