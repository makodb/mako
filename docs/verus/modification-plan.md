# Modification plan: a Raft core that ghost-log refinement can verify, with no performance loss

Scope: Mako's Rust-lane Raft (`MAKO_RAFT_LANE=rust`). Paths without a prefix are
under `src/deptran/raft/` in this repo. Paths prefixed `glr/` are under
`/home/users/zyang2/ghost-log-refinement/`. Companion document:
[README.md](README.md) (the thread audit and the summary of what the method needs).

Status: this plan comes from reading code and docs. Nothing in it was built,
run, or checked with Verus. Anything I did not confirm is marked
**not verified**. I checked these facts directly in this tree at `76ea6cb46`:
- the snapshot-off branch of `InitializeSnapshotManagerLocked`
  (`src/server_h.rs:2607-2625`);
- that `Start`'s index and term out-params are discarded (`raft_worker.cc:853-858`),
  but its `REJECTED` result is not (`:856-858`);
- that `CommitIndex()` is an unlocked read (`src/server_h.rs:4655-4657`);
- that `AeDecodePayload` does not reject term-0 entries (`src/server_h.rs:4263-4288`);
- that the poll thread's epoll timeout is a fixed 1 ms
  (`src/srpc/reactor/epoll_wrapper.rs:124`);
- that a success AppendEntries reply already carries the end index it proves
  (`src/server_h.rs:5371`, `:5466-5469`).

Every file:line citation in this revision was re-read against the tree.

---

## 1. Plain-language summary

### 1.1 What changes, and why

Today Raft's decisions are made in Rust code that several threads reach under a
mutex. Two fibers also sleep in the middle of a protocol step. Ghost-log
refinement checks a node only if each call into its protocol code runs from
start to finish with nothing else touching that state, and only if that code is
plain safe Rust with no FFI, locks or atomics.

So the plan pulls Raft's decisions out into a small **core**: one Rust struct,
plus one function per event. The core does no I/O. Everything else (srpc,
encode/decode, timers, the apply thread, the C++ shim) stays as a trusted
**shell** around it. Then the group's method is applied to the core, and their
spec, safety proof and composition theorem are reused unchanged.

A concrete example. Today one heartbeat fiber sends AppendEntries (phase 1),
then sleeps in 1 ms steps while replies arrive (phase 2,
`src/server_cc.rs:1501-1690`, sleep at `:1676`), then advances the commit index
(phase 3). After the change, the same round is three kinds of core call, and
none of them sleeps:

```
TickHeartbeat          -> [Send AE to f1, Send AE to f2]   (and ApplyRange, if
                                                             phase 0 advanced commit)
RecvAppendResp(f1, r)  -> []            (match_index[f1] updated)
RoundEnd               -> [ApplyRange(41..45)]
```

`TickHeartbeat` can also return `ApplyRange`, because today's phase 0 already
runs the commit rule (`heartbeat_phase0_locked` calls `raft_commit_advance` at
`src/server_cc.rs:763`, and the caller enqueues at `:828-833`).

The shell keeps the 1 ms polling loop and calls the core between sleeps, so the
timing does not change. Each core call becomes one or more closed segments of
the ghost log, and each segment is proved to be a legal spec step.

### 1.2 The end state in one picture

```
 app threads ─add_log_to_nc─► RaftWorker::Submit (C++, API unchanged)
                                   │
          ┌────────────────────────▼──────────── trusted shell ──────────────┐
          │ srpc reactor + rpc, seam.rs/transport.rs, C++ batch codec,       │
          │ timers, apply thread, snapshot store, raft_worker.cc             │
          │                                                                  │
          │   with_core(|core| core.step(event, &mut out))   ◄─ one call     │
          │   then: encode+send out.sends, push ApplyRange to apply thread,  │
          │         fire callbacks, re-arm timers, publish atomic mirrors    │
          │  ┌──────────── verified core (verus!, plain safe Rust) ────────┐ │
          │  │ term, vote, role, log of (term, Cmd handle), commit,        │ │
          │  │ applied, match/next, in-flight slots, vote set, round       │ │
          │  │ + erased ghost log: Recv/Tick, Set, Send, Close             │ │
          │  └─────────────────────────────────────────────────────────────┘ │
          └──────────────────────────────────────────────────────────────────┘
 readers on other threads see only atomic mirrors: is_leader, hint, commit, applied
```

Payload bytes never enter the core. The core stores an opaque handle per entry
(today's `RaftCommand`, a refcounted C++ pointer whose clone only bumps a count,
`server.cc:461-464`).

### 1.3 Is "assume the RPC is correct" right?

Partly. The method assumes **less** than "RPC is correct". Our code needs one
thing **more** than the method, and that extra need can be removed cheaply.

**What the method assumes about the network** (glr/`src/protocol/Raft/ghost_log_compose.rs:378-386`,
the `causal` predicate; glr/`docs/ghost-log/raftrs/composition.md` §3;
glr/`docs/ghost-log/spec/raft-spec.md` §2):

(`msg_view` is the function that reads a wire message as a spec message; §2
defines it.)

*Assumed* (environment; cannot be proved inside one node):

1. *Genuine packets.* Every message handed to node i's core equals, under
   `msg_view`, a message some verified core earlier queued for i. Each entry of
   a batched append counts on its own.
2. *Truthful sender.* The packet's source id is the real sender, so there are
   no Byzantine peers. (That the sender is a configured voter is *not* an
   assumption: it is a code check, B20 in glr/`docs/ghost-log/raftrs/composition.md:80-84`,
   which `step_checked` performs in Phase 6.)
3. *Static, shared configuration.* Every node has the same voter set, so the
   ranks the proof uses agree (composition.md §3 item 2).
4. *Per-node trusted base.* Storage, FFI and the node's constructor
   (composition.md §3 item 4, pointing at coupling.md §7).
5. *Nothing else about the network.* Loss, duplication, reordering, arbitrary
   delay and timers firing at any moment are all allowed. The method needs no
   FIFO, exactly-once delivery, retries or liveness. A message the core refuses
   counts as dropped.

**What our code relies on in addition today:**

- **Forged "unavailable" vote replies.** When a server is disconnected or not
  RPC-ready, `ServeVote` answers `reply_term = can_term, vote_granted = 0`
  (`src/server_h.rs:4704-4716`). No core produced that reply, and its term is
  the candidate's, not the responder's. `rpc_ready_` is false in production
  windows, not only in tests: before startup is published, after FailStop
  (`:2886`), on an apply-thread failure (`:3413`) and during shutdown
  (`:4570`). On the wire this reply looks like a real rejection, so no gate can
  exclude it; this breaks assumption 1. Fixed in Phase 1 (below).
  `ServeAppendEntries`' unavailable reply (0/0/0, `:4718-4732`) is already read
  as "unavailable", which counts as a drop (`src/server_cc.rs:1563-1565`).
- **Reply attribution.** A reply must be attributed to the follower whose
  pending slot it completes. This is part of "truthful sender/routing". The
  rest of pairing is *not* needed for the spec view: a success reply already
  carries the end index it proves, `follower_last_log_index = accepted_through
  = prev + count` (`src/server_h.rs:5371`, `:5466-5469`), and the leader takes
  `min(reported, sent_end, leader_last)` (`raft_server_append_acknowledged_through`,
  `src/server_h.rs:276-288`), so a reply paired with the wrong request of the
  same follower can never raise match above what the reply itself proves. The
  spec's AppendResponse is `{term, success, match_index, follower, read_ctx}`
  (glr/`src/protocol/Raft/types.rs:51`; `read_ctx` is 0 without reads): the first three are read from the
  reply and `follower` is the transport-supplied sender. The leader's
  `sent_term` check and its `CONTRADICTORY` check (`src/server_cc.rs:1453-1469`)
  are extra filters, and a refused reply counts as a drop. (`SentAppend` itself
  is rebuilt at reply time from the slot fields stored at send time,
  `src/server_cc.rs:1538-1575`.)
- **Vote replies counted by number.** The tally counts replies with no voter id
  (`rt/src/transport.rs:555-577`, `:625-644`), so a duplicated reply counts
  twice. Fixed in Phase 1. The snapshot reply is paired through
  `SnapshotReplyCtx` (`server.cc:1302-1356`); out of scope under the snapshot gate.
- **Codec fidelity.** The C++ batch codec must carry the per-entry
  (term, command) list unchanged. Send side: `raft_stamped_commit`
  (`src/server_cc.rs:999`), a deep copy with the term set
  (`server.cc:1450-1460`). Receive side: `raft_batch_term_at` (declared at
  `src/server_h.rs:1498`) and `raft_command_from_bytes` (`rt/src/service.rs:43`).

**So "RPC correct" should mean exactly this:**

> Every message the shell hands to the core (request or reply) was produced by
> the claimed sender's core for this destination, and its `msg_view` fields are
> unchanged. All nodes share one static voter set. The shell's send side
> encodes exactly the entries and terms the core emitted in that call's
> `Output`. The shell may lose, duplicate, delay or reorder messages.

| What | Status after the plan |
|---|---|
| Genuine packets, truthful sender, routing (including reply attribution to the slot's follower) | **Trusted** (the method's own assumption; part of every Raft proof) |
| Forged unavailable vote reply | **Removed** in Phase 1: the shell answers with an RPC error instead |
| Rest of reply pairing | **Not needed** for the spec view (success replies are self-describing). Phase 5 is an optional defensive echo. The vote tally becomes a set of voter ids in Phase 1. |
| Sender is a configured voter | **Checked** by `step_checked` (Phase 6) |
| Encode/decode fidelity | **Trusted, but tested**: a round-trip property test (Phase 5 or 6) plus the replay oracle (§2). Not proved. |
| Timers, clock, randomness, srpc retries and reconnect replay (`src/srpc/rpc/client.rs:1544`) | **Need nothing.** None of them carries a safety obligation. |
| Liveness | **Not proved** by the method. Covered only by the CI suites. |

---

## 2. Concepts

