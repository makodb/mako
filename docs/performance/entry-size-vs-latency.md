# Raft entry size vs latency in Mako

How the size of one Raft entry changes the time from submit to the leader's
apply, and which parts of the current implementation cause that. Two trees
are covered:

- **Current tree**: `mako-srpc-adopt`, branch `srpc-subtree-forward`. It has a
  Rust Raft core (`src/deptran/raft/src/`), a Rust runtime
  (`src/deptran/raft/rt/src/`) and a C++ shim (`raft_worker.cc`,
  `raft_bench.cc`). Paths with no prefix below are in this tree.
- **Baseline**: C++ Raft at `412c225a`, checked out at `/home/users/zyang2/mako-baseline`.
  Paths are written `baseline:src/...`.

Jetpack's Raft (`/home/users/zyang2/jetpack`, written `jetpack:src/...`) is
used only as a contrast.

Every file:line cited here was read when this document was written.
Measurements come from the documents named at each point. Anything marked
**inferred** follows from the code but was not measured. **Not verified**
means it was not checked.

---

## 1. The short answer

A 4 KB entry takes about 2.6 ms and a 1 MiB entry about 8.1 ms (current
tree, rust lane, below saturation). 256x the bytes costs about 3x the time,
because most of a small entry's time is three waits of up to ~1–2 ms each on
timers, not handling data. Under full load a 1 MiB entry takes about
320–340 ms. That is not because it is slower to process: it waits behind
3–4 earlier batches of up to 16 MiB, each taking about 78 ms to reach a
follower and come back.

In more detail, below saturation an entry pays about 1.2–2.9 ms of timer
waits: two waits that are uniform on 0–1 ms, plus a reply-poll wait of 0 to
~1.7 ms that depends on where the reply lands in the 1 ms poll grid. On top
of that it pays roughly 5 ns per byte of copying and encoding on the one-way
leader → first-follower leg (the reply is fixed-size). At saturation, size
matters mostly through how many entries fit in one batch. Every term used
here is defined in §2.

**Worked example (1 MiB entry, 26 entries/s, current Rust lane):**
`docs/performance/raft-latency-breakdown/README.md:105-119`. Each stage is
glossed in §2 and §3.1: the *send-start wake* is the delay before the
replication loop notices the new entry; the *reply poll* is the delay before
it notices the follower's reply; the *apply sleep* is the delay before the
apply thread notices the commit.

| part | µs (p50) |
|---|---|
| timer waits: send-start wake 616 + reply poll 1669 + apply sleep 617 | 2902 |
| per-byte work: build Command 1620 + encode 485 + socket/transfer/read 1383 + follower decode/append 1659 | 5147 |
| hand-offs and the small reply message (73 + 118 + 2 + 2) | 195 |
| sum of the part p50s | 8244 |
| **p50 of the per-entry total** | **8099** (raft_bench reported 8523) |

The last two rows differ because the median of a sum is not the sum of the
medians. The same stages at 4 KB have a per-entry-total p50 of 2580 µs (sum
of part p50s 2570). Of that, 2269 µs is the same three timers and only
151 µs is per-byte work.

Now push the same 1 MiB entries unthrottled, as fast as the leader will take
them. p50 is 317 ms (trace) or 339 ms (sweep). That is not 40 times slower
work. The entry waits behind about 3–4 batch cycles of ~78 ms: the traced
queueing p50 of 231 ms is about 3 cycles
(`raft-latency-breakdown/README.md:152-190`, `:20-21`).

---

## 2. Concepts

These terms are used with exactly these meanings throughout.

**Entry.** One call to `add_log_to_nc(log, len, par_id, batch)` becomes one
Raft log slot. Its *size* N is the application payload length `len`. Raft
adds a fixed 20-byte header (magic, version, partition id, length) before
storing it (`src/deptran/raft/application_log.cc:33-56`). The stored object
is `LogEntry { length, std::string log_entry }` inside a `TpcCommitCommand`
(`src/deptran/replication_log_entry.h:20-32`, `src/deptran/tpc_command.h:33-44`).
In Mako an entry is itself a batch of up to 400 transactions (see §6).

**Leader / follower / replica.** All measurements use 3 replicas: one
leader and two followers.

**Term.** Raft's election epoch, a counter that increases with each
election. Every log entry records the term in which it was created, and
each server has a *current term*. **Election timeout** is how long a
follower waits without hearing from the leader before it starts an
election; a single message that takes longer than that can cause a
spurious election.

**Submit path and hand-off.** A *hand-off* is a transfer of an entry from
one thread to another through a queue. The first one is the submit queue:
on the path raft_bench uses (submit thread running), `add_log_to_nc` →
`RaftWorker::EnqueueLog` copies the payload into a `PendingLog` on
`RaftWorker::submit_queue_` (`src/deptran/raft/raft_worker.cc:787-791`). A
separate *submit thread* takes up to `--batch` entries at a time and calls
`Submit` (and so Raft's `Start()`) once per entry (`raft_worker.cc:1221-1232`).
raft_bench's `--batch` knob is therefore only this take-up-to-K limit.
Without the submit thread, `EnqueueLog` calls `Submit` directly with no copy
(`raft_worker.cc:778-785`).

**AppendEntries message.** The RPC the leader sends to one follower. It
carries `prev_log_index`, `prev_log_term`, the leader's commit index and
one payload `cmd` (`src/deptran/raft/src/server_cc.rs:1231-1239`, the
`raft_phase1_send_append` call). The payload is one of three kinds
(`server_cc.rs:862-864`): empty, which is what this document calls a
**heartbeat** ("An empty Command ... signals a heartbeat", `:1111`); a
single raw entry; or a *batch*. With batching on, the raw-entry kind is used
only for an entry that is not a `TpcCommitCommand` found at the start of a
batch (`:1003-1028`), so an ordinary single entry goes out as a
`TpcBatchCommand` of one. The reply is three `u64` values, the same size
for any entry.

**next_index and match_index.** The leader keeps two progress counters per
follower. *next_index* is the first log index the leader will send to that
follower next (`server_cc.rs:939`, `batch_start_idx = peers_.next_index(ord)`).
*match_index* is the highest index that follower has acknowledged storing
(`src/deptran/raft/src/server_h.rs:864-868`). In normal operation both
advance only when phase 2 processes that follower's reply (the reply
handling around `server_cc.rs:1457-1481`); phase 1 only repairs next_index
in error cases (`:1118-1156`). Sending a batch does not move next_index, so
there is no send watermark ahead of it.

