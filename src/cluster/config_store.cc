/**
 * @file config_store.cc
 * @brief Implementation of RocksDB-backed configuration storage.
 */

#include <stdint.h>
#include <stddef.h>

#include "config_store.h"

#include "rrr/rrr.hpp"

import std;

namespace janus {

using rrr::Log_debug;
using rrr::Log_info;
using rrr::Log_warn;
using rrr::Log_error;


namespace {

// @unsafe - Uses RocksDB C API allocation semantics
std::string take_rocksdb_error(char** errptr) {
    if (errptr == nullptr || *errptr == nullptr) {
        return "";
    }
    std::string err(*errptr);
    rocksdb_free(*errptr);
    *errptr = nullptr;
    return err;
}

// @safe - std::string copy from byte buffer
std::string copy_db_value(const char* data, size_t len) {
    if (data == nullptr || len == 0) {
        return "";
    }
    return std::string(data, len);
}

}  // namespace

// @safe - Constructor, no side effects
ConfigStore::ConfigStore(const std::string& db_path)
    : db_path_(db_path) {
    options_ = rocksdb_options_create();
    write_options_ = rocksdb_writeoptions_create();
    read_options_ = rocksdb_readoptions_create();

    if (options_ == nullptr || write_options_ == nullptr || read_options_ == nullptr) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to allocate RocksDB C API options");
        return;
    }

    // Configure RocksDB options
    rocksdb_options_set_create_if_missing(options_, 1);
    rocksdb_options_set_error_if_exists(options_, 0);

    // Use synchronous writes for durability
    rocksdb_writeoptions_set_sync(write_options_, 1);
}

// @unsafe - Calls RocksDB delete
ConfigStore::~ConfigStore() {
    close();

    if (read_options_ != nullptr) {
        rocksdb_readoptions_destroy(read_options_);
        read_options_ = nullptr;
    }
    if (write_options_ != nullptr) {
        rocksdb_writeoptions_destroy(write_options_);
        write_options_ = nullptr;
    }
    if (options_ != nullptr) {
        rocksdb_options_destroy(options_);
        options_ = nullptr;
    }
}

// @unsafe - RocksDB I/O
bool ConfigStore::open() {
    if (is_open_.get()) {
        return true;  // Already open
    }

    if (options_ == nullptr || write_options_ == nullptr || read_options_ == nullptr) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Cannot open database, options not initialized");
        return false;
    }

    char* err = nullptr;
    // @unsafe { RocksDB C API open is not borrow-checked }
    db_ = rocksdb_open(options_, db_path_.c_str(), &err);
    if (err != nullptr) {
        std::string err_str = take_rocksdb_error(&err);
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to open database at %s: %s",
                  db_path_.c_str(), err_str.c_str());
        db_ = nullptr;
        return false;
    }
    if (db_ == nullptr) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to open database at %s (null handle)",
                  db_path_.c_str());
        return false;
    }

    is_open_.set(true);
    // @unsafe { logging I/O }
    Log_info("ConfigStore: Opened database at %s", db_path_.c_str());
    return true;
}

// @unsafe - RocksDB I/O
void ConfigStore::close() {
    if (!is_open_.get()) {
        return;  // Already closed
    }

    if (db_ != nullptr) {
        // @unsafe { RocksDB C API close is not borrow-checked }
        rocksdb_close(db_);
        db_ = nullptr;
    }

    is_open_.set(false);
    // @unsafe { logging I/O }
    Log_info("ConfigStore: Closed database at %s", db_path_.c_str());
}

// @unsafe - Marshal operations
bool ConfigStore::serialize_to_string(const rrr::Marshal& m, std::string* out) const {
    size_t size = m.content_size();
    out->resize(size);
    // @unsafe { Marshal read is not borrow-checked }
    const_cast<rrr::Marshal&>(m).read(out->data(), size);
    return true;
}

// @unsafe - Marshal operations
bool ConfigStore::deserialize_from_string(const std::string& data, rrr::Marshal* m) const {
    // @unsafe { Marshal write is not borrow-checked }
    m->write(data.data(), data.size());
    return true;
}

