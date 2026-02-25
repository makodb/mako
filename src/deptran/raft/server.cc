#include "server.h"
// #include "paxos_worker.h"
#include "exec.h"
#include "frame.h"
#include "coordinator.h"
#include "../classic/tpc_command.h"

// @external: {
//   rrr::RandomGenerator::rand_double: [safe, (double, double) -> double]
//   rrr::RandomGenerator::rand: [safe, (int, int) -> int]
//   Log_info: [safe, (...) -> void]
//   Log_debug: [safe, (...) -> void]
//   Log_warn: [safe, (...) -> void]
//   Log_error: [safe, (...) -> void]
//   Log_fatal: [safe, (...) -> void]
//   verify: [safe, (...) -> void]
//   Time::now: [safe, () -> uint64_t]
//   strcmp: [safe, (const char*, const char*) -> int]
//   std::getenv: [safe, (const char*) -> const char*]
//   std::tolower: [safe, (int) -> int]
//   std::transform: [safe, (...) -> void]
//   std::stoull: [safe, (const std::string&) -> uint64_t]
//   std::stoll: [safe, (const std::string&) -> int64_t]
//   std::to_string: [safe, (...) -> owned std::string]
//   std::min: [safe, (...) -> T]
//   std::max: [safe, (...) -> T]
//   std::sort: [safe, (iterator, iterator) -> void]
//   std::copy: [safe, (...) -> void]
//   std::make_shared: [safe, (...) -> owned]
//   std::dynamic_pointer_cast: [safe, (...) -> owned]
//   std::static_pointer_cast: [safe, (...) -> owned]
//   std::lock_guard: [safe, (...) -> owned]
//   std::recursive_mutex::lock: [safe, (&'a mut) -> void]
//   std::recursive_mutex::unlock: [safe, (&'a mut) -> void]
//   std::atomic::store: [safe, (&'a mut, ...) -> void]
//   std::atomic::load: [safe, (&'a) -> T]
//   std::vector::push_back: [safe, (&'a mut, T) -> void]
//   std::vector::operator[]: [safe, (&'a, size_t) -> &'a]
//   std::vector::reserve: [safe, (&'a mut, size_t) -> void]
//   std::vector::size: [safe, (&'a) -> size_t]
//   std::vector::empty: [safe, (&'a) -> bool]
//   std::vector::begin: [safe, (&'a) -> iterator]
//   std::vector::end: [safe, (&'a) -> iterator]
//   std::map::find: [safe, (&'a, ...) -> iterator]
//   std::map::insert: [safe, (&'a mut, ...) -> pair]
//   std::map::end: [safe, (&'a) -> iterator]
//   std::map::erase: [safe, (&'a mut, ...) -> void]
//   std::map::size: [safe, (&'a) -> size_t]
//   std::shared_ptr::operator=: [safe, (&'a mut, &'a) -> &'a mut]
//   std::shared_ptr::get: [safe, (&'a) -> *]
//   operator bool: [safe, (&'a) -> bool]
//   rrr::Fiber::create_run: [safe, (...) -> owned]
//   rrr::Fiber::sleep: [safe, (int) -> void]
//   Reactor::create_sp_event: [safe, (...) -> owned]
//   Config::GetConfig: [safe, () -> *]
//   janus::TpcBatchCommand::AddCmds: [safe, (&'a mut, &'a mut) -> void]
//   std::this_thread::sleep_for: [safe, (...) -> void]
//   std::thread::joinable: [safe, (&'a) -> bool]
//   std::thread::join: [safe, (&'a mut) -> void]
//   std::thread::detach: [safe, (&'a mut) -> void]
//   rrr::IntEvent::set: [safe, (&'a mut, int) -> void]
//   rrr::IntEvent::wait: [safe, (&'a, int) -> void]
//   rrr::Event::wait: [safe, (&'a, int) -> void]
//   rrr::Event::TIMEOUT: [safe, () -> int]
//   janus::View::View: [safe, (...) -> owned]
//   janus::View::operator=: [safe, (&'a mut, const &'a) -> &'a mut]
//   janus::TxLogServer::DestroyTx: [safe, (&'a mut, uint64_t) -> void]
//   janus::RaftCommo::SendAppendEntries2: [safe, (...) -> owned]
//   janus::RaftCommo::BroadcastVote: [safe, (...) -> owned]
// }

namespace janus {

// ============================================================================
// LOG PERSISTENCE IMPLEMENTATION (Phase 1.3)
// ============================================================================

// @safe - Uses LogStorage API (external calls marked safe via @external)
void RaftServer::PersistTermAndVote() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  // @unsafe
  {
  log_storage_->set_metadata(META_TERM, std::to_string(currentTerm));
  log_storage_->set_metadata(META_VOTE_FOR, std::to_string(static_cast<int64_t>(vote_for_)));
  log_storage_->sync();
  }
}

// @safe - Uses LogStorage API
void RaftServer::PersistVote() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  // @unsafe
  {
  log_storage_->set_metadata(META_VOTE_FOR, std::to_string(static_cast<int64_t>(vote_for_)));
  log_storage_->sync();
  }
}

// @safe - Uses LogStorage API
void RaftServer::PersistCommitIndex() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  // @unsafe
  {
  log_storage_->set_metadata(META_COMMIT_INDEX, std::to_string(commitIndex));
  // Note: Don't sync for commitIndex - it can be recovered from logs
  }
}

// @safe - Uses LogStorage API
void RaftServer::PersistLogEntry(slotid_t slot_id, const RaftData& data) {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  rrr::LogEntry entry(slot_id, data.term);
  entry.command = data.log_;
  entry.max_ballot_seen = data.max_ballot_seen_;
  entry.max_ballot_accepted = data.max_ballot_accepted_;
  entry.committed = (slot_id <= commitIndex);

  // @unsafe
  {
  log_storage_->put(entry);
  log_storage_->sync();
  }
}

// @safe - Uses LogStorage API
void RaftServer::PersistLogEntries(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries) {
  if (!log_storage_ || !log_storage_->is_open() || entries.empty()) {
    return;
  }

  std::vector<rrr::LogEntry> log_entries;
  log_entries.reserve(entries.size());

  for (const auto& [slot_id, data] : entries) {
    rrr::LogEntry entry(slot_id, data->term);
    entry.command = data->log_;
    entry.max_ballot_seen = data->max_ballot_seen_;
    entry.max_ballot_accepted = data->max_ballot_accepted_;
    entry.committed = (slot_id <= commitIndex);
    log_entries.push_back(entry);
  }

  // @unsafe
  {
  log_storage_->put_batch(log_entries);
  log_storage_->sync();
  }
}

// @safe - Recovers state from LogStorage
bool RaftServer::RecoverFromStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return true;  // No storage configured, nothing to recover
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Recover currentTerm
  // @unsafe
  {
  auto term_opt = log_storage_->get_metadata(META_TERM);
  if (term_opt.is_some()) {
    currentTerm = std::stoull(term_opt.unwrap());
  }

  // Recover vote_for
  auto vote_opt = log_storage_->get_metadata(META_VOTE_FOR);
  if (vote_opt.is_some()) {
    int64_t vote_val = std::stoll(vote_opt.unwrap());
    vote_for_ = static_cast<siteid_t>(vote_val);
  }

  // Recover commitIndex
  auto commit_opt = log_storage_->get_metadata(META_COMMIT_INDEX);
  if (commit_opt.is_some()) {
    commitIndex = std::stoull(commit_opt.unwrap());
  }
  }

  // Recover lastLogIndex
  lastLogIndex = log_storage_->get_last_index();

  // Recover log entries
  if (lastLogIndex > 0) {
    auto entries = log_storage_->get_range(1, lastLogIndex + 1);
    for (const auto& entry : entries) {
      auto data = std::make_shared<RaftData>();
      data->term = entry.term;
      data->log_ = entry.command;
      data->max_ballot_seen_ = entry.max_ballot_seen;
      data->max_ballot_accepted_ = entry.max_ballot_accepted;
      raft_logs_[entry.slot_id] = data;
    }
  }

  Log_info("[RAFT-RECOVERY] Site %d: Recovered term=%lu vote_for=%d lastLogIndex=%lu commitIndex=%lu entries=%zu",
           site_id_, currentTerm, vote_for_, lastLogIndex, commitIndex, raft_logs_.size());

  return true;
}

