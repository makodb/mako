# The Raft latency regression: it is srpc's reactor, not the Rust conversion

Measured 2026-09-25/26 on zoo-003, MODE=perf / Release, `raft_test_coro=false`,
three processes on loopback.

## 0. Where the data is

Everything cited below is in this repository; `raft_perf_output/` is
gitignored and was never a home for it.

| what | where | contents |
|---|---|---|
| C++ baseline, 09-12 | `docs/performance/raft-baseline-412c225a/` | 624 records + SUMMARY, tables, plots |
| Rust sweep, 09-25 | `docs/performance/raft-rust-538df5f8c/` | 624 records + SUMMARY |
| the three-binary bisect | `.../raft-rust-538df5f8c/bisect/` | 12 records, `trials.txt`, `strace-summary.txt` |

Reproduce a comparison with:

    tar xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp/before
    tar xzf docs/performance/raft-rust-538df5f8c/records.tar.gz    -C /tmp/after
    python3 scripts/raft_perf/compare.py /tmp/before/records /tmp/after/records

The raw strace traces (18 MB) are not kept; `strace-summary.txt` holds the
derived timeline and names the command that produced it, and
`scripts/raft_perf/strace_poll_timeline.py` regenerates the analysis from a
fresh trace.

## 1. The sweep that found it

`scripts/raft_perf/compare.py` over 624 paired runs (3 trials per point):

    before  412c225a  2026-09-12  C++ Raft, old rrr/srpc
    after   538df5f8c 2026-09-26  Rust Raft, synced srpc subtree

    throughput   129 of 146 points within noise; nothing outside the threshold
    latency      330 regressions, 0 improvements, 194 within noise

compare.py prints latency in MILLISECONDS (`latency_p50_us / 1000`,
`scripts/raft_perf/processing.py:387`). Representative points, p1/single/4096B:

| offered | throughput before -> after | p50 ms | p99 ms |
|---|---|---|---|
| 240    | 240.000 -> 239.967 (noise) | 2.722 -> 3.996  (+46.8%) | 3.700 -> 7.763  (+109.8%) |
| 1200   | 1200.100 -> 1199.600       | 2.903 -> 4.180  (+44.0%) | 4.048 -> 7.979  (+97.1%)  |
| 11900  | 11899.567 -> 11871.000     | 5.963 -> 17.481 (+193.2%)| 11.563 -> 48.169 (+316.6%)|

Throughput is intact everywhere. Only latency moved, and the gap widens with
load.

## 2. The bisect: three binaries, interleaved, one idle host

4096-byte entries, 240 entries/s, 8 s window, three trials each, run
round-robin so drift hits all three equally.

| build | what it is | applied/s | p50 us | p90 us | p99 us |
|---|---|---|---|---|---|
| sep21   | Rust Raft + **old** srpc (`/home/users/zyang2/mako/build`, Sep 21) | 240.0 | 2711 | 3328 | 3659 |
| control | **new** srpc, before the stage 0-3 Raft work (`49defdf5e`)          | 239.9 | 3218 | 5060 | 6939 |
| head    | new srpc + stage 0-3 (`538df5f8c`)                                  | 239.9 | 3231 | 5032 | 6858 |

`control` and `head` are the same within trial spread (+0.4% p50). The Raft
conversion work of this branch contributes nothing measurable. The whole
regression sits between `sep21` and `control` — that is, in the srpc subtree
sync (`9dd6ff492`, 141 upstream commits, 2026-08-26 -> 2026-09-23).

## 3. It grows with run length

Same point, varying only the measured window:

| build | 4 s | 8 s | 20 s |
|---|---|---|---|
| head  | p50 3059 | p50 3231 | p50 4818 |
| sep21 | -        | p50 2711 | p50 2717 |

The old runtime is flat. The new one degrades the longer it runs: this is
accumulation, not a fixed per-operation cost.

## 4. What the syscall trace shows

`strace -f -tt -T` on the leader, steady-state 8 s window:

| | head | sep21 |
|---|---|---|
| epoll_wait calls | 1284/s | 1860/s |
| work between epoll_waits (loop body) p50 | 349 us | 154 us |
| AppendEntries write -> reply read, p50 | 1122 us | 384 us |
| epoll_wait return -> AppendEntries write, p50 | 97 us | 140 us |

The socket path is not slower — the reply comes back off the wire just as fast,
and the poll thread reacts to readiness no later. What changed is the *loop
body*: 2.3x more time per pass doing non-syscall work, so the loop turns 31%
fewer times per second and every queued wake waits longer.

## 5. The mechanism

`src/srpc/reactor/reactor.rs`, `Reactor::run_loop`. The sync deleted a clause
from the retain predicate that evicts timed-out events:

    old (bbbd51d89):  status != EventStatus::DONE && status != EventStatus::TIMEOUT
    new (HEAD):       (*ev).status() != EventStatus::DONE

It also deleted the comment that explained why the clause was there:

    A timeout wakes its waiter while deliberately preserving
    EventStatus::TIMEOUT for the resumed fiber to inspect.  It is nevertheless
    terminal and must leave the polling queue.  Retaining it here leaks one Arc
    per timed wait and makes every subsequent reactor pass rescan all past
    timeouts.

That comment predicts exactly the three observations above: a loop body that
gets slower, latency that grows with run length, and throughput left untouched.
`check_timeout` still sets `EventStatus::TIMEOUT` (`reactor.rs:1876`) and still
moves such events to `ready_events`, but `waiting_events_` and
`composite_events_` now keep an Arc for every timed wait the process has ever
performed, and rescan all of them on every pass.

Raft performs a timed wait per heartbeat round on every replica
(`WaitForReplicationOrHeartbeat` -> `raft_int_event_wait_timeout`,
`server.cc:1470`), so the leaked set grows at roughly the round rate.

This was a Mako-local fix in `7f52613fe` that the subtree merge dropped: the
squash commit message records reactor.rs as one of two files where "the textual
merge produced silently" a hybrid, and lists which local features were
re-applied by hand. This clause was not among them.

## 6. Not yet done

The fix (restore the two-term predicate) is a two-line change to
`src/srpc/reactor/reactor.rs`, but it is a subtree file shared with Paxos and
Mako and it regenerates srpc's C++ lane, so it needs its own build and a full
re-measure before it can be claimed.
