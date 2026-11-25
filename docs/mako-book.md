# The Mako book

Mako's application runtime is Rust. The outer protocol combines distributed
transactions, sharding and range migration. The distributed transaction protocol
uses optimistic concurrency control and two-phase commit. It acquires every
participant's write locks before final read validation, saves one coordinator
decision, and then completes that decision at each participant.

C++ remains in Masstree, RocksDB and their storage ABI/runtime dependencies.
SRPC builds as Rust with its platform C and assembly bindings. Paxos and the C++
application backend are retired. Historical performance reports describe their
recorded revisions and do not measure this protocol.

The [OCC + 2PC plan](plans/occ-2pc.md) records implementation and qualification
status. The [upper-layer contracts](developer/upper-layer-contracts.md) distinguish
verified decisions from assumptions about storage, RPC and replication.

## Code map

| Layer | Code | Responsibility |
| --- | --- | --- |
| Distributed protocol decisions | `crates/mako-protocol-core` | Verus-checked admission, write-lock barrier, prepare votes and immutable decisions |
| Protocol composition | `crates/mako-protocol` | Coordinator driver, journal transitions, identities, wire encoding and transport contracts |
| Rust RocksDB API | `crates/mako-rocksdb` | Compatible transaction API subset, column families, atomic batches, bounded iterators and saved-result resolution |
| Runtime | `crates/mako-runtime` | Native adapters, service-owned participant workers, client, gateway, process ownership and recovery |
| Sharding and migration | `src/cluster` | Immutable routing snapshots, admission, range transfer, receipts and publication |
| Storage-host decisions | `crates/shard-host-core` | Checked translation between migration effects and physical storage |
| Local transaction manager | `crates/sto-core` | OCC read/write sets, locking, validation and terminal cleanup |
| Ordered storage | `crates/sto-masstree`, `crates/sto-tpcc-ffi` | Masstree adapter, versioned rows, scans, retained affine workers and owned redo |
| Log persistence | `src/deptran/raft/store/src/journal.rs` | Shared RocksDB entries, commit prefix, identity and snapshot format for local and Raft ordering |
| Applied storage | `crates/mako-storage-rocksdb`, `crates/sto-rocksdb`, runtime `log/materialization.rs` | Common RocksDB rows and protocol state, direct or batched from the optional Masstree layer |
| Replication | `crates/raft-host`, `src/deptran/raft` | Raft authority, committed prefixes, read barriers, snapshots and recovery |
| RPC | `src/srpc`, runtime `rpc.rs` | Rust transport, reactor, bounded owned requests and pending-reply cancellation |

The outer protocol's development gate does not build Masstree, RocksDB, SRPC or
Raft. Their contracts have small test implementations. Component and native
integration tests run separately.

## Transaction execution

A `Client` identifies its coordinator shard and worker stream. Each attempt has
a random client incarnation and an increasing attempt number. Each participant
also issues a stream lease. The global transaction ID and participant lease are
different identities; a lease cannot replace a transaction ID during recovery.

`Client::begin` enrolls the coordinator's local participant. Reads, scans and
private writes use the same retained worker service on local and remote shards.
The caller's `sto::Worker` argument still enforces database ownership and the
public borrowing contract. It does not own the prepared transaction frame.

Execution is optimistic. A transaction reads committed versions and its own
private writes. The storage adapter records present and absent point reads and
scan predicates. A failed operation poisons that attempt, so ignoring an
operation error cannot commit the successful prefix of a transaction body.
Remote writes are staged immediately as private intents. They do not acquire
all write locks until the commit driver begins preparation.

Participant membership, routes and the read/write sets stop changing before
preparation. The coordinator includes its own shard even for an empty or
read-only local participant. Read-only participants follow the same validation
and decision protocol.

## OCC and two-phase commit

The production entry points are `mako_protocol::commit::coordinate`, runtime
`client.rs::NativeEffects`, and `prepared.rs::NativeEngine`.

1. **Lock every participant.** Freeze execution and acquire each participant's
   write locks. A conflict causes cleanup before any Prepare is sent. Each
   participant returns a timestamp proposal above its read dependencies and its
   durability policy. The driver rejects mixed durable and volatile membership.
2. **Register and prepare.** After every lock acknowledgement, choose the common
   timestamp and save `Begin` at the coordinator. Send the exact frozen
   descriptor to every participant. Each validates its point reads, absent
   reads and scan predicates, exports owned redo, and saves `Prepare` before
   returning Yes. Prepare publishes no committed rows.
