# Configuration Manager Design (Master Shard)

## Overview

The Configuration Manager is **shard 0** — the "master shard." It stores the system-wide configuration (cluster membership, shard assignments, routing metadata) as regular key-value entries replicated via Raft/Paxos. This means config changes are ACID transactions with the same durability and consistency guarantees as application data.

## Motivation

The current system uses YAML files to define the initial cluster topology. A separate ConfigStore/ConfigService prototype exists (`src/deptran/config_store.h`, `config_service.h`) but it is a standalone RocksDB instance outside the shard system — it doesn't benefit from Raft/Paxos replication, and it requires a separate bootstrap path.

YAML config files remain necessary for first boot (both development and production) — they define the initial topology that seeds shard 0. Once the cluster is running, shard 0 becomes the primary source of truth, and runtime changes go through it rather than requiring file edits across all nodes.

By making shard 0 the config manager:
- Config is **replicated** via the same Raft/Paxos protocol as data — no separate replication path.
- Config changes are **transactional** — atomic updates to membership, shard maps, etc.
- Existing `ITable::Put/Get/Delete` API works for config — no new storage layer needed.
- Other shards **bootstrap** by connecting to shard 0's replicas and reading config keys.
- Config **versioning** is automatic via Raft log index / Paxos slot.

## Architecture

```
                  +-----------------------------------------+
                  |           Shard 0 (Master Shard)        |
                  |                                         |
                  |  +-----------------------------------+  |
                  |  |  Config Table ("__mako_config__") |  |
                  |  |                                   |  |
                  |  |  Key                    Value     |  |
                  |  |  ──────────────────     ─────     |  |
                  |  |  __version__            <uint64>  |  |
                  |  |  shard/<id>/replicas    <json>    |  |
                  |  |  shard/<id>/leader      <site_id> |  |
                  |  |  shard/<id>/status      <active>  |  |
                  |  |  shard_count            <uint32>  |  |
                  |  |  sharding/policy        <json>    |  |
                  |  |  node/<site>/addr       <ip:port> |  |
                  |  |  node/<site>/status     <alive>   |  |
                  |  +-----------------------------------+  |
                  |                                         |
                  |  Replicated via Raft/Paxos (3+ replicas)|
                  +-----------------------------------------+
                       |            |             |
            +----------+    +------+------+    +--+----------+
            |               |             |                   |
      Shard 1          Shard 2       Shard N           Clients
      (bootstrap       (bootstrap    (bootstrap        (discover
       from shard 0)    from shard 0) from shard 0)     topology)
```

## Config Table Schema

All configuration is stored in a reserved table named `__mako_config__`. This table lives in shard 0 and is accessed via the standard `ITable` API.

### Key Namespace

| Key Pattern | Value Type | Description |
|-------------|-----------|-------------|
| `__version__` | `uint64` | Config version, incremented on every config change. Monotonically increasing. |
| `shard_count` | `uint32` | Total number of shards in the cluster. |
| `shard/<id>/replicas` | JSON | Ordered list of replica addresses for this shard. First entry is the preferred leader. Example: `["s101:8100","s201:8101","s301:8102"]` |
| `shard/<id>/leader` | `string` | Current leader site name for this shard. Updated on leader changes. |
| `shard/<id>/status` | `string` | Shard status: `active`, `draining`, `adding`, `removing`. |
| `shard/<id>/range_start` | `string` | Start of key range (for range-based sharding). Empty for hash-based. |
| `shard/<id>/range_end` | `string` | End of key range (for range-based sharding). Empty for hash-based. |
| `sharding/policy` | JSON | Sharding policy: `{"type":"hash","func":"murmur3"}` or `{"type":"range"}`. |
| `node/<site>/addr` | `string` | Network address of a node: `"10.0.1.100:8100"`. |
| `node/<site>/status` | `string` | Node status: `alive`, `dead`, `decommissioning`. |
| `node/<site>/last_heartbeat` | `uint64` | Timestamp of last heartbeat from this node. |

### Example Config State

```
__version__              = 42
shard_count              = 3
shard/0/replicas         = ["s101:8100","s201:8101","s301:8102"]
shard/0/leader           = "s101"
shard/0/status           = "active"
shard/1/replicas         = ["s102:8100","s202:8101","s302:8102"]
shard/1/leader           = "s102"
shard/1/status           = "active"
shard/2/replicas         = ["s103:8100","s203:8101","s303:8102"]
shard/2/leader           = "s103"
shard/2/status           = "active"
sharding/policy          = {"type":"hash","func":"murmur3"}
node/s101/addr           = "10.0.1.100:8100"
node/s101/status         = "alive"
```

## Components

### 1. ConfigManager Class

