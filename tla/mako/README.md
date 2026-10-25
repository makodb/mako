# MakoV2 and the sharding protocol

[tla-rs](https://github.com/stonysystems/tla-rs)-style transition systems in
Verus, with inductive safety and strict-serializability proofs. The MakoV2 core
replaces the original-paper vector-clock/per-worker-stream model; its old
verification counts and model-checking results do not apply. Verification is
deductive, not bounded exploration. There is no model-checker dependency.

For a readable end-to-end protocol description, start with
[Sharding and range migration](../../docs/architecture/sharding-migration.md).
It separates the algorithm, application-transaction integration, storage
primitives, recovery, and implemented-versus-conditional behavior. This README
is the detailed specification and verification reference.

**MakoV2** has one globally comparable scalar transaction timestamp and one
physical Raft log per shard. There is no vector-clock mode, worker-indexed
replication stream, centralized timestamp allocator, or cluster-wide shared
transaction log. The configuration manager (CM) uses shard 0's existing log.

There are **three distinct proof scopes**:

- **MakoV2 core:** scalar timestamps, speculative installation/publication and
  fault recovery, with fixed `key % shards` placement.
- **Sharding protocol against interfaces:** dynamic ownership, crash recovery,
  configured-owner removal/rejoin, conditional progress, and administrative
  retention. Transaction atomicity/settlement and committed Raft prefixes are
  explicit contracts; the sharding algorithms themselves are not assumed correct.
- **Native same-source correspondence:** Rust actors, canonical byte values,
  sharded scans and source histories, plus the separate durable-KV native
  library against explicit storage/transaction-recovery interfaces. This is
  not a proof or implementation of the foreign production persistence adapter.

These are not a native crash-safe migration deployment or one combined proof
of MakoV2's distributed transaction engine under changing log participants.
Production migration still requires fixed live owners and non-replicated mode.
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

Additional interface and native contracts:

| Export | Guarantee |
|---|---|
| `sharding_bytes::value_code_injective` | Different byte sequences cannot denote the same logical value; empty bytes and absence remain distinct. |
| `sharding_recovery::theorem_reachable_reconstruction`, `theorem_serving_gate` | Committed per-authority checkpoints plus engine suffixes reconstruct the placement state; serving requires completed replay and a fresh durable admission. |
| `sharding_recovery::theorem_stale_incarnation`, `theorem_safe_removal`, `theorem_rejoin_fence` | Restarted/removed owners reject stale credentials; removal cannot abandon directory ownership, active handoff endpoints or leases; numeric owner reuse advances its membership fence. |
| `sharding_progress::theorem_service_eventual_completion` | Under the primitive fairness/settlement contracts below, an admitted migration eventually finishes with the necessary actual historical physical cleanup. |
| `sharding_retention::{theorem_collection,theorem_forgotten,theorem_space}` and `protocol` | Acknowledged, checkpoint-covered administrative records can be collected without nonce reuse; stale requests/control/copies are fenced, and materialized administrative state has a configured bound. |
| Native `full_scan_proofs::{sharded_scan,native_fixed_coordinate_scan}` | Paged forward/reverse results agree byte-for-byte with the trusted transactional image and placement projection, including fixed warehouse coordinates. |
| Native `full_scan_proofs::{native_direct_scan,native_direct_fixed_coordinate_scan,mixed_sharded_scan}` | Direct local callbacks and mixed local/paged traversal have ordered prefix semantics and exact complete results under one captured snapshot, complete admitted scopes and a common transaction read view. |
| Native `execution_refinement::{finite_source_history,loaded_source_history}` | Actual source-step observations construct a placement history and its invariants; the caller supplies no already-certified model path or ghost journal. |


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

### Native engine-epoch admission and draining

`sharding_epoch_admission*.rs` and `sharding_epoch_drain*.rs` specify reuse of
the existing STO epoch domain for cached admission and a source grace period,
instead of registering each point/range lease. The native integration follows
this contract, with private online pre-copy before Freeze. There is **no
additional authority-epoch counter** and no commit-time epoch-equality test.
Fresh native and independent proof/control runs and real-engine smoke passed;
the dated record below separates them from earlier lease-path results.
The conditional durable path retains its persistent leases. The separate
commit-time alternative is documented below.

#### Metadata, pins, and the publication boundary

A view is immutable sharding metadata, not an MVCC snapshot of application
values. Its grants cover potential accesses, including absent coordinates,
unbounded interval tails, and every physical row alias under an ownership
coordinate. Ownership versions and migration generations retain their existing
fencing meaning.

The admission model separates the reader protocol into observable transitions:

1. `Probe` samples the existing engine epoch.
2. `Announce` publishes the sampled epoch; no view is usable yet.
3. `Validate` rechecks that the engine epoch still matches. Otherwise the
   reader must `Resample` and announce again before accessing metadata/storage.
4. `Capture` reuses the worker's immutable metadata only when the cache tag
   matches the **retained entry epoch**. A miss loads the published object.
5. `Retry` retains that entry epoch and captured object. The protection lasts
   through RPC waits, engine effects, and cleanup; only terminal `Exit` releases
   it. Periodic `Tick` transitions do not revoke captured permission.

`Publish` and `Tick` are deliberately separate. A transaction can reuse cached
Serving metadata after the Frozen object is published but before the epoch
cut. That transaction is legal and must be drained. Conversely, a thread can
sample the old epoch, pause without announcing, and announce it **after** a
barrier completes. The entry recheck prevents that late announcement from
resurrecting the old cached permission.

`history` indexes identify immutable publications in ghost state; `floor[e]`
records which publication preceded advancement to engine epoch `e`. They are
not runtime clocks or arrays to add to the hot path. `completed` records
completed engine grace periods, not authority validity. Logical reader records
are also ghost state, not a proposed shared per-transaction registry.
`worker_slots_refine_reader_drain` proves that scanning one slot containing
each worker's **oldest** announced epoch is equivalent to scanning all its
logical guards. Reporting the newest nested/retry epoch is unsound.

#### Native entry cost and cache contents

The native implementation has a fixed entry cost, not zero synchronization.
Entry safely announces/rechecks the existing STO epoch before accessing metadata.
The cache-hit comparison is just `entry_epoch == worker.cached_epoch`; it does
not compare routing tables or load a current sharding-version counter. A miss
loads the published immutable object and tags it with the retained entry epoch.
Periodic STO ticks can cause a refresh even when no range has moved.

The cached object contains both routing information and complete owner-local
Serving/Frozen interval metadata. Authorization checks use that captured object,
not current mutable participant state. Requests still bind the complete owner
and ownership version; a foreign snapshot number cannot manufacture authority.
Point permission covers absent rows and all physical aliases at its coordinate;
range permission covers the whole interval, not merely its lower endpoint.

The production admission path has no shared transaction/lease registry, per-key reader
counter, or per-access participant mutex. Thread-local cache ownership is retained
across transactions so a cache hit needs no allocation or shared reference-count
increment. Ordinary STO validation, write locks, routing/handle resolution and
correctly ordered epoch entry still cost work. Commit never tests equality with
the current global epoch. Performance must be measured against a matched disabled
run; the proof makes no throughput or zero-overhead claim.

#### Handoff and the safety argument

The composed machine includes the following final handoff portion:

```text
Freeze publication
    -> establish a cut by advancing the existing source epoch
    -> complete the grace period for announcements older than that cut
    -> exact final copy, including absence/deletion
    -> publish ownership, activate the destination, reclaim the old source
```

Old captured transactions may commit after Freeze and after arbitrarily many
ticks. Migration waits for their actual effects, cleanup, and pin release.
An epoch mismatch alone never aborts them. New transactions cannot acquire
Frozen permission through a stale routing grant or an idle worker cache.
Unrelated old read sections conservatively delay the process-wide cut.

The full native sequence starts **before** this portion: prepare nonserving
destination staging and pre-copy rows while the source continues serving.
Pre-copy is advisory and may contain stale values or destination-only rows.
The model records per-row observations independently of later source writes,
accepted outstanding effects, explicit close/join, and retained stale packets.
Acceptance requires the current generation/round and open job. Final and Abort
cannot overtake unjoined effects. The native worker serializes the complete
synchronous pre-copy job with Final/Abort under the same participant borrow;
its reply joins actual effects, and Stage round 0 rejects delayed old work.

Exact final traversal starts from the beginning after the real epoch drain,
copying changed/inserted values and removing destination-only rows, including
source deletions. Equal identity **and bytes** avoid a physical put; they do not
permit skipping coverage. Without complete change tracking, pre-copy can reduce
transfer/write work but the final scan remains proportional to the selected
range. A volatile STO cut cannot replace the conditional durable library's exact
persistent transaction settlement.

The central implication is derived from the admission machine, not imposed as
a source-stability premise on `Copy`: a completed post-Freeze cut excludes
every older captured publication; the source's remaining publication suffix
denies the scope; therefore no unfinished source transaction can remain.
`EngineCommit` does not recheck current authority or pin liveness to hide an
early-release bug. Pin retention is an inductive invariant.

| Export | Guarantee |
|---|---|
| Both modules' `proofs::theorem_finite_behavior` | Every finite enabled execution preserves the inductive invariant, without bounds on workers, transaction IDs, epochs, retries, or handoffs. |
| Admission `completed_excludes_older_views`, `stale_announcement_cannot_enter`, `post_cut_cache_is_refreshed` | Late entry and stale caches cannot bypass a completed publication cut. |
| Admission `worker_slots_refine_reader_drain`, `retained_guard_bounds_worker_slot` | One minimum-announcement slot per worker accounts for retained/nested guards. |
| Admission `certificate_covers_future_footprint`, `interval_certificate_covers_absence_and_aliases` | Captured certificates cover future accesses throughout the complete immutable scope, not merely its first or present row. |
| Drain `drain_removes_old_effects`, `copy_waits_for_real_drain` | Final copy cannot overtake an old successful effect or unfinished cleanup. |
| Drain `pending_effect_preserves` | Placement safety for a pending engine effect does not require the model's conservative OCC equality check. |
| Drain `every_scope_coordinate_agrees`, `publication_preserves_image`, `exact_copy_covers_absence` | One common execution preserves the full logical/physical image, including missing rows and destination-only deletion. |
| Drain `copied_source_cannot_receive_effect`, `source_stable_after_copy`, `stale_control_rejected` | Copied source data cannot receive a late authorized effect; obsolete controls cannot advance a newer move. |
| Drain `precopy::accepted_packet_is_current`, `join_waits_for_effects`, `final_requires_join`, `old_packet_rejected` | Generation/round/closure fencing and joined physical effects precede exact final replacement. |
| Drain `projection::physical_cut_entails_each_owner_drain`, `completed_physical_cut_enables_local_completion` | One physical clock and minimum-slot inventory conservatively drain every colocated owner's retained guards. |

Constructive witnesses exercise successful post-Freeze insertion, cache reuse
between publication and the cut, independent local clocks, retained retries,
cleanup-delayed draining, unrelated readers, late RPC enrollment, cancellation
with destination-only bytes, and A→B→A. `arbitrary_round_trips` constructs an
unbounded number of round trips. Data witnesses use finite-support images
inside an unbounded ownership/absence scope. Additional witnesses interleave
source updates/deletion with stale pre-copy observations, correct the final
image, and reject delayed old packets. Bad-order examples demonstrate lost
inserts, stale overwrites and reclamation underneath unfinished cleanup.

#### What this does not prove about the native implementation

- The transition system supplies sequentially ordered, coherent
  publication/announcement/recheck operations. It does not prove a C++/Rust
  weak-memory implementation, a fence-free reader, or zero instruction cost.
  Native STO fields now use sequentially consistent atomics and a
  sample/announce/recheck kernel; this is an explicit foreign boundary, not a
  weak-memory theorem supplied by the model.
- Real entry must precede metadata/handle dereference. Retained logical
  contexts must keep their oldest pin across engine retries and RPC suspension.
  Idle helper/reset transactions must become quiescent, and a migration waiter
  must not hold the old read section it is waiting to drain.
- All affected readers must be represented. Colocated owners must share the
  actual process-wide STO clock and pin inventory; separate processes exchange
  completion acknowledgements, not numerically comparable epochs. Silo/Masstree
  physical-memory reclamation remains a distinct existing mechanism.
- Ordinary OCC and distributed atomic settlement remain an explicit engine
  interface. The data machine uses conservative full-scope snapshot validation
  and image replacement; its placement-effect lemma does not depend on that
  validation guard. This is not native distributed strict serializability,
  operation-set sealing, or a proof of incremental C++ installation.
  Unknown outcomes are not terminal settlement.
- Exact copy is an interface-level image operation here. Checked Rust mirror
  traversal retains complete row-alias/absence and byte correspondence. Native
  `capture_frozen` checks full Frozen metadata; its real completed STO cut is an
  explicit caller/host premise. Private `precopy_put` checks full Stage round-0
  authority under the effect's immutable borrow. Actual queue serialization,
  cut wiring and C++ engine effects still require native evidence.
- Epochs and generations are mathematical naturals and IDs are not reused.
  Machine wrap/reuse must be excluded or separately proved. This model adds
  no crash recovery, fairness, bounded migration latency, or throughput result.
  An old transaction can delay the grace period indefinitely.

### Proposed commit-time validation (not deployed)

`sharding_commit_validation*.rs` models the earlier concurrent-copy alternative
to per-access leases, not the native engine-epoch algorithm above. It is
independent of the lease/Drain proof: no stable-source, drained-range, or
held-grant lemma from that proof is imported. It remains a separate alternative;
the native cutover does not adopt its commit-time authority validation.

The proposed ordering is:

1. Capture an immutable route and a valid, coherent participant authority
   snapshot; retain the corresponding engine/storage lifetime.
2. Record the complete point/read/write/scan footprint. Freeze the operation
   set and acquire **all** local and remote write intents before authority
   validation. No later packet may extend that prepared transaction.
3. Check each participant's current authority epoch. An unchanged epoch uses
   the captured authority; otherwise check exact Serving owner/version over
   the whole recorded scope. Restart incarnation must also match. An unchanged
   directory publication number is insufficient: Freeze precedes publication.
4. Run ordinary engine validation and settle the transaction against its
   captured physical targets, never by rerouting its already-executed writes.
5. Migration changes the source authority epoch at Freeze, then performs an
   exact, intent-aware final copy. Retire, publish the whole immutable routing
   scope, and activate the destination only after complete coverage. Reclaim
   source storage only after outstanding references and cleanup finish.

The copy argument is local, not a whole-range drain: an authorized writer
already owns its write intent; an unsettled intent prevents certifying that
row or gap. A writer that has not validated when Freeze takes effect cannot
become authorized for that frozen grant. Consequently, once a row/absence is
copied, no later authorized source effect can invalidate it. Exact coverage
then permits publication without assuming the whole source was stable before
copy began. A read-only transaction can outlive handoff; it retains storage,
not serving authority for new accesses.

The unchanged-epoch shortcut requires a baseline that authorizes the **entire
eventual footprint**, not merely a well-formed snapshot or an accepted first
operation. A certificate covering the captured routing view can establish this
without per-operation mutable admission. Otherwise each newly introduced scope
must be checked against the immutable baseline, or use exact validation instead
of this shortcut. Capturing an epoch after a range is already Frozen and later
observing the same epoch proves no authority at all.

The projected machine includes both the captured and current local epochs:
Freeze changes the latter in the same transition that removes Serving.
`EpochBump` represents an unrelated authority change and exercises the exact
fallback. The authority module additionally models fresh incarnations and
coherent snapshot histories. Epochs/generations are mathematical naturals;
an implementation must prevent wrap/reuse.

| Export within `sharding_commit_validation` | Guarantee |
|---|---|
| `authority::certificate_covers_future_footprint` | A valid captured-view certificate covers subsequently discovered scopes drawn from that same immutable view; accepting the first key alone is insufficient. |
| `authority::validation_establishes_authority`, `proofs::epoch_validation_refines_gate` | Same-epoch fast check and changed-epoch exact fallback establish the authority needed by the operational validator. |
| `authority::scan_checks_every_coordinate`, `authority::every_row_alias_is_covered` | Whole intervals include absent keys, internal gaps, unbounded tails, and every physical row under a warehouse ownership coordinate. |
| `proofs::theorem_finite_behavior` | Every finite enabled execution preserves canonical physical/logical agreement, unique Serving authority, captured write targets, and storage pins. Transaction IDs and repeated handoffs are unbounded. |
| `proofs::pending_writer_blocks_copy`, `proofs::copied_prefix_stable` | Unsettled authorized writes—including inserts into absence—cannot be skipped; copied source/destination cells remain stable until activation or cancellation. |
| `proofs::whole_range_agreement`, `proofs::whole_scope_publication` | One common indexed execution covers the complete scope. Publication preserves its logical cells and leaves outside rows unchanged. |
| `proofs::lifetime_prevents_reclaim`, `proofs::sealed_rejects_late_access`, `proofs::stale_control_rejected` | Unfinished references block reclamation; sealed transactions reject additional accesses; obsolete generations cannot advance or erase a move. |

Enabled witnesses exercise an absent-key insert crossing Freeze, cleanup after
unlock, a read-only transaction completing after handoff and a newer write,
unrelated-epoch fallback, cancellation with destination-only data, and A→B→A
without ownership-version ABA. Checked counterexamples show why skipping an
absent-key intent, checking only the unchanged directory, or trusting an
initially Frozen same-epoch baseline is unsound.

`Cell.writer` is semantic provenance, not an assertion that a native STO
timestamp has that encoding. The footprint and immutable write effect are
projected inputs; the model does not implement their collection. In particular,
the common-execution theorem is **physical/logical agreement, not a
strict-serializability theorem**. It does not choose separate serialization
orders for individual keys and call their conjunction a transaction history.

The remaining engine/refinement obligations are substantive:

- One globally complete operation set and distributed prepare/decision, ordinary
  OCC (including read-only/gap validation), and a common transaction history.
  The existing fixed-placement/atomic-prepare proofs are not automatically a
  refinement of this split, moving-placement algorithm.
  In the current engine, `Transaction::shard_validate` checks reads but does not
  seal the prepared operation set; `shard_try_lock_last_writeset` locks only the
  last write item. Their caller must establish the complete aggregate condition
  and reject late pieces; invoking either primitive once is not that proof.
- Fresh, coherent metadata observations with the required memory ordering;
  authority capture and lifetime retention must not race reclamation.
- Raw traversal must certify *every possible unsettled intent* in a copied gap,
  including unpublished/invalid insert carriers. It must not treat an I/O abort
  or a returned candidate row as successful coverage. Destination-only rows
  must be removed, and stale asynchronous copy/cleanup work must be fenced.
- Logical settlement must precede unlocking; cleanup may follow unlocking.
  Keep engine storage alive through that cleanup. A retained metadata `Arc`
  alone does not establish row/index lifetime or old-source retention.

The real-engine smoke exercises this interface, not its universal refinement:
the production `native_sharding_storage.cc` `scan_one` callback and actual
MassTrans/STO lock/validate/install/unlock/cleanup run under deterministic
two-thread schedules, with sharding disabled and no lease/Drain path. Both
non-MV and MV tables are exercised in the nonopaque Release configuration.
Pending locked inserts, updates and deletes reject copy; read-only absent-point
and gap conflicts reject commit. Removing the writer lock demonstrates the
failure directly: copy returns the old value, then the writer installs the new
value. A non-MV committed delete can expose final absence before its removed
value unlocks. Thus the sufficient contract is **no unsettled logical effect**,
not “every physical lock blocks copy” or “locks last through cleanup.”

This model adds no crash recovery, fairness, latency, or performance result.
Native memory ordering, full distributed OCC composition, scan-frontier
refinement, and source reclamation remain outside the mechanized result.


### Trusted recovery and progress interfaces

`sharding_recovery` keeps separate coordinator, participant and transaction
authority journals. Acknowledged RPC acceptance or an unknown append result
does not commit a sharding effect. Replay installs a local committed checkpoint
and overlays that authority's durable engine suffix; the ghost global placement
state is not a recovery input. Its concrete requirements are:

- Raft retains each authority's committed prefix without rewriting it.
- Engine success durably settles the exact atomic write set and outcome;
  a settled abort fences old work. Unknown outcomes and restart do not release
  leases or invent settlement.
- Durable transaction-authority names can be enumerated after restart.
  Namespace publication is atomic with the corresponding authority record.
  Admission incarnations and owner-membership floors survive restart.
- Copy reservations, immutable copy versions, deletion fences and completion
  records are durable engine data. Work starts only after its reservation is
  committed. A deletion receipt denotes completed generation-scoped work,
  not an accepted request or metadata-only `Empty` state.

Progress starts from a verified finite prefix, including a recovered prefix.
The infinite continuation allows arbitrary legal transaction outcomes, not a
scripted sequence of successful migrations. It requires eventual transaction
settlement and delivery of known completion, fair primitive scheduling and
message delivery, a finite selected image, and an eventually error-free raw-I/O
suffix. Restartable mirror/cleanup traversal terminates by a finite rank;
infinitely many isolated successful I/O calls are not enough. One shared
physical-world history binds each job's generation, owner, issue time, complete
inventory (including private stale rows), exact bytes, scan gaps and emissions.
There is no assumption that cleanup or an administrative phase eventually
completes, and there is no wall-clock deadline bound.

Retention's checkpoint is **administrative only**. It preserves request
incarnations, acknowledged sequence floors, next generation and current routes.
External `protocol::Input::Finish` derives its outcome from its own evolving
placement state and requires both terminal receipts. Collection neither erases
participant journals nor proves that all background work is quiescent. A pending
old copy can outlive administrative Finish; its recovery state must still be
retained. The bound is on materialized administrative records/namespaces/routes
for a fixed configured client/key universe, not total bytes, Raft logs, native
receipt tables or pending I/O dictionaries. Exhaustion/backpressure fails closed;
there is no timeout-based identifier reuse.

### Representation and implementation obligations

- The corrected model has an arbitrary finite pool of live shards and arbitrary
  finite logical-key universe, including valid absent keys—not just existing
  rows. No fixed key/transaction/step bound is used by the safety induction.
  A partition representation relation connects its directory projection to
  ordered metadata. Native routing/scan codecs and the canonical row-value
  embedding are proved; native physical-table/warehouse binding remains an
  explicit foreign interface.
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
- IDs/generations never wrap or get reused. Fixed-width actors refuse allocation
  on exhaustion. The administrative retention protocol keeps durable high-water
  marks rather than forgetting identifiers after a delay.
- Protocol recovery and configured-owner lifecycle proofs do not implement a
  production recovery adapter. Arbitrary pool expansion, hash-ring rebalance,
  Byzantine behavior, follower reads and bounded completion time are outside
  these results. Metadata-only membership edits are not the proved lifecycle.

### Native Rust correspondence

`src/cluster/lib.rs` is both the Cargo staticlib entry point linked into Mako
and the whole-crate Verus entry point. Its executable actors are verified from
the same source; proof-only imports reference this directory's independent
`sharding_partition`, `sharding_placement`, `sharding_mirror` and
`sharding_recovery` modules.
There is no alternate executable model in the production data path.

| Native source under `src/cluster/` | Correspondence |
|---|---|
| `directory*`, `routing*` | Ordered interval updates, immutable complete snapshots, paired snapshot/ownership capture, homogeneous scan segments, and snapshot decoding/installation. |
| `participant*`, `epoch_admission` | Generation/round-fenced metadata; exact immutable all-table/all-coordinate capture and Serving owner/version point/whole-range authorization, including absence and aliases. |
| `leases*` | Conditional durable point/range admissions, held scopes and exact registry drain; not the native epoch admission path. |
| `migration*` | Retained caller nonces, fresh generations, issued controls, receipts, publication and terminal outcomes. |
| `coordinator_publication*` | Checked actual snapshot serialization and immutable equal-version publication derived from native effect histories. |
| `storage*`, `transfer*` | Ordered exact mirror, equal-value write elision, explicit EOF, destination-only deletion, guarded private pre-copy, Frozen-source capture, cleanup completion and real failure prefixes. |
| `ghost_log*`, `execution_refinement*`, `transfer_*proofs` | Raw field/entry effects and closed certificates relating concrete views to independent placement paths. |
| `full_scan_core`, `full_scan_proofs` | Transaction/snapshot/grant/table/coordinate-bound codecs, version-bound pages, raw callback/page assembly, ordered pagination, direct callback prefixes and mixed local/paged range coverage with byte-value correspondence. |
| `source_*history`, `source_execution`, `source_causality`, `lease_composition_proofs` | Chained participant/engine images, multi-owner lease union, actual settlement before release, and capture/emission provenance. |
| `transfer_progress` | Exact native scan/put/delete interpretation and checked final-copy/cleanup callbacks under primitive availability. |
| `durable_kv`, `durable_*codec` | Versioned byte keys/records, atomic head+operation append, unknown-outcome settlement, namespace separation and committed-prefix reconstruction. |
| `durable_coordinator`, `durable_participant`, `durable_engine` | Native-operation replay, durable admissions and transaction origins, phase resumption, and data-before-certificate / settlement-before-release ordering. |
| `durable_*refinement`, `durable_participant_*` | Native effect and semantic replay bridges to recovery; erased provenance is not a recovery input. |

The source-history/lease results apply to the conditional lease embedding, not
the new native epoch wiring. Their entry points calculate the placement image
from each native record and construct closed effect certificates. Participant
snapshots and owner-local registries are chained; global lease drain follows
from their union. Finishing one owner cannot release another owner's holds.
Actual engine settlement precedes a known completion callback and lease release;
an unknown callback cannot manufacture completion.

Private completion/certificate tokens distinguish metadata changes from
completed storage and emitted responses. Captures require actual ordered
first-row/gap/EOF evidence; unmodelled private copy/deletion effects still update
the physical image history. A cleanup error cannot erase its already-applied
prefix. Successful Final delivery and executable receipt-preservation contracts
establish the receipt required by the later real seal callback. Loader commits
establish the initial journal with writer identities disjoint from ordinary
transactions.

The row-value embedding is fixed and injective (`code([]) = 1`,
`code(prefix ++ [b]) = 256 * code(prefix) + b`), not a caller-supplied encoding.
It is ghost-only and adds no runtime value conversion.

Native epoch admission separates **request provenance** from **local authority**.
Checked routing point/interval capture supplies the former. `AdmissionSnapshot`
copies the complete participant metadata domain and checks its captured Serving
owner/version for every requested point or full interval; an incoming routing
label never grants permission by itself. The TLS wrapper retains ownership of
this immutable object across transactions and refreshes only when the retained
entry epoch or owner changes. That wrapper and its epoch lifetime are
foreign/unchecked integration, not inferred from a matching integer.

`coordinator_publication_proofs` separately derives immutable equal-version
observations from native initialization and operation histories. The old
production snapshot-certification/hint APIs and their composition module have
been removed, not retained as a compatibility path. The independent
`sharding_fast_path` model remains a historical lease-based comparison; its
certification/lease witnesses are not claims about native epoch admission.

Metadata-only recovery remains inspection-only. Neither a cached read object
nor a volatile STO announcement is journaled or restored as recovered authority.

The theorem remains conditional on named embedding contracts: canonical finite
logical-key/physical-cell labeling (including absence), transport request/result
association, actual atomic engine effects and their frames, transactional
ordered scan semantics, transaction lifecycle (no fresh participant enrollment
after settlement), and exclusion of competing transfer effects on the selected
range. Capture and response provenance are derived from the source
history rather than supplied as a pre-certified placement history. A metadata
mutex does not exclude already-admitted writes to unrelated ranges.

`host.rs`, `wire.rs`, `runtime.rs`, `ffi.rs`, and `gateway_ffi.rs` are native
boundary code, excluded from Verus by `cfg(not(verus_keep_ghost))`. The C++
engine, RPC/lifetime adapters, warehouse binding and allocator/RCU callbacks
are also in the trusted embedding boundary. Executable verification does not
certify those handlers merely because the source audit finds their callers.
Real-engine smoke scenarios exercise this boundary but are not a proof of it.
`full_scan.rs` supplies native byte/FFI/callback adapters around the verified
`full_scan_core.rs`; those adapters are likewise not a verified handler theorem.

The direct-scan contracts describe physical engine enumeration and synchronous
callback delivery, not the desired logical result as an assumption. Complete
logical results are derived through the existing read-view/lease embedding and
placement invariant. Exhaustion must deliver every enumerated row; user stop
permits only an ordered prefix including the stopping row, and errors do not
certify completion. `mixed_sharded_scan` composes direct and paged segments
without changing the captured routing table or transaction read view.
Canonical C++ handle resolution, full-scope admission before callbacks, callback
forwarding/lifetimes, metadata stripping and actual STO/OCC semantics remain
foreign obligations. The live-engine smoke covers local oversized rows,
forward/reverse bounds, callbacks and OCC failures; the isolated
`test_mako_native_mixed_scan` exercises real migrated remote segments through
SRPC rather than treating “stop before remote” as complete mixed-path coverage.

`execution_refinement` imports **placement**, not `sharding_transactions`: it is
not a native distributed transaction-history refinement. The durable library
adds concrete native journal/replay code against primitive byte-KV and engine
contracts; it does not silently add those contracts to `HostAbi` or `runtime.rs`.
Production can persist coordinator metadata and live participant bookkeeping,
but it does not reconstruct serving-capable participant/engine or TP state.
Raw migration writes use the non-Paxos path, and replicated migration is rejected.
MBTA's primary rows are in memory; existing RocksDB log components do not make
them an authoritative durable row store or settle pre-crash distributed work.

### Conditional native durable-KV path

The native wrappers persist inputs to the existing actors, not snapshots of a
second executable state machine. A versioned owner/authority namespace has a
head and numbered operation records. Bootstrap is entry zero; every append
atomically publishes one operation and its new count. Recovery first settles
unknown accepted batches, reads and validates the committed prefix, and runs
the same coordinator/participant transition bodies. It does not accept the
old actor, a ghost journal, cached routing, scheduler state or a pre-certified
recovered state as input.

An exact nonce retry retains its generation and intent; a changed intent is
rejected. Failed/unknown writes make the candidate inaccessible until recovery.
Re-admission persists a new incarnation, and transaction-origin records prevent
recorded old identities from becoming new work merely by using the new receiver
token. Global transaction identities still name the same TP transaction across
all participants and restarts; receiver incarnation is not a transaction-ID
allocator. Clients and RPC adapters must preserve captured identities, not
recycle transaction IDs or relabel old requests. One live serialized writer
owns each namespace; the byte API is not a distributed compare-and-swap or
an election protocol.

Copy and cleanup use the real native ordered transfer loops. A persisted
reservation precedes I/O; a durable complete copy precedes the Ready record;
durable deletion precedes the Done record. Native metadata may become private
`Empty` before deletion finishes. That intermediate state is not a deletion
certificate: the recovery refinement defers abstract terminal publication
until actual completion. Partial destination rows likewise remain private;
unknown work must settle or be excluded before a new admission.

The engine contract names exact durable/working bytes, versions, pending row
effects and transaction write sets/outcomes. Finishing a recovered lease
requires a durable global fence against later old work, including surviving
peers. Even an absent transaction requires a persisted empty-abort tombstone,
not an absence or timeout heuristic. Canonical physical row binding, exact
atomic engine effects, authenticated RPC provenance and committed-prefix
retention remain named primitive interfaces; none assumes migration correctness.

Global transaction lifecycle authorization is also a TP boundary: Begin and
Acquire run under a live enrollment authorization held or fenced through the
metadata append. The owner-local journal cannot tell whether a previously
unseen ID was already settled elsewhere, and these APIs do not consult global
TP state themselves. A production dispatcher must enforce that condition,
not relabel an old request or use a stale phase boolean. The local Resolve
record is a native lease-state marker, not a durable engine outcome.

The reference backend in `durable_test_store.rs` / `durable_test_engine.rs`
uses fsynced files, atomic replacement and real reopen, not a memory-backed
durability substitute. Its transaction fixture is intentionally single-owner,
with an enforced owner-specific identity domain; it is not a distributed TP
implementation. `examples/durable_recovery.rs` exercises two SIGKILL boundaries:
an accepted unknown deletion, followed by cleanup and ownership return; and a
lost lease reply before engine work, followed by journal-only identity discovery,
a durable empty-abort fence, rejection of late old work and resumed migration.
`next_recovery_transaction` enumerates this work from committed Begin records
and native lease state; recovery needs no pre-crash client-ID list.

The conditional durable native path has fixed owners and append-only history.
It does not reclaim logs or transaction tombstones, or implement dynamic
membership. Production now supplies a separate RocksDB metadata-only integration:
`rocks_metadata.rs` implements the trusted byte-KV boundary, the coordinator
reuses checked replay, and `metadata.rs` retains live participant bookkeeping
under a distinct payload tag. Reopened production owners permit historical admin
inspection but reject data admission and migration continuation. This is not a
`DurableEngine` implementation or a deployment of full participant/TP recovery.
The foreign RocksDB kernel and runtime gates are tested, not Verus-verified.
The independent lifecycle/retention/progress theorems keep their stated scopes.
See [the concrete record format and backend obligations](../../docs/storage-interface.md#recovery-and-progress-scope).

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
| `sharding_commit_validation*.rs` | Proposed lease-free commit-time authority, write-intent/copy ordering, full scopes, lifetime pins, and independent witnesses; not the deployed native algorithm. |
| `sharding_epoch_admission*.rs` | Existing-engine-epoch enrollment, immutable cache provenance, minimum worker-slot aggregation, grace-period induction and race witnesses. |
| `sharding_epoch_drain*.rs` | Epoch admission, effects/cleanup, online pre-copy observation/fencing/join, exact full-scope handoff, shared-domain projection and repeated-migration witnesses. |
| `sharding_*witness*.rs` | Enabled forward, abort/retry and partial-return executions. |
| `sharding_bytes.rs` | Shared injective byte-value representation. |
| `sharding_recovery*.rs` | Per-authority recovery, durable engine suffixes, admission/membership fencing and constructive lifecycle witnesses. |
| `sharding_progress*.rs` | Actual-execution fairness, restartable raw traversal, one physical-world interpretation and conditional completion. |
| `sharding_retention*.rs` | Source-connected administrative retention, durable reconstruction, stale-message fences and bounded materialized state. |
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

The `multiShardSingleProcess` CI suite waits for both owners' throughput reports
before stopping their shared process. Its result check requires exactly two
finite, positive rates.

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

The proof gate attests the pinned Verus archive, verifies both complete crates
without positive function/module filters, runs their semantic negative controls,
then runs locked Cargo tests and the file-backed SIGKILL example. The source
coverage audit is an inventory, not a proof. `verify_controls.py --native`
selects the same-source production crate;
the default selects the independent crate. Each mode verifies its entire
unchanged baseline before mutating temporary copies.

The independent runner injects fifty-seven bugs. The original eighteen cover
seven core MakoV2 mutations
(ignoring local holes; bypassing finality coverage; skipping read validation;
accepting a non-increasing timestamp; treating maximum log transaction time as
a frontier; relabeling stale reports; inflating an epoch's close cut), six
placement/transaction mutations (retaining destination-only keys; accepting stale
physical observations; admitting leases after freeze; accepting the wrong copy
round; sealing without coverage; accepting an obsolete abort), stale
recovery admission, collection before checkpoint coverage, retained certificates
across authority changes, unchecked certification, and mixed-version capture.
Eighteen commit-time-validation controls additionally remove baseline
authorization, epoch/incarnation invalidation (including the operational Freeze
step), complete scan/row-alias scopes, atomic whole-scope publication, fresh
generations, Frozen rejection, complete locking, settlement-before-unlock,
intent-aware exact copy, coverage-before-retirement, lifetime-before-reclaim,
stale-control fencing, or the sealed-operation boundary.
Fifteen engine-epoch controls additionally remove the entry recheck, epoch-keyed
cache refresh, retained retry pin, announcement drain, oldest-worker-slot
aggregation, complete alias/scope certificates, post-publication cut, completed
barrier before copy, settlement/cleanup protection, exact destination
replacement, stale-generation fencing, receiver-side enrollment, or copy before
publication. Pending-effect release and post-effect/pre-cleanup release are
checked separately.
Six further controls remove private-copy generation/round/closure fencing,
allow Join with outstanding effects, let exact final copy overtake an unjoined
job, or map a colocated logical guard to a younger physical announcement.
The twenty-one native controls reject lost byte-content injectivity, unknown
completion, lost multi-owner holds, forged copy provenance, aliased durable
namespaces, arbitrary unknown-batch settlement, reused publication versions,
and omitted direct-scan callback rows. Five target immutable point/range
authority (receiver, Serving role, ownership version and complete intervals);
eight target exact mirror identity/byte equality, destination-only deletion,
private-copy envelope/range/round/generation fencing and Frozen-source capture.
The retained lease/effect controls concern the conditional durable contracts,
not a per-access lease path in native epoch admission.
Mutants target named safety lemmas, so unrelated constructive-trace failures
cannot pass a control. Type errors, verifier crashes and resource exhaustion
also do not count as successful controls. Originals remain unchanged.

No `assume`, `admit`, external proof bodies, solver resource-limit overrides,
or model-checking results are used to establish the result.

### Checked results

#### Native engine-epoch cutover (2026-10-21)

Fresh Docker runs on `zoo-002` used Rust `1.97.1` and attested Verus
`0.2026.08.02.b677dd5`:

```text
native src/cluster/lib.rs:  1449 verified, 0 errors
independent model:           707 verified, 0 errors
native negative controls:     21 passed
model negative controls:      57 passed
locked native Cargo tests:    64 passed
file-backed SIGKILL cases:     2 passed
native live layouts:          2 passed
metadata SIGKILL/reopen:       4 passed
affected CTest suites:         6 passed
one-shard Paxos replay:        1 passed
```

The source inventory contained 99 reachable files. Both complete positive
roots ran without module/function filters or resource overrides; all mutants
failed their named semantic obligations. The native and model scopes were run
as separate Docker invocations of `verify_controls.py`, alongside separate
invocations of the locked Cargo and durable-example commands from `verify_native_sharding.sh`.
Long model witness/controller proofs were split into checked phases to avoid
resource exhaustion, without weakening contracts or skipping enabled steps.
The native generation-fence control targets its defining authority lemma:
checking only a caller would trust the mutated callee's postcondition.

The fresh Release build used Clang 22/libc++, `OPACITY=OFF`, and C++ borrow
checking disabled. The storage DSL regeneration check passed. Native smoke
exercised real MBTA/STO and SRPC in separate-process and colocated-owner layouts:
timer-spanning commits; retained old guards through OCC conflict/retry and
cleanup; idle helpers; first access to another coordinate through an old view
after Frozen; actual private pre-copy followed by source update/insert/delete;
full final deletion of stale/private-only rows; late-job fencing after
Final/Abort/new generations; actual source-Commit reply loss/retry; and stale
authentic control replay. Four production SIGKILL/reopen cases retained
committed/pending metadata while rejecting data admission and continuation.

CTest passed `test_client_service`, `test_silo_nontxn_api`,
`test_mako_nontxn_distributed`, `STOTransactionRealTests`,
`test_mako_native_mixed_scan`, and `STOEpochRCUTests`. The last covers nested
old guards, real engine cleanup, lazy helper activation and retirement behind
a newer reader, plus allocation/reclamation stress. The final one-shard Paxos
scenario observed 3,287 follower replay batches with migration disabled.
It is not a replication-plus-migration test.

Full TPC-C exercised foreign lifecycle paths beyond the migration smoke:

- Mode 1 reported an already-aborted engine attempt as active, so a later
  Abort RPC tried to re-enter its epoch. Liveness now follows the engine state;
  terminal rebinding retains the logical pin without reviving the engine.
- Multi-row handlers continued after an engine abort. Failed batches now roll
  back their staged rows and stop; both multi-row read variants consume the
  abort indicator before another storage access.
- With native admission disabled, static clients treat a failed-read reply as
  terminal and skip another Abort RPC. The receiver now restores an empty,
  unpinned helper only after that request's physical cleanup, so the next
  request starts a fresh engine attempt.

Each correction has a real-engine regression that failed before the fix.
The final ordinary binary passed full two-shard TPC-C with native admission
enabled and disabled: both processes exited zero in each run, with strict
startup, workload geometry and report validation. These are verification runs,
not samples substituted into the matched performance comparison.

Evidence: `zoo-002:/home/users/shuai/mako-native-epoch.WfX0NaeP/`.
`proof-native-7` and `proof-model-5` each have command/source-hash/result JSON
and complete logs. Their log SHA-256 values are respectively
`648f14f40e2bec5b2b9a8fecb02c11586abd12351ac00816eae3239c6ae609e5`
and `024b51141f7dbf352f7c3a344a71b5cdffe36773531b815f602a629d6582db4e`.
`build-static-reset`, `smoke-static-reset`, `regressions-static-reset`, and
`replication-static-reset` record the final C++ build/runtime checks.
`workload-native-static-reset` and `workload-disabled-static-reset` retain the
full-workload checks and their frozen binary hashes.
`helper-regression-before`, `batch-regression-before`, and
`static-regression-before` preserve the failing-before tests.
`helper-timestamp-diagnostic.json` records an intermediate stale-object rebuild.
`smoke-batch-fix` retains a listener/ephemeral-port collision; subsequent
launches use the existing `docker_build.sh` reserved-port policy.
`workload-native-reparse` corrects a diagnostic-only parser flag on already
successful ordinary workload logs without rerunning either process.
`cargo-1`, `durable-1`, and `storage-dsl-1` retain the Rust, recovery and
regeneration checks. The C++ lifecycle corrections changed neither Verus root.
All command/result JSON, complete logs, fixture/CTest/replica logs and earlier
failed attempts remain in the same evidence directory, not silently replaced.

These results do not mechanize C++ atomics, allocator/RCU, FFI, RPC queues or
distributed OCC. The durable reference is not production MBTA recovery.
The [matched timing record](../../docs/architecture/sharding-migration.md#engine-epoch-comparison-2026-10-21)
is separate from correctness/proof evidence.

#### Prepared workload routing (2026-10-22)

The first prepared-routing optimization precomputed fixed-coordinate descriptors
and bound their private capture slots once per logical transaction. Immutable point descriptors carry a
context/envelope incarnation, preventing stale local slots, same-transaction
envelope replacement, A→B→A restoration, or a reopened receiver context from
reusing earlier resolutions. This is foreign C++ bridge bookkeeping, not a new
authority epoch. Native admission, STO pin lifetimes, exact wire validation and
handle-directory/binding-cache policies were unchanged in that comparison.

Fresh Docker evidence in the directory above:

- `prepared-build-3`, `prepared-build-smoke-4`, and
  `prepared-build-receiver-6`: final production and fixture builds.
- `prepared-smoke-2`: both real live layouts and all four metadata
  SIGKILL/reopen cases passed. Prepared descriptors survive capture-vector
  growth, interleaved production proxy selection, ownership return, and a real
  OCC conflict/retry through an old pinned view after Freeze. Construction and
  idle resolution do not announce an epoch.
- `prepared-regressions-1`: client service, nontransactional API/distribution,
  and real STO transaction suites passed. `STOEpochRCUTests` passed in
  `prepared-smoke-2`.
- `prepared-receiver-3`: isolated `NativeMixedScan.LocalAndRemoteSegments`
  passed, including stale-token rejection, abort rollback, and a fresh token's
  committed engine effect after reopening identical incoming request bytes.
- `prepared-replication-1`: one-shard Paxos passed with 3,206 follower replay
  batches; migration remained disabled.
- `prepared-dsl-and-drivers-1` and `prepared-integrity-4`: storage regeneration
  passed and all 79 pre-timing source hashes matched.

The initial fixture failures are retained, not replaced: `prepared-smoke-1`
needed a real warehouse proxy opener; the receiver assertions in
`prepared-smoke-2` and `prepared-receiver-2` incorrectly assumed
read-your-writes and omitted the transaction tuple's metadata suffix,
respectively. The corrected test verifies committed payload bytes with the
existing `mako::Encode` boundary instead.

This optimization changed no Rust production or Verus source. The previous
formal results remain results for those sources; the proof roots were **not
rerun**, and the new C++ cache is **not mechanized**. Its foreign lifetime and
provenance obligations are exercised by the real-engine regressions above.
The pre-timing tree is retained in `source-manifest-prepared-v4.json`,
`mako-source-prepared-v4.patch`, and `mako-source-prepared-v4-files.tar`.

#### Cross-transaction prepared routing (2026-10-22)

Fixed-coordinate routes, immutable bindings and selected handles now survive
logical completion while the protected native view's cache-incarnation ID is
unchanged. The ID follows the retained Node/full AdmissionSnapshot Arc pair,
not the engine epoch or routing label. Owner changes, full-view replacement,
worker changes and A→B→A cannot reuse an earlier cache incarnation; checked
allocation fails closed on exhaustion. Same-view epoch refresh allocates no ID.
Raw-key captures remain transaction-scoped. New transactions still acquire
their guards, construct fresh immutable points and perform native point
authorization; old points and unstaged batch captures remain invalid.

Docker evidence under `/home/users/shuai/mako-native-epoch.WfX0NaeP` on zoo-002:

- `prepared-cross-build-1` and `prepared-cross-build-smoke-2`: production and
  final fixture builds passed, including mandatory production source gates.
- `prepared-cross-cargo-1`: all **68** native Rust tests passed. New cases
  cover stable cross-epoch identity, Frozen with unchanged routing labels,
  owner/Node/worker changes, ABA, stopped/closed reads and ID exhaustion.
- `prepared-cross-smoke-2`: both real live layouts and all four metadata
  SIGKILL/reopen cases passed. Persistent copied/interleaved descriptors
  retain real commit/abort behavior, do not retain an idle epoch pin, reject
  stale points and old batch captures, and invalidate through migration and
  ownership return. Warmed descriptors reject fresh Frozen operations while
  the precopy worker's old pinned view survives its actual OCC retry.
- `prepared-cross-regressions-1`: all six CTest targets passed—client service,
  nontransactional API/distribution, isolated NativeMixedScan, real STO
  transactions and epoch/RCU. Receiver exact-envelope, ABA and identical
  close/reopen cases retain their real committed/rolled-back effects.
- `prepared-cross-replication-1`: one-shard Paxos passed with **3,240**
  follower replay batches; migration was disabled.
- `prepared-cross-integrity-1` checked storage regeneration.
  `prepared-cross-integrity-2` matched all **79** final source hashes and
  checked the measurement drivers' syntax.

The failed `prepared-cross-smoke-1` is retained. Its new stable-view assertion
initially took a baseline from a cache predating a replay command's conservative
publication. The fixture now advances the engine epoch before taking that
baseline; the cross-epoch equality assertion remains. No production code
changed between the two smoke runs.

The new Rust cache identity lives in ordinary `ffi.rs`, excluded from the Verus
configuration; the foreign C++ descriptor cache is also **not mechanized**.
Native admission policy and the Verus proof roots are unchanged and were
**not rerun** for this optimization. The runtime tests and actual engine
scenarios above exercise the added cache-incarnation/lifetime obligations;
they are not a new formal proof.

`source-manifest-cross-v2.json`, `mako-source-cross-v2.patch` and
`mako-source-cross-v2-files.tar` preserve the pre-timing snapshot.
[Matched performance results](../../docs/architecture/sharding-migration.md#cross-transaction-prepared-routing-comparison-2026-10-22)
are separate from this correctness evidence.

#### Protected-view ownership memoization (2026-10-23)

The design was documented before implementation in
[sharding migration](../../docs/architecture/sharding-migration.md#view-tagged-authorization-reuse-design)
and the [storage contract](../../docs/storage-interface.md#view-tagged-ownership-authorization).
The C++ bridge now retains each fixed entry's successful ownership predicate
when rebuilding a new transaction's point. The existing transaction-entry
view-ID comparison invalidates the whole fixed cache on a different protected
Node/admission view. This adds no new counter or per-operation generation
comparison. Raw captures, received envelopes, transaction/point provenance,
batch membership and runtime readiness checks keep their previous contracts.

Fresh Docker evidence under `/home/users/shuai/mako-native-epoch.WfX0NaeP`
on zoo-002:

- `prepared-auth-build-1`: all requested production/fixture targets and their
  mandatory production source gates passed.
- `prepared-auth-smoke-1`: both live process layouts and all four metadata
  SIGKILL/reopen scenarios passed. Existing real commit/abort, epoch refresh,
  ownership return, stale points/batches, receiver checks and pinned old-view
  OCC retries remain covered. The strengthened Frozen scenario retries in
  distinct transactions: failure must never become memoized success. Normal
  teardown now warms a fixed authorization, reuses it in a new logical context,
  stops the real services, and directly rejects that context's previously
  authorized point; terminal cleanup releases its pin.
- `prepared-auth-regressions-1`: all six CTest targets passed—client service,
  nontransactional API/distribution, isolated NativeMixedScan, real STO
  transactions and epoch/RCU.
- `prepared-auth-replication-1`: one-shard Paxos passed with **3,340**
  follower replay batches; migration was disabled.
- `prepared-auth-integrity-1`: all **79** source hashes and all three
  measurement drivers' syntax checks passed.

The pure native `AdmissionSnapshot::point` predicate and all Rust/Verus sources
are unchanged. Native cluster Cargo tests and the formal proof roots were
**not rerun** for this C++-only production change. The bridge's cached-decision
lifetime is **not mechanized**; the actual engine scenarios above exercise
its added obligations without claiming a new formal proof.

`source-manifest-auth-v1.json`, `mako-source-auth-v1.patch` and
`mako-source-auth-v1-files.tar` preserve the pre-timing tree, including the
design contract. Final verification/performance documentation is archived
separately in `mako-auth-final-docs.tar`.
[Matched performance results](../../docs/architecture/sharding-migration.md#protected-view-ownership-memoization-comparison-2026-10-23)
include the complete replacement profile window and the excluded window's
separate, unresolved worker-startup map race.

#### Boundary-only access admission (2026-10-24)

The [access contract](../../docs/architecture/sharding-migration.md#boundary-only-access-admission-2026-10-24)
now establishes local ownership while resolving the current operation, rather
than accepting a returned point for repeated identity validation. The old point
admission API is removed. Runtime availability is checked at new logical-context
entry; admitted protected reads do not poll service flags or lose their view
when native service acceptance stops. Owners must finish admitted work before
destroying host/index storage. Received-wire validation and migration drain
semantics remain separate boundaries.

Fresh Docker evidence under `/home/users/shuai/mako-native-epoch.WfX0NaeP`
on zoo-002:

- `prepared-boundary-build-1`: all eight requested production/fixture targets
  and their mandatory source gates passed.
- `prepared-boundary-integrity-1`: all **79** pre-verification source hashes
  matched the isolated build tree.
- `prepared-boundary-native-1`: **68 native Cargo tests passed**, including
  retained-view access after Retry/Io availability changes, rejection of
  subsequent cached/refreshed admission, and active/closed/foreign handle cases.
- `prepared-boundary-smoke-1`: both real live layouts and all four metadata
  SIGKILL/reopen scenarios passed. The fixture now performs a real engine read
  and commits a write after logical service stop under an already-admitted
  guard, observes the committed value, releases the guard, then rejects fresh
  engine work. Retained worker reads preserve their view/ownership outcomes
  through stop; fresh admission fails after release and terminal pins drain.
  Existing Frozen, ownership-return, prepared-cache, receiver-forgery and
  old-view OCC retry scenarios remain exercised.
- `prepared-boundary-regressions-1`: all **six** CTest targets passed—client
  service, nontransactional API/distribution, isolated NativeMixedScan, real
  STO transactions and epoch/RCU. Receiver tests reject actual accesses under
  forged grants/coordinates rather than checking reuse of internal point tokens.
- `prepared-boundary-replication-1`: one-shard Paxos passed with **3,224**
  follower replay batches; native migration was disabled.
- `prepared-boundary-regen-1`: `scripts/regen_storage_dsl.sh --check` passed.

The executable FFI and C++ bridge changed; the ownership predicate and formal
proof roots did not. Formal verification was **not rerun**. Boundary admission
and host/engine lifetime are not newly mechanized by these runtime tests.
That correctness window did not include timing. Later matched measurements are
recorded in `boundary-performance-results-v1.json` and the staged comparison
linked below.

`source-manifest-boundary-v1.json`, `mako-source-boundary-v1.patch` and
`mako-source-boundary-v1-files.tar` preserve the pre-verification source.
`boundary-results-v1.json` records receipts and source provenance;
`mako-boundary-final-docs.tar` separately preserves the final documentation.

#### Thin descriptors and indexed bindings (2026-10-24)

The [bridge optimizations](../../docs/architecture/sharding-migration.md#thin-descriptors-and-indexed-bindings-2026-10-24)
replace owned per-point envelopes with lazy serialization, then share indexed
binding/physical mappings across point, scan and receiver paths. Mapping reuse
is not authorization. The Rust ownership predicate, native view invalidation
and formal proof roots are unchanged. Formal verification was **not rerun**;
these C++ cache/lifetime and wire-bridge changes are not newly mechanized.

Fresh Docker evidence under `/home/users/shuai/mako-native-epoch.WfX0NaeP`
on zoo-002:

- `prepared-descriptor-build-1` and `prepared-descriptor-verify-1` passed for the
  first optimization alone: eight production/fixture targets and mandatory
  source gates, all 79 source hashes, both live layouts, four metadata
  SIGKILL/reopen cases and all six targeted CTest suites.
- `prepared-optimized-build-2` passed for the combined implementation. The
  earlier `prepared-optimized-build-1` compile failure is retained, not counted
  as runtime evidence. The final bridge stores opaque pointer-sized handles
  in Rust collections without changing the pinned transpiler.
- `prepared-optimized-verify-1` matched all 79 source hashes, then passed both
  live layouts and all four metadata SIGKILL/reopen cases. Deferred envelopes
  preserve their own raw/fixed route through vector growth and interleaving;
  new logical transactions get fresh identities. Batch rows cannot borrow
  another row's scope or a preceding transaction's capture.
- The same combined receipt passed all **six** targeted CTest suites. Warm
  receiver mappings are exercised across fresh transactions and interleaved
  raw/warehouse scopes. Actual receiver dispatch rejects malformed coordinate
  lengths, wrong coordinates/owners and transplanted grants. Freeze/migration
  after cache warming still rejects stale ownership while unaffected raw keys
  and destination data remain accessible.
- All **68 native Cargo tests** and `scripts/regen_storage_dsl.sh --check`
  passed. One-shard Paxos passed with **3,227 follower replay batches**;
  native migration was disabled in that replication run.

`source-manifest-descriptor-v1.json` and
`mako-source-descriptor-v1-files.tar` preserve the descriptor-only inputs.
`source-manifest-optimized-v2.json` and `mako-source-optimized-v2-files.tar`
preserve the verified combined inputs, extending the preceding boundary
snapshot. `optimized-verified-binary-v1.json` binds that manifest to binary
SHA-256 `7ca8d15a655195cf8ad1f715d05e23ed4a769088515e5e0a6da9901d3a2755b6`
and the frozen timing/profile/attribution drivers.

`prepared-descriptor-benchmark-1` passed all 24 preliminary timing cases.
`prepared-optimized-benchmark-1` and `prepared-optimized-benchmark-2` passed
both complete 36-case six-arm windows: **72 cases**, with ten measured runs per
arm after excluding the two prespecified warmups. The counterbalanced repeat
and every slow trial are retained. `prepared-optimized-profile-1` passed all
twelve separate captures;
`optimized-profile-loss-v1.json` records inspection of every capture log, with
no loss reported. `prepared-optimized-attribution-1` conserved every sample
count and period total against independent `perf report` totals. The
[performance discussion](../../docs/architecture/sharding-migration.md#separate-cpu-attribution)
separates targeted savings from control drift and the noisy storage/transport
differential; the CPU estimates are not additive wall-time costs.

`optimized-results-v1.json` records the two windows, pooled medians, control
drift, conserved CPU attribution and receipt/source fingerprints.
`mako-optimized-final-docs.tar` separately preserves the final design,
storage contract, book and this evidence index. Documentation changes were
not mixed into the frozen benchmark inputs.

#### Historical independent epoch protocol, before native cutover

The complete independent crate and its semantic controls passed in Docker
with Rust `1.97.1` and attested Verus `0.2026.08.02.b677dd5`:

```text
independent model:       688 verified, 0 errors
model negative controls: 51 passed (15 new epoch-protocol controls)
```

The command was `python3 tla/mako/scripts/verify_controls.py` with the pinned
verifier environment. Its positive baseline used the complete `src/lib.rs`,
`--no-cheating`, and no module/function filters or resource overrides. The
constructive schedules and bad-order counterexamples are part of that crate,
not an external bounded checker.

Evidence: `zoo-002:/home/users/shuai/mako-epoch-proof.vx45VcDm/verification-6.log`
and `verification-6.json`; the JSON records the command, source hashes, log
SHA-256, elapsed time, and successful exit status. This run did not change or
reverify the native lease implementation; its earlier results follow below.


#### Historical direct local scans on the lease path

The full Docker proof gate passed with Rust `1.97.1` and pinned Verus
`0.2026.08.02.b677dd5`:

```text
native src/cluster/lib.rs:  1455 verified, 0 errors
independent model:          595 verified, 0 errors
native negative controls:    21 passed
model negative controls:     36 passed
locked native Cargo tests:   64 passed
file-backed SIGKILL cases:    2 passed
```

The source inventory contained 99 reachable files. Both positive crates ran
without module/function filters; every control failed its named semantic
obligation. The new control rejects a direct-scan exhaustion certificate that
silently drops a callback row. An initial native run exceeded the resource
limit in the existing `transcript_complete` proof after a shared predicate was
factored out. Restoring its original inline form fixed automation; no resource
override or weakened theorem was used.

The Release `OPACITY=OFF` Docker build produced `dbtest`, `mako_admin`, the native
smoke fixture and both affected CTest executables. Borrow checking was disabled
in this build. The storage DSL regeneration check passed. Live smoke passed
two-process and single-process migration/scan scenarios, four metadata-only
SIGKILL/reopen cases and the isolated mixed local/remote scan CTest. The existing
client-service and static distributed CTest suites also passed. These runs
exercise foreign code; they do not turn it into verified C++.

Evidence is retained at `/home/users/shuai/mako-local-scan.SdAzPTRe/`:
`proof-v2.json` records the proof source hashes and command,
`proof-v2.log` records the complete gate, and `build-v2`, `storage-dsl`,
`smoke-v1` and `static-ctest` each have command/result JSON and raw logs.
The proof log SHA-256 is
`a22db3bd1f6211b3cd16ea5c8e3817e7ce2e90d4f57d919731b6f491e2c2f22a`.
That revision retained production leases and Drain; it did not deploy the
commit-time-validation proposal below. Its verification record made no new throughput claim.

#### Commit-time-validation proposal

The complete independent crate and its controls passed in Docker with the
pinned Verus/Rust versions above:

```text
independent model:          595 verified, 0 errors
model negative controls:     36 passed (18 new commit-time controls)
actual-engine schedules:     18 passed (12 mutations, 4 gaps/absent points,
                                       2 expected missing-lock failures)
```

The positive run had no module/function filters. Every mutant failed a named
safety obligation, not parsing, typing, solver resources, or a verifier crash.
The engine schedules used actual production callbacks with `OPACITY=OFF`,
Release, both MV modes, and borrow checking disabled for the fixture build.
They did not run a live lease-free migration or test old-source reclamation.
The temporary CMake inclusion was removed; production behavior is unchanged.

Local evidence is retained in
`/home/users/shuai/mako-sharding-commit-proof.VHE0TnJ6/`:
`proof-certified.json` records all independent source hashes and the attested
toolchain; `proof-certified.log` records the full baseline and all controls.
`engine-v2.json` records fixture/build/run hashes and commands;
`engine-v2-run.log` records the observed engine phases. Their log SHA-256 values
are respectively
`43b66839ca24ed4bb4fb9e8d704527c5ba345063070456043cca49890e98d07a`
and `8c949f524c83301fc61dc75d576f2f455a04d45a2161b5bfc43ef78fffc5b7d8`.
These independent-protocol results do not extend the native
refinement/recovery scope of the earlier gate below.

#### Earlier deployed-path gate

The complete `scripts/verify_native_sharding.sh` gate, including certified
snapshot admission, the durable native extension, and metadata-backend
regressions, passed in Docker with Rust `1.97.1` and pinned Verus
`0.2026.08.02.b677dd5`:

```
native src/cluster/lib.rs:  1436 verified, 0 errors
independent model:          556 verified, 0 errors
native negative controls:   10 passed
model negative controls:    18 passed
locked native Cargo tests:  51 passed
file-backed SIGKILL cases:   2 passed
```

Both unchanged positive crates were verified in full before their mutations.
Every negative control produced one semantic failure in its named theorem;
type errors, resource exhaustion and verifier crashes were not accepted.
The native source inventory reported 98 reachable files. Obligation counts
overlap because the native crate imports shared model modules; the inventory
and count totals are not additional correctness claims.

The results include the original core MakoV2 theorems and counterexamples,
placement/history induction, and the recovery, lifecycle, actual-physical
progress, administrative retention, canonical-value, scan, and certified
snapshot/fallback admission results above.
The durable native extension adds checked byte-journal replay, native
metadata/engine effect correspondence, durable admission and completion ordering.
Its fifteen runtime regressions cover uncertain persistence, every migration
phase, stale work, exact transaction settlement, and Ready arriving after Abort.
Both SIGKILL cases reopen real files and resume from durable bytes alone.
The three proof scopes remain distinct; this is not a production crash-safe
migration deployment.

The native MBTA/STO/srpc smoke passed with separate processes and with two owners
in one process: raw and warehouse handoff/return, values and absence,
destination-only cleanup, post-handoff writes/deletes, forward/reverse pagination,
old ownership versions, retained outcomes, held-lease abort, lost successful Commit
reply, and replay of previously issued controls. Fast-path probes additionally
check paired snapshot/grant capture, old-view fallback for an unchanged range,
rejection of fresh leases after Freeze before the snapshot version changes,
continued use of a held lease, and rejection of a forged gateway ownership
version despite a matching snapshot number. The gateway probe calls the real
registered receiver and MBTA implementation; it is not a transport-authentication
test. Fault injection drops a reply at the real peer callback boundary; it does
not substitute an engine or model oracle. The `native_sharding_smoke`,
`mako_admin`, `test_client_service`, and `test_mako_nontxn_distributed` CMake
targets built successfully; both named client/gateway CTest targets passed.
The durable library does not change the storage DSL headers or compiler pin.

The production metadata-only extension additionally passed four real
RocksDB/MBTA/srpc SIGKILL/reopen scenarios: committed and held-lease-pending
migrations, each in separate-process and two-owner single-process layouts.
Committed history survived a post-handoff ordinary write and process loss;
this does not claim that the row write was recovered. Restart retained nonce
generation/outcome, rejected all lease entrypoints and readiness, and refused
new migration/abort work. A surviving origin replayed actually issued
Start/Final/Commit bytes to a restarted destination and observed handler Retry,
not merely a transport failure. The fixture does not start dbtest's ordinary
FastTransport data plane; it exercises the production native services, real
engine callbacks, and lease entrypoints.

Eleven additional real-RocksDB Rust regressions cover strict initialization,
exclusive writers and settlement locks, ordered mixed batches/reopen, actual
SST read corruption versus absence, retained decisions/intents, malformed
committed payloads, and recovered mutation fencing. SIGKILL is process-death
evidence, not a power-loss test or an injected real write/fsync failure.
Full production row/TP crash recovery remains unimplemented.

The 2026-10-11 `ownership_version` naming cutover reran the complete native/model
gate: 1398/526 obligations, 6/15 controls, and 40 Rust tests before this fast-path
extension. The
`native_sharding_smoke`, `mako_admin`, `test_client_service`, and
`test_mako_nontxn_distributed` targets built; both gateway/client CTest targets
passed, as did the two live and four metadata-restart smoke scenarios.
A throwaway upgrade harness also initialized real journals with the preserved
pre-rename executable, killed it, and reopened them with the renamed executable.
All four committed/pending, separate/single-process combinations retained their
nonce outcomes and recovery fences. Field widths, wire positions, and durable
format tags were unchanged. This compatibility check does not add row recovery.
The smoke harness now keeps stderr diagnostics in its log rather than merging
them into the line-framed stdout control channel, preventing a concurrent log
prefix from hiding a successful control marker.


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
- **Core placement versus sharding.** The scalar/single-Raft MakoV2 model retains
  fixed `key % shards` placement and point operations. The separate sharding
  protocol and native scan theorems have the explicit interface scopes above;
  they do not re-prove the distributed transaction engine.
- **Conditional progress, not availability under arbitrary failure.** Permanent
  partitions, nonterminating transactions, endless I/O failures and unbounded
  restarts are not covered. No latency bound is proved.
- **Administrative collection, not global storage reclamation.** Participant
  recovery journals, native receipt dictionaries and outstanding copy/cleanup
  state are not garbage-collected by this protocol.
- **Provisional replies are revocable.** Treating them as final successful
  operations is outside—and contradicts—the proved client contract.
- **Trusted base:** Verus, its supported vstd specifications, Rust frontend and
  Z3. The theorem does not automatically transfer to current legacy gossip,
  timestamp or recovery code.
