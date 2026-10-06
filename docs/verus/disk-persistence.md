# Disk persistence on verus-raft: a design, and what Verus adds

This describes branch `verus-raft` at `d9c2cb245` (2026-10-06). It builds on
the untracked assessment `/home/users/zyang2/mako-srpc-adopt/docs/raft-disk-assessment.md`
(2026-10-03), which was written against `srpc-subtree-forward` at
`8fc195615`, the Rust Raft before the Verus work; this branch is that tree
plus 82 commits. It answers the owner's question: how to add disk persistence
to the current code, assuming the known bugs (B1, B4, B7, B8, B14, B16-B19)
are fixed, and whether Verus makes it harder. B6, the memory-only state, is
the gap this closes. Nothing here was built, run or verified. Effort figures
are estimates in agent-days, builds and benchmarks included. "Inference"
marks a conclusion drawn from the code rather than read in it.

Paths follow [code-structure.md](code-structure.md): relative to
`src/deptran/raft/` (`src/server_h.rs` is the shell, `core/src/node.rs` the
core), except those beginning `src/deptran/`, `src/srpc/`, `src/mako/`,
`scripts/`, `docs/`, `ci/`, `examples/` or `CMakeLists.txt`, which are from
the repository root. `glr:` paths are in the ghost-log-refinement repository,
read at `40786a2b`; `glr@d7e04ed7:` marks our spec pin. "assessment §N" is
the assessment's section N; its line numbers refer to the other tree.

## 0. Short answer

**Does Verus add difficulty?** Yes, but modestly, and mostly as design
constraints rather than proof work.

- **No spec change and no spec retargeting.** glr verified crash-restart for
  raft-rs (A11, 2026-09-28) by reading a restart as the existing guard-free
  `LStepAside`. Our pin has that action already
  (`glr@d7e04ed7:src/protocol/Raft/raft.rs:210-236`). The plan's "spec v3,
  4-8 days" (`docs/verus/modification-plan.md:837-852`) is not needed.
- **Four design choices become fixed** if restarts are to stay inside the
  certificate (§3.2):
  1. the commit index is durable and restored, not reset to 0;
  2. a step's state change, and every earlier one, is durable before
     anything that step emits, including a follower's reply to a heartbeat
     that only raised its commit index and a leader's round that only
     carries a new one;
  3. a restart enters the core through a new event, `Restore`;
  4. the leader may not send entries before they are on its own disk (the
     assessment's S3). Asynchronous I/O is allowed; sending before the flush
     is not.
- **New proof work is small:** `Restore` must keep `ginv`, which reuses the
  step-aside lemma; optionally, a lemma that the core's persist record is
  exact and covers every send. The rest is host contract (§3.3, §3.4).
- **Where it helps.** The Verus refactoring already made `step` the only
  writer of term, vote, commit and log inside the verified configuration: one
  choke point instead of the assessment's nine term/vote sites in two files.
  If the core reports what to persist, a missed vote-only write, a truncation
  with no matching on-disk delete, and a step that emits without asking for a
  flush become proof failures; whether the shell flushes before it sends
  stays review and tests (§3.5). `Restore` must establish `inv`, which forces
  some of the store checks the assessment recommended: an entry without a
  value, a commit past the last index, a term at the limit, a base other
  than 1 (§3.5; `core/src/node.rs:145-178`, `core/src/coupling.rs:329-334`).
  Entry-term order and bounds and the vote's membership stay defence in
  depth, because the ghost premise that discharges `ginv` is trusted (§3.3).
- **Run-time cost of staying inside.** The same as plain strict sync, plus an
  fsync on each side of a round that carries only a new commit index: the
  leader's before it sends, the follower's before it replies (§3.2). Under
  load the commit rides on rounds that carry entries; under light load (G1,
  G3, G5) each commit costs both, off its own latency path (§2.9). Against
  the fastest uncertified design (S3), about one fdatasync time F more per
  commit (§2.9).

**Recommended design.** Term, vote, commit index and the whole log (base 1)
go into a Rust write-ahead log, one per server. The core reports, with each
step's output, what changed and whether the step emitted anything (with one
server, also whether it advanced the commit index, §2.2); the shell
queues those records under `mtx_` in step order and, before any emission,
writes and fdatasyncs them after releasing `mtx_`, on the emitting thread.
The leader flushes once per round (group commit), a follower once per
AppendEntries. The apply thread applies only through the durable last index.
A restart runs `Restore` between `Configure` and `EnterGates`, with RPCs
closed, and hands (0, commit] to the apply thread. Any write or fsync error
aborts the process. Snapshots stay off.

**Effort (estimates).**

| | Days |
|---|---|
| Without Verus: Raft side (§4, P0-P3, P5, P6, P8) | 7.75-14.5 |
| Without Verus: Mako's replay hazards (P7, uncertain) | 2-5 |
| Added by Verus, durability rule trusted (assumed form) | 2-3.5 |
| Added by Verus, durability rule proved (check form) | 4-7.5 |
| Optional: `log_grows` on `step` | 0.5-1.5 |

On midpoints that is about a fifth more with the rule trusted and about two
fifths more with it proved.

## 1. The assessment, and what changed on verus-raft

