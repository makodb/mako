# Code structure: the Raft implementation and how it runs

This describes the code at commit `9f95bd6da` (branch `verus-raft`,
2026-10-04); the documents it cites are those of the commit that adds it. It
is a snapshot: line numbers and counts will drift. It
answers the questions the owner asked while learning this code. §0 gives the
short answers; the later sections give the evidence.

Paths are relative to `src/deptran/raft/`, as in
[host-contract.md](host-contract.md): `src/server_h.rs` is
`src/deptran/raft/src/server_h.rs`, and `core/src/node.rs` is the core
crate. A path that begins with `src/deptran/` (such as
`src/deptran/raft_main_helper.cc`), `src/srpc/`, `src/rusty-rustc/`,
`src/mako/`, `scripts/`, `docs/` or `CMakeLists.txt` is from the repository
root. This worktree builds Raft on the Rust lane only
(`CMakeLists.txt:465-479`); the hybrid and cpp lanes were removed
(docs/verus/modification-plan.md:49, decision Q9) and are not described.
"Inference" marks a conclusion drawn from the code rather than read in it.

## 0. Short answers

**(1) Do `core`, `rt` and `src` contain all the protocol code?** Yes: every
Raft decision that runs here is in one of them; the C++ beside them holds
Mako glue, timing and batching knobs and a few snapshot-path steps. But not
all of it is in the core. The snapshot half of Raft lives in the shell, and
the shell supplies guards the proof assumes (Start's leader check, the
round end's leadership check, the quorum size, the entry-term check). The
verification code is not protocol and cargo erases it. §2, §10.

**(2) Does every core function follow one pattern: an Event in, run to
completion, something back?** Yes. The core has one entry point,
`RaftCore::step` (`core/src/event.rs:371`), plus `step_checked` (`:327`) for
network messages, over 18 `Event`s. Each call runs synchronously, returns a
`Reply` and leaves actions and log records in a `CoreOutput` that the shell
carries out afterwards; the core takes no lock, does no I/O, reads no clock
and cannot wait. A network round trip is several events with the waits in
the shell. The small exceptions are listed in §3.2.

**(3) Why is `src` so large?** Because the core is pure, everything with an
effect is in `src`: the lock and the action executor, the fiber loops and
the wake gate, startup, shutdown and the apply thread, the Mako interface
(C++ objects reached through about 80 kernel declarations), the RPC glue
and the recorder. The whole snapshot protocol (about 950 lines) never moved
into the core, and 23% of `src/server_h.rs` is comments. §4.

**(4) A server calls out, and other servers call back into it: how is that
handled?** The candidate holds `mtx_` only in short critical sections that
never wait: one to start the campaign before the broadcast and one to
settle it after the wait, each a core call plus the steps its actions ask
for, and brief reads before and after. It sends without blocking and waits
by sleeping its fiber, which hands the poll thread back; that thread then
runs the inbound handlers (each holds `mtx_` for one critical section and
sends nothing but its reply) and the vote reply callbacks (which take no
`mtx_`). The settle step re-checks under the lock whether the campaign is
still current. Taking `mtx_` twice on one thread aborts instead of hanging.
§5.

**The code structure now.** Four cargo crates in one workspace: `raft-core`
(`core/`, verified), `raft` (`src/`, the shell), `raft-rt` (`rt/`, the
runtime) and `raft-replay` (`replay/`), built into one library linked with
the C++ archive `txlog_core`. §1.

**What `replay/` does.** It records every core call as a line of text and
re-runs the recorded events through a fresh core, requiring the same text.
It shows the core is a function of its events, not that threads behave. §9.

**What `mtx_` guards.** The core, by convention: it sits beside the core
(`src/server_h.rs:871-873`), not around it, so it excludes only threads that
also take it. Threads that only read the role or the commit index use
atomic mirrors instead. §7.

**Only the poll thread?** Mostly. In production the poll thread runs every
RPC handler and the heartbeat and election fibers, so it makes every role
and term change. The submit, setup and shutdown threads each make one kind
of call (propose, set the identity, propose), and a Mako thread proposes
itself when no submit thread runs. The apply thread records applied indexes
and, outside the verified gates, compacts the log (which removes nothing
while snapshots are off) and, with snapshots on, creates snapshots. §7.

**Is the core's `Event` the poll thread's event?** No. A reactor event is
something a fiber waits on; the core's `Event` is a value passed to `step`,
which returns before the caller goes on. §8.

## 1. The map

Four crates, one workspace (`Cargo.toml:29-30`), one profile:
`panic = "abort"` (`Cargo.toml:80-84`), so a panic anywhere ends the process.

| Crate | Directory | Role | Depends on |
|---|---|---|---|
| `raft-core` | `core/` | the protocol state and every decision over it; Verus-checked | Verus's crates only (`core/Cargo.toml:14-21`); `#![forbid(unsafe_code)]` (`core/src/lib.rs:5`) |
| `raft` | `src/` | the shell: `RaftServerBase` (the lock, the core, loops, action executor, Mako interface, snapshot paths, the C ABI) | `raft-core`, `raft-replay`, the `rusty` facade; no srpc (`Cargo.toml:63-70`) |
| `raft-rt` | `rt/` | the runtime: the poll thread, the srpc server and clients, the wire codec, fibers and events, the snapshot store | `raft`, `srpc` (`rt/Cargo.toml:34-37`) |
| `raft-replay` | `replay/` | the recorder's text format and the replayer | `raft-core` |

CMake runs `cargo build` on `rt/Cargo.toml` and links the result,
`libraft_rt.a`, which contains the other crates as rlibs
(`CMakeLists.txt:1204-1263`). The C++ files of this directory are compiled
into `txlog_core` (`CMakeLists.txt:1090-1117`; `raft_main_helper.cc` at
`:1121-1125`), and the two archives are linked as a rescan group
(`:1329-1333`). raft-rt's own tests run on every build (`:1351-1374`).

Tags below: PROD (every Mako binary), LAB (only with `-DRAFT_TEST=ON`, the
cargo feature `raft_test`, `CMakeLists.txt:1226-1233`), TEST, VERIFY (only
Verus compiles it), UNUSED (compiled, never called).

```
src/deptran/raft/
  Cargo.toml        84   workspace root; also the manifest of the shell crate `raft`
  core/             raft-core: the core (PROD); 44% of its lines are VERIFY (§10)
    src/node.rs        2209  RaftCore (state); identity, configure, gates; append; vote;
                             campaign start and settle; role changes; AppendEntries handler
    src/heartbeat.rs   1760  the leader's round: tick (payload choice), reply, round end;
                             the commit rule (raft_commit_advance)
    src/authority.rs    912  read-index authority ledger
    src/helpers.rs      631  pure predicates and index arithmetic (const fns)
    src/log.rs          618  RaftLog, the in-memory log in blocks
    src/event.rs        538  Event, Reply, step, step_checked (F9 admission)
    src/progress.rs     393  PeerTable: next and match index per follower
    src/output.rs       220  CoreOutput: actions and log records
    src/pending.rs      184  the in-flight AppendEntries slots
    src/election.rs     181  VoteSet (the core's vote count), CampaignStart, ElectionTick
    src/logging.rs       94  log lines as data
    src/lib.rs           41
    src/coupling.rs    2706  VERIFY: the coupling to the spec (cfg(verus_keep_ghost))
    tests/step_checked.rs 133, tests/b17_round_end.rs 166   TEST, by hand (B17's is ignored)
  rt/               raft-rt (PROD)
    src/transport.rs  1052  RaftTransport: poll thread, server, clients, vote tally, C ABI
    src/snapshot.rs    627  the in-memory snapshot store; InstallSnapshot send
    src/rpc.rs         582  generated codec and dispatch (scripts/rpcgen_rust.py)
    src/seam.rs        478  kernels: events, fibers, wake job, vote broadcast and wait, sends
    src/service.rs     177  the four RPC handlers, each one call to Serve*
    src/trace.rs        44  trace hooks, inert unless MAKO_RAFT_TRACE_FILE is set
    src/lib.rs          11
    src/lab_runtime.rs 190  LAB
    tests/ (4 files, 795 lines)  TEST, run on every build
  src/              the shell crate `raft`
    server_h.rs       4360  RaftServerBase and nearly all shell logic (PROD; 159 lines lab)
    server_cc.rs       709  the heartbeat round driver; the generated C ABI exports (PROD)
    scheduler_h.rs      81  the TxLogServer and RaftSpecific traits (PROD)
    server_pods_h.rs    52  plain structs passed across the kernel boundary (PROD)
    lib.rs              34
    lab.rs 901, lab_cases.rs 945, lab_snapshot_cases.rs 1398, lab_main.rs 48   LAB
    14 extracted views, 3-207 lines each, 669 in all   UNUSED (see below)
  replay/src/lib.rs 882, replay/tests/core_replay.rs 47   recorder PROD (off by default);
                                                          replayer TEST, by hand
  tests/server_is_send.rs 28   TEST: RaftServerBase is Send + Sync
  verus/            VERIFY: commit_rule.rs (186, a standalone Verus model, run by hand);
                    spec/ (the frozen spec version and its two patches)
  *.cc, *.h, *.hpp  C++, below
```

The 14 small files in `src/` (`communicator_h.rs` 207 down to
`snapshot_manager_hpp.rs` 3) are Rust copies of inline-DSL blocks kept in
C++ files (`rust-modules.toml:1-66`, `:110-140`). They are compiled into the
crate (`src/lib.rs:9-34`), but a search finds Rust references only to
`scheduler_h` and `server_pods_h`. `server_h.rs`, `server_cc.rs`,
`server_pods_h.rs` and the lab files are canonical Rust
(`rust-modules.toml:68-108`). Headers that call the crate a generated
verification view (`Cargo.toml:1-16`, `src/lib.rs:1-7`) or say it is built
into `libraft.a` (`Cargo.toml:23-26`, `src/server_h.rs:1-2`) are out of date.

The C++ in this directory:

| Files (lines) | Status | Role |
|---|---|---|
| `server.cc` 1751, `server.h` 545 | PROD | the kernels the shell calls (Mako's commands, callbacks, config, env knobs, logging, the lock); `RaftCheckedMutex` (`server.h:241-275`); `class RaftServer`, a C++ shim holding a pointer to the Rust struct (`server.h:509-543`) |
| `server_exports.h` 53, `transport_exports.h` 75, `lane_kernels.h` 52, `raft_kernel_pods.h` 49, `rust_facade_types.h` 67, `snapshot_callbacks.h` 36, `raft_lane.h` 71, `raft_lane_rust.cc` 105 | PROD | declarations of the C ABI both ways; the worker's calls into raft-rt (serve, connect peers, post a job) |
| `raft_worker.cc` 1252, `raft_worker.h` 298, `frame.cc` 417, `frame.h` 98, `application_log.cc` 85, `application_log.h` 23 | PROD | the Mako worker: setup, submit thread, apply callback, leader callbacks; `RaftFrame::CreateRaftScheduler` (`frame.cc:264`) |
| `commo.cc` 325, `commo.h` 269, `service.cc` 125, `service.h` 47, `messages.hpp` 205, `macros.h` 75 | linked, unreachable | the removed C++ lane's communicator and service; the Rust-lane branches return before reaching them (`raft_worker.cc:353-357`, `:392-396`), and `set_commo` aborts if called (`rt/src/seam.rs:237-241`) |
| `quorum.hpp` 169, `memory_snapshot_manager.hpp` 264, `snapshot_manager.hpp` 267, `log_storage.hpp` 416 | unused by Raft here | the first two are included by `server.cc:34-35` and the third by `server.h:42`, whose only use is the removed lane's `#else` (`server.h:338-341`); nothing on this lane uses them (inference from a search); the last two serve Paxos |
| `raft_bench.cc` 2045, `raft_bench_jetpack.h` 458 | BENCH | `raft_bench` (`CMakeLists.txt:1738`) |
| `channel_transport.hpp`, `dispatcher.hpp`, `transport.hpp`, `srpc_transport.hpp`, `raft_node.hpp`, `test_cluster.hpp`, `snapshot_format.hpp`, `raft_lab_standalone.cc` | TEST | gtest targets (`CMakeLists.txt:2072-2152`) |
| `memory_log_storage.hpp` 373, `rocksdb_log_storage.hpp` 878 | UNUSED | included only by `src/srpc/tests/rpc_*log_storage_test.cc`, which no build file names |

The call chain in production:

```
Mako transaction thread
  -> src/deptran/raft_main_helper.cc        add_log_to_nc, setup, setup2, ...
  -> raft_worker.cc                         RaftWorker: submit thread, apply callback, startup
  -> server.h:509-543                       class RaftServer, a C++ shim over a pointer
  -> server_exports.h, src/server_cc.rs:417-709    the C ABI (generated, scripts/raft_gen_exports.py)
  -> src/server_h.rs:864-980                RaftServerBase { mtx_, core, shell state }
  -> src/server_h.rs:1407                   RaftServerBase::step, the recording wrapper
  -> core/src/event.rs:371                  RaftCore::step

inbound RPC:  rt/src/rpc.rs:419-472 (dispatch) -> rt/src/service.rs:107-176
              -> src/server_h.rs:3925-3973 (Serve*) -> on_*_body -> step_checked
outbound:     the shell calls server.cc's kernels for Mako's objects, and raft-rt's
              seam (rt/src/seam.rs) for fibers, events, waits and sends
```

## 2. Q1: where the protocol lives

Every part of Raft that runs here is in `core/`, `src/` or `rt/`. The core
holds the decisions of the verified configuration; the shell holds the
snapshot half of the protocol, some guards the core's proof assumes as
premises, and all timing. C++ holds only knobs, glue and a few
snapshot-path steps.

| Part of Raft | Where | In the core? | Covered by the proof? |
|---|---|---|---|
| Campaign start, vote count, settle | `core/src/node.rs:702-792`, `:800-1077`; `core/src/election.rs:73-113` | yes | yes |
| Whether the election timeout fired | `core/src/node.rs:1365-1381`, a read-only query (§3) | yes | the predicate (`core/src/helpers.rs:48-55`) |
| Answering RequestVote | `core/src/node.rs:1404-1600`, `:587-696` | yes | yes |
| Answering AppendEntries: log matching, append, truncate, follower commit | `core/src/node.rs:1749-2207` | yes | yes |
| The leader's round: payload choice, reply handling and backoff, the commit rule | `core/src/heartbeat.rs:771`, `:1435`, `:1712`; `:63-135` | yes | yes |
| Read-index authority | `core/src/authority.rs:438` | yes | kept by every step; not modelled by the spec (`core/src/heartbeat.rs:1678-1679`) |
| Message admission (F9) | `core/src/event.rs:285-368` | yes | yes |
| The decision to send a snapshot | `core/src/heartbeat.rs:959-976` | yes | snapshots are gated off |
| Start's leader check | `src/server_h.rs:3887-3894`; the core's `append_local` has none (`core/src/node.rs:494-524`) | no | a premise (host-contract.md:98) |
| The round end's leadership check | `src/server_cc.rs:310-316`; phase 3 advances commit regardless (`core/src/heartbeat.rs:1689`) | no | a premise that lab builds (B17) and shutdown (B18) can violate (§6) |
| The quorum size `n_total` | `rt/src/transport.rs:332-334`, `:562` | no | a premise (host-contract.md:100) |
| Which voter a reply came from (F1) | `rt/src/transport.rs:580-584` | no | a premise (host-contract.md:45-48) |
| Entry terms at least 1 in an inbound payload (F4) | `src/server_h.rs:3326-3345` | no | a trusted contract (host-contract.md:111-120) |
| The "unavailable" answers | `src/server_h.rs:3928-3934`, `:3946-3952`, `:4232-4241`, `:4311-4322` | no | modelled as dropped messages (host-contract.md:79-85) |
| Executing the leader's no-op | decided at `core/src/node.rs:1280`; done at `src/server_h.rs:3498-3514`, skipped in lab builds (`:3499-3501`) | decided there | yes |
| Applying committed entries | `src/server_h.rs:3075-3144` (hand-off), `:2572-2733` (apply thread); `server.cc:893-906` | records the applied index only (`core/src/node.rs:1083-1100`) | no |
| Snapshots: install, reply, create, compact, recover | `src/server_h.rs:2226-2510`, `:2129-2197`, `:2750-2823`, `:1309-1349`, `:1904-2122`; `rt/src/snapshot.rs`; `server.cc:848-886` | no | no: gated off (F5, `src/server_h.rs:1816-1841`) |
| Timing: election timeouts, the timer's cadence, the vote wait, the heartbeat interval, the reply deadline | `src/server_h.rs:1453-1476`, `:1369-1375`; `server.cc:164-217`; `rt/src/seam.rs:276-284`; `server.h:169-173`; `src/server_cc.rs:198-210` | no | no: liveness is not proved (host-contract.md:30-31) |
| Batch limits | `server.cc:219-240`, passed in at `src/server_cc.rs:93-95` | the selection is (`core/src/heartbeat.rs:519`) | the selection |
| Membership: the static yaml configuration | read at `src/server_h.rs:3531-3547` | stored there (`Configure`) | a premise (static configuration) |
| The preferred leader (locale 0) | `src/deptran/raft_main_helper.cc:870-924` | no | no (timing only) |

The snapshot half is outside the verified configuration: with
`MAKO_RAFT_VERIFIED_GATES=1` startup fails closed when snapshots are on,
and `CompactLog` and `MaybeCreateSnapshot` return at once
(`src/server_h.rs:1821-1841`, `:1342-1346`, `:2952-2956`). Verification
code is not protocol: `core/src/coupling.rs` is compiled only by Verus
(`core/src/lib.rs:27-29`), and cargo erases the contracts and proofs in the
other core files (`core/Cargo.toml:15-18`; §10).

## 3. Q2: how the shell calls the core

Every state-changing core function is reached through one entry point,
`RaftCore::step(Event, &mut CoreOutput) -> Reply` (`core/src/event.rs:371`),
or `step_checked` (`:327`), which first drops a malformed or foreign message
(F9, `:285-319`) and otherwise calls `step`. `step` only dispatches: each
arm calls one core function and wraps its result (`:398-534`). The shell
reaches it through one wrapper, `RaftServerBase::step` / `step_checked`
(`src/server_h.rs:1407-1443`), which also records the call when the
recorder is on (§9).

### 3.1 The 18 events

`core/src/event.rs:24-108`; 12 `Reply` variants (`:110-127`). There are 21
call sites in the shell; a plain search finds 27 matches, of which two are
the wrapper's definitions and four are the wrapper calling the core. "Poll"
is the transport's poll thread (§7).

| Event | Core function | Sent from | Thread | `mtx_` | May push |
|---|---|---|---|---|---|
| SetIdentity | `core/src/node.rs:233` | `src/server_h.rs:3719` | setup thread | no | nothing |
| Configure | `core/src/node.rs:262` | `src/server_h.rs:3545` (from `:1811`) | poll (startup job) | no | nothing |
| EnterGates | `core/src/node.rs:310` | `src/server_h.rs:3525` | poll (startup job) | yes | nothing |
| RebuildPeers | `core/src/node.rs:342` | `src/server_h.rs:1401` (from `:2942`) | poll (heartbeat fiber) | yes | nothing |
| Propose | `core/src/node.rs:494` | `src/server_h.rs:3490`, from `Start` (`:3900`) and the no-op (`:3506`) | submit, Mako or shutdown thread; poll for the no-op | yes | nothing |
| StartElection | `core/src/node.rs:702` | `src/server_h.rs:3178` | poll (election fiber) | yes | RESET_ELECTION |
| SettleElection | `core/src/node.rs:800` | `src/server_h.rs:3264` | poll (election fiber) | yes | ROLE_SET, APPEND_NOOP, RESET_ELECTION |
| ResetElectionTimer | `core/src/node.rs:1107` | `src/server_h.rs:1487` | poll | yes | nothing |
| RecvRequestVote | `core/src/node.rs:1404` | `src/server_h.rs:4220` (checked) | poll (inline handler) | yes | RESET_ELECTION, ROLE_SET |
| RecvAppendEntries | `core/src/node.rs:1749` | `src/server_h.rs:4303` (checked) | poll (inline handler) | yes | RESET_ELECTION, ROLE_SET, APPLY_RANGE |
| TickHeartbeat | `core/src/heartbeat.rs:771` | `src/server_cc.rs:96` | poll (heartbeat fiber) | yes | APPLY_RANGE |
| RecvAppendReply | `core/src/heartbeat.rs:1435` | `src/server_cc.rs:251` (checked) | poll (heartbeat fiber) | yes | ROLE_SET, RESET_ELECTION (step-down) |
| AbandonRound | `core/src/heartbeat.rs:1611` | `src/server_cc.rs:297` | poll (heartbeat fiber) | yes | nothing |
| RoundEnd | `core/src/heartbeat.rs:1712` | `src/server_cc.rs:317` | poll (heartbeat fiber) | yes | APPLY_RANGE |
| ResetRoundState | `core/src/node.rs:328` | `src/server_cc.rs:364`, `:390` | poll (heartbeat fiber) | yes | nothing |
| Applied | `core/src/node.rs:1083` | `src/server_h.rs:1295`, from `:2682`; with snapshots on also from `:2109` and `:2502` | apply thread; poll (snapshot recovery in the startup job, inline InstallSnapshot handler) | yes | a log line |
| SetFollower | `core/src/node.rs:1130` | `src/server_h.rs:2332`; `:3577` (lab) | poll; constructor | yes; lab: no | ROLE_SET, RESET_ELECTION |
| StepDown | `core/src/node.rs:1310` | `src/server_h.rs:2173`, `:2330` | poll (snapshot paths) | yes | ROLE_SET, RESET_ELECTION |

The four action kinds are `APPLY_RANGE`, `RESET_ELECTION`, `APPEND_NOOP`
and `ROLE_SET` (`core/src/output.rs:25-39`). `CoreOutput` also carries the
call's log records (`:126-133`); a core function never logs or calls out
(`core/src/output.rs:15-20`, `core/src/logging.rs:1-6`). After the call the
shell runs `run_locked_actions` (`src/server_h.rs:1516-1561`) while still
holding `mtx_`: it prints the log lines, then per action enqueues committed
entries for the apply thread, resets the election timer, appends the
leader's no-op, or queues a leader-change notice, and finally publishes the
mirrors. After releasing `mtx_` it runs `run_unlocked_actions` (`:1566-1589`),
which writes the role-change log entry and fires Mako's leader-change
callback with no lock held (F6, `:1596-1643`), except on the InstallSnapshot
reply path, whose caller still holds the callback-lifetime mutex (§5.5).

### 3.2 Why each call runs to completion, and the exceptions

The core has nothing that can wait. It uses no lock, socket, thread, clock
or fiber: a search of its executable files (all of `core/src/` but
`coupling.rs`) finds none, and Verus admits a call only to a function with a
specification. Its dependencies alone would not rule them out: it depends
only on Verus's crates (`core/Cargo.toml:14-21`) and forbids `unsafe`
(`core/src/lib.rs:5`), but it is not `no_std`, and vstd itself offers a safe
`thread::spawn` and `RwLock` (vstd 0.0.0-2026-08-02-0125, `thread.rs:107`,
`rwlock.rs:338`). Time arrives as data (`now` in
`core/src/event.rs:39`, `:53`; `core/src/node.rs:1365`). Every loop in its
executable code has a `decreases` clause, which Verus checks. Decision Q1
makes this the host's guarantee (docs/verus/modification-plan.md:41).

1. Shell code runs inside a call through the core's type parameters: the
   payload decoder `WireBatch` (`src/server_h.rs:3286-3371`, called at
   `core/src/node.rs:1857`, `:2127`) and the command's `Clone` and `Drop`
   (`src/rusty-rustc/src/lib.rs:399-407`, `:786-787`; clones at
   `core/src/heartbeat.rs:580`, `:696`, `:707`; drops on truncation,
   `core/src/node.rs:2094`). They are C++ casts and reference counts
   (`server.cc:464-467`, `:1562-1597`): no lock, no wait, trusted
   (host-contract.md:111-120).
2. Two actions make the shell issue a further step in the same critical
   section, after the first returns: `RESET_ELECTION` becomes
   `step(ResetElectionTimer)` (`src/server_h.rs:1527-1542` -> `:1487`) and
   `APPEND_NOOP` becomes `step(Propose)` (`:1543-1544` -> `:3498-3506`). The
   core itself calls `step` only from `step_checked` (`core/src/event.rs:364-366`).
3. Read-only queries bypass `step`, as `core/src/event.rs:14-16` allows:
   the timer's gather (`src/server_h.rs:1162-1167`), membership checks
   (`:2826-2828`), the term-change log line (`:1360`) and reads for log lines
   and mirrors.
4. The snapshot paths write core fields directly: 26 writes in
   `OnInstallSnapshotLocked`, `InitializeSnapshotManagerLocked`,
   `InstallSnapshotReplyAcceptedLocked`, `CreateSnapshotLocked` and
   `CompactLogLocked`, all under `mtx_`, each after marking the recording
   (`src/server_h.rs:2235`, `:2076`, `:2152`, `:2808`, `:1330`). With
   snapshots off on every server none of them changes the core: no leader
   sends InstallSnapshot (`core/src/heartbeat.rs:959-961`,
   `src/server_cc.rs:90-92`), recovery and snapshot creation return before
   writing (`src/server_h.rs:1905-1923`, `:2751-2758`), and compaction is
   clamped to index 0, which removes nothing (`core/src/helpers.rs:388-403`,
   `core/src/log.rs:539-541`). A server whose own snapshots are off still
   changes its core on an InstallSnapshot that passes the sender checks,
   before it reaches the storage check that refuses it: a higher term and a
   cleared vote, the leader hint, a role step (`StepDown` or
   `SetFollower`), cleared campaign flags and a timer reset
   (`src/server_h.rs:2306-2346`, refused at `:2402-2412`). The receive path
   never checks `MAKO_RAFT_SNAPSHOTS` (`:3961-3973`, `server.cc:1608-1619`).
5. Three steps run without `mtx_`, before anything else can reach the
   server: `SetIdentity` (`src/server_h.rs:3719`, before the transport
   exists), `Configure` (`:3545`, before the fibers and the apply thread
   start, `:1843-1866`) and the lab-only `SetFollower` (`:3577`). The
   wrapper's comment says so (`:1404-1406`), and so does host-contract.md:51-58;
   `core/src/event.rs:370` does not.
6. A broken premise is not reliably a panic. The core's 17
   `runtime_assert`s and its `unwrap`s are proved not to fire under
   `step`'s precondition (`core/src/event.rs:373-375`), which nothing
   checks at run time. A violation may hit one of them, and
   `panic = "abort"` ends the process; or it may go unnoticed and leave the
   core outside its invariant. Some premises have no run-time check at all
   (`SetIdentity`'s empty configuration and `Configure`'s sorted members,
   `core/src/event.rs:242-247`, are not checked by `core/src/node.rs:233-254`
   or `:262-304`), and the library is built with `cargo build --release`
   (`CMakeLists.txt:1247`) and no `overflow-checks` (`Cargo.toml:80-84`), so
   an arithmetic overflow wraps instead of panicking.

The census script finds 171 direct accesses to core fields in 35 shell
functions (`scripts/verus/core_access_census.py`): 124 on snapshot paths
(the 26 writes among them), 17 in the lab's getters, 30 elsewhere, all
reads.

### 3.3 How a network round trip becomes several events

| Operation | Events, in order | Wait in between (shell, no lock held) | State carried across the wait |
|---|---|---|---|
| Election | StartElection; SettleElection | broadcast, then poll the tally every 200 us for up to 1 s (`rt/src/seam.rs:263-290`) | core: term, own vote, `election_in_progress_`, `election_term_`, `req_voting_` (`core/src/node.rs:756-774`); shell: the campaign's term and last log index and term (`src/server_h.rs:3189-3193`), the tally |
| Replication round | TickHeartbeat; RecvAppendReply per reply; RoundEnd, or AbandonRound | send, then poll the replies every 1 ms for up to min(heartbeat, 100 ms) (`src/server_cc.rs:198-210`, `:278-290`) | core: `round_`, `pending_rpcs_`, `authority_rounds_`, `peers_` (`core/src/node.rs:79-84`, `:50`); shell: one response handle per follower (`src/server_h.rs:922-924`), the round id |
| Proposal | Propose, then the next TickHeartbeat | a wake job starts the round (`src/server_h.rs:3912`, `:2841-2868`) | the log |
| Apply | APPLY_RANGE (an action); Applied per entry | the apply thread polls its queue every 1 ms (`src/server_h.rs:2610-2627`) | the apply queue (`src/server_h.rs:919`) |
| Inbound RPC | RecvRequestVote or RecvAppendEntries | none: one call, then the reply | none |

## 4. Q3: why `src` is large

`src/` has 9,197 lines: 5,236 in production (`server_h.rs` 4,360,
`server_cc.rs` 709, `scheduler_h.rs` 81, `server_pods_h.rs` 52, `lib.rs`
34), 3,292 in the lab files, and 669 in the unused extracted views (§1).
Before Phase 6 moved the core out, `server_h.rs` and `server_cc.rs` had
8,843 lines (`git show 43c57e3ac:src/deptran/raft/src/server_h.rs`, and
`server_cc.rs`); the move took about 3,800 lines out.

`src/server_h.rs` by what the code does (line ranges read at this commit;
the boundaries are a judgement, the totals add up to the file):

| What | Lines | Main ranges |
|---|---|---|
| Mako host interface: env knobs and startup, the apply thread and shutdown, the hand-off to the apply queue, the `TxLogServer`/`RaftSpecific` methods (`Start`, `Serve*`, `IsLeader`, ...) | 988 | 1670-1870, 2511-2742, 3003-3144, 3551-3583, 3668-3974 |
| Snapshot protocol: install, reply, create, compact, recover (outside the verified configuration) | 951 | 1871-2510, 2743-2823, 2950-3002, 1309-1349 |
| Driving the runtime: lock guards, the wake gate, waits, the election-timer fiber | 640 | 99-466, 2830-2948, 4089-4166 |
| Core calls and their effects: the step wrapper, timer reset, action executor, leader notices, mirrors, `RequestVoteImpl`, `AppendLocal` and the no-op | 563 | 1393-1669, 3145-3279, 3443-3550 |
| Inbound RPC glue: `WireBatch`, the `On*` entry points, the handler bodies | 339 | 3280-3371, 4002-4054, 4167-4360 |
| The struct and its constructor | 219 | 858-1076 |
| `extern "C"` kernel declarations | 182 | 467-648 |
| Shell-side types: apply-queue entries, response slots, leader notices, env errors | 157 | 649-805 |
| Lab: the registry (not compiled in production) and the `Lab*` getters (compiled) | 159 | 24-98, 3584-3667 |
| The recorder and printing the core's log records | 123 | 806-857, 3372-3442 |
| Header and separators | 39 | |

1,010 of its lines (23%) are comment-only and 278 are blank.
`src/server_cc.rs` is the heartbeat round (tick `:81-180`, reply collection
`:182-306`, round end `:308-330`, the driver `:332-402`), its kernel
declarations (`:1-80`), and 306 generated lines of C ABI exports
(`:404-709`).

Why so much, when the decisions are in the core:

1. The core is pure, so every effect lives in the shell: taking `mtx_`,
   carrying out actions, publishing mirrors, the fiber loops, their waits
   and the cross-thread wake.
2. The snapshot protocol never moved into the core; it is the second
   largest block and holds most of the direct core accesses (§3.2).
3. Mako's commands and callbacks are C++ objects held as opaque carriers,
   so every copy, call and destruction is a kernel: 69 `extern "C"`
   declarations in `src/server_h.rs` (`:100-105`, `:180-184`, `:484-648`)
   and 13 in `src/server_cc.rs` (`:28-67`).
4. Host duties that are not Raft, in `src/server_h.rs`: environment knobs
   (`:1680-1736`), a fail-closed startup (`:1744-1869`), the apply thread
   (`:2572-2733`), shutdown (`:2533-2564`, `:3781-3813`) and the interface
   Mako calls (`:3752-4002`).
5. Leftovers: the unused extracted views; in `src/server_h.rs`, the `Lab*`
   getters and a dozen lab-only methods compiled into production
   (`GetState` `:1243-1254`, `Disconnect` `:3005-3022`, others called only
   from `src/lab*.rs`), fields never read or never changed
   (`in_applying_logs_` `:901`, `term_mirror_` `:1662-1663`, `n_prepare_`
   and its siblings `:977-979`), and shapes kept for a transpiler that no
   longer runs here (`:1738-1741`, `:1700-1705`, the C++ spellings of
   `:982-984`).

## 5. Q4: one RequestVote round

The candidate's campaign holds `mtx_` twice, briefly: once to start the
campaign and once to settle it, each time for a core call and the steps its
actions ask for (the timer fiber also takes it for brief reads before and
after). It sends and waits with the lock free, and its wait gives the poll
thread back, so the calls other servers make into it run in the gaps. Each
of those holds `mtx_` for one critical section and sends nothing but its
reply. Nothing in the round holds a lock while it waits for another server.

### 5.1 Who runs where

| Actor | Thread | Takes `mtx_`? |
|---|---|---|
| The election-timer fiber, which runs the campaign: one long-lived fiber per server (`rt/src/seam.rs:206-211`; `src/server_h.rs:1859-1866`, `:3059-3066`) | the transport's poll thread | in two short sections, plus brief reads before and after |
| Inbound RPC handlers, run inline without a fiber: Raft registers all four RPCs as "fast" (`rt/src/rpc.rs:386-404`; `src/srpc/rpc/server.rs:1476-1479`) | the same poll thread | once per call |
| Vote reply callbacks, run from the client's frame handling (`src/srpc/rpc/client.rs:1036-1041`, `:2987-2991`) | the same poll thread | never; the tally has its own mutex |
| The heartbeat fiber, which declines every tick while the server does not lead (`src/server_cc.rs:376-379`) | the same poll thread | once per tick |
| `Start` and `Applied` | the submit and apply threads | once per call; neither changes the role |

### 5.2 The normal round: A wins with B's vote

```
who A, candidate: A's poll thread                        A.mtx_ | who B, voter: B's poll thread                       B.mtx_
--- --------------------------------------------------   ------ | --- ---------------------------------------------   ------
E   timer wait: IntEvent, 2-4 heartbeat intervals        free   |
E   gather: lock, raft_election_tick, unlock             brief  |
E   lock                                                 HELD   |
E     step(StartElection): term t+1, vote A, flags set   HELD   |
E     run_locked_actions: step(ResetElectionTimer),      HELD   |
E     publish the mirrors                                HELD   |
E   unlock                                               free   |
E   broadcast: vote_async per peer, frames queued        free   |
E   Fiber::sleep(200 us): park the fiber, yield          free   |
p   writes the frame ======= RequestVote(t+1, A, last index, last term) =======>
                                                                | p   read the frame; a fast RPC, run inline          free
                                                                | in  ServeVote gate: disconnected? not ready?        free
                                                                | in  lock                                            HELD
                                                                | in    step_checked(RecvRequestVote): grant, vote A  HELD
                                                                | in    run_locked_actions: timer reset, mirrors      HELD
                                                                | in  unlock; run_unlocked_actions; reply queued      free
    <======= VoteResponse(max_ballot t+1, vote_granted 1) =======   p writes the frame
in  reply callback (site B): tally mutex, feed           free   |
S   Start (submit thread): lock, REJECTED, unlock        brief  |
p   the sleep's timeout passes: resume E                 free   |
E   tally.decided(): leave the wait                      free   |
E   lock; read the replies through the kernels           HELD   |
E     step(SettleElection): yes quorum, current term,    HELD   |
E     so set_is_leader(true)                             HELD   |
E     run_locked_actions: APPEND_NOOP -> step(Propose)   HELD   |
E     and a wake job; notice queued; is_leader mirror    HELD   |
E   unlock                                               free   |
E   run_unlocked_actions: Mako's leader callback         free   |
E   await_vote_settled: req_voting_ is false             brief  |
p   wake job (own fiber): sets H's IntEvent              free   |
H   tick: lock, step(TickHeartbeat), unlock; send        brief  |
H     AppendEntries carrying the no-op                   free   |
```

E is A's election-timer fiber, H its heartbeat fiber, `in` an inline RPC
handler or reply callback, p the poll loop, and S A's submit thread, a
separate OS thread. HELD means the lock is held after the row; "brief" means
it was taken and released within the row; t is A's term before the
campaign. Had the poll thread wanted `mtx_` while S held it, the whole poll
thread would have blocked in `lock()` until S released it.

### 5.3 Step by step

A1. Timer. The fiber waits 2-4 heartbeat intervals on an `IntEvent` only
shutdown sets (`src/server_h.rs:4113-4120`, `:1369-1375`), then gathers
under `mtx_`: the clock and the read-only query `raft_election_tick`
(`:4121`, `:1162-1167`). The timeout has fired when the server does not lead
and the time since the last reset exceeds the sampled timeout
(`core/src/node.rs:1365-1381`): 0.5-1 s, or 150-300 ms for the preferred
leader, or 1-2 s for the others in the first 5 s (`src/server_h.rs:1453-1476`;
`server.cc:164-217`).

A2. Start, under `mtx_` (`src/server_h.rs:3173-3185`). `start_election`
refuses when stopped, leading, already campaigning, or reset since the
gather (`core/src/node.rs:725-750`); otherwise it pushes a timer reset,
increments the term, votes for itself, sets the campaign flags (`:755-774`)
and returns the term and last log index and term (`:775-782`). The reset
action becomes `step(ResetElectionTimer)`; the mirrors are published
(`src/server_h.rs:1538-1539`, `:1481-1501`, `:1560`). The clock is read
under the lock (`:3176-3177`), whatever `core/src/node.rs:738-741` says.

A3. Broadcast, no lock (`src/server_h.rs:3220-3226`, `rt/src/seam.rs:263-275`,
`rt/src/transport.rs:560-591`). The quorum size counts every site recorded
for the partition, A included (`:332-334`). Each other peer gets one
`vote_async`, which registers the callback and appends the frame to an
outbound buffer without waiting (`src/srpc/rpc/client.rs:2388-2471`;
`src/srpc/rpc/tcp_channel.rs:877-899`); a failed send is ignored
(`rt/src/transport.rs:586-588`).

A4. Wait (`rt/src/seam.rs:276-284`) until peer yes votes reach n/2 or peer
no votes exceed n - n/2 (`rt/src/transport.rs:639-646`), or 1 s passes.
`Fiber::sleep(200)` parks the fiber and yields
(`src/srpc/reactor/reactor.rs:3087-3093`, `:2482-2534`); the poll loop then
writes frames, runs inbound handlers and reply callbacks, and resumes ready
fibers (`:3302-3380`, `:1508-1610`). With three servers a lost campaign
always waits the full second (bugs-found B1).

B1. B's poll thread decodes the request and calls `ServeVote` inline
(`src/srpc/rpc/server.rs:1388-1479`; `rt/src/rpc.rs:426-436`;
`rt/src/service.rs:108-114`). A disconnected or not-yet-ready B answers "no
at the candidate's term" without locking (`src/server_h.rs:3928-3934`);
srpc's own admission flag (`rt/src/transport.rs:236`) is not read on this
path (inference from a search).

B2. Under B's `mtx_` (`src/server_h.rs:4174-4188`),
`step_checked(RecvRequestVote)`. F9 drops a request from B itself, from a
non-member or at term 0, answered as an unavailable server would
(`core/src/event.rs:314-316`; `src/server_h.rs:4232-4241`). Otherwise
`raft_on_request_vote` refuses when stopped, malformed, from a non-voter, at
a stale term or after voting for someone else this term
(`core/src/node.rs:1445-1544`) and grants only to a log at least as up to
date as B's (`:1550-1586`). A higher term is adopted even when refusing
(`:652-681`); a grant records the vote and pushes a timer reset (`:683-695`).

B3. After the unlock the handler returns, and `reply_with` queues the reply
frame (`rt/src/rpc.rs:476-491`; `src/srpc/rpc/server.rs:1246-1285`). B sends
no RPC of its own.

A5. A's reply callback ignores errors and undecodable bodies and feeds the
tally under the tally's mutex, crediting the peer the callback was created
for (F1; the reply carries no voter id) (`rt/src/transport.rs:569-585`,
`:622-637`; `rt/src/rpc.rs:79-83`). No `mtx_`, no core.

A6. Settle, under `mtx_` (`src/server_h.rs:3232-3274`), with no suspension
between the end of the wait and the lock. A reads the quorum size, the
timeout flag and the replies (`:3235-3258`); `election_settle` counts again,
one vote per voter (`core/src/node.rs:850-879`), and decides from the state
it finds now: a higher reply term wins over everything (`:887-919`); a
campaign no longer current is ignored (`:921-942`); a yes quorum at the
current term makes A leader (`:956-1003`); a no quorum, a follower
(`:1026-1051`); otherwise a timeout (`:1052-1076`).

A7. Leader. `set_is_leader(true)` rebuilds the peer table at match 0 and
pushes `APPEND_NOOP` and `ROLE_SET` (`core/src/node.rs:1190-1244`, `:1280`,
`:1303-1304`). Still under `mtx_`, the shell appends the no-op with
`step(Propose)`, queues a wake job and the leader notice, and publishes
`is_leader = true` (`src/server_h.rs:1543-1560`, `:3498-3514`, `:2841-2868`).
After the unlock it fires Mako's callback with no lock held (`:1566-1643`).

A8. `await_vote_settled` re-reads `req_voting_` under the lock every 100 ms
until it is false (`src/server_h.rs:4147-4160`). The wake job sets the
heartbeat fiber's `IntEvent` (`:459-465`, `:312-328`); its next tick sends
AppendEntries carrying the no-op (`src/server_cc.rs:85-180`).

### 5.4 When the other servers call back in

A calls out, and other servers send their own RPCs into A while A's call is
open. The 3-server split vote, where A and B campaign at the same term and C
has already voted for B:

```
who  A's poll thread                              A.mtx_ | who  B's poll thread                          B.mtx_
---  ------------------------------------------   ------ | ---  --------------------------------------   ------
E_A  lock, StartElection(t+1), unlock             brief  | E_B  lock, StartElection(t+1), unlock         brief
E_A  RequestVote to B and C; sleep                free   | E_B  RequestVote to A and C; sleep            free
in   B's RequestVote(t+1): lock; already          brief  | in   A's RequestVote(t+1): lock; already      brief
       voted for A -> (t+1, no); unlock                  |        voted for B -> (t+1, no); unlock
in   replies: B no, C no (tally only)             free   | in   replies: A no, C yes (tally only)        free
E_A  no = 2 is not > 3 - 1: keeps waiting         free   | E_B  decided: lock, settle -> leader,         brief
                                                         |        unlock
                                                         | H_B  tick: AppendEntries(t+1) to A and C      brief
in   B's AppendEntries(t+1): lock; accepted       brief  |
       -> follower, campaign flags cleared;              |
       timer reset; unlock                               |
E_A  1 s deadline: lock, settle ->                brief  |
       IGNORE_STALE, unlock                              |
```

Each handler runs while the other server's campaign fiber sleeps, holds
`mtx_` for one critical section and waits for nothing. A's settle, late
(B1), finds the campaign no longer current.

Every interleaving of the round, and what handles it:

| # | During A's campaign | What happens | Where |
|---|---|---|---|
| 1 | B's RequestVote at the same term reaches A | runs inline while A's fiber sleeps; A already voted for itself, so it answers "no" and changes nothing | `core/src/node.rs:1523-1544` |
| 2 | ... at a higher term | A adopts the term, drops its vote and both campaign flags, and may grant; A's settle then finds the campaign stale, or adopts a still higher reply term | `core/src/node.rs:652-695`, `:882-942` |
| 3 | A new leader's AppendEntries reaches A | a higher term, or an accepted append, makes A a follower and clears both flags, so the settle ignores the stale campaign. A same-term append refused on the log check only resets the timer and sets the leader hint; the campaign then cannot reach a majority, since every voter votes once per term (inference) | `core/src/node.rs:1898-1925`, `:1947-1956` |
| 4 | A's election timer fires again | impossible: the timer fiber is the one waiting; a second campaign would also be refused by the admission and generation checks | `src/server_h.rs:4110-4137`; `core/src/node.rs:732-750` |
| 5 | A vote reply arrives after the wait, or after the settle | it lands in this campaign's own tally, which nothing reads any more; the next campaign makes a new one | `rt/src/transport.rs:563-565` |
| 6 | A reply arrives between the end of the wait and the settle, or during the settle | impossible: same thread, no suspension point in between | `rt/src/seam.rs:278-290`; `src/server_h.rs:3226-3274` |
| 7 | The same voter's reply is delivered twice | counted once, by the tally and again by the core | `rt/src/transport.rs:622-628`; `core/src/election.rs:84-86` |
| 8 | The voter is the leader of a lower term, mid-round | its handler steps it down; its heartbeat fiber sees the mirror at its next check and abandons the round, or its next tick declines | `core/src/node.rs:669-670`; `src/server_cc.rs:221-224`, `:293-300` |
| 9 | A's submit thread calls `Start` | `Start` takes `mtx_` on its own thread and is refused (A does not lead); if the poll thread wants `mtx_` meanwhile it blocks briefly, since `Start` waits for nothing while holding it | `src/server_h.rs:3886-3914`; `raft_worker.cc:859-863` |
| 10 | A's apply thread records an applied index | one `Applied` step under `mtx_`; no role or term change | `src/server_h.rs:1290-1307` |
| 11 | A would send RequestVote to itself | never: the broadcast skips self, and F9 would drop it | `rt/src/transport.rs:324`; `core/src/event.rs:315` |
| 12 | A peer is down, disconnected or not ready | the send fails and is ignored, or the peer answers "no at the candidate's term"; either way a refusal or a missing vote | `rt/src/transport.rs:572-574`, `:586-588`; `src/server_h.rs:3928-3934` |
| 13 | Code on a thread that holds `mtx_` tries to take it again | the owner check aborts with a message instead of hanging | `server.h:243-250`; `server.cc:106-123` |

The wait ends only when the tally decides or the second passes; a campaign
cancelled by case 2 or 3 still waits it out (`rt/src/seam.rs:278`). That
costs time, not safety.

### 5.5 Why the call-back pattern cannot deadlock here

Three properties break the cycle "A waits for B, B calls A, A's handler
waits for A":

1. No one holds `mtx_` while waiting. Every suspension point in Raft is
   outside the lock's scopes: the heartbeat wait (`src/server_cc.rs:370`),
   the reply polling (`:289`), the vote wait (`src/server_h.rs:3222`), the
   timer wait (`:4117`), `await_vote_settled` (`:4155`) and the shutdown
   barrier (`:3803`); docs/verus/README.md:88 states the rule.
2. Waiting gives the thread back: a fiber's wait returns to the poll loop
   (`src/srpc/reactor/reactor.rs:2482-2534`), which runs the inbound handlers
   and reply callbacks on the same thread.
3. No handler or reply callback suspends or waits for another server. The
   four Raft RPCs run inline, without a fiber, so a handler cannot suspend.
   A RequestVote or AppendEntries handler holds `mtx_` for one critical
   section, one checked step plus a `ResetElectionTimer` step when it resets
   the timer (`src/server_h.rs:1527-1542` -> `:1487`), and sends nothing but
   its reply. After unlocking, if its step stepped a leader down, it runs
   Mako's leader-change callback on the poll thread (`:4187`, `:4273` ->
   `:1587`, `:1638`; `core/src/node.rs:669-670`, `:1911-1914`,
   `:1950-1951`). With snapshots on, the InstallSnapshot handler takes the
   apply gate before `mtx_` (`src/server_h.rs:4063-4065`), so it can block
   for as long as the apply thread runs Mako's apply callback under that
   gate (`:2636-2685`), and it runs the embedder's prepare-snapshot callback
   under both (`:2432-2438`). Vote and AppendEntries reply callbacks touch
   only their tally or reply slot (or store an error). With snapshots on,
   an InstallSnapshot reply callback takes the callback-lifetime mutex and
   then `mtx_` (`rt/src/transport.rs:507-510`, `rt/src/snapshot.rs:419`,
   `server.cc:1376-1383` -> `src/server_h.rs:2136`). It may step the core
   down or advance a follower's indices (`:2160-2175`, `:2191`), and it
   fires Mako's leader-change callback with the lifetime mutex still held
   (`:2142`). It runs from the poll loop, never inside a critical section
   (the inline no-peer delivery passes term 0 and returns before taking any
   lock, `server.cc:1371-1375`), so it cannot re-enter `mtx_`.

If a fiber ever suspended while holding `mtx_`, the next lock on that
thread would abort through the owner check (`server.h:243-250`) instead of
hanging. Nothing in the types prevents such a suspension; the convention
and the abort do.

## 6. The replication round, and B17

The leader's heartbeat fiber runs `HeartbeatDriver::run`
(`src/server_cc.rs:357-394`). Each turn:

1. Wait on the wake gate for one heartbeat interval, 5 ms by default
   (`src/server_cc.rs:370`; `server.h:169-173`), or until a wake job ends the wait
   (`src/server_h.rs:2841-2868`).
2. Tick (`src/server_cc.rs:85-180`): under `mtx_`, `step(TickHeartbeat)`
   with `IsLeaderLocked()`. The core declines the round when the server
   does not lead (`core/src/heartbeat.rs:259-270`, `:835-842`); otherwise it
   picks each follower's payload, reading the commit index it sends in the
   same call (F3, `:917`). It skips a follower whose earlier AppendEntries
   is still in flight (`:908-911`), one it sends a snapshot instead
   (`:959-976`) and one whose previous entry is missing (`:990-997`). After
   the unlock the shell builds and sends at most one AppendEntries per
   follower and keeps the response handles (`src/server_cc.rs:134-178`).
   Only InstallSnapshot is sent under the lock (`:101-123`), and only with
   snapshots on.
3. Collect (`:196-306`): poll the handles every 1 ms for at most
   min(heartbeat interval, 100 ms); hand each completed reply to
   `step_checked(RecvAppendReply)` under `mtx_` (`:243-261`). Stop early
   when a reply steps the server down (`:266-268`) or the unlocked mirror
   says it no longer leads (`:221-224`), and then `AbandonRound`
   (`:293-300`). Also
   stop, without abandoning, once the round has a quorum or no reply of
   this round is outstanding (`:273-277`); if a reply from an older round
   freed a follower's slot, another round is requested at once
   (`:301-305`).
4. Round end (`:309-330`): `step(RoundEnd)` advances the commit index and
   settles read-index authority; if the commit moved, another round is
   requested at once.

A follower handles each AppendEntries as one `step_checked` under `mtx_`
(`src/server_h.rs:4249-4359`), decoding the payload only after the
authority gates (`core/src/node.rs:1817-1858`). `APPLY_RANGE` hands newly
committed entries to the apply queue under `mtx_`
(`src/server_h.rs:3075-3144`); the apply thread runs Mako's callback under
its apply gate, not `mtx_`, then records `Applied` under `mtx_`
(`:2630-2685`).

B17 is the example of a check made outside the lock:

```
src/server_cc.rs:310   if !server.IsLeader() { return; }          // mirror, no lock
src/server_cc.rs:315   let _lock = RaftLockGuard::new(&mut server.mtx_);
src/server_cc.rs:316   let is_leader: bool = server.IsLeaderLocked();
src/server_cc.rs:317   server.step(Event::RoundEnd { is_leader }, &mut out)
```

Phase 3 calls `raft_commit_advance` first and unconditionally
(`core/src/heartbeat.rs:1689`); `is_leader` reaches only the read-index
settlement (`:1690-1696`), whereas phase 0 returns before its advance when
not leading (`:259-270`, `:358`). So the check protecting the commit rule
is the one at `:310`, outside the lock. A server that stepped down between
`:310` and `:315`, and took a newer leader's entries meanwhile, would count
its old term's match indices against an entry of the new term and could
commit an entry no majority holds (docs/verus/bugs-found.md:415-516).
`core/tests/b17_round_end.rs` reproduces this at the core. The proof takes
"the round end runs while leading" as a premise (host-contract.md:105,
:132-143).

Can the shell reach it? On this lane, inference says not in production.
Every step that can change the role runs on the poll thread, nothing
between `src/server_cc.rs:310` and `:315` suspends, and if `:315` waits for
another thread's `mtx_`, the whole poll thread waits, so no handler runs;
the other threads (submit, apply, shutdown) never change the role. The
mirror is published at the end of every critical section that can change it
(`src/server_h.rs:1560`, `:4080`). The bug log's interleaving, the driver
"blocks on `mtx_` while the RPC handler takes D's messages"
(bugs-found.md:443-444), needs a handler on another thread. Lab builds have
one: the harness calls `ServeVote` and `ServeAppendEntries` from site 0's
own thread (`src/lab.rs:520`, `:534`; `src/deptran/server_worker.cc:135-138`).
The core defect stands either way; the proposed fix moves the check into
the core (bugs-found.md:504-510). B17's entry said "nothing excludes it"
until this document was written; its reachability now says the above
(bugs-found.md:473-499).

One premise gap does reach production, at shutdown (bugs-found B18, found
while writing this). `IsLeaderLocked()` is false
whenever `looping_` is (`src/server_h.rs:1236-1240`), and
`PrepareForShutdown` clears `looping_` under `mtx_` on the shutdown thread
(`:3781-3792`). If that happens between the driver's mirror check and its
lock (`src/server_cc.rs:221` and `:245` for a reply, `:310` and `:315` for
the round end), the step gets `is_leader = false` while the core still
leads, which its premise excludes (`core/src/coupling.rs:2611`, `:2622`).
The protocol is unharmed: the reply is ignored unless its higher term steps
the core down, and the round end commits as the leader the core still is.
But that step is outside the certificate.

## 7. Threads and `mtx_`

The transport's poll thread does nearly everything; four other threads
each make one kind of core call, and the apply thread also compacts the log
outside the verified gates and, with snapshots on, creates snapshots.
`mtx_` serializes them, by convention.

| Thread | Created at | Core calls | Other Raft state it touches |
|---|---|---|---|
| Transport poll thread, one per Raft server | `rt/src/transport.rs:198`; spawned at `src/srpc/reactor/reactor.rs:3649-3672` | all events except `SetIdentity`; `Applied` only on the snapshot paths (§3.1) | handlers, the heartbeat and election fibers, the startup job, wake jobs, reply callbacks (tally and reply slots only, except the InstallSnapshot reply's, which takes `mtx_`, §5.5), Mako's leader-change callback (no lock held, except the callback-lifetime mutex on the InstallSnapshot reply path) |
| Submit thread, one per worker | `raft_worker.cc:743-753` | `Propose`, through `Start` | the submit queue |
| Mako's transaction threads | Mako (`src/mako/sto/Transaction.cc:833`) | `Propose`, only when no submit thread runs (`src/deptran/raft_main_helper.cc:644-648`) | the mirrors (`src/deptran/raft_main_helper.cc:1166-1167`, `:988-989`); `SetPreferredLeader`, an atomic (`src/server_h.rs:3833-3842`) |
| Apply thread, one per server | `server.cc:987-990`, from `src/server_h.rs:1843` | `Applied`; an idle-time read of the commit index (`src/server_h.rs:2614-2617`); after each entry whose index is a multiple of 5000, `CompactLog` under `mtx_`, which reads core fields and calls `raft_log_.compact_through` outside `step`, removing nothing while snapshots are off, and is skipped under `MAKO_RAFT_VERIFIED_GATES=1` (`:2720-2728`, `:1342-1349`, `:1311-1339`); with snapshots on, `MaybeCreateSnapshot` -> `CreateSnapshotLocked` under the apply gate and `mtx_`, which runs the embedder's create-snapshot callback, writes `snapidx_` and `snapterm_` and compacts (`:2701-2716`, `:2952-2970`, `:2799-2818`) | the apply queue; Mako's apply callback, under the apply gate, not `mtx_` (`src/server_h.rs:2636-2666`) |
| Setup thread (the caller of `setup()`) | Mako | `SetIdentity`, without the lock (`raft_worker.cc:308`) | waits for startup on a condition variable (`src/server_h.rs:3763-3775`) |
| Shutdown thread | Mako | `Propose` while draining the submit queue (`raft_worker.cc:776-778`) | sets the stop flags under `mtx_`, then waits outside it (`src/server_h.rs:3781-3813`) |
| Lab harness thread (lab builds) | `src/deptran/server_worker.cc:135-138` | handlers and getters, called directly (`src/lab.rs:520`, `:534`) | |

There is no timer thread (timeouts are reactor waits,
`src/srpc/reactor/reactor.rs:1568-1574`). srpc's reconnect threads
(`src/srpc/rpc/client.rs:900`, `:1405`) only complete callbacks with an
error code, which Raft's callbacks ignore or store (inference). In the
default single-group mode one poll thread also serves the other partitions'
ports (`src/deptran/raft_main_helper.cc:377-389`).

What `mtx_` is: a C++ `RaftCheckedMutex`, a `std::mutex` plus the owner's
thread id, whose `lock()` aborts if the calling thread already holds it
(`server.h:224-275`, `server.cc:106-123`). Rust sees a 48-byte opaque
carrier (`src/rusty-rustc/src/lib.rs:668-670`) and locks it through
`RaftLockGuard`, which calls two C kernels (`src/server_h.rs:100-146`).

What it guards: the whole core, and the shell fields documented as "under
mtx_" (`install_out_`, `src/server_h.rs:925-929`; the snapshot manager and
callbacks; the shutdown flags' transition, `:3783-3792`; the mirrors'
writes). It does so by convention: `mtx_` and `core` are sibling fields
(`:871-873`), every `RaftCore` field is `pub` (`core/src/node.rs:38-104`),
and `server.h:189-200` says so. It does not stop a thread that skips it,
and nothing checks that a core access holds it; the owner check catches
only a second lock on one thread. The discipline rests on the single step
wrapper, the `*Locked` names and "CALLER MUST HOLD mtx_" comments, the
census (which measures and does not gate,
`scripts/verus/core_access_census.py:15`) and the replay (§9); the planned
compiler-enforced accessor was not built (docs/verus/reports/phase-6.md:174-178).

What other threads read without it (F8): the four mirrors
(`src/server_h.rs:952-959`), written with release stores at the end of every
`run_locked_actions` and after recovery and InstallSnapshot (`:1560`,
`:1650-1664`, `:1805-1809`, `:4080`), read with acquire loads by
`IsLeader`, `GetLeaderHint` and `CommitIndex` (`:3817-3828`, `:3876-3878`);
`term_mirror_` has no reader. Also `appliedIndexForWait_` (`:1228-1233`),
the flags `stop_`, `looping_`, `rpc_ready_`, `disconnected_` and
`preferred_leader_site_id_`, all atomics. The response handles and the
batch buffer belong to the heartbeat fiber alone (`:922-950`);
`heartbeat_interval_us_` is a plain field the lab writes while the loops
read it (bugs-found B14).

Why there is no deadlock:

1. No wait under the lock (§5.5).
2. One lock order whose inner locks are leaves: the callback-lifetime
   mutex, taken only by the InstallSnapshot reply callback and by shutdown
   (`server.cc:1376`, `:998`), then the apply gate
   (`state_machine_apply_mtx_`), then `mtx_`, then the apply queue, the
   leader-notice queue, the wake gate's owner slot or the vote tally, and
   on the snapshot paths the snapshot store's slot, the transport's
   `installs` set and srpc's client mutexes (`src/server_h.rs:2211-2215`;
   `server.cc:1361-1366`; `rt/src/snapshot.rs:36-40`;
   `src/server_cc.rs:104-123`, `rt/src/transport.rs:501-502`,
   `src/srpc/rpc/client.rs:2392-2449`, `src/srpc/rpc/tcp_channel.rs:878`);
   srpc releases its locks before it runs or drops a callback
   (`src/srpc/rpc/client.rs:2987-2991`, `:2467`, `:1244-1254`). Every path
   that takes the gate and `mtx_` takes the gate first
   (`src/server_h.rs:1938-1940`, `:2636-2682`, `:2957-2959`, `:4063-4065`),
   and no code takes `mtx_` while holding an inner lock (inference from
   reading each use). The new
   leader's no-op takes the owner slot under `mtx_` (`:1543-1544` ->
   `:3513` -> `:260`), contrary to the comment on `Start` (`:3880-3882`) but
   in this order.
