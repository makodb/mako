#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace janus {

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

/**
 * ReplicatedKV — abstract interface for the replicated key-value store
 * that ConfigManager talks to.
 *
 * Production: implemented by ReplicatedDB (Raft + RocksDB) in
 * src/deptran/raft/replicated_db.{h,cc}. Writes go through Raft and
 * block until commit; Get is a stale local-RocksDB read.
 *
 * Tests: an in-memory fake can implement this interface to exercise
 * ConfigManager / ConfigWatcher without spinning up Raft.
 */
// @unsafe - Polymorphic base; concrete impls may touch RocksDB / Raft.
class ReplicatedKV {
public:
    virtual ~ReplicatedKV() = default;

    // Writes — durability semantics depend on the implementation.
    virtual bool Put(const std::string& key, const std::string& value) = 0;
    virtual bool Delete(const std::string& key) = 0;
    virtual bool Batch(const std::vector<KVOperation>& ops) = 0;

    // Read (stale).
    virtual bool Get(const std::string& key, std::string* value) = 0;
};

} // namespace janus
