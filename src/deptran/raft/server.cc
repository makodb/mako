#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "server.h"
// #include "paxos_worker.h"
#include "exec.h"
#include "frame.h"
#include "coordinator.h"
#include "../classic/tpc_command.h"
#include "file_snapshot_manager.hpp"
#include "replicated_db.h"

import std;

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
//   rrr::EventStatus::TIMEOUT: [safe, () -> int]
//   janus::View::View: [safe, (...) -> owned]
//   janus::View::operator=: [safe, (&'a mut, const &'a) -> &'a mut]
//   janus::TxLogServer::DestroyTx: [safe, (&'a mut, uint64_t) -> void]
//   janus::RaftCommo::SendAppendEntries2: [safe, (...) -> owned]
//   janus::RaftCommo::BroadcastVote: [safe, (...) -> owned]
// }

namespace janus {

namespace {

uint64_t ParseEnvUint64OrDefault(const char* env_name, uint64_t default_value) {
  const char* env = std::getenv(env_name);
  if (env == nullptr || *env == '\0') {
    return default_value;
  }

  char* endptr = nullptr;
  unsigned long long parsed = std::strtoull(env, &endptr, 10);
  if (endptr != env && *endptr == '\0' && parsed > 0) {
    Log_info("[LEADER-ELECTION] Using {}={}", env_name, parsed);
    return static_cast<uint64_t>(parsed);
  }

  Log_warn("[LEADER-ELECTION] Invalid {}='{}'; using default {}",
           env_name, env, static_cast<unsigned long>(default_value));
  return default_value;
}

uint64_t GetPreferredLeaderGracePeriodUs() {
  constexpr uint64_t kDefaultGracePeriodUs = 5000000ULL;  // 5s
  static uint64_t grace_period_us =
      ParseEnvUint64OrDefault("MAKO_RAFT_PREFERRED_GRACE_US", kDefaultGracePeriodUs);
  return grace_period_us;
}

uint64_t GetNonPreferredGraceElectionMinUs() {
  constexpr uint64_t kDefaultMinUs = 1000000ULL;  // 1s
  static uint64_t min_us = ParseEnvUint64OrDefault(
      "MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US", kDefaultMinUs);
  return min_us;
}

uint64_t GetNonPreferredGraceElectionMaxUs() {
  constexpr uint64_t kDefaultMaxUs = 2000000ULL;  // 2s
  static uint64_t max_us = ParseEnvUint64OrDefault(
      "MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US", kDefaultMaxUs);
  return max_us;
}

uint64_t RandomInRangeUs(uint64_t min_us, uint64_t max_us) {
  if (max_us < min_us) {
    std::swap(min_us, max_us);
  }
  if (max_us == min_us) {
    return min_us;
  }
  uint64_t range = max_us - min_us;
  if (range > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
    range = static_cast<uint64_t>(std::numeric_limits<int>::max());
  }
  return min_us + static_cast<uint64_t>(RandomGenerator::rand(0, static_cast<int>(range)));
}

uint64_t GetPreferredElectionTimeoutUs() {
  return RandomInRangeUs(150000ULL, 300000ULL);
}

uint64_t GetNonPreferredGraceElectionTimeoutUs() {
  return RandomInRangeUs(GetNonPreferredGraceElectionMinUs(),
                         GetNonPreferredGraceElectionMaxUs());
}

uint64_t GetNonPreferredSteadyElectionTimeoutUs() {
  return RandomInRangeUs(500000ULL, 1000000ULL);
}

uint64_t GetAppendEntriesBatchMaxEntries() {
  // Keep catch-up payload bounded to avoid oversized RPCs and timeout stalls
  // when a follower is far behind.
  constexpr uint64_t kDefaultMaxEntries = 256ULL;
  static uint64_t max_entries = ParseEnvUint64OrDefault(
      "MAKO_RAFT_APPEND_BATCH_MAX_ENTRIES", kDefaultMaxEntries);
  return max_entries;
}

bool IsPreferredLeaderConfigured(siteid_t preferred_leader_site_id) {
  return preferred_leader_site_id != INVALID_SITEID;
}

}  // namespace

// ============================================================================
// LOG PERSISTENCE IMPLEMENTATION
// ============================================================================

// @unsafe - Uses LogStorage API
void RaftServer::PersistTermAndVoteToLogStorage() {
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

// @unsafe - Uses LogStorage API
void RaftServer::PersistVoteToLogStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  // @unsafe
  {
  log_storage_->set_metadata(META_VOTE_FOR, std::to_string(static_cast<int64_t>(vote_for_)));
  log_storage_->sync();
  }
}

// @unsafe - Uses LogStorage API
void RaftServer::PersistCommitIndexToLogStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  // @unsafe
  {
  log_storage_->set_metadata(META_COMMIT_INDEX, std::to_string(commitIndex));
  // Also persist speculative indices alongside commitIndex
  log_storage_->set_metadata(META_SPEC_COMMIT_INDEX, std::to_string(specCommitIndex_));
  log_storage_->set_metadata(META_SECURED_LOG_INDEX, std::to_string(securedLogIndex_));
  // Note: Don't sync for commitIndex - it can be recovered from logs
  }
}

// @unsafe - Uses LogStorage API
// @unsafe - Persists specCommitIndex_ and securedLogIndex_ to storage
void RaftServer::PersistSpeculativeIndicesToLogStorage() {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  // @unsafe
  {
  log_storage_->set_metadata(META_SPEC_COMMIT_INDEX, std::to_string(specCommitIndex_));
  log_storage_->set_metadata(META_SECURED_LOG_INDEX, std::to_string(securedLogIndex_));
  // Note: Don't sync - these can be recovered conservatively from logs
  }
}

// @unsafe - Uses LogStorage API
void RaftServer::PersistLogEntryToLogStorage(slotid_t slot_id, const RaftData& data) {
  if (!log_storage_ || !log_storage_->is_open()) {
    return;
  }

  janus::raft::LogEntry entry(slot_id, data.term);
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

// @unsafe - Uses LogStorage API
void RaftServer::PersistLogEntriesToLogStorage(const std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>>& entries) {
  if (!log_storage_ || !log_storage_->is_open() || entries.empty()) {
    return;
  }

  std::vector<janus::raft::LogEntry> log_entries;
  log_entries.reserve(entries.size());

  for (const auto& [slot_id, data] : entries) {
    janus::raft::LogEntry entry(slot_id, data->term);
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

// @unsafe - Recovers state from LogStorage
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

  // Recover specCommitIndex
  auto spec_str = log_storage_->get_metadata(META_SPEC_COMMIT_INDEX);
  if (spec_str.is_some()) {
    specCommitIndex_ = std::stoull(spec_str.unwrap());
    Log_info("Recovered specCommitIndex={}", specCommitIndex_);
  }

  // Recover securedLogIndex
  auto secured_str = log_storage_->get_metadata(META_SECURED_LOG_INDEX);
  if (secured_str.is_some()) {
    securedLogIndex_ = std::stoull(secured_str.unwrap());
    Log_info("Recovered securedLogIndex={}", securedLogIndex_);
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
      // prep1/2: both LogEntry::command and RaftData::log_ are
      // janus::Command — direct copy (Command is cheap to copy via
      // its inner shared_ptr).
      data->log_ = entry.command;
      data->max_ballot_seen_ = entry.max_ballot_seen;
      data->max_ballot_accepted_ = entry.max_ballot_accepted;
      raft_logs_[entry.slot_id] = data;
    }
  }

  // Clamp speculative indices to maintain invariant:
  // securedLogIndex_ <= specCommitIndex_ <= lastLogIndex
  if (specCommitIndex_ > lastLogIndex) {
    Log_warn("[RAFT-RECOVERY] Clamping specCommitIndex {} -> {} (lastLogIndex)",
             specCommitIndex_, lastLogIndex);
    specCommitIndex_ = lastLogIndex;
  }
  if (securedLogIndex_ > specCommitIndex_) {
    Log_warn("[RAFT-RECOVERY] Clamping securedLogIndex {} -> {} (specCommitIndex)",
             securedLogIndex_, specCommitIndex_);
    securedLogIndex_ = specCommitIndex_;
  }

  Log_info("[RAFT-RECOVERY] Site {}: Recovered term={} vote_for={} lastLogIndex={} "
           "commitIndex={} specCommitIndex={} securedLogIndex={} entries={}",
           site_id_, currentTerm, vote_for_, lastLogIndex, commitIndex,
           specCommitIndex_, securedLogIndex_, raft_logs_.size());

  return true;
}

// @unsafe - Replays committed entries (callbacks wrapped in @unsafe blocks)
void RaftServer::ReplayCommittedEntries() {
  if (!app_next_) {
    Log_warn("[RAFT-REPLAY] Site {}: No app_next_ callback, skipping replay", site_id_);
    return;
  }

  std::lock_guard<std::recursive_mutex> lock(mtx_);

  slotid_t start = executeIndex + 1;
  slotid_t end = commitIndex;

  if (start > end) {
    Log_info("[RAFT-REPLAY] Site {}: No entries to replay (executeIndex={} >= commitIndex={})",
             site_id_, executeIndex, commitIndex);
    return;
  }

  Log_info("[RAFT-REPLAY] Site {}: Replaying entries {}..{}", site_id_, start, end);

  size_t replayed = 0;
  for (slotid_t id = start; id <= end; id++) {
    auto instance = GetRaftInstance(id);
    if (instance && instance->log_.has_value()) {
      // @unsafe
      {
      app_next_(id, instance->log_);
      executeIndex = id;
      replayed++;
      }
    } else {
      Log_warn("[RAFT-REPLAY] Site {}: Missing log entry at slot {}, stopping replay", site_id_, id);
      break;
    }
  }

  Log_info("[RAFT-REPLAY] Site {}: Replayed {} entries, executeIndex now {}",
           site_id_, replayed, executeIndex);

  // Log uncommitted entries status
  size_t uncommitted = GetUncommittedCount();
  if (uncommitted > 0) {
    Log_info("[RAFT-RECOVERY] Site {}: {} uncommitted entries (lastLogIndex={}, commitIndex={}) - will be resolved by consensus",
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

// @unsafe - Initializes SnapshotManager from environment config
void RaftServer::InitializeSnapshotManager() {
  const char* snapshot_flag = std::getenv("MAKO_RAFT_SNAPSHOTS");  // @unsafe
  bool should_enable = (snapshot_flag &&
                       (strcmp(snapshot_flag, "1") == 0 ||
                        strcmp(snapshot_flag, "true") == 0));

  if (!should_enable) {
    Log_info("[RAFT-SNAPSHOT] Snapshots disabled for site {} (set MAKO_RAFT_SNAPSHOTS=1 to enable)",
             site_id_);
    return;
  }

  // Build snapshot config
  janus::raft::SnapshotConfig snap_config;
  // @unsafe { getenv is not borrow-checked }
  const char* custom_path = std::getenv("MAKO_RAFT_SNAPSHOT_PATH");
  if (custom_path && custom_path[0] != '\0') {
    snap_config.storage_path = std::string(custom_path) + "/raft_snap_" +
                               std::to_string(site_id_) + "_partition_" +
                               std::to_string(partition_id_);
  } else {
    snap_config = janus::raft::SnapshotConfig::for_replica(partition_id_, loc_id_);
  }

  // Check for custom snapshot interval
  const char* interval_str = std::getenv("MAKO_RAFT_SNAPSHOT_INTERVAL");  // @unsafe
  if (interval_str && interval_str[0] != '\0') {
    snap_config.snapshot_interval = std::stoull(interval_str);
    snapshot_threshold_ = snap_config.snapshot_interval;
  }

  // Create the FileSnapshotManager
  auto manager = std::make_shared<janus::raft::FileSnapshotManager>(snap_config);
  SetSnapshotManager(manager);

  // If a snapshot exists, load its metadata into snapidx_/snapterm_
  auto latest = manager->GetLatestSnapshot();
  if (latest.is_some()) {
    auto meta = latest.unwrap();
    snapidx_ = meta.last_included_index;
    snapterm_ = meta.last_included_term;
    Log_info("[RAFT-SNAPSHOT] Loaded snapshot metadata: index={} term={} size={}",
             snapidx_, snapterm_, meta.size_bytes);
  }

  // @unsafe - Recover server state from snapshot metadata
  // InitializeSnapshotManager() runs AFTER RecoverFromStorage() in Setup(),
  // so log-recovered values may already be set. Only advance, never go backwards.
  if (snapidx_ > 0) {
    if (snapidx_ > executeIndex) {
      executeIndex = snapidx_;
      Log_info("[RAFT-SNAPSHOT] Recovery: set executeIndex={} from snapshot", executeIndex);
    }
    if (snapidx_ > commitIndex) {
      commitIndex = snapidx_;
      PersistCommitIndexToLogStorage();
      Log_info("[RAFT-SNAPSHOT] Recovery: set commitIndex={} from snapshot", commitIndex);
    }
    if (snapidx_ > lastLogIndex) {
      lastLogIndex = snapidx_;
      Log_info("[RAFT-SNAPSHOT] Recovery: set lastLogIndex={} from snapshot", lastLogIndex);
    }
    if (snapidx_ + 1 > min_active_slot_) {
      min_active_slot_ = snapidx_ + 1;
      Log_info("[RAFT-SNAPSHOT] Recovery: set min_active_slot_={} from snapshot", min_active_slot_);
    }
  }

  Log_info("[RAFT-SNAPSHOT] Initialized for site {} partition {}: path={} interval={}",
           site_id_, partition_id_, snap_config.storage_path.c_str(),
           snap_config.snapshot_interval);
}

// @safe - Returns true if a snapshot is available
bool RaftServer::HasSnapshot() const {
  // @unsafe
  {
    if (!snapshot_manager_) return false;
    auto latest = snapshot_manager_->GetLatestSnapshot();
    return latest.is_some();
  }
}

// @safe - Returns the last snapshotted log index
uint64_t RaftServer::GetSnapshotIndex() const {
  return snapidx_;
}

// @safe - Returns the term of the last snapshotted log entry
uint64_t RaftServer::GetSnapshotTerm() const {
  return snapterm_;
}

// @unsafe - Log compaction (storage operations wrapped in @unsafe blocks)
size_t RaftServer::CompactLog(slotid_t up_to_index) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Safety check: don't compact beyond commit index
  if (up_to_index > commitIndex) {
    Log_warn("[RAFT-COMPACT] Site {}: Cannot compact beyond commitIndex ({} > {})",
             site_id_, up_to_index, commitIndex);
    up_to_index = commitIndex;
  }

  // Determine first compactable slot. Without persistent storage we still
  // compact the in-memory map and advance min_active_slot_.
  slotid_t first_slot = min_active_slot_;
  if (log_storage_) {
    // @unsafe
    {
    slotid_t persisted_first = log_storage_->get_first_index();
    if (persisted_first != 0 && !log_storage_->empty()) {
      first_slot = persisted_first;
    }
    }
  }

  // Nothing to compact if up_to_index is before first slot
  if (up_to_index < first_slot) {
    Log_debug("[RAFT-COMPACT] Site {}: up_to_index {} < first_slot {}, nothing to compact",
              site_id_, up_to_index, first_slot);
    return 0;
  }

  size_t removed_storage = 0;
  if (log_storage_) {
    // @unsafe
    {
    if (log_storage_->remove_range(first_slot, up_to_index + 1)) {
      removed_storage = up_to_index - first_slot + 1;
    } else {
      Log_error("[RAFT-COMPACT] Site {}: Failed to compact persistent log entries [{}..{}]",
                site_id_, first_slot, up_to_index);
    }
    }
  }

  size_t removed_memory = 0;
  auto it = raft_logs_.lower_bound(first_slot);
  while (it != raft_logs_.end() && it->first <= up_to_index) {
    it = raft_logs_.erase(it);
    removed_memory++;
  }

  // Update min_active_slot_ even when persistence is disabled.
  if (up_to_index + 1 > min_active_slot_) {
    min_active_slot_ = up_to_index + 1;
  }

  if (log_storage_) {
    Log_info("[RAFT-COMPACT] Site {}: Compacted [{}..{}] (storage={}, memory={})",
             site_id_, first_slot, up_to_index, removed_storage, removed_memory);
    return removed_storage != 0 ? removed_storage : removed_memory;
  }

  Log_info("[RAFT-COMPACT] Site {}: Compacted in-memory entries [{}..{}] (memory={})",
           site_id_, first_slot, up_to_index, removed_memory);
  return removed_memory;
}

// @unsafe - Creates snapshot from current state, persists, and compacts log
void RaftServer::CreateSnapshot() {
  if (!snapshot_manager_) {
    Log_debug("[RAFT-SNAPSHOT] Site {}: No snapshot manager, skipping CreateSnapshot",
              site_id_);
    return;
  }

  slotid_t snap_index = executeIndex;
  if (snap_index == 0) {
    Log_debug("[RAFT-SNAPSHOT] Site {}: executeIndex is 0, nothing to snapshot",
              site_id_);
    return;
  }

  // Determine the term at the snapshot index
  ballot_t snap_term = 0;
  auto instance = GetRaftInstance(snap_index);
  if (instance) {
    snap_term = instance->term;
  } else {
    // If the instance has been cleaned up, use currentTerm as fallback
    snap_term = currentTerm;
    Log_warn("[RAFT-SNAPSHOT] Site {}: No instance at index {}, using currentTerm {}",
             site_id_, snap_index, snap_term);
  }

  // Serialize state machine data.
  // If a state machine snapshot callback is registered (e.g., ReplicatedDB),
  // use it to produce a full checkpoint. Otherwise, fall back to the minimal
  // 16-byte marker (executeIndex + term).
  // @unsafe { string operations, callback invocation }
  std::string state_data;
  if (create_sm_snapshot_cb_) {
    state_data = create_sm_snapshot_cb_();
    Log_info("[RAFT-SNAPSHOT] Site {}: State machine snapshot callback produced {} bytes",
             site_id_, state_data.size());
  } else {
    // Fallback: 8 bytes executeIndex + 8 bytes term
    state_data.resize(sizeof(uint64_t) * 2);
    char* ptr = state_data.data();
    std::memcpy(ptr, &snap_index, sizeof(uint64_t));
    ptr += sizeof(uint64_t);
    std::memcpy(ptr, &snap_term, sizeof(uint64_t));
  }

  // Persist the snapshot via the snapshot manager
  // @unsafe { snapshot_manager_ I/O operations }
  bool saved = snapshot_manager_->TakeSnapshot(
      snap_index, snap_term,
      state_data.data(), state_data.size());

  if (!saved) {
    Log_error("[RAFT-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
              site_id_, snap_index, snap_term);
    return;
  }

  // Update snapshot metadata
  slotid_t old_snapidx = snapidx_;
  snapidx_ = snap_index;
  snapterm_ = snap_term;

  Log_info("[RAFT-SNAPSHOT] Site {}: Snapshot saved at index={} term={} (prev snapidx={})",
           site_id_, snap_index, snap_term, old_snapidx);

  // Compact the log up to the snapshot index
  size_t compacted = CompactLog(snap_index);
  Log_info("[RAFT-SNAPSHOT] Site {}: Compacted {} entries up to index={}",
           site_id_, compacted, snap_index);
}

// ============================================================================

// @unsafe - Logs term changes (Log_info marked safe via @external)
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
    Log_info("[RAFT-TERM] server {} term {} -> {} ({}, source_site={})",
             site_id_, old_term, new_term, why, source);
  } else {
    Log_info("[RAFT-TERM] server {} term {} -> {} ({})",
             site_id_, old_term, new_term, why);
  }
  }
}