**Spec / spec action.** The group's atomic Raft spec, `LNextAtomic`
(defined at glr/`src/protocol/Raft/raft_refinement.rs:35`; the actions are in
glr/`src/protocol/Raft/raft.rs`; table in glr/`docs/ghost-log/spec/raft-spec.md` §1).
It is a list of single-server steps such as LTimeout, LGrantVote,
LFollowerAppendEntries and LAdvanceCommitIndex. Each step has a guard and an
update over a small abstract state: term, role, vote, log, commit index,
`votes_granted`, `match_index`, config. It also lists the messages the step sends.

**Ghost log.** A list that exists only in proof code and is erased at compile
time (glr/`src/protocol/Raft/ghost_log.rs:178-195`). Its entries are:
- `Recv(src, msg)` or `Tick`: what started this call;
- `Set(field, value)`: each write to a tracked field;
- `Send(dst, msg)`: each message queued;
- `Close(label)`: one spec action is finished.

**Segment.** The entries between two `Close`s. One call may close several
segments. For example, a vote request with a higher term closes LStepDown and
then LGrantVote. Only closed segments must match the spec; every function
carries one invariant, `inv`, that it must preserve
(glr/`docs/ghost-log/method/design.md` §3, `:104-117`).

**Refinement.** At every `Close(label)`, Verus proves that the writes and sends
since the previous close are exactly what the named action allows: its guard,
its update and its sent messages (glr/`src/ports/raftrs/coupling.rs:2399-2431`,
`close_seg`). Refinement is proved over the log, not function by function, so
our handlers need not line up one-to-one with spec actions.

**Coupling, alpha, msg_view.** `alpha` (also called `state_view`) reads the
core's concrete fields as the spec state. `msg_view` reads a wire message as a
spec message. "Coupled" means `alpha(state) == replay(log)`, and that is part of
the invariant (glr/`docs/ghost-log/raftrs/coupling.md` §1-§2). Fields that alpha
does not read are **unmarked**. Writes to them are invisible to the proof, so
they count as stutters (steps that change nothing in the spec).

**Gate.** A configuration check that keeps a feature the spec does not model
unreachable, so the proof may ignore it. It is checked at start-up, or at each
message as `step_checked`, which refuses out-of-gate messages; a refused message
counts as dropped (glr/`src/ports/raftrs/raw_node.rs:527`, `:634-656`). Example:
snapshots off.

**B-list.** The group's list of boundary conditions (glr/`docs/ghost-log/raftrs/coupling.md` §5,
B1-B35). Each entry is something the code may see but the spec does not admit,
and each one is handled by a gate, a runtime check or a view choice.

**Spec widening.** Changing the spec so that it admits something our code does.
Only the group can do this, and each widening forces them to re-run their safety
proof.

**RawNode / sans-I/O core.** raft-rs's shape: a library object with no sockets,
threads or clocks. You feed it events and read back outputs
(glr/`src/ports/raftrs/raw_node.rs:634-1157`). "Sans-I/O" means the same thing:
the core only computes, and the shell does all I/O.

**Event, handler, action.**
- An *event* is one call into the core, such as `RecvAppendEntries`, `TickHeartbeat`
  or `Propose`.
- The *handler* is the core function that processes it. A handler runs to
  completion and never suspends.
- An *action* is an output the handler returns for the shell to carry out:
  `Send(peer, msg)`, `ApplyRange(i, j)`, `LeaderChanged(bool)`,
  `ResetElection`, `WakeReplication`.

**Suspension point.** A place where a fiber gives up the poll thread mid-action:
`raft_fiber_sleep_us`, or an event wait. Today they are the phase-2 sleep
(`src/server_cc.rs:1676`), the vote wait (`rt/src/seam.rs:264-301`, the
`Fiber::sleep(200)` poll), `await_vote_settled` (`src/server_h.rs:4917-4930`,
sleep at `:4925`) and the heartbeat wait (`src/server_h.rs:3648`).

**Single owner / serialized calls.** The per-function invariant above
(design.md §3, `:104-117`) only means something if no other thread writes the
core's fields while a call is running. The group got this for free: raft-rs is
single-threaded and processes one message at a time, and "multi-threaded
instruction-level interleaving remain[s] deferred" (glr/`docs/ghost-log/method/design.md` §8,
`:229-233`). Their stated host obligation is only "the host calling the
verified entry points as documented" (glr/`docs/ghost-log/README.md:106`).
**No document of theirs states a lock-serialization host contract**; that is
why coordination question 1 (§4) exists. What we need is: core calls are
serialized, run to completion, do not re-enter, and reach an object only
through `&mut`. Two ways to get this:
1. a lock held for each **whole** core call, with the core reachable only
   through that lock guard;
2. only the poll thread ever calls the core.

Either way the verified core just sees `&mut self`. The serialization is a
trusted host property. Option 1 is cheaper. Whether the group accepts option 1
in their host contract is **not verified**; their docs only discuss a
single-threaded host.

**Shell.** Everything outside the verified core. It is trusted, not verified.

**Atomic mirror.** A copy of one core field (`is_leader`, leader hint,
`commit_index`, applied index) that the shell writes after each core call.
Other threads read the mirror, never the core.

**Regression gate.** A fixed perf measurement that must stay within a bound
of the parent commit's build before a phase can merge, and, at milestones, of
the Phase 0 baseline build (§6). Its pass rule combines a size bound with a
significance test (§6, "Pass rule").

**Differential test / replay oracle.** A check that a refactor did not change
behaviour. The group's version runs the original and the ported raft-rs on the
same seeded inputs and compares traces byte for byte (112/112 scenarios,
glr/`docs/ghost-log/README.md:173` and glr/`docs/ghost-log/spec/raft-spec.md:374`;
port-audit.md §8 reports the earlier 80/80 and 32/32 runs). **Our lab cannot do
that today.** It runs on a live reactor with wall-clock fibers
(`rt/src/lab_runtime.rs:17-31`; the harness sleeps through `raft_fiber_sleep_us`,
`src/lab.rs:76-78`, `:168`), injects faults with unseeded libc `rand()`
(`src/lab.rs:65-67`, `:566-572`), and randomizes election timeouts from the
real clock. Before Phase 3 the code also has no event boundary at which recorded
events could be fed back in. So the oracle has two stages:
- *Before Phase 3: comparison modulo timing.* Per lab case, the same sequence of
  leaders and terms and the same committed logs, plus Tier 1 (§6). Not
  byte-identical.
- *From Phase 3 on: record and replay.* A recorder at the new `step()` boundary
  (in lab and perf runs) writes each node's inbound events (with the `now` and
  `timeout_sample` parameters Phase 2 introduced) and its `Output`s. The next
  phase's core replays the recorded events deterministically and its `Output`s
  must be byte-identical.

**Glossary of labels used below.** Labels from the group's documents, defined
here so that the phases can name them:
- *BR1 / BR2* (glr/`docs/ghost-log/raftrs/coupling.md` §2.2, `:109`, `:118`):
  BR1 reads one wire append carrying k entries as k single-entry spec messages;
  BR2 proves the follower's two writes (log, commit) decompose into those k
  steps. *BR3* is raft-rs's `batch_append` merge of new entries into a queued
  append; it is outside their gate (B30, coupling.md:354).
- *A-numbers* (A0-A10): the group's work items, in order. A5: panic freedom
  (prove every `assert!` cannot fire). A7: spec simplifications. A8: ReadIndex
  reads. A9: membership; also the spec clause that an append carrying entries
  carries the leader's exact commit index (glr/`src/protocol/Raft/raft.rs:363-368`).
  A10: snapshots.
- *B-numbers* (coupling.md §5): individual boundary conditions (see "B-list").
  Those named here: B4 (a success reply may not claim more than the leader's
  log), B16 (entry terms are positive), B17 (a reject's match index is viewed as
  0), B19 (a candidate that sees a same-term append steps aside first), B20 (a
  modelled message comes from a configured voter), B23 (raft-rs's
  `become_leader` asserts `last_index == persisted`, plus its Ready ordering
  contract).
- *T1-T9* (glr/`docs/ghost-log/raftrs/port-audit.md:45-55`): the labelled,
  behaviour-preserving rewrites they allowed when porting raft-rs into the
  Verus subset. Those used here: T1 logging becomes no-ops, T3 iterator chains
  become loops, T7 collection type swaps.
- *V3*: raft-rs's view choice that `next_index` is a shadow field, not part of
  the spec state.
- *LStepDown / LStepAside*: spec actions (glr/`src/protocol/Raft/raft.rs:223-250`).
  LStepDown adopts a higher term and becomes follower. LStepAside is guard-free:
  any node may become follower in its current term, clearing its vote set (it
  replaced the older LLoseElection; glr/`docs/ghost-log/spec/raft-spec.md:112-113`).
- *Diff-1 ledger*: their list of every difference between upstream raft-rs and
  the port. *Diff-2 lint*: their check that a proof-only commit changes no
  executable code (port-audit.md:684-707).
- *Fiber*: an srpc coroutine that runs on the poll thread and can give it up
  with a sleep or wait.
- *AuthorityLedger / HeartbeatAuthority*: the leader's record of which
  followers acknowledged which heartbeat round, for read-index authority
  (`src/server_cc.rs:130-470`). *ReplicationWakeGate*: the flag-and-wake object
  the submit path uses to wake the heartbeat fiber (`src/server_h.rs:1143-1405`).
- *The R4 lesson*: a fix in the shared srpc subtree was lost on a subtree sync
  and had to be re-applied (`docs/performance/raft-latency-regression.md` §5).
- *CV*: coefficient of variation, standard deviation over mean. *MDE*:
  minimum detectable effect, the smallest change a given number of rounds can
  resolve. *Sign test*: counts rounds where arm B beat arm A and asks how
  likely that split is by chance (`scripts/raft_perf/paired_stats.py:36-41`).
- *Knee*: the client count N at which latency starts rising steeply.
  *Jetpack*: the closed-loop client sweep in
  `scripts/raft_perf/jetpack/run_jetpack_sweep.sh`
  (`docs/performance/jetpack-comparison/README.md`).
