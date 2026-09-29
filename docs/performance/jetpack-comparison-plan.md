# Plan: compare the Rust Raft and the C++ baseline Raft under Jetpack's test suite

Status: **approved 2026-09-29**, with one addition: the network delay is
recorded in the final results. The §8 decisions follow the plan's
defaults (see §8).

## 1. The intention, as I understand it

- **The goal is a comparison.** Mako's Raft on the Rust runtime (today's
  tree) against the original C++ Raft (the pre-conversion baseline,
  `412c225a`, 2026-09-12). The conclusion should read: "under Jetpack's
  test, the baseline does X and the Rust runtime does Y; they are about
  equal (or Rust is better), so the conversion kept performance."
- **Why Jetpack's suite:** our own benchmark (`raft_bench`, 25 paired
  rounds) already makes this comparison, but it is our format, and people
  may not trust it. Jetpack's suite and output format are the group's
  recognised yardstick, so the comparison is more convincing when
  Jetpack's code produces and processes the numbers.
- **So copy as much of Jetpack's real suite as possible**, not build a
  look-alike. Where something cannot be copied, say exactly what was
  changed and why.
- **Both arms go through the identical copied suite,** on the same machine
  with the same settings. Only the Raft underneath differs.

## 2. The two arms

| arm | source | build |
|---|---|---|
| **baseline**: C++ Raft | `412c225a` in the worktree `/home/users/zyang2/mako-baseline` (already created and building) | `build_base`: Release, `MODE=perf`, clang 22, libc++ |
| **rust**: Rust Raft on the Rust runtime | current branch head (`srpc-subtree-forward`) | `build_rust` (`MAKO_RAFT_LANE=rust`) |
| *optional:* hybrid (Rust core, C++ runtime) | current head | `build` |

The copied suite is applied identically to both trees. Only the Raft code
differs.

## 3. What gets copied, at two levels of fidelity

Jetpack's suite has two halves: **the front**, which generates load,
measures and reports; and **the request path**, which carries a client's
request into Raft. How much of the second half is copied decides how
literally this is "Jetpack's suite".

### Copied verbatim at both levels

These are unchanged files from Jetpack `c03e318e`, under MIT with the
notice kept:
- the analysis scripts, which read our output unchanged:
  `derive_fixed_conc.py` (the knee), `build_per_protocol_tables.py`,
  `merge_latency_csv.py`, `camera-ready/gen_tput_p90_figures.py`,
  `gen_latency_cdf.py`;
- the experiment configs: `concurrent_<N>.yml` (the concurrency levels),
  `client_open_raft.yml` (open loop, `max_undone` 300),
  `rw_1000000.yml` / `rw_zipf_<θ>.yml` (the workloads) and `YCSB_A.yml`
  (50/50);
- the output format: the `.res` lines (`All-efficient-attempts statistics
  …`, `Mid throughput is …`), the 11-column per-request CSV, and the file
  naming `<protocol>-<site>-<workload>-concurrent_<N>-<mode>-YCSB_A-<host>.res`.
- `Distribution` (the percentile and average math) and `ZipfDist` (the
  key distribution).

Script parameters (host count, site name) are set on the command line or by
a one-line constant. Every such edit is listed.

### Level 1: copy the client behaviour, submit straight into Raft (about 5 hours)

- **Copied, as logic:** Jetpack's client loop.
  - 60 client sites × N open-loop coroutines;
  - each waits U(0, 1/N) s, sends, then waits U(0.5, 1.5) s;
  - a site pauses at `max_undone`;
  - timing from send to reply, counting only requests sent in the middle
    third of a 30 s run.
- **Changed:** the coroutines run inside the leader process and hand each
  request directly to Raft (`add_log_to_nc`), instead of sending it over
  RPC through Jetpack's coordinator and scheduler.
- **Apply:** a read or write on an in-memory table on every replica. The
  "reply" is the leader's apply.
- **What it measures:** Jetpack's load pattern and Jetpack's statistics, on
  each Raft. It is *not* Jetpack's request path: the client-to-leader RPC
  and the scheduler layer are missing.
- A draft of this exists, uncommitted:
  `src/deptran/raft/raft_bench_jetpack.h`.

### Level 2: copy the request path too (about 3-4 weeks)

- **Copied:**
  - Jetpack's `ClientWorker`;
  - `CoordinatorNone`;
  - the `Dispatch` RPC;
  - `SchedulerNone` / `SchedulerClassic` (execute, then replicate, then
    apply);
  - the `rw` benchmark procedures;
  - the in-memory database.
- **Route:** restore Mako's own copies of these, deleted on 2026-08-23 (the
  same Janus lineage). Paste in Jetpack's later changes, and point the
  scheduler's Raft calls at each arm's Raft.
- **Measures:** Jetpack's request path end to end, on both arms. This is as
  close to "running Jetpack's suite" as possible without Jetpack's own
  Raft.
