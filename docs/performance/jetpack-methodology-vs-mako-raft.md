# How the Stony Brook group tests consensus, and whether it fits Mako's Raft

Question: Shuai Mu said the group tests consensus performance with the
Jetpack methodology.
1. What is that methodology?
2. Is it a consistent way to test the Raft that Mako uses? Mako puts very
   different entries into Raft and applies them very differently. Does
   Jetpack even apply its entries?

Sources:
- The papers below, read from their PDFs.
- The Jetpack repository (`github.com/stonysystems/jetpack`, branch
  `jetpack`, head `c03e318e`, 2026-06-07; the local clone is `~/jetpack`
  and matches the remote).
- This repository's `src/mako`.

File references are to those trees. Where something was not checked, it is
marked.

---

## Short answers

1. **The methodology** (Jetpack, OSDI '26, and its repo):
   - 5 replicas across AWS regions, with the leader in region 0;
   - 60 client sites on all 10 hosts;
   - **open-loop** clients issuing tiny single-key reads and writes, 50/50;
   - one run of 30 s per point, statistics from the middle 10 s;
   - latency measured at the client, from dispatch to reply;
   - throughput = replies in the middle 10 s / 10 s, summed over hosts.

   The load knob is `n_concurrent`: each of the 60 sites offers about
   `n_concurrent` requests per second. The swept result is a
   throughput-latency curve. A fixed "knee" concurrency is then used for
   the skew and contention experiments.

2. **Yes, Jetpack applies every entry.** Each committed Raft entry is
   applied on every replica: a single-key read or write against an
   in-memory table. The leader answers the client only after its own
   apply. Apply runs inline in the Raft loop, not on a separate thread
   (§3).

3. **Not directly consistent with how Mako uses Raft.** The two systems use
   Raft for different jobs:
   - Jetpack uses Raft as a client-facing replicated state machine: one
     small command per entry, a per-command reply, and a cheap apply.
   - Mako uses Raft as a background bulk-log stream:
     - one entry = a batch of up to 400 transactions' write-sets;
     - one Raft group per worker core;
     - no per-entry reply; the leader's "apply" only advances a
       watermark;
     - followers replay the write-sets into Masstree, gated by that
       watermark.

   A Jetpack-style test of Mako's Raft measures a regime Mako never runs
   in, and leaves out the costs that dominate Mako's regime: bytes per
   entry, and follower replay. §5 lists what a consistent test would keep
   and change.

---

## 1. The papers, and how each one tests consensus

From Shuai Mu's publication list (mpaxos.com), the consensus and
replication papers of the four people asked about:

| paper | authors of the four | what the evaluation does with consensus |
|---|---|---|
| **Jetpack: Consensus Made Generally Fast**, OSDI '26 | Ze Tang, Zihao Zhang, Weihai Shen, Shuai Mu | The methodology in question (§2). Raft is "built on DepFast". |
| **DepFast: Orchestrating Code of Quorum Systems**, ATC '22 | Weihai Shen, Shuai Mu | DepFast-Raft vs etcd. Its protocol is in the rows below. |
| **Mako: Speculative Distributed Transactions with Geo-Replication**, OSDI '25 | Weihai Shen, Shuai Mu | Replication is not benchmarked on its own. It is measured through TPC-C transaction throughput and latency (§4). |
| **Rolis**, EuroSys '22 | Weihai Shen, Shuai Mu | replicating multi-core transactions; *not read for this report* |
| **Fault-Tolerant Replication with Pull-Based Consensus in MongoDB**, NSDI '21 | Shuai Mu | *not read for this report* |
| **On the Parallels between Paxos and Raft**, PODC '19 | Shuai Mu | *not read for this report* |
| **AutoMan**, SOSP '25 | Zihao Zhang, Shuai Mu | verified distributed systems; not a performance evaluation of consensus |

DepFast's own protocol (ATC '22, §6.1) differs from Jetpack's in ways that
matter here:
- Azure, 3 and 5 replicas, and the server process bound to one core.
- "A single K-V, 100% write workload".
- "Each trial runs for 120s and is repeated for 3 times … the median
  throughput of the 3 trials, with the error bars showing the deviation".
