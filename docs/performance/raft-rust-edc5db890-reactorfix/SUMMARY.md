# Reactor timeout-eviction fix (edc5db890): the Raft latency regression, closed

Measured 2026-09-26 on zoo-003, MODE=perf / Release, loopback, idle host.
The fix and its cause: `docs/performance/raft-latency-regression.md` and
commit edc5db890 (srpc reactor.rs run_loop retains TIMEOUT events again).

## R3(a): the three-binary point, interleaved (`r3a-*.txt`, `r3a-bisect.tar.gz`)

4 KB entries, 240/s, `scripts/raft_perf/arms_roundrobin.sh`, three trials at
8 s, then one at 20 s. Latencies in microseconds.

| arm | what it is | p50 (8 s) | p99 (8 s) | p50 / p99 at 20 s |
|---|---|---|---|---|
| sep21 | Rust Raft + old srpc (preserved binary) | 2696 2715 2720 | 3680 3646 3766 | 2724 / 3677 |
| head | 70fc2c0da, before the fix | 3265 3300 3253 | 7112 6938 7172 | 5096 / 10160 |
| fixed | edc5db890 | 2745 2749 2703 | 3715 3668 3640 | 2742 / 3728 |

The fixed arm is at sep21 (within ~1%) and flat with run length; head degrades
with run length, which is the accumulation the fix removes. Zero gaps,
duplicates and out-of-order applies on every arm.

## R3(b): the rate slice against the C++ baseline (`r3b-*`)

`run_sweep.sh --phase rate --only p1-single-pb4096` (63 runs: 21 rates x 3
trials, the full sweep's flags), compared by `scripts/raft_perf/compare.py`
against `docs/performance/raft-baseline-412c225a/` (C++ Raft, old srpc).

62 of 63 metric points within noise or better. One flag: p50 at 8900/s,
+5.45% against a 4.58% noise floor, while p99 at the same point is -10.6% and
the neighbouring rates are within noise -- recorded as marginal, re-checked
by the final full sweep. Before the fix this slice read +46.8% p50 at 240/s
and +193% p50 at 11900/s.
