# Converting Raft's RPC path to Rust — the island plan

**Revision 5 (2026-09-26) changes the destination: Raft becomes two-lane,
like srpc -- one Rust source, a C++ runtime and a Rust runtime. Read "The
two-lane plan" right before the TODO first; it supersedes the single-lane
destination the rest of this file was written toward.**

Third revision underneath. No longer only a plan: stages 0 through 3 are built and
verified apart from 3e, which is half done, so most of what follows is a record of what was done
and what it cost. The first two revisions were refuted by adversarial
verification and this one is built on what those refutations established.
Every claim is measured, with file:line. Where something is unverified it says
so, and where a claim was later found wrong it is corrected in place with the
measurement that overturned it rather than quietly edited out.

## History, so the reasoning is auditable

- **v1** proposed Raft's RPC on the rustc srpc lane with Mako and Paxos on the
  C++ lane, carrying the AppendEntries payload as opaque bytes. Refuted: Raft
  *does* read into the payload (`server.cc:1705-1726`), and
  `SerializableEnvelope` is unframed (`serializable_envelope.rs:111-120`), so
  "opaque bytes" was not even expressible.
- **v2** proposed keeping the C++ shim, keeping Raft's Rust logic, and swapping
  only the bottom srpc layer. Refuted on six of seven boundaries. The decisive
  one: srpc is *above* Raft as well as below it, so layer 3 mints layer 1's
  arguments and cannot be swapped underneath it.

Both failures shared a cause: they drew the boundary **inside** the RPC stack.

## The design

*(Revisions 1-3. Revision 5 keeps the island idea for the Rust lane but no
longer retires the C++ lane: see "The two-lane plan".)*

**Move the Raft island in one cut, and push the FFI boundary outward.**

The unit that changes lanes is not a layer but everything Raft owns:

```
  server (already Rust)  +  commo  +  service
  +  the Raft slice of rcc_rpc.h (rcc_rpc.h:495-1096, 602 lines)
  +  the poll thread they share
```

and the C++ boundary moves *out* to Mako's embedder API, which already exists
and is **already bytes on both ends**:

```
  in   RaftWorker::Submit(const char* log, int len, uint32_t par_id)   raft_worker.cc:760
  out  (log, len, par_id, slot_id, queue)                             raft_worker.cc:1071-1074
```

`par_id` already travels inside the payload at byte offset 12
(`application_log.cc:50`).

## The two-lane plan (revision 5, 2026-09-26)

**This section replaces the destination of everything below it.** Revisions
1–3 aimed at ONE Raft on the Rust srpc lane, with the C++ RPC slice retired
(stage 5). The goal is now the model srpc itself uses: **one Rust source, two
runtimes.**

> Raft is Rust source, with a C++ runtime (that calls the C++ srpc runtime)
> and a Rust runtime (that calls the Rust srpc runtime). A few low-level
> things may stay C++, but the Rust lane is a Rust runtime, including the
> message types and the poll thread.

The TODO below is still the record of what stages 0–3 did and why. The parts
that assumed a single lane are superseded here and marked where they stand:
3e's C++ deletions, 4a, 4b and 5a.

### Status (kept current as steps land)

| step | state | where |
|---|---|---|
| H1 docs committed | done | `65754b66f` |
| H2 mako-dev merged | done -- 22 conflicts resolved, each recorded | `70fc2c0da` |
| R1-R3 reactor fix | done -- latency back to the pre-regression binary | `edc5db890`, data `4149bf438` |
| R4 mako-dev | applied in the mako-dev worktree, awaiting its build | -- |
| L1 crate split | done (raft + raft-rt, one workspace) | `217cc26d4` |
| S1b seam split | done (server.cc host / server_seam_cpp.cc / rt seam.rs) | `217cc26d4` |
| T1, T2, T4 Rust lane | done -- RaftLabTest 25/25 on both lanes | `217cc26d4` |
| large payloads (both lanes) | done -- byte-bounded batches, two srpc hot paths | `b6141e723` |
| Rust lane zero-copy payload | done | `9a361eccd` |
| T3 mixed-lane cluster | done -- 12/12 across three mixes (1 early failure unexplained) | `9a361eccd` |
| T4 suites + paired trial | done -- see below | `9a361eccd` |
| **full sweep vs the C++ baseline** | **done -- 624/624 runs; see below** | `docs/performance/raft-rust-9a361eccd` |
| T5 default lane = rust | done; hybrid stays built and lab-tested (`ci.sh raftLabTestHybrid`) | this commit |
| S1 kernel classification | done -- computed by gen_correspondence.py, checked by the build | this commit |
| S2 runtime handles | done differently -- the thread binding is checked, not moved; see below | this commit |
| S3 lane-neutral replies | done by construction -- see below | `217cc26d4` |
| M payload types | **not done, deliberately** -- see below | -- |
| L2 lab without custom macros | done (the transpiled lab can no longer be hollow) | this commit |
| L2 (rest)-L4 transpiled C++ lane | next | -- |
| D1-D4 | D1 nothing left to delete (dead snapshot send removed), D2 by design, D3 nothing to collapse (all 31 exports used), D4 done | this commit |

**The full sweep** (`docs/performance/raft-rust-9a361eccd`, 3 trials per
point, against the C++ baseline 412c225a): 624 of 624 runs succeeded, and of
624 metric points per metric --

| metric | better | within noise | worse, < 5% | flagged |
|---|---|---|---|---|
| throughput | 34 | 167 | 6 | 1 |
| p50 latency | 203 | 5 | 0 | 0 |
| p99 latency | 196 | 12 | 0 | 0 |

The one flag is the baseline over-delivering (69.4/s applied at 65/s offered,
draining backlog); the Rust lane applied exactly 65.0/s there, and from 72/s
up it keeps the offered rate where the baseline falls behind.

**S2, done differently.** Moving the wake gate's handles out of the core, as
S2 said, would have duplicated the gate's wait/wake logic in both lanes'
seams. The handles stay opaque carriers the core never reads; what S2 was
protecting against -- a thread-bound Rust `IntEvent` inside a `Send + Sync`
server -- is now CHECKED: the Rust seam aborts with a message if an event is
set or waited on off its owner thread (`rt/src/seam.rs`, owner_thread_check).
RaftLabTest runs with the check on.

**Execution order changed, deliberately.** The plan ordered L (the C++ lane
from the core) before T (the Rust lane). T ran first, on the user's explicit
priority of a Rust runtime benchmarked against the base. Nothing in T depended
on L: the seam kernels are the existing extern "C" ones, so the Rust lane
needed only L1 (the crate split) and S1b (the file split).

**Decisions recorded as they were taken:**

- **T1, the four vote differences: match C++ on all four.** Correctness is a
  hard constraint and the lab's timing cases were tuned against the C++
  lane. So the Rust tally counts peer votes against `n/2`, loses only past
  `n - n/2` rejections, takes the term only from non-negative replies
  (seeded 0), and reads membership from the whole recorded partition, self
  included -- the worker records every site of every partition, as
  Communicator does.
- **T1, the vote's wake: poll, not park.** The campaigning fiber polls the
  tally every 200 us for at most 1 s. A reply callback must be `Send` and an
  `IntEvent` is neither `Send` nor `Sync`, and elections are not a hot path.
- **T2, what replaces ReconnectToSite: nothing, yet.** Nothing on this
  branch calls it (the C++ caller was mako-dev's NotifyRestart RPC, which
  this branch's reduced RaftService does not have). `add_peer` retries for
  Communicator's 120 s at 1 s intervals.
- **Wiring shape.** The Rust-lane worker code is its own files
  (`raft/raft_lane.h`, `raft_lane_rust.cc`), reached by one-line
  `#if MAKO_RAFT_LANE_RUST` hooks in the existing workers rather than by
  moving the C++ lane's code into per-lane files -- which would have turned
  every future mako-dev merge of those files into a conflict.
- **Build layout.** One build tree per lane (`build`/`build_raftlab` hybrid,
  `build_rust`/`build_rust_lab` rust) rather than L4's suffixed binaries from
  one tree. T3's mixed cluster takes a binary per replica instead
  (`examples/test_1shard_replication_simple_raft.sh`, `BIN_*`).
- **Logging and the clock stay HOST for now** (S1 classified them SEAM). The
  Rust lane still logs through the process's one C++ logger, so Raft's lines
  interleave with Mako's. Revisit in S1.

**M is withdrawn, and this is the reason.** M1 asked for a Rust payload type
byte-identical to today's envelopes. Reading the encodings shows what that
means: a `janus::Command` is `[v32 kind][payload]` with NO length, a
`TpcCommitCommand` nests a second `Command` (a `LogEntry`, or a legacy
`VecPieceData` still accepted on apply) followed by an optional third
(`ViewData`), and fourteen kinds are registered in `mako_commands.h`. An
unframed polymorphic envelope can only be delimited by decoding it, so Rust
could not even find where a commit ends without re-implementing every kind
that can appear -- a second implementation of Mako's registry-dispatched
serialization, which would then have to track Mako's C++ types on every
mako-dev merge. That is a second authority over Mako's wire format, against
the two constraints this branch keeps (Mako and Paxos undisturbed; mergeable
with mako-dev). And M's performance motive is gone: the Rust lane no longer
copies payloads (the command is encoded straight into the frame and decoded
from a slice of it), and it now beats the C++ lane at large payloads. So the
payload stays a HOST object -- one of the "low-level things" the goal allows
to stay C++ -- and M3's generator change is unnecessary: the opaque field's
arithmetic framing is correct once `from_body` measures from the argument
bytes.

**S3 is satisfied by construction.** The core already sees only lane-neutral
values: PHASE 2 polls a plain `AppendRespView` through
`raft_append_response_read`, "no peer" is completed-and-failed on both lanes,
and the InstallSnapshot reply is delivered to a host-owned context
(`raft_snapshot_reply_deliver`, `lane_kernels.h`) with 0 inline and no lock
when there is no peer. Each lane fills the reply carrier its own way.

**T4's numbers** (ABBA, 25 pairs each, B = rust against A = hybrid, one host,
`scripts/raft_perf/paired_trial.sh`):

| point | throughput | p50 | p99 |
|---|---|---|---|
| 4 KB @ 240/s | +0.00% | -1.57% (23 of 25, p < 0.001) | -0.73% (19 of 25, p = 0.015) |
| 4 KB saturation | +40.85% (25 of 25) | -29.0% (25 of 25) | -26.8% (25 of 25) |

and, after the Rust lane stopped copying payloads (the command is encoded
straight into the request frame through a C++ `EmitSink`, and the handler
decodes from a slice of the frame -- `AppendEntriesRequestRef`), large entries,
2-3 trials each:

| point | hybrid | rust | C++ baseline 412c225a |
|---|---|---|---|
| 286 KB saturation, entries/s | 380-413 | 682-696 | ~280 |
| 1 MiB saturation, entries/s | 107-114 | 183-188 | ~66-71 |
| 286 KB @ 45/s, p50 | 6.3 ms | 3.0-3.1 ms | 7.2 ms |

**Found while measuring T: a second srpc regression, in both lanes.** At
saturation with large entries both lanes collapsed (286 KB at ~10/s) with
leadership flapping. The batch cap counted entries only, so a catch-up batch
of 256 x 286 KB = 73 MB exceeded srpc's 64 MiB frame limit and was refused
and re-sent forever; separately, srpc's inbound compaction was quadratic in
the backlog and its frame copy was a per-byte loop. Mako's dbtest replication
suite had been hitting the same bug on this branch since the subtree pull:
followers replayed 155-368 batches where they now replay ~8,000.

**Found while building T, not anticipated by the plan** -- three wire**Found while building T, not anticipated by the plan** -- three wire
defects, each of which would have broken the Rust lane on real traffic:

1. Stage 2c's claim that "Rust holds the whole frame as `Request::body`" was
   wrong. `body` also holds the frame header, so `from_body` measured `cmd`
   from the wrong origin and read every field five bytes off. It now
   measures from the argument bytes `req.src` points at. Found by the first
   AppendEntries sent over real TCP.
2. `cmd` was serialized as a `Vec<u8>`, which prefixes a v64 length the
   decoder does not expect (W1). It is now written raw.
3. C++ `std::string` mapped to Rust `String`, whose decoder rejects
   non-UTF-8, so every binary snapshot would have been refused. It is now
   `WireBytes`.

*Validated the same day.* A ten-agent pass checked 143 claims in this section
against the tree: 48 problems were reported and 45 survived an adversarial
re-check. They are folded in below. Where a finding changed the design rather
than a citation, the text says so.

### Target architecture

