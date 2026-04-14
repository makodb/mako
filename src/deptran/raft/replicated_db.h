#pragma once
#include "__dep__.h"
#include "rrr/misc/marshal.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <rocksdb/c.h>

namespace janus {

class RaftServer;  // Forward declaration

// @safe - Trivially copyable enum for operation type
enum class ReplicatedDBOp : uint8_t {
    PUT = 1,
    DELETE = 2,
    BATCH = 3
};

// @safe - Plain data struct for batch operations
struct KVOperation {
    ReplicatedDBOp op;
    std::string key;
    std::string value;  // empty for DELETE
};

// @unsafe - Inherits from non-borrow-checked Marshallable
class ReplicatedDBCommand : public Marshallable {
public:
    ReplicatedDBOp op_ = ReplicatedDBOp::PUT;
    std::string key_;
    std::string value_;
    std::vector<KVOperation> batch_ops_;  // Only used when op_ == BATCH

    // @unsafe - Calls Marshallable constructor (non-borrow-checked)
    ReplicatedDBCommand() : Marshallable(MarshallDeputy::CMD_REPLICATED_DB) {}

    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreatePut(const std::string& key, const std::string& value);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateDelete(const std::string& key);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateBatch(const std::vector<KVOperation>& ops);

    // @unsafe - Marshallable interface (non-borrow-checked I/O)
    Marshal& to_marshal(Marshal& m) const override;
    // @unsafe - Marshallable interface (non-borrow-checked I/O)
    Marshal& from_marshal(Marshal& m) override;
};

/**
 * ReplicatedDB - A replicated key-value store built on Raft + RocksDB.
 *
 * Write path (Put/Delete): serializes as ReplicatedDBCommand, submits through
 * Raft via Start(), blocks until commit callback fires.
 *
 * Read path (Get): reads directly from local RocksDB (stale reads).
 *
 * Apply callback (ApplyEntry): registered via RegLearnerAction, applies
 * committed entries to local RocksDB with idempotency tracking.
 */
// @unsafe - Wraps RocksDB C API pointers and Raft server interaction
class ReplicatedDB {
public:
    // @unsafe - Opens RocksDB, stores raw pointers
    ReplicatedDB(RaftServer* raft, const std::string& db_path);

    // @unsafe - Closes RocksDB
    ~ReplicatedDB();

    // Non-copyable, non-movable
    ReplicatedDB(const ReplicatedDB&) = delete;
    ReplicatedDB& operator=(const ReplicatedDB&) = delete;

    // Write operations (go through Raft)
    // @unsafe - Submits command via Raft, blocks until committed
    bool Put(const std::string& key, const std::string& value);

    // @unsafe - Submits delete command via Raft, blocks until committed
    bool Delete(const std::string& key);

    // Read operation (local RocksDB, stale reads)
    // @unsafe - Direct RocksDB read
    bool Get(const std::string& key, std::string* value);

    // Apply callback - registered as app_next_ on RaftServer's scheduler
    // @unsafe - Applies commands to RocksDB
    void ApplyEntry(int slot, shared_ptr<Marshallable> cmd);

    // @safe - Returns whether RocksDB is open
    bool IsOpen() const { return db_ != nullptr; }

    // @safe - Returns last applied Raft index
    uint64_t GetLastAppliedIndex() const { return last_applied_index_; }

    // Snapshot support: create and load RocksDB checkpoints for Raft snapshots
    // @unsafe - Creates RocksDB checkpoint, serializes files into binary blob
    std::string CreateStateMachineSnapshot();

    // @unsafe - Deserializes binary blob, replaces current RocksDB with checkpoint
    void LoadStateMachineSnapshot(const std::string& data);

private:
    // @unsafe - Helper to free RocksDB error strings
    static std::string take_rocksdb_error(char** errptr);

    // @unsafe - Applies a single PUT to RocksDB
    void ApplyPut(const std::string& key, const std::string& value);

    // @unsafe - Applies a single DELETE to RocksDB
    void ApplyDelete(const std::string& key);

    // @unsafe - Persists last_applied_index_ to RocksDB metadata
    void PersistLastAppliedIndex();

    // @unsafe - Loads last_applied_index_ from RocksDB metadata
    void LoadLastAppliedIndex();

    // @unsafe - Closes the current RocksDB instance (for snapshot loading)
    void CloseDB();

    // @unsafe - Opens (or reopens) RocksDB at db_path_
    bool OpenDB();

    RaftServer* raft_;  // Non-owning pointer, lifetime managed externally
    rocksdb_t* db_ = nullptr;
    rocksdb_options_t* options_ = nullptr;
    rocksdb_writeoptions_t* write_options_ = nullptr;
    rocksdb_readoptions_t* read_options_ = nullptr;
    std::string db_path_;
    uint64_t last_applied_index_ = 0;

    static constexpr const char* META_LAST_APPLIED = "__raft_last_applied__";
};

} // namespace janus
