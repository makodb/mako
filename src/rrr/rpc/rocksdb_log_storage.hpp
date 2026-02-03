#pragma once

/**
 * RocksDB Log Storage Implementation
 *
 * Persistent implementation of LogStorage for Raft/Paxos consensus logs.
 * Uses RocksDB as the underlying storage engine.
 *
 * RustyCpp Compliance: Uses rusty::Cell, rusty::Option
 * Note: RocksDB operations are marked @unsafe (C++ library, not borrow-checked)
 */

#include <string>
#include <vector>
#include <iomanip>
#include <sstream>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <rusty/cell.hpp>
#include <rusty/option.hpp>

#include "log_storage.hpp"
#include "misc/marshal.hpp"

namespace rrr {

/**
 * RocksDB-backed implementation of LogStorage.
 *
 * Suitable for:
 * - Production deployments requiring durability
 * - Node crash recovery with state restoration
 *
 * Key format:
 * - Log entries: "log:{20-digit padded slot_id}"
 * - Metadata: "meta:{key}"
 *
 * Thread-safe: RocksDB provides internal thread safety.
 */
class RocksDBLogStorage : public LogStorage {
private:
    // Database handle (raw pointer, RocksDB manages memory)
    rocksdb::DB* db_{nullptr};  // @unsafe - Raw pointer managed by RocksDB API
    std::string db_path_;

    // Configuration
    rocksdb::Options options_;
    rocksdb::WriteOptions write_options_;
    rocksdb::ReadOptions read_options_;

    // State
    rusty::Cell<bool> is_open_{false};

    // Key prefixes
    static constexpr const char* LOG_PREFIX = "log:";
    static constexpr const char* META_PREFIX = "meta:";

    // @unsafe - Uses ostringstream operations
    std::string make_log_key(slotid_t slot_id) const {
        std::ostringstream ss;
        ss << LOG_PREFIX << std::setfill('0') << std::setw(20) << slot_id;  // @unsafe
        return ss.str();  // @unsafe
    }

    // @unsafe - String concatenation
    std::string make_meta_key(const std::string& key) const {
        return std::string(META_PREFIX) + key;  // @unsafe
    }

    // @unsafe - Uses Marshal which has non-borrow-checked operations
    bool serialize_entry(const LogEntry& entry, std::string* out) const {
        Marshal m;
        const_cast<LogEntry&>(entry).to_marshal(m);  // @unsafe
        size_t size = m.content_size();
        out->resize(size);
        m.read(out->data(), size);  // @unsafe - read Marshal contents into string
        return true;
    }

    // @unsafe - Uses Marshal which has non-borrow-checked operations
    bool deserialize_entry(const std::string& data, LogEntry* out) const {
        Marshal m;
        m.write(data.data(), data.size());  // @unsafe - write string bytes into Marshal
        out->from_marshal(m);  // @unsafe
        return true;
    }

public:
    /**
     * Construct a RocksDB log storage.
     * @param db_path Path to the database directory
     */
    // @safe - Constructor, no side effects beyond initialization
    explicit RocksDBLogStorage(const std::string& db_path)
        : db_path_(db_path) {
        // Configure RocksDB options
        options_.create_if_missing = true;
        options_.max_open_files = 256;
        options_.write_buffer_size = 64 * 1024 * 1024;  // 64MB
        options_.target_file_size_base = 64 * 1024 * 1024;
        options_.compression = rocksdb::kLZ4Compression;
        options_.max_background_jobs = 4;

        // Write options - sync for durability
        write_options_.sync = true;

        // Read options - defaults are fine
        read_options_.verify_checksums = true;
    }

    // @unsafe - Calls close() which uses RocksDB API
    ~RocksDBLogStorage() override {
        close();
    }

    /**
     * Open the database. Must be called before other operations.
     * @return true on success, false on failure
     */
    // @unsafe - Uses RocksDB C++ API
    bool open() {
        if (is_open_.get()) {
            return true;  // Already open
        }

        rocksdb::Status status = rocksdb::DB::Open(options_, db_path_, &db_);  // @unsafe
        if (!status.ok()) {
            return false;
        }

        is_open_.set(true);
        return true;
    }

    // ========================================================================
    // Single Entry Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    rusty::Option<LogEntry> get(slotid_t slot_id) const override {
        if (!is_open_.get() || db_ == nullptr) {
            return rusty::None;
        }

