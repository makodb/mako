#pragma once
#include "__dep__.h"
#include "rrr/rrr.hpp"
#include "../mako_commands.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <rocksdb/c.h>
#include "lz4.h"

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

// TypeList-derived kind. Wire payload preserved
// byte-for-byte:
//   uint8_t op | std::string key | std::string value
//   [if op == BATCH] uint32_t count
//   for each batch op: uint8_t op | std::string key | std::string value
// Construction sites use `wrap_typed_marshallable` (still works through
// the Phase 4d-prep bridge dispatch) and `marshallable_cast<T>` (also
// dispatched through the bridge for any non-Marshallable T). The
// legacy `to_marshal` / `from_marshal` member functions are gone as
// thin wrappers that build a BinaryWriteArchive/BinaryReadArchive on
// top of the archive serde surface via save/load — this
// keeps the existing test.cc round-trip sites compiling unchanged.
class ReplicatedDBCommand : public rrr::Serializable<ReplicatedDBCommand,
                                                     MakoCommands> {
public:
    ReplicatedDBOp op_ = ReplicatedDBOp::PUT;
    std::string key_;
    std::string value_;
    std::vector<KVOperation> batch_ops_;  // Only used when op_ == BATCH

    ReplicatedDBCommand() = default;

    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreatePut(const std::string& key, const std::string& value);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateDelete(const std::string& key);
    // @unsafe - Factory: creates shared_ptr (non-borrow-checked ownership)
    static shared_ptr<ReplicatedDBCommand> CreateBatch(const std::vector<KVOperation>& ops);

    // Serializable interface.
    void save(BinaryWriteArchive& ar) const;
    void load(BinaryReadArchive& ar);

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

    // @unsafe - Submits batch command via Raft, blocks until committed
    bool Batch(const std::vector<KVOperation>& ops);

    // Read operation (local RocksDB, stale reads)
    // @unsafe - Direct RocksDB read
    bool Get(const std::string& key, std::string* value);

    // Linearizable read via ReadIndex protocol.
    // Confirms this server is still leader and all committed entries are applied,
    // then reads from local RocksDB. Avoids writing a log entry for reads.
    // @unsafe - Calls RaftServer::ReadIndex and RocksDB read
    bool LinearizableGet(const std::string& key, std::string* value);

    // Apply callback - registered as app_next_ on RaftServer's scheduler
    // @unsafe - Applies commands to RocksDB
    // take const janus::Command&;
    // shared_ptr<Marshallable> callers auto-convert via Command's
    // implicit ctor.
    void ApplyEntry(int slot, const janus::Command& cmd);

    // @safe - Returns whether RocksDB is open
    bool IsOpen() const { return db_ != nullptr; }

    // @safe - Returns last applied Raft index
    uint64_t GetLastAppliedIndex() const { return last_applied_index_; }

    // @safe - Returns whether snapshot compression is enabled
    bool IsCompressionEnabled() const { return compression_enabled_; }

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
    bool compression_enabled_ = true;  // LZ4 snapshot compression (env: MAKO_SNAPSHOT_COMPRESSION)

    static constexpr const char* META_LAST_APPLIED = "__raft_last_applied__";

    // Snapshot blob header byte values
    static constexpr uint8_t SNAPSHOT_UNCOMPRESSED = 0;
    static constexpr uint8_t SNAPSHOT_LZ4 = 1;
};

} // namespace janus

// (ReplicatedDBCommand is now a Serializable — no
// TypedMarshallableAdapterTraits specialization. See replicated_db.cc
// for the `reg_serializable_in_deputy` registration. Construction
// sites use `wrap_typed_marshallable(cmd)`; cast sites use
// `marshallable_cast<ReplicatedDBCommand>(value)` — both routed
// through the Phase 4d-prep bridge overloads — the non-Marshallable
// path that wraps as `SerializableMarshallableAdapter`.)