- **Cost:**
  - the restored code predates our srpc library and must be moved onto
    it, in **both** trees: the baseline predates the srpc switch too, so it
    needs its own adaptation;
  - `docs/performance/jetpack-copy-plan.md` has the step list.

### Never copied, at either level

- Jetpack's own Raft: it is the thing being replaced by each arm.
- Its fast path and recovery (not vanilla Raft).
- Its other protocols, Docker, AWS and multi-host scripts. We have one
  machine, and SSH to the other Zoo hosts is refused.

## 4. Network delay

- **Copied as behaviour:** Jetpack's Zoo runs use `WAN_DELAY_MS=20` (every
  outbound RPC waits 20 ms, so a round trip is 40 ms), not real WAN.
- **To implement:** the same opt-in delay in both arms' Raft transports:
  the baseline's C++ commo, and the Rust lane's transport. It must be an
  asynchronous timer on requests and replies, never a sleep on the poll
  thread.
- **Cost:** about 1-2 hours at Level 1.
- **Without it,** the runs are loopback, which is valid for the comparison
  but not comparable in absolute terms to Jetpack's published numbers.

## 5. Which configurations (representative subset of Jetpack's Exp 0/1)

| sweep | points |
|---|---|
| Exp 0, concurrency (workload `rw_1000000`, uniform, YCSB_A) | N = 1, 10, 50, 100, 150, 200, 300, 500: from low load through Jetpack's Raft knee (150) to overload |
| Exp 1, skew (at the knee) | Zipf θ = 0.8 and 1.0 |
| both | with `WAN_DELAY_MS=20` (if §4 is accepted), and once without |

- **Repetitions:** 5 rounds per point, arms rotated. Jetpack itself runs
  each point once; the extra rounds are ours. Each round's output is still
  a Jetpack-format `.res`/CSV, and the table reports the median round,
  plus the spread.
- **Machine time, Level 1:** 8 + 2 points × 2 arms × 5 rounds × ~40 s ≈
  15 minutes per delay setting.

## 6. The result

- For every run: the `.res` and CSV files in Jetpack's naming and layout,
  plus a small sidecar file recording the arm, commit, network delay
  (`WAN_DELAY_MS`, and the resulting round trip), concurrency, workload and
  round.
- **The network delay is recorded throughout:**
  - in each run's sidecar and in its `.res` (a `[jetpack] wan_delay_ms=…`
    line);
  - as a column in every table;
  - in every figure title;
  - in the conclusion (for example, "at 20 ms one-way / 40 ms RTT injected
    delay" vs "loopback, no injected delay").
- **Tables** from Jetpack's own scripts: throughput and p50/p90/p99 per
  concurrency, and the knee from `derive_fixed_conc.py`, baseline and
  Rust side by side.
- **Figures** from `gen_tput_p90_figures.py` (throughput vs p90) and
  `gen_latency_cdf.py`.
- **Short conclusion:**
  - the per-point difference, Rust vs baseline, with its spread;
  - "equivalent or better" if Rust is within ±2% on throughput and ±5% on
    p50/p99, or better;
  - Jetpack's published Raft numbers as context only, labelled as a
    different machine and harness.

## 7. Steps and checks

1. Build both arms. The baseline build is running now.
2. Copy Jetpack's scripts and configs into `scripts/raft_perf/jetpack/`,
   with a provenance note and a list of edits. **Check:** Jetpack's scripts
   parse a hand-made `.res`.
3. Add the Jetpack mode to both trees' benchmark (Level 1: the copied
   client, `.res`/CSV output, table apply), or restore the request path
   (Level 2).
4. **No-consensus check:** with Raft short-circuited, offered load must be
   60 × min(N, 300) requests/s. With the delay on, p50 must be about the
   injected round trip. This proves the copied client behaves like
   Jetpack's before any Raft number is trusted.
5. Add the delay to both transports (if §4 is accepted). **Check:** the
   no-consensus p50 is about 40 ms.
6. Run the §5 sweep on both arms. Produce the tables and figures with
   Jetpack's scripts, and write the conclusion in
   `docs/performance/`. The raw records go to `~/raft-test-results/`.

## 8. Decisions (resolved)

1. **Level 1 now; Level 2 later** if anyone questions the missing RPC layer.
2. **Network delay: yes.** `WAN_DELAY_MS=20` in both transports (40 ms
   RTT), plus one loopback sweep with no injected delay. The delay is
   recorded in every result (§6).
3. **Hybrid lane: included** as a third arm.
4. **Configurations:** as in §5.

## 9. State right now

- The baseline worktree `/home/users/zyang2/mako-baseline` exists at
  `412c225a`, configured, and `raft_bench` is compiling in the background.
  It only builds; nothing is changed. I can stop it if you prefer.
- `src/deptran/raft/raft_bench_jetpack.h` is a Level 1 draft. It is
  **uncommitted** and not yet wired into anything. It is kept or dropped
  per your decision.
- Nothing else has changed.
