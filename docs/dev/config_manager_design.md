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
| `reshard/active` | `bool` | Whether a resharding operation is in progress. |
| `reshard/type` | `string` | Resharding type: `add`, `remove`, `split`. |
| `reshard/source_shard` | `uint32` | Source shard ID (for split/remove). |
| `reshard/target_shard` | `uint32` | Target/new shard ID. |
| `reshard/phase` | `string` | Current phase: `prepare`, `migrating`, `committed`. |
| `reshard/split_key` | `string` | Split point key (for range split). |

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

    // Get the shard responsible for a given key
    Status GetShardForKey(const string& key, uint32_t* shard_id);

    // Get the key range for a shard (range-based sharding)
    Status GetShardRange(uint32_t shard_id, string* range_start, string* range_end);

    // --- Write operations (must go to shard 0 leader) ---

    // Add a new shard (2PC-style: PREPARE -> migrate -> COMMIT)
    Status PrepareAddShard(uint32_t shard_id, const vector<string>& replicas,
                           const string& range_start, const string& range_end);
    Status CommitAddShard(uint32_t shard_id);
    Status AbortAddShard(uint32_t shard_id);

    // Remove a shard (2PC-style: PREPARE -> drain -> COMMIT)
    Status PrepareRemoveShard(uint32_t shard_id, uint32_t target_shard_id);
    Status CommitRemoveShard(uint32_t shard_id);
    Status AbortRemoveShard(uint32_t shard_id);

    // Split a shard's key range at a given key
    Status PrepareSplitShard(uint32_t source_shard_id, uint32_t new_shard_id,
                             const string& split_key, const vector<string>& new_replicas);
    Status CommitSplitShard(uint32_t source_shard_id, uint32_t new_shard_id);

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

## Data Partitioning

The Configuration Manager is the authority on how the key space is divided among shards. It stores the **shard map** — the mapping from key ranges to shard IDs — and all nodes consult this map for routing.

### Partitioning Strategies

Mako supports two partitioning strategies, stored in `sharding/policy`:

**Hash-based partitioning** (default):
```
shard_id = hash(key) % shard_count
```
- Even distribution regardless of key patterns.
- No range queries across shards (each key is independent).
- Resharding requires moving data when shard count changes.

**Range-based partitioning**:
```
shard_id = lookup(key, range_map)
```
- Each shard owns a contiguous key range: `[range_start, range_end)`.
- Supports efficient range scans within a shard.
- Can develop hot spots if key distribution is skewed.
- Resharding is a range split/merge — no full data shuffle.

### Shard Map (Range-Based)

For range-based sharding, the shard map is stored as per-shard range boundaries:

| Key | Value | Example |
|-----|-------|---------|
| `shard/<id>/range_start` | Lower bound (inclusive) | `""` (empty = min key) |
| `shard/<id>/range_end` | Upper bound (exclusive) | `"m"` |

Example 3-shard range map:
```
shard/0/range_start = ""     shard/0/range_end = "h"     (keys "" to "g...")
shard/1/range_start = "h"    shard/1/range_end = "p"     (keys "h" to "o...")
shard/2/range_start = "p"    shard/2/range_end = ""      (keys "p" to max, "" = unbounded)
```

**Invariants enforced by ConfigManager:**
1. Ranges are contiguous — no gaps between shards.
2. Ranges are non-overlapping — every key maps to exactly one shard.
3. The union of all ranges covers the entire key space.

### Shard Map (Hash-Based)

For hash-based sharding, no per-shard ranges are stored. The shard map is implicit:
```
shard_id = murmur3(key) % shard_count
```
The `shard_count` and `sharding/policy` keys fully determine the mapping.

### Key Routing

Every node caches the shard map in `ClusterConfig`. The routing function:

```cpp
uint32_t ClusterConfig::GetShardForKey(const string& key) const {
    if (sharding_policy.type == "hash") {
        return murmur3_hash(key) % shard_count;
    } else {  // range
        for (auto& [id, info] : shards) {
            if (key >= info.range_start &&
                (info.range_end.empty() || key < info.range_end)) {
                return id;
            }
        }
    }
}
```

Clients and coordinators call this before every transaction to determine which shard(s) to contact.

---

## Resharding Protocol (2PC-Style)