**Batch.** With `RAFT_BATCH_OPTIMIZATION` defined (`src/deptran/constants.h:158`),
the leader walks the log from the follower's `next_index` and packs entries
into one `TpcBatchCommand`. It stops at the last log index, at 256 entries,
or before the entry that would push the running total past 16 MiB, but it
always carries at least one entry (`src/deptran/raft/src/server_cc.rs:966-995`).
Each entry is counted as `64 + log_entry.size()` bytes, that is 84 + N
(`src/deptran/raft/server.cc:1426-1437`). Both limits can be changed with
`MAKO_RAFT_APPEND_BATCH_MAX_ENTRIES` and `MAKO_RAFT_APPEND_BATCH_MAX_BYTES`
(`server.cc:216-237`). A separate batch is built for each follower
(`server_cc.rs:933-934`). This batch is not the same thing as raft_bench's
`--batch` knob (the submit thread's take-up-to-K limit, above) or the Mako
400-transaction batch.

**Poll thread and fiber.** The leader's network I/O runs on a *poll
thread*: a loop around `epoll_wait` with a 1 ms timeout
(`src/srpc/reactor/epoll_wrapper.rs:123-124`). A *fiber* is a coroutine
scheduled on that same thread. The replication loop (below) is one fiber.
Jobs queued with `PollThread::add` only go onto a channel
(`src/srpc/reactor/reactor.rs:2215-2219`) and are run by the poll thread
between `epoll_wait` returns. That fiber sleeps and deferred socket writes
are also serviced between `epoll_wait` returns, so a fiber that is running
delays the poll thread's socket writes, is **inferred** from this structure
and was not traced.

**Slot and stop-and-wait (per follower).** The leader keeps one *pending
slot* per follower, holding that follower's in-flight AppendEntries.
Each follower has at most one AppendEntries outstanding. Phase 1 (below)
skips a follower whose slot is occupied (`server_cc.rs:1104-1107`; the
comment "At most one AppendEntries in flight per follower" is at `:1806`).
The slot is released only when phase 2 processes that follower's reply
(`:1646-1648`). Followers do not wait for each other. Baseline:
`if (pending_rpcs.count(site_id) > 0) continue;`
(`baseline:.../server.cc:2044-2046`). In this document *outstanding*
always means in-flight AppendEntries RPCs.

**Replication loop, loop pass.** The leader runs a single replication
fiber, named `HeartbeatLoop` / `HeartbeatDriver` in the code, even though
it carries all data, not only heartbeats. A *loop pass* is one iteration of
`HeartbeatDriver::run` (`server_cc.rs:1847-1872`). Each pass does these
steps in order:

1. **Wait.** `HeartbeatWait()` returns as soon as a replication wake is
   pending, or after the loop idle interval (`src/deptran/raft/src/server_h.rs:3674-3676`).
2. **Phase 0.** Under the lock: check leadership, recompute the commit
   index and enqueue newly committed entries for apply
   (`server_cc.rs:798`).
3. **Phase 1.** For each follower whose slot is empty, build a payload and
   send it without blocking, then record the RPC in that slot
   (`server_cc.rs:1089`, `:1104-1107`, `:1242-1244`).
4. **Phase 2.** Poll the replies (see *reply poll*) and process every
   completed one, from this pass or an earlier one. Stop when any of these
   holds (`server_cc.rs:1655-1668`): this pass's *authority quorum* is
   reached; no RPC sent in this pass is still outstanding; or the 5 ms
   deadline passes. The authority quorum is a leadership (read-index)
   count, not the commit rule. The leader counts itself from the start
   (`:139-143`). A follower's reply adds a vote only if it answers an RPC
   sent in this pass, carries the same term, and passes
   `raft_server_read_index_reply_confirms_authority` (`:392-409`). With
   3 replicas one such follower is enough. So phase 2 can exit without any
   commit progress, and a reply that lets an entry commit can arrive after
   phase 2 has exited.
5. **Phase 3.** Recompute the commit index. If it advanced, enqueue the
   entries for apply and request one more pass, so that followers learn
   the new commit index (`server_cc.rs:1749-1787`).

A pass is short: at most about 5 ms of phase 2 plus the sends. An RPC can
outlive the pass that sent it; it is picked up in a later pass's phase 2.
The baseline has the same four phases inline in `RaftServer::HeartbeatLoop`
(`baseline:src/deptran/raft/server.cc:1918`).

**Batch cycle.** For one follower, the time from the start of an
AppendEntries send to the processing of its reply (and, for the first
follower to reply, the commit). Because of stop-and-wait, this is also the
gap between consecutive batches to that follower. It is a different thing
from a loop pass: with 1 MiB entries a batch cycle is ~78 ms and spans many
passes, because phase 2 exits at its 5 ms deadline and later passes find
the follower's slot occupied. The breakdown doc's "round: send start →
commit" and "gap between consecutive rounds" rows
(`raft-latency-breakdown/README.md:166-167`) measure batch cycles, not
passes. Measured cycles: 7.4 ms (gap) at 4 KB, 83 ms at 286 KB, 78 ms at
1 MiB, unthrottled.

**Pipelining.** More than one AppendEntries outstanding to the same
follower. The leader sends new entries before earlier batches are
acknowledged, and uses a send watermark that runs ahead of the acknowledged
index. Mako does not do this in either tree. Jetpack does by default
(§5.6).

**Majority.** More than half of the replicas, counting the leader. With 3
replicas, the leader plus one follower is a majority.

**Commit index.** The highest log index known to be stored on a majority.
`PeerTable::majority_match_index` picks the follower match index at
ascending rank `(nservers-1)/2` (`server_h.rs:881-900`). With 3 replicas
that is the larger of the two followers' match indices, so one follower's
acknowledgement is enough to commit. The index advances only for an entry
from the current term. It is recomputed in phase 0 and in phase 3.

**Apply.** Handing a committed entry to the application callback. On the
leader, phase 0 and phase 3 push committed entries onto `apply_queue_`
(`server_h.rs:3837`). A separate apply thread pops them and calls back with
a pointer into the stored bytes, with no copy
(`src/deptran/raft/raft_worker.cc:1142-1167`). Latency in this document
ends at the leader's apply callback.

**Loop idle interval.** The field is `heartbeat_interval_us_`: 5000 µs in
production and 100000 µs under `RAFT_TEST_CORO`
(`src/deptran/raft/server.h:169-173`). It is the longest the replication
loop sleeps when nothing wakes it. It also sets the phase-2 deadline,
`max(1, min(100000, interval))` = 5 ms (`server_cc.rs:1507-1517`).

**Replication wake.** `Start()` appends the entry under the lock, then calls
`RequestReplication()` (`server_h.rs:4663-4692`). That sets a pending flag
and, if the loop is asleep, queues a job on the poll thread to wake it
(`server_h.rs:3607`). `PollThread::add` only sends on a channel
(`src/srpc/reactor/reactor.rs:2215-2219`). The poll thread drains that
channel after `epoll_wait`, which has a 1 ms timeout
(`src/srpc/reactor/epoll_wrapper.rs:123-124`).

**Reply poll.** Phase 2 does not wake when a reply arrives. It reads each
slot's completed flag, then sleeps for `min(1000 µs, time left to the
deadline)` (`RESPONSE_POLL_STEP_US = 1000`) and checks again
(`server_cc.rs:1505`, `:1662-1677`). The *reply-poll wait* is the time from
the reply's arrival to the next check. Its value depends on where the reply
lands within the poll grid, that is on the round-trip time modulo the
step, and so only indirectly on N. The requested step is 1 ms, but the
observed wait can exceed it (1119 µs p50 at 4 KB,
`raft-latency-breakdown/README.md:114`), so the effective step is longer,
probably because fiber sleeps are serviced only after a 1 ms `epoll_wait`
returns (**inferred**). Baseline: the same constant and `Fiber::sleep`
(`baseline:.../server.cc:2352`, `:2565-2569`).

**Apply idle sleep.** When the apply queue is empty, the apply thread
sleeps 1 ms (`server_h.rs:3372`; `raft_thread_sleep_ms` is `sleep_for`,
`server.cc:892`). Nothing notifies it when entries are enqueued. Baseline:
`std::this_thread::sleep_for(1ms)` (`baseline:.../server.cc:1478`).

**Fixed overhead vs per-byte cost.** Model the latency of an isolated entry
as `T(N) = C + N/B`.

- The *fixed overhead* C is every part whose distribution does not depend
  on N: the two uniform timer waits (send-start wake, apply sleep),
  hand-offs and the fixed-size reply. A part can be random and still fixed.
  The apply sleep, for example, is uniform on 0–1 ms at every size.
- A *quantisation wait* is a third kind: the reply-poll wait. It is bounded
  by the poll step whatever N is, but its value depends on RTT modulo the
  step, and RTT grows with N, so it is neither fixed nor proportional to N.
- The *per-byte cost* N/B is time proportional to N: memcpy, zero-fill,
  allocation and page faults, and syscalls per 64 KiB.
- A *per-batch* cost is paid once per AppendEntries, however many entries
  it carries.

"Timer waits" below means the two uniform waits plus the reply-poll wait.

**Queueing delay.** Time an entry spends waiting for earlier work, not
being worked on. There are two queues. The first is the submit queue
(stage "queued → submit thread"), which costs about 70 µs below saturation
but 8–9 ms p50 unthrottled at 286 KB and 1 MiB
(`raft-latency-breakdown/README.md:107`, `:154`). The second, and
larger, is the stage "appended → send starts". That stage is *wake latency
+ slot wait*: the time for the replication loop to notice the entry, plus
the time until a follower's slot is free and a batch that includes the
entry is sent. Below saturation the slot is free when the entry arrives, so
only the wake remains; at saturation the slot wait dominates. When the
system is full, Little's law gives the *mean* latency: in-flight entries ÷
throughput. The p50 comparisons with it below are therefore approximate.

**Throughput: entries/s vs bytes/s.** Entries/s is applied entries per
second. Bytes/s is entries/s × N, the *goodput* (useful payload bytes
delivered) at one replica. Each byte crosses the network twice, once to
each follower. A limit on entries per batch caps entries/s. A limit on
bytes per batch caps bytes/s.

**Offered rate, saturation.** The *offered rate* is raft_bench's target
submit rate (`--rate`, entries/s across all partitions,
`src/deptran/raft/raft_bench.cc:190-191`). The system is *saturated* when
the offered rate exceeds what it can deliver, about entries per batch ÷ batch
cycle. Queues then grow until raft_bench's *in-flight entry cap*
(`--max-outstanding`, per partition, `raft_bench.cc:143`, `:194`,
`:1178-1179`; 4096 / 256 / 64 for 4 KB / 286 KB / 1 MiB,
`scripts/raft_perf/run_sweep.sh:94-102`) stops new submits. This document
uses one pair of terms: *below saturation* (the "throttled" points, where
the system delivers the offered rate exactly) and *saturated* (including
"unthrottled", where raft_bench submits as fast as the cap allows).

**Lanes.** *rust*: the Rust core on the Rust srpc runtime. *hybrid*: the
same Rust core on the C++ srpc runtime. Both lanes share the Raft
replication loop. They differ in the *seam functions* (the small functions
through which the core calls the runtime) for send, response read, fiber
sleep and event wait (`server_cc.rs:1231-1239`; C++-lane seams in
`server_seam_cpp.cc:261`, `:307`; rust-lane seams in `rt/src/seam.rs`,
whose send goes through `send_append_entries_with` at
`rt/src/seam.rs:359-409` and `rt/src/transport.rs:395-408`), and in the
runtime beneath them: the srpc implementation (Rust vs transpiled C++), the
transport, and the follower's RPC service (`rt/src/service.rs` on the rust
lane, `raft-latency-breakdown/README.md:57`). The per-byte steps (encode,
socket, follower handler) run in that runtime code. Unless stated
otherwise, the trace numbers are for the rust lane.

---

## 3. The latency model

```
latency ≈ timer waits  +  per-byte work  +  queueing
        ≈ (~1.2–2.9 ms)  +  (≈5 ns/B × N)  +  (batches ahead × batch cycle)