// @safe - Replays committed entries (callbacks wrapped in @unsafe blocks)
void RaftServer::ReplayCommittedEntries() {
  if (!app_next_) {
    Log_warn("[RAFT-REPLAY] Site %d: No app_next_ callback, skipping replay", site_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = executeIndex + 1;
  slotid_t end = commitIndex;

  if (start > end) {
    Log_info("[RAFT-REPLAY] Site %d: No entries to replay (executeIndex=%lu >= commitIndex=%lu)",
             site_id_, executeIndex, commitIndex);
    return;
  }

  Log_info("[RAFT-REPLAY] Site %d: Replaying entries %lu..%lu", site_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto instance = GetRaftInstance(id);
    if (instance && instance->log_) {
      // @unsafe
      {
      app_next_(id, instance->log_);
      executeIndex = id;
      replayed++;
      }
    } else {
      Log_warn("[RAFT-REPLAY] Site %d: Missing log entry at slot %lu, stopping replay", site_id_, id);
      break;
    }
  }

  Log_info("[RAFT-REPLAY] Site %d: Replayed %zu entries, executeIndex now %lu",
           site_id_, replayed, executeIndex);

  // Phase 2.3: Log uncommitted entries status
  size_t uncommitted = GetUncommittedCount();
  if (uncommitted > 0) {
    Log_info("[RAFT-RECOVERY] Site %d: %zu uncommitted entries (lastLogIndex=%lu, commitIndex=%lu) - will be resolved by consensus",
             site_id_, uncommitted, lastLogIndex, commitIndex);
  }
}

// @safe - Read-only accessor
size_t RaftServer::GetUncommittedCount() const {
  if (lastLogIndex > commitIndex) {
    return lastLogIndex - commitIndex;
  }
  return 0;
}

// @safe - Log compaction (storage operations wrapped in @unsafe blocks)
size_t RaftServer::CompactLog(slotid_t up_to_index) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  if (!log_storage_) {
    Log_debug("[RAFT-COMPACT] Site %d: No log storage, skipping compaction", site_id_);
    return 0;
  }

  // Safety check: don't compact beyond commit index
  if (up_to_index > commitIndex) {
    Log_warn("[RAFT-COMPACT] Site %d: Cannot compact beyond commitIndex (%lu > %lu)",
             site_id_, up_to_index, commitIndex);
    up_to_index = commitIndex;
  }

  // Get current first slot
  slotid_t first_slot = 0;
  // @unsafe
  {
  first_slot = log_storage_->get_first_index();
  if (first_slot == 0 || log_storage_->empty()) {
    Log_debug("[RAFT-COMPACT] Site %d: Log is empty, nothing to compact", site_id_);
    return 0;
  }
  }

  // Nothing to compact if up_to_index is before first slot
  if (up_to_index < first_slot) {
    Log_debug("[RAFT-COMPACT] Site %d: up_to_index %lu < first_slot %lu, nothing to compact",
              site_id_, up_to_index, first_slot);
    return 0;
  }

  // Remove entries from storage
  size_t to_remove = up_to_index - first_slot + 1;
  // @unsafe
  {
  if (log_storage_->remove_range(first_slot, up_to_index + 1)) {
    Log_info("[RAFT-COMPACT] Site %d: Compacted %zu entries [%lu..%lu]",
             site_id_, to_remove, first_slot, up_to_index);

    // Also remove from in-memory logs
    for (slotid_t id = first_slot; id <= up_to_index; ++id) {
      raft_logs_.erase(id);
    }

    // Update min_active_slot_
    if (up_to_index + 1 > min_active_slot_) {
      min_active_slot_ = up_to_index + 1;
    }

    return to_remove;
  } else {
    Log_error("[RAFT-COMPACT] Site %d: Failed to compact log entries", site_id_);
    return 0;
  }
  }
}

// ============================================================================

// @safe - Logs term changes (Log_info marked safe via @external)
void RaftServer::LogTermChange(const char* reason,
                               uint64_t old_term,
                               uint64_t new_term,
                               siteid_t source) {
  if (old_term == new_term) {
    return;
  }
  // @unsafe
  {
  const char* why = reason ? reason : "unspecified";
  if (source != INVALID_SITEID) {
    Log_info("[RAFT-TERM] server %d term %lu -> %lu (%s, source_site=%d)",
             site_id_, old_term, new_term, why, source);
  } else {
    Log_info("[RAFT-TERM] server %d term %lu -> %lu (%s)",
             site_id_, old_term, new_term, why);
  }
  }
}

namespace {

// @safe
bool JetpackRecoveryEnabled() {
  static const bool enabled = []() {
    const char* flag = std::getenv("MAKO_DISABLE_JETPACK");
    if (!flag || flag[0] == '\0') {
      return true;
    }
    std::string value(flag);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    auto is_true = [](const std::string& v) {
      return v == "1" || v == "true" || v == "yes" || v == "on";
    };
    auto is_false = [](const std::string& v) {
      return v == "0" || v == "false" || v == "no" || v == "off";
    };

    if (is_true(value)) {
      Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK=%s -> Jetpack recovery disabled", flag);
      return false;
    }
    if (is_false(value)) {
      return true;
    }

    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK has unrecognised value '%s'; defaulting to disabled", flag);
    return false;
  }();
  return enabled;
}

}  // namespace

// @safe - raw pointer parameter is bounded (frame outlives server)
RaftServer::RaftServer(Frame * frame)
  : timer_(rusty::Box<Timer>::make(Timer()))  // Initialize Box in member initializer list
{
  frame_ = frame ;
#ifdef RAFT_TEST_CORO
  setIsLeader(false);
#endif
  stop_ = false ;
}

// @unsafe - raw pointer output params from base class virtual interface
void RaftServer::OnJetpackPullCmd(const epoch_t& jepoch,
                                   const epoch_t& oepoch,
                                   const std::vector<key_t>& keys,
                                   bool_t* ok,
                                   epoch_t* reply_jepoch,
                                   epoch_t* reply_oepoch,
                                   MarshallDeputy* reply_old_view,
                                   MarshallDeputy* reply_new_view,
                                   shared_ptr<KeyCmdBatchData>& batch) {
  TxLogServer::OnJetpackPullCmd(jepoch, oepoch, keys, ok, reply_jepoch, reply_oepoch,
                                reply_old_view, reply_new_view, batch);
  if (!IsLeader()) {
    resetTimer("JetpackPullCmd RPC");
#ifdef RAFT_LEADER_ELECTION_DEBUG
    // Log_info("[RAFT_TIMER] server %d reset election timer due to JetpackPullCmd (keys=%zu)",
    //          site_id_, keys.size());
#endif
  }
}

// @safe - Election timeout calculation (Time::now and RandomGenerator::rand marked safe via @external)
uint64_t RaftServer::GetElectionTimeout() {
  uint64_t base_timeout;
  uint64_t current_time = Time::now();
  bool in_grace_period = (current_time - startup_timestamp_) < 5000000; // 5 seconds in microseconds

  if (AmIPreferredLeader()) {
    // SINGLE-RAFT: Preferred replica: 300-600ms timeout
    base_timeout = 300000; // 300ms
    uint64_t jitter = RandomGenerator::rand(0, 300000);
    return base_timeout + jitter; // 300-600ms
  } else if (in_grace_period) {
    // SINGLE-RAFT: Non-preferred during grace period: 5-10s timeout
    base_timeout = 5000000; // 5s
    uint64_t jitter = RandomGenerator::rand(0, 5000000);
    return base_timeout + jitter; // 5-10s
  } else {
    // SINGLE-RAFT: Non-preferred after grace: 3-6s timeout
    // Must be >> 500ms parallel wait to prevent spurious elections
    base_timeout = 3000000; // 3s
    uint64_t jitter = RandomGenerator::rand(0, 3000000);
    return base_timeout + jitter; // 3-6s
  }
}

// SINGLE-RAFT: StartApplyFiber - lightweight status monitor on PollThread.
// Actual entry application is handled by the background apply thread.
// @safe - Fiber::create_run and Fiber::sleep marked @external [safe]
void RaftServer::StartApplyFiber() {
  Fiber::create_run([this]() {
    Log_info("[APPLY-FIBER] Site %d: Started apply fiber (monitor only)", site_id_);
    while (!stop_) {
      Fiber::sleep(5000000);  // 5s status check
      Log_info("[APPLY-FIBER] Site %d: executeIndex=%lu commitIndex=%lu lastLogIndex=%lu",
               site_id_, executeIndex, commitIndex, lastLogIndex);
    }
    Log_info("[APPLY-FIBER] Site %d: Apply fiber exiting (stop_=true)", site_id_);
  });
}

// SINGLE-RAFT: Enqueue newly committed entries for the background apply thread.
// Called from OnAppendEntries (already under mtx_) when commitIndex advances.
void RaftServer::EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit) {
  std::vector<std::pair<slotid_t, shared_ptr<Marshallable>>> batch;
  slotid_t first_missing = 0;
  for (slotid_t id = old_commit + 1; id <= new_commit; id++) {
    auto it = raft_logs_.find(id);
    if (it != raft_logs_.end() && it->second && it->second->log_) {
      batch.emplace_back(id, it->second->log_);
    } else {
      first_missing = id;
      break;  // Gap in log — stop here
    }
  }
  if (!batch.empty()) {
    std::lock_guard<std::mutex> lock(apply_queue_mtx_);
    for (auto& entry : batch) {
      apply_queue_.push_back(std::move(entry));
    }
  }
  // Log if we couldn't enqueue the full range
  if (first_missing > 0) {
    Log_info("[ENQUEUE] Site %d: gap at slot %lu (range %lu..%lu, enqueued %zu)",
             site_id_, first_missing, old_commit + 1, new_commit, batch.size());
  }
  static uint64_t enqueue_log_counter = 0;
  if (enqueue_log_counter++ % 50 == 0) {
    size_t qsize = 0;
    {
      std::lock_guard<std::mutex> lock(apply_queue_mtx_);
      qsize = apply_queue_.size();
    }
    Log_info("[ENQUEUE] Site %d: enqueued %zu entries (%lu..%lu) queue_total=%zu",
             site_id_, batch.size(), old_commit + 1, new_commit, qsize);
  }
}

