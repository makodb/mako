# Raft data structures — classified by exposure to the memory/durable boundary

Written 2026-09-07. **This file governs what may be converted without review.**

`async_persistence` (owner's description): *run the consensus logic in memory
and publish only after whatever supported the committed decision has been
written to disk and can't be flipped.* That is a two-tier acknowledgement
design, and it is the safety argument of this implementation. It is not
textbook Raft, so a mechanically-correct refactor can silently weaken it.

The classification below is by **exposure to that boundary**, not by how hard
the type is to convert.

---

## The boundary itself, for reference

```
server.h:2347   // Invariant: securedLogIndex_ <= specCommitIndex_ <= lastLogIndex
server.h:2348   uint64_t securedLogIndex_ = 0;   // highest index with DURABLE ack quorum
server.h:2349   uint64_t specCommitIndex_ = 0;   // highest index with MEMORY  ack quorum

AckType 0 = MEMORY      raft_server_ack_is_memory   (server.h:714)
AckType 1 = DURABLE     raft_server_ack_is_durable  (server.h:791)

AppendEntries         -> returns followerAckType            rcc_rpc.rpc:61
AppendEntriesDurable  -> second RPC, sent AFTER fsync       rcc_rpc.rpc:63
VoteDurable           -> same, for votes                    rcc_rpc.rpc:30
```

Entries in `(securedLogIndex_, specCommitIndex_]` are committed in memory but
**not yet unflippable**. That half-open interval *is* the speculative window.

---

## CLASS A — ON the boundary. Do not convert without review.

Touching any of these can weaken the safety argument without failing a test.

| structure | file:line | why it is Class A |
|---|---|---|
| `securedLogIndex_`, `specCommitIndex_` | `server.h:2348-2349` | the boundary itself |
| `specVoters_`, `durableVoters_` | `server.h:2336-2337` | memory vs durable election quorums |
| `securedLeader_` | `server.h:2333` | "a quorum has votedFor = me **on disk**" |
| `durableAcks_` | `server.h:2354` | per-index durable ack tracking |
| `AckType` enum | `commo.h:226` (DSL) | the MEMORY/DURABLE tag itself |
| `CommitStatus` enum | `server.h:129` (DSL) | SPECULATIVE vs DURABLE client callback tiers |
| `StepDownReason` enum | `server.h:86` (DSL) | Unsecured/SecuredFailure rollback semantics |
| `PendingCommitCallback` | `server.h:2387` | fires at each tier |
| `LogPersistenceTicketCompletion` | `server.h:2145` | the durable-write handshake |
| `AsyncCallbackLifetime` | `server.h:1964` | guards callbacks across async persistence |
| `AtomicFlag` + `async_threads_` | `server.h:2360`, `:2376` | async persistence worker completion |
| `RecoveryMode` enum + `RecoveryManager` | `recovery_manager.hpp` | decides what survived a crash |

**DSL predicates that are Class A** (13, all in `server.h`): `ack_is_memory`,
`ack_is_durable`, `should_become_secured`,
`unsecured_leader_needs_quorum_check`, `persistence_can_report_durable`,
`sync_reply_is_durable`, `durable_write_succeeded`,
`async_persistence_should_queue`, `persistence_ticket_is_ready`,
`persisted_reply_context_is_current`, `can_buffer_early_durable_vote`,
`commit_status_is_durable`, `follower_append_ack_type`.

---

## CLASS B — commit-outcome adjacent. Convertible, but the shape must be argued.

These reason about *log-slot resolution*, not about the ack tier. They read
`commitIndex` / `snapidx_`, **not** `specCommitIndex_` / `securedLogIndex_` —
verified by reading `GetSubmissionProgress` (`server.cc:2771`).

| structure | file:line | note |
|---|---|---|
| `RaftSubmissionProgress` | `server.h:1905` | 3 bools. Its `indeterminate` field is a *terminal commit-outcome ambiguity* after a divergent snapshot — outcome reporting, not the ack tier. **Safe to convert as a struct**; do not let its `impl` start reading spec/secured state. |
| `RaftResolvedSubmissionLedger` | `server.h:1922` | one-shot terminal results consumed by `GetSubmissionProgress`. Holds a map; needs the crate. |
| `QueuedApplyEntry` | `server.h:2679` | apply-queue epoch guard. Blocked on `Command` regardless. |
| the submission predicates | `server.h:829-905` | `submission_is_committed/_superseded`, `snapshot_submission_*` |

---

## CLASS C — OFF the boundary. Safe to convert autonomously.

No relationship to acknowledgement tiers, durability, or election quorums.

| structure | file:line | status |
|---|---|---|
| `CRC32` | `snapshot_format.hpp:370` | one `uint32_t` of state, 3 methods. Pure checksum arithmetic. **Best target.** |
| `SnapshotCompression`, `SnapshotChecksumType` | `snapshot_format.hpp:44` (DSL) | already owned; format tags |
| `SnapshotHeader` | `snapshot_format.hpp:259` | blocked by `#pragma pack(1)` + padding array, unrelated to safety |
| `SnapshotMetadata` | `snapshot_manager.hpp:59` | plain scalars |
| the CRC32 + format predicates | `snapshot_format.hpp:115-346` (DSL) | already owned |
| `quorum.hpp` predicates | `quorum.hpp:48` (DSL) | pure counting arithmetic — note: *quorum size*, not *which tier of quorum* |
| `RaftGroupMode`, arg parsing | `raft_main_helper.cc` (DSL) | startup only |
| `raft_testconf.index_math` | `testconf.cc:28` (DSL) | test harness only |
| `ReplicatedDBOp` | `replicated_db.h:28` (DSL) | already owned, but see open Q2 (invalid-byte decode) |

---

## A documentation bug found while classifying

`server.h:82`, in the `StepDownReason` doc comment, describes the suspect
range for `SecuredFailure` as:

```
 *   Only unsecured entries (specCommitIndex, securedLogIndex] are suspect.
```

The bounds are **reversed**. The invariant at `server.h:2347` is
`securedLogIndex_ <= specCommitIndex_`, so that interval is empty. The suspect
entries — memory-acked but not yet durable — are
`(securedLogIndex_, specCommitIndex_]`.

The code appears to behave correctly; this is the comment being wrong. But it
is exactly the kind of thing that misleads someone converting this logic, so
it should be fixed. I have **not** changed it: it sits in a Class A doc block
and I would rather you confirm the intended reading.

---

## Rule I am following overnight

Convert **Class C only**. Convert `RaftSubmissionProgress` from Class B as a
plain struct because it is genuinely 3 bools, but do not give it methods that
reach into spec/secured state. Anything Class A waits for you.