3. Mako's callbacks run outside `mtx_`: the leader-change callback with no
   lock (on the InstallSnapshot reply path, with only the callback-lifetime
   mutex), the apply callback under the apply gate only. The embedder's
   snapshot callbacks run under `mtx_` (`server.h:232-237`), outside the
   verified configuration.
4. Re-entry aborts instead of hanging.

Contention remains: while the submit or apply thread holds `mtx_`, the poll
thread blocks in `lock()` and runs nothing (docs/verus/README.md:108-109).
With snapshots off those sections are short and never wait for the poll
thread; with snapshots on, the apply thread's snapshot creation holds the
apply gate and `mtx_` for as long as the embedder's create-snapshot
callback takes (`src/server_h.rs:2957-2959`, `:2799-2805`). One caveat
(inference): every thread or fiber that reaches the server through a raw
pointer (the C ABI, the RPC service, the fibers) makes its own
`&mut RaftServerBase` (`src/server_cc.rs:417-709`,
`rt/src/service.rs:88-91`). The election-timer fiber holds one across its
suspension (`src/server_h.rs:4131`), the heartbeat driver across every
wait of its loop (`src/server_cc.rs:358`, waits at `:370` and `:289`; both
wake-gate waits take `&mut self`, `src/server_h.rs:2882`, `:2896`), and the
apply thread for its whole life (`src/server_cc.rs:644-646` ->
`src/server_h.rs:2572`); `Start` takes `&mut self` on the submit, shutdown
and Mako threads, and `IsLeader` and `GetLeaderHint` on any Mako thread
(`:3884`, `:3817`, `:3826`; `src/server_cc.rs:522-531`, `:580-585`).
`mtx_`, the atomics and the per-field mutexes serialize the accesses that
matter (`heartbeat_interval_us_` has none of them, B14), but the references
alias, across threads too, which Rust's aliasing rules do not allow
(recorded as bugs-found B19).

