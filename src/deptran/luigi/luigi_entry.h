#pragma once

#include "../__dep__.h"
#include "../scheduler.h"
#include "../tx.h"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace janus {

//=============================================================================
// Execution Status (mirrors Tiga's EXEC_* states)
//=============================================================================
enum LuigiExecStatus {
  LUIGI_EXEC_INIT = 0,       // Not started
  LUIGI_EXEC_SPEC = 1,       // Speculatively executing (before agreement)
  LUIGI_EXEC_DIRECT = 2,     // Direct execution (agreement already done or single-shard)
  LUIGI_EXEC_COMMITTING = 3, // Agreement succeeded, committing
  LUIGI_EXEC_ROLLBACK = 4,   // Agreement failed, need to rollback
  LUIGI_EXEC_DONE = 5,       // Execution complete
  LUIGI_EXEC_ABANDONED = 6   // Txn abandoned (conflict, will not execute)
};

//=============================================================================
// Agreement Status (for multi-shard timestamp agreement)
//=============================================================================
enum LuigiAgreeStatus {
  LUIGI_AGREE_INIT = 0,      // Not started
  LUIGI_AGREE_PENDING = 1,   // Waiting for other leaders
  LUIGI_AGREE_COMPLETE = 2,  // All leaders agreed
  LUIGI_AGREE_CONFLICT = 3   // Conflict detected, need to re-agree with updated timestamp
};

//=============================================================================
// LuigiLogEntry: Container for one transaction as it flows through Luigi
//=============================================================================
struct LuigiLogEntry {
  //--- Timestamps ---
  uint64_t local_deadline_ = 0;   // Proposed timestamp (may be updated on conflict)
  uint64_t agreed_deadline_ = 0;  // Final agreed timestamp (after multi-shard agreement)

  //--- Status flags (atomic for thread-safe reads) ---
  std::atomic<uint32_t> exec_status_{LUIGI_EXEC_INIT};
  std::atomic<uint32_t> agree_status_{LUIGI_AGREE_INIT};

  //--- Transaction identity ---
  txnid_t tid_ = 0;                              // Unique transaction ID
  std::shared_ptr<Marshallable> cmd_ = nullptr;  // Command payload

  //--- Callback to return result to coordinator ---
  std::function<void(const TxnOutput&)> reply_cb_ = nullptr;

  //--- Keys touched by this txn on THIS shard (for conflict detection) ---
  std::vector<uint32_t> local_keys_;

  //--- Timing info ---
  uint64_t send_time_ = 0;  // When coordinator sent the txn
  uint32_t owd_ = 0;        // One-way delay (microseconds)

  //--- For multi-shard txns: which shards are involved ---
  std::set<uint32_t> involved_shards_;
  uint32_t num_shards_ = 1;  // 1 = single-shard, >1 = multi-shard

  //--- Result storage ---
  TxnOutput output_;

  //--- Constructor ---
  LuigiLogEntry(txnid_t tid = 0) : tid_(tid) {}

  //--- Helper: Is this a multi-shard transaction? ---
  bool IsMultiShard() const { return num_shards_ > 1; }

  //--- Helper: Debug string ---
  std::string DebugString() const {
    return "LuigiEntry[tid=" + std::to_string(tid_) +
           ", deadline=" + std::to_string(local_deadline_) +
           ", keys=" + std::to_string(local_keys_.size()) +
           ", shards=" + std::to_string(num_shards_) + "]";
  }
};

} // namespace janus
