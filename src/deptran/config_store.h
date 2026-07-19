#pragma once

/**
 * @file config_store.h
 * @brief RocksDB-backed configuration storage for C-Node.
 *
 * The ConfigStore persists cluster configuration to RocksDB, allowing
 * the C-Node to recover configuration after restarts without needing
 * the original YAML files.
 */

#include <string>
#include <cstdint>

#include <rocksdb/c.h>

#include <rusty/cell.hpp>
#include <rusty/option.hpp>

#include "config_schema.h"
#include "sharding_policy.h"

namespace janus {

/**
 * @brief RocksDB-backed configuration storage.
 *
 * Used by the C-Node to:
 * 1. Save configuration from YAML on first boot
 * 2. Recover configuration from RocksDB on subsequent boots
 * 3. Serve configuration to other nodes via RPC
 *
 * Thread-safe: RocksDB provides internal thread safety.
 */
class ConfigStore {
private:
    // Database handle (raw pointer, RocksDB manages memory)
    rocksdb_t* db_{nullptr};  // @unsafe - Raw pointer managed by RocksDB C API
    std::string db_path_;

    // Configuration
    rocksdb_options_t* options_{nullptr};
    rocksdb_writeoptions_t* write_options_{nullptr};
    rocksdb_readoptions_t* read_options_{nullptr};

    // State
    rusty::Cell<bool> is_open_{false};

public:
    /**
     * Construct a ConfigStore with the specified database path.
     * @param db_path Path to the RocksDB database directory
     */
    // @safe - Constructor, no side effects
    explicit ConfigStore(const std::string& db_path);

    /**
     * Destructor - closes database if open.
     */
    // @unsafe - Calls RocksDB delete
    ~ConfigStore();

    // Disable copy
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    /**
     * Open the database.
     * Creates the database if it doesn't exist.
     * @return true on success, false on failure
     */
    // @unsafe - RocksDB I/O
    bool open();

    /**
     * Close the database.
     */
    // @unsafe - RocksDB I/O
    void close();

    /**
     * Check if the database is open.
     * @return true if open, false otherwise
     */
    // @safe - Read from Cell
    bool get_is_open() const { return is_open_.get(); }

    /**
     * Save configuration to RocksDB.
     * Increments the version number automatically.
     * @param config The configuration to save
     * @return true on success, false on failure
     */
    // @unsafe - RocksDB I/O
    bool save(const PersistentConfig& config);

    /**
     * Load configuration from RocksDB.
     * @return Some(config) if found, None if not found or error
     */
    // @unsafe - RocksDB I/O
    rusty::Option<PersistentConfig> load();

    /**
     * Get the current configuration version without loading the full config.
     * @return The version number, or 0 if not found
     */
    // @unsafe - RocksDB I/O
    uint64_t get_version();

    /**
     * Check if configuration exists in the database.
     * @return true if configuration exists, false otherwise
     */
    // @unsafe - RocksDB I/O
    bool has_config();

    // ========================================================================
    // Sharding Policy Storage
    // ========================================================================

    /**
     * Save sharding policy to RocksDB.
     * @param policy The sharding policy set to save
     * @return true on success, false on failure
     */
    // @unsafe - RocksDB I/O
    bool save_sharding_policy(const ShardingPolicySet& policy);

    /**
     * Load sharding policy from RocksDB.
     * @return Some(policy) if found, None if not found or error
     */
    // @unsafe - RocksDB I/O
    rusty::Option<ShardingPolicySet> load_sharding_policy();

    /**
     * Get the current sharding policy version without loading the full policy.
     * @return The version number, or 0 if not found
     */
    // @unsafe - RocksDB I/O
    uint64_t get_sharding_policy_version();

    /**
     * Check if a sharding policy exists in the database.
     * @return true if policy exists, false otherwise
     */
    // @unsafe - RocksDB I/O
    bool has_sharding_policy();
};

}  // namespace janus
