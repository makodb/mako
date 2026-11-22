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
|            RPC Communication Layer (SRPC)                       |
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

**SRPC Communication Layer** (`src/srpc/`):
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
    mako/             # Mako core
      masstree/       # Masstree storage engine
      lib/            # Transport backends, configuration
      benchmarks/     # Benchmark harness (TPC-C, TPC-A, RW)
    srpc/              # RPC framework and fibers
    bench/            # Benchmark workload implementations
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

`ConfigManager` retains a metadata persistence interface over the reserved
`__mako_config__` table. These keys are metadata utilities, not the authority
for native Rust routing; writing them does not publish a native ownership change.

| Key Pattern | Value | Description |
|-------------|-------|-------------|
| `__version__` | uint64 | Monotonically increasing config version |
| `shard_count` | uint32 | Number of live shards |
| `next_shard_id` | uint32 | Monotonic id allocator — the master hands out `next_shard_id` on `register_shard` and increments it; ids are never reused |
| `shard/<id>/replicas` | JSON array | Ordered replica list (first = preferred leader) |
| `shard/<id>/leader` | string | Current leader site name |
| `shard/<id>/status` | string | `active`, `draining`, `adding`, `removing` |
| `epoch` | uint64 | Global speculative epoch number (all shards converge to this) |
| `shard/<id>/replacement` | uint32 | Recorded replacement for a dead shard; metadata only |
| `sharding/mode` | string | Stored mode label; does not select a native routing implementation |
| `sharding/policy/<table>` | bytes | Opaque bytes persisted without parsing or installing a routing policy |
| `node/<site>/addr` | string | Node address (`ip:port`) |
| `node/<site>/status` | string | `alive`, `dead`, `decommissioning` |

### Key Components

**ConfigManager**: Typed configuration over a `KvStore` port (`get`/`put`/`remove`), not a bespoke store. Provides methods like `get_shard_replicas()`, `add_shard()`, `set_shard_leader()`, `advance_epoch()`. Every write increments `__version__` (written last). On shard 0's leader the port binds to the unified `FullOrderedIndex` — the `__mako_config__` system table — via the `OrderedIndexKvStore` adapter; other nodes bind a `RemoteKvStore` that reads shard 0 over RPC; unit tests bind an in-memory fake. The port keeps `cluster/` standalone-testable with no storage-engine dependency (see [The Storage Interface](storage-interface.md#cluster-metadata-port-srcclusterkv_storeh)).

**Native sharding runtime**: Owns governed routing and migration decisions in
Rust. The former C++ `ClusterConfig`, `ConfigWatcher`, policy builder/cache,
and stub shard manager have been retired; there is no C++ routing fallback.
`RemoteKvStore` and `ConfigKvService` remain metadata read boundaries, not
alternate ownership authorities.

### Bootstrap Protocol

The native sharding runtime is enabled with `MAKO_CLUSTER_CONFIG=1`.
TPC-C registers its canonical catalog and warehouse opener **before loading**.
`bench_runner::run()` calls `BootstrapClusterConfig(db)` after all loaders join;
the bootstrap creates a separate native Rust participant/work queue and srpc
service for each actual owner, including multiple owners in one process.
After loader synchronization, `mako_sharding_wait_ready(owner)` waits for the
native catalog/peer handshake before ordinary workers are launched. A filesystem
load marker alone does not establish native admission readiness.

The `NativeSharding::Invoke` service uses the shared benchmark transport
configuration's shard address with port delta `20000`, not DepTran's partition-0
address. A listener is permanently bound to its actual owner. Its poll-thread
handler only enqueues owned request bytes and a deferred reply into Rust;
blocking peer RPC and physical storage work run on owner-bound native workers.
The callback contract is in `src/mako/native_sharding_host.h`.

Normal shutdown joins ordinary workers while their client transports and every
owner's helper/control service remain available for terminal RPC cleanup.
The Rust runtime coordinates the fixed owners through the existing shared
`getNfsSyncDir()` directory: owner 0 creates a fresh exclusive rendezvous,
distributes its token, waits for every owner, fences new admin work and drains
retained migration receipts. Services stay live until its completion marker.
Rendezvous markers are retained until external run-directory cleanup; they are
not a restart/recovery protocol. Native service acceptance then stops, Rust jobs
join while srpc remains live, and RPC resources are released before indexes.
`ShutdownClusterConfig(owner)` precedes slow-exit index deletion; `db_close()`
provides process-wide teardown.

Live migration is explicitly non-replicated: the bootstrap passes
`allow_migration=0` when replication is enabled. This does not disable ordinary
replicated workloads, and does not claim crash recovery or Raft/migration
composition. With native configuration disabled, the existing static workload
path is unchanged.
Replication replay uses a separate scoped engine-lifetime guard, never ordinary
workload admission. It is accepted only for replicated configuration with no
migration-enabled native node, and latches against later migration activation.
It retains the engine's committed/stale-write behavior without granting normal
workloads permission to access a foreign ownership incarnation.

The `mako_admin` client uses the real native service and the Rust wire codec:

```bash
mako_admin begin HOST:CONTROL_PORT CLIENT SEQUENCE TABLE LO_HEX HI_HEX SOURCE DESTINATION
mako_admin poll HOST:CONTROL_PORT CLIENT SEQUENCE
mako_admin abort HOST:CONTROL_PORT CLIENT SEQUENCE
```

`CONTROL_PORT` is the configured coordinator shard port plus `20000`. Preserve
the same caller-owned `(CLIENT, SEQUENCE)` nonce and identical intent when a
begin reply is lost; poll returns the retained outcome without consuming it.
The table must already exist in the canonical catalog. Bounds are hexadecimal
bytes: `''` is an empty bound, while the literal `inf` in place of `HI_HEX` is an
unbounded upper endpoint. Warehouse IDs use four-byte big-endian coordinates.
The client prints `pending`, `committed`, or `aborted`; a transport error does
not imply that begin was rejected. There is no demo table or synthetic copy path.

### Config Persistence and Routing

`ConfigManager` preserves topology CRUD, monotonic ID allocation, epoch and
version updates, node addressing, and opaque policy-byte persistence. Its
`register_shard`, `kill_shard`, and `remove_shard` methods update metadata;
they do not themselves move data, recover replicas, or change native routing.
Likewise, `set_sharding_policy` and `set_sharding_mode` persist values rather
than install a policy. `RemoteKvStore` reads those values through a caller-supplied
read function; `ConfigKvService` supplies the RPC metadata-read boundary.

Use the native service and `mako_admin begin`/`poll` described above for live
range movement. Table identity and key coordinates come from the canonical
catalog registered by the workload. Governed operations reach the Rust runtime
through the native host bridge; C++ does not compute a second ownership answer.
With native configuration disabled, `src/cluster/shard_router.cc` retains only
static physical table-slot geometry. That helper is not a fallback for a
native route failure.


