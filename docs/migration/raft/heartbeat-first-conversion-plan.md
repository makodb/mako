# Converting `RaftServer` from the HeartbeatLoop outward

A step-by-step plan for moving the core of Raft into Rust without a
line-for-line translation and without a single indivisible big-bang move.

Written 2026-09-13. Everything labelled MEASURED was obtained by running a
command against the tree at commit `5b5825659`; everything labelled INFERRED is
reasoning from those measurements; everything labelled UNTESTED is a hypothesis
with a named way to check it. The distinction matters here more than usual,
because the predecessor document
[`cpp-refactor-plan.md`](cpp-refactor-plan.md) contained four confident claims
that turned out to be false when probed, and this session added two of its own.

## 0. The one-paragraph version

`RaftServer` cannot be converted method-by-method — gate G3 stubs out any
`impl` on a C++-owned type — so the instinctive plan of "convert the small
functions first" does not work. But it does not need to be converted at all in
one piece. Its 64 data members fall into seven cohesive clusters, and **30 of
its 46 stateful functions touch zero or one cluster**. Only seven functions
entangle four or more, and those seven are exactly the protocol entry points
plus `HeartbeatLoop`. That is the shape of a protocol implementation whose
state machine and I/O driver have grown together, and the mature way to take it
apart is the **sans-I/O split**: lift the decisions out as pure types the DSL
can own, leave the I/O in C++ kernels, and let the loop shrink to orchestration.

## 1. The measured structure

MEASURED, `src/deptran/raft/server.cc` and `server.h`:

| | |
|---|---|
| `RaftServer` data members | 64 |
| `RaftServer` functions in server.cc | 50, totalling 3,245 lines |
| the 12 largest functions | 2,639 lines — **81% of the code** |
| the other 38 functions | 606 lines, averaging 15 lines each |
| `HeartbeatLoop` | 789 lines, the single largest |

The twelve that hold 81% of the mass:

```
789  HeartbeatLoop          218  InitializeSnapshotManager   117  CreateSnapshotLocked
312  OnInstallSnapshot      146  StartApplyThread            112  applyLogs
297  OnAppendEntries        132  SetupInternal                96  OnRequestVote
221  RequestVoteImpl        130  setIsLeader                  69  StartElectionTimer
```

### The state clusters

INFERRED, by grouping the 64 members by name and purpose, then MEASURED by
counting which functions reference which group:

| cluster | representative members |
|---|---|
| peer-progress | `match_index_`, `next_index_` |
| election | `vote_for_`, `election_term_`, `election_timeout_us_`, `last_heartbeat_time_`, `req_voting_`, `n_vote_` |
| log-store | `raft_logs_`, `logs_`, `commitIndex`, `lastLogIndex`, `min_active_slot_`, `max_executed_slot_` |
| apply-pipeline | `apply_queue_`, `apply_thread_`, `apply_pending_`, `appliedIndexForWait_`, two mutexes |
| snapshot | `snapshot_manager_`, `snapidx_`, `snapterm_`, the trigger atomics |
| read-index | `heartbeat_round_`, `read_quorum_confirmed_term_`, `read_quorum_confirmed_round_` |
| lifecycle | `stop_`, `looping_`, `rpc_ready_`, `startup_*`, the two loop-running flags |

MEASURED — how many clusters each stateful function spans:

```
spans 0 clusters: 15 functions       spans 4 clusters:  3 functions
spans 1 cluster : 15 functions       spans 5 clusters:  3 functions
spans 2 clusters:  7 functions       spans 7 clusters:  1 function
spans 3 clusters:  2 functions
```

The seven that span four or more:

```
HeartbeatLoop              all seven clusters
OnInstallSnapshot          apply-pipeline, election, lifecycle, log-store, snapshot
InitializeSnapshotManager  apply-pipeline, election, lifecycle, log-store, snapshot
setIsLeader                election, lifecycle, log-store, peer-progress, read-index
OnAppendEntries            election, lifecycle, log-store, snapshot
OnRequestVote              election, lifecycle, log-store, snapshot
StartApplyThread           apply-pipeline, lifecycle, log-store, snapshot
```

**This is the whole finding.** The class is not uniformly tangled. It is a small
number of entangled orchestrators sitting on top of state that is already
cohesive. Thirty functions are, structurally, already extractable.

## 2. Why HeartbeatLoop is the right place to start

It was suggested as the core of Raft in this repository, and the measurements
support it for three independent reasons.

**It is the only function touching all seven clusters.** Every other function
is a leaf or near-leaf by comparison. Whatever structure you impose on
`HeartbeatLoop` propagates outward; whatever you impose elsewhere,
`HeartbeatLoop` will still entangle.

**Its I/O surface is tiny relative to its size.** MEASURED over its 789 lines:

