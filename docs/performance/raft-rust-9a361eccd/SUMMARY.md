# Rust lane (MAKO_RAFT_LANE=rust) against the C++ baseline: full sweep

Commit `9a361eccd` (the Rust lane with the reactor fix, byte-bounded batches,
the two srpc large-frame fixes and zero-copy payloads), 2026-09-26 14:39-19:40
UTC on zoo-003, MODE=perf / Release, loopback, `scripts/raft_perf/run_sweep.sh`
with `BUILD_DIR=build_rust`, 3 trials per point. Baseline:
`docs/performance/raft-baseline-412c225a` (C++ Raft on the old rrr, 09-12).
Comparison: `scripts/raft_perf/compare.py` (minimum-detectable-effect verdicts,
5% threshold) -> `compare-vs-412c225a.txt`.

**624 of 624 runs succeeded** (the previous Rust sweep, 538df5f8c, had 34
failures, all large payloads at high rate -- the two srpc bugs fixed since).

| metric (624 points, 26 configurations) | better | within noise | worse, < 5% | flagged |
|---|---|---|---|---|
| throughput | 34 | 167 | 6 | 1 |
| p50 latency | 203 | 5 | 0 | 0 |
| p99 latency | 196 | 12 | 0 | 0 |

The one flag is not a loss: p6/single/1 MiB at offered 65/s, where the Rust
lane applied exactly the offered 65.0/s (zero variance) and the baseline
applied 69.4/s -- more than offered, i.e. draining backlog inside the window.
From 72/s up the baseline falls behind the offered rate (54-68/s) while the
Rust lane keeps it; unthrottled it is 156.4/s against 54.0/s. p50 at the
flagged point is -61.8%.

Records: `records.tar.gz` (JSON only). Reproduce:

    tar xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp/before
    tar xzf docs/performance/raft-rust-9a361eccd/records.tar.gz    -C /tmp/after
    python3 scripts/raft_perf/compare.py /tmp/before/records /tmp/after
