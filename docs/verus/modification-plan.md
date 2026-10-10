# Modification plan: a Raft core that ghost-log refinement can verify, with no performance loss

Scope: Mako's Rust-lane Raft (`MAKO_RAFT_LANE=rust`). Path convention: `src/*.rs`,
`rt/src/*.rs`, `server.cc`, `raft_worker.cc`, `service.cc`, `rust-modules.toml`,
`Cargo.toml` and `verus/` are under `src/deptran/raft/`; `ci/`, `scripts/`,
`examples/`, `docs/`, `config/`, `CMakeLists.txt` and `src/deptran/raft_main_helper.cc`
are repo-root paths. Paths prefixed `glr/` are under
`/home/users/zyang2/ghost-log-refinement/`. Companion document:
[README.md](README.md) (the thread audit and the summary of what the method needs).
README §6's conclusion that the fit needs "a single owner thread"
(`docs/verus/README.md:204`) is **superseded** by decision Q1 (0.1): a lock held
for each whole core call is enough, and poll-thread ownership (Phase 7 F11d) is
optional performance work.

Status: written from reading code and docs, plus one machine check (the spec
change S1 in §4.2 was run through Verus on a scratch copy; its log was not
kept, so Phase 0 repeats it). Nothing else was
built or run. Anything not confirmed is marked **not verified**. Line
citations were taken at `76ea6cb46`; at the worktree HEAD `6a1b3a80b`,
`git diff --stat 76ea6cb46 HEAD -- src/deptran/raft` changes only 4 files and
5 lines (`Cargo.toml`, `rt/Cargo.toml`, `raft_bench.cc`,
`rust_facade_types.h`), so the `src/*.rs` citations still hold (checked).

---

## 0. For the executing agent

You are an AI coding agent taking over this plan with no access to the
conversation that wrote it. Read this section, then §1-§2 (concepts), §4 (spec
changes) and "Algorithm preservation", then the phase you are in. Where this
section and a phase disagree on procedure (commands, where results go, when to
stop), this section wins.

### 0.1 Decisions in force (user, 2026-10-03)

The previous revision listed six questions to put to the verification group.
The user has answered them; nothing in this plan waits on the group.

| # | Question | Decision |
|---|---|---|
| Q1 | Are lock-serialized, run-to-completion core calls an acceptable host serialization guarantee? | **Yes.** Phase 4's lock is the guarantee. Phase 7 (poll-thread ownership) is optional performance work only. |
| Q2 | Unguarded `LRejectAppendEntries`? | **Yes**: spec change S1 (§4.2). |
| Q3 | Prev-0 entry-less heartbeat | Ours to decide: a view choice, V1, no spec change (§4.3). The prev-0 entry-less *refusal* is already admitted by the existing `LRejectAppendEntries` guard. |
| Q4 | Spec version freezing | Ours: we freeze versions ourselves (§4.5). |
| Q5 | Snapshots (their A10) and restart | Ours: both gated for certificate v1; designs for later in §4.4. |
| Q6 | Refused committed-conflict path | Ours: a view choice, V2, no spec change (§4.3). |
| Q7 | How much testing between phases? | **Less**: correctness at every phase end, performance at three checkpoints only (0.10). Decided by the user after Phase 0's A/A runs had started. |
| Q8 | How is G2 judged, once its throughput turned out to measure a timing race? | **By the time of rounds that sent to both followers**, traced; throughput and the follower-behind count are reported, not gated (0.10). The Phase 3 checkpoint passes. Decided by the user after the Phase 3 investigation. |
| Q9 | Do the cpp and hybrid lanes follow the verified core (Phase 6)? | **No: removed from this worktree** at Phase 6's start. "It's a verus code for verification, we care about implementation of Raft, not compatibility with the rest of the repo." The core is written for rustc and Verus alone; srpc's own C++ lane is not Raft and stays. Decided by the user at the pre-Phase 6 stop. |

**We may change the spec ourselves**, under two rules that apply to every
change:
1. **Abstract.** A spec action describes Raft behaviour in general and is true
   of any correct implementation. It never encodes a Mako code path, constant
   or quirk ("one implementation, one line of spec" is forbidden).
2. **Correct.** After the change the group's safety proof
   (glr/`src/protocol/Raft/refinement_proof/`) and composition theorem
   (glr/`src/protocol/Raft/ghost_log_compose.rs`) still verify, with the
   full-crate command in 0.5.

If a behaviour cannot be specified both abstractly and correctly, fix or gate
the **code** instead. That fix becomes a new F-item in "Algorithm
preservation" (A.2), and you stop and report before landing it (0.7 point 3).

**The algorithm stays ours.** Mako's Raft must not turn into a new
implementation: same decision rules, batching, per-follower stop-and-wait,
preferred-leader grace, read-index rounds and timers. Every change is "move,
don't rewrite", under a behaviour freeze except the numbered fixes in A.2.

**Estimates are agent wall-clock time**, dominated by build, test, benchmark
and Verus machine time ("Schedule", end of §5).

### 0.2 Repositories, branches, worktrees

| What | Where | Notes |
|---|---|---|
| Mako, work branch | `/home/users/zyang2/mako-verus`, branch `verus-raft` | No build dirs exist yet. |
| Parent arm for perf gates | `/home/users/zyang2/mako-verus-pre` (create) | `git -C /home/users/zyang2/mako-verus worktree add --detach ../mako-verus-pre <parent-sha>`; per phase `git -C ../mako-verus-pre checkout --detach <parent-sha>`; build `build_rust` there; `ln -sfn ../mako-verus-pre/build_rust /home/users/zyang2/mako-verus/build_rust_pre`. Same pattern as `mako-srpc-adopt/build_rust_pre -> ../mako-trace/build_rust`. `/build*` is gitignored (`.gitignore:2-4`). Also build the parent's lab tree there for A.4: `(cd /home/users/zyang2/mako-verus-pre && BUILD_DIR=build_rust ./ci/ci.sh raftLabTest)`, which produces `mako-verus-pre/build_rust_raftlab` (`ci/ci.sh:470-476` configures `cmake -S .` from the current directory, so running it from `mako-verus` would compile the child's source). **`<parent-sha>`** is the merge commit made at the start of the phase (0.8) after its Tier 1 passed, not the previous phase's tip, so upstream mako-dev changes are never charged to the phase. |
| Cumulative-baseline arm | `/home/users/zyang2/mako-verus-base` (create at tag `verus-p0`) | `verus-p0` is the commit after **all** Phase 0 binary-affecting commits (trace kit, lab `LABCOMMIT` logging, election-time harness) have landed. Build `build_rust` and its lab tree there, `ln -sfn ../mako-verus-base/build_rust /home/users/zyang2/mako-verus/build_rust_base`. It moves only when a phase-start merge brings in mako-dev changes that touch the Raft binary: then re-create it as `verus-p0` merged with the same mako-dev commit (a branch `verus-base` in that worktree), rebuild, and record both SHAs in the milestone SUMMARY. |
| Group's spec and proofs | `/home/users/zyang2/ghost-log-refinement`, `main` at `d7e04ed7`, clean | Our spec changes go on a **local branch `mako-spec`** (0.5, §4.5). |
| Other worktrees | `/home/users/zyang2/mako`, `mako-srpc-adopt`, `mako-trace`, `mako-baseline`, `mako-devcheck` | Read only. You may use their builds as perf arms; never rebuild or commit there. |

`src/server_h.rs` and `src/server_cc.rs` are canonical Rust
(`kind = "canonical"`, `rust-modules.toml:97-103`): edit them directly. The
header comment of `Cargo.toml` saying they are generated is stale for them.
Through Phase 4 the cpp and hybrid lanes transpiled the same crate with
rusty-cpp, so they inherited every core change (F1, F3-F5, F8, the Phase 1-4
restructuring), and every crate change had to pass the cpp-lane build.
**Removed at Phase 6's start (Q9):** Raft builds the rust lane only on this
branch; `raft_goal0_source_gate` still checks the inline-DSL C++ headers
(`scripts/raft_dsl.sh --check`), which never covered the core crate.

### 0.3 Environment (once per shell)

No root, no Docker; run everything on the host (`CLAUDE.md` calls Docker
mandatory; that is wrong for this host). Write `~/mako-verus-env.sh` (outside
git) and `source` it in every shell:

```bash
export D=$HOME/.local/mako-deps/usr/lib/x86_64-linux-gnu
export PKG_CONFIG_PATH=$D/pkgconfig LIBRARY_PATH=$D
export LD_LIBRARY_PATH=$D:$HOME/.local/llvm-22/lib/x86_64-unknown-linux-gnu
export PATH=/usr/bin:$HOME/.local/llvm-22/bin:$HOME/.local/bin:$PATH  # /usr/bin/cmake 4.2.3 first; ~/.local/bin/cmake is a broken pip wrapper
export CC=$HOME/.local/llvm-22/bin/clang-22 CXX=$HOME/.local/llvm-22/bin/clang++-22  # /usr/bin/c++ is g++-15
export CXXFLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++
export VERUS_NEW=$HOME/.local/opt/verus-x86-linux/verus                             # 0.2026.09.27, toolchain 1.98.1
export VERUS_PIN=$HOME/.local/opt/verus-0.2026.08.02.b677dd5/verus-x86-linux/verus  # group's pin, toolchain 1.97.1
export PY=$HOME/.local/venv-jetpack/bin/python                                      # numpy, pandas, matplotlib
export RESULTS=$HOME/raft-test-results/verus
export GLR=/home/users/zyang2/ghost-log-refinement
```

All these paths exist (checked 2026-10-03). Two known defects:
- **`$VERUS_PIN` does not run yet**: it fails with
  `librustc_driver-832cf6cfb1386559.so: cannot open shared object file`,
  because `~/.rustup/toolchains/1.97.1-x86_64-unknown-linux-gnu/lib/` holds
  only `rustlib/` (checked). Repair: `rustup toolchain uninstall 1.97.1 &&
  rustup toolchain install 1.97.1`. **Not verified** that this works or that
  the host can reach the network; if it fails, stop and report.
- **`scons` is not installed**, so the group's `SConstruct` cannot be used;
  call Verus directly (0.5).

Smoke tests (record output in `$RESULTS/env/`):

```bash
cd /tmp && $VERUS_NEW /home/users/zyang2/mako-verus/src/deptran/raft/verus/commit_rule.rs  # "11 verified, 0 errors" (checked, 3 s)
cd /tmp && $VERUS_PIN /home/users/zyang2/mako-verus/src/deptran/raft/verus/commit_rule.rs  # after the repair
```

### 0.4 Builds

```bash
cd /home/users/zyang2/mako-verus   # same flags in mako-verus-pre and mako-verus-base
cmake -S . -B build_rust -G Ninja -DCMAKE_MAKE_PROGRAM=$HOME/.local/bin/ninja \
  -DCMAKE_BUILD_TYPE=Release -DMODE=perf -DMAKO_USE_RAFT=ON -DUSE_MALLOC_MODE=1 \
  -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DMAKO_RAFT_LANE=rust
cmake --build build_rust -j 48 2>&1 | tee $RESULTS/<phase>/build/build_rust.log
# hybrid: -B build -DMAKO_RAFT_LANE=hybrid    cpp: -B build_cpp -DMAKO_RAFT_LANE=cpp
```

- `ci.sh` configures the lab trees itself (`${BUILD_DIR}_raftlab[_<lane>]`,
  `ci/ci.sh:458-476`) with no compiler or stdlib flags, so the exported `CC`,
  `CXX`, `CXXFLAGS`, `LDFLAGS` are required.
- Timeouts: at least 30 min for a fresh build, 10 min for an incremental one.
- Never build while a perf gate runs.

### 0.5 Verus commands

**Full-crate check of the spec** (the "Correct" rule). Same arguments as
glr/`SConstruct:120` minus `--compile`:

```bash
cd $GLR && /usr/bin/time -v $VERUS_PIN --crate-type=dylib --expand-errors src/lib.rs \
   > $RESULTS/spec/<label>.log 2>&1; tail -3 $RESULTS/spec/<label>.log
```

Reference counts: glr/`README.md:131`'s `1256 verified, 0 errors` is
**stale**: the group's own audit gives 1,970 verified items after A8 under the
pinned 0.2026.08.02 and calls the rolling release comparable
(glr/`docs/ghost-log/raftrs/port-audit.md:921`). A scratch run of `d7e04ed7`
under `$VERUS_NEW` (previous revision of this plan; log not kept) reported
`2129 verified, 0 errors` in 4 min 36 s. Expect the v0 count under `$VERUS_PIN`
to be close to that (1,970-2,129); a much lower count means part of the crate
was not checked: investigate before accepting it. Phase 0 records the count
under `$VERUS_PIN` as the **v0 reference**; every later run must report at
least that many verified and 0 errors.

**Mako-side Verus files** (spike crate, `verus/commit_rule.rs`, later
`core/`): `$VERUS_PIN` once Phase 8 imports the spec (one vstd throughout);
`$VERUS_NEW` is fine before that.

### 0.6 Where results go; quiet machine; investigating perf

- Raw results: `$RESULTS/<phase>/...`, **never in git**, nor summaries of
  them: the table, SHAs, command lines and raw-results path go in the reply
  and the commit message.
- Exact gate commands are in §6 ("Gate commands"). Tier 1 suites run
  serially (they share ports).
- **Quiet machine**: no build, no other benchmark, no Verus job during a perf
  gate. Record `uptime` before each gate. (Phase 0's concurrency check may
  relax this; §5 "Schedule".)
- **Investigating a perf result**: measurement instrumentation only (for
  example the 12-stage trace kit, `~/raft-test-results/perf/trace-kit/`).
  Never change logic, timers or constants while investigating.

### 0.7 Stopping and reporting points

Reports are plain language, concrete example first, every term defined. Anything
longer than a screen goes to `$RESULTS/<phase>/report.md`; the reply gives
the path and the 3-5 conclusions that matter. Stop and report:
1. **After Phase 0**: smoke tests; the v0 count; G1-G7 baselines with paired CV,
   MDE, derived rounds and bounds; spike results (a) cargo, (b) rusty-cpp,
   (c) opt-level/LTO, (d) spec import.
2. **After every perf gate, immediately**: a table of median ratio, sign-test
   p and verdict per point. If a gate fails, stop after the report;
   investigate only afterwards, with instrumentation only.
3. **Before landing any behaviour change not already numbered in A.2**,
   including a fix forced by a spec decision: the change, why the spec cannot
   absorb it abstractly and correctly, its measured cost. Wait for a yes.
4. **After each spec change**: its abstract statement and the verification
   count. (No approval needed to make it; approval needed to push it anywhere.)
5. **At each phase end**: Tier 1 (with first-attempt failures), equivalence
   check (A.4), diff-ledger summary, new bug-log entries (0.9), commit SHA
   pushed to `backup`.
6. **Real blockers only** (toolchain cannot be repaired; Verus cannot express a
   construct, with the attempt shown). Do not stop at self-chosen checkpoints inside a phase.