## 8. The two kinds of events

They share a name and nothing else.

| | srpc reactor event (`IntEvent`, `TimeoutEvent`) | raft-core `Event` |
|---|---|---|
| What it is | a condition a fiber parks on: an `IntEvent` is ready when its value reaches a target, a `TimeoutEvent` when its time has passed (`src/srpc/reactor/reactor.rs:497-503`, `:656-658`) | a plain value, one of 18 inputs to the core (`core/src/event.rs:24-108`) |
| Consumed by | the reactor's run loop, which resumes the parked fiber (`src/srpc/reactor/reactor.rs:1508-1610`) | `step` or `step_checked`, which returns a `Reply` before the caller goes on |
| Thread | only the poll thread that owns it; a set or wait elsewhere aborts (`rt/src/seam.rs:98-128`) | any thread holding `mtx_`, except `SetIdentity`, `Configure` and the lab constructor's `SetFollower`, which run without it before anything else can reach the server (§3.2 item 5) |
| Waits? | waiting is its purpose (`src/srpc/reactor/reactor.rs:2482-2534`) | never |
| Raft uses it for | the wake gate's two waits, the heartbeat fiber's and the timer fiber's (`src/server_h.rs:365-415`; `rt/src/seam.rs:82-85`), and every `Fiber::sleep` (the vote wait, reply polling, `await_vote_settled`) | every decision of the protocol |

