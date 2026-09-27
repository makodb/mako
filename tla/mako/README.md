# Mako in tla-rs: model and proofs

This directory is a [tla-rs](https://github.com/stonysystems/tla-rs) style
model of Mako's speculative two-phase commit (OSDI'25, *Mako: Speculative
Distributed Transactions with Geo-Replication*), written as TLA-style
relations in Verus, with machine-checked proofs of its safety.

The model follows the paper's protocol (Sections 4 and 5, Appendices A and D).
It is not a refinement proof of the C++ code in `src/mako`. Where the code
differs from the paper in a way that matters for safety, the differences are
listed at the end of this file.

## Layout

| File | Contents |
|---|---|
| `src/types.rs` | Transactions, vector clocks, streams, versions, global state, watermark helpers |
| `src/normal.rs` | Failure-free actions: Submit, Read, Prepare, Install, Certify, Commit, Abort, Replicate |
| `src/recovery.rs` | Failure actions: Crash, AdvanceEpoch, CloseEpoch, Rollback |
| `src/behavior.rs` | `Action`, `enabled`, `apply`, `next`, `behavior` |
| `src/history.rs` | The external specification: strict serializability of acknowledged transactions |
| `src/invariants.rs` | The inductive invariant, one named predicate per conjunct |
| `src/stream_lemmas.rs` | Stream-watermark lemmas |
| `src/proofs_*.rs` | Preservation proofs per action, safety theorems, witnesses |

## How the paper maps to the model

| Paper | Model |
|---|---|
| One-shot transaction executed by a coordinator worker thread (4.2) | `Submit` binds a transaction to `(coord, thread)`; a thread runs one transaction at a time |
| Optimistic reads, buffered writes (4.2) | `Read` records the writer, epoch, clock and value of the top version |
| Lock, GetClock, Validate (4.2) | `Prepare`, one atomic step; the clock is the max of same-epoch ReadSet clocks and freshly incremented clocks of the WriteSet shards and the coordinator |
| Install (4.2, Appendix D) | `Install` per shard: speculative version, lock release, counter bump, log entry |
| Coordinator logs to its own stream | `Certify`, after every remote Install |
| Per-core Paxos streams (4.3) | One stream per `(shard, coordinator, thread)`: a durable prefix and a pending suffix; `Replicate` makes the next pending entry durable |
| Vector watermark, commit and client reply (4.4) | `Commit` requires the clock vector to be below every stream's durable watermark |
| Compressed vector clocks (6.1) | `Constants::cidx` maps shards to clock components; identity is the full vector, a constant map is a single timestamp |
| Leader failure, new epoch, closing the epoch (5.2) | `Crash`: a prefix of each stream's pending entries survives, versions without a durable entry are dropped, the epoch advances |
| Healthy shard closes old epoch with INF (5.2) | `AdvanceEpoch`: in-flight work is terminated; INF on every stream except a hung worker's own |
| Finalized shard watermark and FVW (5.2) | `CloseEpoch`: minimum of the shard's durable stream watermarks once nothing of the epoch is pending |
| Rollback of transactions not below the FVW (5.2) | `Rollback` removes those versions at one shard |
| New-epoch reads of old-epoch versions (5.2, Lemma 8) | `Read` of an older-epoch version only once that epoch's FVW exists and the version is below it |

## Assumptions stated by the model

1. **Dedicated, sequential streams.** Each Paxos stream carries entries from
   one coordinator worker thread, and that thread runs one transaction at a
   time. This is what makes the paper's Fact 3 (a stream's clocks increase)
   true. It is a design assumption; see the implementation notes below.
2. **Post-increment GetClock.** A fetched clock exceeds every clock previously
   fetched or installed at that shard (Section 4.2, Appendix D's Install).
3. **Blocking Install.** A worker whose Install cannot finish stays blocked
   until its shard advances the epoch, which terminates the transaction
   (Lemma 6).
4. **Abstract consensus and configuration manager.** Paxos is a per-stream
   durable prefix. The configuration manager's epoch is a global counter.
5. **Commit waits for the watermark.** The client is acknowledged only when
   the transaction's clock is below the vector watermark (Section 4.4 and the
   caption of Figure 1).
6. **Keys and values.** Keys are non-negative integers owned by shard
   `key % shards`; values are integers, initially 0. Operations are `Read`,
   `Put` and `Add` (read-modify-write).

An idle stream holds its shard's watermark at 0, because an idle dedicated
stream may still receive an entry for a clock a transaction has already
fetched. This is safe but means commits need every stream of a shard to make
progress; a heartbeat entry would remove that liveness cost and is not modeled.

## Verifying

The pinned toolchain is Verus `0.2026.08.02.b677dd5` with Rust `1.97.1`, the
same pin tla-rs uses.