namespace {

// @unsafe
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
      Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK={} -> Jetpack recovery disabled", flag);
      return false;
    }
    if (is_false(value)) {
      return true;
    }

    Log_info("[JETPACK-RUNTIME] MAKO_DISABLE_JETPACK has unrecognised value '{}'; defaulting to disabled", flag);
    return false;
  }();
  return enabled;
}

}  // namespace

// @unsafe - raw pointer parameter is bounded (frame outlives server)
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
                                   janus::Command* reply_old_view,
                                   janus::Command* reply_new_view,
                                   KeyCmdBatchData& batch) {
  TxLogServer::OnJetpackPullCmd(jepoch, oepoch, keys, ok, reply_jepoch, reply_oepoch,
                                reply_old_view, reply_new_view, batch);
  if (!IsLeader()) {
    resetTimer("JetpackPullCmd RPC");
#ifdef RAFT_LEADER_ELECTION_DEBUG
    // Log_info("[RAFT_TIMER] server {} reset election timer due to JetpackPullCmd (keys={})",
    //          site_id_, keys.size());
#endif
  }
}

// @unsafe - Election timeout calculation (Time::now and RandomGenerator::rand marked safe via @external)
uint64_t RaftServer::GetElectionTimeout() {
  uint64_t current_time = Time::now(false);
  const uint64_t grace_period_us = GetPreferredLeaderGracePeriodUs();
  bool in_grace_period = (current_time - startup_timestamp_) < grace_period_us;

  if (!IsPreferredLeaderConfigured(preferred_leader_site_id_)) {
    // Traditional Raft behavior when no preferred leader is configured.
    return GetNonPreferredSteadyElectionTimeoutUs();
  }

  if (AmIPreferredLeader()) {
    return GetPreferredElectionTimeoutUs();
  } else if (in_grace_period) {
    // Startup grace timeout is tunable via env for test stability.
    return GetNonPreferredGraceElectionTimeoutUs();
  } else {
    return GetNonPreferredSteadyElectionTimeoutUs();
  }
}

// StartApplyFiber - lightweight status monitor on PollThread.
void RaftServer::StartApplyFiber() {
  Fiber::create_run([this]() {
    Log_info("[APPLY-FIBER] Site {}: Started apply fiber (monitor only)", site_id_);
    while (!stop_) {
      Fiber::sleep(5000000);  // 5s status check
      Log_info("[APPLY-FIBER] Site {}: executeIndex={} commitIndex={} lastLogIndex={}",
               site_id_, executeIndex, commitIndex, lastLogIndex);
    }
    Log_info("[APPLY-FIBER] Site {}: Apply fiber exiting (stop_=true)", site_id_);
  });
}

