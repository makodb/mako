# `src/deptran/raft` C++-toward-Rust refactor — what was done, and what it proved

Companion to [`cpp-refactor-plan.md`](cpp-refactor-plan.md), which this session
executed. The plan states what to do and why; this file states what actually
happened, what the evidence was, and what is left. Where the plan turned out to
be incomplete, that is recorded here rather than quietly worked around.

Read the plan first. This file assumes its vocabulary: gates G1–G4, tranches
0–6, and the three priorities (correctness and performance intact; then
expressiveness; then memory safety, never at expressiveness's cost).

## Status at a glance

| tranche | state | evidence |
|---|---|---|
| 0a — harness as a correctness oracle | **done** | detects a single lost entry in 12 000; clean on every good run |
| 0b — leadership flap | **done** | leader SIGKILLed mid-load, both survivors gap-free |
| 0c — vote-fix regression | **done** | 6 partitions / multi mode, 18 270 entries per survivor, clean |
| 0d — decide about `RAFT_TEST` | **done, wired on** | `ci/ci.sh raftLabTest` runs green end to end on the post-Tranche-4 tree: 25/25 cases, exit 0 |
| 1 — unlock the type gate (G1) | **done** | a foreign type now resolves; bare `rustc` still cannot |
| 2 — unlock header carriers (G2) | **done** | ODR post-pass ported; no-op on the tree at the time |
| 3 — convert `ReplicationWakeGate` | **done** | first non-scalar conversion; correctness + perf checked |
| 4 — free deletions | **done** | dead lease machinery and four vacuous re-acquisitions |
| 6 core — dissolve inheritance (G4/B7) | **done** | `TxLogServer` is now a pure interface; 25/25 lab cases, perf within noise at n=6 |
| B1 — ownership triangle | **done, folded into 6** | frame no longer co-owns the scheduler; worker is sole owner |
| 5 — de-reentrancy proper (B2) | not started | unblocked by 6; see "What is left" |
| 6 step 2 — `RaftServer` into the DSL | **blocked by the pinned transpiler** | `#[cpp_inherit]` silently emits no base under pin `77c3ad5a` |

## Tranche 0 — the correctness net

### 0a. The sequence oracle

`src/deptran/raft/raft_bench.cc` already wrote a per-partition sequence number
into every payload and never read it back. It does now: the apply callback
compares each applied entry's sequence against the last one it saw, which gives
**ordering, no-loss and no-duplicate per partition in one pass**. Four new
counters (`out_of_order`, `gaps`, `duplicates`, `probes_applied`) reach the JSON
record, and a non-zero value in any of them — or in the pre-existing
`foreign_applied` — exits **8** from the driver and from
`examples/raft_bench.sh`.

Three decisions inside it worth knowing:

- **Both roles check.** The callback is registered for leader and follower
  alike, so a follower's verdict is evidence about *replication*, not just
  about the leader's local apply. In the flap test the followers are the only
  witnesses left.
- **The verdict is assigned last**, after every performance verdict, and
  unconditionally. A number measured over a log that lost an entry is not a
  slower number, it is a wrong one, so it must not be overwritten by exit 3 or
  exit 6.
- **Sequence 0 is a leadership probe** and is counted separately rather than
  folded into the ordered stream; probes are offered repeatedly before the
  window opens and carry no ordering information.

It deliberately cannot see truncation at the *end* of the stream — entries in
flight when the drain window closes leave no gap between two applied entries.
That is what `offered_total` versus `applied_total` is for, and it is precisely
the property that makes the same check usable across a leadership flap.

**Proof that it fires.** A one-line fault injection (`if (seq == 500) ++seq;`
in the offer loop) was compiled in, run, and reverted. The run reported
`out_of_order=1 gaps=1 duplicates=0` and exited 8 — one lost entry out of
12 000 detected. Without that step the oracle would be untested code claiming
to be a test.

### 0b. Leadership flap

`examples/raft_bench.sh --kill-leader-at-sec S` waits for a process to announce
that it is offering load, lets it offer for S seconds, then **SIGKILL**s it.
SIGKILL, not SIGTERM: a clean shutdown proves nothing about what a replica does
when the leader vanishes mid-flight.

The leader is discovered from the logs rather than assumed to be `localhost` —
a test that kills the wrong process and then finds the survivors clean would
pass without ever having removed a leader. The killed process's 137 is excluded
from the worst-child status, and no record is expected, because the process
that would have written one is the one that was killed. The assertion is
entirely on the survivors: each must report a gap-free, duplicate-free applied
prefix.

Measured, 1 partition, 2000/s, killed 5 s in: both survivors applied 10 409
entries with zero gaps and zero duplicates.

### 0c. The vote-fix regression

The same mechanism at `--partitions 6 --group-mode multi` is the regression
test the plan asked for: in multi mode with six partitions a leader must be
elected for all six. Measured after the Tranche 3 conversion: both survivors
applied 18 270 entries each, clean, across six partitions, with the leader
SIGKILLed 6 s into the load.

### 0d. `RAFT_TEST` — dormant no longer

The plan asked for a decision. The decision is **wire it on**, because the
suite turned out to work: configured with `-DMAKO_USE_RAFT=ON -DRAFT_TEST=ON`,
`deptran_server` builds and `RaftLabTest` runs **25 cases, 0 failures, in about
90 seconds**. It is now `./ci/ci.sh raftLabTest`, in its own build directory
(RAFT_TEST defines `RAFT_TEST_CORO`, which changes `RaftServer`'s behaviour and
must not contaminate the build every other suite measures), and it is part of
`ci.sh all`.

The suite was re-run through `ci/ci.sh raftLabTest` after every change in this
work had landed — including Tranche 4's deletions, which `test.cc` and
`testconf.cc` compile against — and reported "25 case(s) passed,
deptran_server exited 0".

One thing had to be fixed first. `RaftFrame::RaftLabProcessExitCode()` — and
the DSL predicate behind it, `raft_frame_lab_process_exit_code` — existed with
**zero callers**, so `main` returned `SUCCESS` no matter what the lab fiber
concluded: a run printing "TESTS FAILED" exited 0. `src/deptran/s_main.cc` now
returns it. The CI check tests both the exit status *and* the "ALL TESTS
PASSED" marker, the latter proving the fiber reached a verdict at all rather
than the process exiting 0 having run nothing.

Worth correcting one impression the plan leaves: Raft is not otherwise
untested in CI. `shard1ReplicationRaft` and its three siblings run real Raft
replication through `dbtest`, because `replication_helper.cc` compiles both
helpers and dispatches at runtime — `MAKO_USE_RAFT` is not needed for that
path. What was missing is cluster-level coverage of `RaftServer` itself, which
is what `RaftLabTest` supplies.

## Tranche 1 — the type gate (G1), closed

Two changes, no production C++:

1. `src/deptran/raft/Cargo.toml` now depends on `rusty = { path =
   "../../rrr/rusty-rustc" }`, mirroring `src/rrr/Cargo.toml:14`.
2. `scripts/raft_dsl.sh` compiles the extracted crate through **Cargo** instead
   of bare `rustc`. Without this, step 1 is inert: bare `rustc` reads no
   manifest, so it resolves no dependency.

The plan's step 3 — add opaque rustc models for `IntEvent` and `PollThread` —
turned out to be already done upstream: the facade publishes them as
`rusty::ReactorIntEvent` and `rusty::ReactorPollThread`
(`src/rrr/rusty-rustc/src/lib.rs:337-338`). Only one method had to be added,
`IntEvent::wait_timeout`, which the raft gate waits on and the model lacked.

**Proof the gate is actually open**, rather than merely "`--check` is still
green": a scratch module naming
`rusty::Mutex<rusty::Option<rusty::sync::Arc<rusty::ReactorPollThread>>>`
compiles under Cargo and dies at `error[E0433]: cannot find module or crate
'rusty'` under bare `rustc`. That is G1, exactly as the plan described it, and
it is now closed.

## Tranche 2 — header carriers (G2), unlocked

The 29-line ODR post-pass from `scripts/regen_storage_dsl.sh:72-100` is ported
into `scripts/raft_dsl.sh` and applied on both sides of the `--check`
comparison.

One wrinkle the plan did not anticipate. Unlike the storage script, raft_dsl.sh
also runs the emitter's own `inline-rust --check`, which compares a committed
GEN region against what the emitter renders — and the emitter does not render
the `inline ` this pass adds. Feeding it the committed file would report drift
for every post-passed definition. So the check now runs against copies with the
pass **undone** (`odr_strip_pass`), keeping both gates: the emitter's
source-hash check and the full-file diff. The one-way hazard in that inverse is
documented at the function.

On the tree as it stood, the pass changed **zero lines across all seventeen
carriers** — every existing block is a free `const fn` that already emits as
`constexpr` or as an indented class-body member. That is exactly the gate §6
asks for: generated C++ byte-identical apart from `inline` prefixes, of which
there were none. It stopped being a no-op the moment Tranche 3 landed, which is
the point of installing it first.

## Tranche 3 — `ReplicationWakeGate`, the first real conversion

~220 lines of hand-written C++ replaced by a DSL `pub struct` plus an
`impl` block with fourteen methods and nine fields, and the C++ the emitter
generates from it. This is the first conversion in `raft/` that is not a scalar
predicate and the first that proves `impl` at all.

### What the plan did not account for

The plan said the type "needs only `rrr::PollThread` and `IntEvent`". It also
**creates** `IntEvent`s, and the reactor factory `::rrr::create_sp_int_event`
has no DSL spelling: the facade exposes it only as
`rusty::rrr::reactor::create_sp_int_event`, and inline mode has no `--type-map`
to rewrite that path (crate mode does; run `inline-rust --help` and there is no
such flag).

Rather than invent a nested `rusty::rrr::reactor` namespace in C++ merely to
hold a factory, the two wait entry points take the event as a **parameter** and
`RaftServer` creates it. That is the split CLAUDE.md prescribes — the DSL owns
the shape, C++ owns the surgery — and it is the only behavioural seam in the
conversion; every other body is a statement-for-statement transcription.

**The fast path is preserved exactly.** `begin_wait_for_work()` returns
`Some(answer)` when it could decide without arming a waiter and `None` when the
caller must arm one, so no event is allocated on the path that allocated none
before. `WaitForElectionTimeoutOrShutdown` keeps its `accepting()` check ahead
of the factory call for the same reason.

### The other half: a C++-side type map

Inline mode having no `--type-map` means the two languages must agree on one
spelling, and Rust's side is fixed by the facade crate. So
`src/deptran/raft/rust_facade_types.h` supplies the other side: two aliases,
`rusty::ReactorPollThread = ::rrr::PollThread` and `rusty::ReactorIntEvent =
::rrr::IntEvent` — the same two pairs `src/rrr/rust-type-map.toml` already
declares, written in C++ because inline mode has nowhere else to put them. This
is reusable infrastructure, not a one-off: every future raft conversion that
names a foreign type goes through it.

### Three emitter behaviours worth recording

They cost real time to find, and the next conversion will hit them:

1. **A `let` with no type annotation loses pointer-ness.** `let waiter =
   x.clone()` emits `const auto waiter = ...` and then `waiter.as_ref().unwrap().set(1)`
   — a dot on a `rusty::Arc`, which does not compile. With the type spelled
   out, the same expression emits `->set(1)`. The annotations in
   `wake_on_owner` and `wake_shutdown_on_owner` are load-bearing.
2. **`if let` does not deref through an `Arc`.** Clippy's suggested
   `if let rusty::Some(event) = &waiter { event.set(1) }` transpiles cleanly and
   then emits `event.set(1)` on an `Arc`. Hence the `#[allow(clippy::unnecessary_unwrap)]`
   on those two methods, written with a TODO naming exactly what to re-test
   before removing it.
3. **`rusty::clone` needs its header.** The emitter calls it freely;
   `#include <rusty/move.hpp>` had to be added to `server.cc`.

### What the DSL gives up here

Recorded so neither reads as a decision made for its own sake:

- `final` and `private` have no DSL spelling, so the two `Disarm*` helpers are
  public and the type is open. Both are still called only from `server.cc`.
- A C++ constructor becomes `fn new` → `ReplicationWakeGate::new_()`, because
  the DSL has no default member initializers. The owning `Arc` is therefore
  built with `Arc::make_with`, the entry point rusty-cpp documents for a
  non-movable payload built by a factory (`arc.hpp:170-184`) —
  `ReplicationWakeGate` holds `AtomicBool`s and so has no move constructor.
- `impl Default` was declined deliberately (not deferred): it would emit a
  second C++ construction path that no C++ caller uses.

## Tranche 4 — free deletions

**4a.** `InstallSnapshotCallbackLease` is constructed nowhere in `src/`, and
`InstallSnapshotCallbackGate::TryAcquire` had exactly one caller — that lease's
own constructor initialiser. So `ActiveCallbacks()` was structurally always
zero, which made the shutdown drain loop and the destructor assertion both
vacuous. Both classes, the packed-state constants, the two DSL predicates that
only they used, their `static_assert`s, the `server.h` forward declaration and
member, and the four lifecycle sites are gone.

Per the plan's warning, this was **not** "fixed" by wiring real leases:
`~RaftServer` verifies rather than drains, so a live lease would convert a
vacuous assertion into an abort.

**4b.** Four inner `lock_guard`s on a recursive mutex removed and one dead
method deleted, each justified by exhaustive caller enumeration re-verified in
this tree:

| site | why it is dead weight |
|---|---|
| `AmIPreferredLeader` | one caller repo-wide, `GetElectionTimeout`, which already holds the lock |
| `GetElectionTimeout` | one caller, `resetTimer`, which already holds it |
| `SetLocalAppend` | two callers, `setIsLeader` and `StartImpl`, both already holding it |
| `setIsLeader` | every caller is inside `RequestVoteImpl`, `OnAppendEntries`, `OnRequestVote` (via `doVote`), `OnInstallSnapshot` or `stepDown`, all of which hold `mtx_`; the sole unlocked caller is the `RAFT_TEST_CORO` line in the constructor, where no other thread can see the object yet |
| `GetPreferredLeader` | zero callers repo-wide — the method is deleted |

Neither `scheduler.h` nor any Paxos file is touched.

## The expressiveness delta

The plan (§12) asks each step to state what could not be spelled before and can
be now. In one sentence: **before this work a raft DSL block could contain only
a free `const fn` over scalars; it can now contain a whole DSL-owned type with
real methods over foreign types.** Three things had to be true at once, and
none of them was:

1. the extracted Rust had to be able to *name* a foreign type (Tranche 1);
2. a header-resident type with an inherent `impl` had to be able to *link*
   (Tranche 2);
3. the emitted C++ had to be able to *spell* the foreign names it emitted
   (the shim header in Tranche 3).

Counted: `EXPECTED_BLOCKS` goes from 26 to **27**, and the generated C++ inside
GEN regions from ~795 lines to **971**, against **1 222** lines of DSL Rust.
The line count is the less interesting half — 25 of the 143 pre-existing
converted predicates have no caller anywhere, so line coverage was already
partly theatre. Two of those 25 (`raft_server_callback_gate_is_open` and
`..._count`) are deleted in Tranche 4, which is a small improvement in the
honesty of that number rather than a regression in it.

What still cannot be spelled, unchanged by this work: a method on an existing
hand-written C++ class (G3 — the unit of conversion is a whole type), a type
that inherits implementation (G4), and a packed layout.

## The numbers

Configuration `p6/multi/286208B` throughout, `--phase rate` settings
(`--max-outstanding 256 --duration-sec 10 --warmup-sec 3`), against the
committed `412c225a` baseline, on a machine otherwise idle.

**READ THE THROTTLED TABLES FOR LATENCY ONLY.** At offered 191/s the run sits
far below saturation: `offered_per_sec` 191.0, `applied_per_sec` 191.0,
`offer_stalled_sec` 0.0. Both sides therefore report 191.000 ±0.000 and the
throughput column says nothing at all — it records that the pacer hit its
target, not that capacity is unchanged. The same is true at 450/s and 675/s,
where the baseline's applied rate is also exactly the offered rate. In this
whole series only the **unthrottled** point measures throughput, which is what
`docs/performance/raft-harness.md` means by "throughput conclusions come from
the maximum over the series, not the unthrottled point alone". The saturation
table at the end of this section is the throughput evidence; the three tables
before it are latency evidence.

**After Tranche 3** (n=3 before, n=6 after):

| metric | before | after | delta | verdict |
|---|---|---|---|---|
| throughput | 191.000 ±0.000 | 191.000 ±0.000 | +0.00% | within noise |
| p50 latency | 7.503 ms ±0.087 | 7.565 ms ±0.086 | +0.82% (floor 1.85%) | within noise |
| p99 latency | 9.508 ms ±0.606 | 9.888 ms ±0.422 | +3.99% (floor 8.77%) | within noise |

**After Tranche 4** (n=3 before, n=3 after):

| metric | before | after | delta | verdict |
|---|---|---|---|---|
| throughput | 191.000 ±0.000 | 191.000 ±0.000 | +0.00% | within noise |
| p50 latency | 7.503 ms ±0.087 | 7.476 ms ±0.050 | −0.36% (floor 1.53%) | within noise |
| p99 latency | 9.508 ms ±0.606 | 9.484 ms ±0.201 | −0.26% (floor 7.68%) | within noise |

Do not read the two small negative numbers as a speed-up from removing four
recursive re-acquisitions: they are well inside the floor, and an uncontended
recursive re-lock is a few nanoseconds against a 7.5 ms measurement. What they
support is the claim that matters — the deletions cost nothing.

**After Tranche 6 core + B1** (n=3 before, n=3 after):

| metric | before | after | delta | verdict |
|---|---|---|---|---|
| throughput | 191.000 ±0.000 | 191.000 ±0.000 | +0.00% | within noise |
| p50 latency | 7.503 ms ±0.087 | 7.660 ms ±0.022 | +2.09% (floor 1.36%) | worse, under threshold |
| p99 latency | 9.508 ms ±0.606 | 10.147 ms ±0.113 | +6.72% (floor 7.40%) | within noise |

At n=6 it resolves back into the floor, exactly as Tranche 3's did:

| metric | before | after | delta | verdict |
|---|---|---|---|---|
| throughput | 191.000 ±0.000 | 191.000 ±0.000 | +0.00% | within noise |
| p50 latency | 7.503 ms ±0.087 | 7.601 ms ±0.068 | +1.30% (floor 1.67%) | within noise |
| p99 latency | 9.508 ms ±0.606 | 9.928 ms ±0.370 | +4.42% (floor 8.44%) | within noise |

### Saturation — the throughput evidence

Unthrottled (`--rate 0`) at the same configuration, cumulative over every
change in this work, n=3 each side. Both sides spend most of the window blocked
on the in-flight bound, which is what being past saturation looks like and is
why this is the point that measures capacity:

| metric | before | after | delta | verdict |
|---|---|---|---|---|
| **throughput** | **1587.7 ±29.6 entries/s** | **1631.1 ±48.1** | **+2.73% (floor 3.99%)** | **within noise** |
| p50 latency | 956.6 µs ±27.0 | 934.0 µs ±48.8 | −2.36% (floor 6.78%) | within noise |
| p99 latency | 1497.1 µs ±72.2 | 1483.6 µs ±100.8 | −0.91% (floor 9.53%) | within noise |

Capacity at this configuration is unregressed. Note the noise floor is much
wider here (4-10%) than at a throttled point (1.4-1.9%): a saturation run is
inherently noisier, so this point resolves a regression less finely than the
throttled points resolve a latency change. Both are needed.

### What was NOT measured

One configuration of nine (three cluster shapes × three payloads) and, for
latency, one offered rate of twenty-one. No 4 KB or 1 MB payloads, no
1-partition or 6-single-group shapes. 18 records against the baseline sweep's
567. The plan sanctions a single configuration per tranche, but a claim about
Raft performance in general needs `./scripts/raft_perf/run_sweep.sh` in full,
which is ~4h51m and needs the machine to itself.

### On reading compare.py

Two of the three changes in this work needed a second round of trials at the
same reference point, and both times the n=3 reading sat just above the floor
and the n=6 reading fell back inside it. That is what a 1.4-1.9% noise floor
against a 5% threshold looks like in practice: `compare.py` exiting 0 is NOT on
its own sufficient evidence, because a delta can clear the floor while staying
far under the threshold. Read the floor column, not just the verdict, and add
trials when the two disagree.

At n=3 the Tranche 3 p50 delta read +1.55% against a 1.45% floor — right at the edge of
what three trials can resolve, which is why three more were run rather than
declaring the marginal number a pass. At n=6 it falls back inside the floor.

## Tranche 6 core — implementation inheritance dissolved

`TxLogServer` was never an interface. All 36 lines of `scheduler.h` were six
public data members (`loc_id_`, `site_id_`, `app_next_`, `commo_`,
`partition_id_`, `mtx_`), one non-virtual method assigning one of them, and a
virtual destructor — **zero behavioural virtuals**. RaftServer and PaxosServer
inherited the fields and read them as their own. That is the whole of G4/B7.

Checking what the workers actually did through a base pointer made the fix
much smaller than the plan's framing implies. Exactly four things: write four
fields, call `RegLearnerAction`, `delete`, and `dynamic_cast` back to the
concrete type for anything real.

So the data moved down into the two servers and `scheduler.h` became a genuine
interface with three pure virtuals — `SetSiteIdentity`, `SetCommo`,
`RegLearnerAction` — plus the virtual destructor.

**The fields are flattened, not wrapped in a member struct**, via two macros in
`scheduler.h`. A member struct would have been cleaner C++ but would have
renamed every use: 164 `site_id_`, 20 `partition_id_`, 10 `loc_id_`, 6
`app_next_` in Raft alone, ~200 edits whose only purpose is to satisfy a
scoping rule, inside the same change that moves ownership and locking.
Flattening leaves every body byte-identical, and leaves the fields as plain
members of the concrete type — which is also the shape the DSL wants, since a
DSL-owned struct has fields rather than an embedded base. The trade is
honest: it is a macro, and a macro reads worse than a struct. It is two
definitions and two call sites.

**`mtx_` came down too, deliberately.** It was a `std::recursive_mutex` shared
between the engines purely by inheritance — 48 acquisitions in Raft, 4 in
Paxos. Each server now owns its own, which is precisely what lets Tranche 5
replace Raft's with a single `Mutex<RaftState>` without touching Paxos. That
was the blocker that previously made Tranche 5's stated endpoint unreachable
raft-only.

### B1, folded in

The blast radius coincided, so the ownership triangle was fixed in the same
change. `RaftFrame::svr_` goes from `std::unique_ptr<RaftServer>` to a
**non-owning** `RaftServer*`; `CreateScheduler()` returns `new RaftServer()`
and the worker is sole owner, matching `MultiPaxosFrame` which has never had a
frame-side owner. Keeping the member's *name* means `testconf.cc`'s 22
`frame->svr_->...` sites needed no edit at all — only `svr_.get()` at three
places.

One hazard the plan does not mention and that the change introduces: a
borrowed back-reference can outlive the object. `RaftFrame::ReleaseScheduler()`
is called by both owning workers immediately before their `delete`, so the
harness's `!frame->svr_` guards cannot read a stale non-null pointer.
`server_worker.cc` flips from skipping the delete to performing it.

## The blocker on moving `RaftServer` into the DSL

**`#[cpp_inherit]` does not work under the transpiler this repository pins for
Raft, and it fails silently.**

Established by probing, not by reading. A DSL block of exactly the shape that
works in `src/mako/storage/mbta_wrapper.hh:533-540` — `pub struct`, then an
empty `#[cpp_inherit] impl <Base> for <Struct> {}`, then an inherent `impl` —
emits a struct with **no base class** and no diagnostic of any kind under pin
`77c3ad5a`. Varying the method shape, the receiver, the field count and the
base's name changes nothing.

The decisive test: round-tripping the committed, working `mbta_wrapper.hh`
through the pinned transpiler reproduces its GEN region **without** the
`: public FullOrderedIndex` that is committed in the tree. The storage headers
were generated by a different transpiler —
`scripts/regen_storage_dsl.sh:19` defaults to
`build_local/rusty-cpp-transpiler-a4bcff5f`, which is not the
`REQUIRED_RUSTY_CPP_COMMIT = 77c3ad5a...` that `scripts/raft_dsl.sh:28` and
`scripts/check_rrr_crate_mode.py:26` enforce, and which is not present on this
host at all.

So the route to `RaftServer`-in-the-DSL is gated on the toolchain, not on
Mako's C++. Either the pin moves — which CLAUDE.md requires be done against a
reviewed upstream base, with the transpiler suite run and the gitlink bumped in
the same commit, triple-attested — or the emitter gains the feature. Tranche 6
core has removed every *Mako-side* obstacle: the base is now interface-shaped,
which is the only shape `#[cpp_inherit]` can attach even in principle.

**A related defect found on the way, and fixed.** `regen_storage_dsl.sh --check`
passed *vacuously* when its transpiler was absent: it copies the carrier to a
temp file, fails to rewrite it, then diffs the untouched copy against the
original — identical by construction — reports no drift and exits 0, having
verified nothing. That is the same failure mode as the dormant `RAFT_TEST`: it
reads as coverage. The script now refuses to run without an executable
transpiler and fails on a rewrite error.

## What is left

**Tranche 5 (B2, de-reentrancy proper).** Not started, but now unblocked.
Its stated endpoint — one `Mutex<RaftState>` locked at the four RPC entry
points, with `&mut self` inherent methods — was previously unreachable
raft-only because `mtx_` belonged to `TxLogServer`. Tranche 6 core moved it
down, so the endpoint is now Raft's to change alone.

Doing it should **skip the C++ intermediate**. The plan's `X()`/`XLocked()`
split is a shape that the DSL target does not want: `&mut RaftState` states
"the lock is held" as a type, so the split exists only to say the same thing
in C++ and would be deleted again on conversion.

One item in it is independent of all that and worth doing on its own merits:
the **`next_index_` iterator** at `server.cc:2142`. It is worse than the plan's
"no Rust spelling" framing. `it` is a live cursor into a `std::map`, and
`commo()->SendInstallSnapshot(...)` is called *inside* the `lock_guard` scope
while `it` is held — and that call's completion callback takes the same
**recursive** mutex and writes `next_index_[site_id]`. Recursive means a
synchronous callback re-enters and mutates the map under a live cursor.

**Tranche 6, step 2 — `RaftServer` into the DSL.** Blocked on the toolchain,
not on Mako. See "The blocker" above. Every Mako-side obstacle is removed; the
remaining one is that `#[cpp_inherit]` emits no base under the pin, silently.

**B1.** Done — see above. The plan's advice to copy Paxos rather than attempt
the `rusty::Box` refactor was correct and cost about six production edits,
touching no Paxos file.

## How to re-run the evidence

```bash
# DSL gates (~15 s)
bash scripts/raft_dsl.sh --check

# Storage DSL gate. NOTE: this needs the a4bcff5f transpiler, NOT the 77c3ad5a
# pin the Raft and rrr gates enforce, and it now refuses to run without one
# rather than passing vacuously.
bash scripts/regen_storage_dsl.sh --check <path-to-a4bcff5f-transpiler>

# Raft cluster correctness, 25 cases (~90 s plus its own build)
./ci/ci.sh raftLabTest

# Log integrity under load, and under a leadership flap
examples/raft_bench.sh --out /tmp/p.json --partitions 1 --rate 2000 --duration-sec 8
examples/raft_bench.sh --out /tmp/unused.json --partitions 6 --group-mode multi \
    --payload-bytes 1024 --rate 3000 --duration-sec 20 --kill-leader-at-sec 6

# Performance against the committed baseline, at the reference point
tar -xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp
mkdir -p /tmp/before/rate && cp /tmp/records/rate/*p6-multi-pb286208-b1-r191-t*.json /tmp/before/rate/
# ... run the same point three times into /tmp/after/rate ...
python3 scripts/raft_perf/compare.py /tmp/before/rate /tmp/after/rate
```
