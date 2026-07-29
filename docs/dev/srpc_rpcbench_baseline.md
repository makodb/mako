# rpcbench C++ baseline (Goal-1 parity reference)

Captured 2026-07-29 on the development host. This is the reference the
Rust port's end-to-end performance is measured against; before it, the
only rpcbench numbers in the repo were unpinned and from a previous
kernel, so "parity against measured baselines" had nothing to measure
against.

Regenerate with `/var/tmp/mako-srpc/segv/bench/capture_baseline.sh`
(raw samples land in `baseline.tsv`).

## Conditions — change any of these and the numbers are not comparable

| variable | value |
|---|---|
| host | AMD Ryzen Threadripper 2990WX, 32C/64T, 4 NUMA nodes |
| pinning | server `taskset -c 2`, client `taskset -c 4-7` — both on NUMA node 0, one of the two nodes with locally-attached memory |
| server shape | ONE poll thread. The C++ server saturates a single core; that is the shape being measured |
| client | `-t 4` threads |
| build | as-shipped: C++ `-O2 -march=native`, jemalloc |
| run length | `-n 8` → **7 samples** (the sampler discards the first reading) |
| trials | 3 per cell; the table reports the mean of trial means |
| Nagle | ON — nothing in `src/rrr` sets `TCP_NODELAY` |
| wakeup | no eventfd anywhere; `epoll_wait` uses a hard 1 ms timeout |

**Counting semantics are frozen, not fixed.** Callback mode counts
successful *sends*; await mode counts *OK responses*; a pipeline slot
dies silently on an error reply, so effective concurrency can decay
while the run still prints a confident average. The Rust harness must
mirror these rather than correct them, or the comparison is between
different quantities.

## Throughput (qps, mean of 3 trials)

| mode | depth | 10 B | 100 B | 1024 B |
|---|---:|---:|---:|---:|
| fast | 1 | 35,962 | 35,802 | 36,112 |
| fast | 100 | **1,028,587** | 935,137 | 748,626 |
| async | 1 | 39,685 | 37,687 | 36,881 |
| async | 100 | 898,276 | 868,583 | 621,445 |
| fiber | 1 | 40,073 | 38,687 | 37,195 |
| fiber | 100 | 667,397 | 632,210 | 524,666 |
| defer | 1 | 37,131 | 38,388 | 39,622 |
| defer | 100 | 595,319 | 606,379 | 624,013 |

## What the shape says

**Depth 1 is mode-insensitive and payload-insensitive** — every cell
lands in 36–40k qps whether the RPC is `fast` or `fiber`, 10 bytes or
1 KiB. At depth 1 nothing is being measured except the round-trip
wakeup path, so the dispatch mode is invisible. This is the 1 ms
`epoll_wait` tick and the absence of a wakeup fd, exactly as the plan
predicted. **A Rust port that adds an eventfd will beat these numbers
for reasons that have nothing to do with Rust**, so depth-1 parity must
be judged against a like-for-like wakeup model, or excluded.

**Depth 100 is where the dispatch modes separate**: fast 1.03M, async
898k, fiber 667k, defer 595k at 10 B. The fiber runtime costs ~35%
against `fast` — the gap S8 will be judged on, and it is invisible in
any wire-level benchmark.

**Payload scaling only bites when pipelined**: at depth 100, 10 B →
1 KiB costs fast 27% and fiber 21%; at depth 1 it costs nothing
measurable.

## Run-to-run noise — this constrains the gate

Spread (max−min over 3 trials, as % of mean):

- depth 100: **1–7%**, mostly under 6%.
- depth 1: **5–18%**, with several cells above 10%.

The Goal-1 criterion is "within 10%". At depth 1 the *noise alone*
reaches or exceeds that, so a single depth-1 comparison cannot decide
parity. Either raise the trial count until the confidence interval is
comfortably inside 10%, or gate on depth-100 cells and treat depth-1 as
directional. Recording this now, before any Rust number exists, so the
gate is not quietly redefined later to fit a result.

## Not yet captured

**Latency percentiles.** The criterion covers latency as well as
throughput, and there is no harness: the only percentile code in the
tree is entirely commented out, in a file with no build target. Until
that is revived, half the gate is undefined.