7. **User decisions; stop and wait** at exactly these points: before Phase 6
   starts (fate of the cpp and hybrid lanes: **decided**, removed, Q9); after Phase 8 (whether to do Phase 5 and/or
   Phase 7); before F11d (Propose's result scheme); before falling back from
   V3 to F2 (§4.3).
8. **Comparator exemptions out of hand**: if more than half the lab cases
   exempt the (term, leader) sequence (A.4 item 1), stop and report.
   **Closed** (0.10): it was reached in Phase 0 (16 of 27 cases), and the
   comparator is no longer a gate.

### 0.8 Git rules

- Commit on `verus-raft`, one commit per ledger-coherent step, message ending
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and carrying a
  `Ledger: M1,M2,F3` trailer (A.3).
- **Push only to `backup`**: `git push backup verus-raft`. Never to `origin`
  (makodb/mako), `fork` or `srpc`.
- Stay merged with mako-dev: at each phase start, `git fetch origin mako-dev &&
  git merge origin/mako-dev`, then Tier 1. That merge commit is the phase's
  parent arm (0.2). If the merge touched the Raft binary (srpc, compiler,
  `src/deptran/raft/**`), also re-create the base arm at `verus-p0` merged with
  the same mako-dev commit (0.2), so the cumulative gate measures only this
  plan's changes; report any upstream-induced shift (old base vs new base, one
  Tier 2 run of G1 and G2) separately, not as a gate result.
- Scope: `src/deptran/raft/**`, `src/deptran/raft_main_helper.cc` where a phase
  says so, Raft scripts and docs, verification files. srpc subtree edits (Phase
  7 eventfd) also go on the re-apply list (the R4 lesson). In `$GLR`, commit
  only on `mako-spec`; **never push `mako-spec`** without the user's approval.
- A deferring `#[allow(...)]` carries a `TODO` saying what must be verified
  before removal. Checks derive constants from source (e.g. the lab case count
  from `init2(` greps, as `ci/ci.sh:491-494` does), never literals.

### 0.9 Bug log (user, 2026-10-03)

Record every bug found while executing this plan in
[bugs-found.md](bugs-found.md), whether or not it gets fixed: defects in the
existing Raft code, the harness, the scripts or the toolchain, and bugs this
plan's own changes introduced once a build, test or review caught them (say
what caught them). Each entry gives: where (file:line at a named commit), a
concrete failing scenario, how it was confirmed (read in code or reproduced;
an unconfirmed suspicion is marked as one), severity, and its fate (fixed in
commit X, became an F-item, deferred, report only). Write the entry when the
bug is found, commit it with the step that found it, and name new entries
in the next report. Recording a bug does not license fixing it: the
behaviour freeze (§A) still applies, so a fix needs an A.2 F-item or the
user's yes (0.7 point 3).

### 0.10 Testing between phases (user, 2026-10-03)

The user cut the testing between phases. Where they disagree, this section
overrides §6, the **Gate** line of every phase in §5, A.4 and the Schedule's
quiet-machine hours (about 86 h on the critical path become about 10 h).

- **Correctness, every phase:** Tier 1 (§6's command block) once at the phase
  end, not after every commit. First-attempt failures are still reported.
- **Equivalence:** the lab comparator (A.4 items 1 and 2, and item 3(a)) is not
  a gate. Until Phase 3 the lab passing (every verdict, and replicas agreeing)
  is the check. From Phase 3 the replay checks apply as written: A.4 item 3(b)
  and (c), items 4 and 5. `lab_trace_compare.py` stays as a diagnostic.
- **Performance, three checkpoints only:** after Phases 3, 6 and 8, run G1-G6
  with two arms, `build_rust_base` (`verus-p0`, merged with mako-dev as 0.8
  says) against the child. Rounds and bounds come from
  [gate-params.md](gate-params.md), and the pass rule is §6's. Also run G7
  after Phases 3 and 8, with the same arms. Phases 1, 2 and 4 get no
  performance run, and no checkpoint uses a parent arm.
- **Dropped:** all of Tier 3 (full sweeps, Jetpack, snapshot points), and
  Phase 0's concurrency check, sweep and Jetpack baselines. Phase 0 ends with
  the A/A runs of G1-G7.
- **A failed checkpoint:** stop and report it (0.7 point 2). Investigate
  afterwards (0.6), first by running the failing point on the phase-end
  commits since the last checkpoint.
- **G2 is traced (user, 2026-10-04, after the Phase 3 checkpoint).** At
  G2's saturation the leader skips a follower whose reply missed the round's
  early-quorum pass, and how often that happens is a timing race that decides
  the throughput (Phase 3 report §3.1, since removed). So G2 gates
  on the time of rounds that sent to both followers, from the Phase 0 trace
  kit, over 10 rotated rounds with a +2% bound and §6's pass rule
  (`scripts/verus/two_follower_rounds.py`, run by `gate_point.sh`). Beside
  it, not gated: 25 untraced rounds' throughput and how many runs end with a
  follower behind (`scripts/verus/follower_behind.py`). The Phase 3
  checkpoint's G2 failure is passed by the same decision.
- Optional Phases 5 and 7 get their checks when the user schedules them.

---

## 1. Plain-language summary

### 1.1 What changes, and why

Today Raft's decisions are made in Rust code that several threads reach under a
mutex, and two fibers sleep in the middle of a protocol step. Ghost-log
refinement checks a node only if each call into its protocol code runs from
start to finish with nothing else touching that state, and only if that code is
plain safe Rust with no FFI, locks or atomics.

So the plan pulls Raft's decisions into a small **core**: one Rust struct plus
one function per event, doing no I/O. Everything else (srpc, encode/decode,
timers, the apply thread, the C++ shim) stays as a trusted **shell** around it.
The group's method is then applied to the core, against the group's spec with
one small change of ours (S1, §4.2).

Example. Today one heartbeat fiber sends AppendEntries (phase 1), sleeps in
1 ms steps while replies arrive (phase 2, `src/server_cc.rs:1501-1690`, sleep
at `:1676`), then advances commit (phase 3). After the change the same round is
three kinds of core call, none of which sleeps:

```
TickHeartbeat          -> [Send AE to f1, Send AE to f2]   (and ApplyRange, if
                                                             phase 0 advanced commit)
RecvAppendResp(f1, r)  -> []            (match_index[f1] updated)
RoundEnd               -> [ApplyRange(41..45)]
```

(`TickHeartbeat` may return `ApplyRange` because today's phase 0 already runs
the commit rule: `heartbeat_phase0_locked` calls `raft_commit_advance` at
`src/server_cc.rs:767` and publishes the round's commit snapshot at `:768`; the
caller enqueues at `:833-838`.) The shell keeps the
1 ms polling loop and calls the core between sleeps, so timing does not change.
Each core call closes one or more ghost-log segments, each proved to be a legal
spec step.

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

Payload bytes never enter the core; it stores an opaque handle per entry
(today's `RaftCommand`, a refcounted C++ pointer whose clone only bumps a count,
`server.cc:461-464`).

### 1.3 What "the RPC is correct" must mean

The method assumes **less** than "RPC is correct". Our code needs a little
**more**, and Phase 1 removes that extra need.

**What the method assumes** (glr/`src/protocol/Raft/ghost_log_compose.rs:378-386`,
the `causal` predicate; glr/`docs/ghost-log/raftrs/composition.md` §3;
glr/`docs/ghost-log/spec/raft-spec.md` §2; `msg_view` is defined in §2):
1. *Genuine packets.* Every message handed to node i's core equals, under
   `msg_view`, a message some verified core earlier queued for i. Each entry of
   a batched append counts on its own.
2. *Truthful sender.* The source id is the real sender (no Byzantine peers).
   That the sender is a configured voter is a code check, B20
   (glr/`docs/ghost-log/raftrs/composition.md:80-84`), done by `step_checked`
   in Phase 6.
3. *Static, shared configuration* (composition.md §3 item 2).
3a. *Segments are atomic and globally ordered* (glr/`docs/ghost-log/raftrs/composition.md:117-119`,
   §3 item 3): the schedule is some linearization of all nodes' closes. Within
   our node this is provided by Q1's whole-call lock (one core call at a time,
   run to completion).
4. *Per-node trusted base*: storage, FFI, the node's constructor (composition.md
   §3 item 4). For v1 this also includes "no replica restarts in place under its
   old id" (§4.4.2).
5. *Nothing else about the network*: loss, duplication, reordering, delay and
   timers firing at any moment are all allowed. A refused message counts as
   dropped.

**What our code relies on in addition today, and its fate:**
- **Forged "unavailable" vote replies.** When disconnected or not RPC-ready,
  `ServeVote` answers `reply_term = can_term, vote_granted = 0`
  (`src/server_h.rs:4704-4716`), a reply no core produced, at the candidate's
  term. `rpc_ready_` is false in production windows too: before startup is
  published, after FailStop (`:2886`), on an apply-thread failure (`:3413`),
  during shutdown (`:4570`). **Absorbed by view choice V3** (§4.3), no code
  change: a vote refusal at or below the candidate's term never reaches the
  core as a received message, so the forged reply is never treated as
  genuine. F2 (a code fix) is kept only as a fallback. `ServeAppendEntries`'
  unavailable reply (0/0/0, `:4718-4732`) is already read as "unavailable", a
  drop (`src/server_cc.rs:1563-1565`).
- **Reply attribution.** A reply must be attributed to the follower whose
  pending slot it completes (part of "truthful sender/routing"). The rest of
  pairing is not needed: a success reply carries the end index it proves,
  `follower_last_log_index = accepted_through = prev + count`
  (`src/server_h.rs:5371`, `:5466-5469`), and the leader takes
  `min(reported, sent_end, leader_last)` (`src/server_h.rs:276-288`). The spec's
  AppendResponse is `{term, success, match_index, follower, read_ctx}`
  (glr/`src/protocol/Raft/types.rs:51`); the leader's `sent_term` and
  `CONTRADICTORY` checks (`src/server_cc.rs:1453-1469`) are extra filters.
- **Vote replies counted by number** (`rt/src/transport.rs:555-577`,
  `:625-644`): a duplicated reply counts twice. Fixed by F1 (Phase 1).
- **Codec fidelity.** The C++ batch codec must carry the per-entry
  (term, command) list unchanged: `raft_stamped_commit`
  (`src/server_cc.rs:999`, deep copy with the term set, `server.cc:1450-1460`)
  on send; `raft_batch_term_at` (`src/server_h.rs:1498`) and
  `raft_command_from_bytes` (`rt/src/service.rs:43`) on receive.

> **"RPC correct" means exactly:** every message the shell hands to the core
> was produced by the claimed sender's core for this destination, with its
> `msg_view` fields unchanged; all nodes share one static voter set; the
> shell's send side encodes exactly the entries and terms the core emitted in
> that call's `Output`. The shell may lose, duplicate, delay or reorder.