Resharding — adding, removing, or splitting shards — is a distributed operation that must be coordinated carefully to avoid data loss or inconsistency. Mako uses a **two-phase commit (2PC) style** protocol where the Configuration Manager on shard 0 acts as the coordinator.

### Overview

The resharding protocol has three stages, each recorded as a config change on shard 0:

```
PREPARE  -->  COMMIT  -->  CLEANUP
```

Shard 0 drives the protocol. Each stage is a transaction on shard 0, so the resharding state is durable and recoverable after crashes.

### AddShard (Shard Split)

Adds a new shard by splitting a key range from an existing shard.

```
                Shard 0               Source Shard        New Shard
                (coordinator)         (donor)             (receiver)
                    |                      |                   |
  PREPARE:          |                      |                   |
    1. Write:       |                      |                   |
       shard/<new>/status = "adding"       |                   |
       shard/<new>/replicas = [...]        |                   |
       shard/<new>/range_start = split_key |                   |
       shard/<new>/range_end = old_end     |                   |
       shard/<src>/range_end = split_key   |  (shrink source)  |
       __version__++                       |                   |
                    |                      |                   |
    2. Notify:      |--- PrepareAddShard ->|                   |
       Source shard |   (stop accepting    |                   |
       enters       |    keys >= split_key)|                   |
       dual-write   |                      |                   |
       mode         |--- StartShard -------|------------------>|
                    |                      |   (begin accepting|
                    |                      |    keys in range) |
                    |                      |                   |
    3. Migrate:     |                      |--- stream keys -->|
       Source sends  |                      |   >= split_key    |
       existing data|                      |                   |
       to new shard |                      |                   |
                    |                      |                   |
  COMMIT:           |                      |                   |
    4. Verify:      |<-- MigrationDone ----|                   |
                    |<-- Ready ------------|-------------------|
    5. Write:       |                      |                   |
       shard/<new>/status = "active"       |                   |
       __version__++                       |                   |
                    |                      |                   |
    6. Notify:      |--- CommitAddShard -->|                   |
       Source stops  |   (stop dual-write, |                   |
       dual-write   |    reject keys       |                   |
                    |    >= split_key)     |                   |
                    |                      |                   |
  CLEANUP:          |                      |                   |
    7. Router       |  ConfigWatchers pick up new version      |
       updates      |  All nodes route to new shard            |
```

**Key safety properties:**
- During PREPARE, the source shard enters **dual-write mode**: it accepts writes in the migrating range and forwards them to the new shard. This ensures no writes are lost during migration.
- The new shard only becomes `active` after migration is verified complete.
- If the coordinator (shard 0) crashes during PREPARE, on recovery it sees `status="adding"` and can resume or abort.
- If the coordinator crashes during COMMIT, on recovery it sees the committed state and proceeds to CLEANUP.

### RemoveShard (Shard Merge / Drain)

Removes a shard by draining its data to another shard.

```
                Shard 0               Draining Shard      Target Shard
                (coordinator)         (donor)             (receiver)
                    |                      |                   |
  PREPARE:          |                      |                   |
    1. Write:       |                      |                   |
       shard/<drain>/status = "draining"   |                   |
       __version__++                       |                   |
                    |                      |                   |
    2. Notify:      |--- PrepareDrain ---->|                   |
       Draining     |   (reject new writes,|                   |
       shard stops  |    serve reads only) |                   |
       new writes   |                      |                   |
       Router stops |--- ExpandRange ------|------------------>|
       sending new  |   (target accepts    |                   |
       requests     |    draining range)   |                   |
                    |                      |                   |
    3. Migrate:     |                      |--- stream all --->|
       Drain all    |                      |   remaining keys  |
       data to      |                      |                   |
       target       |                      |                   |
                    |                      |                   |
  COMMIT:           |                      |                   |
    4. Verify:      |<-- DrainComplete ----|                   |
                    |<-- Ready ------------|-------------------|
    5. Write:       |                      |                   |
       shard/<drain>/status = "removed"    |                   |
       shard/<target>/range adjusted       |                   |
       shard_count--                       |                   |
       __version__++                       |                   |
                    |                      |                   |
  CLEANUP:          |                      |                   |
    6. Shutdown:    |--- Shutdown -------->|                   |
       Draining     |  (process exits)     |                   |
       shard stops  |                      |                   |
    7. Delete:      |                      |                   |
       Remove shard/<drain>/* keys         |                   |
```