- *Tier 1-4, G1-G6*: the correctness suites and the perf gate points, defined in
  §6. Each phase below names them.

---

## 3. Target architecture

### 3.1 Verified core vs trusted shell

**The core** (new crate `src/deptran/raft/core/`, built into the `raft_rust`
cargo build; Phase 6) holds:

- the fields of `RaftConsensusState` (`src/server_h.rs:918-1014`): term, vote,
  role, leader hint, log, commit, execute index, peers' next and match;
- config members and the self id, fixed at Setup;
- `stopped`;
- `pending_leader_term`;
- the protocol half of today's `PendingTable` (follower, sent_term, sent_round
  and sent_end per slot); the `RaftResponsePtr` handles stay in the shell,
  indexed by slot ordinal;
- the heartbeat round and `AuthorityLedger` (both unmarked);
- a `VoteSet` of voter ids, keyed by campaign term (fed one
  `RecvVoteResp` event per reply from Phase 3; until then the tally stays in
  the shell, see Phase 1);
- `applied_index`, as input.

It contains no `unsafe`, `extern`, raw pointers, locks, atomics, hashing,
logging or clock reads.

**The shell** holds: srpc, `rt/src/seam.rs`, `rt/src/transport.rs`,
`rt/src/service.rs`, `rt/src/snapshot.rs`, the C++ codec and kernels in
`server.cc`, `raft_worker.cc`, `raft_main_helper.cc`, the fibers and timers, the
apply thread, and `ReplicationWakeGate`.

Rules:

1. **Serialized access.** `&mut RaftCore` can be reached only through
   `with_core(|c| ...)`. Each guard covers exactly one core call, and the shell
   carries out the returned actions *after* releasing it. In Phases 1-5 that
   guard is today's `mtx_`. Phase 7 optionally moves ownership onto the poll
   thread (§5).

   Consequence: **the shell never reads core state after a call.** Everything
   the shell needs in order to act is copied into that call's `Output` while
   the guard is held. In particular, an `Entries` or `ApplyRange` action
   carries the k entry handles and their terms (handle clones are refcount
   bumps). Otherwise, between the call and the shell's encode, another call
   (for example a step-down followed by accepting a different leader's
   AppendEntries, `src/server_h.rs:5434-5441`) could truncate or overwrite
   those slots, and the shell would encode entries that differ from the `Send`
   the core logged. That message is not genuine, which breaks assumption 1 of
   §1.3. Today this cannot happen, because the batch is built inside the same
   lock section that chose prev (`src/server_cc.rs:1117-1222`, selection and
   stamping at `:948-1060`, `raft_stamped_commit` at `:999`); the plan must
   keep that property, not lose it.
2. **No suspension inside a call.** Every wait becomes a timer in the shell plus
   an event: `RoundEnd`, `VoteDeadline`.
3. **Payloads are handles.** `Cmd` is an `#[verifier::external_body]` opaque
   type wrapping `RaftCommand`. Its spec view feeds the spec's uninterpreted
   `value_view` (glr/`src/ports/raftrs/alpha.rs:27`). Size, kind and
   `has_value` are cached in `RaftEntry` at append time, so the core never
   calls FFI per entry. An inbound append's payload enters the core as a second
   opaque type, `WireBatch` (`external_body`), with one trusted accessor
   `materialize(i) -> Cmd` and one `term_at(i)`; the core calls them inside the
   handler, only after its checks pass (§3.2, follower flow).
4. **Outputs are data.** An `Output` struct reuses its `Vec`s across calls, so
   there is no allocation per event.
5. **Batching uses the verified form.** One wire append carrying k entries is
   read as k single-entry spec messages (BR1/BR2, glr/`docs/ghost-log/raftrs/coupling.md`
   §2.2; see the §2 glossary), with Mako's caps of 256 entries and 16 MiB.
   raft-rs's BR3 `batch_append` merge has no Mako counterpart and is never used.
6. **Cheap entry checks.** `step_checked` refuses messages that fall outside the
   gate, using integer compares only.

### 3.2 Data flow

- **Submit.**
  1. `add_log_to_nc` calls `RaftWorker::Submit` (`raft_worker.cc:854`).
  2. The shell clones the command handle outside the lock.
  3. It calls `with_core(|c| c.propose(cmd))`. The core checks the role, appends
     at `current_term` and returns `WakeReplication`.
  4. The shell wakes the heartbeat fiber through the existing gate.

  This stays on the submit or app thread through Phase 6, as today
  (`src/server_h.rs:4663-4697`). The index and term the core returns are
  discarded by the only caller (`raft_worker.cc:853-858`), so `Start` can keep
  its signature. Its accept/reject result is **not** discarded: `Submit`
  returns early on `REJECTED` and otherwise increments `n_tot`
  (`raft_worker.cc:856-862`), and `get_outstanding_logs` reports
  `n_tot - CommitIndex()` (`raft_main_helper.cc:977-990`). So `propose` returns
  accept or reject synchronously, as today.
- **Replication.**
  1. `TickHeartbeat` first runs today's phase 0, which may advance commit
     (`src/server_cc.rs:763`); if so it returns `ApplyRange` (one
     LAdvanceCommitIndex segment). It then returns, for each follower, one of
     `Heartbeat{prev, prev_term, commit}`,
     `Entries{prev, prev_term, [(term, Cmd)]×k, commit}` or `Skip`. The
     per-entry byte cap is applied inside the call, as today. `Snapshot` cannot
     be reached under the gate. `commit` is the core's `commit_index` read in
     the same call that builds the action, not a snapshot from an earlier lock
     section (see Phase 1, "Phase-1 check under the lock").
  2. After releasing the guard, the shell stamps (`raft_stamped_commit`, a deep
     copy with the term set, `server.cc:1450-1460`) and finalizes
     (`raft_batch_finalize`, `server.cc:1462-1472`) **from the handles in
     `Output`**, never from the log; sends; and stores the response handle in
     the slot. The stamped term the follower reads (`raft_batch_term_at`) is
     written here, so this step is part of the "codec fidelity" trust in §1.3.
     The deep copy moves out of the lock, which shortens the critical section;
     this is gated on G2, G4 and G6.
- **Reply.** For each completed slot, the shell calls
  `RecvAppendResp(slot, Option<AppendReply>)`. `None` means unavailable; today
  the code uses the 0/0/0 sentinel, `src/server_cc.rs:1563-1565`.
- **Commit.** `RoundEnd` runs today's `heartbeat_phase3_locked`
  (`src/server_cc.rs:1725-1747`): `raft_commit_advance` plus the ledger settle.
  It returns `ApplyRange(i, j)`.
- **Apply.**
  1. `ApplyRange` already carries the per-entry handle clones, taken inside
     the call (rule 1). The shell pushes them to the apply queue, as
     `EnqueueCommittedEntries` does today (`src/server_h.rs:3837-3905`).
  2. The apply thread pops one entry per iteration and runs the app callback
     (`src/server_h.rs:3335-3355`).
  3. It reports `Applied(n)` once per entry, as today (`:3429`). Batching that
     report needs a restructured apply loop; that is Phase 7's blocking apply
     channel, not Phase 4.
