# OCC and two phase commit

Replace Mako's distributed commit protocol with optimistic execution and explicit
two phase commit. Keep the upper protocol independent of Masstree, RocksDB, SRPC
and Raft implementations. Starting revision: `7cdaecafe` on `mako-dev`.
Implementation branch: `occ-2pc` in `/var/tmp/mako-shard-rust`.

## Status and scope

The user authorized implementation through completion, including updating the
book. The agreed OCC rule is to acquire every transaction write lock across all
participants before final read validation. Do not add shard-wide certification
locks. Implementation is in progress; the inventory is complete.

The implementation target includes recovery for durable/replicated deployments,
with an explicit volatile mode for development. This follows the recommended
scope stated when implementation began. The state and failure rules below apply
to both modes; restart
guarantees apply only when every participating shard supplies durable records.

The initial design prioritizes a small protocol with a direct safety argument.
Do not add early client success, speculative visibility, dependency rollback,
one-phase commit shortcuts or read-only voting shortcuts. Evaluate optimizations
after the baseline passes its correctness gates.

## Current implementation

| Area | Current behavior | Required change |
| --- | --- | --- |
| `mako-protocol-core/src/coordinator.rs` | Flush remote writes, lock locally, select timestamp, validate locally/remotely, install locally/remotely, append local log, finish | Explicit prepare votes, immutable global decision before installation, and decision delivery/recovery |
| `mako-protocol-core/src/admission.rs` | Active, Sealed, Committed, Aborted and Unknown; newer terms can replace active work | Prepared state that survives request-lane replacement and cannot be aborted by a newer request or timeout |
| `mako-runtime/src/receiver.rs` | Stage acquires remote write locks; Install installs, logs and finishes each participant | Freeze each participant's read/write set at prepare, retain its vote and protections, apply only the recorded decision |
| `sto-core/src/participant.rs` | Validated participants retain write locks but dropping them aborts; installed participants finish committed | Transfer ownership of prepared work to transaction recovery rather than ordinary request-guard cleanup |
| `mako-runtime/src/replication.rs` | Replicates already-installed row redo; waits for application before returning append success | Typed participant-prepare, coordinator-decision and participant-completion records, with replay semantics for each |
| `mako-runtime/src/durable.rs` | Persists committed rows, applied positions and gateway receipts | Persist unresolved prepares and decisions without treating prepared writes as committed rows |
| `sto-masstree/src/distributed.rs` | Distributed history, term visibility and selected-version dependency tracking | Identify and remove speculative-policy callers from the distributed path; retain version metadata needed by OCC, replay and scans |
| Gateway and single-shard operations | Separate commit/receipt integration | Participate in the same conflict protection and durable publication rules |
| Migration | Epoch guards protect active/unknown work; unresolved work can block drain | Prepared work retains protection across disconnects and, in durable mode, recovery before serving traffic |

The old `docs/architecture/speculative-2pc.md` describes the removed C++/Paxos
implementation. It is historical context, not the specification of the current
Rust path. In particular, the current Rust coordinator already waits for its log
callback; the problem is the ordering and recovery of distributed decisions.

## Protocol target

1. Execute optimistically over committed data, with read-your-writes from private
   intents. Track point reads, absent keys and scan predicates. Fix participant
   membership and route generations before prepare begins.
2. Prepare every participant, including the coordinator's local shard. Freeze
   the exact intent, acquire all participants' write locks before any participant
   performs final validation, then certify reads and scan predicates. These are
   substeps of the prepare phase. A Yes vote is a promise to obey the final
   decision, not permission to install. In durable mode, persist the complete
   prepare record before Yes.
3. Choose Commit only after every enrolled participant has voted Yes. Record one
   immutable Commit or Abort decision at the transaction's coordinator authority.
   In durable mode, persist or replicate that decision before sending it.
4. Deliver the decision idempotently. Commit applies the prepared intent and
   records completion; Abort discards it and records completion. Release physical
   conflict protection and migration admission only after that terminal action.
5. Return client success after all participants acknowledge Commit. A lost reply
   leaves a queryable transaction ID; it never authorizes a fresh execution of
   the same logical request. Once Commit is recorded, failures require completion
   or recovery, not a conflicting Abort decision.