**Key safety properties:**
- The draining shard stops accepting new writes immediately in PREPARE. Reads continue until shutdown.
- The target shard expands its range to cover the draining shard's keys before migration starts.
- Data migration streams all keys from the draining shard to the target.
- The shard is only removed from the config after migration is verified complete.

### Resharding for Hash-Based Partitioning

Hash-based resharding (changing `shard_count`) requires a full data shuffle since every key's shard assignment changes. The protocol:

```
PREPARE:
  1. Write new shard_count to shard/pending_shard_count.
  2. All shards enter dual-routing mode: accept writes for both old and new hash assignments.
  3. Migrate: each shard scans its data, sends keys whose new hash maps to a different shard.

COMMIT:
  4. All migrations verified complete.
  5. Write shard_count = pending_shard_count, delete pending_shard_count.
  6. All shards switch to new hash routing.

CLEANUP:
  7. Each shard deletes keys that no longer belong to it.
```

This is expensive (full data shuffle) and should be avoided if possible. Range-based sharding is preferred for workloads that may need resharding.

### Crash Recovery During Resharding

Since resharding state is stored as config entries on shard 0, the protocol is recoverable:

| Crash Point | Shard 0 State on Recovery | Action |
|-------------|--------------------------|--------|
| During PREPARE | `status = "adding"` or `"draining"` | Resume migration or abort (set status back to previous) |
| During COMMIT | `status = "active"` or `"removed"` | Proceed to CLEANUP |
| During CLEANUP | Config committed, nodes updating | Nodes re-fetch config and converge |

The coordinator (shard 0 leader) checks for in-progress resharding operations on startup and resumes or aborts them.

---

## Config Change Operations

### AddReplica / RemoveReplica

```
1. Admin calls ConfigManager::SetShardReplicas(shard_id, new_replicas)
2. Writes new replica list to shard/<id>/replicas
3. Increments __version__
4. ConfigWatcher on the target shard detects change
5. Raft membership change initiated on the target shard (see Raft TODO)
```

## Speculation Recovery and Epoch Management

