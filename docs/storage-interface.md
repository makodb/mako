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

Replica integrity checks select `shard_for_index(local_shard)` and scan that
physical table, as the Paxos and Raft replication examples do. Scanning the
logical sharded wrapper instead visits other owners and requires a transaction
client; it does not restrict verification to this replica's replayed rows.

### TPC-C warehouse handles

With `MAKO_CLUSTER_CONFIG` enabled, each TPC-C worker precomputes a
`ShardingPreparedRoute` for each governed `(canonical table, global warehouse)`.
Construction encodes the fixed coordinate only: no metadata read, handle
selection, authorization or epoch pin. Local and remote accessors index the same
per-table descriptor vector.

First use in a protected metadata view resolves the captured owner **and
ownership version**, immutable table binding, and physical/proxy handle. The
descriptor retains a private fixed-cache slot tagged by
`mako_sharding_read_view_id`. These values survive logical transactions and
engine epoch ticks while the complete native admission view remains unchanged.
Raw-key captures remain transaction-scoped; arbitrary keys do not accumulate
in the persistent cache.

Each new logical context establishes a fresh transaction identity, not fresh
request bytes for every point. A returned point is a thin routing result for
immediate dispatch, not an admission token accepted by another API. Only an
outgoing RPC materializes the full envelope, directly into its destination.
Batch captures remain transaction-scoped. Successful ownership decisions for
fixed coordinates belong to the protected view, not the transaction, and are
reused without a new metadata lookup or repeated point authorization.
The following storage verb reuses a prepared slot only when
its physical table ID matches. Raw-key and received requests cannot establish
this shortcut. Slot indices survive capture-vector growth; element addresses
are never retained across callbacks.

A changed native view clears retained fixed routes, physical-handle mappings and
bindings, discarding borrowed references before their binding owners. This
includes Freeze with unchanged routing labels, owner changes and conservative
control republication. OCC retries retain their original protected view,
including across Freeze; there is no current-epoch comparison at commit.
Without an open logical context (including setup), accessors select without
beginning admission or pinning an epoch. Idle cached descriptors retain no pin.
Cold selection from an existing grant uses `mako_sharding_warehouse_select`;
it never refreshes the grant.
`src/cluster/warehouse.rs` owns the handle cache and selection. The narrow
`src/mako/benchmarks/tpcc_warehouse_bridge.h` callback only opens an engine
index, registers its immutable canonical binding, and returns a borrowed
opaque pointer; the engine retains those indexes until shutdown.

Cold warehouse-directory hits take shared read guards; misses take an exclusive
guard and recheck so concurrent opens create one index per physical identity.
Warm prepared descriptors bypass that directory as well as binding discovery.
The worker's immutable admission snapshot retains routing and complete local
authorization metadata. Reuse does not acquire the participant/control mutex.
Engine-owned handles remain stable but do not
independently authorize access.

#### View-tagged ownership authorization

The cached fact is a successful native ownership decision for the exact
`(Node/admission view, canonical table, fixed coordinate, grant)` tuple.
`AdmissionSnapshot::point` is a pure check of captured Serving state and exact
owner/ownership version; it does not depend on a transaction ID or acquire a
transaction lease. Repeating it on identical inputs cannot change its result.

At transaction entry, pin the existing STO epoch, activate the native read, and
obtain `mako_sharding_read_view_id`. The existing integer comparison with the
TLS cache's view ID covers all retained fixed entries:

- **Equal:** retain their successful ownership decisions alongside routes and
  handles. Rebuilding a transaction's point must not clear these decisions.
- **Different:** discard the fixed entries, including their decisions. First
  local use authorizes against the new protected view and caches only success.
  A failed authorization leaves the entry unauthorized; another attempt cannot
  turn that failure into a cache hit.

No per-entry generation, new global counter, current-global-version read per
operation, or version-equality check at commit is needed. The native ID denotes
the complete captured admission view, not merely a routing label. Freeze with
unchanged routing labels must invalidate a refreshed cache; an already-pinned
old transaction may retain its old authorization until its normal cleanup.

This memoizes only ownership. Every transaction still establishes its own
engine/logical guards, request identity and batch membership. Runtime readiness,
stop and metadata-failure errors reject **new logical contexts**, including
native read-cache hits. An admitted context uses its protected view until
cleanup; accesses do not poll service availability. Actual storage, transport
and cold resource-acquisition errors retain their existing error paths.
Incoming envelopes retain independent exact validation and per-envelope
authorization; arbitrary raw-key captures remain transaction-scoped. Receiver
success cannot seed a local fixed-coordinate authorization, and routing/handle
selection alone is never a successful authorization.