Coordinator states: Executing, Preparing, CommitDecided or AbortDecided, Complete.
Participant states: Executing, Prepared, Committed or Aborted. Keep uncertainty
about an RPC or log result separate from the recorded transaction decision.

Prepare, Commit, Abort and outcome lookup carry a stable transaction ID, the
coordinator authority, protocol version, participant membership and the expected
intent identity. Reusing an ID with different intent is an error. Terminal
records fence delayed requests; garbage collection needs an explicit rule that
prevents an old request from becoming a new transaction after records disappear.

## OCC and conflict protection

Local validation is normal in distributed OCC. The unsafe composition is a
one-time local check with neither retained read/predicate protection nor an
ordering rule that makes the votes globally compatible. 2PC agrees on a commit
decision; it does not supply that missing concurrency-control rule.

The current coordinator flushes and locks remote writes before the validation
round, then locks local writes before validating any participant. Replacing that
sequence with independent lock-and-validate Prepare handlers needs a new
cross-shard argument.

For example, transaction A reads x and writes y, while B reads y and writes x.
If each participant certifies an unprotected read before the other transaction
locks its write, independently successful votes can admit a cycle. Range scans
and absent-key reads require the equivalent protection against phantoms.

The baseline preserves the existing all-participant write-lock barrier before
final validation. Freeze membership and intents first; acknowledge every write
lock acquisition before starting any final validation. Validation must reject
changed observations and conflicting locks, including the engine's absence and
scan-predicate checks. Retain acquired protections through the final decision.
This adds coordination within Prepare, not a third transaction-commit phase.

Do not add a shard-wide reservation. It is unnecessary for the proposed baseline
and would serialize unrelated transactions. A different design could instead
retain appropriate read/write and predicate locks from each participant's local
validation through decision, but that is not required merely to implement 2PC.

Prepared ownership must outlive the request and, in durable mode, be reconstructed
before serving new conflicting work. An unprepared participant may vote No on a
lock conflict; a prepared participant cannot release protection on timeout.
Audit every live writer, gateway, replay and migration path against these rules.
Do not claim global serializability until the composition and its physical
adapter obligations have been checked, including read-only transactions.

## Recovery and durability

For durable mode, a prepare record contains enough owned data to reconstruct the
participant's immutable intent, route identity, vote and protections without the
original thread or client connection. Recovery restores conflict protection before
serving new transactions. Prepared data is separate from committed rows.

The coordinator's record is the authority for Commit or Abort. Use the existing
Raft group for replicated configurations and the existing RocksDB adapter for
local durability. Do not add a consensus implementation to the upper layer.
Authoritative decision writes serialize against leadership changes and preserve
an earlier decision. A failed append may have succeeded and must be resolved by
lookup or retry of the same record.

A recovered participant queries that authority and retries delivery. Missing or
unreachable information does not permit unilateral abort after Yes. Choosing
Abort after a coordinator restart requires authority that fences any old writer
and proves that no Commit decision can still be established. Ordinary 2PC may
block while the decision authority is unavailable.

Committed row application and the participant completion receipt must be
recoverable together. Test crashes before and after each log acknowledgement,
row application and receipt publication. Preserve terminal decisions until the
request-retention and recovery policy permits reclamation.

Volatile mode supplies the same protocol operations in memory and explicitly
makes no process-restart guarantee. Do not silently mix volatile participants
into an attempt advertised as durable.

The existing restriction against live migration with replication or local
persistence remains until that combination has its own completed recovery
design. This transaction change does not by itself remove the restriction.

## Implementation stages

| Stage | Deliverable | Completion check |
| --- | --- | --- |
| 1 | Inventory, protocol specification and recovery scope | State transitions, authority, conflict protection and success semantics are explicit |
| 2 | Executable coordinator/participant core and journal contracts | Verus checks the production transitions; deterministic tests reject conflicting votes/decisions and premature installation |
| 3 | Preserve the global write-lock barrier and implement prepared ownership | Cross-shard write-skew, point/absence/scan conflicts, read-only transactions, gateway concurrency and prepared teardown tests pass |
| 4 | Durable prepare/decision/completion and recovery adapters, if in scope | Restart and leader-change fault matrix preserves decisions, intents and conflict protection |
| 5 | Runtime, client and migration integration | The deployed path uses the verified core; lost replies resolve by transaction ID; prepared work blocks migration drain |
| 6 | Remove obsolete speculative paths and qualify the replacement | Fast upper gate, focused native regressions, protocol compatibility checks and updated documentation pass |