- "The server replies to the client after the log entry has been
  persisted on disk."

So DepFast repeated trials and showed error bars; Jetpack's pipeline does
not (§2.4).

---

## 2. The Jetpack methodology, precisely

### 2.1 What the paper says (§6.2)

- **Implementation:** C++. "For Raft and Copilot, we built on DepFast."
  Mencius is in the same codebase; MongoDB is 8.2.0.
- **Testbed:**
  - AWS EC2 in 10 regions;
  - replicas in regions 0-4, clients in all 10;
  - 8 vCPU / 16 GB instances;
  - the Raft, etcd and ZooKeeper leaders in region 0.
- **Workload:**
  - YCSB-inspired, "50% reads and 50% writes over 1M Zipfian-distributed
    keys by default";
  - sweeps of skew 0.5-1.0 and key range 100-1M;
  - "60 open-loop clients with multi-threaded request issuance".
- **Figure 10**, the cross-protocol comparison: each replica's consensus
  thread pinned to one core, "a uniform 50/50 read/write workload over 1 M
  keys". The result: Raft ~355 ms at low load and saturating at ~12 k
  ops/s; the 1-RTT protocols at 165-210 ms.
- **Figure 13** reports CPU and memory overhead.
- **What latency means:** a command is answered once "finalized by the
  original protocol (committed and executed)" (§5). After commit, commands
  "are executed and applied to the state machine".

### 2.2 What the repository does

The driver is `scripts/10-run_all.sh` + `scripts/experiment_defs.sh`, and
`scripts/camera-ready/settings.md` is the paper's run sheet. One run starts
`build/deptran_server` on every host with a stack of config files:

```
-f <protocol>.yml  -f client_open_<proto>.yml  -f 60c1s5r10p.yml
-f <workload>.yml  -f concurrent_<N>.yml  -f YCSB_A.yml  -m <mode>  -d 30
```

**Client model: open loop.**
- `derive_client_config` (`experiment_defs.sh:217`) always picks
  `client_open_<suffix>.yml`, or falls back to `client_open.yml`. The
  closed-loop config is never used.
- The active client is `ClientWorker::Work` at `client_worker.cc:198`. A
  second, rate-based `Work()` at `:537` is inside a `/* … */` comment, so
  the `rate: 1000` field in the configs has no effect.
- Each client site runs `n_concurrent` coroutines. Each coroutine repeats:
  dispatch one request, then sleep a random 0.5-1.5 s (`:450`), whether or
  not the reply has come back.
- A site pauses issuing while it has `max_undone` requests outstanding
  (`:372`).
- The config states the resulting load itself (`config/client_open_raft.yml`):

  > aggregate offered tput = 60 × min(c, max_undone) … Raft: ~12k r/s sat
  > → 60×300 = 18k cluster cap.

So **"concurrency" is an offered-rate knob**: N = 150 offers about 9,000
ops/s. The "throughput vs concurrency" curves are throughput and latency
against offered load.

**Requests:**
- `bench: rw` (`src/bench/rw/`): one row of table `history`, an integer key
  and an integer value.
- The `rw_<N>` configs set the key range (`rw_1000000` = 1 M keys,
  uniform). The `rw_zipf_<θ>` configs keep 1 M keys and pick them with a
  Zipf distribution.
- `YCSB_A.yml` overrides the mix to 50/50.
- Payloads are about 8 bytes. An opt-in `RW_VALUE_SIZE` env var pads writes
  (`workload.cc:62`), but no script or result uses it. **Nothing measures
  or reports bytes.**

**Timing, all client-side:**
- The clock is `gettimeofday` in ms.
- It starts at dispatch (`none/coordinator.cc:57`) and stops when the
  dispatch reply arrives (`:75-80`).
- Only requests dispatched in the middle third of the run count
  (`latency_window`, `:46`).
