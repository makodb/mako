# S5 — first cross-stack perf gate (2026-07-30)

Rust client vs C++ client, **both driving the same unmodified C++
`rpcbench -s`**, same session, same pinning (server `taskset -c 2`,
clients `taskset -c 4-7`), `-t 4`, `fast`/`fast_nop`. There is no Rust
server yet (S6), so this isolates exactly one variable: the client
stack.

## Result

| depth | payload | Rust qps | C++ qps | ratio |
|---:|---:|---:|---:|---:|
| 100 | 10 B | 995,406 | 1,014,676 | **98.1%** |
| 100 | 100 B | 932,079 | 911,846 | **102.2%** |
| 100 | 1024 B | 737,045 | 679,178 | **108.5%** |
| 1 | 10 B | 26,682 | 35,930 | 74.3% |
| 1 | 1024 B | 26,500 | 34,968 | 75.8% |

**Depth 100 passes the 10% criterion** in all three payload cells —
that is the gate, per the baseline's own noise analysis (depth-1
run-to-run spread is 5–18%, at or beyond the criterion, so depth 1 was
declared directional before any Rust number existed).

## Depth 1 is a real gap, and it is attributable

~25% down in both cells, consistently — too large and too repeatable to
be the depth-1 noise the baseline recorded. It is the **blocking-wait
model**: `Future::wait_timeout` parks the calling thread on a condvar,
so every request costs a park/unpark pair plus the poll thread's
signal. The C++ at depth 1 resumes a coroutine *on the poll thread*,
paying no cross-thread wakeup at all.

At depth 100 this is amortized across 100 in-flight requests and
vanishes; at depth 1 it is the entire round trip. **S7 (the stackless
executor) is the stage that addresses it** — completing on the poll
thread instead of parking a user thread is precisely what it buys. This
number is the baseline S7 will be judged against.

## The harness decided the result once already

The first version of `rbench` issued a batch of `depth` requests,
drained the whole batch, then issued the next. That measured **822,860
qps at depth 100 / 10 B — a 20% shortfall** and an apparent gate
failure.

It was the harness, not the client. Batch-and-drain lets occupancy
sawtooth from `depth` to zero, so the pipeline stands empty for the
tail of every batch. Switching to a sliding window — retire the oldest,
immediately issue one more, holding occupancy at `depth` continuously,
which is what the C++'s independent per-thread coroutines do — moved
the same binary from 822,860 to 995,406 with no change to the client.

This is exactly the "harness-semantics divergence deciding the outcome
silently" the plan flagged as S5's riskiest element. Recording it
because the failing number was the plausible one: a new port measuring
20% slow is entirely believable, and it would have been accepted.

## Reproduce

```sh
taskset -c 2 build_clang22/rpcbench -s 127.0.0.1:19401 &
taskset -c 4-7 target/release/rbench   -c 127.0.0.1:19401 -n 6 -t 4 -o 100 -b 10
taskset -c 4-7 build_clang22/rpcbench  -c 127.0.0.1:19401 -n 6 -t 4 -o 100 -b 10 -m fast
```

## Still not covered

**Latency percentiles.** Unchanged from the baseline: the criterion
covers latency, no harness exists, and half the gate stays undefined
until one does. The depth-1 throughput gap above is a proxy for it, not
a substitute.

## S6 addendum — reverse interop (2026-07-30)

The unmodified C++ `rpcbench -c` driving **this crate's server**
(`rbench -s`, serving `fast_nop`), same pinning, `-t 4 -o 100 -b 10`:

| direction | qps |
|---|---:|
| Rust client → C++ server | 995,406 |
| C++ client → **Rust server** | **690,243** |
| C++ client → C++ server | 1,014,676 |

The C++ client counts OK RESPONSES, so 690k means it parsed 690k of our
reply envelopes as valid — the strongest available check that the reply
side is correct, since that peer did not come from this source tree.

**The Rust server is ~32% down and that is not yet explained.** Two
candidates worth measuring before assuming either: dispatch runs
synchronously on the poll thread (so a handler's cost is inline with the
read pump), and every reply allocates a fresh `Vec` and takes the
outbound mutex. Neither has been profiled; recording the number now so
the investigation starts from a measurement rather than a guess. This is
a SERVER-side figure and does not affect the S5 client gate above.

## S7 addendum — the executor (2026-07-30)

### CORRECTION: the first version of this section was wrong

It reported the executor at **41.4% at depth 100** and attributed it to
continuations serializing under the connection's reader lock. **Both the
number and the explanation were artifacts of the harness.**

`rbench` created ONE poll thread and shared it across all `-t`
connections. The C++ does not: `rpcbench.cc`'s `client_proc` calls
`PollThread::create()` *inside each client thread*, so `-t 4` is four
poll threads. In `-m await` the continuation runs on the poll thread, so
one shared poll thread funnelled every task's send through a single
thread while the C++ spread them across four.

With the structure matched, the regression does not exist. The reader
lock was never implicated; that hypothesis was never profiled and should
not have been written down as a cause.