| What | Status after the plan |
|---|---|
| Genuine packets, truthful sender, routing (incl. reply-to-slot attribution) | Trusted (the method's own assumption) |
| Forged unavailable vote reply | Unmodelled input under V3 (never a `Recv`); F2 fallback only |
| Rest of reply pairing | Not needed; Phase 5 is an optional echo; the vote tally becomes a voter-id set (F1) |
| Sender is a configured voter | Checked by `step_checked` (F9) |
| Encode/decode fidelity | Trusted, tested (round-trip property test, replay recorder) |
| Timers, clock, randomness, srpc retries and reconnect replay (`src/srpc/rpc/client.rs:1546`) | Need nothing |
| No in-place restart under the old id | Trusted assumption of v1 (§4.4.2) |
| Liveness | Not proved; CI suites only |

---

## 2. Concepts

**Spec / spec action.** The group's atomic Raft spec `LNextAtomic`
(glr/`src/protocol/Raft/raft_refinement.rs:35`; actions in
glr/`src/protocol/Raft/raft.rs`; table in glr/`docs/ghost-log/spec/raft-spec.md`
§1): single-server steps such as LTimeout, LGrantVote, LFollowerAppendEntries,
LAdvanceCommitIndex. Each has a **guard** (what it requires of the pre-state)
and an **effect** (the post-state and the messages it sends) over a small
abstract state: term, role, vote, log, commit, `votes_granted`,
`match_index`, config.

**Spec change.** An edit we make to that spec (§4), subject to the two rules
in 0.1 (abstract, correct). Numbered S1, S2, ...

**Ghost log.** A list that exists only in proof code and is erased at compile
time (glr/`src/protocol/Raft/ghost_log.rs:178-195`): `Recv(src, msg)` or
`Tick` (what started the call), `Set(field, value)` (each tracked write),
`Send(dst, msg)`, `Close(label)` (one spec action finished).

**Segment.** The entries between two `Close`s. One call may close several (a
higher-term vote request closes LStepDown then LGrantVote). Each function
preserves one invariant, `inv` (glr/`docs/ghost-log/method/design.md:104-117`).

**Refinement.** At every `Close(label)`, Verus proves the writes and sends
since the previous close are exactly what the named action allows
(glr/`src/ports/raftrs/coupling.rs:2399-2431`, `close_seg`). Handlers need not
line up one-to-one with spec actions.

**Coupling, alpha, msg_view, view choice.** `alpha` (`state_view`) reads the
core's fields as the spec state; `msg_view` reads a wire message as a spec
message. "Coupled" means `alpha(state) == replay(log)`
(glr/`docs/ghost-log/raftrs/coupling.md` §1-§2). Fields alpha does not read are
**unmarked**; writes to them are stutters. A **view choice** is how we define
alpha or `msg_view`; it is ours, belongs to the coupling proof, and changes no
spec text (V1-V3 in §4.3).

**Component (BR1/BR2).** One wire append carrying k entries is read as k
single-entry spec messages, its components (BR1); BR2 proves the follower's
writes decompose into those k steps; when the follower refuses, only
component 0 counts as received (glr/`docs/ghost-log/raftrs/coupling.md:109-137`).
BR3, raft-rs's `batch_append` merge, has no Mako counterpart.

**Gate.** A configuration check keeping an unmodelled feature unreachable,
checked at start-up or per message by `step_checked`, which refuses
out-of-gate messages (a refusal is a drop;
glr/`src/ports/raftrs/raw_node.rs:527`, `:634-656`). Example: snapshots off.

**Sans-I/O core / event, handler, action.** raft-rs's RawNode shape
(glr/`src/ports/raftrs/raw_node.rs:634-1157`): no sockets, threads or clocks.
An *event* is one call into the core (`RecvAppendEntries`, `TickHeartbeat`,
`Propose`); its *handler* runs to completion; *actions* are outputs the shell
carries out: `Send(peer, msg)`, `ApplyRange(i, j)`, `LeaderChanged(bool)`,
`ResetElection`, `WakeReplication`.

**Suspension point.** Where a fiber gives up the poll thread mid-action: the
phase-2 sleep (`src/server_cc.rs:1676`), the vote wait (`rt/src/seam.rs:264-301`,
`Fiber::sleep(200)`), `await_vote_settled` (`src/server_h.rs:4917-4930`), the
heartbeat wait (`src/server_h.rs:3648`). A *fiber* is an srpc coroutine on the
poll thread.

**Serialized calls (host guarantee).** `inv` means something only if no other
thread writes the core's fields during a call. raft-rs got this for free
(single-threaded; glr/`docs/ghost-log/method/design.md:229-233`). We provide it
with a lock held for each **whole** core call, the core reachable only through
that guard (`with_core`, Phase 4). This is decision Q1. Calls are serialized,
run to completion, never re-enter, and reach the core only through `&mut`.

**Shell / atomic mirror.** The shell is everything outside the core (trusted).
A mirror is a copy of one core field (`is_leader`, leader hint, commit,
applied) the shell writes after each call; other threads read only mirrors.

**Equivalence check / replay.** A check that a refactor did not change
behaviour (A.4). Our lab cannot replay byte for byte today: it runs on a live
reactor with wall-clock fibers (`rt/src/lab_runtime.rs:17-31`; `src/lab.rs:76-78`,
`:168`) and unseeded `rand()` (`src/lab.rs:65-67`, `:566-572`). So before
Phase 3 it compares **modulo timing** (same leaders and terms, same committed
logs); from Phase 3 a recorder at `step()` enables **byte-identical replay**.

**Labels from the group's documents.**
- *A-numbers* (their work items): A5 panic freedom, A8 ReadIndex reads, A9
  membership plus the clause that an append carrying entries carries the
  leader's exact commit (glr/`src/protocol/Raft/raft.rs:363-368`), A10
  snapshots.
- *B-numbers* (glr/`docs/ghost-log/raftrs/coupling.md` §5): B4 (a success reply
  may not claim more than the leader's log), B16 (entry terms positive), B17 (a
  reject's match is viewed as 0), B19 (a candidate seeing a same-term append
  steps aside first), B20 (a modelled message comes from a configured voter).
- *T1-T9* (glr/`docs/ghost-log/raftrs/port-audit.md:45-55`): their labelled
  behaviour-preserving port rewrites; our M-kinds (A.3) follow them.
- *V3*: raft-rs's choice that `next_index` is a shadow field.
- *LStepDown / LStepAside* (glr/`src/protocol/Raft/raft.rs:223-250`): adopt a
  higher term and become follower / become follower in the current term
  (guard-free).
- *Diff-2 lint*: their check that a proof-only commit changes no executable
  code (port-audit.md:684-707).

**Our own labels.** *AuthorityLedger / HeartbeatAuthority*: the leader's record
of heartbeat acknowledgements for read-index authority
(`src/server_cc.rs:130-470`). *ReplicationWakeGate*: the submit path's wake
object (`src/server_h.rs:1143-1405`). *The R4 lesson*: an srpc subtree fix lost
on a sync (the since-removed `raft-latency-regression.md` §5). *CV*: standard
deviation over mean. *MDE*: the smallest change n rounds can resolve. *Sign
test*: how likely the split of rounds where B beat A is by chance
(`scripts/raft_perf/paired_stats.py:36-41`). *Knee*: client count where latency
rises steeply. *Jetpack*: the closed-loop client sweep
(`docs/performance/jetpack-comparison/`, since removed). *Tier 1-3, G1-G7*: §6.

---

## 3. Target architecture

### 3.1 Verified core vs trusted shell

**The core** (crate `src/deptran/raft/core/`, Phase 6) holds:
- the fields of `RaftConsensusState` (`src/server_h.rs:918-1014`): term, vote,
  role, leader hint, log, commit, execute index, peers' next and match;
- config and self id (fixed at Setup), `stopped`, `pending_leader_term`;
- the protocol half of `PendingTable` (follower, sent_term, sent_round,
  sent_end per slot); the `RaftResponsePtr` handles stay in the shell;
- the heartbeat round and `AuthorityLedger` (unmarked);
- a `VoteSet` of voter ids keyed by campaign term (from Phase 3);
- `applied_index`, as input.

No `unsafe`, `extern`, raw pointers, locks, atomics, hashing, logging or clock
reads.

**The shell**: srpc, `rt/src/{seam,transport,service,snapshot}.rs`, the C++
codec and kernels in `server.cc`, `raft_worker.cc`, `raft_main_helper.cc`,
fibers and timers, the apply thread, `ReplicationWakeGate`.

Rules:
1. **Serialized access.** `&mut RaftCore` only through `with_core(|c| ...)`;
   each guard covers exactly one core call (today's `mtx_`); the shell carries
   out actions after releasing it. **The shell never reads core state after a
   call**: everything it needs is copied into that call's `Output` under the
   guard; an `Entries` or `ApplyRange` action carries the k entry handles and
   terms. Otherwise another call (a step-down then a different leader's append,
   `src/server_h.rs:5434-5441`) could rewrite those slots and the shell would
   encode entries that differ from the logged `Send`. Today the batch is built
   in the same lock section that chose prev (`src/server_cc.rs:1117-1222`,
   selection and stamping `:948-1060`); keep that property.
2. **No suspension inside a call.** Every wait becomes a shell timer plus an
   event (`RoundEnd`, `VoteDeadline`).
3. **Payloads are handles.** `Cmd` is an `#[verifier::external_body]` opaque
   type wrapping `RaftCommand`, feeding the spec's uninterpreted `value_view`
   (glr/`src/ports/raftrs/alpha.rs:27`). Size, kind and `has_value` are cached
   in `RaftEntry` at append time. An inbound payload is a second opaque type,
   `WireBatch`, with trusted `materialize(i) -> Cmd` and `term_at(i)`, called
   only after the handler's checks pass.
4. **Outputs are data**, with `Vec`s reused across calls (no allocation per
   event).
5. **Batching uses BR1/BR2** with Mako's caps (256 entries, 16 MiB).
6. **Cheap entry checks.** `step_checked` uses integer compares only (F9).

### 3.2 Data flow

- **Submit.** `add_log_to_nc` → `RaftWorker::Submit` (`raft_worker.cc:854`) →
  the shell clones the handle outside the lock → `with_core(|c| c.propose(cmd))`
  (role check, append at `current_term`, returns `WakeReplication`) → the shell
  wakes the heartbeat fiber. Stays on the submit/app thread (`src/server_h.rs:4663-4697`).
  `propose` returns accept/reject synchronously: `Submit` returns early on
  `REJECTED`, else increments `n_tot` (`raft_worker.cc:856-862`), which
  `get_outstanding_logs` uses (`raft_main_helper.cc:977-990`). The returned
  index/term are discarded (`raft_worker.cc:853-858`).
- **Replication.** `TickHeartbeat` runs today's phase 0 (may return
  `ApplyRange`, one LAdvanceCommitIndex segment), then per follower returns
  `Heartbeat{prev, prev_term, commit}`, `Entries{prev, prev_term, [(term, Cmd)]×k, commit}`
  or `Skip`. `commit` is read in the same call (F3). After the guard the shell
  stamps and finalizes from the `Output` handles (`server.cc:1450-1472`, F7),
  sends, and stores the response handle.
- **Reply.** Per completed slot, `RecvAppendResp(slot, Option<AppendReply>)`;
  `None` = unavailable (today's 0/0/0 sentinel, `src/server_cc.rs:1563-1565`).
- **Commit.** `RoundEnd` runs today's `heartbeat_phase3_locked`
  (`src/server_cc.rs:1725-1747`) and returns `ApplyRange(i, j)`.
- **Apply.** The shell pushes `ApplyRange`'s handles to the apply queue (as
  `EnqueueCommittedEntries`, `src/server_h.rs:3837-3905`); the apply thread pops
  one entry per iteration (`:3335-3355`) and reports `Applied(n)` per entry
  (`:3429`).
- **Follower append.** One event
  `RecvAppendEntries{term, from, prev, prev_term, commit, payload: WireBatch}`.
  Inside one call the core runs the gates (stopped, non-voter, unauthoritative
  sender), reads terms through `term_at`, validates the count, finds the first
  conflict, truncates, materializes each appended entry, advances commit, and
  returns `ApplyRange` plus the reply. This keeps today's single critical
  section (`src/server_h.rs:5420-5460`), which the method needs: the log is a
  marked field the spec writes in the same LFollowerAppendEntries step as
  commit and the reply (glr/`src/protocol/Raft/raft.rs:489-519`), and a marked
  write no action owns is a hard blocker (glr/`docs/ghost-log/raftrs/coupling.md:6-8`).
  It also keeps "decode only after the gates" (`src/server_h.rs:5272-5280`).
  Materializing is one Arc clone (`server.cc:1546-1551`).
- **Election.** `TickElection(now, timeout_sample)` increments the term, votes
  for self, returns `Send RequestVote` to every peer. Each reply becomes
  `RecvVoteResp(from, term, granted)`: the transport callback records it in
  shell state without touching the server (as today, `rt/src/transport.rs:563-577`),
  and the polling fiber (200 µs) hands it to the core. On quorum the core
  becomes leader and returns `AppendNoop`, `LeaderChanged(true)`,
  `WakeReplication`. `VoteDeadline(term)` ends a failed campaign. The 200 µs
  poll and 1 s wait stay through Phase 6.

---

## 4. Spec changes, view choices and gating

### 4.1 Gap table

| Mako behaviour | Handling | Spec change? |
|---|---|---|
| Election, vote, step-down, append accept, append reply, commit rule, leader no-op | Existing actions. `verus/commit_rule.rs` is a reduced standalone model and proof skeleton (`verus/commit_rule.rs:1-20`), not a discharged obligation; Phase 8 re-proves the rule on the core's types against `LAdvanceCommitIndex` / `commit_quorum_ok` (glr/`raft.rs:447-473`, `:806-808`). | none |
| Vote request with higher term | `LStepDown`, then `LGrantVote` or `LRejectVote` (`src/server_h.rs:3199-3258`) | none |
| Candidate (or same-term leader) receiving an accepted current-term append | `LStepAside` (glr/`raft.rs:223-234`), then accept (B19) | none |
| Election win | k × `LReceiveVoteGranted`, `LBecomeLeader`, `LClientRequest` for the no-op | none |
| Append refusals that change no state: stopped (`src/server_h.rs:5241-5246`), non-voter or unauthoritative sender (`:5255-5271`), bad payload (`:5281`, `:5349-5355`) | `LRejectAppendEntries` after **S1**, preceded where the code does so by the `LStepDown`/`LStepAside` segment of the term check (`:5320-5347`). The reply's hint reads as match 0 (B17). | **S1** |
| Ordinary refusals: stale term, prev mismatch or prev beyond the follower's log (reply with the `last_index` hint, `src/server_h.rs:5349-5355`, which drives the leader's FAST backoff, `:762-788`), prev-0 entry-less refusal | `LRejectAppendEntries` under the **existing** guard (glr/`raft.rs:607-616`: `!prev_log_ok`, `!append_pos_ok`, fourth disjunct `ae_prev_index == 0 && !ae_has_entry`) | none |
| Refused committed conflict (`src/server_h.rs:5414-5430`) | **V2** (§4.3): existing guard | none |
| Entry-less append at prev 0 carrying the full commit (`src/server_cc.rs:1236`) | **V1** (§4.3) | none |
| Phase-1 leader check outside the lock that builds the message (`src/server_cc.rs:1101` vs `:1117`) | **Code fix F3.** Covering it in the spec would need "a server may send an append of a term it no longer holds", which needs history and breaks message-term invariants: not abstract, not cheap. | none |
| Forged unavailable vote reply (`src/server_h.rs:4704-4716`) | **View choice V3** (§4.3): vote refusals at or below the candidate's term are unmodelled input; the give-up closes `LStepAside`. Code unchanged. (A spec letting a node *send* a refusal at the candidate's term would break "a message's term ≤ its sender's term", so the spec is not the place for it.) F2 is the fallback. | none |
| Vote replies counted by number (`rt/src/transport.rs:555-577`, `:625-644`) | **Code fix F1** | none |
| Log slot without a command (`src/server_h.rs:5385-5410`) | View invariant: every slot in [base, last] has a command | none |
| `failover_ == false` (`src/server_h.rs:2313`) | Gate on `failover_ == true` (F5) | none |
| Read-index authority ledger (`src/server_cc.rs:130-470`) | Unmarked; `read_ctx` reads as 0. A8 only once a read API exists. | none |
| Snapshots, compaction, InstallSnapshot | **Gated in v1** (`MAKO_RAFT_SNAPSHOTS` unset; `src/server_h.rs:2607-2625`, checked; snapidx stays 0, base 1, the leader's snapshot branch `src/server_cc.rs:1158` unreachable). Design in §4.4.1. | v2 only |
| Restart under the same id | **Gated in v1** as a trusted assumption. Design in §4.4.2. | v3 only |
| Membership change | Static, fixed in `SetupInternal` (`src/server_h.rs:2479-2540`); the constructor closes one `LLoadConfig` segment (glr/`raft.rs:62-67`) | none |
| Lease reads | Out of scope; Mako does not use them | none |

Net for certificate v1: **one spec change (S1), three view choices (V1-V3),
two code fixes (F1, F3)** in this table (F4-F9 in A.2 are gates and checks).

### 4.2 Spec change S1: a follower may refuse any AppendEntries

**Current text** (glr/`src/protocol/Raft/raft.rs:602-625`; used in `LNextAtomic`
at glr/`raft_refinement.rs:72-73` and in `RaftMessageAction` at
glr/`refinement_proof/state_machine.rs:196`). Guard (`:607-616`):
`ae_term < s.current_term` ∨ `!prev_log_ok(..)` ∨ `!append_pos_ok(..)` ∨
`(ae_prev_index == 0 && !ae_has_entry)`. Effect: `s_ == s` plus one
`AppendResponse{term: s.current_term, success: false, match_index: 0, follower: c.my_id, read_ctx: 0}`.

**(a) New text.** Delete the guard. Effect and signature are unchanged, so the
labels in glr/`ghost_log.rs:95`, `:356`, `:619-630`, `:733` and the
`StepKind::RejectAppend` binding (glr/`step_kinds.rs:164-171`) stay.

```rust
/// Refuse an AppendEntries: state unchanged; sends AppendResponse{success: false}.
/// Unguarded, the append analogue of LRejectVote (B1): a refusal withholds an
/// acknowledgement and changes no state; the leader may only use it to move
/// next_index, which LHandleAppendReject treats as unconstrained bookkeeping.
/// raft.tla's reasons (stale term, prev mismatch, wrong position) and reasons
/// the abstract state cannot see (a stopped or fenced server, an unrecognised
/// sender, an undecodable payload) all read as this action.
pub open spec fn LRejectAppendEntries(
    s: LState, s_: LState, c: LConstants,
    ae_term: int, ae_prev_index: int, ae_prev_term: int, ae_has_entry: bool,
    sent_packets: Seq<LRaftMessage>,
) -> bool {
    &&& s_ == s
    &&& sent_packets == seq![LRaftMessage::AppendResponse {
        term: s.current_term, success: false, match_index: 0int,
        follower: c.my_id, read_ctx: 0int,
    }]
}
```

Proof maintenance: `#[verifier::rlimit(150)]` → `#[verifier::rlimit(400)]` on
`lemma_follower_commit_prefix_advanced_common`
(glr/`refinement_proof/log_induction_commit.rs:1056`), whose comment says its
case analysis is near budget; the old guard helped Z3 prune the reject case.
Alternative: an `assert(!(st.kind is RejectAppend))` hint. Also update the
HandleAppendEntriesRequest row of glr/`docs/ghost-log/spec/raft-spec.md`.

**(b) Abstract.** "A server may decline to acknowledge an append" holds for
every correct Raft: a refusal is observationally a loss plus an unsuccessful
reply, and the network may lose messages anyway
(glr/`docs/ghost-log/spec/raft-spec.md:26`). The group made the same argument
for `LRejectVote` (glr/`raft.rs:133-141`) and `LStepAside` (`:218-219`). The
text names no Mako condition; it covers raft-rs or etcd equally.

**(c) Correct.**
- *Argument.* `s_ == s` keeps every per-server invariant. The new packet is an
  `AppendResponse` with `success == false`, `read_ctx == 0` and the sender's own
  term. Packet invariants constrain only success responses
  (glr/`refinement_proof/log_invariants.rs:121-134`, `:463-521`) and read echoes
  (`read_ctx != 0`, glr/`read_induction.rs`); message-term bounds hold.
- *Proof sites.* Every site handling `RejectAppend` uses only `s_ == s` and the
  shape of `sent`: glr/`refinement_proof/log_induction_frame.rs:52`, `:123`;
  `log_induction_commit.rs:153`, `:940`; `log_induction.rs:821`;
  `log_induction_votes.rs:70`; `membership_induction.rs:96`, `:569`, `:631`;
  `read_induction.rs:71`, `:364`, `:469`, `:519`, `:1808`. Sites that *assert*
  the action (glr/`ports/raftrs/coupling.rs:2963`, `:3031`;
  `ports/raftrs/raft.rs:3179`; the composite oracle in `raft_refinement.rs`) get
  easier, since the predicate is weaker.
- *Machine check (prototype, scratch copy, not kept; **unconfirmed** until
  Phase 0 repeats it — a later re-run attempt was refused by that session's
  permissions, so this rests on the previous author's run only).* Phase 0 keeps
  the log of its re-run (command, Verus version, count) under
  `$RESULTS/spec/`. Previous result, under `$VERUS_NEW`:
  unmodified `d7e04ed7` full crate **2129 verified, 0 errors** (4 min 36 s, 30
  threads); S1 alone: one rlimit failure in the lemma above; S1 + rlimit bump:
  **2129 verified, 0 errors** (5 min 41 s), covering `refinement_proof/*`,
  `ghost_log*.rs`, `raft_refinement.rs` and the raft-rs port with its
  `compose_bridge`. **Not yet run under `$VERUS_PIN`** (step S1 below); the
  rlimit value may differ there.

### 4.3 View choices (no spec change)

**V1. Prev-0 entry-less append (Q3).** Our leader puts its full commit in every
append (`src/server_cc.rs:1236`); for an entry-less append at prev 0,
`LSendAppendEntries` requires `heartbeat_commit_ok`, commit ≤ the follower's
acknowledged match (glr/`raft.rs:369`, `:683-688`).
- *View.* For an entry-less append, `msg_view` reads `leader_commit` as
  `min(lc, prev)`; at prev 0 that is 0, satisfying `heartbeat_commit_ok`.
- *Sound because* both ends use the same `msg_view`; the follower only uses
  `min(lc, prev + count)` (`src/server_h.rs:5449`), and `min(min(lc, e), e) =
  min(lc, e)`, so the follower's commit is the same under the view. The success
  reply (match = `accepted_through` = 0, `:5466`) is `LFollowerHeartbeat` with
  commit 0 (glr/`raft.rs:534-558`).
- *Must not* clamp components that carry entries
  (`has_entry ==> leader_commit == s.commit_index`, glr/`raft.rs:368`).
- Cost: one coupling lemma in Phase 8.

**V2. Refused committed conflict (Q6).** The previous revision said
`prev_log_ok` and `append_pos_ok` could both hold here. They cannot, for the
component the refusal belongs to: the refusal fires only when the first
conflicting index c satisfies `c ≤ old_last_log_index` (`src/server_h.rs:5408`,
`:5414-5430`); under BR2 only component 0 is received, with prev = `leader_prev`
and c ≥ prev + 1, so prev < `s.log.len()` and `append_pos_ok(s, prev, true)`
(`prev == s.log.len()`, glr/`raft.rs:661-663`) is false. The **existing** guard
admits the refusal (and S1 would anyway; we keep the lemma so V2 does not
depend on S1).
- The coupling must show the refusal precedes any marked write (check at
  `:5414`, truncate at `src/server_h.rs:5438`); the earlier term and role updates
  (`:5320-5347`, `:5357-5365`) are their own `LStepDown`/`LStepAside` segments.
- The refusal stays in the code (defence against malformed input). We do not
  add raft-rs's unreachability proof (their B7): days of work for no gain.

**V3. Vote refusals at or below the candidate's term are unmodelled input
(replaces code fix F2).** Plain version: a "no" vote never changes anything the
spec tracks, so the core may hear it without the proof treating it as a
received message.
- *Today.* The forged unavailable reply always has `reply_term == can_term`
  (the candidate's own term) and `vote_granted == 0`
  (`src/server_h.rs:4704-4716`). The Rust-lane tally only counts it as a "no"
  (`rt/src/transport.rs:601-624`: `no += 1`; `highest_term` changes only if the
  reply term is greater). A step-down to a higher term happens only on
  `ADVANCE_HIGHER_TERM` from `observed_response_term`
  (`src/server_h.rs:4048-4073`), which a forged reply can never supply, because
  its term equals the campaign term. A "no" quorum only makes the candidate
  give up its campaign.
- *View.* The shell hands vote replies with `granted == false` and
  `term <= campaign term` to the core inside a **Tick-opened** call (no
  `Recv`); the core's "no" count is an unmarked field, so those writes are
  stutters. The give-up closes `LStepAside`, which has no guard and sends
  nothing (glr/`raft.rs:210-234`: "When to step aside is implementation
  policy ... the action has no guard"; listed in `LNextAtomic` at
  glr/`raft_refinement.rs:93`). Only refusals with a **higher** term need a
  `Recv` (they close `LStepDown`), and a higher-term reply is always genuine.
  Granted votes stay `Recv`-opened (`LReceiveVoteGranted`). `causal` is never
  asked to cover a non-genuine packet.
- *Abstract and correct:* no spec text changes.
- *Code:* `ServeVote`'s unavailable branch stays as it is.
- **Not verified** until the Phase 3 coupling table is written: that the "no"
  count reaches no marked field on any path. If it does, fall back to F2
  (A.2), stopping first at 0.7 point 7.

### 4.4 Snapshots and restart: gated in v1, designs for later

#### 4.4.1 Snapshots, compaction, InstallSnapshot (spec v2, not scheduled)

Gated for v1 because they are off by default and in every perf sweep
(`src/server_h.rs:2607-2625`), the group lists A10 as to-do
(glr/`docs/ghost-log/README.md:231`), and our snapshot path crosses C++ kernels
(`server.cc`, `rt/src/snapshot.rs`, `snapshot_seam_cpp.cc`).

Design sketch (holds for any Raft with snapshots):
- *State.* No new `LState` field; the spec `log` stays the full abstract log;
  the core keeps the compacted prefix as an erased ghost `Seq`, so alpha is
  unchanged.
- *Compaction* is a storage event: a stutter (`s_ == s`).
- *New message* `InstallSnapshot{term, leader, last_index, last_term, prefix: Seq<LLogEntry>}`;
  `prefix` is spec-only (the bytes stand in for it; codec-fidelity trust).
- *`LSendSnapshot`*: guard `s.role is Leader`, `0 < last_index <= s.commit_index`,
  `prefix == s.log.take(last_index)`, `last_term == s.log[last_index-1].term`;
  effect `s_ == s` plus one `InstallSnapshot`.
- *`LInstallSnapshot`*: guard `term >= s.current_term`; after the step-down
  if needed, `log' = if last_index <= s.log.len() && s.log[last_index-1].term == last_term { s.log } else { prefix }`,
  `commit' = max(s.commit_index, last_index)`, reply
  `AppendResponse{success: true, match_index: last_index}`.
- *New invariants*: every `InstallSnapshot` packet's
  `prefix == ds.commit_log.take(last_index)` (same shape as the commit-witness
  invariants near glr/`step_kinds.rs:250`); the replace branch implies
  `s.commit_index < last_index` (analogue of `truncate_ok`).
- *Risk*: every `match` on `LRaftMessage` (about 20 proof files) gains an arm.
- *Agent cost*: spec + safety proof 4-8 days; Mako coupling 3-5 days.

#### 4.4.2 Restart from persisted state (spec v3, not scheduled; a real gap)

- Mako's Raft is memory-only (`src/server_h.rs:2219-2221`); a grep found no
  persistence of term, vote or log (**not verified** beyond the grep; with
  snapshots on, recovery restores state-machine bytes only).
- So a replica that restarts and rejoins under its old id comes back at term 0
  with no vote and an empty log: it can vote twice in a term, and if a majority
  restarts, committed entries can be lost. **A real safety gap, not a proof
  limit.** Whether production or CI ever restarts a replica in place is **not
  verified** (a grep of `ci/ci.sh` found only cleanup kills).
- **v1 decision**: the certificate states "no replica restarts in place under
  its old id" as a trusted assumption (§1.3 item 4). Fixing it needs term, vote
  and log made durable before each vote grant and append acknowledgement,
  synchronous I/O on the hot path, which breaks the behaviour freeze and the
  performance contract, so it is out of scope. A "fail-stop if I ran before"
  marker would also break legitimate full-cluster restarts; rejected.
- *v3 design* (raft.tla's Restart, glr/`docs/ghost-log/spec/raft-spec.md:392-397`):
  `LRestart(s, s_, c, ci)`, guard `s.conf_index <= ci <= s.commit_index`, effect
  role Follower, empty `votes_granted`, `match_index`, `next_index`,
  `pending_reads`, `served_ctxs`, `commit_index: ci`, everything else kept (term,
  vote, log, config are the durable state). **Correct only under
  synchronous persistence**: keeping the whole `log` (and term and vote) is
  valid only if every entry, the term and the vote are durable before any
  acknowledgement or vote grant that depends on them (raft-rs's port assumes
  the same, `store.synchronous()`, glr/`src/ports/raftrs/raw_node.rs:525-528`).
  New obligations: a persistence-boundary invariant (durable term, vote, log
  equal the marked fields at every `Send`); and `commit_index` falling back to
  `ci` breaks per-server commit monotonicity, which the safety proof uses in
  many lemmas — the group calls this "the main work"
  (glr/`docs/ghost-log/spec/raft-spec.md:392-397`). Spec + proof: several
  agent-days, same range as the snapshot work (4-8 days), **not verified**;
  meaningless until the code persists.

### 4.5 Where the modified spec lives; version freezing (Q4)

**Choice: a local branch `mako-spec` in `$GLR`, created from `d7e04ed7`, with
the patch series and a manifest committed in Mako.** Why:
- The "Correct" rule is about the group's **whole** crate (safety proof,
  composition, raft-rs port, ten protocol modules). A branch runs exactly that
  check; a vendored minimal subset could not run the raft-rs port, and whether
  it even builds standalone is **not verified**.
- A branch rebases onto the group's future `main`, and the patch series is
  directly offerable upstream (only with the user's approval).
- Vendoring would copy other people's code into Mako.
- Reproducibility from one Mako commit comes from the patches: the base commit
  plus `git am` of the series reproduces `mako-spec` exactly.

Layout in Mako, `src/deptran/raft/verus/spec/`:
- `patches/` = `git -C $GLR format-patch d7e04ed7..mako-spec -o <this dir>`,
  regenerated after each spec commit;
- `SPEC_VERSION.toml`: upstream commit `d7e04ed7`; patch list (S1, ...);
  sha256 of every **statement-bearing** file, i.e. each file that defines what
  the certificate says: the atomic spec (`types.rs`, `raft.rs`,
  `raft_composite.rs`, `raft_refinement.rs`, `read_index.rs`, `membership.rs`;
  the group's own list of the atomic spec is
  glr/`docs/ghost-log/raftrs/port-audit.md:880`), the method layer
  (`ghost_log.rs`: `ActionLabel` `:72`, `action_holds` `:324`;
  `ghost_log_compose.rs`: `certs_ok` `:75`, `causal` `:381`,
  `theorem_compose_safety` `:901`), and the safety property and its guard
  copy (`refinement_proof/invariants.rs`: `RaftSafetyInvariant` `:204`;
  `refinement_proof/state_machine.rs`: `RaftMessageAction` `:176`, which with
  `action_holds` repeats `LNextAtomic`'s guards verbatim,
  glr/`raft_refinement.rs:31-34`), all under glr/`src/protocol/Raft/`; Verus
  version `0.2026.08.02.b677dd5` and toolchain 1.97.1; the verify command (0.5)
  and its result line;
- `scripts/verus/verify_spec.sh` (in Mako): checks `$GLR` is clean at
  `mako-spec`, that `git -C $GLR diff d7e04ed7..mako-spec` equals the committed
  patches, recomputes the hashes, runs the full-crate command, and exits 1 if
  any hash or the count differs or errors > 0.

**Rules for every spec change.**
1. One logical change per commit on `mako-spec`; the message states the
   abstract behaviour (no Mako names) and which plan item needs it.
2. Full-crate run: verified ≥ the previous version's count, 0 errors.
3. No new `assume`, `admit`, `external_body` or `#[verifier::external]` in
   `src/protocol/Raft/`: `git -C $GLR diff d7e04ed7..mako-spec -- src/protocol/Raft | grep -nE '^\+.*(assume\(|admit\(|external_body|verifier::external)'`
   prints nothing.
4. Abstractness self-check, recorded in `docs/verus/spec-changes.md`: no Mako
   identifier, constant or code path; phrased as "any server in state X may ..."
   over spec fields only. Plus a row: the change, why it is still Raft, log
   path, count.
5. Report (0.7 point 4).

**Freezing.** A version is frozen as a local tag `raft-spec-vN` on `mako-spec`
plus the manifest in Mako. Between freezes, spec files change only through
numbered S-changes. Proof-only edits (rlimits, hints) in files **outside** the
hashed list do not bump the version; **any** edit to a hashed file does, even
one that looks proof-only (those files also hold proofs, and a hash cannot
tell a lemma edit from a definition edit). If that becomes a burden,
`verify_spec.sh` may instead extract and hash just the named definitions
(`action_holds`, `ActionLabel`, `causal`, `certs_ok`, the signature of
`theorem_compose_safety`, `RaftSafetyInvariant`, `RaftMessageAction`, and every
`spec fn` of the atomic-spec files); decide in Phase 0 and record it in the
manifest. Rebasing onto a newer group `main` happens
only at a freeze, followed by a full re-run.
- **v0**: upstream unmodified (Phase 0 baseline).
- **v1**: v0 + S1. Frozen in Phase 0, before any core code is written against
  it. The Phase 8 certificate targets v1.
- **v2** (+ snapshots), **v3** (+ restart): only if scheduled.

How the Phase 8 proof imports the spec is a Phase 0 spike item (d): (i) Verus
`--export`/`--import` of the spec crate, or (ii) a Mako port module on
`mako-spec` (`src/ports/mako/`, as the group did with `src/ports/raftrs/`)
whose `#[path]` points at the Mako core sources. **Neither verified.** If
both fail, fall back to vendoring the whole upstream `src/` (8.1 MB) into
`src/deptran/raft/verus/glr/` at the frozen version, and report.

---

## A. Algorithm preservation

**Plain statement.** After all phases, Mako's Raft makes the same decisions, in
the same order, at the same times, as at the Phase 0 baseline, except for the
numbered fixes in A.2. The code changes shape (one struct, returned actions,
cut fibers, a separate crate, ghost code); the algorithm is not rewritten.

### A.1 Frozen behaviour

| Behaviour (rule, constants, order of effects) | Where today |
|---|---|
| Vote grant: term, log up-to-date, one vote per term | `doVote`, `src/server_h.rs:3199-3258` |
| Commit: majority match by rank selection, current-term entry | `raft_commit_advance`, `src/server_cc.rs:633-666`; `majority_match_index`, `src/server_h.rs:881-903` |
| Append accept, conflict search, truncation, refused committed conflict, follower commit = min(leader_commit, prev+count) | `src/server_h.rs:5214-5460` |
| Ack = min(reported, sent_end, leader_last) | `src/server_h.rs:276-288` |
| Batching caps (256 entries, 16 MiB) and per-entry byte cap while selecting | `src/server_cc.rs:935-1060` |
| Per-follower stop-and-wait: one in-flight slot, cleared on term change or leadership loss; follower order; where `pending_rpcs.place` happens | `PendingTable`, `src/server_cc.rs:38-90` |
| Heartbeat round: phase 0 commit, 1 send, 2 collect with early-quorum exit, 3 commit + ledger settle | `src/server_cc.rs:710-1791` |
| Preferred-leader election timeouts and startup grace | `GetElectionTimeout`, `src/server_h.rs:2223-2245` |
| Read-index authority rounds | `src/server_cc.rs:130-470` |
| Timers: 1 ms phase-2 poll (`src/server_cc.rs:1505`, sleep `:1676`), min(100 ms, heartbeat) round cap (`:1508-1517`), 200 µs vote poll and 1 s deadline (`rt/src/seam.rs:264-301`), 1 ms apply idle sleep (`src/server_h.rs:3366-3372`), election-timeout knobs | as listed |
| Apply one entry per iteration, `Applied` per entry | `src/server_h.rs:3335-3355`, `:3429` |
| Synchronous submit accept/reject | `raft_worker.cc:853-862`, `raft_main_helper.cc:977-990` |

Timers stay frozen through Phase 6; Phase 7 may change only what its own
sub-items list.

### A.2 Allowed behaviour changes (exhaustive)

Each lands in its own commit with a lab case showing the new behaviour.

| No. | Phase | Change | Why |
|---|---|---|---|
| F1 | 1 | Vote tally counts **distinct voter ids** for the campaign term (Rust lane `TallyState`, `rt/src/transport.rs:555-577`, `:625-644`; the callback knows its peer, `:559`); quorum rule unchanged; C++ lane untouched | Duplicated replies must not count twice |
| F2 | fallback only | **Not planned**: V3 (§4.3) absorbs the forged reply. Only if the Phase 3 coupling table shows the "no" count reaches a marked field, and after stopping at 0.7 point 7: `ServeVote`'s unavailable branch (`src/server_h.rs:4704-4716`) answers with an RPC error. The Rust-lane handler can already do that: `fn vote(&self, req) -> Result<VoteResponse, i32>` (`rt/src/service.rs:108`), and the caller's callback drops `code != 0` (`rt/src/transport.rs:567-569`). `ServeVote` reports through out-parameters, so it must also signal "unavailable" (a return value) for `vote` to return `Err(code)`. Cost: a not-ready peer's "no" becomes silence, so a candidate waits for its 1 s deadline (G7) | The forged reply is not genuine |
| F3 | 1 | Inside the phase-1 locked block (`src/server_cc.rs:1117`): `if !is_leader_ \|\| current_term_ != round.term() { break }`, and send the `commit_index_` read there, not phase 0's snapshot (`:768`, `:1236`) | LSendAppendEntries' commit clause (glr/`raft.rs:363-368`); the values agree today but Phase 7 could break that |
| F4 | 1 | Reject term-0 entries in `AeDecodePayload` (`src/server_h.rs:4263-4288`, checked: no check today) and on the single-entry path | B16 |
| F5 | 1 | `verified_config_ok()` at the end of `SetupInternal`: snapshots off ⇒ snapidx 0, base 1; `failover_` true; static config containing self. Logs in a normal build, fail-stops in a gated build | Makes the gate explicit |
| F6 | 2 | Leader-change callback and `raft_log_set_is_leader_entry` run after `mtx_` is released, in emitted order | Effects become actions; removes a lock-order hazard |
| F7 | 3 | Entry stamping and finalize (`server.cc:1450-1472`) run after the guard, from `Output` handles | Same deep copy, outside the lock |
| F8 | 4 | `CommitIndex`, `IsLeader`, `GetLeaderHint` read atomic mirrors | Removes a data race (`src/server_h.rs:4655-4657` is an unlocked read today) |
| F9 | 6 | `step_checked` refuses (no reply) only: sender self or not a configured voter (B20), term 0, a success reply claiming more than the leader's log (B4), entry term 0 (B16), and raft-rs's prev shape (prev index 0 ⇔ prev term 0; index + count must not overflow; glr/`src/ports/raftrs/raft.rs:2999-3000`, which has **no** prev ≤ last check). A prev beyond the follower's log is **not** refused: it is ordinary log repair, answered today with a reject carrying the `last_index` hint (`src/server_h.rs:5349-5355`) that drives the leader's FAST backoff (`:762-788`), and it is `LRejectAppendEntries` under the existing guard (`!prev_log_ok`, glr/`raft.rs:612`). That path stays unchanged | Out of gate; a refusal is a drop |
| F10 | 5, optional | Reply echo `(follower, sent_term, sent_end)` on the Rust-lane wire; mismatch refused | Defence in depth only |
| F11a-d | 7, optional | (a) replies as events, `RoundEnd` at quorum or deadline; (b) eventfd wake; (c) blocking apply channel with batched `Applied(n)`; (d) poll-thread ownership | Performance only |
| F12 | fixes | The round end advances the commit index only while the core leads: `heartbeat_phase3_locked` calls `raft_commit_advance` only when `core.is_leader_` (bugs-found B17) | The commit rule is the leader's (`LAdvanceCommitIndex`'s guard) |
| F13 | fixes | `Start` refuses (REJECTED) a command without a value, before appending (B16) | Every log entry has a value |
| F14 | fixes | The AppendEntries decoder refuses an entry whose term exceeds the leader's term, on the batch and single-entry paths (the rest of B4) | A genuine leader never sends one; refused like any undecodable payload (S1) |
| F15 | fixes | A campaign is lost as soon as enough peers refuse that a yes quorum is out of reach, `no > (n - 1) - n/2`, in raft-rt's tally and the core's `VoteSet` (B1) | Standard Raft; a lost election ends early (G7 timing) |
| F16 | fixes | `set_is_leader`'s dead stale-publication term check removed (B8) | Dead code; no behaviour change |
| F17 | fixes | `heartbeat_interval_us_` becomes an atomic (B14) | Removes a data race; no behaviour change |
| F18 | fixes | `get_outstanding_logs` counts this worker's own accepted submissions above the commit index (B7) | A metric; no protocol effect |
| F19 | fixes | The shell's entry points take `&RaftServerBase`; the fields they change move behind interior mutability (B19). `RaftSpecific` (scheduler.h) takes `&self` throughout, so its C++ virtuals become `const`; `TxLogServer`, which PaxosServer shares, is unchanged, and its two late-callable methods export `&self` twins | Removes aliased `&mut`; no behaviour change |
| F20 | disk P1 | `Event::ObserveTerm { term, stopped, failover }`: a newer term seen outside a modeled message (InstallSnapshot and its reply) is raised, the vote and leader hint cleared, and the server steps down as `SettleElection` does; the shell no longer writes `current_term_`/`vote_for_` itself (disk plan, the user's decisions of 2026-10-08) | Only core steps write term and vote, so a persist note covers every change; uncoupled (`coupled` false), never stepped under the verified gates |
| F21 | disk P1 | The persist note: `step` compares term, vote and commit before and after its arm, and the two log writers (`Propose`, the AppendEntries handler) mark the lowest index they wrote; `CoreOutput` carries the note (disk design §3, Decision 4) | Each step's saved-state change, exactly; no behaviour change (the shell reads it only in disk builds) |
| F22 | disk P1 | `Event::Restore { term, vote, commit, entries }` loads a recovered state before `EnterGates`, refusing (`Restored(false)`, unchanged) a state no step produces; it sets no persist note and queues the apply of the committed prefix (disk design §3 "Startup") | A restart resumes from its saved state (B6); uncoupled until disk plan P9 |

Because of S1, the extra append refusals (stopped, non-voter, unauthoritative
sender, bad payload) and the refused committed conflict
(`src/server_h.rs:5240-5349`, `:5413-5429`) **stay exactly as they are**.

**Adding to the list**: a behaviour that cannot be specified abstractly and
correctly becomes F12, F13, ... with a row as above; stop at 0.7 point 3 first.

F12-F19 were approved together by the user on 2026-10-06: "can you first fix
all remaining bugs of Raft on this branch?" B6 (memory-only state) is not
among them; it is the disk-persistence project ([disk-persistence.md](disk-persistence.md)).
F11a's first half is taken (2026-10-10, disk plan §5): collect waits on an
event the AppendEntries reply callback sets (`raft_collect_wait_us`, rt
`seam.rs`), its step now the timeout, instead of sleeping the step; the
round end stays where it was. Measured as a memory-mode gate in the commit
that takes it. Two more pieces of it followed (2026-10-10), both found by the
disk model's G2 miss: collect also ends when a reply from an earlier round
frees a follower's slot, so the tick sends to that follower then instead of
at the round's deadline (`server_cc.rs`, `heartbeat_collect_body`) -- only
when the leader's log is ahead of that follower: ending early for a
caught-up one chained an idle leader's rounds, each one's replies landing
as an earlier round's (lab TEST 9: 67 RPCs in an idle second, limit 60);
and a
reply that lands after a round that ended at its deadline without a majority
wakes the replication loop (rt `transport.rs`, `reply_landed`; the gate's
`stalled_round_`). EmptyAppendEntries replies now wake collect as well.

F20 was decided by the user on 2026-10-08 (disk plan, "The user's decisions");
F21 and F22 with the plan, on 2026-10-09: "start implementing the plan".

### A.3 "Move, don't rewrite": allowed transformation kinds

Every executable change that is not an F-item is one of these (modelled on
glr/`docs/ghost-log/raftrs/port-audit.md:45-55`). Anything else is a rewrite:
make it an F-item or undo it.

| Kind | Transformation | Rule |
|---|---|---|
| M1 move | Relocate a field or function | Body unchanged except paths |
| M2 retype | `&mut RaftServerBase` → `&mut RaftCore`; `self.x` → `self.core.x` | No statement added, removed or reordered |
| M3 effect → action | A side effect inside a decision becomes a push into `Output` at the same point | Shell executes in push order; only F6/F7 move work relative to the lock |
| M4 parameterize | A clock read or random sample becomes an event parameter (`now`, `timeout_sample`) sampled by the shell at the same point | Same distribution and site |
| M5 fiber cut | Code before a suspension becomes one handler, code after another; locals crossing the cut become fields; the wait and its constant stay in the shell | Same handler order per round, same follower order |
| M6 cached metadata | A per-entry FFI query (`has_value`, `payload_bytes`, `kind`, `is_tpc_commit`) is stored in `RaftEntry` at append time | Value-identical |
| M7 logging (T1) | Core logging becomes a no-op; the shell logs from report structs | No decision reads a log result |
| M8 loops (T3) | Iterator chains and closures become loops | Same order |
| M9 collections (T7) | `rusty::BTreeSet<u16>` → sorted `Vec<u16>` and other subset swaps | Same order and membership; measured on G2 |
| M10 panics | `raft_verify` / `panic!` → `assert!`, same condition | Proved unreachable later |
| M11 opaque wrapper | FFI handles as `external_body` (`Cmd`, `WireBatch`) | Same calls, same points |
| M0 instrumentation | Trace kit, lab `LABCOMMIT` lines, election-time timestamps (Phase 0) | Env-gated, inert when unset; no decision reads it; ledgered |
| M12 ghost (Phase 8) | `proof {}`, `let ghost`, `Ghost(..)`, `requires`/`ensures`/`invariant`, `spec fn`/`proof fn`, `#[verifier]`, `open_recv`/`open_tick`/`ghost_send`/`close_seg` | Erased; diff-2 lint |

**Diff ledger** `docs/verus/diff-ledger.md`: one row per changed hunk in
`src/deptran/raft/{src,rt/src,core}/`, `server.cc`, `raft_worker.cc`,
`raft_main_helper.cc` (phase, commit, file:line before → after, kind, reason,
evidence from A.4). Put `// [move, M<n>]` or `// [fix, F<n>]` on or just above
the changed line. From Phase 6, a lint adapted from glr/`scripts/port_diff2_lint.py`
(`scripts/verus/ledger_lint.py`) classifies every `+`/`-` line of
`git diff <phase-0-commit> -- src/deptran/raft/core` as ghost, structural or
labelled and exits 1 on anything else; in Phase 8 its ghost-only mode is the
diff-2 lint.

### A.4 Equivalence check per phase (before the perf gate)

1. **Phase 0: comparator.** `scripts/verus/lab_trace_compare.py` extracts per
   lab case (i) the (term, leader) sequence, (ii) each node's committed log as
   (index, term, payload hash), (iii) the verdict. Item (ii) does not exist in
   today's lab output (a lab log has no payload-hash line; its commit lines are
   only `[COMMIT-CALC]` match-index dumps), so Phase 0 first adds an env-gated
   line per applied entry, `LABCOMMIT <node> <idx> <term> <fnv64(payload)>`,
   printed only when `MAKO_RAFT_LAB_COMMIT_LOG=1` (kind M0, ledgered).
   **Calibrate** on 5 runs of every case on one build; fields that vary are
   exempt for that case, listed with reasons in
   `docs/verus/lab-compare-exemptions.md`. **Validate** on 5 fresh same-build
   runs: the comparator must report "equal" without any new exemption. Report
   the exempt fraction per case; stop at 0.7 point 8 if more than half the
   cases exempt the (term, leader) sequence. Committed-log agreement and
   verdicts are never exempt. No lab seeding: the lab's only `srand` is
   clock-seeded (`service.cc:122`) and no `MAKO_RAFT_LAB_SEED` exists; adding
   one would be a code change for little gain, since the comparison is modulo
   timing anyway. `raftLabTest` deletes its `/tmp/raft_lab_test_XXXX.log` on
   success (`ci/ci.sh:479`, `:509`), so the comparator runs
   `<arm lab dir>/deptran_server -f config/raft_lab_test.yml -P localhost`
   itself (one `deptran_server` path per arm: `build_rust_raftlab` here,
   `../mako-verus-pre/build_rust_raftlab` for the parent), logging to `$RESULTS`.
2. **Phases 1-2: modulo timing.** Each case 3× on parent and child (6 full
   lab runs, about 1 h); non-exempt fields equal. Cases an F-item changes on
   purpose (F1, F4) are named in the commit and covered by the new lab cases.
3. **Phase 3:** add the recorder at `step()` (per node: each event with `now`
   and `timeout_sample`, each `Output`, entries as (term, kind, length, payload
   hash), deterministic text) writing to `$RESULTS/replay/<commit>/`; capture a
   full rust-lane lab run and short G1, G4 runs. Pass (a) modulo timing against
   Phase 2, (b) determinism self-replay (Phase 3's recordings through Phase 3's
   core reproduce every `Output` byte for byte), (c) an explicit per-follower
   order review in the report.
4. **Phases 4, 5, 6, 8: byte-identical replay** of the parent's recordings
   through the child's core (`cargo test` target `core_replay`). Phase 5's echo
   compares the shared projection. Phase 8 also runs the diff-2 lint and checks
   the ghost-instrumented core replays identically to Phase 6's.
5. **Phase 7:** its recordings replayed through the Phase 6 core must be
   byte-identical (only delivery timing changes); modulo timing must hold. A
   sub-change that touches the core is a new F-item.

---

## 5. Phased plan

Every phase passes Tier 1, its equivalence check (A.4) and its perf gate (§6)
before merging, and can ship on its own. "Gate" lines name the exact commands
of §6. Times are agent wall-clock (see "Schedule").

### Phase 0. Baseline, harnesses, spikes, spec v1 (1.6-2.2 days)
**Goal.** Make the gates meaningful, settle the build questions, freeze spec v1.

**Order.** First land every commit that changes the Raft binary (trace kit,
`LABCOMMIT` lines, election-time harness), then tag that commit **`verus-p0`**
and build the base and pre arms there, then run the A/A and the baselines.
Otherwise the A/A run compares two different binaries and the base arm lacks
code every child carries.

**Changes:**
- Worktrees and builds (0.2, 0.4): `build_rust`, `build`, `build_cpp` here;
  `build_rust` in `mako-verus-base` and `mako-verus-pre` at `verus-p0`; the lab
  trees `build_rust_raftlab`, `build_raftlab_hybrid`, `build_cpp_raftlab_cpp`
  that `ci.sh` configures itself, plus `build_rust_raftlab` in both worktrees
  (0.2): about 10 builds.
- `scripts/raft_perf/rotation_trial.sh`: forward `GROUP="${GROUP:-single}"` as
  `--group-mode "$GROUP"` in `run()` (`:28-33`; `examples/raft_bench.sh` already
  accepts it, `:73`, `:111`). Same in `paired_trial.sh`.
- Gate scripts of §6: `scripts/verus/gate_point.sh`, `scripts/verus/perf_gate.py`,
  `scripts/verus/paired_cv.py`, `scripts/verus/election_times.py`,
  `scripts/verus/jetpack_gate.py`, and `docs/verus/gate-params.md` seeded with
  the default rows (25 rounds, default bounds) before the A/A run.
- **Election-time harness for G7**: `examples/raft_bench.sh` already kills the
  leader (`--kill-leader-at-sec`, `:115`, `:384-424`); add timestamps for leader
  loss, the new leader's first "offering load" and its first commit (about
  50-100 lines, kind M0), written into the run's JSON as
  `leader_loss_us`, `new_leader_us`, `first_commit_us` (names fixed by this
  plan) and read by `election_times.py`.
- Commit the 12-stage trace kit behind `MAKO_RAFT_TRACE_FILE` (inert when
  unset). Its patch is `~/raft-test-results/perf/trace-kit/raft-trace-insertions.patch`;
  `git apply --check` fails at HEAD (`error: patch failed:
  src/deptran/raft/server.cc:1708`), so re-anchor it by hand, including its
  stages 3, 4, 7, 8.
- The lab comparator and the `LABCOMMIT` lines (A.4 item 1).
- Trace the leader-change callback (`raft_worker.cc:311-330` →
  `raft_main_helper.cc:651-726`) for re-entry into Raft (needed by F6).
- **Spikes.** Put `PeerTable::majority_match_index` (`src/server_h.rs:881`) in a
  small `verus!` crate and (a) build it in the `raft_rust` cargo lane with ghost
  code erased; (b) run it through rusty-cpp (cpp/hybrid lanes); (c) match the
  Release opt-level and LTO; (d) try the two spec-import options of §4.5.
  Fallback for (a): a build step running `verus --compile` or emitting erased
  Rust.
- **Spec S0**: repair `$VERUS_PIN` (0.3); create `mako-spec`; run the
  full-crate command at `d7e04ed7` → record the v0 count; write
  `verify_spec.sh` and `SPEC_VERSION.toml`; tag `raft-spec-v0`.
- **Spec S1** (§4.2): commit the new text, rlimit bump and doc row on
  `mako-spec`; full-crate run under `$VERUS_PIN`, log kept under
  `$RESULTS/spec/`; export patches; tag `raft-spec-v1`; report (0.7 point 4).
- **Concurrency check** (1.25 h, harness only): rerun the A/A rotation for G1
  and G2 (benchmarks pinned to cores 0-31) while this load runs:
  `while :; do taskset -c 32-63 nice -n 19 cmake --build /home/users/zyang2/mako-verus/build_scratch --clean-first -j 32; done`
  (`build_scratch` is a throwaway rust-lane configure, 0.4). If
  `paired_cv.py`'s CV_paired stays within 1.2× the quiet A/A value for every
  metric, builds, Tier 1 and Verus (with `--num-threads 16`) may run during
  benchmarks pinned to cores 32-63, and benchmarks are pinned to 0-31;
  otherwise the quiet-machine rule stands.

**Gate.** Tier 1 (all three lanes). Comparator calibration and validation pass
(A.4 item 1). A/A baselines: for each G1-G6,
`scripts/verus/gate_point.sh p0 G<k> build_rust_base build_rust`, then
`$PY scripts/verus/paired_cv.py $RESULTS/p0/G<k> 25 build_rust_base build_rust`,
which reads `r<i>.<arm>.json`, computes per metric the per-round ratios
B/A − 1, **CV_paired = stdev of those ratios** (already relative, since a ratio
is unitless), the MDE `2.8 × CV_paired / √n`, and the derived round count and
bound (§6), and writes the point's row into `docs/verus/gate-params.md`
(`paired_stats.py --json` gives only `median_ratio, pos, neg, sign_p, better,
n, median_a, median_b`, `scripts/raft_perf/paired_stats.py:66-79`, so it
cannot supply these). G7 A/A baseline and its bound (§6). One full sweep
(under `$RESULTS/p0/sweep/child`) and one Jetpack run of `build_rust` as the
Tier 3 baseline. `scripts/verus/verify_spec.sh` exits 0 at v1. Then stop and
report (0.7 point 1).

### Phase 1. One `RaftCore` struct; F1, F3-F5 (0.8-1.2 days)
**Goal.** Every field that survives between segments lives in one struct alpha
can read; the code-side spec mismatches are gone.

**Changes:**
- `pub struct RaftCore` in `src/server_h.rs` holding §3.1's fields; move the
  heartbeat round state out of `HeartbeatRoundState` (`src/server_cc.rs:1805-1831`).
  `RaftServerBase` holds `core: RaftCore` in place of `state_` (M1/M2, about
  413 mechanical lines).
- Pure functions take `&mut RaftCore`/`&RaftCore` with unchanged bodies:
  `raft_commit_advance` (`src/server_cc.rs:633`), `heartbeat_apply_append_reply`
  (`:1397`), `heartbeat_phase0_locked` (`:710`), `heartbeat_phase3_locked`
  (`:1725`), the ~40 predicates at `src/server_h.rs:101-445`.
- `raft_on_request_vote` (`src/server_h.rs:4974`) and `raft_on_append_entries`
  (`:5214`) are not pure (they reach `stepDown`/`setIsLeader`, FFI, clocks,
  `EnqueueCommittedEntries`) and keep `&mut RaftServerBase` until Phase 3.
- F1, F3, F4, F5 (A.2; F2 is not done, V3 replaces it). M9: `rusty::BTreeSet`
  in HeartbeatAuthority → sorted `Vec<u16>` (`src/server_cc.rs:130-250`).
- New lab cases: duplicated vote reply, stale-term vote reply, vote request to
  a server whose `rpc_ready_` is false (pins today's behaviour: the candidate
  counts a "no" and gives up on a "no" quorum without waiting for its
  deadline), term-0 entry.

**Gate.** Tier 1; A.4 item 2; Tier 2 points G1, G3, G5 (+G7, because the vote
tally changes with F1).

### Phase 2. Side effects become returned actions (1.0-1.4 days)
**Goal.** Core functions make no FFI calls, take no locks, read no clocks, fire
no callbacks.

| Today | Becomes |
|---|---|
| `EnqueueCommittedEntries` (`src/server_h.rs:3837-3905`) | `ApplyRange{from, to, handles}` (M3) |
| `setIsLeader`/`stepDown` (`:2281-2406`, `:4175-4194`) | pure `become_leader()`/`become_follower(hint)` returning `LeaderChanged`, `AppendNoop`, `ResetElection`, `WakeReplication`; FFI `raft_log_set_is_leader_entry` (`:2284`) and the callback move to the shell (F6) |
| `resetTimerLocked` (`:2251-2271`), `GetElectionTimeout` (`:2223-2245`) | `now`, `timeout_sample` event parameters (M4) |
| `AppendLeaderNoop` (`:4224`) | shell builds the handle; core appends |
| `RequestReplication` (`:3607`) | `WakeReplication` |
| per-entry `raft_command_has_value`/`payload_bytes`/`is_tpc_commit`/`kind` | `RaftEntry` fields (M6) |
| `raft_verify` (22 sites), explicit panics (7) | `assert!` (M10) |

Plus a test that the leader-change callback fires exactly once per transition.
Dry run: lift `raft_commit_advance` and `heartbeat_apply_append_reply` into the
spike crate.

**Gate.** Tier 1; A.4 item 2; Tier 2 G1-G6 + G7. Watch G2 and G6 for the larger
`RaftEntry`.

### Phase 3. Cut the fibers (2.5-3.6 days; milestone 1)
**Goal.** No core call contains a sleep or wait; timing identical.

**Changes (M5):**
- **Heartbeat** (`src/server_cc.rs:798-1791`, ~1,000 lines): phases 0-1 →
  `tick_heartbeat`; each completed slot → `on_append_resp(ord, Option<AppendReply>)`
  (step-down folded in, today `:1591`); phase 3 → `round_end`. The shell keeps
  the 1 ms loop (`:1505`), the deadline (`:1508-1517`, checked `:1664-1667`) and
  the early-quorum exit (`:1658-1662`). Same follower order and
  `pending_rpcs.place` position. F7.
- **Election:** `RequestVoteImpl` section 1 (`src/server_h.rs:3947-4005`) →
  `start_election`; replies → `on_vote_resp(from, term, granted)` (raises
  `decided`); section 2 (`:4049-4170`) → `election_settle`, the only place that
  becomes leader. The shell keeps `raft_broadcast_vote_and_wait`'s 200 µs / 1 s
  loop (`rt/src/seam.rs:264-301`), polling `core.decided()`.
- **Inbound:** `ServeVote`/`ServeAppendEntries` call `core.on_request_vote` /
  `core.on_append_entries` (§3.2); the payload enters as `WireBatch`.
- `ElectionTimerLoop` (`src/server_h.rs:4880-4911`) calls `core.tick_election`.
- Replay recorder (A.4 item 3).
- Write `docs/verus/coupling-table.md`, the path-by-path analogue of
  glr/`docs/ghost-log/raftrs/coupling.md` §3: the work list for Phase 8. It
  must settle V3's open point (does the vote "no" count reach a marked
  field?); if it does, stop at 0.7 point 7 before F2.
- New lab cases: reply landing during `round_end`; vote reply after the
  deadline.

**Gate.** Tier 1; A.4 item 3; Tier 3 milestone (includes G1-G6 three-arm) + G7.

### Phase 4. Enforced serialization and mirrors (0.6-1.0 days)
**Goal.** The serialization guarantee (Q1) holds by construction.

**Changes:**
- `with_core` is the only accessor; delete direct field access (the compiler
  finds every site; 96 `mtx_` lines in `server_h.rs`).
- F8: mirrors `commit_index`, `is_leader`, `leader_hint`, `term` published after
  each call; `CommitIndex()` (called from `raft_main_helper.cc:989`) and
  `IsLeader`/`GetLeaderHint` (`src/server_h.rs:4600-4609`; app threads at
  `raft_main_helper.cc:1166-1167`) read them without `mtx_`.
- `PublishAppliedIndex` (`src/server_h.rs:2089`) → `with_core(|c| c.on_applied(n))`,
  still once per entry; `appliedIndexForWait_` stays a mirror (readers:
  `:2076`, `:2716`, `:2815`, `:3036`, `:3387`).
- `SetPreferredLeader` (`raft_main_helper.cc:889`, `:918`, `:1300`) writes a
  shell atomic (only the timeout choice reads it).
- Under the gate, `CompactLog` (`src/server_h.rs:2124`) and
  `MaybeCreateSnapshot` (`:3718`) do nothing.

**Gate.** Tier 1 run **twice**, counting first-attempt failures (a threading
bug hides behind the retry, `ci/ci.sh:519-540`); a TSan lab build if the
toolchain allows (**not verified**); A.4 item 4; Tier 2 G1-G6.

### Phase 5 (optional; user's call). Reply echo, F10 (0.4-0.75 day)
Not needed for the proof (§1.3). `AppendReply` (`rt/src/rpc.rs`) gains
`(follower, sent_term, sent_end)` (~24 bytes); a mismatch is refused. The
hybrid lane either updates both codecs or keeps the old layout; cpp lane
untouched. Snapshot reply echo only once snapshots are in scope. Whether srpc
can fire a reply callback twice or cross-wire replies is **not verified**; that
decides whether this is worth doing.

**Gate.** Tier 1 (+ `raftLabTestHybrid` if its codec changes); round-trip
property test; A.4 item 4 (shared projection); Tier 2 G2, G4 + Jetpack
loopback N=500.

### Phase 6. Separate core crate in the Verus subset (2.4-3.8 days; milestone 2)
**Goal.** The core compiles in the Verus subset with ghost code erased. It is
the verified code itself: no separate port.

**Changes:**
- `src/deptran/raft/core/` (log, election, replication, commit) exposing
  `step(&mut self, ev: Event, out: &mut Output)` and `new_gated(cfg) -> Result`;
  `server_h.rs`/`server_cc.rs` keep only the shell.
- Labelled rewrites only (A.3): M7, M8, M9, terms as a single u64
  (`src/server_h.rs:549-557`), M11, no SipHash, no allocating `Default`.
- `RaftLog` keeps its blocked layout (`src/server_h.rs:581-725`, chosen against
  reallocation stalls, `:636-639`) behind a `view(): Seq<Term>` spec.
- F9 (`step_checked`, integer checks only).
- Cpp and hybrid lanes: **removed** (Q9), the phase's first commit. The core
  crate is written for rustc and Verus only; B13-class transpiler limits no
  longer apply to it.
- Prove panic freedom (A5) for the ~29 `assert!`s (peer-table size, missing
  entry, tail mismatch, vote-handler panics). Use `verus/commit_rule.rs` as the
  skeleton for the commit rule.
- Turn on the ledger lint (A.3).

**Gate.** Tier 1; A.4 item 4 (byte-identical replay); `$VERUS_NEW` (or
`$VERUS_PIN`) over `core/` with 0 errors; Tier 3 milestone. Risk: rewrites
forced by the subset cost raft-rs 17-24% until fixed
(glr/`docs/ghost-log/raftrs/port-audit.md` §9.1-9.2); measure each on G2.

### Phase 8. Coupling and the proof (2.5-4.8 days)
**Goal.** A per-node certificate against spec v1, then the N-node
`theorem_compose_safety` (glr/`src/protocol/Raft/ghost_log_compose.rs:901-915`).

**Changes.** Ghost code and proof files only (M12), enforced by the diff-2
lint.
- `core/coupling.rs`: `state_view` (term; role with Candidate =
  `!is_leader && election_in_progress && election_term == current_term`; vote;
  log; commit; `votes_granted`; `match_index`, 0 = absent; `next_index` shadow
  as raft-rs V3; unmarked: leader hint, ledger, rounds, backoff); `msg_view`
  (BR1/BR2; B17; **V1**); the invariant conjuncts as in
  glr/`docs/ghost-log/raftrs/coupling.md:16-17`.
- Lemmas for **V1**, **V2** and **V3** (§4.3).
- Per handler: `open_recv`/`open_tick`, `ghost_set` + lemma per tracked write,
  `ghost_send` per message, `close_seg(label)`
  (glr/`src/ports/raftrs/coupling.rs:2205-2431`).
- **Decided (read in code): the leader updates `match_index` by `max()`.**
  `accept_through` raises `match_` only if `acknowledged_through > match_`, and
  `next_` only moves forward (`src/server_h.rs:788-798`). The acknowledged
  value is `min(reported, sent_end, leader_last)` (`src/server_h.rs:276-288`,
  applied at `src/server_cc.rs:1474-1482`); a reply with
  `reported < sent_end` is dropped as `CONTRADICTORY` (`src/server_cc.rs:1468-1469`), a genuine
  follower reports exactly `accepted_through = prev + count = sent_end`, and
  `leader_last` cannot fall below `sent_end` within a term, so ack equals the
  reply's own match. `LNextAtomic` binds `nmi == rmi` (glr/`raft_refinement.rs:78-81`)
  and `LHandleAppendResponse` assigns it (glr/`raft.rs:408-409`). Hence: a
  reply whose ack does **not** exceed the current match (every match-0
  heartbeat reply included) writes nothing marked and is a **stutter**; a
  larger ack closes `LHandleAppendResponse` with `nmi = ack`.
- Instantiate composition with our `msg_view` and **our** host contract, written
  in `docs/verus/host-contract.md`: §1.3's "RPC correct"; serialized
  run-to-completion calls (Q1), which also provide composition's "segments are
  atomic and globally ordered" (glr/`docs/ghost-log/raftrs/composition.md:117-119`,
  §1.3 item 3a); the shell never reads core state outside a call and encodes
  exactly the handles and terms in `Output`; the shell hands vote refusals at
  or below the campaign term to the core only as Tick-opened input (V3); no
  in-place restart (§4.4.2); gates (snapshots off, `failover_`, static config).