```
commo() RPC sends           3   (only TWO distinct: SendAppendEntries2, SendInstallSnapshot)
future poll / wait          1
clock reads                 2
fiber sleep                 1
lock acquisitions           5
log-store accesses          8
apply-pipeline calls        2
snapshot accesses           3
```

Two outbound RPC kinds in 789 lines. The rest is decision logic. That ratio is
what makes a sans-I/O split cheap here: there is very little I/O to leave
behind.

**It is already written in phases**, and the phases are already the sans-I/O
shape:

```
PHASE 0  (2067-2138)  decide: advance round, snapshot config, compute commit index
PHASE 1  (2139-2465)  emit:   build and send AppendEntries / InstallSnapshot
PHASE 2  (2466-2699)  collect: poll replies through one round deadline, process
PHASE 3  (2700-2778)  decide: recalculate commit index from the new evidence
```

decide → emit → collect → decide. That is precisely the loop a sans-I/O core
sits inside. Nobody has to invent the structure; it is already there, expressed
as comments and stack variables rather than as types.

**And it already has a private state object that is not a type.** MEASURED, its
loop-carried locals:

```cpp
std::map<siteid_t, std::unique_ptr<PendingAppendEntries>> pending_rpcs;
std::map<uint64_t, PendingHeartbeatAuthority>             authority_rounds;
std::optional<uint64_t>                                   pending_leader_term;
```

Those three locals *are* the heartbeat state machine's state. They live on the
stack because nobody named them.

## 3. The strategy: sans-I/O

The mature pattern for porting protocol code between languages is to separate
the **protocol state machine** — pure, no I/O, no clock, no threads — from the
**driver** that performs I/O and feeds it. The state machine consumes events
(messages, timer ticks) and returns a description of what should happen
(messages to send, entries to persist, entries to apply, state transitions).

In Rust this is the dominant structure for exactly this kind of code:
`tikv/raft-rs` exposes `RawNode` + `Ready`; `quinn-proto`, `rustls` and `h2` are
all I/O-free cores with separate drivers. It is also how the same problem is
solved in other language communities, where it goes by the name *sans-io*.

**Why it fits the DSL's constraints specifically**, which is the part that
matters more than the pedigree:

| DSL constraint (all MEASURED this session) | what sans-I/O does about it |
|---|---|
| G3: `impl` on a C++-owned type is stubbed out; the unit of conversion is a whole type | a lifted state machine IS a new whole type, so it converts |
| Rust structs have no inherited fields | a lifted type owns its fields outright |
| `std::thread`, rocksdb handles, `rrr::Client` have no clean spelling | those are all in the driver, which stays C++ |
| foreign types need a model plus a C++ alias | a pure core needs almost none — its fields are scalars and small containers |
| a DSL body that touches state must own that state | decisions become methods on the type that owns the decision's inputs |

The alignment is not a coincidence. G3 forces conversion at type granularity;
sans-I/O is the discipline that produces types worth converting.

## 4. Other approaches, and why they are not the plan

Considered and rejected, with reasons, so they are not re-proposed:

**Line-by-line translation.** Blocked outright by G3 for anything that is a
member function, and by `error[E0425]` for the free-function workaround the
emitter itself suggests — a free `fn` taking `self_: &RaftServer` emits correct
C++ but never typechecks as Rust, because `RaftServer` is not a Rust type. That
route produces Rust-shaped text that exists only to generate C++, abandoning the
verification that is the point. MEASURED both halves.

**`c2rust`-style mechanical transpilation.** Aimed at C, produces `unsafe` Rust,
and this project already has a transpiler with a different contract. Not
applicable.

**`cxx` / `autocxx` FFI bridge.** The right tool if the goal were two
independently-compiled languages linked at a checked boundary. It is not this
project's model: rusty-cpp generates C++ *from* Rust, and production compiles
the generated C++. Worth revisiting only if the project ever moves to real
separate compilation.

**Strangler fig at the process boundary.** Stand up a Rust Raft beside the C++
one and migrate traffic. Too coarse: it discards the existing test suite, the
existing wire compatibility, and ten years of behaviour encoded in
`RaftServer`, and there is no traffic-routing layer to strangle behind.

**Rewriting against `tikv/raft-rs` wholesale.** Tempting because that crate is
the thing this code would eventually resemble. Rejected because it is a
replacement, not a migration: different wire format, different storage trait,
different apply model, and no incremental path from here to there. The *shape*
is worth borrowing; the crate is not.

**Convert the data structures first (`std::map` → `rusty::BTreeMap` everywhere).**
Plausible, and CLAUDE.md does ask for rusty containers over STL. But on its own
it converts no logic and would touch every function in the class at once — the
opposite of incremental. Do it opportunistically inside each extracted type
instead.

## 5. The plan

