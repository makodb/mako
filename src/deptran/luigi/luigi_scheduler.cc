#include "luigi_scheduler.h"

#include <chrono>
#include <iostream>
#include <functional>

// Mako includes for execution integration
#include "deptran/s_main.h"  // For add_log_to_nc

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
// LuigiDispatchFromRequest: Entry Point from server.cc
//
// Creates a LuigiLogEntry from parsed request data and enqueues it.
//=============================================================================

void SchedulerLuigi::LuigiDispatchFromRequest(
    uint64_t txn_id,
    uint64_t send_time,
    uint32_t bound,
    const std::vector<LuigiOp>& ops,
    std::function<void(int status, uint64_t commit_ts, const std::vector<std::string>& read_results)> reply_cb) {
  
  auto entry = std::make_shared<LuigiLogEntry>(txn_id);
  entry->send_time_ = send_time;
  entry->bound_ = bound;
  entry->local_deadline_ = send_time + bound;
  entry->ops_ = ops;
  entry->reply_cb_ = reply_cb;

  // Extract keys for conflict detection
  // We use a simple hash of (table_id, key) as the conflict key
  for (const auto& op : ops) {
    // Simple key hash: combine table_id and first few bytes of key
    uint32_t conflict_key = op.table_id;
    if (op.key.size() >= 4) {
      conflict_key ^= *reinterpret_cast<const uint32_t*>(op.key.data());
    }
    entry->local_keys_.push_back(conflict_key);
  }

  // Calculate one-way delay for diagnostics
  uint64_t now = GetMicrosecondTimestamp();
  entry->owd_ = (now > send_time) ? (uint32_t)(now - send_time) : 1000;

  // Enqueue to incoming queue (lock-free, thread-safe)
  incoming_txn_queue_.enqueue(entry);
}

//=============================================================================
// LuigiDispatch: Original Entry Point (for deptran-style compatibility)
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
  entry->bound_ = bound;
  entry->local_deadline_ = send_time + bound;
  entry->local_keys_ = local_keys;
  
  // Wrap the old-style callback
  entry->reply_cb_ = [reply_cb](int status, uint64_t commit_ts, const std::vector<std::string>& read_results) {
    TxnOutput output;
    // Convert read_results to TxnOutput if needed
    if (reply_cb) {
      reply_cb(output);
    }
  };

  uint64_t now = GetMicrosecondTimestamp();
  entry->owd_ = (now > send_time) ? (uint32_t)(now - send_time) : 1000;

  incoming_txn_queue_.enqueue(entry);
}

//=============================================================================
// HoldReleaseTd: The Core of Luigi
//
// This thread runs in a loop and does two things:
// 1. Pulls txns from incoming_txn_queue_, checks conflicts, adds to priority_queue_
// 2. Releases txns from priority_queue_ when their deadline passes, sends to ready_txn_queue_
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
      if (entry->local_deadline_ <= max_last_released) {
        entry->local_deadline_ = max_last_released + 1;
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
    //-------------------------------------------------------------------------
    if (cnt == 0 && priority_queue_.empty()) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

//=============================================================================
// ExecuteEntry: Execute a single transaction
//
// This is where we integrate with Mako's execution machinery.
// For now, we implement a simplified version that:
// 1. Processes all operations
// 2. Triggers replication via add_log_to_nc (same as Mako)
// 3. Calls the reply callback
//=============================================================================

void SchedulerLuigi::ExecuteEntry(std::shared_ptr<LuigiLogEntry> entry) {
  entry->exec_status_.store(LUIGI_EXEC_DIRECT);
  
  // The commit timestamp is the deadline at which we're executing
  uint64_t commit_ts = entry->local_deadline_;
  int status = 0;  // SUCCESS
  
  // TODO: Actual execution logic would go here
  // For now, we simulate successful execution
  // In a real implementation:
  // 1. For reads: query the database, store results in entry->read_results_
  // 2. For writes: apply writes to database
  // 3. Call shard_serialize_util() equivalent for replication
  
  // Process operations (placeholder - actual DB interaction needed)
  entry->read_results_.clear();
  for (auto& op : entry->ops_) {
    if (op.op_type == 0) {
      // Read operation - would query DB here
      // For now, return empty result
      entry->read_results_.push_back("");
    } else {
      // Write operation - would apply to DB here
    }
    op.executed = true;
  }
  
  // Trigger replication (using Mako's background Paxos)
  // This is where we serialize the log entry and submit to Paxos workers
  // The actual implementation would serialize entry->ops_ into a log buffer
  // and call add_log_to_nc(log_buffer, log_len, partition_id_)
  
  // For now, we skip actual replication and just mark as done
  // TODO: Implement log serialization and call add_log_to_nc
  
  entry->exec_status_.store(LUIGI_EXEC_DONE);
  
  // Call reply callback
  if (entry->reply_cb_) {
    entry->reply_cb_(status, commit_ts, entry->read_results_);
  }
}

//=============================================================================
// ExecTd: Execution Thread
//
// This thread:
// 1. Pulls txns from ready_txn_queue_ (deadline has passed, ready to execute)
// 2. Executes each transaction via ExecuteEntry()
//=============================================================================

void SchedulerLuigi::ExecTd() {
  std::shared_ptr<LuigiLogEntry> entries[64];

  while (running_) {
    size_t cnt = ready_txn_queue_.try_dequeue_bulk(entries, 64);

    for (size_t i = 0; i < cnt; i++) {
      ExecuteEntry(entries[i]);
    }

    if (cnt == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
}

} // namespace janus