Regression obligations: stable-view commit/abort across transaction and epoch
boundaries; transaction-scoped batch membership; independent scopes; repeated
failed authorization under Frozen; migration/ownership return; admitted work
finishing after logical service stop while fresh admission fails; and no idle
engine pin. Engine and host owners must finish admitted work before destroying
their storage; stopping service acceptance is not engine destruction.

The fixed routing coordinate is the four-byte big-endian global warehouse ID.
Row keys keep their existing warehouse-local encoding. Each governed warehouse
gets a separate physical index, even when the static benchmark layout would
share a tree. Local handle identity includes the physical participant: two
shards in one process never share source and destination bytes. Cross-participant
access uses an origin-owned remote proxy, including in that single-process
case, so the destination admits the operation through its own immutable view.
Proxies have canonical bindings but are excluded from the reverse physical
index; RPC resolves the destination's binding rather than copying a numeric
source table-id window.

The migration storage adapter must call `mako_sharding_warehouse_local` for
every warehouse coordinate intersecting its range **before** scanning/copying
the destination. This opens adopted local storage before publication and reuses
the correct participant's existing index on partial return. Source cleanup uses
that same explicit physical-owner API, not workload route resolution. The cache
does not authorize access: captured admission checks the exact owner and
ownership version; a new workload attempt resolves again.

The immutable replicated `item` table is explicitly classified as replicated
by the native catalog; the mutable microbenchmark's `item` is explicitly static.
Neither invents a single warehouse owner. When `MAKO_CLUSTER_CONFIG` is absent,
the existing static vectors and single-process table wiring remain in use.

### Governed operations and epoch-pinned lifetime

Raw and transactional point entrypoints capture
`(snapshot_version, owner, ownership_version)` in one native lookup before
selecting the actual local physical table or origin-owned proxy. An ownership
return reuses that owner's physical table, not the startup wrapper. The complete
captured pair survives OCC and RPC retries; receivers bind its canonical address
to the actual point operation rather than refreshing the snapshot version.
Shared proxy flags never change.
Explicit static/immutable tables and disabled deployments retain their existing
dispatch behavior.

The engine bridge is `src/mako/lib/sharding_admission.{h,cc}`; its feature
predicate is `sharding_admission_enabled`. `sharding_resolve_point` selects the
current context's capture and establishes local ownership before returning.
Its immutable `ShardingPoint` carries a borrowed index and a tagged route
reference, not an owned request. Captured routes use stable slot indices rather
than addresses into movable vectors; unmanaged points borrow their caller's key
only for immediate dispatch. It is not an authority token: no API accepts a
previously returned point for local admission. Local storage kernels therefore
do not revalidate transaction IDs, private revisions, slots or handles on every
operation. A non-null captured handle is published only after selection and
local authorization succeed. Warm selection reuses both results. The private
cache revision tracks transaction-scoped batch membership, not request refresh.
The first successful authorization in a fixed entry's protected-view lifetime
calls `mako_sharding_read_point`; subsequent transactions may reuse that decision.

`ShardingPoint::write_request` constructs the wire envelope only at remote
dispatch, combining the captured route/grant with the current context's
transaction identity. Received envelopes remain borrowed under their receiver
guard. Control RPCs take their identity directly from the context; they do not
inherit the most recently accessed row or scan. Explicit scan envelopes remain
explicit. There is no mutable TLS outgoing-request slot.

Received envelopes retain exact wire-request validation against the actual key
and physical owner. Changing an envelope, including A→B→A, clears that envelope's
selected handle and authorization; closing/reopening establishes a new logical
context. The shared physical mapping may remain cached for the unchanged view,
but a mapping hit is never ownership authorization. The private TLS revision
fails closed on overflow; it is not an authority epoch or commit-time equality
check. Captured-view and engine-pin contracts remain. There are no participant
scope-slot hints or production per-access lease books.

