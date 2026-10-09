# The Storage Interface

How Mako's table layer is shaped today. (Design history lives in git:
the plan documents `silo-masstree-api-unification.md`,
`mako-nontxn-api-plan.md`, and `ordered-index-trait-plan.md` were
removed after implementation; `git log --follow docs/` finds them.)

For how to author inline-Rust DSL in general — reshaping C++ so it
converts, the per-class translation recipe, the lowering footguns — see
the field guide [porting-cpp-to-rust-dsl.md](porting-cpp-to-rust-dsl.md);
this page covers only the storage-specific mechanics.

## One interface, three traits, three backends

The interface is authored as **rusty-cpp inline-Rust traits** in
`src/mako/storage/abstract_ordered_index.h` — the `#if RUSTYCPP_RUST`
block is the source of truth; the committed `GEN` block holds the
transpiler-lowered pure-virtual C++ classes:

- **`OrderedIndex`** — the non-transactional KV surface every backend
  implements: `get / put / insert / remove / scan / rscan` (+ `size`,
  `clear`, `get_table_id`, `get_is_remote`). Each op is
  self-contained and immediately visible; on STO/MassTrans-backed tables it is
  an internal one-op OCC transaction, so writes replicate through the
  normal commit path. `put` returns "newly inserted"; `insert` is
  put-if-absent; `remove` returns "existed" (and is a direct raw
  write on mbta — a documented asymmetry). Must not be called from a
  thread with an open transaction.
- **`TxnOrderedIndex: OrderedIndex`** — the transactional ops, all
  prefixed: `tx_get / tx_put / tx_insert / tx_remove / tx_scan /
  tx_rscan / tx_scan_remote_one`, taking the opaque txn handle from
  `abstract_db::new_txn`.
- **`ShardParticipant`** — cross-shard 2PC RPC-handler ops
  (`shard_get / shard_put / shard_scan`) that stage into the serving
  thread's ambient Sto transaction; the coordinator drives
  commit/abort over later RPCs.
- **`FullOrderedIndex: TxnOrderedIndex + ShardParticipant`** — the
  full-role combination. `abstract_ordered_index` is an alias for it.

**No name has more than one spelling on the class surface**, and
there are no default arguments or string-key members — Rust traits
can't express them, and C++ overloading on a class invites name-hiding
bugs. Convenience spellings live in **free functions** (same header):
`tx_get(t, txn, key, value)` etc., with `std::string` keys and
default `max_bytes_read`/`arena` — free-function overload sets need
no `using`-declarations and may carry defaults.

## Value conventions

- **Non-txn ops: raw bytes both directions.** Backends apply their
  storage encoding internally (mbta wraps writes with `mako::Encode`;
  reads/scans come back stripped). This is what makes backends
  interchangeable.
- **Txn'd ops: caller encodes.** `tx_put`/`tx_insert` store a pointer
  into the caller's buffer until commit, so the caller must pass a
  `mako::Encode()`d value that outlives the commit.

## Backends (`src/mako/storage/`)

| class | layer | authored | notes |
|---|---|---|---|
| `masstree_ordered_index` | plain Masstree (L1) | DSL struct | implements `OrderedIndex` ONLY — "no transactions" is a type fact. Owns value buffers in the RCU arena (`[u32 len][bytes]`), frees RCU-deferred, every op pins a `scoped_rcu_region`. |
| `mbta_ordered_index` | STO/MassTrans | DSL struct (`mbta_wrapper.hh`) | DSL entrypoints call narrow engine kernels; governed point dispatch borrows canonical native handles, not startup remote flags. Kernels own the exception boundary (`STD_OP` catch of `Transaction::Abort`, non-txn retry loops, RPC retries) and `UPDATE_VS` bookkeeping. MassTrans (non-movable) sits behind a raw pointer; build via `mbta_index_build(name, table_id, is_remote)`. |
| `mbta_sharded_ordered_index` | Mako routing | DSL struct | FNV-1a per-key routing over `abstract_ordered_index*` shards; txn'd range reads visit every shard; non-txn scans are local-shard-only. |

