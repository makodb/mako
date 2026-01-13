/**
 * @file config_store.cc
 * @brief Implementation of RocksDB-backed configuration storage.
 */

#include "config_store.h"
#include "../rrr/base/logging.hpp"
#include <cstring>

namespace janus {

// @safe - Constructor, no side effects
ConfigStore::ConfigStore(const std::string& db_path)
    : db_path_(db_path) {
    // Configure RocksDB options
    options_.create_if_missing = true;
    options_.error_if_exists = false;

    // Use synchronous writes for durability
    write_options_.sync = true;
}

// @unsafe - Calls RocksDB delete
ConfigStore::~ConfigStore() {
    close();
}

// @unsafe - RocksDB I/O
bool ConfigStore::open() {
    if (is_open_.get()) {
        return true;  // Already open
    }

    // @unsafe { RocksDB Open is not borrow-checked }
    rocksdb::Status status = rocksdb::DB::Open(options_, db_path_, &db_);
    if (!status.ok()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to open database at %s: %s",
                  db_path_.c_str(), status.ToString().c_str());
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
        // @unsafe { RocksDB delete is not borrow-checked }
        delete db_;
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

    // @unsafe { RocksDB WriteBatch is not borrow-checked }
    rocksdb::WriteBatch batch;

    // Serialize and write version
    {
        std::string version_str(sizeof(uint64_t), '\0');
        uint64_t version = config.version;
        // @unsafe { memcpy is not borrow-checked }
        std::memcpy(version_str.data(), &version, sizeof(uint64_t));
        batch.Put(config_keys::VERSION, version_str);
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
        batch.Put(config_keys::SITES, sites_str);
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
        batch.Put(config_keys::REPLICAS, replicas_str);
    }

    // Serialize and write settings
    {
        rrr::Marshal m;
        // @unsafe { Marshal write not borrow-checked }
        m << config.settings;
        std::string settings_str;
        serialize_to_string(m, &settings_str);
        batch.Put(config_keys::SETTINGS, settings_str);
    }

    // @unsafe { RocksDB Write is not borrow-checked }
    rocksdb::Status status = db_->Write(write_options_, &batch);
    if (!status.ok()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to save configuration: %s",
                  status.ToString().c_str());
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
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::VERSION, &value);
        if (!status.ok()) {
            // @unsafe { logging I/O }
            if (status.IsNotFound()) {
                Log_debug("ConfigStore: No configuration found");
            } else {
                Log_error("ConfigStore: Failed to read version: %s",
                          status.ToString().c_str());
            }
            return rusty::None;
        }
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
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::SITES, &value);
        if (!status.ok()) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read sites: %s",
                      status.ToString().c_str());
            return rusty::None;
        }
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
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::REPLICAS, &value);
        if (!status.ok()) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read replica groups: %s",
                      status.ToString().c_str());
            return rusty::None;
        }
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
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::SETTINGS, &value);
        if (!status.ok()) {
            // @unsafe { logging I/O }
            Log_error("ConfigStore: Failed to read settings: %s",
                      status.ToString().c_str());
            return rusty::None;
        }
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

    std::string value;
    // @unsafe { RocksDB Get is not borrow-checked }
    rocksdb::Status status = db_->Get(read_options_, config_keys::VERSION, &value);
    if (!status.ok() || value.size() != sizeof(uint64_t)) {
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

    std::string value;
    // @unsafe { RocksDB Get is not borrow-checked }
    rocksdb::Status status = db_->Get(read_options_, config_keys::VERSION, &value);
    return status.ok();
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

    // @unsafe { RocksDB WriteBatch is not borrow-checked }
    rocksdb::WriteBatch batch;

    // Serialize and write version
    {
        std::string version_str(sizeof(uint64_t), '\0');
        uint64_t version = policy.version;
        // @unsafe { memcpy is not borrow-checked }
        std::memcpy(version_str.data(), &version, sizeof(uint64_t));
        batch.Put(sharding_keys::VERSION, version_str);
    }

    // Serialize and write the full policy
    {
        rrr::Marshal m;
        // @unsafe { Marshal write not borrow-checked }
        m << policy;
        std::string policy_str;
        serialize_to_string(m, &policy_str);
        batch.Put(sharding_keys::POLICY, policy_str);
    }

    // @unsafe { RocksDB Write is not borrow-checked }
    rocksdb::Status status = db_->Write(write_options_, &batch);
    if (!status.ok()) {
        // @unsafe { logging I/O }
        Log_error("ConfigStore: Failed to save sharding policy: %s",
                  status.ToString().c_str());
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
    std::string value;
    // @unsafe { RocksDB Get is not borrow-checked }
    rocksdb::Status status = db_->Get(read_options_, sharding_keys::POLICY, &value);
    if (!status.ok()) {
        // @unsafe { logging I/O }
        if (status.IsNotFound()) {
            Log_debug("ConfigStore: No sharding policy found");
        } else {
            Log_error("ConfigStore: Failed to read sharding policy: %s",
                      status.ToString().c_str());
        }
        return rusty::None;
    }

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

    std::string value;
    // @unsafe { RocksDB Get is not borrow-checked }
    rocksdb::Status status = db_->Get(read_options_, sharding_keys::VERSION, &value);
    if (!status.ok() || value.size() != sizeof(uint64_t)) {
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

    std::string value;
    // @unsafe { RocksDB Get is not borrow-checked }
    rocksdb::Status status = db_->Get(read_options_, sharding_keys::VERSION, &value);
    return status.ok();
}

}  // namespace janus
