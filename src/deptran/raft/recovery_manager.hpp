#pragma once

/**
 * Recovery Manager for Raft/Paxos Consensus Protocols
 *
 * This header defines:
 * - RecoveryMode: Fresh start vs recovery detection
 * - RecoveryConfig: Configuration for recovery behavior
 * - RecoveryResult: Statistics from recovery operation
 * - RecoveryManager: Coordinates recovery sequence
 *
 * RustyCpp Compliance: Uses rusty::Cell for interior mutability
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <rusty/cell.hpp>
#include <rusty/rusty.hpp>
#include <rusty/slice.hpp>

#include "log_storage.hpp"
#include "rocksdb_log_storage.hpp"
#include "rrr/rrr.hpp"

namespace janus {
namespace raft {

/**
 * Mode of operation for recovery.
 */
// @safe - Simple enum
#if RUSTYCPP_RUST
pub enum RecoveryMode {
    FRESH_START,
    NORMAL_RECOVERY,
    FORCED_FRESH,
}
#endif
/*RUSTYCPP:GEN-BEGIN id=recovery_manager.1 version=1 rust_sha256=277fe509da36d69e8bf08f6c863af4f7adc21208cec820ebb7922eeb1fbe48ff*/
enum class RecoveryMode;
inline constexpr RecoveryMode RecoveryMode_FRESH_START();
inline constexpr RecoveryMode RecoveryMode_NORMAL_RECOVERY();
inline constexpr RecoveryMode RecoveryMode_FORCED_FRESH();

enum class RecoveryMode {
    FRESH_START,
    NORMAL_RECOVERY,
    FORCED_FRESH
};
inline constexpr RecoveryMode RecoveryMode_FRESH_START() { return RecoveryMode::FRESH_START; }
inline constexpr RecoveryMode RecoveryMode_NORMAL_RECOVERY() { return RecoveryMode::NORMAL_RECOVERY; }
inline constexpr RecoveryMode RecoveryMode_FORCED_FRESH() { return RecoveryMode::FORCED_FRESH; }
/*RUSTYCPP:GEN-END id=recovery_manager.1*/

/**
 * Configuration for recovery behavior.
 */
struct RecoveryConfig;

inline RecoveryConfig recovery_config_defaults();
inline RecoveryConfig recovery_config_for_replica(uint32_t partition_id,
                                                  uint32_t locale_id);

#if RUSTYCPP_RUST
pub struct RecoveryConfig {
    storage_path: std::string,
    force_fresh_start: bool,
    recovery_timeout_ms: u32,
    verify_on_recovery: bool,
    clear_on_forced_fresh: bool,
}

impl RecoveryConfig {
    fn defaults() -> RecoveryConfig {
        recovery_config_defaults()
    }

