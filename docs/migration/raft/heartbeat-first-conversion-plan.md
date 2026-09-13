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
functions first" does not work. But it does not need to be converted in one
piece either. Three measurements change the shape of the problem:

1. Its 64 data members fall into seven cohesive clusters, and **30 of its 46
   stateful functions touch zero or one cluster**. Only seven entangle four or
   more, and those seven are exactly the protocol entry points plus
   `HeartbeatLoop`.
2. **All of consensus runs on one poll thread**, as cooperative fibers, with
   three steady-state suspension points and three cross-thread edges. The
   recursive mutex is defending those three edges, not forty-eight races.
3. `src/rrr` has already made this migration and its three inheritance patterns
   — trait, one-field Shim, accessor trait — are in production in this
   repository and directly reusable.

Together these say: lift the decisions out of `HeartbeatLoop` as pure types the
DSL can own — **which need no locks at all**, because they run on one thread —
leave the two RPC sends in C++ kernels, and let the loop shrink to
orchestration. That is the **sans-I/O split**, and G3 is satisfied by
construction, because a lifted state machine is a new whole type.

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

## 3.5 The concurrency model: what is actually non-deterministic

This section exists because the obvious reading of the code — 48 `mtx_`
acquisitions, a recursive mutex, fibers, threads — suggests pervasive
concurrency, and that reading is wrong. The consensus path is a **cooperative,
single-threaded state machine** with three suspension points and three
cross-thread edges. Knowing exactly where the non-determinism enters is what
makes the rest of this plan tractable, and it is what makes deterministic
testing possible later.

### One poll thread runs all of consensus

MEASURED, by following the handles:

```
raft_worker.cc:337   svr_poll_thread_worker_ = PollThread::create()
raft_worker.cc:343     -> rpc_server_          (hosts Vote/AppendEntries/InstallSnapshot)
raft_worker.cc:372     -> CreateCommo(clone)   (so commo()->PollThread() is the same thread)
server.cc:1623         -> BindReplicationWakeOwner(commo()->PollThread())
server.cc:1683/1702    -> Fiber::create_run(HeartbeatLoop), Fiber::create_run(StartElectionTimer)
```

`rrr::PollThread` is **one OS thread** — `reactor.rs:2076` holds a single
`join_handle_` and one `poll_thread_id_bits_`, and `create()` "spawns the worker
thread", singular.

So `HeartbeatLoop`, `StartElectionTimer`, and every RPC handler run as fibers on
the *same* thread. `service.h:38` confirms the handler model: "The rrr codegen
wraps each one in a `Fiber::create_run`".

The second poll thread, `svr_hb_poll_thread_worker_g`
(`raft_worker.cc:495`), is a red herring: it hosts `ServerControlServiceImpl`,
the benchmark/CI control plane. It never touches Raft state.

### Fibers are cooperative, so the code runs to completion

A fiber is not preempted. Between suspension points, a Raft operation is atomic
with respect to every other Raft operation on that thread. If `stepDown()` is
running, nothing else runs; if an RPC handler is mid-flight, it finishes.

MEASURED — every suspension point in production `server.cc`, with its owner:

```
line 290,312,468,485   ReplicationWakeGate        wait_timeout   <- the heartbeat-interval wait
line 2687              HeartbeatLoop              Fiber::sleep
line 2941              RequestVoteImpl            wait_timeout   <- waiting for a vote quorum
line 1337,1342         PrepareForShutdown         sleep          (shutdown only)
line 1578              StartApplyThread           sleep          (startup only)
line 1683-1709         SetupInternal              create_run x4  (startup only)
line 3160,3212         StartElectionTimer         create_run, sleep
```

Discounting startup and shutdown, the **steady-state consensus path has three
suspension points**: the wake gate's interval wait, one `Fiber::sleep` inside
`HeartbeatLoop`, and one `wait_timeout` in `RequestVoteImpl`.

### The three cross-thread edges

MEASURED. Everything that touches Raft state from *off* the poll thread:

| edge | who | how it reaches state |
|---|---|---|
| **submit** | a client/worker thread | `RaftWorker::Submit` (`raft_worker.cc:776`) calls `raft_server->Start(...)` **inline**, not via a hop; `StartImpl` takes `mtx_` |
| **apply** | `apply_thread_`, a `std::thread` | pops `apply_queue_` under `apply_queue_mtx_`, applies under `state_machine_apply_mtx_`, and reaches `mtx_` through exactly two functions: `PublishAppliedIndex` and `MaybeCreateSnapshot` |
| **lifecycle** | worker / test threads | `PrepareForShutdown`, `Disconnect` |

