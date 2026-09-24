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
| `mbta_ordered_index` | STO/MassTrans | DSL struct (`mbta_wrapper.hh`) | remote/local dispatch in the DSL; per-verb C++ kernels own the exception boundary (`STD_OP` catch of `Transaction::Abort`, non-txn retry loops, RPC retries) and the `UPDATE_VS` bookkeeping. MassTrans (non-movable) sits behind a raw pointer; build via `mbta_index_build(name, table_id, is_remote)`. |
| `mbta_sharded_ordered_index` | Mako routing | DSL struct | FNV-1a per-key routing over `abstract_ordered_index*` shards; txn'd range reads visit every shard; non-txn scans are local-shard-only. |

Backends are chosen at construction; callers hold the narrowest
interface they need (`OrderedIndex*` for KV consumers,
`abstract_ordered_index*` where txn'd + 2PC roles are both required,
e.g. `ShardReceiver::open_tables_table_id`).

## Authoring & regenerating the DSL blocks

- Regenerate with `scripts/regen_storage_dsl.sh` (never bare
  `inline-rust --rewrite`): it wraps the transpiler and prefixes
  `inline` onto out-of-line definitions inside GEN regions — the
  transpiler's single-TU module precedent is an ODR violation in
  these multi-TU headers. `--check` is the drift guard.
- **Which transpiler binary: pass it explicitly.** The committed GEN
  blocks came from the older `a4bcff5f`-era build the script still
  names as its default (`build_local/rusty-cpp-transpiler-a4bcff5f`),
  a path no clean checkout has. The submodule pin has since moved to
  `a1f8fef8` — upstream main's tip; gitlink = submodule HEAD = the
  built binary's `--build-info`, `git_dirty=false` — and the rusty
  runtime headers are present there
  (`third-party/rusty-cpp/include/rusty/`), so this page's older note
  ("the pin does NOT move — upstream main dropped runtime headers srpc
  needs") described July 2026 and is retired. So is its rationale that
  GEN output has no rusty-runtime dependency: three of the four storage
  blocks have none, but `mbta_sharded_ordered_index.hh` lowers through
  `rusty::detail::deref_if_pointer_like` (`rusty/ptr.hpp`), and the
  cluster blocks below emit `rusty::Option`/`Cell`/`Mutex`/
  `sync::atomic` over an explicit `#include <rusty/…>`.
- **Regenerating at the pin is a migration, not a no-op.** Measured
  2026-09-17 over the FILES list with
  `third-party/rusty-cpp/target/release/rusty-cpp-transpiler`: 11 files
  drift (trait forward declarations appear; synthesized copy ctors
  become `= delete`; `!x` becomes `rusty::detail::rust_not(x)`;
  `kv_store.h` gains a second GEN region outside `export { }`), and the
  `inline` post-pass then mis-fires on the pin's column-0 lambda
  statements (`inline while (…)`). Three — the `#[cpp_inherit]` storage
  headers — stop outright: `cpp_inherit` now "requires a Cargo manifest
  so the `rusty` provider can be authenticated", and no `Cargo.toml`
  sits above `src/mako` or `src/cluster` (only `src/srpc` has one). One
  (`cluster_config.cc`) reproduces byte-for-byte. `--check` therefore
  guards drift only against the binary you hand it.
- Trait → interface lowering: `pub trait` (namespace scope; non-pub
  gets a TU-local wrapper). Struct + `#[cpp_inherit] impl Trait for
  Struct` ADJACENT to the struct lowers to direct inheritance (no
  adapter). For multi-trait backends: attach the empty
  `#[cpp_inherit] impl FullOrderedIndex for X {}` and put methods in
  the inherent `impl X` — merged members override inherited virtuals
  by signature.
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

### Value types too (`sharding_policy.h`)

`RangeMapping`, `TableShardingPolicy` and `ShardingPolicySet` are also
authored in the DSL, as `pub struct` + **inherent** `impl`. The lowering
subtlety that makes this work: a struct is move-only *only* when it
attaches a trait via `#[cpp_inherit] impl Trait for X` (inheritance). A
plain struct with an inherent `impl X` lowers to a **copyable aggregate**
(no synthesized ctor/move) — so these keep living in `std::map`/
`std::vector` by value, and the srpc marshal reader (default-construct +
field fill, which stays C++ at the boundary) is unchanged. `get_shard`'s
binary search is expressed directly in the DSL; the iterator insert and
map lookups stay as C++ kernels the DSL calls (the same "DSL owns shape,
C++ owns pointer surgery" split as the masstree header).

Two sharp edges, both handled — and worth knowing before converting more
POD:
- The DSL cannot express default member initializers (`= -1` is a parse
  error), so the old constructors become factories that set them
  (`RangeMapping::make`, `TableShardingPolicy::create`,
  `ShardingPolicySet::with_shards`).
- Because of that, **every construction site must move to the factories** —
  not for compilation (C++20 parenthesized aggregate init means
  `ShardingPolicySet(2)` still *compiles*) but for correctness: paren-init
  fills fields in declaration order, so `ShardingPolicySet(2)` would set
  `version = 2`, not `num_shards`. The compiler will not flag this.

### What stays C++, and why

- `KeyExtractor` — its `type` field is a Rust keyword; the DSL emits
  `r#type` verbatim, which is invalid C++. Left as a hand-written struct
  (referenced by name as a field type from the DSL structs, which works).
- `config_manager` / `cluster_config` — stateful, STL-backed, already
  `@safe`; a DSL rewrite would relocate 400+ lines of logic into
  `@unsafe` C++ kernels for no borrow-checking gain.
- `config_watcher` (threads) and `config_store` (RocksDB) — same
  reasoning as the storage kernels.

The DSL earns its place at interfaces (this port; the `OrderedIndex`
traits), in copyable value types whose bodies are simple enough to
express directly, and in genuine raw-pointer surgery (the masstree RCU
kernels) — not in stateful STL glue.
