# Mako RocksDB-Compatible Interface

This document describes Mako's RocksDB-compatible `ITable` and `IDatabase` interfaces, located in `src/rocks_interface/idb.hh`. It covers three things:

- **Conceptual model** — how Mako's STO/MassTrans/Masstree building blocks map to RocksDB's (`DB`, `ColumnFamily`, `Transaction`, `Snapshot`, `Iterator`, persistence, …), what aligns, and where the two systems irreconcilably diverge.
- **Interface reference** — the abstract `ITable` and `IDatabase` methods, their semantics, and worked examples.
- **RocksDB migration & extensibility** — a `RocksDB API → Mako` translation table, a compatibility feasibility matrix, and a roadmap for what extending the interface further would unlock.

Read the conceptual section first if you're deciding *whether* Mako's interface fits your use case; jump to the interface reference if you're wiring code against it.

> **Backend terminology:** Current Mako transactions use STO `Transaction`
> with MassTrans/Masstree through `mbta_wrapper`. Older revisions of this
> document used “Silo” as shorthand for that lineage. The original Silo
> `transaction_base`/`dbtuple`/`txn_btree` engine is retired and guarded
> against compilation. Literal `SiloRuntime` references below remain current:
> it is live allocator, RCU, and Masstree runtime support for STO/MassTrans.

## Direct RocksDB service profile

`MAKO_BUILD_PROFILE=MAKO` remains the default described by the C++ interface
reference below. `MAKO_BUILD_PROFILE=ROCKSDB_RAFT` selects a separate native Rust
service: reused Rust STO concurrency control, direct RocksDB committed storage,
and Raft replication, synchronous by default. It does **not** instantiate `IDatabase`, C++ STO,
MassTrans, Masstree, `SiloRuntime`, or a Mako value cache.

This profile preserves the RocksDB-like transaction programming model, not the
RocksDB C++ ABI. Its Rust client is `rocksdb_raft_service::client::Client`; its
standalone C boundary is `src/rocks_interface/rocksdb_raft.h`, linked against
`librocksdb_raft_service.so`. The C header includes no Mako engine headers.

### Build and launch

Run builds and qualification through Docker:

```bash
./docker_build.sh build-rocksdb-raft
./docker_build.sh ci rocksdbRaft
```

Inside the Docker development environment, generate and run a local cluster:

```bash
python3 scripts/rocksdb_raft/cluster.py \
  --bin-dir build_rocksdb_raft --data-dir /tmp/rocksdb-cluster \
  --replicas 3 --shards 2 --server-workers 40
```

The data directory must be empty. The helper emits `node1.json` through
`node3.json`, a separate connection-only `client.json`, and the chosen endpoints.
`--generate-only` writes configuration without launching servers. The running
helper creates `alpha`, `beta`, and `bench`; generate-only mode declares their
routes but does not create persisted tables. The two-shard layout splits each
at byte `m`. A one-member group is useful for measuring local durability, not
for fault tolerance.

Explicit first creation is
`rocksdb_raft_server --config node1.json --bootstrap`. Restart the same node
without `--bootstrap`; missing Raft authority is an error, not a fresh cluster.
Keep node/group IDs, peer membership and the routing catalog unchanged. A
`raft.service-routing.json` sibling of each Raft directory binds that identity;
it is outside the store's atomically created directory. This profile does not
implement online placement migration or membership changes.

`rocksdb_raft_client --config client.json --stdio` accepts one request JSON
object per line. Byte arrays preserve binary keys/values. Unlike the CLI, the
C `mako_rocksdb_raft_request` entrypoint accepts a versioned envelope:
`{"version":1,"request":{...}}`. Free every returned C buffer exactly once with
`mako_rocksdb_raft_buffer_free`, and close the client after its calls finish.

Inside Docker, `cmake --install build_rocksdb_raft --prefix /tmp/rocksdb-install`
installs the three executables under `bin`, the client library under `lib`,
and the standalone header as `include/mako/rocksdb_raft.h`. An installed C
consumer includes `<mako/rocksdb_raft.h>` and links `-lrocksdb_raft_service`;
set its runtime library search path to the installation's `lib` directory.
The installed CLI and a real C consumer are covered by the qualification evidence.

### Durability selection

The default is `durability: "sync"` in server JSON. For the explicit weak
persistence experiment, set the top-level field to `"async"` on **every**
replica, or pass `--durability async` to `scripts/rocksdb_raft/cluster.py`.
Status reports both `durability` and `power_crash_durable`; async startup emits
a warning.

- **Sync:** acknowledged writes require durable quorum and synchronous atomic
  user-DB application.
- **Async:** user RocksDB WAL remains enabled with `sync=false`; Raft writes
  still occur, but foreground log/file/directory persistence barriers are
  omitted. Commit still waits for quorum ordering and local atomic application,
  **not power-crash durability**. Acknowledged data and consensus state may be
  lost after machine/power failure. This is not a production durable quorum.
- OS writeback and RocksDB background flush/compaction perform persistence.
  There is no configured periodic WAL-sync deadline or bounded loss window.
  RocksDB-internal startup/background syncs and explicit user-DB checkpoint/
  generation maintenance are not globally suppressed.
- The async Raft base enables its own WAL with `sync=false` so nonwaiting base
  flushes and Raft-segment reclamation remain process-crash recoverable. The
  default sync base keeps its existing separate-Raft-WAL protection.
- Modes have distinct persistent identities; reopening or installing a
  checkpoint under the opposite mode is rejected. Use fresh directories for
  comparisons. Raft wire modes also differ, so a sync group cannot count async
  peer acknowledgments as durable votes/replication.

`rocksdb_raft_bench` and `scripts/rocksdb_raft/performance.py` accept the same
`--durability sync|async` selection. Native `OptimisticTransactionDB`, local STO,
and replicated application use the selected WAL policy; the remote benchmark
checks the service's reported mode before running. The matrix records the
selection, guarantee, actual server configs and commands. Compare both policies
with the same binary, dataset and concurrency, in separate data directories.

`smoke.py --durability async` exercises transaction semantics and process
restart/failover, **not** power-loss durability. CI runs both policies; the Raft
store suite separately models loss of unsynchronized data on power failure.

