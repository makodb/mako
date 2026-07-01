#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

namespace janus {

struct ShardInfo {
    uint32_t id = 0;
    std::vector<std::string> replicas;
    std::string leader;
    std::string status;  // "active", "draining", etc.
};

/**
 * ClusterConfig - In-memory read-only cache of the cluster topology.
 *
 * Every node holds a local copy. Provides GetShardForKey() routing using
 * hash-based sharding. Updated by ConfigWatcher or populated from
 * ConfigManager via LoadFromConfigManager().
 *
 * Thread-safe: all accessors and mutators are protected by a mutex.
 */
class ClusterConfig {
public:
    // @safe
    ClusterConfig() = default;

    // Load from a ConfigManager (reads all config keys)
    // @unsafe - Calls ConfigManager which reads from RocksDB via Raft
    bool LoadFromConfigManager(class ConfigManager* cm);

    // Routing: hash-based shard assignment
    // @safe - Pure computation under lock
    uint32_t GetShardForKey(const std::string& key) const;

    // Accessors (thread-safe, return copies)
    // @safe - Read under lock
    uint32_t GetShardCount() const;
    std::vector<std::string> GetShardReplicas(uint32_t shard_id) const;
    std::string GetShardLeader(uint32_t shard_id) const;
    std::string GetShardStatus(uint32_t shard_id) const;
    uint64_t GetVersion() const;
    uint64_t GetEpoch() const;

    // Mutators (thread-safe, for bulk updates)
    // @unsafe - Acquires mutex
    void UpdateShard(uint32_t id, const ShardInfo& info);
    void SetShardCount(uint32_t count);
    void SetVersion(uint64_t version);
    void SetEpoch(uint64_t epoch);

private:
    mutable std::mutex mtx_;
    uint32_t shard_count_ = 0;
    uint64_t version_ = 0;
    uint64_t epoch_ = 0;
    std::map<uint32_t, ShardInfo> shards_;

    // @safe - Pure hash function (FNV-1a)
    static uint32_t HashKey(const std::string& key);
};

} // namespace janus