**This is the second time the harness decided a result in this file**
(the first: batch-and-drain vs sliding window, S5). Both times the wrong
number was the plausible one, and both times the fix was in the harness,
not the system under test. The standing rule is now: before attributing
a perf result to a design property, check that the harness matches the
C++ structurally — thread counts, poll threads, and concurrency shape.

### Result (one poll thread per connection, matching the C++)

`-m await` uses the stackless executor (continuations resume on the poll
thread); `-m block` is the S5 path (one park/unpark per request).

| depth | payload | C++ | await | block |
|---:|---:|---:|---:|---:|
| 1 | 10 B | 36,552 | **39,034 (106.8%)** | 26,182 (71.6%) |
| 100 | 10 B | 1,010,425 | **1,063,391 (105.2%)** | 978,982 (96.9%) |
| 100 | 100 B | 921,540 | **991,539 (107.6%)** | — |
| 100 | 1024 B | 689,435 | **756,473 (109.7%)** | — |

**The executor passes every cell**, depth 1 and depth 100, across all
three payloads — 105–110% of the C++. The blocking path passes depth 100
and fails depth 1 (71.6%), which is what S7 was built to fix and did:
the park/unpark per request was the entire gap.

`await` is therefore the parity path, and this is not a mode chosen per
cell to flatter the table — it is the same mode in every row.

## S9 (part) — latency (2026-07-30)

`rbench -l` records per-request round-trip times per worker (merged at
exit, so the hot path touches no shared state) and reports percentiles.

**Timestamping does not perturb the throughput measured beside it** —
the S1 question, now answered: depth 1 38,721 → 38,373 (−0.9%), depth
100 1,068,684 → 1,066,370 (−0.2%). Both inside run-to-run noise, so
`-l` numbers and qps from the same run are usable together.

### Rust client latency, `-m await`, 10 B (µs)

| depth | n | mean | p50 | p90 | p99 | p99.9 | max |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 223,765 | 107.2 | 78.1 | 155.3 | 159.0 | 233.1 | 5,412.9 |
| 100 | 6,356,098 | 379.2 | 381.7 | 465.7 | 616.1 | 771.1 | 51,625.9 |

Internal consistency check: at `-t 2 -o 1`, Little's Law predicts
2 / 19,210 qps = 104 µs and the measured mean was 103.6 µs.

### Mean latency, both stacks (Little's Law, from measured throughput)

| depth | concurrency | C++ | Rust | ratio |
|---:|---:|---:|---:|---:|
| 1 | 4 | 109.4 | 107.5 | **98.2%** |
| 100 | 400 | 395.9 | 377.1 | **95.2%** |

Mean latency is derivable for both without touching the C++, because
concurrency is fixed and known. **Both cells are inside 10%**, matching
the throughput result.

### What is still missing, and the decision it needs

**C++ latency PERCENTILES.** Little's Law gives the mean only; it says
nothing about the tail, and the criterion plausibly cares about the
tail. The only percentile code in the C++ tree is
`src/rrr/tests/rpc_microbench.cc` — **all 282 lines commented out, no
build target, written against an older API.**

Reviving it is not free and is not obviously the right call:

 - it changes a measured binary in `src/rrr`, and the pinned throughput
   baseline was captured from the current one;
 - `src/rrr` is legacy-until-strangled, so investing in its test
   harness may be wasted;
 - the alternative — accept mean-latency parity plus Rust-side
   percentiles, and treat C++ tail latency as unmeasured — leaves a
   real hole in the criterion.

Flagging rather than choosing: this is a scope decision about the C++
tree, not a port implementation detail.

## S8a-3 — fiber dispatch gate (2026-07-31)

C++ `rpcbench -c` driving our server, `-t 4 -o 100`, inline dispatch vs
fiber-per-request (the `reg_fast_rpc` / `reg_rpc` distinction).

| payload | inline | fiber | fiber/inline |
|---:|---:|---:|---:|
| 10 B | 671,835 | 595,243 | **88.6%** |
| 100 B | 667,399 | 589,547 | **88.3%** |
| 1024 B | 40,405 | 41,898 | 103.7% |