// Enqueue newly committed entries for the background apply thread.
// Called from OnAppendEntries (already under mtx_) when commitIndex advances.
void RaftServer::EnqueueCommittedEntries(slotid_t old_commit, slotid_t new_commit) {
  // apply_queue_ now holds Command — direct copy from
  // RaftData::log_ (also Command after prep2).
  std::vector<std::pair<slotid_t, Command>> batch;
  slotid_t first_missing = 0;
  for (slotid_t id = old_commit + 1; id <= new_commit; id++) {
    auto it = raft_logs_.find(id);
    if (it != raft_logs_.end() && it->second && it->second->log_.has_value()) {
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
    Log_info("[ENQUEUE] Site {}: gap at slot {} (range {}..{}, enqueued {})",
             site_id_, first_missing, old_commit + 1, new_commit, batch.size());
  }
  static uint64_t enqueue_log_counter = 0;
  if (enqueue_log_counter++ % 50 == 0) {
    size_t qsize = 0;
    {
      std::lock_guard<std::mutex> lock(apply_queue_mtx_);
      qsize = apply_queue_.size();
    }
    Log_info("[ENQUEUE] Site {}: enqueued {} entries ({}..{}) queue_total={}",
             site_id_, batch.size(), old_commit + 1, new_commit, qsize);
  }
}

// Background OS thread for entry application.
// Drains from apply_queue_ (populated by OnAppendEntries) to avoid contention on mtx_.
void RaftServer::StartApplyThread() {
  apply_thread_running_.store(true);
  apply_thread_ = std::thread([this]() {
    Log_info("[APPLY-THREAD] Site {}: Started background apply thread", site_id_);
    uint64_t apply_count = 0;
    auto last_log_time = std::chrono::steady_clock::now();
    while (!stop_ && apply_thread_running_.load()) {
      // Drain entries from the queue
      // apply_queue_ holds Command; entry is
      // pair<slotid_t, Command>.  RuleWitnessGC takes
      // shared_ptr<Marshallable> so unwrap at the boundary; app_next_
      // takes Command directly.
      std::pair<slotid_t, Command> entry;
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
          Log_info("[APPLY-THREAD] Site {}: ABOUT TO APPLY entry {} (queue_remaining={})",
                   site_id_, id, queue_size);
        }
        // @unsafe - callback may have side effects
        RuleWitnessGC(log_entry);
        app_next_(id, log_entry);
        if (id >= 470 && id <= 500) {
          Log_info("[APPLY-THREAD] Site {}: DONE APPLYING entry {}", site_id_, id);
        }
        executeIndex = id;
        apply_count++;

        // Log progress periodically
        if (apply_count % 100 == 0) {
          Log_info("[APPLY-THREAD] Site {}: applied {} entries, executeIndex={} queue_remaining={}",
                   site_id_, apply_count, executeIndex, queue_size);
        }

        // Snapshot trigger for queued apply path.
        // Cheap unlocked pre-check so the hot path stays lock-free when
        // we're far from the threshold or have no manager configured.
        if (snapshot_manager_ && snapidx_ < executeIndex &&
            (executeIndex - snapidx_) > snapshot_threshold_) {
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          // Re-check under the lock: CreateSnapshot mutates snapidx_ and
          // raft_logs_ via CompactLog.
          if (snapshot_manager_ && snapidx_ < executeIndex &&
              (executeIndex - snapidx_) > snapshot_threshold_) {
            CreateSnapshot();
          }
        }

        // Cleanup old log entries periodically to prevent memory buildup.
        // Only erase from the map — skip DestroyTx since the apply callback
        // may still reference transaction state on this thread.
        if (id % 5000 == 0) {
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          slotid_t cutoff = (executeIndex > 10000) ? executeIndex - 10000 : 0;
          while (min_active_slot_ < cutoff) {
            raft_logs_.erase(min_active_slot_);
            min_active_slot_++;
          }
        }
      } else {
        // Periodic heartbeat when queue is empty
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
          Log_info("[APPLY-THREAD] Site {}: IDLE executeIndex={} commitIndex={} queue_size={} applied_total={}",
                   site_id_, executeIndex, commitIndex, queue_size, apply_count);
          last_log_time = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    Log_info("[APPLY-THREAD] Site {}: Background apply thread exiting", site_id_);
  });
  // Keep the thread joinable so the destructor can await it. Detaching here
  // causes use-after-free: the thread captures `this` and keeps running after
  // ~RaftServer destroys the RaftServer, resulting in an empty std::function
  // invocation when it next pulls from apply_queue_.
}

// @unsafe - Server setup (Time::now, Log_debug, Fiber::create_run marked safe via @external)
void RaftServer::Setup() {
  // Record startup time for grace period logic
  startup_timestamp_ = Time::now(false);

  // ========== INITIALIZE PERSISTENCE (LogStorage + RecoveryManager) ==========
  const char* persistence_flag = std::getenv("MAKO_RAFT_PERSISTENCE");
  bool should_enable = (persistence_flag &&
                       (strcmp(persistence_flag, "1") == 0 ||
                        strcmp(persistence_flag, "true") == 0));

  if (should_enable) {
    // Check if async persistence is requested (default: sync)
    const char* async_flag = std::getenv("MAKO_RAFT_ASYNC_PERSISTENCE");
    async_persistence_ = (async_flag &&
                         (strcmp(async_flag, "1") == 0 ||
                          strcmp(async_flag, "true") == 0));

    Log_info("[RAFT-PERSISTENCE] Initializing LogStorage for site {} partition {} (mode={})",
             site_id_, partition_id_, async_persistence_ ? "async" : "sync");

    // Create RecoveryConfig
    raft::RecoveryConfig config;
    std::string base_path = "/tmp";
    const char* custom_path = std::getenv("MAKO_RAFT_PERSISTENCE_PATH");
    if (custom_path && custom_path[0] != '\0') {
      base_path = custom_path;
    }
    config.storage_path = base_path + "/raft_" + std::to_string(site_id_) +
                         "_partition_" + std::to_string(partition_id_);

    // Create RecoveryManager and storage
    raft::RecoveryManager manager(config);
    auto storage = manager.create_storage();

    if (!storage) {
      Log_error("[RAFT-PERSISTENCE] Failed to create LogStorage - continuing without persistence");
    } else {
      // Use RecoveryManager to orchestrate recovery
      auto result = manager.recover(
        [this](std::shared_ptr<janus::raft::LogStorage> s) { SetLogStorage(s); },
        [this]() { return RecoverFromStorage(); },
        [this](raft::RecoveryResult& r) {
          r.recovered_term = currentTerm;
          r.recovered_entries = raft_logs_.size();
        }
      );

      if (result.success) {
        Log_info("[RAFT-PERSISTENCE] Recovery complete: mode={} term={} entries={} time={}ms",
                 static_cast<int>(result.mode), result.recovered_term,
                 result.recovered_entries, result.recovery_time_ms);
      } else {
        Log_error("[RAFT-PERSISTENCE] Recovery failed: {}", result.error_message.c_str());
      }
    }
  } else {
    Log_info("[RAFT-PERSISTENCE] Disabled (set MAKO_RAFT_PERSISTENCE=1 to enable)");
  }

  // ========== HEARTBEAT INTERVAL (runtime override) ==========
  // @unsafe { std::getenv and Log_info are not borrow-checked }
  {
    const char* hb_str = std::getenv("MAKO_RAFT_HEARTBEAT_INTERVAL_US");
    if (hb_str && hb_str[0] != '\0') {
      heartbeat_interval_us_ = std::stoull(hb_str);
      Log_info("[RAFT] Heartbeat interval set to {} us from env", heartbeat_interval_us_);
    }
  }

  // ========== LOG RETENTION WINDOW (runtime override) ==========
  // @unsafe { std::getenv and Log_info are not borrow-checked }
  {
    const char* lrw_str = std::getenv("MAKO_RAFT_LOG_RETENTION_WINDOW");
    if (lrw_str && lrw_str[0] != '\0') {
      uint64_t val = std::stoull(lrw_str);
      log_retention_window_ = (val > 0) ? val : 1;
      Log_info("[RAFT] Log retention window set to {} from env", log_retention_window_);
    }
  }

  // ========== INITIALIZE SNAPSHOT MANAGER ==========
  InitializeSnapshotManager();

  // ========== INITIALIZE REPLICATED DB (optional) ==========
  // @unsafe { std::getenv, std::make_shared, RegLearnerAction, Log_info }
  {
    const char* rdb_flag = std::getenv("MAKO_REPLICATED_DB");
    if (rdb_flag && (strcmp(rdb_flag, "1") == 0 || strcmp(rdb_flag, "true") == 0)) {
      std::string db_path;
      const char* custom_path = std::getenv("MAKO_REPLICATED_DB_PATH");
      if (custom_path && custom_path[0] != '\0') {
        db_path = std::string(custom_path) + "/replicated_db_" + std::to_string(site_id_);
      } else {
        db_path = "/tmp/mako_replicated_db_" + std::to_string(site_id_);
      }
      replicated_db_ = std::make_shared<ReplicatedDB>(this, db_path);

      // Register apply callback so committed Raft entries are applied to RocksDB
      RegLearnerAction([this](int slot, janus::Command md) -> int {
        if (replicated_db_) {
          replicated_db_->ApplyEntry(slot, md);
        }
        return 0;
      });

      Log_info("[RAFT-REPLICATED-DB] Initialized for site {} at path {}",
               site_id_, db_path.c_str());
    }
  }

  // ========== INITIALIZE MEMBERSHIP CONFIGURATION ==========
  // Populate current_config_ from the static partition configuration.
  // This gives us the initial set of replicas; AddServer/RemoveServer will
  // modify it dynamically at runtime.
  {
    auto config = Config::GetConfig();
    auto replicas = config->SitesByPartitionId(partition_id_);
    for (auto& site : replicas) {
      current_config_.insert(site.id);
    }
    Log_info("[RAFT-CONFIG] Initialized current_config_ for site {} partition {} with {} replicas",
             site_id_, partition_id_, current_config_.size());
  }

#ifdef RAFT_TEST_CORO
  if (heartbeat_) {
		Log_debug("starting heartbeat loop at site {}", site_id_);
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
		Log_debug("starting heartbeat loop at site {}", site_id_);
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

  // Start apply infrastructure.
  StartApplyFiber();
  if (commitIndex > executeIndex) {
    EnqueueCommittedEntries(executeIndex, commitIndex);
  }
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
    // Clear any stale proxy data from previous Kill/Restart cycle
    // This can happen when a server is killed, restarted (with new proxies),
    // and then killed again - the old _proxies data was never cleared
    if (_proxies[partition_id_][loc_id_].size() > 0) {
      Log_info("[DISCONNECT] Clearing stale proxy data for partition {}, site {} (had {} entries)",
               partition_id_, loc_id_, _proxies[partition_id_][loc_id_].size());
      _proxies[partition_id_][loc_id_].clear();
    }
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

// @safe - read-only leader hint lookup
siteid_t RaftServer::GetLeaderHint() const {
  // Note: mtx_ is recursive_mutex, but this is const - callers should hold lock
  // or accept a slightly stale value. current_leader_id_ is set under lock in
  // setIsLeader() and OnAppendEntries(), so reads are safe for hint purposes.
  if (is_leader_) {
    return site_id_;
  }
  return current_leader_id_;
}

// @unsafe - Leadership state transition (callbacks and logging wrapped in @unsafe blocks)
void RaftServer::setIsLeader(bool isLeader) {
  bool prev_is_leader = is_leader_;
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_STATE] setIsLeader invoked site {} (loc {}) term {}: prev_is_leader={} new_is_leader={}",
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
      verify(c != nullptr);
      proxies = c->rpc_par_proxies_[partition_id_];
      }
      if(failover_) {
        for (auto& p : proxies) {
          if (p.first != site_id_) {
            // set matchIndex = 0
            match_index_[p.first] = 0;
            // set nextIndex = lastLogIndex + 1
            next_index_[p.first] = lastLogIndex + 1;
            Log_debug("loc_id_={} match_index_[{}]={}, next_index_[{}]={}", loc_id_, p.first, match_index_[p.first], p.first, next_index_[p.first]);
          }
        }
        // matchedIndex and nextIndex should have indices for all servers + learners except self
        verify(match_index_.size() == current_config_.size() + learners_.size() - 1);
        verify(next_index_.size() == current_config_.size() + learners_.size() - 1);
      }
    }
  }


  // This 2 lines MUST put BEFORE is_leader_ = isLeader ! otherwise they will become 0, and new view will without leader
  bool become_new_leader = isLeader && (!is_leader_);
  bool become_new_follower = (!isLeader) && is_leader_;

  // Update the leader state after view handling
  is_leader_ = isLeader;

  // Track current leader identity for GetLeaderHint()
  if (isLeader) {
    current_leader_id_ = site_id_;
  }

  // Only log on actual transitions, not no-op calls
  if (become_new_leader || become_new_follower) {
    Log_info("RaftServer::setIsLeader site_id_ {} become_new_leader {} become_new_follower {} isLeader {}", site_id_, become_new_leader, become_new_follower, isLeader);
  }

  // Only update view when transitioning from non-leader to leader
  if (become_new_leader) {
    Log_info("[RAFT_STATE] setIsLeader transition LEADER: site {} term {} prev_is_leader={} become_new_leader={}",
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
      n_replicas = static_cast<int>(current_config_.size());
      }
      new_view_ = View(n_replicas, site_id_, currentTerm);
      Log_info("[RAFT_VIEW] Server {} became leader for partition {}, term={}, old_view={}, new_view={}", 
               site_id_, partition_id_, currentTerm, 
               old_view_.ToString().c_str(), new_view_.ToString().c_str());
      
      // IMPORTANT: Update the communicator's view so it knows this server is the leader
      if (commo_) {
        auto view_data = std::make_shared<ViewData>(new_view_, partition_id_);
        // @unsafe
        { commo()->UpdatePartitionView(partition_id_, *view_data); }
        Log_info("[RAFT_VIEW] Updated communicator view for partition {} with new leader {}",
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
      Log_info("[LEADERSHIP-TRANSFER] Site {}: Became non-preferred leader, starting transfer monitoring",
               site_id_);
      StartLeadershipTransferMonitoring();
    }
  } else if (become_new_follower) {
    Log_info("[RAFT_STATE] setIsLeader transition FOLLOWER: site {} term {} prev_is_leader={} become_new_follower={}",
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
    Log_info("[RAFT_TIMER] Site {} reset election timer when becoming follower (last_hb now={})",
             site_id_, last_heartbeat_time_);

    // When transitioning from leader to non-leader
    Log_info("[RAFT_VIEW] Server {} stepping down as leader for partition {}", site_id_, partition_id_);

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
      Log_info("[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(true) - became leader", site_id_);
      leader_change_cb_(true);
    } else if (become_new_follower) {
      Log_info("[LEADER_CALLBACK] Site {}: Firing leader_change_cb_(false) - became follower", site_id_);
      leader_change_cb_(false);
    }
    }
  }
}

// @unsafe - ReadIndex protocol for linearizable reads
// Confirms this server is leader and that executeIndex >= commitIndex,
// meaning all committed entries have been applied to the state machine.
bool RaftServer::ReadIndex(uint64_t timeout_us) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  if (!IsLeader()) {
    Log_debug("[READ-INDEX] site={} not leader, rejecting read", site_id_);
    return false;
  }

  uint64_t read_index = commitIndex;

  // Wait for executeIndex to catch up to commitIndex
  // (entries may be committed but not yet applied to state machine)
  if (executeIndex >= read_index) {
    Log_debug("[READ-INDEX] site={} executeIndex={} >= readIndex={}, serving read",
              site_id_, executeIndex, read_index);
    return true;
  }

  // Need to wait for apply to catch up - release lock while sleeping
  uint64_t waited = 0;
  uint64_t wait_step = 100;  // 100 microseconds
  while (executeIndex < read_index) {
    if (timeout_us > 0 && waited >= timeout_us) {
      Log_warn("[READ-INDEX] site={} timed out waiting for executeIndex={} to reach readIndex={}",
               site_id_, executeIndex, read_index);
      return false;
    }
    // @unsafe { release lock, sleep, re-acquire }
    mtx_.unlock();
    usleep(wait_step);
    waited += wait_step;
    mtx_.lock();
    if (!IsLeader()) {
      Log_debug("[READ-INDEX] site={} lost leadership while waiting", site_id_);
      return false;
    }
  }

  Log_debug("[READ-INDEX] site={} executeIndex={} caught up to readIndex={} after {} us",
            site_id_, executeIndex, read_index, waited);
  return true;
}

// @unsafe - Applies committed logs (callbacks wrapped in @unsafe blocks)
void RaftServer::applyLogs() {
  // Log commit state for debugging
  Log_info("[APPLY-LOGS] site={} commitIndex={} executeIndex={}",
           site_id_, commitIndex, executeIndex);

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
      if (next_instance && next_instance->log_.has_value()) {
        // @unsafe
        {
        // RuleWitnessGC takes shared_ptr<Marshallable>;
        // app_next_ takes Command — Command's auto-conversion +
        // explicit unwrap meet at the boundary.
        RuleWitnessGC(next_instance->log_);
        Log_info("[APPLY-LOGS] site={} applying index={}", site_id_, id);
        app_next_(id, next_instance->log_);  // Pass both id and log (signature requires 2 args)
        executeIndex = id;
        }
      } else {
        Log_info("[APPLY-LOGS] site={} SKIP index={} (no instance or log)", site_id_, id);
        break;
      }
    }

    // Check if new work arrived while we were applying
    // If so, loop again to process it
  } while (apply_pending_.load(std::memory_order_acquire));

  in_applying_logs_ = false;

  // Check if we should take a snapshot
  if (snapshot_manager_ && snapidx_ < executeIndex &&
      (executeIndex - snapidx_) > snapshot_threshold_) {
    CreateSnapshot();
  }

  // Cleanup old commands to prevent memory buildup.
  slotid_t cutoff = (executeIndex > log_retention_window_) ? executeIndex - log_retention_window_ : 0;
  // Coordinate with snapshots: don't compact beyond what the latest snapshot covers
  if (snapidx_ > 0 && cutoff > snapidx_) {
    cutoff = snapidx_;
  }
  while (min_active_slot_ < cutoff) {
    removeCmd(min_active_slot_);
    min_active_slot_++;
  }
}

// @unsafe - external calls marked @external [safe], core replication loop
// TODO: Revisit borrow checker errors in this function.
// The checker reports "use after move" for loop-local variables (matchedIndices,
// batch_buffer_, batch_cmd, cmd) due to 2-iteration loop simulation. These variables
// are declared fresh each iteration, but the checker may not be resetting state
// correctly for loop-local declarations. Additionally, SendAppendEntries2 takes
// shared_ptr<Marshallable> by value (moves), which compounds the issue.
// Potential fixes: (1) Change SendAppendEntries2 to take const shared_ptr&,
// (2) Investigate checker's loop-local variable handling.
// ============================================================================
// PARALLEL HEARTBEAT FIX
// ============================================================================
// This struct holds context for each pending AppendEntries RPC.
// Used to send RPCs in parallel and process responses without blocking.
// The response field uses shared_ptr to ensure memory validity when callback fires.
struct PendingAppendEntries {
  siteid_t follower_id;
  shared_ptr<AppendEntriesResponse> response;  // shared_ptr ensures callback memory safety
  // migrated from
  // `shared_ptr<Marshallable>` to `janus::Command`.  Empty Command
  // (has_value() == false) signals heartbeat.
  janus::Command cmd;
  uint64_t sent_term;  // term when RPC was sent
};