| Assessment | What it said | On verus-raft | Evidence |
|---|---|---|---|
| §1.1 durable state | term, vote, log, snapshot meta; commit an optional hint | same fields, now in the core; commit becomes required for a certified restart (§3.2) | `core/src/node.rs:49`, `:55`, `:69-74` |
| §1.2 term/vote sites | 9 sites in 2 files, no setter; T4 (a grant) logs no term change | 6 sites, all inside `step`; T4 is `core/src/node.rs:685`. Outside `step` only the snapshot paths, including the InstallSnapshot term write that runs even with snapshots off | `core/src/node.rs:663-665`, `:685`, `:759-761`, `:889-890`, `:1901-1902`; `core/src/heartbeat.rs:1359-1360`; `src/server_h.rs:2306-2307` |
| §1.2 log mutators | 4 mutators, 6 sites | 3 sites in the verified configuration; compaction removes nothing under the gate | `core/src/node.rs:510`, `:2094`, `:2127`; `core/src/log.rs:314`, `:404`, `:532`, `:600` |
| §1.3 barriers | vote reply, AppendEntries body (both ids), InstallSnapshot, candidate before broadcast, leader self-count | three of them (vote reply, AppendEntries reply, campaign broadcast) plus the leader's AppendEntries sends (S2's flush), each at a critical-section boundary (§2.3); InstallSnapshot needs none, since it refuses first while snapshots are off; the self-count is still implicit | `core/src/heartbeat.rs:104`; `core/src/helpers.rs:326-341` |
| §1.3, §8.12 candidate gap | persist T1 under `mtx_`, or a late write can roll back a newer term | closed by queueing records under `mtx_` in step order (§2.4) | — |
| §1.4 read-back | load before snapshot-manager init, with its own validation | load after `Configure` (whose premise is a fresh core, and the vote's rank needs the configuration), before `EnterGates`; `inv` forces part of the validation (§0, §3.5) | `src/server_h.rs:1795-1830`; `core/src/coupling.rs:2588-2591` |
| §2 storage manager | Option C: a Rust storage trait and a log manager as the one choke point | the choke point exists: the `step` wrapper. The "log manager" reduces to the persist record and the barrier | `src/server_h.rs:1407-1443` |
| §4.1 batching | one RPC in flight per follower; ack = min(reported, sent end, last); CONTRADICTORY; commit computed twice per round | unchanged, now in the core | `core/src/heartbeat.rs:908-911`, `:1400-1414`, `:358`, `:1689` |
| §4.2 S1, S2, S3 | S2, with the leader counted at a durable index | S1 and S2 fit the certificate; S3 is outside it (glr B45). Without S3 the explicit self-count is not needed (§2.5) | §3.2 |
| §4.3, §4.4 watermarks | no durable index; "memory means durable" sites hold trivially under strict sync | unchanged; `Start` still reports APPENDED from memory | `src/server_h.rs:3884-3914` |
| §5 layout | a DB per (site, partition), never under `/tmp/$USER_*`; term is u64 in state, i64 in entries | both still true; a Rust WAL is recommended instead (§2.7) | `ci/ci.sh:84`; `examples/run_rocksdb_test.sh:27`; `core/src/node.rs:69`; `core/src/log.rs:38` |
| §6 restart tests | none; the lab only disconnects; `shardFaultTolerance` disabled; `recover_fresh` the precedent | unchanged; `recover_fresh` writes the core directly, a `T` line | `src/lab.rs:671-702`; `ci/ci.sh:615-627`; `src/lab_snapshot_cases.rs:1246-1290` |
| §6 in-process restart | rebuild the server object, re-point every holder; `PrepareForShutdown` on a fiber | a soft restart that replaces `core` avoids the re-pointing (§2.8); `PrepareForShutdown` runs on the shutdown thread | `raft_worker.cc:611`; code-structure.md:944-945 |
| §7 items 3-4 | leader self-count and follower ack from a durable index | now changes to the verified core; item 4 also needs deferred replies or a spec change. Neither is needed under strict sync without S3 (§2.5) | `core/src/heartbeat.rs:63-135` |
| §7 threading | handlers hold `mtx_` for the whole body; re-locking aborts | the same in substance: one critical section per handler, then the unlocked actions | `src/server_h.rs:4181-4186`, `:4264-4272`; `server.h:241-250` |
| §8.1 blocking I/O on the reactor | which thread runs the fibers was unverified | the poll thread | `rt/src/seam.rs:166-168`, `:187-211`; `src/server_h.rs:1742`, `:1851-1867` |
| §8.2, §8.3 apply gaps | recovery never enqueues (applied, commit]; a gap is only logged | still true; `Restore` pushes the range itself (§2.6) | `src/server_h.rs:3075-3144` |
| §8.4-§8.6 Mako replay | control entries not idempotent; apply depends on the role | unchanged | `raft_worker.cc:1091-1147`; `src/mako/mako.hh:198-335` |
| §8.8-§8.10 | lanes; `raft_test` divergence; compaction needs a durable snapshot | lanes removed; the other two unchanged (compaction is skipped under the gates) | `CMakeLists.txt:465-479`; `src/server_h.rs:3499-3501`, `:1342-1349` |
| §8.14 redial | unverified whether Raft uses srpc's reconnect policy | it does (§2.6) | `src/srpc/rpc/client.rs:1586`, `:1659` |

## 2. The design on the current core/shell split

### 2.1 What is durable

| State | Field | Durable | Why |
|---|---|---|---|
| term | `current_term_` (`core/src/node.rs:69`) | yes | every reply carries it; a forgotten term allows a second vote (`docs/verus/bugs-found.md:180-183`) |
| vote | `vote_for_` (`:55`) | yes | the same |
| log | `raft_log_` (`:49`), base 1 under the gate (`:172-176`) | yes, from index 1 | an acknowledged entry must survive |
| commit index | `commit_index_` (`:70`) | yes, in the same records | the certificate needs it (§3.2); it also tells startup what to apply |
| applied index | `execute_index_` (`:71`), `appliedIndexForWait_` (`src/server_h.rs:951`) | no | Masstree is memory-only; restarts at 0 |
| snapshot boundary | `snapidx_`, `snapterm_` (`:73-74`) | no | snapshots stay off; disk mode refuses `MAKO_RAFT_SNAPSHOTS` (`server.cc:798-799`) |
| role, peers, round state, timers, leader hint | the rest of `RaftCore` | no | volatile in Raft; the spec's step-aside drops the role and the votes but keeps the match and next tables, which the core holds only as ghost state (`g_match_`, `g_next_`, O1) and `LBecomeLeader` resets before use (`glr@d7e04ed7:src/protocol/Raft/raft.rs:200-201`, `:226-232`; glr:docs/ghost-log/raftrs/roadmap.md:519-520) |
| membership | `config_members_`, read from yaml (`src/server_h.rs:3531-3547`) | a fingerprint in the store's header | a store written under another configuration fails closed |

### 2.2 Decisions in the core, I/O in the shell

A core call already returns its effects as data: actions and log lines in
`CoreOutput` (`core/src/output.rs:25-39`, `:126-133`). Add one more output,
the step's **persist record**:

- `hard`: term, vote and commit, when any of them differs from the core's
  copy of what was last recorded (three new fields in `RaftCore`);
- `log_from`: the lowest log index the step changed; the shell copies the
  entries from there to the last index;
- `sync`: whether the step emitted something (a vote or append reply, the
  campaign's broadcast, a tick with sends); with one server, also whether it
  advanced the commit index, because a lone leader has no peers: after its
  election none of its steps emits (`core/src/heartbeat.rs:102`, `:884`;
  `core/src/coupling.rs:885-893`), so nothing else would flush (§2.5).

Only two places change the log in the verified configuration, and both know
the index: `append_local` (`core/src/node.rs:510`) and the AppendEntries
handler, which truncates at `first_write_index` and appends from there
(`:2094`, `:2127`); an entry already held is not rewritten (`:2033`). Each
pushes `log_from`, as handlers push actions now. Term, vote and commit need
no marks: comparing with the copies catches a write anywhere, the vote-only
grant at `:685` included. The alternative, Option S, has the step wrapper
(`src/server_h.rs:1407-1443`) diff the state around each call; it changes no
core code, but then the rule rests on shell code only (§3.3).

### 2.3 The rule and the four barriers

- **R1.** A step's record, and every record before it, is durable before
  anything that step emits leaves the server.
- **R2.** Records are written in step order, each one atomically; a
  truncation and the entries that replace it are one record.
- **R3.** The apply thread applies only through the durable last index (§2.5).
- **R4.** A write or fdatasync error aborts the process; nothing is retried.
- **R5.** Checked at run time, aborting: no reply acknowledges beyond the
  durable last index; with two or more servers, neither does a leader's
  commit index (§2.5).

| Emission | Decided in | Leaves at | Barrier |
|---|---|---|---|
| vote reply | `on_request_vote_body`, critical section `src/server_h.rs:4181-4186` | returned to `rt/src/service.rs:108-113`, queued by `rt/src/rpc.rs:476-491` | after `:4186`, before the return |
| AppendEntries reply, both RPC ids | `on_append_entries_body`, `:4264-4272` | `rt/src/service.rs:118-152` | after `:4272` |
| campaign broadcast | `RequestVoteImpl`, `:3173-3185` | `raft_broadcast_vote_and_wait`, `:3220-3226` | between them |
| AppendEntries sends | `heartbeat_tick_body`, `src/server_cc.rs:87-125` | `:139-178` | between them, when the tick has sends |
| InstallSnapshot | `OnInstallSnapshotLocked`, `src/server_h.rs:2226` | -- | none: with snapshots off, refuse before the term write at `:2306-2307` (move the storage check at `:2402-2412` up) |

No barrier is needed for the unavailable answers and F9's drops
(`src/server_h.rs:3928-3934`, `:3946-3952`, `:4232-4241`, `:4311-4322`), which
echo the request's term or send zeros, nor for `Start`'s APPENDED
(`:3884-3914`), which promises nothing (assessment §4.4). A step that changes
durable state without emitting (a reply's step-down,
`core/src/heartbeat.rs:1359-1360`; a proposal; a round end's commit, `:1689`)
is queued and flushed by the next emitting step (with one server, a commit
advance flushes at once, §2.2).

### 2.4 Threading

All four Raft RPCs are fast RPCs, run inline on the transport's poll thread
(`rt/src/rpc.rs:386-404`; `src/srpc/rpc/server.rs:1476-1479`), and the
heartbeat and election fibers run on the same thread: the owner-thread
startup job spawns them (`src/server_h.rs:1742`, `:1851-1867`), and a fiber
stays on the thread that created it (`rt/src/seam.rs:166-168`, `:187-211`).
srpc only queues a frame; the poll loop writes it later
(`src/srpc/rpc/server.rs:1246-1285`; `src/srpc/rpc/tcp_channel.rs:864-897`),
so "send, then fsync" on one thread overlaps nothing. A follower has at most
one AppendEntries from its leader in flight (`core/src/heartbeat.rs:908-911`),
so it has nothing else to do while it waits for its disk.

| Shape | Who waits for F | Group commit | In the certificate | Verdict |
|---|---|---|---|---|
| A1: fsync under `mtx_` at every change, `Start` included (submit thread, `raft_worker.cc:743-753`) | every thread that wants `mtx_`, once per proposal | none: at most 1/F proposals per second | yes | reject |
| A2: fsync under `mtx_` at the four barriers only | the poll thread, plus `Start` and `Applied` while it holds `mtx_` | per round (leader), per RPC (follower) | yes | correct, blocks two threads needlessly |
| **B1: queue records under `mtx_`; write and fdatasync after unlocking, on the emitting thread** | the poll thread only | the same | yes | **recommended** |
| B2: a disk thread; deferred replies; sends after the completion | nothing | across rounds | yes | later, if measurements call for it |
| S3: the leader sends before its own flush | nothing on the leader's commit path | across rounds | no (glr B45) | outside |

B1, at each barrier:

```
critical section (mtx_ held):
  step(...); run_locked_actions(...)
  append this section's records to the queue (command handles cloned)
  if a queued record has sync: take the store lock; take the queue
release mtx_
if taken: encode (raft_command_encode, server.cc:1414); write; fdatasync; error -> abort
          release the store lock; publish durable_last and durable_commit (atomics)
send, or return the reply
```

Because the store lock (a leaf, taken after `mtx_`) is taken before `mtx_` is
released, records reach the disk in step order, and a stale term and vote can
never land after a newer one; this is the hazard of assessment §1.3 and §8.12.
While the poll thread waits on the disk, `Start` and `Applied` can still take
`mtx_`; in production nothing else emits, since every emitting step runs on
the poll thread. Lab builds also call `ServeAppendEntries` and `ServeVote`
from the harness thread (`src/lab.rs:508-536`; code-structure.md §7), so
there a handler can wait for the store lock under `mtx_` while the poll
thread fsyncs: the order still holds, but it is a wait under `mtx_`, in the
builds §2.8 uses. The lab either accepts that wait or routes its injections
through the server's poll thread.

Timing: the heartbeat is 5 ms (`server.h:169-173`) and replies are collected
for min(heartbeat, 100 ms) (`src/server_cc.rs:201-210`), so when F plus the
round trip exceeds 5 ms every round misses its own replies; raising
`MAKO_RAFT_HEARTBEAT_INTERVAL_US` (`src/server_h.rs:1753-1766`) raises the
deadline too. Election timeouts are 150-300 ms for the preferred leader and
0.5-2 s for the others (`server.cc:165-202`); they need the "persistence
floor" that `src/server_h.rs:1450-1452` says does not exist only if F's
p99.9 nears 150 ms.

### 2.5 Applying committed entries

Mako's leader callback acknowledges a transaction (it advances
`local_timestamp_`, `src/mako/mako.hh:417`) and does not replay it into
Masstree; the follower callback replays (`:271`). So the leader's apply is an
external effect, and it must not get ahead of durability.

- **Two or more servers.** A commit index is a follower's acknowledged index
  (`core/src/progress.rs:292-361`), which is capped at the end of what this
  leader sent (`core/src/heartbeat.rs:1400-1414`), which R1 flushed before
  sending. Match indexes restart at 0 when a server becomes leader
  (`core/src/node.rs:1200-1202`), and a leader's log only grows while it
  leads (an accepted AppendEntries steps it down first, `:1949-1954`). So
  every committed entry is already durable on a majority, the leader
  included (inference). The leader's implicit self-count at its in-memory
  tail (`core/src/heartbeat.rs:104`) is harmless here.
- **One server.** The candidate is the in-memory last index
  (`core/src/helpers.rs:326-341`), committed in the tick
  (`core/src/heartbeat.rs:358`, `:844`) before any flush. Applying it would
  acknowledge an entry durable nowhere.

R3 bounds the apply thread (which polls its queue every 1 ms,
`src/server_h.rs:2625`) by `durable_last`: no wait with two or more servers,
one flush with one (the committing step's, which `sync` requests there,
§2.2; without that trigger nothing would flush after the election and the
apply thread would wait for ever), and a follower waits for its own flush,
which costs nothing that matters. The verified commit rule does not change,
unlike the assessment's §7 item 3.

A stricter option, **Rule A**, applies only through the commit index of the
last completed flush. Every acknowledgement then rests on a durable commit
and so lies inside the theorem; without it the case is argued, as glr argues
it (glr:docs/ghost-log/raftrs/roadmap.md:547-551). The leader then waits for
the next tick's flush, which a commit advance requests at once
(`src/server_cc.rs:327-329`): about +F per commit. With one server that
tick has no sends, and the wait is for the committing step's own flush
(§2.2).

### 2.6 Restart

In `SetupInternal` (`src/server_h.rs:1744-1869`):

1. `rpc_ready_` is false (`:1747-1748`): the handlers answer "unavailable".
   Snapshot-manager initialization (`:1795-1804`) returns early with
   snapshots off (`:1904-1923`, after the uncovered-progress check at
   `:1907-1918`).
2. `LoadCurrentConfig` steps `Configure` (`:1811`, `:3545`).
3. **New.** Open the store under `MAKO_RAFT_DATA_DIR` (unset: disk mode off);
   check its header; read records to the first bad checksum, cutting a torn
   tail and failing closed if good records follow a bad one; decode each
   command (`server.cc:438`) and build each entry (`src/server_h.rs:3446-3459`);
   under `mtx_`, `step(Restore{term, vote, commit, entries})` and
   `run_locked_actions`; publish `durable_last` and `durable_commit` as the
   loaded last index and commit (all of it is durable), or R3 holds back the
   replay backlog `Restore` queues.
4. `EnterGates` (`:1830`) still passes: the log starts at 1, with no snapshot
   (`core/src/node.rs:317`).
5. The apply thread starts (`:1843`), RPCs open (`:1844-1845`), the fibers
   start (`:1851-1867`); the election fiber does not campaign until the
   applied index reaches the restored commit.

`Restore` is admitted only on a core `Configure` has just set up. It refuses,
and the shell fails closed, unless the indexes run from 1 without a gap,
every entry has a value and a term of at least 1, entry terms never decrease
or exceed the stored term, the commit index is at most the last index, the
term is below the index limit, and the vote is a member or none. It sets term,
vote, log and commit, leaves a follower with applied index 0, and pushes
`APPLY_RANGE(0, commit)`, which `EnqueueCommittedEntries`
(`src/server_h.rs:3075-3144`) turns into the replay backlog.

**Mako's state** is rebuilt by that backlog through the follower callback.
Two Mako hazards remain (assessment §8.4-§8.6). The callback is chosen by the
role at apply time (`raft_worker.cc:1091-1147`): holding the campaign covers
the restart, but a server that wins later still applies earlier-term entries
through the leader callback, which does not replay them (true without a disk
too). And control entries run again in the new process: the advancer marker
starts a thread (`src/mako/mako.hh:198-202`), noops call `set_epoch` and
NFSSync's `set_key` and `wait_for_key` (`:224`, `:242`, `:283`), and `reset`
runs (`:335`); whether replaying old epochs is safe is not verified.

**Transport.** Raft connects to each peer once (`rt/src/transport.rs:276-296`),
but srpc's clients reconnect on their own with the default policy
(`src/srpc/rpc/client.rs:1586`, `:1659`, `:2131-2149`;
`src/srpc/rpc/reconnect_policy.rs:20-28`, `:42-44`, `:82-106`): an
immediate attempt, then 5 retries 1, 2, 4, 8 and 16 s apart (the 30 s cap is
never reached; each delay is jittered by a factor of 0.5-1.5), about 31 s in
all (roughly 15-47 s); nothing in `rt/src` changes it. A relaunch inside
that window should be picked up (inference, untested). **Recorder.**
`Restore` is an ordinary recorded event (`replay/src/lib.rs:110-266`,
`:624`), never a `T` line, which ends a replay (`:762-764`).

### 2.7 Storage engine and failure policy

**A Rust write-ahead log** in a small crate beside the shell, with no FFI: a
header (magic, format version, site, partition, configuration fingerprint,
command format version), then records (length, CRC32C, a hard state and/or
"replace from index i with these entries"), appended to preallocated
segments; one fdatasync per barrier; a new segment and its directory are
fsynced before use. A fault-injecting in-memory backend, which drops records
not yet synced on a simulated crash, serves the tests.

**Why not RocksDB** (assessment §5; `txlog_core` already links it,
`CMakeLists.txt:1311-1321`): the entries are 4 KB to 1 MiB values written once
and read only at startup, since memory stays the read cache (assessment §3);
an LSM tree writes each byte at least twice, plus compaction (inference), and
needs `extern "C"` kernels where the WAL needs none (CLAUDE.md: new logic in
Rust). If it is chosen anyway: one `WriteBatch` per barrier with
`sync = true`, `DeleteRange` for a replaced suffix, never `rocksdb_flush` as
a sync. Verus is indifferent: storage is trusted either way.

**Location.** Never under `/tmp/$USER_*`, which `examples/run_rocksdb_test.sh:27`
deletes (`ci/ci.sh:84` deletes `/tmp/$USER_mako_rocksdb_shard*`). On
zoo-003 (checked read-only, 2026-10-06) `/` is ext4 on `/dev/sda2`, a
hardware RAID logical volume over rotational disks that the kernel reports
as "write through"; `/tmp` is tmpfs; home is NFS.
Durability tests use ext4; tmpfs only measures the software cost.

**Failure.** A write or fdatasync error panics, which aborts the process
(`Cargo.toml:80-84`) before the reply or send; after a failed fsync Linux
may have dropped the dirty pages, so a retry proves nothing. A store with a
wrong header or mid-file corruption fails closed, and that replica must not
rejoin empty under its old id (B6 again); the static configuration cannot
re-add it.

**Growth.** Compaction is off (`src/server_h.rs:1342-1349`), so the log
grows without bound: about 155 MB/s per replica at G2's rate (37,760 × 4 KB;
inference). The whole log is loaded into memory and replayed at restart, so
restart time and memory grow with it.

### 2.8 Testing

- **Lab, soft restart in process.** A job on the victim's poll thread, under
  `mtx_`, drops the fault-injecting store's unsynced records, replaces `core`
  with a fresh one stepped through `SetIdentity`, `Configure`, `Restore` and
  `EnterGates`, bumps the apply queue's epoch so queued entries are skipped
  (`src/server_h.rs:2641`), and resets the mirrors, the response slots and the
  lab learner's table for that server. It also sets `appliedIndexForWait_`
  to 0, which is not a mirror (`:1650-1664`): left as it was, the apply
  thread would skip the replay backlog as snapshot-covered (`:2640`,
  `:2646-2650`) and `on_applied` would refuse to move it back
  (`core/src/node.rs:1090-1097`); and it publishes `durable_last` and
  `durable_commit` as `Restore` loaded them (§2.6). It avoids re-pointing
  every holder of the server pointer (assessment §6) but skips
  `SetupInternal`.
- **Cases**, after MIT 6.824's persistence tests, as cases 1-11 follow its
  earlier ones (`src/lab_cases.rs:141-549`): restart a follower, the leader, a
  majority, all; no second vote in a term; a crash between write and
  fdatasync with no reply carrying that state; a torn tail; mid-file
  corruption and a foreign store refused; Figure 8 with crashes. CI counts
  new `init2` ids by itself (`ci/ci.sh:485-490`).
- **Process restart.** A restart mode beside `examples/raft_bench.sh`'s
  leader kill (`:77`): `kill -9`, relaunch on the same directory, integrity
  counters at 0 (`docs/verus/modification-plan.md:1467-1476`); then a `dbtest`
  follower restart. `kill -9` keeps the page cache, so it tests ordering, not
  fsync; only the fault-injecting store tests what fsync promises.

### 2.9 Performance

Baselines: `docs/verus/modification-plan.md:1461-1464`,
`docs/performance/raft-rust-9a361eccd/compare-vs-412c225a.txt:15-18`,
`docs/verus/reports/phase-0.md:164-167`. Bytes per second are rate × payload
(inference); effects are a model to be measured, with F the fdatasync time
and W the time to write a batch.

| Point | Today | Bytes per replica | Effect of B1 + S2 | zoo-003, three replicas on one volume |
|---|---|---|---|---|
| G1, 4 KB at 240/s | p50 2.647 ms | ~1 MB/s | +2F per commit, plus a commit-only round off its path (below); Rule A +F more; S3 (outside) would be about +F | the same |
| G2, 4 KB unthrottled | 37,760/s | ~155 MB/s | one fdatasync per round on the leader, per AppendEntries on a follower | ~465 MB/s in all: likely disk-bound |
| G3, 286 KB × 6 at 190/s | p50 3.343 ms | ~54 MB/s | +2F + 2W | contention |
| G4, 286 KB × 6 unthrottled | 3,132/s | ~896 MB/s | six groups, six fsync streams (`examples/raft_bench.sh:73`, multi-group) | ~2.7 GB/s in all: far beyond |
| G5, 1 MiB at 55/s | p50 8.767 ms | ~58 MB/s | +2F + 2W | contention |
| G6, 1 MiB unthrottled | 183/s | ~192 MB/s | W dominates | ~576 MB/s in all |
| G7, leader kill | ~630 ms to first commit | -- | the new leader's no-op adds about 2F | the same |

The per-commit figures leave out one cost of staying inside. A commit
advanced at a round's end goes out at once in a follow-up round
(`src/server_cc.rs:324-329`), whose sends read the new commit
(`core/src/heartbeat.rs:917`) and follow its segment in the ghost log, so the
leader fdatasyncs the commit record before sending them, and each follower
fdatasyncs its raised commit before replying (§3.2), even when no entry is
new. Under G2, G4 and G6 the commit rides on rounds that carry entries.
Under G1, G3 and G5 most commits are followed by such a commit-only round:
F on the leader and F on each follower, off that commit's latency path, but
holding the poll threads and each follower's one AppendEntries in flight,
so they can delay the next proposal's round (inference). Plain strict sync,
with the commit a hint, pays neither.

Group commit keeps throughput: one fdatasync per proposal would cap a group
at 1/F proposals per second (5,000/s at F = 0.2 ms, 87% below G2). Typical F,
not measured here: 0.02-0.3 ms behind a protected cache, 0.5-5 ms on consumer
SSDs, 5-15 ms on uncached disks. Memory mode must stay inside today's G1-G7
bounds against `verus-p0` (`docs/verus/reports/phase-8.md` §4), the barrier
costing one branch; disk mode needs its own baselines, on tmpfs and ext4.

## 3. What Verus adds

### 3.1 How glr verified crash-restart (A11)

- **No spec action.** raft-rs's restart (term, vote, log and commit back from
  storage; role, votes, progress and reads forgotten) is, field for field, the
  guard-free `LStepAside`; keeping the commit index leaves every
  commit-monotonicity lemma alone (glr:docs/ghost-log/raftrs/roadmap.md:514-522).