For expected cross-shard ratios under TPC-C (the canonical benchmark), see `docs/reference/tpcc-sharding.md` — warehouse-based sharding yields ~5% remote NewOrder and ~8% remote Payment with 2 shards.

### Data Migration Protocol (Live Native Sharding)

For the reviewable end-to-end design, including the separate conditional durable
path and its exact transaction/storage dependencies, start with
[Sharding and range migration](architecture/sharding-migration.md).

`src/cluster/` owns the protocol in native Rust; C++ supplies narrow storage,
RPC and lifetime callbacks. Owner 0 coordinates one active range move among
fixed, live, non-replicated owners. The service retains caller nonces, plans,
issued controls, receipts and outcomes. Repeating an identical request returns
its existing generation/outcome; it does not allocate another attempt.

1. **Stage and privately pre-copy.** The destination stages under the new
   generation while the source continues serving. Concurrent source reads use
   actual STO transactions, not unsynchronized raw storage scans.
2. **Publish Frozen, cut, and drain.** Publish immutable non-serving metadata
   before advancing the existing STO global epoch. Retain that same-clock cut
   and wait for every worker slot to be zero or at least the cut. Old readers,
   absent reads, scans and writers retain their oldest pin through effects,
   unlocks, engine cleanup and terminal peer cleanup, including OCC retries.
3. **Join, fence, and exactly reconcile.** Join background copy and fence its
   generation/round before final reconciliation. With the source range stable,
   merge complete ordered `(coordinate, row key)` scans, copy actual values and
   delete destination-only rows. EOF is explicit; an error or short nonempty
   page is not EOF. Readiness requires exact coverage, not a checksum or a
   dirty-only final delta.
4. **Retire and publish.** Fence the old source incarnation before publishing
   an immutable complete `(owner, ownership_version)` directory snapshot. Unrelated
   intervals retain their routes. A returned owner receives a new ownership version, so an old grant
   cannot become valid merely because its owner number matches again.
5. **Finish both participants.** Report a terminal outcome only after the
   required source/destination completion receipts. A storage cleanup failure
   retains its real partial state and is retried; changing metadata alone is
   not a cleanup acknowledgement.

Pre-copy reduces work that may remain but does not bound the final scan or freeze
duration. Final/Abort/new-move fences reject delayed pre-copy packets.
Abort before the irrevocable cutover restores source service and cleans staging;
after the decision, retries complete that decision. Generation, copy-round and
terminal fences reject delayed authentic controls from older attempts. No timeout
is interpreted as proof that an effect did not happen.

Ordinary native requests use epoch-cached immutable admission. The bridge is
`src/mako/lib/sharding_admission.{h,cc}` (`sharding_admission_enabled`).
After safely announcing the existing STO epoch with sample/announce/recheck,
`mako_sharding_read_begin(owner, epoch, out)` activates a reusable TLS
`MakoShardRead`. Owner/epoch cache hits compare integers rather than full metadata,
without per-transaction allocation, Arc clone or participant mutex. Cache misses
take an Arc to the already-published complete local metadata and routing view;
they do not recopy or recertify all metadata on each worker refresh.

The `mako_sharding_read_route`, `mako_sharding_read_point` and
`mako_sharding_read_range` APIs use captured Serving state and exact local
owner/ownership version, including whole-range coverage and absence.
Readiness, stop and metadata-failure checks belong to `mako_sharding_read_begin`,
including cache hits, not each access. An admitted context retains its view
through cleanup even if native service acceptance stops; host/index owners must
finish that work before destroying storage. `mako_sharding_read_end` follows
actual cleanup before the logical pin leaves. There is no epoch-at-commit
equality. Actual storage/transport failures follow their normal error paths.
Foreign snapshot numbers alone are not authorization: received requests still
validate provenance, coordinates and captured local metadata. Local resolution
authorizes before dispatch; no API accepts a returned point for revalidation.
Retries retain their exact request identity. No production per-access lease
registry, scope hint or participant mutex is used for admission.

Local points are thin routing results: selected index plus a captured-route
reference. They do not construct or copy full RPC envelopes. Remote dispatch
materializes an envelope directly into its destination, retaining the captured
grant and using the current transaction identity. Deferred batches use each
row's own capture from that transaction; control messages do not inherit a
previous row's scope. See the [storage contract](storage-interface.md#tpc-c-warehouse-handles).
Point, scan and receiver paths also share view-scoped indexed binding and
physical-handle caches. Warm lookup avoids registry locking and ownership
reference-count updates. A physical mapping is not authorization: incoming
requests still establish their own admission, and a changed native view clears
the mappings and borrowed binding references.

This is not zero overhead: fixed epoch entry/lifetime work and ordinary STO
synchronization/OCC remain. The engine guard and logical guard conservatively
retain the oldest nested epoch; no second authority clock, transaction registry
or per-key reader counter replaces true drain. Volatile pin state is not crash
recovery. The conditional durable lease/recovery library remains a separate
supported contract with its existing transaction and storage assumptions.

Native peer handshakes use version 4 for the epoch/pre-copy control contract;
gateway requests remain version 2 and bound scans version 3.
Upgrade communicating binaries together.
The persistent metadata deployment identity and journal encodings are unchanged;
opening retained metadata still does not restore rows or permit serving.

TPC-C routes its first eleven canonical tables by four-byte big-endian global
warehouse coordinates; row keys keep their existing encoding. The immutable
`item` table is explicitly replicated/static, not assigned a fictitious mutable
warehouse owner. Adopted local indexes are materialized before publication and
identified by physical owner as well as canonical table/coordinate. Same-process
cross-owner access still uses the real participant RPC and immutable admission path.

The old C++ policy/cache/migration graph, checksum cutover and write-only drain
are removed. There is no replicated `reshard/*` journal, live membership
change, automatic rebalance or primary-row/TP crash recovery. Configuration CRUD
does not perform migration.

