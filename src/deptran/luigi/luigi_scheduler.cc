#include "luigi_scheduler.h"

#include <chrono>
#include <iostream>

namespace janus {

//=============================================================================
// Construction / Destruction
//=============================================================================

SchedulerLuigi::SchedulerLuigi() : SchedulerClassic() {
  // Nothing special needed here; vectors/maps init lazily
}

SchedulerLuigi::~SchedulerLuigi() { Stop(); }

//=============================================================================
// Thread Management
//=============================================================================

void SchedulerLuigi::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) return;

  hold_thread_ = new std::thread(&SchedulerLuigi::HoldReleaseTd, this);
  exec_thread_ = new std::thread(&SchedulerLuigi::ExecTd, this);
}

void SchedulerLuigi::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) return;

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

//=============================================================================
// Utility
//=============================================================================

uint64_t SchedulerLuigi::GetMicrosecondTimestamp() {
  auto tse = std::chrono::system_clock::now().time_since_epoch();
  return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(tse).count();
}

//=============================================================================
// LuigiDispatch: Entry Point (called by coordinator)
//=============================================================================

void SchedulerLuigi::LuigiDispatch(txnid_t tx_id,
                                   std::shared_ptr<Marshallable> cmd,
                                   uint64_t send_time,
                                   uint32_t bound,
                                   const std::vector<uint32_t>& local_keys,
                                   std::function<void(const TxnOutput&)> reply_cb) {
  auto entry = std::make_shared<LuigiLogEntry>(tx_id);
  entry->cmd_ = cmd;
  entry->send_time_ = send_time;
  entry->local_deadline_ = send_time + bound;
  entry->local_keys_ = local_keys;
  entry->reply_cb_ = reply_cb;

  // Calculate one-way delay for diagnostics
  uint64_t now = GetMicrosecondTimestamp();
  entry->owd_ = (now > send_time) ? (uint32_t)(now - send_time) : 1000;

  // Enqueue to incoming queue (lock-free, thread-safe)
  // HoldReleaseTd will pick this up, check conflicts, and add to priority queue
  incoming_txn_queue_.enqueue(entry);
}

//=============================================================================
// HoldReleaseTd: The Core of Luigi
//
// This thread runs in a loop and does two things:
// 1. Pulls txns from incoming_txn_queue_, checks conflicts, adds to priority_queue_
// 2. Releases txns from priority_queue_ when their deadline passes, sends to ready_txn_queue_
//
// Why a loop instead of event-driven?
// - ConcurrentQueue doesn't support blocking wait (like Go channels)
// - We add a small sleep when idle to avoid wasting CPU
// - In high-throughput scenarios, the loop stays busy doing useful work
//=============================================================================

void SchedulerLuigi::HoldReleaseTd() {
  std::shared_ptr<LuigiLogEntry> entries[256];  // bulk dequeue buffer

  while (running_) {
    uint64_t now = GetMicrosecondTimestamp();

    //-------------------------------------------------------------------------
    // Phase 1: Pull from incoming_txn_queue_, do conflict check, add to priority_queue_
    //-------------------------------------------------------------------------
    size_t cnt = incoming_txn_queue_.try_dequeue_bulk(entries, 256);
    for (size_t i = 0; i < cnt; i++) {
      auto entry = entries[i];
      uint64_t txn_key = entry->tid_;

      // CONFLICT DETECTION (from Algorithm 1, line 1-4 in paper):
      // Find the maximum lastReleasedDeadline among all keys this txn touches
      uint64_t max_last_released = 0;
      for (auto& k : entry->local_keys_) {
        auto it = last_released_deadlines_.find(k);
        if (it != last_released_deadlines_.end() && it->second > max_last_released) {
          max_last_released = it->second;
        }
      }

      // If txn's deadline is too small (conflict), update it
      // This is the LEADER PRIVILEGE: we can bump the timestamp
      // (In Tiga, followers would reject/abandon here, but we only have leaders)
      if (entry->local_deadline_ <= max_last_released) {
        entry->local_deadline_ = max_last_released + 1;
        // Log for debugging (optional)
        // std::cout << "Luigi: Updated deadline for txn " << txn_key
        //           << " to " << entry->local_deadline_ << std::endl;
      }

      // Insert into priority_queue_ (sorted by deadline, then txn_id)
      priority_queue_[{entry->local_deadline_, txn_key}] = entry;
    }

    //-------------------------------------------------------------------------
    // Phase 2: Release txns whose deadline has passed -> ready_txn_queue_
    //-------------------------------------------------------------------------
    while (!priority_queue_.empty()) {
      auto it = priority_queue_.begin();
      uint64_t deadline = it->first.first;

      if (now < deadline) {
        // Earliest deadline not yet reached, stop releasing
        break;
      }

      // Deadline reached! Release this entry
      auto entry = it->second;
      priority_queue_.erase(it);

      // Update lastReleasedDeadlines for all keys this txn touches
      // (Algorithm 1, line 14-15 in paper)
      for (auto& k : entry->local_keys_) {
        if (last_released_deadlines_[k] < entry->local_deadline_) {
          last_released_deadlines_[k] = entry->local_deadline_;
        }
      }

      // Hand off to execution thread
      ready_txn_queue_.enqueue(entry);
    }

    //-------------------------------------------------------------------------
    // Small sleep to avoid busy-waiting when queues are empty
    // (This is the tradeoff: not truly event-driven, but doesn't hog CPU)
    //-------------------------------------------------------------------------
    if (cnt == 0 && priority_queue_.empty()) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

//=============================================================================
// ExecTd: Execution Thread
//
// This thread:
// 1. Pulls txns from ready_txn_queue_ (deadline has passed, ready to execute)
// 2. Executes the transaction (calls Mako's ExecutePiece machinery)
// 3. For multi-shard txns: triggers timestamp agreement with other leaders
// 4. If agreement fails: rollback, update timestamp, reposition in queue
//
// TODO: Currently a placeholder. Next step is to wire in actual execution.
//=============================================================================

void SchedulerLuigi::ExecTd() {
  std::shared_ptr<LuigiLogEntry> entries[64];

  while (running_) {
    size_t cnt = ready_txn_queue_.try_dequeue_bulk(entries, 64);

    for (size_t i = 0; i < cnt; i++) {
      auto entry = entries[i];

      // TODO: Actual execution logic
      // 1. Create Tx object: auto tx = GetOrCreateTx(entry->tid_);
      // 2. Parse cmd_ into pieces and call ExecutePiece for each
      // 3. For multi-shard: do timestamp agreement
      // 4. On agreement success: commit
      // 5. On agreement failure: rollback, update agreed_deadline_, re-enqueue

      // For now, just log that we would execute
      // std::cout << "Luigi: Would execute txn " << entry->tid_
      //           << " with deadline " << entry->local_deadline_ << std::endl;

      // Call reply callback with empty output (placeholder)
      if (entry->reply_cb_) {
        TxnOutput output;
        entry->reply_cb_(output);
      }
    }

    if (cnt == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

} // namespace janus
