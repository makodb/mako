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
| Runtime | `crates/mako-runtime` | Native adapters, service-owned participant workers, client, gateway, process ownership and recovery |
| Sharding and migration | `src/cluster` | Immutable routing snapshots, admission, range transfer, receipts and publication |
| Storage-host decisions | `crates/shard-host-core` | Checked translation between migration effects and physical storage |
| Local transaction manager | `crates/sto-core` | OCC read/write sets, locking, validation and terminal cleanup |
| Ordered storage | `crates/sto-masstree`, `crates/sto-tpcc-ffi` | Masstree adapter, versioned rows, scans, retained affine workers and owned redo |
| Persistence | `crates/sto-rocksdb`, runtime `durable.rs` | Atomic persistent row, intent, decision and receipt records |
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

Each participating shard chooses one journal policy:

| Policy | Journal acknowledgement | Restart guarantee |
| --- | --- | --- |
| Volatile | In-memory transition | None |
| Local persistent | Synchronous RocksDB transition | Local process restart |
| Replicated | Current Raft authority and applied committed record, with synchronous durable storage | Recovery from the surviving committed replica prefix |

A configuration with synchronous writes disabled cannot advertise durable
transactions. All members of a transaction must agree on the durability policy.

The journal records `Begin`, `Prepare`, `Decide` and `Complete`. A prepare stores
the full descriptor, final row intents and any gateway receipt. Completion
atomically writes committed rows, the participant outcome and the gateway
receipt in the durable adapter. Prepared rows stay separate from committed rows.
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

The native durable format does not import the retired C++ WAL. Existing native
row-command records remain readable. The new transaction journal uses a separate
versioned command. Mixed old/new protocol operation is not a supported rolling
upgrade: old distributed Timestamp/Validate/Install commands are refused by the
new runtime.

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

Live migration currently supports volatile, unreplicated shards. Combining it
with local persistence or Raft is rejected by configuration validation. The
conditional durable sharding models do not establish production support for
that combination. See [sharding and range migration](architecture/sharding-migration.md)
for the transfer protocol and its proof boundary.

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

`NATIVE_RUST` is the default CMake profile. `ROCKSDB_RAFT` is the separate direct
RocksDB service profile. The retired `MAKO` application profile is rejected.
Native source and link audits constrain the remaining C++ dependency closure.

The primary programs are `mako_node`, `mako_transactions`, `mako_bench`,
`mako_client`, `mako_admin` and `mako_redis`. Each has `--help`. JSON/YAML parsing
and CLI compatibility live under runtime `configuration.rs` and
`configuration/`. Validation runs before creating databases, listeners or
workers.

Node configuration specifies owner count, listen addresses, worker counts,
tables, peer endpoints and optional replica groups. Choose either local
`persistence` or `replication`. Both default to synchronous writes. Creating a
new durable directory is explicit. Initial durable data is loaded through
transactions after authority is ready. Colocated shards require explicit ports
and consistent topology and schema.

Inside the container, a bounded two-shard example is:

```sh
python3 examples/run_native.py transactions --build build --shards 2 --workers 3 --count 50
```

Add `--persistent` for local durability or `--replicas 3` for Raft. The launcher
owns its processes, assigns ports and retains failure diagnostics. Consult its
`--help` for supported combinations.

## Development tests and proofs

Use the fast upper gate for ordinary transaction, sharding and migration changes:

```sh
./docker_build.sh ci upper 32
```

It disables C/C++ compilers, checks the dependency graph, verifies the production
Verus cores, runs semantic negative controls, and executes deterministic
contract tests. It has a five-minute deadline. Choose a CPU budget suitable for
the machine. Detailed reports record the source inputs and wall time. The
[OCC + 2PC qualification](testing/occ-2pc-2026-11-25/README.md) records the
checks for this cutover.

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