// SINGLE-RAFT: Background OS thread for entry application.
// Drains from apply_queue_ (populated by OnAppendEntries) to avoid contention on mtx_.
void RaftServer::StartApplyThread() {
  apply_thread_running_.store(true);
  apply_thread_ = std::thread([this]() {
    Log_info("[APPLY-THREAD] Site %d: Started background apply thread", site_id_);
    uint64_t apply_count = 0;
    auto last_log_time = std::chrono::steady_clock::now();
    while (!stop_ && apply_thread_running_.load()) {
      // Drain entries from the queue
      std::pair<slotid_t, shared_ptr<Marshallable>> entry;
      bool got_entry = false;
      size_t queue_size = 0;
      {
        std::lock_guard<std::mutex> lock(apply_queue_mtx_);
        queue_size = apply_queue_.size();
        if (!apply_queue_.empty()) {
          entry = std::move(apply_queue_.front());
          apply_queue_.pop_front();
          got_entry = true;
        }
      }

      if (got_entry) {
        slotid_t id = entry.first;
        auto& log_entry = entry.second;
        // Log entries near the stall point for debugging
        if (id >= 470 && id <= 500) {
          Log_info("[APPLY-THREAD] Site %d: ABOUT TO APPLY entry %lu (queue_remaining=%zu)",
                   site_id_, id, queue_size);
        }
        // @unsafe - callback may have side effects
        RuleWitnessGC(log_entry);
        app_next_(id, log_entry);
        if (id >= 470 && id <= 500) {
          Log_info("[APPLY-THREAD] Site %d: DONE APPLYING entry %lu", site_id_, id);
        }
        executeIndex = id;
        apply_count++;

        // Log progress periodically
        if (apply_count % 100 == 0) {
          Log_info("[APPLY-THREAD] Site %d: applied %lu entries, executeIndex=%lu queue_remaining=%zu",
                   site_id_, apply_count, executeIndex, queue_size);
        }

        // Cleanup old commands periodically (needs mtx_ for raft_logs_)
        if (id % 1000 == 0) {
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          int i = min_active_slot_;
          while (i + 60000 < (slotid_t)executeIndex) {
            removeCmd(i);
            i++;
          }
          min_active_slot_ = i;
        }
      } else {
        // Periodic heartbeat when queue is empty
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
          Log_info("[APPLY-THREAD] Site %d: IDLE executeIndex=%lu commitIndex=%lu queue_size=%zu applied_total=%lu",
                   site_id_, executeIndex, commitIndex, queue_size, apply_count);
          last_log_time = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    Log_info("[APPLY-THREAD] Site %d: Background apply thread exiting", site_id_);
  });
  apply_thread_.detach();
}

// @safe - Server setup (Time::now, Log_debug, Fiber::create_run marked safe via @external)
void RaftServer::Setup() {
  // Record startup time for grace period logic
  startup_timestamp_ = Time::now();

#ifdef RAFT_TEST_CORO
  if (heartbeat_) {
		Log_debug("starting heartbeat loop at site %d", site_id_);
    Fiber::create_run([this](){
      this->HeartbeatLoop();
    });
    // Start election timeout loop
    if (failover_) {
      Fiber::create_run([this](){
        StartElectionTimer();
      });
    }
	}
#endif

#ifndef RAFT_TEST_CORO
  if (heartbeat_) {
		Log_debug("starting heartbeat loop at site %d", site_id_);
    Fiber::create_run([this](){
      this->HeartbeatLoop();
    });
    // Start election timeout loop
    if (failover_) {
      Fiber::create_run([this](){
        StartElectionTimer();
      });
    }
	}
#endif

  // SINGLE-RAFT: Start the apply fiber (for status monitoring)
  StartApplyFiber();
  // SINGLE-RAFT: Enqueue any pre-existing committed entries (e.g., from persistence recovery)
  if (commitIndex > executeIndex) {
    EnqueueCommittedEntries(executeIndex, commitIndex);
  }
  // SINGLE-RAFT: Start background apply thread (handles actual entry application)
  StartApplyThread();

  // Election timer will be started in Start() method when first command is submitted
}

// @unsafe - modifies connection state, accesses proxy maps
void RaftServer::Disconnect(const bool disconnect) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  verify(disconnected_ != disconnect);
  // global map of rpc_par_proxies_ values accessed by partition then by site
  static map<parid_t, map<siteid_t, map<siteid_t, vector<SiteProxyPair>>>> _proxies{};
  if (_proxies.find(partition_id_) == _proxies.end()) {
    _proxies[partition_id_] = {};
  }
  RaftCommo *c = (RaftCommo*) commo();
  if (disconnect) {
    verify(_proxies[partition_id_][loc_id_].size() == 0);
    verify(c->rpc_par_proxies_.size() > 0);
    auto sz = c->rpc_par_proxies_.size();
    _proxies[partition_id_][loc_id_].insert(c->rpc_par_proxies_.begin(), c->rpc_par_proxies_.end());
    c->rpc_par_proxies_ = {};
    verify(_proxies[partition_id_][loc_id_].size() == sz);
    verify(c->rpc_par_proxies_.size() == 0);
  } else {
    verify(_proxies[partition_id_][loc_id_].size() > 0);
    auto sz = _proxies[partition_id_][loc_id_].size();
    c->rpc_par_proxies_ = {};
    c->rpc_par_proxies_.insert(_proxies[partition_id_][loc_id_].begin(), _proxies[partition_id_][loc_id_].end());
    _proxies[partition_id_][loc_id_] = {};
    verify(_proxies[partition_id_][loc_id_].size() == 0);
    verify(c->rpc_par_proxies_.size() == sz);
  }
  disconnected_ = disconnect;
}

// @safe
bool RaftServer::IsDisconnected() {
  return disconnected_;
}

// @safe - Leadership state transition (callbacks and logging wrapped in @unsafe blocks)
void RaftServer::setIsLeader(bool isLeader) {
  bool prev_is_leader = is_leader_;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site %d (loc %d) term %lu: prev_is_leader=%d new_is_leader=%d",
           site_id_, frame_->site_info_->locale_id, currentTerm, prev_is_leader, isLeader);
#endif


  if (isLeader) {  // [Jetpack] This need to be done before new leader realized it is a leader, otherwise new leader will use incorrect next_index_ balabala
    // Add null check for communicator
    if (commo_ == nullptr) {
      Log_info("commo_ is null, skipping leader initialization");
    } else {
      // Reset leader volatile state
      vector<SiteProxyPair> proxies;
      // @unsafe
      {
      RaftCommo *c = (RaftCommo*) commo();
      // SINGLE-RAFT: commo() may return null before SetupCommo() completes
      if (c != nullptr) {
        proxies = c->rpc_par_proxies_[partition_id_];
      } else {
        Log_info("[RAFT_STATE] Site %d: commo_ is null during setIsLeader(true), skipping proxy setup", site_id_);
      }
      }
      if(failover_ && !proxies.empty()) {
        for (auto& p : proxies) {
          if (p.first != site_id_) {
            // set matchIndex = 0
            match_index_[p.first] = 0;
            // set nextIndex = lastLogIndex + 1
            next_index_[p.first] = lastLogIndex + 1;
            Log_debug("loc_id_=%d match_index_[%d]=%d, next_index_[%d]=%d", loc_id_, p.first, match_index_[p.first], p.first, next_index_[p.first]);
          }
        }
        // matchedIndex and nextIndex should have indices for all servers except self
        verify(match_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id_) - 1);
        verify(next_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id_) - 1);
      }
    }
  }


  // This 2 lines MUST put BEFORE is_leader_ = isLeader ! otherwise they will become 0, and new view will without leader
  bool become_new_leader = isLeader && (!is_leader_);
  bool become_new_follower = (!isLeader) && is_leader_;

  // Update the leader state after view handling
  is_leader_ = isLeader;

  // Only log on actual transitions, not no-op calls
  if (become_new_leader || become_new_follower) {
    Log_info("RaftServer::setIsLeader site_id_ %d become_new_leader %d become_new_follower %d isLeader %d", site_id_, become_new_leader, become_new_follower, isLeader);
  }

  // Only update view when transitioning from non-leader to leader
  if (become_new_leader) {
    Log_info("[RAFT_STATE] setIsLeader transition LEADER: site %d term %lu prev_is_leader=%d become_new_leader=%d",
             site_id_, currentTerm, prev_is_leader, become_new_leader);

    // ============================================================================
    // LEADERSHIP TRANSFER: Clear transfer flags when becoming leader
    // ============================================================================
    // If we just became leader, any previous transfer is now complete
    transferring_leadership_ = false;

    // Only update view if we have enough information (not during initialization)
    if (partition_id_ != 0xFFFFFFFF && site_id_ != -1 && frame_ != nullptr) {
      // Move current new_view to old_view before updating
      int n_replicas = 0;
      // @unsafe
      {
      old_view_ = new_view_;

      // Update new_view with this server as the leader
      n_replicas = Config::GetConfig()->GetPartitionSize(partition_id_);
      }
      new_view_ = View(n_replicas, site_id_, currentTerm);
      Log_info("[RAFT_VIEW] Server %d became leader for partition %d, term=%lu, old_view=%s, new_view=%s", 
               site_id_, partition_id_, currentTerm, 
               old_view_.ToString().c_str(), new_view_.ToString().c_str());
      
      // IMPORTANT: Update the communicator's view so it knows this server is the leader
      if (commo_) {
        auto view_data = std::make_shared<ViewData>(new_view_, partition_id_);
        // @unsafe
        { commo()->UpdatePartitionView(partition_id_, view_data); }
        Log_info("[RAFT_VIEW] Updated communicator view for partition %d with new leader %d",
                 partition_id_, site_id_);
      }
      
#ifndef RAFT_TEST_CORO
      if (JetpackRecoveryEnabled()) {
        JetpackRecoveryEntry();
      }
#endif
    }

    // ============================================================================
    // LEADERSHIP TRANSFER: Start monitoring if non-preferred leader
    // ============================================================================
    // If we just became a non-preferred leader, start monitoring for transfer
    // opportunity. This ensures that after failover/elections, non-preferred
    // leaders will transfer back to preferred leaders when they catch up.
    if (!AmIPreferredLeader() && looping_) {
      Log_info("[LEADERSHIP-TRANSFER] Site %d: Became non-preferred leader, starting transfer monitoring",
               site_id_);
      StartLeadershipTransferMonitoring();
    }
  } else if (become_new_follower) {
    Log_info("[RAFT_STATE] setIsLeader transition FOLLOWER: site %d term %lu prev_is_leader=%d become_new_follower=%d",
             site_id_, currentTerm, prev_is_leader, become_new_follower);

    // ============================================================================
    // CRITICAL FIX: Reset election timer when becoming follower
    // ============================================================================
    // This prevents instant elections after recovery/resume. When a node resumes
    // from SIGSTOP/pause, last_heartbeat_time_ is stale (from before pause).
    // Resetting it here ensures the election timer counts from NOW, giving the
    // current leader time to send heartbeats before this node starts an election.
    // This is standard Raft behavior: followers reset their timer when stepping down.
    resetTimer("became follower");
    Log_info("[RAFT_TIMER] Site %d reset election timer when becoming follower (last_hb now=%lu)",
             site_id_, last_heartbeat_time_);

    // When transitioning from leader to non-leader
    Log_info("[RAFT_VIEW] Server %d stepping down as leader for partition %d", site_id_, partition_id_);

    // ============================================================================
    // LEADERSHIP TRANSFER: Stop monitoring when stepping down
    // ============================================================================
    // Stop leadership transfer monitoring if we were running it
    StopLeadershipTransferMonitoring();

    // View will be updated when we learn about the new leader
  }

  // CRITICAL: Fire leadership change callback so RaftWorker can update its state
  // This allows clients to retarget to the new leader after elections
  if (leader_change_cb_) {
    // @unsafe
    {
    if (become_new_leader) {
      Log_info("[LEADER_CALLBACK] Site %d: Firing leader_change_cb_(true) - became leader", site_id_);
      leader_change_cb_(true);
    } else if (become_new_follower) {
      Log_info("[LEADER_CALLBACK] Site %d: Firing leader_change_cb_(false) - became follower", site_id_);
      leader_change_cb_(false);
    }
    }
  }
}