3. **Record the decision.** Commit requires every member's Yes vote. Otherwise
   the coordinator chooses Abort. Its journal atomically establishes one
   immutable decision. A lost acknowledgement means Unknown until that record
   can be read or the identical operation retried.
4. **Complete the decision.** Participants check the coordinator authority and
   save completion. Commit publishes the saved intent; Abort discards it.
   Completion is idempotent. Locks and migration pins remain until the physical
   terminal action finishes. A native installation failure after saved Commit
   fences the shard for recovery.
5. **Reply to the client.** Success requires Commit acknowledgements from every
   member. A saved Commit with missing acknowledgements is still committed and
   returns Unknown to the caller. It must never become a retryable Abort.

The lock barrier and validation round are substeps of Prepare. They do not add
another transaction-commit phase. There is no shard-wide certification lock.

For example, A reads x and writes y, while B reads y and writes x. Both cannot
commit with their original observations: after all write locks are acquired,
at least one final validation observes a changed version or a conflicting lock.
The same obligation applies to absent keys and range predicates. Protecting
only existing point rows would leave phantom cycles possible.

Under the storage contract, the global lock barrier supplies a candidate serial
position. All of the transaction's reads precede that barrier; final validation
follows it. A conflicting writer that could invalidate an observation must
change its version or hold protection detectable by validation. The storage
adapter must enforce this for every writer and predicate. The upper state proof
alone does not prove that physical storage supplies these facts.

### Participant states and uncertain results

| State | Meaning | May release protection without a saved decision? |
| --- | --- | --- |
| Executing | Collecting private intents and observations | Yes |
| Locked | Frozen write locks acknowledged; no Yes yet | Yes |
| Preparing | Validated intent exists; prepare append may have succeeded | No |
| Prepared | Prepare is saved and Yes can be returned | No |
| Committed / Aborted | Saved outcome physically completed | Protection has been released |

Preparing is necessary because a storage error can follow persistence. A
participant must not treat a lost prepare acknowledgement as proof that it never
voted Yes. Higher terms, new stream claims, delayed ordinary Abort messages and
listener teardown cannot discard Preparing or Prepared work.

`Transaction::id()` exposes the stable ID. `Client::resolve(store, id)` resolves
the client's last registered attempt, reads or establishes the authoritative
decision, and retries completion without running the transaction body again.
It clears the client's fence only after every participant acknowledges that
outcome. A missing registration or unavailable authority remains Unknown.

## Durability and recovery

All three backends implement `CommitLog: Journal`. The shared contract includes
journal apply/lookup, immutable decisions, durability reporting, admission,
readiness and shutdown. Every backend owns its journal; the store never supplies
a hidden memory fallback.

| Implementation | Use | Storage |
| --- | --- | --- |
| `MemoryLog` | Isolated tests and explicit volatile native test fixtures | Memory only, with no restart guarantee |
| `LocalLog` | Production without Raft | Local RocksDB log |
| `ReplicatedLog` | Production with Raft | Raft-ordered RocksDB log |

`MemoryLog` lives in `mako-protocol` and has no native backend dependencies.
Native test configuration must opt into it with `volatile: true`. `LocalLog`
and `ReplicatedLog` live in `mako_runtime::log`; they are typed specializations
of one implementation, accepting only their respective local or Raft config.
Both submit the same application command bytes and call the same state machine.
The upper protocol uses their backend-independent `Journal` contract.

Each participating shard chooses one journal policy:

| Policy | Journal acknowledgement | Restart guarantee |
| --- | --- | --- |
| Volatile | In-memory transition | None |
| Local persistent | Synchronous RocksDB log append and committed state-machine application | Local process restart |
| Replicated | Current Raft authority and applied committed record, with synchronous durable storage | Recovery from the surviving committed replica prefix |

A configuration with synchronous writes disabled cannot advertise durable
transactions. All members of a transaction must agree on the durability policy.

Both production modes use `<log-root>/log/base` for the ordered RocksDB log.
The configured `engine.directory` holds the common applied RocksDB state.
The `kind: masstree` compatibility spelling derives `<log-root>/data`. The log stores index/term/payload
entries, hard state, committed prefix, durable sequence, snapshot boundary and
exact authority metadata in one versioned format. Raft additionally uses the
vote and election-term fields. RocksDB WAL is enabled; synchronous atomic
batches supply the default durability barrier. The production Raft host no longer
uses a separate segmented WAL.