`sharding_table_binding` and `sharding_physical_handle` share a view-scoped
indexed mapping cache across point, scan and receiver paths. Prepared/received
points retain direct immutable binding pointers; other table IDs use an integer
map whose `Arc` values own those bindings. Raw physical mappings use only the
canonical table ID, never arbitrary row keys. Fixed mappings additionally use
the exact coordinate, borrowing its bytes from a retained immutable binding,
not from an incoming envelope or a movable map element. The protected view
identifies the local participant. Physical index pointers use the native ABI's
pointer-sized integer representation inside Rust collections; the engine owns
the indexes until shutdown. Only successful lookups are cached. Cold misses use
the existing registry; warm hits need no registry mutex, name lookup, allocation
or `Arc` clone. Mapping reuse does not bypass incoming coordinate/grant checks,
point/range ownership admission, or view invalidation.

Before metadata or storage access, the caller announces a pin in the existing
STO worker epoch slot using sample/announce/recheck on the same atomic STO
global clock. Nested and retained guards conservatively keep the oldest pin
until all guards leave. The engine guard lasts through effects, unlocks, cleanup
and its end callback; the logical guard additionally spans OCC retries and
terminal RPC cleanup. An idle mode-1 helper placeholder is not pinned; actual
received work enters before using the engine.
`Transaction::in_progress()` tests the actual engine state, not helper mode.
An engine-local abort leaves a stopped attempt even if its logical RPC identity
is retained for terminal cleanup. Rebinding that identity must not reactivate
the stopped engine; its logical guard remains until request cleanup finishes.
`shard_reset()` explicitly starts the next empty, unpinned helper placeholder.
Multi-row handlers stop at the first failed access. A failed write batch rolls
back its staged rows before retaining the terminal reply; later rows must not
touch the stopped engine. Multi-row reads likewise stop on an engine-local
abort rather than continuing with a stale abort indicator into another request.

With native admission disabled, the existing static client treats a failed-read
reply as terminal and omits a second Abort RPC. After that request's cleanup,
the receiver restores only the empty, unpinned helper placeholder; the next
request enters a new engine attempt rather than reusing the stopped one.

`Transaction::register_thread()` assigns a process-wide physical-worker slot,
including replay workers and every colocated owner. Slots are not recycled;
registration refuses `MAX_THREADS` exhaustion rather than aliasing another
worker. The existing timer is initialized once per process, not by assuming a
particular owner's worker is slot zero. Epoch cuts do not wrap. Separate
processes exchange completion acknowledgments, never comparable epoch numbers.

`mako_sharding_read_begin(owner, epoch, out)` borrows the safely pinned nonzero
epoch and activates a reusable thread-local `MakoShardRead`. An owner/epoch cache
hit reuses immutable metadata without a per-transaction allocation, Arc clone,
or participant mutex. A miss takes an Arc to the already-published complete
admission/routing view under its separate publication lock, not the participant
mutex held by long-running transfer I/O. One logical context is active
per worker; nested operations reuse it rather than overwriting the cache.
`mako_sharding_read_begin` is the availability/error boundary, even on an
owner/epoch cache hit. The protected route/point/range/scan APIs and view-ID
retrieval validate active same-thread FFI handles but do not poll Node readiness.
The former per-operation `mako_sharding_read_live` API is removed.
`mako_sharding_read_end` marks the context inactive after engine and terminal
cleanup, before the logical epoch guard leaves. TLS may retain metadata and Node
Arcs while idle; those memory lifetimes do not extend host/index lifetimes.
Logical service stop rejects new contexts without revoking admitted ones.
Worker join, terminal acknowledgments and quiesce must precede physical
host/index destruction. Retained metadata startup still cannot admit work.

`mako_sharding_read_view_id` returns a nonzero, process-unique cache-incarnation
ID only for an active same-thread protected read; failures clear its output. On epoch
refresh, identical retained Node and full AdmissionSnapshot Arcs keep the same
ID. Actual replacement, owner switching, A→B→A, and distinct worker caches get
different IDs. Arc identity is compared while both owned values remain alive,
so allocator address reuse cannot produce an ABA hit. ID allocation happens
only on replacement, not every transaction/epoch; exhaustion fails closed.
This label is neither a routing version nor an authority epoch. It can establish
that a previously successful ownership decision still has identical inputs;
it cannot manufacture success or replace receiver validation, begin-time
availability checks or engine lifetime guards. An old view can remain cached until the existing
epoch-refresh boundary; Freeze's publication-before-cut and drain rules below
still govern that case.