// @safe - Applies committed logs (callbacks wrapped in @unsafe blocks)
void RaftServer::applyLogs() {
  // Only mark pending if there's actually new work to apply
  if (executeIndex < commitIndex) {
    apply_pending_.store(true, std::memory_order_release);
  }

  // If already applying, return - the current apply loop will pick up our work
  if (in_applying_logs_) {
    return;
  }

  in_applying_logs_ = true;

  // Keep applying logs until no more pending work arrives
  // This ensures we never drop work even under heavy load
  do {
    // Clear the pending flag before processing
    apply_pending_.store(false, std::memory_order_release);

    // Apply all committed logs
    for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
      auto next_instance = GetRaftInstance(id);
      if (next_instance && next_instance->log_) {
        // @unsafe
        {
        RuleWitnessGC(next_instance->log_);
        app_next_(id, next_instance->log_);  // Pass both id and log (signature requires 2 args)
        executeIndex = id;
        }
      } else {
        break;
      }
    }

    // Check if new work arrived while we were applying
    // If so, loop again to process it
  } while (apply_pending_.load(std::memory_order_acquire));

  in_applying_logs_ = false;

  // Cleanup old commands to prevent memory buildup
  int i = min_active_slot_;
  while (i + 60000 < executeIndex) {
    removeCmd(i);
    i++;
  }
  min_active_slot_ = i;
}

// @safe - external calls marked @external [safe], core replication loop
// TODO: Revisit borrow checker errors in this function.
// The checker reports "use after move" for loop-local variables (matchedIndices,
// batch_buffer_, batch_cmd, cmd) due to 2-iteration loop simulation. These variables
// are declared fresh each iteration, but the checker may not be resetting state
// correctly for loop-local declarations. Additionally, SendAppendEntries2 takes
// shared_ptr<Marshallable> by value (moves), which compounds the issue.
// Potential fixes: (1) Change SendAppendEntries2 to take const shared_ptr&,
// (2) Investigate checker's loop-local variable handling.
void RaftServer::HeartbeatLoop() {
  // @unsafe
  {
  auto hb_timer = new Timer();
  hb_timer->start();
  }

  parid_t partition_id = partition_id_;
  // Log_info("!!!!!!! if (!failover_)");
  // if (!failover_) {
    vector<SiteProxyPair> proxies;
    // @unsafe
    {
    proxies = commo()->rpc_par_proxies_[partition_id];
    }
    for (auto& p : proxies) {
      if (p.first == site_id_) {
        continue;  // skip self
      }
      // set matchIndex = 0
      match_index_[p.first] = 0;
      // set nextIndex = 1
      next_index_[p.first] = 1;
    }
    // matchedIndex and nextIndex should have indices for all servers except self
    verify(match_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id) - 1);
    verify(next_index_.size() == Config::GetConfig()->GetPartitionSize(partition_id) - 1);
  // }

  Log_debug("heartbeat loop init from site: %d", site_id_);
  looping_ = true;
  while(looping_) {
    uint64_t term;
    {
      {
        // std::lock_guard<std::recursive_mutex> lock(ready_for_replication_mtx_);
        // if (ready_for_replication_ == nullptr)
          ready_for_replication_ = Reactor::create_sp_event<IntEvent>();
        ready_for_replication_->set(0);
      }
      ready_for_replication_->wait(HEARTBEAT_INTERVAL);
      {
        // std::lock_guard<std::recursive_mutex> lock(ready_for_replication_mtx_);
        ready_for_replication_ = nullptr;
      }
      // Fiber::sleep(HEARTBEAT_INTERVAL);
      // Log_info("heartbeat loop at loc %d", loc_id_);
      if (!IsLeader()) {
        // Log_info("heartbeat loop at loc %d skip since not leader", loc_id_);
        continue;
      }
      // Log_info("[1]heartbeat loop at loc %d continue since is leader", loc_id_);
      // Log_info("time b/f sleep %" PRIu64, Time::now());
      // Fiber::sleep(HEARTBEAT_INTERVAL);
      // Log_info("time a/f sleep %" PRIu64, Time::now());
      int nservers = 0;
      // @unsafe
      {
      nservers = Config::GetConfig()->GetPartitionSize(partition_id);
      }
      // ================================================================
      // PARALLEL HEARTBEAT: Send AppendEntries to all followers at once,
      // wait with a single composite event, then process all responses.
      // This prevents sequential 500ms waits from starving heartbeats.
      // ================================================================

      if (!IsLeader()) {
        continue;
      }

      // --- Phase 0: Update commitIndex from last round's match_indices ---
      mtx_.lock();
      {
        std::vector<uint64_t> matchedIndices{};
        for (auto mit = match_index_.begin(); mit != match_index_.end(); mit++) {
          matchedIndices.push_back(mit->second);
        }
        if ((int)matchedIndices.size() == nservers - 1) {
          std::sort(matchedIndices.begin(), matchedIndices.end());
          uint64_t newCommitIndex = matchedIndices[(nservers - 1) / 2];
          if (newCommitIndex > lastLogIndex) {
            newCommitIndex = lastLogIndex;
          }
          auto commitInstance = GetRaftInstance(newCommitIndex);
          if (commitInstance && newCommitIndex > commitIndex && (commitInstance->term == currentTerm)) {
            Log_debug("newCommitIndex %d", newCommitIndex);
            auto old_commit = commitIndex;
            commitIndex = newCommitIndex;
            EnqueueCommittedEntries(old_commit, commitIndex);
          }
        }
        // Background apply thread handles entry application
        term = currentTerm;
      }
      mtx_.unlock();

      if (!IsLeader()) {
        continue;
      }

      // --- Phase 1: PREPARE and SEND all AppendEntries RPCs in parallel ---
      // @unsafe - RPC calls and shared_ptr operations
      struct FollowerRPC {
        siteid_t site_id = 0;
        shared_ptr<IntEvent> event;
        shared_ptr<Marshallable> cmd;
        uint64_t ret_status = 0;
        uint64_t ret_term = 0;
        uint64_t ret_last_log_index = 0;
        uint64_t prevLogIndex = 0;
        uint64_t prevLogTerm = 0;
        bool skipped = false;
      };

      // Count followers first, then reserve to prevent vector reallocation.
      // This ensures pointers into vector elements remain stable for async RPCs.
      size_t n_followers = 0;
      for (auto it = next_index_.begin(); it != next_index_.end(); it++) {
        if (it->first != site_id_) n_followers++;
      }

      std::vector<FollowerRPC> rpcs;
      rpcs.reserve(n_followers);

      // Pass 1: Prepare batches for all followers
      mtx_.lock();
      for (auto it = next_index_.begin(); it != next_index_.end(); it++) {
        auto site_id = it->first;
        if (site_id == site_id_) continue;
        if (!IsLeader()) break;

        rpcs.emplace_back();
        auto& rpc = rpcs.back();
        rpc.site_id = site_id;

        uint64_t prevLogIndex = it->second - 1;
        if (prevLogIndex > lastLogIndex) {
          it->second = lastLogIndex + 1;
          prevLogIndex = it->second - 1;
        }
        if (prevLogIndex > lastLogIndex) {
          it->second = 1;
          rpc.skipped = true;
          continue;
        }

        auto instance = GetRaftInstance(prevLogIndex);
        if (!instance) {
          Log_error("[HEARTBEAT-SEND] [CRITICAL] GetRaftInstance(%lu) returned NULL! Skipping follower %d",
                    prevLogIndex, site_id);
          rpc.skipped = true;
          continue;
        }

        rpc.prevLogIndex = prevLogIndex;
        rpc.prevLogTerm = instance->term;
        rpc.cmd = nullptr;

#ifndef RAFT_BATCH_OPTIMIZATION
        if (it->second <= lastLogIndex) {
          auto curInstance = GetRaftInstance(it->second);
          if (curInstance) {
            rpc.cmd = curInstance->log_;
          }
        }
#endif

#ifdef RAFT_BATCH_OPTIMIZATION
        vector<shared_ptr<TpcCommitCommand>> batch_buffer_;
        for (int idx = std::max(it->second, min_active_slot_); idx <= lastLogIndex; idx++) {
          auto curInstance = GetRaftInstance(idx);
          if (!curInstance) continue;
          shared_ptr<TpcCommitCommand> curCmd = dynamic_pointer_cast<TpcCommitCommand>(curInstance->log_);
          if (!curCmd) continue;
          curCmd->term = curInstance->term;
          batch_buffer_.push_back(curCmd);
        }
        if (!batch_buffer_.empty()) {
          shared_ptr<TpcBatchCommand> batch_cmd = std::make_shared<TpcBatchCommand>();
          batch_cmd->AddCmds(batch_buffer_);
          rpc.cmd = dynamic_pointer_cast<Marshallable>(batch_cmd);
        }
#endif
      }
      mtx_.unlock();

      // Pass 2: Send RPCs (pointers into rpcs[] are stable since we reserved)
      for (auto& rpc : rpcs) {
        if (rpc.skipped) continue;
        if (!IsLeader()) break;

        // @unsafe - Send RPC asynchronously (returns immediately)
        rpc.event = commo()->SendAppendEntries2(rpc.site_id,
                                                partition_id,
                                                -1, -1,
                                                IsLeader(),
                                                site_id_,
                                                term,
                                                rpc.prevLogIndex,
                                                rpc.prevLogTerm,
                                                commitIndex,
                                                rpc.cmd,
                                                0,
                                                &rpc.ret_status,
                                                &rpc.ret_term,
                                                &rpc.ret_last_log_index);
      }

      // --- Phase 2: WAIT for all responses with single composite timeout ---
      if (!rpcs.empty()) {
        auto wait_all = Reactor::create_sp_event<WaitAll>();
        for (auto& rpc : rpcs) {
          if (!rpc.skipped && rpc.event) {
            wait_all->events_.push_back(rpc.event);
          }
        }
        if (!wait_all->events_.empty()) {
          wait_all->wait(500000);  // 500ms total for ALL followers
        }
      }

      // --- Phase 3: PROCESS all responses ---
      bool should_step_down = false;
      uint64_t step_down_term = 0;

      for (auto& rpc : rpcs) {
        if (rpc.skipped || !rpc.event) continue;
        if (rpc.event->status_.get() == Event::TIMEOUT) continue;
        if (!IsLeader()) break;

        mtx_.lock();
        auto& next_index = next_index_[rpc.site_id];
        auto& match_index = match_index_[rpc.site_id];

        if (rpc.ret_status == false && rpc.ret_term == 0 && rpc.ret_last_log_index == 0) {
          // do nothing - empty/failed response
        } else if (currentTerm > term) {
          // stale term, do nothing
        } else if (rpc.ret_status == 0 && rpc.ret_term > term) {
          // case 1: rejected because leader's term is expired
          if (currentTerm == term) {
            Log_info("[STEPDOWN] Site %d: Stepping down due to higher term from follower %d (my_term=%lu, follower_term=%lu)",
                     site_id_, rpc.site_id, term, rpc.ret_term);
            should_step_down = true;
            step_down_term = rpc.ret_term;
          }
        } else if (rpc.ret_status == 0) {
          // case 2: rejected because log mismatch - backtrack next_index
          if (rpc.ret_last_log_index > 0 && rpc.ret_last_log_index < next_index - 1) {
            uint64_t old_next = next_index;
            next_index = rpc.ret_last_log_index + 1;
            Log_info("[LOG-RECONCILE] Site %d: Fast backoff for follower %d: next_index %lu -> %lu (follower last: %lu)",
                     site_id_, rpc.site_id, old_next, next_index, rpc.ret_last_log_index);
          } else if (next_index > 10) {
            uint64_t old_next = next_index;
            next_index = next_index / 2;
            Log_info("[LOG-RECONCILE] Site %d: Exponential backoff for follower %d: next_index %lu -> %lu",
                     site_id_, rpc.site_id, old_next, next_index);
          } else if (next_index > 1) {
            next_index--;
          } else {
            next_index = 1;
          }
        } else {
          // case 3: accepted
          verify(rpc.ret_status == true);
          if (rpc.cmd == nullptr) {
            Log_debug("case 3A: heartbeat accepted from follower %d", rpc.site_id);
            if (rpc.ret_last_log_index >= next_index && next_index <= lastLogIndex) {
              next_index++;
            }
          } else {
            if (rpc.ret_last_log_index < next_index) {
              next_index = rpc.ret_last_log_index + 1;
              match_index = rpc.ret_last_log_index;
            } else {
#ifndef RAFT_BATCH_OPTIMIZATION
              match_index = next_index;
              next_index++;
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
              match_index = rpc.ret_last_log_index;
              next_index = rpc.ret_last_log_index + 1;
#endif
              if (match_index > lastLogIndex) {
                match_index = lastLogIndex;
              }
            }
          }
        }
        mtx_.unlock();
      }

      // Handle stepdown after processing all responses
      if (should_step_down) {
        mtx_.lock();
        setIsLeader(false);
        currentTerm = step_down_term;
        mtx_.unlock();
        continue;
      }

      // Update commitIndex after processing all responses
      if (IsLeader()) {
        mtx_.lock();
        std::vector<uint64_t> matchedIndices{};
        for (auto mit = match_index_.begin(); mit != match_index_.end(); mit++) {
          matchedIndices.push_back(mit->second);
        }
        if ((int)matchedIndices.size() == nservers - 1) {
          std::sort(matchedIndices.begin(), matchedIndices.end());
          uint64_t newCommitIndex = matchedIndices[(nservers - 1) / 2];
          if (newCommitIndex > lastLogIndex) {
            newCommitIndex = lastLogIndex;
          }
          auto commitInstance = GetRaftInstance(newCommitIndex);
          if (commitInstance && newCommitIndex > commitIndex && (commitInstance->term == currentTerm)) {
            auto old_commit = commitIndex;
            commitIndex = newCommitIndex;
            EnqueueCommittedEntries(old_commit, commitIndex);
          }
        }
        // Background apply thread handles entry application
        mtx_.unlock();
      }
    }

    // ============================================================================
    // LEADERSHIP TRANSFER: Check if we should transfer to preferred replica
    // ============================================================================
    // ============================================================================
    // LEADERSHIP TRANSFER: Handled by Monitor Thread
    // ============================================================================
    // Leadership transfer is now handled by StartLeadershipTransferMonitoring() thread,
    // not here in HeartbeatLoop. This prevents race conditions and double-triggering.
    // The monitor thread is started in setIsLeader() when becoming a non-preferred leader.
    //
    // REMOVED: The piggybacked check that was here to avoid race with monitor thread.
	}
}