That is the entire non-deterministic surface. Three edges.

### What follows from this

**The mutex defends three edges, not forty-eight races.** Of the 23 functions
that take `mtx_`, the consensus ones — `HeartbeatLoop` (5), `RequestVoteImpl`
(2), `StartElectionTimer` (2), the three RPC handlers (1 each) — all run on the
*same* thread and therefore never contend with one another. They are defending
against the submit edge and the apply edge only. This is the strongest available
evidence that the 24 nested re-acquisitions are safe to remove: most of them are
not synchronising anything.

**The wake gate is already the model answer for one edge.** `ReplicationWakeGate`
exists precisely because submit happens on a foreign thread and the loop waits
on the poll thread. It converts a shared-state problem into a message-passing
one: publish a flag, queue a job onto the owner `PollThread`, let the owner act.
INFERRED: the apply edge could be given the same treatment, and the apply queue
is already half of it.

**The sans-I/O core needs no locks at all.** If the state machine only ever runs
on the poll thread, a lifted decision type is single-threaded by construction —
no `Mutex<RaftState>`, no interior mutability, plain `&mut self`. The lock stays
at the *edges*, guarding the handoff, not inside the core. That is a materially
simpler target than the one this plan's predecessor described.

**Deterministic simulation testing becomes reachable.** Given one thread, three
suspension points and three edges, the execution schedule is a small, seedable
object: the order in which the poll thread delivers events plus the choice at
each suspension point. This is the property FoundationDB and TigerBeetle exploit,
and `madsim` provides in Rust. UNTESTED here, and not a near-term step, but it
is worth not designing it away — every edge converted from shared state to a
queued message makes the schedule more explicit and the simulation more faithful.

## 4. How `rrr` solved inheritance, with its actual code

`src/rrr` is the only part of this repository that has completed the migration,
so its answers are precedent rather than theory. MEASURED: 15 `#[cpp_inherit]`
uses across 7 files. They fall into three patterns, and Mako needs all three.

### Pattern 1 — interface inheritance becomes `pub trait` + `#[cpp_inherit]`

The base is declared as a trait with **no state**, in its own module, and
implementors attach it with an empty impl:

```rust
// src/rrr/reactor/reactor.rs:217
pub trait EventPollable {
    fn test(&self) -> bool;
    fn is_ready(&self) -> bool;
    fn status(&self) -> EventStatus;
    ...
}

// six implementors: IntEvent, NeverEvent, TimeoutEvent, WaitAny, WaitAll, QuorumEvent
#[cpp_inherit]
impl EventPollable for IntEvent {
    fn test(&self) -> bool { event_test_impl(self) }
    ...
}
```

emitting `export struct IntEvent : public EventPollable` in
`rrr.reactor.cppm`. Polymorphism travels as `Box<dyn Trait>` / `Rc<dyn Trait>`,
which the type map lowers to a C++ base pointer —
`rust-type-map.toml:5` maps `LegacyChannelConnectionBase` to
`rrr::ChannelConnectionBase`, and `fiber_channel.rs:42` declares
`type LegacyChannelConnectionBase = dyn ChannelConnectionBase;`.

**Mako already has this**, as of `5b5825659`: `TxLogServer` is a `pub trait` and
both engines implement it. The one difference is that rrr declares the trait in a
*different module* from its implementors and this works fine in crate mode; in
inline mode the same thing needs the glob-import rule recorded in
[`cpp-refactor-progress.md`](cpp-refactor-progress.md).

### Pattern 2 — implementation reuse becomes a one-field Shim

When the thing being adapted is a real object with its own state, rrr does **not**
make that object inherit. It puts the inheritance in a separate, trivial type:

```rust
// src/rrr/rpc/tcp_channel.rs:238
struct TcpChannelShim {
    conn_: Arc<TcpConnection>,          // composition: holds the real object
}

#[cpp_inherit]
impl ChannelConnectionBase for TcpChannelShim {   // inherits ONLY the interface
    unsafe fn send_frame(&mut self, frame: &ChannelFrame) -> ChannelError {
        unsafe { self.conn_.send_frame(frame) }   // forwards
    }
    fn flush(&mut self) { self.conn_.flush() }
}

Box::new(TcpChannelShim { conn_: conn })          // trait object == C++ base pointer
```

MEASURED — **all eight shims have exactly one field**:

```
TcpChannelShim           conn_: Arc<TcpConnection>
TcpPollableShim          conn_: Arc<TcpConnection>
TcpListenerChannelShim   listener_: Arc<TcpListener>
TcpListenerPollableShim  listener_: Arc<TcpListener>
TcpFactoryShim           factory_: Arc<TcpFactory>
InMemoryChannelShim      conn_: Arc<InMemoryChannel>
InMemoryListenerShim     listener_: Arc<InMemoryListener>
InMemoryFactoryShim      factory_: Arc<InMemoryFactory>
```

The real implementations — `TcpConnection`, `TcpListener`, `InMemoryChannel` —
inherit **nothing**. They are plain structs. The shim absorbs the inheritance so
the implementation does not have to.

**Why this matters for Mako.** `RaftServer` currently implements `TxLogServer`
directly. Under this pattern it would not: a one-field `RaftServerShim` would
carry the interface and forward, leaving `RaftServer` a plain struct with no base
at all. That removes one constraint from the eventual big conversion, and it is
the difference between "convert a type that must also satisfy an interface" and
"convert a type".

### Pattern 3 — shared base *state* becomes duplicated fields plus an accessor trait

This is the one that answers the question C++ programmers actually ask: if the
base held data, where does it go? rrr's answer is to **duplicate the fields into
every implementor** and recover the shared *code* through a trait of accessors
plus generic free functions.

MEASURED — `IntEvent`, `TimeoutEvent` and `QuorumEvent` have 7, 7 and 18 fields
respectively, and all three begin with the same ones (`status_: Cell<EventStatus>`,
`owner_thread_: rusty::thread::ThreadId`, ...). That is the old C++ `Event`
base's state, copied three times. Then:

```rust
// src/rrr/reactor/reactor.rs:229
trait EventCore: EventPollable {          // supertrait: an EventCore is an EventPollable
    fn core_status(&self) -> &Cell<EventStatus>;
    fn core_owner_thread(&self) -> rusty::thread::ThreadId;
    fn core_state(&self) -> &EventState;
    fn core_state_mut(&mut self) -> &mut EventState;
    fn core_self(&self) -> &Weak<dyn EventPollable>;
    fn core_self_mut(&mut self) -> &mut Weak<dyn EventPollable>;
    fn core_is_composite(&self) -> bool;
}

fn event_core_set_self<W: EventCore>(ev: &mut W, p: Weak<dyn EventPollable>) {
    *ev.core_self_mut() = p;
}
fn event_core_wakeup_time<W: EventCore>(ev: &W) -> u64 {
    ev.core_state().wakeup_time_.get()
}
```

MEASURED: 30 call sites use that shared behaviour. **Shared code without shared
storage** — which is exactly what C++ implementation inheritance provides and
Rust does not.

**This is the pattern to adopt for `TXLOG_SERVER_SITE_FIELDS`.** Tranche 6
duplicated `TxLogServer`'s six fields into both engines — the same choice rrr
made — but did it with a **C preprocessor macro**. The macro is a C++-only
device that must be deleted at conversion time. `EventCore` is the form that
survives into Rust. See Step 0 below.

## 5. Other approaches, and why they are not the plan

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

## 6. The plan

Each step is: a C++ refactor that is behaviour-preserving and verifiable with
the existing net, followed by a DSL conversion of the type that refactor
created. Each step is independently committable and independently revertible.
No step requires the next one to be worthwhile.

### Step 0 — replace `TXLOG_SERVER_SITE_FIELDS` with an accessor trait

**Why before everything else.** It is small, it is already load-bearing, and it
is the only piece of this migration currently written in a form that cannot
survive into Rust.

Tranche 6 duplicated `TxLogServer`'s six fields into `RaftServer` and
`PaxosServer` through two C preprocessor macros in `scheduler.h`. Duplicating
was the right call — rrr made the same choice for `EventState` — but the macro
is a C++-only device. `rrr`'s Pattern 3 is the form that converts:

```rust
// the shape to move toward, modelled on reactor.rs:229
pub trait ReplicationSite: TxLogServer {
    fn site(&self) -> &ReplicationSiteContext;
    fn site_mut(&mut self) -> &mut ReplicationSiteContext;
}

fn replication_site_id<W: ReplicationSite>(s: &W) -> u16 { s.site().site_id_ }
```

Each engine declares a `ReplicationSiteContext` member and implements two
accessors; the shared behaviour becomes free functions generic over the trait.
Note the supertrait bound `: TxLogServer`, which is exactly how `EventCore`
relates to `EventPollable`.