- `scripts/verus/verify_core.sh`: `$VERUS_PIN` over the core plus spec v1 (via
  the spike (d) choice); run on every change to `core/`. Their crate took 291 s
  and 8 GB (glr/`docs/ghost-log/raftrs/port-audit.md:928`).

**Certificate** (under the gates): at most one leader per term; log matching;
committed entries never lost or changed. Liveness not proved.

**Size.** Extrapolated, **not measured**: for a 2-3k-line core, about 1.5-2k
proof lines plus a 2-3k-line coupling layer (raft-rs: 3,357 coupling + 3,319
ghost lines for 7,265 lines of code, port-audit.md §10).

**Gate.** `verify_core.sh` 0 errors; `verify_spec.sh` exits 0; ledger lint in
ghost-only mode; A.4 item 4 (instrumented core replays as Phase 6); Tier 1;
Tier 3 milestone (cumulative; expected within noise, the group saw ±4%,
port-audit.md §9.5).

### Phase 7 (optional, performance only). Wakes, not polls (3.4-4.7 days; +1-1.9 for ownership)
**Goal.** Win latency: about 2.3 of the 2.6 ms at 4 KB is three ~1 ms waits
(`docs/performance/raft-latency-breakdown/`, since removed). Not required for the
proof (Q1). Do it after Phase 8, or skip it. Each sub-change is landed and
gated on its own; the core must not change (A.4 item 5).
- **F11a replies as events.** The srpc callback (poll thread,
  `rt/src/seam.rs:25-29`) calls `on_append_resp` directly; `RoundEnd` at quorum
  or deadline; vote replies settle elections immediately. The callback must
  capture a server reference valid through round end and teardown, stay `Send`,
  and take `mtx_` once per reply. F3 is what keeps A9 true here.