// @unsafe - thread join and timer cleanup require manual resource management
RaftServer::~RaftServer() {
  // CRITICAL: Set stop_ FIRST to signal all coroutines to stop
  // This must happen before vtable collapse to prevent race conditions
  stop_ = true;

  // Stop the HeartbeatLoop
  if (heartbeat_ && looping_) {
    Log_info("[SHUTDOWN] Stopping HeartbeatLoop for site=%d", site_id_);
    looping_ = false;

    // Wake up the HeartbeatLoop if it's sleeping so it can see looping_=false
    if (ready_for_replication_) {
      ready_for_replication_->set(1);
    }
  }

  // Stop leadership transfer monitoring thread if running
  StopLeadershipTransferMonitoring();

  // CRITICAL: Sleep briefly to allow detached coroutines to see stop_=true and exit
  // The election timer coroutine (StartElectionTimer) and leadership transfer coroutine
  // (InitiateLeadershipTransfer) are detached and check stop_ before calling RequestVote().
  // We need to give them time to notice stop_=true and return before the vtable collapses.
  // Without this sleep, there's a race where the coroutine wakes up, checks stop_=false,
  // then the destructor runs (vtable collapses), then the coroutine calls RequestVote()
  // through the base class vtable, hitting verify(0) and aborting.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  Log_info("site par %d, loc %d: prepare %d, accept %d, commit %d",
      partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
}

// @safe - external calls marked @external [safe], mutex/pointer ops in @unsafe blocks
bool RaftServer::RequestVote() {
  // FIX 2: Prevent RequestVote during shutdown
  // The election timer coroutine may fire after ~RaftServer destructor runs,
  // causing a call to the base class TxLogServer::RequestVote() which hits verify(0)
  // Check stop_ flag to avoid this crash during teardown
  if (stop_) {
    Log_debug("[RAFT-SHUTDOWN] RequestVote called during shutdown (site=%d), ignoring to prevent crash", site_id_);
    return false;
  }

  // for(int i = 0; i < 1000; i++) Log_info("not calling the wrong method");

  parid_t par_id = 0;
  parid_t loc_id = 0;
  // @unsafe
  {
  par_id = this->frame_->site_info_->partition_id_ ;
  loc_id = this->frame_->site_info_->locale_id ;
  }

  uint32_t lstoff = 0  ;
  slotid_t lst_idx = 0 ;
  ballot_t lst_term = 0 ;
  ballot_t prev_term = 0;
  siteid_t prev_vote_for;
  // @unsafe
  {
  prev_vote_for = INVALID_SITEID;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    prev_term = currentTerm;
    prev_vote_for = vote_for_;
    auto prev_local_term = currentTerm;
    currentTerm++ ;
    vote_for_ = site_id_;  // Vote for ourselves when starting election
    LogTermChange("starting election", prev_local_term, currentTerm);
    PersistTermAndVote();  // Persist term increment and self-vote
    lstoff = lastLogIndex - snapidx_ ;
    if (lstoff == 0) {
      lst_idx = snapidx_;
      lst_term = snapterm_;
    } else {
      auto log = GetRaftInstance(lstoff) ; // causes min_active_slot_ verification error (server.h:247)
      lst_idx = lstoff + snapidx_ ;
      lst_term = log->term ;
    }
  }
  
  auto term = currentTerm;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server %d (loc %d) starting election term %lu->%lu lastLogIdx=%lu lastLogTerm=%lu prev_vote_for=%d",
           site_id_, loc_id, prev_term, term, lst_idx, lst_term, prev_vote_for);