- **Follower AppendEntries.** One event,
  `RecvAppendEntries{term, from, prev, prev_term, commit, payload: WireBatch}`.
  Inside that one call the core:
  1. runs the gates (stopped, non-voter, unauthoritative sender);
  2. reads the term list through `payload.term_at(i)` and validates the count;
  3. finds the first conflict, truncates, and calls `payload.materialize(i)`
     for each entry it appends;
  4. advances commit and returns `ApplyRange` plus the reply.

  Today truncate, append, commit advance and `EnqueueCommittedEntries` all
  happen in one critical section (`src/server_h.rs:5420-5460`), and the method
  needs that too: the log (including each entry's `value_view`) is a marked
  field that the spec writes in the same LFollowerAppendEntries step that sets
  commit and sends the success reply (glr/`src/protocol/Raft/raft.rs:489-519`).
  Coupling requires `alpha(state) == replay(log)` at every call exit
  (glr/`docs/ghost-log/method/design.md:108-111`), and a write to a marked
  field that no action owns is a hard blocker
  (glr/`docs/ghost-log/raftrs/coupling.md:6-8`). So commands cannot be filled in
  by the shell after the call. This order keeps today's rule that the payload
  is decoded only after the stopped/unauthoritative gates
  (`src/server_h.rs:5272-5280`): rejects pay no decode. Materializing an entry
  is one Arc clone (`raft_batch_command_into`, `server.cc:1546-1551`).

  Rejected alternatives: (a) materialize every `Cmd` before the call (costs the
  decode on reject paths, which the code avoids on purpose; would need a G2/G4
  check); (c) two calls with an open segment carried across them
  (glr/`docs/ghost-log/method/failure-patterns.md:64`, Pattern 1), which needs a proof that no
  observable point lies in between.
- **Election.**
  1. `TickElection(now, timeout_sample)`: the core increments the term, votes
     for itself and returns `Send RequestVote` to every peer.
  2. Each reply arrives as `RecvVoteResp(from, term, granted)` and updates the
     voter set. The transport's vote callback does not touch the server today:
     it locks a private `Arc<Mutex<TallyState>>` (`rt/src/transport.rs:563-577`,
     `:625-644`). It keeps that shape: it records `(from, term, granted)` in
     shell-side state, and the polling fiber, which already wakes every 200 µs,
     hands each recorded reply to the core.
  3. On quorum, the core runs `become_leader`, which returns `AppendNoop`,
     `LeaderChanged(true)` and `WakeReplication`.
  4. `VoteDeadline(term)` ends a campaign that did not reach quorum.

  Through Phase 6 the shell keeps today's 200 µs poll and 1 s wait, so the
  timing is unchanged.

---

## 4. Spec gap and gating

| Mako behaviour | Handling | Who decides |
|---|---|---|
| Election, vote, step-down, AppendEntries accept, append reply, commit rule, leader no-op | Covered with no spec change. For the commit rule, `verus/commit_rule.rs` is evidence and a proof skeleton, not a discharged obligation: it is a standalone reduced model (a `Vec<u64>` of match indexes, a `Vec` of terms, panics turned into preconditions; `verus/commit_rule.rs:1-20`) that proves the paper's Figure-2 conditions. It is not a proof against the group's LAdvanceCommitIndex / `commit_quorum_ok` over rank-keyed `match_index` maps, and it does not cover the production function. Phase 8 re-proves it on the core's real types against the group's guard. | — |
| Vote request with a higher term | Two segments: LStepDown, then LGrantVote or LRejectVote (`src/server_h.rs:3199-3258`) | us (coupling) |
| Candidate receiving a same-term AppendEntries | LStepAside, then accept (raft-rs B19) | us |
| Election win | k × LReceiveVoteGranted, then LBecomeLeader, then LClientRequest for the no-op | us |
| Extra AppendEntries rejects that change no state: non-voter, unauthoritative sender, bad payload, stopped (`src/server_h.rs:5240-5349`) | **Widen**: make LRejectAppendEntries unguarded (s' = s, success false, match 0), the append analogue of their B1. **Fallback**: refuse those messages at `step_checked` (no reply). That only affects liveness, and would need a perf check. Not verified against their packet invariants. | **verification group** |
| Refused committed conflict: the first conflicting entry is at or below max(commit, execute) (`src/server_h.rs:5413-5429`). For that message `prev_log_ok` and `append_pos_ok` can both hold and the term is current, so none of LRejectAppendEntries' four disjuncts (glr/`src/protocol/Raft/raft.rs:607-614`) holds, and a per-node proof cannot rule the path out with global safety. raft-rs avoided this with its `m.index < committed` shortcut (LFollowerStaleAppend) plus a per-node unreachability proof (coupling.md B7). | Same decision as the row above: **widen** (unguarded LRejectAppendEntries covers it) or **fallback** to `step_checked`. A `step_checked` test for it must read the follower's state and log (find the first conflict), as raft-rs's `valid_incoming` does for B4/B22; it is not an integer compare. Third option: add an LFollowerStaleAppend-style path and prove the conflict unreachable, as raft-rs did. | **verification group** |
| Entry-less append at prev 0 carrying the full commit (`src/server_cc.rs:1236`) | View only: read `leader_commit` as `min(lc, prev+count)`, which is all the follower uses (`src/server_h.rs:5449`) | us; tell the group |
| Phase-1 leader check outside the lock that builds the message (`src/server_cc.rs:1101` vs `:1117`); the message's commit is phase 0's snapshot (`:764`, sent at `:1236`) | **Code fix** (Phase 1): re-check role and term inside the locked block, and read commit there too. LSendAppendEntries requires `leader_commit <= s.commit_index` and, when entries are carried, `leader_commit == s.commit_index` (A9; glr/`src/protocol/Raft/raft.rs:363-368`) | us |
| Log slot without a command (`src/server_h.rs:5385-5410`) | View invariant: every slot in [base, last] has a command | us |
| `failover_ == false` (`src/server_h.rs:2313`) | Gate on `failover_ == true` | us |
| Unavailable branch of `ServeVote` forging a reply (`src/server_h.rs:4704-4716`; taken when `IsDisconnected() \|\| !IsRpcReady()`; `Disconnect` is at `:3767`) | Not test-only: `rpc_ready_` is false in production windows (§1.3). **Code fix** (Phase 1): answer with an RPC error code, which the Rust vote callback already treats as a drop (`rt/src/transport.rs:569-571`). `ServeAppendEntries`' 0/0/0 branch already counts as a drop and needs no change. | us |
| Read-index authority ledger (`src/server_cc.rs:130-470`) | Unmarked. A grep found no reader of `read_quorum_confirmed*` outside `server_h.rs` and `server_cc.rs`. A8 ReadIndex only once a read API exists. | us |
| Snapshots, compaction, InstallSnapshot | **Gate first.** With `MAKO_RAFT_SNAPSHOTS` unset, `InitializeSnapshotManagerLocked` returns before building a manager and fail-stops on uncovered progress (`src/server_h.rs:2607-2625`, **checked**). snapidx stays 0, compaction is clamped to nothing (`src/server_h.rs:2096`, `:359`), base stays 1, and the leader's snapshot branch (`src/server_cc.rs:1158`) cannot be reached. This is also the perf-sweep setting. **Later** with the group's A10. | joint |
| Restart from persisted state | **Gate**: "a restarted process is a new node". Raft is memory-only (`src/server_h.rs:2219-2221`), so a replica that restarts and rejoins under its old id could vote twice in a term. That is a possible **real safety gap**, not just a proof limit. Whether production ever restarts a replica is not verified. | **user** |
| Membership change | Not applicable (static, fixed in `SetupInternal`, `src/server_h.rs:2479-2540`) | — |
| Lease reads | Permanently out of the method's scope; Mako does not use them | — |

**Coordinating with the group** (send at the start of Phase 0; settle before Phase 8):
1. Do they accept lock-serialized, run-to-completion calls as the host's
   serialization guarantee? A "no" moves Phase 7 (poll-thread ownership) ahead
   of Phase 8.
2. Do they accept an unguarded LRejectAppendEntries?
3. Do they accept the prev-0 heartbeat view?
4. Freeze one spec version per milestone.
5. Timeline for A10 (snapshots). Restart is not modelled and not scheduled
   (glr/`docs/ghost-log/spec/raft-spec.md:30`, `:392-397`;
   glr/`docs/ghost-log/raftrs/coupling.md` §7.5): would they design the
   Restart action and the persistence-boundary invariant?
6. Would they accept the refused-committed-conflict path (the row above) under
   the widening, or do they want the raft-rs-style unreachability proof?

---

## 5. Phased plan

Every phase must pass the CI gate and the perf gate in §6 before it merges, and
each one can ship and be measured on its own. Through Phase 6 the timer
constants stay frozen: the 1 ms phase-2 poll, the min(100 ms, heartbeat) round
cap, the 200 µs vote poll, the 1 s vote deadline, the 1 ms apply idle sleep, and
the election timeouts.

### Phase 0. Baseline, harnesses, spikes (about 1 week, mostly machine time)
**Goal.** Make the gates meaningful, and settle the build questions that could
block later phases.

**Changes:**
- Fresh baselines at HEAD `76ea6cb46`: G1-G6, one full `run_sweep.sh`, and the
  Jetpack suite. The existing records are `9a361eccd-dirty`, from another
  worktree.
- Teach `scripts/raft_perf/rotation_trial.sh` (and `paired_trial.sh`) to select
  multi-group mode for G3 and G4. Today `rotation_trial.sh`'s `run()` passes no
  group-mode flag (`:28-33`), but `examples/raft_bench.sh` already accepts
  `--group-mode single|multi` (`:73`, `:111`). So the change is one env var,
  `GROUP="${GROUP:-single}"`, forwarded as `--group-mode "$GROUP"`.
- **Election-time harness** (for gate G7, §6): a lab case or `raft_bench` mode
  that stalls or kills the leader N times and records the time from leader loss
  to a new leader and to the first commit after it. Today nothing measures
  this: `raft_bench` only waits up to `LEADER_WAIT_SEC` for the first leader
  (`examples/raft_bench.sh:39`, `:109`), and `shardFaultTolerance` is disabled
  (`ci/ci.sh:820`).
- Commit the 12-stage latency-trace kit behind `MAKO_RAFT_TRACE_FILE`; it is
  inert when unset. Today it lives only on zoo-003. Re-anchor its stages 3, 4,
  7 and 8, which sit in the heartbeat fiber.
- A **trace comparator modulo timing** for the lab: per lab case, the sequence
  of (term, leader) changes and the committed log per node. This is the
  pre-Phase-3 oracle (§2, "Differential test / replay oracle"). The
  record-and-replay oracle can only be built at Phase 3, when a `step()`
  boundary exists.
- **Verus build spike.** Put `PeerTable::majority_match_index`
  (`src/server_h.rs:881`) in a small `verus!` crate and:
  - (a) build it in the `raft_rust` cargo lane with ghost code erased;
  - (b) see what rusty-cpp does with it, since the cpp and hybrid lanes
    transpile the same sources;
  - (c) match the Release opt-level and LTO.

  If (a) fails, the fallback is a build step that runs `verus --compile` or
  emits erased Rust.
- Trace the leader-change callback (`raft_worker.cc:311-330` →
  `raft_main_helper.cc:651-726`) for re-entry into Raft.

**Verification artifacts.** None yet.

**Correctness.** Tier 1. The comparator must report "equal" when the same
build runs each lab case twice (if a case is not stable even then, mark it
exempt and say why).

**Gate.** Baselines exist. Each point has a recorded paired CV and MDE, and
from them its round count and bound (§6, "Deriving rounds and bounds"). The
Phase 0 commit is recorded as the cumulative baseline (§6).

**Risks.** The spike fails (see the fallback above).

### Phase 1. One `RaftCore` struct; spec-alignment fixes (about 1.5 weeks)
**Goal.** Every field that survives between segments lives in one struct alpha
can read. Also remove the places where the code itself, not just the proof,
disagrees with the spec.

**Changes:**
- Define `pub struct RaftCore` in `src/server_h.rs` holding the fields listed in
  §3.1. Move the heartbeat round state out of `HeartbeatRoundState`
  (`src/server_cc.rs:1805-1831`) into it. The vote tally stays in the shell's
  `TallyState` in this phase (`rt/src/transport.rs:625-644`); it moves into the
  core in Phase 3 as one `RecvVoteResp` event per reply (§3.2, election).
- `RaftServerBase` holds `core: RaftCore` in place of `state_`.
- The functions that really are pure take `&mut RaftCore` (or `&RaftCore`),
  with unchanged bodies: `raft_commit_advance` (`src/server_cc.rs:633`),
  `heartbeat_apply_append_reply` (`:1397`), `heartbeat_phase0_locked` (`:710`),
  `heartbeat_phase3_locked` (`:1725`), and the ~40 const predicates at
  `src/server_h.rs:101-445`.
- `raft_on_request_vote` (`src/server_h.rs:4974`) and `raft_on_append_entries`
  (`:5214`) are **not** pure and keep `&mut RaftServerBase` until Phase 2 has
  pulled their effects out. Both are `pub unsafe fn`. The vote handler calls
  `doVote` (`:3199-3258`), which calls `stepDown`/`setIsLeader` (FFI
  `raft_log_set_is_leader_entry` at `:2284`, the leader-change callback,
  `AppendLeaderNoop`), `resetTimerLocked` (`raft_time_now_us` at `:2253`) and
  `LogTermChange`. The append handler calls `AeDecodePayload`/`AeApplyIncoming`
  (FFI `raft_wire_is_batch`, `raft_batch_term_at`, `raft_batch_command_into`,
  `:4263-4340`), `raft_command_has_value` (`:5393`), `stepDown`/`setIsLeader`,
  `resetTimerLocked` and `EnqueueCommittedEntries` (`:5458`). They are retyped
  in Phase 3, when they become `core.on_request_vote` / `core.on_append_entries`.
- **Vote tally keyed by voter id** (still in the shell's `TallyState`). It
  counts distinct voters for the campaign term only, so a duplicated reply has
  no effect. The quorum rule is unchanged. The callback already knows which
  peer it was sent to (`rt/src/transport.rs:559`, the `_site` of the loop), so
  it captures that id; it still touches no server state and takes no `mtx_`.
  Rust lane only; the C++ lane's `RaftVoteQuorumEvent` is not touched.
- **No forged vote replies.** In `ServeVote`'s unavailable branch
  (`src/server_h.rs:4707-4713`), answer with an RPC error code rather than
  `reply_term = can_term, vote_granted = 0`. The Rust vote callback already
  feeds nothing on `code != 0` (`rt/src/transport.rs:569-571`), so the reply
  counts as a drop. **Not verified**: how the service layer
  (`rt/src/service.rs`) returns a non-zero code for a handled request; if it
  cannot, the alternative is to route the branch through the core's reject so
  the reply carries the responder's real current term.
- **Phase-1 check under the lock.** Inside the block at `src/server_cc.rs:1117`,
  add `if !is_leader_ || current_term_ != round.term() { break }`, and send the
  `commit_index_` read inside that same block rather than phase 0's
  `round.commit_index()` snapshot (`:764`, used at `:1236`). Reason: the spec's
  LSendAppendEntries requires `leader_commit <= s.commit_index` and, for an
  append carrying entries, `leader_commit == s.commit_index` (A9;
  glr/`src/protocol/Raft/raft.rs:363-368`). Today no other path advances a
  leader's commit within the same term between phase 0 and phase 1, so the two
  values agree in practice; Phase 7 (replies as events, `RoundEnd` at quorum)
  and Phase 9 pipelining would break that, so the code should not rely on it.
  (Alternative: keep the snapshot and add a commit-equality check to the
  recheck.)
- **Entry-term check (B16).** `AeDecodePayload` (`src/server_h.rs:4263-4288`)
  pushes `raft_batch_term_at` for every entry with no term check and validates
  only the count's index space (`raft_server_append_batch_count_is_valid`); the
  single-entry path pushes `leader_next_log_term` unchecked. So term-0 entries
  are **not** rejected today (checked). Add: reject any entry with term 0.
- **Gate assertion.** At the end of `SetupInternal`, add `verified_config_ok()`:
  with snapshots off, snapidx == 0 and base == 1; `failover_` is true; the config
  is static and contains self. It logs in a normal build and fail-stops in a
  gated build.
- Replace `rusty::BTreeSet` in HeartbeatAuthority with a sorted `Vec<u16>`
  (`src/server_cc.rs:130-250`).

**Verification artifacts.** None. This phase is the precondition for alpha.

**Correctness.**
- Tier 1.
- Comparison modulo timing (§2) on every lab case, except those this phase
  changes on purpose; list them in the commit with the reason: cases with
  duplicated vote replies (dedup), any case that disconnects a server while a
  candidate is campaigning (the unavailable vote reply becomes a drop), and any
  case that injects term-0 entries.
- New lab cases: a duplicated vote reply, a stale-term vote reply, and a vote
  request to a server whose `rpc_ready_` is false.

**Gate.** G1, G3 and G5, each at the round count Phase 0 derived for it (§6);
the 10-15-round shortcut applies only to points whose paired MDE at that count
is below the bound. G7 (election time), because the vote path changes.
Expected neutral: one compare per follower per round, and a set on the election
path only.

**Risks.** Turning the unavailable vote reply into a drop changes candidate
timing when a peer is not RPC-ready: a reject arrives as "no answer", so the
candidate waits for its deadline instead of counting a no. G7 and the
`raftLabTest` election cases watch this.

### Phase 2. Side effects become returned actions (1.5-2 weeks)
**Goal.** Core functions make no FFI calls, take no locks, read no clocks and
fire no callbacks.

**Changes:**

| Today | Becomes |
|---|---|
| `EnqueueCommittedEntries` (`src/server_h.rs:3837-3905`) | `ApplyRange{from, to, handles}`: the core clones the handles inside the call (§3.1 rule 1); the shell pushes them |
| `setIsLeader` / `stepDown` (`:2281-2406`, `:4175-4194`) | pure `become_leader()` / `become_follower(hint)` returning `LeaderChanged`, `AppendNoop`, `ResetElection`, `WakeReplication`; the FFI `raft_log_set_is_leader_entry` (`:2284`) and the leader-change callback move to the shell |
| `resetTimerLocked` (`:2251-2271`), `GetElectionTimeout` (`:2223-2245`) | `now` and `timeout_sample` become event parameters supplied by the shell |
| `AppendLeaderNoop` (`:4224`) | the shell builds the noop handle; the core appends it |
| `RequestReplication` (`:3607`) | `WakeReplication` |
| per-entry `raft_command_has_value` / `payload_bytes` / `is_tpc_commit` / `kind` | `RaftEntry` fields set at append time (removes three FFI calls per entry per send) |
| `raft_verify` (~20 sites), explicit panics (7) | `assert!`, proved in Phase 8 |
| logging inside decisions | stays for now; turned into no-ops in Phase 6 (T1); the shell logs from `AppendReport` (`:5136-5200`) and `AppendReplyOutcome` |

The leader-change callback now fires after the lock is released, not while
`mtx_` is held. This removes a lock-order hazard; Phase 0 checks re-entry first.

**Verification artifacts.** As a dry run, lift `raft_commit_advance` and
`heartbeat_apply_append_reply` into the spike crate.

After this phase `raft_on_request_vote` and `raft_on_append_entries` have no
remaining FFI, clock or callback effects (the append payload is still read
through FFI until Phase 3 gives it the `WireBatch` type).

**Correctness.**
- Tier 1.
- Comparison modulo timing (§2) on every lab case.
- A test that the callback fires exactly once per transition.

**Gate.** G1-G6 at their Phase 0 round counts (at least 25), since the entry
metadata touches the hot path. Watch G2 and G6 for the larger `RaftEntry`. G7,
because timer reset and timeout sampling become event parameters.

**Risks.** Memory per entry grows by a few bytes.

### Phase 3. Cut the fibers at their suspension points (2-3 weeks; milestone 1)
**Goal.** No core call contains a sleep or wait. Timing stays identical.

**Changes:**
- **Heartbeat** (`src/server_cc.rs:798-1791`):
  - phases 0 and 1 become `tick_heartbeat`;
  - each completed slot becomes `on_append_resp(ord, Option<AppendReply>)`, with
    the step-down folded in (today `:1591`);
  - phase 3 becomes `round_end`.
  - The shell keeps the phase-2 loop, its 1 ms step (`:1505`), its deadline
    (computed at `:1508-1517`, checked at `:1664-1667`) and its early-quorum
    exit (`:1658-1662`), and only calls the core.
  - `tick_heartbeat` returns each follower's entries as handles (§3.1 rule 1),
    so the batch is still built from exactly what the core chose.
  - Keep the follower order and the place where `pending_rpcs.place` happens.
- **Election:**
  - `RequestVoteImpl` section 1 (`src/server_h.rs:3947-4005`) becomes
    `start_election`;
  - each vote reply becomes `on_vote_resp(from, term, granted)`, which updates
    the set and raises a `decided` flag. The transport callback still records
    the reply in shell state without touching the server; the polling fiber
    hands each recorded reply to the core (§3.2, election);
  - section 2 (`:4049-4170`) becomes `election_settle`.
  - The shell keeps `raft_broadcast_vote_and_wait`'s 200 µs / 1 s loop
    (`rt/src/seam.rs:264-301`), polling `core.decided()`.
  - Make `election_settle` the only place that becomes leader, so behaviour
    matches today's. Acting the moment a reply arrives comes in Phase 7.
- **Inbound:** `ServeVote` and `ServeAppendEntries` call `core.on_request_vote`
  and `core.on_append_entries` (§3.2 follower flow). This is where those two
  handlers are retyped to the core (Phase 1 left them on `RaftServerBase`). The
  append payload enters as `WireBatch`, and the core materializes entries
  inside the call.
- **Replay recorder** at the new `step()` boundary, in lab and perf runs: each
  node's inbound events (with their `now` / `timeout_sample` parameters) and
  `Output`s (§2). From here on, each phase's core must reproduce the previous
  phase's recorded `Output`s byte for byte when replaying the same events.
- `ElectionTimerLoop` (`src/server_h.rs:4880-4911`) keeps its randomized wake and
  calls `core.tick_election`.

**Verification artifacts.** Every protocol transition is now one core call.
Write the path-by-path coupling table (the analogue of
glr/`docs/ghost-log/raftrs/coupling.md` §3). It becomes the work list for Phase 8.

**Correctness.**
- Tier 1.
- Comparison modulo timing (§2) against Phase 2 on every lab case. This phase
  cannot be replay-checked against Phase 2, because Phase 2 has no `step()`
  boundary to record at.
- New lab cases: a reply landing during `round_end`, and a vote reply after the
  deadline.

**Gate.** G1-G6 at their Phase 0 round counts, G7, then **milestone 1**: full
sweep plus Jetpack, against both the parent and the Phase 0 baseline (§6).
Expected neutral.

**Risks.** A subtle reordering inside phase 1. Only the modulo-timing
comparison and Tier 1 can catch it in this phase, and they are weaker than a
replay; review the per-follower order in the diff explicitly.

### Phase 4. Enforced serialization and atomic mirrors (1-1.5 weeks)
**Goal.** The method's concurrency premise holds by construction: each core call
is serialized, and nothing reads the core outside a call.

**Changes:**
- The only accessor to the core is `with_core`. Delete direct field access; the
  compiler finds every site.
- After each call the shell publishes the mirrors `commit_index`, `is_leader`,
  `leader_hint` and `term`. Then:
  - `CommitIndex()` (`src/server_h.rs:4655-4657`; an unlocked read today, a data
    race; called from `raft_main_helper.cc:989`) reads the mirror;
  - `IsLeader` and `GetLeaderHint` (`:4600-4609`; called from app threads at
    `raft_main_helper.cc:1166-1167`) read the mirrors and no longer take `mtx_`.
- `PublishAppliedIndex` (`src/server_h.rs:2089`) becomes
  `with_core(|c| c.on_applied(n))`, still **once per entry** as today: the
  apply thread pops one entry per iteration (`:3335-3355`), sleeps 1 ms when
  the queue is empty (`:3366-3372`) and publishes after each entry (`:3429`).
  Per-batch publication needs a new drain loop and would delay
  `appliedIndexForWait_` and `execute_index_`, which the follower's
  committed-conflict check reads (`:5413-5418`); that change belongs to Phase
  7's blocking apply channel. `appliedIndexForWait_` stays as a mirror. Its readers are all inside
  `server_h.rs` (a grep: `:2076`, `:2716`, `:2815`, `:3036`, `:3387`).
- `SetPreferredLeader` (`raft_main_helper.cc:889`, `:918`, `:1300`) writes a
  shell atomic, because only the shell's timeout choice reads it.
- Under the gate, `CompactLog` (`src/server_h.rs:2124`) and `MaybeCreateSnapshot`
  (`:3718`) do nothing.

**Verification artifacts.** Serialization premise established, assuming the
group accepts lock serialization.

**Correctness.**
- Tier 1, with first-attempt failures counted. This is the step where a
  threading bug would hide behind the retry (`ci/ci.sh:523`).
- A TSan build of the lab suite, if the toolchain allows it (**not verified**).

**Gate.** G1-G6 at their Phase 0 round counts. Expected neutral to better: app threads stop
contending on `mtx_`.

**Risks.** The group rejects lock serialization; then do Phase 7 before Phase 8.

### Phase 5 (optional, defensive). Reply echo (about 1 week)
**Goal.** Make each reply name the request it answers, as defence in depth.
This is **not needed** for the proof: success AppendEntries replies already
carry the end index they prove, the leader already takes
min(reported, sent_end, leader_last), and the spec's AppendResponse fields are
all readable from the reply plus the transport-supplied sender (§1.3). The only
pairing property the proof needs, that a reply is attributed to the follower
whose slot it completes, is part of "truthful sender/routing" and stays
trusted with or without this phase.

**Changes:**
- `AppendReply` (Rust-lane wire layout, `rt/src/rpc.rs`) gains
  `(follower, sent_term, sent_end)`, about 24 bytes. The leader refuses a reply
  whose echo does not match the slot (counts as a drop). The existing
  `sent_term` and `CONTRADICTORY` filters (`src/server_cc.rs:1453-1469`) stay as
  refusals.
- Snapshot reply: echo the last included index and add an explicit `ok`. Today
  term 0 means failed. (Only relevant once the snapshot gate is lifted.)
- Hybrid lane: either update both codecs, or keep the hybrid lane on the old
  layout. The cpp lane is not touched.

**Verification artifacts.** None required. It shrinks what a buggy srpc could
do without changing what the proof assumes.

**Correctness.**
- Tier 1, plus `raftLabTestHybrid` if the hybrid codec changes.
- An encode/decode round-trip property test.
- Replay against Phase 4's recording (§2): identical `Output`s.

**Gate.** G2, G4 and Jetpack loopback N=500, at their Phase 0 round counts;
these points are sensitive to per-message bytes.

**Risks.** A wire-format change; decide whether the hybrid lane must
interoperate.

**Note.** Skipping it keeps reply-to-slot attribution as a written trusted
property of srpc, which it is in any case. Whether srpc can ever fire a reply
callback twice, or cross-wire replies, is **not verified**.

### Phase 6. Separate core crate in the Verus subset (2-3 weeks; milestone 2)
**Goal.** The core compiles in the Verus subset with ghost code erased. This is
the code that gets verified, so there is no separate port and no diff-1 ledger.

**Changes:**
- Create `src/deptran/raft/core/` (log, election, replication, commit), exposing
  `step(&mut self, ev: Event, out: &mut Output)` and `new_gated(cfg) -> Result`.
  `server_h.rs` and `server_cc.rs` keep only the shell.
- Apply only labelled rewrites in the spirit of T1-T9
  (glr/`docs/ghost-log/raftrs/port-audit.md:45-55`):
  - T1: logging becomes no-ops;
  - T3: iterator chains become loops;
  - T7: collection swaps;
  - terms become a single u64 (`src/server_h.rs:549-557`);
  - `Cmd` becomes `external_body`;
  - no SipHash and no allocating `Default`.
- `RaftLog` keeps its blocked layout (`src/server_h.rs:581-725`, chosen to avoid
  reallocation stalls, `:636-639`) behind a `view(): Seq<Term>` spec. Swapping
  the layout is a separate, measured change.
- `step_checked`, with the integer B-list checks:
  - sender is self or a non-voter (B20);
  - term 0;
  - prev beyond the log;
  - a success reply that claims more than the log (B4);
  - entry terms of 0 (B16).
  - If the group chooses the fallback for the extra rejects (§4), it also
    refuses those messages here, including the refused committed conflict; that
    check reads the follower's log (it must find the first conflict), so it is
    not an integer compare. Measure it on G2 and G4.
- Cpp and hybrid lanes: depends on Phase 0 spike (b). Either generate erased Rust
  for rusty-cpp, or freeze those lanes on the pre-Phase 6 core. User's choice.

**Verification artifacts.** The core can now be type-checked by Verus. Prove
panic freedom for the `assert!`s (their A5): peer-table size, missing entry,
tail mismatch, and the vote-handler panics. Use `verus/commit_rule.rs` as a
proof skeleton for the commit rule; it is a reduced model (§4), so the proof
is redone on the core's real types.

**Correctness.**
- Tier 1.
- **Byte-identical replay** (§2): the new crate replays the events recorded on
  the Phase 5 (or Phase 4) core and must produce identical `Output`s.

**Gate.** G1-G6 at their Phase 0 round counts, plus **milestone 2** against both
the parent and the Phase 0 baseline (§6). G5 and G6 catch payload
copies; G2 and Jetpack loopback catch per-event cost.

**Risks.** Rewrites forced by the Verus subset. On raft-rs they cost 17-24% until
fixed (glr/`docs/ghost-log/raftrs/port-audit.md` §9.1-9.2). Measure each rewrite
on G2.

### Phase 7 (optional, performance only, each change measured separately). Wakes, not polls (1-2 weeks)
**Goal.** Win latency. The trace shows about 2.3 of the 2.6 ms at 4 KB is three
~1 ms waits (`docs/performance/raft-latency-breakdown/README.md`).

**Changes, each landed and measured on its own:**
- **Replies as events.** The srpc callback runs on the poll thread
  (`rt/src/seam.rs:25-29`) and calls `on_append_resp` directly, so the phase-2
  1 ms poll goes away. `RoundEnd` fires at quorum or on a deadline timer. Vote
  replies likewise settle the election immediately. New hazard: today the
  callbacks hold no server reference (the vote callback holds only a
  `TallyState` clone, `rt/src/transport.rs:563-577`). Calling the core from the
  callback means it must capture a server reference that stays valid after the
  round or campaign ends and through teardown, stay `Send`, and take `mtx_` on
  the poll thread once per reply (until poll-thread ownership removes `mtx_`).
  Because `RoundEnd` can now fire while a later round's `TickHeartbeat` is
  being prepared, the Phase 1 rule (commit read in the call that builds the
  message) is what keeps the A9 clause true here.
- **eventfd wake** on `PollThread::add` (`src/srpc/reactor/reactor.rs:2215-2219`).
  It is written only when an atomic "sleeping" flag is set. The epoll timeout
  is a fixed 1 ms literal today (`srpc_epoll_wait(poll_fd, ..., 100, 1)`,
  `src/srpc/reactor/epoll_wrapper.rs:124`, called from `pollworker_poll_loop`,
  `src/srpc/reactor/reactor.rs:3302-3313`; there is no next-timer computation)
  and stays 1 ms. What the eventfd removes is the existing 0-1 ms hop from the
  submit thread's wake job to the poll thread ("appended → send starts",
  `docs/performance/raft-latency-breakdown/README.md:109`, `:133-135`). This is
  a shared subtree file, so it goes upstream or onto the re-apply list (the R4
  lesson, §2 glossary). The transpiled C++ lane that Paxos uses must keep
  working.
- **Blocking apply channel.** One message per ready batch replaces
  `raft_thread_sleep_ms(1)` (`src/server_h.rs:3372`). Only here does the apply
  thread drain several entries at once (pop up to K under one queue lock,
  apply, then one `Applied(n)`). Coarser publication delays
  `appliedIndexForWait_` and `execute_index_`; their readers
  (`src/server_h.rs:2076`, `:2716`, `:2815`, `:3036`, `:3387`, and the
  committed-conflict check at `:5413-5418`) must be reviewed for that delay,
  and a lab case that waits on the applied index added.
- **Poll-thread ownership.** With the eventfd in place, `Propose` and
  `Applied(n)` become jobs and `mtx_` is deleted along with `ReplicationWakeGate`
  (`src/server_h.rs:1143-1405`). Mandatory if the group rejects lock
  serialization. Two things must be specified first:
  - **Propose's result.** `Submit` needs a synchronous accept/reject
    (`raft_worker.cc:856-862`; `n_tot` feeds `get_outstanding_logs`,
    `raft_main_helper.cc:977-990`). Options: (a) pre-check the atomic
    `is_leader` mirror, enqueue the job, and count late rejections back out of
    `n_tot` asynchronously (`n_tot` is then briefly too high); (b) block the
    submit thread on the job with an eventfd plus a completion, which costs one
    poll-thread hop per entry. Gate (a) and (b) separately on G1 p50/p99 and on
    G2 throughput.
  - **Who batches.** `Submit` is called once per entry. The submit thread
    already drains up to `batch_limit_` entries from `submit_queue_` under one
    lock, but then calls `Submit` for each one (`raft_worker.cc:1221-1233`);
    without a submit thread, `enqueue_to_worker` calls `Submit` directly
    (`raft_main_helper.cc:643-647`). So `Propose(batch)` means: the submit loop
    hands its drained batch to one job, with per-entry accept/reject returned
    (as above), and the no-submit-thread path sends a batch of one. Gate it on
    G1 (it must not add latency at low load) and G2.

**Verification artifacts.** Unchanged: timers are Tick events.

**Correctness.** Tier 1, plus the Paxos suites and `srpcTests` for the srpc
change. Comparison modulo timing (§2): same terms, leaders and committed logs.
Replay is not expected to be identical, because event order changes.

**Gate.**
- G1-G6 at their Phase 0 round counts per sub-change; G7 for "replies as
  events" (elections settle earlier).
- Expected: G1 improves (inferred, up to about −2 ms p50); no point gets worse
  than its bound.
- Milestone after the last sub-change.

**Risks.**
- Without the sleeping flag, a wake storm, which G2 would catch.
- srpc subtree merge conflicts.

### Phase 8. Coupling and the proof (4-8 weeks; runs alongside the group's re-proof)
**Goal.** A per-node certificate, then their N-node `theorem_compose_safety`
(glr/`src/protocol/Raft/ghost_log_compose.rs:901-915`).

**Changes.** Ghost code and proof files only, with no executable change. A
diff-2 lint enforces this, modelled on glr/`docs/ghost-log/raftrs/port-audit.md:684-707`.
- `core/coupling.rs`:
  - `state_view`. Tracked: term, role (Candidate =
    `!is_leader && election_in_progress && election_term == current_term`),
    vote, log, commit, `votes_granted`, `match_index` (0 = absent).
    `next_index` is a shadow (raft-rs V3). Unmarked: leader hint, ledger,
    rounds, backoff.
  - `msg_view`. Batched appends map through BR1/BR2; a reject's match is viewed
    as 0 (B17); the heartbeat commit uses the §4 view.
  - The invariant conjuncts, as in glr/`docs/ghost-log/raftrs/coupling.md:16-17`.
- In each handler:
  - `open_recv` or `open_tick` at entry;
  - `ghost_set` plus a lemma after each tracked write;
  - `ghost_send` per queued message;
  - `close_seg(label)`.
  - These follow glr/`src/ports/raftrs/coupling.rs:2205-2431`.
- Instantiate composition with our `msg_view` and the written host contract:
  - §1.3's definition of "RPC correct" (genuine packets, truthful sender and
    reply-to-slot attribution, one static voter set);
  - serialization (§2, "Single owner"): calls serialized, run to completion,
    no re-entry;
  - the shell never reads core state outside a call, and **encodes exactly the
    handles and terms in `Output`** (§3.1 rule 1), including the stamped
    per-entry term;
  - the shell never answers a request on the core's behalf (Phase 1's
    unavailable-vote fix).
  This contract is ours; none of the group's documents states one for a
  lock-serialized host (§2), so it must be agreed with them (§4 question 1).