**The gate is ≥60%** (C++'s own fiber/fast ratio is 64.9 / 67.6 / 70.1%).
All three cells pass, and the first two beat the C++ ratio.

### Recycling was worth 10x, and a counter would have missed it

The first measurement was **9.0%** (61,277 vs 684,153). Not a fiber
cost — an allocator cost: every spawn was `mmap` + `mprotect` and every
finish a `munmap`, three syscalls per request. Adding the C++'s
`REUSE_FIBER` stack pool took it to 88.6%.

The test that guards it asserts stack-base ADDRESS identity across 64
sequential spawns, not a spawn count — a counting test would have
passed the whole time.

### OPEN: a 1 KiB throughput cliff, and it is NOT the fibers

Both modes collapse from ~670k to ~40k at 1 KiB, a 16x drop that
payload size does not explain — the C++ server does 689k in the same
cell. Since it hits inline and fiber *equally*, the fiber ratio above is
still meaningful; the absolute number is not.

First hypothesis was `Vec::remove(0)` in the outbound drain going
quadratic once backpressure lets the queue grow. That is a genuine
latent bug and is fixed (it is a `VecDeque` now) — but it was **not**
this: the numbers did not move. Cause unknown, not profiled, not
guessed at again.

Likely the same defect as the S6 ~32% Rust-server deficit, which is also
unattributed. Both are server-side and neither is S8. Profile before
theorizing — this file has twice recorded a plausible explanation that
measurement then refuted.

### Investigation (2026-07-31) — two more hypotheses dead, one partial hit

**The knee, measured:** 100 B 655k · 200 B 655k · 400 B 645k · 600 B
627k · **800 B 297k** · **1024 B 58k**. Ratios 1x / 2.1x / 10.8x — not a
cost curve, a collapse.

**Hypothesis 2 — quadratic outbound drain (`Vec::remove(0)`). DEAD.**
Real latent bug, fixed (it is a `VecDeque` now), but the numbers did not
move.

**Hypothesis 3 — compaction feedback loop.** The collapse shape
suggested falling behind grows the buffer, which makes the O(buffer)
`drain` slower, which… **DEAD, killed by instrumentation**: reader
high-water is 127 KB at 600 B and 169 KB at 1024 B. Bounded and modest,
so the buffer is not running away and compaction is not the cost.
(`frame::buffer_high_water()` is the instrument, kept.)

**Hypothesis 4 — Nagle / delayed-ACK. PARTIAL HIT.** With
`TCP_NODELAY` on accepted connections, 1 KiB goes 44,037 → **112,052
(2.5x)**. Real, and not sufficient: still ~6x below the small-payload
number and far below the C++'s 689k in the same cell.

So Nagle explains part of the cliff and something else explains the
rest. **`TCP_NODELAY` is NOT enabled** — nothing in `src/rrr` sets it,
so enabling it would invalidate every comparison in this file. It is
available as `SRPC_DIAG_NODELAY=1`, diagnostic only.

**RESOLVED (2026-07-31) — it was reply coalescing.** The C++
`send_frame` (tcp_channel.cpp:1285-1303) never writes inline: it encodes
into ONE contiguous `outbound_` buffer and only arms the write interest.
So N replies produced in one poll iteration leave as ONE `send`. Ours
held a `VecDeque<Vec<u8>>` and called `send` per frame — at depth 400,
400 syscalls and 400 small segments where the C++ does one, which is
also precisely what Nagle punishes (tying hypothesis 4 to the cause).

Fixed by adopting the C++ shape: one contiguous buffer, accumulate while
on the poll thread, and flush once at the end of the read pump. Off the
poll thread the write still happens inline, because there is no
iteration to piggyback on and no eventfd to make a deferral cheap.

| payload | before | after |
|---:|---:|---:|
| 10 B | 671,835 | **2,060,785** |
| 1024 B | 40,405 | **1,479,627** |

The cliff is gone (36x at 1 KiB), and **this also explains the S6 ~32%
server deficit** — same missing coalescing. The Rust server now measures
roughly 2x the C++ server in the same cells (C++: 1,014,676 at 10 B,
689,435 at 1 KiB).

Correctness re-verified after the change, not assumed: 186 lib tests,
5 server round-trip, 4 loopback, 3 fiber-seam, and 13 interop against
the live C++ `rpcbench -s` including the value-checking `fast_add`
cases.

Note a correction this turned up: the C++ comment at tcp_channel.cpp
says posting `update_mode` writes to the mpsc channel's EVENTFD, which
wakes `epoll_wait` immediately. The earlier claim in this file that
there is "no eventfd anywhere" is true of the epoll timeout, not of the
command channel.

**S8a-3 fiber gate re-measured on the new baseline**, 1 KiB:
inline 1,492,384 vs fiber 1,209,277 = **81.0%**, still well clear of the
60% gate.

### The coalescing change COST the client ~12 points

It is a trade, not a free win, and the cost is on the other side of the
system from the gain.

Client depth 100, 10 B, three trials after the change: 93.8 / 93.3 /
92.3% of C++ (spread under 2%). Before it: 105.2%. That is well outside
the 1-7% depth-100 noise this file records, so it is real.

Mechanism, and this part is NOT verified: in `-m await` the task
continuation runs on the poll thread, so a follow-up `call()` now
accumulates instead of writing, and the request does not leave until the
whole read batch has been decoded. That should be a pipeline bubble —
the server sits idle while we finish decoding — which coalescing does
not repay on the client, because a client's sends are spread over time
where a server's replies arrive in a burst.

Both sides still pass their gates (93% client, and the server went from
40k to 1.48M at 1 KiB), so the trade is heavily positive overall. But it
is recorded as a trade: an asymmetry between the two roles that one
shared `send_frame` cannot serve optimally, and a candidate for a
per-connection policy if the client number ever needs to come back.
