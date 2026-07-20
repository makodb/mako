#pragma once

/**
 * RocksDB Log Storage Implementation
 *
 * Persistent implementation of LogStorage for Raft/Paxos consensus logs.
 * Uses RocksDB C API as the underlying storage interface.
 *
 * RustyCpp Compliance: Uses rusty::Cell, rusty::Option
 * Note: RocksDB operations are marked @unsafe (third-party library, not borrow-checked)
 */

#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <cstring>

#include <rocksdb/c.h>

#include <rusty/cell.hpp>
#include <rusty/option.hpp>

#include "log_storage.hpp"
#include "rrr/rrr.hpp"

namespace janus {
namespace raft {

inline std::string rocksdb_log_take_error_cpp(char** errptr) {
    if (errptr == nullptr || *errptr == nullptr) {
        return "";
    }
    std::string err(*errptr);
    rocksdb_free(*errptr);
    *errptr = nullptr;
    return err;
}

inline std::string rocksdb_log_copy_slice_cpp(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return "";
    }
    return std::string(reinterpret_cast<const char*>(data), len);
}

inline std::string rocksdb_log_make_log_key_cpp(uint64_t slot_id) {
    std::ostringstream ss;
    ss << "log:" << std::setfill('0') << std::setw(20) << slot_id;
    return ss.str();
}

// @safe boundary split - the DSL owns pure key construction and status/range
// predicates. Raw RocksDB handles, iterators, write batches, and error object
// lifetimes remain inside RocksDBLogStorage's C++ methods.
#if RUSTYCPP_RUST
pub fn rocksdb_log_copy_slice(data: *const u8, len: usize) -> std::string {
    rocksdb_log_copy_slice_cpp(data, len)
}

pub fn rocksdb_log_make_log_key(slot_id: u64) -> std::string {
    rocksdb_log_make_log_key_cpp(slot_id)
}

pub fn rocksdb_log_make_meta_key(key: &std::string) -> std::string {
    std::string("meta:") + key
}

pub fn rocksdb_log_storage_ready(is_open: bool, has_db: bool) -> bool {
    is_open && has_db
}

pub fn rocksdb_log_range_valid(start: u64, end: u64) -> bool {
    start < end
}

pub fn rocksdb_log_empty_from_size(size: usize) -> bool {
    size == 0
}

pub fn rocksdb_log_value_present(has_value: bool) -> bool {
    has_value
}

pub fn rocksdb_log_key_in_range(key: &std::string,
                                end_key: &std::string,
                                is_log_key: bool) -> bool {
    key < end_key && is_log_key
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rocksdb_log_storage.helpers version=1 rust_sha256=0a3b6f0293563a2e30a7c72b3e5674b8c14f7b5ce73c2e60ef8f7045d5dc9b24*/
inline std::string rocksdb_log_copy_slice(const uint8_t* data, size_t len);
inline std::string rocksdb_log_make_log_key(uint64_t slot_id);
inline std::string rocksdb_log_make_meta_key(const std::string& key);
inline bool rocksdb_log_storage_ready(bool is_open, bool has_db);
inline bool rocksdb_log_range_valid(uint64_t start, uint64_t end);
inline bool rocksdb_log_empty_from_size(size_t size);
inline bool rocksdb_log_value_present(bool has_value);
inline bool rocksdb_log_key_in_range(const std::string& key, const std::string& end_key, bool is_log_key);

inline std::string rocksdb_log_copy_slice(const uint8_t* data, size_t len) {
    return rocksdb_log_copy_slice_cpp(data, std::move(len));
}

inline std::string rocksdb_log_make_log_key(uint64_t slot_id) {
    return rocksdb_log_make_log_key_cpp(std::move(slot_id));
}

inline std::string rocksdb_log_make_meta_key(const std::string& key) {
    return std::string("meta:") + key;
}

inline bool rocksdb_log_storage_ready(bool is_open, bool has_db) {
    return is_open && has_db;
}

inline bool rocksdb_log_range_valid(uint64_t start, uint64_t end) {
    return start < end;
}

inline bool rocksdb_log_empty_from_size(size_t size) {
    return size == 0;
}

inline bool rocksdb_log_value_present(bool has_value) {
    return has_value;
}

inline bool rocksdb_log_key_in_range(const std::string& key, const std::string& end_key, bool is_log_key) {
    return key < end_key && is_log_key;
}
/*RUSTYCPP:GEN-END id=rocksdb_log_storage.helpers*/

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
class RocksDBLogStorageState {
private:
    // @unsafe - RocksDB database handle. open() creates it, close()/destructor
    // close it with rocksdb_close().
    rocksdb_t* db_{nullptr};
    std::string db_path_;

    // @unsafe - RocksDB option handles. The constructor creates these and the
    // destructor destroys them after close(); close() intentionally leaves them
    // alive so open() can be called again with the same configuration.
    rocksdb_options_t* options_{nullptr};
    rocksdb_writeoptions_t* write_options_{nullptr};
    rocksdb_readoptions_t* read_options_{nullptr};

    // State
    rusty::Cell<bool> is_open_{false};

    // Key prefixes
    static constexpr const char* LOG_PREFIX = "log:";
    static constexpr const char* META_PREFIX = "meta:";

    // @unsafe - Frees RocksDB error strings returned through char** out params.
    static std::string take_rocksdb_error(char** errptr) {
        return rocksdb_log_take_error_cpp(errptr);
    }

    // @unsafe - std::string constructor is treated as non-borrow-checked
    static std::string copy_slice(const char* data, size_t len) {
        return rocksdb_log_copy_slice(reinterpret_cast<const uint8_t*>(data), len);
    }

    // @unsafe - Uses ostringstream operations
    std::string make_log_key(slotid_t slot_id) const {
        return rocksdb_log_make_log_key(slot_id);
    }

    // @unsafe - String concatenation
    std::string make_meta_key(const std::string& key) const {
        return rocksdb_log_make_meta_key(key);
    }

    // @unsafe - Uses Marshal which has non-borrow-checked operations.
    // routes through `LogEntry::save(BinaryWriteArchive&)`
    // (the migrated archive method) by way of a `MarshalSink` over the
    // backing `Marshal`. Wire format byte-for-byte unchanged.  The
    // const_cast on `entry` was removed — `save` is `const`-qualified.
    bool serialize_entry(const LogEntry& entry, std::string* out) const {
        Marshal m;
        rrr::MarshalSink sink(&m);
        BinaryWriteArchive writer(rrr::make_sink_proxy(&sink));
        entry.save(writer);
        size_t size = m.content_size();
        out->resize(size);
        m.read(out->data(), size);  // @unsafe - read Marshal contents into string
        return true;
    }

    // @unsafe - Uses Marshal which has non-borrow-checked operations.
    // routes through `LogEntry::load(BinaryReadArchive&)`
    // by way of a `MarshalSource` over the same backing `Marshal`.
    // The Phase 3f-prep MarshallDeputy archive op requires a
    // MarshalSource (no length prefix on the deputy payload), which
    // this satisfies.
    bool deserialize_entry(const std::string& data, LogEntry* out) const {
        Marshal m;
        m.write(data.data(), data.size());  // @unsafe - write string bytes into Marshal
        rrr::MarshalSource src(&m);
        BinaryReadArchive reader(rrr::make_source_proxy(&src));
        out->load(reader);
        return true;
    }

public:
    /**
     * Construct a RocksDB log storage and open the database.
     * @param db_path Path to the database directory
     */
    // @unsafe - Allocates RocksDB option handles and opens db_path_.
    explicit RocksDBLogStorageState(const std::string& db_path)
        : db_path_(db_path) {
        // Constructor-owned option handles; destroyed in ~RocksDBLogStorage().
        options_ = rocksdb_options_create();
        write_options_ = rocksdb_writeoptions_create();
        read_options_ = rocksdb_readoptions_create();

        if (options_ == nullptr || write_options_ == nullptr || read_options_ == nullptr) {
            rrr::Log_error("[RocksDBLogStorage] Failed to allocate RocksDB C API options");
            return;
        }

        // Configure RocksDB options
        rocksdb_options_set_create_if_missing(options_, 1);
        rocksdb_options_set_max_open_files(options_, 256);
        rocksdb_options_set_write_buffer_size(options_, 64 * 1024 * 1024);  // 64MB
        rocksdb_options_set_target_file_size_base(options_, 64 * 1024 * 1024);
        rocksdb_options_set_compression(options_, rocksdb_lz4_compression);
        rocksdb_options_set_max_background_jobs(options_, 4);

        // Write options - sync for durability
        rocksdb_writeoptions_set_sync(write_options_, 1);

        // Read options
        rocksdb_readoptions_set_verify_checksums(read_options_, 1);

        // Open the database immediately
        open();
    }

    // @unsafe - Closes db_ if open, then destroys constructor-owned options.
    ~RocksDBLogStorageState() {
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

    /**
     * Open the database. Must be called before other operations.
     * @return true on success, false on failure
     */
    // @unsafe - Opens db_path_ with constructor-owned option handles.
    bool open() {
        if (is_open_.get()) {
            return true;  // Already open
        }

        if (options_ == nullptr || write_options_ == nullptr || read_options_ == nullptr) {
            rrr::Log_error("[RocksDBLogStorage] Options not initialized");
            return false;
        }

        char* err = nullptr;
        db_ = rocksdb_open(options_, db_path_.c_str(), &err);
        if (err != nullptr || db_ == nullptr) {
            std::string err_str = take_rocksdb_error(&err);
            rrr::Log_error("[RocksDBLogStorage] Failed to open %s: %s",
                      db_path_.c_str(), err_str.empty() ? "null handle" : err_str.c_str());
            db_ = nullptr;
            return false;
        }

        is_open_.set(true);
        rrr::Log_info("[RocksDBLogStorage] Opened database at %s", db_path_.c_str());
        return true;
    }

    // ========================================================================
    // Single Entry Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API. The returned value buffer is owned by
    // RocksDB and must be released with rocksdb_free().
    rusty::Option<LogEntry> get(slotid_t slot_id) const {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return rusty::None;
        }

        std::string key = make_log_key(slot_id);
        size_t value_len = 0;
        char* err = nullptr;
        char* value_ptr = rocksdb_get(db_, read_options_, key.data(), key.size(), &value_len, &err);

        if (err != nullptr) {
            take_rocksdb_error(&err);
            return rusty::None;
        }
        if (!rocksdb_log_value_present(value_ptr != nullptr)) {
            return rusty::None;
        }

        std::string value = copy_slice(value_ptr, value_len);
        rocksdb_free(value_ptr);

        LogEntry entry = LogEntry::defaults();
        if (!deserialize_entry(value, &entry)) {  // @unsafe
            return rusty::None;
        }

        return rusty::Some(entry);
    }

    // @unsafe - Uses RocksDB API
    bool put(const LogEntry& entry) {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return false;
        }

        std::string key = make_log_key(entry.slot_id);
        std::string value;
        if (!serialize_entry(entry, &value)) {  // @unsafe
            return false;
        }

        char* err = nullptr;
        rocksdb_put(db_, write_options_, key.data(), key.size(), value.data(), value.size(), &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // @unsafe - Uses RocksDB API. The existence-check buffer is owned by
    // RocksDB and must be released with rocksdb_free().
    bool remove(slotid_t slot_id) {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return false;
        }

        std::string key = make_log_key(slot_id);

        // Check if key exists first
        size_t value_len = 0;
        char* err = nullptr;
        char* value_ptr = rocksdb_get(db_, read_options_, key.data(), key.size(), &value_len, &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        if (!rocksdb_log_value_present(value_ptr != nullptr)) {
            return false;  // Key doesn't exist
        }
        rocksdb_free(value_ptr);

        rocksdb_delete(db_, write_options_, key.data(), key.size(), &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // ========================================================================
    // Batch Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API. Iterator is a temporary handle destroyed
    // before return.
    std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const {
        std::vector<LogEntry> result;
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr) ||
            !rocksdb_log_range_valid(start, end)) {
            return result;
        }

        std::string start_key = make_log_key(start);
        std::string end_key = make_log_key(end);

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            return result;
        }

        rocksdb_iter_seek(it, start_key.data(), start_key.size());
        for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
            size_t key_len = 0;
            size_t value_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            const char* value_ptr = rocksdb_iter_value(it, &value_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (!rocksdb_log_key_in_range(
                    key, end_key,
                    key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) == 0)) {
                break;
            }

            LogEntry entry = LogEntry::defaults();
            std::string value = copy_slice(value_ptr, value_len);
            if (deserialize_entry(value, &entry)) {  // @unsafe
                result.push_back(entry);
            }
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return result;
    }

    // @unsafe - Uses RocksDB API. Write batch is a temporary handle destroyed
    // on every exit path after creation.
    bool put_batch(const std::vector<LogEntry>& entries) {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return false;
        }

        rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
        if (batch == nullptr) {
            return false;
        }

        for (const auto& entry : entries) {
            std::string key = make_log_key(entry.slot_id);
            std::string value;
            if (!serialize_entry(entry, &value)) {  // @unsafe
                rocksdb_writebatch_destroy(batch);
                return false;
            }
            rocksdb_writebatch_put(batch, key.data(), key.size(), value.data(), value.size());
        }

        char* err = nullptr;
        rocksdb_write(db_, write_options_, batch, &err);  // @unsafe
        rocksdb_writebatch_destroy(batch);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // @unsafe - Uses RocksDB API. Write batch and iterator are temporary
    // handles destroyed on every exit path after creation.
    bool remove_range(slotid_t start, slotid_t end) {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr) ||
            !rocksdb_log_range_valid(start, end)) {
            return false;
        }

        // Use WriteBatch for atomicity
        rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
        if (batch == nullptr) {
            return false;
        }

        std::string start_key = make_log_key(start);
        std::string end_key = make_log_key(end);

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            rocksdb_writebatch_destroy(batch);
            return false;
        }