Work crosses threads on a third mechanism, the poll thread's job channel
(`src/srpc/reactor/reactor.rs:2215-2219`, run in a new fiber at
`:3467-3474`): the worker posts `EnsureSetup` on it and `RequestReplication`
the wake job (`rt/src/seam.rs:155-164`). Core events never travel on it.

## 9. The replay crate

With `MAKO_RAFT_REPLAY_DIR` set when a server is built
(`src/server_h.rs:3386-3401`), each server writes `<dir>/<pid>.<n>.rec`,
flushing every line (`:3409-3414`). The `step` and `step_checked` wrappers
write one line per call (`:1407-1443`):

```
E <event and its fields> | <log level> | A <action>... L <log record>... R <reply>
C ...       the same, for step_checked; a dropped message ends with "R dropped"
T <why>     a write to the core that bypassed step
```

The event is recorded as the shell built it, clock readings included
(`replay/src/lib.rs:144-175`); a command appears only as a 64-bit FNV-1a
digest of its wire bytes (`src/server_h.rs:3426-3437`). The crate's header
shows four sections (`replay/src/lib.rs:5-7`); the writer and the parser use
three (`:296-298`, `:765`). `T` lines come from the five snapshot functions
of §3.2 and the lab's direct writes (`src/lab_snapshot_cases.rs:1259`).

