#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../paxos_worker.h"
#include "rrr/rpc/log_storage.hpp"

namespace janus {
class Command;
class CmdData;

struct PaxosData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  bool is_no_op = false;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};
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
  // LOG PERSISTENCE (Phase 1.4)
  // ========================================================================
  std::shared_ptr<rrr::LogStorage> log_storage_;

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
  void SetLogStorage(std::shared_ptr<rrr::LogStorage> storage) { log_storage_ = std::move(storage); }
  // @safe - Read-only access
  std::shared_ptr<rrr::LogStorage> GetLogStorage() const { return log_storage_; }
  // @unsafe - Uses LogStorage which has non-borrow-checked operations
  bool RecoverFromStorage();

  /**
   * Replay committed entries after recovery.
   * Called after app_next_ callback is registered to apply recovered entries.
   * Must be called AFTER RegLearnerAction() sets up the callback.
   */
  // @unsafe - Calls app_next_ which may have side effects
  void ReplayCommittedEntries();

#ifdef CHECK_KEY_DISTRIBUTION
  KeyDistribution key_distribution_;
#endif

#ifdef LATENCY_DEBUG
  Distribution client2follower_;
#endif

  ~PaxosServer() {
    Log_info("site par %d, loc %d: prepare %d, accept %d, commit %d", partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
#ifdef CHECK_KEY_DISTRIBUTION
    if (loc_id_ == 0)
      key_distribution_.Print();
#endif
#ifdef LATENCY_DEBUG
    Log_info("site par %d, loc %d: client2follower 50pct: %.2f 90pct: %.2f 99pct: %.2f", partition_id_, loc_id_, client2follower_.pct50(), client2follower_.pct90(), client2follower_.pct99());
#endif
  }

  shared_ptr<PaxosData> GetInstance(slotid_t id) {
    if(id<min_active_slot_) Log_info("XXXXXX: id: %d, min_active_slot_:%d", id, min_active_slot_);
    verify(id >= min_active_slot_);
    auto& sp_instance = logs_[id];
    if(!sp_instance)
      sp_instance = std::make_shared<PaxosData>();
    return sp_instance;
  }

  void OnForward(shared_ptr<Marshallable> &cmd,
                 uint64_t dep_id,
                 uint64_t* coro_id,
                 rusty::Function<void()> cb);

  void OnPrepare(slotid_t slot_id,
                 ballot_t ballot,
                 ballot_t *max_ballot,
                 uint64_t* coro_id,
                 rusty::Function<void()> cb);

  void OnAccept(const slotid_t slot_id,
		const uint64_t time,
                const ballot_t ballot,
                shared_ptr<Marshallable> &cmd,
                ballot_t *max_ballot,
                uint64_t* coro_id,
                rusty::Function<void()> cb);

  void OnCommit(const slotid_t slot_id,
                const ballot_t ballot,
                shared_ptr<Marshallable> &cmd);

  void OnBulkPrepare(shared_ptr<Marshallable> &cmd,
                    i32 *ballot,
                    i32* valid,
                    rusty::Function<void()> cb);

  void OnHeartbeat(shared_ptr<Marshallable> &cmd,
                    i32 *ballot,
                    i32* valid,
                    rusty::Function<void()> cb);

  void OnBulkAccept(shared_ptr<Marshallable> &cmd,
                    i32* ballot,
                    i32 *valid,
                    rusty::Function<void()> cb);

  void OnBulkCommit(shared_ptr<Marshallable> &cmd,
                    i32* ballot,
                    i32 *valid,
                    rusty::Function<void()> cb);

  void OnBulkPrepare2(shared_ptr<Marshallable> &cmd,
                      i32* ballot,
                      i32 *valid,
                      shared_ptr<BulkPaxosCmd> ret_cmd,
                      rusty::Function<void()> cb);

  void OnSyncLog(shared_ptr<Marshallable> &cmd,
                      i32* ballot,
                      i32 *valid,
                      shared_ptr<SyncLogResponse> ret_cmd,
                      rusty::Function<void()> cb);

  void OnSyncCommit(shared_ptr<Marshallable> &cmd,
                      i32* ballot,
                      i32 *valid,
                      rusty::Function<void()> cb);


  void OnSyncNoOps(shared_ptr<Marshallable> &cmd,
                  i32* ballot,
                  i32 *valid,
                  rusty::Function<void()> cb);
  
  void OnForwardToLearner(const rrr::i32& par_id,
                        const uint64_t& slot, 
                        const ballot_t& ballot,
                        shared_ptr<Marshallable> &cmd,
                        rusty::Function<void()> cb);

  int get_open_slot(){
    return cur_open_slot_++;
  }

  void FreeSlots(){
    // TODO should support snapshot for freeing memory.
    // for now just free anything 1000 slots before.
    int i = min_active_slot_;
    while (i + 100 < max_executed_slot_) {
      //Log_info("Erasing entry number %d", i);
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