        rocksdb_iter_seek(it, start_key.data(), start_key.size());
        for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (!rocksdb_log_key_in_range(
                    key, end_key,
                    key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) == 0)) {
                break;
            }
            rocksdb_writebatch_delete(batch, key.data(), key.size());
        }

        char* iter_err = nullptr;
        rocksdb_iter_get_error(it, &iter_err);
        rocksdb_iter_destroy(it);  // @unsafe
        if (iter_err != nullptr) {
            take_rocksdb_error(&iter_err);
            rocksdb_writebatch_destroy(batch);
            return false;
        }

        char* err = nullptr;
        rocksdb_write(db_, write_options_, batch, &err);  // @unsafe
        rocksdb_writebatch_destroy(batch);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // ========================================================================
    // Index Queries
    // ========================================================================

    // @unsafe - Uses RocksDB API. Iterator is a temporary handle destroyed
    // before return.
    slotid_t get_first_index() const {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return 0;
        }

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            return 0;
        }

        rocksdb_iter_seek(it, LOG_PREFIX, std::strlen(LOG_PREFIX));

        slotid_t first_index = 0;
        if (rocksdb_iter_valid(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) == 0) {
                std::string slot_str = key.substr(std::strlen(LOG_PREFIX));
                first_index = std::stoull(slot_str);
            }
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return first_index;
    }

    // @unsafe - Uses RocksDB API. Iterator is a temporary handle destroyed
    // before return.
    slotid_t get_last_index() const {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return 0;
        }

        std::string prefix_end = "log;";

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            return 0;
        }

        rocksdb_iter_seek(it, prefix_end.data(), prefix_end.size());

        slotid_t last_index = 0;
        if (rocksdb_iter_valid(it)) {
            rocksdb_iter_prev(it);  // Go back to last log entry
        } else {
            rocksdb_iter_seek_to_last(it);  // No keys >= prefix_end, try last key
        }

        if (rocksdb_iter_valid(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) == 0) {
                std::string slot_str = key.substr(std::strlen(LOG_PREFIX));
                last_index = std::stoull(slot_str);
            }
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return last_index;
    }

    // @unsafe - Uses RocksDB API
    rusty::Option<ballot_t> get_term(slotid_t slot_id) const {
        auto entry_opt = get(slot_id);  // @unsafe
        if (entry_opt.is_none()) {
            return rusty::None;
        }
        return rusty::Some(entry_opt.unwrap().term);
    }

    // @unsafe - Uses RocksDB API. Iterator is a temporary handle destroyed
    // before return.
    size_t size() const {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return 0;
        }

        size_t count = 0;
        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            return 0;
        }

        rocksdb_iter_seek(it, LOG_PREFIX, std::strlen(LOG_PREFIX));
        for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) != 0) {
                break;
            }
            count++;
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return count;
    }

    // @unsafe - Calls size() which uses RocksDB
    bool empty() const {
        return rocksdb_log_empty_from_size(size());  // @unsafe
    }

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    bool set_metadata(const std::string& key, const std::string& value) {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return false;
        }

        std::string meta_key = make_meta_key(key);
        char* err = nullptr;
        rocksdb_put(db_, write_options_, meta_key.data(), meta_key.size(),
                    value.data(), value.size(), &err);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // @unsafe - Uses RocksDB API. The returned value buffer is owned by
    // RocksDB and must be released with rocksdb_free().
    rusty::Option<std::string> get_metadata(const std::string& key) const {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return rusty::None;
        }

        std::string meta_key = make_meta_key(key);
        size_t value_len = 0;
        char* err = nullptr;
        char* value_ptr = rocksdb_get(db_, read_options_,
                                      meta_key.data(), meta_key.size(),
                                      &value_len, &err);

        if (err != nullptr) {
            take_rocksdb_error(&err);
            return rusty::None;
        }
        if (!rocksdb_log_value_present(value_ptr != nullptr)) {
            return rusty::None;
        }

        std::string value = copy_slice(value_ptr, value_len);
        rocksdb_free(value_ptr);
        return rusty::Some(value);
    }

    // ========================================================================
    // Lifecycle Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API. Flush options are a temporary handle
    // destroyed before return.
    bool sync() {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return false;
        }

        rocksdb_flushoptions_t* flush_opts = rocksdb_flushoptions_create();
        if (flush_opts == nullptr) {
            return false;
        }
        rocksdb_flushoptions_set_wait(flush_opts, 1);

        char* err = nullptr;
        rocksdb_flush(db_, flush_opts, &err);  // @unsafe
        rocksdb_flushoptions_destroy(flush_opts);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // @unsafe - Closes db_ only; constructor-owned option handles remain
    // alive so the storage can be reopened.
    bool close() {
        if (!is_open_.get()) {
            return false;
        }

        if (db_ != nullptr) {
            rocksdb_close(db_);  // @unsafe
            db_ = nullptr;
        }

        is_open_.set(false);
        return true;
    }

    // @safe - Uses Cell for thread-safe access
    bool is_open() const {
        return is_open_.get();
    }

    // @unsafe - Uses RocksDB API. Write batch and iterator are temporary
    // handles destroyed on every exit path after creation.
    bool clear() {
        if (!rocksdb_log_storage_ready(is_open_.get(), db_ != nullptr)) {
            return false;
        }

        rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
        if (batch == nullptr) {
            return false;
        }

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            rocksdb_writebatch_destroy(batch);
            return false;
        }

        rocksdb_iter_seek_to_first(it);
        for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            rocksdb_writebatch_delete(batch, key_ptr, key_len);
        }

        char* iter_err = nullptr;
        rocksdb_iter_get_error(it, &iter_err);
        rocksdb_iter_destroy(it);  // @unsafe
        if (iter_err != nullptr) {
            take_rocksdb_error(&iter_err);
            rocksdb_writebatch_destroy(batch);
            return false;
        }

        char* err = nullptr;
        rocksdb_write(db_, write_options_, batch, &err);  // @unsafe
        rocksdb_writebatch_destroy(batch);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
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
    // @unsafe - Uses a temporary RocksDB options handle for destroy_db.
    static bool destroy(const std::string& db_path) {
        rocksdb_options_t* options = rocksdb_options_create();
        if (options == nullptr) {
            return false;
        }

        char* err = nullptr;
        rocksdb_destroy_db(options, db_path.c_str(), &err);  // @unsafe
        rocksdb_options_destroy(options);

        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }
};

