# Mako Non-Transactional API Plan

**Sequel to** [`silo-masstree-api-unification.md`](silo-masstree-api-unification.md), which gave Silo's L3 layer (`abstract_ordered_index`) a non-transactional API matching Masstree's shape. This plan extends the same API to **Mako — the distributed (sharded + replicated) layer** — and defines the uniform surface that lets application code switch between the three backends (Masstree, Silo, Mako) by switching a namespace.

## Goal

1. Non-txn `get / put / insert / remove / scan / rscan` work on Mako deployments: a key owned by a **remote shard** is reached over RPC; writes on a replicated leader reach the **replication log**.
2. A **uniform three-backend surface**: the same application code (modulo a namespace/typedef choice) drives Masstree, Silo-local, or Mako-distributed.

## What the investigation established (facts, with citations)

### F1. One-op-txn writes already replicate

`Sto::commit()` → `Transaction::try_commit(no_paxos=false)` (`Transaction.cc:386`). When `BenchmarkConfig::getIsReplicated()` is true, the commit path calls `serialize_util` (`Transaction.cc:634-655`), which packs the committed write-set into a per-thread, per-shard batch buffer; when `checkPushRequired()` fires it synchronously calls `add_log_to_nc(...)` (`Transaction.cc:833`) → Raft (`raft_main_helper.cc:786`) or Paxos (`paxos_main_helper.cc:384`) → `worker->Submit(...)` **iff leader**, plus `persistAsync` to RocksDB (`Transaction.cc:849`).

**Consequence**: the Silo non-txn ops from the previous plan (each an internal one-op OCC txn) get replication *for free* on a leader. No new replication plumbing is needed for Mako's non-txn writes — only routing.

**Caveat**: submission is batched (default `large_batch_num=400`, `Transaction.cc:637-643`; env override `MAKO_BATCH_SIZE`). A single non-txn write may sit in the batch buffer until enough writes accumulate. See D2.

### F2. Reads are already remote-capable; writes are not