The common keys are `f` (format version), `a` (exact authority and sync policy),
`i` (store identity), `h` (term, vote and committed index), `c` (durable transition
sequence), `s` (snapshot boundary/reference), and `e` followed by a big-endian
index (entry). Entry values contain a little-endian term and a tagged payload:
`0` is a no-op; `1` precedes application command bytes. Local mode uses term 1
and no vote. The shared store validates transitions before persisting them.

A local append atomically saves its command and advances the committed prefix,
then calls the application state machine. Raft saves entries and advances that
prefix only through its consensus decisions. In either mode, recovery replays
committed commands above the application's saved position. Failure after a saved
command fences the driver until recovery, preserving an uncertain or committed
outcome. Append alone cannot make an uncommitted Raft suffix visible.

Sharing bytes does not make local and replicated authority interchangeable.
Reopen checks the exact mode, group/membership and sync policy. Changing a
configuration cannot turn an old Raft suffix into local committed data.

The journal records `Begin`, `Prepare`, `Decide` and `Complete`. A prepare stores
the full descriptor, final row intents and any gateway receipt. Completion
records committed rows, the participant outcome and the gateway receipt. The
direct adapter persists them immediately; the Masstree path batches them into
RocksDB while the committed log covers the gap. Prepared rows stay separate from committed rows.
The descriptor binds protocol version, transaction ID, common timestamp,
membership, participant leases and recovery endpoints.

Before foreground admission after startup or a leadership change, the service
loads unresolved prepares and reconstructs their native write protection. It
compares reconstructed redo with the saved intent. A mismatch fences the shard;
it does not turn a promised Commit into an Abort. Only after all unresolved
intents have been protected can new transactions execute.

A coordinator authority can save Abort for a still-pending registration. This
races atomically with the foreground Commit writer: whichever decision is saved
first remains authoritative. A participant cannot make that choice on its own.
If the coordinator is unreachable, prepared work blocks until authority returns.

Retained workers retry resolution after inactivity. Unprepared work may expire
and abort after 30 seconds without progress. Preparing and Prepared work instead
ask the coordinator to resolve the decision. Recovery retries preserve the
same ID and intent; no timeout authorizes an autonomous prepared abort.

A former leader with active native transactions can fail closed when a different
leader's committed prefix reaches it. Restart reconstructs its state from the
durable authority. This is a conservative availability limit of the native
adapter, not a second way to choose transaction outcomes.

Closing an RPC listener preserves prepared workers in the `Store`. Explicit
shard shutdown joins those workers and closes their native scopes. Durable
records survive and must be recovered before serving again. Volatile mode loses
its state on process shutdown. Terminal journal entries currently have no
reclamation policy; removing them without a stale-request fence is unsafe.
Both local and Raft logs trim a covered prefix after durable checkpoint
publication. Protocol records and receipts remain in that checkpoint until a
separate safe reclamation protocol exists.

The shared log directory format is `mako-rocks-log/1`. Existing segmented Raft
WAL directories and local stores with applied rows but no command log require
format-specific recovery. Startup refuses them and preserves their data.
`mako_convert` handles the shared RocksDB log format and its old row mirror;
it does not import a segmented Raft WAL or data without a command log. The applied row/receipt encoding and
application command versions remain unchanged. The retired C++ WAL is not
imported. Mixed old/new protocol operation is not a supported rolling
upgrade: old distributed Timestamp/Validate/Install commands are refused by the
new runtime.

Local logs reclaim obsolete checkpoint generations and abandoned attempts only
after validating recovered authority or flushing the replacement snapshot
reference. The current checkpoint remains intact. Raft retirement separately
waits for outgoing snapshot streams to release their image leases. Both
transaction adapters use the same native RocksDB checkpoint format. Protocol decisions, receipts and replay fingerprints
remain retained until a separate stale-request-safe garbage collection protocol
exists.

## RocksDB with optional Masstree

RocksDB is the final data store in both configurations. Select the transaction
adapter with one flag and keep the data and log directories unchanged:

```yaml
engine:
  kind: rocksdb
  directory: /data/shard/data
  masstree: true # false uses the direct RocksDB STO adapter
persistence:
  directory: /data/shard
  create: false
  sync_writes: true
```