        std::string key = make_log_key(slot_id);
        std::string value;
        rocksdb::Status status = db_->Get(read_options_, key, &value);  // @unsafe

        if (!status.ok()) {
            return rusty::None;
        }

        LogEntry entry;
        if (!deserialize_entry(value, &entry)) {  // @unsafe
            return rusty::None;
        }

        return rusty::Some(entry);
    }

    // @unsafe - Uses RocksDB API
    bool put(const LogEntry& entry) override {
        if (!is_open_.get() || db_ == nullptr) {
            return false;
        }

        std::string key = make_log_key(entry.slot_id);
        std::string value;
        if (!serialize_entry(entry, &value)) {  // @unsafe
            return false;
        }

        rocksdb::Status status = db_->Put(write_options_, key, value);  // @unsafe
        return status.ok();
    }

    // @unsafe - Uses RocksDB API
    bool remove(slotid_t slot_id) override {
        if (!is_open_.get() || db_ == nullptr) {
            return false;
        }

        std::string key = make_log_key(slot_id);

        // Check if key exists first
        std::string value;
        rocksdb::Status get_status = db_->Get(read_options_, key, &value);  // @unsafe
        if (!get_status.ok()) {
            return false;  // Key doesn't exist
        }

        rocksdb::Status status = db_->Delete(write_options_, key);  // @unsafe
        return status.ok();
    }

    // ========================================================================
    // Batch Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const override {
        std::vector<LogEntry> result;
        if (!is_open_.get() || db_ == nullptr || start >= end) {
            return result;
        }

        std::string start_key = make_log_key(start);
        std::string end_key = make_log_key(end);

        rocksdb::Iterator* it = db_->NewIterator(read_options_);  // @unsafe
        for (it->Seek(start_key); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            if (key >= end_key || key.substr(0, 4) != LOG_PREFIX) {
                break;
            }

            LogEntry entry;
            if (deserialize_entry(it->value().ToString(), &entry)) {  // @unsafe
                result.push_back(entry);
            }
        }
        delete it;  // @unsafe

