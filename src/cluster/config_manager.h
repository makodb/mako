#pragma once
#include "cluster/replicated_kv.h"
#include <string>
#include <vector>

namespace janus {

/**
 * ConfigManager - Typed configuration management over a ReplicatedKV.
 *
 * Wraps a ReplicatedKV (production: a Raft+RocksDB-backed ReplicatedDB
 * on shard 0; tests: an in-memory fake) to provide typed methods for
 * cluster configuration. Every write atomically increments the
 * __version__ key via a BATCH operation.
 *
 * Key schema:
 *   __version__             — monotonically increasing config version (uint64)
 *   shard_count             — total number of shards (uint32)
 *   shard/<id>/replicas     — comma-separated list of replica site IDs
 *   shard/<id>/leader       — current leader site name
 *   shard/<id>/status       — active, draining, adding, removing, dead
 *   shard/<id>/replacement  — for status=dead: taker shard id whose Raft
 *                             group receives requests that used to route
 *                             here. Chase transitively (dead -> dead ok)
 *                             with cycle detection.
 *   epoch                   — global speculative epoch number (uint64)
 *   node/<site>/addr        — node network address
 *   node/<site>/status      — alive, dead, decommissioning
 */
// @unsafe - Wraps a ReplicatedKV (raw pointer; concrete impl may touch
// RocksDB/Raft).
class ConfigManager {
public:
    // @unsafe - Stores non-owning raw pointer to ReplicatedKV
    explicit ConfigManager(ReplicatedKV* db);

    // Non-copyable, non-movable
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // =========================================================================
    // Version management
    // =========================================================================

    // @unsafe - RocksDB read
    uint64_t GetVersion();

    // =========================================================================
    // Shard management
    // =========================================================================

    // @unsafe - RocksDB read
    uint32_t GetShardCount();

    // @unsafe - RocksDB batch write via Raft
    bool SetShardCount(uint32_t count);

    // @unsafe - RocksDB read, parses comma-separated string
    std::vector<std::string> GetShardReplicas(uint32_t shard_id);

    // @unsafe - RocksDB batch write via Raft
    bool SetShardReplicas(uint32_t shard_id, const std::vector<std::string>& replicas);

    // @unsafe - RocksDB read
    std::string GetShardLeader(uint32_t shard_id);

    // @unsafe - RocksDB batch write via Raft
    bool SetShardLeader(uint32_t shard_id, const std::string& leader);

    // @unsafe - RocksDB read
    std::string GetShardStatus(uint32_t shard_id);

    // @unsafe - RocksDB batch write via Raft
    bool SetShardStatus(uint32_t shard_id, const std::string& status);

    // =========================================================================
    // Shard lifecycle
    // =========================================================================

    // @unsafe - RocksDB batch write via Raft (sets replicas, status, increments shard_count)
    bool AddShard(uint32_t shard_id, const std::vector<std::string>& replicas);

    // @unsafe - RocksDB batch write via Raft (deletes shard keys, decrements shard_count)
    bool RemoveShard(uint32_t shard_id);

    // Kill a non-durable shard whose data is lost and hand its routing
    // over to a live shard. Semantics: shard/<dead_id>/status flips to
    // "dead", shard/<dead_id>/replacement is set to taker_id, replicas
    // are cleared, and the speculative epoch is advanced so any
    // in-flight speculative state that touched dead_id is invalidated.
    //
    // shard_count is NOT decremented — the hash used for routing stays
    // `hash(key) % shard_count`; ClusterConfig::GetShardForKey follows
    // the replacement pointer when the hash lands on a dead shard.
    //
    // Fails if:
    //   - dead_id == taker_id
    //   - taker shard doesn't exist (or has no replicas)
    //   - dead shard doesn't exist (nothing to kill)
    // @unsafe - RocksDB batch write via Raft (multi-key atomic)
    bool KillShard(uint32_t dead_id, uint32_t taker_id);

    // @unsafe - RocksDB read. Returns 0 if not set. A shard_id with
    // status != "dead" should never have a replacement pointer.
    uint32_t GetShardReplacement(uint32_t shard_id);

    // =========================================================================
    // Epoch management
    // =========================================================================

    // @unsafe - RocksDB read
    uint64_t GetEpoch();

    // @unsafe - RocksDB batch write via Raft (increments epoch + version)
    bool AdvanceEpoch();

    // =========================================================================
    // Node management
    // =========================================================================

    // @unsafe - RocksDB read
    std::string GetNodeAddr(const std::string& site);

    // @unsafe - RocksDB batch write via Raft
    bool SetNodeAddr(const std::string& site, const std::string& addr);

    // @unsafe - RocksDB read
    std::string GetNodeStatus(const std::string& site);

    // @unsafe - RocksDB batch write via Raft
    bool SetNodeStatus(const std::string& site, const std::string& status);

private:
    ReplicatedKV* db_;  // Non-owning pointer, lifetime managed externally

    // Helper: atomically write key+value together with __version__ increment
    // @unsafe - RocksDB read-modify-write via Raft BATCH
    bool PutWithVersion(const std::string& key, const std::string& value);

    // Helper: atomically execute multiple ops together with __version__ increment
    // @unsafe - RocksDB read-modify-write via Raft BATCH
    bool BatchWithVersion(const std::vector<KVOperation>& extra_ops);

    // @safe - Pure string formatting
    static std::string ShardKey(uint32_t id, const std::string& field);

    // @safe - Pure string formatting
    static std::string NodeKey(const std::string& site, const std::string& field);

    // @safe - Join strings with comma separator
    static std::string JoinReplicas(const std::vector<std::string>& replicas);

    // @safe - Split comma-separated string into vector
    static std::vector<std::string> SplitReplicas(const std::string& csv);

    // Key constants
    static constexpr const char* KEY_VERSION = "__version__";
    static constexpr const char* KEY_SHARD_COUNT = "shard_count";
    static constexpr const char* KEY_EPOCH = "epoch";
};

} // namespace janus