- Throughput = counted replies / 10 s per host, summed over hosts.
- Latency percentiles are the median across hosts
  (`gen_tput_p90_figures.py`). The CDFs pool the per-host samples.

### 2.3 The experiments and the knee

The camera-ready matrix (`settings.md` §3), per protocol:

| experiment | varies | held fixed |
|---|---|---|
| Exp 0 | concurrency, i.e. offered rate. Raft: 1, 10, 20, 40, 60, 80, 100, 120, 140, 150, 160, 170, 180, 190, 200, 250, 300, 400, 500, 750, 1000, 1250, 1500, 2000 | `rw_1000000`, uniform |
| Exp 1 | Zipf θ 0.5-1.0 | the knee concurrency |
| Exp 2 | key range 1-1 M | the knee concurrency |

Each point runs as vanilla (`none_raft`, `-m 0`) and as Jetpack
(`rule_raft`) with `-m 0/100/101`. The knee is set by hand on AWS (Raft
150). On the Zoo path it is computed by `scripts/derive_fixed_conc.py`:

> the largest concurrency whose median (p50) latency does not exceed
> `LATENCY_MULTIPLIER` (= 2.0) times the baseline (lowest-concurrency) p50

The knee is taken from the vanilla protocol and reused for every variant.

### 2.4 What the pipeline does not do

- **One run per point.** Runs are retried only on failure, and there are
  no repeated trials or error bars. Compare DepFast: 3 trials, median,
  error bars.
- **Bytes and bandwidth** are neither varied nor reported. The only value
  size ever run is about 8 bytes.

---

## 3. Does Jetpack apply the entries? Yes

The path for vanilla Raft (`none_raft`, `cc: none, ab: raft`), in the
repository's `src/deptran`:

1. **The leader executes first.** `SchedulerNone::Dispatch`
   (`none/scheduler.cc:11`) calls `SchedulerClassic::Dispatch`, which runs
   the request's read/write piece against the local in-memory database
   `mdb`. A read's result is ready at this point.
2. **The commit is replicated through Raft.** `OnCommit`
   (`classic/scheduler.cc:221`) wraps the command in a `TpcCommitCommand`,
   hands it to the Raft coordinator (`coo->Submit`, which calls
   `RaftServer::Start`), and blocks on `commit_result->Wait()`. There is
   one command per log entry; AppendEntries batches entries (camera-ready
   builds: batching on, pipelining off).
3. **Apply runs inline.**
   - On the leader, `HeartbeatLoop` advances `commitIndex` and calls
     `applyLogs()` right there, holding `mtx_` (`raft/server.cc:700`).
   - On a follower, the AppendEntries handler calls `applyLogs()` when the
     leader's commit index moves (`:1413`).
   - `applyLogs` (`:641`) walks the new entries and calls `app_next_` →
     `SchedulerClassic::Next` → `CommitReplicated` (`classic/scheduler.cc:302`).
4. **What "apply" does:**
   - On a follower, `CommitReplicated` re-dispatches the command against
     its own `mdb` (`SchedulerClassic::Dispatch` + `DoPrepare`), then
     `DoCommit`: the local transaction commits into the in-memory table.
   - On the leader it only calls `DoCommit`, since the piece already ran
     in step 1.
   - Either way, it then sets `commit_result`.
5. **The reply** leaves after the leader's apply sets `commit_result`, so
   `OnCommit` returns and the dispatch RPC replies.

So the measured latency is:
- client → leader RPC, plus the leader's local execution;
- Raft replication to a majority (1 WAN RTT to followers);
- commit and the leader's apply of that entry;
- leader → client reply.

**Reads are replicated too.** Every read goes through Raft, except under
the read-lease variant (`IsRaftReadLease`), which skips replication.

**Apply cost is tiny and synchronous:** one key, in memory, inside the
replication loop. Jetpack's paper states the same model: commands commit,
then "are executed and applied to the state machine".

---

## 4. How Mako uses Raft

### What goes into an entry (`src/mako/sto/Transaction.{hh,cc}`)

