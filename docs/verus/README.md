# Verifying Mako's Raft (Rust runtime) with Verus: where it stands

Worktree `/home/users/zyang2/mako-verus`, branch `verus-raft` (from
`srpc-subtree-forward` at `1a346fde4`). Only the Rust lane
(`MAKO_RAFT_LANE=rust`) is in scope.

## Short answer

- **Single thread: not today.** Each Raft group has exactly one poll thread,
  but protocol state is also touched by three other threads: the application
  threads that call `add_log_to_nc`, the submit thread and the apply thread.
  All four share one mutex (`mtx_`). Making the protocol core single-threaded
  is a bounded refactor, described below. It would also fix one data race
  that exists today.
- **Verus: the protocol logic fits; the code as written does not.** A Verus
  model of the leader's commit rule, keeping the production control flow, is
  proved (`src/deptran/raft/verus/commit_rule.rs`, 11 verified, 0 errors).
  The core files themselves are full of what Verus cannot verify: FFI calls
  into C++, raw pointers, mutexes, atomics. For example, `server_h.rs` has
  ~219 `unsafe`, 179 raw-pointer uses, 40 C++ kernel declarations,
  30 `Mutex` and 136 atomic uses. The realistic shape is a pure, verified
  protocol state machine that the runtime calls.

## 1. What Verus is, briefly

Verus is a verifier for Rust. You write ordinary Rust inside a `verus! { }`
block, plus:
- *specifications*: `requires` (preconditions), `ensures` (postconditions)
  and loop `invariant`s;
- *spec functions*: mathematical definitions used only in proofs;
- *proof functions*: lemmas.

Verus translates this to logic and asks the Z3 solver to prove that every
execution satisfies the specifications, with no testing involved. Code
outside `verus!`, or marked `#[verifier::external]`, is trusted rather than
checked.

**What it handles well:** structs and enums, `Vec`, `Option`, arithmetic
(with overflow checked), loops with invariants, `&mut` parameters, traits in
moderation.

**What it does not verify** (it must be marked external/trusted, or
rewritten):
- `unsafe` code, raw pointers and FFI (`extern "C"`) calls;
- `std::sync::Mutex` and atomics as ordinary types. Verus has its own
  ghost-state tools for concurrency, but they are a different, much heavier
  way of writing the code;
- trait objects (`dyn`) and closures, in general;
- most of `std` beyond what `vstd` specifies;
- async code.

Installed here without root:
- Verus `0.2026.09.27.3cf1832` in `~/.local/opt/verus-x86-linux`;
- the Rust 1.98.1 toolchain it requires, installed with rustup.

Run it as `~/.local/opt/verus-x86-linux/verus <file>.rs`.

## 2. The thread picture (Rust lane)

Evidence: `server.cc:964` (apply thread), `raft_worker.cc:747` (submit
thread), `rt/src/transport.rs:198` (the poll thread),
`server_h.rs:4600-4691` (`IsLeader`, `Start`), `server_h.rs:4655`
(`CommitIndex`, unlocked).

| thread | touches Raft state? | how |
|---|---|---|
| poll thread (one per group) | yes, almost all of it | fibers: heartbeat, election, RPC handlers, reply callbacks; under `mtx_` |
| submit thread | yes | `Start()` appends to the log under `mtx_`, then `RequestReplication()` |
| application threads (Mako workers, raft_bench) | yes | `IsLeader`, `GetLeaderHint`, sometimes `Start()` directly, and the **unlocked** `CommitIndex()` read (a data race) |
| apply thread | yes | publishes the applied index, compacts the log, creates snapshots; under `mtx_` and the apply mutex |
| worker/main thread | at startup/shutdown | `Setup`, `PrepareForShutdown` |
| srpc reconnect threads | not Raft state | fill reply slots on connection close (not exhaustively verified) |

**Written only by the poll thread:** term, vote, leader id and role,
election state, per-follower next/match index, commit index, and the
heartbeat round state.

**Crossing threads:** the log (appended by poll and submit/app threads,
compacted by apply), the applied index, the snapshot index and store, and the
apply queue.

**Fibers still interleave.** Even with one OS thread, the heartbeat,
election and handler fibers interleave where they suspend:
- the heartbeat wait;
- phase 2's up-to-1 ms sleep;
- the election's vote-wait sleeps.

No fiber holds `mtx_` across a suspension point, but the heartbeat and
election fibers do carry state captured before the suspension (round term,
commit index, election term) and re-validate it afterwards. A single-threaded
verifier therefore still has to model these fibers as interleaved event
handlers, with each handler atomic between its suspension points.

## 3. Making the protocol core single-threaded

Route every cross-thread touch through the poll thread as a message (a job),
so that only the poll thread ever owns Raft state:

| today | change | performance effect |
|---|---|---|
| `Start()` on the submit/app threads | the submit thread posts one job per batch that appends and marks replication pending | about neutral: a wake job is already posted per wake. Jobs wait up to 1 ms for `epoll_wait` (no eventfd); adding an eventfd wake would remove that for both paths |
| `IsLeader` / leader hint / `CommitIndex` from other threads | the poll thread publishes atomic mirrors; readers never touch `state_` | faster (no mutex), and fixes the race |
| apply thread writing the applied index, compacting, snapshotting | keep the thread (the app callback must not block the poll thread); it posts "applied through N" and "snapshot saved at (i,t)" jobs; the poll thread updates and compacts | one job per batch |
| `OnInstallSnapshot` on the poll thread taking the apply mutex | hand the install to the apply thread, which posts completion | removes an existing stall |
| the apply queue behind a mutex | a one-way channel (poll → apply), treated as output by the verifier | neutral |
| shutdown, preferred leader, snapshot callbacks | posted as jobs, and the caller waits | cold paths |