- CI job: Verus over `core/`, run on changes to it only. Their crate took 291 s
  and 8 GB.

**Verification artifacts.** Under the gate (fresh start, static config,
snapshots off, no restart):
- at most one leader per term;
- log matching;
- committed entries never lost or changed.

Liveness is not proved.

**Correctness.** Verus passes; the diff-2 lint passes; Tier 1.

**Gate.** G1-G6 at their Phase 0 round counts to confirm erasure, and the
Phase 0 cumulative comparison (§6). Expected within noise; the
group saw ±4% (glr/`docs/ghost-log/raftrs/port-audit.md` §9.5).

**Size.** Extrapolated, **not measured**. If the core is 2-3k lines: about
1.5-2k core proof lines plus a 2-3k-line coupling layer. raft-rs paid 3,357
coupling lines plus 3,319 ghost lines for 7,265 lines of code
(glr/`docs/ghost-log/raftrs/port-audit.md` §10).

**Risks.** The group rejects the widening (use the §4 fallback). Mako's B-list
may be larger than expected.

### Phase 9 (joint, open-ended). Remove the gates
- **Snapshots, compaction, InstallSnapshot**, against their A10. New events:
  `SnapshotCreated`, `Compact`, and `RecvInstallSnapshot` split into decide →
  trusted install → `InstallDone`. Gate with the snapshot perf points.