    fn for_replica(partition_id: u32, locale_id: u32) -> RecoveryConfig {
        recovery_config_for_replica(partition_id, locale_id)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=recovery_manager.config version=1 rust_sha256=cf73357af5c6b57f725c90d16cccaebcb6648a7ecf0d0a5ecc3ee46b8ad448e0*/
struct RecoveryConfig;

struct RecoveryConfig {
    std::string storage_path;
    bool force_fresh_start;
    uint32_t recovery_timeout_ms;
    bool verify_on_recovery;
    bool clear_on_forced_fresh;

    static RecoveryConfig defaults();
    static RecoveryConfig for_replica(uint32_t partition_id, uint32_t locale_id);
};


inline RecoveryConfig RecoveryConfig::defaults() {
    return recovery_config_defaults();
}

inline RecoveryConfig RecoveryConfig::for_replica(uint32_t partition_id, uint32_t locale_id) {
    return recovery_config_for_replica(std::move(partition_id), std::move(locale_id));
}
/*RUSTYCPP:GEN-END id=recovery_manager.config*/

inline RecoveryConfig recovery_config_defaults() {
  RecoveryConfig config{};
  config.storage_path = "";
  config.force_fresh_start = false;
  config.recovery_timeout_ms = 30000;
  config.verify_on_recovery = true;
  config.clear_on_forced_fresh = true;
  return config;
}

inline RecoveryConfig recovery_config_for_replica(uint32_t partition_id,
                                                  uint32_t locale_id) {
  RecoveryConfig config = RecoveryConfig::defaults();
  // Use username prefix to avoid conflicts between users.
  std::string username;
  auto user = std::getenv("USER");  // @unsafe
  if (user) {
    username = user;
  } else {
    username = "unknown";
  }
  config.storage_path = "/tmp/" + username + "_mako_log_shard" +
                       std::to_string(partition_id) + "_replica" +
                       std::to_string(locale_id);
  return config;
}

/**
 * Results from a recovery operation.
 */
struct RecoveryResult;

inline RecoveryResult recovery_result_defaults();
inline RecoveryResult recovery_result_success_fresh();
inline RecoveryResult recovery_result_failure(const std::string& error);

#if RUSTYCPP_RUST
pub struct RecoveryResult {
    mode: RecoveryMode,
    success: bool,
    error_message: std::string,
    recovered_entries: u64,
    recovered_term: u64,
    recovered_epoch: u64,
    recovery_time_ms: u64,
}

impl RecoveryResult {
    fn defaults() -> RecoveryResult {
        recovery_result_defaults()
    }

    fn success_fresh() -> RecoveryResult {
        recovery_result_success_fresh()
    }

    fn failure(error: &std::string) -> RecoveryResult {
        recovery_result_failure(error)
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=recovery_manager.result version=1 rust_sha256=23802ad907328337a65459c9cc51e48b006d4168a69feb16971aa87d1fcad2e1*/
struct RecoveryResult;

struct RecoveryResult {
    RecoveryMode mode;
    bool success;
    std::string error_message;
    uint64_t recovered_entries;
    uint64_t recovered_term;
    uint64_t recovered_epoch;
    uint64_t recovery_time_ms;

    static RecoveryResult defaults();
    static RecoveryResult success_fresh();
    static RecoveryResult failure(const std::string& error);
};


inline RecoveryResult RecoveryResult::defaults() {
    return recovery_result_defaults();
}

inline RecoveryResult RecoveryResult::success_fresh() {
    return recovery_result_success_fresh();
}

inline RecoveryResult RecoveryResult::failure(const std::string& error) {
    return recovery_result_failure(error);
}
/*RUSTYCPP:GEN-END id=recovery_manager.result*/

inline RecoveryResult recovery_result_defaults() {
  RecoveryResult result{};
  result.mode = RecoveryMode::FRESH_START;
  result.success = false;
  result.error_message = "";
  result.recovered_entries = 0;
  result.recovered_term = 0;
  result.recovered_epoch = 0;
  result.recovery_time_ms = 0;
  return result;
}

inline RecoveryResult recovery_result_success_fresh() {
  RecoveryResult result = RecoveryResult::defaults();
  result.mode = RecoveryMode::FRESH_START;
  result.success = true;
  return result;
}

inline RecoveryResult recovery_result_failure(const std::string& error) {
  RecoveryResult result = RecoveryResult::defaults();
  result.success = false;
  result.error_message = error;
  return result;
}

// @safe - pure recovery policy/result helpers. Filesystem cleanup, storage
// open/rebuild, RocksDB handles, and snapshot/log replay stay in
// RecoveryManager's hand-C++ orchestration.
#if RUSTYCPP_RUST
pub fn recovery_mode_is_fresh_start(mode: RecoveryMode) -> bool {
    mode == RecoveryMode::FRESH_START || mode == RecoveryMode::FORCED_FRESH
}

pub fn recovery_mode_needs_recovery(mode: RecoveryMode) -> bool {
    mode == RecoveryMode::NORMAL_RECOVERY
}

pub fn recovery_result_mark_fresh_success(result: &mut RecoveryResult) {
    result.success = true;
    result.recovered_entries = 0;
}

pub fn recovery_result_begin(mode: RecoveryMode) -> RecoveryResult {
    let mut result = RecoveryResult::defaults();
    result.mode = mode;
    result
}

pub fn recovery_result_mark_success(result: &mut RecoveryResult,
                                    recovered_entries: u64,
                                    recovery_time_ms: u64) {
    result.success = true;
    result.recovered_entries = recovered_entries;
    result.recovery_time_ms = recovery_time_ms;
}

pub fn recovery_should_clear_forced_fresh(mode: RecoveryMode,
                                          clear_on_forced_fresh: bool) -> bool {
    mode == RecoveryMode::FORCED_FRESH && clear_on_forced_fresh
}

pub fn recovery_storage_open_failed(has_storage: bool,
                                    storage_is_open: bool) -> bool {
    !has_storage || !storage_is_open
}

pub fn recovery_fresh_start_should_attach_storage(mode: RecoveryMode,
                                                  has_storage: bool) -> bool {
    (mode == RecoveryMode::FRESH_START || mode == RecoveryMode::FORCED_FRESH) && has_storage
}

pub fn recovery_normal_storage_missing(mode: RecoveryMode,
                                       has_storage: bool) -> bool {
    mode == RecoveryMode::NORMAL_RECOVERY && !has_storage
}

pub fn recovery_replay_failed(recover_ok: bool) -> bool {
    !recover_ok
}

pub fn recovery_result_assign_recovery_time(result: &mut RecoveryResult,
                                            recovery_time_ms: u64) {
    result.recovery_time_ms = recovery_time_ms;
}
#endif
/*RUSTYCPP:GEN-BEGIN id=recovery_manager.small_helpers version=1 rust_sha256=1505488c466c02fb636e67d285883f401311492497a958303ce3f53aab8c5cc1*/
inline bool recovery_storage_open_failed(bool has_storage, bool storage_is_open);
inline bool recovery_replay_failed(bool recover_ok);

inline bool recovery_mode_is_fresh_start(RecoveryMode mode) {
    return mode == RecoveryMode::FRESH_START || mode == RecoveryMode::FORCED_FRESH;
}

inline bool recovery_mode_needs_recovery(RecoveryMode mode) {
    return mode == RecoveryMode::NORMAL_RECOVERY;
}

inline void recovery_result_mark_fresh_success(RecoveryResult& result) {
    result.success = true;
    result.recovered_entries = 0;
}

inline RecoveryResult recovery_result_begin(RecoveryMode mode) {
    auto result = RecoveryResult::defaults();
    result.mode = std::move(mode);
    return std::move(result);
}

inline void recovery_result_mark_success(RecoveryResult& result, uint64_t recovered_entries, uint64_t recovery_time_ms) {
    result.success = true;
    result.recovered_entries = std::move(recovered_entries);
    result.recovery_time_ms = std::move(recovery_time_ms);
}

inline bool recovery_should_clear_forced_fresh(RecoveryMode mode, bool clear_on_forced_fresh) {
    return mode == RecoveryMode::FORCED_FRESH && clear_on_forced_fresh;
}

inline bool recovery_storage_open_failed(bool has_storage, bool storage_is_open) {
    return !has_storage || !storage_is_open;
}

inline bool recovery_fresh_start_should_attach_storage(RecoveryMode mode, bool has_storage) {
    return (((rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::FRESH_START)) || (rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::FORCED_FRESH)))) && rusty::detail::deref_if_pointer_like(has_storage);
}

inline bool recovery_normal_storage_missing(RecoveryMode mode, bool has_storage) {
    return (rusty::detail::deref_if_pointer_like(mode) == rusty::clone(RecoveryMode::NORMAL_RECOVERY)) && !has_storage;
}

inline bool recovery_replay_failed(bool recover_ok) {
    return !recover_ok;
}

inline void recovery_result_assign_recovery_time(RecoveryResult& result, uint64_t recovery_time_ms) {
    result.recovery_time_ms = std::move(recovery_time_ms);
}
/*RUSTYCPP:GEN-END id=recovery_manager.small_helpers*/

/**
 * Recovery Manager coordinates the recovery sequence for Raft/Paxos servers.
 *
 * Usage:
 *   RecoveryConfig config = RecoveryConfig::for_replica(partition_id, locale_id);
 *   RecoveryManager manager(config);
 *   auto storage = manager.create_storage();
 *   if (storage) {
 *     server->SetLogStorage(storage);
 *     auto result = manager.recover_raft(server);
 *   }
 */
class RecoveryManager {
 public:
  // @safe - Constructor with config
  explicit RecoveryManager(RecoveryConfig config)
      : config_(std::move(config)),
        initialized_(false),
        detected_mode_(RecoveryMode::FRESH_START) {}

  // @unsafe - Uses filesystem operations
  RecoveryMode detect_mode() const {
    if (config_.force_fresh_start) {
      return RecoveryMode::FORCED_FRESH;
    }

    // Check if storage directory exists and has RocksDB data
    std::error_code ec;
    bool exists = std::filesystem::exists(config_.storage_path, ec);  // @unsafe
    if (!exists || ec) {  // @unsafe
      return RecoveryMode::FRESH_START;
    }

    // Check for CURRENT file which indicates valid RocksDB
    std::string current_file = config_.storage_path + "/CURRENT";
    exists = std::filesystem::exists(current_file, ec);  // @unsafe
    if (!exists || ec) {  // @unsafe
      return RecoveryMode::FRESH_START;
    }

    return RecoveryMode::NORMAL_RECOVERY;
  }

  // @unsafe - Create/open storage backend
  std::shared_ptr<LogStorage> create_storage() {
    if (storage_) {
      return storage_;
    }

    detected_mode_.set(detect_mode());

    // Handle forced fresh start
    if (recovery_should_clear_forced_fresh(detected_mode_.get(),
                                           config_.clear_on_forced_fresh)) {
      // @unsafe { filesystem operations }
      std::error_code ec;
      std::filesystem::remove_all(config_.storage_path, ec);
      if (ec) {
        Log_error("Failed to clear storage at %s: %s",
                  config_.storage_path.c_str(), ec.message().c_str());
      }
    }

    // Create storage
    storage_ = std::make_shared<RocksDBLogStorage>(config_.storage_path);
    if (recovery_storage_open_failed(storage_ != nullptr,
                                     storage_ != nullptr && storage_->is_open())) {
      Log_error("Failed to open RocksDB at %s", config_.storage_path.c_str());
      storage_ = nullptr;
      return nullptr;
    }

    initialized_.set(true);
    Log_info("Recovery: Storage opened at %s (mode=%d)",
             config_.storage_path.c_str(), static_cast<int>(detected_mode_.get()));
    return storage_;
  }

  // @lifetime: (&'a) -> &'a
  const std::string& storage_path() const {
    return config_.storage_path;
  }

  // @safe - Check if recovery is needed (vs fresh start)
  bool needs_recovery() const {
    return recovery_mode_needs_recovery(detected_mode_.get());
  }

  // @safe - Get detected mode
  RecoveryMode get_detected_mode() const {
    return detected_mode_.get();
  }

  // @safe - Check if initialized
  bool is_initialized() const {
    return initialized_.get();
  }

  // @unsafe - Get storage (may be nullptr)
  std::shared_ptr<LogStorage> get_storage() const {
    return storage_;
  }

  /**
   * Generic recovery method that works with any server type.
   *
   * @param set_storage Function to call to set the storage on the server
   * @param recover Function to call to recover state from storage
   * @param get_stats Function to call to get recovery statistics
   * @return RecoveryResult with statistics
   */
  template <typename SetStorageFn, typename RecoverFn, typename GetStatsFn>
  RecoveryResult recover(SetStorageFn set_storage, RecoverFn recover, GetStatsFn get_stats) {
    RecoveryResult result = recovery_result_begin(detected_mode_.get());

    auto start_time = std::chrono::steady_clock::now();

    // Fresh start: nothing to recover
    if (recovery_mode_is_fresh_start(result.mode)) {
      // Set storage for future persistence
      if (recovery_fresh_start_should_attach_storage(result.mode, storage_ != nullptr)) {
        set_storage(storage_);
      }
      recovery_result_mark_fresh_success(result);
      return result;
    }

    // Normal recovery
    if (recovery_normal_storage_missing(result.mode, storage_ != nullptr)) {
      return RecoveryResult::failure("Storage not initialized");
    }

    // Set storage first
    set_storage(storage_);

    // Recover state
    if (recovery_replay_failed(recover())) {
      return RecoveryResult::failure("RecoverFromStorage failed");
    }

    // Get statistics
    uint64_t recovered_entries = storage_->size();
    result.recovered_entries = recovered_entries;
    get_stats(result);

    auto end_time = std::chrono::steady_clock::now();
    uint64_t recovery_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    recovery_result_assign_recovery_time(result, recovery_time_ms);
    recovery_result_mark_success(result, recovered_entries, recovery_time_ms);
    Log_info("Recovery complete: %lu entries in %lu ms",
             result.recovered_entries, result.recovery_time_ms);
    return result;
  }

 private:
  RecoveryConfig config_;
  std::shared_ptr<LogStorage> storage_;
  rusty::Cell<bool> initialized_;
  rusty::Cell<RecoveryMode> detected_mode_;
};

}  // namespace raft
}  // namespace janus
