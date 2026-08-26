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

#include <charconv>
#include <stdexcept>
#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <utility>

#include <rocksdb/c.h>

#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>

#include "log_storage.hpp"
#include "rrr/rrr.hpp"
// the variadic Log_* wrappers live outside src/rrr now
#include "rrr_log.h"

namespace janus {
namespace raft {

#if RUSTYCPP_RUST
pub const fn rocksdb_log_storage_is_closed(is_open: bool) -> bool {
    !is_open
}

pub const fn rocksdb_log_storage_missing_db(has_db: bool) -> bool {
    !has_db
}

pub const fn rocksdb_log_range_valid(start: u64, end: u64) -> bool {
    start < end
}

pub const fn rocksdb_log_empty_from_size(size: usize) -> bool {
    size == 0
}

pub const fn rocksdb_log_value_present(has_value: bool) -> bool {
    has_value
}
#endif
/*RUSTYCPP:GEN-BEGIN id=raft_rocksdb_log.scalar_decisions version=1 rust_sha256=6873d59cc6d236720ec1fe897315083d9cb97b9636d9825edaf651a8e2ad4555*/
constexpr bool rocksdb_log_storage_is_closed(bool is_open);
constexpr bool rocksdb_log_storage_missing_db(bool has_db);
constexpr bool rocksdb_log_range_valid(uint64_t start, uint64_t end);
constexpr bool rocksdb_log_empty_from_size(size_t size);
constexpr bool rocksdb_log_value_present(bool has_value);
constexpr bool rocksdb_log_storage_is_closed(bool is_open) {
    return !is_open;
}
constexpr bool rocksdb_log_storage_missing_db(bool has_db) {
    return !has_db;
}
constexpr bool rocksdb_log_range_valid(uint64_t start, uint64_t end) {
    return rusty::detail::deref_if_pointer_like(start) < rusty::detail::deref_if_pointer_like(end);
}
constexpr bool rocksdb_log_empty_from_size(size_t size) {
    return rusty::detail::deref_if_pointer_like(size) == static_cast<size_t>(0);
}
constexpr bool rocksdb_log_value_present(bool has_value) {
    return std::move(has_value);
}
/*RUSTYCPP:GEN-END id=raft_rocksdb_log.scalar_decisions*/

static_assert(!rocksdb_log_storage_is_closed(true));
static_assert(rocksdb_log_storage_is_closed(false));
static_assert(!rocksdb_log_storage_missing_db(true));
static_assert(rocksdb_log_storage_missing_db(false));
static_assert(rocksdb_log_range_valid(1, 2));
static_assert(!rocksdb_log_range_valid(2, 2));
static_assert(rocksdb_log_empty_from_size(0));
static_assert(!rocksdb_log_empty_from_size(1));
static_assert(rocksdb_log_value_present(true));

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
    // Database handle (raw pointer, RocksDB C API manages lifetime via rocksdb_close)
    rocksdb_t* db_{nullptr};  // @unsafe - Raw pointer managed by RocksDB C API
    std::string db_path_;

    // Configuration
    rocksdb_options_t* options_{nullptr};
    rocksdb_writeoptions_t* write_options_{nullptr};
    rocksdb_readoptions_t* read_options_{nullptr};

    // State
    rusty::Cell<bool> is_open_{false};

    // Key prefixes
    static constexpr const char* LOG_PREFIX = "log:";
    static constexpr const char* META_PREFIX = "meta:";

    // @unsafe - Uses RocksDB C API allocator semantics
    static std::string take_rocksdb_error(char** errptr) {
        if (errptr == nullptr || *errptr == nullptr) {
            return "";
        }
        std::string err(*errptr);
        rocksdb_free(*errptr);
        *errptr = nullptr;
        return err;
    }

    // @unsafe - std::string constructor is treated as non-borrow-checked
    static std::string copy_slice(const char* data, size_t len) {
        if (data == nullptr || len == 0) {
            return ""; // @unsafe
        }
        return std::string(data, len); // @unsafe
    }

    // @unsafe - Strictly decodes the canonical fixed-width key emitted by
    // make_log_key. Recovery must never accept an entry whose physical key and
    // embedded slot disagree, because later point reads use the physical key.
    static slotid_t parse_log_key_or_throw(const std::string& key) {
        const size_t prefix_size = std::strlen(LOG_PREFIX);
        if (key.size() != prefix_size + 20 ||
            key.compare(0, prefix_size, LOG_PREFIX) != 0) {
            throw std::runtime_error("invalid RocksDB Raft log key shape");
        }
        const char* begin = key.data() + prefix_size;
        const char* end = key.data() + key.size();
        slotid_t slot = 0;
        const auto parsed = std::from_chars(begin, end, slot, 10);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            throw std::runtime_error("invalid RocksDB Raft log key index");
        }
        return slot;
    }

