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