Backends are chosen at construction; callers hold the narrowest
interface they need (`OrderedIndex*` for KV consumers,
`abstract_ordered_index*` where txn'd + 2PC roles are both required,
e.g. `ShardReceiver::open_tables_table_id`).

### TPC-C warehouse handles

With `MAKO_CLUSTER_CONFIG` enabled, TPC-C's governed table accessors resolve
`(canonical logical table, global warehouse)` through the current native
owner **and epoch**, rather than retaining the startup local/remote pointer.
`src/cluster/warehouse.rs` owns the handle cache and selection. The narrow
`src/mako/benchmarks/tpcc_warehouse_bridge.h` callback only opens an engine
index, registers its immutable canonical binding, and returns a borrowed
opaque pointer; the engine retains those indexes until shutdown.

The fixed routing coordinate is the four-byte big-endian global warehouse ID.
Row keys keep their existing warehouse-local encoding. Each governed warehouse
gets a separate physical index, even when the static benchmark layout would
share a tree. Local handle identity includes the physical participant: two
shards in one process never share source and destination bytes. Cross-participant
access uses an origin-owned remote proxy, including in that single-process
case, so the destination admits the operation through its own lease registry.
Proxies have canonical bindings but are excluded from the reverse physical
index; RPC resolves the destination's binding rather than copying a numeric
source table-id window.

The migration storage adapter must call `mako_sharding_warehouse_local` for
every warehouse coordinate intersecting its range **before** scanning/copying
the destination. This opens adopted local storage before publication and reuses
the correct participant's existing index on partial return. Source cleanup uses
that same explicit physical-owner API, not workload route resolution. The cache
does not authorize access: native lease admission rejects a stale incarnation,
and a subsequent workload attempt resolves again.

The immutable replicated `item` table is explicitly classified as replicated
by the native catalog; the mutable microbenchmark's `item` is explicitly static.
Neither invents a single warehouse owner. When `MAKO_CLUSTER_CONFIG` is absent,
the existing static vectors and single-process table wiring remain in use.

### Governed point operations and terminal leases

Raw and transactional point entrypoints capture the full canonical owner/epoch
grant before selecting the actual local physical table or origin-owned proxy.
An ownership return reuses that owner's physical table, not the startup wrapper.
The captured grant survives OCC and RPC retries; receivers retain the sender's
envelope rather than consulting a newer route. Shared proxy flags never change.
Explicit static/immutable tables and disabled deployments retain their existing
dispatch behavior.

Transactional lock batches carry a distinct put/delete operation byte per row.
A remote delete stages `MassTrans::transDelete` at the participant, including
the absent-key read when no row exists; it is not encoded as an empty put.
Absent transactional reads return `NOT_FOUND` without dropping their read set.
Read, write, and absent-key native leases remain held through actual engine
install/unlock/cleanup. Terminal reply retries replay retained results rather
than executing again, and abort/unlock also retry uncertain transport outcomes.
The origin uses `transDeleteRemote` to stage an initialized delete carrier even
with `READ_MY_WRITES` disabled. Aborted carriers are removed; put-then-delete
keeps delete write-data instead of transmitting an empty value as a put.

Governed same-process peers use the same SRPC envelope and participant engine
path as separate processes. Their helper queues, transports, allocator/RCU
bindings, and admission contexts are owner-specific. Helper initialization may
borrow loader allocation setup, but clears loader admission before serving RPCs.
Nontransactional scan callbacks are not replayed after observable delivery.
Callback exceptions close the actual engine transaction and finish touched
remote participants before the exception escapes; successful read-only scans
validate and release remote read sets.

### Native migration I/O boundary

`native_sharding_storage.cc` resolves physical indexes through the canonical
registry and materializes every intersecting warehouse coordinate, including
previously unopened aliases. Its bounded cursor returns one row strictly after
the full `(coordinate, row key)` cursor. Only `MAKO_SHARD_NOT_FOUND` certifies
EOF; an OCC conflict or engine exception returns an error, never a short
successful scan. Callbacks publish copied bytes only after engine commit.