- **The persistence boundary is ghost state in the invariant:** the
  certificate of the last Ready the host reported persisted (`durable`) and
  those handed out since (`pending`), each a closed ghost log whose replayed
  term, vote and commit are the hard state on disk, each a prefix of the next
  (glr:src/ports/raftrs/certs.rs:22-27, :39-49, :66-71).
- **Persist, then send** (B45, glr:docs/ghost-log/raftrs/coupling.md:391).
  A crash truncates the history to the last persisted certificate, which
  holds every emitted `Send`, so it is itself a certificate (roadmap.md:526-534).
  `new_from_store` checks the store in exec, takes that certificate `prev` as
  a ghost argument, and starts its ghost log as `prev` plus one step-aside
  segment (glr:src/ports/raftrs/raw_node.rs:751-757, :785-806, :860-867, :877).
  The composition theorem is unchanged: a node's certificate across crashes
  is its last incarnation's log (glr:docs/ghost-log/raftrs/composition.md:150-174).
- **Cost:** planned at 4-6 weeks, green in about two hours (roadmap.md:506,
  :569-570); commit `e1f4df62`, 19 files, +902/-100, of which the spec layer
  took only +106 lines of generic lemmas in `ghost_log.rs`.
- **Still trusted or open in glr:** storage reliability (coupling.md:555-556,
  :625-627) and the store matching `prev` (B46, B47, :353-354). Its audit says
  the end-to-end chain is not closed: no Verus host carries the certificate
  across a crash, and the harness assumes `prev` with `Ghost::assume_new()`
  (glr:docs/ghost-log/raftrs/audit-2026-09-30.md:16-18, :86-96;
  roadmap.md:760-764). raft-rs's leader-side early send stays outside (B45).