    // @unsafe - Converts a RocksDB-owned error into an exception after freeing
    // its C allocation. Read APIs cannot represent backend failure separately
    // from a legitimate missing value, so recovery relies on this fail-closed
    // distinction.
    [[noreturn]] static void throw_rocksdb_read_error(
        const char* operation, char** errptr) {
        const std::string detail = take_rocksdb_error(errptr);
        throw std::runtime_error(
            std::string(operation) + ": " +
            (detail.empty() ? "RocksDB read failed" : detail));
    }

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

    // @unsafe - raw byte copy of the sink into the out string.
    // Routes through `LogEntry::save(BinaryWriteArchive&)` over a
    // BufferSink. Wire format byte-for-byte unchanged (the former
    // Marshal + MarshalSink scratch pair is gone).
    bool serialize_entry(const LogEntry& entry, std::string* out) const {
        rrr::BufferSink sink;
        BinaryWriteArchive writer(rrr::make_sink_proxy_buffer(&sink));
        entry.save(writer);
        out->assign(reinterpret_cast<const char*>(sink.bytes.data()),
                    sink.bytes.len());
        return true;
    }

    // @unsafe - archive read over the rocksdb value's raw bytes.
    // Routes through `LogEntry::load(BinaryReadArchive&)` over a
    // BufferSource view of `data` — no copy into a scratch Marshal.
    // (The MarshallDeputy payload has no length prefix; BufferSource
    // bounds reads to the value's size, same as the former
    // MarshalSource satisfied.)
    bool deserialize_entry(const std::string& data, LogEntry* out) const {
        rrr::BufferSource src(reinterpret_cast<const std::uint8_t*>(data.data()),
                              data.size());
        BinaryReadArchive reader(rrr::make_source_proxy_buffer(&src));
        out->load(reader);
        return true;
    }

public:
    /**
     * Construct a RocksDB log storage and open the database.
     * @param db_path Path to the database directory
     */
    // @unsafe - Opens RocksDB database
    explicit RocksDBLogStorage(const std::string& db_path)
        : db_path_(db_path) {
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

    // @unsafe - Calls close() which uses RocksDB API
    ~RocksDBLogStorage() override {
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
    // @unsafe - Uses RocksDB C API
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
            rrr::Log_error("[RocksDBLogStorage] Failed to open {}: {}",
                      db_path_.c_str(), err_str.empty() ? "null handle" : err_str.c_str());
            db_ = nullptr;
            return false;
        }

        is_open_.set(true);
        rrr::Log_info("[RocksDBLogStorage] Opened database at {}", db_path_.c_str());
        return true;
    }