`replay()` (`replay/src/lib.rs:754-794`) feeds each recorded event to a
fresh core over digest-only commands, renders the result with the same
writer and compares the text, stopping at the first `T` line. The test
`core_replay` (`replay/tests/core_replay.rs:13-47`) does this for every
`.rec` file and requires no mismatch; with the variable unset it passes
vacuously (`:14-17`). It is run by hand (`:7-8`), as the plan's equivalence
check A.4 item 4 (docs/verus/modification-plan.md:1047-1050); no build
target or script runs it. Phases 6 and 8 replayed a lab run (31,792 steps,
every file stopping at a `T` line), a G1 run (41,361) and a G4 run (108,318)
with no mismatch (docs/verus/reports/phase-6.md:106-110,
reports/phase-8.md:99-103).

It shows that on those runs the core's output is a function of the recorded
events alone, that before the first `T` line the shell changed the core only
through `step`, and that later core changes (Phase 8's proof text included)
changed no output. It does not show that the shell built the right events
or carried out the actions correctly, anything about threads (it replays one
serialized history per server), anything across servers, anything after a
`T` line (so nothing about snapshots), the reads that bypass `step`, or
paths the runs never took.

## 10. Where the verification code is

Skip these when reading for behaviour. Cargo never compiles them; Verus
does (`scripts/verus/verify_core.sh`).

| What | Where |
|---|---|
| The coupling to the spec, the per-node certificate, the cluster theorem | `core/src/coupling.rs`, the whole file, compiled only under `cfg(verus_keep_ghost)` (`core/src/lib.rs:27-29`) |
| Ghost fields of the core | `core/src/node.rs:105-116` |
| Contracts and proofs inside the executable files | lines starting with `requires`, `ensures`, `invariant`, `decreases`, `proof {`, `let ghost`, `assert(`, `spec fn`, `proof fn`, and comments tagged `[M12]`; `admits` and `message_admitted`'s `ensures` in `core/src/event.rs:237-302` |
| A standalone model of the commit rule | `verus/commit_rule.rs`, run with `verus` by hand (`verus/commit_rule.rs:22`) |
| The frozen spec version | `verus/spec/` |
| Scripts and documents | `scripts/verus/` (`verify_core.sh`, `ledger_lint.py`, `core_check.sh`, ...), `docs/verus/` |

Other out-of-date comments: `rt/src/seam.rs:4` calls `src/` "the core";
`server.h:197-200` gives a reason that no longer holds (`mtx_` is already
non-recursive, `:227-228`); `server.h:213-221` says the timer loop holds a
`c_void` (it holds a typed pointer, `src/server_h.rs:4097-4100`);
`src/server_h.rs:3777` says `PrepareForShutdown` runs on a fiber (it runs on
the shutdown thread, `raft_worker.cc:611`); the "wakeup" of
`src/server_h.rs:650-657` does not exist (the wait polls);
`core/src/heartbeat.rs:1074-1089` says the caller steps down (the core does,
`:1507`); and `src/server_h.rs:4295` and `core/src/election.rs:144` name
functions that no longer exist.

`scripts/verus/ledger_lint.py --stats` classifies every core line with
the scanner at `scripts/verus/ledger_lint.py:57-68` and `:143`. At this
commit 4,580 of the core's 10,487 lines are ghost (node.rs 733,
heartbeat.rs 464, log.rs 328, authority.rs 354, coupling.rs 2,271, the rest
under 140 each), about 2,240 are comments or blank, and cargo compiles
about 3,680. The core's tests (`core/tests/`) are ordinary cargo tests run
by hand; `b17_round_end.rs` is ignored because it fails until B17 is fixed.