```
                   raft (core crate) -- Rust source, no srpc dependency
          protocol, state, log, the payload types and their byte codec,
          apply, time/env/threads; calls a named RUNTIME SEAM for everything
          that touches a reactor, a socket, the logger or the clock
                 │                                          │
   C++ lane      ▼  rusty-cpp --crate, clang   Rust lane    ▼  rustc
   ───────────────────────────────────────    ─────────────────────────────────
   seam = server_seam_cpp.cc over the          seam = raft-rt, a Rust crate
   C++ srpc lane (libsrpc.a):                  over the Rust srpc crate:
     fibers, IntEvent, PollThread,               Fiber, IntEvent, PollThread,
     RaftCommo (commo.cc),                       RaftTransport (transport.rs),
     RaftServiceImpl (service.cc),               RaftRpcService (service.rs),
     RaftService/RaftProxy (rcc_rpc.h)           generated rpc.rs
                 │                                          │
                 └───── same C symbols to Mako (link level) ┘
                   server_exports.h + the RaftServer shim (server.h)

   HOST, C++ in BOTH lanes (server_host.cc; the permitted "low-level things"):
     the embedder edge (RaftWorker / ServerWorker), snapshot storage backends
     (SnapshotManager, rocksdb), YAML config and site lookup, raft_catch
     around Mako's callbacks, the one payload <-> janus::Command conversion
     at the apply edge (see M2).
```

**Three build configurations, not two.** Today's production is a hybrid:
the rustc-compiled core (`libraft.a`) running over the C++ kernels. The plan
keeps it until the Rust lane wins, so production never silently moves from
rustc code to transpiled code.

| `MAKO_RAFT_LANE` | core compiled by | runtime | role |
|---|---|---|---|
| `hybrid` (default until T5) | rustc (`libraft.a`) | C++ kernels + C++ srpc | today's production; baseline for L3 |
| `cpp` | rusty-cpp → clang | C++ seam + C++ srpc | the C++ lane |
| `rust` (default from T5) | rustc (`libraft_rt.a`) | raft-rt + Rust srpc | the Rust lane |

- **One build tree builds all lanes** and produces lane-suffixed binaries
  (`deptran_server_cpp`/`_rust`, `raft_bench_cpp`/`_rust`, ...). The option
  only chooses which one gets the unsuffixed name. That is what lets T3 and
  the paired trials have both binaries without a CI matrix.
- **Raft's sources leave `txlog_core_obj`.** Today Raft and Paxos share it
  (`CMakeLists.txt:1043-1053`, `:1215-1229`). Per-lane source selection
  goes into a Raft-only object library, so Paxos's compile does not change.
- **Mako and Paxos: what is shared, exactly.**
  - Mako reaches Raft through `server_exports.h`. Its symbols are identical
    in every lane (measured, below), but its C++ *types* are not.
  - Paxos shares these with Raft:
    - `TxLogServer`, including `LearnerAction = std::function<int(int,
      Command)>` (`scheduler.h:62`);
    - the `RaftSpecific` declarations in the same header (`scheduler.h`,
      which Paxos includes via `paxos/server.h:6`);
    - `Communicator`;
    - `rcc_rpc.rpc`;
    - Raft's own carriers, `log_storage.hpp` and `snapshot_manager.hpp`,
      which `paxos/server.h:8-9` includes and holds by `shared_ptr`
      (`:57`, `:76-78`).
  - None of these changes shape. Any step that edits one runs the Paxos
    suites.
- **Wire compatibility between lanes is a requirement.** Both lanes'
  messages come from `rcc_rpc.rpc`, and the ids are pinned in `rpc_ids.txt`.
  The payload encoding lives once, in the core. A mixed-lane cluster (T3)
  proves it.
- **Later, not now:** once rusty-cpp supports calls across two transpiled
  crates, `raft-rt` itself could be transpiled and `commo.cc`/`service.cc`
  would become the same source too. That is upstream transpiler work, and
  nothing here depends on it.

### What was measured before writing this (pinned rusty-cpp `1689f438`)

Probes on scratch copies of the crate; the tree was not changed. Each
number below was reproduced independently during validation.

1. **The whole crate cannot be transpiled while it depends on srpc.** The
   preflight refuses before emitting output: "local dependency
   src/srpc/Cargo.toml contains source-owned C++ contracts; cross-crate
   adapter calls are unsupported" (`transpiler/src/main.rs:669`). The core
   must therefore not name `srpc::` at all, and that is what forces the seam.
2. **A Cargo feature does not hide a module from the transpiler.** With
   `srpc` made optional and `transport`/`service`/`rpc` behind
   `#[cfg(feature = "rust_runtime")]`, the transpiler still walked every file
   in `src/` and tried to transpile srpc recursively. So the Rust-lane code
   must live in a **separate crate**.
3. **The production core transpiles; the lab harness transpiles HOLLOW.**
   - The 18 modules left after removing `rpc`, `service`, `transport` and
     the three `lab*` modules, plus `lib.rs` (19 files), produced **0
     errors**. There was one hand-attention slot:
     `// TODO: derive(Copy)`, `raft.server_h.cppm:5325`.
   - The lab harness first stopped at an unannotated `Vec::new()`
     (`lab_cases.rs:366`). Once that is annotated it reports 0 errors too.
     But the transpiler emitted **189 more hand-attention slots**: every
     `macro_rules!` assertion call lowers to a `// TODO: name!(...)`
     comment. `check_msg!` alone accounts for 102 of them.
   - So the transpiled `test_initial_election` is `// TODO: init2!(...)`
     ... `::raft::passed(); return 0;`. "0 errors" does not mean correct: a
     C++-lane lab built this way would print 25/25 while checking nothing.
4. **The link-level ABI is identical; the C++ types are not.**
   `#[no_mangle] pub extern "C" fn raft_server_new` lowers to
   `export extern "C" server_h::RaftServerBase* raft_server_new()`, and the
   kernel declarations lower to `extern "C" { ... }` blocks. So the symbols
   match `server_exports.h`. But that header declares
   `janus::RaftServerBase*`, and the facade types differ too
   (`rusty::RaftCheckedMutex*`). A TU must never both import the generated
   module and include `server_exports.h`; the shim TU includes only the
   header.

**NOT measured, and it is L2's whole job:** whether that C++ *compiles*,
links against the kernels, and passes a lab that actually asserts. The same
code ran in production as transpiled C++ until the `1de45affa` cutover
(09-21). Since then it has been written for rustc alone, so rustc-only
constructs are expected.

### Phases, in order

**H** housekeeping and the mako-dev merge · **R** reactor fix · **L** the
C++ lane from the core · **M** the payload types move into the core · **S**
the runtime seam made explicit · **T** the Rust lane's runtime · **D**
cleanup. Each step is its own commit and clears the standing rules in
"Verification that must pass at every stage" below. Phase L extends those
rules to every lane.

#### Phase H — housekeeping, then the mako-dev merge

- [ ] **H1. Commit the pending docs and data.**
  - What to commit: the conversion-log extension,
    `docs/performance/raft-latency-regression.md`,
    `docs/performance/raft-rust-538df5f8c/` (with the `.gitignore`
    exemption), `scripts/raft_perf/strace_poll_timeline.py`, the overview,
    and this revision.
  - `HANDOFF.txt` is session scaffolding: fold into the docs anything it has
    that they lack, then leave it uncommitted.
  - Fix CLAUDE.md's stale pin text: on this branch it still says
    `a1f8fef8` and cites `REQUIRED_RUSTY_CPP_COMMIT (:36)`. The gitlink is
    `1689f438`, and the constant lives in four places (L2).
- [ ] **H2. Merge mako-dev (`3e102604d`). This is real work, not
  housekeeping.**
  - `git merge-tree --write-tree HEAD 3e102604d` reports **22 conflicts**.
    The cause: both branches squashed the same srpc range
    `683c506ef..99f625d33`, separately (`9dd6ff492` here, `fa696ce29`
    there).
  - The substantive conflicts:
    - `src/deptran/raft/{frame.cc,service.cc,service.h}`;
    - a modify/delete on `raft/replicated_db.cc`;
    - rename/delete on `src/rusty-rustc` and `src/rusty-cpp-markers`.
      Keep the relocated copies: `raft/Cargo.toml:56` depends on
      `../../rusty-rustc`, which `3e102604d` deletes.
  - Mechanical conflicts:
    - `reactor.rs`, where only the `use std::sync::{...}` line conflicts;
    - `future.rs` and `server.rs`, where the 9dd6ff492 re-applications must
      be kept (BoxEvent `Mutex`+`AtomicBool`, the admission gate);
    - `src/srpc/build.rs` (add/add), `src/srpc-cmake/CMakeLists.txt`,
      `rcc_rpc.h`, `benchmark_control_rpc.*`, `ci.yml` and
      `check_srpc_crate_mode.py`.
  - *Done when:* a full build, the Raft gates, RaftLabTest 25/25, srpc's
    tests and the Paxos suites all pass on the merge. Then do R1 again,
    because the merge touches `reactor.rs`.

#### Phase R — the reactor timeout leak (both srpc lanes, so every Raft lane)

- **The defect.** `src/srpc/reactor/reactor.rs:1534-1536` and `:1553-1555`
  retain an event while `status() != DONE`. At `bbbd51d89` they also
  required `!= TIMEOUT`.
- **Why it leaks.** run_loop's `move_matching` (`:1528`, `:1547`) takes
  READY events only. `check_timeout` extracts TIMEOUT from
  `timeout_events_`, a different queue. So a timed-out event that is never
  re-waited stays in `waiting_events_`/`composite_events_` and is rescanned
  on every pass. Raft's per-round timed waits are exactly that case. A
  re-waited one is pushed again while its stale entry is still there.
- **Scope of the fix.** The file is canonical for both srpc lanes: the C++
  reactor is its transpilation (`src/srpc-cmake/CMakeLists.txt:547-557`).
  Nothing relies on a TIMEOUT event staying queued. It is also a local edit
  to the vendored subtree, carried until upstreamed.

- [ ] **R1. See it fail, in the right place.**
  - `build/` has never built the test, so running ctest there reports Not
    Run, which proves nothing. Build and run it in `build_raftlab/`:

        cmake --build build_raftlab --target test_srpc_timeout_race
        ctest --test-dir build_raftlab -R test_srpc_timeout_race --output-on-failure

  - The failure must be `TimeoutEventCleanup` (`test_timeout_race.cc:156-243`),
    specifically the size checks at `:225-226` and the TIMEOUT checks at
    `:234` and `:240`. A missing executable does not count.
  - Measured during validation: it fails exactly there, and the other five
    tests pass.
- [ ] **R2. The fix.** Restore the two-term predicate at both sites, with
  `7f52613fe`'s comment. It is written ONCE, and R4 carries the same bytes.
  *Done when:* all of these pass:
  - that test and srpc's `cargo test`;
  - the Raft gates and RaftLabTest 25/25 on an idle host;
  - the four production Raft suites;
  - because this changes Paxos's and Mako's reactor too: `simplePaxos`,
    `shard1Replication` and `shardNoReplication`.
- [ ] **R3. Measure; print the comparison first.**
  - **First, preserve the `sep21` arm.** It is the binary in
    `/home/users/zyang2/mako/build`, dated Sep 21
    (`raft-latency-regression.md:57`). mako-dev HEAD has moved on, so any
    rebuild there, including R4's own, destroys it. Copy the binaries and
    their libs to a named directory first.
  - (a) The bisect point: 4 KB, 240/s, round-robin over
    `sep21`/`head`/`head+R2`, at 8 s and 20 s windows. The effect is +19%
    p50 and +87% p99, and it grows with window length, so a few trials
    suffice to see it. Expected: `head+R2` ≈ `sep21`, and flat across
    windows.
  - (b) The rate phase of `run_sweep.sh` against `412c225a`, through
    `compare.py`. Data goes in `docs/performance/raft-rust-<commit>/`.
  - *Done when:* `compare.py` reports no latency regression, meaning no
    point outside its minimum-detectable-effect threshold, the rule that
    judged the original sweep. If (a) recovers and (b) does not, there is a
    second cause; find it before L.
- [ ] **R4. mako-dev.**
  - It has the same DONE-only predicate
    (`/home/users/zyang2/mako/src/srpc/reactor/reactor.rs:1524`, `:1543`).
    There it is upstream's latent leak, not a merge loss: `7f52613fe` is not
    an ancestor.
  - **Cherry-pick the R2 commit**, byte-identical, so the next re-sync is a
    no-op for these hunks. Bring the test's extra assertions with it.
  - mako-dev's top-level `CMakeLists.txt` has **no** `test_srpc_timeout_race`
    target. It exists only in srpc's standalone battery. Register it
    there, as this branch does at `CMakeLists.txt:1904-1913`, or the
    assertions never run.
  - Upstreaming to `stonysystems/srpc` is outward-facing: ask first. Until
    it lands, every subtree pull carries this local edit.

#### Phase L — the C++ lane, built from the core

The first step toward two lanes, and the one that decides whether the rest is
cheap: prove the core lowers to working C++. The runtime seam here is still
today's C++ kernels; only how the core is compiled changes.