The entry fast path compares an integer epoch, not all metadata. Complete
table/interval, absence and warehouse-alias copying belongs to publication;
worker refresh does not recopy the maps. Point admission checks captured Serving
state and exact local owner/ownership version; range admission covers the whole requested
interval, not merely its first point. A foreign snapshot number alone is not
proof: routing provenance and coordinates must match or captured local metadata
must validate the request. An already-pinned old view may authorize later
accesses for that context even after Frozen publication. No epoch-at-commit
equality is imposed. This has nonzero fixed entry/lifetime cost and retains
normal STO OCC, synchronization and storage costs.

An idle cached non-Serving view can conservatively reject after Abort or
activation until a new entry refreshes at a later engine epoch. Ordinary
transaction retry handles this; a current routing label alone is not evidence
that the caller's captured view authorizes physical access.

Incoming pieces retain their exact transported full request, including
`NO_SNAPSHOT`. Receiver dispatch selects this participant's registered physical
handle once; storage validates the actual coordinate and uses that same handle.
A new batch row or envelope invalidates the incoming handle selection, and a
previous resolved point cannot replace a changed incoming grant or snapshot.
Gateway pre-admission remains separate from execution-time admission.

Frozen publication removes new authorization before the host advances the same
STO clock. Drain scans the existing worker slots and requires each to be zero or
at least the retained cut. This covers readers, absent reads, scans, writers and
their actual completion, not just a decision or RPC response. No separate
authority clock, transaction registry or per-key reader counter supplies drain.
Volatile pins cannot establish crash recovery: retained metadata startup remains
inspection-only and readiness/read admission stay fenced.

Migration privately pre-copies while the source serves. Each source lookahead
copies bytes under the row lock and publishes them only after its STO/OCC
transaction commits; observations across calls need not form one snapshot.
A concurrent OCC conflict ends the advisory pass with Retry, not false EOF.
The synchronous destination job joins every accepted physical effect before its
reply; Final/Abort share its participant borrow and reject delayed Stage-round-0
work after the round changes. After Frozen-before-cut and true drain, the final
merge still scans the complete range, skips only equal-identity/equal-byte puts,
and removes destination-only rows. Pre-copy does not justify a dirty-only final
delta. The conditional durable library keeps its separate lease/recovery
contract and is not replaced by volatile native pins.

Transactional lock batches carry a distinct put/delete operation byte and exact
captured route per row. A governed commit row must already have been resolved
in the current logical transaction when its proxy write was staged. Batching
materializes that row's envelope from its capture; it never performs a fresh
route lookup or substitutes the last outgoing request. Explicit snapshot-segment
full scans keep their separate interval admission, and generic raw scans still
use `NO_SNAPSHOT`.
A remote delete stages `MassTrans::transDelete` at the participant, including
the absent-key read when no row exists; it is not encoded as an empty put.
Absent transactional reads return `NOT_FOUND` without dropping their read set.
Read, write, and absent-key native epoch guards remain held through actual engine
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
Frozen publication and the completed same-STO-clock cut protect the **selected
range**, not all node data: transactions may still change unrelated ranges.
Transfer calls bypass workload admission, not normal engine synchronization;
pre-copy runs in real STO transactions while source workload remains active.
The host binds each native thread to its actual shard and allocator/Masstree
runtime and releases its transaction state before worker exit.

### Governed range scans

For remote segments, the V3 scan request binds the complete transaction identity,
`(snapshot_version, owner, ownership_version)` tuple, logical table, and
raw-versus-fixed warehouse coordinate. Each V2 page echoes the retained snapshot
version and is rejected before callbacks if it differs. The C++ receiver
compares that embedded identity with the outer `ShardingRequest` before immutable
admission or engine access. `mako_full_scan_new` takes `MakoScanIdentity`;
`mako_scan_page_identity_matches` validates it without allocating a coordinate
copy. Raw scans carry no fixed coordinate, so long raw keys are not constrained
by the warehouse-coordinate buffer.

The segment visitor retains one immutable snapshot and passes a `MakoShardRoute`
for every homogeneous interval. It never combines segment ownership with a
fresh global version. Only that full-interval provenance authorizes the range
fast path; a generic local scan with only a lower-endpoint point capture uses
exact range validation, including absent keys and ownership gaps.

Native-enabled scans use that one retained snapshot for both execution paths:

- A segment owned by the current transaction participant resolves the canonical
  local physical index, admits its **complete interval**, then calls MassTrans
  directly. Fixed-coordinate indexes admit their warehouse coordinate, not a
  range of physical row keys. No local cursor, transport page, encoding or
  decoding is created. The epoch guard remains held through transaction cleanup.
- Other participants retain the V3 request/V2 page protocol, even if they share
  the process. The visitor does not refresh the snapshot between local and
  remote segments. A callback stop prevents visiting later segments.

Immutable admission checks the captured complete interval. A cached routing
version alone is insufficient authorization. Frozen prevents new captured views
from serving the range; an old view remains valid while its context is pinned,
and drain waits for that context's actual completion.
Loading and disabled-sharding paths retain their existing admission rules and
use direct execution for physically local segments.

Direct execution retains forward `[start, end)` and reverse `(end, start]`
bounds, binary and empty keys, caller-arena value lifetime, metadata stripping,
and callback exception cleanup. Public scans avoid incidental transport-page
read-ahead and retain MassTrans's OCC observations. The internal normalized
reverse adapter also accepts an unbounded upper end: it discovers an actual
maximum in the same transaction, retains that forward traversal's observations,
then scans backwards. It does not invent an infinity key. OCC failure aborts
without replaying user callbacks.

`full_scan_core.rs` owns remote request encoding/decoding, callback page assembly
and cursor consumption. `full_scan_proofs.rs` relates both paged and direct
callbacks to the transactional physical image, then derives logical ordered
coverage from the held grant and placement coherence. `native_direct_scan`,
`native_direct_fixed_coordinate_scan` and `mixed_sharded_scan` cover direct
segments, warehouse aliases and mixed traversal. Callback stop proves only a
prefix; errors do not certify completion. For remote pages, a nonempty short
page is not EOF and a byte-budget stop is not exhaustion. FFI lifetimes, callback
forwarding, SRPC response association, physical-handle labeling, actual STO/OCC
semantics and MBTA metadata stripping remain foreign contracts, not verified
C++ bodies.

### Recovery and progress scope

Start with the [end-to-end sharding/migration design](architecture/sharding-migration.md)
for the algorithm and its separation from application transactions. This section
specifies the concrete persistence and integration contracts.

The production `runtime.rs` / `HostAbi` primary-row boundary is still the live
MBTA/STO adapter: rows are in memory, raw migration uses `try_commit_no_paxos`,
and replicated migration is rejected. Primary storage and application TP remain
unchanged. Existing row-log components do not supply the authoritative durable
row image and exact distributed-transaction settlement required for full recovery.