The direct adapter applies each committed command atomically to RocksDB. With
Masstree enabled, its STO adapter serves transactions from a full in-memory
committed image. Each transaction still saves its Prepare and decision through
LocalLog or ReplicatedLog. Already committed effects accumulate in bounded
batches for RocksDB. No second validation or transaction decision occurs during
materialization. Undecided Prepare rows remain invisible.

The default flush targets are 128 log records, 8 MiB of estimated buffered data,
50,000 row effects, or a 100 ms timer. A single legal command larger than a target
flushes alone. Checkpoints and orderly shutdown also flush. A failed or uncertain
flush fences admission, leaving recovery to the committed log.

Let C be the committed log position, A the applied application position and D the
materialized RocksDB position. Healthy application cuts satisfy D <= A <= C. Direct mode
keeps D = A; Masstree can have D < A. Each RocksDB batch atomically saves its rows,
tombstones, logical versions, protocol metadata, receipts, replay identities and
D. A checkpoint pauses committed publication, flushes D to A, then creates and
publishes the native RocksDB snapshot before the host trims covered log entries.
This includes a saved Complete even if its affine worker has not installed yet.

Recovery loads the RocksDB base and replays the committed log suffix above D.
Masstree reconstructs its in-memory image before restoring unresolved prepares,
write protection and migration pins. Previous RAM progress never authorizes
skipping replay. The two modes use the same owner/schema identity and checkpoint
format. Compatible replicas in one Raft group may use different adapters.

Stop the node, change only `masstree`, and restart with `create: false` to switch
modes. No conversion or data copy is required. This is a restart toggle; it does
not add live switching, eviction or lazy cache misses. Masstree must fit the full
committed image in RAM. Old file-only Masstree stores require the offline upgrade
below. The [adjustment plan](plans/rocksdb-optional-masstree.md) tracks the change.

## Gateway and application APIs

The gateway commits each storage operation independently. Session grouping does
not undo already committed operations. Redis MULTI/EXEC uses the distributed
transaction client instead.

Gateway writes, reads, unsuccessful inserts and missing-key deletes all use the
same OCC + 2PC driver. The saved prepare includes the exact response. Completion
persists that response with the row effects, allowing an identical gateway
request to recover its result after a lost reply or failover. Reusing a gateway
identity with another payload is rejected. A pending prepared gateway operation
returns Unknown and cannot be executed as a fresh operation.

The native SDK accepts owned keys and values. Routed requests include the table,
owner and ownership generation. Paged scans verify cursor identity, ordering,
bounds, explicit EOF and snapshot provenance before returning rows to the caller.
Transaction resource and wire limits remain enforced independently of the scan
page size. Individual wire messages and durable journal commands are bounded to
4 MiB. Volatile prepared intents may aggregate bounded messages up to 128 MiB,
which supports chunked large Redis values without enlarging the RPC frame limit.

## Rust RocksDB compatibility