        return result;
    }

    // @unsafe - Uses RocksDB API
    bool put_batch(const std::vector<LogEntry>& entries) override {
        if (!is_open_.get() || db_ == nullptr) {
            return false;
        }

        rocksdb::WriteBatch batch;
        for (const auto& entry : entries) {
            std::string key = make_log_key(entry.slot_id);
            std::string value;
            if (!serialize_entry(entry, &value)) {  // @unsafe
                return false;
            }
            batch.Put(key, value);
        }

        rocksdb::Status status = db_->Write(write_options_, &batch);  // @unsafe
        return status.ok();
    }

    // @unsafe - Uses RocksDB API
    bool remove_range(slotid_t start, slotid_t end) override {
        if (!is_open_.get() || db_ == nullptr || start >= end) {
            return false;
        }

        // Use WriteBatch for atomicity
        rocksdb::WriteBatch batch;
        std::string start_key = make_log_key(start);
        std::string end_key = make_log_key(end);

        rocksdb::Iterator* it = db_->NewIterator(read_options_);  // @unsafe
        for (it->Seek(start_key); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            if (key >= end_key || key.substr(0, 4) != LOG_PREFIX) {
                break;
            }
            batch.Delete(key);
        }
        delete it;  // @unsafe

        rocksdb::Status status = db_->Write(write_options_, &batch);  // @unsafe
        return status.ok();
    }

    // ========================================================================
    // Index Queries
    // ========================================================================

    // @unsafe - Uses RocksDB API
    slotid_t get_first_index() const override {
        if (!is_open_.get() || db_ == nullptr) {
            return 0;
        }

        rocksdb::Iterator* it = db_->NewIterator(read_options_);  // @unsafe
        it->Seek(LOG_PREFIX);

        slotid_t first_index = 0;
        if (it->Valid()) {
            std::string key = it->key().ToString();
            if (key.substr(0, 4) == LOG_PREFIX) {
                // Parse slot_id from key
                std::string slot_str = key.substr(4);
                first_index = std::stoull(slot_str);
            }
        }
        delete it;  // @unsafe

        return first_index;
    }

    // @unsafe - Uses RocksDB API
    slotid_t get_last_index() const override {
        if (!is_open_.get() || db_ == nullptr) {
            return 0;
        }

        // Seek to end of log entries
        // Keys are "log:XXXX", so we seek to "log;" (next char after ':')
        std::string prefix_end = "log;";

        rocksdb::Iterator* it = db_->NewIterator(read_options_);  // @unsafe
        it->Seek(prefix_end);

        slotid_t last_index = 0;
        if (it->Valid()) {
            it->Prev();  // Go back to last log entry
        } else {
            it->SeekToLast();  // No keys >= prefix_end, try last key
        }

        if (it->Valid()) {
            std::string key = it->key().ToString();
            if (key.substr(0, 4) == LOG_PREFIX) {
                std::string slot_str = key.substr(4);
                last_index = std::stoull(slot_str);
            }
        }
        delete it;  // @unsafe

        return last_index;
    }

    // @unsafe - Uses RocksDB API
    rusty::Option<ballot_t> get_term(slotid_t slot_id) const override {
        auto entry_opt = get(slot_id);  // @unsafe
        if (entry_opt.is_none()) {
            return rusty::None;
        }
        return rusty::Some(entry_opt.unwrap().term);
    }

    // @unsafe - Uses RocksDB API
    size_t size() const override {
        if (!is_open_.get() || db_ == nullptr) {
            return 0;
        }

        size_t count = 0;
        rocksdb::Iterator* it = db_->NewIterator(read_options_);  // @unsafe
        for (it->Seek(LOG_PREFIX); it->Valid(); it->Next()) {
            std::string key = it->key().ToString();
            if (key.substr(0, 4) != LOG_PREFIX) {
                break;
            }
            count++;
        }
        delete it;  // @unsafe

        return count;
    }

    // @unsafe - Calls size() which uses RocksDB
    bool empty() const override {
        return size() == 0;  // @unsafe
    }

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    bool set_metadata(const std::string& key, const std::string& value) override {
        if (!is_open_.get() || db_ == nullptr) {
            return false;
        }

        std::string meta_key = make_meta_key(key);
        rocksdb::Status status = db_->Put(write_options_, meta_key, value);  // @unsafe
        return status.ok();
    }

    // @unsafe - Uses RocksDB API
    rusty::Option<std::string> get_metadata(const std::string& key) const override {
        if (!is_open_.get() || db_ == nullptr) {
            return rusty::None;
        }

        std::string meta_key = make_meta_key(key);
        std::string value;
        rocksdb::Status status = db_->Get(read_options_, meta_key, &value);  // @unsafe

        if (!status.ok()) {
            return rusty::None;
        }

        return rusty::Some(value);
    }

    // ========================================================================
    // Lifecycle Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    bool sync() override {
        if (!is_open_.get() || db_ == nullptr) {
            return false;
        }

        // Flush WAL and memtables
        rocksdb::FlushOptions flush_opts;
        flush_opts.wait = true;
        rocksdb::Status status = db_->Flush(flush_opts);  // @unsafe
        return status.ok();
    }

    // @unsafe - Uses RocksDB API
    bool close() override {
        if (!is_open_.get()) {
            return false;
        }

        if (db_ != nullptr) {
            delete db_;  // @unsafe
            db_ = nullptr;
        }

        is_open_.set(false);
        return true;
    }

    // @safe - Uses Cell for thread-safe access
    bool is_open() const override {
        return is_open_.get();
    }

    // @unsafe - Uses RocksDB API
    bool clear() override {
        if (!is_open_.get() || db_ == nullptr) {
            return false;
        }

        // Delete all log entries and metadata
        rocksdb::WriteBatch batch;
        rocksdb::Iterator* it = db_->NewIterator(read_options_);  // @unsafe

        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            batch.Delete(it->key());
        }
        delete it;  // @unsafe

        rocksdb::Status status = db_->Write(write_options_, &batch);  // @unsafe
        return status.ok();
    }

    // ========================================================================
    // Additional Utility Methods
    // ========================================================================

    /**
     * Get the database path.
     */
    // @lifetime: (&'a) -> &'a
    const std::string& get_db_path() const {
        return db_path_;
    }

    /**
     * Destroy the database (delete all files).
     * Database must be closed first.
     */
    // @unsafe - Uses RocksDB API
    static bool destroy(const std::string& db_path) {
        rocksdb::Options options;
        rocksdb::Status status = rocksdb::DestroyDB(db_path, options);  // @unsafe
        return status.ok();
    }
};

}  // namespace rrr