- [ ] **L1. Split the crate.**
  - `src/deptran/raft/` keeps the core crate `raft`, with **no** `srpc`
    dependency. It keeps `crate-type = ["rlib", "staticlib"]`, because
    `libraft.a` is still the `hybrid` lane.
  - New crate `src/deptran/raft/rt/` (`raft-rt`):
    - It takes `transport.rs`, `service.rs`, `rpc.rs`, their tests
      (`transport_roundtrip.rs`, `service_is_a_service.rs`,
      `rpc_wire_golden.rs`, with `raft::` paths renamed) and the `srpc`
      dependency.
    - Its staticlib is `libraft_rt.a`, and it links the core as an rlib.
  - **Make it a workspace member.** The core's `Cargo.toml` declares its own
    `[workspace]` (`:23`), with `panic = "abort"` only in that root's
    profiles (`:62-71`). A package under it that is not a member fails
    (`current package believes it's in a workspace when it's not`), and a
    separate workspace would silently lose `panic = "abort"`. Use
    `members = [".", "rt"]`: one profile block, one `Cargo.lock`.
  - The one core site that names the transport, `Disconnect`
    (`server_h.rs:3895`), goes back to calling the seam
    (`raft_commo_set_network_enabled`).
  - CMake:
    - the rpcgen custom command (`:1142-1157`) writes `rt/src/rpc.rs`;
    - `RAFT_RUST_SOURCES` (`:1167-1172`) adds `rt/src/*.rs`;
    - a second cargo edge builds `libraft_rt.a` from `rt/Cargo.toml`.
  - *Done when:* `cargo test` and clippy pass in both crates with both
    feature sets, `hybrid` passes RaftLabTest 25/25 unchanged, and both
    artifacts are built with `panic = "abort"`.
- [ ] **L2. Transpile the core in crate mode, and make it compile.**
  - **The command.** Add the crate-mode command beside srpc's, modelled on
    `src/srpc-cmake/CMakeLists.txt:547-557`. The output directory is keyed on
    `RAFT_TEST`: pass `--features raft_test` exactly when cargo gets it
    (`CMakeLists.txt:1180-1183`), since the lab modules are whole-file
    `#![cfg(feature = "raft_test")]` (`lab.rs:22` etc.). Output is
    regenerated every build and nothing generated is checked in. **First,
    probe** that crate mode honours `#![cfg]`, `#[cfg]` and `cfg!`: the
    `f83537b7f` ban on `#[cfg]` exists because *inline* mode dropped them
    silently.
  - **The carrier twins must not be re-emitted.** *(Corrected by
    validation. An earlier draft said `--type-map`/`--cpp-module-index`
    could suppress them. They cannot: the type map only renames spellings,
    the module index is for `use cpp::` imports, and the one suppression path
    (`cpp_native_type`) accepts only impl-free `#[repr(C)]` structs,
    `transpiler/src/cpp_native_types.rs:141-232`.)*
    - The twins are every `source =` entry in `raft/rust-modules.toml`:
      `scheduler_h` (`TxLogServer`, `RaftSpecific`), `communicator_h`,
      `commo_h`, `frame_cc`, `raft_worker_cc`, `raft_main_helper_cc`,
      `server_pods_h`, and the `*_hpp` twins. Two of those twins are
      Paxos-shared: `log_storage_hpp` and `snapshot_manager_hpp`.
    - Choose one mechanism that exists, and probe it before committing to
      it:
      - (a) `--crate-namespace-wrap`, so `raft::TxLogServer` cannot collide
        with `janus::TxLogServer`. Unlike a bare `--cxx-namespace`, it also
        requalifies the core's own uses. This is safe because the C++ shim
        reaches the Rust object only through the C ABI.
      - (b) Move the twins into a third crate that the C++ lane does not
        transpile.
      - (c) Add module exclusion to rusty-cpp upstream.
  - **The facade carriers need a C++ spelling.** Rust-side, `RaftCommand`,
    `RaftByteString`, `LearnerAction`, `RaftIntEventPtr` and the rest are
    `#[repr(C)]` byte arrays whose `Drop`/`Clone` call `raft_destroy_*` and
    clone kernels (`src/rusty-rustc/src/lib.rs:630-790`). C++-side they are
    header aliases (`scheduler.h:80-87`, `server.h:326`,
    `rust_facade_types.h:53-64`).
    - Give Raft its own `module-preamble.toml`, including `scheduler.h`,
      `server.h`/`rust_facade_types.h` and the srpc reactor, plus a type
      map. Crate-mode `.cppm` files see header aliases only through the
      preamble.
    - The `Drop`/`Clone` kernels are **rustc-lane only**; in the C++ lane
      the real destructors and copy constructors run.
    - `rust_facade_types.h:22-27` claims `src/srpc/rust-type-map.toml`
      declares the Reactor pairs, but it does not. Fix that comment.
    - Probe one carrier end to end first.
  - **Make the lab assert.** Rewrite the lab's 14 assertion `macro_rules!`
    (`lab_cases.rs:39-117`, `lab_snapshot_cases.rs:84-92`) as functions or
    closures, which rustc and the transpiler both lower. Annotate
    `lab_cases.rs:366`. Until that lands, the C++-lane lab must not count.
  - **Reimplement the silent-loss gates; they cannot be reused.**
    `f83537b7f`, `54fed1ad5`, `fd49793dd` and `cfd311109` each edit only
    `scripts/raft_dsl.sh`, and they parse `#if RUSTYCPP_RUST` blocks and
    `RUSTYCPP:GEN-BEGIN/END` regions, which crate-mode `.cppm` files do not
    have. Run as they are, they would pass vacuously. The crate-mode gate
    scans whole generated files and `rusty_hand_slots.md` and **fails on any
    `// TODO`** or patcher marker. The `derive(Default)`/`#[cpp_inherit]`
    checks run over the canonical `.rs`. The `#[cfg]` ban is replaced by the
    cfg probe above.
  - **Fix the rustc-only constructs** the compile turns up, using the rule
    in `docs/porting-cpp-to-rust-dsl.md` §5: reshape the Rust when that is
    natural. Where the transpiler is wrong, fix it upstream and bump the
    pin.
    - The pin lives in four places: `scripts/extract_srpc_rust.py:49`,
      `src/srpc/scripts/extract_srpc_rust.py:36`,
      `src/srpc/scripts/check_srpc_crate_mode.py:27` and
      `scripts/raft_dsl.sh:36`.
    - A bump regenerates all srpc C++, so it also owes srpc's crate-mode
      gates and the Paxos/Mako suites.
  - *Done when:* every generated module compiles, and the crate-mode gate
    reports zero hand-attention slots.