- **Restart.** The group has no Restart action and none is scheduled
  (glr/`docs/ghost-log/spec/raft-spec.md:30`, §6 `:392-397`;
  glr/`docs/ghost-log/raftrs/coupling.md` §7.5, "restarting from existing
  data: not yet modeled; not scheduled"; glr/`docs/ghost-log/method/design.md:236`,
  crash/persistence deferred). A restart extension needs a new spec action plus
  a persistence-boundary invariant that the group has not designed. Until they
  do, the only option the method can certify is "restart = new node". If
  restart is added later, the code side would persist term, vote and log
  before sending any reply (analogous to raft-rs's persist-before-send Ready
  ordering); persistence puts I/O on the hot path and needs its own perf
  study.
- **Reads (A8)**, once a read API exists.
- **Pipelining**, i.e. more than one append in flight. `max_inflight > 1` is
  already inside the method's gate (glr/`docs/ghost-log/raftrs/port-audit.md:134`).
  It is a measured capacity win for large entries.

### Schedule

| Phase | Weeks (rough) | Gate | New property |
|---|---|---|---|
| 0 baseline/spikes | 1 | baselines | — |
| 1 RaftCore + fixes | 1.5 | G1/G3/G5 + G7 | spec mismatches in the code removed; no forged vote replies |
| 2 actions | 1.5-2 | G1-G6 + G7 | decisions are pure |
| 3 events | 2-3 | G1-G6 + G7 + milestone 1 | no suspension in protocol steps; replay recorder |
| 4 serialization + mirrors | 1-1.5 | G1-G6 | method's concurrency premise |
| 5 reply echo (optional, defensive) | 1 | G2/G4/Jetpack loopback | defence in depth; proof assumptions unchanged |
| 6 core crate | 2-3 | G1-G6 + milestone 2 + byte-identical replay | Verus-ready, panic-free |
| 7 wakes (optional, perf) | 1-2 | G1-G6 per change, G7 | — (latency) |
| 8 proof | 4-8 | G1-G6 (erasure) + Verus CI | Raft safety under the gate |
| 9 extensions | open | snapshot/restart perf sets | snapshots, restart, reads |