// @unsafe - RocksDB I/O
bool ConfigStore::save(const PersistentConfig& config) {
    if (!is_open_.get()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Cannot save - database not open");
        return false;
    }

    // @unsafe { RocksDB C API WriteBatch is not borrow-checked }
    rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
    if (batch == nullptr) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to create write batch");
        return false;
    }

    // Serialize and write version
    {
        std::string version_str(sizeof(uint64_t), '\0');
        uint64_t version = config.version;
        // @unsafe { memcpy is not borrow-checked }
        std::memcpy(version_str.data(), &version, sizeof(uint64_t));
        rocksdb_writebatch_put(batch,
                               config_keys::VERSION, std::strlen(config_keys::VERSION),
                               version_str.data(), version_str.size());
    }

    // Serialize and write sites
    {
        rrr::Marshal m;
        uint32_t size = static_cast<uint32_t>(config.sites.size());
        // @unsafe { Marshal write not borrow-checked }
        m << size;
        for (const auto& site : config.sites) {
            m << site;
        }
        std::string sites_str;
        serialize_to_string(m, &sites_str);
        rocksdb_writebatch_put(batch,
                               config_keys::SITES, std::strlen(config_keys::SITES),
                               sites_str.data(), sites_str.size());
    }

    // Serialize and write replica groups
    {
        rrr::Marshal m;
        uint32_t size = static_cast<uint32_t>(config.replica_groups.size());
        // @unsafe { Marshal write not borrow-checked }
        m << size;
        for (const auto& group : config.replica_groups) {
            m << group;
        }
        std::string replicas_str;
        serialize_to_string(m, &replicas_str);
        rocksdb_writebatch_put(batch,
                               config_keys::REPLICAS, std::strlen(config_keys::REPLICAS),
                               replicas_str.data(), replicas_str.size());
    }

    // Serialize and write settings
    {
        rrr::Marshal m;
        // @unsafe { Marshal write not borrow-checked }
        m << config.settings;
        std::string settings_str;
        serialize_to_string(m, &settings_str);
        rocksdb_writebatch_put(batch,
                               config_keys::SETTINGS, std::strlen(config_keys::SETTINGS),
                               settings_str.data(), settings_str.size());
    }

    char* err = nullptr;
    // @unsafe { RocksDB C API Write is not borrow-checked }
    rocksdb_write(db_, write_options_, batch, &err);
    rocksdb_writebatch_destroy(batch);
    if (err != nullptr) {
        std::string err_str = take_rocksdb_error(&err);
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to save configuration: %s",
                  err_str.c_str());
        return false;
    }

    // @unsafe { logging I/O }
    Log_info("ConfigStore: Saved configuration version %lu", config.version);
    return true;
}