The image compared by Rust is the logical row value, excluding MBTA's internal
metadata trailer. Point puts rebuild that trailer with the normal engine
encoder. Successful point puts/deletes have a one-physical-key frame.
The Rust participant authorization guard protects the **selected range**, not
all node data: already-admitted transactions may still change unrelated ranges.
Raw transfer calls do not reenter ordinary lease admission; a scoped engine
lifecycle hook suppresses only that reentry and leaves normal admission intact.
The host binds each native thread to its actual shard and allocator/Masstree
runtime and releases its transaction state before worker exit.

## Authoring & regenerating the DSL blocks

- Regenerate with `scripts/regen_storage_dsl.sh` (never bare
  `inline-rust --rewrite`): it wraps the transpiler and prefixes
  `inline` onto out-of-line definitions inside GEN regions — the
  transpiler's single-TU module precedent is an ODR violation in
  these multi-TU headers. `--check` is the drift guard.
- **Use the repository's pinned compiler.** The cutover was regenerated with
  clean `7e0c201f1b0d548f0166dc9ee700f24bc18066a4`, matching the development
  branch's gitlink, submodule HEAD and compiler `--build-info`. Native sharding
  is compiled by rustc and does not require a transpiler pin change.
  Pass the actual binary to `scripts/regen_storage_dsl.sh --check BINARY`;
  the default is `third-party/rusty-cpp/target/release/rusty-cpp-transpiler`.
- The current census is **six carriers**: the four storage headers plus
  `src/cluster/kv_store.h` and `src/cluster/config_manager.h`. Retired C++
  sharding carriers are no longer regenerated. Keep Rust source adjacent to
  its single GEN region; module exports wrap the containing namespace.
- Trait → interface lowering uses namespace-scope `pub trait`. Storage backend
  inheritance uses the compiler-owned inert spelling
  `#[cfg_attr(any(), cpp_inherit)] impl Trait for Struct`, adjacent to the
  struct. It needs no fake Cargo/proc-macro facade. For multi-trait backends,
  attach the empty `FullOrderedIndex` impl with that marker and put method
  bodies in the inherent `impl`; merged members override virtuals by signature.
- Lowering gotchas: `&self.field` lowers to a POINTER (C++ helpers
  take `const T*`); raw-pointer spellings need `using c_void = void;`
  style aliases; opaque C++ types (e.g. `oi_stats_map`,
  `shard_table_vec`) pass through single-ident aliases; DSL structs
  are move-only with a synthesized fieldwise ctor, so non-movable
  fields (masstree trees, MassTrans) live behind raw pointers.
- `oi_scan_callback` is the shared scan callback type (namespace
  scope, so traits can name it).

## Facades above this layer

`src/rocks_interface/` (RocksDB-style `ITable`/`IDatabase`/`Status`)
consumes this interface; see `docs/rocksdb_interface.md`.

## Cluster metadata port (`src/cluster/kv_store.h`)

The cluster component's dependency on storage is a three-method
`KvStore` port (`get`/`put`/`remove`, string keys, raw byte values).
It is authored in this same DSL — a `pub trait KvStore` — and is in the
`regen_storage_dsl.sh` FILES list, so the drift guard covers it. In
production the port binds to the unified store via `OrderedIndexKvStore`
(the `__mako_config__` system table on shard 0); tests bind an
`InMemoryKvStore` fake. See
[mako-book §3](mako-book.md#3-configuration-manager-master-shard) for
the sharding design.

### Native sharding boundary

`ConfigManager`, `KvStore`, `RemoteKvStore`, and `OrderedIndexKvStore` retain
metadata persistence and reads. They are not a second routing or migration
implementation. The former C++ policy value types, builder/cache,
`ClusterConfig`, `ConfigWatcher`, and in-memory shard manager were retired
when governed sharding moved to native Rust.

The remaining `cluster` static shard-router helper describes physical table
slots only, for the disabled/static workload path. Native route failures must
not fall back to this geometry. The Rust runtime reaches real storage through
the host bridge; the bootstrap and lifecycle contract are documented in
[mako-book](mako-book.md#bootstrap-protocol).
