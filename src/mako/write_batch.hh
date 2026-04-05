#pragma once

/**
 * mako/write_batch.hh - WriteBatch implementing IWriteBatch
 *
 * WriteBatch accumulates Put, Delete, and DeleteRange operations and applies
 * them atomically in a single transaction via Write().
 *
 * Design:
 *   1. Queue operations in an in-memory vector (no reads allowed in a batch).
 *   2. On Write(): begin transaction, apply all queued operations, commit.
 *   3. If any operation fails or commit throws (OCC abort), rollback the
 *      entire transaction — all-or-nothing semantics.
 *   4. On success, the batch is automatically cleared.
 *
 * StringWrapper aliasing note:
 *   mako::Encode() returns a StringWrapper whose underlying buffer may be
 *   invalidated if used as a temporary.  We pre-encode ALL Put values into a
 *   named std::vector<std::string> before opening the transaction so the
 *   encoded strings remain live until after Commit().
 */

#include "idb.hh"
#include <string>
#include <vector>

namespace mako {

// @safe - WriteBatch class using IDatabase interface
class WriteBatch : public IWriteBatch {
public:
    // @safe - Constructor takes borrowed pointer (not owned)
    explicit WriteBatch(IDatabase* db) : db_(db) {}

    // @safe - Queue a Put operation
    void Put(const std::string& table_name,
             const std::string& key,
             const std::string& value) override {
        ops_.push_back({OpType::PUT, table_name, key, value, ""});
    }

    // @safe - Queue a Delete operation
    void Delete(const std::string& table_name,
                const std::string& key) override {
        ops_.push_back({OpType::DELETE, table_name, key, "", ""});
    }

    // @safe - Queue a DeleteRange operation
    void DeleteRange(const std::string& table_name,
                     const std::string& start_key,
                     const std::string& end_key) override {
        ops_.push_back({OpType::DELETE_RANGE, table_name, start_key, "", end_key});
    }

    // @safe - Return number of queued operations
    size_t Count() const override {
        return ops_.size();
    }

    // @safe - Clear all queued operations
    void Clear() override {
        ops_.clear();
    }

    // Apply all queued operations atomically.
    // Declared here; inline implementation provided below under _MAKO_COMMON_H_.
    Status Write() override;

private:
    enum class OpType { PUT, DELETE, DELETE_RANGE };

    struct BatchOp {
        OpType type;
        std::string table_name;
        std::string key;
        std::string value;    // for PUT: raw (un-encoded) value
        std::string end_key;  // for DELETE_RANGE
    };

    IDatabase* db_;           // Borrowed pointer (not owned)
    std::vector<BatchOp> ops_;
};

}  // namespace mako

// ============================================================================
// Inline implementation — requires mako.hh to be included first so that
// mako::Encode() (from lib/common.h) and abstract_db internals are available.
// ============================================================================
#ifdef _MAKO_COMMON_H_

namespace mako {

// @unsafe - Calls IDatabase/ITable methods which may use raw pointers
inline Status WriteBatch::Write() {
    if (ops_.empty()) {
        return Status::OK();
    }

    // Pre-encode all PUT values into named strings that outlive the transaction.
    // CRITICAL: never pass mako::Encode() as a temporary to Put — the
    // StringWrapper aliasing bug will cause the pointer to dangle.
    std::vector<std::string> encoded;
    encoded.reserve(ops_.size());
    for (const auto& op : ops_) {
        if (op.type == OpType::PUT) {
            encoded.push_back(Encode(op.value));  // @unsafe: calls Encode
        } else {
            encoded.emplace_back();  // placeholder to keep indices aligned
        }
    }

    void* txn = db_->BeginTransaction();

    try {
        for (size_t i = 0; i < ops_.size(); ++i) {
            const auto& op = ops_[i];

            ITable* table = db_->GetTable(op.table_name);
            if (!table) {
                db_->Rollback(txn);  // @unsafe: abort_txn
                return Status::InvalidArgument("WriteBatch::Write: table not found: " + op.table_name);
            }

            Status s = Status::OK();
            if (op.type == OpType::PUT) {
                s = table->Put(txn, op.key, encoded[i]);
            } else if (op.type == OpType::DELETE) {
                s = table->Delete(txn, op.key);
            } else if (op.type == OpType::DELETE_RANGE) {
                s = table->DeleteRange(txn, op.key, &op.end_key);
            }

            if (!s.ok()) {
                db_->Rollback(txn);  // @unsafe: abort_txn
                return s;
            }
        }

        db_->Commit(txn);  // @unsafe: may throw on OCC abort
    } catch (...) {
        try { db_->Rollback(txn); } catch (...) {}
        return Status::IOError("WriteBatch::Write: transaction aborted (OCC conflict)");
    }

    Clear();
    return Status::OK();
}

}  // namespace mako

#endif  // _MAKO_COMMON_H_