```bash
VERUS_PATH=/path/to/verus tla/mako/scripts/verify.sh --triggers-mode silent
```

The modules also verify unchanged inside a tla-rs checkout, as
`src/protocol/Mako` with a `mod.rs` listing the same modules as `src/lib.rs`
and `pub mod Mako;` added to `src/protocol/mod.rs`:

```bash
cd tla-rs
mods=(); for f in src/protocol/Mako/*.rs; do m=$(basename "$f" .rs)
  [[ $m != mod ]] && mods+=(--verify-only-module "protocol::Mako::$m"); done
"$VERUS_PATH" --crate-type=lib src/lib.rs "${mods[@]}" --triggers-mode silent
```

The whole crate verifies with `274 verified, 0 errors` in about 15 seconds.
No file uses `assume`, `admit`, `external_body`, `external` or an `rlimit`
override.

## What is proved

The main result is `theorem_mako_strictly_serializable` in
`src/proofs_main.rs`: in every state of every behavior, the history of
acknowledged transactions is strictly serializable. The specification in
`src/history.rs` mentions only client-observable facts: when each transaction
was submitted and acknowledged, and the values it read. A serial order must
contain every acknowledged transaction, respect real time between an
acknowledgment and a later submission, and reproduce every value read. The
proof's witness is the certification order restricted to transactions whose
clocks are below the vector watermark.

| Theorem | File | Statement |
|---|---|---|
| `theorem_invariant` | `proofs_main.rs` | The inductive invariant `inv` holds in every reachable state |
| `theorem_mako_strictly_serializable` | `proofs_main.rs` | Every reachable state is strictly serializable |
| `theorem_ack_is_final` | `proofs_stable.rs` | An acknowledged transaction's record, and so its results, never change |
| `theorem_durability` | `proofs_history.rs` | An acknowledged transaction's log entries are durable on every stream it logged to, it is never rolled back, and all its writes are present |
| `theorem_atomicity` | `proofs_history.rs` | Paper Theorem 1 and Lemma 6: once an epoch's FVW exists, a transaction terminated during Install is rolled back, a surviving one has all its writes, and a rolled-back one has none at any shard that ran rollback |
| `theorem_rollback_safety` | `proofs_history.rs` | Paper Theorem 2 with Lemmas 1, 7 and 8: a reader's writer is in the same or an older epoch, same-epoch clocks are ordered, a rolled-back writer drags its readers down, and an acknowledged reader's writer is durable and never rolled back |
| `witness_single_shard_commit`, `witness_two_shard_commit`, `witness_crash_rollback` | `proofs_witness.rs` | Verified traces from the initial state: a commit, a cross-shard read-write commit, and a crash that dooms and rolls back a certified transaction |

The specification lets the serial order include certified transactions that
are not yet acknowledged, the usual convention for pending operations. Such a
transaction may later be rolled back, since rollback does not change its
status. The proof's witness never includes one, and `theorem_durability` and
`theorem_rollback_safety` separately rule out an acknowledged transaction
depending on a rolled-back write.

The per-state theorems in `proofs_history.rs` require `inv`, which
`theorem_invariant` supplies for every reachable state.

## Evidence that the proof has teeth

- **Injected bugs fail verification.** Letting `Commit` skip the watermark
  check fails 1 obligation. Making GetClock return the pre-increment value
  fails 5.
- **Bounded model checking.** `modelcheck/mako_mc.py` mirrors the Verus model
  function by function and explores it exhaustively for small instances,
  checking the invariant, the strict-serializability specification by brute
  force, and the other theorems in every state. Fifteen configurations, each
  explored exhaustively and up to 6.8 million states, found no violation.
  Three cross-checks of the checker's own state abstractions also found none;
  one of them stopped at its 5-million-state cap. Commits, commits after a crash,
  dependent commits and cross-epoch reads are all reachable. Of twelve
  deliberate mutations, eleven were caught in at least one configuration. The
  one not caught keeps unreplicated versions at a crash; it is harmless,
  because such a version can never be read and a later rollback removes it.
  `modelcheck/SUMMARY.txt` has the counts; `modelcheck/run_all.sh` reproduces
  them.
- **The tla-rs model checker was tried first** and cannot check this model: it
  builds initial states by enumerating the whole state type, and aborts on the
  nested maps of streams and transactions. `modelcheck/tlars-attempt/` has the
  wrapper and the constructs it rejects.

## Limits of the result

- **Protocol, not code.** Nothing here is proved about `src/mako`.
- **Atomic steps.** Lock, GetClock and Validate run as one step, and each
  Install is one step at one shard. A real execution interleaves these RPC
  rounds; the model assumes that coarsening is sound and does not prove it.