### 3.2 Our pinned spec suffices, and what it forces

- `LStepAside` at our pin has no guard and keeps term, vote, log and commit
  (`glr@d7e04ed7:src/protocol/Raft/raft.rs:210-236`); our core already
  records it, with its lemma (`core/src/coupling.rs:468-530`, used by
  `SetFollower` and `StepDown`, `core/src/event.rs:499-532`). A11's generic
  lemmas are not at our pin, and `ghost_log.rs` is a hashed file of our frozen
  spec (`verus/spec/SPEC_VERSION.toml`), so any we need go into
  `core/src/coupling.rs`. The cluster theorem's statement stays
  (`:2678-2704`). The newer glr layers (25 spec-layer files, +1,759/-249 from
  `d7e04ed7` to `40786a2b`: joint consensus, snapshots, A11, reads) matter for
  snapshots, not for restart.
- **Commit must be durable.** `ginv` requires the replayed ghost state to
  equal `state_view` (`core/src/coupling.rs:305-308`), which maps
  `commit_index_` exactly (`:289`); `LStepAside` keeps the commit index, no
  action lowers it, and an append carrying an entry must carry the leader's
  exact commit (`glr@d7e04ed7:src/protocol/Raft/raft.rs:368`). So a core
  restarted at commit 0 could never lead inside the certificate. A
  follower's commit raise on a heartbeat and its reply are one spec action:
  the coupling records every accepted AppendEntries, an entry-less one at
  prev included, as `LFollowerAppendEntries`
  (`glr@d7e04ed7:src/protocol/Raft/raft.rs:489-520`, commit at `:508`, reply
  at `:513-519`), through `empty_msg` and `fae_seg`, which sets the commit,
  sends the answer and closes in one segment (`core/src/coupling.rs:1633-1646`,
  `:1691-1698`, `:1872-1873`, `:1975-1983`; `core/src/node.rs:2143-2151`).
  The spec's `LFollowerHeartbeat`, a prev-0 heartbeat with a match-0 reply,
  is not used. So the raise must be durable before the reply: an fdatasync
  the plain design skips. Likewise a leader's commit advance at a round's end
  precedes, in its ghost log, the follow-up round's sends
  (`src/server_cc.rs:324-329`), so the leader too flushes the commit before
  sending, even when no entry is new (§2.9).