- Each worker thread keeps its own replication stream: one Raft group per
  core, `partition_id` = the core's partition.
- A committed transaction's write-set is appended to the thread's
  `StringAllocator` buffer: table id, key-value pairs, and the commit
  timestamp.
- The buffer is submitted with `add_log_to_nc` as one Raft entry when
  either of these is true (`checkPushRequired`, `Transaction.hh:174`):
  - it holds `batch_size` transactions (`large_batch_num = 400`,
    `Transaction.cc:639`, matching the paper's "400 in our
    implementation");
  - it reaches 90% of `MAX_ARRAY_SIZE_IN_BYTES`, about 50 MB
    (`Transaction.hh:87`).
- So an entry carries up to 400 transactions' write-sets. Its size depends
  on the workload's write-sets; it was not measured for this report.
- **Control entries** go through the same streams:
  - a no-op per partition at an epoch change;
  - an "advancer" marker (`len == ADVANCER_MARKER_NUM`);
  - a zero-length "ending" entry;
  - loading-phase entries.

### What "apply" means (`src/mako/mako.hh`)

- **Leader callback** (`register_paxos_leader_callback`, `:345`): no replay.
  It reads the batch's latest commit timestamp and publishes it
  (`local_timestamp_[par_id].store(...)`, `:417`). That feeds Mako's
  replication watermark, which decides when speculatively executed
  transactions are safe (the paper's §4.2-4.4).
- **Follower callback** (`register_paxos_follower_callback`, `:180`):
  - reads the batch's commit timestamp;
  - if the watermark `safety_check` passes, **replays the whole batch into
    Masstree** (`treplay_in_same_thread_opt_mbta_v2`, `:271`);
  - otherwise queues it in `un_replay_logs_` for later.

  No-ops additionally run a cross-shard watermark exchange through
  `NFSSync` keys.
- **Clients never wait on a Raft reply.** Transactions execute
  speculatively and replicate in the background. A client-visible commit
  waits for the watermark to pass the transaction's timestamp. The paper
  (§7.3) splits TPC-C latency (median 121 ms at batch 600) into ~50 ms WAN
  RTT, 13 ms of batching, and "the rest to advance the vector watermark".

### How Mako's paper tested it (OSDI '25 §7.1)

- Azure 32-vCPU VMs; 50 ms injected RTT between 3 groups of servers;
- each shard replicated to 1 leader, 1 learner and 2 followers;
- TPC-C and a microbenchmark;
- metrics: transaction throughput, latency, scalability and recovery.

Replication appears only through end-to-end transaction numbers and the
batching / watermark latency breakdown. There is no stand-alone Raft or
Paxos benchmark.

---

## 5. Is Jetpack's methodology consistent for Mako's Raft?

| | Jetpack's Raft test | Raft as Mako uses it |
|---|---|---|
| role of Raft | client-facing replicated state machine | background log stream behind speculative 2PC |
| groups | 1 Raft group, 5 replicas | one group per worker core, per shard |
| entry | one command (1 key, ~8 B) | a batch of up to 400 transactions' write-sets (size set by the workload, up to ~50 MB) |
| submit rate | one entry per client request | one entry per 400 transactions per core |
| who waits for commit | the client, per request | nobody per entry; the watermark advances |
| leader apply | commit the in-memory KV write, then reply | publish the batch's timestamp to the watermark |
| follower apply | re-execute one key in memory, inline | replay the batch into Masstree, gated by the watermark; may be deferred |
| where apply runs | inline in the replication loop, under the Raft lock | Raft's apply thread calls the embedder callback (`RaftWorker::Next`) |
| control entries | none in steady state | no-ops, advancer markers, end markers, loading |
| load model | open loop, 60 × N req/s | batches whenever 400 transactions accumulate |
| latency measured | client dispatch → reply after leader apply (ms) | not measured per entry; the transaction latency includes batching and the watermark |
| bytes | ignored | the dominant cost at large entries |
| repetitions | 1 run per point | (Mako's paper: not stated for the replication part) |

**Consistent parts, worth keeping:**
- A sweep of offered load, which shows the knee.
- Client-side end-to-end latency with warm-up and drain excluded.
- Multi-region placement, or an injected RTT.
- Pinning the consensus thread to a core, which makes CPU-bound saturation
  comparable across builds.

**Inconsistent parts:**

1. **Entry size and count.** Jetpack's Raft saturates on per-command CPU
   at ~12 k tiny entries/s. Mako's Raft carries far fewer, much larger
   entries. Its costs are encoding, the socket write, transfer, the
   follower's decode and replay: the data path. In this repository's own
   trace (`docs/performance/raft-latency-breakdown/`) a 1 MiB entry spends
   ~5 ms of its 8 ms moving bytes. A tiny-command test never exercises
   that.
2. **Apply.** Jetpack's apply is a single in-memory write, inline. Mako's
   follower apply replays hundreds of transactions and can be deferred by
   the watermark, and the leader's apply is only a timestamp publish. A
   test with a trivial apply cannot show replay throughput, deferred-log
   growth, or apply-thread latency. The trace shows ~0.6 ms of the 1 ms
   apply-thread idle sleep on every entry.
3. **What latency means.** Jetpack's number is per command and includes a
   client → leader hop. Mako never replies per entry. The quantity that
   matters to Mako is how fast a batch's timestamp reaches the watermark
   input (submit → leader apply), and how fast followers catch up in
   replay.
4. **The load model.** Mako's submitter is its worker cores, not 60
   external clients at 1 request/s per coroutine. The relevant knob is
   transactions per second per core and batch size, which together set
   the entry rate and entry size.
5. **Statistics.** One run per point cannot resolve the few-percent
   differences a conversion must rule out. This repository's
   `scripts/raft_perf/paired_trial.sh` and `rotation_trial.sh` use 25
   paired or rotated rounds for that reason.

**A consistent test for Mako's Raft** keeps Jetpack's structure and changes
the inputs:
- Drive Raft through `RaftWorker` (the path `add_log_to_nc` takes), with
  entries sized like Mako's batches:
  - the entry sizes, from a TPC-C-like write-set distribution × 400;
  - one group per core, several groups at once (`raft_bench --partitions`).
- Sweep the offered entry rate, or batch size × transaction rate, up to
  the knee.
- Report:
  - submit → leader-apply latency, the watermark input;
  - follower apply lag;
  - entries/s and bytes/s;
  - p50/p99 over repeated rounds.
- Optionally, a real apply callback that replays into a Masstree
  instance, so apply cost is included.
- For comparability with the group's papers, also run the Jetpack shape:
  tiny commands, an open-loop sweep, and a client → leader hop.
- Report both regimes, not only one.

`examples/raft_bench.sh` already covers the entry-size and statistics
parts: payloads 4 KB-1 MiB, partitions, paired and rotated trials, and
integrity checks. What it lacks from Jetpack's method:
- the offered-load sweep reported as a knee curve;
- a client → leader hop;
- multi-host placement. SSH to the other Zoo hosts is refused for this
  account today.

---

## 6. Limits of this report

- Rolis, the MongoDB NSDI paper and the PODC paper were listed but not
  read.
- The camera-ready AWS results cited by `scripts/camera-ready/plot_*.py`
  live in another user's home
  (`/home/users/ztang/janus/results/2026-05-13-camera-ready-exp0-fixes-v3/`)
  and are not readable from this account.
- Nothing from Jetpack was built or run here. Its behaviour is read from
  source at `c03e318e`.
- The follower-apply path (§3 step 4) is as read in `CommitReplicated`. It
  was not traced at run time.

References:
- Jetpack, OSDI '26: https://www.usenix.org/conference/osdi26/presentation/tang
- DepFast, ATC '22: https://www.usenix.org/conference/atc22/presentation/luo
- Mako, OSDI '25: https://www.usenix.org/conference/osdi25/presentation/shen-weihai
- Publication list: http://mpaxos.com/pubs.html
- Code: https://github.com/stonysystems/jetpack
