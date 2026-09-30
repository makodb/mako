# Rust Raft vs the C++ baseline Raft, under Jetpack's test suite

Date: 2026-09-29/30, host zoo-003. Plan: [../jetpack-comparison-plan.md](../jetpack-comparison-plan.md).

## Conclusion

**Under Jetpack's test suite and output format, the Rust Raft preserves the
C++ Raft's performance, and on most points improves it.**

The network delay is recorded in every result below.

- **With 20 ms of injected delay** (`WAN_DELAY_MS=20`, a 20 ms round trip:
  requests are delayed, replies are not, as in Jetpack's Raft):
  - **Throughput:** equal (±1%) at every point the cluster can carry.
  - **Saturation capacity:** 1–3% higher for Rust. For example, at N=300,
    11.34k req/s on the Rust runtime against 11.11k for the baseline.
  - **Latency:** equal or lower.
  - **Knee:** N=150 for all three arms in all 5 rounds, the same knee
    Jetpack reports for its own Raft.
- **With no injected delay** (loopback):
  - **Throughput:** equal everywhere, up to 30k req/s. No arm saturates.
  - **Rust runtime latency:** 2–15% lower, with the gap growing with load
    (p50 2.83 vs 3.42 ms at N=500).
  - **Knee:** N=500 for all arms.
- **One cell misses the plan's ±5% bar:** 20 ms delay, N=10, p99. Both Rust
  arms are +5.5% there (42.7 vs 40.8 ms). The p50 at the same point is
  within 1%. That p99 comes from ~570 samples per host group, so it rests on
  about 6 tail samples each. At loopback the same N=10 p99 is 2.4% *lower*
  on the Rust runtime, and equal on the hybrid. We read it as noise, but it
  is reported as measured.

## The three arms

| arm | code | build |
|---|---|---|
| C++ baseline | `412c225a9` (2026-09-12, before the Rust conversion), plus the Jetpack hooks below (uncommitted in worktree `mako-baseline`) | `build_base` |
| Rust Raft, Rust runtime | `074ad88a9` (`srpc-subtree-forward`) | `build_rust` (`MAKO_RAFT_LANE=rust`) |
| Rust Raft, C++ runtime (hybrid) | `074ad88a9` | `build` (`MAKO_RAFT_LANE=hybrid`) |

All three builds are Release, `MODE=perf`, clang 22 with libc++. Every arm
ran with 3 replicas on one host, 1 partition, and 64-byte entries.

## What is Jetpack's, and what is not

- **Copied byte for byte** from Jetpack `c03e318e`: the analysis scripts
  (`derive_fixed_conc.py`, `gen_tput_p90_figures.py`, `gen_latency_cdf.py`
  and others) and the configs. They are in `scripts/raft_perf/jetpack/`; see
  its `PROVENANCE.md`. The scripts are run unedited. `jetpack_compare.py`
  only sets their protocol lists and axis limits, and renames files by
  symlink.
- **Reproduced from Jetpack's code** (`src/deptran/raft/raft_bench_jetpack.h`):
  - 60 client sites × N open-loop clients. Each waits U(0, 1/N) s, sends,
    then waits U(0.5, 1.5) s, and a site pauses at 300 outstanding requests.
  - YCSB_A: 50/50 reads and writes over 10^6 keys, uniform or Zipf.
  - Latency counts only requests dispatched in the middle 10 s of a 30 s
    run.
  - Jetpack's `Distribution` statistics and its `.res`/CSV output, one set
    per host group.
- **The network delay** (`netdelay.c`): Jetpack's WAN_DELAY_MS, applied as a
  link delay on outbound sockets through `LD_PRELOAD`. It is identical for
  all three arms and changes none of their code.
- **Not Jetpack's (Level 1 of the plan):**
  - The clients run inside the leader process and submit with
    `add_log_to_nc`, so there is no client-to-leader RPC and no Jetpack
    scheduler layer.
  - A request's reply is the leader's own apply of it.
  - Absolute numbers are therefore not comparable with Jetpack's published
    ones. The comparison between arms is the result.

## Method

- **Points:**
  - N = 1, 10, 50, 100, 150, 200, 300, 500 on `rw_1000000` (uniform);
  - Zipf θ = 0.8 and 1.0 at N=150.
- **Rounds:** 5 at 20 ms delay and 3 at loopback. The arms are rotated in
  every round, and 240 runs completed with none failed.
- **Throughput:** Jetpack's windowed throughput (commits in 15–25 s), from
  `gen_tput_p90_figures.aggregate`.
- **Latency:** the median over the 10 host groups of each `.res` percentile.
- **Ratios:** paired within a round (arm ÷ baseline), then the median over
  rounds.

## Key numbers (median over rounds)

### 20 ms injected delay

| N | offered r/s | throughput r/s: base / hybrid / rust | p50 ms: base / hybrid / rust | p99 ms: base / hybrid / rust |
|---|---|---|---|---|
| 1 | 40 | 40 / 40 / 40 | 34.5 / 31.0 / 30.8 | 47.3 / 46.2 / 45.9 |
| 10 | 581 | 573 / 573 / 572 | 28.5 / 28.7 / 28.6 | 40.8 / 43.2 / 42.7 |
| 150 (knee) | 9130 | 8977 / 8971 / 8973 | 27.5 / 27.3 / 27.1 | 36.9 / 36.3 / 36.6 |
| 200 | 12178 | 11776 / 11955 / 12083 | 808 / 591 / 488 | 891 / 617 / 510 |
| 300 | 11719–11956 | 11110 / 11187 / 11341 | 1617 / 1603 / 1581 | 1647 / 1634 / 1614 |
| 500 | 11676–11855 | 11085 / 11213 / 11238 | 1619 / 1605 / 1594 | 1647 / 1639 / 1624 |

- **N=200:** the offered 12.18k req/s is just above every arm's capacity.
  The baseline falls behind fastest, so a 2.6% capacity edge becomes 40%
  lower latency for Rust.
- **N≥300:** all three arms are saturated. Offered load drops below 60 × N
  because the 300-outstanding cap is reached, as in Jetpack.

### Loopback, no injected delay

| N | throughput r/s: base / hybrid / rust | p50 ms: base / hybrid / rust | p99 ms: base / hybrid / rust |
|---|---|---|---|
| 1 | 40 / 40 / 40 | 2.61 / 2.67 / 2.60 | 3.59 / 3.56 / 3.53 |
| 150 | 8969 / 8969 / 8970 | 2.83 / 2.77 / 2.67 | 3.98 / 3.88 / 3.75 |
| 500 | 29966 / 29996 / 29966 | 3.42 / 3.20 / 2.83 | 4.75 / 4.42 / 3.97 |

The full tables, with the min..max spread, the paired ratios and the knees,
are [summary-wan20ms.md](summary-wan20ms.md) and
[summary-loopback.md](summary-loopback.md).

## Figures

- Throughput vs p90, median of rounds, drawn in Jetpack's style:
  [20 ms](tput_p90_median-wan20ms.png) and [loopback](tput_p90_median-loopback.png).
- Jetpack's `draw_compare` latency CDF at N=150, 20 ms, round 3:
  [latency_cdf_c150-wan20ms-round3.png](latency_cdf_c150-wan20ms-round3.png).
- Every round's figures from Jetpack's own drawing functions are in
  `~/raft-test-results/jetpack/2026-09-29/<setting>/round<k>/view/figs/`.

## Reproduce

```bash
source <env with ~/.local/mako-deps libs on LD_LIBRARY_PATH>
P="rw_1000000:1 rw_1000000:10 rw_1000000:50 rw_1000000:100 rw_1000000:150 \
   rw_1000000:200 rw_1000000:300 rw_1000000:500 rw_zipf_0.8:150 rw_zipf_1:150"
A="baseline=../mako-baseline/build_base rust=build_rust hybrid=build"
WAN_DELAY_MS=20 scripts/raft_perf/jetpack/run_jetpack_sweep.sh OUT/wan20ms 5 "$P" $A
WAN_DELAY_MS=0  scripts/raft_perf/jetpack/run_jetpack_sweep.sh OUT/loopback 3 "$P" $A
python3 scripts/raft_perf/jetpack/jetpack_compare.py OUT   # needs numpy, pandas, matplotlib
```

- **The baseline arm** is `412c225a9` with `src/deptran/raft/raft_bench_jetpack.h`
  and the four `raft_bench.cc` hooks from `8a50533f4` applied.
- **Raw data** (`.res`/CSV, and a sidecar JSON per run with the arm, commit
  and WAN_DELAY_MS) is in `~/raft-test-results/jetpack/2026-09-29/`, outside
  git.