A thin wrapper around the `ITable` interface on shard 0:

```cpp
class ConfigManager {
public:
    // Initialize — opens (or creates) the __mako_config__ table on shard 0
    static Status Open(DB* shard0_db, ConfigManager** mgr);

    // --- Read operations (any shard 0 replica can serve) ---

    // Get the full cluster topology
    Status GetClusterConfig(ClusterConfig* config);

    // Get replicas for a specific shard
    Status GetShardReplicas(uint32_t shard_id, vector<string>* replicas);

    // Get current leader for a shard
    Status GetShardLeader(uint32_t shard_id, string* leader_site);

    // Get config version (for cache invalidation)
    Status GetVersion(uint64_t* version);

    // Get shard count
    Status GetShardCount(uint32_t* count);

    // Get sharding policy
    Status GetShardingPolicy(ShardingPolicy* policy);

    // --- Write operations (must go to shard 0 leader) ---

    // Add a new shard to the cluster
    Status AddShard(uint32_t shard_id, const vector<string>& replicas);

    // Remove a shard from the cluster
    Status RemoveShard(uint32_t shard_id);

    // Update replicas for a shard (add/remove replica)
    Status SetShardReplicas(uint32_t shard_id, const vector<string>& replicas);

    // Update leader for a shard (called on leader change)
    Status SetShardLeader(uint32_t shard_id, const string& leader_site);

    // Register a node
    Status RegisterNode(const string& site_name, const string& address);

    // Mark a node as dead
    Status DecommissionNode(const string& site_name);

    // Update sharding policy
    Status SetShardingPolicy(const ShardingPolicy& policy);

private:
    DB* db_;           // Shard 0 database instance
    ITable* table_;    // The __mako_config__ table

    // Increment version on every write
    Status IncrementVersion();
};
```

### 2. ClusterConfig (In-Memory Cache)

Every node caches the cluster config locally. The cache is refreshed when the version changes.

```cpp
struct ShardInfo {
    uint32_t shard_id;
    vector<string> replicas;  // Ordered: first = preferred leader
    string current_leader;
    string status;            // "active", "draining", etc.
    string range_start;       // For range sharding
    string range_end;
};

struct NodeInfo {
    string site_name;
    string address;           // "ip:port"
    string status;            // "alive", "dead", etc.
};

struct ClusterConfig {
    uint64_t version;
    uint32_t shard_count;
    ShardingPolicy sharding_policy;
    map<uint32_t, ShardInfo> shards;    // shard_id -> info
    map<string, NodeInfo> nodes;        // site_name -> info

    // Routing helper
    uint32_t GetShardForKey(const string& key) const;

    // Serialize/deserialize for RPC transfer
    void ToMarshal(Marshal& m) const;
    void FromMarshal(Marshal& m);
};
```

### 3. ConfigWatcher

A background fiber on each non-master shard that periodically polls shard 0 for config changes:

```cpp
class ConfigWatcher {
public:
    ConfigWatcher(const vector<string>& shard0_replicas,
                  function<void(const ClusterConfig&)> on_change);

    // Start watching in a background fiber
    void Start();
    void Stop();

    // Get cached config (lock-free read via atomic version)
    const ClusterConfig& GetCachedConfig() const;

    // Force an immediate refresh
    Status Refresh();

private:
    vector<string> shard0_replicas_;
    ClusterConfig cached_config_;
    atomic<uint64_t> cached_version_{0};
    function<void(const ClusterConfig&)> on_change_cb_;

    // Poll interval (default: 1 second)
    uint64_t poll_interval_us_ = 1000000;

    // Watch loop (runs as fiber)
    void WatchLoop();
};
```

## Bootstrap Protocol

### First Boot (No Existing Config)

1. Shard 0 leader starts, detects no `__mako_config__` table.
2. Reads the static YAML config file.
3. Creates `__mako_config__` table and populates it from YAML.
4. Sets `__version__ = 1`.
5. Other shards connect to shard 0 replicas, call `GetClusterConfig()`.
6. Each shard caches the config locally and begins normal operation.

### Subsequent Boots (Config Exists)

1. Shard 0 leader starts, finds existing `__mako_config__` table in Masstree (recovered from Raft log + snapshot).
2. Config is immediately available from the replicated state — the original YAML is not re-read.
3. Other shards connect to shard 0 and fetch latest config.

### Bootstrap Sequence Diagram