```

Here "batches ahead" is entries ahead of yours ÷ entries per batch;
equivalently, queueing ≈ entries ahead ÷ throughput.

### 3.1 Below saturation: timer waits + per-byte work

At the throttled points each AppendEntries carries exactly one entry
(`raft-latency-breakdown/README.md:100-103`). That alone does not make
queueing zero; what does is that each follower's slot is free when the
entry arrives, so "appended → send starts" is only the wake latency, with
no slot wait (§2, *Queueing delay*). Trace: rust lane, commit c23e397e4,
one trial.

| component (p50 µs) | 4 KB @240/s | 286 KB @90/s | 1 MiB @26/s | kind |
|---|---|---|---|---|
| appended → send starts (wake; slot wait ≈ 0) | 561 | 606 | 616 | fixed, 0–1 ms uniform |
| reply arrived → commit (reply poll) | 1119 | 14 | 1669 | quantisation: depends on where the reply lands in the 1 ms poll step (RTT mod step) |
| apply queue → apply thread (apply sleep) | 589 | 588 | 617 | fixed, 0–1 ms uniform |
| **timer waits** | **2269** | **1208** | **2902** | |
| Start(): build Command + append | 12 | 437 | 1620 | per-byte |
| send call (encode + queue) | 13 | 185 | 485 | per-byte |
| send done → follower handler | 95 | 601 | 1383 | per-byte |
| follower handler | 31 | 493 | 1659 | per-byte |
| **per-byte work** | **151** | **1716** | **5147** | |
| hand-offs: queued → submit thread, commit → apply queue, apply thread → callback | 75 | 74 | 77 | fixed |
| follower reply → leader (fixed-size reply) | 75 | 123 | 118 | fixed |
| sum of the part p50s | 2570 | 3121 | 8244 | |
| p50 of the per-entry total (not the sum of the rows) | 2580 | 3151 | 8099 | |

Source: `docs/performance/raft-latency-breakdown/README.md:105-119`. The
subtotals and the sum row are computed here. The last row is the
breakdown doc's own total, the p50 of per-entry totals; a median of sums is
not the sum of medians, so the two rows differ.

What this shows:

- **At 4 KB, 88% of the latency is timers.** The same floor shows up in the
  Jetpack-format runs with 64-byte entries: loopback p50 2.60–2.67 ms at
  N=1 (`docs/performance/jetpack-comparison/README.md:106-112`).
- **The per-byte slope is roughly linear.** Per-byte work grows by 1565 µs
  from 4 KB to 286 KB (5.5 ns/B) and by 3431 µs from 286 KB to 1 MiB
  (4.5 ns/B). That is about 0.2 GB/s end to end, although each step alone
  runs at 0.6–2 GB/s (computed from the table above).
- **The headline curve is not smooth, because the reply poll is not.** The
  reply-poll wait (§2) is the time from the reply's arrival to the next poll
  check. The poll steps start right after the sends, so the wait depends
  on the round-trip time modulo the step length, not on N directly. That
  is why 286 KB paid 14 µs and 1 MiB paid 1669 µs. The simple model is:
  checks happen at k × 1 ms after the send (k = 1, 2, ...), so a reply that
  arrives RTT after the send waits `k × 1 ms − RTT` for the smallest k
  that makes this non-negative. That model does not reproduce these values
  exactly (it cannot give 1119 µs, which is more than one step). The
  effective step is probably longer than 1 ms, because `Fiber::sleep`
  expiries are only checked on poll-loop passes that follow a 1 ms
  `epoll_wait` (**inferred**; see §8).

The sweep shows the same shape over three trials per point (rust lane
9a361eccd vs baseline 412c225a, 1 partition, one group;
`docs/performance/raft-rust-9a361eccd/compare-vs-412c225a.txt`).

| point | rust p50 / p99 ms | baseline p50 / p99 ms | lines |
|---|---|---|---|
| 4 KB @240/s | 2.647 / 3.589 | 2.722 / 3.700 | 222-225 |
| 4 KB @10100/s | 3.269 / 4.580 | 4.337 / 7.537 | 266-269 |
| 286 KB @5/s | 3.266 / 4.101 | 7.543 / 9.623 | 124-127 |
| 286 KB @191/s | 3.137 / 4.300 | 9.537 / 21.117 | 168-171 |
| 1 MiB @1/s | 8.392 / 9.360 | 20.343 / 23.228 | 26-29 |
| 1 MiB @26/s | 7.985 / 10.067 | 17.604 / 23.978 | 46-49 |
| 1 MiB @55/s | 8.560 / 10.270 | 17.434 / 26.324 | 70-73 |

The baseline's floor at 4 KB is about the same (2.72 ms), but its
large-entry latency is much higher: about 17.5 ms at 1 MiB against about
8 ms. Only end-to-end latencies exist for the baseline, so no per-byte
slope was derived for it.
The baseline was never traced per stage (`raft-latency-breakdown/README.md:194-195`),
so which of its steps cost more is not measured. §5.4 lists the known code
differences.

### 3.2 At saturation: queueing dominates

Unthrottled trace, p50 µs (`raft-latency-breakdown/README.md:152-190`):

| | 4 KB | 286 KB | 1 MiB |
|---|---|---|---|
| appended → send starts (wake + slot wait: **queueing**) | 109,270 | 260,046 | 230,952 |
| send call, whole batch | 816 | 25,837 | 23,491 |
| send done → follower handler | 969 | 32,976 | 31,253 |
| follower handler | 2,544 | 18,807 | 17,675 |
| reply arrived → commit | 1,708 | 3,330 | 3,255 |
| total | 116,800 | 352,485 | 317,137 |
| entries per AppendEntries | 256 | 58 | 15 |
| which cap binds | count (256) | bytes (16 MiB) | bytes (16 MiB) |
| batch cycle: send start → commit | 6.4 ms | 83 ms | 78 ms |
| gap between consecutive batches | 7.4 ms | 83 ms | 78 ms |
| throughput (entries/s) | 34,345 | 684 | 189 |

- **Queueing is 73–94% of the latency.**
- **Throughput ≈ entries per batch ÷ batch cycle,** because a follower's
  next batch cannot be sent until its previous reply is processed
  (per-follower stop-and-wait, §2). With 3 replicas the first follower to
  reply sets the pace. Batches to *different* followers do overlap: phase 2
  stops at quorum (`server_cc.rs:1655-1661`), and the slower follower's RPC
  stays outstanding while the next pass sends to the faster one. The right
  cycle length is the gap between consecutive batches: 256 ÷ 7.4 ms ≈ 34.6k/s
  (measured 34,345), 58 ÷ 83 ms ≈ 700/s (684), 15 ÷ 78 ms ≈ 192/s (189)
  (`raft-latency-breakdown/README.md:166-179`). At 4 KB the gap (7.4 ms)
  is longer than send start → commit (6.4 ms); at the large sizes they are
  equal.
- **Mean latency = in-flight entries ÷ throughput (Little's law).** It
  predicts the mean; the measured values are p50s, so the match is
  approximate. Using the
  sweep's unthrottled numbers (`compare-vs-412c225a.txt:22-25`, `:120-123`,
  `:218-221`):
  - 1 MiB: 64 ÷ 183.3/s = 349 ms, measured p50 339 ms.
  - 286 KB: 256 ÷ 670.7/s = 382 ms, measured 379 ms.
  - 4 KB: 4096 ÷ 37,760/s = 108 ms, measured 106 ms.

  So unthrottled latency is mostly **set by raft_bench's in-flight cap**,
  which was chosen per size as a memory bound (`run_sweep.sh:94-102`). It
  is not an intrinsic cost of the entry size.
- **Bytes/s plateaus once the byte cap binds.** Rust lane unthrottled:
  - 286 KB: 670.7 × 286,208 = 192.0 MB/s
  - 1 MiB: 183.3 × 1,048,576 = 192.2 MB/s
  - 4 KB: 37,760 × 4,096 = 154.7 MB/s, lower because the 256-entry count
    cap binds

  Baseline: 79.8 / 71.2 / 50.6 MB/s (computed from the same lines). The
  crossover between the two caps is where `256 × (84 + N) = 16 MiB`, that
  is N ≈ 65,452 B (derived). Below that size a full batch carries fewer
  than 16 MiB and entries/s is capped at 256 per batch cycle. Above it,
  every full batch carries about 16 MiB and its cycle takes about 80 ms, so
  bytes/s is flat and entries/s falls as 1/N.

### 3.3 Where the knee is

The *knee* is the offered rate at which p50 jumps. Here a point is *flat*
if its p50 is within 3x of the p50 at the lowest offered rate for that
size and lane, and *jumped* otherwise. The knee lies between the last flat
and the first jumped rate (1 partition, one group,
`compare-vs-412c225a.txt`; line numbers are the "offered" line of each
point).

| size | rust: last flat | rust: first jumped | baseline: last flat | baseline: first jumped |
|---|---|---|---|---|
| 4 KB | 35,700/s, p50 7.43 ms (:298-301) | none in the rate array; unthrottled 37,760/s, p50 106.5 ms (:218-221) | 11,900/s, p50 5.96 ms (:278-281) | 13,100/s, p50 35.9 ms ±38 (:282-285) |
| 286 KB | 450/s, p50 3.36 ms (:196-199) | 675/s, p50 156.1 ms ±53 (:200-203) | 214/s, p50 10.1 ms (:176-179) | 225/s, p50 25.4 ms ±3.9 (:180-183) |
| 1 MiB | 130/s, p50 7.86 ms, p99 24.6 ms (:98-101) | 195/s, p50 300.4 ms (:102-105) | 65/s, p50 22.4 ms (:82-85) | 72/s, p50 172.9 ms ±195 (:86-89) |

The rust 4 KB point at 35,700/s is flat by this threshold (2.8x its
low-rate 2.65 ms) but already rising, and its p99 is 78 ms ±59. The rust
1 MiB point at 130/s has a flat p50 but a p99 of 24.6 ms.

In bytes/s the rust knee is nearly independent of size once the byte cap
binds: between about 130 MB/s (last flat: 450 × 286,208 B, 130 × 1 MiB)
and about 193–204 MB/s (first jumped), against about 192 MB/s measured
unthrottled capacity. Below the knee, size affects latency only through the per-byte
term. Above it, size affects latency through throughput, via Little's law.

---

## 4. Summary: which part dominates

| | small entries (≤ ~64 KB) | large entries (≥ ~286 KB) |
|---|---|---|
| **below saturation** | timer waits (~2.3 ms of ~2.6 ms at 4 KB) | per-byte work (5.1 of 8.1 ms at 1 MiB), timers still 1.2–2.9 ms |
| **saturated** | queueing; batch cycles are short (7.4 ms gap) but a batch is capped at 256 entries | queueing; batch cycles are ~80 ms, because a batch is capped at 16 MiB |

---

## 5. How the current implementation influences it

For each mechanism: where it is in the current tree, whether the baseline
has it, and what it does to the size-latency curve.

### 5.1 Per-follower stop-and-wait

- **Current:** `server_cc.rs:1104-1107` skips a follower with an occupied
  slot. `:1242-1244` places the RPC in the slot. `:1646-1648` releases it
  in phase 2. Releasing a slot whose RPC was sent in an older pass only
  sets `retry_released_follower` (`:1649-1650`); after the phase-2 poll
  loop ends, `RequestReplication()` is called once so the next pass starts
  without waiting out the idle interval, unless phase 2 stopped because
  leadership was lost (`:1680-1687`). The next pass still runs phase 3 and
  `HeartbeatWait` first.
- **Baseline:** same design (`baseline:.../server.cc:2044-2046`).
- **Effect:** one batch's per-byte work never overlaps the next batch's
  work for the same follower. When saturated this makes throughput ≈ one
  batch per batch cycle and queueing ≈ batches ahead × batch cycle (§3.2).
  Below saturation it has no effect: each entry finds its follower's slot
  free.
- **Correction to the existing breakdown doc.** That doc says "the next
  round starts only after the previous one commits"
  (`raft-latency-breakdown/README.md:18-21`). The code gates each
  *follower*, not the whole loop pass. A follower's next batch waits only for that
  follower's own reply. With 3 replicas, the first follower to reply is the
  majority, so for that follower "reply processed" and "commit" happen in
  the same pass. The second follower runs its own independent
  stop-and-wait and does not gate commit.

### 5.2 Batch caps (count and bytes)

- **Current:** 256 entries and 16 MiB, each entry counted as 84 + N
  (`server.cc:216-237`, `:1426-1437`; loop at `server_cc.rs:966-995`). The
  source comment says the byte cap exists because 256 × 286 KB ≈ 73 MB
  exceeds srpc's 64 MiB frame limit, which caused a lagging follower to be
  re-sent the same unsendable batch forever. It also estimates about 30 ms
  per 16 MiB on loopback (`server.cc:216-222`). The measured batch cycle
  at 16 MiB is about 80 ms (§3.2). The frame limit is `kMaxFramePayloadSize = 64 * 1024 * 1024`
  (`src/srpc/rpc/frame_codec.rs:86`).
- **Baseline:** count cap only (`baseline:.../server.cc:553-560`, loop
  condition at `:2237-2239`; no byte term). Its rrr frame limit is
  `0x7fffffff` (`baseline:src/rrr/rpc/frame_codec.rs:10`). Whether baseline
  production actually linked this `.rs` transport is **not verified**.
- **Effect:**
  - For N below about 64 KB the count cap binds, so entries/s is capped per
    batch cycle.
  - For N above about 64 KB the byte cap binds, so each full batch is about
    16 MiB, its cycle about 80 ms, and bytes/s is capped.
  - Because at least one entry always goes out, a single entry larger than
    about 64 MiB would be refused by the frame limit on every send, which
    would stall that follower permanently (**inferred** from
    `server_cc.rs:986-993` and `tcp_channel.rs:872-874`; not tested). Real
    Mako entries top out near 25 MB (§6).

### 5.3 Timers and polls (the fixed overhead)

All three waits on an isolated entry's path are inherited from the
baseline. The two uniform ones do not depend on N; the reply-poll wait is
bounded by the step but its value depends on RTT (§2, *quantisation
wait*).

| wait | current | baseline | measured p50 |
|---|---|---|---|
| wake → poll thread (channel add, `epoll_wait` 1 ms timeout) | `server_h.rs:4691`, `:3607`; `reactor.rs:2215-2219`; `epoll_wrapper.rs:123-124` | `baseline:.../server.cc:1192`; `baseline:src/rrr/reactor/epoll_wrapper.rs:101` (1 ms `epoll_wait`); `baseline:.../server.cc:1192` **not re-read** | 561–616 µs |
| reply poll, 1000 µs steps | `server_cc.rs:1505`, `:1662-1677` | `baseline:.../server.cc:2352`, `:2565-2569` | 14–1669 µs, depends on where the reply lands in the poll step |
| apply idle sleep, 1 ms, no notify | `server_h.rs:3372` | `baseline:.../server.cc:1478` | 588–617 µs |

(The breakdown doc cites the apply sleep as `src/server_h.rs:3397`. Its line
numbers refer to c23e397e4 with an uncommitted trace patch applied
(`raft-latency-breakdown/README.md:65-67`). In the unpatched current tree
the sleep is at `:3372`.)

Two cases where size does change what a timer costs:

- **The phase-2 deadline (5 ms).** A pass whose replies take longer than
  5 ms leaves phase 2 at the deadline (`server_cc.rs:1662-1668`). Later
  passes whose followers are all still occupied send nothing and leave
  phase 2 without sleeping, because `waiting_for_current_round` is set only
  for RPCs sent in the current pass (`:1549-1552`, `:1656-1661`). The late
  reply is then seen on the next wake, or after up to one 5 ms loop idle
  interval. This probably explains the
  unthrottled "reply arrived → commit" of 3.3 ms p50 / 5.5 ms p90 at
  286 KB and 1 MiB, against 1.7 ms at 4 KB
  (`raft-latency-breakdown/README.md:161`). **Inferred**, not traced.
- **The phase-3 commit piggyback.** The AppendEntries sent in a pass carry
  the commit index from phase 0. When phase 3 advances it, one extra pass is
  requested so followers learn it (`server_cc.rs:1782-1786`). This is per
  pass, not per byte.

### 5.4 Copies and encoding (the per-byte cost)

This is the path of one N-byte entry, rust lane. Every step that grows
with N is a memcpy, a zero-fill, an allocation or a syscall. Nothing
encodes the payload one byte at a time: strings are written as a length
followed by one bulk `write_bytes`.

| # | where | what | current cite | baseline |
|---|---|---|---|---|
| 1 | leader, submit | `entry.payload.assign(log, len)` into the submit queue. Only on the submit-thread path (the one raft_bench uses); without the submit thread `EnqueueLog` calls `Submit` directly with no copy (`raft_worker.cc:778-785`) | `raft_worker.cc:787-791` | same (`baseline:.../raft_worker.cc:701-705`) |
| 2 | leader, Start | `EncodeApplicationLog`: 20-byte header + N copied into `LogEntry` | `application_log.cc:33-56` | same |
| – | leader log, batch | handles only; batch stamps a new `TpcCommitCommand` per entry (per entry, not per byte) | `server.cc:1450-1471` | `const_cast` term write into the shared command (`baseline:.../server.cc:2273`) |
| 3 | leader, per follower | serialize into a fresh request `Vec` that starts at 64 B and doubles | `src/srpc/rpc/client.rs:699`, `:2425-2434` | not re-read |
| 4 | leader, per follower | append `[len][frame]` to the connection's outbound `Vec` (resize zero-fill + copy) | limit checks in `tcpconn_send_frame`, `src/srpc/rpc/tcp_channel.rs:864-899`; the resize and `copy_nonoverlapping` in `tcpconn_append_frame`, `:1200-1212` | same resize, then a source-level byte loop (`baseline:src/rrr/rpc/tcp_channel.rs:825-831`); whether it compiles to memcpy, and whether this file was linked in the baseline, are **not verified** |
| 5 | kernel | `send(2)` / `recv(2)`, `recv` into a 64 KiB scratch buffer | `tcp_channel.rs:779` (`kRecvScratchBytes`); the send side was **not re-read** | **not verified** |
| 6 | follower | frame bytes → `Request.body` via `extend_from_slice` | `src/srpc/rpc/server.rs:1348-1351` (`request_fill_body`) | **not verified** |
| 7 | follower | decode: `std::string` rebuilt per entry (`resize`, so zero-fill, then copy) | `src/srpc/misc/serializable_support.hpp:90-95` | – |
| – | leader apply | zero-copy pointer into the stored `LogEntry`, except the rare safety-fail replay copy | `raft_worker.cc:1150-1156` | same |

Consequences for the curve:

- **Copies 3 and 4 are paid once per follower, one after the other, on the
  replication fiber.** Phase 1 encodes follower 1's batch, then follower
  2's (`server_cc.rs:1231-1239` inside the per-follower loop). The socket
  write is deferred: `send_frame` only sets `pending_write_update_`
  (`tcp_channel.rs:896-898`). On the rust lane that flag is served by the
  same poll thread that runs the fiber, so follower 1's bytes probably
  start leaving only after follower 2 has also been encoded
  (**inferred**). At 1 MiB unthrottled this depends on the open question
  in §8: if the traced 23 ms "send call, whole batch" is one follower's
  encode, the second follower adds about 23 ms more before follower 1's
  bytes leave; if it already covers both, that serial cost is included in
  the 23 ms.
- **The measured per-step speed (0.6–2 GB/s) is well below memcpy speed.**
  This suggests allocation and first-touch page faults on fresh N-sized
  buffers, because several copies land in newly allocated memory. This is
  **inferred and not verified**; see §8.
- **Partial writes memmove the unsent tail** (`tcpconn_trim_sent`,
  `tcp_channel.rs:1215-1227`). For a frame much larger than the socket send
  buffer that is superlinear in frame size. The loopback send-buffer size
  was not checked, so whether this matters is **not verified**.
- **The 4 MiB outbound high-water mark** (`tcp_channel.rs:36`) is checked
  only before an append (`:882-884`). A 16 MiB batch is accepted, but any
  other send on that connection while 4 MiB or more is still queued
  returns `WouldBlock`. Stop-and-wait normally prevents a second send
  queuing behind a large frame. Whether a phase-2 deadline can trigger one
  is **not verified**.
- **Baseline vs current.** The baseline's source-level byte loop in the
  outbound copy may be part of its higher large-entry latency (17.5 ms vs
  8 ms at 1 MiB), but only if it did not compile to memcpy and the file was
  linked, neither of which is verified. Because the baseline was never
  traced, how much of the gap it explains is **not measured**.
- **Hybrid vs rust.** Paired runs at an older, dirty commit (b6141e72)
  show the hybrid lane about 2x slower on large entries: 286 KB @45/s p50
  6.29–6.33 ms against 3.01–3.12 ms, and unthrottled 1 MiB 107–114/s
  against 183–188/s (`docs/performance/raft-rust-t4-paired/large-payload.txt:7-14`,
  latencies in µs). The file labels the two arms only `build` and
  `build_rust`; that `build` is the hybrid lane, and which payload size and
  rate each block is, are **not verified** from the file itself. The C++ runtime is transpiled from the same srpc `.rs` sources,
  so the copy count should be equal. The cause of the gap is **not
  verified**.

### 5.5 The apply thread

- **Current:** `EnqueueCommittedEntries` pushes handles
  (`server_h.rs:3837`). The apply thread pops them and sleeps 1 ms when the
  queue is empty (`:3372`). The callback gets a pointer into the stored
  bytes, with no copy (`raft_worker.cc:1142-1167`).
- **Baseline:** same structure (`baseline:.../server.cc:1464-1479`).
- **Effect:** about 0.6 ms p50 of fixed overhead at every size below
  saturation. Under backlog it is still paid, once per committed burst:
  commits arrive about once per batch cycle, the apply thread drains the
  burst and goes back to its 1 ms idle sleep (`server_h.rs:3362-3378`). The
  unthrottled trace shows 813 / 663 / 596 µs p50 at 4 KB / 286 KB / 1 MiB
  (`raft-latency-breakdown/README.md:162`): small next to queueing, but not
  zero. Size-neutral: measured "commit → apply queue" and
  "thread → callback" are 2 µs at every size
  (`raft-latency-breakdown/README.md:115-117`).

### 5.6 Contrast: Jetpack's Raft

| | Mako (both trees) | Jetpack |
|---|---|---|
| outstanding AppendEntries per follower | 1 (stop-and-wait) | up to `RAFT_PIPELINE_CAP` = 8000, pipelining on unless `RAFT_PIPELINE_OFF` (`jetpack:src/deptran/constants.h:212-231`) |
| send watermark | `next_index` only | `sent_index_` runs ahead of `next_index_` (`jetpack:.../raft/server.h:48-63`) |
| batch bound | 256 entries / 16 MiB (current), 256 entries (baseline) | every entry from `send_idx` to `lastLogIndex`, no cap (`jetpack:.../raft/server.cc:740-758`) |
| reply detection | 1 ms poll | a coroutine per RPC blocks on `r->Wait(60 s)` and handles the reply when it fires (`jetpack:.../raft/server.cc:802-803`) |
| commit wait | apply thread with 1 ms idle sleep | the coordinator polls `commitIndex` behind a 1000 µs `TimeoutEvent` (`jetpack:.../raft/coordinator.cc:120-123`) |

Pipelining lets consecutive batches to one follower overlap, so throughput
is not capped at one batch per batch cycle. Jetpack's unbounded batch means
a lagging follower can be sent an arbitrarily large message. Jetpack still
has a 1 ms poll, on the client side.

**Why the Jetpack-format test says little about size.** Every arm of the
Jetpack comparison ran with 64-byte entries
(`docs/performance/jetpack-comparison/README.md:40-41`). At 64 bytes the
per-byte term is about 0.3 µs by the slope in §3.1, so those runs measure
only the fixed overhead and the per-message RPC cost. Their loopback p50
of 2.6–3.4 ms (`:106-112`) is the same ~2.3–2.6 ms timer floor seen at
4 KB. The native Jetpack runs' payload size was **not verified**.

---

## 6. Mako's real entry sizes

From the Mako Raft entry-size profile notes (removed; see git history) (TPC-C via dbtest, 6
workers, one run, Release `MODE=perf` build logging at DEBUG level,
profile `:15-18`):

- **An entry is a batch of transactions.** STO accumulates up to 400
  transactions (`MAKO_BATCH_SIZE`) into one buffer and flushes it as one
  Raft entry. The flush trigger is "400 transactions or 90% of a
  50,380,812-byte buffer" (profile `:59-77`, citing
  `src/mako/sto/Transaction.cc:636-653` and `Transaction.hh:87`; those
  source lines were not re-read).
- **Size distribution.** p50 is 281,088–287,232 B per partition, p90
  about 304–307 KB and p99 320,000–328,192 B. Rare outliers reach
  13,124,336 B and 24,924,008 B, and 20 entries exceeded 8 MiB.
- **Rate.** About 50 entries/s per partition and about 303/s aggregate,
  roughly 85 MB/s. Raft in Mako carries many bytes but few messages.
- **Implication.** Real traffic sits in the byte-cap regime, above the
  ~64 KB crossover. One 13 MB entry alone fills most of a 16 MiB batch;
  one 25 MB entry exceeds the cap and is sent as a one-entry batch. Using
  the measured ~5 ns/B slope, a 25 MB entry would carry about 125 ms of
  serial per-byte work (**extrapolated, not measured**).
- **raft_bench uses one fixed size per run.** It allocates a single
  `payload(opt.payload_bytes)` buffer (`raft_bench.cc:1141`). The sweep
  runs 4096, 286208 and 1048576 separately. 286208 is the p50, chosen over
  the mean because the outliers inflate the mean (`run_sweep.sh:83-92`).
  The harness deliberately does not reproduce the tail (profile `:101-104`).
  Mixed sizes, where one 13 MB entry shares a batch with ordinary entries,
  have never been measured.

---

## 7. What would change the curve

This section is **analysis, not measurement.** Each option would be a
separate decision, and none is recommended here.

| option | term it targets | expected effect |
|---|---|---|
| **Pipelining** (several AppendEntries per follower, as in Jetpack) | queueing when saturated | Consecutive batches to one follower overlap, so throughput is no longer one batch per batch cycle. The encode of batch k+1 could overlap the transfer and follower decode of batch k. At large N, bytes/s would then be limited by the slowest stage (currently about 23–31 ms of an ~80 ms batch cycle) rather than by their sum. By Little's law, latency at a fixed in-flight cap would fall by the same ratio. No change below saturation. Needs out-of-order-safe match/next updates and a bound on outstanding bytes (the frame limit and high-water mark still apply). |
| **Event-driven reply wake** (reply callback wakes the fiber) | quantisation wait | Removes the 0–1.7 ms reply-poll wait, whose value depends on where the reply lands in the poll step. Below saturation that is up to ~1.7 ms off every entry, and it removes the non-monotone dip at 286 KB. At large N it also removes the fall-back to noticing a late reply only on the next wake or loop idle interval (5 ms) after the phase-2 deadline. |
| **Event-driven apply wake** (condvar instead of 1 ms sleep) | fixed overhead | About 0.6 ms p50 / 1 ms p90 off every entry below saturation. When saturated, about 0.6–0.8 ms p50 off each committed burst (§5.5), small next to queueing. |
| **Wake the poll thread on job add** (eventfd/pipe instead of channel-only add) | fixed overhead | About 0.6 ms p50 off "appended → send starts" below saturation. The mechanism is inferred (§5.3). |
| **Encode once for all followers** | per-byte, × followers | The phase-1 encode is paid once instead of F times (F = number of followers). At 1 MiB unthrottled that saves one follower's encode per batch cycle: ~23 ms if the traced 23 ms is one follower's encode, about half that if it covers both (§8), and only if the inferred serialization in §5.4 is right. |
| **Fewer or pre-sized copies** (pre-sized request `Vec`, no zero-fill before copy, reuse follower buffers) | per-byte slope | Lowers the ~5 ns/B slope. How much depends on whether the cost is page faults or memcpy (§8). |
| **Tune the byte cap** | batch cycle at large N | The cap bounds one batch. A smaller cap gives shorter batch cycles, and so lower queueing latency per batch, but more batches. Under stop-and-wait, bytes/s would fall if the fixed per-batch cost matters. A larger cap does the opposite, up to the 64 MiB frame limit and the election-timeout ceiling (a batch whose transfer takes longer than the election timeout could trigger a spurious election; §2, *Term*). |

Together, the three event-driven wake-ups would remove most of the
1.2–2.9 ms of timer waits. That matters most for small entries. Pipelining matters most
at saturation, for any size.

---

## 8. Not measured and open questions

- **Missing sizes.** Nothing between 4 KB, 286,208 B and 1 MiB was
  measured, and nothing above 1 MiB. The ~64 KB count/byte crossover is
  derived, not observed.
- **Baseline and hybrid breakdown.** There is no per-stage trace for the
  baseline or the hybrid lane. They show roughly 2x higher large-entry
  latency or lower large-entry throughput than the rust lane (§3.1, §5.4),
  but no per-byte slope was derived for either, and the cause is **not
  measured**.
- **Why each step runs at only 0.6–2 GB/s.** Page faults on fresh buffers,
  allocator behaviour (jemalloc vs glibc in raft_bench and in raft-rt) or
  copy cost? **Not verified.** A `perf stat -e page-faults` plus
  `perf record` at 1 MiB would settle it.
- **Poll-step and wake granularity.** The effective length of
  `Fiber::sleep(1000)` on an idle poll thread, and the delay from
  `RequestReplication` to fiber resume, are **not measured**. Both are
  inferred to be ~1 ms+ from the 1 ms `epoll_wait`.
- **Phase-2 deadline fall-back.** That it explains the unthrottled 3.3 ms
  "reply → commit" at large N is **inferred**. It needs a trace of passes
  whose phase 2 exits by deadline.
- **Per-follower encode serialization.** Whether follower 1's `send(2)`
  waits for follower 2's encode (§5.4) is **inferred**. It needs a stamp at
  the first `handle_write`.
- **"Send call" scope.** Whether the traced 23–26 ms "send call" is one
  follower or both is **not verified**.
- **Follower persistence.** The breakdown does not include whether
  followers or the leader persist entries (e.g. RocksDB) before replying.
  All runs were memory-only on loopback. Real NIC bandwidth and RTT, and
  disk, were **not measured**.
- **Pipelining in Mako** has never been measured.
- **Multi-partition.** At 6 partitions with per-partition groups, the rust
  lane gives p50 2.73 / 3.35 / 9.89 ms at 4 KB@10000/s, 286 KB@190/s and
  1 MiB@55/s (`compare-vs-412c225a.txt:509-513`, `:411-415`, `:303-307`).
  Capacity was measured by the unthrottled 6-partition points: 851/s at
  1 MiB (about 892 MB/s aggregate, p50 432.8 ms), 3,132/s at 286 KB (p50
  483.9 ms) and 191,532/s at 4 KB (p50 117.7 ms) (`:309-313`, `:417-421`,
  `:515-519`). Only the throttled rate arrays at 6 partitions stop short
  of the knee.
- **Trial counts.** The trace is one trial at c23e397e4. The sweep is 3
  trials at 9a361eccd. Their 4 KB unthrottled throughput differs (34,345/s
  vs 37,760/s), and the cause was **not verified**.