## Checkpoints

- Created branch `occ-2pc` from `7cdaecafe`, now on `mako-dev`.
- Located the book at `docs/mako-book.md`. Its speculative commit chapter and
  related architecture text require replacement with the implemented protocol.
- Starting the shared executable state machine and durable journal contracts.
- Added the executable coordinator/participant state, journal transitions and
  generic commit driver. The full upper check passed in 147.200 seconds with 32
  CPUs: 1495 sharding, 40 storage-host and 46 protocol verified entries, 20
  semantic negative controls, and 53 executable cases. Report:
  `build_docker/upper/runs/20261125T035517-6vkos7t2/report.json`.
- The native prepared-redo regression passes: exporting intent leaves the
  participant validated and abortable without publishing rows. A real RocksDB
  restart regression passes across prepare, decision and completion boundaries;
  a conflicting later decision retains the first outcome.
- Local and replicated journal adapters compile. The foreground runtime still
  uses the previous coordinator; production cutover and Raft journal regressions
  remain pending.
- Prepared work must be owned by the service rather than a client stack frame.
  Next, route the local participant through the same retained worker service as
  remote participants, keeping the public transaction API. This permits client
  teardown without dropping a prepared STO frame, and gives recovery a worker
  that can finish the saved decision. Use row/predicate locks, not a shard-wide
  certification reservation.

## Verification and test obligations

Verus targets should establish:

- A participant commits only from its matching prepared intent and a Commit
  decision whose participant set has all Yes votes.
- A transaction never acquires both Commit and Abort decisions. Participants
  cannot finish with conflicting decisions for the same transaction ID.
- A Yes vote freezes membership and intent, retains conflict/migration protection,
  and forbids autonomous abort on timeout, disconnect or term change.
- No committed row is installed before the authoritative Commit decision.
- Duplicate messages preserve the decision and do not repeat committed effects.
- Retryable Abort is returned only for an authoritative Abort decision. A
  committed transaction cannot become retryable because delivery failed.
- Given successful local certification, available journals and eventual delivery,
  the protocol can complete. Do not accept an implementation that always aborts.

Separate serializability reasoning from atomic-commit reasoning. The storage
contract must state exactly what conflict protection certifies, including
predicates and absence. Add concurrent executable histories with a serial-order
oracle; individual successful state transitions alone do not prove serializability.

The fast suite uses small in-memory storage, journals and controllable transport.
It exercises the same executable core as production. Cases include partial
prepare, No votes, lost/repeated/reordered messages, changed payloads, delayed
Abort after Commit, decision-append uncertainty, mixed durability rejection,
client disconnect, two- and three-shard dependency cycles, scan phantoms,
migration freeze, and recovery from each durable boundary when enabled.

Semantic negative controls deliberately allow Commit without all Yes votes,
installation before a decision, a changed decision, timeout-abort after Yes,
released prepared protection, stale identity reuse and reject-all behavior.

Keep `./docker_build.sh ci upper 32` within the existing five-minute bound and
record its wall time. Lower-component tests stay separate. Run focused native
adapter/process and relevant sanitizer regressions after integration changes.
Performance remains weekly/manual; do not trade protocol semantics for the old
benchmark result or add performance runs to normal compilation.

## References

- [Gray and Lamport, Consensus on Transaction Commit](https://www.microsoft.com/en-us/research/wp-content/uploads/2004/01/consensus-on-transaction-commit.pdf),
  especially the 2PC state, stable-storage and failure rules in section 3.
- [Tu et al., Speedy Transactions in Multicore In-Memory Databases](https://people.csail.mit.edu/stephentu/papers/silo.pdf),
  for local OCC read/write-set validation. Its shared-memory correctness argument
  does not substitute for an argument about independently timed distributed votes.