After that, `mtx_` can go. It is itself a hazard: a fiber that blocks on it
stalls the whole poll thread.

## 4. The architecture that fits Verus

Split the Raft core into two layers:

1. **Protocol state machine, verified (Verus).** A plain struct
   (term, vote, log metadata, commit index, per-follower progress, role)
   with one function per event:
   - on submit, and on the election timeout and heartbeat tick;
   - on each of AppendEntries, RequestVote and InstallSnapshot, and on
     their replies;
   - on applied-through.

   Each function returns a list of actions: send message M to peer P, append
   or truncate the log, hand entries to apply, reset a timer.

   It uses no I/O, no locks, no FFI and no raw pointers, so everything in
   §1's "handles well" list is enough. Specifications state Raft's
   invariants: election safety, log matching, leader completeness and state
   machine safety, as in the Raft paper, as properties of the handlers.
2. **Runtime shell, trusted (unchanged in spirit).** The poll thread,
   fibers, srpc, the payload bytes and the C++ kernels. It turns network and
   timer events into handler calls and carries out the returned actions. The
   only place concurrency appears is between shell and core, as messages.

This is also the natural input for a tool that verifies distributed
protocols: a set of single-threaded nodes, each a state machine driven by
messages.

How far the current code is from it:
- **Already close.** Much of the logic is already written as pure functions
  over `RaftConsensusState`: `raft_commit_advance`, `majority_match_index`,
  the `raft_server_*` predicates, and the phase functions of the heartbeat.
- **The main work is the other half.** The fibers' multi-step flows need to
  become event handlers, with no sleeping inside protocol logic:
  - the heartbeat's send, collect and commit;
  - the election's broadcast, wait and tally.

## 5. Done so far

- **`src/deptran/raft/verus/commit_rule.rs`:** a Verus model of
  `majority_match_index` (`server_h.rs:881`), `commit_index_candidate`
  (`:313`) and `raft_commit_advance` (`server_cc.rs:633`), keeping the same
  loops and checks. Proved:
  - the commit index never decreases;
  - a new commit index is in the log;
  - its entry has the current term;
  - a majority (the leader plus at least nservers/2 followers) holds it.
- **Mutation checks:** three planted bugs are each rejected by Verus:
  - selecting the wrong rank;
  - dropping the current-term check;
  - dropping the forward-only check.
- **Caveat:** this is a *model*. It proves the same algorithm with reduced
  types; it does not compile the production function itself. Closing that
  gap is what §4 is for.

## 6. What the verification group's method needs (answered from their repo)

Source: `git@github.com:ZhangZihao270/ghost-log-refinement.git` at `d7e04ed7`,
mainly `docs/ghost-log/README.md`, `docs/ghost-log/method/design.md` §3-4 and
`docs/ghost-log/raftrs/coupling.md` §7.

The method is *ghost-log refinement*, in Verus. It proves that an existing
implementation refines their hand-written atomic Raft spec without
rewriting the code. Ghost instrumentation records what the code does
(`Recv`, `Tick`, `Set(field, value)`, `Send`, `Close(action)`), and
refinement is proved over that log.

| question | their answer |
|---|---|
| input | the implementation itself, ported into `verus!` with only registered, semantics-preserving transformations (raft-rs: 98% of lines verbatim), plus added ghost lines. Their spec (2,323 lines), safety proof (12,582) and method layer (2,801) are reused; a new implementation supplies a coupling layer (raft-rs: 3,357 lines) |
| properties | safety only: at most one leader per term, log matching, committed entries never lost or changed, linearizable reads; plus panic-freedom. Liveness is not proved |
| network | N nodes under any causal message schedule (`theorem_compose_safety`); a refused message counts as a dropped packet |
| code or model | the executable code (the port) is verified; differential testing against the unmodified original shows fidelity |
| threads | the raft-rs mainline is "single-threaded and processes one message at a time" (`method/design.md` §4); multi-threaded interleaving is explicitly deferred |
| Verus limits | unsafe, async, trait objects and third-party crates are handled by porting |
| not covered yet | snapshots and log compaction (their A10), restarting from existing storage; storage is trusted |

**What this means for our Raft.**
- **The target shape is a library like raft-rs's `RawNode`.** It has entry
  points (`step` a message, `tick`, `propose`, take `ready`, `advance`) that
  one host thread calls one at a time. It owns protocol state and does no
  I/O itself.
- **Our core is not that shape yet**, in three ways:
  - Protocol steps live inside fibers that sleep mid-action: heartbeat
    phase 2, and the election's vote wait. That is the same problem as
    async.
  - It reaches the runtime through `extern "C"` kernels and raw-pointer
    carriers.
  - Four threads touch it under `mtx_` (§2).
- **The ghost log does tolerate our call structure.** An action may be split
  across helpers, or a helper may close several actions. So the handlers do
  not have to line up one-to-one with spec actions; they only need to run
  to completion without suspending.
- **The fit is therefore: §3 (single owner thread) + §4 (`RawNode`-style
  core).** The core then moves into `verus!` and is instrumented against
  their spec, with snapshots gated out at first, as in their raft-rs port.