**The cost this trades against.** The macro was chosen precisely to avoid
renaming 164 `site_id_`, 20 `partition_id_`, 10 `loc_id_` and 6 `app_next_` uses
to `site_.site_id_` and so on. An accessor trait reintroduces that rename —
~200 mechanical edits. It is worth paying once, deliberately, rather than
discovering at conversion time that the macro has to be unwound anyway. But it is
the single largest mechanical diff in this plan, and it produces no behaviour
change whatsoever, which makes it a good candidate for a commit of its own.

**Verify.** Compilation is nearly the whole test; every rename is checked by the
compiler. Then the standard suite. No performance run needed: field access
through an inlined accessor on a member struct is the same load.

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

**The conversion.** A DSL `pub struct` with five methods.

**On the container: MEASURED by experiment, and my original estimate here was
wrong in three ways.** This section previously said `rusty::BTreeSet` "is a real
rusty runtime type, so it emits as itself and needs no alias — UNTESTED, and the
check is one transpiler run". Running it:

| claim | verdict |
|---|---|
| emits verbatim as `rusty::BTreeSet<uint16_t>`, no alias | **true** |
| it is a header type like `rusty::Mutex` | **false** — it is a C++20 module. `rusty.hpp:79` says the headers no longer provide it; `rusty.cppm:184` aliases `::btree_port::btree::set::BTreeSet` into namespace `rusty` |
| nothing else is needed | **false** — the TU must `import rusty;` |
| mixing `import rusty;` with `#include <rusty/*.hpp>` will be trouble | **false** — server.cc already does `import std;`, and adding `import rusty;` alongside the rusty headers compiled cleanly |
| "the check is one transpiler run" | **false** — it was four build cycles |

`rusty::BTreeSet` does carry everything this type needs: `insert`, `remove`,
`contains`, `len`, `clear`.

**A general tax, discovered three times now.** The emitter freely generates calls
to free functions in namespace `rusty` and gives no indication which header
declares them. Each one costs a build cycle to find:

```
rusty::clone     -> #include <rusty/move.hpp>
rusty::len       -> #include <rusty/array.hpp>
rusty::contains  -> #include <rusty/array.hpp>
```

The diagnostic is good (`missing '#include "rusty/array.hpp"'`) but arrives one
at a time. Budget for it in every step that touches a rusty container.

**Whether the container migration belongs in this step is an open question.**
Extracting the type and swapping `std::set` for `rusty::BTreeSet` are separable,
and bundling them means a performance or behaviour change cannot be attributed
to one or the other. The extraction is the point; the container swap is
opportunistic. Recommend splitting unless there is a reason not to.

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

  **And lock-free.** Per §3.5, everything in this core runs on the single poll
  thread, so it needs no `Mutex`, no interior mutability, and no
  `Cell`/`RefCell` — plain `&mut self`. The locking stays at the three edges,
  guarding the handoff. This is the point where the concurrency model pays off:
  the core is not "a state machine behind a mutex", it is a single-threaded
  state machine, and the mutex was only ever protecting the edges.
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

### Step 7 — what is left, and the Shim that makes it easier

`RaftServer` becomes a shell: the lifecycle flags, the owned component types,
and the thread/fiber plumbing. At that point the question "can `RaftServer`
itself be a DSL struct?" is worth re-asking, because its field count and field
*types* will both have collapsed. Not before.

**And when it is asked, apply rrr's Pattern 2.** `RaftServer` currently
implements `TxLogServer` directly, which means converting it requires converting
a type that must *also* satisfy an interface. rrr's shims show the alternative:

```rust
struct RaftServerShim { inner_: Arc<RaftServer> }   // one field, like all eight rrr shims

#[cpp_inherit]
impl TxLogServer for RaftServerShim {
    fn set_commo(&mut self, commo: *mut rusty::Communicator) {
        self.inner_.set_commo(commo)                 // forwards
    }
    ...
}
```

`RaftServer` then inherits nothing and is a plain struct — strictly easier to
convert, and the interface obligation lives in a three-method forwarder. The
workers' `TxLogServer*` becomes a pointer to the shim, which is exactly how
`Box<dyn ChannelConnectionBase>` already works in rrr.

INFERRED, not measured: this is worth doing only if the interface obligation is
actually in the way. If `RaftServer` converts cleanly while implementing
`TxLogServer` directly, the shim is a layer of indirection for nothing.

## 7. Verification, per step

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

## 8. What could go wrong

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
