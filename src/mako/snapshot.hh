#pragma once

/**
 * mako/snapshot.hh - EagerSnapshot: per-table buffered point-in-time snapshot
 *
 * Design: Option A with per-table lazy buffering.
 *
 * On first access to a table through this snapshot, a short read-only
 * transaction is started, the entire table is scanned into an in-memory
 * std::map, and the transaction is committed. All subsequent reads for
 * that table are served from the buffer — Masstree is not touched again.
 *
 * Consistency guarantee: within a single table all reads are consistent
 * (they all come from one Sto transaction). Cross-table consistency is
 * NOT guaranteed (each table is captured at first-access time, not at
 * GetSnapshot() time).
 *
 * Limitations vs RocksDB snapshots:
 *   - O(N) memory per buffered table.
 *   - O(N) latency on first access per table.
 *   - No cross-table consistency.
 *   - Sequence number is a process-local counter, not a global LSN.
 *
 * See docs/snapshot_design.md for the full feasibility analysis.
 */

#include "idb.hh"
#include "status.hh"
#include "benchmarks/abstract_db.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mako {

/**
 * SnapshotIterator - Iterator over a buffered snapshot table.
 *
 * Wraps a const reference to an std::map<string,string> buffer.
 * All navigation is purely in-memory; no transaction or table access.
 */
// @safe - Read-only iterator over an immutable buffer
class SnapshotIterator : public IIterator {
public:
    using Buffer = std::map<std::string, std::string>;

    // @safe - Constructor borrows buffer (must outlive iterator)
    explicit SnapshotIterator(const Buffer& buf)
        : buf_(buf), it_(buf_.end()), valid_(false), status_(Status::OK()) {}

    void Seek(const std::string& target) override {
        it_ = buf_.lower_bound(target);
        valid_ = (it_ != buf_.end());
        status_ = Status::OK();
    }

    void SeekToFirst() override {
        it_ = buf_.begin();
        valid_ = (it_ != buf_.end());
        status_ = Status::OK();
    }

    void SeekToLast() override {
        if (buf_.empty()) {
            valid_ = false;
            it_ = buf_.end();
        } else {
            it_ = std::prev(buf_.end());
            valid_ = true;
        }
        status_ = Status::OK();
    }

    void Next() override {
        if (!valid_) return;
        ++it_;
        valid_ = (it_ != buf_.end());
    }

    void Prev() override {
        if (!valid_) return;
        if (it_ == buf_.begin()) {
            valid_ = false;
        } else {
            --it_;
        }
    }

    bool Valid() const override { return valid_; }

    std::string key() const override {
        return valid_ ? it_->first : "";
    }

    std::string value() const override {
        return valid_ ? it_->second : "";
    }

    Status status() const override { return status_; }

private:
    const Buffer& buf_;
    Buffer::const_iterator it_;
    bool valid_;
    Status status_;
};

/**
 * EagerSnapshot - Concrete ISnapshot implementation (Option A, lazy per-table)
 *
 * Usage:
 *   ISnapshot* snap = db->GetSnapshot();
 *   std::string v;
 *   snap->Get("my_table", "k1", v);    // first access: scans my_table
 *   snap->Get("my_table", "k2", v);    // served from buffer
 *   db->ReleaseSnapshot(snap);
 */
// @safe - All mutation through named methods; buffer access protected by mutex
class EagerSnapshot : public ISnapshot {
public:
    /**
     * Construct a snapshot.
     * @param db  IDatabase to scan tables through (borrowed; must outlive snapshot)
     * @param seq Sequence number assigned by DB::GetSnapshot()
     */
    // @safe - Constructor records borrowed db pointer and seq number
    EagerSnapshot(IDatabase* db, uint64_t seq)
        : db_(db), seq_(seq) {}

    // @safe - Destructor frees all table buffers
    ~EagerSnapshot() override = default;

    // @safe - Get a key from the snapshot; buffers table on first access
    Status Get(const std::string& table_name,
               const std::string& key,
               std::string& value) override {
        const auto& buf = ensure_buffered(table_name);
        auto it = buf.find(key);
        if (it == buf.end()) {
            return Status::NotFound("Key not found in snapshot");
        }
        value = it->second;
        return Status::OK();
    }

    // @safe - Check existence; buffers table on first access
    Status Exists(const std::string& table_name,
                  const std::string& key,
                  bool* exists) override {
        const auto& buf = ensure_buffered(table_name);
        *exists = (buf.find(key) != buf.end());
        return Status::OK();
    }

    // @safe - Create iterator over buffered table
    IIterator* NewIterator(const std::string& table_name) override {
        const auto& buf = ensure_buffered(table_name);
        return new SnapshotIterator(buf);
    }

    // @safe - Returns the snapshot sequence number
    uint64_t GetSequenceNumber() const override { return seq_; }

private:
    using Buffer = std::map<std::string, std::string>;

    IDatabase* db_;   // borrowed; not owned
    uint64_t seq_;

    // Per-table buffers (populated lazily on first access)
    std::unordered_map<std::string, Buffer> table_buffers_;
    // Tracks tables that have been scanned (including empty tables)
    std::unordered_map<std::string, bool> scanned_;
    std::mutex mu_;

    /**
     * Return a const reference to the buffer for `table_name`.
     * If not yet scanned, scans the entire table into the buffer first.
     * Retries up to kMaxRetries times if the OCC transaction aborts.
     *
     * @safe - mutex protects concurrent first-access races
     */
    static constexpr int kMaxRetries = 20;

    const Buffer& ensure_buffered(const std::string& table_name) {
        std::lock_guard<std::mutex> lock(mu_);
        if (scanned_.count(table_name)) {
            return table_buffers_[table_name];
        }

        Buffer& buf = table_buffers_[table_name];
        scanned_[table_name] = true;

        ITable* table = db_->GetTable(table_name);
        if (!table) {
            // Table doesn't exist — buffer stays empty
            return buf;
        }

        // Begin a short read-only transaction, scan the whole table, commit.
        // OCC may abort the transaction (e.g., if the epoch advancer races with
        // our read set). Retry up to kMaxRetries times.
        // @unsafe { calls into OCC transaction layer }
        for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
            buf.clear();
            db_->BeginTransaction();  // returns nullptr; starts thread-local Sto txn

            Status scan_s = table->Scan(nullptr, "", nullptr,
                [&](const std::string& k, const std::string& v) -> bool {
                    buf[k] = v;
                    return true; // scan all keys
                });

            if (!scan_s.ok()) {
                // Scan was aborted inside the try-catch in LocalTable::Scan.
                // Rollback and retry.
                db_->Rollback(nullptr);
                buf.clear();
                continue;
            }

            try {
                db_->Commit(nullptr);
                return buf;  // success
            } catch (abstract_db::abstract_abort_exception&) {
                // OCC validation failed; retry
                buf.clear();
            } catch (...) {
                // Unknown error; leave buffer empty
                buf.clear();
                return buf;
            }
        }

        // All retries exhausted — return whatever is in the buffer
        // (may be empty, which means all Gets will return NotFound)
        return buf;
    }
};

}  // namespace mako
