#pragma once

/**
 * mako/local_table.hh - Local Table Wrapper implementing ITable
 *
 * This header provides LocalTable, a wrapper around mbta_sharded_ordered_index
 * that implements the ITable interface for unified database access.
 */

#include "idb.hh"
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

    // @safe - Access underlying index (for advanced operations)
    mbta_sharded_ordered_index* GetIndex() { return index_; }
    const mbta_sharded_ordered_index* GetIndex() const { return index_; }

private:
    mbta_sharded_ordered_index* index_;  // Borrowed pointer (not owned)
    std::string name_;
};

}  // namespace mako
