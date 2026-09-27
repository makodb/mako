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