Phases 0-8 take about 15-22 weeks for one person (extrapolated). The group's
A0-A7 took 8 days, but they had no production refactor to do. That refactor is
Phases 1-6 here.

---

## 6. Performance contract

**Baseline.** In every comparison, the parent commit's Rust-lane build runs as
one arm (`build_rust_pre` vs `build_rust`), measured fresh. Old C++ numbers are
not the baseline.

**Cumulative baseline.** Parent-versus-child gates alone let in-bound losses
add up: nine phases each allowed +2% p50 / −2% throughput could drift about
15-20% from today with no gate failing. So at every milestone (after Phases 3
and 6, after Phase 7 if done, and at Phase 8) a third arm is built from the
Phase 0 baseline commit (`76ea6cb46`, or whatever Phase 0 records), and the
cumulative change is gated against it with the same bounds as Tier 2. At a
milestone, `compare.py` runs with the Phase 0 records as `<before-dir>`
(`scripts/raft_perf/compare.py <before-dir> <after-dir>`). For reference only, Rust-lane figures from
`docs/performance/raft-rust-9a361eccd/compare-vs-412c225a.txt`:
- 4 KB p1 at 240/s: p50 2.647 ms;
- 4 KB p1 unthrottled: 37,760/s;
- 286 KB p6 multi at 190/s: p50 3.343 ms;
- 286 KB p6 multi unthrottled: 3,132/s;
- 1 MiB p1: 183/s.

**Tier 1, every commit (correctness).**
- `raftLabTest`, `raftLabTestHybrid`/`Cpp` (while those lanes are kept),
  `shard1ReplicationRaft`, `shard2ReplicationRaft`,
  `shard1ReplicationSimpleRaft`, `shard2ReplicationSimpleRaft` (`ci/ci.sh`).
- For any srpc change, also the Paxos suites and `srpcTests`.
- Count first-attempt failures, because the Raft suites retry once
  (`ci/ci.sh:523`).
- Every perf run must show gaps 0, dup 0, ooo 0.

**Tier 2, every phase.** `scripts/raft_perf/rotation_trial.sh` with the
parameters below (env vars of that script; `GROUP` is the Phase 0 addition).
Every point sets all of them explicitly, because the script's defaults
(`MAXOUT=4096`, `DUR=8`, `rotation_trial.sh:23-24`) are wrong for large
payloads: `submit_queue_` holds a copy of every payload, so 4096 in flight at
286 KB is about 1.2 GB per partition (`run_sweep.sh:94-102`), about 7 GB at p6,
and could OOM. MAXOUT and DUR follow `run_sweep.sh` (`:94-102`, `:174`), so the
points are comparable with the sweep. All seven take about 2-3 h at 25 rounds.