// @unsafe - RocksDB I/O
rusty::Option<PersistentConfig> ConfigStore::load() {
    if (!is_open_.get()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Cannot load - database not open");
        return rusty::None;
    }

    PersistentConfig config;

    // Read version
    {
        size_t value_len = 0;
        char* err = nullptr;
        // @unsafe { RocksDB C API Get is not borrow-checked }
        char* value_ptr = rocksdb_get(db_, read_options_, config_keys::VERSION,
                                      std::strlen(config_keys::VERSION), &value_len, &err);
        if (err != nullptr) {
            std::string err_str = take_rocksdb_error(&err);
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read version: %s", err_str.c_str());
            return rusty::None;
        }
        if (value_ptr == nullptr) {
            // @unsafe { logging I/O }
            Log_debug("ConfigStore: No configuration found");
            return rusty::None;
        }

        std::string value = copy_db_value(value_ptr, value_len);
        rocksdb_free(value_ptr);

        if (value.size() != sizeof(uint64_t)) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Invalid version data size");
            return rusty::None;
        }
        // @unsafe { memcpy is not borrow-checked }
        std::memcpy(&config.version, value.data(), sizeof(uint64_t));
    }

    // Read sites
    {
        size_t value_len = 0;
        char* err = nullptr;
        // @unsafe { RocksDB C API Get is not borrow-checked }
        char* value_ptr = rocksdb_get(db_, read_options_, config_keys::SITES,
                                      std::strlen(config_keys::SITES), &value_len, &err);
        if (err != nullptr) {
            std::string err_str = take_rocksdb_error(&err);
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read sites: %s", err_str.c_str());
            return rusty::None;
        }
        if (value_ptr == nullptr) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read sites: not found");
            return rusty::None;
        }
        std::string value = copy_db_value(value_ptr, value_len);
        rocksdb_free(value_ptr);
        rrr::Marshal m;
        deserialize_from_string(value, &m);
        uint32_t size;
        // @unsafe { Marshal read not borrow-checked }
        m >> size;
        config.sites.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            m >> config.sites[i];
        }
    }

    // Read replica groups
    {
        size_t value_len = 0;
        char* err = nullptr;
        // @unsafe { RocksDB C API Get is not borrow-checked }
        char* value_ptr = rocksdb_get(db_, read_options_, config_keys::REPLICAS,
                                      std::strlen(config_keys::REPLICAS), &value_len, &err);
        if (err != nullptr) {
            std::string err_str = take_rocksdb_error(&err);
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read replica groups: %s", err_str.c_str());
            return rusty::None;
        }
        if (value_ptr == nullptr) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read replica groups: not found");
            return rusty::None;
        }
        std::string value = copy_db_value(value_ptr, value_len);
        rocksdb_free(value_ptr);
        rrr::Marshal m;
        deserialize_from_string(value, &m);
        uint32_t size;
        // @unsafe { Marshal read not borrow-checked }
        m >> size;
        config.replica_groups.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            m >> config.replica_groups[i];
        }
    }

    // Read settings
    {
        size_t value_len = 0;
        char* err = nullptr;
        // @unsafe { RocksDB C API Get is not borrow-checked }
        char* value_ptr = rocksdb_get(db_, read_options_, config_keys::SETTINGS,
                                      std::strlen(config_keys::SETTINGS), &value_len, &err);
        if (err != nullptr) {
            std::string err_str = take_rocksdb_error(&err);
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read settings: %s", err_str.c_str());
            return rusty::None;
        }
        if (value_ptr == nullptr) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read settings: not found");
            return rusty::None;
        }
        std::string value = copy_db_value(value_ptr, value_len);
        rocksdb_free(value_ptr);
        rrr::Marshal m;
        deserialize_from_string(value, &m);
        // @unsafe { Marshal read not borrow-checked }
        m >> config.settings;
    }

    // @unsafe { logging I/O }
    Log_info("ConfigStore: Loaded configuration version %lu with %zu sites and %zu replica groups",
             config.version, config.sites.size(), config.replica_groups.size());

    return rusty::Some(std::move(config));
}

// @unsafe - RocksDB I/O
uint64_t ConfigStore::get_version() {
    if (!is_open_.get()) {
        return 0;
    }

    size_t value_len = 0;
    char* err = nullptr;
    // @unsafe { RocksDB C API Get is not borrow-checked }
    char* value_ptr = rocksdb_get(db_, read_options_, config_keys::VERSION,
                                  std::strlen(config_keys::VERSION), &value_len, &err);
    if (err != nullptr) {
        take_rocksdb_error(&err);
        return 0;
    }
    if (value_ptr == nullptr) {
        return 0;
    }
    std::string value = copy_db_value(value_ptr, value_len);
    rocksdb_free(value_ptr);

    if (value.size() != sizeof(uint64_t)) {
        return 0;
    }

    uint64_t version;
    // @unsafe { memcpy is not borrow-checked }
    std::memcpy(&version, value.data(), sizeof(uint64_t));
    return version;
}

// @unsafe - RocksDB I/O
bool ConfigStore::has_config() {
    if (!is_open_.get()) {
        return false;
    }

    size_t value_len = 0;
    char* err = nullptr;
    // @unsafe { RocksDB C API Get is not borrow-checked }
    char* value_ptr = rocksdb_get(db_, read_options_, config_keys::VERSION,
                                  std::strlen(config_keys::VERSION), &value_len, &err);
    if (err != nullptr) {
        take_rocksdb_error(&err);
        return false;
    }
    if (value_ptr != nullptr) {
        rocksdb_free(value_ptr);
        return true;
    }
    return false;
}

// ============================================================================
// Sharding Policy Storage Implementation
// ============================================================================