### Transaction and failure contract

The durability guarantees below describe the default **sync** policy. Async
keeps transaction isolation, ordering and error handling but has the weaker
persistence contract above.

- Begin selects **one shard-local user DB** and establishes a committed
  current-term Raft barrier before capturing a RocksDB snapshot: a fresh
  correlated quorum round plus local application catch-up, not a new durable
  log entry per Begin. Operations reuse that snapshot; they do not acquire a
  fresh authority barrier per Get.
- Get, Exists, Put, Insert, Delete, bounded Scan/ReverseScan, Commit and Rollback
  operate through Rust STO items. Multiple logical tables in that DB can commit
  atomically. Tables are byte namespaces, not separately configured native
  column families.
- Every read is tracked, including absence. Scans use a conservative
  database-wide change generation; unrelated writes can conflict with scans.
  RocksDB's own block cache remains enabled; there is no separate committed-value
  mirror or delayed cache-to-RocksDB writeback.
- A key, range, or explicit DB outside the selected shard poisons the **whole**
  transaction. No partial writes are published, even for co-located shards.
- Write success requires quorum commitment and local durable application of one
  atomic batch containing rows, catalog/count/version metadata, outcome, and
  applied-prefix receipt. Followers replay the decided command, not the client
  body or old OCC validation. Read-only completion has no user write command or
  durable write-outcome record.
- A stable `(client, sequence)` request identity resolves uncertain write
  outcomes. `unknown_outcome`, lost replies, or storage failure after possible
  submission do **not** authorize replaying the transaction body or claiming a
  rollback. Query Outcome with the same identity; an absent outcome is not proof
  of abort while the accepted log suffix remains unresolved.
- Storage/application failures fence admission. Restart reconciles the user DB
  against durable Raft authority before serving; it never truncates an
  acknowledged user state to make recovery appear successful.

### Performance matrix

Inside Docker, the harness starts fresh clusters and databases itself; do not
leave another benchmark or qualification run competing for the storage device:

```bash
python3 scripts/rocksdb_raft/performance.py \
  --bin-dir build_rocksdb_raft --data-dir /tmp/rocksdb-performance \
  --modes native,sto,replicated --replicas 1,3 --shards 1,2 --threads 8,16 \
  --workloads point,disjoint,hot,multikey,scan \
  --seconds 10 --dataset-keys 1024 --output /tmp/rocksdb-performance.json
```

`native` uses real native `OptimisticTransactionDB`; `sto` uses Rust STO and
ordered durable RocksDB application without Raft/network; `replicated` uses the
actual service. Data preload is outside the measured interval. All writing
commits retain synchronous WAL durability. The service uses production timeout
defaults, not the shorter failure-injection fixture deadlines: 10-second commit/
read-barrier waits, 12-second client requests and 30-second idle transactions.
Native Raft uses 40-ms heartbeats, one-second durable peer RPCs and randomized
three-to-six-second election intervals. Those peer timings include storage
latency; they are not just network round-trip budgets.

The service workload uses two client processes with 8 or 16 threads each.
A single local DB cannot have two writer processes, so the corresponding
one-shard local references use one process with 16 or 32 threads; two-shard local
references use two independent DB processes. Server worker counts, replica
counts and this process-layout difference are reported separately.
Native scans track returned keys but do not certify range phantoms as Rust STO
does. All test transactions are shard-local; this is not unchanged TPC-C.
The native reference also calls synchronous `Commit` for read-only transactions;
this RocksDB version syncs an empty WAL batch, unlike the STO read-only fast
path. Do not interpret their read-only throughput ratio as CPU-only overhead.

Both references use strict WAL recovery and ordered writes. Native OCC requires
its own retained memtable conflict history (128 MiB with the qualified RocksDB
build); the raw STO database does not enable that extra history. Persisted
OPTIONS capture this engine-specific difference rather than claiming identical
native internals.