The executable Rust actors and source histories construct a refinement of the
independent placement specification in
[`tla/mako`](../tla/mako/README.md#native-rust-correspondence). The native proofs
also cover injective byte values, full scan identity and forward/reverse range
coverage under transactional ordered-scan semantics. This is not a native
distributed transaction-history or combined Raft/migration theorem. Native
threading, wire/FFI dispatch, engine atomicity, canonical index binding and
callback lifetimes remain explicit trusted boundaries.

Separate sharding protocols establish per-authority crash reconstruction,
admission/membership fencing, conditional progress from primitive fairness,
and safe administrative compaction. The native production integration provides
the metadata-only mode below, not durable primary-row/TP recovery. Administrative
compaction does not collect participant journals, receipt dictionaries or
outstanding copy work.
See [the trusted interface contract](../tla/mako/README.md#trusted-recovery-and-progress-interfaces).
Run `./docker_build.sh ci nativeShardingProof` and
`./docker_build.sh ci nativeShardingSmoke` for the separate proof and real-engine
gates. Fresh Docker smoke covers retained guards through OCC retry/cleanup,
old-view first access after Frozen, idle helpers, colocated owners, source
mutation after private pre-copy, exact final absence, delayed-job fencing, and
metadata SIGKILL/reopen. Engine/RCU tests cover nested guards and retirement
behind newer readers. See the [checked results](../tla/mako/README.md#checked-results)
for proof scopes and commands; native weak-memory ordering and distributed OCC
remain explicit foreign boundaries.

#### Optional durable migration metadata

Enable metadata persistence on every native owner using a stable storage root.
For the **first initialization only**, set:

```bash
export MAKO_SHARD_METADATA_DIR=/var/lib/mako/sharding-metadata
export MAKO_SHARD_METADATA_INIT=1
```

Then start the owners using their normal native-sharding configuration
(`MAKO_CLUSTER_CONFIG=1`). Each owner uses `owner-N/participant`; owner 0 also
uses `owner-0/coordinator` in a multi-owner deployment. Initializing an existing
or partially initialized role directory is rejected rather than replacing it.
Without `MAKO_SHARD_METADATA_DIR`, the existing live-only mode remains available.
Peers must agree on metadata mode and catalog identity.

For a retained startup, keep the same directory and **unset**
`MAKO_SHARD_METADATA_INIT` (or set it to `0`). Missing/corrupt state, a different
catalog, or a competing writer causes failure; there is no genesis fallback.
The backend uses WAL-enabled synchronous RocksDB batches and retains an
exclusive writer lock across settlement/reopen. Uncertain persistence fences
the running candidate rather than acknowledging success.

**Retained startup is inspection-only.** The native admin service can return
historical nonce outcomes through Poll or an identical Begin retry, but readiness
and read admission return Retry. New Begin, Abort, capture,
control, copy, and cleanup are rejected. A retained pending migration stays
pending. The normal workload readiness wait also fails closed; a successful
historical admin reply does not mean the data is available.

Primary MBTA rows, ordinary TP outcomes, and transaction IDs have not acquired
restart recovery. Workload loaders are not a recovered post-migration image.
There is no force-resume, automatic wipe, or recovered-lease release operation.
Metadata histories remain append-only. The same restriction applies after a
clean shutdown. Each metadata-changing acknowledgement incurs synchronous
persistence; this mode adds write latency and storage growth.

The Docker smoke covers committed and held-lease-pending SIGKILL/reopen in both
process layouts, plus rejection of authentic old Start/Final/Commit controls at
a restarted destination. It does not establish power-loss durability; that
depends on RocksDB and the filesystem/device honoring synchronization.


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

Mako contains two replication backends. Raft is the supported, tested one, and
the default. Paxos is **deprecated**: still built, but no longer tested, and run
only when selected explicitly (`src/deptran/paxos/DEPRECATED.md`).

| Replication | Build Command | Binary | Use Case |
|-------------|---------------|--------|----------|
| **Raft** | `make mako-raft -j64` | `dbtest` | Mako with Raft replication (supported) |
| **Paxos** (deprecated) | `make -j32` | `dbtest` | Deprecated and untested; kept buildable |

### Multi-Paxos (deprecated)

The original replication engine, described here because the code is still in
the tree. It is deprecated and no test runs it. Multi-Paxos replicates as
follows:
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

Raft is the supported replication engine, with clearer leader election semantics. Build and test with:

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

### Transaction timestamp design

The single-machine transaction cache uses a hybrid logical clock, or HLC, to
name the serialization point of each committed write transaction represented
by a cache record. The target distributed design will use the same timestamp.
The clock combines approximate Unix time with a logical counter. Unix time
makes versions useful across processes and future machines. The logical counter
preserves order when the physical clock repeats, moves backward, or trails an
observed timestamp.

The physical part is a hint about when a commit happened. Correctness comes
from the HLC update rules and the transaction protocol, not from assuming that
two machine clocks agree perfectly.

#### Contract status and rollout boundary

This section specifies the target timestamp contract for all of Mako. The
implementation is landing in two stages. The current stage covers only the
single-machine transaction cache. The later stage converts the existing C++
distributed path.

The current single-machine stage implements:

- The 16-byte timestamp in the public local C ABI and the safe Rust API, with
  the complete operation and ownership rules in the
  [revision-1 ABI contract](reference/mako-local-abi-v1.md).
- A process-wide 63-bit HLC stamp, with the validation gate in the remaining
  bit of the same atomic word.
- Post-validation timestamp allocation and cache-record binding while Silo's
  write locks remain held.
- Cache record formats v5 and v6, Rust replay comparison, last-writer-wins
  filtering, and recovery flooring with the full timestamp.
- RDTSCP-based physical-time sampling when runtime checks accept the TSC, with
  a permanent `clock_gettime` fallback when they do not.
- A fixed origin of 1. That is valid only because this stage admits one local
  timestamp allocator and one recovered cache namespace per process.

Release candidate `e282a44b2` completed revision-1 verification. The
hook-enabled native suite passed 123 of 123 tests; the production-default
profile passed 93 of 94 and intentionally skipped its one hook-only test.
`mako-cache` passed 165 unit tests, 23 integration tests, and three doctests;
and `mako-history` passed 25 application tests plus 12 base transaction-oracle
tests. The native-backed and fake-ABI
`mako-local` suites, release Cargo check, strict fingerprint, symbol, C11, and
C++ conformance gates, canonical hook gate, ASan/LSan, strict TSan, UBSan, and
pinned Miri also passed. The combined timestamp mutation campaign killed all
12 mutants with zero survivors and zero harness errors. Its `85495f8dd`
ancestor passed the final health-hardening sweep's incremental cycle limits;
the release follow-up finalizes capability negotiation and contract wording
without changing the measured production write path. The separately failed
HLC-introduction performance result is preserved under an explicit
owner/date/scope waiver in the
[Milestone 1 acceptance record](mako-cache-milestone1-acceptance.md).

That record accepts the single-machine library, not a complete production
service. Volatile acknowledgement, `sync=false`, resident-value and retained-log
limits, one cache namespace per process, and the missing distributed and
network-service integrations remain explicit boundaries.

The current stage does not change distributed Mako's `uint32_t tid_unique_`,
its timestamp-allocation RPC, its replicated value and log trailers, or its
watermarks. That code still uses the legacy `timestamp * 10 + term` encoding.
Those values must not enter a v5 or v6 cache record, and a new HLC timestamp
must not be truncated for a legacy distributed call.

The distributed cutover is a later milestone with one coordinated format and
protocol break:

1. Each participant validates its local transaction and returns its greatest
   full HLC bound. The reply reports a bound; it does not allocate a commit
   timestamp.
2. After all validations succeed, the coordinator allocates exactly one
   timestamp strictly greater than every returned and locally observed bound.
   It sends that exact timestamp to every participant.
3. Distributed value metadata, replication records, replay records, failure
   history, and transaction watermarks move to the 16-byte timestamp. Failure
   epochs remain separate fields.
4. The replication callback returns status separately. Raft and Paxos stop
   decoding `timestamp * 10 + status`, and Mako removes every remaining
   `timestamp * 10 + term` commit representation.

Until that milestone lands, the global HLC rules below are the required target
for distributed work, not a claim about the old C++ wire format.

#### Keep the order domains separate

Several counters appear on the commit path. They have different jobs and must
not be converted into one another.

| Name | Scope | Purpose | Mako serialization order? |
|------|-------|---------|---------------------------|
| Silo/STO TID | In-memory record | OCC validation, lock state, and record-version checks | No |
| Mako commit timestamp | Cache commit record | Commit serialization, replay ordering, cache last-writer-wins filtering, and greatest-applied timestamp | Yes |
| `CacheSeq` | Cache log lane | Physical record identity, ingestion progress, and hole detection | No |
| Raft term and log index | Consensus group | Leader generation and replicated-log position | No |

A transaction ID is separate too. The future distributed format will use it to
identify retries and support message deduplication. It is not a commit
timestamp. Current local v5 and v6 cache records do not persist a transaction
ID; their retry rule is tied to the same physical log key and batch.

In particular:

- The Silo TID can contain lock and OCC flag bits. It never leaves the
  concurrency-control implementation.
- A `CacheSeq` increases densely within one lane. Across lanes, its numeric
  order has no commit meaning and can differ from Mako timestamp order because
  workers publish and replay asynchronously.
- RocksDB's internal sequence number describes RocksDB apply order. It does not
  describe Mako serialization order.
- Raft metadata remains in explicit `uint64_t term` and `uint64_t index`
  fields. Mako must not encode a term as `timestamp * 10 + term`.

#### Durable and C ABI representation

The target public C ABI, Rust type, cache log, replication record, and
persisted value metadata use the same 16-byte logical representation:

```c
typedef struct mako_timestamp_v1 {
    uint64_t physical_us;
    uint32_t logical;
    uint32_t origin;
} mako_timestamp_v1;
```

The fields have these meanings:

- `physical_us` is approximate Unix time in microseconds. The first allocator
  emits millisecond readings, so this value is initially a multiple of 1,000.
  Keeping microseconds in the format allows a finer synchronized clock later
  without another ABI or disk-format change.
- `logical` orders commits that share a physical reading or need to advance
  beyond an observed timestamp.
- `origin` is the nonzero ID of the timestamp allocator. The cluster manager
  leases origins. No two active allocators may hold the same origin, and an
  origin may not be reused while records bearing that origin remain live.

Compare timestamps as unsigned tuples in this exact order:

```text
(physical_us, logical, origin)
```

The wire and disk encoding is 16 bytes in big-endian field order. That makes a
bytewise comparison agree with the tuple comparison. Code must encode each
field explicitly. It must not copy the native C structure directly because
native padding and byte order are platform properties.

`{0, 0, 0}` is invalid, and a live timestamp must have a nonzero `origin`.
Every operation in one cache commit record carries the same timestamp. A
timestamp is immutable once its commit record becomes visible.

This format deliberately has no backward-compatibility path. The local cutover
uses cache record versions v5 and v6 and rejects v3, v4, and other legacy cache
records. Log GC also moves the private RocksDB keyspace from version 1 to
version 2, with a checksummed namespace marker, row envelopes, and lane metadata.
The native v5/v6 transaction bytes do not change. Opening an old or mixed
keyspace fails; deployment must rebuild it in a fresh database directory. The later
distributed cutover will likewise bump its replication-record and persisted
value formats and reject their old 32-bit timestamp layouts. There is no
legacy sentinel, decoder, or mixed-version comparison rule inside either new
format.

#### Hot clock representation

The first implementation keeps the local HLC and the general validation gate
in one 64-bit atomic word:

```text
63                         20 19                       1 0
+----------------------------+--------------------------+-+
| Unix milliseconds, 44 bits | logical counter, 19 bits |G|
+----------------------------+--------------------------+-+
```

The packed value is:

```text
(unix_ms << 20) | (logical << 1) | validation_gate
```

The bit budget provides:

- Unix-millisecond coverage until approximately year 2527.
- 524,288 logical values in one millisecond.
- A nominal capacity of 524 million timestamps per second before synthetic
  clock advancement.

The 19-bit counter is ample for the measured peak of about 35.8 million local
transactions per second. If it fills, the allocator increments the physical
millisecond and resets the counter. The HLC can therefore move ahead of wall
time, but it never wraps or waits for the wall clock to catch up.

`G` is not part of a timestamp. It is a short process-local certification lock
used by general transactions during final validation and timestamp binding.
Code masks it before unpacking or comparing clock values. No other status or
format flags belong in this word.

A general transaction sets `G` with compare-and-exchange and clears it with a
release `fetch_and` on the same atomic word. It advances the physical and
logical fields while preserving the set gate bit. Restricted allocators that
observe `G` wait or retry instead of allocating around the general transaction.
The release operation lets a later allocator observe the completed timestamp
and log binding. The gate is never serialized to a cache record.

The 16-byte timestamp does not need a 128-bit atomic operation. `origin` is
constant for the allocator's lifetime, and the hot physical and logical state
fits in the 63 timestamp bits of the existing 64-bit atomic.

#### Clock update rule

Each allocation starts with two inputs:

1. A physical candidate from the fast Unix clock.
2. The greatest timestamp that the transaction must follow, called its
   observed bound.

The observed bound includes commit timestamps attached to relevant data and
predicate observations, participant bounds for a distributed transaction, and
the allocator's last published timestamp. Silo TIDs are validated separately
and never compared with this bound. The allocator chooses the least
representable timestamp that is greater than the bound and no earlier than
physical time.

In the current local implementation, the allocator's prior HLC value is the
runtime bound. Recovery sets the allocator floor so the next timestamp is
strictly greater than the greatest surviving cache record before it admits
workers. The validation gate orders local dependencies. The local path does not
fetch a Mako timestamp from every Masstree row. Explicit participant and row
bounds belong to the later distributed protocol.

Conceptually, allocation performs this loop:

```text
wall_ms = fast_unix_time_ms()

repeat:
    old = atomic_clock.load()
    local = unpack_timestamp_bits(old)
    floor = max(local_timestamp(local, origin), observed_bound)

    next = first_local_tuple_at_or_after(wall_ms, floor)
    if next <= floor:
        next = next_representable_local_tuple_after(floor)

until atomic_clock.compare_exchange(old, pack(next, old.G)) succeeds
```

`next_representable_local_tuple_after` compares the full
`(physical_us, logical, origin)` tuple. It increments the logical field when
possible, carries to the next millisecond when the logical field cannot
represent the bound, and accounts for origin when the other fields are equal.
The real implementation checks arithmetic before every addition and
conversion. If the 44-bit physical field overflows, it stops admitting writes
instead of wrapping. CAS failure only means another transaction advanced the
clock. The allocator reloads and tries again.

Timestamp sequences are strictly increasing, but they need not be contiguous.
A reservation or bind rejection after allocation may leave an HLC gap.
Recovery and replay must never interpret that gap as a missing transaction.

#### Commit protocol

Timestamp order must agree with the order established by Silo locks and STO
validation. Some hot paths reserve capacity and preselect a reusable lane
generation or `CacheSeq` candidate before native commit. That candidate remains
invisible and reusable until native code accepts it into the commit order. A
local commit then follows this sequence:

1. Acquire the complete write set in STO's configured order. A failed try-lock
   aborts the transaction.
2. Enter the general validation gate if the transaction class requires it.
3. Validate the read set and all range predicates while the write locks remain
   held.
4. Use the process HLC as the current local bound. The future distributed path
   also collects participant and row timestamp bounds.
5. Allocate one HLC timestamp after validation succeeds.
6. Irrevocably bind or advance the physical `CacheSeq` only after timestamp
   allocation, then attach the timestamp and sequence to an exact cache-log
   obligation. General paths still hold the gate. A restricted path instead
   uses an atomic timestamp operation that refuses to cross a held gate.
   Depending on the hot path, binding transfers ownership of a reserved lane
   slot, arena extent, or holder whose final record bytes are fixed by the
   native write set.
7. Release the validation gate and finish the remaining preinstall ownership
   checks. Keep every record lock held.
8. Cross the native commit seam. From here, failure to finish the bound log
   obligation is a fail-stop condition, not an ordinary transaction abort.
9. Install every write in Masstree and release the record locks.
10. After native commit returns, seal any deferred record fields and publish
    the bound slot as ready before acknowledging the transaction.

Validation failure happens before allocation and does not create a log
obligation. The gate covers certification, timestamp allocation, and ordered
binding. It does not cover record-byte serialization, Masstree installation,
or ready publication. This keeps the serial section short. A rejection before
irrevocable `CacheSeq` binding can unwind and leave only an unused HLC gap. Once
a nonzero sequence is accepted or advanced, any ownership, installation, or
publication uncertainty pins that exact obligation and makes the cache
unhealthy. The sequence must not be cancelled or reused. Out-of-order ready
publication is safe because replay compares Mako timestamps.

The validation gate closes a race that record locks alone cannot close. For
example, one transaction can read `A` and write `B` while another writes `A`.
Final validation and timestamp binding must act as one certification step or
the timestamp-ordered replay history can disagree with the accepted Silo
history. Restricted transactions may avoid owning the gate only when their
record locks and validation rules establish the same ordering guarantee. They
must still observe the gate according to the STO fast-path protocol.

The cache log owns the transaction's normalized write set. C++ code that owns
the Silo locks also constructs this write set, including same-key overwrite and
delete semantics. Rust accepts and replays that record. It must not reconstruct
transaction semantics by making another round of C ABI calls after commit.

#### Multi-shard allocation

A multi-shard transaction uses one commit timestamp on every participant. Each
participant locks and validates its local work, then returns the greatest
timestamp it observed. The coordinator computes:

```text
commit_timestamp = next_after(max(participant_bounds))
```

`next_after` applies the same HLC carry rule as the local allocator. It always
returns a timestamp strictly greater than the maximum, even when the
coordinator's physical clock is behind it. The coordinator sends that exact
timestamp to every participant.

Participants keep conflicting locks until the outcome is fixed. Before making
the commit visible, each participant merges the chosen timestamp into its
local HLC. Later local commits therefore sort after the distributed commit.
An abort publishes no commit record. Coordinator identity, Raft term, prepare
position, and retry identity remain separate fields.

#### RDTSCP as the physical clock source

RDTSCP makes the physical read cheap. It is an input to the HLC, not the source
of uniqueness or serialization correctness.

At startup, the clock code brackets a Unix-time anchor with `CLOCK_REALTIME`
samples and measures TSC progress against `CLOCK_MONOTONIC_RAW` for 5
milliseconds. It records the Unix-time base, TSC base, and a fixed-point
conversion from cycles to nanoseconds. The fast path then computes:

```text
unix_ns = base_unix_ns + scale(rdtscp() - base_tsc)
```

It rounds this result down to milliseconds for the initial allocator. Each
thread periodically compares the estimate with `CLOCK_REALTIME`. If any worker
measures drift beyond the accepted limit, it atomically disables RDTSCP for
the process. Every subsequent read uses the fallback clock. The current
implementation does not promise live recalibration. NTP or PTP can improve
the Unix-time base in a future distributed deployment.

The RDTSCP path is enabled only when CPUID reports RDTSCP and an invariant TSC,
and does not report that the process is running under a hypervisor. A worker
tracks `TSC_AUX`. When it changes, the worker takes an immediate second RDTSCP
sample. Another change during that retry disables RDTSCP. A stable new value is
accepted only after the estimated time passes a realtime drift check.
`TSC_AUX` is an opaque migration indicator, not a portable CPU ID. It is never
encoded into a Mako timestamp. Unsupported hardware, excessive measured drift,
TSC discontinuity, or an untrusted virtual-machine clock switches the process
permanently to `clock_gettime(CLOCK_REALTIME)`.

Neither RDTSCP nor `CLOCK_REALTIME` has to be monotonic. The HLC CAS clamps a
backward reading to the last published value and advances `logical`. Locks and
atomics provide memory ordering for the commit protocol, so correctness does
not depend on treating RDTSCP as a full memory fence.

Clock synchronization alone does not provide external consistency. If Mako
later promises that timestamp order follows real-time order across machines,
the clock service must also publish a maximum error bound, reject excessive
skew, and perform a commit wait when necessary. The timestamp format already
has enough physical precision for that protocol.

#### Failure rules

The allocator fails closed. These cases have explicit outcomes:

| Condition | Required response |
|-----------|-------------------|
| Wall clock moves backward | Keep the current physical field and increment `logical` |
| Logical counter fills | Advance one synthetic millisecond and reset `logical` |
| TSC is unavailable or untrusted | Read Unix time with `clock_gettime` |
| TSC calibration or drift check fails | Reject the sample, retain the HLC floor, and permanently fall back |
| CAS loses to another allocator | Reload the word and retry |
| Origin lease is lost | Stop admitting new writes |
| Physical field is exhausted | Return a clock-exhausted error and stop admitting writes |
| Reservation or hook rejection before irrevocable `CacheSeq` binding | Abort and allow a harmless HLC gap |
| Failure after a nonzero `CacheSeq` is accepted or advanced | Pin that exact obligation and mark the cache unhealthy |
| Timestamp or record encoding is malformed | Stop recovery with a corruption error |

No fallback may reset the clock, reuse zero, truncate a field, or silently
reuse an origin.

#### Recovery and origin management

The current local stage recovers each lane's permanent maximum HLC from its
checkpoint metadata and validates the surviving rows and retained logs against
it. It sets the process clock floor above the greatest lane maximum before
accepting writes, even if every log has been reclaimed and every key deleted.
The local stage uses fixed origin 1 and has no origin lease service.

The target distributed recovery protocol first obtains a nonzero origin lease,
then initializes the hot HLC so its first result is greater than both the
recovered maximum and the current physical candidate. It must not reset the
HLC to wall time when the wall clock is behind recovered data.

An origin lease is an exclusive writer credential. Losing it stops new
timestamp allocation. Startup also refuses to write if another live process
holds the same origin or if the lease service cannot prove exclusivity. A new
process incarnation normally receives a new origin, but recovery still floors
the HLC because physical clocks can move backward.

Only records covered by the configured durability contract take part in
recovery. The timestamp scheme does not make an asynchronous, unflushed record
durable. If the current mode acknowledges before disk synchronization, a crash
can lose those acknowledged records until a stronger durability mode is added.

Recovery treats these conditions as corruption or configuration errors:

- A timestamp whose `origin` is zero. The physical and logical fields may
  independently be zero.
- A malformed 16-byte encoding.
- Two distinct commit records with the same full timestamp.
- A non-increasing timestamp within one tagged, single-producer worker lane.
- A legacy record format.

#### RocksDB replay rules

The asynchronous replay path may receive records in a different order from
their Mako timestamps. In the current single-machine cache, every commit-log
record present in RocksDB carries the full timestamp. A process-wide apply
coordinator keeps an in-memory per-key winner index and writes a materialized
mutation only when the incoming timestamp is newer than the indexed winner.
Versioned rows and tombstones preserve that index across restart. Recovery
streams those entries and validates the retained suffix of each lane before
loading current values into a private Silo table. It rejects conflicting
timestamp/source identities among surviving rows and logs. Repeating the exact
same physical RocksDB batch is idempotent.

A returned RocksDB error cannot prove whether the atomic batch took effect.
The coordinator therefore treats every `write_batch` error as an ambiguous
outcome. It retains the exact operation bytes and target lane metadata and
requires that batch to retry before another apply or GC batch. Only a
successful return updates the in-memory winner index, retires queue records,
or advances the applied watermark. Repeating the same log keys and
timestamp-filtered row operations makes both the “not applied” and “already
applied” outcomes safe. In panic-unwind builds the same rule covers a caught
backend panic; the workspace release profile uses `panic = "abort"` and relies
on process recovery instead.

Each materialized RocksDB row now contains its winning full HLC, source lane
and local sequence, and either user bytes or a tombstone. CRC32C protects these
envelopes regardless of the chosen native log-checksum policy. The backend is private to the cache: ordinary
callers open it with `Db::open` and cannot obtain a backend handle or inject a
shared backend. External fault tests use the explicitly enabled `test-support`
feature. Production applications must leave that feature disabled.

The exclusive apply coordinator serializes the timestamp comparison and atomic
row/metadata batch without a RocksDB merge operator. A
second backend writer would require a separate conditional-update protocol.

One transaction's operations enter RocksDB in one atomic write batch. A higher
`CacheSeq` or RocksDB sequence number does not make a value newer. Those
sequences only report physical ingestion and backend application progress.

RocksDB user-defined timestamps are not a drop-in replacement for this check.
They impose ordering constraints between an application timestamp and
RocksDB's sequence order for each key. Mako intentionally permits out-of-order
background application. It therefore keeps explicit timestamp metadata in its
materialized row envelopes and retained transaction records.

The system also keeps its ordering and progress values distinct:

- Each cache lane uses `CacheSeq` and explicit hole tracking for ingestion.
  In concurrent mode the public acknowledged and applied sequence values are
  aggregate record counts, not physical `CacheSeq` values.
- `AppliedWatermark` also carries the greatest applied Mako timestamp. Mako
  timestamps order commit records and resolve asynchronous cache
  materialization winners.
- Current live Masstree visibility and conflicts still use Silo row versions.
  The later distributed design will use Mako timestamps for its transaction
  watermarks.

Code must never cast one watermark into the other. A historical snapshot at a
Mako timestamp requires retained versions or a proven materialization
watermark. RocksDB's latest internal sequence number alone is insufficient.

#### Application-log reclamation

The [log-GC design and implementation plan](plans/mako-cache-log-gc.md) is now
implemented and undergoing release validation. It reclaims Mako transaction
records stored under private RocksDB keys. In-memory queue records still remain
until successful application; RocksDB manages its own WAL and SST file lifetime.

Each atomic apply batch stores the winning row/tombstone envelopes, full
transaction records, and permanent lane metadata together:

| Lane field | Meaning |
| --- | --- |
| `A` | Largest contiguous local sequence applied to RocksDB |
| `G` | Largest contiguous local sequence whose log has been reclaimed |
| `H` | Maximum applied HLC, including records whose mutations were superseded |
| Retained bytes | Sum of the remaining log key and value lengths |

Exactly the log suffix `(G, A]` remains. Recovery resumes the lane after `A`,
recovers the applied count as `sum(A)`, and establishes the HLC floor from
`max(H)`. These values remain valid when `G = A` and no log keys remain.
Recovery validates format checksums, coverage, source identities, and retained
mutations, then loads current Put values in bounded native transactions. It
does not replay lifetime transaction history or claim to audit deleted history.

`CacheOptions::log_retention` defaults to 300 seconds and `gc_interval` defaults
to 10 seconds. Both settings remain fixed while the cache is open. Zero
retention is supported; zero interval and unrepresentable durations are rejected.
A monotonic timer schedules sweeps, which sample Unix time once and delete only
records with `physical_us < now_us.saturating_sub(retention_us)`. Records exactly
at the boundary remain. A backward clock jump delays collection; a forward jump
makes more applied records eligible.

The background writer visits each persisted lane, including idle lanes, and
reclaims its consecutive expired prefix in atomic batches of at most 1,024 log
deletes and 1 MiB of delete-key bytes. It updates `G` and retained bytes in the
same batch, preserves `A` and `H`, and interleaves batches with application until
the sweep has no eligible prefixes left. It stops at the first unexpired log
even in the untagged stream, whose historical timestamps may run backward.
RocksDB reads each bounded prefix with one iterator seek followed by sequential
steps, rather than a separate point lookup for every expired record. The
iterator stops at the batch limit or the first unexpired record and is released
before the atomic deletion batch.
An expired transaction still waiting in memory remains untouched until its own
row/log/metadata batch succeeds.

Uncertain GC errors retain identical retry bytes and block later backend
updates. The ordinary writeback retry rules and bounded queue backpressure
still apply. The adapter explicitly selects ordered writes and point-in-time
WAL recovery. GC-enabled `Db::open` rejects WAL-disabled durability. ACK remains
volatile with WAL enabled and `sync=false`; GC adds no WAL sync, memtable flush,
or foreground compaction.

`CacheStatus::log_gc` reports retained/reclaimed counts and bytes, the observed
lane-head timestamp, GC errors, and pending or stalled attempts. Its oldest timestamp is a best
known lane-head minimum, so unscanned lanes or reversed untagged timestamps
limit that observation. `disk_usage_bytes()` separately reads database file
sizes and may perform filesystem I/O; it is not part of the nonblocking health
snapshot. File sizes include WAL, SST, and metadata files, and do not measure
allocated disk blocks. Compaction determines when deleted bytes leave SST files.

Five-minute retention is not a fixed byte bound. Write rate, payload size,
backlog, and compaction lag determine storage use. Tombstones and their winner
timestamps remain so delayed old writes cannot resurrect deleted keys. Live
values still reside in Silo. Value eviction and tombstone reclamation remain
separate work. Release acceptance, including sustained runs with boost disabled,
is tracked in the linked plan.

#### Single-machine service lifecycle

The cache is a library component, not yet a production network server. A host
must complete `Db::open` and recovery before it opens a listener or reports
ready. Startup failure leaves the service unready. The legacy distributed C++
server cannot host this cache unchanged: it owns a different native table
facade and uses transient request threads, while the local ABI requires one
fixed set of long-lived STO workers. A standalone Rust host is the next
integration step. Connecting this cache to distributed routing, 2PC, and
replication remains a later milestone.

While serving, the host polls `Db::status()`. A returned error is itself an
unhealthy host condition; the host must not treat a failed status read as
`Healthy`. Cache status is only one readiness input. The host must combine it
with `pool.metrics()` and require `healthy_workers > 0`:
`quarantined_workers` is a process-wide informational count, may include
workers outside this cache, and does not affect `CacheHealth`. A successful
snapshot is interpreted as follows:

- `Healthy` is a necessary cache-local admission signal, not proof that the
  surrounding service, network, dependencies, or durability policy is ready.
- `Degraded` means that the writer is retrying a record or backend batch, or
  that a backend call exceeded `backend_stall_threshold` (30 seconds by
  default). The host should
  alert and shed or stop admission before the bounded writeback queue fills.
- `Unhealthy` means that the writer stopped or the cache latched a fail-stop
  error. The host must stop write admission immediately.

The status counters are cumulative diagnostics. An active retry or stalled
backend call determines current degradation. Operators should inspect
`pending_backend_retry`, `active_retryable_failures`, and
`backend_write_in_progress`; a pure backend stall may have no `last_failure`,
while `last_failure` intentionally remains populated after recovery and may be
historical. Operators should also track the difference between
`acknowledged_transactions` and `applied_watermark.sequence()` together with
`queued_transactions`. Those values show volatile writeback backlog, not
disk-sync progress. A live RocksDB `write_batch` call cannot safely be
cancelled in process. If it hangs, status remains readable and exposes its
monotonic age, but an external supervisor must enforce a shutdown deadline and
terminate the process if the call never returns. Such termination can lose the
acknowledged, unapplied tail under this milestone's durability contract.

In panic-unwind builds, the cache converts a backend panic into an ambiguous
batch failure and retains the exact batch for retry; it also records a caught
outer runtime panic. The workspace release profile deliberately uses
`panic = "abort"` for the Rust/static-library boundary. A production panic
therefore terminates the process before those counters can update, and the
same supervisor/recovery policy applies. Normal RocksDB error returns retain
and retry the exact batch in every profile.

Clean shutdown has a strict order:

1. Stop admission and close the listener.
2. Join all request workers and release every shared `Arc<Db>` clone.
3. Call the owning `Db::close()` and treat an error as a failed shutdown.

A successful `close()` drains every acknowledged lane snapshot and joins the
writer. It does not add a RocksDB flush or WAL sync. The default `Wal` mode
still uses `sync=false`. If the synchronous retry budget is exhausted, close
returns an error after consuming the instance; a RocksDB call that never
returns can block close. An error or supervisor timeout is a failed shutdown
and requires restart/recovery under the volatile-tail contract. `Drop` is
best-effort cleanup and is not the service lifecycle protocol. Deployments
must use a bounded pool of long-lived workers because native worker
registrations are process-lifetime and capped. They must also keep exactly one
cache namespace and one exclusive backend/keyspace in each process.

#### Required invariants

The implementation and review checklist uses these invariants:

1. Every committed cache write represented by a commit-log record has exactly
   one nonzero timestamp.
2. Tuple comparison defines the same total order in C++, Rust, log decoding,
   recovery, and RocksDB replay.
3. One allocator never emits a timestamp less than or equal to its previous
   timestamp.
4. When the protocol supplies dependency or remote bounds, the transaction
   receives a timestamp greater than every bound it observed.
5. If Silo serializes two conflicting commits as `A` before `B`, then
   `timestamp(A) < timestamp(B)`.
6. No transaction installs writes without first binding its timestamp to an
   exact cache-log obligation. A committed transaction is not acknowledged
   until that obligation is published as ready.
7. All operations in one local cache record use one timestamp. The future
   distributed record also carries one separate transaction ID for the whole
   transaction.
8. Physical log order and RocksDB sequence order never decide logical
   last-writer-wins behavior.
9. Recovery sets the HLC floor so the next allocation is greater than every
   record that it can restore.
10. Clock regression, counter carry, restart, and CPU migration cannot cause a
    duplicate or decreasing timestamp.

#### Verification status and plan

The revision-1 functional run passed the native, Rust cache, native-backed and
fake-ABI local-wrapper, history-oracle, release-check, fingerprint, symbol, and
C/C++ conformance suites listed in the rollout status above. The
single-machine implementation keeps these correctness requirements:

- C++ and Rust golden-vector tests for exact 16-byte big-endian encoding,
  tuple comparison, zero-origin rejection, and all field boundaries.
- A mock-clock test that holds one millisecond constant for more than 524,288
  allocations and verifies carry into the next synthetic millisecond.
- Tests for backward wall-clock jumps, physical-field overflow, CAS retries,
  logical overflow, and a recovered timestamp far ahead of wall time.
- A multithreaded uniqueness test that exercises the validation gate and checks
  strict per-allocator monotonicity under contention.
- Conflict tests for write/write, read/write, range-predicate, insert/delete,
  and repeated modification of one key in a transaction.
- Application-history checks that compare Silo's accepted serialization order
  with timestamp-sorted cache replay, including histories whose physical lane
  order is the reverse of their HLC order.
- RDTSCP capability selection and fixed-point-overflow tests. Runtime code
  resamples a changed `TSC_AUX` and accepts a stable migration only after a
  realtime drift check. Any repeated `TSC_AUX` instability, backward or
  discontinuous TSC input, or failed drift validation causes permanent
  fallback to `clock_gettime`.
- Replay tests that permute log-lane order, retry the same physical batch and
  log key, reject a duplicate timestamp under another `CacheSeq`, and mix puts
  with tombstones while producing the same final RocksDB contents.
- Crash-injection tests before timestamp allocation, after allocation, before
  log publication, after publication, during Masstree install, and during
  asynchronous RocksDB application.

The completed worker-count benchmark sweep used CPU boost disabled.
Deterministic fault injection for raw TSC changes, `TSC_AUX` migration, and
drift remains clock hardening work for a later service release. Distributed
clock-skew tests belong to the later cutover. Neither is a claim made by the
single-machine landing gate.

The combined timestamp mutation campaign killed all 12 mutants, with zero
survivors and zero harness errors. The first run killed 11 and exposed a weak
oracle. After
`recovery_advances_mako_timestamp_past_the_recovered_maximum` began using a
future but representable HLC, the focused rerun killed
`missing-recovery-clock-floor`. The source-integrity check matched before and
after the campaign.

The revision-1 HLC-introduction performance gate failed. On `zoo-002`, three
complete paired repetitions of the concurrent cache write-ACK path compared `a71dba682` with
its immediate parent `e22d937a1` at 1, 4, 8, 16, 24, and 32 workers. CPU boost
was disabled, RocksDB WAL remained enabled with `sync=false`, and the primary
metric was per-thread phase cycles per commit. The paired cycle regressions
were respectively 6.65%, 8.68%, 2.80%, 4.14%, 0.83%, and -1.34%. The maximum
regression exceeded the 5% cell limit, and the 3.57% geometric-mean regression
exceeded the 3% aggregate limit. The new path consistently added about 128 to
130 instructions per commit.

The planned five repetitions could not finish because the host's snap LXD
daemon entered a persistent restart loop. A lifecycle-aware follow-up probe
showed that the original two-snapshot interference screen missed substantial
short-lived LXD work. The three complete repetitions are therefore sufficient
to reject this candidate, but not to accept a future one. Recorded arrays, build
hashes, protocol details, and qualifications are in the
[machine-readable HLC comparison](benchmarks/mako-cache-hlc-ab-zoo002-20260907.json),
SHA-256 `924574abf93fbcdc3430301a440835ff9a768fdf012c76433a374acbc96794d5`.
This remains a failed measurement. Shuai Mu waived only that incremental HLC
cost for the Milestone 1 single-machine library on 2026-09-07 because the HLC
is required for stable replay and the planned distributed timestamp protocol.
The waiver requires a clean-host repeat before distributed or network-service
production cutover and does not cover correctness, safety, or durability.

The final hardening comparison then ran five accepted repetitions of candidate
`85495f8dd` against HLC baseline `a71dba682` at 1, 4, 8, 16, 24, and 32
workers. Its maximum paired cycle regression was 3.10% and its geometric mean
was 1.80%, within the predeclared 5% and 3% limits; instructions per transaction
were effectively unchanged. Release follow-up `e282a44b2` does not alter that
production write path. Full arrays and validation metadata are in the
[machine-readable health comparison](benchmarks/mako-cache-health-ab-zoo002-20260907.json).
Its SHA-256 is
`d405824c2bab58cbdd7ffa228496920c7ebacde1f27cb419d0b5195f3decc1da`.
The canonical all-in-one hook gate also passed. These results accept the scoped
single-machine library and do not widen the boundaries above.

The later distributed cutover adds these required tests before the global HLC
contract is complete:

- Multi-shard tests that shuffle participant replies and prove the coordinator
  always chooses `next_after(max(bounds))` and every participant merges it.
- Origin-lease tests for restart, lease loss, duplicate active origins, and
  exhausted or unavailable origin allocation.
- Wire-format, replication, replay, failure-history, and watermark tests that
  reject legacy 32-bit or `timestamp * 10 + term` data rather than mixing it
  with HLC records.

[CockroachDB's HLC](https://github.com/cockroachdb/cockroach/blob/master/pkg/util/hlc/hlc.go),
[MongoDB's seconds-plus-ordinal timestamp](https://www.mongodb.com/docs/manual/reference/bson-types/#timestamps),
and [TiDB's physical-plus-logical TSO](https://docs.pingcap.com/tidb/stable/tso/)
use the same broad separation between physical time and logical order.
[RocksDB](https://github.com/facebook/rocksdb/wiki/User-defined-Timestamp)
likewise keeps application timestamps separate from its internal sequence
number. Mako follows those ideas but keeps its Silo version, cache position,
and consensus position as explicit independent types.

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
- `mbta_ordered_index` — STO/MassTrans transactional table; each non-txn op
  is an internal one-op OCC transaction (Encode/strip handled internally).
- `mbta_sharded_ordered_index` — per-key routing; remote keys travel
  self-contained non-txn RPCs, and writes on a replicated leader
  reach the replication log through the normal commit path.

Design and semantics: [`storage-interface.md`](storage-interface.md).

---

## 7. Networking and RPC

### Transport Backend

Mako has a single RPC backend:

| Feature | srpc/rpc |
|---------|---------|
| Latency | ~10-50 us (TCP/IP) |
| Hardware | Standard Ethernet |
| Portability | Any platform |
| Use case | Dev, testing, cloud, production |

```bash
./build/dbtest config/tpcc.yml
```

Worker threads never see the transport: they reach requests through the
`TransportRequestHandle` interface, implemented by `SrpcRequestHandle`.

```cpp
class TransportRequestHandle {
    virtual uint8_t GetRequestType() const = 0;
    virtual char* GetRequestBuffer() = 0;
    virtual char* GetResponseBuffer() = 0;
    virtual void EnqueueResponse(size_t msg_size) = 0;
};
```

### srpc/rpc Reliability Features

The srpc/rpc backend includes production-grade reliability:
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
| Default | `make -j32` | Default build (~2-3 min); contains Raft and the deprecated Paxos |
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
| `build/deptran_server` | Raft lab harness (`make raft-test` only) |
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
./docker_build.sh ci all          # Run every suite
./docker_build.sh ci <testname>   # Run specific test
```

### Test Suite

| Test | Description |
|------|-------------|
| `srpcTests` | SRPC framework unit tests |
| `simpleTransaction` | Basic transaction execution |
| `shardNoReplication` | 2 shards, no replication |
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

static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::RAFT};

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
| Raft leader churn | Increase `MAKO_RAFT_HEARTBEAT_INTERVAL_US` |
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
| **SRPC** | "Simple RPC" - Mako's custom RPC/fiber framework |
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
