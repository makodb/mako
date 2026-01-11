/**
 * @file config_store.cc
 * @brief Implementation of RocksDB-backed configuration storage.
 */

#include "config_store.h"
#include "../rrr/base/logging.hpp"

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
        Log_error("ConfigStore: Failed to open database at %s: %s",
                  db_path_.c_str(), status.ToString().c_str());  // @unsafe
        return false;
    }

    is_open_.set(true);
    Log_info("ConfigStore: Opened database at %s", db_path_.c_str());  // @unsafe
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
    Log_info("ConfigStore: Closed database at %s", db_path_.c_str());  // @unsafe
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
        Log_error("ConfigStore: Cannot save - database not open");  // @unsafe
        return false;
    }

    // Use a write batch for atomic writes
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
        m << size;  // @unsafe
        for (const auto& site : config.sites) {
            m << site;  // @unsafe
        }
        std::string sites_str;
        serialize_to_string(m, &sites_str);
        batch.Put(config_keys::SITES, sites_str);
    }

    // Serialize and write replica groups
    {
        rrr::Marshal m;
        uint32_t size = static_cast<uint32_t>(config.replica_groups.size());
        m << size;  // @unsafe
        for (const auto& group : config.replica_groups) {
            m << group;  // @unsafe
        }
        std::string replicas_str;
        serialize_to_string(m, &replicas_str);
        batch.Put(config_keys::REPLICAS, replicas_str);
    }

    // Serialize and write settings
    {
        rrr::Marshal m;
        m << config.settings;  // @unsafe
        std::string settings_str;
        serialize_to_string(m, &settings_str);
        batch.Put(config_keys::SETTINGS, settings_str);
    }

    // Write batch atomically
    // @unsafe { RocksDB Write is not borrow-checked }
    rocksdb::Status status = db_->Write(write_options_, &batch);
    if (!status.ok()) {
        Log_error("ConfigStore: Failed to save configuration: %s",
                  status.ToString().c_str());  // @unsafe
        return false;
    }

    Log_info("ConfigStore: Saved configuration version %lu", config.version);  // @unsafe
    return true;
}

// @unsafe - RocksDB I/O
rusty::Option<PersistentConfig> ConfigStore::load() {
    if (!is_open_.get()) {
        Log_error("ConfigStore: Cannot load - database not open");  // @unsafe
        return rusty::None;
    }

    PersistentConfig config;

    // Read version
    {
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::VERSION, &value);
        if (!status.ok()) {
            if (status.IsNotFound()) {
                Log_debug("ConfigStore: No configuration found");  // @unsafe
            } else {
                Log_error("ConfigStore: Failed to read version: %s",
                          status.ToString().c_str());  // @unsafe
            }
            return rusty::None;
        }
        if (value.size() != sizeof(uint64_t)) {
            Log_error("ConfigStore: Invalid version data size");  // @unsafe
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
            Log_error("ConfigStore: Failed to read sites: %s",
                      status.ToString().c_str());  // @unsafe
            return rusty::None;
        }
        rrr::Marshal m;
        deserialize_from_string(value, &m);
        uint32_t size;
        m >> size;  // @unsafe
        config.sites.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            m >> config.sites[i];  // @unsafe
        }
    }

    // Read replica groups
    {
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::REPLICAS, &value);
        if (!status.ok()) {
            Log_error("ConfigStore: Failed to read replica groups: %s",
                      status.ToString().c_str());  // @unsafe
            return rusty::None;
        }
        rrr::Marshal m;
        deserialize_from_string(value, &m);
        uint32_t size;
        m >> size;  // @unsafe
        config.replica_groups.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
            m >> config.replica_groups[i];  // @unsafe
        }
    }

    // Read settings
    {
        std::string value;
        // @unsafe { RocksDB Get is not borrow-checked }
        rocksdb::Status status = db_->Get(read_options_, config_keys::SETTINGS, &value);
        if (!status.ok()) {
            Log_error("ConfigStore: Failed to read settings: %s",
                      status.ToString().c_str());  // @unsafe
            return rusty::None;
        }
        rrr::Marshal m;
        deserialize_from_string(value, &m);
        m >> config.settings;  // @unsafe
    }

    Log_info("ConfigStore: Loaded configuration version %lu with %zu sites and %zu replica groups",
             config.version, config.sites.size(), config.replica_groups.size());  // @unsafe

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

}  // namespace janus
