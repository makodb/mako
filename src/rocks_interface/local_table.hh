#pragma once

/**
 * mako/local_table.hh - Local Table Wrapper implementing ITable
 *
 * This header provides LocalTable, a wrapper around mbta_sharded_ordered_index
 * that implements the ITable interface for unified database access.
 */

#include "idb.hh"
#include "status.hh"
#include "mako/benchmarks/mbta_sharded_ordered_index.hh"
#include "mako/benchmarks/abstract_db.h"
#include <string>

namespace mako {

/**
 * LocalTable - Wrapper around mbta_sharded_ordered_index implementing ITable
 *
 * This class adapts the local sharded index to the common ITable interface,
 * enabling unified code that works with both local and remote tables.
 */
// @safe - Wrapper class delegating to underlying index
class LocalTable : public ITable {
public:
    // @safe - Constructor takes borrowed pointer to underlying index
    LocalTable(mbta_sharded_ordered_index* index, const std::string& name)
        : index_(index), name_(name) {}

    // @safe - Delegates to underlying index
    // Note: txn can be NULL for mbta_wrapper which uses thread-local transaction state
    Status Put(void* txn, const std::string& key, const std::string& value) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            return index_->Put(txn, key, value);
        } catch (abstract_db::abstract_abort_exception& ex) {
            return Status::IOError("Transaction aborted");
        } catch (...) {
            return Status::IOError("Unknown error in Put");
        }
    }

    // @safe - Delegates to underlying index
    // Note: txn can be NULL for mbta_wrapper which uses thread-local transaction state
    Status Get(void* txn, const std::string& key, std::string& value) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            return index_->Get(txn, key, value);
        } catch (abstract_db::abstract_abort_exception& ex) {
            return Status::IOError("Transaction aborted");
        } catch (...) {
            return Status::IOError("Unknown error in Get");
        }
    }

    // @safe - Delegates to underlying index
    // Note: txn can be NULL for mbta_wrapper which uses thread-local transaction state
    Status Delete(void* txn, const std::string& key) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            return index_->Delete(txn, key);
        } catch (abstract_db::abstract_abort_exception& ex) {
            return Status::IOError("Transaction aborted");
        } catch (...) {
            return Status::IOError("Unknown error in Delete");
        }
    }

    // @safe - Returns stored name
    const std::string& GetName() const override {
        return name_;
    }

    // @safe - Bridges scan_callback::invoke to std::function
    class ScanAdapter : public abstract_ordered_index::scan_callback {
    public:
        explicit ScanAdapter(std::function<bool(const std::string&, const std::string&)> fn)
            : fn_(std::move(fn)) {}
        // @safe - Converts raw key pointer to std::string then calls user function
        bool invoke(const char* keyp, size_t keylen, const std::string& value) override {
            return fn_(std::string(keyp, keylen), value);
        }
    private:
        std::function<bool(const std::string&, const std::string&)> fn_;
    };

    // @safe - Scans local shard only via mbta_sharded_ordered_index::scan().
    // In multi-shard deployments, returns NotImplemented — full cross-shard scan
    // is pending and will be built on top of Shuai's upcoming changes.
    Status Scan(void* txn,
                const std::string& start_key,
                const std::string* end_key,
                std::function<bool(const std::string& key, const std::string& value)> callback) override {
        if (!index_) {
            return Status::InvalidArgument("Scan: invalid table");
        }
        if (!callback) {
            return Status::InvalidArgument("Scan: callback must not be null");
        }
        if (end_key != nullptr && *end_key <= start_key) {
            return Status::InvalidArgument(
                "Scan: end_key must be strictly greater than start_key");
        }
        // Multi-shard scan is not yet implemented. In single-shard mode the local
        // shard is the only shard, so the scan is correct. In multi-shard mode keys
        // are distributed across shards and a local-only scan would silently return
        // incomplete results, so we reject it explicitly.
        if (index_->shard_for_index(1) != nullptr) {
            return Status::NotSupported(
                "Scan across multiple shards is not yet implemented");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            ScanAdapter adapter(std::move(callback));
            index_->scan(txn, start_key, end_key, adapter);
            return Status::OK();
        } catch (abstract_db::abstract_abort_exception&) {
            return Status::IOError("Scan: transaction aborted");
        } catch (...) {
            return Status::IOError("Scan: unknown error");
        }
    }

    // @safe - Delegates to underlying index rscan.
    // In multi-shard deployments, returns NotImplemented for the same reason as Scan.
    Status ReverseScan(void* txn,
                       const std::string& start_key,
                       const std::string* end_key,
                       std::function<bool(const std::string& key, const std::string& value)> callback) override {
        if (!index_) {
            return Status::InvalidArgument("ReverseScan: invalid table");
        }
        if (!callback) {
            return Status::InvalidArgument("ReverseScan: callback must not be null");
        }
        if (end_key != nullptr && *end_key >= start_key) {
            return Status::InvalidArgument(
                "ReverseScan: end_key must be strictly less than start_key");
        }
        if (index_->shard_for_index(1) != nullptr) {
            return Status::NotSupported(
                "ReverseScan across multiple shards is not yet implemented");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            ScanAdapter adapter(std::move(callback));
            index_->rscan(txn, start_key, end_key, adapter);
            return Status::OK();
        } catch (abstract_db::abstract_abort_exception&) {
            return Status::IOError("ReverseScan: transaction aborted");
        } catch (...) {
            return Status::IOError("ReverseScan: unknown error");
        }
    }

    // @safe - Check key existence without returning value
    Status Exists(void* txn, const std::string& key, bool* exists) override {
        if (!index_ || !exists) {
            return Status::InvalidArgument("Invalid argument");
        }
        std::string unused;
        Status s = Get(txn, key, unused);
        if (s.ok()) {
            *exists = true;
            return Status::OK();
        }
        if (s.IsNotFound()) {
            *exists = false;
            return Status::OK();
        }
        return s;
    }

    // @safe - Insert only if key does not exist (uses native transInsert path)
    Status Insert(void* txn, const std::string& key, const std::string& value) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            // Uses mbta_sharded_ordered_index::Insert() which calls transInsert
            // (native insert OCC semantics) rather than transPut (overwrite semantics).
            return index_->Insert(txn, key, value);
        } catch (abstract_db::abstract_abort_exception&) {
            return Status::IOError("Transaction aborted");
        } catch (...) {
            return Status::IOError("Unknown error in Insert");
        }
    }

    // @safe - Returns approximate size of the local shard only
    // Note: index_->size() sums over all local shard_tables_, but only one shard
    // holds actual data (the local shard). See GetApproximateSize doc in idb.hh.
    Status GetApproximateSize(size_t* size) override {
        if (!index_ || !size) {
            return Status::InvalidArgument("Invalid argument");
        }
        // @unsafe { Calls underlying index which uses raw pointers }
        *size = index_->size();
        return Status::OK();
    }

    // =========================================================================
    // Non-transactional API (implements the ITable non-txn surface
    // over the L3 non-txn ops; see idb.hh for semantics)
    // =========================================================================

    // @unsafe - L3 non-txn op runs an internal one-op OCC txn
    Status Put(const std::string& key, const std::string& value) override {
        if (!index_) return Status::InvalidArgument("Invalid table");
        index_->put(lcdf::Str(key), value);  // returns "newly inserted"
        return Status::OK();
    }

    // @unsafe - L3 non-txn op runs an internal one-op OCC txn
    Status Insert(const std::string& key, const std::string& value) override {
        if (!index_) return Status::InvalidArgument("Invalid table");
        return index_->insert(lcdf::Str(key), value)
                   ? Status::OK()
                   : Status::InvalidArgument("key already exists");
    }

    // @unsafe - L3 non-txn op runs an internal one-op OCC txn
    Status Get(const std::string& key, std::string& value) override {
        if (!index_) return Status::InvalidArgument("Invalid table");
        return index_->get(lcdf::Str(key), value) ? Status::OK()
                                                  : Status::NotFound();
    }

    // @unsafe - direct raw write through the MassTrans cursor
    Status Delete(const std::string& key) override {
        if (!index_) return Status::InvalidArgument("Invalid table");
        return index_->remove(lcdf::Str(key)) ? Status::OK()
                                              : Status::NotFound();
    }

    // @unsafe - L3 non-txn op runs an internal one-op OCC txn
    Status Exists(const std::string& key, bool* exists) override {
        if (!index_ || !exists) return Status::InvalidArgument("Invalid argument");
        std::string unused;
        *exists = index_->get(lcdf::Str(key), unused);
        return Status::OK();
    }

    // @safe - Access underlying index (for advanced operations)
    mbta_sharded_ordered_index* GetIndex() { return index_; }
    const mbta_sharded_ordered_index* GetIndex() const { return index_; }

private:
    mbta_sharded_ordered_index* index_;  // Borrowed pointer (not owned)
    std::string name_;
};

}  // namespace mako