```
Shard 0 Leader                  Shard 1                    Shard 2
     |                              |                          |
     |--- Start, recover state ---->|                          |
     |    (Masstree + Raft log)     |                          |
     |                              |                          |
     |    If no config: load YAML   |                          |
     |    Populate __mako_config__  |                          |
     |                              |                          |
     |<--- Connect to shard 0 -----|                          |
     |<--- GetClusterConfig() -----|                          |
     |---- Return ClusterConfig -->|                          |
     |                              |--- Start with config -->|
     |                              |                          |
     |<--- Connect to shard 0 ------------------------------ |
     |<--- GetClusterConfig() --------------------------------|
     |---- Return ClusterConfig ------------------------------>|
     |                              |                          |
     |    Normal operation          |    Normal operation       |
```

### Shard 0 Discovery

Nodes need to know how to find shard 0 in the first place. Options:

1. **Static seed list** (recommended for v1): Command-line arg `--master-addrs=s101:8100,s201:8101,s301:8102` provides the addresses of shard 0's replicas. The node tries each in order until one responds.
2. **DNS-based** (future): A DNS SRV record `_mako-master._tcp.cluster.local` resolves to shard 0 replicas.
3. **Environment variable**: `MAKO_MASTER_ADDRS=s101:8100,s201:8101,s301:8102`.

## Config Change Protocol

All config changes go through shard 0 as regular transactions:

```
Admin/Node                    Shard 0 Leader              Shard 0 Followers
    |                              |                            |
    |--- AddShard(3, replicas) --->|                            |
    |                              |--- BeginTransaction() ---->|
    |                              |--- Put("shard/3/...") ---->|
    |                              |--- Put("shard_count", 4) ->|
    |                              |--- IncrementVersion() ---->|
    |                              |--- Commit() -------------->|
    |                              |    (Raft replicates)       |
    |                              |--------------------------->|
    |<--- OK ----------------------|                            |
    |                              |                            |
    |                         ConfigWatcher on other shards     |
    |                         polls version, detects change,    |
    |                         fetches new ClusterConfig         |
```

### Consistency Guarantees

- **Config reads**: Linearizable (served by shard 0 leader) or stale-ok (from any replica, with version check).
- **Config writes**: Serializable (regular Mako transactions on shard 0).
- **Version monotonicity**: `__version__` is incremented atomically with every config change. Watchers only apply configs with higher versions.

## Config Change Operations

### AddShard

```
1. Admin calls ConfigManager::AddShard(shard_id, replicas)
2. ConfigManager begins transaction on shard 0
3. Writes: shard/<id>/replicas, shard/<id>/status="adding", shard_count++, __version__++
4. Commits transaction (replicated via Raft)
5. New shard nodes bootstrap from shard 0, begin accepting data
6. Once caught up, admin calls SetShardStatus(shard_id, "active")
```

### RemoveShard

```
1. Admin calls ConfigManager::RemoveShard(shard_id)
2. ConfigManager sets shard/<id>/status="draining"
3. Shard router stops sending new requests to this shard
4. Existing data migrated to other shards (if range-based) or dropped (if hash-based with resharding)
5. Once drained, ConfigManager deletes shard/<id>/* keys, decrements shard_count
```

### AddReplica / RemoveReplica

```
1. Admin calls ConfigManager::SetShardReplicas(shard_id, new_replicas)
2. Writes new replica list to shard/<id>/replicas
3. Increments __version__
4. ConfigWatcher on the target shard detects change
5. Raft membership change initiated on the target shard (see Raft TODO)
```

## Relation to Existing Config Infrastructure

The existing `ConfigStore`/`ConfigService` in `src/deptran/` was a prototype using a standalone RocksDB instance. The master shard design supersedes it:

| Aspect | Old (ConfigStore) | New (Master Shard) |
|--------|------------------|-------------------|
| Storage | Standalone RocksDB | Masstree on shard 0 |
| Replication | None | Raft/Paxos (same as data) |
| Consistency | Single-node | Linearizable (replicated) |
| API | Custom RPC | Standard ITable Put/Get/Delete |
| Bootstrap | Separate path | Same as data recovery |
| Versioning | Manual | Automatic via Raft log index |

The existing `config_schema.h`, `config_converter.h`, and `ClusterConfig` structures can be reused — they just need to be wired to shard 0's `ITable` instead of the standalone `ConfigStore`.

## Implementation Plan

See `docs/TODO-raft.md` and the Mako TODO for task breakdown. Key phases:

1. **Phase 1**: Create `__mako_config__` table on shard 0, implement `ConfigManager` read/write methods.
2. **Phase 2**: Implement `ConfigWatcher` polling on non-master shards.
3. **Phase 3**: Implement bootstrap protocol (first boot from YAML, subsequent from shard 0).
4. **Phase 4**: Wire shard router to use dynamic config instead of static YAML.
5. **Phase 5**: Implement AddShard/RemoveShard operations and admin CLI.
