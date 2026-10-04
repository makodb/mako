# Bugs found during the Verus Raft work

Every defect noticed while executing [modification-plan.md](modification-plan.md),
whether or not it is fixed. The plan freezes behaviour (plan §A), so most
entries are recorded here and fixed only through a numbered F-item, or after
the user approves a new one (plan 0.7 point 3).

Each entry gives: where (file:line at the commit named), a concrete scenario,
how it was confirmed, severity, and its fate. "Read in code" means confirmed by
reading the source, not by running it. Paths are relative to
`src/deptran/raft/` unless they start at the repo root.

Severity: **safety** (can break Raft's guarantees), **liveness** (delays or
stalls progress), **race** (undefined behaviour or a torn read, harmless in
practice so far), **robustness** (only malformed or non-genuine input triggers
it), **metric** (wrong number in a measurement, no protocol effect),
**proof coverage** (a run the certificate does not cover, no protocol
effect). "Latent" means nothing in production reaches it today.

| # | Severity | Summary | Status | Fate |
|---|---|---|---|---|
| B1 | liveness | Candidate's "no" quorum is off by one: a lost election is never decided early | read in code; already known and pinned for lane parity by `rt/tests/transport_roundtrip.rs:142-155` | not in the plan; changing it would be a new F-item (needs approval) |
| B2 | race | `CommitIndex()` reads `state_.commit_index_` without `mtx_` | read in code | F8 (Phase 4) |
| B3 | robustness | Vote replies are counted by number, so a duplicated reply counts twice | read in code; whether srpc can duplicate is open | Rust lane: fixed by F1 in `b75f285e2` (Phase 1); C++ lanes: by the core's own count (Phase 3) |
| B4 | robustness | Decoded AppendEntries entry terms are never checked (0, negative, above the leader's term) | read in code | 0 and negative: fixed by F4 in `a38c5012e` (Phase 1); above the leader's term: F9 (Phase 6) |
| B5 | latent | Phase 1 sends with the round's term and never re-checks leadership under the lock that builds the message; the Rust send kernel ignores its `is_leader` argument | read in code; not reachable today | fixed by F3 in `372b73e6f` (Phase 1) |
| B6 | safety (assumed away) | Raft state is memory-only: a replica restarted under its old id can vote twice in a term, and a majority restart loses committed entries | read in code (plan §4.4.2) | v1 trusted assumption; spec v3 later |
| B7 | metric | `get_outstanding_logs` subtracts the global commit index from a per-node submission count | read in code | report only |
| B8 | dead code | `setIsLeader`'s "stale leadership publication" check compares `current_term_` with a copy of itself, so its term half never fires | read in code | report only (behaviour freeze) |
| B9 | test infra | `ci.sh`'s `cleanup_processes` kill -9s every same-user process named `dbtest`, `simpleTransactionRep`, ... and deletes the shared `/tmp/$USER_mako_rocksdb_shard*`, so a suite in one worktree kills tests running in another | read in code | worked around: `scripts/verus/tier1.sh` waits until no such process runs outside this worktree |
| B10 | toolchain | rusty-cpp's transpiled `BTreeMap` port cannot compile `clone()` of a `BTreeMap<u32, Vec<i32>>` (no matching `push` in the internal-node clone path) | reproduced (cpp-lane lab build) | worked around in `src/lab.rs`; third-party, report only |
| B11 | race (latent) | `RegisterLeaderChangeCallback` writes `leader_change_cb_` with no lock while a role change reads and calls it under `mtx_` | read in code | fixed with F6 (Phase 2): registration and every read of the slot take `leader_notices_`'s lock; the callback runs from a copy |
| B12 | dead code | `heartbeat_phase0_body` returns `true` when phase 0 declines the round (not leader), so the driver's `continue` never fires and phases 1-3 run, each exiting at its first leadership check | read in code | Phase 3: the cut heartbeat ends the round when `tick_heartbeat` declines it (no protocol-visible difference) |
| B13 | toolchain | The transpiled C++ cannot redeclare a name in one scope: a Rust `let` that shadows a binding, or a function parameter, at the same block level is a C++ redefinition | reproduced (cpp-lane lab build, Phase 1) | worked around by renaming; a constraint on every transpiled file |
| B14 | race | `heartbeat_interval_us_` is a plain field the lab's case 67 writes while the heartbeat and election loops read it | reproduced (Phase 4 TSan lab build, cpp lane) | recorded only (user, 2026-10-04): lab-only, outside the core |
| B15 | race (latent) | The heartbeat driver reset the core's round state with no `mtx_` (`reset_round_state` at the loop's start and before its epilogue), taking `&mut` of the whole `RaftCore` while other threads may hold it under the lock | read in code (Phase 6) | fixed in Phase 6: both resets are `step(ResetRoundState)` under `mtx_` (plan §3.1 rule 1) |
| B16 | liveness (latent) | An entry whose command has no value (an empty `Command`, which `Start` accepts) is never replicated: the leader's payload selection reads it as a missing entry and skips every follower behind it, every round; a follower's conflict scan likewise reads such a slot as absent | read in code (Phase 8) | recorded only (user, 2026-10-04): no caller proposes an empty command today; the proof takes "a proposal has a value" as a host-contract premise |
| B17 | safety (latent race) | The round end (PHASE 3) advances the commit index without checking that this server still leads: `heartbeat_round_end_body` reads `IsLeader()` before taking `mtx_`, and `heartbeat_phase3_locked` calls `raft_commit_advance` whatever `is_leader` is. A server that lost leadership in that window, and meanwhile took a newer leader's entries over its tail, counts its old term's match indices against an entry of the new term. In production the Rust lane's threading keeps the window closed; the lab's direct handler calls can open it (Reachability, corrected 2026-10-04) | reproduced at the core: `core/tests/b17_round_end.rs` (Phase 8) | recorded only (user, 2026-10-04): not fixed; the proof takes "the round end runs while leading" as a premise, which the race can violate |
| B18 | proof coverage | Shutdown clears `looping_`, so `IsLeaderLocked()` reads false while the core still leads: a leader's `RecvAppendReply` or `RoundEnd` stepped after `PrepareForShutdown` (or `FailStop`) breaks its coupling premise (`is_leader` is the core's role). The core does nothing wrong then (a reply is ignored unless its term steps the core down; the round end commits as the leader the core still is and confirms no read authority), but the step is outside the certificate | read in code (while writing `code-structure.md`) | recorded; not fixed (needs the user's approval) |
| B19 | race | Every thread and fiber that enters the shell through a raw pointer makes its own `&mut RaftServerBase`, and some hold it long: the apply thread for its whole life, the heartbeat driver across every wait, each RPC handler through `RaftRpcService::server(&self) -> &mut`. These `&mut` alias across threads, which is undefined behaviour in Rust's model whatever lock or atomic guards the fields | read in code (while writing `code-structure.md` §7) | recorded only; no plan item |

Found at commit `150be3e3b` (2026-10-03) unless stated.

---

## B1. The candidate's "no" quorum can never form early (off by one)

**Where.** `rt/src/transport.rs:591-628` (`TallyState`); its C++ original is the
generic `QuorumEvent::no` (repo `src/srpc/reactor/reactor.rs:2309-2315`), used
by `RaftCommo::BroadcastVote` (repo `src/deptran/raft/commo.cc:118-119`) as
`RaftVoteQuorumEvent(n, n / 2)`.

**What is wrong.** The tally counts votes from the `n - 1` **peers** (self is
not fed), and "yes" is `yes >= n / 2` peer votes, which is correct (self plus
`n/2` peers is a majority). But "no" is `no > n_total - quorum` with
`n_total = n`, **self included**. Self never votes no, so the right bound for
peers is `no > (n - 1) - n/2`, i.e. "enough peers refused that yes can no
longer be reached".

| Cluster size n | peers | yes needs | no needs today | no should need |
|---|---|---|---|---|
| 3 | 2 | 1 | 3 (**impossible**) | 2 |
| 5 (the lab, `config/raft_lab_test.yml:8`) | 4 | 2 | 4 (every peer) | 3 |

**Scenario.** In a 3-node cluster, node A campaigns at term 7; B and C both
refuse (they already voted for someone else, or their logs are newer). The
tally reaches `no = 2`, which is not `> 2`, so `decided()` stays false and
`raft_broadcast_vote_and_wait` (`rt/src/seam.rs:264-291`) polls every 200 µs
until its 1 s deadline. Only then does A settle. If a refusal carried a higher
term, A also keeps campaigning at the stale term for that second.

**Effect.** Liveness/latency only: a candidate that has already lost waits up
to 1 s before giving up. No safety impact (a yes quorum is computed
correctly).

**Known, deliberately.** The raft-rt test
`a_rejected_three_replica_campaign_waits_out_its_timeout`
(`rt/tests/transport_roundtrip.rs:142-155`) pins exactly this, with the same
arithmetic in its comment ("no() is n_voted_no > n - n/2 = 2, which two peers
cannot reach -- so, exactly as on the C++ lane, the campaign does not lose
early"). So it is a conscious parity choice with the C++ lane, whose generic
`QuorumEvent::no` was written for events where every participant votes. It
is recorded here because it is still a liveness cost against standard Raft,
and because it contradicts one of the plan's lab-case descriptions (below).

**Fate.** Not in the plan's F-list; the behaviour freeze (plan §A) keeps it.
Plan correction: the Phase 1 lab case "vote request to a server whose
`rpc_ready_` is false ... gives up on a 'no' quorum without waiting for its
deadline" holds in the 5-node lab only if **all four** peers refuse; with one
peer silent or granting, the candidate waits for the deadline. Fixing B1 would
be a new F-item (plan 0.7 point 3) and would change G7 election times.

## B2. `CommitIndex()` is an unlocked cross-thread read

**Where.** `src/server_h.rs:4654-4657`; caller `get_outstanding_logs`
(repo `src/deptran/raft_main_helper.cc:977-990`). The trait comment
(repo `src/deptran/scheduler.h:158-161`) already calls it "tolerated racy".

**What is wrong.** The poll thread writes `state_.commit_index_` under `mtx_`;
`CommitIndex(&self)` reads it with no lock and no atomic. In Rust's memory
model this is a data race (undefined behaviour), not merely a stale read.

**Effect today.** The only live caller is `raft_bench`'s sampled
`peak_raft_outstanding` metric (repo `src/deptran/raft/raft_bench.cc:1225-1236`);
the transaction-path caller in repo `src/mako/sto/Transaction.cc:810-821` is
commented out. On x86-64 an aligned u64 load does not tear, so no wrong value
has been observed.

**Fate.** Fixed by F8 (Phase 4): `CommitIndex()` reads `commit_index_mirror_`, an
atomic the shell publishes (`publish_mirrors`) at the end of every critical
section that runs a core decision, and after the shell's own writes
(snapshot recovery and install).

## B3. Vote replies counted by number

**Where.** `rt/src/transport.rs:555-577` (callback), `:600-610` (`feed`).

**What is wrong.** `feed` increments `yes`/`no` per reply and does not record
which peer answered. If one peer's reply were delivered twice, it would count
as two votes; in a 5-node cluster, one granted reply delivered twice would be
taken for a quorum (`yes >= 2`).

**Reachability.** Each peer gets one RPC per campaign, and srpc fires each
reply callback once as far as read so far; whether srpc can ever fire a
callback twice (for example around reconnect replay, repo
`src/srpc/rpc/client.rs:1544`) is the plan's open question 1, **not verified**.

**Fate.** F1 (Phase 1): count distinct voter ids for the campaign term.

## B4. AppendEntries entry terms are not validated

**Where.** `src/server_h.rs:4263-4287` (`AeDecodePayload`): batch terms come
from `raft_batch_term_at` (an `i64`), the single-entry term from
`leader_next_log_term`; neither is checked.

**What is wrong.** A malformed append carrying an entry of term 0, a negative
term, or a term above the leader's own term is accepted and stored. A genuine
leader never produces such an entry, so only corrupt or non-genuine input
triggers it; but a stored term-0 entry breaks the "entry terms are positive"
invariant (the group's B16) that log-matching proofs use.

**Fate.** F4 (Phase 1): reject term-0 (and, as a refusal, any non-positive)
entry terms on both the batch and single-entry paths.

## B5. Phase 1 does not re-check leadership where it builds the message (latent)

**Where.** `src/server_cc.rs:1101` (`IsLeader()` takes and drops `mtx_`),
`:1117-1222` (the locked block that picks `prev` and the payload, with no
role or term check), `:1229-1237` (send with `round.term()` and phase 0's
`round.commit_index()`). The Rust send kernel ignores its `_is_leader`
argument (`rt/src/seam.rs:359-362`).

**What could go wrong.** If the node stepped down between the check at `:1101`
and the locked block, it would send an append stamped with the old term,
carrying entries read from a log that a newer leader may already have
rewritten.

**Why it is not live today.** Every step-down site (`src/server_h.rs:2858`,
`:3007-3009`, `:3234-3248`, `:4067-4143`, `:5335-5364`; `src/server_cc.rs:1591`)
runs on the poll thread (docs/verus/README.md line 74), and nothing between
`:1101` and the send yields the fiber, so no step-down can interleave. Any
change that moves replies or inbound RPCs off that fiber's schedule (plan
Phase 7, F11a) would make it reachable.

**Fate.** F3 (Phase 1): check `is_leader_ && current_term_ == round.term()`
inside the locked block, and send the `commit_index_` read there.

## B6. Memory-only Raft state: in-place restart is unsafe

**Where.** `src/server_h.rs:2219-2221` (memory-only); plan §4.4.2.

**What is wrong.** Term, vote and log are never persisted. A replica that
restarts and rejoins under its old id comes back at term 0 with no vote and an
empty log: it can grant a second vote in a term it already voted in, and if a
majority restarts, committed entries are lost.

**Reachability.** Whether production or CI ever restarts a replica in place
is **not verified** (a grep of `ci/ci.sh` found only cleanup kills).

**Fate.** Certificate v1 states "no in-place restart under the old id" as a
trusted assumption. A real fix needs synchronous persistence (out of scope).

## B7. `get_outstanding_logs` mixes two different counters

**Where.** repo `src/deptran/raft_main_helper.cc:977-990`.

**What is wrong.** It returns `n_tot - CommitIndex()`. `n_tot` counts the
commands this worker submitted and had accepted (repo
`src/deptran/raft/raft_worker.cc:856-862`); the commit index counts every
committed log slot, including each new leader's no-op and entries submitted
on other nodes before a failover. After the first election's no-op the value
is biased low by one per leader change, and it can go negative. Both are also
cast to `int`, which truncates past 2^31 entries.

**Effect.** Only `raft_bench`'s `peak_raft_outstanding` metric reads it today
(see B2). No protocol effect.

**Fate.** Report only; out of the plan's scope.

## B8. `setIsLeader`'s stale-publication term check is a tautology

**Where.** `src/server_h.rs:2290-2301`.

**What is wrong.**

```rust
let publication_term: u64 = self.state_.current_term_;
if self.stop_.load(..) || self.state_.current_term_ != publication_term {
    // "suppressing stale leadership publication"
    return;
}
```

`publication_term` is read from `current_term_` on the line before the
comparison, with `mtx_` held (the function is caller-holds) and nothing in
between, so `current_term_ != publication_term` is always false. Only the
`stop_` half of the guard can fire. The log message suggests the intent was
to refuse a leadership won in a term that has since moved on; that would need
the term the election was won in, captured by the caller before any yield.

**Effect.** None: the only promotion, `setIsLeader(true)` at
`src/server_h.rs:4123`, is preceded at `:4116-4121` by exactly the check this
guard was meant to make (`stop_ || current_term_ != term`, with `term` the
campaign's term). The check in `setIsLeader` is dead code that reads as a
safety net.

**Fate.** Report only. The Phase 2 restructuring (`become_leader()` taking the
campaign term) is the natural place to make the check real or delete it.

## B9. `ci.sh` cleanup reaches into other worktrees' test runs

**Where.** repo `ci/ci.sh:78-110` (`cleanup_processes`), called before every
replication suite attempt.

**What is wrong.** It lists processes with `ps -u $USER` and `kill -9`s every
one whose executable basename is `simpleTransactionRep`, `dbtest`,
`simplePaxos`, `simpleTransaction` or `simpleRaft`, skipping only its own
ancestors; it also runs `rm -rf /tmp/${USER}_mako_rocksdb_shard*`. Neither is
scoped to the worktree that runs the suite. On a host with several worktrees
of this repository (this one has five), starting any suite in one worktree
kills a suite in progress in another and deletes its RocksDB data.

**Effect.** A spurious failure (or a retry, counted as a first-attempt
failure) in whichever suite was running elsewhere.

**Fate.** Not a Raft bug, so outside the plan; worked around by
`scripts/verus/tier1.sh`, which waits until no such process runs from a
directory outside this worktree before each suite. A real fix would match on
`/proc/<pid>/cwd` under the repository root.

## B10. rusty-cpp's btree port cannot clone a map of vectors

**Where.** `third-party/rusty-cpp/transpiled/btree_port/btree_port.btree.map.cppm:6713`
(pinned rusty-cpp `1689f438`), the `clone` of a map with internal nodes:
`out_node.push(k, v, subroot)` finds no matching `push`.

**How found.** Phase 0's `dump_commit_log` cloned the lab's
`BTreeMap<u32, Vec<i32>>` committed table. rustc accepted it and the Rust and
hybrid lanes built; the cpp lane, which transpiles the lab, failed to compile
`raft.lab.cppm` (`rusty::clone(*(COMMITTED.lock().unwrap()))`). Reproduced in
the `build_cpp_raftlab_cpp` build, log `$RESULTS/p0/build/build_cpp_raftlab_cpp.log`.

**Fate.** Worked around: the dump copies one row at a time under the lock.
A transpiler limitation, not Mako's; the same restriction applies to any
code the C++ lanes transpile (relevant to Phase 6's crate if those lanes are
kept).

## B11. The leader-change callback slot is written with no lock

**Where.** `src/server_h.rs`, `RegisterLeaderChangeCallback` (copies the
callback into `leader_change_cb_`, no lock: "a plain move into the
notification slot; no lock, exactly as the C++ had it") against
`setIsLeader`, which tests and calls `leader_change_cb_` with `mtx_` held,
at Phase 1 tip `22d63ca1e`.

**What is wrong.** The two never share a lock, so a registration that
overlaps a role change is a data race on a `std::function`: a torn copy can
be called.

**Effect.** Latent. The one caller, `RaftWorker::SetupBase`
(`raft_worker.cc:315`), registers during setup, before the server's
election and heartbeat loops run, so no overlap has been observed.

**How found.** Read in code while implementing F6, which moves the
callback out from under `mtx_` and so would have widened the window.

**Fate.** Fixed in the F6 commit (Phase 2): registration takes
`leader_notices_`'s lock, the queue that F6 fires notices from; the
callback slot is read only under that lock and called from a copy taken
there.

## B12. A declined heartbeat round still runs phases 1-3

**Where.** `src/server_cc.rs`, `heartbeat_phase0_body` and
`HeartbeatDriver::run`, at Phase 0 tip `c0197443f` (`:830-834`, `:1859-1861`).

**What is wrong.** When phase 0 declines the round because this server is
not the leader, `heartbeat_phase0_body` returns `true` ("Was `continue`;
the Rust driver starts the next round when this returns true"), but the
driver skips phases 1-3 only on `false` ("PHASE 0 declines the round when
leadership is not held. The C++ spelled that `continue`."). The two comments
contradict each other, and the function returns `true` on every path, so
the `continue` is dead.

**Effect.** None on the protocol: on a non-leader, phase 1 breaks at its
first `IsLeader()` check, phase 2 finds every slot empty (phase 0 abandoned
them) and stops, and phase 3 returns at its `IsLeader()` check. Each round
costs a few extra `mtx_` acquisitions on every follower.

**Fate.** Recorded at Phase 1. Phase 3's cut heartbeat ends the round when
`tick_heartbeat` declines it, as the C++ did; nothing a follower could
observe changes.

## B13. A shadowing `let` is a C++ redefinition

**Where.** rusty-cpp `1689f438`'s emitter, for any crate the cpp or hybrid
lane transpiles (`src/*.rs`, the lab included).

**What is wrong.** Rust lets a `let` shadow an earlier binding in the same
block, or a parameter at the top of a function body; the emitter writes
both as C++ declarations in one scope, which clang rejects
("redefinition of 'ok'"). It surfaces only in the cpp lane's build, which
transpiles the lab; rustc and clippy accept the code.

**How found.** Phase 1's lab case 12 bound `ok` twice
(`let Some((ok, _, _)) = ...`); the cpp-lane lab build failed
(`$RESULTS/p1/tier1/cpp.log`). Phase 2 had the same shape, a `let stopped`
redeclaring `raft_on_append_entries`' `stopped` parameter, found by
reading before any cpp build.

**Fate.** Worked around by renaming (case 12's bindings) and by using the
parameter (Phase 2). A transpiler limitation, not Mako's. Moot on this branch
from Phase 6: the cpp and hybrid lanes, the only ones that transpiled the
crate, were removed (plan Q9).


## B14. The heartbeat interval is a plain field written while the loops read it

**Where.** `src/server_h.rs`, `RaftServerBase::heartbeat_interval_us_` and
`SetHeartbeatInterval` (`:3609` at Phase 4's tip); readers `HeartbeatWait`
(`:5192`) and the election timeout's window (`:3710`), on poll threads.

**What is wrong.** `SetHeartbeatInterval` writes the field with no lock and
no atomic. Production writes it only before the loops start
(`ConstructRuntime`, the `MAKO_RAFT_HEARTBEAT_INTERVAL_US` override in
`SetupInternal`), but lab case 67 ("heartbeat interval configurable",
`src/lab_snapshot_cases.rs:866-886`) calls the setter on running servers, so
the lab has a data race: undefined behaviour in both Rust's and C++'s memory
models, a stale or torn read in practice at worst.

**How found.** Phase 4's TSan lab build (cpp lane, `$RESULTS/p4/tsan/`):
three reports, all this field (the heartbeat loop on two servers, the
election timer on one), and nothing else.

**Fate.** Recorded at Phase 4 and left as is (2026-10-04): the race is
lab-only and outside the core, and the cpp lane, the only one TSan could
check, was removed (plan Q9). The fix, if wanted later, is a relaxed atomic
(a plain `mov` on x86-64).

## B15. The round state was reset without `mtx_`

**Where.** `src/server_cc.rs`, `HeartbeatDriver::run` (`:354` and `:376` at
`7aa01a586`): `server.core.reset_round_state()` when the heartbeat loop
starts and again before its epilogue, with no `RaftLockGuard`.

**What is wrong.** Phase 1 moved the driver's round state (in-flight slots,
authority ledger, leader epoch, round scope) into `RaftCore`, and the two
resets kept their old place outside the lock. Only the heartbeat fiber
reads or writes those four fields, so no field is raced in practice; but the
call takes `&mut RaftCore` while an RPC handler or the election loop may hold
`&mut RaftCore` under `mtx_` on another thread, which is aliasing undefined
behaviour in Rust's model, and it breaks the serialization the proof relies
on (plan §3.1 rule 1, Q1: one core call at a time, under `mtx_`).

**How found.** Converting every core call to `step` in Phase 6: these were
the only two made without the lock.

**Fate.** Fixed in Phase 6 (the `step` commit): both are
`step(Event::ResetRoundState)` under `mtx_`. No decision changes.

## B16. An entry with no value is never replicated (latent)

**Where.** The leader's payload selection, `core/src/heartbeat.rs`
(`select_payload`'s raw and batch paths: `usable = slot.is_some() &&
slot.unwrap().has_value()`); the follower's conflict scan,
`raft_on_append_entries` in `core/src/node.rs` (`local_exists =
entry.has_value()`); `Start` in `src/server_h.rs`, which appends whatever
command it is given.

**What is wrong.** `has_value()` is whether the entry's command is
non-empty. `Start` does not refuse an empty command, so a leader can hold an
entry without a value. The leader then reads that slot as a missing entry
("Missing log entry ..., skipping follower") and skips every follower whose
next index reaches it, in every round, so replication to them stops for the
rest of the term. On a follower, a slot without a value counts as absent in
the conflict scan, so a duplicated append over a matching slot would truncate
the follower's suffix where Raft keeps it; that case needs the follower to
hold a same-term entry it can only have proposed itself, so it does not
arise, but it is why the proof needs every log entry to have a value.

**Reachability.** None today: both callers, `raft_worker.cc`'s `Submit`
(through the C++ shim's `RaftServer::Start`, `raft_server_start`) and the
lab's `start`, pass a `TpcCommitCommand`, and the leader's no-op is a
`TpcNoopCommand`, all non-empty.

**How found.** Phase 8, coupling the follower's append to
`LFollowerAppendEntries`: the spec keeps a same-term entry, and the exec
keeps it only when it has a value.

**Fate.** Recorded only (user, 2026-10-04): `Start` keeps accepting an empty
command. The proof takes "a proposal has a value" as a premise of the host
contract (`docs/verus/host-contract.md`), and carries "every log entry has a
value" in its invariant.

## B17. The round end can commit after leadership is lost (latent race)

**Where.** `src/server_cc.rs`, `heartbeat_round_end_body`: `if
!server.IsLeader() { return; }`, then `RaftLockGuard::new(&mut
server.mtx_)` and `step(Event::RoundEnd { is_leader })` with `is_leader =
IsLeaderLocked()`. `core/src/heartbeat.rs`, `heartbeat_phase3_locked`:
`raft_commit_advance(core, nservers)` first, unconditionally; `is_leader`
reaches only the read-index settlement.

**What is wrong.** The commit rule (Raft, Figure 2) is the leader's: an
index N is committed when a majority of the leader's match indices reach N
and the leader's entry at N is of its current term. `raft_commit_advance`
reads the peer table's match indices, which a step-down does not reset
(`set_is_leader(false)` rebuilds nothing), and checks the entry's term
against `current_term_`. After a step-down both are wrong to combine: the
matches are the old term's acknowledgements of the old leader's entries,
while `current_term_` is the new term.

**Scenario.** Five servers, A to E; entries 1..8 (term 2) everywhere.
A, leader at term 2, appended 9 and 10 (term 2) and replicated neither. D
won term 4 with E's and B's votes (their logs equal to its own), appended
its no-op at 9 (term 4), reached only E, and went quiet. A won term 5 with
B's and C's votes (its log 1..10 at term 2 is at least theirs; D and E
refuse, their last term being 4), appended its no-op at 11 (term 5), and
replicated 9 and 10 to B and C, not yet to D or E (a batch stops before the
no-op, which is not a `TpcCommitCommand`), so its peer table holds B = C =
10, D = E = 0. Entry 10 is of term 2, so A rightly commits nothing. D returns and wins term 7: its last
term, 4, beats B's and C's, 2. It appends its no-op at 10 (term 7). A's
heartbeat driver passes the unlocked `IsLeader()` check and blocks on
`mtx_` while the RPC handler takes D's messages: D's RequestVote steps A down
to term 7; after D backs off, an append at prev 8 replaces A's 9 to 11 with
D's 9 (term 4) and 10 (term 7). When the driver gets the lock, `RoundEnd`
runs with `is_leader = false`: the majority match is 10 (A, B, C, from term
5), A's entry 10 is now of term 7 = `current_term_`, and A sets its commit
index to 10 and hands entry 10 to its state machine. Only D and A hold it.

**Consequence.** If D then fails before its entry 10 reaches anyone else, E can
win term 8 (B and C vote for it: its last term, 4, beats their 2) and append
its own no-op at 10. In the
scenario above the entry A applied is D's no-op, so A's state machine is
unharmed, but A's commit index is past the cluster's, and A now refuses
E's entry at 10 for ever as a conflict at or below its commit index (the
`refused_committed_conflict` check): A can no longer rejoin. The same works
one index further with a client command: had A's term-2 entries reached 11
and B and C acknowledged 11, D would put its no-op at 10 and a client's
command at 11, and A would commit and apply that command, which the
cluster then discards. That is a divergence of A's state machine.

**Reproduction.** `core/tests/b17_round_end.rs` drives server A's core
through the scenario's inputs in the race's order (its round's replies, D's
RequestVote and append, then the round end); at the core no timing is
involved, since it takes each input as a whole call. Every intermediate
state is as described, and the round end then moves A's commit index from 8
to 10. The test asserts the correct outcome and is ignored, so the suite
stays green: `cargo test -p raft-core --test b17_round_end -- --ignored`
fails today. With the fix below applied to a working copy (not committed),
it passes, and so do the core's other tests.

**Reachability.** Not in production on this lane (inference, from reading the
threading while writing [code-structure.md](code-structure.md) §6; corrected
2026-10-04: this paragraph first said nothing excludes it). The interleaving
above needs D's messages handled while A's driver sits between its check
(`src/server_cc.rs:310`) and its lock (`:315`), and in production nothing
can handle them there:
- every step that changes the role or replaces the log's tail runs on the
  server's transport poll thread: the inline RPC handlers, the reply
  callbacks, the heartbeat and election fibers (code-structure.md §3.1, §7);
- every critical section that changes the role publishes the mirror the
  check reads before it releases `mtx_` (`run_locked_actions` ->
  `publish_mirrors`, `src/server_h.rs:1560`);
- nothing between the check and the lock suspends, and `mtx_` is a plain
  `std::mutex` (`server.h:243-250`, `:273`): if another thread holds it, the whole
  poll thread waits, so no handler runs in the window;
- the other threads that take `mtx_` (submit, apply, shutdown) never change
  the role or replace the tail.

Lab builds can open the window: `src/lab.rs`'s `serve_vote` and
`serve_append` call `ServeVote` and `ServeAppendEntries` directly from the
harness thread (`src/lab.rs:520`, `:534`), not the replica's poll thread. So
the defect is the core's (its round end does not check), latent in
production behind the shell's threading; moving a handler off the poll
thread, or adding a suspension point between the check and the lock, would
expose it. PHASE 0's advance is guarded (`heartbeat_phase0_locked` returns
before it when not leading) and the reply path counts a reply only while
leading.

**How found.** Phase 8, coupling the round end to `LAdvanceCommitIndex`,
whose guard is `role is Leader`.

**Fix (proposed, not applied).** In `heartbeat_phase3_locked`, advance only
while leading: call `raft_commit_advance` when `core.is_leader_` holds (the
shell's `is_leader` is the same value, read under the same lock, except at
shutdown: B18), and otherwise return no advance. One branch; the read-index
settlement after it is unchanged. With it, the round end's proof premise
becomes a check, and the certificate no longer depends on the race not
happening.

**Fate.** Recorded only (user, 2026-10-04): not fixed. The proof's round-end
contract takes "the round end runs while leading" as a premise, which the
race above violates, so a run that hits the race (a lab build, as things
stand) is outside the certificate. The reproduction stays in the suite,
ignored.

## B18. Shutdown takes a leader outside its reply and round-end premises (proof coverage)

**Where.** `src/server_h.rs:1236-1240`, `IsLeaderLocked()`: false when
`looping_` is false, otherwise `core.is_leader_`. `looping_` is cleared by
`PrepareForShutdown`, under `mtx_` on the shutdown thread (`:3781-3792`),
and by `FailStop`, without the lock (`:2200-2209`; called when a startup or
snapshot step fails). The heartbeat driver passes `IsLeaderLocked()` as
`is_leader` to `RecvAppendReply` (`src/server_cc.rs:245-251`) and to
`RoundEnd` (`:316-317`). The coupling's premises for both ask that
`is_leader` be the core's role (`core/src/coupling.rs:2606-2611`, `:2622`).
Host contract (`docs/verus/host-contract.md` §3) said the collection loop's
`IsLeaderLocked()` guarantees it.

**Scenario.** A leader's heartbeat driver passes its unlocked `IsLeader()`
check (`src/server_cc.rs:221` for a reply, `:310` for the round end), and
the shutdown thread runs `PrepareForShutdown`'s critical section before the
driver takes `mtx_`. Under the lock the driver reads `is_leader = false`,
while `core.is_leader_` is still true: the step breaks its premise.

**Consequence.** None on the protocol. With `is_leader` false, the reply is
recorded as read-index evidence and then ignored, unless its higher term
steps the core down (`core/src/heartbeat.rs:1340-1382`). The round end
advances the commit index as the leader the core still is (PHASE 3's own
contract asks only that the core leads, `:1680`) and confirms no read
authority (`core/src/authority.rs:690-692`). But the certificate covers a
run only while every step meets its premise, so a run in which a leader
shuts down mid-round leaves it at that step.

**How found.** Writing [code-structure.md](code-structure.md), reading the
round end's premise against what the shell passes.

**Fix (not applied).** Ghost only: the two premises could ask for less, the
round end only that the core leads (as PHASE 3's contract does), the reply
only `is_leader ==> core.is_leader_` (as `TickHeartbeat`'s `admits` does),
with the two handlers' proofs redone for `is_leader` false while leading.

**Fate.** Recorded; not fixed (a new item needs the user's approval, plan
0.7 point 3).

## B19. Every thread holds its own `&mut RaftServerBase` (race)

**Where.** The shell is reached through raw pointers, and each entry turns
its pointer into a `&mut RaftServerBase`:
- the C ABI exports call `&mut self` methods through `(*s)`
  (`src/server_cc.rs:417-709`). `raft_server_apply_thread_loop` (`:644-646`)
  runs `ApplyThreadLoop(&mut self)` (`src/server_h.rs:2572`) for the apply
  thread's whole life.
- `Start`, `IsLeader` and `GetLeaderHint` take `&mut self`
  (`src/server_h.rs:3884`, `:3817`, `:3826`) and are called on the submit,
  shutdown and Mako threads.
- the RPC service makes `&mut *self.server.0` from `&self` for every
  handler (`rt/src/service.rs:88-91`, under
  `#[allow(clippy::mut_from_ref)]`).
- the heartbeat driver binds one per run and keeps it across every wait
  (`src/server_cc.rs:358`). The election-timer fiber calls
  `RequestVoteFromElectionTimer(&mut self)` through its pointer
  (`src/server_h.rs:4131-4133`, `:1137`), which keeps that `&mut` across
  the vote wait in `RequestVoteImpl` (`:3155`, `:3222`).

**What is wrong.** Two live `&mut` to one object are undefined behaviour in
Rust's model, even when every field they touch is locked or atomic: while a
`&mut` lives, the compiler may assume nothing else reaches that memory.
`mtx_`, the atomics and the per-field mutexes serialize the accesses that
matter (all but `heartbeat_interval_us_`, B14), so nothing observed goes
wrong; correctness rests on the optimizer not exploiting the aliasing. B15
was the same defect for `&mut RaftCore`, which Phase 6 fixed by moving two
calls under `mtx_`; this one is in how every entry reaches the shell.

**How found.** Writing [code-structure.md](code-structure.md) §7; one of
its checkers raised it.

**Fix (not applied).** Entries take `&RaftServerBase`, and every field that
changes moves behind interior mutability: the core behind the lock it
already has, the rest atomics or cells. The plan's F11d (Phase 7) would
shrink the problem, because `Propose` and `Applied` become poll-thread jobs
and `mtx_` goes, but the threads that enqueue them or read the mirrors
would still need `&self`.

**Fate.** Recorded only: no plan item covers it, and a fix needs the user's
approval.