- [ ] **L3. Link and run the C++ lane.**
  - The transpiled core becomes a named static library (`libraft_cpp.a`),
    so `nm` has something to read.
  - Every hard-wired `raft_rust` link site moves to one lane-selected
    interface target (`raft_lane`):
    - `txlog_core_obj` (`CMakeLists.txt:1225`, `:1229`);
    - `txlog_core` (`:1253`, `:1256`);
    - `TXLOG_LINK_LIBS`/`TXLOG_BUILD_DEPS` (`:1275`, `:1278`);
    - `mako` (`:1467`, Mako's own target: a build edit, not a code one);
    - `simpleTransaction` (`:1587-1588`);
    - `raft_lab_standalone` via `txlog_core_obj` (`:2057`).

    The header-only harness (`raft_node.hpp`/`test_cluster.hpp`) is
    lane-independent.
  - *Done when:* on `cpp`, RaftLabTest 25/25 passes with the asserting lab
    from L2, and so do the four production Raft suites. Also, an ABBA
    paired trial (≥25 pairs) of `cpp` against `hybrid` shows no regression
    in throughput, p50 or p99. Both lanes run the same C++ runtime, so the
    trial isolates transpiled code from rustc code. (`raft_field_census.py`
    is a lane-independent source scan, so it stays a standing rule and is
    not a lane criterion.)
- [ ] **L4. Keep the lanes honest.**
  - **Core parity.** The exported `raft_server_*` sets of `libraft.a` and
    `libraft_cpp.a` must be identical. Their `raft_`-prefixed *imports* must
    also be identical, except for an explicit, reviewed per-lane
    allow-delta (the rustc lane's `raft_destroy_*`/clone kernels). This
    compares cores, not whole lanes: after T1, `raft-rt` defines the seam
    inside the Rust lane's archive, so a whole-lane diff would diverge by
    design.
  - **Each lane's link defines every import exactly once** (`nm
    --defined-only` over that lane's inputs), which catches a seam defined
    in both C++ and Rust.
  - *Done when:* both checks run on every build, and one build tree
    produces all lanes' suffixed binaries.

Risks:
- rustc-only constructs in 2,700+ lines written since 09-21;
- the twin mechanism (L2's probe decides);
- `panic = "abort"` has no C++-lane equivalent, so decide what a panic
  path lowers to there;
- transpiled-code performance (L3's trial).

#### Phase M — the payload and message types move into the core

*(Corrected by validation. An earlier draft said Mako never hands Raft a
Command and apply returns bytes. Only the first half is true.)* Submit hands
Raft bytes: `RaftWorker::Submit` wraps them itself
(`raft_worker.cc:733-757`). But **apply delivers a `janus::Command`**. It goes
through `LearnerAction = std::function<int(int, Command)>`
(`scheduler.h:62`) to `raft_apply_invoke` (`server.cc:1137-1150`, which
filters `TpcNoopCommand` in C++), and from there to `RaftWorker::Next`
(`raft_worker.cc:391-393`, `:454`, which unwraps it at `:930-1003`), the
`ServerWorker` stub (`server_worker.cc:49-53`), and the lab learner, which
reads `tx_id` (`server.cc:624-634`).

- [ ] **M1. A Rust payload type and its codec, in the core.** It must be
  byte-identical to today's envelopes:
  - `Commit`, a `TpcCommitCommand`: `[tx_id][ret][term][inner envelope]`
    followed by `[bool has_view_data][opt ViewData]` (`tpc_command.cc:40-54`).
    - `term` is load-bearing: it is restamped per batch entry
      (`server.cc:1728-1739`) and read back (`:1845`).
    - `tx_id` is too: it is the lab oracle's identity.
    - The inner kind is `LogEntry` (`[i32 length][string]` over the
      20-byte `MAKORAFT` header plus payload, `application_log.cc:33-55`),
      or `LegacyVecPieceData`, which is still accepted on apply
      (`raft_worker.cc:987-1003`). **Decide and record here:** keep the
      legacy kind decode-only in the core, or retire it with a stated
      compatibility cut.
  - `Batch`: `[v32 kind][u32 count][Commit body]*count`, with the elements
    unenveloped (`tpc_command.cc:85-91`).
  - `Noop`: a bare top-level `TpcNoopCommand` (`server.cc:939-942`).
  - **The codec lives in the core, not in srpc.** `v32`, `Serialize` and
    `BinaryWriteArchive` are srpc types (`basetypes.rs:560`,
    `serializable.rs:278-301`), and the core must not name srpc.
    - So the core owns a SparseInt-compatible v32/v64 codec, pinned by
      golden tests against srpc's `dump32`/`dump64`.
    - `raft-rt`'s generated code writes the pre-encoded bytes raw, through a
      local newtype. A direct `impl srpc::Serialize for raft::Payload` would
      break the orphan rule.
    - The core's copy is the canonical codec for the payload.
  - *Done when:* golden tests pin each variant against frames the C++
    encoder produced. That includes a three-element batch with its
    boundaries, a `Commit` with a restamped `term`, and a legacy-inner frame
    if that kind is kept.
- [ ] **M2. The core holds the payload instead of the carrier.**
  - **Chosen shape, which leaves `scheduler.h` untouched.** `RaftSpecific`'s
    `Start`/`ServeAppendEntries`/`ServeInstallSnapshot` keep their Command
    signatures (`scheduler.h:160-184`). The C++ shim converts before
    calling new byte-shaped Raft-only exports (`raft_server_submit_bytes`,
    `raft_server_serve_append_entries_bytes`). The core sees only the
    payload.
  - On apply, the core calls one HOST conversion kernel that builds the
    `janus::Command` for `LearnerAction`, once per entry, at the apply edge.
    `LearnerAction` is shared with Paxos, and Paxos calls
    `reg_learner_action` at `paxos_worker.cc:557/566/575`, so its type stays.
  - The C++ lane's `rcc_rpc.h` AppendEntries still carries `Command cmd`
    (`rcc_rpc.rpc:37`). Its seam converts payload to Command **once per
    entry, cached beside the log entry**, not once per peer per send.
  - Deleted in both lanes:
    - `raft_command_*` and `raft_wire_*`;
    - the batch kernels (`raft_batch_len`, `_term_at`, `_command_into`,
      `_finalize`, but **not** `raft_batch_optimization_enabled`, which is S1
      config);
    - `raft_noop_command_into`, `raft_stamped_commit*`, `raft_apply_invoke`'s
      Command parameter, `raft_phase1_send_append`'s `cmd` parameter, and
      `raft_destroy_command`/`_tpc_commit_ptr` (`server.cc:1418`, `:1437`);
    - the lab's Command kernels (`raft_lab_make_commit_command`,
      `raft_lab_commit_tx_id`, `raft_lab_make_learner_action`,
      `server.cc:610-653`), since the lab then reads the payload directly.
  - `raft_ensure_legacy_payload_registered` is **kept**. It moves into the
    C++ conversion kernel, because the C++ lane still decodes Command frames
    through `SerializableRegistry`.
  - *Done when:* the Raft lab and suites pass on `hybrid` and `cpp`. Also, a
    paired trial of `hybrid` before against after shows no regression, which
    catches the conversion's cost in the commit that adds it.
- [ ] **M3. The generator types the payload.**
  - `rpcgen_rust.py` stops treating `cmd` as opaque bytes at offset 50
    (`:92-94`, `:116-117`). The offset arithmetic and `from_body` go.
  - The latent AppendEntries bug goes with them: `rpc.rs:96` writes a
    v64-length-prefixed `Vec<u8>` (`serializable.rs:529-538`), while
    `from_body` reads the bytes unframed.
  - *Done when:* `rpc_wire_golden.rs` pins an AppendEntries carrying each
    payload variant, byte-equal to the C++ encoder's output.

#### Phase S — the runtime seam, made explicit

- [ ] **S1. Classify every kernel** as SEAM (lane-specific), HOST (C++ in
  both lanes), or CORE (becomes plain Rust in the core, because it is C++
  only by position). `gen_correspondence.py` emits the table from the extern
  blocks, so it is complete by construction. It is wired into a build target
  with `--check`; today nothing runs it. Starting assignments:

  | class | kernels |
  |---|---|
  | SEAM | fiber spawns (heartbeat, both election-timer spawns); `raft_fiber_sleep_us`; IntEvent new/set/wait_timeout; `raft_queue_wake_job`; `raft_bind_replication_poll`; `raft_shutdown_barrier_yield`; the commo operations (bind/unbind, set_network_enabled, broadcast_vote + wait, vote snapshot, append send + `raft_append_response_read`, snapshot send); **logging** (`raft_log_enabled`/`raft_log_line`, `raft_log_set_is_leader_entry`) and **the clock** (`raft_time_now_us` is srpc `Time::now`), so that the Rust lane uses the Rust srpc logger and clock; `raft_lab_frame_rpc_count`, which reads `RaftCommo::rpc_count_` |
  | HOST | callbacks and apply, snapshot, the `raft_catch` guards (`*_guarded`), the M2 conversion kernel, the lab's snapshot and callback kernels, `raft_new_callback_lifetime` |
  | CORE | the locks (`raft_mutex_*`, `raft_std_mutex_*`, which become Rust mutexes); `raft_verify`; `raft_thread_sleep_ms`; `raft_monotonic_now_*`, random, env, config values; the apply thread (std::thread); the libc imports (`usleep`, `rand`, `setenv`, `tolower`). Each moves only if the transpiler lowers it and L4 stays green |
  | gone | the Command group (M2); `raft_command_from_bytes`/`raft_byte_string_from_bytes` (T2); the `raft_destroy_*`/clone kernels stay rustc-lane-only (L2) |

- [ ] **S1b. Split `server.cc` along that table.** Today SEAM and HOST
  kernels sit in the same `extern "C"` block (`server.cc:454-1449`).
  `server_host.cc` holds HOST and builds in every lane; `server_seam_cpp.cc`
  holds SEAM and builds only for `hybrid`/`cpp`. Without this split, the Rust
  lane's link defines the seam twice. L4's exactly-once check is what
  catches that.
- [ ] **S2. The core stops holding runtime handles.**
  - `waiter_`, `election_waiter_` and the wake gate's `owner_`
    (`server_h.rs:1268-1270`) are C++-lane objects stored in the core. On
    the Rust lane they cannot be stored there at all:
    - a Rust `IntEvent` is `!Sync` (its `Cell` fields) and `!Send` (the
      `rc::Weak<Fiber>` in its `EventState` and an unbounded
      `Weak<dyn EventPollable>`, `reactor.rs:226`, `:411-419`), and its
      `owner_thread_` enforces this at runtime;
    - `RaftServerBase` must stay `Send + Sync`, both for the service bound
      and for `Submit` calls from Mako's threads.
  - So each lane keeps a per-server **runtime object** on its poll thread,
    reached by server identity. That is the shape 3a gave `commo_of`
    (lock-free: one acquire load plus a hash find) and 3e gave
    `transport_of`.
  - The seam passes lane-neutral values (timeouts, outcomes, reply records),
    never handles.
  - Cross-thread wakes go through the lane's `PollThread::add`, which is
    `Send + Sync`: an mpsc `Sender`, a `Mutex<Option<JoinHandle>>` and
    atomics (`reactor.rs:2109-2118`). A cargo probe confirmed this.
- [ ] **S3. Replies are lane-neutral.** `PendingAppend.response_`
  (`server_cc.rs:18`) and the InstallSnapshot callback
  (`server.cc:1654-1700`) become one seam contract. The core asks for a send
  and later polls for a plain reply record (or "failed"). Today's rules
  stay:
  - no peer means completed and failed (`commo.cc:43-45`);
  - the snapshot no-peer path reports 0 inline, without taking a lock.

  The C++ seam implements the contract over `RaftCommo`, and `raft-rt`
  implements it over `RaftTransport`'s `Pending<T>`.

  *Done when (S as a whole):* L4 is green, RaftLabTest 25/25 passes on
  `hybrid` and `cpp`, and the L3 trial numbers hold.

#### Phase T — the Rust lane's runtime (`raft-rt`)

- [ ] **T1. The seam, in Rust.**
  - Implement every SEAM function over the Rust srpc crate: fibers
    (`Fiber::create_run`, `reactor.rs:928`), `IntEvent` (`:411`, `:431`),
    `PollThread::add` (`:2204`), sleep, the Rust srpc logger and clock, and
    S2's per-server runtime object.
  - **The vote's wake.** The tally is updated inside the client's reply
    callback, whose type is `Box<dyn FnMut(...) + Send>`
    (`src/srpc/rpc/client.rs:118`). A posted `Job` is `Send + Sync`
    (`base/misc.rs:77`). **Neither can capture an `IntEvent`.** Pick one of
    two shapes, and add a compile test for it:
    - the callback posts a job that looks up S2's runtime object on the
      poll thread and sets the event there; or
    - the campaigning fiber waits in a short sleep loop on
      `tally.decided()`, with a 1 s deadline.
  - **Decide, and record here, whether to match C++ on four differences:**
    1. Early loss. C++ loses only after more than `n - floor(n/2)`
       rejections (`reactor.rs:2298-2304` with `commo.cc:119`), which in
       practice means never for n=3 and all four peers for n=5. Rust loses
       at a majority of rejections: 2 of 3, 3 of 5.
    2. `n_voted_yes_` counts self in Rust. It is used only in logs.
    3. The max term is seeded with the candidate's own term in Rust, and
       with 0 in C++.
    4. Membership comes from `Config::GetPartitionSize` in C++
       (`commo.cc:118`) but from the recorded partitions in Rust
       (`transport.rs:317-322`).
- [ ] **T2. The transport's gaps, found on 09-26.**
  - Add an EmptyAppendEntries send path, chosen when there is no payload,
    as `commo.cc:69` does.
  - Reject malformed frames. The service now decodes through the core's
    codec, so `raft_command_from_bytes`/`raft_byte_string_from_bytes` go,
    and with them their `bool`-vs-`()` ABI mismatch (`server.cc:553` vs
    `service.rs:35`).
  - `serve()` closes admission before `start`, as `raft_worker.cc:351` does.
    The Rust server starts open (`src/srpc/rpc/server.rs:869`). Also add
    `set_admission_ready` and `bound_port`.
  - `add_peer` retries like `ConnectToAddress` (`communicator.cc:90-117`).
    Decide what replaces `ReconnectToSite` (`:131-173`).
- [ ] **T3. Tests for what will run.**
  - Over loopback TCP: every RPC, `serve()` with the real `RaftRpcService`,
    and the ABI plus registry lifecycle.
  - **The mixed-lane cluster, and how it runs.** RaftLabTest cannot host
    it: it is one process with five in-process sites (`s_main.cc:52-92`,
    `raft/frame.cc:287`). Both lanes also export the same unmangled C
    symbols, so one process cannot link two lanes.
    - Instead, add a variant of `examples/test_1shard_replication_simple_raft.sh`
      that takes a binary per replica. The existing script launches one
      binary for all three (`:22`, `:119-124`).
    - Use L4's suffixed binaries, and run both mixes: one `cpp` with two
      `rust`, and two `cpp` with one `rust`. That way each lane is tested
      as leader and as follower.
    - Pass criteria: an election and replication in the logs, and the
      client's `ALL VERIFICATIONS PASSED`.
- [ ] **T4. Rust-lane wiring, in per-lane files that CMake selects.** Two
  embedders need it, and both are Raft-only:
  - **`RaftWorker`** (production):
    - `SetupService`/`SetupCommo` become transport `new`, then `serve`,
      then one `add_peer` per site.
    - The `EnsureSetup` jobs (`raft_main_helper.cc:471`, `:540`) are posted
      to the Rust poll thread.
    - The `kSingleGroup` stubs (`raft_main_helper.cc:370-394`, old 4b)
      use a serve-only variant, so the thread count does not grow.
  - **`ServerWorker`**, which is what RaftLabTest actually runs (`s_main.cc:9`,
    `:76`, `:92`). It is a Raft-only harness (`server_worker.h:14-15`), so
    Paxos is not involved.
    - Its C++ `SetupService` (`server_worker.cc:56-94`) must not bind a C++
      `srpc::Server` on the Rust lane.
    - `SetupCommo` (`:110-131`) posts `EnsureSetup` to the Rust poll
      thread.
    - The lab fiber created in `CreateCommo` (`raft/frame.cc:279-354`)
      moves there too.
  - **Shutdown, in full order** (`raft_worker.cc:521-599`):
    1. `StopSubmitThread`.
    2. Close admission, then drain.
    3. `PrepareForShutdown`.
    4. Drop the RPC server, and the heartbeat server if it stays.
    5. `ReleaseScheduler`, then delete `rep_sched_`.
    6. Stop the poll threads.

    `RaftTransport` owns both its `Server` and an `Arc<PollThread>`
    (`transport.rs:108-118`), and the `Server` holds a clone of that poll
    Arc. So `take()` the server explicitly before the last poll handle is
    dropped. Check in `rpc/server.rs` what it retains; do not assume.
  - Assert that nothing is left on `svr_poll_thread_worker_`.
  - *Done when:* on `rust`, RaftLabTest 25/25, the four production Raft
    suites and T3 all pass. In addition, an ABBA paired trial (≥25 pairs) of
    `cpp` against `rust`, plus the sweep's rate phase against R3, shows no
    regression in throughput, p50 or p99. All of these are required.
- [ ] **T5.** Make `rust` the default. `cpp` and `hybrid` keep being built
  and tested; retiring `hybrid` is a separate decision, taken on T4's
  numbers.

#### Phase D — cleanup (the old stages 4 and 5, restated for two lanes)

- [ ] **D1.** Delete what the two-lane design orphans: the offset-50 opaque
  path, the `transport_of` fallbacks, `transport_exports.h` (replaced by the
  seam), and the Rust lane's last dependence on C++-lane kernels. The C++
  lane's `commo.cc`, `service.cc`, `server_seam_cpp.cc` and `rcc_rpc.h`'s
  `RaftService` **stay**: together they are the C++ lane's runtime.
- [ ] **D2. Old 5a is withdrawn; old 5b becomes permanent.** `RaftService`
  stays in `rcc_rpc.rpc`, because both generators read it. The id table
  plus T3's mixed-lane cluster keep the two lanes' wire identical.
- [ ] **D3. (old 4a, rewritten)** After M2 and T4, count the exports again
  and collapse the ones reached only from per-lane code.
  - The three `TxLogServer` exports stay, because Paxos implements that
    interface.
  - `EnsureSetup`/`WaitForStartup` are `RaftSpecific` (`scheduler.h:218-223`)
    and called only from Raft code (`raft_worker.cc`, `raft_main_helper.cc`
    and the Raft-only `ServerWorker`, `server_worker.cc:123`, `:131`). They
    are Raft-only exports, not shared ones.
- [ ] **D4.** Regenerate `cpp-rust-correspondence.md` with a per-lane view.

## TODO

Ordered. Each item names the files, the change, and how you know it is done.
Nothing after step 2 should begin until step 2's number is known.

### Prerequisite — finish the srpc merge

- [x] **P1. Gate passes.** (was exit 118, then 190, 151, load_balancer symbols) `scripts/check_srpc_crate_mode.py`: the oracle's
  clock stubs return `monotonic_now_us`, which upstream's body no longer
  sets, so `current_time_us()` reads 0. Give it a non-zero base that
  advances per call.
  *Done when:* the gate prints `checked whole srpc crate` and exits 0.
- [x] **P2. Full build.** `build_raftlab/deptran_server` builds. (It was 41,573,032 bytes at P2 and is larger at every commit since, so the number is not a check.) `cmake --build build_raftlab -j32` with
  `LIBRARY_PATH`/`LD_LIBRARY_PATH` set to the mako-deps lib dir.
  *Done when:* `build_raftlab/deptran_server` exists.
  *Expect:* more `Cell`→`SharedCell` and `std::string`→`rusty::String`
  fallout in `src/deptran` and `src/mako`; no full build has reached those
  files yet.
- [x] **P3. RaftLabTest on the merged tree.** 25/25, exit 0. The merged
  srpc works with Raft; the earlier 25/25 was built pre-pull and proved
  nothing about it.
  *Done when:* `ALL TESTS PASSED`, exit 0, 25 `Passed` markers.
  *This is the first evidence that the new srpc works with Raft at all* —
  the 25/25 on record was built 16 Sep against the pre-pull srpc.
- [x] **P4. Committed** on `srpc-subtree-forward` (`66776cfed` was the tip when this plan was written; 30 commits have landed since). Nothing is pushed. Split at least: gate
  reconciliation (whoever next pulls srpc on mako-dev needs it), the
  `cpp_value_init` retirement, the service-constness wave, the
  `rusty-rustc` move.

### Stage 0 — unwire what is already solved (no design needed)

- [x] **0a.** DONE. Deleted `#[no_mangle]` on `fiber_task_entry_thunk`,
  `src/srpc/reactor/reactor.rs:3690`.
  Verified it is reached only as a function POINTER passed to
  `srpc_fiber_init`, and no C or C++ file names the symbol, so the export
  bought nothing. Now 0 in the rustc lane, 1 in the C++ lane; was 1 and 1.
- [x] **0b, 0c. DONE, and they were a correctness defect rather than the
  tidy-up this listed.** The nine kernels in
  `src/srpc/scripts/native-kernel-sources.txt` were compiled TWICE and by
  DIFFERENT compilers, and both copies reached the binary: `build.rs` with
  `$CC` (falling back to `cc`, which is gcc-15 here) into a
  `libsrpc_native.a` bundled through the rlib into `libraft.a`, and
  `src/srpc-cmake` with `CMAKE_C_COMPILER` (clang) into `libsrpc.a`.

  Measured before touching it: 10 of the 12 external symbols in just
  `srpc_rand.o` and `srpc_timing.o` were defined in both archives. That
  matters because `srpc_rand.c:17` branches on `__clang__`, so the binary
  carried two DIFFERENT implementations of `srpc_rand_raw` -- clang's, a
  `pthread_key_t` seed malloc'd per thread and freed by the key destructor
  at thread exit; gcc's, a `_Thread_local` seed with no teardown -- and
  archive order decided which shipped. (`srpc_timing.c:32`'s `__clang__`
  arm is unreachable on x86_64, so that one was harmless.)

  0c passes `CC` and `AR` through to cargo, so the two copies are the same
  code. 0b emits `static:-bundle=` from `build.rs`, so cargo stops copying
  the objects into the rlib: `libraft.a` now holds 0 of the 9, down from
  9. The final link still resolves them, and srpc's own 281 tests pass in
  the gate, which is where that would break first.

  This is the second edit to the vendored subtree on this branch, after
  0a's `#[no_mangle]` removal, and like that one it belongs upstream
  rather than here.
- [x] **0d. DONE.** `src/srpc/{base,misc,reactor,rpc,src}/*.rs` are in the
  libraft rebuild glob (`CMakeLists.txt`, `RAFT_RUST_SOURCES`). This
  stopped being cosmetic when the Raft crate gained a dependency on the
  srpc crate: without it, editing a canonical srpc `.rs` leaves
  `libraft.a` stale, because ninja sees no changed dependency, never
  re-invokes cargo, and cargo therefore never gets the chance to notice.

### Stage 1 — the decisive measurement (DONE; it gated everything after it)

1a and 1b below were written as the *experiment* that would produce the
measurement. 1c produced it by asking the compiler directly instead, which
was cheaper and gave a different answer, so neither was ever run. They are not
measurements and they are not stage 1: both are steps of the lane move, and
they are restated there (3e) rather than left here looking outstanding.

- [x] ~~**1a.** `server_h.rs`: change `waiter_` and `election_waiter_` from
  `rusty::RaftIntEventPtr` to `std::sync::Arc<srpc::reactor::IntEvent>`.~~
  Superseded -- moved to 3e, where it belongs.
- [x] ~~**1b.** `lib.rs`: `struct RaftServiceShim` with
  `impl srpc::server::Service`.~~ Superseded -- moved to 3e.
- [x] **1c. MEASURED, and the answer is a third outcome the plan did not
  anticipate.** Asking the compiler directly against the built rlib:

      assert_send::<raft::server_h::RaftServerBase>()
       -> E0277: `*mut rusty::Communicator` cannot be sent between threads

  `RaftServerBase` has **48 fields, of which exactly one** is non-Send:
  `commo_: *mut rusty::Communicator` (server_h.rs:1775). The other 47 are
  already fine.

  So the price is neither "0 unsafe impls" nor "unsafe impl for the whole
  server". It is one raw pointer -- and it is the field this plan already
  moves into Rust in stage 3, which turns the question from an assertion
  into something the compiler checks.

  Not asserted, deliberately: `Communicator` holds `peers_` and
  `partition_peers_` as unguarded `std::map`s at the time; only
  `network_enabled_` was atomic and the per-peer `request_mutex_` guards a
  peer's client, not the maps. They are populated at construction and read
  after, which is probably why one poll thread is safe -- and "probably" is
  exactly what an `unsafe impl Send` would convert into a guarantee.

### Stage 2 — the Raft RPC slice in Rust, GENERATED

Corrected after measurement. An earlier revision said to hand-write this
because adding `lang_rust.py` to `src/srpc/pylib/simplerpcgen/` would put
mako's code inside the vendored subtree and conflict on every pull. That
objection is still right, and it does not apply to the option it obscured:
an emitter that lives OUTSIDE the subtree and imports upstream's parser.

`bin/rpcgen` is already mako's own file doing exactly that --
`sys.path += src/srpc/pylib`, `from simplerpcgen import rpcgen`. And the
parser hands over everything an emitter needs, with nothing to modify:

    Vote  attr=fiber
   in : [('uint64_t','lst_log_idx'), ('ballot_t','lst_log_term'),
          ('siteid_t','site_id'), ('ballot_t','cur_term')]
   out: [('ballot_t','max_ballot'), ('bool_t','vote_granted')]

Field names, wire types, order. The hand-written `rpc.rs` from 2a is that
same content typed out by hand -- and typed wrongly at first, as `bool`
rather than the `bool_t`/`int8_t` the parser states plainly.

**What is mechanical, and therefore generated.** Mirror the split C++ already
uses, because it is the right one:

| C++ today | lines | Rust counterpart | authored how |
|---|---|---|---|
| `rcc_rpc.h` `class RaftService` | 381 | `trait RaftService` + `__dispatch__` | generated |
| `rcc_rpc.h` `class RaftProxy` | 221 | `struct RaftProxy`, one method per RPC | generated |
| `service.{h,cc}` `RaftServiceImpl` | 170 | `impl RaftService for RaftServiceImpl` | hand-written |

The generated half is 603 lines of C++ and is pure boilerplate: deserialize
the request, switch on the rpc id, call a handler, serialize the reply. The
hand-written half is the 4 call sites where `RaftServiceImpl` actually reaches
into the Raft server. Generating the first and hand-writing the second keeps
the same seam that works today, rather than inventing a new one.

- [x] **2a. DONE.** `scripts/rpcgen_rust.py`, wired into the build
  by the custom command beside `rcc_rpc_gen` in `CMakeLists.txt`, with the
  emitted `rpc.rs` as a dependency of the cargo edge so it is regenerated
  before the crate compiles. Golden wire vectors pin the bytes:
  `src/deptran/raft/tests/rpc_wire_golden.rs`, four tests, derived from
  the serialiser's rule rather than captured from a run.

  Original text, unchanged:

  **Write `scripts/rpcgen_rust.py`** -- mako-local, imports the
  subtree's parser the way `bin/rpcgen` does, emits Rust. Nothing under
  `src/srpc/` changes, so nothing conflicts on a pull.
  *Emits:* the wire structs with `Serialize`/`Deserialize` in declaration
  order, the `RaftService` trait with its `__dispatch__`, the `RaftProxy`
  with one method per RPC, and the four rpc-id constants.
  *Does not emit:* handler bodies. Those are stage 3's hand-written impl.
- [x] **2b. DONE.** `src/deptran/raft/rpc_ids.txt` is the source of truth and
  `rcc_rpc.h` became a check. Two checks, both failing the build rather
  than the wire, and both tested by deliberately breaking them: the header
  disagreeing with the table, and any OTHER service having drawn one of
  the reserved ids. The second is exactly the 5a hazard described below --
  once RaftService leaves the `.rpc`, rpcgen stops reserving these four and
  can redraw one -- caught before it ships instead of silently. After 5a
  the table is simply the only record, and the emitter says so rather than
  failing, because the header legitimately has no RaftService by then.

  Original text, unchanged:

  **Make the ids the generator's business, not a hand-pinned list.**
  `rpcgen.py:326-338` preserves ids ONLY by scraping them back out of the
  header it previously wrote; the `.rpc` file does not record them. So the
  moment `Raft` leaves `rcc_rpc.rpc`, the scrape stops finding
  `0x2802b911`, `0x3935326f`, `0x6e089268`, `0x5276442f`, `used_codes`
  stops knowing they are taken, and a later service added to that file can
  draw one of them at random.
  *Fix:* have the Rust emitter read the four ids and keep them in one
  checked-in place that BOTH generators consult, so the wire contract
  survives the split.
- [x] **2c. DONE, and it needs no wire change at all.** The framing below was
  the plan's biggest risk -- a wire break with no mixed-version path -- and
  it turned out to be unnecessary.

  `cmd` sits at a FIXED byte offset 50, because every field before it is
  fixed-width, and exactly one fixed-width field follows it
  (`leaderNextLogTerm`, 8 bytes). Rust also holds the whole frame as
  `Request::body`. So the payload's extent is arithmetic:

      cmd               = body[50 .. len - 8]
      leaderNextLogTerm = body[len - 8 ..]

  Rust copies that range verbatim and hands it back to C++ untouched. C++
  keeps writing and reading the envelope exactly as today, so the wire is
  byte-identical. Raft reading INTO the payload -- the fact that refuted
  plan v1 -- stops mattering at this boundary, because C++ still does the
  reading.

  A struct with an opaque field gets `from_body(&[u8])` instead of a
  streaming `Deserialize`, since the end is only knowable from the whole
  frame. The emitter still refuses when the extent genuinely is not
  arithmetic (two opaque fields, or a variable-width field beside one).

- [x] ~~2c-old. Decide the AppendEntries payload framing before generating it.~~
  The generator can emit Vote, EmptyAppendEntries and InstallSnapshot from
  the parser alone; `AppendEntries` it cannot, because `Command cmd` is a
  `janus::Command` whose contents Raft reads and which carries no length
  prefix. Repeated `(u32 len, bytes, i64 term)` -- not three scalars beside
  one blob. Until this is settled the emitter should refuse that RPC
  loudly rather than emit a field list that silently misparses.
  *Done when:* a `cargo test` round-trip decodes a three-element batch
  captured from the C++ encoder, element boundaries included.
- [x] **2d. DONE.** `src/deptran/raft/src/rpc.rs` is a build artifact, not a
  snapshot: the custom command in `CMakeLists.txt` regenerates it from
  `rcc_rpc.rpc` and `rcc_rpc.h` and ninja orders it before cargo. It stays
  `kind = "canonical"` in `rust-modules.toml`, as this item said it should
  -- rustc compiles it directly either way; what changed is who writes it.
  The emitted file was byte-identical to the hand-checked one, which is
  the evidence that the emitter reproduces it rather than replaces it.

### Stage 3 — commo, service and the poll thread cross together

- [x] **3a. DONE, and the blocker it existed to remove is gone -- but by
  deletion, not by a move.** The plan below said to make `commo_` a Rust
  type. The better answer, found while doing it, is that
  `RaftServerBase` should not hold a communicator at all.

  `commo_` is removed from the struct. C++ keeps a
  `server -> RaftCommo*` table (`server.cc`, `commo_of`) and the five
  kernels resolve the communicator by server identity. The shim binds on
  `TxLogServer::set_commo` and `raft_server_delete` unbinds.

  Three things this buys, all measured rather than argued:

  1. **`RaftServerBase` is now `Send + Sync`, and the compiler says so.**
     `src/deptran/raft/tests/server_is_send.rs` asserts it; before this
     change the same assertion was `E0277`. Nothing was waved through
     with an `unsafe impl`.
  2. **It should be cheaper than what it replaces -- STRUCTURALLY
     argued, NOT TIMED.** No benchmark backs this; see the evidence map
     at the end of this TODO. The old `commo_of`
     (then at `server.cc:373-377`, now `:446`) ran a `dynamic_cast` on EVERY call -- an RTTI
     walk per AppendEntries send. The cast now runs once per server, at
     bind time; sends do a flat hash lookup.
  3. **It is honest.** Asserting `unsafe impl Send` over the old field
     would have been false: `janus::Communicator` then held `peers_` and
     `partition_peers_` as unguarded `std::map`s. (Stage 3d has since
     replaced all five of its members with one `PeerRegistry registry_`, so
     the hazard is gone — but it was real when the field was removed, which
     is why it was removed.)

  **What did NOT move, and the measurement that says it cannot yet.** The
  plan wanted a Rust `RaftCommo` owning the peers and their
  `srpc::Client`s. A first pass wrote one (`src/deptran/raft/src/commo.rs`)
  and it was **never wired into anything**; it has since been deleted,
  because 3d superseded its stated purpose and a second unused peer table
  in the tree was worse than no second peer table. The reason it could not
  be wired up stands, and is what 3e is for:

  - The live connections belong to the **C++ lane** (`libsrpc.a`,
    transpiled). A Rust-lane `srpc::client::Client` cannot adopt them: the
    two lanes do not share a layout (`srpc::CircuitBreaker` is 344 bytes
    under clang and 96 under rustc), so a Rust `Client` would have to open
    its own sockets and be polled by a Rust `PollThread`.
  - **That is NOT a second reactor, and an earlier revision of this item
    was wrong to say it was.** Measured: Raft already owns a dedicated
    poll thread. `RaftWorker::SetupService` creates
    `svr_poll_thread_worker_` (`raft_worker.cc:344`) and hands it to both
    `rpc_server_` (`:350`) and `rep_commo_` (`:379`); the only service
    registered on that server is `RaftServiceImpl`, because
    `RaftFrame::CreateRpcServices` pushes exactly one proxy
    (`frame.cc:373-378`); and the only other consumer is a one-shot
    `EnsureSetup` job (`raft_main_helper.cc:471`, `:540`). Nothing of
    Mako's or Paxos's runs on it.
  - So a lane move **replaces** Raft's reactor rather than adding one, and
    the performance question becomes a like-for-like comparison of two
    implementations of the same thread rather than a question about thread
    count. That is a much smaller decision than this item first recorded,
    and it is what makes stage 4 reachable at all.
  - What it was gated on was **3d**, which has since landed: the peer
    table is Rust now. What remains for 3e is narrower than this bullet
    first said -- not the table, only the CLIENTS. `ConnectToAddress`
    still builds C++-lane `rusty::Arc<srpc::Client>`s, and it is a
    private member of `janus::Communicator`, the base shared with Paxos.
    The stub-server path in `raft_main_helper.cc` (kSingleGroup, the build
    default) stands up N more servers with the same one service and moves
    with it, which is stage 4b.

  The field removal is what THIS item delivers. The peer table itself
  moved in 3d, which landed later and by a different route than this
  bullet expected. The `commo.rs` this bullet was written around is gone:
  it was never that table, never became one, and once 3d landed it was
  simply an unused second answer to a solved question.

- [x] **3b. MEASURED: no redesign needed. The wake already satisfies this.**
  Traced end to end:

  1. `PublishReplicationWork` (any thread) takes `owner_` under a mutex and
     clones the `Arc<PollThread>` -- an atomic refcount bump, nothing more.
  2. `raft_queue_wake_job` calls `PollThread::add`, which is
     `self.sender_.send(PollCommand::AddJob { job })`
     (`src/srpc/reactor/reactor.rs:2204-2208`) -- an mpsc channel send, so
     the cross-thread hop carries only an `Arc<dyn Job>`.
  3. `pollworker_process_commands` (`:3416`) drains that channel **on the
     poll thread** and `job_spawn_work` runs the job in a fiber there.
  4. Only then does `GateWakeJob::run -> wake_on_owner -> IntEvent::set`
     happen -- on the owner thread, which is the whole point of routing
     through `add` instead of setting the event directly.

  So the `!Send` handle (the `IntEvent`) is never *used* off its thread;
  what crosses is an Arc and a channel message. The `Future` rewrite this
  item proposed would be a second way to do what the job queue already
  does correctly.
- [x] **3c. DONE for the gate.** The drain stays; the reason is below.

  **The gate: done.** `service.cc` used to run it in C++ at a cost of
  three virtual-plus-FFI round trips per inbound RPC -- `IsDisconnected`,
  `IsRpcReady`, then the handler. `RaftSpecific` now carries `ServeVote`,
  `ServeAppendEntries` and `ServeInstallSnapshot`, which apply the gate and
  write the unavailable reply themselves, so the cost is one crossing.
  `IsDisconnected` and the three `On*` entries left the interface with it
  (four exports, four shim forwarders and four virtuals deleted, three of
  each added), and `service.cc` is now a null check plus one call per RPC.
  Its dead DSL predicate, the four `static_assert`s and
  `src/deptran/raft/src/service_cc.rs` went too.

  AppendEntries is the hottest inbound path in the system, so this
  *should* be a throughput change rather than tidiness -- but that is an
  argument from call counts, NOT a measurement. Nothing here has been
  timed; see the evidence map at the end of this TODO.

  **The drain stays.** `RaftWorker::ShutDown` calls
  `rpc_server_->set_admission_ready(false)` and `rpc_server_->drain(...)`
  on a **C++-lane** `srpc::Server`. It moves when the service moves lanes,
  which is the same reactor question as 3a -- not before.

- [x] **3d. DONE. `Communicator`'s data is Rust now, written once and
  compiled twice.** This item was opened against a now-deleted file, as
  "commo.rs flattened an
  inheritance hierarchy and the flattening is not faithful". Both halves
  of that are addressed, and by a route the item did not anticipate.

  **What shipped.** `src/deptran/communicator.h` carries a
  `#if RUSTYCPP_RUST` block defining `PartitionSites`, `PeerEntry` and
  `PeerRegistry`. rusty-cpp translates it into the C++ that BOTH engines
  link, and the same block is extracted to
  `src/deptran/raft/src/communicator_h.rs` for rustc. One definition, two
  lanes, nothing duplicated -- the mechanism `scheduler.h` already uses
  for `TxLogServer`.

  **How the inheritance is answered.** Not by emulating it. `Communicator`
  keeps its name, its base-class role and its entire public surface, so
  `MultiPaxosCommo` and `RaftCommo` are untouched and Paxos is undisturbed.
  What changed is that its FIVE data members -- `rpc_poll_`,
  `owns_poll_thread_`, `peers_`, `partition_peers_`, `network_enabled_` --
  became ONE `PeerRegistry registry_` held by composition. That is the
  same move Tranche 6 made on `TxLogServer`: the interface stays C++, the
  state goes somewhere Rust owns it.

  **The defect that opened this item is fixed, not papered over.** The C++
  stored each peer twice, once in `peers_` and again inside
  `partition_peers_[par]`, and the `belongs_to_partition` scan existed to
  check the two agreed. Now a partition owns site ids and the peer table
  owns peers, so there is one place a peer can be and
  `peers_for_partition(par_id)` is genuinely partition-aware -- which
  the deleted `commo.rs`'s `peers_except(self_site_id)` never was.

  **Four things measured on the way, worth knowing before the next one:**

  1. **The transpiler handles a stateful struct.** Every other transpiled
     DSL entity in deptran is a scalar `const fn`, a POD, an enum or a
     trait; this is the first with containers and interior mutability.
     `rusty::Vec`, `rusty::sync::atomic::AtomicBool`,
     `rusty::Option<rusty::Arc<T>>`, `push`, `clone`, returning a `Vec` or
     an `Option` by value, nested struct literals and field-init shorthand
     all lower correctly. Probe first, in a scratch file, if the next type
     needs something not on that list.
  2. **`rusty::Vec` is a C++20 MODULE, not a header.** `<rusty/vec.hpp>`
     is empty and says so. A header declaring a `rusty::Vec` member needs
     `import rusty;` at global scope, before `namespace janus` -- inside
     it, the import names `janus::rusty` and shadows `::rusty` for the
     whole file. `src/deptran/raft/server.h:30` already does exactly this.
  3. **A field and a method may not share a name.** The emitter renames
     the field (`network_enabled` became `network_enabled_field`) without
     telling anyone. Hence `net_enabled`.
  4. **The gate runs clippy on the EXTRACTED crate**, so the DSL source
     has to be clippy-clean Rust, not merely valid Rust. Three lints bit:
     `redundant_field_names` (write `par_id`, not `par_id: par_id`),
     `question_mark` (`if x.is_none() { return None }` -- invert it), and
     `unnecessary_unwrap` (`is_some()` then `unwrap()` -- clone the Option
     instead, which is the handle copy in both lanes anyway).

  **What stayed C++, and why it is a kernel rather than a shortfall.**
  `ConnectToAddress` (srpc `Client::create`/`connect`/`close`, chrono,
  sleep), `RpcPeer::WithClient` (a template returning `decltype(auto)`),
  `RpcPeer::ReplaceClient` and `Close`. These are the surgery the DSL
  genuinely cannot express; the shape around them is Rust's.

  **What this does NOT do.** The peers it holds are still C++-lane
  `std::shared_ptr<RpcPeer>`, carried as opaque bytes
  (`rusty::CommoPeerPtr`) and never followed. Rust owning the *clients*
  is the lane move, 3e -- which starts from `PeerRegistry`'s shape rather
  than from a sketch, since the sketch (`commo.rs`) has been deleted.

- [ ] **3e. HALF BUILT.** The service side is done and proven; the client
  side is what remains.

  **Done:** `src/deptran/raft/src/service.rs` --
  `impl srpc::server::Service for RaftRpcService`, four handler bodies
  over `ServeVote`/`ServeAppendEntries`/`ServeInstallSnapshot`, with
  `register()` and `dispatch()` generated. It compiles against the real
  srpc crate and `tests/service_is_a_service.rs` pins the `Send + Sync`
  bound that stage 3a made satisfiable. Two kernels landed with it,
  `raft_command_from_bytes` and `raft_byte_string_from_bytes`, which are
  the price of 2c's decision that Rust carries the payload without
  interpreting it.

  **Not done:** nothing registers the service; `rpc_server_` is still the
  C++ one (`raft_worker.cc:350`, `:359`) and `transport_exports.h` is
  included by no `.cc`. The remaining work is the FIBER RUNTIME, not the
  send paths -- those are already Rust (`transport.rs`) and proven over TCP
  by `tests/transport_roundtrip.rs`. See the three coupled sites below.

  The rest of this item is what that cut involves. This is what
  stages 1a and 1b were really about, restated where it belongs. It is the
  last structural step before stage 4 and the first that can change
  performance.

  **Why it is reachable at all.** The measurement in 3a: Raft already owns
  a dedicated poll thread. `RaftWorker::SetupService` creates
  `svr_poll_thread_worker_` (`raft_worker.cc:344`) and gives it to both
  `rpc_server_` (`:350`) and `rep_commo_` (`:379`); the only service on
  that server is `RaftServiceImpl`, because `RaftFrame::CreateRpcServices`
  pushes exactly one proxy (`frame.cc:373-378`); and its only other
  consumer is a one-shot `EnsureSetup` job (`raft_main_helper.cc:471`,
  `:540`). Nothing of Mako's or Paxos's runs on it. So this SWAPS a
  reactor rather than adding one.

  **Why it must be one change, measured rather than assumed.** A
  half-move gives Raft TWO poll threads -- a Rust one serving inbound
  while the C++ one still sends outbound -- and that is genuinely adding
  a thread, which is the performance change the verification rules say
  must be benchmarked. There is no smaller cut that avoids it, because
  the inbound and outbound paths share `svr_poll_thread_worker_`
  (`raft_worker.cc:344`, `:350`, `:379`).

  **All five commo operations can move.** The table under 3a-old claimed
  two could not; re-measured, both entries were wrong -- see the
  correction there. `SendInstallSnapshot` takes bytes, not the snapshot
  manager, and `BroadcastVote`'s quorum event never escapes its three
  kernel calls, so a Rust broadcast can produce the six-scalar
  `RaftVoteOutcome` Raft already consumes.

  **The Rust client is thread-bound, and the design has to accept that
  rather than work around it.** Measured: `srpc::client::Client` holds one
  `RefCell` and EIGHT `Cell` fields (`rpc/client.rs:1550-1566`), so it is `Send` but `!Sync`, which makes
  `Arc<Client>` neither. And `Client::new` is private (`:1575`) -- only
  `Client::create` is public and it returns `Arc<Client>` -- so a
  `Mutex<Client>` cannot be built either.

  The consequence is not a blocker but a constraint: the transport that
  owns the clients is `!Send`, C++ holds it as an opaque pointer, and
  every send happens on the poll thread. That is already true today --
  the heartbeat fiber runs there -- so it changes nothing about the
  runtime; it only rules out a transport type that could be handed
  between threads. `Disconnect` is the one caller from elsewhere and it
  touches only an atomic.

  (This also settles, retroactively, that the deleted `commo.rs` could
  never have worked: its `client: Mutex<srpc::client::Client>` field was
  nameable but uninstantiable, because nothing can produce a `Client` by
  value. It compiled only because no caller ever tried.)

  **What moves, in one change, because a half-move leaves handles
  straddling lanes:**

  - `rpc_server_` becomes a Rust `srpc::Server`, and `RaftServiceImpl`
    becomes an `impl srpc::server::Service` over `RaftServerBase` --
    possible now that `RaftServerBase` is `Send + Sync` (3a) and the
    dispatch half is already generated (`rpc.rs`, `trait RaftHandler` and
    `dispatch`, 2a/2d). This absorbs **1b**.
  - `waiter_` and `election_waiter_` stop being opaque carriers and become
    real `std::sync::Arc<srpc::reactor::IntEvent>`. This absorbs **1a**.
    No redesign of the wake is needed with them (3b).
  - The commo's clients move with the server. 3d put the peer TABLE in
    Rust but deliberately left the clients alone: they are carried as
    opaque `rusty::CommoPeerPtr` and never followed, because
    `ConnectToAddress` still builds C++-lane `rusty::Arc<srpc::Client>`s.
    Replacing those is this step's real work, and it is why 3d could land
    without touching Paxos while this cannot.
  - `RaftWorker::ShutDown`'s `set_admission_ready(false)` + `drain()` moves
    with the server it acts on (the deferral recorded in 3c).
  - The `kSingleGroup` stub servers in `raft_main_helper.cc` stand up N
    more servers with that same one service, so they move too -- that is
    **4b**, and it is part of this change rather than after it.

  **THE CUTOVER, SITE BY SITE.** Everything above is built and green; this
  is the remaining change, and it is ONE change. Measured 2026-09-25.

  *Rust — the sends stop being kernels and call the transport.*

  Each entry is **site**, then what is there today, then what replaces it.

  - `server_cc.rs:18`
    - now: `PendingAppend.response_: rusty::RaftResponsePtr`
    - then: `Pending<AppendEntriesResponse>`
  - `server_cc.rs:490`, `:1539`
    - now: the `raft_append_response_read` kernel
    - then: gone; read the Rust `Pending`
  - `server_cc.rs:1224-1237`
    - now: a `sent_response` C++ carrier
    - then: the `Pending` the transport returns
  - `server_cc.rs:1164`
    - now: `raft_phase1_load_and_send_snapshot`
    - then: `transport.send_install_snapshot`
  - `server_cc.rs:1226`
    - now: `raft_phase1_send_append`
    - then: `transport.send_append_entries`
  - `server_h.rs:2635`
    - now: `raft_bind_replication_poll`
    - then: `transport.poll_thread()`
  - `server_h.rs:3895` --- DONE already, as the worked example of the
    fallback pattern
    - now: `match transport_of(this) { Some(t) => .., None => kernel }`
    - then: unchanged; the kernel arm dies with the last caller
  - `server_h.rs:4149` (was cited as `:4141`, which is now the comment above
    the call; re-measured at `087df006d`)
    - now: `raft_broadcast_vote_and_wait` + `raft_vote_quorum_snapshot`
    - then: `transport.broadcast_vote`, then `tally.outcome()`

  The server cannot HOLD the transport: `RaftTransport` is `!Send`, and
  `RaftServerBase` must stay `Send + Sync` or the service loses its trait
  bound. So Rust resolves it by server identity, the same shape 3a gave
  the C++ side -- a registry keyed by `*const RaftServerBase`.

  *(Revision 5: the deletions in this paragraph now apply to the Rust lane
  ONLY, through T4's per-lane files. The C++ lane keeps `server_seam_cpp.cc`,
  `commo.cc`, `service.cc` and `frame.cc`'s Raft arms as its runtime.)*

  *C++ — the five kernels go, and the worker builds a transport instead:*

  `server.cc`: delete the five. `raft_worker.cc`: `SetupService`
  (`:343-360`) and `SetupCommo` (`:378-384`) become
  `raft_transport_new` + `serve` + `add_peer` per site; `ShutDown`
  (`:521+`) calls `raft_transport_drain` instead of the C++ server's.
  `frame.cc`: `CreateRpcServices` and `CreateCommo` lose their Raft arms.
  `raft_main_helper.cc`: the `kSingleGroup` stub servers (`:370-395`) and
  the two `GetPollThreadWorker` users (`:471`, `:540`) -- that is 4b.

  **The CODE can land incrementally; the SWITCH cannot.** Found while
  doing it, and it is better than this item first said. Each site can
  prefer the transport and fall back to its kernel:

      match crate::transport::transport_of(this) {
          Some(t) => t.set_network_enabled(!disconnect),
          None    => raft_commo_set_network_enabled(this, !disconnect),
      }

  `transport_of` returns None until `raft_transport_serve` binds one, so
  every rerouted site is inert until the worker builds a transport. The
  plumbing therefore lands verified, a site at a time, and the behavioural
  switch stays a single call. `Disconnect` (`server_h.rs:3895`) is already
  rerouted this way.

  **But three sites are coupled to the FIBER runtime, not to the
  clients** *(corrected 2026-09-26: at least eight kernels are, plus the
  `EnsureSetup` jobs and the lab-test fiber -- see the table in phase F of
  the work plan above)* -- which is a sharper constraint than "they share a poll
  thread", and it is what actually decides the unit of work:

  Each is coupled to the fiber runtime, not to the clients:

  - `raft_broadcast_vote_and_wait` --- it does not only send, it WAITS:
    `(*out)->wait_timeout(1000000)` on a `RaftVoteQuorumEvent`, whose
    `ready_` is a `rusty::Arc<::srpc::IntEvent>` (`quorum.hpp:103`, `:135`,
    `:163`). *(Corrected: those lines are the generic `RaftQuorum<Reply>`,
    which nothing uses. The event actually waited on is
    `RaftVoteQuorumEvent : QuorumEventBase` at `commo.h:64`, whose ready/wait
    logic is `src/srpc/reactor/reactor.rs:2295-2304`. The conclusion stands.)* An earlier revision of this line said the wait was
    `raft_fiber_sleep_us`; it is not, and the kernel never calls that. The
    conclusion survives the correction -- waiting on a C++ `IntEvent`
    suspends a C++ fiber -- but the mechanism is the event, not a sleep,
    which makes this the same coupling as the wake gate below rather than a
    separate one.
  - `raft_bind_replication_poll` --- hands a C++ `Arc<PollThread>` to the
    wake gate, whose `owner_` is `rusty::RaftPollThreadPtr`. That is stage
    1a's field-type change.
  - the heartbeat loop --- `raft_spawn_heartbeat_loop` creates a C++ fiber
    on the C++ reactor.

  So the real unit is **"Raft's fibers move lanes, and the sends follow"**,
  not "the commo moves". The three send paths are already Rust
  (`transport.rs`) and proven over TCP; what is left is the runtime they
  run on. That also explains why 1a belongs here: the wake gate's handles
  have to become Rust types in the same change as the fibers.

  *Done when:* RaftLabTest 25/25 with Raft's RPC served entirely by the
  Rust lane, AND the before/after RPC benchmark in the verification rules
  shows no regression. Both, not either.

- [x] ~~3a-old. Move `Communicator`/`RaftCommo` in the *same* change as the~~
  service (`communicator.h:92`, `:51`; `commo.h:112`) — they own the
  `Arc<Client>`s, so a half-move leaves handles straddling lanes.

  **Measured: the boundary is five operations, not 546 lines.** Raft's Rust
  reaches the communicator only through these kernels in server.cc:

      :400   BroadcastVote          :1513  SendAppendEntries
      :441   SetNetworkEnabled      :1635  SendInstallSnapshot
      :1071  PollThread

  Everything else in commo.{h,cc} is C++ plumbing around those five. What
  the Rust side needs is a peer registry of `srpc::Client`s, those five
  operations, the network-enabled flag and the poll-thread handle.

  ~~**But only three of the five can move.**~~ **WRONG ON TWO OF THEM.
  Re-measured while scoping 3e: all five can move.**

  Each row is the operation and what was found:

  - `SetNetworkEnabled` --- movable -- an atomic bool, no C++ object
  - `PollThread` --- movable -- the Rust lane has `PollThread`
  - `SendAppendEntries` --- movable as of the 2c work above
  - `SendInstallSnapshot` --- movable. It does NOT take a `RaftSnapshotManagerPtr` -- read the
    signature, `commo.h:168-175`: scalars, `const std::string& data`, and
    a `std::function<void(uint64_t)>`. The manager belongs to the KERNEL
    (`raft_phase1_load_and_send_snapshot`), which loads the bytes and then
    calls the send. SnapshotManager's virtuals staying C++ is true and
    irrelevant to this operation
  - `BroadcastVote` --- movable. The C++ quorum event never escapes three kernel calls:
    construct (`server_h.rs:4147`), fill-and-wait (`:4149`; was cited as `:4144`), snapshot
    (`:4167`), drop. Everything Rust consumes is `RaftVoteOutcome` -- six
    scalars, `server.h:516-526`. A Rust broadcast can produce that POD
    directly and no C++ quorum event is needed on the path

  The original entries confused "this operation hands over a C++ object"
  with "a C++ object appears anywhere near this operation". The first is a
  blocker; the second is not, and both of these were the second.

  ~~So "move commo to Rust" cannot complete as one step.~~ That conclusion
  rested on the two table rows above that were wrong, so it does not
  follow. It can complete as one step; it is simply a large one, which is
  what 3e is. (The rest of this paragraph is kept as written: the shape it
  describes -- a Rust commo owning peers, clients, flag and poll handle --
  is still the destination, and `commo_` did become unnecessary, though by
  deletion rather than by becoming a Rust type.) The reachable shape
  is a Rust `RaftCommo` that owns the peers, the clients, the flag and the
  poll handle, while the snapshot manager stays C++ -- which remains true
  and remains no obstacle, because the SEND does not take the manager.
### Stage 4 — push the boundary outward

*(Revision 5: 4a is superseded by phase D3, and 4b by T4's stub bullet. Note
that D3 counts three shared `TxLogServer` exports; `EnsureSetup` and
`WaitForStartup` are Raft-only `RaftSpecific` methods, not shared.)*

- [ ] **4a. AS WRITTEN IT IS NOT ACHIEVABLE, and the reason is Paxos, not
  3e.** Measured: THREE of the 31 exports -- `set_commo`,
  `set_site_identity` and `reg_learner_action` -- are what `TxLogServer`
  declares (`scheduler.h:201-213`), and `PaxosServer : public TxLogServer`
  (`paxos/server.h:26`) implements them, so `server_worker.cc` and
  `paxos_worker.cc` call them on both engines. (An earlier revision said
  four and named `IsLeader`; that one is on `RaftSpecific`, which only Raft
  implements, so it is not shared.) Collapsing those to two byte-shaped functions means
  changing the shared interface, which is the one thing this branch is
  not allowed to do.

  What IS achievable, once 3e lands: the Raft-only exports. Of the 31,
  most of the lifecycle and leadership ones are reached only
  from `raft_worker.cc` and `raft_main_helper.cc`, both of which move with
  the fibers -- but NOT `EnsureSetup` and `WaitForStartup`, which
  `ServerWorker::SetupCommo` also calls (`server_worker.cc:123`, `:131`) on
  a `RaftServer*`. So the achievable set is smaller than it looks and needs
  counting before this item is rewritten; the three `Serve*` go with `service.cc`. So this
  item should be rewritten as "collapse the Raft-only exports and leave
  the four shared ones", with a measured count, rather than as "31 to 2".

  Original text: collapse the exports to the two byte-shaped embedder
  functions: `RaftWorker::Submit(const char*, int, uint32_t)` in
  (`raft_worker.cc:760`), `(log, len, par_id, slot_id, queue)` out
  (`:1071-1074`).
- [ ] **4b. Folded into 3e**, not sequenced after it: those stub servers host
  the same one service the lane move is moving, so they cannot be left on
  the other lane for a stage. Kept numbered here because the problem is
  stage 4's shape, not stage 3's. Handle `raft_main_helper.cc:383`, which registers
  `RaftServiceImpl` directly, **bypassing** `CreateRpcServices`, under
  `kSingleGroup` — the build default (`CMakeLists.txt:429`).

### Stage 5 — retire the C++ slice

*(Superseded by revision 5: the C++ lane is kept, so `RaftService` stays in
`rcc_rpc.rpc` and 5a is withdrawn; 5b's check becomes permanent. See phase D
of the two-lane plan.)*

- [ ] **5a. Strictly after 3e's switch, and here is what holds it.**
  Measured: removing `RaftService` from the `.rpc` deletes the class
  `RaftServiceImpl` inherits from (`service.h:20`) and the four return
  types its handlers name (`service.cc:43`, `:58`, `:77`, `:101`).
  Removing `RaftProxy` deletes what `commo.cc` sends through -- 12 uses
  across its four methods. So both files must already be gone, which is
  3e's switch and nothing earlier. The Rust replacements exist and are
  tested (`rpc.rs`'s proxy and dispatch, `transport.rs`'s three send
  paths), so this is sequencing, not missing work.

  Remove `RaftService`/`RaftProxy` from `rcc_rpc.rpc`, regenerate.
- [ ] **5b. Half of this is already automated by 2b.** The "all twelve RPC
  ids are unchanged" half no longer needs a human to check: the frozen
  table (`src/deptran/raft/rpc_ids.txt`) pins Raft's four and the build
  fails if any other service in `rcc_rpc.h` has drawn one of them -- which
  is precisely the collision 5a opens up. What is left to check by hand is
  the other half, that the three remaining services' generated C++ is
  byte-identical, and that is a `git diff` on `rcc_rpc.h` after 5a.

  *Done when:* the other three services are byte-identical and all
  twelve RPC ids are unchanged.

### What "measured" means for each item

Written down because the word was doing too much work. Every item below cites
evidence, but of three different kinds, and only one of them is timing.

```
kind                      what it establishes                        items
------------------------  -----------------------------------------  ----------------------------
structural                counts and locations: symbols, fields,     0a 0b 0c 0d 1c 2b 2c 3a 3b
                      call sites, signatures. Read off the       3c 3d 4a 5a
                      source; refutable against it.
behavioural               the system still does what it did:         P3 2a 2d 3a 3c 3d 3e
                      RaftLabTest 25/25, cargo tests, the
                      gates. Every commit carries one.
timing                    ns/op or ops/s.                            NONE of the above
```

**No item in this TODO has timing evidence.** The three committed paired
trials (`docs/migration/raft/paired-trial-19cfbb213-vs-*.csv`, 25 pairs each,
ABBA) and `rust-vs-cpp-lane-benchmark.md` all predate this plan; they cover
the earlier conversion stages, not 3a, 3c or 3d. So the two performance
claims in 3a and 3c are arguments from call counts and have been relabelled
as such.

What would settle them is a paired trial of `66776cfed` (this plan's base)
against HEAD, which measures 3a's removed `dynamic_cast` and 3c's two removed
ABI crossings together. It needs a production `build/dbtest` in each tree;
HEAD has only `build_raftlab`.

### Verification that must pass at every stage

Standing rules, not items to tick off once. Every commit on this branch has
cleared the first four; the fifth is owed at 3e.

1. **RaftLabTest 25/25.** On an IDLE machine: TEST 9 counts idle RPCs against
   a ceiling of 60, and a concurrent build pushes it over. Measured: 62 with
   this branch compiling alongside, 44-47 quiet. A failure there is the first
   thing to re-run before believing it.
2. **`scripts/raft_field_census.py` exits 0** -- no hand-written C++ names a
   field of `RaftServerBase` or `RaftConsensusState`.
3. **`bash scripts/raft_dsl.sh --check`** -- every carrier's generated C++ is a
   fresh rendering of its Rust, and the crate has no drift.
4. **`cargo clippy --all-targets -- -D warnings`, AND the same with
   `--features raft_test`.** Both, always. The lab configuration is a
   different compilation: an unused import once survived the first because the
   module it was in sits behind `#[cfg(feature = "raft_test")]`.
5. **A before/after RPC benchmark, owed at 3e.** Performance is a hard
   constraint here. Note what this comparison is and is not: Raft already owns
   a dedicated poll thread (the measurement in 3a), so the lane move swaps one
   reactor implementation for another rather than adding a thread. The
   benchmark is therefore a like-for-like comparison of two implementations,
   not a question about thread count.
6. **Every lane, from phase L3 on** (revision 5). Every rule above runs on
   `hybrid`, `cpp` and (from T4) `rust`, plus L4's core-parity and
   exactly-once checks. The benchmark in 5 is owed at L3 (`cpp` vs
   `hybrid`), M2 (`hybrid` before/after) and T4 (`cpp` vs `rust`). Any step
   that touches srpc or a Paxos-shared file also runs `simplePaxos`,
   `shard1Replication` and `shardNoReplication`.

## Why this shape and not the previous two

The premise both earlier versions asserted — "no srpc object crosses lanes" —
becomes **true by construction** instead of true by assertion. It is now half
true: stage 3a deleted `commo_`, so `RaftServerBase` is `Send + Sync` and
`tests/server_is_send.rs` is the compiler saying so. What still crosses is the
wake gate — `waiter_` and `election_waiter_` are C++ `IntEvent` carriers
(`server_h.rs:1269-1270`, minted at `server.cc:1458-1459`) and reply events
are minted C++-side (`commo.cc:41`). Those are 3e's remaining work.

It does NOT collapse the exports to two. There are 31 (`server_exports.h`), of
which 8 carry a non-scalar beyond `RaftServerBase*`, and 4a measures why three
of them cannot go at all: they are `TxLogServer`'s, the interface Paxos also
implements.

*(Written when `commo_` was still a field and the export count was 25. Both
sentences above were present tense and wrong by the time the audit ran; they
are corrected rather than deleted, because the reasoning they supported —
fix `Send` at the root, do not paper over it — is what stage 3a then did.)*

## The measurement that gated this plan — taken

It was item **1c**, and the answer decided everything after it:
`RaftServerBase` had 48 fields of which exactly one, `commo_: *mut
rusty::Communicator`, was not `Send`.

The outcome is the good one this section hoped for. Stage 3a deleted the field
rather than asserting over it, so the count of `unsafe impl Send`/`Sync`
required of `RaftServerBase` is **zero**, and `tests/server_is_send.rs` and
`tests/service_is_a_service.rs` are the compiler saying so rather than this
document claiming it. There is exactly one `unsafe impl` in the new code, on
`service.rs`'s `ServerHandle` (`service.rs:49`; an earlier revision placed it in `transport.rs`), and it exists only because a raw pointer is
unconditionally `!Send` whatever it points at — the pointee's `Send + Sync` is
checked next door.

Two things this section asserted are no longer true and are worth correcting
rather than deleting, because they show what the estimate missed:

- *"the `srpc` dependency is already declared and unused (`raft/Cargo.toml:57`)"*
  — it is declared and now **used**: `transport.rs` builds real
  `srpc::client::Client`s and `srpc::reactor::PollThread`s against it, and
  `service.rs` implements `srpc::server::Service`.
- *"well under an hour"* — the experiment as scoped was never run. 1c got the
  same answer more cheaply by asking the compiler directly, and 1a and 1b were
  retired into 3e, where they belong.

What the section got right, and what still stands: `Arc<srpc::reactor::IntEvent>`
fails `Send`, and the carriers Raft holds pass only because `rusty-rustc`
models them as opaque bytes that check nothing. That is recorded again in
`tests/server_is_send.rs`, which says so in its own comment.

## Where it stands

The TODO above is the source of truth; this is the one-screen version. An
earlier revision restated the whole plan again here in prose, which drifted
from the checklist within two stages, so it is deliberately not that any more.

| stage | state |
|---|---|
| Prerequisite P1-P4 | done -- gate, build, RaftLabTest 25/25, committed |
| 0a, 0d | done |
| 0b, 0c | done -- one copy of srpc's nine C kernels, built by one compiler |
| 1 | done. The measurement answered the question and retired 1a/1b into 3e |
| 2a, 2b, 2c, 2d | done. The RPC slice is generated from `rcc_rpc.rpc` and its ids checked against `rcc_rpc.h` on every build |
| 3a, 3b, 3c, 3d | done. `RaftServerBase` is `Send + Sync`, the gate is one ABI crossing, and `Communicator`'s data is Rust |
| **3e** | **half built.** Service, transport, all three send paths, C ABI and registry are in; only vote traffic is tested over TCP. Re-reading on 09-26 found an AppendEntries wire bug and six more gaps -- phases W and F of the work plan |
| 4a | superseded by revision 5's D3 — three of the exports are `TxLogServer`'s, which Paxos implements (an earlier row said four) |
| 4b | superseded by revision 5's T4 (each lane has its own stubs) |
| 5a, 5b | superseded by revision 5 -- 5a withdrawn (the C++ lane keeps `RaftService`), 5b permanent |
| **revision 5** | two-lane plan: H, R, L (C++ lane from the core), M (payload types), S (seam), T (Rust lane), D. Nothing started. Validated 09-26 (143 claims, 45 upheld findings folded in). Probes: production core transpiles with 0 errors; the lab transpiles hollow until its macros become functions; compile unverified |

Every open item above carries a measured reason rather than a dependency
note. The one that decides the rest is 3e, and its remaining work is not
"move the commo" — the commo's three send paths are already Rust and proven
over TCP. It is "move Raft's fibers": the vote broadcast waits on a C++
`srpc::IntEvent` (the `RaftVoteQuorumEvent` at `commo.h:64`; earlier cited as `quorum.hpp:135`, `:163`, which is the unused generic quorum), the wake gate holds C++
`IntEvent` and `PollThread` handles, and the heartbeat loop is a C++ fiber.
One coupling, three places — which is also why the old item 1a belongs in
that change.

Read 3e first. Everything left of it is done; everything right of it depends
on it.

## Known hard parts, not yet solved

- ~~**The batch wire format.**~~ **DISSOLVED by 2c, not solved -- and the
  distinction matters.** `TpcBatchCommand::save` still writes N back-to-back
  variable-length records with no offsets (`tpc_command.cc:85-91`), and
  `AeApplyIncoming` still needs element *i* as its own ownable value
  (`server_h.rs:4394-4423`). What changed is that **Rust never reads inside
  the batch**: 2c hands the payload across as a byte range computed by
  arithmetic and C++ does all the reading, exactly as today. So the wire break
  this item feared -- repeated `(u32 len, bytes, i64 term)`, with no
  mixed-version path -- is not required by anything currently planned. It
  comes back the moment Rust has to interpret a batch element itself, which is
  5a's territory, so the analysis above is kept rather than deleted.
- **Leader-side kernels untouched by the above**: `server.cc:1728`, `:1741`,
  `:1823`, `:1831` still inspect and manufacture payloads
  (`raft_wire_is_batch`, `raft_batch_finalize`, `raft_stamped_commit`).
- **`LearnerAction` sits on the shared `TxLogServer`** (`scheduler.h:62` for the alias, `:206` for `TxLogServer::reg_learner_action`; an earlier revision cited `:196`, which is a `RaftStartResult` enumerator)
  and Paxos re-forwards the Command (`paxos_worker.cc:82-88`).
- **`panic="abort"`** (`raft/Cargo.toml:68,71`) becomes the whole RPC stack's
  failure mode once Raft owns the server.
- **Stays C++ under every variant**, so not an argument against this design but
  a limit on "Rust owns everything": `SnapshotManager` virtuals
  (`snapshot_manager.hpp:164-225`), the four embedder `std::function`s, and the
  7 `raft_catch` sites (`server.cc:282`, `:981`, `:1068`, `:1138`, `:1209`,
  `:1285`, `:1873`; a `grep -c` returns 8 because it counts the template
  definition at `:254`).

## What is already proven to work

- The Rust srpc runtime: `cargo test --offline --all-targets` in `src/srpc` →
  **281 passed, 0 failed** *(measured at the srpc merge; re-run rather than quote)*, including a real `Server` dispatching to a
  registered Rust `Service` and replying to a Rust `Client` over TCP.
- Rust-side service registration, fiber-per-request dispatch and drain all
  exist (`server.rs:897`, `:1221`, `:1365`, `:1478-1500`). Only Raft's
  generated code is missing.

## Prerequisite — met

The merged tree must build and pass RaftLabTest before any of this starts, and
it does. The srpc gate reconciliation is finished (P1-P4 above, and the
appendix records the five layers of drift it took). The text that stood here
described gate run 7 stopping at exit 118 on a clock stub; that was several
fixes ago and is kept only in the appendix, where it belongs, so this section
does not read as an open blocker.

## Appendix — the exact Mako/srpc gate delta, measured

Probing upstream's gate against Mako's build tree (a verbatim copy placed at
`scripts/` so `repository_root()` resolves correctly) enumerated the real
adaptations. This is what a slimmed gate must own; everything else in the
13,794-line fork is duplication.

1. **Script location.** `repository_root()` is `Path(__file__).parents[1]`, so
   the copy must live at `scripts/`, not be invoked from `src/srpc/scripts/`.
2. **Crate root.** Upstream assumes the srpc crate *is* the repo root:
   `root / EXTRACTION_MANIFEST` (2 sites) and the three emitter inputs
   (`MODULE_PREAMBLE`, `TYPE_MAP`, `CPP_MODULE_INDEX`) need a `src/srpc/` prefix.
3. **Manifest base.** `extraction.load_manifest(root, ...)` must take the crate
   root as its base, or module sources resolve to `base/basetypes.rs` at the
   repo root.
4. **Absent module roots.** CMake renamed the `import std;` BMI directory
   (`__cmake_cxx_std_23.dir` -> `__cmake_cxx23.dir`); upstream errors on a
   missing root, Mako passes both spellings and must skip the absent one.
5. **`--configured-module-map-root`.** Upstream requires it; Mako's invocation
   does not pass it, so `configured_module_dependencies` raises `KeyError` on
   the first module. This one needs CMake plumbing, not a patch.

Plus the ABI delta: **5 symbols**, all Mako's admission gate
(`SERVER_ERR_TRY_AGAIN`, `RpcServiceContext::new_with_admission`, the `Server`
constructor's extra `Arc<Atomic<bool>>`, `admission_ready`,
`set_admission_ready`). If that feature went upstream to stonysystems/srpc the
ABI delta would be zero.

So the slimmed design is: a mechanically refreshed copy of upstream's gate plus
a small patch covering (2), (3), (4) and the 5 symbols, with (1) and (5)
handled by where it is placed and how CMake invokes it.
