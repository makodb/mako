# Sharding and range migration: end-to-end design

This is the review entry point and white-paper-style description of Mako's
native sharding design. It separates live migration from application
transactions, persistence, and conditional recovery. The native implementation
uses engine-epoch admission and drained handoff with online private pre-copy.
Historical lease-path measurements remain labeled separately below.

The [production durability integration plan](../plans/range-sharding/durable-production-integration.md)
maps the remaining backend, primary-data, transaction-settlement, and runtime
work. It distinguishes metadata-only persistence from complete crash recovery.

## 1. Read this first

**Sharding decides where a key belongs. Migration changes that ownership without
losing data or letting two owners independently serve the same range.**

The native implementation first copies into private destination staging while the source
continues serving. It then closes new admission with an immutable Frozen
publication, establishes a cut of the existing STO epoch, and waits for older
transactions to finish their actual effects and cleanup. After background work
is joined/fenced, an exact final reconciliation precedes ownership publication.
The move is **not** one multi-key transaction.

The [engine-epoch protocol model](../../tla/mako/README.md#native-engine-epoch-admission-and-draining)
composes cached admission, advisory pre-copy, Freeze → drain → join/fence →
exact final copy → handoff. It adds no authority-epoch counter and never aborts
a transaction merely because the periodic epoch changes. Its native
weak-memory, physical-engine and distributed-OCC boundaries remain explicit.
Exercised verification and performance evidence are separate obligations.

The earlier [commit-time-validation alternative](../../tla/mako/README.md#proposed-commit-time-validation-not-deployed)
instead uses complete write-intent acquisition, Freeze-aware authority
validation, and intent-aware concurrent copy. Its separate proof and bounded
engine smoke likewise do not establish native distributed OCC or memory ordering.

Three distinctions matter:

| Concern | Current design |
|---|---|
| Moving application rows | Individual row copies/deletes, protected by range fencing and ordered protocol steps. No transaction spanning the entire move. |
| Persisting migration metadata | The durable library and optional production RocksDB adapter use a local atomic batch for a journal entry and its head. This is a multi-key write requirement, not a general transactional query/commit engine. |
| Coexisting with application transactions | Retained STO epoch protection lasts through actual effects, unlocks, cleanup and terminal participant settlement. Migration does not replace ordinary OCC or distributed commit. |

There are three storage/deployment scopes:

- **Production live path:** native Rust actors connected to MBTA/STO and srpc.
  Fixed live owners, non-replicated migration, no row/TP crash recovery.
- **Production metadata-only persistence:** optional synchronous RocksDB
  journals retain coordinator decisions/nonces and participant control/transfer
  bookkeeping. The epoch cutover removes per-access lease journaling. Reopening
  remains inspection-only and fences data access and migration continuation.
- **Conditional durable path:** executable Rust wrappers around the same actors,
  checked against durable-KV **and durable-engine/TP** contracts. A real file-backed
  reference exercises full recovery; the production durable-engine/TP adapter is
  still not implemented.

Persisting bookkeeping does not make the in-memory primary store recoverable.

### Reading order

1. [Algorithm](#3-the-migration-algorithm): what moves and when ownership changes.
2. [Application transactions](#4-integration-with-application-transactions): what retained epoch protection covers.
3. [Storage primitives](#5-storage-primitives-required-by-the-durable-path): exactly where atomicity is required.
4. [Crash recovery](#6-crash-recovery): what survives and how execution resumes.
5. [Implementation status](#7-implemented-versus-conditional-behavior): what can be used today.
6. [Review decisions](#8-decisions-to-review): current tradeoffs, not promised features.

## 2. Vocabulary and responsibilities

A migration selects a half-open range `[lo, hi)` in **one logical table**, moving
it from source owner A to destination owner B. An absent upper bound means no
upper limit. The physical row identity includes table, routing coordinate, and
row key. For ordinary tables the coordinate is the raw key; warehouse tables
can group multiple row keys under a warehouse coordinate.

| Term | Meaning |
|---|---|
| Directory / routing snapshot | Immutable mapping from table and coordinate to an ownership grant. |
| Ownership grant `(owner, ownership_version)` | Which owner may serve this placement incarnation. Returning a range to A does not resurrect A's old grant. |
| Migration coordinator | Retains the move's plan, issues controls, records participant certificates, and decides Commit or Abort. The live service uses owner 0 and allows one active move. |
| Participant | An owner applying controls and guarding its local rows. It checks its own state; it does not assume it can see the coordinator's latest phase. |
| Migration generation | Identifies a move and fences controls/copy work from older moves. |
| Administrative nonce | Identifies the caller's move request. Identical retries retain the same intent and generation; changed intent is rejected. |
| Retained epoch guard | A logical operation's protection in the existing process STO epoch domain, held through effects and cleanup; not a per-key lease registration. |
| Receiver admission incarnation | Durable-path restart identity. Old queued work cannot acquire the new identity simply because its receiver restarted. |
| Certificate | A participant's protocol acknowledgement, such as Drained or Ready. It is not a cryptographic proof; authenticated provenance remains an RPC contract. |

A transaction ID, migration generation, ownership version, and receiver admission
are different identities. They must not be substituted for one another.

Colocated owners share the same STO clock and complete worker-pin inventory.
Epoch numbers from separate processes are never compared. The conditional
durable library still uses persistent access leases and exact settlement;
volatile epoch completion cannot replace its crash-recovery obligations.

`ownership_version` belongs to a particular range's ownership incarnation.
`Snapshot.version` identifies a complete routing snapshot: another range can move
and advance the snapshot version while this range keeps its ownership version.
Neither is Mako's global transaction/speculation epoch. A captured request carries
`(snapshot_version, owner, ownership_version)` from one immutable snapshot.
`Grant` and its persistent record encoding remain unchanged; the transient
`RoutedGrant` / `MakoShardRoute` adds the snapshot version. Data/scan/gateway
envelopes carrying this tuple use the new request format, not a mixed-version
compatibility path.

## 3. The migration algorithm

Consider moving table T's range `[m, n)` from A to B. Initially A serves it, and B
is not allowed to serve it. B can still have leftover physical rows; those are
not authoritative merely because they exist.

```text
Prepare private B
   -> Background pre-copy A -> B while A serves
   -> Freeze publication at A, then establish the STO epoch cut
   -> Drain older admitted transactions through effects and cleanup
   -> Join/fence all accepted pre-copy work
   -> Exact final reconciliation A -> B; B becomes Ready
   -> Retire A, keeping A's data
   -> Commit the directory change to B
   -> Activate B and delete A's old copy
   -> Finish after both participant acknowledgements
```

This is a sequence of local steps and messages, not one atomic cross-shard
operation. There may be a period with no owner accepting new accesses. The
safety property is **at most one serving owner**, not uninterrupted availability.

### 3.1 Successful move

| Step | Action and required evidence | What clients can rely on |
|---|---|---|
| Admit the move | Validate that A owns the selected interval. Retain the nonce, fresh generation, old routing snapshot, and intended replacement. | An identical retry is the same move, not another allocation. |
| Prepare B (`Start`, coordinator phase `Copy`) | Put B's selected range in private staging for this generation. | A still owns the range. B's staged bytes are not client-visible data. |
| Background pre-copy | Copy committed row observations from A into B's round-0 staging while A continues serving. | The observations need not form one data snapshot and cannot certify Ready. |
| Freeze A (`Freeze`, `Freezing`) | Publish Frozen metadata, then advance the existing process STO clock to cut C. Admission must announce/recheck before using metadata. | New post-cut contexts cannot obtain the old Serving authority. Pre-cut cached contexts remain valid and must drain. |
| Drain A | Wait until every worker slot is quiescent or at least C. A slot represents the oldest retained guard, including retries, RPC waits and cleanup. Only then acknowledge Drained. | No authorized old effect or unfinished cleanup can outlive the barrier. Unrelated old transactions may conservatively delay it. |
| Fence background work | Join accepted pre-copy effects and close round 0 before final reconciliation; bind packets to the full plan/generation/round. | A delayed old packet cannot overwrite final values or mutate a later move. |
| Final copy (`Final`) | Merge complete ordered scans, copy A's current values and remove destination-only rows. Require explicit EOF and complete coverage. B then records Ready. | B equals the stable source over the selected range, including absences. B still does not serve it. |
| Retire A (`Retire`, `Retiring`) | After Ready, stop source serving and receive Retired. Keep A's data through the decision. | The old source cannot continue serving after the routing cutover. Abort is still possible before Commit. |
| Decide Commit (`Committed`) | Select the already-validated replacement directory and retain an irreversible committed outcome. | Ownership has been decided for B. In the durable path, this decision must be durable before the new route is exposed. |
| Deliver Commit and finish | B enters Serving. A becomes nonserving and deletes its old range. Collect DestinationDone and SourceDone before clearing the active move. | A recorded decision and a completely finished migration are different milestones. |

Pre-copy is an optimization, not a safety certificate. The native target performs
a full exact final scan after draining; it does not claim a delta-only cutover.
Without complete change tracking, pre-copy can reduce repeated writes/transfer
but final traversal remains proportional to the selected range. Freeze duration
also depends on the oldest admitted transaction; elapsed ticks are not a drain.

The final mirror merges ordered `(coordinate, row key)` scans. It preserves rows
outside the selected range, copies actual values, and deletes extras at B. An
empty value is not an absent key. An error, conflict, or short nonempty page is
not evidence that the scan has reached the end.

The protocol fence is range-scoped. It is not a requirement to stop unrelated
ranges, although an adapter's locking or execution model can impose additional
serialization.

### 3.2 Abort and retries

Before Commit, the coordinator may decide Abort. A retains its authoritative
data and returns to Serving; B remains nonserving and cleans its staging range.
Both participants still have to acknowledge their required terminal work.
After Commit, the protocol must finish the committed decision, not roll it back.

Messages can be delayed, duplicated, or reordered. Each participant checks the
plan, generation, role, copy round, and terminal fence. A late packet cannot
become current by looking up a newer route or changing its generation. Retained
nonce outcomes let the caller discover what happened after a lost response.

An important race is allowed: B can finish its previously authorized final copy
after the coordinator has aborted but before B receives Abort. B's Ready state
is still private. That late Ready cannot reverse the retained Abort; B later
cleans the private copy.

The native transition sources are [the coordinator](../../src/cluster/migration.rs)
and [participant role transitions](../../src/cluster/participant_metadata.rs).

## 4. Integration with application transactions

### 4.1 Does migration require multi-key transactions?

**Not to perform the move.** Migration does not invoke a transaction that reads
or writes all migrating keys atomically, nor a transaction that atomically
updates both source and destination.

However, Mako already supports application transactions touching multiple keys
and owners. Migration must preserve their correctness. The transaction engine
still owns read validation, commit/abort decisions, atomic application effects,
and terminal peer cleanup. The migration layer owns admission, placement
fencing, and waiting until those accesses no longer depend on the old placement.

For a store exposing only single-key application operations, the analogous
obligation would be to register and drain those in-flight operations. That does
not by itself satisfy the current durable library's metadata and engine APIs;
we have not supplied a separate minimal single-key-only adapter.

### 4.2 Epoch-cached admission and transaction lifetime

Before metadata or physical-handle use, an operation enters STO's existing
epoch domain: sample the global epoch, publish its worker announcement, recheck
the global epoch, and retry entry if it changed. Correct atomic ordering is
required; plain shared epoch fields are insufficient.

At entry, the common cache check compares **two integers**:

```text
entry_epoch == worker.cached_epoch
```

A match reuses the worker's immutable metadata object. A miss loads the published
object and updates the worker-local cache. It does not compare routing tables,
scan metadata on every transaction, or read a second global authority counter.
Periodic STO ticks can cause refresh even without migration. Retained ownership
keeps idle caches memory-safe; a hit should allocate nothing and increment no
shared reference count.

The captured view describes the complete owner-local Serving/Frozen geometry,
including absent keys, unbounded tails and every row alias at a warehouse
coordinate. Point/range authorization reads this immutable view, not a mutable
participant registry. A peer-supplied snapshot number is not proof: the receiver
checks the full owner/version grant and the complete requested scope. An older
routing version can still name an unchanged grant. A lower-endpoint point grant
never certifies the rest of a scan.

Point resolution shares a captured route across TPC-C handle selection and local
storage, not a full wire request. A thin point retains the selected index and a
route reference for immediate dispatch. Remote dispatch alone materializes its
envelope, combining the captured grant with the current transaction identity.
Bindings retain physical index ownership; commit batches materialize each row's
own transaction-scoped capture. Incoming requests cannot replace a live logical
identity or silently recapture a newer grant. Existing session/sequence/terminal
response fences remain in force.

#### What stays protected

The engine guard covers physical effects, unlocks, cleanup and end callbacks.
The logical context retains an additional guard across OCC attempts, RPC waits
and terminal participant settlement. Nested guards conservatively retain the
oldest worker epoch until all guards leave. An idle helper placeholder has no
admitted work and must not pin the process indefinitely; real work enters
before any metadata/storage access.
Helper mode alone does not make a stopped engine attempt active. An Abort RPC
arriving after an engine-local abort retains the logical identity through
terminal cleanup without re-entering the stopped engine.
A failed batch rolls back previously staged rows and does not attempt the
remaining rows; its terminal response follows physical cleanup.

```text
Unsafe: release old pin -> copy A -> publish B -> old write reaches A
Safe:   publish Frozen -> cut -> effects/cleanup -> release pin -> final copy
```

A commit decision, first unlock or lost RPC reply is not sufficient completion.
An already admitted transaction may access another coordinate covered by its
retained view, and may commit after arbitrarily many epoch ticks. **Commit does
not compare its entry epoch with the current epoch.** Ordinary STO/OCC still
checks data conflicts.

Frozen publication and the cut are distinct. A pre-cut transaction may reuse
old cached Serving metadata after publication; migration accounts for it by
waiting for the cut. A thread that sampled the old epoch but announced too late
cannot pass the entry recheck. A request that reaches a receiver only after the
cut must acquire fresh receiver protection; client start time grants no exception.

#### Remaining overhead and proof boundary

This removes per-key shared lease registration, the participant admission mutex
and per-access journal writes from the target native path. It does **not** remove
ordinary STO validation/write locks, routing and handle work, cache misses, or
properly ordered epoch entry/exit. The goal is a fixed, amortized transaction
entry cost instead of shared synchronization on every data access—not zero cost.

Native publication must precede the same-clock cut; every affected worker must
be included, including colocated owners and suspended participant transactions.
The migration waiter must not hold the old guard it waits for. Silo/Masstree
physical reclamation remains a separate obligation.

The independent Verus model includes retained epochs, advisory pre-copy
observations, close/join and exact final replacement. It does not prove the
foreign C++ atomics, physical row locks, native worker queue or RPC bodies.
Native smokes exercise retained retries, idle workers, late requests, actual
post-Freeze effects/cleanup, precopy fencing, exact deletion and repeated moves.
The conditional durable lease proofs retain their original settlement contract;
an empty production lease registry is never evidence of an epoch drain.

#### Direct local range scans

Native-enabled scans no longer round-trip local rows through the page codec.
The immutable snapshot visitor selects each homogeneous segment once. If its
owner is the current transaction participant, the adapter resolves the canonical
physical index, admits the entire captured segment, and invokes MassTrans
directly. Warehouse-index admission uses the fixed warehouse coordinate.
The logical context's oldest STO guard remains announced through transaction
cleanup. Captured metadata authorizes the interval without a shared lease
registration or participant admission mutex.

Remote segments still paginate through their actual participant, including
another shard in the same process. Local and remote segments share one capture,
direction, callback and caller arena; stopping a callback stops the traversal.
The direct path preserves bounds, metadata stripping and OCC validation without
local page allocation or serialization. A routing-version comparison alone
would be unsafe during the Freeze-before-publication window described above.

The native smoke gate exercises real MassTrans/STO and SRPC: local rows larger
than a transport page, binary/empty bounds, retained arena values, callback
stop/exception cleanup, an interleaved OCC conflict, Frozen/stale rejection, and
complete local/remote/local scans in both directions after a real migration.
`test_mako_native_mixed_scan` runs in a separate process from the static-routing
suite because native actors are process-global. The direct and mixed scan
theorems retain explicit foreign engine/ABI contracts.

#### Engine-epoch comparison (2026-10-21)

A fresh matched window compared the previous lease-native direct-scan binary
with the epoch binary, both native-enabled and disabled. All 12 cases passed:
one excluded warmup and three measured runs per arm, with both shard processes
exiting zero and strict startup, geometry and report validation.

| Arm | Run 1 TPS | Run 2 TPS | Run 3 TPS | Median TPS |
|---|---:|---:|---:|---:|
| Previous lease-native | 97,313.1 | 103,341.0 | 99,165.0 | 99,165.0 |
| Engine-epoch native | 122,720.7 | 133,972.6 | 133,287.4 | 133,287.4 |
| Same epoch binary, sharding disabled | 163,537.4 | 163,525.4 | 164,439.3 | 163,537.4 |

The native median improved **34.41%** over the lease build in this window.
It remains **18.50% below disabled**, or 81.50% of disabled throughput.
These are end-to-end results including the helper/batch lifecycle corrections,
not an isolated estimate of cache or epoch-instruction cost. Per-shard reported
`agg_abort_rate` ranges were 17.48–23.61, 28.79–37.26 and 47.51–50.35 aborts/sec
respectively; raw transaction/remote metrics are retained with every run.

Method: Docker on shared `zoo-002`, AMD EPYC 7702P (64 cores/128 threads),
approximately 503 GiB RAM, container CPU set `0-63`, and localhost SRPC.
Two concurrent shard processes each used eight workers and eight warehouses,
the original 45/43/4/4/4 TPC-C mix, normal remote NewOrder/Payment, and 30-second
loops. TPS is the sum of the two shard reports. Replication, migration,
persistence and timing instrumentation were disabled; no build or verification
job was started alongside the timing window. Cases ran serially with arm order
lease/native/disabled in round 1, disabled/lease/native in round 2, and
native/disabled/lease in round 3.

The unchanged configuration was `src/mako/config/local-shards2-warehouses8.yml`
(SHA-256 `d54154290fb70b743931076cac4bc4384c2dd93d5d86241a7c8bae3b069d44ca`).
Each shard ran `dbtest --num-threads 8 --shard-index 0|1 --shard-config
CASE/work/shards.yml -P localhost --sync-dir CASE/work/sync`, with a fresh
case environment and `MAKO_CLUSTER_CONFIG=1` or `0`. The plan retains exact
argv/environment, thread-affinity snapshots, host load, compiler commands,
linked-library hashes and Docker image identity.

Evidence root: `zoo-002:/home/users/shuai/mako-native-epoch.WfX0NaeP/`.
`comparison-3/plan.json`, `comparison-3/summary.json`, all per-case logs and
`epoch_compare_v3.py` retain the complete successful window. Frozen binaries:

- `comparison-3/binaries/lease-dbtest`:
  `253bc26eb3eddd0ddbd4d19eb0d297e7c359630b7f21538c312b85810737a6a9`
- `comparison-3/binaries/epoch-dbtest` (both epoch arms):
  `39bd883b7d5ffe2dbeb363d754887d44590d8cd6a8ba988f0ab9a47960d2dc49`

`source-manifest-v3.json`, `source-v3.patch` and `source-v3-files.tar` preserve
the verified pre-timing source; all 78 file checksums passed before timing.
`benchmark-3.json` records successful completion and log SHA-256
`28bceaf0187ba81461ab0d81c1a11ee0ad108fc45de52a7ec1270ece46f98a8c`.
The earlier `comparison/` and `comparison-2/` windows failed during the native
warmup, each with **zero measured cases**; both remain preserved alongside the
failing-before regressions and fixes. No successful measured case was replaced.

Three samples on a shared host do not establish an isolated or universal
speedup. This window measures steady-state admission, **not migration pause
time, copy bandwidth, WAN behavior, replicated migration or crash durability**.
Correctness/proof evidence is recorded separately in
[the checked-results record](../../tla/mako/README.md#native-engine-epoch-cutover-2026-10-21).
The historical optimization sequence below predates this cutover and is not its control arm.

#### Prepared workload routing comparison (2026-10-22)

The first prepared-routing optimization targeted repeated capture/validation
work: TPC-C precomputed table/warehouse descriptors, bound each to its
transaction's capture slot, and passed that fixed-coordinate capture to the
following storage verb. Immutable local points replaced repeated request-byte
comparisons with private context/slot checks. Binding-cache lifetime and
handle-selection policy were unchanged in that comparison.

A fresh interleaved window compared the previous epoch binary against the
prepared-routing binary, native-enabled and disabled. All 12 cases passed:
one excluded warmup and three measured runs per arm.

| Arm | Run 1 TPS | Run 2 TPS | Run 3 TPS | Median TPS |
|---|---:|---:|---:|---:|
| Previous epoch-native | 133,632.8 | 132,934.0 | 130,286.1 | 132,934.0 |
| Prepared-routing native | 129,690.1 | 137,446.5 | 137,452.5 | 137,446.5 |
| Same prepared binary, sharding disabled | 162,663.4 | 164,058.8 | 161,909.3 | 162,663.4 |

The native median increased **3.39%**. Its gap to the disabled median fell
from **18.28% to 15.50%**, not to zero. The prepared arm's run range overlaps
the control's; three samples on this shared host do not establish a universal
speedup or statistical confidence interval.

The host, image, CPU set, configuration hash, two-shard/eight-worker/eight-
warehouse geometry, normal remote mix and 30-second loops match the preceding
method. Measured order was baseline/prepared/disabled,
disabled/baseline/prepared, then prepared/disabled/baseline. No profiling,
replication, migration or persistence was enabled during timing, and no build
or verification job was started alongside it.

Under the same evidence root, `comparison-prepared-v1/{plan,summary}.json`
retains complete cases, environments, process exits, metrics and library hashes.
`prepared_compare_v1.py` is the driver; `prepared-benchmark-1.json` records
successful completion and log SHA-256
`3c448bad39d2ffdece6c90529eee592cb25b40bd6b1803763468ea7db16f45b6`.
The frozen baseline retains SHA-256
`39bd883b7d5ffe2dbeb363d754887d44590d8cd6a8ba988f0ab9a47960d2dc49`;
the prepared binary is
`8c788963dc257ea271067fb1fa48a87eaaa85c8836ebf2601b9d6383311c1c87`.
`source-manifest-prepared-v4.json`, `mako-source-prepared-v4.patch` and
`mako-source-prepared-v4-files.tar` preserve the 79-file pre-timing snapshot.
[Correctness evidence](../../tla/mako/README.md#prepared-workload-routing-2026-10-22)
is separate from these steady-state throughput measurements.

Separate CPU profiles used the same frozen binaries: two captures per arm,
99 Hz `cycles:u`, 20-second windows, DWARF call chains and 16 MiB ring buffers.
All six cases passed, with **156,837 recorded samples and no reported loss**.
Nearest-matching-frame attribution assigns each sample once; every case's
sample count and total period matched an independent `perf report` aggregation.

| Category | Before: extra sampled cycles/commit | After: extra sampled cycles/commit | Change |
|---|---:|---:|---:|
| Capture/validation | 9,899 | 5,266 | −46.8% |
| Binding lookup | 6,315 | 6,346 | +0.5% |
| Handle selection | 2,505 | 2,622 | +4.7% |

These are mean native-minus-disabled sample-period estimates, normalized by
reported TPS × 20 seconds, not exact window commit counts, wall time, or
causal throughput percentage points. Binding lookup is now the largest of
these remaining CPU costs. First capture, native authorization and liveness
checks remain; precomputing routing inputs does not eliminate all admission
work. The other cache policies were unchanged; their sample differences are
not isolated causal effects.

`profiles-prepared-v2/` retains the accepted capture window;
`profiles-prepared-attribution-v2/` retains disjoint attribution, compressed
stacks and independent count/period checks. `prepared-results-v1.json`
combines timing, CPU estimates and verification references. The first complete
profile window, `profiles-prepared-v1/`, reported lost chunks in three cases
and was excluded from quantitative comparison. All six cases were repeated
with larger buffers; no individual successful case was substituted.

#### View-tagged authorization reuse design

Extend the protected-view cache from routing/handle reuse to **successful
ownership-decision reuse**. Transaction entry already compares
`mako_sharding_read_view_id` with the TLS cache identity. An equal ID retains
the fixed entries' successful decisions; a different ID discards them before
any reuse. A new transaction rebuilds its immutable point/request identity,
not the unchanged ownership decision. Only a successful native point check may
set the cached decision.

The key is the complete captured Node/admission view plus the exact
table/coordinate/grant, not a global routing label. Frozen metadata can differ
while routing labels remain equal. This is memoization of the pure captured
ownership predicate, not a new authority clock or permission to bypass engine
guards. Already-pinned old views remain valid through their existing drain
contract; no current-version equality is checked during access or commit.

Raw-key captures and received-envelope authorization keep their own lifetimes.
The boundary-only cutover below removes repeated local descriptor validation
and moves availability errors to new-context admission. The 2026-10-23
comparison measured ownership-decision reuse alone, before that later cutover.
The detailed invariant and regression obligations are in the
[storage contract](../storage-interface.md#view-tagged-ownership-authorization).

#### Cross-transaction prepared routing comparison (2026-10-22)

The stronger cache retains fixed-coordinate routes, immutable table bindings
and selected physical/proxy handles across transactions. Its private locator is
tagged by the protected native read's full-view identity, not by transaction ID,
engine epoch or routing version. An unchanged view survives timer ticks;
Freeze, owner changes and conservative metadata republication invalidate it.
That version still enters guards and rechecks native ownership in each new transaction.
Raw-key captures, received envelopes and batch provenance remain
transaction-scoped. See the [cache contract](../storage-interface.md#tpc-c-warehouse-handles)
and [real-engine correctness evidence](../../tla/mako/README.md#cross-transaction-prepared-routing-2026-10-22).

A fresh 12-case matched window passed, with one excluded warmup and three
measured runs per arm:

| Arm | Run 1 TPS | Run 2 TPS | Run 3 TPS | Median TPS |
|---|---:|---:|---:|---:|
| Previous per-transaction prepared cache | 137,418.1 | 137,356.6 | 136,971.1 | 137,356.6 |
| Cross-transaction prepared cache | 154,585.3 | 154,528.0 | 154,496.3 | 154,528.0 |
| Same new binary, sharding disabled | 164,715.1 | 164,706.9 | 164,630.3 | 164,706.9 |

The native median improved **12.50%**. Relative to the disabled median in this
same window, the gap fell from **16.61% to 6.18%**, closing **62.78%** of that
gap. The earlier window's 15.50% gap used a different disabled median; it is
not the denominator for this comparison. All three new-cache runs exceeded
all three previous-cache runs, but three measurements on a shared host are
not a universal speedup guarantee or a statistical confidence interval.

Conditions match the preceding comparison: zoo-002 Docker, CPUs 0–63, two
shards, eight workers and eight warehouses per shard, normal remote accesses,
45/43/4/4/4 TPC-C mix, 30-second loops. Measured order was
baseline/new/disabled, disabled/baseline/new, then new/disabled/baseline.
Profiling, replication, migration and persistence were disabled during timing.
No build, verification or profiling job ran alongside the timing window.

Separate profiles used the same frozen binaries, two captures per arm,
99 Hz `cycles:u`, 20-second windows, DWARF stacks and 16 MiB buffers. All six
captures completed with **157,217 samples** and no reported lost chunks.
Disjoint nearest-frame attribution matched independent `perf report` sample
counts and periods exactly in every capture:

| Targeted overhead | Previous extra cycles/commit | New extra cycles/commit | Reduction |
|---|---:|---:|---:|
| Capture, descriptor validation and admission/liveness | 5,324.55 | 4,964.68 | 6.76% |
| Binding/registry work | 6,198.61 | 1,151.39 | 81.43% |
| Warehouse-directory handle selection | 2,657.11 | 0 sampled | No samples remained |

These are mean native-minus-disabled sampled-cycle estimates, divided by
reported aggregate TPS times 20 seconds—not exact window commit counts,
wall time, or causal throughput percentage points. Zero samples does not
prove zero cost. The largest remaining named sharding cost is
admission/liveness and fresh transaction-point handling. Remaining binding
samples include `TableRegistry::native_handle`, `native_binding` and the TLS
binding lookup; the receiver path still selects and validates its actual
physical table. Storage/transport deltas are retained in the full attribution
rather than assigned to the cache as isolated causal effects.

Evidence under `/home/users/shuai/mako-native-epoch.WfX0NaeP` on zoo-002:
`comparison-cross-v1/{plan,summary}.json`, `cross_compare_v1.py`,
`profiles-cross-v1/`, `profiles-cross-attribution-v1/` and
`cross-results-v1.json`. `prepared-cross-benchmark-1.json` records success
with log SHA-256
`985d576dba168ff44565b2c210b129c00ff7795d4edd8c39984e04a24a6ffe9a`.
The previous prepared binary retains SHA-256
`8c788963dc257ea271067fb1fa48a87eaaa85c8836ebf2601b9d6383311c1c87`;
the new binary is
`744d128656d3e8852f7c7327f9c3a686c0323ff7080d676916d227bb61b7da38`.
`source-manifest-cross-v2.json`, `mako-source-cross-v2.patch` and
`mako-source-cross-v2-files.tar` preserve the 79-file pre-timing source
snapshot. Final result documentation is archived separately.

#### Protected-view ownership memoization comparison (2026-10-23)

The bridge now preserves `CapturedRoute::ownership_authorized` when rebuilding
a fixed entry's fresh transaction point. The existing transaction-entry integer
comparison of protected view IDs invalidates the whole cache on a view change.
Only successful ownership checks are retained; no extra version counter or
per-operation generation comparison was added. Transaction provenance,
runtime liveness/readiness and received-envelope checks remain in force.
See the [design above](#view-tagged-authorization-reuse-design) and
[fresh correctness evidence](../../tla/mako/README.md#protected-view-ownership-memoization-2026-10-23).

All 12 matched timing cases passed: one excluded warmup and three measured
runs per arm, interleaved under the same conditions as the preceding comparison
(zoo-002 Docker, CPUs 0–63, two shards, eight workers and eight warehouses per
shard, normal remote accesses, 45/43/4/4/4 mix, 30-second loops). No build,
verification or profiling job ran alongside timing; replication, migration,
persistence and profiling were disabled.

| Arm | Run 1 TPS | Run 2 TPS | Run 3 TPS | Median TPS |
|---|---:|---:|---:|---:|
| Previous cross-transaction route/handle cache | 154,715.7 | 154,376.5 | 154,745.0 | 154,715.7 |
| Plus protected-view ownership memoization | 155,284.4 | 155,795.0 | 155,349.2 | 155,349.2 |
| Same new binary, sharding disabled | 164,251.9 | 164,115.8 | 163,689.9 | 164,115.8 |

The measured native median improved **0.41%**. Against this window's disabled
median, the gap fell from **5.73% to 5.34%**. The preceding experiment's 6.18%
gap used a different disabled median and is not the denominator here. All
three new runs exceeded all three baseline runs, but the improvement is small;
three measurements on a shared host do not establish a universal speedup or
a statistical confidence interval.

A separate complete six-capture profile window used the same frozen binaries:
two captures per arm, 99 Hz `cycles:u`, 20-second windows, DWARF call chains
and 16 MiB buffers. It recorded **159,052 samples**, with no reported lost
chunks. Disjoint nearest-frame attribution matched independent `perf report`
sample counts and total periods exactly for every capture. This comparison
splits the pure `AdmissionSnapshot::point` ownership predicate out of the
previous broader capture/validation bucket:

| Sharding overhead | Before: extra sampled cycles/commit | After: extra sampled cycles/commit |
|---|---:|---:|
| Pure ownership predicate | 767.57 | 263.98 |
| Other capture, point validation and runtime checks | 3,985.41 | 3,844.39 |
| Binding/registry work | 1,191.17 | 1,178.28 |
| Epoch/context work | 233.58 | 271.42 |

Ownership-predicate samples fell **65.61%**; the combined ownership plus
capture/validation bucket fell from **4,752.97 to 4,108.37** extra sampled
cycles/commit. Most of that larger bucket remains: this change does not remove
fresh point construction, transaction checks or runtime fencing. Raw-key and
received-envelope authorizations deliberately retain their previous lifetimes.
These are mean native-minus-disabled sample-period estimates normalized by
reported TPS × 20 seconds, not exact window commit counts, wall time, or
causal throughput percentage points. Differences in unchanged categories are
not isolated effects of this optimization.

The first profile window was excluded in full: its third case, the
sharding-disabled control, segfaulted during worker startup before profiler
attachment. The preserved core faults in `shardClientAll`'s `unordered_map`
rehash at `src/mako/benchmarks/bench.cc:281`, where workers insert into the
shared map without synchronization. That startup path is unchanged by this
optimization. No workaround or startup fix was mixed into the comparison.
The entire six-capture window was repeated with the same binaries and settings;
all six replacement captures passed. The startup race remains an unresolved,
separate risk, not an authorization-cache failure hidden from the results.

Evidence under `/home/users/shuai/mako-native-epoch.WfX0NaeP` on zoo-002:
`comparison-auth-v1/{plan,summary}.json`, `auth_compare_v1.py`,
`profiles-auth-v2/`, `profiles-auth-attribution-v2/` and `auth-results-v1.json`.
The excluded `profiles-auth-v1/`, `auth-profile-crash-v1.core` and
`prepared-auth-crash-analysis-1.{json,log}` retain the failure and postmortem.
`prepared-auth-benchmark-1.json` records timing success with log SHA-256
`90d9ebc067cea4c2ed66dea6053f84b02a600f60e422462375e29ea9e027a2aa`.
The previous binary is
`744d128656d3e8852f7c7327f9c3a686c0323ff7080d676916d227bb61b7da38`;
the new binary is
`142a5801020f87dac5c8e0d7062c73c8fd0cf530255685896bb55ed7b4110d21`.
`source-manifest-auth-v1.json`, `mako-source-auth-v1.patch` and
`mako-source-auth-v1-files.tar` preserve the 79-file pre-timing tree.
`mako-auth-final-docs.tar` separately preserves the final results documentation.

#### Boundary-only access admission (2026-10-24)

This stage removed the API that accepted a returned descriptor as authority and
then repeatedly checked whether it belonged to the current transaction.
`sharding_resolve_point` selects the current private capture, authorizes a local
target, and returns the routing result for immediate dispatch. The separate
`sharding_admit_point` API was removed. At this stage, cache-entry revisions
tracked lazy request refresh and transaction-scoped batch membership. The
subsequent thin-descriptor change removes request refresh as well; its private
slot is a route address for serialization, never an admission credential.
Received requests still validate their actual coordinate, grant and physical
owner at the receiver boundary.

Availability is established when a **new logical transaction/request context**
enters `mako_sharding_read_begin`, including cache hits. Initialization, stop,
metadata-failure and metadata-only recovery errors reject new admissions there.
An already-admitted context does not poll service flags on each access:
`mako_sharding_read_live` is removed, and protected native reads/view-ID retrieval
do not repeat Node readiness checks. Storage, transport and cold resource
acquisition remain fallible through their existing error paths.

Logical service stop and physical engine destruction are distinct. Admitted
work may finish against its protected metadata while the engine remains alive;
owners must finish/join that work before destroying host/index storage.
Production benchmark workers join and quiesce before native service teardown.
Migration still uses its existing publication/cut/drain protocol; this change
adds no clock, lease, commit-time version comparison or failure-recovery claim.

[Fresh correctness evidence](../../tla/mako/README.md#boundary-only-access-admission-2026-10-24)
covers actual engine read/commit after logical service stop, rejection of fresh
work afterward, receiver forgery rejection and the existing migration cases.
The preceding throughput/profile tables describe frozen earlier binaries.
Subsequent boundary-only timings are recorded in
`boundary-performance-results-v1.json`; the staged comparison below uses a new
common measurement window rather than mixing medians from separate windows.

#### Thin descriptors and indexed bindings (2026-10-24)

Two native engine/registry bridge optimizations preserve the same Rust ownership
predicate, protected-view lifetime and receiver validation:

1. **Thin local descriptors, lazy wire envelopes.** Local dispatch carries the
   selected index plus a tagged route reference. Fixed/raw captures use stable
   slot indices, not pointers into movable vectors; unmanaged keys are borrowed
   only for immediate dispatch. A selected capture publishes its handle only
   after local authorization succeeds, so a warm hit does not repeat admission.
   `ShardingPoint::write_request` materializes the full envelope directly at
   remote dispatch. Transaction identity comes from the current context; the
   owner/version grant comes from the capture. Deferred rows require their own
   capture from that transaction. Control RPCs no longer inherit a last-row
   envelope, and explicit scan envelopes remain explicit.
2. **Shared indexed binding and physical mapping caches.** Point, scan and
   receiver paths reuse the same view-scoped mappings. Prepared/received
   accesses keep direct immutable binding pointers; integer-keyed Rust maps
   retain the binding owners. Raw physical mappings are table-scoped, not
   row-key-scoped. Fixed mappings additionally key the exact coordinate, whose
   bytes belong to a retained immutable binding. The view identifies the local
   participant. Warm hits avoid registry locking, name lookup, allocation and
   `Arc` cloning. Cold misses use the existing registry; negative results are
   not cached. A changed view clears borrowed mappings before binding owners.
   A mapping hit never grants ownership or bypasses incoming-envelope checks.

The [storage contract](../storage-interface.md#tpcc-warehouse-handles) specifies
the lifetimes and invalidation rules.
[Fresh runtime evidence](../../tla/mako/README.md#thin-descriptors-and-indexed-bindings-2026-10-24)
covers deferred serialization through capture-vector growth, interleaved raw
and warehouse scopes, warmed receiver mappings, malformed/forged requests,
Freeze, ownership migration and unaffected raw keys. Formal roots did not
change and were not rerun.

The preliminary descriptor-only window passed all 24 timing cases. Native
median throughput improved from **155,981.9** to **158,777.2 txn/s** (**1.79%**);
the disabled control improved **0.34%**, and the own-control gap moved from
**4.56%** to **3.18%**. Baseline native runs ranged from **145,799.3** to
**156,527.4 txn/s**. No slow trials were excluded. The final comparison uses all
three frozen versions and their own disabled controls in two complete windows.
The preliminary record is `comparison-descriptor-v1/{plan,summary,totals}.json`.

##### Matched throughput and replication

Both six-arm windows passed all **72 cases**: one excluded warmup and five
measured runs per arm per window, giving **10 measured runs per arm**. Window 2
reverses the measured ordering of window 1. The preliminary descriptor-only
window above is preserved separately and is not pooled into this comparison.
All runs used Docker on zoo-002, CPUs 0–63, two shards, eight workers and eight
warehouses per shard, the normal remote TPC-C mix **45/43/4/4/4**, and 30-second
loops. Replication, migration, persistence and timing instrumentation were off.
No build, test, profile or attribution job overlapped a timing window. Binary
and library identities were checked for every case.

Pooled medians; gap is `100 × (1 − native / own disabled control)`:

| Version | Native txn/s | Disabled-control txn/s | Native below own control |
|---|---:|---:|---:|
| Boundary-only baseline | 156,260.05 | 163,906.85 | 4.67% |
| Thin descriptors only | 154,440.60 | 161,722.20 | 4.50% |
| Both optimizations | 158,016.90 | 163,227.65 | 3.19% |

The combined native median improved **1.12%** over the baseline. Descriptor-only
throughput did **not** improve consistently: its pooled native median fell
**1.16%**. The combined version was **2.32%** above descriptor-only, but that is
not a clean causal estimate of the binding cache's contribution.

| Complete window | Descriptor native gain | Combined native gain | Combined control change | Combined own-control gap |
|---|---:|---:|---:|---:|
| 1 | +0.28% | +1.44% | −1.06% | 1.85% |
| 2, counterbalanced | −2.39% | +0.66% | +2.37% | 4.77% |

Gains/control changes in this table compare each version's median with the
baseline version's corresponding median in that same window. The first
window's **1.85%** gap is not a stable result: control drift materially changes
the ratio. Pooled, the combined control is **0.41% slower** than the baseline
control; comparing combined native throughput with the original control instead
gives a **3.59%** gap. The defensible conclusion is a modest observed combined
throughput gain and a remaining gap of roughly **3%**, not a large demonstrated
win from both individual changes. No slow trials were removed, and these
two-window ranges are not confidence intervals.

`comparison-optimized-v1/` and `comparison-optimized-v2/` retain each plan,
every run, raw logs, frozen binaries/libraries and totals. Under
`/home/users/shuai/mako-native-epoch.WfX0NaeP` on zoo-002,
`optimized-results-v1.json` binds the pooled calculations to the source
manifests, binary/driver/configuration identities, verification receipts and
CPU attribution. `mako-optimized-final-docs.tar` preserves the final
documentation separately from the frozen measurement inputs.

##### Separate CPU attribution

Twelve separate captures (two per version/control arm) completed with no loss
reported in their `perf record` logs. Sampling used `cycles:u` at 99 Hz,
20-second windows, DWARF stacks of 8,192 bytes and 16 MiB ring buffers. Each
sample period is assigned once to its nearest matching frame; all twelve
sample counts and period totals matched independent `perf report` totals.
Normalization uses reported aggregate TPS times nominal 20 seconds, **not exact
profile-window commit counts**. These are estimated CPU cycles per commit,
not wall-time costs or additive throughput percentage points.

| Native minus its own disabled control | Boundary-only | Thin descriptors | Both optimizations |
|---|---:|---:|---:|
| Route capture, dispatch and envelopes | 2,342 | 1,733 | 1,578 |
| Binding/registry/mapping lookup | 1,146 | 1,143 | 1,119 |
| Ownership predicate | 238 | 292 | 317 |
| Epoch/context | 176 | 116 | 32 |
| Storage | 188 | 2,321 | 1,529 |
| Transport | 538 | 1,246 | 936 |
| Other/unattributed | 138 | 625 | 473 |
| Total sampled differential | 4,766 | 7,475 | 5,984 |

The targeted capture/dispatch/envelope differential fell **32.6%**. Both native
and disabled paths benefited; subtracting the control leaves **764** estimated
cycles/commit saved.
The historical machine label `capture_validation` includes routing and envelope
work; it does **not** mean the removed per-operation identity checks remain.
The remaining native bucket is dominated by `sharding_warehouse_handle` and
`sharding_resolve_point`. No samples were attributed to cold warehouse-directory
selection in these captures.

The indexed cache did **not** demonstrate a large binding-cost reduction: that
bucket moved only **2.4%**. Hot frames moved from
`TableRegistry::native_binding`/`native_handle` to `sharding_physical_handle`,
the integer binding cache and hashbrown lookup/callback machinery. This avoids
warm registry locking and owner-reference updates, but lookup itself still costs
CPU. It would be incorrect to claim the old binding cost was eliminated.

The unchanged storage/transport buckets moved substantially. Total native
sampled CPU **increased by 755 cycles/commit** in this profile window despite
improved uninstrumented throughput. The profiles support
the targeted capture reduction, not an aggregate CPU-efficiency improvement or
a complete causal decomposition of the throughput gap. The ownership-predicate
increase is not evidence of an added ownership check; that predicate is unchanged.

Detailed evidence is `profiles-optimized-v1/`,
`optimized-profile-loss-v1.json` and
`profiles-optimized-attribution-v1/{summary,totals}.json`, including compressed
per-sample stacks and the conserved attribution rules.

#### Measured optimization sequence (2026-10-14)

Docker measurements on zoo-002 used two shards, eight workers and eight
warehouses per shard, the original 45/43/4/4/4 TPC-C mix and 30-second loops.
Each primary arm has five measured runs; warmups and separate profiles are
excluded. Replication, migration, persistence and instrumentation were disabled.

| Cumulative change | Native median TPS | Native step delta | Disabled median TPS | Disabled step delta |
|---|---:|---:|---:|---:|
| Shared-read routing/warehouse caches | 38,315.5 | +40.64% | 133,227.6 | +0.04% |
| Physically-local disabled scan elision | 38,239.5 | -0.31% | 165,163.8 | +24.29% |
| Operation binding/routing/handle reuse | 70,715.2 | +84.40% | 164,674.2 | -0.22% |
| Transaction-retained participant Node | 92,291.9 | +30.02% | 165,013.7 | +0.41% |
| Checked admission hints, scalar boundary and inlining | 109,585.3 | +1.60% | 181,506.2 | -1.63% |

Each delta uses its own interleaved preceding-stage control, **not** the previous
row's median. In the final window those controls were 107,855.4 native and
184,516.9 disabled TPS. The original baseline measured 27,236.9 native and
152,535.7 disabled TPS in that same window: final ratios were **4.02x native**
and **1.19x disabled**. Shared-host conditions changed substantially; final native
runs ranged from 93,213.4 to 113,089.4 TPS. The admission-hint delta is small and
noisy, not evidence of a stable isolated speedup.

Separate profiles estimated native instructions/commit at 445k, 447k, 305k, 289k
and 287k across these stages. Disabled scan elision reduced its estimate from
217k to 111k. These divide all-process counters by reported TPS times a nominal
20-second window, not exact window commit counts. Final native hot paths still
include point admission (17.62% inclusive) and paged scans (23.86% inclusive).
Fixed-eight-warehouse local-only diagnostics scaled from 115,173 to 94,256 TPS
when workers/shard increased from four to eight; disabled scaled from 411,747
to 776,230. Native scaling remains a limitation.

The first hint implementation regressed 4.78% and was revised before acceptance.
Its failed disabled profile (98.50% sample loss) is excluded. An original-baseline
warmup also hung before one shard started; it is preserved with one explicitly
recorded replacement warmup. No successful or measured case was rerun. Earlier
invalid diagnostic geometry and a parser-only failure retain their original
records alongside corrected diagnostics/reparsed output.

Reproduction artifacts live at
`/home/users/shuai/mako-sharding-fixes.ATINk42Z`: immutable stage binaries and
source/build closures, raw runs, separate profiles, `verification-05-v2.json`,
and `final-results-05-admission-reuse-v2.json`. That benchmark's accepted binary
is `stages/05-admission-reuse-v2/dbtest`, SHA-256
`f07e6fe6e6fccbb6cfcf9144bb6f1501be072e1e6ca45dea1a51188768d92da6`.

### 4.3 Additional TP requirements for crash recovery

The durable path retains unfinished identities and leases. Its TP adapter must:

- Keep transaction IDs stable across participants and restarts; no worker-ID
  reset may make an old transaction appear new.
- Settle the exact recorded effects and terminal outcome, and exclude later
  effects from surviving peers or queued requests before releasing a lease.
- Fence even a journaled identity that never reached the engine. Local absence
  is not proof of completion; the contract permits a durable empty-abort fence
  only after old work is actually excluded.
- Authorize transaction enrollment through the metadata operation, including a
  late accepted operation whose reply was lost. A transaction settled elsewhere
  must not join a new owner afterward.

`DurableParticipant::finish` and `release` call
`DurableEngine::settle_transaction` before releasing the native lease. A local
`Resolve` record is not a substitute for that engine outcome. These are
**requirements on TP integration**, not a distributed transaction-recovery
implementation provided by the migration library.

## 5. Storage primitives required by the durable path

This section describes the separate durable library, not the current MBTA
production adapter.

| Primitive | Required guarantee | Does it require a multi-key application transaction? |
|---|---|---|
| Ordered range scan | Canonical row identities and values, strict cursor progression, explicit EOF, and no conversion of errors into successful partial coverage. | No. The source is stabilized by the protocol. |
| Row mutation | One atomic durable logical row/value-version effect; writes outside the selected range are preserved. | No whole-range transaction. Value/version coherence is the engine's responsibility. |
| Durability / settlement barrier | Establish which accepted I/O happened and exclude later application of old work. | No transaction query API, but stronger than treating a timeout as failure. |
| Journal append | Atomically persist the operation record and updated head in one authority namespace. | **Requires an atomic two-key metadata batch today**, not general transaction isolation or cross-shard commit. |
| Writer ownership and fencing | One serialized live writer per namespace; stale writers cannot apply after recovery. | Atomic batch writes alone do not provide this. Rust `&mut` is not distributed locking or leader election. |
| Transaction settlement | Preserve whatever application transaction semantics the embedding engine provides. The current interface carries exact write sets and outcomes. | For Mako's multi-key transactions, those existing TP guarantees remain required. |

The engine facade is exclusively borrowed during a transfer call. A successful
admission check must expose a coherent working/durable image with no unresolved
raw mutation. Wrapping a primary store that can mutate independently behind that
facade does not satisfy the contract. Physical table/coordinate/key bindings must
also survive reopening.

### 5.1 What the journal stores

Each coordinator or participant authority has a separate namespace. A head
stores the receiver admission incarnation and committed entry count. Numbered
entries contain versioned encodings of native inputs, starting with Bootstrap at
entry zero. They include migration decisions and receipts, plan bindings, lease
operations, transfer reservations, and completion records as appropriate to the
authority.

Replay rebuilds the existing native actor by executing those inputs. It does not
load a second executable protocol or accept the old actor's memory as recovery
input. The committed coordinator input determines both its decision and routing
snapshot; these are not independently published mutable records.

The metadata-only production coordinator uses this checked replay. Its
participant journal instead retains versioned **live bookkeeping**: successful
controls and transfer intent/result records. Historical lease records remain
inspectable but are no longer emitted. The separate format tag prevents treating
an observed live Ready/Done as a durable-row completion. Reopening validates the
prefix without reactivating the actor or restoring lost epoch protection. It is
not a substitute implementation of `DurableParticipant`.

### 5.2 The exact atomic-batch dependency

Using symbolic key names, suppose one journal contains:

```text
head = { incarnation: 3, count: 7 }
entry[0] ... entry[6] = committed operations
```

Appending operation X currently means one atomic durable batch:

```text
put entry[7] = X
put head = { incarnation: 3, count: 8 }
```

Recovery reads the head and replays exactly entries `0..count-1`. Atomicity makes
this append retain either the old prefix or the new prefix, never a head that
requires an uncommitted entry. The batch belongs to one authority; it does not
atomically update A's journal, B's journal, and the coordinator's journal.

This is a **current implementation choice**, not a claim that sharding inherently
needs multi-key transactions. An entry-first, single-key head-publication design
is a possible alternative: make the entry durable before publishing the head,
and explicitly handle unpublished tails, uncertain writes, and old-writer
fencing. That alternative is not implemented or proved here. Merely replacing
the batch with two arbitrary puts would not preserve the current contract.

### 5.3 Ordering across data and metadata

No atomic transaction spans the row store and all migration journals. Instead,
the durable wrappers enforce these orderings:

```text
durable transfer reservation -> row copy -> durable complete copy -> Ready record
Ready + Retired evidence -> durable Commit decision -> expose new routing
Commit/Abort cleanup obligation -> durable deletion -> Done record
```

A participant can become logically nonserving before its physical rows are
removed. That intermediate state is not Done. Likewise, partially copied rows
at B are not Ready and cannot be served.

The KV API distinguishes `Committed`, `NotCommitted`, and `Unknown`. Unknown is
not success or failure. Failed/unknown persistence fences the candidate wrapper;
recovery resolves the accepted work before execution continues. A coordinator
applies an operation privately, appends it, and only exposes the resulting actor
through its admitted API after success.

The concrete key encoding and full interface obligations are specified in
[Storage Interface: recovery scope](../storage-interface.md#recovery-and-progress-scope).
The implementation is [durable_kv.rs](../../src/cluster/durable_kv.rs) and
[durable_engine.rs](../../src/cluster/durable_engine.rs).

## 6. Crash recovery

The production metadata-only mode reopens its stores, validates the retained
identity/prefix, and reconstructs the coordinator for historical admin polling.
An identical Begin retry can return its retained outcome. It does **not** publish
recovered serving routes: readiness, read admission, capture, control/copy/cleanup,
new Begin and Abort remain fenced. Pending work stays pending; lost in-memory
rows and epoch announcements are not interpreted as completed transactions.
This applies after clean shutdown as well as process death.

The full recovery steps below apply only when the backend actually satisfies
both durable metadata and durable engine/TP contracts. Restoring journal bytes
beside lost in-memory primary rows is not valid data recovery.

### 6.1 Reconstruct, fence, then resume

1. **Stop using the old wrapper.** Do not reuse its cached actor, cursor, or
   admission after an uncertain persistence result or restart.
2. **Settle old I/O.** Participant recovery first restores authoritative rows and
   settles/excludes old raw copy/delete submissions. Journal recovery resolves
   accepted metadata batches before reading the definitive head.
3. **Read and replay the committed prefix.** Validate records, construct the
   actor from Bootstrap, and replay its native operations. A missing/malformed
   required record is an error, not permission to initialize fresh state.
4. **Persist a fresh admission incarnation.** Install it in the participant's
   recovered engine before returning an admission to callers. Settle a recovered
   transfer reservation before permitting replacement work. Never relabel an
   old queued RPC with the new admission.
5. **Keep unfinished access leases.** Enumerate old identities from committed
   Begin records with `next_recovery_transaction`. Settle them through the engine
   before `finish` releases their leases. Activation alone does not prove that
   the source range is drained.
6. **Resume the retained phase.** Retransmit missing controls, repeat authorized
   copy/cleanup when needed, and record actual certificates. Do not allocate a
   new move just because its previous reply was lost.

Recovered serving still depends on the participant's role and normal access/TP
authorization. Recovery does not turn Stage, Ready, Frozen, or Retired into
Serving merely by opening the database.

### 6.2 Work selected from the coordinator's phase

| Recovered phase | Next work |
|---|---|
| `Copy` | Ensure destination staging, then request freeze. No background bulk copy is implied. |
| `Freezing` | Continue source freeze/drain; advance only with Drained. |
| `Final` | Continue/retry final copy; advance only with Ready. |
| `Retiring` | Obtain/replay Retired; then request Commit. |
| `Committed` | Deliver the retained Commit and finish outstanding participant work. Never change it to Abort. |
| `Aborted` | Deliver the retained Abort, restore source service, and complete destination cleanup. |
| No active move | No migration work. Retained nonce outcomes remain queryable. |

`DurableCoordinator::resume` selects work; a driver performs it and journals the
result. It is not a background recovery service installed in production.

### 6.3 Representative crash boundaries

| Crash or uncertainty | Required recovery behavior |
|---|---|
| Some destination rows copied, no committed Ready | B stays private. Settle raw I/O and redo the authorized exact mirror; never infer completion from the rows that happen to exist. |
| Copy durable, completion append uncertain | Resolve the journal prefix. Retained completion can replay Ready; otherwise completion must be re-established through the native path. |
| Commit append reply lost | Resolve whether Commit entered the committed prefix before exposing routing or deciding what to retry. If committed, retain that decision. |
| Source deletion partly applied, no Done | Keep the committed route to B. Settle old deletion work and resume generation-scoped cleanup. |
| Begin/lease reply lost before engine registration | Discover the identity from the journal; require a real old-work fence before recording an empty abort and releasing its lease. |
| Old cleanup arrives after ownership returns to A | Reject it using the retained generation/terminal/admission fences; owner number equality is insufficient. |

Permanent failures, indefinitely delayed messages, or transactions that cannot
settle can block completion. There is no unconditional or bounded-time progress
guarantee.

## 7. Implemented versus conditional behavior

| Area | Status |
|---|---|
| Native coordinator, participant, immutable admission, exact ordered transfer, ownership versions, nonce retention | Implemented in Rust and connected to live MBTA/STO/srpc. Fresh Docker smoke exercises native epoch admission and pre-copy in separate-process and colocated-owner layouts. |
| Live production migration | Fixed live owners, one active move, non-replicated mode. Private online pre-copy precedes the same-clock Freeze/drain cut and complete final reconciliation. |
| Production metadata persistence | Implemented, opt-in via `MAKO_SHARD_METADATA_DIR`. [`rocks_metadata.rs`](../../src/cluster/rocks_metadata.rs) supplies synchronous atomic journal batches; [`metadata_coordinator.rs`](../../src/cluster/metadata_coordinator.rs) and [`metadata.rs`](../../src/cluster/metadata.rs) integrate coordinator replay and live participant bookkeeping. Reopened owners remain inspection-only. |
| Production crash-safe migration | **Not implemented.** Primary MBTA rows are in memory; raw migration uses `try_commit_no_paxos`. Existing RocksDB logs do not satisfy the new authoritative-row/recovery contracts. |
| Durable journal, actor replay, admissions, phase-resume selection, completion ordering | Verified against explicit backend/TP contracts. Production uses the journal/coordinator metadata pieces, not durable participant/engine recovery or automatic phase resumption. |
| File-backed reference backend | Exercises actual persistence, reopen, unknown outcomes, and SIGKILL. Its TP fixture is single-owner, not distributed TP recovery. |
| Automatic rebalance, dynamic membership, journal/tombstone compaction | Not implemented by this native path. Independent model results do not add these deployment features. |
| Formal guarantees | Native correspondence and independent protocol proofs have separate scopes. They are not a proof of the foreign C++ storage/RPC bodies or a combined production TP/Raft/migration deployment. |

The checked invariants include at most one serving owner, stable held grants,
exact final copy, retained decisions, stale-work rejection, and data-before-completion
ordering under the named interfaces. The [verification record](../../tla/mako/README.md#checked-results)
lists complete proof runs, negative controls, Rust regressions, reference-backend
SIGKILL cases, and production live/metadata-restart smoke evidence. Their scopes
remain separate: retaining metadata and refusing service is not MBTA durability.

## 8. Decisions to review

These are the current choices exposed for review, not requests to silently widen
or change the implementation:

1. **Storage floor:** accept an atomic local metadata batch, or require a backend
   with only atomic durable single-key writes? The latter needs a different
   journal publication protocol and corresponding recovery proofs.
2. **Availability during movement:** online pre-copy can avoid unchanged final
   writes, but source freeze still covers a complete exact final scan. A bounded
   delta-only cutover is not provided.
3. **Approved deployment boundary:** metadata persistence only; primary storage
   and TP remain unchanged. Automatic crash-safe migration would require a
   separately approved authoritative-row and transaction-recovery integration.
4. **Growth and lifecycle:** accept append-only history and retained fences for
   this path? Bounded retention and changing membership require additional native
   protocols, not just deleting old records or adding owners to configuration.

## 9. Code and deeper references

| Subject | Source |
|---|---|
| Names and identities | [types.rs](../../src/cluster/types.rs) |
| Coordinator and publication | [migration.rs](../../src/cluster/migration.rs), [durable_coordinator.rs](../../src/cluster/durable_coordinator.rs) |
| Local roles, immutable admission, conditional durable leases | [participant.rs](../../src/cluster/participant.rs), [epoch_admission.rs](../../src/cluster/epoch_admission.rs), [leases.rs](../../src/cluster/leases.rs), [durable_participant.rs](../../src/cluster/durable_participant.rs) |
| Actual copy and cleanup loops | [storage.rs](../../src/cluster/storage.rs), [transfer.rs](../../src/cluster/transfer.rs) |
| Durable primitives and record format | [durable_kv.rs](../../src/cluster/durable_kv.rs), [durable_engine.rs](../../src/cluster/durable_engine.rs) |
| Live integration and operational limits | [Mako book](../mako-book.md#data-migration-protocol-live-native-sharding) |
| Detailed storage and TP obligations | [Storage interface](../storage-interface.md#recovery-and-progress-scope) |
| Formal specifications and proof boundaries | [Sharding specification](../../tla/mako/README.md#dynamic-range-sharding) |