// @unsafe - RocksDB I/O
bool ConfigStore::save_sharding_policy(const ShardingPolicySet& policy) {
    if (!is_open_.get()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Cannot save sharding policy - database not open");
        return false;
    }

    // @unsafe { RocksDB C API WriteBatch is not borrow-checked }
    rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
    if (batch == nullptr) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to create write batch");
        return false;
    }

    // Serialize and write version
    {
        std::string version_str(sizeof(uint64_t), '\0');
        uint64_t version = policy.version;
        // @unsafe { memcpy is not borrow-checked }
        std::memcpy(version_str.data(), &version, sizeof(uint64_t));
        rocksdb_writebatch_put(batch,
                               sharding_keys::VERSION, std::strlen(sharding_keys::VERSION),
                               version_str.data(), version_str.size());
    }

    // Serialize and write the full policy
    {
        rrr::Marshal m;
        // @unsafe { Marshal write not borrow-checked }
        m << policy;
        std::string policy_str;
        serialize_to_string(m, &policy_str);
        rocksdb_writebatch_put(batch,
                               sharding_keys::POLICY, std::strlen(sharding_keys::POLICY),
                               policy_str.data(), policy_str.size());
    }

    char* err = nullptr;
    // @unsafe { RocksDB C API Write is not borrow-checked }
    rocksdb_write(db_, write_options_, batch, &err);
    rocksdb_writebatch_destroy(batch);
    if (err != nullptr) {
        std::string err_str = take_rocksdb_error(&err);
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to save sharding policy: %s",
                  err_str.c_str());
        return false;
    }

    // @unsafe { logging I/O }
    Log_info("ConfigStore: Saved sharding policy version %lu with %zu tables",
             policy.version, policy.table_count());
    return true;
}

// @unsafe - RocksDB I/O
rusty::Option<ShardingPolicySet> ConfigStore::load_sharding_policy() {
    if (!is_open_.get()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Cannot load sharding policy - database not open");
        return rusty::None;
    }

    // Read the full policy (version is inside ShardingPolicySet)
    size_t value_len = 0;
    char* err = nullptr;
    // @unsafe { RocksDB C API Get is not borrow-checked }
    char* value_ptr = rocksdb_get(db_, read_options_, sharding_keys::POLICY,
                                  std::strlen(sharding_keys::POLICY), &value_len, &err);
    if (err != nullptr) {
        std::string err_str = take_rocksdb_error(&err);
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to read sharding policy: %s",
                  err_str.c_str());
        return rusty::None;
    }
    if (value_ptr == nullptr) {
        // @unsafe { logging I/O }
        Log_debug("ConfigStore: No sharding policy found");
        return rusty::None;
    }
    std::string value = copy_db_value(value_ptr, value_len);
    rocksdb_free(value_ptr);

    // Deserialize
    rrr::Marshal m;
    deserialize_from_string(value, &m);
    ShardingPolicySet policy;
    // @unsafe { Marshal read not borrow-checked }
    m >> policy;

    // @unsafe { logging I/O }
    Log_info("ConfigStore: Loaded sharding policy version %lu with %zu tables",
             policy.version, policy.table_count());

    return rusty::Some(std::move(policy));
}

// @unsafe - RocksDB I/O
uint64_t ConfigStore::get_sharding_policy_version() {
    if (!is_open_.get()) {
        return 0;
    }

    size_t value_len = 0;
    char* err = nullptr;
    // @unsafe { RocksDB C API Get is not borrow-checked }
    char* value_ptr = rocksdb_get(db_, read_options_, sharding_keys::VERSION,
                                  std::strlen(sharding_keys::VERSION), &value_len, &err);
    if (err != nullptr) {
        take_rocksdb_error(&err);
        return 0;
    }
    if (value_ptr == nullptr) {
        return 0;
    }
    std::string value = copy_db_value(value_ptr, value_len);
    rocksdb_free(value_ptr);

    if (value.size() != sizeof(uint64_t)) {
        return 0;
    }

    uint64_t version;
    // @unsafe { memcpy is not borrow-checked }
    std::memcpy(&version, value.data(), sizeof(uint64_t));
    return version;
}

// @unsafe - RocksDB I/O
bool ConfigStore::has_sharding_policy() {
    if (!is_open_.get()) {
        return false;
    }

    size_t value_len = 0;
    char* err = nullptr;
    // @unsafe { RocksDB C API Get is not borrow-checked }
    char* value_ptr = rocksdb_get(db_, read_options_, sharding_keys::VERSION,
                                  std::strlen(sharding_keys::VERSION), &value_len, &err);
    if (err != nullptr) {
        take_rocksdb_error(&err);
        return false;
    }
    if (value_ptr != nullptr) {
        rocksdb_free(value_ptr);
        return true;
    }
    return false;
}

}  // namespace janus