- **F11b eventfd wake**: done upstream. Since the Lion merge (2026-10-09)
  `PollThread::add` wakes the poll thread's driver on its pending edge
  (`src/srpc/reactor/reactor.rs:3082-3087`, `:1008-1024`), which Lion signals
  through its eventfd, and the epoll wait blocks until a wake or the next
  deadline (`src/srpc/reactor/epoll_wrapper.rs:146-169`) instead of the fixed
  1 ms, so the 0-1 ms submit→poll hop is gone without a Raft change.
- **F11c blocking apply channel** replacing `raft_thread_sleep_ms(1)`
  (`src/server_h.rs:3372`), draining up to K entries then one `Applied(n)`.
  Review the readers of `appliedIndexForWait_`/`execute_index_` (Phase 4 list
  and the committed-conflict check `:5413-5418`) for the delay; add a lab case
  that waits on the applied index.
- **F11d poll-thread ownership** (only if a-c leave a measured win): `Propose`
  and `Applied(n)` become jobs; delete `mtx_` and `ReplicationWakeGate`.
  Propose's result (**user decision**): (a) pre-check the `is_leader` mirror,
  enqueue, count late rejections out of `n_tot` asynchronously; or (b) block on
  an eventfd completion (one hop per entry). Batching: the submit loop hands its
  drained batch (`raft_worker.cc:1221-1233`) to one job; the no-submit-thread
  path (`raft_main_helper.cc:643-647`) sends a batch of one.