- **Safety only.** Liveness (that transactions eventually commit, that an
  epoch eventually closes) is not claimed.
- **Followers.** Replay is represented by the durable stream prefixes. The
  follower state machine and Thomas's write rule are not modeled.
- **Trusted base.** Verus, Z3 and vstd.

## Findings

### About the paper

1. **Fact 3 needs a stream discipline the paper does not state.** Section 4.3
   gives each worker thread a stream, but does not say which stream a remote
   Install at a shard is logged to. If it shares the local worker's stream, two
   independent clock sequences interleave and a stream's clocks are no longer
   increasing, so "clock below watermark" stops implying "replicated". The model
   gives each coordinator thread a dedicated stream at every shard.
2. **GetClock must return the post-increment value.** Appendix D's Install
   comment requires it; the prose is ambiguous. With the pre-increment value
   the proof fails (see above).
3. **The coordinator must fetch its own clock.** Appendix D's `getClock` omits
   a coordinator with no local write. Without it, the model checker finds
   atomicity and strict-serializability violations with two shards, because a
   transaction that hangs in Install is no longer guaranteed to be above the
   coordinator stream's watermark (Lemma 6).
4. **Idle streams stall the watermark.** An idle stream must hold its shard's
   watermark, which is safe but blocks commits until every stream moves. A
   heartbeat entry is needed for liveness.

### About the documentation

`docs/architecture/speculative-2pc.md` says the client is told "success"
before replication and that a lost transaction was already acknowledged. The
paper (Figure 1 caption, Section 4.4) acknowledges only after the vector
watermark covers the transaction, and the proof depends on that. The same
document says readers see only transactions below the watermark; they see
every installed version.

### About the C++ implementation

These were found by reviewing the default build against the paper and the
model. Each was then confirmed by a second, independent reading of the code
with file and line evidence. None was reproduced by running the system.

| # | Issue | Where | What it breaks |
|---|---|---|---|
| 1 | GetClock and the coordinator's clock return the pre-increment value, so consecutive transactions can get equal clocks; followers then drop the later of two equal-clock writes | `lib/server.cc:225`, `sto/Transaction.hh:789-798`, `sto/Transaction.cc:586-589` | Fact 3; follower replicas diverge |
| 2 | Remote installs are logged into the local worker's stream with the same index, from a separate batch buffer | `benchmarks/rpc_setup.cc:57-60`, `sto/Transaction.cc:831-833` | Fact 3; atomicity after a leader failure |
| 3 | Shard watermarks are combined with max instead of min | `lib/shardClient.cc:430-436`, `sto/Transaction.cc:559-565`, `sto/sync_util.hh:313-318` | Watermark meaning; atomicity after failover |
| 4 | Streams that are idle or below the current watermark are dropped from the minimum | `sto/sync_util.hh:132-145`, `:246-252` | Watermark meaning; rollback safety |
| 5 | A failed or timed-out remote Install is ignored: the catch block breaks immediately and the status is never checked | `sto/Transaction.cc:606-627` | Lemma 6 and atomicity |
| 6 | Install at a shard that already moved to a new epoch is applied, logged and unlocked before the epoch check, and tagged with the new epoch | `lib/server.cc:169-190` | Atomicity across an epoch change |
| 7 | The client is acknowledged before replication or any watermark wait | `sto/Transaction.cc:653-661` | Durability of acknowledged transactions |
| 8 | Remote reads are never validated: Validate is sent only to shards in the write set | `lib/shardClient.cc:417-419` | Serializability, for read-only and read-write transactions |
| 9 | Rollback uses per-shard or max-combined thresholds in mixed units instead of one pairwise-minimum FVW | `mako.hh:677-695`, `benchmarks/bench.cc:176-191`, `sto/multiversion.hh:119-130` | Atomicity (Lemma 5, Theorem 1) |
| 10 | A new-epoch transaction can read an old-epoch version before that epoch's FVW is known | `sto/multiversion.hh:113-117`, `sto/MassTrans.hh:369-379` | Lemma 8 and rollback safety |
| 11 | The multiversion read picks the wrong version during rollback and never hides rolled-back inserts | `sto/multiversion.hh:128-165` | Rollback |
| 12 | Followers discard a whole queued batch at an epoch boundary instead of filtering per transaction | `mako.hh:299-318`, `sto/ReplayDB.cc:14-77` | Durability after a second failure |
| 13 | The encoded replication callback value `timestamp*10+status` overflows `int` | `mako.hh:339`, `mako.hh:429` | Batches that fail the safety check are neither replayed nor queued |

Paths in the table are relative to `src/mako`. Item 7 is a deviation from the
paper's stated design rather than a violation of its formal theorems; the
others can violate a property the paper proves.
