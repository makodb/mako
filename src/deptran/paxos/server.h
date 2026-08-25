#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../paxos_worker.h"
#include "deptran/raft/log_storage.hpp"
#include "deptran/raft/snapshot_manager.hpp"

namespace janus {
class CmdData;

// polymorphic command fields
// (`accepted_cmd_` / `committed_cmd_`) migrated from
// `shared_ptr<Marshallable>` to `janus::Command`.  See
// `docs/dev/l10-unblock-plan.md`.
struct PaxosData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  bool is_no_op = false;
  Command accepted_cmd_{};
  Command committed_cmd_{};
};

struct BulkPrepare{
  ballot_t seen_ballot;
  int leader_id;
};

class PaxosServer : public TxLogServer {
 public:
  // ----min_active <= max_executed <= max_committed---
  slotid_t min_active_slot_ = 0; // anything before (lt) this slot is freed
  slotid_t max_executed_slot_ = 0;
  slotid_t max_committed_slot_ = 0;
  slotid_t cur_min_prepared_slot_ = 0;
  slotid_t max_accepted_slot_ = 0;
  slotid_t max_possible_slot_ = INT_MAX;
  slotid_t cur_open_slot_ = 1;
  slotid_t max_touched_slot = 0;
  int leader_id;
  map<pair<slotid_t, slotid_t>, BulkPrepare> bulk_prepares{};  // saves all the prepare ranges.
  map<slotid_t, shared_ptr<PaxosData>> logs_{}; // the committed values
  ballot_t cur_epoch;
  // for learner for the later on takeover
  slotid_t max_committed_slot_learner_ = 0;

  int n_prepare_ = 0;
  int n_accept_ = 0;
  int n_commit_ = 0;
  bool in_applying_logs_{false};

  // ========================================================================
  // LOG PERSISTENCE
  // ========================================================================
  std::shared_ptr<janus::raft::LogStorage> log_storage_;

  // Metadata keys for persistence
  static constexpr const char* META_EPOCH = "cur_epoch";
  static constexpr const char* META_MAX_COMMITTED = "max_committed_slot";
  static constexpr const char* META_MAX_EXECUTED = "max_executed_slot";

 private:
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  void PersistEpoch();
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  void PersistMaxCommitted();
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  void PersistLogEntry(slotid_t slot_id, const PaxosData& data);
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  void PersistLogEntries(const std::vector<std::pair<slotid_t, std::shared_ptr<PaxosData>>>& entries);

 public:
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  void SetLogStorage(std::shared_ptr<janus::raft::LogStorage> storage) { log_storage_ = std::move(storage); }
  // @unsafe - Read-only access
  std::shared_ptr<janus::raft::LogStorage> GetLogStorage() const { return log_storage_; }
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  bool RecoverFromStorage();

  // ========================================================================
  // SNAPSHOT SUPPORT
  // ========================================================================
  std::shared_ptr<janus::raft::SnapshotManager> snapshot_manager_;

  // @unsafe - Moves ownership of snapshot manager
  void SetSnapshotManager(std::shared_ptr<janus::raft::SnapshotManager> manager) {
    snapshot_manager_ = std::move(manager);
  }
  // @unsafe - Read-only access
  std::shared_ptr<janus::raft::SnapshotManager> GetSnapshotManager() const {
    return snapshot_manager_;
  }

  /**
   * Replay committed entries after recovery.
   * Called after app_next_ callback is registered to apply recovered entries.
   * Must be called AFTER RegLearnerAction() sets up the callback.
   */
  // @unsafe - Calls app_next_ which may have side effects
  void ReplayCommittedEntries();

  /**
   * Get count of uncommitted entries after recovery.
   * These entries will be resolved by the consensus protocol.
   * @return Number of uncommitted entries (max_accepted_slot_ - max_committed_slot_)
   */
  // @safe - Read-only accessor
  size_t GetUncommittedCount() const;

  /**
   * Compact log entries up to the given index.
   * Removes entries from storage that are covered by a snapshot.
   * @param up_to_index Remove entries with index <= this value
   * @return Number of entries removed
   */
  // @unsafe - Modifies log storage
  size_t CompactLog(slotid_t up_to_index);

#ifdef CHECK_KEY_DISTRIBUTION
  KeyDistribution key_distribution_;
#endif

#ifdef LATENCY_DEBUG
  Distribution client2follower_;
#endif