// @unsafe - Heartbeat loop mutates shared state, performs RPCs, and uses raw pointers.
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
    // matchedIndex and nextIndex should have indices for all servers + learners except self
    verify(match_index_.size() == current_config_.size() + learners_.size() - 1);
    verify(next_index_.size() == current_config_.size() + learners_.size() - 1);
  // }

  Log_debug("heartbeat loop init from site: {}", site_id_);
  looping_ = true;
  while(looping_) {
    uint64_t term = 0;
    {
      {
        std::lock_guard<std::recursive_mutex> lock(ready_for_replication_mtx_);
        ready_for_replication_ = Reactor::create_sp_event<IntEvent>();
        ready_for_replication_.as_ref().unwrap()->set(0);
      }
      ready_for_replication_.as_ref().unwrap()->wait_timeout(heartbeat_interval_us_);
      {
        std::lock_guard<std::recursive_mutex> lock(ready_for_replication_mtx_);
        ready_for_replication_ = rusty::None;
      }
      // Fiber::sleep(HEARTBEAT_INTERVAL);
      // Log_info("heartbeat loop at loc {}", loc_id_);
      if (!IsLeader()) {
        // Log_info("heartbeat loop at loc {} skip since not leader", loc_id_);
        continue;
      }

      auto nservers = current_config_.size();

      // ========================================================================
      // PHASE 0: Calculate commit index ONCE per heartbeat round (not per-follower)
      // ========================================================================
      uint64_t current_commit_index = 0;
      uint64_t current_last_log_index = 0;
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        std::vector<uint64_t> matchedIndices{};
        for (auto it = match_index_.begin(); it != match_index_.end(); it++) {
          // Exclude learners from quorum calculation
          if (learners_.count(it->first) > 0) continue;
          matchedIndices.push_back(it->second);
          Log_debug("[COMMIT-CALC] match_index_[{}] = {}", it->first, it->second);
        }
        Log_debug("[COMMIT-CALC] nservers={}, matchedIndices.size()={}", nservers, matchedIndices.size());
        verify(matchedIndices.size() == nservers - 1);
        std::sort(matchedIndices.begin(), matchedIndices.end());
        uint64_t newCommitIndex = matchedIndices[(nservers - 1) / 2];
        Log_debug("[COMMIT-CALC] newCommitIndex={} (median at index {}), currentCommitIndex={}", newCommitIndex, (nservers - 1) / 2, commitIndex);

        if (newCommitIndex > lastLogIndex) {
          newCommitIndex = lastLogIndex;
        }

        if (newCommitIndex > commitIndex && (GetRaftInstance(newCommitIndex)->term == currentTerm)) {
          uint64_t old_commit = commitIndex;
          Log_debug("newCommitIndex {}", newCommitIndex);
          commitIndex = newCommitIndex;
          PersistCommitIndex(commitIndex, "HeartbeatLoop: leader commit");
          EnqueueCommittedEntries(old_commit, commitIndex);
        }
        term = currentTerm;
        current_commit_index = commitIndex;
        current_last_log_index = lastLogIndex;
      }

      // ========================================================================
      // PHASE 1: Send all AppendEntries RPCs in PARALLEL (non-blocking)
      // ========================================================================
      // Use unique_ptr to ensure stable memory addresses for the callback pointers.
      // The async RPC callback writes to ret_status/ret_term/ret_last_log_index,
      // so these must remain at fixed addresses until the RPC completes.
      std::vector<std::unique_ptr<PendingAppendEntries>> pending_rpcs;

      for (auto it = next_index_.begin(); it != next_index_.end(); it++) {
        auto site_id = it->first;
        if (site_id == site_id_) {
          continue;
        }
        if (!IsLeader()) {
          break;  // Stop sending if we lost leadership
        }

        uint64_t prevLogIndex = 0;
        uint64_t prevLogTerm = 0;
        // migrated from
        // `shared_ptr<Marshallable> cmd = nullptr` to `janus::Command{}`.
        // Empty Command (has_value() == false) signals heartbeat.
        janus::Command cmd{};
        uint64_t cmdLogTerm = 0;
        bool skip_follower = false;
        {
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          prevLogIndex = it->second - 1;
          if (prevLogIndex > lastLogIndex) {
            Log_info("[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > lastLogIndex ({}), fixing next_index", prevLogIndex, lastLogIndex);
            it->second = lastLogIndex + 1;
            prevLogIndex = it->second - 1;
          }

          if (prevLogIndex > lastLogIndex) {
            Log_info("[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > lastLogIndex ({}), skipping",
                     site_id, prevLogIndex, lastLogIndex);
            it->second = 1;
            skip_follower = true;
          } else if (it->second < min_active_slot_ && snapshot_manager_) {
            // @unsafe - Follower is too far behind (log compacted), send InstallSnapshot
            Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < min_active_slot_={}, sending InstallSnapshot",
                     site_id_, site_id, it->second, min_active_slot_);
            janus::raft::SnapshotMetadata snap_meta;
            std::string snap_data;
            if (snapshot_manager_->LoadLatestSnapshot(&snap_meta, &snap_data)) {
              uint64_t snap_last_idx = snap_meta.last_included_index;
              uint64_t snap_last_term = snap_meta.last_included_term;
              uint64_t send_term = currentTerm;
              commo()->SendInstallSnapshot(
                  site_id, partition_id_,
                  send_term, site_id_,
                  snap_last_idx, snap_last_term,
                  snap_data,
                  [this, site_id, snap_last_idx, send_term](uint64_t follower_term) {
                    // @unsafe - callback modifies shared state under lock
                    std::lock_guard<std::recursive_mutex> lock(mtx_);
                    if (follower_term > currentTerm) {
                      Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Follower {} has higher term {} > {}, stepping down",
                               site_id_, site_id, follower_term, currentTerm);
                      currentTerm = follower_term;
                      stepDown(StepDownReason::HigherTerm);
                      return;
                    }
                    if (currentTerm != send_term) {
                      Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Term changed since snapshot send, ignoring response",
                               site_id_);
                      return;
                    }
                    next_index_[site_id] = snap_last_idx + 1;
                    match_index_[site_id] = snap_last_idx;
                    Log_info("[HEARTBEAT-SNAPSHOT] Site {}: Updated follower {}: next_index={} match_index={}",
                             site_id_, site_id, snap_last_idx + 1, snap_last_idx);
                  });
              skip_follower = true;  // Skip normal AppendEntries for this follower
            } else {
              Log_warn("[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                       site_id_, site_id);
              skip_follower = true;
            }
          } else {
            verify(prevLogIndex <= lastLogIndex);
            if (prevLogIndex == 0) {
              prevLogTerm = 0;
            } else if (prevLogIndex == snapidx_ && snapidx_ > 0) {
              // Keep using snapshot boundary metadata after compaction.
              prevLogTerm = snapterm_;
            } else {
              auto instance = GetRaftInstance(prevLogIndex);
              if (!instance) {
                Log_error("[HEARTBEAT-SEND] [CRITICAL] GetRaftInstance({}) returned NULL! Skipping follower {}",
                          prevLogIndex, site_id);
                skip_follower = true;
              } else {
                prevLogTerm = instance->term;
              }
            }

            if (!skip_follower) {
#ifndef RAFT_BATCH_OPTIMIZATION
              Log_debug("[BATCH_CHECK] site={} follower={} next_index={} min_active_slot_={} lastLogIndex={}",
                       site_id_, site_id, it->second, min_active_slot_, lastLogIndex);
              if (it->second <= lastLogIndex) {
                auto curInstance = GetRaftInstance(it->second);
                if (!curInstance) {
                  Log_error("[HEARTBEAT-SEND] GetRaftInstance({}) returned NULL, skipping", it->second);
                } else {
                  // cmd is Command; assign directly from
                  // curInstance->log_ (also Command).
                  cmd = curInstance->log_;
                  cmdLogTerm = curInstance->term;
                  // 2 step 1: debug log no longer needs the
                  // inner shared_ptr's raw pointer; the kind tag is
                  // a more useful identifier anyway.
                  Log_debug("[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                      site_id_, it->second, site_id, cmd.kind_);
                }
              }
#endif

#ifdef RAFT_BATCH_OPTIMIZATION
              vector<rusty::Arc<TpcCommitCommand>> batch_buffer_;
              const uint64_t max_batch_entries = GetAppendEntriesBatchMaxEntries();
              const uint64_t batch_start_idx = std::max<uint64_t>(it->second, min_active_slot_);
              Log_debug("[BATCH_CHECK] site={} follower={} next_index={} min_active_slot_={} lastLogIndex={}",
                       site_id_, site_id, it->second, min_active_slot_, lastLogIndex);
              for (uint64_t idx = batch_start_idx;
                   idx <= lastLogIndex && batch_buffer_.size() < max_batch_entries;
                   idx++) {
                auto curInstance = GetRaftInstance(idx);
                if (!curInstance) {
                  Log_error("[HEARTBEAT-BATCH] GetRaftInstance({}) returned NULL, skipping", idx);
                  continue;
                }
                // curInstance->log_ is Command; the
                // `marshallable_cast<T>(SerializableEnvelope&)`
                // overload (in serializable_envelope.hpp) handles
                // this directly.
                auto curCmd = marshallable_cast<TpcCommitCommand>(curInstance->log_);
                if (curCmd.is_none()) {
                  Log_info("[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log",
                           site_id_, idx, curInstance->log_.has_value() ? curInstance->log_.kind_ : -1);
                  cmd = curInstance->log_;
                  break;
                }
                // @unsafe { sanctioned writeback through the shared payload — see server_atomic_* precedent }
                { auto& mut_cmd = *const_cast<TpcCommitCommand*>(curCmd.as_ref().unwrap().get()); mut_cmd.term = curInstance->term; }
                batch_buffer_.push_back(curCmd.unwrap());
              }
              if (batch_buffer_.size() > 0) {
                // Fill-then-wrap: assemble locally, wrap once complete.
                TpcBatchCommand batch_local;
                batch_local.AddCmds(batch_buffer_);
                auto batch_cmd =
                    rusty::Arc<TpcBatchCommand>::make(std::move(batch_local));
                cmd = std::move(batch_cmd);
                const uint64_t batch_end_idx = batch_start_idx + batch_buffer_.size() - 1;
                const bool truncated = batch_end_idx < lastLogIndex;
                Log_info("[BATCH_SEND] site={} sending batch of {} entries to follower {} "
                         "(from={} to={}{})",
                         site_id_, batch_buffer_.size(), site_id,
                         batch_start_idx, batch_end_idx, truncated ? ", truncated" : "");
              }
#endif
            }
          }
        }
        if (skip_follower) {
          continue;
        }

        // Create pending RPC context
        auto pending = std::make_unique<PendingAppendEntries>();
        pending->follower_id = site_id;
        pending->cmd = cmd;
        pending->sent_term = term;

        // Send RPC (non-blocking - just initiates the async call)
        // Response is allocated with shared_ptr - callback captures it to ensure memory validity
        pending->response = commo()->SendAppendEntries2(site_id,
                                              partition_id,
                                              -1,
                                              -1,
                                              IsLeader(),
                                              site_id_,
                                              term,
                                              prevLogIndex,
                                              prevLogTerm,
                                              current_commit_index,
                                              cmd,
                                              cmdLogTerm);

        pending_rpcs.push_back(std::move(pending));
      }

      // ========================================================================
      // PHASE 2: Wait for responses with SHORT timeout and process them
      // ========================================================================
      // Use a shorter per-RPC timeout (100ms) since we're processing in parallel.
      // Total round time is bounded by the slowest responder, not sum of all.
      const uint64_t PER_RPC_TIMEOUT = 100000;  // 100ms per RPC

      for (auto& pending_ptr : pending_rpcs) {
        if (!IsLeader()) {
          break;  // Stop processing if we lost leadership
        }

        auto& pending = *pending_ptr;  // Dereference unique_ptr for cleaner access
        auto& resp = *pending.response;  // Access response data

        resp.event.as_ref().unwrap()->wait_timeout(PER_RPC_TIMEOUT);

        if (resp.event.as_ref().unwrap()->status_.get() == EventStatus::TIMEOUT) {
          Log_debug("[PARALLEL-HB] Timeout waiting for follower {}", pending.follower_id);
          continue;  // Skip this follower, try again next round
        }

        bool stepped_down = false;
        {
          std::lock_guard<std::recursive_mutex> lock(mtx_);
          auto& next_index = next_index_[pending.follower_id];
          auto& match_index = match_index_[pending.follower_id];

          if (resp.status == false && resp.term == 0 && resp.last_log_index == 0) {
            // RPC failed or no response - do nothing
          } else if (currentTerm > pending.sent_term) {
            // Stale response from old term - ignore
          } else if (resp.status == 0 && resp.term > pending.sent_term) {
            // case 1: AppendEntries rejected because leader's term is expired
            if (currentTerm == pending.sent_term) {
              Log_info("[STEPDOWN] Site {}: Stepping down due to higher term from follower {} (my_term={}, follower_term={})",
                       site_id_, pending.follower_id, pending.sent_term, resp.term);
              currentTerm = resp.term;
              stepDown(StepDownReason::HigherTerm);
              stepped_down = true;
            }
          } else if (resp.status == 0) {
            // case 2: AppendEntries rejected - log inconsistency
            if (resp.last_log_index > 0 && (resp.last_log_index + 1) < next_index) {
              uint64_t old_next = next_index;
              next_index = resp.last_log_index + 1;
              Log_info("[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})",
                       site_id_, pending.follower_id, old_next, next_index, old_next - next_index, resp.last_log_index);
            } else if (resp.last_log_index > 0 && (resp.last_log_index + 1) == next_index && next_index > 1) {
              // Follower has prevLogIndex but still rejected, which indicates a term conflict.
              // Step one slot further back so the next AppendEntries can overwrite conflict.
              uint64_t old_next = next_index;
              next_index--;
              Log_info("[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}",
                       site_id_, pending.follower_id, old_next, next_index);
            } else if (next_index > 10) {
              uint64_t old_next = next_index;
              next_index = next_index / 2;
              Log_info("[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)",
                       site_id_, pending.follower_id, old_next, next_index);
            } else if (next_index > 1) {
              next_index--;
              Log_debug("[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}",
                        site_id_, pending.follower_id, next_index + 1, next_index);
            } else {
              next_index = 1;
            }
          } else {
            // case 3: AppendEntries accepted
            verify(resp.status == true);

            // ==================================================================
            // SPECULATIVE REPLICATION: Track memory acks
            // ack_type=0 means Memory ack (immediate response before fsync)
            // ack_type=1 means Durable ack (handled via AppendEntriesDurable RPC)
            // ==================================================================
            if (resp.ack_type == 0) {  // Memory ack
              // Add follower to memoryAcks for all indices up to last_log_index
              for (uint64_t idx = 1; idx <= resp.last_log_index; ++idx) {
                memoryAcks_[idx].insert(pending.follower_id);
              }
              Log_debug("[SPEC-RAFT] Memory ack from follower {} for index {}",
                        pending.follower_id, resp.last_log_index);
            }

            if (!pending.cmd.has_value()) {
              Log_debug("case 3A: AppendEntries accepted for heartbeat msg");
              if (resp.last_log_index > match_index) {
                match_index = resp.last_log_index;
                if (match_index > lastLogIndex) {
                  match_index = lastLogIndex;
                }
                Log_debug("heartbeat updated match_index for site {}: match_index={}", pending.follower_id, match_index);
              }
              if (resp.last_log_index >= next_index) {
                if (next_index <= lastLogIndex) {
                  next_index++;
                  Log_debug("empty heartbeat incrementing next_index for site: {}, next_index: {}", pending.follower_id, next_index);
                }
              }
            } else {
              Log_debug("case 3B: AppendEntries accepted for non-empty msg");
              if (resp.last_log_index < next_index) {
                next_index = resp.last_log_index + 1;
                match_index = resp.last_log_index;
              } else {
                Log_debug("loc {} followerLastLogIndex={} followerNextIndex={} followerMatchedIndex={}",
                    pending.follower_id, resp.last_log_index, next_index, match_index);
#ifndef RAFT_BATCH_OPTIMIZATION
                match_index = next_index;
                next_index++;
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
                match_index = resp.last_log_index;
                next_index = resp.last_log_index + 1;
#endif
                if (match_index > lastLogIndex) {
                  match_index = lastLogIndex;
                }
                Log_debug("leader site {} receiving site {} followerLastLogIndex={} followerNextIndex={} followerMatchedIndex={}",
                    site_id_, pending.follower_id, resp.last_log_index, next_index, match_index);
              }
            }
          }
        }
        if (stepped_down) {
          break;  // Stop processing - we're no longer leader
        }
      }

      // ========================================================================
      // PHASE 3: Recalculate commit index after all responses processed
      // ========================================================================
      // This ensures commits happen promptly after replication, matching the
      // original behavior where commit index was recalculated after each response.
      if (IsLeader()) {
        std::lock_guard<std::recursive_mutex> lock(mtx_);
        std::vector<uint64_t> finalMatchedIndices{};
        for (auto it = match_index_.begin(); it != match_index_.end(); it++) {
          // Exclude learners from quorum calculation
          if (learners_.count(it->first) > 0) continue;
          finalMatchedIndices.push_back(it->second);
        }
        std::sort(finalMatchedIndices.begin(), finalMatchedIndices.end());
        uint64_t finalCommitIndex = finalMatchedIndices[(nservers - 1) / 2];
        if (finalCommitIndex > lastLogIndex) {
          finalCommitIndex = lastLogIndex;
        }
        if (finalCommitIndex > commitIndex && (GetRaftInstance(finalCommitIndex)->term == currentTerm)) {
          uint64_t old_commit = commitIndex;
          Log_debug("[PHASE3-COMMIT] Advancing commitIndex {} -> {}", commitIndex, finalCommitIndex);
          commitIndex = finalCommitIndex;
          PersistCommitIndex(commitIndex, "HeartbeatLoop: post-response commit");
          EnqueueCommittedEntries(old_commit, commitIndex);
        }

        // ==================================================================
        // LEARNER CATCH-UP: Check if any learners are caught up and promote
        // ==================================================================
        CheckAndPromoteLearners();

        // ==================================================================
        // SPECULATIVE REPLICATION: Update specCommitIndex based on memory acks
        // ==================================================================
        size_t quorum = GetQuorumSize();

        // Find the highest index with memory ack quorum
        // Leader's own entry counts as a memory ack
        uint64_t newSpecCommitIndex = specCommitIndex_;
        for (uint64_t idx = specCommitIndex_ + 1; idx <= lastLogIndex; ++idx) {
          // Check if we have quorum for this index
          auto it = memoryAcks_.find(idx);
          size_t ack_count = 0;
          if (it != memoryAcks_.end()) {
            ack_count = it->second.size();
          }
          // Leader's own log counts as an ack (we have the entry)
          ack_count += 1;  // +1 for leader's own entry

          if (ack_count >= quorum) {
            // Verify the entry is from current term
            auto instance = GetRaftInstance(idx);
            if (instance && instance->term == currentTerm) {
              newSpecCommitIndex = idx;
            }
          } else {
            // Stop at first index without quorum (monotonic advance)
            break;
          }
        }

        if (newSpecCommitIndex > specCommitIndex_) {
          uint64_t oldSpecCommitIndex = specCommitIndex_;
          Log_info("[SPEC-RAFT] Site {}: Advancing specCommitIndex {} -> {}",
                   site_id_, specCommitIndex_, newSpecCommitIndex);
          specCommitIndex_ = newSpecCommitIndex;

          // Persist updated speculative indices
          PersistSpeculativeIndicesToLogStorage();

          // Notify clients with SPECULATIVE status for newly committed entries
          if (lastSpecNotifiedIndex_ < newSpecCommitIndex) {
            uint64_t notifyFrom = std::max(lastSpecNotifiedIndex_, oldSpecCommitIndex);
            NotifyCallbacks(notifyFrom, newSpecCommitIndex, CommitStatus::SPECULATIVE);
            lastSpecNotifiedIndex_ = newSpecCommitIndex;
          }
        }

        // Verify invariants
        VerifySpeculativeInvariants();
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

  // Stop and join the background apply thread if it was started. The thread
  // captures `this` and walks apply_queue_ / app_next_, so it must finish
  // before any member state is destroyed.
  apply_thread_running_.store(false);
  if (apply_thread_.joinable()) {
    apply_thread_.join();
  }

  // Stop the HeartbeatLoop
  if (heartbeat_ && looping_) {
    Log_info("[SHUTDOWN] Stopping HeartbeatLoop for site={}", site_id_);
    looping_ = false;

    // Wake up the HeartbeatLoop if it's sleeping so it can see looping_=false
    if (ready_for_replication_.is_some()) {
      ready_for_replication_.as_ref().unwrap()->set(1);
    }
  }

  // Stop leadership transfer monitoring thread if running
  StopLeadershipTransferMonitoring();

  // Join all async persistence threads to prevent use-after-free.
  // These threads capture `this` for PersistState/PersistLogEntry/commo() calls.
  // They must complete before this object is destroyed.
  // NOTE: We swap the vector out under the lock, then join WITHOUT the lock.
  // This prevents deadlock if an in-flight RPC handler tries to emplace_back
  // a new thread while we're joining (it would block on async_threads_mtx_).
  {
    std::vector<std::pair<std::thread, rusty::Arc<AtomicFlag>>> threads_to_join;
    {
      std::lock_guard<std::mutex> lk(async_threads_mtx_);
      threads_to_join = std::move(async_threads_);
    }
    for (auto& [t, done_flag] : threads_to_join) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  // CRITICAL: Sleep briefly to allow detached coroutines to see stop_=true and exit
  // The election timer coroutine (StartElectionTimer) and leadership transfer coroutine
  // (InitiateLeadershipTransfer) are detached and check stop_ before calling RequestVote().
  // We need to give them time to notice stop_=true and return before the vtable collapses.
  // Without this sleep, there's a race where the coroutine wakes up, checks stop_=false,
  // then the destructor runs (vtable collapses), then the coroutine calls RequestVote()
  // through the base class vtable, hitting verify(0) and aborting.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  Log_info("site par {}, loc {}: prepare {}, accept {}, commit {}",
      partition_id_, loc_id_, n_prepare_, n_accept_, n_commit_);
}

