#include "luigi_scheduler.h"

#include <chrono>
#include <iostream>

namespace janus {

SchedulerLuigi::SchedulerLuigi() : SchedulerClassic() {
  // lazy init of vectors may happen in Start() when txn registry / schema
  // information is available.
}

SchedulerLuigi::~SchedulerLuigi() { Stop(); }

void SchedulerLuigi::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;

  // initialize per-key vectors using txn registry if available
  // For now keep empty; will be sized when schema info is known.

  hold_thread_ = new std::thread(&SchedulerLuigi::HoldReleaseTd, this);
  exec_thread_ = new std::thread(&SchedulerLuigi::ExecTd, this);
}

void SchedulerLuigi::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) return;
  hold_cv_.notify_all();
  if (hold_thread_) {
    hold_thread_->join();
    delete hold_thread_;
    hold_thread_ = nullptr;
  }
  if (exec_thread_) {
    exec_thread_->join();
    delete exec_thread_;
    exec_thread_ = nullptr;
  }
}

void SchedulerLuigi::LuigiDispatch(txnid_t tx_id,
                                   std::shared_ptr<Marshallable> cmd,
                                   uint64_t send_time,
                                   uint32_t bound,
                                   std::function<void(const TxnOutput&)> reply_cb) {
  auto entry = std::make_shared<LuigiLogEntry>(tx_id);
  entry->cmd_ = cmd;
  entry->send_time_ = send_time;
  entry->local_deadline_ = send_time + bound;
  entry->reply_cb_ = reply_cb;

  // NOTE: for now we don't compute local_keys_. The next step will
  // populate local_keys_ using the scheduler's txn registry / pre-processing
  // logic similar to SchedulerClassic::DispatchPiece.

  InsertIntoHoldBuffer(entry);
}

void SchedulerLuigi::InsertIntoHoldBuffer(std::shared_ptr<LuigiLogEntry> entry) {
  std::unique_lock<std::mutex> lk(hold_mtx_);
  hold_buffer_[{entry->local_deadline_, entry->tid_}] = entry;
  hold_cv_.notify_one();
}

void SchedulerLuigi::HoldReleaseTd() {
  std::unique_lock<std::mutex> lk(hold_mtx_);
  while (running_) {
    if (hold_buffer_.empty()) {
      hold_cv_.wait_for(lk, std::chrono::milliseconds(10));
      continue;
    }
    auto it = hold_buffer_.begin();
    uint64_t now = (uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (it->first.first <= now) {
      auto entry = it->second;
      // move entry to execution - for now we simply call ExecTd via a queue
      // For simplicity, directly perform a minimal "execute" action here.

      // TODO: real conflict check, spec/commit logic, and timestamp agreement
      // For now, run DispatchPiece/ExecutePiece (synchronous) as a placeholder

      // perform pre-dispatch: create Tx object and run DispatchPiece
      // We'll use the existing SchedulerClassic::DispatchPiece/ExecutePiece
      // helpers in follow-up steps when we populate entry->cmd_

      // remove from hold buffer
      hold_buffer_.erase(it);

      // TODO: hand off to ExecTd properly; for now we just call ExecutePiece
      // synchronously as a placeholder

    } else {
      // Wait a little until next deadline
      uint64_t wait_us = it->first.first - now;
      if (wait_us > 1000000) wait_us = 1000000; // cap to 1s
      hold_cv_.wait_for(lk, std::chrono::microseconds(wait_us));
    }
  }
}

void SchedulerLuigi::ExecTd() {
  // Placeholder execution loop. Real implementation will consume a
  // hand-off queue from HoldReleaseTd and perform spec/direct execution
  // similar to Tiga's ExecTd.
  while (running_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

} // namespace janus