    // ========================================================================
    // Single Entry Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    rusty::Option<LogEntry> get(slotid_t slot_id) const override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
            return rusty::None;
        }

        std::string key = make_log_key(slot_id);
        size_t value_len = 0;
        char* err = nullptr;
        char* value_ptr = rocksdb_get(db_, read_options_, key.data(), key.size(), &value_len, &err);

        if (err != nullptr) {
            throw_rocksdb_read_error("RocksDB log get", &err);
        }
        if (!rocksdb_log_value_present(value_ptr != nullptr)) {
            return rusty::None;
        }

        std::string value = copy_slice(value_ptr, value_len);
        rocksdb_free(value_ptr);

        LogEntry entry;
        if (!deserialize_entry(value, &entry)) {  // @unsafe
            return rusty::None;
        }
        if (entry.slot_id != slot_id) {
            throw std::runtime_error(
                "RocksDB point-read key does not match embedded slot");
        }

        return rusty::Some(entry);
    }

    // @unsafe - Uses RocksDB API
    bool put(const LogEntry& entry) override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
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

    // @unsafe - Uses RocksDB API
    bool remove(slotid_t slot_id) override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
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

    // @unsafe - Uses RocksDB API
    std::vector<LogEntry> get_range(slotid_t start, slotid_t end) const override {
        std::vector<LogEntry> result;
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr) ||
            !rocksdb_log_range_valid(start, end)) {
            return result;
        }

        std::string start_key = make_log_key(start);
        std::string end_key = make_log_key(end);

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            throw std::runtime_error("RocksDB log range iterator creation failed");
        }

        rocksdb_iter_seek(it, start_key.data(), start_key.size());
        for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
            size_t key_len = 0;
            size_t value_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            const char* value_ptr = rocksdb_iter_value(it, &value_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (key >= end_key || key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) != 0) {
                break;
            }
            try {
                const slotid_t physical_slot = parse_log_key_or_throw(key);
                LogEntry entry;
                std::string value = copy_slice(value_ptr, value_len);
                if (deserialize_entry(value, &entry)) {  // @unsafe
                    if (entry.slot_id != physical_slot) {
                        throw std::runtime_error(
                            "RocksDB log key does not match embedded slot");
                    }
                    result.push_back(entry);
                }
            } catch (...) {
                rocksdb_iter_destroy(it);  // @unsafe
                throw;
            }
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            rocksdb_iter_destroy(it);  // @unsafe
            throw_rocksdb_read_error("RocksDB log range iterator", &err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return result;
    }

    // @unsafe - Uses RocksDB API
    bool put_batch(const std::vector<LogEntry>& entries) override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
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

    // @unsafe - Uses RocksDB API
    bool remove_range(slotid_t start, slotid_t end) override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr) ||
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
            if (key >= end_key || key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) != 0) {
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

    // @unsafe - Uses RocksDB API
    slotid_t get_first_index() const override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
            return 0;
        }

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            throw std::runtime_error("RocksDB first-index iterator creation failed");
        }

        rocksdb_iter_seek(it, LOG_PREFIX, std::strlen(LOG_PREFIX));

        slotid_t first_index = 0;
        if (rocksdb_iter_valid(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) == 0) {
                try {
                    first_index = parse_log_key_or_throw(key);
                } catch (...) {
                    rocksdb_iter_destroy(it);  // @unsafe
                    throw;
                }
            }
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            rocksdb_iter_destroy(it);  // @unsafe
            throw_rocksdb_read_error("RocksDB first-index iterator", &err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return first_index;
    }

    // @unsafe - Uses RocksDB API
    slotid_t get_last_index() const override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
            return 0;
        }

        std::string prefix_end = "log;";

        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            throw std::runtime_error("RocksDB last-index iterator creation failed");
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
                try {
                    last_index = parse_log_key_or_throw(key);
                } catch (...) {
                    rocksdb_iter_destroy(it);  // @unsafe
                    throw;
                }
            }
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            rocksdb_iter_destroy(it);  // @unsafe
            throw_rocksdb_read_error("RocksDB last-index iterator", &err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

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
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
            return 0;
        }

        size_t count = 0;
        rocksdb_iterator_t* it = rocksdb_create_iterator(db_, read_options_);  // @unsafe
        if (it == nullptr) {
            throw std::runtime_error("RocksDB size iterator creation failed");
        }

        rocksdb_iter_seek(it, LOG_PREFIX, std::strlen(LOG_PREFIX));
        for (; rocksdb_iter_valid(it); rocksdb_iter_next(it)) {
            size_t key_len = 0;
            const char* key_ptr = rocksdb_iter_key(it, &key_len);
            std::string key = copy_slice(key_ptr, key_len);
            if (key.compare(0, std::strlen(LOG_PREFIX), LOG_PREFIX) != 0) {
                break;
            }
            try {
                (void)parse_log_key_or_throw(key);
            } catch (...) {
                rocksdb_iter_destroy(it);  // @unsafe
                throw;
            }
            count++;
        }

        char* err = nullptr;
        rocksdb_iter_get_error(it, &err);
        if (err != nullptr) {
            rocksdb_iter_destroy(it);  // @unsafe
            throw_rocksdb_read_error("RocksDB size iterator", &err);
        }
        rocksdb_iter_destroy(it);  // @unsafe

        return count;
    }

    // @unsafe - Calls size() which uses RocksDB
    bool empty() const override {
        return rocksdb_log_empty_from_size(size());  // @unsafe
    }

    // ========================================================================
    // Metadata Operations
    // ========================================================================

    // @unsafe - Uses RocksDB API
    bool set_metadata(const std::string& key, const std::string& value) override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
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

    // @unsafe - A single RocksDB WriteBatch is atomic across every metadata key
    bool set_metadata_batch(
        const std::vector<std::pair<std::string, std::string>>& entries)
        override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
            return false;
        }

        rocksdb_writebatch_t* batch = rocksdb_writebatch_create();
        if (batch == nullptr) {
            return false;
        }
        for (const auto& [key, value] : entries) {
            const std::string meta_key = make_meta_key(key);
            rocksdb_writebatch_put(batch, meta_key.data(), meta_key.size(),
                                   value.data(), value.size());
        }

        char* err = nullptr;
        rocksdb_write(db_, write_options_, batch, &err);
        rocksdb_writebatch_destroy(batch);
        if (err != nullptr) {
            take_rocksdb_error(&err);
            return false;
        }
        return true;
    }

    // @unsafe - Uses RocksDB API
    rusty::Option<std::string> get_metadata(const std::string& key) const override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
            return rusty::None;
        }

        std::string meta_key = make_meta_key(key);
        size_t value_len = 0;
        char* err = nullptr;
        char* value_ptr = rocksdb_get(db_, read_options_,
                                      meta_key.data(), meta_key.size(),
                                      &value_len, &err);

        if (err != nullptr) {
            throw_rocksdb_read_error("RocksDB metadata get", &err);
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

    // @unsafe - Uses RocksDB API
    bool sync() override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
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

    // @unsafe - Uses RocksDB API
    bool close() override {
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
    bool is_open() const override {
        return is_open_.get();
    }

    // @unsafe - Uses RocksDB API
    bool clear() override {
        if (rocksdb_log_storage_is_closed(is_open_.get()) ||
            rocksdb_log_storage_missing_db(db_ != nullptr)) {
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
    // @unsafe - Uses RocksDB API
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

}  // namespace raft
}  // namespace janus