// @unsafe - external calls marked @external [safe], mutex/pointer ops in @unsafe blocks
bool RaftServer::RequestVote() {
  // FIX 2: Prevent RequestVote during shutdown
  // The election timer coroutine may fire after ~RaftServer destructor runs,
  // causing a call to the base class TxLogServer::RequestVote() which hits verify(0)
  // Check stop_ flag to avoid this crash during teardown
  if (stop_) {
    Log_debug("[RAFT-SHUTDOWN] RequestVote called during shutdown (site={}), ignoring to prevent crash", site_id_);
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

    // CRITICAL: Persist term and vote BEFORE sending RequestVote RPCs
    PersistState(currentTerm, vote_for_, "RequestVote: starting election");

    LogTermChange("starting election", prev_local_term, currentTerm);
    // PersistState() already called above - no need for duplicate persistence
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
  Log_info("[RAFT_ELECTION] server {} (loc {}) starting election term {}->{} lastLogIdx={} lastLogTerm={} prev_vote_for={}",
           site_id_, loc_id, prev_term, term, lst_idx, lst_term, prev_vote_for);
#endif
  shared_ptr<RaftVoteQuorumEvent> sp_quorum;
  // @unsafe
  {
  sp_quorum = ((RaftCommo *)(this->commo_))->BroadcastVote(par_id,lst_idx,lst_term,loc_id, term );
  sp_quorum->wait_timeout(1000000);
  }
  std::lock_guard<std::recursive_mutex> lock1(mtx_);
#ifdef RAFT_LEADER_ELECTION_DEBUG
  Log_info("[RAFT_ELECTION] server {} term {} vote outcome yes={} no={} highest_term_seen={} timeout={}",
           site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get(), sp_quorum->Term(), sp_quorum->q().timeouted_.get());
#endif
  if (sp_quorum->yes()) {
    verify(currentTerm >= term);
    if (term != currentTerm) {
#ifdef RAFT_LEADER_ELECTION_DEBUG
      Log_info("[RAFT_ELECTION] server {} abandoning leadership claim because local term advanced to {}", site_id_, currentTerm);
#endif
      return false;
    }

    // =========================================================================
    // SPECULATIVE VOTING: Initialize specVoters from vote responses
    // =========================================================================
    // These are memory votes - not yet durable
    specVoters_ = sp_quorum->GetSpecVoters();
    specVoters_.insert(site_id_);  // Add self vote

    // Self vote is always durable (we persisted before broadcasting)
    durableVoters_.clear();
    durableVoters_.insert(site_id_);

    // Reset commit indices
    specCommitIndex_ = commitIndex;
    securedLogIndex_ = commitIndex;

    // Persist updated speculative indices
    PersistSpeculativeIndicesToLogStorage();

    // Clear ack tracking maps for new term
    memoryAcks_.clear();
    durableAcks_.clear();

    // Start as unsecured leader until we receive VoteDurable from quorum
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site {}: Won election term {} - specVoters={} durableVoters={}",
             site_id_, term, specVoters_.size(), durableVoters_.size());
    // =========================================================================

    // become a leader
    setIsLeader(true) ;
    // verify(currentTerm == term); // [Jetpack] Comment this since in failure recovery test this will fail after experiment end.
    Log_debug("site {} became leader for term {}", site_id_, term);

#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} won election term {} (votes yes={} no={})",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get());
#endif

    this->rep_frame_ = this->frame_ ;

    // auto co = ((TxLogServer *)(this))->CreateRepCoord(0);
    // auto empty_cmd = std::make_shared<TpcEmptyCommand>();
    // verify(TpcEmptyCommand::kMarshallKind == TpcEmptyCommand::static_kind());
    // auto sp_m = wrap_typed_marshallable(empty_cmd);
    // ((CoordinatorRaft*)co)->Submit(sp_m);
    
    if(IsLeader()) {
	  	//for(int i = 0; i < 100; i++) Log_info("wait wait wait");
      Log_debug("vote accepted {} curterm {}", loc_id, currentTerm);
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
      Log_debug("vote rejected {} curterm {}, do rollback", loc_id, currentTerm);
      setIsLeader(false) ;
    	return false;
		}
  } else if (sp_quorum->no()) {
    // become a follower
    Log_debug("site {} requestvote rejected", site_id_);
    setIsLeader(false) ;
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} lost election term {} (yes={} no={}) highest_term={}",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get(), sp_quorum->Term());
#endif
    //reset cur term if new term is higher
    ballot_t new_term = sp_quorum->Term() ;
    if (new_term > currentTerm) {
      auto prev_local_term = currentTerm;
      currentTerm = new_term;
      vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term

      // CRITICAL: Persist term after observing higher term from election responses
      PersistState(currentTerm, vote_for_, "RequestVote: observed higher term");

      LogTermChange("observed higher term from RequestVote replies", prev_local_term, currentTerm);
    }
  	req_voting_ = false ;
		return false;
  } else {
    Log_debug("vote timeout {}", loc_id);
#ifdef RAFT_LEADER_ELECTION_DEBUG
    Log_info("[RAFT_ELECTION] server {} election timed out term {} (yes={} no={})",
             site_id_, term, sp_quorum->q().n_voted_yes_.get(), sp_quorum->q().n_voted_no_.get());
#endif
  	req_voting_ = false ;
		return false;
  }
}

// @unsafe - calls @safe doVote, external calls marked @external [safe]
void RaftServer::OnRequestVote(const slotid_t& lst_log_idx,
                               const ballot_t& lst_log_term,
                               const siteid_t& can_id,
                               const ballot_t& can_term,
                               ballot_t *reply_term,
                               bool_t *vote_granted) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  Log_debug("raft receives vote from candidate: {:x}", can_id);

  uint64_t cur_term = currentTerm ;
  if( can_term < cur_term)
  {
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;
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
    Log_debug("site {} vote NO for {} (already voted for {} in term {})",
              site_id_, can_id, vote_for_, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;
    return ;
  }
  }

  // If we already voted for this same candidate in this term, vote YES again (idempotent)
  if( can_term == cur_term && vote_for_ == can_id )
  {
    Log_debug("site {} vote YES for {} (already voted for them in term {}, idempotent)",
              site_id_, can_id, cur_term);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true) ;
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

  Log_debug("vote for lstoff {}, curlstterm {}, curlstidx {}", lstoff, curlstterm, curlstidx  );


  // TODO del only for test
  verify(lstoff == lastLogIndex ) ;

  if( lst_log_term > curlstterm || (lst_log_term == curlstterm && lst_log_idx >= curlstidx) )
  {
    Log_debug("site {} vote for request vote from {}, lastidx {}, lastterm {}", site_id_, can_id, curlstidx, curlstterm);
    doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, true) ;
    return ;
  }

  doVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted, false) ;

}

// ============================================================================
// VoteDurable RPC Handler - Speculative Voting Protocol
// ============================================================================

void RaftServer::OnVoteDurable(const ballot_t& term,
                                const siteid_t& voter_id,
                                bool_t* acknowledged) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Reject stale votes from old terms
  if (term != currentTerm) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring VoteDurable from {} - term mismatch (got {}, current {})",
              site_id_, voter_id, term, currentTerm);
    *acknowledged = false;
    return;
  }

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring VoteDurable from {} - not leader",
              site_id_, voter_id);
    *acknowledged = false;
    return;
  }

  // Add voter to durable voters set
  durableVoters_.insert(voter_id);
  *acknowledged = true;

  Log_info("[SPEC-RAFT] Site {}: Received VoteDurable from {} - durableVoters size={}",
           site_id_, voter_id, durableVoters_.size());

  // Check if we've achieved secured leader status
  size_t quorum = GetQuorumSize();
  if (!securedLeader_ && durableVoters_.size() >= quorum) {
    securedLeader_ = true;
    Log_info("[SPEC-RAFT] Site {}: Became SECURED leader with {} durable votes (quorum={})",
             site_id_, durableVoters_.size(), quorum);
  }
}

// ============================================================================
// AppendEntriesDurable RPC Handler - Speculative Commit Protocol
// ============================================================================

