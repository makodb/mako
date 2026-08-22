
#include "coordinator.h"
#include "frame.h"
#include "benchmark_control_rpc.h"

namespace janus {

/** thread safe */

void CoordinatorNone::GotoNextPhase() {
  Log_debug("GoToNextPhase on client side");
  // uint64_t coroutine_id = Fiber::current_fiber()->id;
  // uint64_t coroutine_global_id = Fiber::current_fiber()->global_id;
  // Log_info("Enter GotoNextPhase CoroutineID {} {} phase_ = {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id, phase_);
  int n_phase = 2;
  // int judgement_phase = phase_;
  switch (phase_++ % n_phase) {
    case Phase::INIT_END:
      // Log_info("Enter switch CoroutineID {} {} phase_ = {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id, phase_);
      verify(phase_ % n_phase == Phase::DISPATCH);
      dispatch_time_ = SimpleRWCommand::GetCurrentMsTime();
      dispatch_duration_3_times_ = (dispatch_time_ - created_time_) * 3;
      DispatchAsync();
      break;
    case Phase::DISPATCH:
      // Log_info("Enter switch CoroutineID {} {} phase_ = {}", Fiber::current_fiber()->id, Fiber::current_fiber()->global_id, phase_);
      committed_ = true;
      verify(phase_ % n_phase == Phase::INIT_END);
      // Log_info("End");
      client_worker_->commit_time_.push_back(std::make_pair(dispatch_time_ - created_time_, SimpleRWCommand::GetCurrentMsTime() - dispatch_time_));
      End();
      break;
    default:
      verify(0);
  }
}

} // namespace janus