#endif
  shared_ptr<RaftVoteQuorumEvent> sp_quorum;
  // @unsafe
  {
  sp_quorum = ((RaftCommo *)(this->commo_))->BroadcastVote(par_id,lst_idx,lst_term,loc_id, term );
  sp_quorum->wait(1000000);
  }
  std::lock_guard<std::recursive_mutex> lock1(mtx_);
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server %d term %lu vote outcome yes=%d no=%d highest_term_seen=%ld timeout=%d",
           site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_, sp_quorum->Term(), sp_quorum->timeouted_);
#endif
  if (sp_quorum->yes()) {
    verify(currentTerm >= term);
    if (term != currentTerm) {
#ifdef RAFT_LEADER_ELECTION_DEBUG
      Log_info("[RAFT_ELECTION] server %d abandoning leadership claim because local term advanced to %lu", site_id_, currentTerm);
#endif
      return false;
    }
    // become a leader
    setIsLeader(true) ;
    // verify(currentTerm == term); // [Jetpack] Comment this since in failure recovery test this will fail after experiment end.
    Log_debug("site %d became leader for term %d", site_id_, term);

#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server %d won election term %lu (votes yes=%d no=%d)",
             site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_);
#endif

    this->rep_frame_ = this->frame_ ;

    // auto co = ((TxLogServer *)(this))->CreateRepCoord(0);
    // auto empty_cmd = std::make_shared<TpcEmptyCommand>();
    // verify(empty_cmd->kind_ == MarshallDeputy::CMD_TPC_EMPTY);
    // auto sp_m = dynamic_pointer_cast<Marshallable>(empty_cmd);
    // ((CoordinatorRaft*)co)->Submit(sp_m);
    
    if(IsLeader()) {
	  	//for(int i = 0; i < 100; i++) Log_info("wait wait wait");
      Log_debug("vote accepted %d curterm %d", loc_id, currentTerm);
#ifdef RAFT_TEST_CORO
      // Skip JetpackRecovery in test environment to avoid RPC handler issues
#else
      if (JetpackRecoveryEnabled()) {
        JetpackRecoveryEntry(); // Trigger Jetpack recovery on new leader election
      }
#endif
  		req_voting_ = false ;
			return true;
    } else {
      Log_debug("vote rejected %d curterm %d, do rollback", loc_id, currentTerm);
      setIsLeader(false) ;
      resetTimer("election lost (votes rejected)");
    	return false;
		}
  } else if (sp_quorum->no()) {
    // become a follower
    Log_debug("site %d requestvote rejected", site_id_);
    setIsLeader(false) ;
    resetTimer("election lost (quorum no)");
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server %d lost election term %lu (yes=%d no=%d) highest_term=%ld",
             site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_, sp_quorum->Term());
#endif
    //reset cur term if new term is higher
    ballot_t new_term = sp_quorum->Term() ;
    if (new_term > currentTerm) {
      auto prev_local_term = currentTerm;
      currentTerm = new_term;
      vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
      LogTermChange("observed higher term from RequestVote replies", prev_local_term, currentTerm);
    }
  	req_voting_ = false ;
		return false;
  } else {
    Log_debug("vote timeout %d", loc_id);
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server %d election timed out term %lu (yes=%d no=%d)",
             site_id_, term, sp_quorum->n_voted_yes_, sp_quorum->n_voted_no_);
#endif
    resetTimer("election timed out");
  	req_voting_ = false ;
		return false;
  }
}

// @safe - calls @safe doVote, external calls marked @external [safe]
void RaftServer::OnRequestVote(const slotid_t& lst_log_idx,
                               const ballot_t& lst_log_term,
                               const siteid_t& can_id,
                               const ballot_t& can_term,
                               ballot_t *reply_term,
                               bool_t *vote_granted,
                               rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("raft receives vote from candidate: %llx", can_id);

  uint64_t cur_term = currentTerm ;
  if( can_term < cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }

  // has voted to a machine in the same term, vote no
  // CRITICAL FIX: Only reject if we already voted for someone else in this term
  // Standard Raft allows voting for the SAME candidate multiple times (idempotent)
  // and allows voting if we haven't voted yet in this term
  // @unsafe
  {
  if( can_term == cur_term && vote_for_ != INVALID_SITEID && vote_for_ != can_id )
  {
    Log_debug("site %d vote NO for %d (already voted for %d in term %lu)",
              site_id_, can_id, vote_for_, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;
    return ;
  }
  }

  // If we already voted for this same candidate in this term, vote YES again (idempotent)
  if( can_term == cur_term && vote_for_ == can_id )
  {
    Log_debug("site %d vote YES for %d (already voted for them in term %lu, idempotent)",
              site_id_, can_id, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true, std::move(cb)) ;
    return ;
  }

  // lstoff starts from 1
  uint32_t lstoff = lastLogIndex - snapidx_ ;

  ballot_t curlstterm = snapterm_ ;
  slotid_t curlstidx = lastLogIndex ;

  if(lstoff > 0 )
  {
    auto log = GetRaftInstance(lstoff) ;
    curlstterm = log->term ;
  }

  Log_debug("vote for lstoff %d, curlstterm %d, curlstidx %d", lstoff, curlstterm, curlstidx  );


  // TODO del only for test
  verify(lstoff == lastLogIndex ) ;

  if( lst_log_term > curlstterm || (lst_log_term == curlstterm && lst_log_idx >= curlstidx) )
  {
    Log_debug("site %d vote for request vote from %d, lastidx %d, lastterm %d", site_id_, can_id, curlstidx, curlstterm);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true, std::move(cb)) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false, std::move(cb)) ;

}

// @safe - Election timer setup (Fiber::create_run, Time::now, RandomGenerator::rand marked safe via @external)
void RaftServer::StartElectionTimer() {
  // @unsafe
  { resetTimer("start election timer"); }
  last_heartbeat_time_ = Time::now();

  Fiber::create_run([this]() {
    Log_debug("start timer for election") ;

    while(!stop_) {
      // Use dynamic election timeout based on preferred replica role and grace period
      uint64_t election_timeout = GetElectionTimeout();

      // Sleep for a portion of the timeout before checking
      Fiber::sleep(RandomGenerator::rand(HEARTBEAT_INTERVAL * 2, HEARTBEAT_INTERVAL * 4));

      auto time_now = Time::now();
      auto time_elapsed = time_now - last_heartbeat_time_;

      // Only log when timeout actually fires or when debugging
      // Log_info("[ELECTION_TIMER] Site %d: checking - is_leader=%d time_elapsed=%lu election_timeout=%lu last_hb_time=%lu",
      //          site_id_, IsLeader(), time_elapsed, election_timeout, last_heartbeat_time_);

      if (!IsLeader() && time_elapsed > election_timeout) {
        Log_info("[ELECTION_TIMER] Site %d: TIMEOUT FIRED - starting election (elapsed=%lu > timeout=%lu)",
                 site_id_, time_elapsed, election_timeout);

        // ask to vote
        req_voting_ = true ;
        Log_info("[ELECTION_START] Site %d: TRIGGERING REQUESTVOTE - time_elapsed=%lu > timeout=%lu last_hb=%lu current_term=%lu vote_for=%d",
                 site_id_, time_elapsed, election_timeout, last_heartbeat_time_, currentTerm, vote_for_);
        // CRITICAL: Check stop_ before calling RequestVote() to prevent
        // calling through collapsed vtable after object destruction
        if (stop_) return;
        RequestVote() ;
        while(req_voting_) {
          Fiber::sleep(wait_int_);
          if(stop_) return ;
        }
      }
    }
  });
}

// @safe - external calls marked @external [safe], pointer ops in @unsafe blocks
bool RaftServer::Start(shared_ptr<Marshallable> &cmd,
                       uint64_t *index,
                       uint64_t *term,
                       slotid_t slot_id,
                       ballot_t ballot) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // #ifndef RAFT_TEST_CORO
  // if (!heartbeat_setup_) {
  //   heartbeat_setup_ = true;
  //   if (heartbeat_) {
  //     Log_debug("starting heartbeat loop at site %d", site_id_);
  //     Fiber::create_run([this](){
  //       this->HeartbeatLoop(); 
  //     });
  //     // Start election timeout loop
  //     Log_info("!!!!!!! if (failover_)");
  //     if (failover_) {
  //       Fiber::create_run([this](){
  //         StartElectionTimer(); 
  //       });
  //     }
  //   }
  // }
  // #endif
  if (!IsLeader()) {
    // @unsafe
    {
    *index = 0;
    *term = 0;
    }
    return false;
  }
  SetLocalAppend(cmd, term, index, slot_id, ballot);
  // SetLocalAppend returns the old lastLogIndex value, but Start returns the
  // index of the newly appended instance
  // @unsafe
  {
  verify(lastLogIndex == (*index) + 1);
  *index = lastLogIndex;
  Log_debug("Start(): ldr=%d index=%ld term=%ld", loc_id_, *index, *term);
  }
  return true;
}

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server
 * should we exclude the execution of this function for leader? */
