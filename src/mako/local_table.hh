#pragma once

/**
 * mako/local_table.hh - Local Table Wrapper implementing ITable
 *
 * This header provides LocalTable, a wrapper around mbta_sharded_ordered_index
 * that implements the ITable interface for unified database access.
 */

#include "idb.hh"
#include "buffered_iterator.hh"
#include "status.hh"
#include "benchmarks/mbta_sharded_ordered_index.hh"
#include "benchmarks/abstract_db.h"
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
            std::string key(keyp, keylen);
            return fn_(key, value);
        }
    private:
        std::function<bool(const std::string&, const std::string&)> fn_;
    };

    // @safe - Delegates to underlying index scan
    Status Scan(void* txn,
                const std::string& start_key,
                const std::string* end_key,
                std::function<bool(const std::string& key, const std::string& value)> callback) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            ScanAdapter adapter(std::move(callback));
            index_->scan(txn, start_key, end_key, adapter);
            return Status::OK();
        } catch (abstract_db::abstract_abort_exception&) {
            return Status::IOError("Transaction aborted");
        } catch (...) {
            return Status::IOError("Unknown error in Scan");
        }
    }

    // @safe - Delegates to underlying index rscan
    Status ReverseScan(void* txn,
                       const std::string& start_key,
                       const std::string* end_key,
                       std::function<bool(const std::string& key, const std::string& value)> callback) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        try {
            // @unsafe { Calls underlying index which uses raw pointers }
            ScanAdapter adapter(std::move(callback));
            index_->rscan(txn, start_key, end_key, adapter);
            return Status::OK();
        } catch (abstract_db::abstract_abort_exception&) {
            return Status::IOError("Transaction aborted");
        } catch (...) {
            return Status::IOError("Unknown error in ReverseScan");
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

    // @safe - Returns approximate size from underlying index
    // Note: Currently always returns 0 (Masstree approx_size() not yet implemented)
    Status GetApproximateSize(size_t* size) override {
        if (!index_ || !size) {
            return Status::InvalidArgument("Invalid argument");
        }
        // @unsafe { Calls underlying index which uses raw pointers }
        *size = index_->size();
        return Status::OK();
    }

    // @safe - Create a buffered iterator over this table; caller owns result
    IIterator* NewIterator(void* txn) override {
        // @unsafe { BufferedIterator calls Scan which uses raw pointers }
        return new BufferedIterator(this, txn);
    }

    // @safe - Scan-then-delete strategy; atomicity provided by the enclosing txn
    Status DeleteRange(void* txn,
                       const std::string& start_key,
                       const std::string* end_key,
                       size_t* deleted_count = nullptr) override {
        if (!index_) {
            return Status::InvalidArgument("Invalid table");
        }
        if (deleted_count) *deleted_count = 0;

        // Step 1: collect all keys in the range via Scan
        std::vector<std::string> keys;
        Status scan_status = Scan(txn, start_key, end_key,
            [&keys](const std::string& key, const std::string& /*value*/) -> bool {
                keys.push_back(key);
                return true;  // continue scanning
            });
        if (!scan_status.ok()) {
            return scan_status;
        }

        // Step 2: delete each collected key
        size_t count = 0;
        for (const std::string& key : keys) {
            Status s = Delete(txn, key);
            if (!s.ok()) {
                if (deleted_count) *deleted_count = count;
                return s;
            }
            ++count;
        }
        if (deleted_count) *deleted_count = count;
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
