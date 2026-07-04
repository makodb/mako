#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdint>

#include "sharding_policy.h"

namespace janus {

struct ShardInfo {
    uint32_t id = 0;
    std::vector<std::string> replicas;
    std::string leader;
    std::string status;  // "active", "draining", "dead", etc.
    // For status == "dead": taker shard whose Raft group inherits
    // requests that would hash to this shard. 0 means unset. The taker
    // may itself be dead — GetShardForKey chases transitively with a
    // cycle guard.
    uint32_t replacement = 0;
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

    // Routing entry point. If a per-table policy has been registered
    // for `table`, the policy's KeyExtractor extracts the sharding key
    // from `key` and the resulting shard id is returned. Otherwise the
    // default sharding mode kicks in (hash-mod of the raw key). Either
    // way, if the landed shard is currently "dead", the replacement
    // pointer is chased transitively.
    //
    // The single-arg overload is preserved for callers that already
    // know they want the default mode.
    // @safe - Pure computation under lock
    uint32_t GetShardForKey(const std::string& table,
                            const std::string& key) const;
    uint32_t GetShardForKey(const std::string& key) const;

    // Register or replace a per-table sharding policy. The policy is
    // owned by ClusterConfig by value.
    // @unsafe - Acquires mutex
    void SetTablePolicy(const std::string& table, TableShardingPolicy policy);

    // Remove a per-table policy. No-op if none is registered.
    // @unsafe - Acquires mutex
    void ClearTablePolicy(const std::string& table);

    // @safe - Read under lock. Returns true iff a policy exists for the table.
    bool HasTablePolicy(const std::string& table) const;

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
    std::map<std::string, TableShardingPolicy> table_policies_;

    // @safe - Pure hash function (FNV-1a)
    static uint32_t HashKey(const std::string& key);

    // @safe - Under-lock helper: chase replacement pointer for dead shards
    uint32_t FollowReplacement_(uint32_t sid) const;

    // @safe - Under-lock helper: extract a sharding key value from a raw
    // key using the given extractor (FIELD_INDEX / PREFIX_BYTES). Returns
    // an int64 like the existing shard_router.cc byte-decode path.
    static int64_t ExtractKeyValue_(const KeyExtractor& ext,
                                    const std::string& key);
};

// Process-global ClusterConfig — the routing cache the production shard
// router consults. Populated by the ConfigWatcher update callback (a
// LoadFromConfigManager into this instance). Until it has a nonzero
// shard_count, the router falls back to the legacy ShardingPolicyCache
// path, so this is a no-op until something wires the watcher in.
// @safe - returns a reference to a function-local static
ClusterConfig& get_cluster_config();

} // namespace janus