Each step is: a C++ refactor that is behaviour-preserving and verifiable with
the existing net, followed by a DSL conversion of the type that refactor
created. Each step is independently committable and independently revertible.
No step requires the next one to be worthwhile.

### Step 1 — `HeartbeatAuthority` (the read-index quorum evidence)

**Why first.** It is the smallest genuinely-stateful thing in the loop, it is
pure, and its logic *already* calls converted DSL predicates.

MEASURED — the existing struct is four fields, one scalar and three sets of
scalars:

```cpp
struct PendingHeartbeatAuthority {
  uint64_t term;
  std::set<siteid_t> config;
  std::set<siteid_t> voters;
  std::set<siteid_t> outstanding;
};
```

and the operations on it, scattered across PHASE 1/2/3, are set arithmetic:
`outstanding.insert`, `outstanding.erase`, `voters.insert`, `voters.size()`
against a majority of `config.size()`, and a term equality check. Two of those
comparisons already go through DSL predicates
(`raft_server_read_index_reply_confirms_authority`,
`raft_server_read_index_quorum_reached`).

**The refactor.** Give the struct its methods: `launch(site)`, `retire(site)`,
`record_vote(site)`, `has_quorum()`, `is_complete()`. Move the inline set
manipulation at `server.cc:2461-2462`, `2506-2519`, `2667-2678` and `2732-2734`
into them. No behaviour changes; the same sets get the same elements.

**The conversion.** A DSL `pub struct` with `rusty::BTreeSet<u16>` fields and
five methods. `rusty::BTreeSet` is a real rusty runtime type, so it emits as
itself and needs no alias — UNTESTED, and the check is one transpiler run.
Converting `std::set` → `rusty::BTreeSet` here is the opportunistic container
migration CLAUDE.md asks for, confined to one type.

**Verify.** `raftLabTest` exercises read-index confirmation; log integrity and
the flap test cover the loop. Perf: throttled point only — this is not on the
per-entry path.

**Unlocks.** The first DSL-owned type carved out of `HeartbeatLoop`, and the
first use of a rusty container in raft.

### Step 2 — `PeerProgress` (`next_index_` / `match_index_`)

**Why second.** It is the cluster with the fewest external touchers, it is the
site of the one genuine aliasing hazard in the file, and it is where a container
change pays for itself.

**The refactor, in two parts.**

*(a)* Replace the two `std::map<siteid_t, uint64_t>` with one type holding a
dense, ordinal-indexed table. MEASURED: the replica set is fixed —
`current_config_` is established at configuration and `HeartbeatLoop` asserts
`match_index_.size() == expected` on entry. So a `Vec<u64>` indexed by peer
ordinal is behaviour-identical, is trivially spellable in the DSL, and removes
the map-cursor problem at its root rather than working around it as the
`next_index_[site_id]` fix currently does.

*(b)* Move the per-follower index arithmetic — the rewind-on-reject, the
snapshot-boundary clamp, the `next_index_ <= lastLogIndex` batching decision —
onto that type. These are already partly DSL predicates
(`raft_server_append_reject_halved`, `..._decremented`, `..._floor`,
`raft_server_follower_next_index`), currently called from inline C++.

**Conversion.** DSL `pub struct` over `rusty::Vec<u64>`, with the index
arithmetic as methods.

**Verify.** This one is on the hot path: throttled point *and* saturation point,
n=6 each, per the measurement discipline in
[`cpp-refactor-progress.md`](cpp-refactor-progress.md).

**Unlocks.** Removes OWN-03, the one aliasing hazard with no Rust spelling, by
deleting the construct rather than avoiding it.

### Step 3 — `CommitDecision` (PHASE 0 and PHASE 3)

**Why third.** Both phases compute the same thing from the same inputs, and it
is the purest computation in the loop.

MEASURED, PHASE 0's decision: collect `match_index_` values, sort, take the
median at `(nservers-1)/2`, clamp to `lastLogIndex`, accept only if the entry at
that index is from the current term.

**The refactor.** One function:

```
commit_index_from_match(matched: &[u64], nservers: usize,
                        last_log_index: u64, current_commit: u64) -> u64
```

pure, total, with the term check left outside because it needs a log lookup.
PHASE 3 calls the same function. This deletes a duplicated computation.

**Conversion.** Directly convertible today — it is a free function over scalars
and a slice. After Step 2 it becomes a method on `PeerProgress`, which is
better, because the matched indices are that type's data.

**Verify.** Commit-index errors are exactly what the log-integrity oracle
catches: a wrong commit index shows up as a gap or a duplicate.

### Step 4 — `HeartbeatRound` (the loop's own state object)

**Why fourth.** By now three of the loop's concerns are types, and what remains
of its loop-carried state is nameable.