An optional **metadata-only** production path now uses `rocks_metadata.rs`,
`metadata_coordinator.rs`, and `metadata.rs`. The real RocksDB backend supplies
exact reads, synchronous WAL-enabled atomic batches, an exclusive writer lock
held across reopen, and fail-closed uncertain-I/O settlement. Creation is explicit;
retained open never recreates missing state. The coordinator uses the checked
`DurableCoordinator` replay. The participant uses a separately tagged live
bookkeeping journal, not a counterfeit `DurableEngine`: it records controls and
transfer intent/results, with no claim that Ready/Done rows survive. Historical
lease records do not make the new volatile worker pins recoverable.
Reopening validates that prefix but leaves the participant and data admission
closed. Historical admin outcomes remain inspectable; pending migrations and
old leases cannot be finished or automatically resumed.
See [configuration and restart behavior](mako-book.md#optional-durable-migration-metadata).

After epoch-cutover integration, run `./docker_build.sh ci nativeShardingProof`
and `./docker_build.sh ci nativeShardingSmoke`. Required native checks include
announcement/recheck races, nested oldest pins, post-Frozen old-view access,
effects and terminal cleanup before drain, idle-helper entry, colocated owners,
concurrent pre-copy writes/deletes, joined/fenced delayed rounds, exact final
absence, admitted-view completion after service stop, rejected fresh admission,
and retained-metadata startup fencing. These obligations are not a claim
that the new runtime has already passed every formal proof gate.

`durable_kv.rs`, `durable_coordinator.rs`, `durable_participant.rs`, and
`durable_engine.rs` also provide the stronger conditional durable-interface path.
They reuse the native actors and ordered transfer loops. Production reuses its
KV/coordinator pieces; the full participant/engine/TP recovery adapter remains
unimplemented. The following replay, settlement, and data-before-completion
requirements describe that stronger path, not metadata-only restart.

The system-record encoding is concrete. A 32-byte little-endian key contains
`KEY_TAG:u64`, `owner:u32`, `authority:u32` (participant 0, coordinator 1),
`kind:u64` (head 0, operation 1) and `index:u64`. A head stores
`HEAD_TAG`, admission incarnation and record count; an operation stores
`RECORD_TAG`, a u64 byte length and the encoded native input. Record zero is
bootstrap. Appending atomically writes the operation and its new head; replay
runs those decoded inputs through the actual native transition bodies.
Decision/publication, retained nonces, controls, leases and completion receipts
are reconstructed from that committed prefix, not a caller-supplied actor.
Protocol replay semantics are part of the versioned format.

After activation, `next_recovery_transaction(kv, admission, 0)` discovers an
unfinished old-incarnation identity from committed Begin records and the
reconstructed lease registry. Continue with the returned cursor and settle each
identity through `finish`; already-finished entries are excluded. A restarted
driver needs no retained client-ID list, and an unknown settlement never clears
the lease.

The backend contract is deliberately narrower than migration correctness:

- `DurableKv` supplies exact byte reads, atomic durable batches, and a barrier
  that resolves or excludes **every** old accepted submission. Unknown is not
  failure or success. There must be one serialized live writer per namespace;
  `&mut K` is not a distributed CAS. A Raft adapter must fence old writers at
  application time, not merely when an RPC is received.
- `DurableEngine` restores authoritative canonical row bytes and versions,
  settles old raw submissions, and atomically settles exact transaction effects
  and terminal outcomes. Returning a known outcome must exclude later effects
  from surviving peers and queued RPCs. An absent transaction may release a
  restored lease only after a durable empty-abort identity fence, never from
  absence or elapsed time alone. Global transaction IDs must retain their TP
  identity across all participants and restarts, not be recycled by a restarted
  worker. A receiver admission fences requests; it does not allocate global IDs.
  The engine facade is exclusively borrowed during a transfer call. Its
  successful admission check exposes matching working/durable state with no
  pending raw effect. A production adapter must provide that coherent view;
  wrapping independently mutating primary storage is not sufficient.
- Begin and lease acquisition must run under TP's global enrollment/lifecycle
  authorization, held or fenced through the metadata operation. A transaction
  already settled elsewhere cannot enroll at a previously uninvolved owner.
  The owner-local journal cannot establish this from absence; these APIs do not
  query global TP state themselves. Dispatch must enforce this ordinary TP
  lifecycle contract, not pass a stale boolean. Local `resolve` is not an engine
  outcome and cannot replace global settlement.
- Reservations precede copy/deletion I/O. Durable complete copy precedes the
  Ready record; durable deletion precedes Done. Failed or unknown writes fence
  the candidate actor. Recovery settles I/O, replays records and persists a fresh
  admission before exposing serving APIs. RPCs must retain their captured
  namespace/incarnation rather than being relabeled with the receiver's new one.
  The checked adapter exposes complete canonical row images and retains an
  erased range/context frame through the actual transfer loop, so outside-range
  byte/version preservation is not vacuous over a range-filtered image.
- Canonical table/coordinate/key bindings must survive reopening. Mutable
  handles, cursors, actor caches and scheduler state are not recovery inputs.
  The journal is append-only; this path does not compact participant history,
  reclaim transaction fences or implement dynamic membership.

The Docker proof gate also runs `examples/durable_recovery.rs`: a real
file-backed reference KV/engine, SIGKILL during an accepted unknown deletion,
fresh byte-only replay, phase resumption and ownership return. A second SIGKILL
case discovers an orphaned lease solely from its journal, persists an
empty-abort identity fence, rejects a late old write and resumes migration. Its fixture
transactions are single-owner and enforce an owner-specific identity domain;
it is neither a distributed TP simulator nor evidence of production MBTA
durability. The real-MBTA smoke separately checks live migration and four
metadata-only SIGKILL/reopen cases, including refusal of data admission,
destructive old controls, and recovered lease release. The RocksDB foreign
kernel and filesystem synchronization remain trusted, unverified boundaries.
The recovery regressions also complete a destination copy after the coordinator
has aborted but before the destination receives Abort. Reopening retains the
abort decision and private Ready state; resumption deletes that private copy
without changing source ownership or the retained nonce outcome.
See [the proof scopes and interfaces](../tla/mako/README.md).


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