void RaftServer::OnAppendEntriesDurable(const ballot_t& term,
                                         const siteid_t& follower_id,
                                         const uint64_t& lastLogIndex,
                                         bool_t* acknowledged) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Reject stale acks from old terms
  if (term != currentTerm) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring AppendEntriesDurable from {} - term mismatch (got {}, current {})",
              site_id_, follower_id, term, currentTerm);
    *acknowledged = false;
    return;
  }

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring AppendEntriesDurable from {} - not leader",
              site_id_, follower_id);
    *acknowledged = false;
    return;
  }

  // Add follower to durable acks for all indices up to lastLogIndex
  // We track this for all indices since the follower has durably persisted everything up to lastLogIndex
  for (uint64_t idx = 1; idx <= lastLogIndex; ++idx) {
    durableAcks_[idx].insert(follower_id);
  }
  *acknowledged = true;

  Log_info("[SPEC-RAFT] Site {}: Received AppendEntriesDurable from {} for index={}",
           site_id_, follower_id, lastLogIndex);

  // Check if we can advance securedLogIndex
  // Only if we're a secured leader (have durable vote quorum)
  if (securedLeader_) {
    size_t quorum = GetQuorumSize();

    // Find the highest index with durable ack quorum
    uint64_t newSecuredIndex = securedLogIndex_;
    for (uint64_t idx = securedLogIndex_ + 1; idx <= lastLogIndex && idx <= specCommitIndex_; ++idx) {
      auto it = durableAcks_.find(idx);
      if (it != durableAcks_.end() && it->second.size() >= quorum) {
        newSecuredIndex = idx;
      } else {
        // Stop at first index without quorum (monotonic advance)
        break;
      }
    }

    if (newSecuredIndex > securedLogIndex_) {
      uint64_t oldSecuredLogIndex = securedLogIndex_;
      Log_info("[SPEC-RAFT] Site {}: Advancing securedLogIndex {} -> {}",
               site_id_, securedLogIndex_, newSecuredIndex);
      securedLogIndex_ = newSecuredIndex;

      // Persist updated speculative indices
      PersistSpeculativeIndicesToLogStorage();

      // Notify clients with DURABLE status for newly secured entries
      if (lastDurableNotifiedIndex_ < newSecuredIndex) {
        uint64_t notifyFrom = std::max(lastDurableNotifiedIndex_, oldSecuredLogIndex);
        NotifyCallbacks(notifyFrom, newSecuredIndex, CommitStatus::DURABLE);
        lastDurableNotifiedIndex_ = newSecuredIndex;
      }
    }
  }

  // Verify invariants in debug mode
  VerifySpeculativeInvariants();
}

// @unsafe - Calls undeclared Fiber::create_run()
void RaftServer::StartElectionTimer() {
  // @unsafe
  { resetTimer("start election timer"); }
  last_heartbeat_time_ = Time::now(false);

  Fiber::create_run([this]() {
    Log_debug("start timer for election") ;

    while(!stop_) {
      // Use dynamic election timeout based on preferred replica role and grace period
      uint64_t election_timeout = GetElectionTimeout();

      // Sleep for a portion of the timeout before checking
      Fiber::sleep(RandomGenerator::rand(heartbeat_interval_us_ * 2, heartbeat_interval_us_ * 4));

      // Retry NotifyRestart for any PENDING peers
      // This handles the case where a peer was partitioned when we restarted
      auto c = commo();
      if (c != nullptr && c->HasPendingNotifyRestart()) {
        Log_debug("[NOTIFY-RESTART-RETRY] Site {}: retrying for pending peers", site_id_);
        c->RetryPendingNotifyRestart();
      }

      auto time_now = Time::now(false);
      auto time_elapsed = time_now - last_heartbeat_time_;

      // Only log when timeout actually fires or when debugging
      // Log_info("[ELECTION_TIMER] Site {}: checking - is_leader={} time_elapsed={} election_timeout={} last_hb_time={}",
      //          site_id_, IsLeader(), time_elapsed, election_timeout, last_heartbeat_time_);

      if (!IsLeader() && time_elapsed > election_timeout) {
        Log_info("[ELECTION_TIMER] Site {}: TIMEOUT FIRED - starting election (elapsed={} > timeout={})",
                 site_id_, time_elapsed, election_timeout);

        // ask to vote
        req_voting_ = true ;
        Log_info("[ELECTION_START] Site {}: TRIGGERING REQUESTVOTE - time_elapsed={} > timeout={} last_hb={} current_term={} vote_for={}",
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

// @unsafe - external calls marked @external [safe], pointer ops in @unsafe blocks
bool RaftServer::Start(const janus::Command& cmd,
                       uint64_t *index,
                       uint64_t *term,
                       slotid_t slot_id,
                       ballot_t ballot) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // #ifndef RAFT_TEST_CORO
  // if (!heartbeat_setup_) {
  //   heartbeat_setup_ = true;
  //   if (heartbeat_) {
  //     Log_debug("starting heartbeat loop at site {}", site_id_);
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
  Log_debug("Start(): ldr={} index={} term={}", loc_id_, *index, *term);
  }
  return true;
}

/* NOTE: same as ReceiveAppend */
/* NOTE: broadcast send to all of the host even to its own server
 * should we exclude the execution of this function for leader? */
// @unsafe - external calls marked @external [safe], output pointer writes in @unsafe blocks
void RaftServer::OnAppendEntries(const slotid_t slot_id,
                                 const ballot_t ballot,
                                 const uint64_t leaderCurrentTerm,
                                 const siteid_t leaderSiteId,
                                 const uint64_t leaderPrevLogIndex,
                                 const uint64_t leaderPrevLogTerm,
                                 const uint64_t leaderCommitIndex,
                                 const janus::Command& cmd,
                                 const uint64_t leaderNextLogTerm, // disabled in batched version (term recorded in the TpcCommitCommand)
                                 uint64_t *followerAppendOK,
                                 uint64_t *followerCurrentTerm,
                                 uint64_t *followerLastLogIndex,
                                 bool trigger_election_now) {
  std::unique_lock<std::recursive_mutex> lock(mtx_);

  bool term_ok = (leaderCurrentTerm >= this->currentTerm);
  const bool compacted_prefix_miss =
      (leaderPrevLogIndex != 0 &&
       leaderPrevLogIndex < min_active_slot_ &&
       leaderPrevLogIndex != snapidx_);
  bool index_ok = (leaderPrevLogIndex <= this->lastLogIndex) && !compacted_prefix_miss;
  uint64_t local_prev_term = 0;
  if (leaderPrevLogIndex == 0) {
      local_prev_term = 0;
  } else if (leaderPrevLogIndex == snapidx_) {
      // Snapshot boundary is still valid even when log entries are compacted.
      local_prev_term = snapterm_;
  } else if (leaderPrevLogIndex <= this->lastLogIndex && !compacted_prefix_miss) {
      auto prev_instance = GetRaftInstance(leaderPrevLogIndex);
      local_prev_term = prev_instance ? prev_instance->term : 0;
  }
  bool prev_term_ok = (leaderPrevLogIndex == 0 || local_prev_term == leaderPrevLogTerm);

  // Only log rejections or when cmd is present (actual log entries)
  if (!term_ok || !index_ok || !prev_term_ok || cmd.has_value()) {
  }

  // CRITICAL FIX: Reset timer if we hear from a current-term leader, even if log conflicts
  // This prevents followers with divergent logs from constantly starting elections
  // while the leader is trying to repair their log via backtracking
  if (term_ok) {
      // Track the leader's identity for GetLeaderHint()
      current_leader_id_ = leaderSiteId;
      // @unsafe
      { resetTimer("AppendEntries from current-term leader"); }
      if (leaderCurrentTerm > this->currentTerm) {
          auto prev_term = currentTerm;
          currentTerm = leaderCurrentTerm;
          vote_for_ = INVALID_SITEID;  // Reset vote when advancing to new term

          // CRITICAL: Persist term before accepting any entries from new leader
          PersistState(currentTerm, vote_for_, "OnAppendEntries: new leader term");

          LogTermChange("AppendEntries leader term is newer", prev_term, currentTerm, leaderSiteId);
          Log_debug("server {}, set to be follower", loc_id_ ) ;
          setIsLeader(false) ;
          // PersistState() already called above - no need for duplicate persistence
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
      //     Log_info("[RAFT_VIEW_FOLLOWER] Server {} observed leader change {}->{} term={} prev_term={}",
      //              site_id_, prev_leader, leaderSiteId, leaderCurrentTerm, currentTerm);
      // }

      // ==================================================================
      // SPECULATIVE REPLICATION: Append to memory, respond immediately,
      // then persist asynchronously and send AppendEntriesDurable
      // ==================================================================

      // Capture state for async persistence before modifying in-memory state
      std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>> entries_to_persist;
      uint64_t log_index_for_durable_ack = 0;

      if (cmd.has_value()) {
#ifndef RAFT_BATCH_OPTIMIZATION
        lastLogIndex = leaderPrevLogIndex + 1;
        auto instance = GetRaftInstance(lastLogIndex);
        instance->log_ = cmd;
        instance->term = leaderNextLogTerm;

        // Capture entry for async persistence
        entries_to_persist.push_back({lastLogIndex, instance});
        log_index_for_durable_ack = lastLogIndex;
#endif
#ifdef RAFT_BATCH_OPTIMIZATION
        const auto cmds = marshallable_cast<TpcBatchCommand>(cmd);
        verify(cmds.is_some());
        int cnt = 0;
        for (const rusty::Arc<TpcCommitCommand>& c: cmds.unwrap()->cmds_) {
          cnt++;
          lastLogIndex = leaderPrevLogIndex + cnt;
          auto instance = GetRaftInstance(lastLogIndex);
          instance->log_ = c.clone();
          instance->term = c->term;

          // Capture entry for async persistence
          entries_to_persist.push_back({lastLogIndex, instance});
        }
        log_index_for_durable_ack = lastLogIndex;  // Highest index in batch
#endif
      }

      // Advance commit index and enqueue committed entries for background apply.
      if (leaderCommitIndex > commitIndex) {
        auto old_commit = commitIndex;
        commitIndex = std::min(leaderCommitIndex, lastLogIndex);
        verify(lastLogIndex >= commitIndex);
        PersistCommitIndex(commitIndex, "OnAppendEntries: follower commit");
        EnqueueCommittedEntries(old_commit, commitIndex);
      }

      // @unsafe
      {
      *followerAppendOK = 1;
      *followerCurrentTerm = this->currentTerm;
      *followerLastLogIndex = this->lastLogIndex;
      }

      // Capture state needed for async persistence thread
      ballot_t term_copy = currentTerm;
      siteid_t follower_id_copy = site_id_;
      siteid_t leader_id_copy = leaderSiteId;
      parid_t par_id_copy = partition_id_;
      uint64_t commit_index_copy = commitIndex;

      // Release mutex before persistence work.
      lock.unlock();

      // ==================================================================
      // PERSISTENCE: Either async (speculative) or sync (traditional)
      // ==================================================================
      if (async_persistence_) {
        // ASYNC MODE (speculative): Start async persistence and send durable ack later
        if (!entries_to_persist.empty()) {
          // Capture entries by move to avoid dangling references
          // Track async persistence thread (joined in destructor to prevent UAF)
          {
            std::lock_guard<std::mutex> lk(async_threads_mtx_);
            // Prune completed threads to prevent unbounded accumulation
            async_threads_.erase(
              std::remove_if(async_threads_.begin(), async_threads_.end(),
                [](auto& entry) {
                  if (entry.second->get()) {
                    if (entry.first.joinable()) entry.first.join();
                    return true;
                  }
                  return false;
                }),
              async_threads_.end());
            auto done = rusty::Arc<AtomicFlag>::make(false);
            async_threads_.emplace_back(
              std::thread([this, entries = std::move(entries_to_persist),
                         log_index_for_durable_ack, term_copy, follower_id_copy,
                         leader_id_copy, par_id_copy, commit_index_copy, done]() {
              // Persist all log entries
              for (const auto& entry : entries) {
                PersistLogEntry(entry.first, *entry.second, "OnAppendEntries: async follower entry");
              }

              // Also persist commit index (async is fine for commit index)
              PersistCommitIndex(commit_index_copy, "OnAppendEntries: async follower commit");

              // Send AppendEntriesDurable RPC to leader
              auto c = commo();
              if (c != nullptr) {
                c->SendAppendEntriesDurable(leader_id_copy, par_id_copy, term_copy,
                                            follower_id_copy, log_index_for_durable_ack);
              }
              done->set(true);
            }), done);
          }
        }
      } else {
        // SYNC MODE (traditional): Persist entries synchronously before returning
        // No separate AppendEntriesDurable RPC needed - the ack implies durability
        if (!entries_to_persist.empty()) {
          for (const auto& entry : entries_to_persist) {
            PersistLogEntry(entry.first, *entry.second, "OnAppendEntries: sync follower entry");
          }
          PersistCommitIndex(commit_index_copy, "OnAppendEntries: sync follower commit");
        }
      }

      // Re-acquire mutex before returning (to handle remaining code safely)
      lock.lock();

#ifndef RAFT_TEST_CORO
      if (cmd.has_value()) {
        if (cmd.kind_ == TpcCommitCommand::static_kind()){
          const auto p_cmd = marshallable_cast<TpcCommitCommand>(cmd);
          const auto vec_piece_data = marshallable_cast<VecPieceData>(p_cmd.unwrap()->cmd_);
          verify(vec_piece_data.is_some());
          auto sp_vec_piece = vec_piece_data.unwrap()->sp_vec_piece_data_;
          
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
        Log_info("[APPEND_REJECT] Site {} rejecting AppendEntries from leader {} - term_ok={} index_ok={} prev_term_ok={} (leaderTerm={} myTerm={} prevIdx={} myLastIdx={} local_prev_term={})",
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
                Log_info("[PIGGYBACKED-TRANSFER] Site {} (preferred): Received transfer signal from leader {} - will start election after 30ms",
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
                Log_info("[PIGGYBACKED-TRANSFER] Site {} (preferred): Received transfer signal but already leader - ignoring",
                         site_id_);
            }
        } else {
            // I'm a NON-PREFERRED replica - just log and do nothing
            Log_info("[PIGGYBACKED-TRANSFER] Site {} (non-preferred): Received transfer signal (preferred={})",
                     site_id_, preferred_leader_site_id_);
        }
    }

    lock.unlock();
}

// @unsafe - Removes command from log (external calls wrapped in @unsafe blocks)
void RaftServer::removeCmd(slotid_t slot) {
  auto it = raft_logs_.find(slot);
  if (it == raft_logs_.end()) {
    return;
  }

  // Committed log replay and callback execution may overlap with log cleanup.
  // Only evict the log entry here; transaction destruction is handled elsewhere.
  raft_logs_.erase(it);
}

// @unsafe - Stores callback for later invocation
void RaftServer::RegisterLeaderChangeCallback(std::function<void(bool)> cb) {
  leader_change_cb_ = std::move(cb);
}

// @unsafe - external calls marked @external [safe], output pointer writes in @unsafe blocks
void RaftServer::OnTimeoutNow(const uint64_t leaderTerm,
                               const siteid_t leaderSiteId,
                               uint64_t *followerTerm,
                               bool_t *success) {
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
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow - server shutting down", site_id_);
    return;
  }

  // ============================================================================
  // Edge Case 1: Stale TimeoutNow from old term
  // ============================================================================
  if (leaderTerm < currentTerm) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring stale TimeoutNow from leader {} (leader_term={} < my_term={})",
             site_id_, leaderSiteId, leaderTerm, currentTerm);
    return;
  }

  // ============================================================================
  // Edge Case 1b: Leader is ahead of us - update term
  // ============================================================================
  if (leaderTerm > currentTerm) {
    Log_info("[TIMEOUT-NOW] Site {}: Leader {} has higher term ({} > {}) - updating term and stepping down",
             site_id_, leaderSiteId, leaderTerm, currentTerm);

    currentTerm = leaderTerm;
    // @unsafe
    {
    vote_for_ = INVALID_SITEID;  // Reset vote for new term
    }

    // CRITICAL: Persist term before responding to TimeoutNow
    PersistState(currentTerm, vote_for_, "OnTimeoutNow: leader higher term");

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
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow from leader {} - already leader in term {}",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already leader (goal achieved)
    return;
  }

  // ============================================================================
  // Edge Case 3: Currently candidate (already in election)
  // ============================================================================
  if (req_voting_) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow from leader {} - already requesting votes (term={})",
             site_id_, leaderSiteId, currentTerm);
    *success = true;  // Success = already trying to become leader
    return;
  }

  // ============================================================================
  // Edge Case 4: We're transferring leadership (stepping down)
  // ============================================================================
  if (transferring_leadership_) {
    Log_info("[TIMEOUT-NOW] Site {}: Ignoring TimeoutNow from leader {} - currently transferring leadership",
             site_id_, leaderSiteId);
    return;
  }

  // ============================================================================
  // Valid TimeoutNow - Start Election Immediately
  // ============================================================================
  Log_info("[TIMEOUT-NOW] *** Site {}: Received TimeoutNow from leader {} (term={}) - STARTING ELECTION IMMEDIATELY ***",
           site_id_, leaderSiteId, leaderTerm);

  // Start election immediately (bypass random timeout)
  // This will increment term and send RequestVote RPCs
  bool election_started = RequestVote();

  if (election_started) {
    *success = true;
    Log_info("[TIMEOUT-NOW] Site {}: Election started successfully (new_term={})",
             site_id_, currentTerm);
  } else {
    *success = false;
    Log_warn("[TIMEOUT-NOW] Site {}: Failed to start election",
             site_id_);
  }
}

// ============================================================================
// InstallSnapshot RPC Handler
// ============================================================================

// @unsafe - Modifies log state, snapshot metadata, calls snapshot_manager_
void RaftServer::OnInstallSnapshot(const uint64_t term,
                                    const uint64_t leader_id,
                                    const uint64_t last_included_index,
                                    const uint64_t last_included_term,
                                    const std::string& data,
                                    uint64_t* term_out) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // @unsafe
  { *term_out = currentTerm; }

  // ============================================================================
  // Edge Case 0: Server shutting down
  // ============================================================================
  if (stop_) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Ignoring InstallSnapshot - server shutting down", site_id_);
    return;
  }

  // ============================================================================
  // Edge Case 1: Stale term - reject
  // ============================================================================
  if (term < currentTerm) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Rejecting InstallSnapshot from leader {} "
             "(leader_term={} < my_term={})",
             site_id_, leader_id, term, currentTerm);
    return;
  }

  // ============================================================================
  // Edge Case 2: Higher or equal term - accept as legitimate leader
  // ============================================================================
  if (term > currentTerm) {
    Log_info("[INSTALL-SNAPSHOT] Site {}: Leader {} has higher term ({} > {}) - updating",
             site_id_, leader_id, term, currentTerm);
    auto prev_term = currentTerm;
    currentTerm = term;
    // @unsafe
    {
    vote_for_ = INVALID_SITEID;
    }
    setIsLeader(false);
    PersistState(currentTerm, vote_for_, "OnInstallSnapshot: leader higher term");
    LogTermChange("InstallSnapshot carried newer term", prev_term, currentTerm, leader_id);
    // @unsafe
    { *term_out = currentTerm; }
  }

  // Track the leader's identity for GetLeaderHint()
  current_leader_id_ = static_cast<siteid_t>(leader_id);

  // Reset election timer (legitimate leader contact)
  resetTimer("received InstallSnapshot");

  // ============================================================================
  // Save snapshot data via snapshot_manager_
  // ============================================================================
  if (snapshot_manager_) {
    // @unsafe { snapshot_manager_ I/O operations }
    bool saved = snapshot_manager_->TakeSnapshot(
        last_included_index, last_included_term,
        data.data(), data.size());

    if (!saved) {
      Log_error("[INSTALL-SNAPSHOT] Site {}: Failed to save snapshot at index={} term={}",
                site_id_, last_included_index, last_included_term);
      // Still update in-memory state even if persistence fails
    } else {
      Log_info("[INSTALL-SNAPSHOT] Site {}: Snapshot saved at index={} term={}",
               site_id_, last_included_index, last_included_term);
    }
  }

  // ============================================================================
  // Update snapshot metadata
  // ============================================================================
  snapidx_ = last_included_index;
  snapterm_ = last_included_term;

  // ============================================================================
  // Discard log entries covered by the snapshot
  // ============================================================================
  // Remove all log entries up to and including last_included_index
  std::vector<slotid_t> to_erase;
  for (auto& kv : raft_logs_) {
    if (kv.first <= last_included_index) {
      to_erase.push_back(kv.first);
    }
  }
  for (auto slot : to_erase) {
    raft_logs_.erase(slot);
  }

  // Update min_active_slot_ to reflect compacted log
  if (last_included_index + 1 > min_active_slot_) {
    min_active_slot_ = last_included_index + 1;
  }

  // Also compact persistent log storage if available
  if (log_storage_) {
    // @unsafe { log_storage_ I/O }
    log_storage_->remove_range(log_storage_->get_first_index(),
                               last_included_index + 1);
  }

  // ============================================================================
  // Advance commitIndex and executeIndex
  // ============================================================================
  if (last_included_index > commitIndex) {
    commitIndex = last_included_index;
    PersistCommitIndexToLogStorage();
  }
  if (last_included_index > executeIndex) {
    executeIndex = last_included_index;
  }

  // Update lastLogIndex if the snapshot covers beyond it
  if (last_included_index > lastLogIndex) {
    lastLogIndex = last_included_index;
  }

  // ============================================================================
  // Load state machine snapshot if callback is registered
  // ============================================================================
  if (load_sm_snapshot_cb_) {
    // @unsafe { callback invocation }
    Log_info("[INSTALL-SNAPSHOT] Site {}: Loading state machine snapshot ({} bytes)",
             site_id_, data.size());
    load_sm_snapshot_cb_(data);
  }

  Log_info("[INSTALL-SNAPSHOT] Site {}: Installed snapshot from leader {} "
           "(snapidx={}, snapterm={}, commitIndex={}, executeIndex={}, lastLogIndex={})",
           site_id_, leader_id, snapidx_, snapterm_, commitIndex, executeIndex, lastLogIndex);
}

