# Where a Raft entry's latency goes (Rust lane)

A per-entry trace of the Rust lane (`MAKO_RAFT_LANE=rust`: the Rust Raft core
on the Rust srpc runtime). Every entry is timestamped at 12 points between
being handed to Raft and the leader's apply callback. The question it answers
is why an entry takes milliseconds when one RPC round trip is about 0.1 ms,
and what the ~339 ms unthrottled 1 MiB figure is made of.

**Short answer.**

- **At low load, an entry mostly waits on timers.** Three ~1 ms waits sit on
  the leader's path: polling for the AppendEntries reply, the apply thread's
  idle sleep, and the start of the send after the entry is appended. For a
  4 KB entry they are ~2.3 of its 2.6 ms. For 1 MiB they are ~2.9 of 8.1 ms.
  The rest is handling the data, which runs at 0.6–2 GB/s per step: well
  below copy speed.
- **Unthrottled, an entry waits in the log for its turn.** Replication is
  stop-and-wait: one AppendEntries round in flight at a time, carrying at most
  256 entries or 16 MiB. The next round starts only after the previous one
  commits. So latency = rounds queued ahead × round time. At 1 MiB that is
  3–4 rounds of ~78 ms: the sweep's 339 ms (318 ms in this run).
- **The two timers are inherited, not introduced.** The same 1 ms reply poll
  and 1 ms apply sleep exist in the C++ baseline (`412c225a`,
  `src/deptran/raft/server.cc:2567` and `:1478`).

## Settings

Identical to the full sweep (`docs/performance/raft-rust-9a361eccd`), except
that these runs are traced and there is one trial per point.