- `mbta_wrapper::get()` (txn'd) branches on `mbta.get_is_remote()`: local → `transGet`, remote → `TThread::sclient->remoteGet(table_id, key, value)` (`mbta_wrapper.hh:119`). Same for scan via `remoteScan`.
- The **write** path across shards is exclusively ambient-2PC: `remoteBatchLock` (→ server-side `shard_put`, no commit) / `remoteValidate` / `remoteInstall` / `remoteAbort`, driven by the coordinator's `try_commit` (`Transaction.cc:455,560,612,667`). There is **no self-contained remote write RPC** today.
- The Phase-2 non-txn overrides on `mbta_ordered_index` deliberately `ALWAYS_ASSERT(!is_remote)` — remote tables are excluded pending this plan.

### F3. Two routing schemes coexist

- **Table-level**: each table belongs wholly to one shard; `preallocate_open_index` computes `shard_index = (table_id-1)/NUM_TABLES_PER_SHARD` and sets `is_remote` if it isn't the local shard (`mbta_wrapper.hh:1383-1408`).
- **Key-hash**: `mbta_sharded_ordered_index::pick_shard` (FNV-1a mod nshards) picks a per-key backing table, which may itself be an `is_remote` table; `ShardClient::compute_shard_for_key` (`shard_router.h:41`) does the equivalent for RPC destinations.

Non-txn ops must work under both: fixing the `is_remote` branch on `mbta_ordered_index` automatically fixes `mbta_sharded_ordered_index` (its non-txn mirrors just route to per-key tables).

### F4. The existing RemoteDB KV write path is unsound (pre-existing bug)

`RemoteDB::Put` → RPC 23 → `MakoClientService::HandlePut` (`client_service.cc:135`) → `shard_put(key, value)` with **no surrounding transaction, no commit, no unlock, no replication**. `shard_put` stages the write in the server worker's ambient Sto transaction and locks its write-set entry (`mbta_wrapper.hh:167-175`); nothing ever commits or releases it. `CommitClientTransaction` merely erases a tracking-map entry (`server.cc:695`).

**Consequence**: the new self-contained non-txn RPCs added by this plan are not just a feature — they are the correct foundation to *re-base RemoteDB's KV path on* (Phase 4).

### F5. Only leaders submit to the log

`add_log_without_queue` submits only if `worker->is_leader` (`paxos_main_helper.cc:392`). Non-txn writes on a follower would silently skip replication.

## Design decisions

**D1 — Remote transport: new self-contained RPCs for writes; reuse existing ones for reads.**
- Reads: reuse `remoteGet` / `remoteScan` (already self-contained server-side).
- Writes: add `nontxnPut`, `nontxnInsert`, `nontxnRemove` RPC types. Server-side handlers call the server's **local L3 non-txn API** (`table->put(key, value)` etc.) — each op runs its one-op OCC txn on the owning shard, which replicates per F1. No ambient-txn staging, no separate lock/validate/install phases.

**D2 — Replication batching is accepted and documented.**
A non-txn write on a replicated leader is durable/replicated only after its batch flushes (F1 caveat). This is the same semantics every Mako txn already has. Out of scope: a per-op flush knob (note as future work if a consumer needs synchronous durability).

**D3 — Leader-only writes, enforced loudly.**
Non-txn writes on a non-leader must fail loudly (assert or error return), not silently skip replication (F5). Reads may be served locally (stale-read semantics on followers is a documented future extension, not in scope).

**D4 — Uniform surface pinned to the Silo shape.**
`lcdf::Str` keys, `std::string` values, `bool` returns, callback scans — as established in the previous plan. Masstree does not get renamed; it gets a thin **adapter** (see Phase 5) presenting the same verbs. The three backends become:

| Backend | Type | Notes |
|---|---|---|
| Masstree | `masstree_kv::Table` (new adapter) | wraps `mbtree<concurrent_btree>`; owns value memory (masstree stores raw pointers); RCU-deferred frees |
| Silo | `mbta_ordered_index` via `abstract_ordered_index*` | exists (previous plan) |
| Mako | `mbta_sharded_ordered_index` (or L7 `ITable`) | this plan makes its non-txn ops distributed-correct |

**D5 — Cross-shard scan stays unsupported.**
Non-txn `scan`/`rscan` inherit the existing single-local-shard limitation (`scanRemoteAll` TODO at `mbta_sharded_ordered_index.hh:242`). A non-txn scan over a remote *table* (whole-table remote) uses `remoteScan`; a scan over a hash-sharded keyspace stays local-shard-only, documented.

## Phases

### Phase 1 — RPC layer: self-contained non-txn write ops (~2 days)

- Add three request types to the shard RPC protocol (alongside `getReqType`/`scanReqType`): `nontxnPutReqType`, `nontxnInsertReqType`, `nontxnRemoveReqType`.
- `ShardClient` gains `nontxnPut(table_id, key, value) -> bool`, `nontxnInsert(...) -> bool`, `nontxnRemove(table_id, key) -> bool` (bool = the op's Masstree-parity return, shipped back in the reply).
- `server.cc` handlers: look up the table by id, call the **local L3 non-txn API** (which internally does the one-op txn + retry; replication via F1). Handlers must run on a thread with proper Silo thread-init (same worker threads that serve `HandleGetRequest` already qualify).
- Leader check per D3: if the receiving process is not the leader for its shard group, return an error code; the client surfaces it.

### Phase 2 — Remote branch in `mbta_ordered_index` non-txn ops (~1 day)

Replace `ALWAYS_ASSERT(!mbta.get_is_remote())` in the six non-txn overrides:
- `get`: remote → `TThread::sclient->remoteGet(table_id, key, value)` (mirror the txn'd get's remote branch, including EXTRA_BITS handling, `mbta_wrapper.hh:118-128`).
- `put`/`insert`/`remove`: remote → the new `ShardClient::nontxn*` calls.
- `scan`/`rscan`: remote → `remoteScan` for forward scan (single-key-range semantics as the txn'd path); `rscan` remote returns `NotSupported`-style loud failure initially (no `remoteRScan` exists; add only if a consumer needs it).

`mbta_sharded_ordered_index`'s non-txn mirrors then handle mixed local/remote shard tables with no further change (F3).

### Phase 3 — Distributed gating tests (~2 days)

Extend `tests/test_silo_nontxn_api.cc` or add `tests/test_mako_nontxn_api.cc`:
- **Multi-shard single-process** harness (the pattern CI's `multiShardSingleProcess` uses): two shards in one process, tables assigned to each, non-txn ops on keys owned by the "other" shard exercising the RPC path end-to-end.
- Routing: non-txn put on remote-owned table lands on the owning shard (verify via direct read on the owner).
- Return values: remote insert-dup returns false; remote remove-absent returns false.
- Replication smoke (if harness permits): `getIsReplicated()` on, verify a non-txn write reaches `serialize_util` (e.g., via the batch buffer stats or a small `MAKO_BATCH_SIZE`).
- Leader-enforcement: non-leader write fails loudly.
- Existing gates stay green: masstree suites (152), silo non-txn (13), silo/STO suites.

### Phase 4 — L7 facade: non-txn ITable ops + fix the RemoteDB KV path (~2 days)

- `ITable` gains non-txn overloads: `Get(key, &value)`, `Put(key, value)`, `Insert(key, value)`, `Delete(key)`, `Exists(key, &exists)` — no `void* txn` parameter, `mako::Status` returns (facade convention).
- `LocalTable` implements them over the L3 non-txn API.
- `RemoteTable` implements them over the Phase-1 RPCs — **replacing** the unsound `HandlePut`-via-`shard_put` path (F4). The txn'd `RemoteTable` methods stay as-is for now; a follow-up can re-base or deprecate them once the server-side transaction tracking story is decided.
- `examples/rocksdbInterfaceTest.cc` gains non-txn coverage.

### Phase 5 — The namespace-switch surface (~3 days)

- **`masstree_kv` adapter** (new, ~200 LOC): `masstree_kv::Table` wrapping `mbtree<concurrent_btree>` with the Silo-shape API. Owns value allocations (`std::string` copies on put; masstree holds pointers; overwrite/remove frees are RCU-deferred via the masstree threadinfo machinery). Requires per-thread masstree threadinfo registration — same contract the masstree tests already use.
- **Uniform aliasing header** (`src/mako/kv_backends.hh` or similar): three namespaces each exporting `Table` + `open(name)` + the six ops with identical signatures:
  - `kv_masstree::Table` = the adapter
  - `kv_silo::Table` = `mbta_ordered_index` behind a thin open/name registry
  - `kv_mako::Table` = `mbta_sharded_ordered_index` (or L7 `ITable` — decide during implementation based on which needs fewer glue types)
- **Compile-both-ways test**: one templated test body `template <typename Backend> void exercise()` instantiated against all three backends, asserting identical observable behavior for the single-node-reachable subset. This is the artifact that proves "switch namespace" works.

### Phase 6 — Docs (~half day)

- This plan updated with as-implemented notes (as done for the previous plan).
- `rocksdb_interface.md`: note that ITable now has non-txn methods; RemoteDB KV path re-based.
- `mako-book.md` §6 (storage engines): cross-link the three-backend surface.

## Effort estimate

| Phase | Days |
|---|---|
| 1. RPC layer | ~2 |
| 2. Remote branch in mbta non-txn ops | ~1 |
| 3. Distributed gating tests | ~2 |
| 4. L7 facade + RemoteDB KV fix | ~2 |
| 5. Namespace-switch surface + masstree adapter | ~3 |
| 6. Docs | ~0.5 |
| **Total** | **~10-11 days** |

Phases 1–3 are the core "Mako has the API" milestone; 4–5 are the consumer-facing milestone. They can ship as two PRs.

## Risks & open questions

- **Batch-buffer latency (D2)**: a lone non-txn write on an idle replicated system may wait indefinitely for its batch to fill. Mitigation candidates if this bites: small `MAKO_BATCH_SIZE`, a time-based flush, or an explicit flush RPC. Decide when a real consumer hits it.
- **Server-side thread context for new handlers**: the Phase-1 handlers must run on properly Silo-initialized worker threads. The existing `HandleGetRequest` path proves the environment exists; new handlers must use the same dispatch.
- **`RemoteTable` txn'd path**: fixing the KV path (F4) properly may reveal that server-side `client_transactions_` tracking should be removed or reworked; scoped as follow-up, not blocking.
- **Leader detection**: D3 needs a clean "am I the leader for this shard group" predicate available at handler level; verify `worker->is_leader` (or equivalent) is queryable there.
- **masstree_kv adapter concurrency**: value frees on overwrite/remove must be RCU-deferred to be safe under concurrent readers; the adapter must not silently be single-threaded-only unless documented.

## Non-goals

- Cross-shard scan / `scanRemoteAll` (existing limitation, unchanged).
- Follower/stale reads (documented future extension).
- Per-op synchronous durability / flush control (D2).
- Renaming Masstree's own verbs (`search` etc.) — the adapter provides the uniform surface.
- Reworking the ambient-2PC `shard_*` protocol (untouched, as in the previous plan).
- `remoteRScan` (add only on demand).

## As implemented (2026-07)

All six phases landed on `worktree-api-unification`. Deviations and
discoveries, by phase:

**Phase 1** — Landed as planned, plus one type the plan didn't call
for: `nontxnGetReqType = 17`. Wire format `nontxn_write_request_t`
must start with `targert_server_id` because the transport backends
peek the first `uint16_t` of every shard request to pick the helper
queue (`TargetServerIDReader`, rrr_rpc_backend.cc). Handler
registration ranges widened to 1..17.

**Phase 2 (D1-reads revised)** — The plan reused `remoteGet` for the
remote non-txn get. That is wrong: `HandleGetRequest → shard_get`
stages a read-set item in the serving worker's participant
transaction, which the txn path cleans up via its later 2PC
abort/commit — a non-txn caller never sends one, so the worker is
left permanently "busy" and every later non-txn write spins on
SERVER_BUSY. Reads got their own self-contained `nontxnGet` (server
runs the L3 get; stages nothing). Corollary: the server's L3 get
already strips EXTRA_BITS, so the client must NOT strip again —
values longer than the suffix were being silently truncated
(long-value regression tests now pin this on both the ShardClient and
RemoteDB paths). Client remote branches retry transient failures
(TIMEOUT / SERVER_BUSY) instead of asserting.

**Phase 3** — `tests/test_mako_nontxn_distributed.cc`: two shards in
one process (client-view `is_remote` tables driving RPCs into a
`ShardServer` over the loopback transport), 8 tests. Server-side
"busy" needed sharpening: `Transaction::has_staged_items()` requires
`in_progress()` — after a mode-0 commit `tset_size_` keeps its final
count until the next `start_transaction`, so committed leftovers must
not read as staged 2PC state. Replication smoke and leader-
enforcement tests were not implementable in this harness (no
replicated single-process fixture); leader gating is enforced in
`RunNontxnOp` and exercised implicitly by CI's replicated suites.

**Phase 4** — The investigation found the decoupled-client stack has
two parallel server paths (raw-struct handlers types 20-25 dispatched
via `ReceiveRequest`, and a never-registered rrr `MakoClientService`)
and that `RemoteDB`'s rrr protocol cannot talk to `ClientTcpServer`'s
raw `[type][len][payload]` framing at all — both halves were
scaffolding. Implementation: the non-txn core is factored into
`ShardReceiver::RunNontxnOp` (leader check, staged-2PC busy check,
mode-0 one-op commit, mode-1 idle-participant restore) and ALL
decoupled-client KV handlers were re-based onto it (F4 fixed in both
families; "delete" is now a real remove instead of an uncommitted
empty-value put). `RemoteTable`'s non-txn methods speak the raw
framing with types 14-17 via `RemoteDB::ConnectNontxn` +
`GetTable(name, table_id)` — the first client that interoperates with
`ClientTcpServer` end-to-end. Enablers fixed along the way:
`ClientTcpServer` workers now bind Silo thread state
(`scoped_db_thread_ctx`, the TODO the original code deferred);
`ClientTcpServer::Stop` must `shutdown()` the listen fd (close alone
never wakes a Linux `accept()`); `ShardReceiver::Register` only
establishes the idle-participant invariant in mode 1; and
`mbta_sharded_ordered_index::Insert(txn)` got its duplicate detection
restored (regressed to a blind `put()` in 9d66d336 —
`rocksdbInterfaceTest` I1.4 had been red since, unnoticed because CI
never runs it). `ITable` non-txn methods default to `NotSupported`
rather than pure-virtual so other implementers keep compiling.

**Phase 5 (revised: consolidated, no new surface)** — the first
implementation added `src/mako/kv_backends.hh` with `kv_masstree` /
`kv_silo` / `kv_mako` namespaces; review concluded that was yet
another API layer beside the layers it wrapped, so it was deleted and
consolidated into `abstract_ordered_index` itself:

- The RAW-value-bytes convention moved INTO the L3 non-txn ops:
  `mbta_ordered_index::put/insert` apply `mako::Encode()` internally
  (reads/scans already come back stripped), so no caller of the
  non-txn surface sees the EXTRA_BITS convention. The txn'd ops keep
  caller-encodes (they store a pointer into the caller's buffer until
  commit). Remote non-txn ops carry raw bytes on the wire; the owning
  shard encodes at its local L3.
- The masstree adapter became `masstree_ordered_index`
  (`src/mako/benchmarks/masstree_ordered_index.hh`), a real
  `abstract_ordered_index` subclass: the six non-txn ops implemented
  (values owned in the RCU arena, overwrite/remove frees deferred,
  every op pins a `scoped_rcu_region`); the transactional/2PC
  virtuals abort via `NDB_UNIMPLEMENTED` — masstree has no
  transaction runtime.
- `mbta_sharded_ordered_index` now actually inherits
  `abstract_ordered_index` (it had only mirrored the methods), with
  compiler-verified `override`s and the remaining pure virtuals
  implemented (point `shard_get`/`shard_put` route per-key; the range
  2PC ops abort — RPC handlers operate on per-table objects, never
  the routing wrapper).

"Switch backend" is therefore plain runtime polymorphism: construct
`masstree_ordered_index`, `mbta_ordered_index`, or
`mbta_sharded_ordered_index` and hold an `abstract_ordered_index*`.
Gate: `tests/test_kv_backends.cc` — one TYPED_TEST body over the
three concrete types through the base pointer, plus a single ordinary
function exercised against all three (25 tests).

**Phase 6** — this section; consumer notes added to
`rocksdb_interface.md` and `mako-book.md`.

## Related documents

- [`silo-masstree-api-unification.md`](silo-masstree-api-unification.md) — the Silo-level predecessor (implemented).
- [`rocksdb_interface.md`](rocksdb_interface.md) — the L7 facade this plan's Phase 4 extends.
- `src/mako/benchmarks/mbta_wrapper.hh`, `mbta_sharded_ordered_index.hh` — Phase 2 targets.
- `src/mako/lib/shardClient.cc`, `src/mako/lib/server.cc` — Phase 1 targets.
- `src/rocks_interface/` (`remote_db.hh`, `idb.hh`, `local_table.hh`, ...) — Phase 4 targets; the facade moved to its own folder in 2026-07.