The JSON output contains throughput, conflicts/unknowns/errors, latency
histograms, per-process CPU/RSS/IO, actual RocksDB options and storage growth.
Client Begin/operation/Commit timings are wall time, not internal CPU attribution.
The design records the complete 80-case matrices on the
[HDD host](plans/rocksdb-raft-mode.md#complete-installed-product-matrix) and
[local SSD host](plans/rocksdb-raft-mode.md#local-ssd-rerun), including their
hardware and comparison limitations. The separate
[CPU attribution](plans/rocksdb-raft-mode.md#cpu-attribution-and-interpretation)
and [durability traces](plans/rocksdb-raft-mode.md#measured-durability-cost)
were measured on the HDD host, not the SSD.

The [paired sync/async SSD comparison](plans/rocksdb-raft-mode.md#matched-sync-versus-async-ssd-comparison)
uses the same binaries and includes native RocksDB with `sync=false`, local STO,
and one/three replicas. It records the remaining gap and shared-host/short-run
limits; async results are not power-crash-durable throughput.

See the [implementation design and evaluation](plans/rocksdb-raft-mode.md) for
source provenance, retained-attempt lifetime, checkpoint ordering and measurements.
The remaining sections describe the unchanged default Mako facade.

---

## Conceptual Model

### Vocabulary at a glance

| RocksDB concept | Mako STO/MassTrans analogue | Aligned? |
|---|---|---|
| `DB` (a directory of SSTables + WAL) | `SiloRuntime` (per-shard runtime) | Partially — see §1 |
| `ColumnFamily` (namespace within a DB) | `abstract_ordered_index` / `mbtree` instance | Yes — see §2 |
| `Key` (`Slice`, arbitrary bytes) | `varkey` / `lcdf::Str` (byte string) | Yes |
| `Value` (`Slice`, arbitrary bytes) | Encoded strings held by MassTrans values | Largely aligned — see §7 |
| `Transaction` | Mako's `abstract_db::new_txn` returning `void*` handle | Yes for OCC — see §3 |
| `OptimisticTransactionDB` (OCC) | STO OCC with optional opacity checking | Yes — see §3 |
| `TransactionDB` (pessimistic 2PL) | No equivalent | No |
| `Snapshot` (explicit `SetSnapshot`) | Implicit txn-start snapshot only | Diverges — see §4 |
| `WriteBatch` (atomic multi-key write) | Implicit within a single txn | Yes — see §3 |
| `Iterator` (stateful, snapshot-pinned) | `Scan` callback (single pass, per-node consistency only) | Diverges — see §8 |
| `MergeOperator` | No equivalent | No |
| `CompactionFilter` | No equivalent | No |
| `Comparator` (customizable key ordering) | Bytewise lex only (Masstree layered trie) | Limited |
| `WAL` / `SST files` / `LSM` (persistence architecture) | Independent RocksDB-backed WAL layer (`RocksDBPersistence`) | Fundamentally different — see §5 |
| `BackupEngine`, `Checkpoint` | Masstree `checkpoint.hh` + `kvio.hh` exist but are not wired into `mbtree` | Absent from user-facing API |

### 1. Storage container: `DB` vs `SiloRuntime`

**RocksDB `DB`.** A `DB` is a directory on disk containing SSTables + a WAL. One process opens as many `DB` instances as it wants, each with its own path, WAL, block cache, and set of Column Families. `DB::Open("/tmp/foo")` and `DB::Open("/tmp/bar")` are entirely independent.

**Mako `SiloRuntime`.** Declared in `src/mako/silo_runtime.h:38-244`. A runtime bundles the resources for one "site" or shard: its own `MasstreeContext` (epoch counter, threadinfo list), per-site core-id allocator, per-site memory allocator, per-site ticker, per-site RCU system. Multiple runtimes can coexist in one process via `SiloRuntime::Create()`; threads bind to exactly one via `SiloRuntime::BindCurrentThread(runtime)`.

**Mapping.** A `SiloRuntime` is closer to a RocksDB `DB` than to a shared allocator or a global. **One process can host N independent `SiloRuntime`s**, each with its own tables, mirroring the multi-`DB` RocksDB pattern. The current `mako::DB` in `db.hh` is a facade over exactly one `SiloRuntime`; a hypothetical `mako::DB::Open("path_a")` + `mako::DB::Open("path_b")` would each internally create their own runtime.

The one meaningful mismatch: RocksDB's `DB` corresponds to a persistence directory. Mako's `SiloRuntime` corresponds to an in-memory shard. The `name` argument to `DB::Open` in RocksDB names the filesystem path; in `mako::DB::Open` it names logical shard identity (or is ignored for in-memory-only builds).

### 2. Namespaces: `ColumnFamily` vs table / index

**RocksDB `ColumnFamily`.** A CF is a logical KV namespace within a single `DB`. All CFs in one DB share the WAL and block cache, but their key spaces are disjoint. Every put/get/delete/iterator takes a `ColumnFamilyHandle*`. A DB has at minimum one CF (`default`).

**STO/MassTrans tables.** Each table is a separate `abstract_ordered_index`
(`src/mako/storage/abstract_ordered_index.h`), instantiated by the production
wrapper as `mbta_ordered_index` over `MassTrans`. The historical
`typed_txn_btree` implementation belongs to the retired original Silo engine.
TPC-C creates approximately ten primary and secondary table instances.

**Mapping.** RocksDB CF ↔ Mako table ↔ MassTrans/Masstree instance, all natural fits. `IDatabase::GetTable(name)` already returns an `ITable*` per name — this is functionally equivalent to `DB::CreateColumnFamily` / `DB::GetColumnFamilyHandle`.

**Divergences to note**:

- **Shared WAL semantics don't apply**: RocksDB CFs share a WAL, so cross-CF atomic writes are cheap. Mako has no per-runtime WAL; atomicity comes from the txn layer instead. Effect: multi-table atomicity in Mako uses the transaction, not a `WriteBatch`.
- **CF creation is dynamic in RocksDB, mostly static in Mako**: RocksDB lets you `CreateColumnFamily` at runtime. Mako's benchmarks open all tables at startup. `GetTable(name)` currently creates on demand — this is more RocksDB-like than the benchmarks suggest.
- **Options per CF**: RocksDB supports per-CF options (block size, compression, comparator). Mako has no per-table options — all masstree instances behave identically.

### 3. Transaction semantics

**RocksDB.** Two flavors: `OptimisticTransactionDB` (OCC) and `TransactionDB`
(pessimistic locking). Snapshot reads require explicit snapshot selection;
`GetForUpdate()` tracks point reads for optimistic validation, whereas ordinary
Get and iterators do not provide complete serializable range certification.
Optimistic `Prepare()` is unsupported by the qualified RocksDB 8.9 library;
prepared-transaction support belongs to configured `TransactionDB` modes.

**STO/MassTrans.** OCC with optional opacity checking. `abstract_db::new_txn(flags, arena, buf)` returns a `void*` handle; ops are staged in a per-txn item set (`tset_`, `src/mako/sto/Transaction.hh:571-577`), validated at `commit_txn(txn)` returning `bool`. Aborts throw `abstract_abort_exception` or set an error flag; retry is caller-driven (no automatic loop). Isolation is **serializable** via read-set validation at commit — stricter than RocksDB's default snapshot isolation. No 2PC surface.

**Mapping**:

| Feature | RocksDB (`OptimisticTransactionDB`) | STO/MassTrans | Notes |
|---|---|---|---|
| `BeginTransaction` | `db->BeginTransaction(...)` → `Transaction*` | `db->BeginTransaction()` → `void*` | Present |
| `Put/Get/Delete` in txn | `txn->Put(cf, k, v)` | `table->Put(txn, k, v)` | Present; API shape flipped (Mako passes `void* txn` first) |
| `GetForUpdate` | `txn->GetForUpdate(...)` | No equivalent | STO OCC doesn't distinguish — every read is tracked implicitly |
| Isolation level | Snapshot (default) or serializable | Always serializable | Mako is stricter |
| Read-your-writes | Yes | Yes (`find_item()` in `Transaction.hh:603-639`) | Aligned |
| Commit | `txn->Commit()` returning `Status` | `db->commit_txn(txn)` returning `bool` | Aligned semantics; different signatures |
| Rollback | `txn->Rollback()` | `db->abort_txn(txn)` | Aligned |
| Retry on conflict | Caller loop | Caller loop | Aligned |
| 2PC (`Prepare`) | Not supported by the optimistic engine | Not supported | Neither optimistic surface exposes prepared transactions |
| `SetName` (named txn for recovery) | Not supported by the optimistic engine | Not supported | Prepared/named recovery is a different native engine mode |

**No pessimistic flavor**. RocksDB's `TransactionDB` (2PL) has no counterpart. Any consumer requiring lock-based blocking semantics can't be supported without a fresh implementation.

**Non-transactional access.** Mako's `abstract_ordered_index` also exposes a **non-transactional API** mirroring Masstree's operation set — `get / put / insert / remove / scan / rscan` without a txn handle, each op per-key atomic on its own (internally a one-op OCC transaction with retry; `remove` is a direct raw write). This is the analog of RocksDB's plain `db->Put/Get/Delete` outside any `Transaction`. See [`storage-interface.md`](storage-interface.md) for the full contract, including the constraint that these must not be called from a thread with an open transaction.

### 4. Snapshots

**RocksDB.** `Snapshot* s = db->GetSnapshot()` captures a global sequence number. Reads with `ReadOptions{.snapshot=s}` see the committed state as of that seq. `ReleaseSnapshot(s)` decrements a refcount. Snapshots pin resources (SSTables can't be compacted away). Explicit inside a txn via `txn->SetSnapshot()`.

**STO/MassTrans.** No explicit snapshot handle. A transaction implicitly reads at its start-tid; the "snapshot" is what the txn observes across the lifetime of its ops. There's no way to hand a snapshot to a different code path or hold one open past commit.

**Mapping.** RocksDB's snapshot API is `Status::NotSupported` territory for Mako. Any RocksDB code that does `s = db->GetSnapshot(); ... use s ...; ReleaseSnapshot(s);` outside a transaction cannot be directly ported. Two workaround patterns:

- Wrap the "using the snapshot" region in a Mako transaction (`BeginTransaction` → do reads → `Commit`).
- Read at higher isolation via GetForUpdate-equivalent — but STO has no such distinction.

Callers relying on cross-txn snapshot handles (e.g., long-running analytical queries against a snapshot fixed at some past time) don't have a natural mapping.

### 5. Persistence architecture

The most fundamental divergence. Worth stating clearly:

**RocksDB's persistence is intrinsic**. `db->Put(...)` writes to memtable + WAL; on memtable flush, immutable SSTables land on disk; compaction merges them. `sync=true` fsyncs the WAL. Recovery replays the WAL from the last checkpoint. Persistence is not a separate layer — it *is* the storage engine.

**Mako/Masstree persistence is external and optional**. Masstree is a pure in-memory index; its `checkpoint.hh` (`ckstate::visit_value`) and `kvio.hh` (`kvout` msgpack serializer) exist as infrastructure but are **not wired into `mbtree`**. Durability in Mako is provided by an entirely separate component: `src/mako/rocksdb_persistence.h` uses RocksDB as a partitioned write-ahead log, and `src/deptran/raft/rocksdb_log_storage.hpp` uses RocksDB for Raft's consensus log. These are Mako's persistence layers; masstree itself doesn't participate.

**Mapping implications**:

- `Options.write_buffer_size`, `Options.max_write_buffer_number`, all LSM tuning: N/A. Absorbed silently by an options struct, unused.
- `WriteOptions.sync = true`: cannot be honored by the in-memory path. Either accept silently (misleading) or return `Status::NotSupported`.
- `WriteOptions.disableWAL`: N/A (no WAL to disable).
- `db->Flush()`, `db->CompactRange()`, `db->GetProperty("rocksdb.stats")`: N/A.
- `db->NewCheckpoint(...)` / `BackupEngine`: could conceptually be built on `ckstate` + `kvio` — real work if pursued, but the primitives exist.

**Practical guidance**: don't try to make the compat layer look persistent. The right model is "RocksDB API shape, in-memory backing, durability via the separate `RocksDBPersistence` WAL if needed at a higher layer."

### 6. Concurrency & epoch model

**RocksDB.** Multiple threads share a `DB`; internal locking. Global monotonic sequence number ordered by write time. Snapshots reference a seq.

**Mako/STO.** Multiple threads share a `SiloRuntime` after `BindCurrentThread`. **Epoch-based advancement**: the runtime's ticker advances a global epoch ~every 100µs (`Transaction.cc:122`). Epochs are used for **RCU deferred reclamation only** — not durability, not commit visibility. `txn commit tid` is drawn from a per-thread counter combined with the current epoch.

**Mapping.** Not directly observable from RocksDB's API — mostly internal. But two visible knock-on effects:

- Deletes in Mako become tombstoned tuples reclaimed at some later epoch; a caller who deletes then quickly reads may still find the value visible until epoch advance (though the txn layer masks this).
- No monotonic global sequence number to expose as `SequenceNumber` in a RocksDB-shaped API. Any consumer relying on RocksDB's sequence numbers for external ordering can't be served.

### 7. Values: opaque bytes vs typed rows

**RocksDB.** Values are opaque `Slice`s. Applications serialize their own structs to bytes. `PinnableSlice` avoids copying on read.

**STO/MassTrans.** Values are encoded strings stored by MassTrans. TPC-C still
uses schema-generated record types in application code, then encodes those
records before staging them through `mbta_ordered_index`.

**Mapping.** The `ITable` interface (below) takes `std::string` for both key and
value, using `mako::Encode()` where the Mako storage format requires it. The
compatibility layer's opaque-byte model is preserved end-to-end.

Encoding and wrapper copies can add overhead relative to RocksDB's direct
application-to-memtable path; measure that cost for latency-sensitive uses.

### 8. Iteration model

**RocksDB.** `Iterator* it = db->NewIterator(ro, cf);` returns a stateful, snapshot-pinned cursor. Methods: `SeekToFirst`, `SeekToLast`, `Seek(key)`, `SeekForPrev(key)`, `Next`, `Prev`, `Valid`, `key`, `value`, `status`. `~Iterator` releases resources. The iterator observes a consistent snapshot regardless of concurrent writes.

**MassTrans/Masstree.** No stateful iterator. `mbtree::search_range_call(lower, upper, callback)` is push-based: the caller provides a callback invoked for each key until it returns false. `ITable::Scan` and `ITable::ReverseScan` follow this shape. Consistency: Masstree's normal per-node version-check retry; **not** a pinned snapshot across the whole scan.

**Mapping**:

- Callback-based scan is functional and already in `ITable`. Consumers who can restructure code to callbacks can use it directly.
- Consumers who need a *stateful* pull-based iterator (`for (it->SeekToFirst(); it->Valid(); it->Next())`) need an adapter. Two designs:
  - **Chunked materialization**: on `SeekToFirst`/`Seek`, run `search_range_call` collecting up to N pairs into a buffer; on `Next` past the buffer, refill from the next key onward. Bounded memory. Loses point-in-time consistency across chunks unless run inside a transaction.
  - **Transaction-scoped iterator**: an iterator opened inside an STO transaction naturally gets serialisable consistency across its lifetime, because all its reads are tracked in the txn's read-set and validated together at commit.
- Snapshot-pinned iterators outside a transaction (RocksDB's default) have no clean Mako mapping.

### 9. Secondary indexes

**RocksDB.** No first-class secondary index; applications maintain them manually as auxiliary CFs.

**Mako.** Same — no automatic secondary indexes. TPC-C's `customer_name_idx` (`tpcc.h:80-87`) is a separate MassTrans/Masstree instance keyed by `(warehouse, district, last_name, first_name) → customer_id`, updated by the application whenever the primary `customer` table is updated.

**Mapping.** Both systems put secondary index maintenance on the application. No compat gap.

### 10. Extension points RocksDB has that Mako doesn't

Three RocksDB features are architecturally absent from Mako/Masstree:

- **`MergeOperator`**: RocksDB supports read-modify-write with user-defined associative merge functions (`db->Merge(key, delta)`). At read time, RocksDB replays all pending merges. Mako has no equivalent — a Mako-side merge would require the application to Get → modify → Put inside a transaction.
- **`CompactionFilter`**: RocksDB invokes user callbacks during compaction to filter or transform values (e.g., TTL-based expiry). Mako has no background compaction, so nothing to hook into.
- **Custom `Comparator`**: RocksDB accepts a custom key comparator per CF; the default is bytewise lex. Masstree's trie-of-B+trees hardcodes bytewise lex order internally; there's no way to swap in a custom comparator without rewriting Masstree.

None of these have workarounds. `NotSupported` is the honest answer for all three.

---

## Interface Summary

### ITable

| Method | What it does | What it lacks | Why this implementation |
|--------|-------------|---------------|------------------------|
| `Put` | Write a key-value pair | No blind-write shortcut; value must be `mako::Encode()`'d | |
| `Get` | Read a value by key | No bloom-filter hint or short-circuit path | |
| `Delete` | Remove a key | No range delete | |
| `GetName` | Return the table name | — | |
| `Scan` | Forward range scan [start, end); returns `NotSupported` if `num_shards > 1` | No stateful iterator; cross-shard not yet implemented | Reverted to local-shard after discussion with Shuai; cross-shard support will be built on top of his upcoming changes. Multi-shard scan is non-trivial because keys are hash-distributed, not range-distributed — results across shards have no global ordering guarantee |
| `ReverseScan` | Reverse range scan descending; returns `NotSupported` if `num_shards > 1` | No stateful iterator; cross-shard not yet implemented | Same as Scan |
| `Exists` | Check key presence without reading value | Does a full Get internally; no bloom-filter hint | Chosen over a separate existence flag to reuse the OCC read-set tracking already done by Get |
| `Insert` | Insert only if key absent | Aborts transaction on duplicate | Uses `transInsert` instead of `transPut` — non-obvious distinction; `transInsert` registers the key in the OCC write-set so a concurrent insert on the same key causes abort rather than silent overwrite |
| `GetApproximateSize` | Approximate key count for the local shard | Local shard only; count may be stale | Counter updated under lock in `install()` (commit phase) rather than atomics in the hot path, as reviewer noted atomics are too expensive for an approximate metric |

### ITable non-transactional methods (2026-07)

`ITable` also carries a non-transactional surface (no `void* txn`
parameter; each op is a self-contained, immediately-visible operation
— internally a one-op OCC transaction on the owning shard, so writes
replicate through the normal commit path). Semantics: `Put` = blind
overwrite (OK); `Insert` = put-if-absent (`InvalidArgument` if
present); `Delete` = real remove (`NotFound` if absent); `Get` = OK /
`NotFound`; `Exists` = OK + flag. Values are raw bytes in both
directions (the backend applies its storage encoding internally,
unlike the transactional methods, which require caller-Encoded
values). Defaults
return `NotSupported`; `LocalTable` implements them over the L3
non-txn API and `RemoteTable` over the self-contained non-txn request
types (14-17). Callers must not have an open transaction on the
calling thread. See
[`storage-interface.md`](storage-interface.md).

The `RemoteDB` KV path was re-based onto this machinery: the previous
implementation "wrote" via `shard_put` — staging + locking a 2PC
participant write that nothing ever committed (never visible, never
replicated, lock leaked) — and "deleted" via an empty-value put. Both
decoupled-client server paths (the raw-struct handlers and
`MakoClientService`) now run `ShardReceiver::RunNontxnOp`.
`RemoteDB::ConnectNontxn(host, port)` + `GetTable(name, table_id)`
give a client that interoperates with `ClientTcpServer` end-to-end
(the srpc-protocol txn'd client still has no matching live server —
pre-existing).

### IDatabase

| Method | What it does | What it lacks | Why this implementation |
|--------|-------------|---------------|------------------------|
| `BeginTransaction` | Start a transaction, return opaque handle | No isolation-level choice | |
| `Commit` | Commit all writes in the transaction | No auto-retry on OCC abort | |
| `Rollback` | Discard all writes since `BeginTransaction` | No savepoints | |
| `GetTable` | Get or create a table proxy by name | Remote table must already exist on the server | |
| `ListTables` | List names of tables opened in this session | Only reflects tables opened via `GetTable`; not a full schema query | Remote DB has no name→table mapping, so listing all tables is not possible; local DB returns only opened tables for the same interface consistency |
| `Connect` / `Disconnect` / `IsConnected` | Connection lifecycle management | No-op for local DB | Exists on `IDatabase` so the same client code works for both local and remote without branching |
| `InitThread` | Initialize per-thread database context | Required for local DB; no-op for `RemoteDB` | |

---

## ITable API

The `ITable` interface provides key-value table operations within a transaction context.

### Put

```cpp
virtual Status Put(void* txn, const std::string& key, const std::string& value) = 0;
```

Write a key-value pair. The value **must** be encoded with `mako::Encode(value)` before passing.

**Returns:** `Status::OK()` on success.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string encoded = mako::Encode("hello");
Status s = table->Put(txn, "mykey", encoded);
db->Commit(txn);
```

### Get

```cpp
virtual Status Get(void* txn, const std::string& key, std::string& value) = 0;
```

Read a value by key. The returned value has `EXTRA_BITS_FOR_VALUE` metadata stripped automatically.

**Returns:** `Status::OK()` on success, `Status::NotFound()` if the key does not exist.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string value;
Status s = table->Get(txn, "mykey", value);
if (s.ok()) { /* use value */ }
db->Commit(txn);
```

### Delete

```cpp
virtual Status Delete(void* txn, const std::string& key) = 0;
```

Remove a key-value pair from the table.

**Returns:** `Status::OK()` on success.

**Example:**
```cpp
void* txn = db->BeginTransaction();
Status s = table->Delete(txn, "mykey");
db->Commit(txn);
```

### GetName

```cpp
virtual const std::string& GetName() const = 0;
```

Return the table name.

**Example:**
```cpp
std::cout << "Table: " << table->GetName() << std::endl;
```

### Scan

```cpp
virtual Status Scan(void* txn,
                    const std::string& start_key,
                    const std::string* end_key,
                    std::function<bool(const std::string& key, const std::string& value)> callback) = 0;
```

Forward range scan. Iterates keys from `start_key` (inclusive) up to `end_key` (exclusive, or until end of table if `end_key` is `nullptr`). The callback receives each key-value pair and returns `true` to continue or `false` to stop early.

Values passed to the callback are already stripped of internal metadata — no decoding needed.

**Current limitation:** Only works in single-shard deployments. Returns `Status::NotSupported` if more than one shard is present. Cross-shard scan is not yet implemented — it will be built on top of Shuai's upcoming changes. Note that when implemented, cross-shard scan cannot guarantee globally ordered results because keys are hash-distributed across shards, not range-distributed.

**Returns:** `Status::OK()` on success, `Status::NotSupported()` in multi-shard mode, `Status::InvalidArgument()` for an invalid range or null callback, `Status::IOError()` if the transaction was aborted.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string end = "scan_key_040";
std::vector<std::string> keys;
Status s = table->Scan(txn, "scan_key_020", &end,
    [&](const std::string& key, const std::string& value) -> bool {
        keys.push_back(key);
        return true;  // continue
    });
db->Commit(txn);
// keys contains scan_key_020 .. scan_key_039 in order
```

To scan to the end of the table:
```cpp
table->Scan(txn, "prefix_", nullptr, callback);
```

### ReverseScan

```cpp
virtual Status ReverseScan(void* txn,
                           const std::string& start_key,
                           const std::string* end_key,
                           std::function<bool(const std::string& key, const std::string& value)> callback) = 0;
```

Reverse range scan. Iterates keys from `start_key` (inclusive) down to `end_key` (exclusive), delivering results in descending key order. Callback semantics are identical to `Scan`.

**Current limitation:** Same as `Scan` — returns `Status::NotSupported` in multi-shard deployments.

**Returns:** `Status::OK()` on success, `Status::NotSupported()` in multi-shard mode, `Status::InvalidArgument()` for an invalid range or null callback, `Status::IOError()` if the transaction was aborted.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string end = "scan_key_020";
std::vector<std::string> keys;
Status s = table->ReverseScan(txn, "scan_key_039", &end,
    [&](const std::string& key, const std::string& value) -> bool {
        keys.push_back(key);
        return true;
    });
db->Commit(txn);
// keys contains scan_key_039, scan_key_038, ..., scan_key_020 in descending order
```

### Exists

```cpp
virtual Status Exists(void* txn, const std::string& key, bool* exists) = 0;
```

Check whether a key exists without reading its value (avoids unnecessary data copy). Sets `*exists = true` if found, `*exists = false` if not found.

**Returns:** `Status::OK()` even when key is absent (the result is in `*exists`). Returns an error status only if an unexpected error occurs.

**Example:**
```cpp
void* txn = db->BeginTransaction();
bool exists = false;
Status s = table->Exists(txn, "mykey", &exists);
if (s.ok() && exists) { /* key is present */ }
db->Commit(txn);
```

### Insert

```cpp
virtual Status Insert(void* txn, const std::string& key, const std::string& value) = 0;
```

Insert a key only if it does not already exist (Put-if-not-exists semantics). The value **must** be encoded with `mako::Encode(value)` before passing.

Uses the native `transInsert` path (not `transPut`), providing correct insert OCC semantics. Duplicate detection is performed via a Get check before the insert. Aborted concurrent transactions that overlap with this key may cause OCC-level retries.

**Returns:** `Status::OK()` if the key was inserted. `Status::InvalidArgument("Key already exists")` if the key is already present.

**Example:**
```cpp
void* txn = db->BeginTransaction();
std::string encoded = mako::Encode("initial_value");
Status s = table->Insert(txn, "newkey", encoded);
if (s.IsInvalidArgument()) {
    // key already exists — handle conflict
}
db->Commit(txn);
```

### GetApproximateSize

```cpp
virtual Status GetApproximateSize(size_t* size) = 0;
```

Return an approximate count of keys in the **local shard** of this table. Does not require a transaction handle.

The count is tracked via a plain `size_t` updated under lock in the commit phase (`install()`). Only reflects data in the local shard — for a cluster-wide count, a remote RPC scan would be needed (not yet implemented).

**Returns:** `Status::OK()` with `*size` set to the approximate local key count.

**Example:**
```cpp
size_t sz = 0;
Status s = table->GetApproximateSize(&sz);
if (s.ok()) {
    printf("Approximate size: %zu\n", sz);
}
```

---

## IDatabase API

The `IDatabase` interface manages transactions, table access, and connection lifecycle.

### BeginTransaction

```cpp
virtual void* BeginTransaction() = 0;
```

Begin a new transaction. Returns an opaque transaction handle to pass to table operations.

**Returns:** Transaction handle (opaque pointer), `nullptr` on failure.

### Commit

```cpp
virtual void Commit(void* txn) = 0;
```

Commit a transaction, making all writes durable and visible.

### Rollback

```cpp
virtual void Rollback(void* txn) = 0;
```

Abort a transaction, discarding all writes made since `BeginTransaction`.

### GetTable

```cpp
virtual ITable* GetTable(const std::string& name) = 0;
```

Retrieve (or create) a table by name. The returned pointer is owned by the database and valid for the lifetime of the `IDatabase` instance.

**Returns:** Pointer to `ITable`, `nullptr` on failure.

### ListTables

```cpp
virtual std::vector<std::string> ListTables() = 0;
```

Return the names of all tables currently tracked by the database (i.e., all tables previously accessed via `GetTable`). Tables that exist in the underlying store but have never been opened in this session will not appear.

**Returns:** Vector of table name strings.

**Example:**
```cpp
db->GetTable("users");
db->GetTable("orders");
db->GetTable("products");
std::vector<std::string> names = db->ListTables();
// names contains {"users", "orders", "products"} (order unspecified)
```

### Connect / Disconnect / IsConnected

```cpp
virtual Status Connect() { return Status::OK(); }
virtual void Disconnect() {}
virtual bool IsConnected() const { return true; }
```

Connection lifecycle methods. For local `mako::DB` these are no-ops (always connected). For `mako::RemoteDB` these establish/close the RPC connection.

### InitThread

```cpp
virtual void InitThread() {}
```

Initialize the current thread for database operations. Required for leader nodes before performing transactions. No-op for follower/learner nodes and remote DB.

---

## Usage Examples

### Basic CRUD

```cpp
#include "mako/mako.hh"
#include "mako/db.hh"

mako::DB* db = nullptr;
mako::Options opts;
opts.num_threads = 1;
mako::DB::Open(opts, "/tmp/mako_db", &db);

db->InitThread();
ITable* tbl = db->GetTable("my_table");

// Write
void* txn = db->BeginTransaction();
std::string enc = mako::Encode("world");
tbl->Put(txn, "hello", enc);
db->Commit(txn);

// Read
txn = db->BeginTransaction();
std::string val;
tbl->Get(txn, "hello", val);
db->Commit(txn);

delete db;
```

### Forward Scan

```cpp
void* txn = db->BeginTransaction();
std::string end_key = "key_050";
std::vector<std::pair<std::string,std::string>> results;
tbl->Scan(txn, "key_000", &end_key,
    [&](const std::string& k, const std::string& v) -> bool {
        results.emplace_back(k, v);
        return true;
    });
db->Commit(txn);
```

### Reverse Scan

```cpp
void* txn = db->BeginTransaction();
std::string end_key = "key_000";
std::vector<std::string> keys_desc;
tbl->ReverseScan(txn, "key_049", &end_key,
    [&](const std::string& k, const std::string&) -> bool {
        keys_desc.push_back(k);
        return true;
    });
db->Commit(txn);
```

### Exists Check

```cpp
void* txn = db->BeginTransaction();
bool exists = false;
tbl->Exists(txn, "some_key", &exists);
db->Commit(txn);
if (exists) { /* ... */ }
```

### Conditional Insert

```cpp
void* txn = db->BeginTransaction();
std::string enc = mako::Encode("new_value");
Status s = tbl->Insert(txn, "unique_key", enc);
if (s.IsInvalidArgument()) {
    db->Rollback(txn);  // key existed, abort
} else {
    db->Commit(txn);
}
```

### Approximate Size

```cpp
size_t sz = 0;
tbl->GetApproximateSize(&sz);
printf("~%zu keys in table\n", sz);
```

### ListTables

```cpp
auto names = db->ListTables();
for (const auto& n : names) {
    printf("  table: %s\n", n.c_str());
}
```

---

## Current consumers

Three programs currently use the ITable / IDatabase interface:

| Consumer | Role | Notes |
|---|---|---|
| `examples/simpleTransactionRep.cc` | Database server + transaction test with colocated / server-only / remote-client modes | The interface's flagship consumer — exercised by Docker CI (`shard1ReplicationSimpleRaft`, `shard2ReplicationSimpleRaft`). Proves the "same client code runs against `mako::DB` or `mako::RemoteDB`" claim by using `IDatabase*` polymorphically. |
| `examples/makoCon.cc` | Redis-compatible server built on top of `mako::DB`; MULTI/EXEC wraps in one transaction | Proof-of-concept for building higher-level protocol layers on the interface. Not currently in CI. |
| `examples/rocksdbInterfaceTest.cc` | Integration test for `Scan` / `ReverseScan` / `Exists` / `Insert` / `GetApproximateSize` / `ListTables` | Safety-net regression suite for the six ops added in PR #60. Not currently in CI. |

No internal Mako subsystem consumes `IDatabase`. The Mako runtime (TPC-C and
`dbtest`) uses `mbta_wrapper`/`mbta_ordered_index` and the abstract ordered-index
interface directly. The original `txn_proto2_impl`/`typed_txn_btree` path is
retired. RocksDB-backed producers call the real RocksDB C API directly.

The compat interface is deliberately an **external-consumer surface** — its purpose is to let code that thinks in RocksDB terms compile and run against Mako, whether that's a client program (`simpleTransactionRep`), a higher-level protocol shim (`makoCon`), or an eventual downstream project.

---

## Compatibility Summary Matrix

Feasibility assessment for every notable RocksDB feature, from the conceptual analysis above:

| RocksDB feature | Mako support today | Feasibility to add | Notes |
|---|---|---|---|
| Multiple DBs | Via multiple `SiloRuntime`s | Already possible; `mako::DB::Open` chooses one | Trivial extension of factory |
| Column families | Via `GetTable(name)` returning per-name `ITable` | Already there | Naming semantics differ |
| Put/Get/Delete | Yes | — | Present |
| MultiGet | No | Straightforward as batch of Get in one txn | Easy |
| WriteBatch (atomic) | No, use txn | Straightforward as internal single-op txn | Easy |
| Range delete | No | Feasible via `Scan` + per-key delete | Medium (perf concern) |
| Iterator (stateful, pull-based) | No, only callback Scan | Chunked-materialisation adapter | Medium |
| Snapshot outside txn | No | No clean mapping | Hard/impossible |
| OCC transactions | Yes | Extension: expose STO-style `Transaction` object with `Put/Get/GetForUpdate/Commit/Rollback` | Medium; largely already there |
| Pessimistic 2PL transactions | No | Would require new locking layer over masstree | Hard |
| 2PC (`Prepare`) | No | Would require durability + coordinator support | Hard |
| MergeOperator | No | No equivalent | Impossible without RMW loop |
| CompactionFilter | No | No compaction to hook | Impossible |
| Custom Comparator | No | Masstree hardcodes bytewise | Impossible without Masstree rewrite |
| Bloom filter, prefix seek | No | Masstree's trie structure IS prefix-optimised natively; bloom N/A for in-memory | Trivial to expose "prefix seek" as callback semantics |
| Persistence (WAL, SSTables) | Via separate `RocksDBPersistence` layer | Would replace Masstree's whole storage engine | Out of scope |
| Backup / Checkpoint | Infrastructure exists (`checkpoint.hh`, `kvio.hh`), unwired | Feasible if wanted | Medium project |
| Options tuning | Absorb-and-ignore | Options struct exists conceptually; most fields N/A | Cosmetic |
| `Flush`, `CompactRange` | No | N/A | `NotSupported` |
| `GetProperty("rocksdb.stats")` | Minimal | Could expose masstree counters | Cosmetic |

---

## RocksDB → Mako Migration Guide

Method-level mapping for translating RocksDB code to Mako's interface:

| RocksDB | Mako | Notes |
|---------|------|-------|
| `DB::Open(opts, path, &db)` | `mako::DB::Open(opts, path, &db)` | Options struct differs; no column families |
| `db->Put(wo, key, value)` | `table->Put(txn, key, mako::Encode(value))` | Encode value; use txn handle |
| `db->Get(ro, key, &value)` | `table->Get(txn, key, value)` | Value is decoded automatically |
| `db->Delete(wo, key)` | `table->Delete(txn, key)` | Needs txn |
| `db->NewIterator(ro)` → seek/next | `table->Scan(txn, start, end, cb)` | Replace stateful iterator with callback |
| Reverse iterator | `table->ReverseScan(txn, start, end, cb)` | Callback-based |
| `db->KeyMayExist(...)` | `table->Exists(txn, key, &exists)` | Exact check, not bloom filter hint |
| `db->Merge(wo, key, value)` | Not available | No merge operators |
| `db->GetSnapshot()` | Not available | No snapshot isolation |
| `db->GetColumnFamilyHandle(name)` | `db->GetTable(name)` | Returns `ITable*` |
| `db->DefaultColumnFamily()` | `db->GetTable("default")` or any name | |
| `db->ListColumnFamilies(...)` | `db->ListTables()` | Only lists opened tables |
| `WriteBatch` | Multiple `Put`/`Delete` in one `BeginTransaction/Commit` | |
| `db->Close()` | `delete db` (destructor calls Close) | |

---

## Implications for Extending the Interface

The existing interface is already at "80% of the common-case surface." What extensions move the needle:

**High value, low-to-medium effort**:
- **WriteBatch as a single-txn wrapper**: `makoCon.cc` already does this manually — its Redis `MULTI/EXEC` path calls `BeginTransaction` → op → op → `Commit`. Exposing an explicit `WriteBatch` lets it drop that custom wrapper and lets any RocksDB code using `WriteBatch` compile against `mako::DB` unchanged.
- **MultiGet as batched read within one implicit txn**: `makoCon.cc`'s Redis `MGET` currently issues N individual `Get` calls, each in its own transaction. A native `MultiGet` gives it snapshot-consistent multi-key reads with less overhead. Same win for any consumer doing bulk lookups.
- **Stateful iterator adapter**: no current consumer needs this. Useful when porting RocksDB code that uses `for (it->SeekToFirst(); it->Valid(); it->Next())`. Worth building only when a specific porting target motivates it.
- **Explicit `Transaction` class** (`OptimisticTransactionDB`-shaped): `simpleTransactionRep.cc` gets what it needs from the `BeginTransaction/Commit/Rollback` triple already. A separate `Transaction` object is mostly a shape match for RocksDB code being ported in; low urgency compared to WriteBatch/MultiGet.

**Medium value, medium effort**:
- **Cross-shard Scan** (the existing single-shard limitation; requires RPC fan-out for `RemoteDB`).
- **Snapshot API** limited to transaction-scoped snapshots only; `db->GetSnapshot()` outside a txn returns `NotSupported`.
- **Range delete** as `Scan` + per-key delete inside a txn.

**Low value or infeasible**:
- Pessimistic `TransactionDB` (no locking substrate in Mako).
- 2PC `Prepare`.
- `MergeOperator`, `CompactionFilter`, custom `Comparator`.
- Persistence-adjacent (`Flush`, `CompactRange`, `BackupEngine`, `Checkpoint` unless the existing `ckstate`/`kvio` infrastructure is wired up as a follow-on project).

**Deliberately don't try**:
- Making `WriteOptions.sync` mean anything for in-memory paths.
- Exposing a `SequenceNumber` (Mako has no monotonic global counter).
- Snapshots that outlive a transaction.

---

## Related files

- `src/rocks_interface/idb.hh` — the abstract `IDatabase` and `ITable` interfaces this doc describes.
- `src/rocks_interface/db.hh` — local `mako::DB` implementation.
- `src/rocks_interface/remote_db.hh` — remote-client `mako::RemoteDB` implementation.
- `src/rocks_interface/local_table.hh` — local table backing.
- `src/mako/silo_runtime.h` — the `SiloRuntime` container that each local DB wraps.
- [`masstree-test-plan.md`](masstree-test-plan.md) — masstree correctness testing.
