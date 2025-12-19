#pragma once

#include "../__dep__.h"
#include "../constants.h"
#include "../scheduler.h"
#include "../classic/tpc_command.h"
#include "../classic/tx.h"
#include "coordinator.h"
#include <chrono>
#include <ctime>
#include <mutex>
#include <set>
#include <unordered_map>

namespace janus {
class Command;
class CmdData;

struct MenciusData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_suggested_ = 0;
  bool is_skip = false;
  shared_ptr<Marshallable> cmd_{nullptr};
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};
  bool executed_ = false;
};

class MenciusServer : public TxLogServer {
 public:
  std::mutex g_mutex{};
  std::mutex c_mutex{}; // conflict mutex
  // ----min_active <= max_executed <= max_committed---
  slotid_t min_active_slot_ = 0; // anything before (lt) this slot is freed
  slotid_t max_executed_slot_ = 0;
  slotid_t max_committed_slot_ = 0;
  slotid_t max_active_slot_ = 0;
  map<slotid_t, shared_ptr<MenciusData>> logs_{};
  unordered_map<uint32_t, set<uint64_t>> skip_potentials_recd{};
  unordered_map<uint32_t, atomic<int>> unexecuted_keys_{};  // [max_executed+1, ...]
  unordered_map<uint32_t, int> executed_slots_{}; // used along with unexecuted_keys_
  int n_prepare_ = 0;
  int n_suggest_ = 0;
  int n_commit_ = 0;
  bool in_applying_logs_{false};

  MenciusServer() {
    witness_.set_belongs_to_leader(true);
  }

  ~MenciusServer() {
    Log_info("site par %d, loc %d: prepare %d, accept %d, commit %d", partition_id_, loc_id_, n_prepare_, n_suggest_, n_commit_);
  }

  bool IsLeader() override {
    return true;
  }

  shared_ptr<MenciusData> GetInstance(slotid_t id) {
    g_mutex.lock();
    verify(id >= min_active_slot_);
    auto& sp_instance = logs_[id];
    // for (auto const& x : logs_) {
    //   Log_info("GetInstance slot_id: %llu on loc_id_:%d [logs]", x.first, this->loc_id_);
    // }

    if(!sp_instance)
      sp_instance = std::make_shared<MenciusData>();
    g_mutex.unlock();
    return sp_instance;
  }

  void OnPrepare(slotid_t slot_id,
                 ballot_t ballot,
                 ballot_t *max_ballot,
                 uint64_t* coro_id,
                 const function<void()> &cb);

  void OnSuggest(const slotid_t slot_id,
		            const uint64_t time,
                const ballot_t ballot,
                const uint64_t sender,
                const std::vector<uint64_t>& skip_commits, 
                const std::vector<uint64_t>& skip_potentials,
                shared_ptr<Marshallable> &cmd,
                ballot_t *max_ballot,
                uint64_t* coro_id,
                const function<void()> &cb);

  void OnCommit(const slotid_t slot_id,
                const ballot_t ballot,
                shared_ptr<Marshallable> &cmd,
                bool is_skip=false);

  void Setup();

  virtual bool HandleConflicts(Tx& dtxn,
                               innid_t inn_id,
                               vector<string>& conflicts) {
    verify(0);
  };

#ifdef ZERO_OVERHEAD
  bool ConflictWithOriginalUnexecutedLog(const shared_ptr<Marshallable>& cmd) override;
#endif
};
} // namespace janus