  ~PaxosServer() {
    Log_info("site par {}, loc {}: prepare {}, accept {}, commit {}", partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
#ifdef CHECK_KEY_DISTRIBUTION
    if (loc_id_ == 0)
      key_distribution_.Print();
#endif
#ifdef LATENCY_DEBUG
    Log_info("site par {}, loc {}: client2follower 50pct: {:.2f} 90pct: {:.2f} 99pct: {:.2f}", partition_id_, loc_id_, client2follower_.pct50(), client2follower_.pct90(), client2follower_.pct99());
#endif
  }

  shared_ptr<PaxosData> GetInstance(slotid_t id) {
    if(id<min_active_slot_) Log_info("XXXXXX: id: {}, min_active_slot_:{}", id, min_active_slot_);
    verify(id >= min_active_slot_);
    auto& sp_instance = logs_[id];
    if(!sp_instance)
      sp_instance = std::make_shared<PaxosData>();
    return sp_instance;
  }

  // removed `OnForward` declaration —
  // body was `verify(0); // Should never be called in Mako`; the
  // `MultiPaxosServiceImpl::Forward(janus::Command, ...)` handler
  // has an empty body that never reaches this method (Mako uses
  // `OnForwardToLearner` instead via the `ForwardToLearnerServer`
  // RPC).

  void OnPrepare(slotid_t slot_id,
                 ballot_t ballot,
                 ballot_t *max_ballot,
                 uint64_t* coro_id,
                 rusty::Function<void()> cb);

  // handler parameters take
  // const janus::Command&; shared_ptr<Marshallable> callers
  // auto-convert via Command's implicit ctor.
  void OnAccept(const slotid_t slot_id,
		const uint64_t time,
                const ballot_t ballot,
                const janus::Command& cmd,
                ballot_t *max_ballot,
                uint64_t* coro_id,
                rusty::Function<void()> cb);

  void OnCommit(const slotid_t slot_id,
                const ballot_t ballot,
                const janus::Command& cmd);

  // removed `OnBulkPrepare`, `OnHeartbeat`
  // declarations — only callers were the now-deleted
  // `MultiPaxosServiceImpl::BulkPrepare` / `Heartbeat` handlers.

  void OnBulkAccept(const janus::Command& cmd,
                    i32* ballot,
                    i32 *valid,
                    rusty::Function<void()> cb);

  void OnBulkCommit(const janus::Command& cmd,
                    i32* ballot,
                    i32 *valid,
                    rusty::Function<void()> cb);

  // removed `OnBulkPrepare2` declaration —
  // only caller was the now-deleted
  // `MultiPaxosServiceImpl::BulkPrepare2` handler.

  // Fill-then-wrap: fills the caller-owned response; the caller packs
  // it after this returns. (The old Function<void()> cb param was dead
  // ceremony — never invoked; it only carried the DeferredReply to its
  // destructor.)
  void OnSyncLog(const janus::Command& cmd,
                      i32* ballot,
                      i32 *valid,
                      SyncLogResponse& ret_cmd);

  void OnSyncCommit(const janus::Command& cmd,
                      i32* ballot,
                      i32 *valid,
                      rusty::Function<void()> cb);


  // removed `OnSyncNoOps` declaration — only
  // caller was the now-deleted `MultiPaxosServiceImpl::SyncNoOps`
  // handler.

  void OnForwardToLearner(const srpc::i32& par_id,
                        const uint64_t& slot,
                        const ballot_t& ballot,
                        const janus::Command& cmd,
                        rusty::Function<void()> cb);

  int get_open_slot(){
    return cur_open_slot_++;
  }

  void FreeSlots(){
    // TODO should support snapshot for freeing memory.
    // for now just free anything 1000 slots before.
    int i = min_active_slot_;
    while (i + 100 < max_executed_slot_) {
      //Log_info("Erasing entry number {}", i);
      logs_.erase(i);
      i++;
    }
    min_active_slot_ = i;
  }

  // should be called from locked state.
  void clear_accepted_entries(){
    for(int i = max_committed_slot_; i <= max_accepted_slot_; i++){
      logs_.erase(i);
    }
  }

  virtual bool HandleConflicts(Tx& dtxn,
                               innid_t inn_id,
                               vector<string>& conflicts) {
    verify(0);
  };
};
} // namespace janus