**Gate.** Per sub-change: Tier 1 (+ `simplePaxos shard1Replication
shard2Replication shardNoReplication srpcTests` for F11b); A.4 item 5; Tier 2
G1-G6 (+G7 for F11a); Tier 3 milestone after the last. Expected: G1 improves
(inferred, up to about −2 ms p50).

### Phase 9 (not scheduled). Remove the gates
- **Snapshots**: spec v2 (§4.4.1) on `mako-spec`, then events `SnapshotCreated`,
  `Compact`, `RecvInstallSnapshot` (decide → trusted install → `InstallDone`);
  `rt/src/snapshot.rs` (628 lines) and `src/lab_snapshot_cases.rs` (1,397 lines)
  come into scope. Gate with the snapshot perf points (§6 Tier 3). About 1.5-2.5
  agent-weeks.
- **Restart**: spec v3 (§4.4.2), only once Mako persists term, vote and log
  (a separate project with its own perf study).
- **Reads (A8)** once a read API exists. **Pipelining** (`max_inflight > 1` is
  inside the method's gate, glr/`docs/ghost-log/raftrs/port-audit.md:134`).

### Schedule

**Superseded in part by 0.10** (less testing): the quiet-machine hours
below assume the original gates.

An AI agent executes this plan. Its time goes mostly to waiting on this machine
(zoo-003, 64 cores): builds, suites, benchmarks, Verus. **Unit: 1 d = 24 h of
continuous agent wall clock.** Every number below is derived from the per-phase
table, so the table is the source and the headline is its sum.

- Critical path (Phases 0 → 1 → 2 → 3 → 4 → 6 → 8), every step run serially
  (agent + machine + quiet): **about 213-289 h** (the "Serial" column), of
  which **about 86 h** are benchmarks needing a quiet machine.
- Plus a 30-50% allowance for gate failures and rework (applied to each
  phase's serial hours): **11.4-18.1 d** (the "With rework" column sums to
  this).
- Minus overlaps (below; mainly the agent drafting the next phase while a
  quiet-machine run goes, estimated at 40-60 h saved, **not verified**):
  **about 9.5-15.5 d of continuous agent time**, about 2-3 calendar weeks with
  the stops of 0.7. Optional Phases 5 and 7 add 0.4-0.75 d and 3.4-6.6 d.
- (The earlier 15-22 person-weeks assumed a person typing; it is withdrawn.)

**Machine time per step** (stated or from prior logs, not re-measured; Phase 0
re-measures):

| Step | Wall clock |
|---|---|
| Fresh full build, one lane / incremental | 10-30 min / 2-10 min (a `src/*.rs` change rebuilds all three lanes while they are kept) |
| Tier 1, rust lane (lab ~10 min from `~/raft-test-results/correctness/lab-and-suites` timestamps + 4 suites, **not measured**) | ~30-45 min; +20 min for hybrid and cpp labs; a retry adds ~15 min |
| One full lab run (A.4 comparator) | ~10 min; Phases 1-2 need 6 (≈1 h), Phase 0 calibration + validation 10 (≈1.7 h) |
| Tier 2 point, 25 rounds, 2 arms (~45 s per arm per round) | ~38 min; G1-G6 ≈ 3.75 h |
| G7 (20 leader kills per arm) | ~1 h (**not verified**) |
| Full sweep, one arm (`scripts/raft_perf/run_sweep.sh --dry-run` at HEAD: `TOTAL 624 runs ~4h51m`) | ~4.85 h |
| Tier 3 milestone: **two** sweeps, child and parent (2 × 4.85 h) + Jetpack (loopback 3 rounds ~1.2 h from `2026-09-29.*.log` mtimes; WAN 5 rounds ~2-3 h, **not measured**) + G1-G6 three-arm ~5.6 h (doubles as that phase's Tier 2) | **~20 h** |
| Full-crate Verus (group's crate) | ~5-6 min, 8 GB |

**Per phase** (hours; quiet = needs the quiet machine; "With rework" =
Serial × 1.3-1.5 ÷ 24):

| Phase | Agent (editing, debugging) | Machine, not quiet | Quiet | Serial | With rework | Gate |
|---|---|---|---|---|---|---|
| 0 | 10-14 h | 6-8 h (about 10 builds, Tier 1 three lanes, 10 lab runs, S0/S1 Verus, `$VERUS_PIN` repair) | 14 h (A/A 3.75, G7 1, concurrency 1.25, sweep 4.85, Jetpack one arm ~3) | 30-36 h | 1.6-2.3 d | baselines, v1 frozen |
| 1 | 8-12 h | 3-3.5 h (builds, Tier 1, 6 lab runs) | 2.9 h (G1/G3/G5 1.9, G7 1) | 14-18.5 h | 0.8-1.2 d | G1/G3/G5 + G7 |
| 2 | 10-14 h | 4 h | 4.75 h (G1-G6, G7) | 18.75-22.75 h | 1.0-1.4 d | G1-G6 + G7 |
| 3 | 20-30 h | 6 h (incl. recorder runs) | 21 h (milestone 20, G7 1) | 47-57 h | 2.5-3.6 d | milestone 1 + G7 |
| 4 | 6-10 h | 2-3 h (Tier 1 twice) | 3.75 h | 11.75-16.75 h | 0.6-1.0 d | G1-G6 |
| 6 | 20-36 h (+12-48 h if rusty-cpp cannot take the crate) | 5 h + Verus | 20 h | 45-61 h | 2.4-3.8 d | milestone 2 |
| 8 | 25-55 h (proof iteration, Verus runs inside) | 1.5 h (Tier 1, replay) | 20 h | 46.5-76.5 h | 2.5-4.8 d | Verus + milestone |
| **Critical path** | | | **≈86 h** | **≈213-289 h** | **11.4-18.1 d** | |
| 5 (opt.) | 4-8 h | 1 h | ~3 h | 8-12 h | 0.4-0.75 d | G2/G4/Jetpack loopback |
| 7 (opt.) | 8-12 h per sub-change ×3 (ownership +16-24) | ~2 h each | 4.5 h each + 20 h milestone | 63.5-75.5 h (+22.5-30.5) | 3.4-4.7 d (+1-1.9) | per sub-change |
| 9 | sized after Phase 8 | — | — | — | — | — |

Proof pace: the only record is the group's statement that all commits of the
raft-rs track for A0-A7 fall within 8 working days (2026-09-06 to 09-09 and
09-22 to 09-23, 63 commits), by one researcher with an AI assistant, and that
"there is no finer-grained time record"
(glr/`docs/ghost-log/raftrs/port-audit.md:922-924`). Per-item durations are
**not verified**; if wanted, derive them from commit timestamps
(`git -C $GLR log --date=iso --format='%ad %s' -- src/ports/raftrs`) and say so.
Phase 8's agent range (25-55 h) is a judgement against that 8-day bound, not a
calibration.

**Overlaps.**
1. During any quiet-machine run the agent only edits, reads and writes (Phase 3
   coupling table, Phase 8 `state_view`/`msg_view` drafts, next-phase diffs not
   yet built).
2. If Phase 0's concurrency check passes, builds, Tier 1 and capped Verus run
   on the other half of the cores during benchmarks, removing most quiet hours
   from the critical path.
3. Milestones run overnight while the agent drafts the next phase.
4. Phase 8 drafting starts once Phase 3's coupling table exists; only its Verus
   runs wait for Phase 6.

**Ways to shorten** (user's choice): freeze the hybrid and cpp lanes (saves ~20
min per Tier 1, one or two lane builds per iteration, and the rusty-cpp branch of
Phase 6); use Phase 0's derived round counts (points whose paired MDE clears
the bound at 10-15 rounds cut their gate by 40-60%); drop the parent sweep at
milestones and compare the child sweep with the previous milestone's recorded
child sweep (saves ~4.85 h per milestone, ~14.5 h on the critical path, at the
cost of comparing across time rather than fresh pairs).

---

## 6. Performance contract

**Arms.** Every comparison runs the parent commit's Rust-lane build
(`build_rust_pre`) against the child (`build_rust`), measured fresh. At
milestones (Phases 3, 6, 8, and 7 if done) a third arm, `build_rust_base`
(tag `verus-p0`, merged with the current mako-dev when upstream touched the
binary, 0.2/0.8), gates the **cumulative** change with the same bounds, so
in-bound losses cannot add up (nine phases at +2% each could otherwise drift
15-20%). Old C++ numbers are not the baseline. Reference Rust-lane figures
(`docs/performance/raft-rust-9a361eccd/`, since removed): 4 KB p1 at
240/s p50 2.647 ms; 4 KB p1 unthrottled 37,760/s; 286 KB p6 multi at 190/s p50
3.343 ms, unthrottled 3,132/s; 1 MiB p1 183/s.

**Tier 1 (every commit).** The suites in the command block below; Paxos suites
and `srpcTests` too for any srpc change; first-attempt failures counted (the
Raft suites retry once, `ci/ci.sh:519-540`); TEST 56 ("CreateSnapshot and
compaction", `src/lab_snapshot_cases.rs:303-306`) is intermittently flaky on
hybrid: rerun once, two failures is a failure. Every perf run must show
`out_of_order`, `gaps`, `duplicates` and `foreign_applied` all 0. Enforced:
`raft_bench.sh` exits 8 with `log integrity violation` in its log when any is
non-zero (`examples/raft_bench.sh:638-645`); `rotation_trial.sh` then deletes
that round's JSON but keeps `r<i>.<arm>.json.log`
(`scripts/raft_perf/rotation_trial.sh:40`), so the violation would otherwise
look like one missing pair. `gate_point.sh` greps those logs and fails the
point (below).

**Tier 2 points (every phase).** Parameters follow `run_sweep.sh` (`:94-102`,
`:174`); every point sets them explicitly because `rotation_trial.sh`'s
defaults (`MAXOUT=4096`, `DUR=8`, `:23-24`) could OOM at large payloads
(`submit_queue_` copies every payload: 4096 × 286 KB ≈ 1.2 GB per partition).

| Point | PAYLOAD | RATE | MAXOUT | DUR | PARTS | GROUP | Measures | Bound |
|---|---|---|---|---|---|---|---|---|
| G1 | 4096 | 240 | 4096 | 10 | 1 | single | low-load latency, wake path | p50 ≤ +2%, p99 ≤ +5% |
| G2 | 4096 | 0 | 4096 | 10 | 1 | single | per-message CPU | thr ≥ −2% |
| G3 | 286208 | 190 | 256 | 10 | 6 | multi | production shape | p50 ≤ +2%, p99 ≤ +5% |
| G4 | 286208 | 0 | 256 | 10 | 6 | multi | production capacity | thr ≥ −2% |
| G5 | 1048576 | 55 | 64 | 10 | 1 | single | per-byte path | p50 ≤ +2%, p99 ≤ +5% |
| G6 | 1048576 | 0 | 64 | 10 | 1 | single | large-batch round | thr ≥ −2% |
| G7 | 4096 | 240 | 4096 | 15 | 1 | single | election/failover time (leader kill at 5 s, 20 kills per arm) | median and p90 of leader-loss → new leader and → first commit; rule below |

G7 runs on Phases 1, 2, 3 and 7 (F11a), which change the election path.

**G7 rule.** `election_times.py` reads, from each `<i>.<arm>.json`, the fields
`leader_loss_us`, `new_leader_us`, `first_commit_us` added by the Phase 0
harness, and computes two durations per kill: `new_leader_us − leader_loss_us`
and `first_commit_us − leader_loss_us`. A run missing a field counts as a
failed kill; more than 2 failed kills per arm fails the gate. **Bound**, set
once from the Phase 0 A/A G7 run and written to `docs/verus/gate-params.md`:
per duration and statistic (median, p90), `max(10%, MDE)`, where MDE is the
spread between the two A/A arms' statistic (|A − B| / A) observed at n = 20.
**Fail** if, for either duration, the child's median or p90 exceeds the
parent's by more than the bound **and** a one-sided Mann-Whitney U test
(child slower) gives p < 0.05. Exit 1 on fail.

**Deriving rounds and bounds (Phase 0).** Per point, from the A/A run, with
`scripts/verus/paired_cv.py` (Phase 0 changes): CV_paired = standard deviation
of the per-round ratios B/A − 1; paired MDE = `2.8 × CV_paired / √n`; the point's round count is the smallest n with
MDE below the bound, at most 25; if 25 rounds cannot get there, the bound is
widened to the 25-round MDE and the widening is written next to the point in
`docs/verus/gate-params.md`. (Today's unpaired 4 KB unthrottled CV is about 8%,
an MDE of ~4.5% at 25 rounds, `compare-vs-412c225a.txt:219`; paired spreads
should be smaller, **not verified** by how much.)

**Pass rule.** A metric fails only if **both** (1) the median paired ratio
B/A − 1 is past its bound in the bad direction, and (2) the two-sided sign test
is significant (p < 0.05) in the bad direction. A smaller consistent shift is
reported, not failed (the sign test alone flags any shift: p = 0.000 for −1.57%,
`docs/performance/raft-rust-t4-paired/`, since removed). A shift past a bound in
the good direction is reported as "improved". Never gate on p1 286 KB p99
(bimodal, `raft-baseline.md` Known gap 6, since removed) or on throughput at
throttled points. `paired_stats.py` exits 1 when fewer than `--min-pairs`
rounds completed (`paired_stats.py:6-10`); that is a failed run.

**Tier 3 (milestones).** Full sweep compared with `compare.py` (5% plus the
MDE; exit 1 = regression) against both the parent's and the Phase 0 records;
G1-G6 three-arm against `build_rust_base`; Jetpack at `WAN_DELAY_MS=20` (5
rounds) and loopback (3 rounds): throughput and p50 within ±2% at every N, p90
within ±5%, knee stays at N=150 (WAN) and N=500 (loopback), p99 at N ≤ 10
reported but exempt. `jetpack_compare.py` only prints a report: it has no
failing exit path and its `--baseline` defaults to an arm called `baseline`
(`scripts/raft_perf/jetpack/jetpack_compare.py:132-134`). So Phase 0 writes
`scripts/verus/jetpack_gate.py ROOT --baseline ARM --candidate ARM`, which reads
the same per-round files `jetpack_compare.py` reads (`<setting>/round*`),
applies the ±2% / ±5% / knee rules above, prints one row per setting and N,
and exits 1 on any violation. Snapshot points (`stall_*`, `s286k_r190_p6`, with
`MAKO_RAFT_SNAPSHOTS=1`) whenever snapshot or compaction code moves.

### Gate commands

Run from `/home/users/zyang2/mako-verus` with `~/mako-verus-env.sh` sourced;
`P` is the phase label (e.g. `p3`).

**Tier 1:**

```bash
P=p1; mkdir -p $RESULTS/$P/tier1; rc=0
for t in raftLabTest shard1ReplicationRaft shard2ReplicationRaft shard1ReplicationSimpleRaft shard2ReplicationSimpleRaft; do
  BUILD_DIR=build_rust ./ci/ci.sh $t > $RESULTS/$P/tier1/$t.log 2>&1 || { echo "FAIL $t"; rc=1; }; done
grep -c '^Retrying' $RESULTS/$P/tier1/*.log    # first-attempt failures: report every non-zero
# srpc touched: also simplePaxos shard1Replication shard2Replication shardNoReplication srpcTests
echo "tier1 rc=$rc"
```

**Tier 2, one point** (`scripts/verus/gate_point.sh`, written in Phase 0;
this is its whole body). Rounds and bounds come from
`docs/verus/gate-params.md`, one row per point,
`| G1 | <rounds> | <metric=bound,...> | <note> |`; Phase 0 seeds every row with
25 rounds and the default bounds of the table above, then overwrites them from
the A/A run.

```bash
#!/usr/bin/env bash
# gate_point.sh PHASE POINT PARENT CHILD [BASE]
# Runs one G point (rotating all given arms), then applies the pass rule
# PARENT vs CHILD and, if BASE is given, BASE vs CHILD (cumulative gate).
set -euo pipefail
P=$1 G=$2 A=$3 C=$4 BASE=${5:-}
case $G in
  G1) export PAYLOAD=4096    RATE=240 MAXOUT=4096 PARTS=1 GROUP=single;;
  G2) export PAYLOAD=4096    RATE=0   MAXOUT=4096 PARTS=1 GROUP=single;;
  G3) export PAYLOAD=286208  RATE=190 MAXOUT=256  PARTS=6 GROUP=multi;;
  G4) export PAYLOAD=286208  RATE=0   MAXOUT=256  PARTS=6 GROUP=multi;;
  G5) export PAYLOAD=1048576 RATE=55  MAXOUT=64   PARTS=1 GROUP=single;;
  G6) export PAYLOAD=1048576 RATE=0   MAXOUT=64   PARTS=1 GROUP=single;;
  *) echo "unknown point $G"; exit 2;;
esac
export DUR=10
row=$(grep -E "^\| $G \|" docs/verus/gate-params.md) || { echo "no row for $G in gate-params.md"; exit 2; }
N=$(echo "$row" | awk -F'|' '{gsub(/ /,"",$3); print $3}')
B=$(echo "$row" | awk -F'|' '{gsub(/ /,"",$4); print $4}')
mkdir -p "$RESULTS/$P"; OUT=$RESULTS/$P/$G; uptime > "$OUT.uptime"
scripts/raft_perf/rotation_trial.sh "$OUT" "$N" "$A" "$C" ${BASE:+"$BASE"}
if grep -l 'log integrity violation' "$OUT"/*.log 2>/dev/null; then echo "GATE FAIL $G: log integrity"; exit 1; fi
rc=0
python3 scripts/raft_perf/paired_stats.py "$OUT" "$N" "$A" "$C" --json > "$OUT/$A-vs-$C.json" || rc=1
python3 scripts/verus/perf_gate.py "$OUT/$A-vs-$C.json" --bounds "$B" || rc=1
if [ -n "$BASE" ]; then
  python3 scripts/raft_perf/paired_stats.py "$OUT" "$N" "$BASE" "$C" --json > "$OUT/$BASE-vs-$C.json" || rc=1
  python3 scripts/verus/perf_gate.py "$OUT/$BASE-vs-$C.json" --bounds "$B" || { echo "GATE FAIL $G cumulative"; rc=1; }
fi
exit $rc
```

`perf_gate.py` (Phase 0, about 30 lines): for each `metric=bound` in
`--bounds`, read `metrics[metric]` from the JSON; "bad" is
`median_ratio > bound` for a positive bound and `median_ratio < bound` for a
negative one; fail if bad **and** `sign_p < 0.05` **and** the majority
direction (`pos` vs `neg`) is the bad one; print a table row per metric
(median ratio, p, verdict); exit 1 on any failure or if the JSON lacks a
bounded metric.

Phase gates (labels are always `p<n>`, e.g. `P=p0`, `P=p3`):

```bash
# Tier 2
for g in G1 G2 G3 G4 G5 G6; do scripts/verus/gate_point.sh $P $g build_rust_pre build_rust || echo "GATE FAIL $g"; done
# Phase 1 only: for g in G1 G3 G5; ...
# Phase 0 A/A: for g in G1 G2 G3 G4 G5 G6; do scripts/verus/gate_point.sh p0 $g build_rust_base build_rust; done
# Milestone three-arm (replaces the Tier 2 loop): parent and cumulative gates in one rotation
for g in G1 G2 G3 G4 G5 G6; do scripts/verus/gate_point.sh $P $g build_rust_pre build_rust build_rust_base || echo "GATE FAIL $g"; done
```

**G7** (harness from Phase 0; command shape **not verified** until then):

```bash
for arm in build_rust_pre build_rust; do for i in $(seq 1 20); do
  examples/raft_bench.sh --build-dir $arm --out $RESULTS/$P/G7/$i.$arm.json --payload-bytes 4096 --rate 240 \
    --max-outstanding 4096 --duration-sec 15 --kill-leader-at-sec 5 > $RESULTS/$P/G7/$i.$arm.log 2>&1; done; done
$PY scripts/verus/election_times.py $RESULTS/$P/G7 build_rust_pre build_rust   # exit 1 = past the Phase 0 bound
```

**Tier 3 (milestone):**

```bash
BUILD_DIR=build_rust     scripts/raft_perf/run_sweep.sh --output $RESULTS/$P/sweep/child
BUILD_DIR=build_rust_pre scripts/raft_perf/run_sweep.sh --output $RESULTS/$P/sweep/parent
python3 scripts/raft_perf/compare.py $RESULTS/$P/sweep/parent $RESULTS/$P/sweep/child      # exit 1 = regression
python3 scripts/raft_perf/compare.py $RESULTS/p0/sweep/child  $RESULTS/$P/sweep/child      # cumulative vs Phase 0
PTS="rw_1000000:1 rw_1000000:10 rw_1000000:50 rw_1000000:100 rw_1000000:150 rw_1000000:200 rw_1000000:300 rw_1000000:500 rw_zipf_0.8:150 rw_zipf_1:150"
WAN_DELAY_MS=20 scripts/raft_perf/jetpack/run_jetpack_sweep.sh $RESULTS/$P/jetpack/wan20ms 5 "$PTS" base=build_rust_base pre=build_rust_pre rust=build_rust
WAN_DELAY_MS=0  scripts/raft_perf/jetpack/run_jetpack_sweep.sh $RESULTS/$P/jetpack/loopback 3 "$PTS" base=build_rust_base pre=build_rust_pre rust=build_rust
for set in wan20ms loopback; do for ref in pre base; do
  $PY scripts/raft_perf/jetpack/jetpack_compare.py $RESULTS/$P/jetpack/$set --baseline $ref > $RESULTS/$P/jetpack/$set/report-vs-$ref.txt
  $PY scripts/verus/jetpack_gate.py $RESULTS/$P/jetpack/$set --baseline $ref --candidate rust || echo "GATE FAIL jetpack $set vs $ref"; done; done
```

(`PTS` is the point list of the since-removed jetpack-comparison README.
Whether `run_sweep.sh` already writes per-arm subdirectories that `compare.py`
accepts as given is **not verified**; adjust paths to what it writes. Phase 0
records the sweep under `$RESULTS/p0/sweep/child`.)

**Verus:** `scripts/verus/verify_spec.sh` (§4.5) and, from Phase 8,
`scripts/verus/verify_core.sh`; each exits 1 on any error or count drop.

**Hot-path risks.**

| Risk | Mitigation | Gate |
|---|---|---|
| Payload bytes copied into the core | opaque handles; codec only in the shell | G5, G6 |
| Stamping moved out of the lock (F7) | same deep copy, from `Output` handles | G2, G4, G6 |
| Per-event output overhead | reused Vecs | G2, Jetpack loopback N=500 |
| Verus-forced rewrites | no hashing in the core; measure each rewrite | G2 |
| Larger `RaftEntry` (M6) | a few bytes; removes 3 FFI calls per entry per send | G2, G6 |
| Election slows down | 200 µs / 1 s vote loop and timeouts frozen through Phase 6 | G7 |
| Verified crate built with different opt-level/LTO | match Release (spike c) | all |
| Synchronous Propose through a poll-thread job (F11d only) | eventfd first; gate the scheme separately | G1, G2 |
| Ghost code | erased | G1-G6 |

---

## 7. Risks, open questions, non-goals

**Risks.**
- `verus!` cannot ship through cargo or rusty-cpp (spike a/b). Mitigation:
  erased-Rust generation, or freeze the cpp and hybrid lanes.
- `$VERUS_PIN` cannot be repaired offline. Mitigation: report; `$VERUS_NEW`
  verifies the S1 prototype, but the frozen manifest must name what was used.
- Building the batch after the lock would send entries the core did not log.
  Mitigation: `Output` carries the handles (§3.1 rule 1).
- In-place restart is a real safety gap outside the certificate (§4.4.2).
- The core's size is not measured; Phase 8 could exceed its range.
- The lab is nondeterministic; the comparator may need many exemptions.

**Open questions (not verified).**
1. Can srpc fire a reply callback twice or cross-wire replies? (Decides Phase 5.)
2. Is any Raft replica ever restarted in place under its old id, in production or
   CI? If yes, the v1 assumption fails for it; report to the user.
3. Does the leader-change callback re-enter Raft (`raft_worker.cc:311-330`)?
   (Phase 0.)
4. (Settled: the Rust-lane `vote` handler returns `Result<VoteResponse, i32>`,
   `rt/src/service.rs:108`, so F2 is expressible if ever needed.) Does the
   vote "no" count reach a marked field on any path (V3; Phase 3 coupling
   table)?
5. How large is the pure protocol subset of `server_h.rs`/`server_cc.rs`?
6. Does anything outside `src/deptran/raft/src` read the authority ledger? (A
   grep found nothing.)
7. Paired spread per G point, and so round counts (Phase 0).
8. Does S1 need the same rlimit under `$VERUS_PIN`? Does the spec import work
   by (i) or (ii) (§4.5)?
9. (Settled: `max()`; Phase 8, "Decided".)
10. Can Tier 1 suites of different lanes run concurrently? (Assumed not.)

**Non-goals.** Verifying srpc, the codec, the C++ shim, the apply callback or
RocksDB; liveness; snapshots, restart, membership or reads in certificate v1;
timer changes before Phase 7; changing the `add_log_to_nc`/`RaftWorker` API;
persistence. The cpp and hybrid lanes are gone from this worktree (Q9).

**User decisions still open** (stop and wait at the points listed in 0.7
point 7):
1. Phase 5 reply echo (any time; not needed for the proof).
2. (Decided: the cpp and hybrid lanes are removed, Q9.)
3. Propose's result scheme, only if F11d is done.
4. Offering S1 (and any later S-change) upstream to the group, and pushing
   `mako-spec` anywhere.
5. Any new F-item (A.2, "Adding to the list"), including falling back from V3
   to F2.