A critical responsibility of the Configuration Manager is orchestrating **epoch-based speculation recovery** when a shard leader fails. This is the protocol described in Section 5.2 of the Mako paper (OSDI'25).

### The Problem

Mako speculatively executes transactions before replication completes. When a shard leader fails, some speculative transactions may be lost (not yet replicated). The system must:
1. Detect the failure and elect a new leader for the failed shard.
2. Determine which transactions were replicated and which were lost.
3. Close the old epoch so that healthy shards can finalize their watermarks.
4. Prevent unbounded cascading aborts of dependent transactions.

### Epochs

Mako groups Paxos log entries into **epochs**. An epoch is a period during which a particular leader is active. The CM maintains the current epoch number for each shard.

- Each shard's Paxos streams use the epoch number to group log entries.
- When a leader fails, the CM **advances the epoch** and broadcasts the increment to all shards.
- Healthy shards also advance their epoch on receiving the CM broadcast — this is how the entire cluster learns about the failure, even shards that weren't directly affected.

### Recovery Protocol

When the CM detects a shard leader failure:

```
CM                          Failed Shard            Healthy Shards
 |                          (new leader)                |
 |  1. Detect failure           |                       |
 |  2. Trigger leader election  |                       |
 |  3. Advance epoch number     |                       |
 |                              |                       |
 |--- Broadcast new epoch ----->|                       |
 |--- Broadcast new epoch --------------------------->  |
 |                              |                       |
 |                   4. Close old epoch:                 |
 |                      - Retrieve replicated entries    |
 |                        from peers                     |
 |                      - Re-commit recovered entries    |
 |                      - No-op unrecoverable entries    |
 |                      - Replicate INF shard clock      |
 |                        (signals epoch closure)        |
 |                              |                       |
 |                              |          5. Healthy shards:
 |                              |             - Finish speculative
 |                              |               execution for old epoch
 |                              |             - Replicate INF entries
 |                              |             - Declare old epoch closed
 |                              |                       |
 |                   6. Compute finalized shard watermark|
 |                      (min shard clock across streams) |
 |                              |                       |
 |                   7. Exchange finalized watermarks     |
 |                      to form Finalized Vector         |
 |                      Watermark (FVW)                  |
 |                              |                       |
 |                   8. Rollback: any speculative txns    |
 |                      below FVW that aren't replicated |
 |                      are rolled back; txns above FVW  |
 |                      depend on lost txns and are also |
 |                      rolled back transitively         |
```

### Step-by-Step Details

**Step 1-3: Failure detection and epoch advance.** The CM detects the failure (via heartbeat timeout or Raft/Paxos leader election), triggers a new leader election for the failed shard, and advances the epoch number. The CM broadcasts the new epoch to all shards.

**Step 4: Close old epoch on failed shard.** The newly elected leader retrieves all replicated log entries from the previous epoch from its peers (the surviving replicas). It re-commits any recovered entries and issues **no-ops** for any entries that are not recoverable (entries after the no-op will be ignored). This "closes" the old epoch — no more entries can be added to it. The ending entry has an **INF shard clock value**, indicating: (1) this is the maximum clock, (2) there are no more transactions in this epoch on this shard, and (3) all previous transactions have been successfully replicated without lost dependencies. When INF is replicated, the leader considers the old epoch closed and declares its finalized shard watermark as INF.

**Step 5: Close old epoch on healthy shards.** Healthy shards finish speculative execution and certification of all transactions in the old epoch, then replicate a special **INF ending entry** into all Paxos streams. When the INF entry is replicated, the healthy shard's old epoch is closed.

**Step 6: Compute finalized shard watermark.** Once the old epoch is closed on a shard, its **finalized shard watermark** can be computed by choosing the minimum shard clock of all its streams. Note that INF is the minimum shard clock of all streams, so the finalized shard watermark is the same for both recovered and healthy shards.

**Step 7: Exchange finalized watermarks.** Each shard computes its finalized watermark, then broadcasts it to all other shards. All shards exchange their finalized watermarks to form the **Finalized Vector Watermark (FVW)** — a consistent global cutoff across all shards.

**Step 8: Rollback.** For any transactions on healthy shards that are below the FVW but were not replicated (marked as speculative), they are **rolled back** because they could have depended transitively or directly on a lost transaction from the failed shard. After rollback, all shards advance to the new epoch and resume normal operation.

### Key Properties

- **No unbounded cascading aborts**: The FVW provides a clean cutoff. Only transactions in the old epoch that depended on lost transactions are rolled back — not all speculative transactions.
- **Healthy shards are minimally affected**: They simply close the old epoch, replicate INF, and roll back affected transactions. Transactions on healthy shards that don't depend on the failed shard are unaffected.
- **CM is replicated**: The CM itself is replicated (it is shard 0), so it is considered always available.
- **Epoch-based grouping**: Using epochs to group log entries is a classic consensus approach — the CM is not introducing new algorithmic complexity in each consensus instance, just coordinating the epoch transitions.

### CM's Responsibilities for Recovery

| Responsibility | Config Key Used |
|----------------|----------------|
| Detect shard leader failure | `node/<site>/status`, heartbeat monitoring |
| Trigger leader election | `shard/<id>/replicas` (candidate list) |
| Record new leader | `shard/<id>/leader` |
| Advance epoch | `shard/<id>/epoch` (new key) |
| Broadcast epoch to all shards | ConfigWatcher poll detects `__version__` change |
| Track epoch closure status | `shard/<id>/epoch_status` (`open`, `closing`, `closed`) |

### Additional Config Keys for Epoch Management

| Key Pattern | Value Type | Description |
|-------------|-----------|-------------|
| `shard/<id>/epoch` | `uint64` | Current epoch number for this shard |
| `shard/<id>/epoch_status` | `string` | `open`, `closing`, `closed` |
| `shard/<id>/prev_leader` | `string` | Previous leader (for recovery reference) |

---

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
4. **Phase 4**: Wire shard router to use dynamic config and `GetShardForKey()` instead of static YAML.
5. **Phase 5**: Implement range-based shard map management (range_start/range_end, invariant enforcement).
6. **Phase 6**: Implement 2PC-style resharding protocol (PrepareAddShard/CommitAddShard, PrepareSplitShard/CommitSplitShard, PrepareRemoveShard/CommitRemoveShard).
7. **Phase 7**: Implement data migration streaming between shards (dual-write mode, key streaming, verification).
8. **Phase 8**: Implement crash recovery for in-progress resharding operations.
9. **Phase 9**: Admin CLI for resharding operations and cluster management.
