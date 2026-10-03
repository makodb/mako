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
it), **metric** (wrong number in a measurement, no protocol effect).

| # | Severity | Summary | Status | Fate |
|---|---|---|---|---|
| B1 | liveness | Candidate's "no" quorum is off by one: a lost election is never decided early | read in code | new; not in the plan; candidate for an F-item (needs approval) |
| B2 | race | `CommitIndex()` reads `state_.commit_index_` without `mtx_` | read in code | F8 (Phase 4) |
| B3 | robustness | Vote replies are counted by number, so a duplicated reply counts twice | read in code; whether srpc can duplicate is open | F1 (Phase 1) |
| B4 | robustness | Decoded AppendEntries entry terms are never checked (0, negative, above the leader's term) | read in code | F4 (Phase 1) |
| B5 | latent | Phase 1 sends with the round's term and never re-checks leadership under the lock that builds the message; the Rust send kernel ignores its `is_leader` argument | read in code; not reachable today | F3 (Phase 1) |
| B6 | safety (assumed away) | Raft state is memory-only: a replica restarted under its old id can vote twice in a term, and a majority restart loses committed entries | read in code (plan §4.4.2) | v1 trusted assumption; spec v3 later |
| B7 | metric | `get_outstanding_logs` subtracts the global commit index from a per-node submission count | read in code | report only |
| B8 | dead code | `setIsLeader`'s "stale leadership publication" check compares `current_term_` with a copy of itself, so its term half never fires | read in code | report only (behaviour freeze) |
| B9 | test infra | `ci.sh`'s `cleanup_processes` kill -9s every same-user process named `dbtest`, `simpleTransactionRep`, ... and deletes the shared `/tmp/$USER_mako_rocksdb_shard*`, so a suite in one worktree kills tests running in another | read in code | worked around: `scripts/verus/tier1.sh` waits until no such process runs outside this worktree |
| B10 | toolchain | rusty-cpp's transpiled `BTreeMap` port cannot compile `clone()` of a `BTreeMap<u32, Vec<i32>>` (no matching `push` in the internal-node clone path) | reproduced (cpp-lane lab build) | worked around in `src/lab.rs`; third-party, report only |

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

**Fate.** F8 (Phase 4): an atomic mirror written after each core call.

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