// @unsafe - Stops monitor thread (std::thread and std::atomic operations marked safe via @external)
void RaftServer::StopLeadershipTransferMonitoring() {
  leadership_monitor_stop_ = true;

  // Detach the monitor thread so it can exit gracefully without deadlock
  // The thread will see leadership_monitor_stop_ and exit on its own
  if (leadership_monitor_thread_.joinable()) {
    Log_debug("[LEADERSHIP-TRANSFER] Site {}: Detaching monitor thread (will exit on its own)", site_id_);
    leadership_monitor_thread_.detach();
  }
}

// @unsafe - Starts monitor thread (threading and mutex operations marked safe via @external)
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

  Log_info("[LEADERSHIP-TRANSFER] Site {}: Starting leadership transfer monitoring thread",
           site_id_);

  // Launch monitoring thread
  leadership_monitor_thread_ = std::thread([this]() {
    const uint64_t CHECK_INTERVAL_MS = 1000;  // Check every 1 second
    const uint64_t MIN_STABLE_TIME_US = 500000; // Wait 0.5 seconds (in microseconds) after becoming leader before transferring

    uint64_t became_leader_time = Time::now(false);

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Monitor thread started (will check every {}ms)",
             site_id_, CHECK_INTERVAL_MS);

    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));

      bool should_transfer = false;

      // Critical section: check shared state with proper locking
      {
        std::lock_guard<std::recursive_mutex> lock(mtx_);

        // Check if we should stop monitoring
        if (leadership_monitor_stop_) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: Monitor stop requested, exiting", site_id_);
          break;
        }

        // Check if server is shutting down
        if (stop_) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: Server shutting down, exiting monitor", site_id_);
          break;
        }

        // Check if we're still leader
        if (!is_leader_) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: No longer leader, exiting monitor", site_id_);
          break;
        }

        // Check if we became preferred (no longer need to transfer)
        if (AmIPreferredLeader()) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: I am now preferred leader, exiting monitor",
                   site_id_);
          break;
        }

        // Wait for cluster to stabilize after becoming leader
        uint64_t time_as_leader = Time::now(false) - became_leader_time;
        if (time_as_leader < MIN_STABLE_TIME_US) {
          continue;
        }

        // Check if we should transfer leadership
        if (ShouldTransferLeadership()) {
          Log_info("[LEADERSHIP-TRANSFER] Site {}: Conditions met, initiating transfer NOW",
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

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Monitor thread exiting", site_id_);
  });
}

// @unsafe - Calls Setup if not already initialized
void RaftServer::EnsureSetup() {
  if (heartbeat_setup_) {
    return;
  }
  heartbeat_setup_ = true;
  Setup();
}

// @unsafe - Checks conditions for leadership transfer (mutex and map access marked safe via @external)
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
    Log_debug("[LEADERSHIP-TRANSFER] Site {}: Preferred replica {} not in peer list",
              site_id_, preferred_leader_site_id_);
    return false;
  }

  // Check if preferred replica is caught up
  slotid_t preferred_match_index = it->second;
  bool is_caught_up = (preferred_match_index >= commitIndex);

  if (!is_caught_up) {
    Log_debug("[LEADERSHIP-TRANSFER] Site {}: Preferred replica {} not caught up (match={}, commit={})",
              site_id_, preferred_leader_site_id_, preferred_match_index, commitIndex);
    return false;
  }

  Log_info("[LEADERSHIP-TRANSFER] Site {}: Preferred replica {} is caught up! Ready to transfer",
           site_id_, preferred_leader_site_id_);
  return true;
}

// @unsafe - Initiates leadership transfer (RPC calls wrapped in @unsafe blocks)
void RaftServer::InitiateLeadershipTransfer() {
  // Check if server is shutting down
  if (stop_) {
    Log_info("[LEADERSHIP-TRANSFER] Site {}: Aborting transfer - server shutting down", site_id_);
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
    leadership_transfer_start_time_ = Time::now(false);

    Log_info("[LEADERSHIP-TRANSFER] Site {} (partition {}): Starting transfer to site {}",
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
        janus::Command{},
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

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Stepping down from leadership (current_term={})",
             site_id_, currentTerm);

    // Become follower - this stops heartbeats and allows new leader to emerge
    setIsLeader(false);

    Log_info("[LEADERSHIP-TRANSFER] Site {}: Leadership transfer complete - now follower",
             site_id_);
  }
}

// ============================================================================
// SPECULATIVE REPLICATION STATE
// ============================================================================

void RaftServer::ResetSpeculativeState() {
  // Note: caller must hold mtx_ lock

  if (is_leader_) {
    // On becoming leader: initialize with self votes
    specVoters_.clear();
    specVoters_.insert(site_id_);  // voted for self
    durableVoters_.clear();
    durableVoters_.insert(site_id_);  // self vote is always durable

    // Reset commit indices to current commitIndex (from previous term)
    securedLogIndex_ = commitIndex;
    specCommitIndex_ = commitIndex;

    // Leader starts unsecured until durable vote quorum is achieved
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site {}: Reset speculative state as new leader - "
             "specVoters={{{}}} durableVoters={{{}}} securedLogIndex={} specCommitIndex={}",
             site_id_, site_id_, site_id_, securedLogIndex_, specCommitIndex_);
  } else {
    // On stepping down: clear all speculative state
    specVoters_.clear();
    durableVoters_.clear();
    securedLogIndex_ = 0;
    specCommitIndex_ = 0;
    securedLeader_ = false;

    Log_info("[SPEC-RAFT] Site {}: Cleared speculative state (stepped down)",
             site_id_);
  }

  // Persist the updated speculative indices
  PersistSpeculativeIndicesToLogStorage();

  // Clear ack tracking maps
  memoryAcks_.clear();
  durableAcks_.clear();

  // Reset callback notification tracking
  // Note: We don't clear pendingCallbacks_ here because:
  // - On becoming leader: there shouldn't be any pending callbacks yet
  // - On stepping down: NotifyRollback() handles clearing after notification
  lastSpecNotifiedIndex_ = commitIndex;  // Don't re-notify already-committed entries
  lastDurableNotifiedIndex_ = commitIndex;
}

void RaftServer::VerifySpeculativeInvariants() const {
  // Invariant 1: securedLogIndex <= specCommitIndex <= lastLogIndex
  if (securedLogIndex_ > specCommitIndex_) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: securedLogIndex ({}) > specCommitIndex ({})",
              securedLogIndex_, specCommitIndex_);
    verify(securedLogIndex_ <= specCommitIndex_);
  }

  if (specCommitIndex_ > lastLogIndex) {
    Log_error("[SPEC-RAFT] INVARIANT VIOLATION: specCommitIndex ({}) > lastLogIndex ({})",
              specCommitIndex_, lastLogIndex);
    verify(specCommitIndex_ <= lastLogIndex);
  }

  // Note: durableVoters ⊆ specVoters is NOT strictly enforced after crashes.
  // A crashed node loses its memory vote but keeps its durable vote on disk.
  // This is expected behavior, not an invariant violation.
  //
  // Key insight: |durableVoters| >= quorum is sufficient for securedLeader = true.
  // Once durable quorum is reached, specVoters quorum is no longer required.
  // See docs/dev/phase6_relax_invariant_plan.md for full safety argument.

  Log_debug("[SPEC-RAFT] Site {}: Invariants OK - securedLogIndex={} specCommitIndex={} lastLogIndex={}",
            site_id_, securedLogIndex_, specCommitIndex_, lastLogIndex);
}

