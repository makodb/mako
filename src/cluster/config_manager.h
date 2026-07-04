#pragma once
#include "kv_store.h"
#include <string>
#include <vector>

namespace janus {

// A single point write in a config-change batch.
enum class ConfigOp : uint8_t { PUT, DELETE };
struct ConfigWrite {
    ConfigOp op;
    std::string key;
    std::string value;  // empty for DELETE
};

/**
 * ConfigManager - Typed configuration management over a KvStore.
 *
 * Wraps a KvStore port (get/put/remove). In production the port is
 * bound to Mako's unified FullOrderedIndex — the __mako_config__ system
 * table on shard 0 — via the OrderedIndexKvStore adapter; tests bind an
 * in-memory fake. cluster depends only on the port, so it compiles and
 * unit-tests with no storage-engine headers. The metadata really lives
 * in the unified store; this is a decoupling seam, not a parallel one.
 *
 * Consistency: multi-key writes are applied as a sequence of point puts
 * with the __version__ key written LAST, so a reader that observes a new
 * __version__ is guaranteed to see all keys of that version. This is not
 * atomic across keys the way a transaction would be, but config is
 * single-writer (shard 0's leader) and low-frequency; a reader that
 * races an in-flight change self-heals on its next version poll, and a
 * transient misroute is caught by the WrongShard-retry path. (Under the
 * "shard 0 never fails" assumption we don't replicate this table.)
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
 *   sharding/mode           — default routing mode "hash" or "range"
 *                             when no per-table policy is registered
 *   sharding/policy/<table> — opaque serialized TableShardingPolicy
 *                             bytes for one table. Absence means fall
 *                             back to sharding/mode.
 *   sharding/policy_tables  — comma-separated list of tables that
 *                             currently have a policy (maintained by
 *                             SetShardingPolicy / DeleteShardingPolicy
 *                             so ClusterConfig can enumerate without a
 *                             Scan primitive).
 */
// @unsafe - Wraps a KvStore (raw pointer; concrete impl is the adapter
// onto the storage engine, or an in-memory fake in tests).
class ConfigManager {
public:
    // @unsafe - Stores non-owning raw pointer to the KvStore.
    explicit ConfigManager(KvStore* kv);

    // Non-copyable, non-movable
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    // =========================================================================
    // Version management
    // =========================================================================

    // @unsafe - index read
    uint64_t GetVersion();

    // =========================================================================
    // Shard management
    // =========================================================================

    // @unsafe - index read
    uint32_t GetShardCount();

    // @unsafe - index write + version
    bool SetShardCount(uint32_t count);

    // @unsafe - index read, parses comma-separated string
    std::vector<std::string> GetShardReplicas(uint32_t shard_id);

    // @unsafe - index write + version
    bool SetShardReplicas(uint32_t shard_id, const std::vector<std::string>& replicas);

    // @unsafe - index read
    std::string GetShardLeader(uint32_t shard_id);

    // @unsafe - index write + version
    bool SetShardLeader(uint32_t shard_id, const std::string& leader);

    // @unsafe - index read
    std::string GetShardStatus(uint32_t shard_id);

    // @unsafe - index write + version
    bool SetShardStatus(uint32_t shard_id, const std::string& status);

    // =========================================================================
    // Shard lifecycle
    // =========================================================================

    // @unsafe - index write + version (sets replicas, status, increments shard_count)
    bool AddShard(uint32_t shard_id, const std::vector<std::string>& replicas);

    // @unsafe - index write + version (deletes shard keys, decrements shard_count)
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
    // @unsafe - index writes + version (see note: not cross-key atomic)
    bool KillShard(uint32_t dead_id, uint32_t taker_id);

    // @unsafe - index read. Returns 0 if not set. A shard_id with
    // status != "dead" should never have a replacement pointer.
    uint32_t GetShardReplacement(uint32_t shard_id);

    // =========================================================================
    // Epoch management
    // =========================================================================

    // @unsafe - index read
    uint64_t GetEpoch();

    // @unsafe - index write + version (increments epoch + version)
    bool AdvanceEpoch();

    // =========================================================================
    // Node management
    // =========================================================================

    // @unsafe - index read
    std::string GetNodeAddr(const std::string& site);

    // @unsafe - index write + version
    bool SetNodeAddr(const std::string& site, const std::string& addr);

    // @unsafe - index read
    std::string GetNodeStatus(const std::string& site);

    // @unsafe - index write + version
    bool SetNodeStatus(const std::string& site, const std::string& status);

    // =========================================================================
    // Sharding policy — opaque bytes
    // =========================================================================
    //
    // Callers serialize a TableShardingPolicy into a std::string and pass
    // it as bytes; ConfigManager stores those bytes verbatim under
    // sharding/policy/<table> and maintains sharding/policy_tables so the
    // set of registered tables can be enumerated without a KV Scan
    // primitive. This keeps ConfigManager free of the rrr::Marshal
    // dependency the policy types drag in.

    // @unsafe - index read
    std::string GetShardingMode();  // "" if unset (treated as "hash")

    // @unsafe - index write + version
    bool SetShardingMode(const std::string& mode);  // "hash" | "range"

    // @unsafe - index read. Returns empty string if unset.
    std::string GetShardingPolicy(const std::string& table);

    // @unsafe - index write + version. Serialized_policy is stored
    // verbatim; empty policy is refused (use DeleteShardingPolicy).
    bool SetShardingPolicy(const std::string& table,
                           const std::string& serialized_policy);

    // @unsafe - index write + version
    bool DeleteShardingPolicy(const std::string& table);

    // @unsafe - index read. Order is not stable across calls.
    std::vector<std::string> ListShardingPolicyTables();

private:
    KvStore* kv_;  // Non-owning; the __mako_config__ store (via the port).

    // Helper: write key+value, then bump __version__ (written last so a
    // reader that sees the new version sees this key too).
    // @unsafe - KvStore writes
    bool PutWithVersion(const std::string& key, const std::string& value);

    // Helper: apply extra_ops, then bump __version__ last.
    // @unsafe - KvStore writes
    bool BatchWithVersion(const std::vector<ConfigWrite>& extra_ops);

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
    static constexpr const char* KEY_SHARDING_MODE = "sharding/mode";
    static constexpr const char* KEY_SHARDING_POLICY_TABLES =
        "sharding/policy_tables";

    // @safe - Pure string formatting
    static std::string ShardingPolicyKey(const std::string& table) {
        return std::string("sharding/policy/") + table;
    }
};

} // namespace janus