// @unsafe - allocates the C++ RocksDB state object and opens the database.
inline RocksDBLogStorageState* rocksdb_log_storage_state_new_cpp(
    const std::string& db_path) {
    return new RocksDBLogStorageState(db_path);
}

// @unsafe - destroys the C++ RocksDB state object.
inline void rocksdb_log_storage_state_delete_cpp(
    RocksDBLogStorageState* state) {
    delete state;
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_open_cpp(RocksDBLogStorageState* state) {
    return state->open();
}

// @unsafe - delegates to RocksDB-backed state.
inline rusty::Option<LogEntry> rocksdb_log_storage_get_cpp(
    const RocksDBLogStorageState* state, slotid_t slot_id) {
    return state->get(slot_id);
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_put_cpp(RocksDBLogStorageState* state,
                                        const LogEntry& entry) {
    return state->put(entry);
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_remove_cpp(RocksDBLogStorageState* state,
                                           slotid_t slot_id) {
    return state->remove(slot_id);
}

// @unsafe - delegates to RocksDB-backed state.
inline std::vector<LogEntry> rocksdb_log_storage_get_range_cpp(
    const RocksDBLogStorageState* state, slotid_t start, slotid_t end) {
    return state->get_range(start, end);
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_put_batch_cpp(
    RocksDBLogStorageState* state, const std::vector<LogEntry>& entries) {
    return state->put_batch(entries);
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_remove_range_cpp(
    RocksDBLogStorageState* state, slotid_t start, slotid_t end) {
    return state->remove_range(start, end);
}

// @unsafe - delegates to RocksDB-backed state.
inline slotid_t rocksdb_log_storage_first_index_cpp(
    const RocksDBLogStorageState* state) {
    return state->get_first_index();
}

// @unsafe - delegates to RocksDB-backed state.
inline slotid_t rocksdb_log_storage_last_index_cpp(
    const RocksDBLogStorageState* state) {
    return state->get_last_index();
}

// @unsafe - delegates to RocksDB-backed state.
inline rusty::Option<ballot_t> rocksdb_log_storage_term_cpp(
    const RocksDBLogStorageState* state, slotid_t slot_id) {
    return state->get_term(slot_id);
}

// @unsafe - delegates to RocksDB-backed state.
inline size_t rocksdb_log_storage_size_cpp(
    const RocksDBLogStorageState* state) {
    return state->size();
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_empty_cpp(
    const RocksDBLogStorageState* state) {
    return state->empty();
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_set_metadata_cpp(
    RocksDBLogStorageState* state, const std::string& key,
    const std::string& value) {
    return state->set_metadata(key, value);
}

// @unsafe - delegates to RocksDB-backed state.
inline rusty::Option<std::string> rocksdb_log_storage_get_metadata_cpp(
    const RocksDBLogStorageState* state, const std::string& key) {
    return state->get_metadata(key);
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_sync_cpp(RocksDBLogStorageState* state) {
    return state->sync();
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_close_cpp(RocksDBLogStorageState* state) {
    return state->close();
}

// @safe - reads state open flag.
inline bool rocksdb_log_storage_is_open_cpp(
    const RocksDBLogStorageState* state) {
    return state->is_open();
}

// @unsafe - delegates to RocksDB-backed state.
inline bool rocksdb_log_storage_clear_cpp(RocksDBLogStorageState* state) {
    return state->clear();
}

// @lifetime: (&'a) -> &'a
inline const std::string& rocksdb_log_storage_db_path_cpp(
    const RocksDBLogStorageState* state) {
    return state->get_db_path();
}

// @unsafe - destroys a closed RocksDB database directory.
inline bool rocksdb_log_storage_destroy_cpp(const std::string& db_path) {
    return RocksDBLogStorageState::destroy(db_path);
}

#if RUSTYCPP_RUST
pub struct RocksDBLogStorageCore {
    state_: *mut RocksDBLogStorageState,
}

impl RocksDBLogStorageCore {
    // @unsafe - Allocates RocksDB C++ state and opens the database.
    #[cpp_ctor]
    fn new(db_path: std::string) -> RocksDBLogStorageCore {
        RocksDBLogStorageCore {
            state_: unsafe { rocksdb_log_storage_state_new_cpp(&db_path) },
        }
    }

    // @unsafe - Destroys RocksDB C++ state.
    fn Destroy(&mut self) {
        unsafe { rocksdb_log_storage_state_delete_cpp(self.state_) }
    }

    // @unsafe
    fn open(&mut self) -> bool {
        unsafe { rocksdb_log_storage_open_cpp(self.state_) }
    }

    // @unsafe
    fn get(&self, slot_id: u64) -> rusty::Option<LogEntry> {
        unsafe { rocksdb_log_storage_get_cpp(self.state_, slot_id) }
    }

    // @unsafe
    fn put(&mut self, entry: &LogEntry) -> bool {
        unsafe { rocksdb_log_storage_put_cpp(self.state_, entry) }
    }

    // @unsafe
    fn remove(&mut self, slot_id: u64) -> bool {
        unsafe { rocksdb_log_storage_remove_cpp(self.state_, slot_id) }
    }

    // @unsafe
    fn get_range(&self, start: u64, end: u64) -> std::vector<LogEntry> {
        unsafe { rocksdb_log_storage_get_range_cpp(self.state_, start, end) }
    }

    // @unsafe
    fn put_batch(&mut self, entries: &std::vector<LogEntry>) -> bool {
        unsafe { rocksdb_log_storage_put_batch_cpp(self.state_, entries) }
    }

    // @unsafe
    fn remove_range(&mut self, start: u64, end: u64) -> bool {
        unsafe { rocksdb_log_storage_remove_range_cpp(self.state_, start, end) }
    }

    // @unsafe
    fn get_first_index(&self) -> u64 {
        unsafe { rocksdb_log_storage_first_index_cpp(self.state_) }
    }

    // @unsafe
    fn get_last_index(&self) -> u64 {
        unsafe { rocksdb_log_storage_last_index_cpp(self.state_) }
    }

    // @unsafe
    fn get_term(&self, slot_id: u64) -> rusty::Option<i64> {
        unsafe { rocksdb_log_storage_term_cpp(self.state_, slot_id) }
    }

    // @unsafe
    fn size(&self) -> usize {
        unsafe { rocksdb_log_storage_size_cpp(self.state_) }
    }

    // @unsafe
    fn empty(&self) -> bool {
        unsafe { rocksdb_log_storage_empty_cpp(self.state_) }
    }

    // @unsafe
    fn set_metadata(&mut self, key: &std::string,
                    value: &std::string) -> bool {
        unsafe { rocksdb_log_storage_set_metadata_cpp(self.state_, key, value) }
    }

    // @unsafe
    fn get_metadata(&self, key: &std::string) -> rusty::Option<std::string> {
        unsafe { rocksdb_log_storage_get_metadata_cpp(self.state_, key) }
    }

    // @unsafe
    fn sync(&mut self) -> bool {
        unsafe { rocksdb_log_storage_sync_cpp(self.state_) }
    }

    // @unsafe
    fn close(&mut self) -> bool {
        unsafe { rocksdb_log_storage_close_cpp(self.state_) }
    }

    // @safe
    fn is_open(&self) -> bool {
        rocksdb_log_storage_is_open_cpp(self.state_)
    }

    // @unsafe
    fn clear(&mut self) -> bool {
        unsafe { rocksdb_log_storage_clear_cpp(self.state_) }
    }

    // @lifetime: (&'a) -> &'a
    fn get_db_path(&self) -> &std::string {
        unsafe { rocksdb_log_storage_db_path_cpp(self.state_) }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=rocksdb_log_storage.core version=1 rust_sha256=8928316677be9a8e1417bc3f8c3026fe5c994d882cd3c3a250968942b3bce086*/
struct RocksDBLogStorageCore;

struct RocksDBLogStorageCore {
    RocksDBLogStorageState* state_;

    RocksDBLogStorageCore(std::string db_path);
    void Destroy();
    bool open();
    rusty::Option<LogEntry> get(uint64_t slot_id) const;
    bool put(const LogEntry& entry);
    bool remove(uint64_t slot_id);
    std::vector<LogEntry> get_range(uint64_t start, uint64_t end) const;
    bool put_batch(const std::vector<LogEntry>& entries);
    bool remove_range(uint64_t start, uint64_t end);
    uint64_t get_first_index() const;
    uint64_t get_last_index() const;
    rusty::Option<int64_t> get_term(uint64_t slot_id) const;
    size_t size() const;
    bool empty() const;
    bool set_metadata(const std::string& key, const std::string& value);
    rusty::Option<std::string> get_metadata(const std::string& key) const;
    bool sync();
    bool close();
    bool is_open() const;
    bool clear();
    const std::string& get_db_path() const;
};


inline RocksDBLogStorageCore::RocksDBLogStorageCore(std::string db_path)
    : state_(rocksdb_log_storage_state_new_cpp(db_path))
{}

inline void RocksDBLogStorageCore::Destroy() {
    // @unsafe
    {
        rocksdb_log_storage_state_delete_cpp(this->state_);
    }
}

inline bool RocksDBLogStorageCore::open() {
    // @unsafe
    {
        return rocksdb_log_storage_open_cpp(this->state_);
    }
}

inline rusty::Option<LogEntry> RocksDBLogStorageCore::get(uint64_t slot_id) const {
    // @unsafe
    {
        return rocksdb_log_storage_get_cpp(this->state_, std::move(slot_id));
    }
}

inline bool RocksDBLogStorageCore::put(const LogEntry& entry) {
    // @unsafe
    {
        return rocksdb_log_storage_put_cpp(this->state_, entry);
    }
}

inline bool RocksDBLogStorageCore::remove(uint64_t slot_id) {
    // @unsafe
    {
        return rocksdb_log_storage_remove_cpp(this->state_, std::move(slot_id));
    }
}

inline std::vector<LogEntry> RocksDBLogStorageCore::get_range(uint64_t start, uint64_t end) const {
    // @unsafe
    {
        return rocksdb_log_storage_get_range_cpp(this->state_, std::move(start), std::move(end));
    }
}

inline bool RocksDBLogStorageCore::put_batch(const std::vector<LogEntry>& entries) {
    // @unsafe
    {
        return rocksdb_log_storage_put_batch_cpp(this->state_, entries);
    }
}

inline bool RocksDBLogStorageCore::remove_range(uint64_t start, uint64_t end) {
    // @unsafe
    {
        return rocksdb_log_storage_remove_range_cpp(this->state_, std::move(start), std::move(end));
    }
}

inline uint64_t RocksDBLogStorageCore::get_first_index() const {
    // @unsafe
    {
        return rocksdb_log_storage_first_index_cpp(this->state_);
    }
}

inline uint64_t RocksDBLogStorageCore::get_last_index() const {
    // @unsafe
    {
        return rocksdb_log_storage_last_index_cpp(this->state_);
    }
}

inline rusty::Option<int64_t> RocksDBLogStorageCore::get_term(uint64_t slot_id) const {
    // @unsafe
    {
        return rocksdb_log_storage_term_cpp(this->state_, std::move(slot_id));
    }
}

inline size_t RocksDBLogStorageCore::size() const {
    // @unsafe
    {
        return rocksdb_log_storage_size_cpp(this->state_);
    }
}

inline bool RocksDBLogStorageCore::empty() const {
    // @unsafe
    {
        return rocksdb_log_storage_empty_cpp(this->state_);
    }
}

inline bool RocksDBLogStorageCore::set_metadata(const std::string& key, const std::string& value) {
    // @unsafe
    {
        return rocksdb_log_storage_set_metadata_cpp(this->state_, key, value);
    }
}

inline rusty::Option<std::string> RocksDBLogStorageCore::get_metadata(const std::string& key) const {
    // @unsafe
    {
        return rocksdb_log_storage_get_metadata_cpp(this->state_, key);
    }
}

inline bool RocksDBLogStorageCore::sync() {
    // @unsafe
    {
        return rocksdb_log_storage_sync_cpp(this->state_);
    }
}

inline bool RocksDBLogStorageCore::close() {
    // @unsafe
    {
        return rocksdb_log_storage_close_cpp(this->state_);
    }
}

inline bool RocksDBLogStorageCore::is_open() const {
    return rocksdb_log_storage_is_open_cpp(this->state_);
}

inline bool RocksDBLogStorageCore::clear() {
    // @unsafe
    {
        return rocksdb_log_storage_clear_cpp(this->state_);
    }
}

inline const std::string& RocksDBLogStorageCore::get_db_path() const {
    // @unsafe
    {
        return rocksdb_log_storage_db_path_cpp(this->state_);
    }
}
/*RUSTYCPP:GEN-END id=rocksdb_log_storage.core*/

class RocksDBLogStorage : public LogStorage {
public:
    // @unsafe - Allocates RocksDB option handles and opens db_path.
    explicit RocksDBLogStorage(const std::string& db_path)
        : core_(db_path) {}

    // @unsafe - Destroys RocksDB state.
    ~RocksDBLogStorage() override {
        core_.Destroy();
    }

    bool open() {
        return core_.open();
    }

    rusty::Option<LogEntry> get(slotid_t slot_id) const override {
        return core_.get(slot_id);
    }

    bool put(const LogEntry& entry) override {
        return core_.put(entry);
    }

    bool remove(slotid_t slot_id) override {
        return core_.remove(slot_id);
    }

    std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const override {
        return core_.get_range(start, end);
    }

    bool put_batch(const std::vector<LogEntry>& entries) override {
        return core_.put_batch(entries);
    }

    bool remove_range(slotid_t start, slotid_t end) override {
        return core_.remove_range(start, end);
    }

    slotid_t get_first_index() const override {
        return core_.get_first_index();
    }

    slotid_t get_last_index() const override {
        return core_.get_last_index();
    }

    rusty::Option<ballot_t> get_term(slotid_t slot_id) const override {
        return core_.get_term(slot_id);
    }

    size_t size() const override {
        return core_.size();
    }

    bool empty() const override {
        return core_.empty();
    }

    bool set_metadata(const std::string& key, const std::string& value) override {
        return core_.set_metadata(key, value);
    }

    rusty::Option<std::string> get_metadata(const std::string& key) const override {
        return core_.get_metadata(key);
    }

    bool sync() override {
        return core_.sync();
    }

    bool close() override {
        return core_.close();
    }

    bool is_open() const override {
        return core_.is_open();
    }

    bool clear() override {
        return core_.clear();
    }

    // @lifetime: (&'a) -> &'a
    const std::string& get_db_path() const {
        return core_.get_db_path();
    }

    // @unsafe - Uses a temporary RocksDB options handle for destroy_db.
    static bool destroy(const std::string& db_path) {
        return rocksdb_log_storage_destroy_cpp(db_path);
    }

private:
    RocksDBLogStorageCore core_;
};

}  // namespace raft
}  // namespace janus