// ============================================================================
// OnPeerRestart - Handle speculative state invalidation on peer restart
// ============================================================================

void RaftServer::OnPeerRestart(siteid_t restarted_site_id) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // Only process if we're the leader
  if (!is_leader_) {
    Log_debug("[SPEC-RAFT] Site {}: Ignoring peer restart from {} - not leader",
              site_id_, restarted_site_id);
    return;
  }

  Log_info("[SPEC-RAFT] Site {}: Handling peer restart from site {}",
           site_id_, restarted_site_id);

  // Remove from specVoters (their memory vote is no longer reliable)
  size_t removed_from_voters = specVoters_.erase(restarted_site_id);
  if (removed_from_voters > 0) {
    Log_info("[SPEC-RAFT] Site {}: Removed site {} from specVoters (now size={})",
             site_id_, restarted_site_id, specVoters_.size());
  }

  // Remove from memoryAcks for unsecured entries only
  // Entries at or below securedLogIndex are already durably committed,
  // so removing the restarted server doesn't affect their status
  size_t entries_affected = 0;
  for (auto& entry : memoryAcks_) {
    uint64_t idx = entry.first;
    if (idx > securedLogIndex_) {
      if (entry.second.erase(restarted_site_id) > 0) {
        entries_affected++;
      }
    }
  }
  if (entries_affected > 0) {
    Log_info("[SPEC-RAFT] Site {}: Removed site {} from memoryAcks for {} unsecured entries",
             site_id_, restarted_site_id, entries_affected);
  }

  // Note: We don't remove from durableVoters or durableAcks because:
  // 1. durableVoters represents votes that were persisted to disk BEFORE the crash
  //    - If the vote was durable, it survives the crash
  //    - If it wasn't durable, it was never in durableVoters
  // 2. durableAcks represents entries that were persisted to disk
  //    - Same logic: durable acks survive crashes by definition

  // Check if we need to become secured or step down
  // Relaxed invariant - durableVoters and specVoters are independent after crashes
  if (!securedLeader_ && is_leader_) {
    size_t quorum = GetQuorumSize();

    // NEW: Check if durable quorum is sufficient for secured status
    // Note: site_id_ is already in durableVoters_ (inserted by ResetSpeculativeState
    // or RequestElection), so no +1 needed. This matches OnVoteDurable() at line 1417.
    size_t durable_vote_count = durableVoters_.size();
    if (durable_vote_count >= quorum) {
      // We have durable quorum - become secured leader
      // Safety: durableVoters have votedFor=us on disk, can't vote for others in this term
      securedLeader_ = true;
      Log_info("[SPEC-RAFT] Site {}: Became secured via durable quorum ({}/{}) "
               "despite spec quorum loss (specVoters={})",
               site_id_, durable_vote_count, quorum, specVoters_.size());
    } else {
      // No durable quorum yet - check speculative quorum
      // Note: site_id_ is already in specVoters_ (inserted by ResetSpeculativeState
      // or RequestElection), so no +1 needed.
      size_t vote_count = specVoters_.size();
      if (vote_count < quorum) {
        // No durable quorum AND no speculative quorum - must step down
        Log_info("[SPEC-RAFT] Site {}: Lost both spec quorum ({}/{}) and durable quorum ({}/{}) - stepping down",
                 site_id_, vote_count, quorum, durable_vote_count, quorum);
        stepDown(StepDownReason::UnsecuredFailure);
        return;  // Don't verify invariants after stepping down
      }
    }
  }

  VerifySpeculativeInvariants();
}

// ============================================================================
// stepDown - Central leader step-down function
// ============================================================================

static const char* StepDownReasonToString(StepDownReason reason) {
  switch (reason) {
    case StepDownReason::UnsecuredFailure: return "UnsecuredFailure";
    case StepDownReason::SecuredFailure: return "SecuredFailure";
    case StepDownReason::HigherTerm: return "HigherTerm";
    default: return "Unknown";
  }
}

void RaftServer::stepDown(StepDownReason reason) {
  // Must be called with mtx_ held (caller's responsibility)
  // Most callers already hold the lock

  Log_info("[SPEC-RAFT] Site {}: Stepping down as leader (reason={}, term={}, "
           "securedLeader={}, specVoters={}, durableVoters={})",
           site_id_, StepDownReasonToString(reason), currentTerm,
           securedLeader_, specVoters_.size(), durableVoters_.size());

  // Reset speculative state
  // This clears specVoters_, durableVoters_, etc.
  ResetSpeculativeState();

  // Transition to follower state
  // This handles view updates, callback notifications, etc.
  setIsLeader(false);

  // Reset election timer
  // Important: Give other servers time to elect a new leader
  resetTimer("stepDown");

  Log_info("[SPEC-RAFT] Site {}: Step-down complete, now follower", site_id_);

  // Notify pending callbacks of rollback based on step-down reason
  NotifyRollback(reason);
}

// ============================================================================
// CLIENT NOTIFICATION CALLBACKS
// ============================================================================

void RaftServer::RegisterCommitCallback(uint64_t index,
                                        std::function<void(CommitStatus)> callback) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // If already speculatively committed, invoke immediately
  if (index <= specCommitIndex_) {
    Log_debug("[SPEC-CALLBACK] Index {} already spec-committed, notifying SPECULATIVE",
              index);
    callback(CommitStatus::SPECULATIVE);
  }

  // If already durably committed, invoke immediately
  if (securedLeader_ && index <= securedLogIndex_) {
    Log_debug("[SPEC-CALLBACK] Index {} already durable-committed, notifying DURABLE",
              index);
    callback(CommitStatus::DURABLE);
    return;  // No need to track - already fully committed
  }

  // Store callback for future notification
  pendingCallbacks_[index] = std::move(callback);
  Log_debug("[SPEC-CALLBACK] Registered callback for index {}", index);
}

void RaftServer::NotifyCallbacks(uint64_t from, uint64_t to, CommitStatus status) {
  // Note: Caller must hold mtx_
  // Notify callbacks for indices in (from, to]

  for (uint64_t idx = from + 1; idx <= to; ++idx) {
    auto it = pendingCallbacks_.find(idx);
    if (it != pendingCallbacks_.end()) {
      Log_debug("[SPEC-CALLBACK] Notifying index {} with status {}",
                idx, static_cast<int>(status));
      it->second(status);

      // If DURABLE, remove callback (fully committed)
      if (status == CommitStatus::DURABLE) {
        pendingCallbacks_.erase(it);
      }
    }
  }
}

// @unsafe - Invokes callbacks, clears pendingCallbacks_
void RaftServer::NotifyRollback(StepDownReason reason) {
  // Note: Caller must hold mtx_
  // Notify pending callbacks based on step-down reason

  Log_info("[SPEC-CALLBACK] NotifyRollback reason={}, pending={}, "
           "commitIndex={}, specCommitIndex={}, securedLogIndex={}, lastLogIndex={}",
           StepDownReasonToString(reason), pendingCallbacks_.size(),
           commitIndex, specCommitIndex_, securedLogIndex_, lastLogIndex);

  switch (reason) {
    case StepDownReason::UnsecuredFailure:
      // Lost speculative quorum while unsecured leader.
      // All current-term entries are suspect -> rollback everything
      // from commitIndex + 1 to lastLogIndex.
      Log_info("[SPEC-CALLBACK] UnsecuredFailure: rolling back entries ({}, {}]",
               commitIndex, lastLogIndex);
      NotifyCallbacks(commitIndex, lastLogIndex, CommitStatus::ROLLEDBACK);  // @unsafe
      break;

    case StepDownReason::SecuredFailure:
      // Lost quorum but was secured leader. Only unsecured entries
      // (above securedLogIndex_) are suspect -> rollback from
      // securedLogIndex_ + 1 to specCommitIndex_.
      Log_info("[SPEC-CALLBACK] SecuredFailure: rolling back entries ({}, {}]",
               securedLogIndex_, specCommitIndex_);
      NotifyCallbacks(securedLogIndex_, specCommitIndex_, CommitStatus::ROLLEDBACK);  // @unsafe
      break;

    case StepDownReason::HigherTerm:
      // Saw higher term from another server. Entries may still be
      // valid under the new leader - don't send rollback notifications.
      Log_info("[SPEC-CALLBACK] HigherTerm step-down - no automatic rollback");
      break;
  }

  // Clear ALL pending callbacks regardless of reason (we're no longer leader)
  pendingCallbacks_.clear();

  // Reset notification tracking
  lastSpecNotifiedIndex_ = 0;
  lastDurableNotifiedIndex_ = 0;
}

// ============================================================================
// MEMBERSHIP CHANGE IMPLEMENTATION
// ============================================================================

// @safe - Read-only computation on member field
size_t RaftServer::GetQuorumSize() const {
  size_t config_size = 0;
  // @unsafe
  { config_size = current_config_.size(); }
  return config_size / 2 + 1;
}

// @safe - Read-only accessor
// @lifetime: (&'a) -> &'a
const std::set<siteid_t>& RaftServer::GetCurrentConfig() const {
  return current_config_;
}

// @unsafe - Modifies config state
void RaftServer::OnAddServer(const uint64_t term,
                             const uint64_t new_server_id,
                             const std::string& addr,
                             bool_t* success,
                             std::string* error_msg,
                             uint64_t* leader_hint) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // @unsafe
  {
    *leader_hint = (current_leader_id_ != INVALID_SITEID) ? current_leader_id_ : 0;
  }

  // Check if this server is the leader
  if (!IsLeader()) {
    // @unsafe
    {
      *success = false;
      *error_msg = "not leader";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: not leader (site {})", site_id_);
    return;
  }

  // Check if a config change is already pending
  if (config_change_pending_) {
    // @unsafe
    {
      *success = false;
      *error_msg = "config change already pending";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: config change pending (site {})", site_id_);
    return;
  }

  // Check if server is already in config or is already a learner
  if (current_config_.count(static_cast<siteid_t>(new_server_id)) > 0) {
    // @unsafe
    {
      *success = false;
      *error_msg = "server already in config";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: server {} already in config (site {})",
             new_server_id, site_id_);
    return;
  }

  if (learners_.count(static_cast<siteid_t>(new_server_id)) > 0) {
    // @unsafe
    {
      *success = false;
      *error_msg = "server already a learner (catch-up in progress)";
    }
    Log_info("[RAFT-CONFIG] AddServer rejected: server {} already a learner (site {})",
             new_server_id, site_id_);
    return;
  }

  // Add server as a learner first. It will receive log entries via HeartbeatLoop
  // (through next_index_/match_index_) but will NOT count towards quorum.
  // Once caught up (match_index_ within catchup_threshold_ of lastLogIndex),
  // CheckAndPromoteLearners() will promote it to a full member.
  auto sid = static_cast<siteid_t>(new_server_id);
  learners_.insert(sid);
  config_change_pending_ = true;
  pending_config_index_ = lastLogIndex;  // Track where this change happened

  // Initialize replication state so HeartbeatLoop sends entries to this learner
  if (next_index_.find(sid) == next_index_.end()) {
    next_index_[sid] = lastLogIndex + 1;
  }
  if (match_index_.find(sid) == match_index_.end()) {
    match_index_[sid] = 0;
  }

  // @unsafe
  {
    *success = true;
    *error_msg = "";
  }

  Log_info("[RAFT-CONFIG] AddServer: added server {} as learner (site {}), "
           "learners={}, config_size={}, next_index={}",
           new_server_id, site_id_, learners_.size(),
           current_config_.size(), next_index_[sid]);
}

// @unsafe - Modifies config state, logs output
void RaftServer::PromoteLearner(siteid_t id) {
  // Must be called with mtx_ held
  learners_.erase(id);
  current_config_.insert(id);
  config_change_pending_ = false;
  Log_info("[RAFT-CONFIG] Promoted learner {} to full member "
           "(config size={}, quorum={}, learners={})",
           id, current_config_.size(), GetQuorumSize(), learners_.size());
}

// @unsafe - Reads match_index_, calls PromoteLearner
void RaftServer::CheckAndPromoteLearners() {
  // Must be called with mtx_ held
  if (learners_.empty()) {
    return;
  }

  std::vector<siteid_t> to_promote;
  for (auto learner_id : learners_) {
    auto it = match_index_.find(learner_id);
    if (it != match_index_.end() && lastLogIndex > 0) {
      // Learner is caught up if within catchup_threshold_ of leader's log
      if (it->second >= lastLogIndex ||
          (lastLogIndex - it->second) <= catchup_threshold_) {
        to_promote.push_back(learner_id);
      }
    }
  }
  for (auto id : to_promote) {
    PromoteLearner(id);
  }
}

// @unsafe - Modifies config state
void RaftServer::OnRemoveServer(const uint64_t term,
                                const uint64_t server_id,
                                bool_t* success,
                                std::string* error_msg,
                                uint64_t* leader_hint) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);

  // @unsafe
  {
    *leader_hint = (current_leader_id_ != INVALID_SITEID) ? current_leader_id_ : 0;
  }

  // Check if this server is the leader
  if (!IsLeader()) {
    // @unsafe
    {
      *success = false;
      *error_msg = "not leader";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: not leader (site {})", site_id_);
    return;
  }

  // Check if a config change is already pending
  if (config_change_pending_) {
    // @unsafe
    {
      *success = false;
      *error_msg = "config change already pending";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: config change pending (site {})", site_id_);
    return;
  }

  // Check if server is in config
  if (current_config_.count(static_cast<siteid_t>(server_id)) == 0) {
    // @unsafe
    {
      *success = false;
      *error_msg = "server not in config";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: server {} not in config (site {})",
             server_id, site_id_);
    return;
  }

  // Cannot remove the last server
  if (current_config_.size() <= 1) {
    // @unsafe
    {
      *success = false;
      *error_msg = "cannot remove last server";
    }
    Log_info("[RAFT-CONFIG] RemoveServer rejected: cannot remove last server (site {})", site_id_);
    return;
  }

  // TODO: In the future, this should append a configuration change entry to the
  // Raft log and only take effect when committed. For now, we apply the change
  // directly in memory.

  // Apply config change immediately
  current_config_.erase(static_cast<siteid_t>(server_id));
  config_change_pending_ = true;
  pending_config_index_ = lastLogIndex;  // Track where this change happened

  // @unsafe
  {
    *success = true;
    *error_msg = "";
  }

  Log_info("[RAFT-CONFIG] RemoveServer: removed server {} from config (site {}), new config size={}, quorum={}",
           server_id, site_id_, current_config_.size(), GetQuorumSize());
}

} // namespace janus