`crates/mako-rocksdb` provides a Rust API subset modeled on the community
[`rocksdb` crate 0.25.0](https://github.com/rust-rocksdb/rust-rocksdb/tree/v0.25.0).
Its `OptimisticTransactionDB` and `DB` alias run Mako's distributed OCC + 2PC.
Every committed write reaches the configured local or Raft decision log.
The API supports direct RocksDB and RocksDB with optional Masstree through the
same types and methods. It does not introduce another native RocksDB binding.

Enable the `rocksdb-engine` Cargo feature. Enable `masstree-engine` as well to
allow `Options::set_masstree(true)`. With default features disabled, the crate
contains only the facade and backend contracts, which the fast upper gate tests
without RocksDB, Masstree, SRPC or Raft.

```rust
use mako_rocksdb::{OptimisticTransactionDB, Options};

let mut options = Options::default();
options.create_if_missing(true);
let db = OptimisticTransactionDB::open(&options, "/data/mako")?;
let tx = db.transaction();
tx.put(b"account:alice", b"100")?;
tx.put(b"account:bob", b"200")?;
tx.commit()?;
assert_eq!(db.get(b"account:alice")?, Some(b"100".to_vec()));
```

`open` starts a durable single-shard node with four concurrent transaction
streams by default. `open_default` enables creation. `open_cf` accepts the full
set of column-family names and includes `default` automatically. Reopening must
supply the same family set; order does not matter. Names map to stable raw table
IDs. `set_error_if_exists` and `create_if_missing` are enforced. These directories
contain Mako's common RocksDB data and transaction log, not an ordinary RocksDB
application database. There is no on-disk compatibility or automatic import of
an arbitrary RocksDB directory.

For distributed deployments, `DB::open_config(MakoConfig)` starts and owns one
shard using the existing runtime configuration. Raw governed or static tables
become named column families, including a required `default` table. Opening
returns once the listener starts, so replicas can be started before a quorum
forms. `ready()` reports recovered coordinator authority on this handle;
`wait_ready(Duration)` waits with a deadline. Followers stay open and replicate
while not ready to coordinate transactions. Single-node `open` waits for readiness. Configure
all shard peers, the same schema and worker count on every shard, and local
persistence or Raft with synchronous durability. Warehouse tables and the
runtime's origin-local `replicated` table kind are rejected by this API. The
`replication` node setting still enables the normal Raft log. Point operations
and scans use the transaction's captured migration routing epoch.

The [compiled example](../crates/mako-rocksdb/examples/rocksdb_transactions.rs)
shows transactions and batches across column families. In the build container:

```sh
cargo run --locked --manifest-path crates/Cargo.toml -p mako-rocksdb \
  --features rocksdb-engine --example rocksdb_transactions -- /tmp/mako-api
```

Use the normal native build environment and `--features masstree-engine` to add
`--masstree` to the example's arguments. The same directory can be reopened with
that flag enabled or disabled.

### Supported operations and options

| API | Behavior |
| --- | --- |
| DB `get`, `put`, `delete`, and their `_cf` / `_opt` variants | Each call is one certified distributed transaction. Deleting an absent key succeeds. |
| `transaction`, `transaction_opt` | Shared-reference transaction methods with consuming `commit`. A transaction starts on its first operation. |
| Transaction `get`, `get_cf`, read-option variants, `get_for_update`, `get_for_update_cf` | Reads include staged writes. Both exclusive settings record OCC dependencies; write locks are acquired at prepare. |
| Transaction `put`, `put_cf`, `delete`, `delete_cf` | Stage work until commit. An operation error poisons the attempt even if the application ignores it. |
| `rollback` and Drop | Abort staged work. Successful explicit rollback allows reuse as a new attempt. Uncertain cleanup stays fenced. |
| `multi_get` on DB or transaction | Preserve key order, duplicates and missing values. DB multi-get returns errors for all keys if final certification fails. |
| `WriteBatch`, `write`, `write_opt` | Ordered puts and deletes commit atomically across families and shards. A foreign handle or invalid batch rejects the batch. |
| `iterator`, `iterator_opt`, `iterator_cf`, `iterator_cf_opt` | Bytewise Start, End and From seek modes, forward or reverse. ReadOptions provide inclusive lower and exclusive upper bounds. |
| `WriteOptions::set_sync` | Both values use synchronous durability. The facade does not weaken the configured durable log. |
| `WriteOptions::disable_wal` | `false` is accepted; `true` returns NotSupported before work starts. |
| `OptimisticTransactionOptions::set_snapshot` | `false` is accepted; `true` returns NotSupported. |
| `Options::set_max_transactions` | Mako extension for the single-node coordinator stream limit. Distributed configuration uses `workers`. |

Only listed methods and options are supported. There is no pessimistic
`TransactionDB` alias, MVCC snapshot, savepoint, merge operator, raw/pinned
iterator, dynamic column-family creation/deletion, SST import/export, custom
comparator or RocksDB tuning facade. Unsupported methods are absent, and
unsupported values of implemented options return an error. For `transaction_opt`,
that error is reported by the first operation or commit. No C/C++ public
compatibility layer is included.

Iterators eagerly collect at most 4,096 rows and 4 MiB. Exceeding either limit
returns an Incomplete item, never a successful truncated range. Use narrower
bounds for larger scans. A DB iterator certifies its read transaction before
returning rows. A transaction iterator leaves final certification to commit;
its buffered rows do not change after later writes in that transaction. Iterators
can themselves yield errors, so collect or handle their `Result` items.
Keys are limited to 1,024 bytes and values to 65,535 bytes. Batches are limited
to 4,096 operations and 4 MiB of key/value bytes; encoded durable transaction
limits can reject a smaller batch because they also include metadata.

### Isolation and uncertain results

All Mako transaction reads, absence observations and ranges participate in OCC
validation after the global write-lock barrier. This differs from plain reads in
RocksDB's optimistic transaction API. It does not provide an MVCC view fixed at
transaction creation. Values read during a transaction become certified only
when commit succeeds. The facade adds no automatic transaction retry.

DB is Send + Sync. A transaction is Send and not Sync. Internal threads own
native workers and SRPC listeners, and the handle joins them during cleanup.
Each started transaction reserves one coordinator stream until its terminal
result is received. Exhausting the limit returns Busy rather than waiting while
the same caller may hold another transaction.

Busy reports a conflict or occupied stream. TryAgain reports changed routing or
unavailable authority. An UnknownCommit error carries `transaction_id()` when an
attempt exists. Save that identity and call `db.resolve(id)` against its
coordinator. Resolution completes the recorded Commit or Abort at every
participant and never reruns application code. It also works after reopening
the coordinator with the same durable data. A missing registration stays unknown;
it does not authorize treating the attempt as aborted. A lost commit reply keeps
its stream unavailable until resolution succeeds.

`pending_transactions()` lists uncertain results and failed Drop cleanup seen by
this DB handle. The list itself is not persistent. Keep returned identities in
the application if they must survive application restart. Resolution refuses a
stream with a live transaction. Preserve an ID before commit with
`tx.transaction_id()` after the first operation, or obtain it from the error.

The older `mrxdb_rocksdb_compat.h` is a separate cache API and does not supply these
distributed transaction semantics. The [implementation plan](plans/rust-rocksdb-api.md)
records the completed work. The [qualification record](testing/rust-rocksdb-api-2026-11-25/README.md)
contains the upper, native, sanitizer and build results. The final upper gate
completed in 159 seconds on 32 CPUs, including the facade contract tests.

## Sharding and migration

Routing uses immutable snapshots. An admitted transaction retains its snapshot
and epoch pin through execution and terminal cleanup. Ownership generations
prevent an old grant from becoming valid merely because a range later returns
to the same owner number.

Range migration stages and pre-copies destination data while the source serves.
It then publishes Frozen metadata, cuts admission and waits for old pins to
drain. After joining the copy worker and fencing its generation, it reconciles
complete ordered scans, including destination-only deletions. Explicit EOF
certifies coverage. The old source is retired before the new ownership snapshot
is published, and both participants must finish before reporting completion.

Prepared transactions retain their migration pins across client disconnects and
listener closure. Migration must wait for their authoritative decision and
physical completion. It must not interpret a missing client or an expired RPC
as permission to drain them.

Migration uses the selected data engine and transaction log. Durable mode
requires `sync_writes: true`; Masstree and direct RocksDB work with LocalLog or
ReplicatedLog, including transfers between adapters. Replicas within one
Raft group may choose different adapters over the common RocksDB format. Volatile fixtures still use MemoryLog and have
no restart guarantee.

The durable coordinator and participant journals live in a reserved internal
table in the selected engine. Their atomic batches use the same OCC + 2PC log
as user rows. The common RocksDB checkpoints therefore include
control state without a second metadata database. Application schemas and
ordinary data requests cannot access this namespace. Checkpoint identity binds
the migration format; enabling migration on an existing fixed-placement
directory requires a separate migration of authority. Engine conversion alone
does not enable migration.

Startup and leader changes restore retained transaction locks and epoch pins
before opening ordinary admission. Recovered prepares can resolve only their
saved decisions; they cannot execute fresh operations using recovery admission.
The migration actor rebuilds new drain cuts after publishing recovered Frozen
state. No pre-crash epoch number proves that work has drained. Recovery settles
old internal row operations before examining migration journals, then resumes
the coordinator's saved phase and any required copy or cleanup. A saved Commit
is never changed to Abort. Temporary peer recovery failures are retried.

Control handles and row callbacks retain the admitting log term. Metadata
reads and source scans confirm quorum authority; writes bind the log proposal
itself to the captured term. Replica-group discovery finds the current owner
leader. Unknown physical outcomes fence or stop the process until recovery.
The older `MAKO_SHARD_METADATA_DIR` inspection path is separate and cannot be
combined with application-log-backed recovery.

The [migration chapter](architecture/sharding-migration.md) gives the protocol
proofs. Actual engine effects, serialization, filesystem durability, the Rust
runtime authority scopes and RPC delivery remain adapter obligations. Crash
qualification and release evidence are tracked in the
[optional-Masstree plan](plans/rocksdb-optional-masstree.md).

## Offline engine conversion

Stop every replica in the shard group before conversion. Each replica retains
its own term, vote, committed cut and uncommitted suffix. Convert the entire
group to the common storage format before restarting. This offline format
upgrade is separate from toggling Masstree on an already upgraded directory.

```sh
mako_convert --config old-node.yml --destination /data/shard-converted --engine masstree
# Or select --engine rocksdb to disable the Masstree layer.
mako_node --config /data/shard-converted/node.json
```

The source must have a shared `mako-rocks-log/1` log. The converter reads legacy
`rows` mirrors, old file-only Masstree checkpoints, or common RocksDB data. It restores
the published checkpoint and committed suffix without starting elections or
choosing transaction outcomes. An existing source log or data lease excludes a
running server. Already committed source effects may be replayed during recovery;
the source log, authority and directory remain available.

The new directory preserves schema, durability, membership, logical versions,
tombstones, pending prepares, saved decisions, receipts and exact replay
identities. It validates the copied rows and protocol index, writes a new
checkpoint at the committed cut, and publishes the directory with an atomic
rename that refuses an existing destination. `node.json` names the new paths and
sets `create: false`; `conversion.json` records the cut and counts. A failed
attempt leaves an unpublished `.mako-convert-*` staging directory for inspection.
Discard that staging directory rather than serving or resuming it.

Both output modes contain a `data` RocksDB directory and the same native
checkpoint and log formats. `--engine masstree` sets `engine.masstree: true`;
`--engine rocksdb` sets it to false. Conversion preserves existing migration
control state but does not enable migration on a fixed-placement schema, import
legacy external migration metadata, or infer missing log history. Those changes
require a separate authority migration.

The converter uses bounded row batches. Its protocol index and an old file-only
source image are held in memory; the target rows are streamed into RocksDB. Checkpoint publication pauses normal committed publication on live
nodes. Performance qualification remains a separate weekly/manual gate.

## Workers, RPC and ownership

Each participant stream runs on one affine native worker. Only owned requests,
redo and responses cross threads. The participant service creates workers as
streams arrive and retains them for later attempts. It currently bounds the
service to 256 participant workers; gateway streams use a separate ID range.

The RPC poll thread decodes owned bounded messages and dispatches work. It does
not run blocking journal or transaction work. A control worker performs recovery
admission and outcome lookup; participant workers serialize execution, prepare
and completion for their streams. A local transport uses the same service
without a socket round trip.

The deployment assumes trusted configured peers and non-Byzantine components.
Owner, transaction and lease checks detect stale or mismatched messages; they
are not a cryptographic authentication scheme. SRPC timeouts bound the caller's
wait and cancel pending futures, but do not prove that a remote effect stopped.

Rust ownership enforces worker affinity and keeps table and database allocations
alive. A prepared participant belongs to the service, so dropping a client frame
cannot invoke its ordinary abort-on-drop path. If that worker panics, its cleanup
fences the shard before releasing native locks and quarantines the migration pin.
The saved decision remains authoritative; serving resumes only after recovery.

## Clocks and identities

Keep these domains separate:

| Value | Purpose |
| --- | --- |
| Native OCC version and lock bits | Validate physical observations and conflicting writes |
| Distributed timestamp and application term | Common transaction row metadata and replay ordering |
| Transaction ID | Identify one coordinator attempt and deduplicate its records |
| Stream lease and request sequence | Fence retired senders and replay exact execution replies |
| Raft term and index | Identify consensus authority and committed log position |
| Routing version and admission epoch | Fence ownership and determine migration drain |
| Cache HLC and cache sequence | Local cache commit metadata and physical log identity |

The distributed runtime uses bounded typed timestamps from `mako-clock`. It
selects the maximum proposal after the lock barrier; each proposal exceeds that
participant's read dependency. Exhaustion is an error, never wraparound. The
existing row/redo encoding is `timestamp * 10 + application_term`. It is not a
Raft term, cache HLC or physical wall-clock timestamp.

The local cache has a separate full HLC contract. Its format and historical
qualification details are preserved in [cache timestamp design](reference/cache-timestamp-design.md)
and the [local ABI contract](reference/mako-local-abi-v1.md). The distributed
OCC + 2PC change does not silently convert that format or truncate an HLC.

## Build, configuration and programs

Use the build container and pinned Rust toolchain:

```sh
./docker_build.sh build
./docker_build.sh ci upper 32
```

`NATIVE_RUST` is the default CMake profile and includes both data adapters.
`ROCKSDB_RUNTIME` builds the same distributed applications without Masstree.
`ROCKSDB_RAFT` remains the separate local-transaction RocksDB service profile.
The retired `MAKO` application profile is rejected.
Native source and link audits constrain the remaining C++ dependency closure.

The primary programs are `mako_node`, `mako_transactions`, `mako_bench`,
`mako_client`, `mako_admin`, `mako_redis` and `mako_convert`. Each has `--help`. JSON/YAML parsing
and CLI compatibility live under runtime `configuration.rs` and
`configuration/`. Validation runs before creating databases, listeners or
workers.

Node configuration specifies owner count, listen addresses, worker counts,
tables, peer endpoints and optional replica groups. Choose either local
`persistence` with a log directory or `replication`. Configure the RocksDB
data directory and optional Masstree layer with `engine`. Both log policies
default to synchronous writes. Omitting both is an error unless `volatile: true` explicitly selects
development/test mode. Volatile mode cannot be combined with either persistent
policy. Creating a new durable directory is explicit; use `create: false` when
reopening it. Initial durable data is loaded through
transactions after authority is ready. Colocated shards require explicit ports
and consistent topology and schema.

Inside the container, a bounded two-shard example is:

```sh
python3 examples/run_native.py transactions --build build --shards 2 --workers 3 --count 50
```

The launcher uses Masstree over RocksDB data and a local RocksDB log by default. Add `--replicas 3` for Raft, or
`--volatile` for an explicitly ephemeral development run. The launcher
owns its processes, assigns ports and retains failure diagnostics. Consult its
`--help` for supported combinations.

## Development tests and proofs

Use the fast upper gate for ordinary transaction, sharding and migration changes:

```sh
./docker_build.sh ci upper 32
```

It disables C/C++ compilers, checks the dependency graph, verifies the production
Verus cores, runs semantic negative controls, and executes deterministic
contract tests, including the backend-independent Rust RocksDB facade. It has a five-minute deadline. Choose a CPU budget suitable for
the machine. Detailed reports record the source inputs and wall time. The
[OCC + 2PC qualification](testing/occ-2pc-2026-11-25/README.md) records the
checks for that cutover. The later
[unified-log qualification](testing/unified-rocks-log-2026-11-25/README.md) records
the shared local/Raft format, crash/refusal tests and explicit backend contract
checks. The optional-Masstree upper run completed in 175 seconds on 32 CPUs,
including 29 semantic negative controls. Backend/component, process recovery,
release, sanitizer and link-audit evidence is recorded separately in the
[optional-Masstree qualification](testing/rocksdb-masstree-2026-11-25/README.md).

The 2PC proof covers the lock barrier, participant-specific Yes votes, the saved
decision requirement, decision immutability, completion acknowledgements and
retained prepared protection. Journal encoding and native mappings are ordinary
Rust tested separately. Small exhaustive histories check committed point,
absence and range observations against a serial-order oracle. They complement
the state proofs; they are not a proof of Masstree or a distributed storage stack.

Run component and adapter tests at their own boundaries. Native integration
checks include real SRPC, cross-shard write skew, prepared teardown, RocksDB
restart, Raft failover and gateway receipt recovery. Storage, RPC and Raft
component suites remain separate. Sanitizers use the repository's dedicated
instrumented builds and fresh processes for poison/fail-stop cases.

Performance belongs to the manual/weekly workflow. Do not run a throughput
benchmark on every edit or use an old speculative-protocol result as a target
that changes commit semantics. See [development](developer/development.md) and
the [native test mapping](developer/native-rust-retirement.md) for commands.

## Operational interpretation

A conflict before a saved decision is a normal abort and may be retried as a new
attempt. Unknown means the caller lacks a confirmed terminal result. Resolve
its stable ID or exact gateway request; do not repeat the application body.

A prepared transaction can block when its coordinator authority is unavailable.
Restore that authority or its committed replica prefix. Releasing its locks or
deleting its journal entry is not a recovery procedure. A native installation
fault requires shard recovery, and new admission stays fenced meanwhile.

For a stuck migration drain, inspect unresolved transactions and their saved
coordinator decisions as well as active execution pins. For a failed startup,
check the retained configuration, directory identity, ports and journal recovery
error. Avoid killing unrelated processes or deleting durable state to clear an
error.