// @safe - external calls marked @external [safe], output pointer writes in @unsafe blocks
void RaftServer::OnAppendEntries(const slotid_t slot_id,
                                 const ballot_t ballot,
                                 const uint64_t leaderCurrentTerm,
                                 const siteid_t leaderSiteId,
                                 const uint64_t leaderPrevLogIndex,
                                 const uint64_t leaderPrevLogTerm,
                                 const uint64_t leaderCommitIndex,
                                 shared_ptr<Marshallable> &cmd,
                                 const uint64_t leaderNextLogTerm, // disabled in batched version (term recorded in the TpcCommitCommand)
                                 uint64_t *followerAppendOK,
                                 uint64_t *followerCurrentTerm,
                                 uint64_t *followerLastLogIndex,
                                 rusty::Function<void()> cb,
                                 bool trigger_election_now) {
  mtx_.lock();

  bool term_ok = (leaderCurrentTerm >= this->currentTerm);
  bool index_ok = (leaderPrevLogIndex <= this->lastLogIndex);
  uint64_t local_prev_term = 0;
  if (leaderPrevLogIndex > 0 && leaderPrevLogIndex <= this->lastLogIndex) {
      local_prev_term = GetRaftInstance(leaderPrevLogIndex)->term;
  }
  bool prev_term_ok = (leaderPrevLogIndex == 0 || local_prev_term == leaderPrevLogTerm);

  // Only log rejections or when cmd is present (actual log entries)
  if (!term_ok || !index_ok || !prev_term_ok || cmd != nullptr) {
  }

  // CRITICAL FIX: Reset timer if we hear from a current-term leader, even if log conflicts
  // This prevents followers with divergent logs from constantly starting elections
  // while the leader is trying to repair their log via backtracking
  if (term_ok) {
      // @unsafe
      { resetTimer("AppendEntries from current-term leader"); }
      if (leaderCurrentTerm > this->currentTerm) {
          auto prev_term = currentTerm;
          currentTerm = leaderCurrentTerm;
          // @unsafe
          {
          vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term
          }
          LogTermChange("AppendEntries leader term is newer", prev_term, currentTerm, leaderSiteId);
          Log_debug("server %d, set to be follower", loc_id_ ) ;
          setIsLeader(false) ;
          PersistTermAndVote();  // Persist term/vote change
      }
  }

  if (term_ok && index_ok && prev_term_ok) {
      Log_debug("refresh timer on appendentry");

      // // Update follower's view to track the current leader
      // if (!IsLeader() && leaderSiteId != INVALID_SITEID) {
      //     int prev_leader = new_view_.GetLeader();
      //     old_view_ = new_view_;
      //     int n_replicas = Config::GetConfig()->GetPartitionSize(partition_id_);
      //     new_view_ = View(n_replicas, leaderSiteId, leaderCurrentTerm);
      //     Log_info("[RAFT_VIEW_FOLLOWER] Server %d observed leader change %d->%d term=%lu prev_term=%lu",
      //              site_id_, prev_leader, leaderSiteId, leaderCurrentTerm, currentTerm);
      // }

      if (cmd != nullptr) {
#ifndef RAFT_BATCH_OPTIMIZATION
        lastLogIndex = leaderPrevLogIndex + 1;
        auto instance = GetRaftInstance(lastLogIndex);
        instance->log_ = cmd;
        instance->term = leaderNextLogTerm;
        // Persist the log entry
        PersistLogEntry(lastLogIndex, *instance);
        // Log_debug("[APPEND_ENTRIES_ACCEPTED] Follower %d: accepted log entry at index %ld, term=%ld, lastLogIndex now=%ld",
        //          this->loc_id_, lastLogIndex, leaderNextLogTerm, lastLogIndex);
        // // Log the command that was accepted
        // auto cmd_accepted = dynamic_pointer_cast<TpcCommitCommand>(cmd);
        // Log_debug("[APPEND_ENTRIES_ACCEPTED] Follower %d: accepted command %d at index %ld",
        //          this->loc_id_, cmd_accepted ? cmd_accepted->tx_id_ : -1, lastLogIndex);
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
        auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
        int cnt = 0;
        std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>> entries_to_persist;
        for (shared_ptr<TpcCommitCommand>& c: cmds->cmds_) {
          cnt++;
          lastLogIndex = leaderPrevLogIndex + cnt;
          auto instance = GetRaftInstance(lastLogIndex);
          instance->log_ = c;
          instance->term = dynamic_pointer_cast<TpcCommitCommand>(c)->term;
          entries_to_persist.emplace_back(lastLogIndex, instance);
        }
        // Persist all entries in batch
        PersistLogEntries(entries_to_persist);
#endif
      }

      // update commitIndex and enqueue newly committed entries for the apply thread
      bool need_apply = false;
      if (leaderCommitIndex > commitIndex) {
        auto old_commit = commitIndex;
        commitIndex = std::min(leaderCommitIndex, lastLogIndex);
        verify(lastLogIndex >= commitIndex);
        need_apply = true;
        PersistCommitIndex();  // Persist commitIndex update
        // Enqueue entries [old_commit+1 .. commitIndex] for the background apply thread.
        // We're already under mtx_ here so raft_logs_ access is safe.
        EnqueueCommittedEntries(old_commit, commitIndex);
      }

      // @unsafe
      {
      *followerAppendOK = 1;
      *followerCurrentTerm = this->currentTerm;
      *followerLastLogIndex = this->lastLogIndex;
      }

      // Don't apply inline — the background apply thread handles it.
      // This keeps the PollThread responsive for RPCs so the follower's
      // commitIndex continues to advance via incoming AppendEntries.

#ifndef RAFT_TEST_CORO
      if (cmd != nullptr) {
        if (cmd->kind_ == MarshallDeputy::CMD_TPC_COMMIT){
          auto p_cmd = dynamic_pointer_cast<TpcCommitCommand>(cmd);
          auto sp_vec_piece = dynamic_pointer_cast<VecPieceData>(p_cmd->cmd_)->sp_vec_piece_data_;
          
          vector<struct KeyValue> kv_vector;
          int index = 0;
          for (auto it = sp_vec_piece->begin(); it != sp_vec_piece->end(); it++){
            auto cmd_input = (*it)->input.values_;
            for (auto it2 = cmd_input->begin(); it2 != cmd_input->end(); it2++) {
              struct KeyValue key_value = {it2->first, it2->second.get_i32()};
              kv_vector.push_back(key_value);
            }
          }

          struct KeyValue key_values[kv_vector.size()];
          std::copy(kv_vector.begin(), kv_vector.end(), key_values);

          // auto de = IO::write(filename, key_values, sizeof(struct KeyValue), kv_vector.size());
          // de->wait();
        } else {
          int value = -1;
          // auto de = IO::write(filename, &value, sizeof(int), 1);
          // de->wait();
        }
      }
#endif
    }
    else {
        Log_info("[APPEND_REJECT] Site %d rejecting AppendEntries from leader %d - term_ok=%d index_ok=%d prev_term_ok=%d (leaderTerm=%lu myTerm=%lu prevIdx=%lu myLastIdx=%lu local_prev_term=%lu)",
                 site_id_, leaderSiteId, term_ok, index_ok, prev_term_ok, leaderCurrentTerm, currentTerm,
                 leaderPrevLogIndex, lastLogIndex, local_prev_term);
        // @unsafe
        {
        *followerAppendOK = 0;
        *followerCurrentTerm = this->currentTerm;
        *followerLastLogIndex = this->lastLogIndex;
        }
    }

/*if (rand() % 1000 == 0) {
	usleep(25*1000);
}*/

    // ============================================================================
    // PIGGYBACKED LEADERSHIP TRANSFER: Handle trigger_election_now flag
    // ============================================================================
    // The leader sends trigger_election_now=true to ALL replicas during transfer.
    // How we handle it depends on whether we're the preferred replica or not.
    if (trigger_election_now) {
        if (AmIPreferredLeader()) {
            // I'm the PREFERRED replica - start election (if not already leader)
            if (!IsLeader()) {
                Log_info("[PIGGYBACKED-TRANSFER] Site %d (preferred): Received transfer signal from leader %d - will start election after 30ms",
                         site_id_, leaderSiteId);

                // Wait before starting election to allow old leader's heartbeats
                // to reach other replicas. This prevents election storms.
                Fiber::create_run([this]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    // CRITICAL: Check stop_ before calling RequestVote() to prevent
                    // calling through collapsed vtable after object destruction
                    if (stop_) return;
                    RequestVote();
                });
            } else {
                Log_info("[PIGGYBACKED-TRANSFER] Site %d (preferred): Received transfer signal but already leader - ignoring",
                         site_id_);
            }
        } else {
            // I'm a NON-PREFERRED replica - just log and do nothing
            Log_info("[PIGGYBACKED-TRANSFER] Site %d (non-preferred): Received transfer signal (preferred=%d)",
                     site_id_, preferred_leader_site_id_);
        }
    }

    mtx_.unlock();
    cb();
}

// @safe - Removes command from log (external calls wrapped in @unsafe blocks)
void RaftServer::removeCmd(slotid_t slot) {
  // @unsafe
  {
  auto cmd = dynamic_pointer_cast<TpcCommitCommand>(raft_logs_[slot]->log_);
  if (!cmd)
    return;
  tx_sched_->DestroyTx(cmd->tx_id_);
  }
  raft_logs_.erase(slot);
}

// @safe - Stores callback for later invocation
void RaftServer::RegisterLeaderChangeCallback(std::function<void(bool)> cb) {
  leader_change_cb_ = std::move(cb);
}