- **S3 is outside.** An entry sent before the leader's flush is recorded in a
  step a crash may discard, leaving the follower's `Recv` with no surviving
  `Send`. Admitting it is glr's S8 on the openraft line (a durable prefix per
  node, `LRestart`, the leader counted only to its durable index), estimated
  at 3-4 weeks and not started (`git show
  origin/openraft:docs/ghost-log/openraft/roadmap.md`, lines 82, 143, 151-183).

### 3.3 New proof obligations

| Obligation | Content | Estimate |
|---|---|---|
| O1: `Restore` keeps `ginv` | from a core `Configure` set up, given the ghost `prev` (fully closed, `log_ok` with the same constants, replaying to the stored term, vote by rank, log view and commit, to the configuration `range(0, n)` with `conf_index` 0, and with no reads; `log_ok` alone does not pin the configuration, since the pinned ghost log admits `LoadConfig` and `ApplyConfChange` segments, while `state_view` fixes both, `core/src/coupling.rs:295-296`, and `step_aside_log` keeps `prev`'s, `:468-473`; glr's `restart_cert_ok` carries the same clauses, its B47, glr:src/ports/raftrs/raw_node.rs:803-807), the new core has `g_log_ = step_aside_log(prev)`, no votes, and `g_match_`, `g_next_` from `replay(prev)`. Reuses `lemma_step_aside` (`core/src/coupling.rs:475-507`); the restore loop needs an invariant over the log view; `prev` is an erased ghost field of the event, the actual previous log as glr's `new_from_store` takes it | 1-2 d |
| O2: `Restore` keeps `inv` | the checks of §2.6 give `inv_config` (`core/src/node.rs:145-178`) and `log_entries_ok` (`core/src/coupling.rs:329-334`); panic freedom | in O1 |
| O3 (check form): the record is exact | `step` ensures that the record applied to the last recorded state gives the new term, vote, commit and log view. The AppendEntries site states its log effect in its loop invariant (`core/src/node.rs:2101-2121`); `append_local` proves its effect only inside (`RaftLog::append`'s ensures, `core/src/log.rs:323`; the assert at `core/src/node.rs:517`) and does not export it (`:501-506`), so O3 adds that ensures | 1-2 d |
| O4 (check form): `sync` covers every send | if the step's ghost segment holds a `Send`, `sync` is set | 0.5-1 d |
| O5: budget and frames | the copies change only at the end of `step`, so existing frames hold (inference); `raft_on_append_entries` runs at `rlimit(40)` (`core/src/node.rs:1748`), and glr raised its append handler to `rlimit(200)` for A11 (glr:docs/ghost-log/raftrs/port-audit.md:843) | 0.5-1 d |
| O6 (optional): `log_grows` on `step` | turns "the durable certificate is a prefix of the live log" from prose into a theorem; 26 ghost-log assignments in the core | 0.5-1.5 d |

The assumed form is O1 and O2 plus the host contract (§3.4) and making the
commit index durable: 2-3.5 days. The check form adds O3-O5 (2-4 days):
4-7.5 days in all; the core-reported record it relies on (§2.2) is in P2's
plain estimate.

### 3.4 Host-contract changes

| Where | Today | With persistence |
|---|---|---|
| §1 item 4 (`docs/verus/host-contract.md:51-58`) | a send is made after the call that recorded it returns; `sched` is the real interleaving of the servers' calls | ...and after that call's record and every earlier one are durable (R1); records are written in call order (R2); `sched` is that interleaving restricted to the segments that survive in the spliced logs, since a crash discards the unsynced tail and `Restore` replaces the new incarnation's `LoadConfig` segment (`core/src/node.rs:298-301`), while the theorem reads only the latest cores' logs (`core/src/coupling.rs:2674-2685`); that `causal` holds for the splice is argued, not proved, as glr argues it (glr:docs/ghost-log/raftrs/composition.md:166-173) |
| §1 item 5 (`:59-61`) | storage: none persisted; no replica restarts in place | storage trusted: a record is atomic, durable when fdatasync returns, read back as written; a crash leaves a prefix of the records |
| §3, a new row | -- | `Restore`: on a core `Configure` just set up, from this server's own store, whose contents are the replay of the previous incarnation's last durable ghost log (glr's B46), written under the same configuration, so that its replay's configuration is `range(0, n)` with `conf_index` 0 (glr's B47) |
| §7, the codec | a command's value view survives the wire codec | ...and the disk encoding (`server.cc:1414`, `:438`) |
| F5's gate (`src/server_h.rs:1816-1841`) | snapshots off, failover on, static configuration | a restart is covered only in disk mode |
| B6 (`docs/verus/bugs-found.md:176-189`) | a trusted assumption | retired for disk-mode runs; kept for memory-mode runs |
| InstallSnapshot | writes term and vote outside `step`, then refuses (`src/server_h.rs:2306-2307`, `:2402-2412`) | refuses first while snapshots are off |

Still outside: a synced write the device loses; a restart with another
configuration or another server's store; the snapshot paths; and, unless
Rule A is chosen, the leader's acknowledgement to Mako (§2.5).

### 3.5 Where Verus catches persistence bugs, and where it does not

With the check form:

| Bug | Where it would be on this branch | How it shows up |
|---|---|---|
| a vote-only write not persisted (the assessment's T4: no term change to hook) | `core/src/node.rs:685` | O3 fails: the record no longer yields the new state |
| a truncation without the matching on-disk delete (the removed C++ wrote truncate and append separately, assessment §2) | `:2094` then `:2127` | O3 fails: the recorded log differs from the log view |
| a new log mutation that forgets to report itself | any future site | O3 fails |
| a step that sends without asking for a flush (candidate before the broadcast, a refusal after a term step) | the four barriers | O4 fails |
| a corrupt or foreign store: commit past the last index, a term at the limit, an entry without a value | the restore | O1/O2 cannot be proved without the checks of §2.6 |
| a panic on the restart path | the restore | panic freedom; glr's work there found a raft-rs restart crash loop (glr:docs/ghost-log/raftrs/findings.md:34) |

It does not check: the shell's queue, lock order and barrier placement; the
WAL code and whether the device honours fdatasync; the store checks `inv`
does not force (entry-term order and bounds, the vote's membership), which
the trusted ghost premise of O1 covers instead; the decoding at load; the
abort on error; Mako's replay; peer redial; the snapshot paths (26 direct
writes, code-structure.md §3.2 item 4). Under strict sync the assessment's
"memory means durable" sites (§4.4) hold trivially, so the proof finds
nothing there. With the assumed form, Verus checks only O1 and O2, and the
durability rule rests on review and tests as it would without Verus.

## 4. Phase plan

Estimates in agent-days. The Verus column is what the certificate adds;
`scripts/verus/verify_core.sh` and the replay check run at the end of every
phase that touches the core.

| Phase | Work | Plain | Verus |
|---|---|---|---|
| P0 | measure fdatasync p50/p99/p99.9 on zoo-003's ext4 and on tmpfs, with one and three writers; settle the controller-cache question | 0.25-0.5 | -- |
| P1 | the WAL crate: records, recovery, the fault-injecting backend, crash-at-every-offset tests | 1.5-2.5 | -- (trusted) |
| P2 | core: `Restore` and its checks; the persist record in `CoreOutput`; recorder and replayer support | 0.5-1.5 | O1, O2 (1-2); the host contract, B6 and the gate text (0.5-1) |
| P3 | shell: the queue and the four barriers (B1); R3 and R5; InstallSnapshot refuses first; the restore in `SetupInternal`, the apply backlog, the campaign hold; knobs; abort on error; disk mode refuses snapshots | 1.5-2.5 | the commit index durable and restored (0.5) |
| P4 | -- | -- | check form: O3-O5 (2-4); O6 optional (0.5-1.5) |
| P5 | lab soft restart and the cases of §2.8 | 1.5-3 | -- |
| P6 | `raft_bench` restart mode; a `dbtest` follower restart | 1.5-2.5 | -- |
| P7 | Mako: apply by entry origin, control entries on replay (uncertain) | 2-5 | -- |
| P8 | memory mode against today's gates; disk-mode baselines | 1-2 | -- |
| later | B2: a disk thread and deferred replies (srpc's `DeferredReply`, `src/srpc/rpc/server.rs:443-516`), inside the certificate | 3-5 | small (inference) |

Order: P0 first, because F decides whether B1 is enough. O1 goes with P2,
since a failing proof may reshape `Restore`; P4 comes before P5, so the
record's shape is settled before tests are written against it.

## 5. Risks and open decisions

Risks:

1. **The disk on zoo-003 is unknown:** F may be 0.1 ms (a flash-backed
   controller cache) or 5-15 ms (none). At 10 ms G1's latency grows several
   times and rounds miss their 5 ms deadline (inference). P0 answers it.
2. **Bandwidth.** With all three replicas on one volume, G2, G4 and G6 need
   0.5-2.7 GB/s of writes in all (§2.9).
3. **Unbounded log.** Restart time and memory grow with the log. Durable
   snapshots are needed for long runs, and they need the spec retarget (1-3
   days, inference), the snapshot coupling (3-5 days,
   `docs/verus/modification-plan.md:819`), and moving the snapshot protocol,
   about 950 lines, into the core (code-structure.md:39-44).
4. **Mako's replay hazards** may block full-cluster restarts (P7).
5. **Over-reading the result.** "Persistence verified" would be conditional
   on the shell's ordering, the store and the device, as in glr.
6. **Proof budget** in the two large handlers (O5).
7. **A lost or corrupt store** takes its replica out for good: rejoining
   empty under the old id is unsafe, and there is no reconfiguration.

Decisions for the owner:

| # | Decision | Options | Recommendation |
|---|---|---|---|
| 1 | Stay inside the certificate? | inside (no S3, durable commit, an fsync on each side of a commit-only round) / an uncertified fast mode | inside; measure; then B2, which stays inside |
| 2 | Durability rule trusted or proved? | assumed form / check form (+2-4 days) | check form: it is where Verus catches persistence bugs |
| 3 | Apply bound | the durable last index (free with two or more servers) / Rule A (+F; acknowledgements inside the theorem) | the durable last index; Rule A if the theorem must cover acknowledgements |
| 4 | Storage engine | Rust WAL / RocksDB through kernels | Rust WAL |
| 5 | Mako's replay hazards | fix in Mako / restart followers only at first | followers only at first |
| 6 | Snapshots | now / later | later, after P8, as their own phase |
| 7 | Disk mode in CI | from the start / later | from the start (assessment §8.16) |
