# RaftServer's state is a DSL struct: what it cost and what it bought

This is the evidence record for the tranche that took `src/deptran/raft`'s
Rust share from 33% to 70%. It exists because the conversion has two hard
constraints -- correctness and performance -- and "the tests passed" is not
by itself a measurement of either.

## The number

Rust share of `server.h` + `server.cc`, counting non-comment, non-blank
lines, with generated C++ (the `RUSTYCPP:GEN` regions) excluded from both
sides so nothing is double-counted:

| | Rust | C++ | share |
|---|---|---|---|
| `server.h` | 2604 | 547 | 82.6% |
| `server.cc` | 1726 | 1298 | 57.1% |
| **total** | **4330** | **1845** | **70.12%** |

Sixteen `RaftServer::` definitions remain, 164 lines in total. Every one is
either an entry point the rrr service layer calls by name, or a call into
the replication wake gate, whose type is only forward declared in the
header.

## What made it possible

`RaftServerBase`, a DSL struct holding all 47 of RaftServer's data members
and implementing `TxLogServer` directly. Before it, the transpiler's
orphan-impl rule meant a C++ method body could not become Rust at all: the
only available move was to write a Rust function beside it and have the C++
call in, which accretes rather than substitutes. With the state in a base
class, a method converts by MOVING -- the C++ is deleted and call sites
resolve through inheritance.

Three pieces of machinery carried the rest:

 - **RaftLockGuard / RaftStdLockGuard.** `mtx_` is opaque to Rust, so before
   these no body that took the Raft mutex could convert. They acquire in
   `new` and release in `Drop`, which reproduces `std::lock_guard`'s scope
   exactly, early returns included.
 - **The logging bridge** (`rust_log_shims.h`). 160 `Log_` call sites were
   the single largest blocker. rrr's loggers are variadic function
   templates, and Rust has no variadic generics, so there is one shim per
   (level, arity). The `std::format_string` parameter survives, so a wrong
   placeholder count is still a compile error at the DSL call site.
 - **The kernel bridge** (top of `server.cc`). Three kinds of thing a DSL
   body cannot do: look inside an opaque C++ field, call a RaftServer method
   that has not converted (these downcast from `RaftServerBase*`, which is
   well defined because the base is never instantiated alone), and compile
   conditionally. The first two kinds are expected to disappear as their
   reason does; the third is permanent.

## Correctness

RaftLabTest, the 25-case cluster suite, passed 25/25 with exit 0 after every
one of the fourteen commits in this tranche -- not only at the end.
`scripts/raft_dsl.sh --check` is clean across all eighteen carriers: pin
attestation, block inventory, generated C++, crate compile, clippy, and no
drift.

**The production path is covered too, and this file used to claim only one
suite.** RaftLabTest builds with `RAFT_TEST=ON`, which selects `ServerWorker`
and compiles `RAFT_TEST_CORO` -- a different server lifecycle from the one
Mako ships. `src/deptran/raft/server.cc` is compiled into production `mako`
regardless of `MAKO_USE_RAFT` (`CMakeLists.txt:1049`, via `TXLOG_HELPER_SRC`),
so the converted Raft is in the database binaries; it just needs a different
suite to exercise. All four Raft replication suites pass at HEAD, run on the
host from a freshly built `build/`:

| suite | binary | result |
|---|---|---|
| `shard1ReplicationSimpleRaft` | `simpleTransactionRep` | data integrity verified on both followers |
| `shard2ReplicationSimpleRaft` | `simpleTransactionRep` | data integrity verified on all four followers |
| `shard1ReplicationRaft` | `dbtest` | 153,019 ops/s agg_persist_throughput, 7,730 replay batches, 0% remote abort |
| `shard2ReplicationRaft` | `dbtest` | 8,007 + 7,994 ops/s across two shards, 12 warehouses, abort ratios 2.3% / 1.8% |

Every one reported "All processes exited cleanly". That matters beyond the
pass: the production path runs `RaftWorker`, not `ServerWorker` -- the
lifecycle with the control-plane poll thread, the stub servers and a
different shutdown ordering, none of which RaftLabTest touches.

## Performance

`examples/raft_bench.sh`, one partition, 1024-byte entries, 10s measured
window after a 2s warmup. Baseline is `cfd311109`, the commit before this
tranche; every binary is built Release from `build_perf` (MAKO_USE_RAFT=ON,
RAFT_TEST=OFF) and the arms are run round-robin so machine drift lands on all
of them equally.

**These numbers were re-measured after the tranche this file was written for,
and then again twice more; read the pooled paragraph below the table rather
than the table alone.** The original pair
(baseline vs the tree at 70.1% conversion) is superseded: the third arm, `p1`,
is the tree immediately BEFORE the apply-queue conversion, and it exists to
separate "did this tranche cost anything" from "has the conversion drifted".