| Point | PAYLOAD | RATE | MAXOUT | DUR | PARTS | GROUP | Measures | Bound |
|---|---|---|---|---|---|---|---|---|
| G1 | 4096 | 240 | 4096 | 10 | 1 | single | low-load latency, wake path | p50 ≤ +2%, p99 ≤ +5% |
| G2 | 4096 | 0 | 4096 | 10 | 1 | single | per-message CPU on the poll thread | thr ≥ −2% |
| G3 | 286208 | 190 | 256 | 10 | 6 | multi | production shape | p50 ≤ +2%, p99 ≤ +5% |
| G4 | 286208 | 0 | 256 | 10 | 6 | multi | production capacity | thr ≥ −2% |
| G5 | 1048576 | 55 | 64 | 10 | 1 | single | per-byte path (payload copies) | p50 ≤ +2%, p99 ≤ +5% |
| G6 | 1048576 | 0 | 64 | 10 | 1 | single | large-batch round time | thr ≥ −2% |
| G7 | 4096 | 240 | 4096 | — | 1 | single | election and failover time (Phase 0 harness: N leader stalls or kills) | median and p90 of leader-loss → new leader and → first commit, bounds fixed in Phase 0 |

G7 runs on Phases 1, 2, 3 and 7, which change the election path (timer
parameters, the cut election, vote replies as events). Every other point is
steady state and cannot see an election regression.

**Deriving rounds and bounds (Phase 0).** For each point, Phase 0 measures the
paired spread (the per-round B/A − 1 of two identical builds) and computes the
paired MDE, `2.8 × CV_paired / √n`. The round count for that point is the
smallest n whose MDE is below the bound. If even 25 rounds cannot get there,
the bound is widened to the 25-round MDE, and the widening is written next to
the point. Today's figures show why this matters: the Rust-lane 4 KB
unthrottled throughput is 37,759.6 ± 3,057 (CV about 8%,
`docs/performance/raft-rust-9a361eccd/compare-vs-412c225a.txt:219`), an
unpaired MDE of about 4.5% at 25 rounds, more than twice G2's 2% bound. The
nearest measured 4 KB throttled p50 (at 5,000/s) has CV 2.8%, MDE 2.5% at
n = 10 (`docs/performance/raft-baseline.md:226-237`), above G1's 2% bound.
Paired spreads should be smaller; **not verified** by how much. The
10-15-round shortcut is allowed only for points whose paired MDE at that count
is below the bound.

**Pass rule.** A point fails only if **both** hold:
1. the median paired ratio B/A − 1 is past the bound in the bad direction; and
2. the two-sided sign test over the rounds is significant (p < 0.05) in the
   bad direction.

A consistent shift smaller than the bound is reported, not failed. (A sign
test alone flags any consistent shift, however small: the existing paired run
shows p = 0.000 for a −1.57% p50 shift,
`docs/performance/raft-rust-t4-paired/paired-4k.txt:4`, so on its own it would
make the effective bound zero.) The command, per point:

```
PAYLOAD=… RATE=… MAXOUT=… DUR=10 PARTS=… GROUP=… \
  scripts/raft_perf/rotation_trial.sh OUT ROUNDS build_rust_pre build_rust
python3 scripts/raft_perf/paired_stats.py OUT ROUNDS build_rust_pre build_rust --json
```

and the gate reads, from the JSON, each bounded metric's median ratio and sign
test p and applies the two conditions above (exit 1 on any failure).
`paired_stats.py` itself exits 1 when fewer than `--min-pairs` rounds are
complete (`paired_stats.py:6-10`); that is a failed run, not a pass.
- A change past a bound in the good direction is reported as "improved", not
  as a failure.
- Never gate on p1 286 KB p99, which is bimodal
  (`docs/performance/raft-baseline.md`, Known gap 6).
- Never gate on throughput at throttled points, which only echoes the offered
  rate.

**Tier 3, milestones (after Phases 3 and 6, after Phase 7 if done, and at
Phase 8).**
- Full `scripts/raft_perf/run_sweep.sh` (about 5 h), compared with
  `scripts/raft_perf/compare.py` (5% plus the MDE; exit 1 = regression)
  against **both** the parent's records and the Phase 0 records (cumulative
  baseline, above).
- G1-G6 against the Phase 0 baseline arm, with the Tier 2 pass rule.
- Jetpack suite (`scripts/raft_perf/jetpack/run_jetpack_sweep.sh`) at
  WAN_DELAY_MS=20 and on loopback:
  - throughput and p50 within ±2% at every N;
  - p90 within ±5%;
  - the knee stays at N=150 (WAN) and N=500 (loopback);
  - p99 at N ≤ 10 is reported but exempt.
- Snapshot points (`stall_*`, `s286k_r190_p6`, with `MAKO_RAFT_SNAPSHOTS=1`)
  whenever snapshot or compaction code moves.

**Tier 4, diagnostic only.** The latency trace (Phase 0). It shows which wait a
change removed.

**Hot-path risks and mitigations:**

| Risk | Where | Mitigation | Gate that catches it |
|---|---|---|---|
| Payload bytes copied into the core (`Vec<u8>` entries) | entry type | opaque handle; encode/decode only in the shell | G5, G6 |
| A synchronous `Propose` result through a job adds a second poll-thread wait (the epoll timeout is a fixed 1 ms, `src/srpc/reactor/epoll_wrapper.rs:124`), on top of today's 0-1 ms wake hop (`docs/performance/raft-latency-breakdown/README.md:109`, `:133-135`) | only if ownership moves to the poll thread | keep lock serialization through Phase 6; add the eventfd first, which removes the existing hop; choose Propose's result scheme (Phase 7) and gate it separately | G1, G2 |
| Election or failover slows down | Phases 2, 3, 7 | keep the 200 µs / 1 s vote loop and timeouts frozen through Phase 6 | G7 |
| Entry stamping moves out of the lock (§3.2) | Phase 3 | it is the same deep copy, done from `Output` handles | G2, G4, G6 |
| Per-event output overhead | `Output` | reused Vecs; one drain per call or pass | G2, Jetpack loopback N=500 |
| Verus-forced rewrites (hashing, allocating `Default`, loops) | Phase 6 | no hashing in the core; measure each rewrite | G2 |
| Larger `RaftEntry` | Phase 2 metadata | a few bytes; removes 3 FFI calls per entry per send | G2, G6 |
| One-entry-per-message gate | spec form | use the BR1/BR2 batched form with today's caps | G4, G6 |
| Work serialized onto the poll thread | Phase 7 ownership | only O(1) handle pushes move there | G2 |
| Verified crate built at a different opt-level or LTO | build | match the Release settings (Phase 0 spike) | all |
| Ghost code | everywhere | erased at compile time | G1-G6 (expected within noise) |

---

## 7. Risks, open questions, non-goals

**Risks:**
- The build spike may show that `verus!` cannot ship through cargo or rusty-cpp.
  Mitigation: an erased-Rust generation step, or freeze the cpp and hybrid lanes.
- The group may decline lock serialization or the reject widening. Mitigation:
  Phase 7 ownership; the `step_checked` fallback (which, for the refused
  committed conflict, is a state-reading check, not an integer compare).
- Moving batch building after the lock would make the shell send entries the
  core did not log. Mitigation: `Output` carries the handles (§3.1 rule 1).
- Memory-only restart is a possible real safety gap that the certificate will
  not cover.
- The core's size is not measured, so the proof effort could exceed the
  estimate.

**Open questions (not verified):**
1. Does the group accept lock-serialized calls as the host's serialization
   guarantee? None of their documents states such a host contract; their
   method assumes a single-threaded caller (design.md §8, `:229-233`). Also:
   the unguarded LRejectAppendEntries (including the refused committed
   conflict), and the prev-0 view?
2. Can srpc fire a reply callback twice, or deliver a reply to the wrong
   request? With success replies already self-describing (§1.3), this only
   affects reply-to-slot attribution, which stays trusted either way; it
   decides whether the optional Phase 5 is worth doing.
3. Is any Raft replica ever restarted in production with lost memory?
4. Does the leader-change callback re-enter Raft (`raft_worker.cc:311-330`)?
5. Is HEAD `76ea6cb46` as fast as the `9a361eccd-dirty` records?
6. Does anything outside `src/deptran/raft/src` read the authority ledger? A
   grep found nothing.
7. How large is the pure protocol subset of `server_h.rs` and `server_cc.rs`?
8. Can the Rust service layer return a non-zero RPC code for a handled request
   (needed by Phase 1's unavailable-vote fix)?
9. How small is the paired spread at each G point, and so how many rounds does
   each need (Phase 0)?

(Settled since the previous revision: `rotation_trial.sh` does not pass
`--group-mode`, but `raft_bench.sh` accepts it, so Phase 0 forwards it;
`AeDecodePayload` does not reject term-0 entries, so Phase 1 adds the check.)

**Non-goals:**
- Verifying srpc, the codec, the C++ shim, the apply callback or RocksDB.
- Proving liveness.
- Covering snapshots, restart, membership or reads in the first certificate.
- Changing any timer constant before Phase 7.
- Changing the `add_log_to_nc` / `RaftWorker` API.
- Changing the C++ lane (`MAKO_RAFT_LANE=cpp`).

**User decisions:**
1. Whether to do the optional defensive reply echo (Phase 5). The proof does
   not need it.
2. Restart policy.
3. Whether a first certificate with snapshots off is worth having.
4. The fate of the cpp and hybrid lanes.
5. Approval to ask the group for the widening.
6. Approval to commit the trace kit, the lab comparator, the replay recorder
   and the election-time harness.
7. Propose's result scheme under poll-thread ownership (Phase 7, (a) or (b)),
   if Phase 7 is done.
