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
#include <filesystem>
#include <memory>
#include <string>

#include <rusty/cell.hpp>

#include "log_storage.hpp"
#include "rocksdb_log_storage.hpp"
#include "rrr/rrr.hpp"

namespace janus {
namespace raft {

/**
 * Mode of operation for recovery.
 */
// @safe - Simple enum
enum class RecoveryMode {
  FRESH_START,      // No previous state, start fresh
  NORMAL_RECOVERY,  // Previous state found, recover from storage
  FORCED_FRESH      // User requested fresh start even if data exists
};

/**
 * Configuration for recovery behavior.
 */
// @safe - POD struct
struct RecoveryConfig {
  std::string storage_path;              // Path for RocksDB storage
  bool force_fresh_start{false};         // Force fresh start even if data exists
  uint32_t recovery_timeout_ms{30000};   // Timeout for recovery operations
  bool verify_on_recovery{true};         // Verify data integrity after recovery
  bool clear_on_forced_fresh{true};      // Clear storage when forcing fresh start

  // @unsafe - Returns struct by value
  static RecoveryConfig defaults() {
    return RecoveryConfig{};  // @unsafe
  }

  // @unsafe - Uses getenv and string operations
  static RecoveryConfig for_replica(uint32_t partition_id, uint32_t locale_id) {
    RecoveryConfig config;
    // Use username prefix to avoid conflicts between users
    std::string username;
    // @unsafe { getenv is not borrow-checked }
    auto user = std::getenv("USER");  // @unsafe
    if (user) {
      username = user;  // @unsafe
    } else {
      username = "unknown";  // @unsafe
    }
    config.storage_path = "/tmp/" + username + "_mako_log_shard" +
                         std::to_string(partition_id) + "_replica" +
                         std::to_string(locale_id);
    return config;  // @unsafe
  }
};

/**
 * Results from a recovery operation.
 */
// @safe - POD struct
struct RecoveryResult {
  RecoveryMode mode{RecoveryMode::FRESH_START};
  bool success{false};
  std::string error_message;
  uint64_t recovered_entries{0};
  uint64_t recovered_term{0};      // For Raft: currentTerm
  uint64_t recovered_epoch{0};     // For Paxos: cur_epoch
  uint64_t recovery_time_ms{0};

  // @unsafe - constructs RecoveryResult with std::string member
  static RecoveryResult success_fresh() {
    RecoveryResult result; // @unsafe { std::string default construction }
    result.mode = RecoveryMode::FRESH_START;
    result.success = true;
    return result;
  }

  // @unsafe - String assignment
  static RecoveryResult failure(const std::string& error) {
    RecoveryResult result;
    result.success = false;
    result.error_message = error;  // @unsafe
    return result;  // @unsafe
  }
};

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
    if (detected_mode_.get() == RecoveryMode::FORCED_FRESH &&
        config_.clear_on_forced_fresh) {
      // @unsafe { filesystem operations }
      std::error_code ec;
      std::filesystem::remove_all(config_.storage_path, ec);
      if (ec) {
        Log_error("Failed to clear storage at {}: {}",
                  config_.storage_path.c_str(), ec.message().c_str());
      }
    }

    // Create storage
    storage_ = std::make_shared<RocksDBLogStorage>(config_.storage_path);
    if (!storage_->is_open()) {
      Log_error("Failed to open RocksDB at {}", config_.storage_path.c_str());
      storage_ = nullptr;
      return nullptr;
    }

    initialized_.set(true);
    Log_info("Recovery: Storage opened at {} (mode={})",
             config_.storage_path.c_str(), static_cast<int>(detected_mode_.get()));
    return storage_;
  }

  // @lifetime: (&'a) -> &'a
  const std::string& storage_path() const {
    return config_.storage_path;
  }

  // @safe - Check if recovery is needed (vs fresh start)
  bool needs_recovery() const {
    return detected_mode_.get() == RecoveryMode::NORMAL_RECOVERY;
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
    RecoveryResult result;
    result.mode = detected_mode_.get();

    auto start_time = std::chrono::steady_clock::now();

    // Fresh start: nothing to recover
    if (result.mode == RecoveryMode::FRESH_START ||
        result.mode == RecoveryMode::FORCED_FRESH) {
      // Set storage for future persistence
      if (storage_) {
        set_storage(storage_);
      }
      result.success = true;
      result.recovered_entries = 0;
      return result;
    }

    // Normal recovery
    if (!storage_) {
      return RecoveryResult::failure("Storage not initialized");
    }

    // Set storage first
    set_storage(storage_);

    // Recover state
    if (!recover()) {
      return RecoveryResult::failure("RecoverFromStorage failed");
    }

    // Get statistics
    result.recovered_entries = storage_->size();
    get_stats(result);

    auto end_time = std::chrono::steady_clock::now();
    result.recovery_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    result.success = true;
    Log_info("Recovery complete: {} entries in {} ms",
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