Saturation (`--rate 0`), applied/s, medians:

| arm | n | median | vs baseline |
|---|---|---|---|
| baseline `cfd311109` | 10 | 40857.6 | -- |
| `p1` (before the apply queue) | 5 | 39877.1 | -2.40% |
| head | 10 | 40409.6 | **-1.10%** |

The middle arm being the slowest looked, from this run alone, like proof that
the spread was the machine rather than the code. TWO LATER RUNS SHOWED THAT
READING WAS WRONG, and the paragraph that used to stand here said so too
confidently on ten pairs at p = 0.344. What the pooled data says:

| run | paired median | favour converted | sign test |
|---|---|---|---|
| this one (10 pairs) | -0.69% | 3/10 | p = 0.344 |
| second (6 pairs) | -2.32% | 0/6 | p = 0.031 |
| third (6 pairs) | -0.22% | 2/6 | p = 0.688 |
| **pooled (22 pairs)** | **-1.71%** | **5/22** | **p = 0.017** |

So there IS a small cumulative cost -- roughly 1.5%, mean -1.51%, per-pair
range -7.74% to +1.47%. It is REAL but it is not localisable by the method
used here: the three runs disagree on its size by an order of magnitude, the
between-run variance exceeds the effect, and a bisect over the conversion
window put the midpoint at -1.20%, which says the cost accumulates across
tranches rather than arriving in one.

One hypothesis was tested and refuted: the DSL lock guards emitted
`~RaftLockGuard() noexcept(false)`, which forces unwind bookkeeping in every
scope holding a guard, and the hot path holds one in about thirty places.
Declaring them noexcept measured +0.54% median over six pairs, p = 0.688 --
the right direction, not established. The change was kept because the
contract is true, not because it closed the gap.

Resolving the remaining 1.5% needs tens of paired trials per candidate on a
quiet host, which is a dedicated measurement session rather than a step in a
conversion tranche.

Throttled (`--rate 20000`), medians of 3 trials each:

| | baseline | head |
|---|---|---|
| applied/s | 20000.4 | 20000.7 |
| p50 latency | 3728 us | 3832 us |
| p99 latency | 5214 us | 5382 us |

Both arms sit exactly on the offered rate, so the throttled point measures
latency only, and 2.8% of 3.7ms is a scheduling artefact at this resolution.

Saturation latency: p50 99569us vs 100278us (+0.71%), p99 118331us vs
122453us (+3.48%) -- with `p1`'s p99 at 129458us, above both, on the same
pattern as the throughput numbers.

Every run on every arm reported zero gaps, zero duplicates and zero
out-of-order applications.

Two changes should be slightly FASTER and are not separable at this
resolution. PHASE 0 and PHASE 3 each rebuilt a `std::vector<siteid_t>` from
`current_config_` on every heartbeat round; that set has exactly one write,
during Setup, so it is mirrored once into `config_members_` now. And
`ElectionTimerLoop` / `HeartbeatDriver` reach the server through direct
calls rather than through ten `extern "C"` trampolines. A third joins them:
`EnqueueCommittedEntries` walks the log once per commit batch instead of
twice, because the scan and the copy-out are no longer on opposite sides of
the language boundary.

## Bugs this found

Two, both in aliasing that only a Rust caller can see. Three DSL functions
(`heartbeat_apply_append_reply`, `heartbeat_phase0_locked`,
`heartbeat_phase3_locked`, and `raft_commit_advance` under them) took
`&mut RaftConsensusState` alongside `&PeerTable` and `&RaftLog` -- and every
C++ call site passed `state_`, `state_.peers_` and `state_.raft_log_`, which
is one mutable borrow overlapping two shared ones. It compiled only because
the caller was C++. Both now reach the fields through `consensus`.

Separately, wrapping a method in ` public: ... private:` to expose it to the
kernel bridge is only correct where the surrounding region was private. Two
such wrappers landed in the public API section and silently made the
constructor, destructor and every RPC entry point private. The build caught
it.

## What is left

The wake-gate methods (`RequestReplication`, `CloseReplicationWakeGate`,
`BindReplicationWakeOwner`, the two waits) cannot convert until
`ReplicationWakeGate` is defined somewhere `server.h` can see it; its DSL
block is in `server.cc`. `LoadStateMachineSnapshotLocked`,
`PrepareStateMachineSnapshotLocked` and the `PreparedStateMachineSnapshot`
transaction are `std::unique_ptr` and exception machinery throughout.
`PeerOrdinal`'s and `RebuildPeerTables`' kernels go away when `peer_sites_`
and `current_config_` become `rusty::Vec` / a sorted `rusty::Vec`.
