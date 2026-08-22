# The Mako Book

A comprehensive developer guide for the Mako distributed transactional datastore.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture Overview](#2-architecture-overview)
3. [Configuration Manager (Master Shard)](#3-configuration-manager-master-shard)
4. [Core Protocol: Speculative Two-Phase Commit](#4-core-protocol-speculative-two-phase-commit)
5. [Replication and Consensus](#5-replication-and-consensus)
6. [Storage Engines](#6-storage-engines)
7. [Networking and RPC](#7-networking-and-rpc)
8. [Concurrency Model: Fibers and the Reactor Pattern](#8-concurrency-model-fibers-and-the-reactor-pattern)
9. [Configuration Reference](#9-configuration-reference)
10. [Build System](#10-build-system)
11. [Testing](#11-testing)
12. [Memory Safety: RustyCpp](#12-memory-safety-rustycpp)
13. [Performance Optimization Techniques](#13-performance-optimization-techniques)
14. [Design Principles](#14-design-principles)
15. [Anti-Patterns to Avoid](#15-anti-patterns-to-avoid)
16. [Troubleshooting](#16-troubleshooting)
17. [Glossary](#17-glossary)

---

## 1. Introduction

**Mako** is a high-performance distributed transactional key-value store with geo-replication support. Published at OSDI'25, it introduces a novel **speculative two-phase commit (S2PC) protocol** that decouples transaction execution from replication, achieving unprecedented performance while maintaining ACID guarantees.

### Key Performance Numbers

- **3.66M TPC-C transactions/second** with 10 geo-replicated shards
- **8.6x higher throughput** than state-of-the-art geo-replicated systems
- **960K TPS** on a single shard (22.5x faster than Calvin)
- **Median latency of 121ms** for geo-replicated transactions
- **~2ms client-visible latency** for speculative commits (vs ~150ms traditional)

### Research Background

**Paper**: *Mako: Speculative Distributed Transactions with Geo-Replication* (OSDI'25)

**Authors**: Weihai Shen, Yang Cui, Siddhartha Sen, Sebastian Angel, Shuai Mu

Mako descends from the **Janus** codebase associated with the OSDI'16 paper *Consolidating Concurrency Control and Consensus for Commits under Conflicts*. The standalone Janus protocol implementation has since been retired.

### When to Use Mako

**Ideal for:**
- Key-value workloads requiring ACID transactions
- Geo-replicated deployments needing low latency (<10ms)
- High-throughput scenarios (100K+ TPS)
- Data that fits in memory (<1TB per shard)

**Not recommended for:**
- Complex SQL queries, joins, or aggregations
- Datasets that exceed available RAM
- High-contention workloads (e.g., global counters)
- Eventual consistency is sufficient (simpler systems may be better)

### System Requirements

| | Minimum | Recommended |
|---|---------|-------------|
| **OS** | Debian 12 / Ubuntu 22.04 | Debian 12 / Ubuntu 22.04 |
| **CPU** | 4 cores | 16+ cores (24 optimal) |
| **RAM** | 8 GB | 64+ GB |
| **Disk** | 20 GB | SSD, 100+ GB |
| **Network** | 1 Gbps | 10 Gbps or InfiniBand/RDMA |

---

## 2. Architecture Overview

Mako has a layered architecture:

```
+--------------------------------------------------------------+
|                    Client Applications                        |
|           (Native C++ API / Benchmark Harness)                |
+------------------------------+-------------------------------+
                               |
+------------------------------v-------------------------------+
|                 Transaction Coordinators                       |
|  +----------+----------+--------------+-----------+          |
|  | Mako 2PC | Paxos Mgr| Dependency   | Watermark |          |
|  | Protocol |          | Tracker      | Manager   |          |
|  +----------+----------+--------------+-----------+          |
+------------------------------+-------------------------------+
                               |
+------------------------------v-------------------------------+
|           Replication Layer (Pluggable)                        |
|         +----------------+----------------+                   |
|         |    Paxos        |     Raft       |                   |
|         +----------------+----------------+                   |
+------------------------------+-------------------------------+
                               |
+------------------------------v-------------------------------+
|            RPC Communication Layer (RRR)                       |
|  +----------+----------+----------+----------+               |
|  | TCP/IP   | Fibers    | Reactor  | Event    |               |
|  | Sockets  |           | Pattern  | Loop     |               |
|  +----------+----------+----------+----------+               |
|         (Optional: DPDK / RDMA)                               |
+------------------------------+-------------------------------+
                               |
+------------------------------v-------------------------------+
|              Sharded Data Partitions                           |
|  +------------------+------------------+------------------+  |
|  |    Shard 0       |     Shard 1      |     Shard N      |  |
|  |  Leader + N      |  Leader + N      |  Leader + N      |  |
|  |  Followers       |  Followers       |  Followers       |  |
|  +------------------+------------------+------------------+  |
+------------------------------+-------------------------------+
                               |
+------------------------------v-------------------------------+
|                   Storage Engines                              |
|  +-------------------------+------------------------------+  |
|  |  Masstree (In-Memory)   |    RocksDB (Persistent)      |  |
|  |  - Concurrent B+tree    |    - LSM-tree on disk         |  |
|  |  - Lock-free reads      |    - Write-ahead log (WAL)    |  |
|  |  - Cache-friendly       |    - Async writes             |  |
|  +-------------------------+------------------------------+  |
+--------------------------------------------------------------+
```

### Key Components

**Transaction Coordinator** (`src/deptran/`, `src/mako/`):
- Orchestrates distributed transactions across shards
- Manages speculative execution and dependency tracking
- Key classes: `Coordinator`, `Transaction`

**Shard Server**:
- Stores and retrieves key-value pairs via Masstree
- Participates in consensus (Paxos or Raft)
- Key classes: `Scheduler`, `TxnRegistry`, `MultiPaxos`

**RRR Communication Layer** (`src/rrr/`):
- Custom RPC framework with asynchronous, fiber-based I/O
- Reactor pattern for event-driven networking
- Supports TCP/IP, DPDK, RDMA

**Storage Engines** (`src/mako/masstree/`):
- Masstree: in-memory concurrent B+tree for primary storage
- RocksDB: optional background persistence for durability

### Directory Structure

```
mako/
  src/
    deptran/          # Transaction protocol implementations
      paxos/          # Paxos consensus
      raft/           # Raft consensus
      occ/            # Optimistic concurrency control
      rcc/            # Rococo protocol
    mako/             # Mako core
      masstree/       # Masstree storage engine
      lib/            # Transport backends, configuration
      benchmarks/     # Benchmark harness (TPC-C, TPC-A, RW)
    rrr/              # RPC framework and fibers
    bench/            # Benchmark workload implementations
    memdb/            # In-memory datastore
  config/             # YAML configuration files
  ci/                 # CI test scripts
  examples/           # Example scripts and tests
  tests/              # Unit and integration tests
  third-party/        # Dependencies (rusty-cpp, yaml-cpp, googletest, etc.)
  rust-lib/           # Rust components
```

---

## 3. Configuration Manager (Master Shard)

Mako uses **shard 0** as a dedicated **master shard** that stores system-wide configuration — cluster membership, shard topology, routing metadata — as regular replicated key-value entries. Config changes are ACID transactions with the same durability guarantees as application data.

### Why a Master Shard?

YAML config files define the initial cluster topology at first boot. Once the cluster is running, shard 0 becomes the **primary source of truth** for configuration. This separation allows:
- The initial YAML to bootstrap the cluster (both development and production).
- Runtime config changes (adding shards, updating replicas) without editing files on every node and restarting.
- A single, replicated source of truth that cannot drift between nodes.

By storing config in shard 0 (replicated via Raft), config changes are:
- **Replicated** automatically — no separate replication path.
- **Transactional** — atomic updates to membership and topology.
- **Discoverable** — other shards bootstrap by reading from shard 0.
- **Versioned** — automatic via Raft log index, enabling cache invalidation.

### Architecture

```
              +------------------------------------------+
              |        Shard 0 (Master Shard)             |
              |                                          |
              |  Config Table: "__mako_config__"         |
              |  +--------------------+------------+    |
              |  | Key                | Value      |    |
              |  +--------------------+------------+    |
              |  | __version__        | 42         |    |
              |  | shard_count        | 3          |    |
              |  | shard/0/replicas   | [s1,s2,s3] |    |
              |  | shard/1/replicas   | [s4,s5,s6] |    |
              |  | shard/1/leader     | "s4"       |    |
              |  | epoch              | 7          |    |
              |  | sharding/policy    | {"hash"}   |    |
              |  | node/s1/addr       | 10.0.1.1   |    |
              |  +--------------------+------------+    |
              |                                          |
              |  Replicated via Raft (3+ replicas)       |
              +------------------------------------------+
                    |              |              |
              Shard 1         Shard 2        Clients
              (bootstrap      (bootstrap     (discover
               from shard 0)   from shard 0)  topology)
```

### Config Table Schema

All configuration lives in a reserved table `__mako_config__` on shard 0, accessed via the standard `ITable::Put/Get/Delete` API.

| Key Pattern | Value | Description |
|-------------|-------|-------------|
| `__version__` | uint64 | Monotonically increasing config version |
| `shard_count` | uint32 | Number of live shards |
| `next_shard_id` | uint32 | Monotonic id allocator — the master hands out `next_shard_id` on `register_shard` and increments it; ids are never reused |
| `bucket_count` | uint32 | Fixed hash-slot count for the consistent-hashing strategy — set once at cluster creation, never changes |
| `shard/<id>/replicas` | JSON array | Ordered replica list (first = preferred leader) |
| `shard/<id>/leader` | string | Current leader site name |
| `shard/<id>/status` | string | `active`, `draining`, `adding`, `removing` |
| `epoch` | uint64 | Global speculative epoch number (all shards converge to this) |
| `shard/<id>/replacement` | uint32 | For `status=dead`: taker shard whose Raft group inherits routing that would hash to this shard. Chased transitively with a cycle guard. |
| `sharding/mode` | string | `hash` (default) or `range` — the routing mode Path A of `get_shard_for_key` uses when no per-table policy is registered |
| `sharding/policy/<table>` | bytes | Serialized `TableShardingPolicy` for one table — its `KeyExtractor` plus a sorted vector of `RangeMapping`s. Absence means "fall back to `sharding/mode`". |
| `node/<site>/addr` | string | Node address (`ip:port`) |
| `node/<site>/status` | string | `alive`, `dead`, `decommissioning` |
| `reshard/active` | bool | Whether resharding is in progress |
| `reshard/phase` | string | `prepare`, `migrating`, `committed` |

### Key Components

**ConfigManager**: Typed configuration over a `KvStore` port (`get`/`put`/`remove`), not a bespoke store. Provides methods like `get_shard_replicas()`, `add_shard()`, `set_shard_leader()`, `advance_epoch()`. Every write increments `__version__` (written last). On shard 0's leader the port binds to the unified `FullOrderedIndex` — the `__mako_config__` system table — via the `OrderedIndexKvStore` adapter; other nodes bind a `RemoteKvStore` that reads shard 0 over RPC; unit tests bind an in-memory fake. The port keeps `cluster/` standalone-testable with no storage-engine dependency (see [The Storage Interface](storage-interface.md#cluster-metadata-port-srcclusterkv_storeh)).

**ClusterConfig**: In-memory cache of the full cluster topology. Every node holds a local copy. Includes a `get_shard_for_key()` routing helper.

**ConfigWatcher**: Background fiber on non-master shards. Polls shard 0's `__version__` key periodically (default: 1 second). On version change, fetches the full `ClusterConfig` and invokes a callback to update local routing.

### Bootstrap Protocol

**First boot**: Shard 0 leader loads the static YAML config, populates `__mako_config__`, sets `__version__ = 1`. Other shards connect to shard 0 (via a static seed list: `--master-addrs=s1:8100,s2:8101,s3:8102`) and fetch the config.

**Subsequent boots**: Shard 0 recovers its Masstree from Raft log + snapshot. Config is immediately available from the replicated state — the original YAML is not re-read. Other shards fetch the latest version from shard 0.

**Runtime wiring** (`src/mako/cluster_bootstrap.cc`): `BootstrapClusterConfig(db)` runs once from `init_env()` after the RPC servers are up. It is the single seam that constructs and connects the read-side components on a live node, and is gated twice — no-op unless `MAKO_CLUSTER_CONFIG=1` **and** `nshards > 1` — so single-shard / unsharded runs keep the legacy routing path untouched. When active it branches on node identity (`BenchmarkConfig::getShardIndex/getLeaderConfig`):

- **Shard 0's leader** opens its `__mako_config__` index, wraps it in an `OrderedIndexKvStore`, seeds it from the cluster-wide topology (`Config::SitesByPartitionId` / `LeaderSiteByPartitionId`), and stands up a dedicated `ConfigKvService` RPC server on `leader_port + 20000` (distinct from the `+10000` heartbeat control delta).
- **Every other node** (shard-0 followers + all non-zero shards) resolves shard 0's leader from the same `Config`, connects there, and wraps the RPC in a `RemoteKvStore`.

Both sides then run a `ConfigWatcher` that keeps the local `janus::get_cluster_config()` routing cache fresh — shard 0's leader watches its local store (no self-RPC), everyone else polls shard 0. Because server bind and client connect derive the address from the same `Config`, they stay symmetric. Functional verification needs a live multi-shard cluster, e.g. `./docker_build.sh ci shard2Replication` with `MAKO_CLUSTER_CONFIG=1`.

### Config Change Protocol

All config changes are regular transactions on shard 0:

1. A joining shard calls `ConfigManager::register_shard(replicas)`; the master allocates its id from `next_shard_id` and returns it.
2. ConfigManager begins a transaction, writes `shard/<id>/replicas`, advances `next_shard_id`, increments `shard_count` and `__version__`.
3. Transaction commits (replicated via Raft).
4. ConfigWatcher on other shards detects the version bump, fetches the new config.
5. Shard router updates routing table.

### Data Partitioning

The Configuration Manager is the authority on how the key space is divided among shards. Two strategies are supported:

**Lex-order ranges** (default): each shard owns a contiguous slice of the raw key space in lexicographic order, `[start_key, end_key) → shard`. This is the natural partitioning for an ordered store (Masstree) — it keeps range scans shard-local. The range→shard assignment *is* the sharding policy (below); publishing a new assignment triggers a rebalance.

**Consistent hashing** (second strategy): for workloads that want even spread without range locality. Crucially this is **not** `hash(key) % shard_count` — dividing by the live shard count remaps the *entire* key space every time the cluster grows or shrinks (which is exactly why a naive `remove_shard` that decrements the count would reshuffle everything). Instead:

1. `bucket = hash(key) % bucket_count`, where `bucket_count` is **fixed once at cluster creation and never changes** (a fixed slot space, à la Redis Cluster's 16384 hash slots).
2. Each shard is placed on a hash ring at `hash(shard_id)` positions; a bucket is owned by the shard that succeeds it on the ring.

Because `key → bucket` is frozen, only the `bucket → shard` map moves on a membership change, and consistent hashing bounds that to ≈`bucket_count / N` buckets — never a full reshuffle. Adding or removing a shard migrates only its arc of the ring.

Every node caches the shard map via `ClusterConfig::get_shard_for_key()` for routing.

> **Current vs target.** The routing described here is the target model. The code today still takes the naive `hash(key) % shard_count` path when no per-table policy is registered (`cc_route`, `src/cluster/cluster_config.cc`), and range policies are keyed on an *extracted int64* (via `KeyExtractor`, below) rather than raw-key lex order. The fixed-`bucket_count` ring and the raw-key lex default are not yet built.

#### Key Extractors

For range-based sharding, the policy doesn't operate on the raw row key directly — it operates on an *extracted* sharding key. `KeyExtractor` (`src/cluster/sharding_policy.h:46`) supports three strategies:

- **`FIELD_INDEX`** — extract the *n*th field from a composite key. TPC-C uses this: `w_id` is field 0 of every table that's keyed off a warehouse.
- **`PREFIX_BYTES`** — read the first N bytes as a big-endian int64.
- **`HASH_MOD`** — hash the whole key, mod `num_shards`. Used as the default fallback.

A `TableShardingPolicy` holds a sorted `vector<RangeMapping>` (each `[start_key, end_key) → shard_id`) and looks up shards via binary search (`src/cluster/sharding_policy.h:152`). Policies are built at startup with a fluent builder:

```cpp
// From src/cluster/sharding_policy_builder.h
ShardingPolicyBuilder(num_shards)
    .table("WAREHOUSE").shardByField(0)
        .addRange(0, 5, 0).addRange(5, 10, 1);
```

The full schema (KeyExtractor types, RangeMapping serialization, TableShardingPolicy lookup) was designed in `docs/plans/range-sharding/task{1..4}.md`.

#### Where the sharding policy lives

The policy is **not** stored in a separate metadata service. It lives in the same `__mako_config__` system table on shard 0, under the `sharding/policy/<table>` key prefix — one key per table. Every shard and every client fetches the policy through **the standard KV interface** (`ITable::Get`), pointed at shard 0.

The one thing that has to be special-cased is *how you find shard 0 in the first place*. `__mako_config__` is **pinned to shard 0** by convention: any lookup against this table skips the normal hash routing and goes straight to shard 0's current leader (whose address the node knows from the initial YAML seed list). There is no chicken-and-egg problem because you know shard 0 before you know anything else.

Everything else flows from this:

- A client that wants to route key `k` in table `T` fetches `sharding/policy/T` from `__mako_config__` (which lives on shard 0), plus `__version__`, and caches both.
- If the client sees the same `__version__` on refresh, it keeps the cached policy.
- If `__version__` bumped, the client re-fetches every table policy it cares about.
- If the client has no cached policy for `T`, it uses the default `sharding/mode` (hash or range) plus `shard_count`.

Because the storage medium is just replicated KV, everything about the policy is already **versioned, transactional, and durable** — no separate config-service RPC, no separate replication path.

#### Cache invalidation: version bumps + wrong-shard errors

Two mechanisms keep every node's routing table converging on the current policy.

**Version-based polling (background, sub-second staleness).** Each node runs a `ConfigWatcher` fiber. It calls `ITable::Get(__mako_config__, __version__)` on shard 0's leader, default every 1 second. When the returned version differs from its cached value, the watcher refreshes the cluster topology and every registered per-table policy, then invokes update callbacks so anything that depends on routing (client shard picker, coordinator, gossip) can react.

**Wrong-shard errors (foreground, single-request recovery).** Any shard that receives an RPC for a key it doesn't own returns a `WrongShard` error containing the `__version__` it thinks is current. The caller compares that version to its cached one:

- Caller's version is behind: refresh immediately, retry the RPC against the shard the new policy resolves to.
- Caller's version is equal to or ahead of the shard's: means the caller is right about routing and the shard is stale. Wait briefly and retry — the shard's own `ConfigWatcher` will catch up in <1 second.

This is the "moved, retry" pattern from the resharding survey (`docs/reference/resharding-survey.md`), specifically the YugabyteDB flavor: the routing state is authoritative on shard 0, but every actor can independently detect its own staleness without needing a broadcast. Together with 1-second polling, a torn cluster reconverges in worst-case one RTT of the client's next request.

#### Master API: the shardmaster commands

Cluster-lifecycle changes are exposed as RPCs on **every shard**, not just shard 0, so a client (or an operator's admin CLI) can issue them on any node without knowing which shard is the master:

- **Non-shard-0 nodes** implement each RPC as a **forwarder**: they hold a client connection to shard 0's current leader and re-issue the RPC there. Return values propagate straight back.
- **Shard 0's leader** implements the RPC as the actual mutation — it invokes the corresponding `ConfigManager` verb, which writes the atomic Raft batch to `__mako_config__`. Every write bumps `__version__`, which every other node's `ConfigWatcher` picks up.

The command surface (✅ implemented today · 🟡 partial · ⬜ not yet built):

**Membership & lifecycle**

| Command | Effect | Status |
|---|---|---|
| `register_shard(replicas) -> id` | **An empty shard joins.** The shard starts with no id, calls this with its replica set, and the **master allocates its id** — `next_shard_id` (monotonic, never reused) — records the replica set + `status=active`, bumps `shard_count`, and returns the id. | ✅ |
| `kill_shard(dead, taker)` | **Brutal** failure handoff (the shard is usually dead): set `status=dead`, `replacement=taker`, clear replicas, advance epoch. The dead shard's range reroutes to `taker`; its data is recovered from replicas, not migrated. `shard_count` is left unchanged so only the dead shard's keys move. | ✅ |
| `remove_shard(id)` | **Gentle** decommission: drain the shard's data onto the other shards *first*, then delete it and shrink the cluster. | 🟡 metadata-only delete today; the drain is not yet wired |
| `drain_shard(id)` | Rebalance a shard's ranges/buckets onto the remaining shards without removing it. | ⬜ |
| `revive_shard(id, replicas)` | Promote a dead shard's replacement back to a first-class shard. | ⬜ |
| `split_shard(source, split_key, dest)` | Split `source`'s range at `split_key`; the upper half becomes `dest`. | ⬜ |
| `merge_shard(a, b)` | Merge two adjacent shards into one. | ⬜ |

**Placement & policy**

| Command | Effect | Status |
|---|---|---|
| `set_sharding_policy(table, assignment)` | Publish a table's `[range) → shard` (or `bucket → shard`) assignment. **Triggers a rebalance** — the affected partitions migrate to match — and bumps `__version__`. | 🟡 stores the policy today; the migration is not yet wired |
| `get_sharding_policy(table)` · `list_sharding_policy_tables()` · `delete_sharding_policy(table)` | Read / list / drop per-table policies. | ✅ |
| `set_sharding_mode(lex \| hash)` | Select the default strategy for tables with no policy. | ✅ |
| `move_range(table, range, from, to)` · `move_bucket(bucket, to)` | Move a single partition between shards — the migration primitive `remove_shard`/`rebalance` build on. | ⬜ |
| `rebalance()` | Even out ranges/buckets across shards after a membership change. | ⬜ |

**Replica set, leadership & epoch**

| Command | Effect | Status |
|---|---|---|
| `set_shard_replicas(id, replicas)` · `set_shard_leader(id, site)` · `set_shard_status(id, status)` | Manage a shard's replica set, Raft/Paxos leader, and lifecycle status. | ✅ |
| `advance_epoch()` | Bump the global speculative epoch (a speculation barrier; see below). | ✅ |
| `set_node_addr(site, addr)` · `set_node_status(site, status)` | Physical node registry. | ✅ |

**Read plane (queries)**

| Command | Effect | Status |
|---|---|---|
| `get_version()` | Current config version — clients poll it to invalidate their routing cache. | ✅ |
| `get_shard_count()` · `get_shard_replicas(id)` · `get_shard_leader(id)` · `get_shard_status(id)` · `get_shard_replacement(id)` | Per-shard topology reads. | ✅ |

Any command that mutates the shard map advances `__version__` in the same atomic batch — the single knob that drives cache invalidation across the cluster. `kill_shard` is the *brutal* verb (a shard died, reassign its range, don't move data); `remove_shard` is the *gentle* one (drain data out, then remove) — see [Resharding](#resharding-2pc-style) for how the migration runs.

#### Routing implementation

The router lives entirely inside `ClusterConfig`. `ClusterConfig::get_shard_for_key(table, key)`:

1. If a `TableShardingPolicy` is loaded for `table`, extract the sharding key using its `KeyExtractor` (`FIELD_INDEX`, `PREFIX_BYTES`, or `HASH_MOD`) and binary-search the sorted range list. This is the **range-based** path.
2. Otherwise, fall back to the default `sharding/mode`:
   - `lex` (default): return the shard whose `[start_key, end_key)` range covers the raw key.
   - `hash`: `bucket = hash(key) % bucket_count` (fixed slot space), then the shard that owns that bucket on the `hash(shard_id)` ring. *(Target model — the code today still computes the naive `hash(key) % shard_count`; see Data Partitioning.)*
3. If the shard the previous step landed on is currently `dead`, follow its `replacement` pointer (transitively, with a cycle guard bounded by `shard_count + 1` hops).

Callers are in `src/cluster/` — `shard_router.{h,cc}` is the public dispatcher; `sharding_policy.h`, `sharding_policy_cache.h`, `sharding_policy_builder.h` are the pure data types, cache, and fluent builder respectively.

`compute_shard_for_key(table_id, key)` (`shard_router.cc`) consults a **process-global `ClusterConfig`** (`janus::get_cluster_config()`, populated by the `ConfigWatcher`'s update callback) as the single source of truth once it has a nonzero `shard_count` — resolving the `table_id` to a table name via `TableRegistry` and delegating to `ClusterConfig::get_shard_for_key(table, key)`. Until the watcher populates that global (i.e. before the shard-0 config path is wired at a node), the router falls back to the legacy `ShardingPolicyCache` and, failing that, the table-ID heuristic `(table_id - 1) / NUM_TABLES_PER_SHARD`. This gate means the `ClusterConfig`-based path is a no-op until wired in, so it can land ahead of the runtime bootstrap without changing behavior.

The byte-key path (`compute_shard_for_key`) hard-codes the "first 8 bytes, big-endian" decoding for `FIELD_INDEX`; callers whose sharding field isn't at offset 0 should use `compute_shard_for_key_value` with the value pre-extracted.

For expected cross-shard ratios under TPC-C (the canonical benchmark), see `docs/reference/tpcc-sharding.md` — warehouse-based sharding yields ~5% remote NewOrder and ~8% remote Payment with 2 shards.

### Data Migration Protocol (Online Resharding)

Moving a key range (or hash bucket) from a **source** shard to a **destination** shard is the primitive under `move_range`, `remove_shard`/`drain_shard`, `rebalance`, and `set_sharding_policy`. Mako does it **online** — the source keeps serving the range until the very last moment — with a long background bulk copy followed by a short two-phase-commit cutover. Shard 0 (the master) is the coordinator; the source and destination are the participants. All phase state lives in `reshard/*` keys on `__mako_config__`, so it is replicated and crash-recoverable like any other config.

Besides the master's global view, **each shard also keeps a small piece of local metadata**: the set of key ranges it is currently in charge of, and — for any migration it is participating in — its role (source or destination), the stage (copying vs. locked), and the attempt's generation. This participant-local state is what lets a shard reject a request for a key it no longer owns (a `WrongShard` error), freeze *its* range at LOCK, and cast its own prepare vote — without consulting the master on every request.

**Design goals.**
- The source serves reads *and* writes for the range throughout the (potentially long) bulk copy — no availability hit while the data moves.
- **No lost writes**: writes that arrive during the copy are captured and applied before cutover.
- The unavailability window is bounded to the *final delta* (small), not the whole dataset.
- Crash-recoverable: every phase transition is a durable, versioned Raft write on shard 0, so a coordinator or participant crash resumes or aborts cleanly.

```
             PREPARE            LOCK            COMMIT
                v                v                 v
 source: serving ── serving ────┤ frozen ├──────── drops range
                 (bulk copy)    │(final  │
 dest:   building ── catching-up┤ sync)  ├──────── serving
                 ^              ^                 ^
          snapshot + delta   delta small     routing flips
          (async, online)   (stop the range)  (version bump)
```

**Phase 0 — PREPARE (intent).** The master writes the migration intent in one atomic Raft batch — `reshard/active=1`, `reshard/src`, `reshard/dst`, `reshard/range=[lo,hi)`, `reshard/phase=copy` — bumping `__version__`. From here the operation is durable: a master crash re-reads the intent and resumes. The routing assignment still points at the **source**; the range is merely flagged *migrating*.

**Phase 1 — BACKGROUND COPY (source live).** The destination pulls a consistent snapshot of the range from the source and bulk-loads it, in the background. Meanwhile the source **keeps serving reads and writes** for the range. Writes that land on the range after the snapshot point are captured as a **delta** — the source dual-writes them into a change buffer the destination tails — so the destination converges toward the source. A **deletion is a tombstone** in that delta (a "null write"), so a key removed after the snapshot is propagated as a positive fact — the destination learns the key is *gone*, not merely "not copied." This phase is unbounded in time and fully online. It ends when the destination has caught up to "most of the data" (the outstanding delta is small).

**Phase 2 — LOCK (2PC prepare: freeze the range).** The master flips `reshard/phase=lock` and tells **both** participants to prepare:
- The **source stops serving the range** — reads/writes for `[lo,hi)` are rejected with a `Migrating`/`WrongShard`-style error, so clients retry (blocking briefly on *that range only*; the rest of the source's key space is unaffected).
- The **destination** stops tailing and readies to take ownership.

This is the *only* window where the range is unavailable, and it covers just the final delta.

**Phase 3 — FINAL SYNC + VERIFY.** The source ships the remaining delta (puts *and* delete-tombstones since the last applied point) to the destination, which applies it. Both sides then compute a **checksum over the range** (a hash folding every live key→value pair *and* every tombstone). The destination acks "prepared" **only if its checksum equals the source's** — proof the copy is complete and byte-identical. A mismatch (a dropped or garbled transfer) is a "no" vote, so the master **aborts** rather than cutting over to a divergent copy. This is what makes the cutover safe: the routing flip in COMMIT is gated on a positive equality proof, not just "the copy finished."

**Phase 4 — COMMIT.** The master commits **only if both participants voted "prepared"** (source frozen, destination caught up). It writes the cutover atomically: the range→shard assignment now names the **destination**, `reshard/phase=committed` (then the `reshard/*` keys clear), and `__version__` bumps. On the new version every node's `ConfigWatcher` re-resolves routing — the range routes to the destination, which begins serving it — and the source **drops** the range's data. The unavailability window closes.

**ABORT (a participant times out).** If either participant fails to ack "prepared" within the timeout — say the destination is unreachable during the final step — the master **aborts** instead of committing. It clears the intent / writes `reshard/phase=abort`; the source **resumes serving** the range (it never lost the data — it only stopped *serving* during LOCK), and the destination discards its partial copy. Because the assignment still points at the source until COMMIT, an abort is invisible to clients beyond the brief LOCK-window retry.

**Stale votes are fenced by a generation.** Each migration attempt carries a monotonic **generation** id, and every prepare-ack the master accepts must match the current one. So if a timed-out participant's ack arrives *after* the master has already aborted (or moved on to a new attempt), the master sees a superseded generation and **ignores it** — a late "prepared" can never resurrect an aborted migration or wrongly count toward a later one. This is the classic 2PC coordinator rule: once the coordinator decides ABORT, no delayed vote can flip it.

**Crash recovery.** The `reshard/*` keys (Raft-replicated on shard 0) are authoritative. On master restart mid-migration: `phase=copy` → restart the copy (idempotent — re-snapshot); `phase=lock`/`final_sync` → roll forward (re-run final sync + commit) or abort; `phase=committed` → finish cleanup. A participant crash during COPY just restarts the copy; during LOCK the master aborts and the source resumes, since no commit was reached.

**How the master commands use it.**
- `move_range(table, range, from, to)` — one migration, `from`→`to`.
- `remove_shard(id)` / `drain_shard(id)` — a *set* of migrations, one per range the leaving shard owns, spread across the remaining shards; the shard is removed only once all commit.
- `rebalance()` — a batch of `move_range`s computed to even the load.
- `set_sharding_policy(table, assignment)` — diff the new assignment against the current one; every moved partition becomes a migration.

Only the COMMIT of each migration bumps the routing version, so clients observe a clean before/after per range — never a half-migrated state. (`kill_shard` skips this protocol entirely: the source is *dead*, so there is nothing to copy — it just reassigns the range via the `replacement` pointer and recovers data from the destination's own replicas.)

### Speculation Recovery (Epoch-Based)

The CM orchestrates recovery when a shard leader fails mid-speculation (Section 5.2 of the Mako paper).

**Replication as a blind log layer.** The paper uses Paxos for replication, but our implementation uses Raft. This distinction does not matter for speculation recovery — both Paxos and Raft are treated as a **blind replicated log**. The speculative execution layer sits above the log layer, and epochs are managed independently of the consensus protocol.

**Epochs.** Mako groups replicated log entries into **epochs**. An epoch is a **global** number — all shards converge to the same epoch during normal operation. The CM maintains the current epoch in the config table (`epoch`). A lagging shard may temporarily be on an older epoch, but it will catch up. When an epoch advances, the CM writes an **AdvanceSpecEpoch entry** to the Raft replicated log on shard 0. This entry is replicated to all shard 0 followers, and ConfigWatchers on other shards detect the version change.

**Explicit epoch advance in the Raft log.** When the CM decides to advance the epoch (e.g., on leader failure), it does not just update a config key — it explicitly inserts an `AdvanceSpecEpoch(new_epoch)` entry into the Raft log. This ensures:
- The epoch advance is **ordered** relative to other config changes in the log.
- The epoch advance is **durable** — it survives CM crashes.
- All replicas of shard 0 see the epoch advance in the same position in the log.
- On recovery, the CM replays the log and reconstructs the correct epoch state.

**Recovery protocol.** When the CM detects a shard leader failure:

1. **Advance epoch**: CM inserts `AdvanceSpecEpoch(epoch+1)` into the Raft log. Once committed, the new epoch is broadcast to all shards via ConfigWatcher polling.

2. **Close old epoch on failed shard**: New leader retrieves replicated entries from peers, re-commits them, and issues no-ops for unrecoverable entries. Replicates an **INF shard clock** to signal epoch closure.

3. **Close old epoch on healthy shards**: Healthy shards finish old-epoch work and replicate their own INF entries.

4. **Global finalized watermark**: Each shard computes its finalized shard watermark (min clock across its Raft streams). The CM collects these and computes the global watermark = min across all shards — a single scalar timestamp providing a consistent global cutoff.

5. **Rollback**: Speculative transactions above the global watermark that depended on lost transactions are rolled back. Unaffected transactions on healthy shards proceed normally.

**Scalar timestamp watermark.** We use a single scalar watermark (not a vector) for simplicity and scalability:

```
global_watermark = min(shard_0_watermark, shard_1_watermark, ..., shard_N_watermark)
shard_i_watermark = min(stream_0_clock, stream_1_clock, ..., stream_K_clock)
```

This is conservative (may slightly delay visibility) but correct and scales to thousands of shards. The trade-off: a transaction that depends only on shard 1 must wait for all shards to catch up. In practice, shards replicate at similar rates, so the lag is bounded by the slowest shard.

### Consistency Guarantees

- **Config writes**: Serializable (regular transactions on shard 0, replicated via Raft).
- **Config reads**: Linearizable from shard 0 leader, or eventually-consistent from watchers (bounded by poll interval).
- **Version monotonicity**: Watchers only apply configs with strictly higher versions.
- **Epoch ordering**: Epoch advances are ordered in the Raft log, ensuring all replicas agree on epoch transitions. The epoch is global — all shards converge to the same number.

---

## 4. Core Protocol: Speculative Two-Phase Commit

### The Problem

Traditional 2PC in geo-replicated settings suffers from high latency because each phase must wait for cross-datacenter round trips:

```
Traditional 2PC:
  Execute -> Wait Paxos (50ms RTT) -> Wait Disk (10ms) -> Return
  Total: ~150-200ms per transaction
```

### Mako's Key Insight

> In most workloads, 99%+ of transactions commit successfully. Why penalize every transaction with replication latency for a rare failure case?

### Speculative 2PC Protocol

Mako executes transactions speculatively, returning success to clients before replication completes:

```
Mako Speculative 2PC:
  Execute -> Return to Client (~2ms)
             |
             +-> (background) Paxos Replication -> Disk Persistence
```

The protocol operates in three phases:

**Phase 1: Speculative Execution (immediate)**
1. Client sends COMMIT to coordinator
2. Coordinator assigns a logical timestamp
3. Transaction executes on the leader shard (writes applied to Masstree)
4. Client receives SUCCESS response (~2ms)

**Phase 2: Background Replication (asynchronous)**
1. Transaction is serialized to a binary log
2. Log entry submitted to Paxos for consensus
3. Paxos replicates to follower replicas
4. No client blocking

**Phase 3: Watermark Advancement (background)**
1. Each shard tracks its replication progress as a `local_timestamp`
2. Global watermark = minimum of all `local_timestamp` values
3. Transactions become visible to readers when their timestamp <= watermark
4. Watermark is monotonically increasing and bounded in lag

### Watermark Mechanism

The watermark is the central safety mechanism that ensures consistency:

```
Timeline:
  T=90   T=95   T=100   T=105   T=110
   |       |       |       |       |
   +-------+-------+-------+-------+
   | VISIBLE       | SPECULATIVE
   | (replicated)  | (not yet replicated)
                   |
             Watermark=100
```

**Properties:**
- Monotonically increasing (never goes backwards)
- Bounded lag (at most replication time behind latest transaction)
- All replicas see consistent ordering below watermark
- Transactions below watermark are durable

**Implementation:**
```cpp
// Per-partition replication progress
static vector<std::atomic<uint32_t>> local_timestamp_;

// Global watermark = min across all partitions
uint32_t computeGlobalWatermark() {
    uint32_t min_ts = UINT32_MAX;
    for (int i = 0; i < num_partitions; i++) {
        uint32_t local_ts = local_timestamp_[i].load(memory_order_acquire);
        min_ts = min(min_ts, local_ts);
    }
    single_watermark_.store(min_ts, memory_order_release);
    return min_ts;
}
```

### Failure Handling

**Leader failure before replication:**
- Transaction was never replicated, so it is lost
- Watermark never advances past the lost transaction
- Client received "success" but the transaction is speculative
- In practice, extremely rare (~0.01%)
- Mitigation: clients can wait for watermark to consider a transaction "final"

**Follower failure:**
- Paxos only requires majority; transaction commits normally
- Failed follower catches up asynchronously when it recovers

**Network partition:**
- Majority partition continues operating
- Minority partition blocks (cannot reach quorum)
- Automatic reconciliation when partition heals

**Cascading abort prevention:**
- Dependency tracking records conflicts between concurrent transactions
- Bounded speculation window limits how far ahead speculation can go
- Watermark acts as a barrier: transactions above watermark don't affect below-watermark reads

### Comparison with Other Protocols

| | Mako S2PC | Traditional 2PC | Calvin | Spanner |
|---|-----------|-----------------|--------|---------|
| Client Latency | ~2ms (speculative) | ~150ms | 1 RTT + sequencer | ~10ms (TrueTime) |
| Consistency | Serializable (watermarks) | Strong | Deterministic | External |
| Speculation | Yes | No | No | No |
| Hardware Req. | None | None | None | GPS/atomic clocks |

---

## 5. Replication and Consensus

Mako supports two pluggable replication backends:

| Replication | Build Command | Binary | Use Case |
|-------------|---------------|--------|----------|
| **Paxos** (default) | `make -j32` | `dbtest` | Production with Paxos consensus |
| **Raft** | `make mako-raft -j64` | `dbtest` | Mako with Raft replication |

### Multi-Paxos

Mako uses Multi-Paxos for efficient replication:
- Stable leader proposes all values (skips PREPARE after first proposal)
- Majority agreement (N/2 + 1) before commit
- Each shard has its own independent Paxos group

**Replication flow:**
1. Leader receives transaction commit
2. Leader proposes to followers
3. Majority responds with ACCEPT
4. Leader commits locally
5. Followers apply committed value

### Raft

Raft provides an alternative consensus mechanism with clearer leader election semantics. Build and test with:

```bash
make mako-raft -j64
./ci/ci_mako_raft.sh all
```

### Fault Tolerance

With N replicas, Mako tolerates floor(N/2) failures:
- 3 replicas: tolerates 1 failure
- 5 replicas: tolerates 2 failures

### Replicated RocksDB (Raft State Machine)

The Configuration Manager (Section 3) needs a **durable, replicated key-value store**. Rather than building a bespoke solution, Mako layers RocksDB on top of the Raft consensus module as a generic **replicated state machine**. This same pattern can serve any component that needs strongly-consistent replicated storage.

#### Architecture

```
  Client
    |
    v
+-------------------+
| ReplicatedDB      |  <-- Application-facing API (Get/Put/Delete)
|   (leader only    |
|    for writes)    |
+-------------------+
    |  writes go through Raft
    v
+-------------------+
| RaftServer        |  <-- Consensus: replicates log entries across nodes
|   app_next_()     |  <-- Callback on commit: applies entry to RocksDB
+-------------------+
    |
    v
+-------------------+
| RocksDB           |  <-- Local durable state machine on each replica
+-------------------+
```

**Write path**: Client calls `ReplicatedDB::Put(key, value)` on the leader. The leader serializes the operation into a Raft log entry and proposes it. Once Raft commits the entry (majority ack), the `app_next_` callback fires on each replica, applying the Put to the local RocksDB instance. The leader returns success to the client.

**Read path**: Reads can be served from any replica's local RocksDB (stale reads), or only from the leader (linearizable reads). For the Configuration Manager, leader reads are sufficient since config changes are infrequent.

**Snapshot integration**: `ReplicatedDB` registers callback hooks on `RaftServer` (`create_sm_snapshot_cb_` / `load_sm_snapshot_cb_`). When `CreateSnapshot()` fires, it calls `rocksdb_checkpoint_create()` to produce a consistent checkpoint, serializes all checkpoint files into a binary blob (format: `num_files(4) + [name_len(4) + name + file_size(8) + file_data]*`), and returns the blob as snapshot data. When `OnInstallSnapshot()` fires on a follower, it deserializes the blob, closes the current RocksDB instance, replaces it with the checkpoint files, and reopens the database. This replaces the previous minimal state marker (16-byte executeIndex + term) with a real state machine snapshot.

#### Operation Encoding

Each Raft log entry encodes a key-value operation:

| Field | Type | Description |
|-------|------|-------------|
| `op` | uint8 | `PUT=1`, `DELETE=2`, `BATCH=3` |
| `key` | string | Key bytes |
| `value` | string | Value bytes (empty for DELETE) |

For `BATCH`, the entry contains a sequence of (op, key, value) tuples applied atomically via `RocksDB::WriteBatch`.

#### ReplicatedDB Class

```cpp
class ReplicatedDB {
  RaftServer* raft_;           // Consensus layer
  rocksdb::DB* db_;            // Local state machine
  
  // Write (leader only) — proposes to Raft, applied on commit
  Status Put(const std::string& key, const std::string& value);
  Status Delete(const std::string& key);
  
  // Read (local) — reads directly from RocksDB
  Status Get(const std::string& key, std::string* value);
  
  // Raft callback — called on each replica when entry is committed
  void ApplyEntry(slotid_t index, shared_ptr<Marshallable> cmd);
  
  // Snapshot — creates RocksDB checkpoint and serializes into binary blob
  std::string CreateStateMachineSnapshot();
  // Snapshot — deserializes binary blob, replaces DB with checkpoint, reopens
  void LoadStateMachineSnapshot(const std::string& data);
};
```

#### Configuration Manager Integration

The Config Manager (Section 3) uses `ReplicatedDB` as its storage backend on shard 0:

- `ConfigManager::Put/Get` delegate to `ReplicatedDB::Put/Get`
- Config keys (`__version__`, `shard/<id>/replicas`, etc.) are regular RocksDB keys
- Config changes are Raft-replicated writes — no separate replication path
- On leader failure, the new leader's RocksDB has the same committed state
- Snapshot/recovery uses RocksDB checkpoints instead of log replay

This means the `__mako_config__` table from Section 3 is physically stored in a RocksDB instance that is replicated via Raft.

---

## 6. Storage Engines

### Masstree (Primary - In-Memory)

A high-performance concurrent B+tree from MIT, optimized for multi-core CPUs:
- **Lock-free reads**: no locks for read operations
- **Multi-version**: supports concurrent readers/writers
- **Cache-friendly**: optimized memory layout for modern CPUs
- **Sub-microsecond** read latency, millions of ops/sec per core

### RocksDB (Optional - Persistent)

LSM-tree based persistent storage for durability:
- **Write-ahead log (WAL)** for crash recovery
- **Asynchronous writes** (don't block transactions)
- **Background compaction** maintains read performance

**Data flow:**
```
READ PATH:  Client -> Masstree (in-memory) -> Response  [no disk I/O]
WRITE PATH: Client -> Masstree -> [async] -> Paxos -> [async] -> RocksDB
```

**Persistence callback mechanism:**
```cpp
persistence.persistAsync(log_data, size, shard_id, partition_id,
    [](bool success) {
        // Called asynchronously when persistence completes
    });
```

RocksDB databases are created at `/tmp/mako_rocksdb_{shard_id}`.

### The three-backend KV surface (non-transactional)

There is ONE non-transactional KV interface — the non-txn ops on
`abstract_ordered_index` (`get / put / insert / remove / scan /
rscan`, raw-byte values in both directions) — with three
implementations picked at construction time:

```cpp
abstract_ordered_index* t = new mbta_ordered_index("mytable", id, db);
// or: new masstree_ordered_index("mytable", id);
// or: new mbta_sharded_ordered_index("mytable", shard_tables);
t->put(lcdf::Str("k"), "value");   // same code either way
```

- `masstree_ordered_index` — plain Masstree (L1), no transactions;
  owns value memory with RCU-deferred frees; the transactional
  virtuals abort loudly.
- `mbta_ordered_index` — Silo's table; each non-txn op is an internal
  one-op OCC transaction (Encode/strip handled internally).
- `mbta_sharded_ordered_index` — per-key routing; remote keys travel
  self-contained non-txn RPCs, and writes on a replicated leader
  reach the replication log through the normal commit path.

Design and semantics: [`storage-interface.md`](storage-interface.md).

---

## 7. Networking and RPC

### Transport Backend

Mako has a single RPC backend:

| Feature | rrr/rpc |
|---------|---------|
| Latency | ~10-50 us (TCP/IP) |
| Hardware | Standard Ethernet |
| Portability | Any platform |
| Use case | Dev, testing, cloud, production |

```bash
./build/dbtest config/tpcc.yml
```

Worker threads never see the transport: they reach requests through the
`TransportRequestHandle` interface, implemented by `RrrRequestHandle`.

```cpp
class TransportRequestHandle {
    virtual uint8_t GetRequestType() const = 0;
    virtual char* GetRequestBuffer() = 0;
    virtual char* GetResponseBuffer() = 0;
    virtual void EnqueueResponse(size_t msg_size) = 0;
};
```

### rrr/rpc Reliability Features

The rrr/rpc backend includes production-grade reliability:
- **Connection state machine**: NEW -> CONNECTING -> CONNECTED -> DISCONNECTING -> DISCONNECTED -> FAILED
- **Automatic reconnection** with exponential backoff and jitter
- **Circuit breaker** for fail-fast cascade prevention
- **Request buffering** during disconnection, replayed after reconnection
- **Heartbeat/keep-alive** for liveness detection
- **Graceful shutdown** with request draining

---

## 8. Concurrency Model: Fibers and the Reactor Pattern

### Why Fibers?

| | Threads | Fibers |
|---|---------|------------|
| Stack size | 1-8 MB | 4-64 KB |
| Context switch | ~1-10 us (kernel) | ~10-100 ns (user) |
| Synchronization | Mutexes, atomics | None needed |
| Practical limit | ~10,000 | 100,000+ |
| Parallelism | True (multi-core) | Concurrent (single thread) |

Mako uses **stackful fibers** and a **reactor pattern** for event-driven I/O.

### Key Concepts

**Reactor**: The event loop that manages all fibers in a thread. One reactor per thread; never share across threads.

```cpp
auto reactor = Reactor::get_reactor();  // Thread-local
reactor->create_run_fiber([]() {
    // Your concurrent task
});
reactor->loop(true);  // Run event loop
```

**Events**: Synchronization primitives for fibers.

| Type | Purpose |
|------|---------|
| `IntEvent` | Wait for an integer condition |
| `TimeoutEvent` | Wait for a time duration |
| `OrEvent` / `WaitAny` | Wait for ANY of multiple events |
| `AndEvent` / `WaitAll` | Wait for ALL events |

**Fiber API** (preferred for new code): Uses `Fiber` terminology with `this_fiber::yield()`, `this_fiber::sleep_ms()`, etc.

### Thread Safety Advantage

Fibers within the same reactor never run simultaneously. This means shared data access is safe without locks:

```cpp
// SAFE - no locks needed within a reactor
class BankAccount {
    int balance = 1000;
public:
    void withdraw(int amount) {
        if (balance >= amount)
            balance -= amount;  // No race condition
    }
};
```

### Thread Model

```
Main Thread
  +-- Reactor Thread 1
  |     +-- Shard 0 (thousands of fibers)
  |     +-- Event Loop (poll sockets, timers, wake fibers)
  +-- Reactor Thread 2
  |     +-- Shard 1 (thousands of fibers)
  |     +-- Event Loop
  +-- Background Threads
        +-- RocksDB Compaction
        +-- Metrics, GC
```

**Design principle:** One reactor per shard, shared-nothing, lock-free where possible.

### Pitfalls

1. **Never access events/fibers across threads**
2. **Events are single-use** (don't Wait() twice on the same event)
3. **One waiter per event** (no multiple fibers waiting on same event)
4. **Always yield in long-running loops** (cooperative scheduling)
5. **Always run the event loop** (fibers that yield are dead until resumed)

---

## 9. Configuration Reference

Mako uses YAML configuration files in `config/`. The configuration defines cluster topology, workload, and runtime settings.

### Cluster Topology

```yaml
# Sites: servers (shards) and clients
site:
  server:
    - ["s101:8100"]                            # Shard 0, no replication
    - ["s102:8100", "s202:8101", "s302:8102"]  # Shard 1, 3 replicas
  client:
    - ["c101"]

# Map site names to process names
process:
  s101: localhost
  s102: localhost
  s202: server1
  s302: server2
  c101: localhost

# Map process names to IP addresses
host:
  localhost: 127.0.0.1
  server1: 10.0.1.100
  server2: 10.0.2.100
```

**Key rules:**
- Each server line = one shard
- First server in a list = leader (primary)
- Additional servers = followers (replicas)
- Multiple sites can share an OS process (different threads)

### Configuration Hierarchy

```
Host (Physical Machine, IP address)
  +-- Process (OS Process)
        +-- Site/Partition (Thread)
              +-- Shard (Data Partition)
              +-- Reactor (Event Loop)
```

### Benchmark Configuration

```yaml
bench:
  workload: tpcc    # Options: tpcc, tpca, rw, micro
  scale: 1          # Scaling factor

  weight:           # TPC-C transaction mix (must sum to 100)
    new_order: 44
    payment: 44
    delivery: 4
    order_status: 4
    stock_level: 4

n_concurrent: 100   # Concurrent transactions
```

### Common Configurations

| Config | Shards | Replicas | Use Case |
|--------|--------|----------|----------|
| `1c1s1p.yml` | 1 | 1 | Development/debugging |
| `1c2s2p.yml` | 2 | 1 | Multi-shard testing |
| `1c2s3r.yml` | 2 | 3 | HA with Paxos |
| Geo-replicated | N | 3 (across DCs) | Production |

### Multi-Shard Single-Process Mode

Run multiple shards in one process for development/testing:

```bash
./build/dbtest \
    --local-shards=0,1,2 \
    --shard-config src/mako/config/local-shards3-warehouses7.yml \
    --num-threads 7
```

Port assignment: Shard N uses base_port + N*100 (e.g., 31000, 31100, 31200).

---

## 10. Build System

### Build Targets

| Target | Command | Description |
|--------|---------|-------------|
| Mako + Paxos | `make -j32` | Default build (~2-3 min) |
| Mako + Raft | `make mako-raft -j64` | Raft replication |
| Raft Lab Tests | `make raft-test -j32` | Only for `raft_lab_test.yml` |
| Clean | `make clean` | Remove build artifacts |

**Build time expectations** (important!):
- Initial full build: 10-30 minutes
- Incremental builds: 2-10 minutes
- Docker image build: 10-30 minutes

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MAKO_USE_RAFT` | OFF | Build with Raft replication |
| `RAFT_TEST` | OFF | Enable Raft lab test fibers |
| `ENABLE_BORROW_CHECKING` | OFF | Enable RustyCpp borrow checking |
| `DEBUG` | OFF | Debug mode with `-DDEBUG` |

### Output Binaries

| Binary | Description |
|--------|-------------|
| `build/dbtest` | Main Mako binary (Paxos or Raft) |
| `build/deptran_server` | Standalone Raft server |
| `build/simpleRaft` | Simple Raft replication test |
| `build/test_rocksdb_persistence` | RocksDB persistence test |

### Quick Start

```bash
git clone --recursive https://github.com/makodb/mako.git
cd mako
bash apt_packages.sh
source install_rustc.sh
make -j$(nproc)
./ci/ci.sh simpleTransaction   # Verify installation
```

---

## 11. Testing

**All CI tests must be run via Docker:**

```bash
./docker_build.sh ci all          # Run all 19 tests
./docker_build.sh ci <testname>   # Run specific test
```

### Test Suite

| Test | Description |
|------|-------------|
| `rrrTests` | RRR framework unit tests |
| `simpleTransaction` | Basic transaction execution |
| `simplePaxos` | Paxos replication |
| `shardNoReplication` | 2 shards, no replication |
| `shard1Replication` | 1 shard with Paxos replication |
| `shard2Replication` | 2 shards with Paxos replication |
| `shard1ReplicationSimple` | 1 shard, simple transaction + Paxos |
| `shard2ReplicationSimple` | 2 shards, simple transaction + Paxos |
| `shard1ReplicationRaft` | 1 shard with Raft |
| `shard2ReplicationRaft` | 2 shards with Raft |
| `shard1ReplicationSimpleRaft` | 1 shard, simple transaction + Raft |
| `shard2ReplicationSimpleRaft` | 2 shards, simple transaction + Raft |
| `rocksdbTests` | RocksDB persistence |
| `shardFaultTolerance` | Shard crash/reboot resilience |
| `multiShardSingleProcess` | Multi-shard in one process |
| `cpuThrottlingScaling` | CPU throttle scaling test |

### Optional Quick Build Path

```bash
./docker_build.sh build                      # Build once
./docker_build.sh ci-quick shardNoReplication # Run without rebuild
```

### Debugging

- Debug build: `MODE=debug make`
- GDB: `gdb --args ./build/dbtest ...`
- Performance profiling: `MODE=perf make`, then use `perf record` / `perf report`

---

## 12. Memory Safety: RustyCpp

All new C++ code **must** be written to be rusty-safe. This is mandatory.

### Required Types

| Use This | NOT This | Purpose |
|----------|----------|---------|
| `rusty::Box<T>` | `std::unique_ptr<T>` | Single ownership |
| `rusty::Arc<T>` | `std::shared_ptr<T>` | Thread-safe shared ownership |
| `rusty::Rc<T>` | `std::shared_ptr<T>` | Single-thread shared ownership |
| `rusty::Cell<T>` | mutable field | Interior mutability (Copy types) |
| `rusty::RefCell<T>` | mutable field | Interior mutability (complex types) |
| `rusty::Option<T>` | `std::optional<T>` | Optional values |

### Safety Annotations

Every function must have a safety annotation:

```cpp
// @safe - Pure function, no side effects
const char* status_to_string(Status s) { ... }

// @safe - Read-only access through Cell::get()
ReplicationType get_replication_type() {
    return g_replication_type.get();
}

// @unsafe - Calls non-borrow-checked legacy code
void dispatch_to_legacy(int arg) {
    legacy_function(arg);  // @unsafe
}
```

### Global State Pattern

Use `rusty::Cell<T>` for global mutable state:

```cpp
#include <rusty/cell.hpp>

static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};

// @safe
ReplicationType get_replication_type() {
    return g_replication_type.get();
}

// @safe
void set_replication_type(ReplicationType type) {
    g_replication_type.set(type);
}
```

### Memory Safety Rules

1. **Ownership**: Every object has a single owner at any given time
2. **Borrowing**: Use references (`&`) for read-only access; avoid raw pointers
3. **Lifetime**: Ensure references don't outlive the objects they refer to
4. **Move semantics**: Prefer `std::move` for ownership transfer; avoid use-after-move
5. **No mutable aliasing**: Don't have multiple mutable references to the same object

### Borrow Checking

- Build runs borrow checking automatically via CMake when `ENABLE_BORROW_CHECKING=ON`
- Run `make borrow_check_deptran` or `make borrow_check_raft` to verify checked files
- Keep `third-party/rusty-cpp` submodule on `main` branch at latest commit
- Files with heavy third-party headers may be excluded (document why in CMakeLists.txt)

---

## 13. Performance Optimization Techniques

Mako employs several multi-core optimization techniques:

### Per-Core Data Partitioning
Each CPU core gets its own private copy of frequently-written data, eliminating cache-line bouncing:

```cpp
template <typename T>
class percore {
    T& my() { return (*this)[coreid::core_id()]; }  // Fast path
};
```

### Cache-Line Alignment
Data structures are padded to 64-byte cache line boundaries to prevent false sharing between cores.

### Lock-Free Data Structures
Masstree uses lock-free reads and optimistic concurrency control with version validation.

### Memory Ordering
Careful use of `memory_order_acquire`, `memory_order_release`, and `memory_order_relaxed` atomics throughout the codebase to minimize synchronization overhead.

### Thread-Local Storage
Per-thread state avoids contention on shared data structures.

### CPU Prefetch and Branch Hints
Strategic use of `__builtin_prefetch()` and `__builtin_expect()` for hot paths.

### Custom Memory Allocation
jemalloc for optimized allocation; per-CPU memory allocators for reduced contention.

---

## 14. Design Principles

### The Seven Principles of Mako

1. **Speculation with Safety Nets**: Execute optimistically, validate with watermarks. Fast path is truly fast; rare failures handled correctly.

2. **Parallelism Over Sequentiality**: Multiple Paxos streams per partition run in parallel. Shards operate independently. Never wait when you can parallelize.

3. **Shared-Nothing Architecture**: Each shard has its own Masstree, Paxos group, and watermark tracking. Only multi-shard transactions require coordination.

4. **Memory-First Storage**: Keep hot data in memory. Disk is for durability, not reads. Read path never touches disk.

5. **Cooperative Concurrency**: Fibers, not threads. Thousands of concurrent operations per thread with no synchronization overhead.

6. **Strong Types, Safe Memory**: RustyCpp smart pointers and borrow checking catch bugs at compile time.

7. **Simplicity at the Interface**: Common use cases are trivial; advanced tuning is possible but not required.

### Key Trade-offs

| Trade-off | Choice | Pro | Con |
|-----------|--------|-----|-----|
| Latency vs. Durability | Speculative execution | ~30x lower latency | Tiny loss window on leader failure |
| Memory vs. Disk I/O | In-memory primary | Sub-us reads | Dataset must fit in RAM |
| Consistency vs. Availability | Strong (serializable) | No anomalies | Unavailable during minority partition |

---

## 15. Anti-Patterns to Avoid

### Hot Shard
**Problem:** All transactions touch the same shard.
**Fix:** Shard counters or use hash-based key distribution.

### Cross-Shard Transaction Storm
**Problem:** Most transactions span multiple shards.
**Fix:** Co-locate related data on the same shard (e.g., shard by `user_id`).

### Ignoring Watermarks
**Problem:** Assuming speculative commits are immediately visible.
**Fix:** Use same-transaction reads, or wait for watermark advancement for read-after-write.

### Long-Running Transactions
**Problem:** Holding locks for extended periods.
**Fix:** Minimize transaction scope; do computation outside the transaction.

---

## 16. Troubleshooting

### Quick Diagnostic Checklist

- Ports available? `lsof -i :8100`
- Build successful? `make -j$(nproc)` completed without errors
- YAML config valid? Hosts reachable?
- Sufficient memory? (8GB+ RAM)
- Processes running? `pgrep dbtest`

### Common Issues

| Issue | Solution |
|-------|----------|
| "Address already in use" | `pkill -9 dbtest; sleep 2` then retry |
| Submodule not found | `git submodule update --init --recursive` |
| Out of memory during build | `make -j2` (reduce parallelism) |
| Borrow checker parse errors | Ensure LIBCLANG_PATH matches system clang version |
| Raft leader churn | Increase heartbeat interval in `config/none_raft.yml` |
| Hanging test processes | `./ci/ci_mako_raft.sh cleanup` |

### Debugging

```bash
# Debug build
MODE=debug make -j$(nproc)

# Run under GDB
gdb --args ./build/dbtest --verbose --bench tpcc ...

# Verbose logging
export MAKO_LOG_LEVEL=debug

# Performance profiling
MODE=perf make -j$(nproc)
perf record ./build/dbtest ...
perf report
```

---

## 17. Glossary

| Term | Definition |
|------|------------|
| **2PC** | Two-Phase Commit - protocol for coordinating distributed transactions |
| **ACID** | Atomicity, Consistency, Isolation, Durability |
| **Ballot** | Unique proposal identifier in Paxos, ordered for precedence |
| **Epoch** | Time period for garbage collection and failure recovery |
| **Fiber** | Lightweight cooperative thread used by Mako |
| **Follower** | Replica that accepts proposals from the leader |
| **Frame** | Protocol-specific transaction processing module |
| **Janus** | OSDI'16 protocol that influenced Mako; its standalone implementation is retired |
| **Leader** | Replica that proposes values and coordinates consensus |
| **Local Timestamp** | Per-partition timestamp of most recently committed transaction |
| **Mako** | Speculative distributed transaction system (named for the fast mako shark) |
| **Master Shard** | Shard 0 — stores cluster configuration in `__mako_config__` table, replicated via Raft/Paxos |
| **Masstree** | In-memory concurrent B+tree storage engine |
| **Multi-Paxos** | Optimized Paxos with stable leader skipping prepare phase |
| **NO-OP** | Heartbeat/sync log entry in Paxos that triggers watermark computation |
| **OCC** | Optimistic Concurrency Control |
| **Partition** | Logical data subdivision within a shard, each with its own Paxos group |
| **Quorum** | Minimum replicas for progress: floor(N/2) + 1 |
| **Reactor** | Event loop managing fibers in a thread |
| **RocksDB** | LSM-tree persistent storage backend |
| **RRR** | "Repeatable Research Runtime" - Mako's custom RPC/fiber framework |
| **RustyCpp** | Library providing Rust-like smart pointers and borrow checking for C++ |
| **Safety Check** | Validation comparing transaction timestamp to watermark for follower replay |
| **Scheduler** | Component managing transaction execution on a shard |
| **Serializability** | Strongest isolation level; concurrent transactions appear to execute serially |
| **Shard** | Horizontal partition of data for scalability |
| **Site** | Logical server or client entity in configuration, mapped to processes and hosts |
| **Slot** | Position in the Paxos replicated log |
| **Speculation** | Executing before full consensus, optimistically assuming success |
| **TPC-C** | E-commerce benchmark for evaluating transaction throughput |
| **Watermark** | Timestamp guaranteeing all transactions at or below are durably replicated |

---

*This document consolidates Mako's design, architecture, and developer guidelines from across the project documentation. For detailed implementation plans and migration guides, see the `docs/` and `doc/` directories.*