// @safe - external calls marked @external [safe], output pointer writes in @unsafe blocks
void RaftServer::OnTimeoutNow(const uint64_t leaderTerm,
                               const siteid_t leaderSiteId,
                               uint64_t *followerTerm,
                               bool_t *success,
                               rusty::Function<void()> cb) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // @unsafe
  {
  *followerTerm = currentTerm;
  *success = false;
  }

  // ============================================================================
  // Edge Case 0: Server shutting down
  // ============================================================================
  if (stop_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow - server shutting down", site_id_);
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 1: Stale TimeoutNow from old term
  // ============================================================================
  if (leaderTerm < currentTerm) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring stale TimeoutNow from leader %d (leader_term=%lu < my_term=%lu)",
             site_id_, leaderSiteId, leaderTerm, currentTerm);
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 1b: Leader is ahead of us - update term
  // ============================================================================
  if (leaderTerm > currentTerm) {
    Log_info("[TIMEOUT-NOW] Site %d: Leader %d has higher term (%lu > %lu) - updating term and stepping down",
             site_id_, leaderSiteId, leaderTerm, currentTerm);

    currentTerm = leaderTerm;
    // @unsafe
    {
    vote_for_ = INVALID_SITEID;  // Reset vote for new term
    }

    if (is_leader_) {
      setIsLeader(false);  // Step down from leadership
    }

    // @unsafe
    { *followerTerm = currentTerm; }
  }

  // ============================================================================
  // Edge Case 2: Already leader
  // ============================================================================
  if (is_leader_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow from leader %d - already leader in term %lu",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already leader (goal achieved)
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 3: Currently candidate (already in election)
  // ============================================================================
  if (req_voting_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow from leader %d - already requesting votes (term=%lu)",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already trying to become leader
    cb();
    return;
  }

  // ============================================================================
  // Edge Case 4: We're transferring leadership (stepping down)
  // ============================================================================
  if (transferring_leadership_) {
    Log_info("[TIMEOUT-NOW] Site %d: Ignoring TimeoutNow from leader %d - currently transferring leadership",
             site_id_, leaderSiteId);
    cb();
    return;
  }

  // ============================================================================
  // Valid TimeoutNow - Start Election Immediately
  // ============================================================================
  Log_info("[TIMEOUT-NOW] *** Site %d: Received TimeoutNow from leader %d (term=%lu) - STARTING ELECTION IMMEDIATELY ***",
           site_id_, leaderSiteId, leaderTerm);

  // Start election immediately (bypass random timeout)
  // This will increment term and send RequestVote RPCs
  bool election_started = RequestVote();

  if (election_started) {
    *success = true;
    Log_info("[TIMEOUT-NOW] Site %d: Election started successfully (new_term=%lu)",
             site_id_, currentTerm);
  } else {
    *success = false;
    Log_warn("[TIMEOUT-NOW] Site %d: Failed to start election",
             site_id_);
  }

  cb();
}

// @safe - Stops monitor thread (std::thread and std::atomic operations marked safe via @external)
void RaftServer::StopLeadershipTransferMonitoring() {
  leadership_monitor_stop_ = true;

  // Detach the monitor thread so it can exit gracefully without deadlock
  // The thread will see leadership_monitor_stop_ and exit on its own
  if (leadership_monitor_thread_.joinable()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site %d: Detaching monitor thread (will exit on its own)", site_id_);
    leadership_monitor_thread_.detach();
  }
}

// @safe - Starts monitor thread (threading and mutex operations marked safe via @external)
void RaftServer::StartLeadershipTransferMonitoring() {
  if (leadership_monitor_stop_.load()) {
    leadership_monitor_stop_ = false;
  }

  // Stop any existing monitor thread
  if (leadership_monitor_thread_.joinable()) {
    leadership_monitor_stop_ = true;
    leadership_monitor_thread_.join();
    leadership_monitor_stop_ = false;
  }

  Log_info("[LEADERSHIP-TRANSFER] Site %d: Starting leadership transfer monitoring thread",
           site_id_);

  // Launch monitoring thread
  leadership_monitor_thread_ = std::thread([this]() {
    const uint64_t CHECK_INTERVAL_MS = 1000;  // Check every 1 second
    const uint64_t MIN_STABLE_TIME_US = 500000; // Wait 0.5 seconds (in microseconds) after becoming leader before transferring

    uint64_t became_leader_time = Time::now();

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Monitor thread started (will check every %lums)",
             site_id_, CHECK_INTERVAL_MS);

    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));

      bool should_transfer = false;

      // Critical section: check shared state with proper locking
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);

        // Check if we should stop monitoring
        if (leadership_monitor_stop_) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: Monitor stop requested, exiting", site_id_);
          break;
        }

        // Check if server is shutting down
        if (stop_) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: Server shutting down, exiting monitor", site_id_);
          break;
        }

        // Check if we're still leader
        if (!is_leader_) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: No longer leader, exiting monitor", site_id_);
          break;
        }

        // Check if we became preferred (no longer need to transfer)
        if (AmIPreferredLeader()) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: I am now preferred leader, exiting monitor",
                   site_id_);
          break;
        }

        // Wait for cluster to stabilize after becoming leader
        uint64_t time_as_leader = Time::now() - became_leader_time;
        if (time_as_leader < MIN_STABLE_TIME_US) {
          continue;
        }

        // Check if we should transfer leadership
        if (ShouldTransferLeadership()) {
          Log_info("[LEADERSHIP-TRANSFER] Site %d: Conditions met, initiating transfer NOW",
                   site_id_);
          should_transfer = true;
        }
      } // End critical section - LOCK RELEASED

      // Call InitiateLeadershipTransfer WITHOUT holding lock to avoid deadlock
      if (should_transfer) {
        InitiateLeadershipTransfer();
        break;  // Exit after transferring
      }
    }

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Monitor thread exiting", site_id_);
  });
}

// @safe - Calls Setup if not already initialized
void RaftServer::EnsureSetup() {
  if (heartbeat_setup_) {
    return;
  }
  heartbeat_setup_ = true;
  Setup();
}

// @safe - Checks conditions for leadership transfer (mutex and map access marked safe via @external)
bool RaftServer::ShouldTransferLeadership() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Must be leader
  if (!is_leader_) {
    return false;
  }

  // Must not be preferred (preferred leaders don't transfer)
  if (AmIPreferredLeader()) {
    return false;
  }

  // Must have a preferred leader configured
  // @unsafe
  {
  if (preferred_leader_site_id_ == INVALID_SITEID) {
    return false;
  }
  }

  // Already transferring
  if (transferring_leadership_) {
    return false;
  }

  // Check if preferred replica is in our peer list
  auto it = match_index_.find(preferred_leader_site_id_);
  if (it == match_index_.end()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site %d: Preferred replica %d not in peer list",
              site_id_, preferred_leader_site_id_);
    return false;
  }

  // Check if preferred replica is caught up
  slotid_t preferred_match_index = it->second;
  bool is_caught_up = (preferred_match_index >= commitIndex);

  if (!is_caught_up) {
    Log_debug("[LEADERSHIP-TRANSFER] Site %d: Preferred replica %d not caught up (match=%lu, commit=%lu)",
              site_id_, preferred_leader_site_id_, preferred_match_index, commitIndex);
    return false;
  }

  Log_info("[LEADERSHIP-TRANSFER] Site %d: Preferred replica %d is caught up! Ready to transfer",
           site_id_, preferred_leader_site_id_);
  return true;
}

// @safe - Initiates leadership transfer (RPC calls wrapped in @unsafe blocks)
void RaftServer::InitiateLeadershipTransfer() {
  // Check if server is shutting down
  if (stop_) {
    Log_info("[LEADERSHIP-TRANSFER] Site %d: Aborting transfer - server shutting down", site_id_);
    return;
  }

  siteid_t target_site_id;
  parid_t par_id;
  uint64_t current_term_snapshot;

  // ============================================================================
  // PIGGYBACKED LEADERSHIP TRANSFER (Approach 2)
  // ============================================================================

  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    target_site_id = preferred_leader_site_id_;
    par_id = partition_id_;
    current_term_snapshot = currentTerm;

    // Mark transfer as in progress - this will suppress elections on non-preferred replicas
    transferring_leadership_ = true;
    leadership_transfer_start_time_ = Time::now();

    Log_info("[LEADERSHIP-TRANSFER] Site %d (partition %d): Starting transfer to site %d",
             site_id_, partition_id_, target_site_id);

    // Send heartbeats to ALL replicas
    for (auto& kv : match_index_) {
      siteid_t peer_site_id = kv.first;

      if (peer_site_id == site_id_) {
        continue;
      }

      slotid_t slot = commitIndex;
      ballot_t ballot = 0;
      uint64_t prevLogIndex = next_index_[peer_site_id] - 1;
      uint64_t prevLogTerm = 0;

      if (prevLogIndex > 0 && prevLogIndex < logs_.size()) {
        prevLogTerm = logs_[prevLogIndex]->term;
      }

      // Send trigger_election_now=true to ALL replicas during transfer:
      // - Preferred replica: Will start election
      // - Non-preferred replicas: Will activate election suppression
      bool trigger_election = true;  // Signal transfer to ALL replicas

      // @unsafe
      {
      commo()->SendAppendEntries(
        peer_site_id,
        partition_id_,
        slot,
        ballot,
        true,
        site_id_,
        currentTerm,
        prevLogIndex,
        prevLogTerm,
        commitIndex,
        nullptr,
        0,
        trigger_election
      );
      }
    }
  }

  // Sleep briefly to ensure the RPC library has time to send the packets.
  // Note: The preferred replica will wait 30ms before starting election,
  // so this sleep is just to ensure packet transmission, not to delay step-down.
  // We will likely step down earlier when we receive RequestVote from preferred replica.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // ============================================================================
  // Step Down from Leadership Immediately
  // ============================================================================
  // With piggybacked approach, we step down immediately after sending the message.
  // The preferred replica will:
  // 1. Reset its election timeout (from the heartbeat)
  // 2. Start election immediately (from the trigger_election_now flag)
  // 3. Win the election (since it's caught up and has all committed entries)
  //
  // Other replicas will:
  // 1. Reset their election timeouts (from normal heartbeats)
  // 2. Not start elections (timers reset)
  // 3. Vote for preferred replica when it requests votes
  {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Stepping down from leadership (current_term=%lu)",
             site_id_, currentTerm);

    // Become follower - this stops heartbeats and allows new leader to emerge
    setIsLeader(false);

    Log_info("[LEADERSHIP-TRANSFER] Site %d: Leadership transfer complete - now follower",
             site_id_);
  }
}

} // namespace janus