**The refactor.** Promote the three stack locals — `pending_rpcs`,
`authority_rounds`, `pending_leader_term` — into a struct, with the
round-lifecycle methods that currently sit inline: `begin_round`,
`abandon_on_leadership_loss` (the `clear()` triple at 2076/2088), `retire_round`.

**Conversion.** Partially. `authority_rounds` is a map of Step 1's type and
converts. `pending_rpcs` holds `shared_ptr<AppendEntriesResponse>` and
`janus::Command`, which are rrr wire types — those stay C++ per CLAUDE.md's
boundary rule, reached through an `@unsafe` kernel.

**Unlocks.** After this, `HeartbeatLoop`'s body is orchestration over four named
types plus two RPC sends. INFERRED from the phase boundaries: that should be a
few hundred lines rather than 789.

### Step 5 — the split proper

Only now is the sans-I/O split a small change rather than a rewrite. Divide what
remains into:

- **`HeartbeatDecision`** — given the four state types and the current term,
  produce a description of what to send: per-peer, either a heartbeat, an
  entry batch, or a snapshot. Pure. DSL-owned.
- **the driver** — take that description and call `SendAppendEntries2` /
  `SendInstallSnapshot`, poll the futures, hand replies back. C++, `@unsafe`,
  two RPC calls.

**This is the step that makes differential testing possible**, and that is worth
more than the conversion. A pure decision function can be run *alongside* the
existing inline logic on every heartbeat round in the existing test suites, with
the outputs compared and a mismatch reported. Cheap, and far stronger evidence
than "the tests still pass". UNTESTED as a mechanism here; it requires only a
temporary comparison shim in the C++ driver.

### Step 6 — the RPC handlers, in order of entanglement

`OnRequestVote` (96 lines, 4 clusters) → `OnAppendEntries` (297, 4) →
`OnInstallSnapshot` (312, 5). By this point `election`, `log-store` and
`peer-progress` are types, so each handler is orchestration over them, and the
same decide/apply split applies. `RequestVoteImpl` (221 lines) goes with
`OnRequestVote`.

### Step 7 — what is left

`RaftServer` becomes a shell: the lifecycle flags, the owned component types,
and the thread/fiber plumbing. At that point the question "can `RaftServer`
itself be a DSL struct?" is worth re-asking, because its field count and field
*types* will both have collapsed. Not before.

## 6. Verification, per step

Non-negotiable, because every step touches consensus:

- `bash scripts/raft_dsl.sh --check` — green
- `./ci/ci.sh raftLabTest` — 25/25
- log integrity at 1 partition and at 6 partitions/multi — zero gaps,
  duplicates, out-of-order
- the flap test — survivors gap-free after the leader is SIGKILLed
- performance: the throttled point for latency; **the saturation point for
  throughput**, because a throttled run reports the offered rate on both sides
  and says nothing about capacity
- read the noise floor, not just `compare.py`'s exit code: a delta can clear the
  floor while staying under the 5% threshold, and three changes this session
  read above the floor at n=3 and fell back inside it at n=6

Two gaps that exist today and should be closed before, not after, this work
begins: **Paxos has been modified and never tested** (`simplePaxos`,
`shard1Replication`, `shard2Replication` and the Simple variants have not been
run), and **the Raft production path through `dbtest`** (`shard1ReplicationRaft`
and siblings) has not been run either. Both matter here because Steps 1-5 change
the leader's replication path.

## 7. What could go wrong

**The recursive mutex.** Steps 1-4 each move state out of `mtx_`'s protection
into a type. That is safe only while the type is still reached under the same
lock. Do not opportunistically make anything non-recursive along the way: there
are 24 nested re-acquisitions in Raft, and every one needs the treatment Tranche
4b gave to four of them — proving by caller enumeration that the lock is already
held. A missed one is a deadlock that neither the bench nor `raftLabTest` is
guaranteed to reproduce.

**`rusty::BTreeSet` / `rusty::Vec` in a hot path.** Step 2 is on the per-entry
path. The container change is justified on expressiveness grounds, but it must
be measured at saturation, not assumed. The `next_index_` cursor fix is the
precedent: plausible mechanism, measured at −0.70% against baseline, no change
needed.

**Extraction that is not behaviour-preserving.** The failure mode to watch is a
set or index being updated at a different *time* rather than with a different
value — e.g. retiring an authority round before rather than after commit
recalculation. `server.cc:2729` carries an explicit comment that publication
must follow recalculation. Preserve orderings even when they look incidental.

**Scope creep into `RaftCommo`.** It is tempting because `RaftCommo :
Communicator` looks like the `TxLogServer` case. It is not: MEASURED,
`Communicator` is 101 header lines plus 191 of implementation with real
behaviour that both engines use, where `TxLogServer` had six fields and zero
behavioural virtuals. `RaftCommo` is not on this path and should not be folded
into it.