| | |
|---|---|
| host | zoo-003: 64-core Intel Xeon E5-2683 v4 @ 2.10 GHz, Linux 7.0.0-29 |
| build | Rust lane, clang with libc++, `MODE=perf`, `CMAKE_BUILD_TYPE=Release` (both recorded in every run's JSON); Raft snapshots off; memory-only log |
| source | commit `c23e397e4` plus the trace insertions listed below; no logic changed |
| cluster | three `raft_bench` processes (leader `localhost`, followers `p2`, `p1`) on one host, TCP over loopback, launched by `examples/raft_bench.sh`; randomized ports; preferred-leader grace 30 s |
| shape | 1 partition, single Raft group, 3 replicas |
| run | 3 s warmup (excluded), 10 s measured window, batch hint 1 |
| in-flight cap | 4096 (4 KB), 256 (286,208 B), 64 (1 MiB) entries, as in the sweep |
| points | below saturation: 4 KB @ 240/s, 286,208 B @ 90/s, 1 MiB @ 26/s; then the same three sizes unthrottled |
| latency means | submit → the leader's own apply callback, measured on the leader. One round trip to one follower (a majority of 3); the followers' apply is not included |

## Method

An opt-in trace (`MAKO_RAFT_TRACE_FILE=<prefix>`) records, per log index, the
`CLOCK_MONOTONIC` time in µs at each stage. That clock is machine-wide, so the
leader's and followers' times share one timeline. Each process writes
`<prefix>.<pid>` as CSV at exit.

| # | stage | where | trace call |
|---|---|---|---|
| 0 | queued | `RaftWorker::EnqueueLog` | time taken at `raft_worker.cc:796`, carried to `:1242`, stamped at `raft_worker.cc:863` |
| 1 | submit thread picks it | `RaftWorker::Submit`, entry | time taken at `raft_worker.cc:849`, stamped at `raft_worker.cc:864` |
| 2 | appended | `RaftWorker::Submit`, after `Start()` | `raft_worker.cc:865` |
| 3 | send starts | core, heartbeat phase 1, before `raft_phase1_send_append` (first follower) | `src/server_cc.rs:1233` |
| 4 | send call returns | core, after it (encode into the frame and append it to the connection's outbound buffer; the socket write happens later, on the poll thread) | `src/server_cc.rs:1243` |
| 5 | follower handler starts | Rust service `append_entries`, entry (follower process) | time taken at `rt/src/service.rs:122`, stamped at `rt/src/service.rs:138` |
| 6 | follower replied | the same, after `ServeAppendEntries` (follower process) | `rt/src/service.rs:139` |
| 7 | reply arrives at leader | Rust transport reply callback (first follower reply = majority) | `rt/src/transport.rs:291` |
| 8 | leader commits | core, `raft_commit_advance` | `src/server_cc.rs:666` |
| 9 | queued for apply | core, apply-queue push | `src/server_h.rs:3900` |
| 10 | apply thread picks it | core, apply-queue pop | `src/server_h.rs:3370` |
| 11 | callback runs | `RaftWorker::Next` | `raft_worker.cc:1010` |

Paths are under `src/deptran/raft/`. Line numbers are in commit `c23e397e4`
with the trace patch applied: the insertions were never committed, so these
lines do not exist in the repository.

**What is at each point.** A timestamp, not a log line. Each point is one
call into the recorder:
- `raft_trace_at(stage, index, t)` stamps one log index;
- `raft_trace_through(stage, up_to_index, t)` stamps every index from the
  stage's previous high-water mark up to `up_to_index`. This covers
  batch-level points (3-8), where one AppendEntries carries many entries.

`t = 0` means "now". Stages 0, 1 and 5 read the clock earlier, where the
event happens, and pass that time in. The first stamp for an (index, stage)
wins.

The recorder is `server.cc:1712-1793` (same patched tree):
- a fixed table of 2^19 rows of 12 `CLOCK_MONOTONIC` µs values;
- allocated only when `MAKO_RAFT_TRACE_FILE` is set, so every call is a
  no-op otherwise;
- written out as `<prefix>.<pid>` CSV (`idx,s0..s11`) at exit, or on
  SIGTERM for the followers the launcher stops.

Insertions, all timestamp calls only:
- `server.cc`: the recorder. It is inert unless the variable is set, and
  includes a SIGTERM handler, installed only when tracing, so the followers
  that the launcher stops with SIGTERM still write their file.
- `raft_worker.h/.cc`, `src/server_cc.rs`, `src/server_h.rs`,
  `rt/src/transport.rs`, `rt/src/service.rs`: the stage marks.

The analysis joins the leader's rows to the first-replying follower's by log
index. It drops the first 20% and last 5% of entries, and reports p50/p90
per step. Tracing costs little. The trace's per-entry totals match
raft_bench's own p50 for the same run (tables below). Against the sweep they
are within ~2% at 4 KB and 286 KB, and ~1% at 1 MiB @ 26/s (8.10 vs 7.99 ms).

## Results: below saturation (one entry at a time)

Every AppendEntries here carries exactly one entry (p50 = p90 = 1). µs, p50
(p90):

| step | 4 KB @ 240/s | 286 KB @ 90/s | 1 MiB @ 26/s | kind |
|---|---|---|---|---|
| queued → submit thread | 71 (76) | 70 (80) | 73 (82) | hand-off |
| Start(): build Command + append | 12 (22) | 437 (458) | 1620 (1831) | data |
| appended → send starts | **561 (1010)** | **606 (1038)** | **616 (1046)** | wait |
| send call (encode + queue) | 13 (15) | 185 (230) | 485 (505) | data |
| send done → follower handler | 95 (110) | 601 (641) | 1383 (1408) | socket write + network + read |
| follower handler | 31 (44) | 493 (529) | 1659 (1732) | data |
| follower reply → leader | 75 (85) | 123 (135) | 118 (131) | network |
| reply arrived → commit | **1119 (1157)** | 14 (156) | **1669 (1778)** | wait |
| commit → apply queue | 2 | 2 | 2 | – |
| apply queue → apply thread | **589 (1002)** | **588 (991)** | **617 (1011)** | wait |
| apply thread → callback | 2 | 2 | 2 | – |
| **total** | **2580 (3194)** | **3151 (3804)** | **8099 (8876)** | |
| raft_bench's own p50 for the run | 2606 | 3213 | 8523 | |

Reading it:

- **"Reply arrived → commit" is the 1 ms reply poll.** After sending, the
  leader checks for replies every 1 ms (`RESPONSE_POLL_STEP_US = 1000`,
  `src/server_cc.rs:1505`; the check loop stops at a majority).
  - At 4 KB the reply is back ~0.2 ms after the send, but is only noticed at
    the next check, ~1.1 ms after it arrives.
  - At 286 KB the reply happens to land just before a check: 14 µs.
  - At 1 MiB it lands between checks and waits ~1.7 ms.
- **"Apply queue → apply thread" is the apply thread's 1 ms idle sleep**
  (`src/server_h.rs:3397`). It is uniform over 0–1 ms: p50 ≈ 0.59 ms and
  p90 ≈ 1.0 ms at every size.
- **"Appended → send starts" has the same 0–1 ms uniform shape.** Waking the
  replication fiber from the submit thread (`RequestReplication` queues a
  wake job on the poll thread) appears to be picked up on a ~1 ms tick
  rather than immediately. *This one is inferred from the shape; the trace
  does not show the mechanism.*
- **The data steps are slow for what they do.** For 1 MiB:
  - `Start()` takes 1.6 ms (wrapping the bytes in a Command and appending);
  - the leader's encode into the outbound buffer takes 0.5 ms;
  - the socket write, loopback and the follower's read take 1.4 ms (at least three copies of the payload, and one `recv` per 64 KiB);
  - the follower's decode + append + reply takes 1.7 ms.

  That is 0.6–2 GB/s per step, where a plain copy on this machine is several
  GB/s. Where those bytes go is the natural next profile; this trace shows
  the time, not the cause.

## Results: unthrottled (the ~339 ms)

µs, p50 (p90):

| step | 4 KB | 286 KB | 1 MiB |
|---|---|---|---|
| queued → submit thread | 9 (19) | 9239 (17067) | 8181 (15042) |
| Start() | 3 (7) | 402 (425) | 1518 (1640) |
| **appended → send starts** | **109270 (121888)** | **260046 (327394)** | **230952 (295368)** |
| send call (encode + queue, whole batch) | 816 (858) | 25837 (30552) | 23491 (28885) |
| send done → follower handler | 969 (2544) | 32976 (34805) | 31253 (32374) |
| follower handler | 2544 (2643) | 18807 (21182) | 17675 (19015) |
| follower reply → leader | 112 (157) | 1352 (3811) | 1566 (2957) |
| reply arrived → commit | 1708 (1973) | 3330 (5522) | 3255 (5448) |
| apply queue → apply thread | 813 (1345) | 663 (1039) | 596 (1027) |
| **total** | **116800 (131180)** | **352485 (428203)** | **317137 (390475)** |
| raft_bench's own p50 for the run | 116472 | 352426 | 317804 |
| entries per AppendEntries | 256 (256) | 58 (58) | 15 (15) |
| round: send start → commit | 6.4 ms | 83 ms | 78 ms |
| gap between consecutive rounds | 7.4 ms | 83 ms | 78 ms |
| throughput (entries/s) | 34,345 | 684 | 189 |

Reading it:

- **73–94% of the latency is "appended → send starts":** time spent in the
  leader's log waiting for a replication round that has room for the entry.
- **Rounds do not overlap.** The gap between consecutive rounds equals the
  round time (1 MiB: 78 ms and 78 ms), and each follower has at most one
  AppendEntries outstanding. So throughput = batch ÷ round time:
  - 1 MiB: 15 entries per 78 ms ≈ 192/s (measured 189);
  - 286 KB: 58 entries per 83 ms ≈ 700/s (measured 684);
  - 4 KB: 256 entries per 7.4 ms ≈ 35k/s (measured 34,345).
- **The batch is capped by count for small entries and by bytes for large
  ones.** 4 KB batches are always exactly 256 entries. 286 KB batches are 58
  entries (≈16.6 MB) and 1 MiB batches 15: the 16 MiB
  `MAKO_RAFT_APPEND_BATCH_MAX_BYTES` cap.
- **A large round is ~80 ms of sequential data handling.** For 15 MiB it is
  23 ms encoding into the outbound buffer on the leader, 31 ms socket write, transfer and read, 18 ms follower
  decode + append, then the reply and the poll. That is the same 0.5–1 GB/s
  per step as the one-entry case, and nothing in one round overlaps the next.
- **So the ~339 ms is queueing:** the in-flight cap keeps ~50–62 entries
  queued, which is 3–4 rounds of ~78 ms ahead of a new entry. It measures
  round time × queue depth, not the cost of an entry.

## What this does not cover

- **The C++ baseline was not traced.** Its code has the same two 1 ms waits;
  what makes it slower (17.6 ms at 1 MiB @ 26/s) is not measured here.
- **One trial per point.** The steps are stable across entries (p90 close to
  p50), and an earlier non-Release run agreed with this one within a few
  percent on every step, but there is no formal across-run spread.
- **The follower-side steps come from the first follower to reply only.**
- **Loopback only:** no real network latency.

## Reproduce

In a worktree with the trace insertions, build the Rust lane (`MODE=perf`,
`CMAKE_BUILD_TYPE=Release`), then for each point:

    MAKO_RAFT_TRACE_FILE=/tmp/trace/p1048576_r26 \
      examples/raft_bench.sh --build-dir build_rust --out /tmp/trace/p1048576_r26.json \
        --partitions 1 --payload-bytes 1048576 --rate 26 --max-outstanding 64 \
        --warmup-sec 3 --duration-sec 10

The per-process CSVs (~150 MB for all six points), the trace patch
(`raft-trace-insertions.patch`, against `c23e397e4`) and the analysis script
(`trace_breakdown.py`) are kept outside the repository, on zoo-003 in
`~/raft-test-results/perf/trace-kit/`.
