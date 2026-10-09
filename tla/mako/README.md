# MakoV2 and live-process range sharding

[tla-rs](https://github.com/stonysystems/tla-rs)-style transition systems in
Verus, with inductive safety and strict-serializability proofs. The MakoV2 core
replaces the original-paper vector-clock/per-worker-stream model; its old
verification counts and model-checking results do not apply. Verification is
deductive, not bounded exploration. There is no model-checker dependency.

**MakoV2** has one globally comparable scalar transaction timestamp and one
physical Raft log per shard. There is no vector-clock mode, worker-indexed
replication stream, centralized timestamp allocator, or cluster-wide shared
transaction log. The configuration manager (CM) uses shard 0's existing log.

There are **two distinct proof scopes**:

- **MakoV2 core:** scalar timestamps, speculative installation/publication and
  fault recovery, with fixed `key % shards` placement.
- **Corrected range handoff:** dynamic placement among live shards, composed
  with an explicit atomic successful-OCC transaction-engine interface.

These are **not one combined migration-plus-failure theorem**. The native Rust
sharding crate now refines the corrected **placement** specification through
source-effect ghost-log certificates, under the native boundaries below.
It does not refine the transaction-history composition or prove the C++ engine.
The source-derived counterexamples describe the retired C++ sharding graph.

## Design sources and deliberate additions

The timestamp contract follows the newer Mako book on `masstree-rocks`, including
its distinction between implemented local storage and the distributed target:

- [Full-HLC/distributed timestamp design, pinned book](https://github.com/makodb/mako/blob/04864417e3cba7f70e046a8a06c97973368ed690/docs/mako-book.md):
  greatest-bound prepare replies, one coordinator-selected timestamp strictly
  above every bound, the same stamp at every participant, separated certification,
  installation and ready publication, recovery clock floors, and the warning
  that a greatest applied timestamp is **not** a complete-time frontier.
- [Single-Raft design](../../doc/single_raft_design.md) and
  [single-versus-multiple-Raft analysis](../../docs/dev/single_vs_multi_raft_analysis.md):
  one shared physical log per shard, not one log per worker. Local producer
  bookkeeping is not another replicated stream.
- [Mako book](../../docs/mako-book.md), sections 3–5: CM epoch control and the
  MakoV2 contract summarized below.

The following are **explicit design completions**, not claims about current
C++ behavior. They close gaps left by those sources and are part of the protocol
being proved:

1. Bind an unresolved local replication obligation before any speculative
   installation. Retire it only when its transaction record is durable.
2. Advance time with a **committed finite barrier**, justified by all local
   obligations and the future timestamp-allocation floor; never by the largest
   transaction timestamp found in the log.
3. Separate an epoch's finite recovery cutoff from its closure flag. No `INF`
   sentinel may convert speculative transactions into durable ones.
4. Give speculative replies an explicitly **provisional** meaning. Final client
   completion waits for epoch-correct reports covering the transaction from
   **every shard**, including its dependency closure.
5. Conservatively discard every old-epoch version above the final global cut.
   A more selective rollback algorithm is not silently assumed.

This is a protocol proof at the OCC, timestamp-service and Raft interfaces
below, **not a refinement proof of the distributed C++ implementation**.

## Timestamp and serialization contracts

`timestamp.rs` models the full HLC value `(physical_us:u64, logical:u32,
origin:u32)` as one lexicographically ordered scalar. Origins are nonzero and
exclusively leased; different owners cannot reuse an origin while their records
remain live. The module proves total ordering, an order-preserving integer
rank, origin separation, and allocation above both local and observed bounds.
Clock regression and logical carry are covered; invalid origins and full-format
exhaustion return `None`, not a wrapped timestamp. The current storage engine's
narrower packed representation is not proved here.

The protocol uses positive integer ranks from that order. It intentionally
admits more timestamps than a finite representation; the safety proof therefore
does not depend on timestamp-space exhaustion being impossible. No arithmetic
`+1` on a compressed clock component is treated as distributed timestamp
allocation. Failure epochs remain separate metadata.

`Prepare` abstracts one successful OCC certification and common-timestamp
binding operation. It requires current reads and available write locks, selects
`ts` strictly above all touched-shard/coordinator clock bounds and observed read
timestamps, takes write locks, raises those local clock bounds, and binds
obligations at every write shard plus the coordinator. A real distributed
implementation must preserve this atomic interface across RPC rounds: there
must be no interval in which a promised timestamp can fall below a barrier
without a corresponding obligation or reservation. That RPC refinement is not
proved by making `Prepare` atomic.

Conflicts and read dependencies respect timestamp order. The strict-serial
history witness is **certification order**, restricted to globally covered
transactions. Independent transactions may have tied ranks in the abstract
model or be certified out of scalar order; the timestamp service's exclusive
origins refine away such ties. Neither physical log order nor wall-clock
synchronization is used as a serializability argument. Final-before-invocation
real time is proved directly from history events; this is not a claim that
unrelated transactions on disjoint shards always allocate increasing HLCs in
wall-clock order.

## Normal transitions

| Action | Meaning |
|---|---|
| `Submit`, `Read` | Start a transaction and record optimistic point reads, including writer identity, value, epoch and timestamp. |
| `Prepare` | Successful validation, locking and common timestamp binding, including local obligations. |
| `Install` | Install one shard's speculative versions and release its write locks. No log publication is implied. |
| `Publish` | Publish a non-coordinator participant's transaction record after its installation. |
| `Certify` | After all writes are installed and all non-coordinator records published, append the coordinator record and mark Certified. Read-only transactions also have a coordinator certificate. |
| `Provisional` | Record an early speculative response; not part of final client completion. |
| `Replicate` | Commit the next physical log entry; a committed transaction record discharges its local obligation. |
| `Pulse`, `Barrier` | Raise a local clock floor; then, if justified, propose a finite closed-timestamp barrier. A pulse alone advances no watermark. |
| `Send`, `Receive` | Send an epoch-tagged committed frontier/closure report and merge it at an observer; delay, duplication and reordering are allowed. |
| `Final` | Complete only after a provisional response and all-shard coverage in the transaction's own epoch. |
| `Abort` | Abort a still-running transaction. Once timestamp-bound, a stalled transaction remains an obligation until durability or epoch fencing. |

For shard `s` and epoch `e`, a proposed `Barrier(e,b)` must satisfy:

```
previous_proposed_frontier(s,e) <= b <= local_clock(s)
for every unresolved local obligation (e,t): b < t
```

Future binds in `e` must exceed `local_clock(s)`. Thus a marker can cover neither
an outstanding hole nor a future low-timestamp transaction. Transactions may
publish to the shared FIFO in any timestamp order. `frontier` reads only
`Barrier` and `Close` entries, never `Tx` timestamps.

The global watermark is the scalar minimum of epoch-correct **committed** shard
frontiers. An observer must have a report from every shard; a missing component
is zero, not skipped. The observer's table of source reports is bookkeeping,
not a vector timestamp on transactions. A stale report cannot be relabeled as
a current-epoch report.

## Failure and recovery transitions

- `Crash` retains the committed prefix and any chosen prefix of the pending
  suffix, discards the remainder, fences transactional serving, clears local
  producer obligations and the failed observer's report cache, and drops local
  versions without a retained transaction record. It aborts in-flight
  transactions coordinated there, but never changes a Final record.
- `ProposeEpoch` appends `AdvanceSpecEpoch` to shard 0's same physical log.
  CM's epoch changes **only when that entry is durable**, whether by normal
  replication or retention during consensus recovery.
- `Recover` requires a newer committed CM epoch, adopts the next local epoch,
  and floors its clock at least as high as all retained transaction/barrier timestamps.
  `ObserveEpoch` fences a healthy lagging shard and its in-flight work.
- `alive` denotes transaction-serving readiness, not Raft quorum availability.
  The recovered Raft control service can propose/commit CM entries while
  speculative serving is fenced. Otherwise failure of shard 0 would deadlock
  its own epoch advance. A constructive trace exercises this case.
- `Close(e,cut)` is appended only after the shard has fenced epoch `e`.
  `cut` is the finite frontier already justified by retained/proposed old-epoch
  barriers. Committing Close commits its preceding prefix. If an uncommitted
  Close is lost in another failure, its replacement recomputes the cut from
  the retained prefix. A committed cut cannot subsequently change.
- Once all shard closure reports for `e` arrive, `Rollback` removes the local
  epoch-`e` versions above their scalar minimum. Closure is a separate boolean;
  it does not inflate that minimum. A new-epoch read of an old version requires
  that complete closure view and coverage of the version.

A failed or stalled participant can prevent progress before epoch fencing.
There is no bounded-lag or termination claim under arbitrary message delay,
missing quorums or repeated failures.

## External specification and proof structure

`history::serial_witness` mentions transaction bodies, returned reads,
invocation events and final-response events—not internal clocks, logs or
watermarks. A legal serial completion:

- contains every Final transaction exactly once;
- may include pending Certified transactions, never aborted ones;
- reproduces **every read of every selected transaction**, including pending
  transactions used to complete the history;
- respects every final-response-before-later-invocation real-time edge.

The proof first establishes `log_inv` and `occ_inv` initially and preserves both
under **every enabled action**, then inducts over arbitrary finite histories.
No transition guard assumes an invariant, serializability, or its own safety
postcondition. The number of shards, keys, transactions, epochs, steps and
message delays is not bounded by a model-checker configuration.

| Theorem / proof | Contract |
|---|---|
| `proofs_main::theorem_invariant` | The combined inductive invariant holds at every reachable state. |
| `proofs_main::theorem_makov2_strictly_serializable` | Every final client history has a legal strict-serial completion. |
| `proofs_main::theorem_reachable_final_durability` | Every Final transaction has durable records at all its log participants and retains all its writes. |
| `proofs_stable::theorem_final_is_irrevocable` | Final transaction records and results are unchanged in every later state, including after failures and rollback. |
| `proofs_history::theorem_atomicity` | A globally covered bound transaction is certified, with every write installed and present and every participant record durable. |
| `proofs_main::theorem_epoch_resolution` | Once all shards close an epoch, each bound transaction is either covered with all writes or doomed with no writes remaining at shards that have rolled back. |
| `proofs_history::theorem_dependency_rollback` | A prepared reader of a doomed writer is also doomed and cannot be Final. |
| `proofs_history::theorem_rollback_removes_doomed` | Once a write shard rolls back an epoch, that shard retains no version of a doomed transaction. |
| `log_lemmas` / `proofs_replication` | Prefix/frontier coverage, durable participant certificates, report soundness and immutable committed close cuts. |
| `timestamp` | Full-format scalar ordering and allocation contracts. |

`proofs_witness.rs` contains actual enabled action sequences from `init`, not
assumed favorable states: single-shard final completion; a cross-shard
read-modify-write; timestamp-2 publication before timestamp-1 with a blocked
barrier; partial-install failure followed by finite-cut rollback; and CM-shard
failure followed by a new-epoch final transaction. These prevent an empty or
stuck model from masquerading as a safety result.

## Dynamic range sharding

### Retired implementation reference and checked counterexamples

The historical source reference is `worktree-sharding` at
`a43a0666ac492df1f358b8033d4159a53fcac886`, integrated on `sharding-rebase` at
`44c6b5d5c278a2916a3112cf2fa4d6d7c2a4691f`. Paths and line numbers in this
counterexample table refer to that revision, not the current native Rust
implementation. Its C++ policy/cache/migration graph has been removed.

Its deployed migration driver is: background mirror; freeze writes; drain
registered writers; catch-up/final mirrors and checksum comparison; source
MOVED marker and deletion; partition publication; destination unfreeze.
Configuration, fences and migration jobs are volatile. The branch explicitly
excludes process failures.

| Source-derived result | Implementation anchor | Verus evidence |
|---|---|---|
| Boundary insertion, remapping and coalescing correctly reassign a valid interval or suffix and preserve outside routes. | `src/cluster/config_manager.h:1106–1161` | `sharding_partition::reassign_partition_theorem` |
| Independent slot/count/version reads can install a well-formed torn map that misroutes an **outside** key. | `src/cluster/cluster_config.cc:180–230`, `config_manager.h:1155–1161` | `sharding_publication::torn_snapshot_counterexample` |
| A read admitted before MOVED can fetch after deletion and return a non-linearizable clean miss. | `src/mako/lib/server.cc:747–826`, `src/mako/shard_data_plane.cc:149–167` | `sharding_reads::witness_admitted_read_crosses_drop` |
| Sixteen drain BEGIN executions, including retries with lost replies, can exclude a live writer and lose its acknowledged update. | `src/mako/lib/migration_fence.cc:258–285`, `src/mako/shard_data_service.h:814–837` | `sharding_drain::witness_wraparound_loses_acknowledged_write` |
| A stable-source put/delete mirror removes destination extras and preserves outside rows, given complete scan coverage. | `src/cluster/shard_data.h:101–162` | `sharding_mirror::theorem_complete_mirror` |
| Swapping values between distinct keys preserves the additive FNV checksum, including u64 wrap. Equality is **not** a byte-equality certificate. | `src/cluster/shard_data.h:64–81` | `sharding_mirror::theorem_swapped_values_collide` |

The three protocol counterexamples are enabled finite executions from explicit
initial states, not source-text tests or model-checker traces. They are
operational abstractions of the cited code, **not verified refinements of the C++ memory model or live
C++ race reproductions**. The checksum result holds for arbitrary hash outputs,
so it requires neither a guessed FNV collision nor injectivity.

### Corrected protocol

The corrected machine retains the branch's single active migration and
live-process envelope, but changes the unsafe interfaces:

1. RPCs name **canonical logical keys** and owner/incarnation grants, not stale
   physical table IDs. Range selection includes table identity and routing
   coordinate. Validate the requested source owns the interval.
2. Atomically admit **all accesses**, including reads and singleton operations,
   into an exact ownership-lease registry. Freeze blocks new admissions;
   existing holders finish. Release only after the engine outcome and all
   participant effects are terminal. No modulo bucket or transaction-lifetime
   bound substitutes for an empty registry.
3. Prepare a nonserving destination. Every copy mutation checks its local
   migration generation, copy round and staging role **together with the
   storage effect**. Final copy uses a distinct round after actual source drain.
   Complete coverage includes valid absent cells/deletion of destination extras.
   Sealing derives exact equality; it does not assume checksum injectivity.
4. Retire source serving while **retaining source data through the decision**.
   Accept generation-matched drain/ready/retire certificates. Publish the
   immutable directory snapshot and irreversible decision atomically.
5. Deliver the decision independently to activate the destination and clean up
   the source. The master finishes only after separately receiving both
   completion certificates. Lost acknowledgements may stall it.
6. Abort only before commit. Locally retained terminal fences reject delayed
   control/copy messages after abort, another move or partial return. Retain
   terminal admin outcomes and transaction results; losing a response never
   reopens the same operation.

Messages may be delayed, duplicated, reordered or never delivered. They are
genuinely issued, uncorrupted protocol messages; the issuance ledger is not a
cryptographic authentication proof. Client snapshots may be arbitrarily stale.
Participants use their local role/incarnation/fence, not a magical observation
of the master's current phase, when handling messages.

### Corrected proof contracts

| Export | Guarantee |
|---|---|
| `sharding_placement::theorem_invariant` | Every finite placement behavior preserves canonical physical/logical agreement and the complete ownership/copy/lease invariant. |
| `lemma_no_dual_serving`, `lemma_read_matches_logical` | At most one serving owner; an admitted physical read agrees with the logical cell, including presence/absence. |
| `lemma_held_grant_stable`, `lemma_exact_drain_observes_every_session` | Handoff cannot move a held ownership grant or overlook an admitted access. |
| `lemma_frozen_store_stable`, `lemma_seal_exact` | Drained source contents remain stable before decision; a sealable destination equals the source. |
| `lemma_snapshots_immutable`, `theorem_commit_partition` | Published snapshots never tear; the actual boundary-insert/remap/coalesce algorithm realizes the pointwise cutover. Partition labels decode to owner/incarnation grants, so coalescing does not erase incarnation boundaries. |
| `lemma_obsolete_control_rejected`, `lemma_obsolete_copy_rejected`, `lemma_terminal_replay` | Old work cannot mutate a later incarnation or restart a terminal admin operation. |
| `sharding_transactions::theorem_finite_behavior` | Every prefix has a strict-serial completion: actual commits only, every successful reply included, no aborted transactions, and Final-before-invoke real time respected. |
| `theorem_terminal_retained`, `theorem_retry_no_effect` | Terminal transaction results survive handoffs, delayed replies and retries without repeated effects. |

Unqualified entries above belong to `sharding_placement`, except the final row,
which belongs to `sharding_transactions`.

The transaction model supports point `Read`, `Put`, `Delete` and `Add`, with
absence represented explicitly and absent-as-zero for `Add`. Reads occur after
separate lease admission. Successful commit validates the recorded cells against
**current physical reads**, then atomically applies the logical multi-key write
set. The first observation of a key is retained; duplicate fetches cannot replace
it. Reply is separate from commit. The serial witness completes pending commits
as needed and independently replays operation semantics; migration steps leave
the logical application store unchanged.

### Constructive executions

The witnesses build executions from the actual initial state, proving the
guards of progressing actions—not merely assuming a desirable final state:

- `sharding_witness::witness_admitted_operations_and_handoff`: a cross-shard
  read/modify/write and a separate reader remain admitted across freeze.
  Both finish before drain; final copy carries the updated value, and a delayed
  background packet cannot overwrite it. Publication precedes destination
  activation; a new reader sees the updated value. Administrative completion
  waits for both participant receipts, and repeated replies retain the result.
- `sharding_abort_witness::witness_abort_then_successful_retry`: abort after
  actual freeze/drain work, preserve source readability, reject nonce reuse,
  then complete a fresh generation despite delayed authentic abort, freeze
  and copy messages from the old attempt.
- `sharding_return_witness::witness_partial_return_and_fenced_read`: move two
  keys, then return only one. Old control/copy messages cannot mutate the
  returned incarnation. A stale grant is rejected even though the physical
  owner is again the original shard; a fresh cross-shard read succeeds.
  The nonreturned key and the other logical table remain unchanged.

These are nonvacuity proofs alongside the arbitrary-finite-behavior induction,
not a substitute for it or a liveness proof.

### Representation and implementation obligations

- The corrected model has an arbitrary finite pool of live shards and arbitrary
  finite logical-key universe, including valid absent keys—not just existing
  rows. No fixed key/transaction/step bound is used by the safety induction.
  A partition representation relation connects its directory projection to
  ordered metadata; byte encodings and native table-binding code are not proved.
- Logical `Cell.writer` is application provenance/OCC revision, not a native
  STO TID or a transplanted Raft record. A new owner may initialize fresh native
  versions because old leases have drained; version comparisons must remain
  sound within the held ownership incarnation.
- `Resolve` is an **atomic successful transaction-engine interface**. The
  composition proves its OCC/history contract with migration, not the C++
  distributed certification/installation RPCs. The core MakoV2 durability and
  epoch-recovery theorems do not automatically cover changing log participants.
- The native implementation must make guard checks atomic with the protected
  storage effects. Checking a generation or MOVED flag, releasing the guard,
  and accessing storage later does not implement this model.
- Final mirror coverage requires complete source/destination scans. A short
  nonempty batch is not EOF; an error is not successful empty EOF. Optional
  upper bounds have the same infinity meaning in routing and the data plane.
  A physical index spanning several warehouses cannot be wholly deleted when
  only one warehouse's routing interval moves.
- IDs/generations never wrap or get reused. A fixed-width implementation must
  refuse allocation on exhaustion. Outcomes, message provenance and fences are
  retained; bounded-memory reclamation needs a separate quiescence/delay proof.
- No process crash/restart, Byzantine corruption, live membership join/removal,
  hash-ring rebalance, range-query semantics, fairness or bounded completion
  time is proved. The current metadata-only membership APIs are not safe
  lifecycle protocols.

### Native Rust correspondence

`src/cluster/lib.rs` is both the Cargo staticlib entry point linked into Mako
and the whole-crate Verus entry point. Its executable actors are verified from
the same source; proof-only imports reference this directory's independent
`sharding_partition`, `sharding_placement`, and `sharding_mirror` modules.
There is no alternate executable model in the production data path.

| Native source under `src/cluster/` | Correspondence |
|---|---|
| `directory*`, `routing*` | Ordered interval updates, immutable complete owner/epoch snapshots, and snapshot decoding/installation. |
| `leases*`, `participant*` | Exact point/range admissions, including absent keys and readers; freeze/drain and generation/round-fenced participant metadata. |
| `migration*` | Retained caller nonces, fresh generations, issued controls, receipts, publication and terminal outcomes. |
| `storage*`, `transfer*` | Ordered exact mirror, explicit EOF, destination-only deletion, cleanup completion and real failure prefixes. |
| `ghost_log*`, `execution_refinement*`, `transfer_*proofs` | Raw field/entry effects and closed certificates relating concrete views to independent placement paths. |

The ghost journal records raw effects, not a logger's assertion that the
desired model action happened. An open segment may contain intermediate
states that do not satisfy placement invariants. Closing it requires both
its replayed concrete effect and a proved independent-spec path. Private
completion/certificate tokens distinguish metadata changes from actual
completed storage and emitted responses. A cleanup error after metadata
changed is therefore not certified as rejection or stutter. Loader commits
establish the initial journal; bootstrap writer identities occupy a namespace
disjoint from ordinary transaction identities.

The theorem is conditional on the explicitly named embedding contracts:
canonical finite logical-key/byte labeling (including absence), authentic
issued RPCs and captures, actual atomic engine effects and their frames,
and exclusion of competing transfer effects on the selected range. A metadata
mutex does not exclude already-admitted writes to unrelated ranges.

`host.rs`, `wire.rs`, `runtime.rs`, `ffi.rs`, and `gateway_ffi.rs` are native
boundary code, excluded from Verus by `cfg(not(verus_keep_ghost))`. The C++
engine, RPC/lifetime adapters, warehouse binding and allocator/RCU callbacks
are also in the trusted embedding boundary. Executable verification does not
certify those handlers merely because the source audit finds their callers.
Real-engine smoke scenarios exercise this boundary but are not a proof of it.
`full_scan.rs` supplies native byte/FFI/callback adapters around the verified
`full_scan_core.rs`; those adapters are likewise not a verified handler theorem.

In particular, `execution_refinement` imports **placement**, not
`sharding_transactions`: the independent model's strict-serializability theorem
is not a native distributed transaction-history refinement. Live migration
requires fixed live owners and non-replicated mode. Crash/restart, membership
change, Raft/migration composition, liveness and retained-state reclamation
remain outside the result.

## Layout

| Files | Responsibility |
|---|---|
| `types.rs`, `timestamp.rs` | Scalar timestamps, one log per shard, transaction and recovery state. |
| `normal.rs`, `recovery.rs`, `behavior.rs` | Entire transition system and finite behaviors. |
| `log_invariants.rs`, `log_lemmas.rs`, `proofs_replication.rs` | Local obligations, closed frontiers, recovery and report induction. |
| `invariants.rs`, `proofs_occ.rs` | OCC, locks, versions, dependency order and combined invariant. |
| `history.rs`, `proofs_history.rs`, `proofs_stable.rs`, `proofs_main.rs` | External history contract and safety theorems. |
| `proofs_witness.rs` | Constructive nonvacuity executions. |
| `sharding_partition.rs`, `sharding_mirror.rs` | Ordered metadata transformation, exact stable mirror and checksum counterexample. |
| `sharding_publication.rs`, `sharding_reads.rs`, `sharding_drain.rs` | Source-derived operational counterexamples, explicitly separate from the corrected protocol. |
| `sharding_placement*.rs` | Corrected ownership, leases, copy rounds, immutable publication, abort/retry and induction. |
| `sharding_transactions.rs` | Successful-OCC composition, observable serial histories and retained terminal results. |
| `sharding_*witness*.rs` | Enabled forward, abort/retry and partial-return executions. |
| `scripts/verify.sh`, `scripts/verify_controls.py` | Complete-crate verifier entry point and negative proof controls. |

## Verification

Pin: **Verus `0.2026.08.02.b677dd5`, Rust `1.97.1`**. Run the commands in a
Docker development container with that toolchain available; do not run project
tests on the host.

Both CI jobs provision this exact Rust release with
`bash scripts/ci/install_rust_toolchain.sh 1.97.1 /opt/rust` before building or
verifying. The Dockerfile uses the same official-tarball installer, which checks
the release archive's published SHA-256 and does not require rustup. PR checks
consume the previously published CI image, so they upgrade an older toolchain
in the job rather than relying on a Dockerfile change having been published.

`ci/ci.sh` exports its resolved `BUILD_DIR` to the proof and smoke runners as
well as the build: CI defaults to `build`, while `docker_build.sh` supplies
`build_docker`. A runner must use the same directory as the compile step.

```bash
# Inside the container, from the repository root:
VERUS_PATH=/path/to/verus tla/mako/scripts/verify.sh
# Or verify the unchanged crate first, then run all negative proof controls:
VERUS_PATH=/path/to/verus python3 tla/mako/scripts/verify_controls.py
```

For the native cutover, use the repository's Docker gates:

```bash
./docker_build.sh ci nativeShardingProof
./docker_build.sh ci nativeShardingSmoke
# After a Docker build:
./docker_build.sh ci-quick nativeShardingSmoke
```

The proof gate attests the pinned Verus archive, verifies all reachable
production proof modules without function/module filters, runs the independent
model and all negative controls, then runs locked Cargo tests. The source
coverage audit is an inventory, not a proof.

The standalone model verifier checks the **entire model crate**, including
witnesses, with `--no-cheating`. Its controls runner invokes that baseline first, then
copies the model into temporary directories and requires semantic proof
failures in the affected safety lemmas. It injects thirteen bugs:
the seven core MakoV2 mutations (ignoring local holes; bypassing finality
coverage; skipping read validation; accepting a non-increasing timestamp;
treating maximum log transaction time as a frontier; relabeling stale reports;
inflating an epoch's close cut), plus six sharding mutations (retaining
destination-only keys; accepting stale physical observations; admitting new
leases after freeze; accepting the wrong copy round; sealing without complete
coverage; accepting an obsolete abort).
Mutants target named safety lemmas, so unrelated constructive-trace failures
cannot pass a control. Type errors, verifier crashes and resource exhaustion
also do not count as successful controls. Originals remain unchanged.

No `assume`, `admit`, external proof bodies, solver resource-limit overrides,
or model-checking results are used to establish the result.

### Checked result

On the pinned toolchain in a network-disabled container
(`python:3.13-slim-trixie`, model mounted read-only):

```
verification results:: 280 verified, 0 errors
```

All **13 negative controls passed**: each mutation produced one semantic failure
in its named safety lemma. The complete baseline includes the five core MakoV2
constructive executions, timestamp proofs, strict serializability, durability,
atomicity and recovery theorems, plus the sharding algorithms, source-derived
counterexamples, corrected ownership/history induction and enabled
forward/abort/retry/partial-return executions. The two proof scopes remain
separate as described above; this is not a combined crash-safe migration theorem.

The native cutover was also checked in the Docker development image on that
same pinned toolchain:

```
native src/cluster/lib.rs: 683 verified, 0 errors
independent model:        280 verified, 0 errors
semantic negative controls: 13 passed
native Cargo tests:         12 passed
```

The native MBTA/STO/srpc smoke passed with separate processes and with two owners
in one process: raw and warehouse handoff/return, values and absence,
destination-only cleanup, post-handoff writes/deletes, forward/reverse pagination,
old epochs, retained outcomes, held-lease abort, lost successful Commit reply,
and replay of previously issued controls. Fault injection drops a reply at the
real peer callback boundary; it does not substitute an engine or model oracle.
Separate live `dbtest` runs completed warehouse forward/return and `new_order`
migration while transactions continued in both topologies.


## Boundaries of the result

- **Raft interface, not a Raft proof.** A committed prefix cannot be lost or
  rewritten under the durable-quorum failure model. Election, quorum protocol,
  follower application and physical storage recovery are abstracted.
- **OCC/RPC interface, not a transaction-engine refinement.** Successful validation,
  common timestamp binding and obligation admission are atomic at the model
  boundary. Installation and publication are separate interleavable actions.
- **Timestamp service.** Origin leases and recovery fencing are trusted service
  contracts. The proof does not establish physical clock synchronization or
  the current packed storage encoding.
- **Core placement and membership.** The scalar/single-Raft model has arbitrary
  positive shard count, fixed for a history, with `key % shards` placement and
  integer values initially zero. Its operations are point `Read`, `Put`, `Add`.
  The separate live-process range-handoff model has the scope stated above;
  neither model proves live process membership change or range-query semantics.
- **Safety, not liveness.** Constructive progress examples are not a fairness
  proof. Histories, logs and replay outcomes are retained; their reclamation and
  follower read APIs are not verified.
- **Provisional replies are revocable.** Treating them as final successful
  operations is outside—and contradicts—the proved client contract.
- **Trusted base:** Verus, its supported vstd specifications, Rust frontend and
  Z3. The theorem does not automatically transfer to current legacy gossip,
  timestamp or recovery code.
