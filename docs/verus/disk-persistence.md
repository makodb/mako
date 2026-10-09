# Disk persistence for the Rust Raft: a design

**Status.** Built on branch `raft-disk` (2026-10-09) as
[disk-persistence-plan.md](disk-persistence-plan.md) records; a memory build
is unchanged, and its Raft state stays memory-only
([bugs-found.md](bugs-found.md) B6). The disk is simulated: tmpfs plus an
injected delay per flush. Code is cited at `44d07a3ee`, under
`src/deptran/raft/` unless a path starts `src/mako/`, `src/srpc/`,
`src/rocks_interface/`, `src/rusty-rustc/`, `docs/`, `ci/`, `examples/` or
`CMakeLists.txt`. `rocksdb/c.h` is the RocksDB 9.11.2 header Mako builds
against; `glr@40786a2b:` paths are in the ghost-log-refinement repository.

## 1. The shape: shell, core, shell

```
 poll thread (RPC handlers, fibers)   submit thread (Start)   apply thread
                 \______________________________|__________________/
 ==== mtx_ held ================================v=========================
  shell  step / step_checked  the step wrappers: the one way in
  core   RaftCore::step       pure; term, vote, log {term, handle}, commit
  shell  reply, CoreOutput {actions, log lines, persist note*}
         queue the record*    by the step wrapper, numbered in step order
         run_locked_actions   apply queue, timer, the no-op (a second step)
 ==== mtx_ released ======================================================
  shell  run_unlocked_actions; hold the reply or send* until the WAL is
         durable through the last record queued before the unlock; let it go
 flusher thread*  takes the queue: one batch, fdatasync, delay; publishes
 applier thread*  folds durable batches into the base, deletes old WAL
                                                             (* = new)
```

Every thread that changes Raft state takes `mtx_` (`src/server_h.rs:913-916`)
and calls a step wrapper (`step`, or `step_checked` for a message from the
network; `src/server_h.rs:1507-1546`), which ends in `RaftCore::step`
(`core/src/event.rs:370-371`). The core does no I/O and takes no locks
(`core/src/lib.rs:1-5`). The poll thread runs the RPC handlers, the
heartbeat and election fibers, and startup. One section can run several
steps (a won election also proposes the no-op). Five shell sites write
saved fields outside `step`, all under `mtx_` (§2).

| The core holds | The shell holds |
|---|---|
| term, vote, commit index, applied index, the snapshot's index and term, the election timer's state | atomic copies of some, for lock-free readers; the transport, the clock and the timer fibers, the apply queue and callbacks, the snapshot store |
| the log: per entry, its term, a 24-byte handle to the command and four facts cached from it (`core/src/log.rs:36-49`); a clone bumps a refcount and copies no bytes (`src/rusty-rustc/src/lib.rs:375-393`, `:885`) | the payload bytes, in C++ behind the handle; never changed once logged; encoded only when sent or flushed, after the lock |

**The core never touches the disk.** It controls the disk the way it already
controls the network: a step returns what to send (`AppendSend` records,
`core/src/heartbeat.rs:418`) and, in disk mode, what to save (a *persist
note*, §3). Under the lock the shell queues a record of what the step
changed; a flusher thread writes the queue to a write-ahead log (WAL); an
applier thread folds the WAL into a disk copy of the state, the *base*; at
startup the shell hands what both hold to the core as an event. etcd's
Raft library has the same shape (`Ready`: save, then send).

## 2. The saved state, and what one step changes

Saved: currentTerm (`current_term_`), votedFor (`vote_for_`), commitIndex
(`commit_index_`; Raft treats it as volatile, but the proof needs it, §6),
the latest snapshot, and the log after it (`raft_log_`). A snapshot is a
file, the state machine's image at an applied index S, plus S and the term
of entry S (`snapidx_`, `snapterm_`). The applied index is not saved: a
restart loads the snapshot and re-applies S+1 through commit.

Mako takes no snapshots today: only the Raft benchmark (`raft_bench.cc:1498`)
and lab cases (`src/lab_snapshot_cases.rs:504`, `:780`) set the callbacks.
`snapshot_callbacks.h` defines create(applied index), which returns the image,
and prepare(image, index), whose staged install `Commit()` makes live once Raft
has saved the snapshot. Until Mako supplies them, S = 0 and the log starts at 1.

| Step (thread) | currentTerm | votedFor | log | commitIndex | Leaves the server |
|---|---|---|---|---|---|
| `StartElection` (election fiber) | +1 | self | - | - | vote requests |
| `RecvRequestVote`, newer term (poll) | the candidate's | cleared; then the candidate, if granted | - | - | vote reply |
| `RecvRequestVote`, same term, granted (poll) | - | the candidate | - | - | vote reply |
| `SettleElection` or `RecvAppendReply` seeing a newer term (fibers) | the one seen | cleared | - | - | nothing |
| InstallSnapshot reply seeing a newer term (poll, leader; `ObserveTerm`, new) | the one seen | cleared | - | - | nothing |
| `SettleElection`, won (election fiber) | - | - | the no-op, by a second step in the same section | - | nothing yet |
| `Propose`, from `Start` (submit thread) | - | - | one entry at currentTerm | - | nothing yet |
| `RecvAppendEntries`, newer term, accepted (poll) | the leader's | cleared | cut at the first conflict, appended | may rise | append reply |
| `RecvAppendEntries`, newer term, refused (poll) | the leader's | cleared | - | - | refusal |
| `RecvAppendEntries`, same term, accepted (poll) | - | - | cut, appended | may rise | append reply |
| `TickHeartbeat` (heartbeat fiber) | - | - | - | may rise (leader) | this tick's AppendEntries: the entries, and the commit just raised |
| `RoundEnd` (heartbeat fiber) | - | - | - | may rise (leader) | nothing; the next round's AppendEntries carry it |
| `MaybeCreateSnapshot` (apply thread; shell code) | - | - | snapshot at S = the applied index; entries through S dropped | - | nothing |
| `OnInstallSnapshot` (poll; shell code, the term by `ObserveTerm`) | the leader's, if newer | cleared, if newer | the leader's snapshot at S; the entries after S kept if entry S matches, else all dropped | S | install reply |
| `Applied`, `StepDown`, `SetFollower`, timer, round and setup events | - | - | - | - | - |

- **The term never changes alone**: a vote write, cleared or self, follows
  each term write, as in the proof's model of a step-down. The vote changes
  alone only on a same-term grant; `Propose` changes only the log.
- **A follower changes all four in one step.** At term 4 it holds entries
  1-10, of which 6-10 are uncommitted, of term 2. The leader of term 5
  sends prev 5, entries 6'-8' and commit 8. In one step the follower sets
  term 5, clears its vote, cuts 6-10, appends 6'-8', raises commit to 8 and
  replies (`core/src/node.rs:1902-1903`, `:2095`, `:2128`, `:2148`).
- **Five shell sites write saved fields outside `step`**, under `mtx_`;
  the replay recorder taints each (`src/server_h.rs:1433`, `:2180`,
  `:2260`, `:2343`, `:2919`), which makes them the checklist for records.
  Two raise the term and clear the vote, then step `StepDown` or
  `SetFollower`, which leave both alone, so no persist note covers the
  raise: InstallSnapshot (`:2334`; the writes at `:2414-2415`) and an
  InstallSnapshot reply with a newer term (`:2268-2269`). Both raises move
  into a new core event, `ObserveTerm` (§3), so only core steps write term
  and vote and the reply's site needs no record. Two need a record the
  shell builds (§3): taking a snapshot, and InstallSnapshot's snapshot, log
  and commit. Startup's snapshot recovery (`:2180`) runs before `Restore`,
  which overwrites it. Compaction (`:1414-1442`) drops only entries at or
  below S (`core/src/helpers.rs:388-391`), outside the saved state.

**Why one ordered record per step keeps a crash safe.** Each step becomes
one record, all or nothing, and records reach the disk in step order, so a
crash leaves the state after some whole step, one the server passed
through. Replay applies each record whole, inside one atomic batch (§4).
Written separately and in any order, the follower's step could leave:

| On disk | Missing | After the restart |
|---|---|---|
| the cleared vote | term 5 | term 4 with its term-4 vote erased: it can vote twice in term 4 |
| entries 6'-8' | the cut of 9-10 | 9-10 of term 2 after 8' of term 5: a log no leader holds |
| commit 8 | entries 6'-8' | the old 6-8 applied as committed; the cluster committed 6'-8' |
| entries of term 5 | term 5 | entries above the stored term, a state no step produces |

## 3. The design

**A write-ahead log, and a copy of the state beside it.** A step changes
memory at once; only what leaves the server waits for the disk, as a
`write()` is visible at once but durable only after `fsync`. Holding each
output until the WAL holds the state it depends on makes the server
synchronous to the outside. A WAL alone shrinks only when a snapshot drops
a prefix, and Raft must not depend on Mako's snapshots. So the shell also
keeps the base, the saved state as of WAL record c, advanced in the
background; the WAL keeps the records after c, and base + WAL is the state
a crash leaves.

**Records, under the lock.** The core adds a *persist note* to its output:
the new term, vote and commit if any changed, and `log_from`, the lowest log
index the step wrote (only `append_local` and the AppendEntries handler
write the log). The step wrapper turns it into a record, the three values
plus "replace the log from `log_from` with these entries" (each entry's term
and a clone of its handle), and pushes it on a queue that numbers it in step
order; the two snapshot paths of §2 push records they build. Term and vote
change only in core steps: where InstallSnapshot or its reply carries a
newer term, the shell steps a new event, `ObserveTerm { term }`, which
raises the term, clears the vote and the leader hint, and steps down, as
`SettleElection` does (`core/src/node.rs:887-898`). A record states
outcomes, never rules ("snapshot (S, T, f), suffix dropped, commit S", the
drop decided from the local term of entry S, `src/server_h.rs:2537`), so
replay decides nothing. Nothing is encoded or written under the lock.

**The flusher thread** owns the WAL. It takes every queued record, encodes
the payloads (the `raft_command_encode` kernel, `rt/src/seam.rs:371`),
appends them as one batch framed by a length and a checksum, calls
`fdatasync`, sleeps the injected delay, and publishes the durable sequence
number d, last index and commit: one flush for what every thread queued
(group commit). The WAL is a series of 64 MB segments, each headed by the
server's identity and its first sequence number; a new one's header and
directory entry are synced before the old one is closed.

**What waits for the disk.** Nothing a critical section produced leaves the
server until the WAL is durable through the last record queued when the
section ended (its *tail*): a reply can acknowledge entries earlier steps
appended, and carry a term an earlier step raised. The tail covers each
output's read set ("Needs on disk") without tracking reads.

| Output | Thread | Needs on disk | How it waits |
|---|---|---|---|
| vote reply | poll, inline handler | term, vote | held until durable |
| append reply (with or without entries) | poll, inline handler | term, vote, the log through the acknowledged index, commit | held until durable |
| install reply, and making the staged image live | poll, inline handler | term, vote, the install's record (its file synced first), commit | blocks on the flusher's condition variable, apply gate held, `mtx_` released |
| a campaign's vote requests | election fiber | term + 1, the self-vote | yields between `StartElection`'s section and the broadcast |
| the leader's AppendEntries | heartbeat fiber | the entries sent, the commit they carry | yields between the tick's section and the sends |
| a leader's apply | apply thread | that entry | blocks before Mako's apply callback |

- **Replies are held, not blocked.** The four RPCs run inline on the poll
  thread (`rt/src/rpc.rs:386-404`, `src/srpc/rpc/server.rs:1476-1479`), and
  the generated dispatch replies as the handler returns
  (`rt/src/rpc.rs:377-382`, `:476-492`). srpc's `DeferredReply`
  (`src/srpc/rpc/server.rs:443-522`) has the shape but not the type: no
  Rust handler gets one, and it is not `Send` (`:145-150`, `:303`). So
  `__dispatch__`, which owns the `Box<Request>` (`rt/src/service.rs:97-99`),
  replies at once if the tail is durable, else keeps (request, connection,
  reply, tail) on a held list only the poll thread touches.
- **The flusher wakes the waiters.** After publishing d it signals a
  condition variable, for the threads that block, and queues one job on the
  poll thread through a gate like `ReplicationWakeGate`
  (`src/server_h.rs:224`, `:2969`). The job sends the held replies now
  durable and sets the `IntEvent`s the heartbeat and election fibers yield
  on, as `WaitForReplicationOrHeartbeat` does (`:2993`).
- **InstallSnapshot blocks**, inline, `mtx_` released: it holds the apply
  gate, a C++ mutex, until the image is live (`src/server_h.rs:4217-4220`),
  and its zero-copy hand-off lasts one call (`rt/src/snapshot.rs:282-286`).
  It is rare and slow anyway; the flusher never takes that gate or needs
  the poll thread, so the wait ends.
- **One exception.** The heartbeat tick sends InstallSnapshot under `mtx_`,
  before any wait (`src/server_cc.rs:104-124`): safe, as the image is a
  committed prefix a majority holds on disk and the leader's term was
  durable before its vote requests left; outside the proof anyway (§6).

Nothing else waits: `Start` returns once its entry is queued; a step that
sends nothing rides the next flush; a follower's apply is memory a restart
rebuilds; a local snapshot waits for nothing.

A leader's apply waits because it tells Mako a transaction is replicated; a
leader never cuts its log, so `i <= durable_last` means entry i is on disk.
With two or more servers the wait is free: a commit needs a follower's
acknowledgement of an entry the leader flushed before sending it, which is
why the commit rule may count the leader at its in-memory tail
(`core/src/heartbeat.rs:103-104`). Ongaro's parallel write (thesis
§10.2.1), sending before the leader's own flush ends, is outside the proof
(§6) and needs the commit rule to count the leader at `durable_last`.

**The base and the applier.** The base holds term, vote, commit, the
snapshot's (S, T, file), the entries S+1 to last, and c, written in the same
atomic batch as the data: base = the replay of records 1 to c. The flusher
hands the applier thread each synced batch's bytes on a bounded queue that
drops a batch when full; when the next batch starts above c+1, the applier
reads c+1 to the published d from the segments. It applies records in order
with recovery's parser, so the base is a replay by construction, of durable
records only (c <= d). Every 256 MB of WAL (more than a segment: the open
one is never deleted) or 10 s it checkpoints: it makes the base durable
through c (§5), then deletes the closed segments whose records are all at or
below c, and the images older than the one it names.

**Why a lagging base is safe.** A record is computed from memory, so
replaying it reproduces its step only on the state that step began from.
Four facts give that. (1) Every write to a saved field is queued as an exact
record in its section (the persist notes and the two snapshot paths, §2), so
the step that builds record r starts from the replay of 1 to r-1 and ends at
the replay of 1 to r. (2) Records are numbered under `mtx_`, in the order
they change memory. (3) The flusher writes in number order and publishes d
only once `fdatasync` covers 1 to d, so a durable record's predecessors are
durable. (4) The base is exactly the replay of 1 to c. A crash leaves the
base at some c no older than the last checkpoint and the WAL durable through
some d >= c; replaying c+1 to d onto the base gives the replay of 1 to d,
the state after record d's step, which holds everything any output revealed.
A lag costs only replay time. Recovery skips the records at or below c still
in the WAL only to save work; replaying them is harmless, as `apply` (§4)
writes values at fixed keys and every later record is replayed after them.
Through the core's log they would do harm: a cut below its start clears it,
and appends go to its tail (`core/src/log.rs:404-416`, `:314-323`).

The same facts make early lock release safe: a section sees only writes of
sections that held `mtx_` before it, so its tail covers all it read. If an
AppendEntries sets term 5 (record 10) and a RequestVote then votes for X in
term 5 (record 11), the vote reply waits for 11: a crash between them loses
only a vote that never left. Each output must be fixed in its own section,
and records exact (Decision 4).

**Snapshots: the file, then the record.** An image is written outside
`mtx_`, to a file of its own (a temporary name, `fdatasync`, rename,
`fsync` of the directory); only then does the snapshot path queue a record
naming it, so a crash in between leaves the previous snapshot and the
longer log. The applier drops the base's entries through S; an image is
deleted once the base durably names a newer one. The WAL never waits for a
snapshot. The latest image also stays in memory, for lagging followers.

**Visible before the flush**, none a send or reply. Mako reads the atomic
copies (`publish_mirrors`, `src/server_h.rs:1753`) to route and count its
submissions (`raft_worker.cc:708`, `:871`, `:890`); with two or more servers a
commit already means a majority saved the entries. Of the leader-change
callbacks, "became follower" stays true after a crash, as a restart begins as
a follower, and "became leader" rests on a term and vote saved before the vote
requests left (Decision 12). The apply queue holds only committed entries.

**One lock, one queue.** Most steps read the log and write term, vote or
commit, or the reverse: a vote reads the log's tail
(`core/src/node.rs:1553-1554`), `Propose` the term (`:511`), the commit rule
entry terms (`core/src/heartbeat.rs:114`). A separate log lock would let a
server vote for T+1 on a log tail read before an append of term T that it
then acknowledged; the leader can commit with that acknowledgement, and the
candidate, missing the entry, win.

**Threads.** The flusher and the applier share one `Arc`'d store object
(queue, segments, base, durable atomics, condition variable, wake gate) and
nothing else, so neither can call `core()`, guarded only by the `mtx_`
convention (`src/server_h.rs:1299`). Both are joinable, like the apply thread
(`:2627`), and stop after the poll thread stops producing records; held
replies go before the RPC drain, which counts each (`raft_worker.cc:253`).

**Startup.** Recovery (§4) merges the base and the WAL into one state
before `SetupInternal` (`src/server_h.rs:1847-1972`), RPCs closed, and
injects the image that state names, as the snapshot store is memory-only.
The snapshot recovery (`:1898-1907`) keeps an injected store
(`rt/src/snapshot.rs:204-216`) and loads it; its reconciliation sees an
empty log and only sets the boundary; its term bump (`src/server_h.rs:2195-2202`) is
skipped in disk mode. With snapshots off it loads nothing (`:2008-2025`),
so a state naming a snapshot fails closed, saying so. Between `Configure`
(`src/server_h.rs:1914`) and `EnterGates` (`:1933`; it passes only with S = 0 and the log
from 1, `core/src/node.rs:317`) the shell steps a new core event,
`Restore`, with term, vote, commit and the entries after S. It sets no
persist note, and fails closed on a state no step produces: a gap after S,
an entry without a value or of term 0, terms that decrease, start below the
snapshot's term or exceed the stored term, commit outside S to last, or a
vote for a non-member.

## 4. The algorithm

```text
// The step wrappers (src/server_h.rs:1510, :1530), under mtx_ after setup;
// a None from step_checked changes nothing (core/src/event.rs:334).
fn step(ev, out) -> Reply
    reply = core.step(ev, out)               // pure; may set out.persist
    if disk build and out.persist.take() = p: // queue lock; seq += 1
        queue.push(Record { hard: p.hard, replace_from: p.log_from,
            entries: [(e.term, clone(e.cmd)) for e in log from p.log_from] })
    return reply              // the two snapshot paths push their own

// A critical section whose result leaves the server.
{ lock mtx_; r = step(..); run_locked_actions(out); tail = queue.last_seq }
run_unlocked_actions(out)
if durable_seq >= tail: reply or send r     // nothing leaves before this
else if an RPC handler: held.push((req, conn, r, tail))   // poll thread
else: yield on the fiber's IntEvent until durable_seq >= tail; send r
// A leader's apply of entry i waits on the condvar while i > durable_last.

// The flusher thread. Never takes mtx_.
loop
    batch = queue.take_all()                 // waits for one; seq order
    bytes = frame(len, crc, [encode(rec) for rec in batch])
    if seg.size >= 64 MB: seg = a new segment headed (identity, first seq =
        batch.first.seq), fdatasynced; fsync the directory; close the old
    seg.write(bytes); seg.fdatasync(); sleep(delay)   // an error aborts
    publish durable_last, durable_commit, durable_seq = batch.last.seq
    condvar.notify_all(); queue the wake job on the poll thread
    applier.offer(bytes)                     // bounded; when full, drop it
// The wake job: reply to each held entry with tail <= durable_seq; set
// the heartbeat and election fibers' IntEvents.

// The applier thread. Holds no Raft lock.
loop
    bytes = next offered batch if it starts <= c+1, else read c+1..durable_seq
    for rec in parse(bytes) with rec.seq > c: apply(wb, rec); c = rec.seq
    base.write(wb + put(c))                  // one atomic WriteBatch
    if WAL > 256 MB or 10 s since the last checkpoint:
        base.flush(wait)                     // durable through c
        delete closed segments whose records are all <= c
        delete the images older than the one the base names

fn apply(wb, rec)                            // recovery's too
    if rec.hard: put it
    if rec.snapshot = (S, T, f, keep): put it; delete_range(..=S)
        if not keep: delete_range(S+1..)
    if rec.replace_from = i: delete_range(i..); put the entries from i

// Taking a snapshot: the apply thread, holding the apply gate.
S = applied index; image = create(S)         // Mako's callback
f = write image to a new file; fdatasync; rename; fsync the directory
lock mtx_: T = term of entry S; snapshot := (S, T); drop entries <= S;
    queue.push(Record { snapshot: (S, T, f, keep: true) })  // no wait

// InstallSnapshot from the leader: the poll thread, inline. Blocks.
f = write image to a new file; fdatasync; rename; fsync the directory
lock apply gate                              // until the image is live
{ lock mtx_; if term is newer: step ObserveTerm{term}   // a persist note
  if not stale and (staged = prepare(image, S)) succeeds:
      keep = (term of entry S == T); snapshot := (S, T); commit := S
      keep or drop the log after S, as keep says
  queue one record of the outcome; tail = queue.last_seq }
wait on the condvar until durable_seq >= tail     // mtx_ released
if staged: staged.Commit() (fail stop on error); publish applied = S
else: delete f
unlock apply gate; reply

// Recovery: before SetupInternal, RPCs closed.
fail closed unless the directory is on a local filesystem (not NFS)
if no base: fail closed (B6) unless this launch creates the cluster; then
    delete any leftover <store>.creating; in it write the base (header,
    c = 0) and segment 1, sync; rename it to <store>; fsync the parent;
    start empty                 // a .creating alone, no flag: fail closed
fsync the store's parent directory       // B31: a creation killed after its
                                         // rename, before this sync
(c, state) = open the base; delete the last segment if its header is short
    or torn (it holds no records); segs = the segments by first seq
fail closed if segs is empty or a header's identity differs from the base's
drop each segment whose successor starts at or below c + 1
fail closed unless segs[0] starts at or below c + 1 and the rest are
    contiguous; read them, skipping records <= c; a bad batch may only end
    the last segment (a torn write, never durable): cut it there, else
    fdatasync the last segment (B30: a killed process's page cache kept
    batches no sync covered, and this read saw them)
d = the last good record (c if none); fail closed if c > d
apply c+1..d to the base and to state; base.flush(wait); create a segment
    at d + 1, fdatasync it, fsync the directory; delete the older segments
delete the images state does not name; if it names (S, T, f), fail closed
    if snapshots are off or f is bad, else inject a snapshot store holding f
SetupInternal: snapshot recovery loads it; Configure; lock mtx_; step
    Restore{state.term, state.vote, state.commit, entries after S}, whose
    actions queue APPLY_RANGE(S, commit); durable_* := state, d; seq := d
continue: EnterGates, apply thread, flusher, applier, RPCs, fibers
no campaign until applied >= state.commit   // Mako's callback is by role
```

## 5. Storage

Mako has no general storage manager that fits Raft. Three pieces come close:

| Code | What it is | Why Raft cannot use it as is |
|---|---|---|
| `mako::RocksDBPersistence` (`src/mako/rocksdb_persistence.{h,cc}`) | Mako's copy of each transaction batch, beside the Raft submission; feeds Mako's disk watermark | unsynced (`sync = 0`, `src/mako/rocksdb_persistence.cc:131-133`) and never truncated; opened only by the initial leader, at a per-pid path, never read at restart (`src/mako/mako.hh:912-920`; only the offline `rocksdb_replay_app` benchmark reads it); one per process, but the lab runs five servers in one; `libmako` links Raft's library, so Raft calling it inverts the layering |
| `LogStorage`, `RocksDBLogStorage` (`log_storage.hpp`, `rocksdb_log_storage.hpp`) | the old C++ Raft's log store | no live user (Paxos's `SetLogStorage` is never called); atomic only within one call, so hard state (`set_metadata_batch`), the cut (`remove_range`) and the entries (`put_batch`) are separate writes; its `sync()` is a memtable flush, not a log sync (`rocksdb_log_storage.hpp:756-776`); entries are C++ `LogEntry`s with Paxos fields |
| `src/rocks_interface` | a RocksDB-shaped API over Masstree | no RocksDB inside; its writes replicate through Raft itself |

**The WAL: a small Rust log of our own**, owned by the shell. A segment's
header holds magic, version, site, partition, configuration fingerprint,
command format, first sequence number and a checksum; each batch has a length
and a checksum, so it is all or nothing and a torn last write is cut whole.
Payloads go through the existing codec kernels (`rt/src/seam.rs:371`,
`rt/src/service.rs:43`): no new C++. Unit tests run in cargo, on an
in-memory backend that drops unsynced writes; crash tests kill processes
(below).

**The base: RocksDB, its own WAL off.** One instance per server, called
from Rust through `rocksdb/c.h`, which Mako already uses
(`src/mako/rocksdb_persistence.cc:131-133`) and Raft's library links
(`CMakeLists.txt:1309-1322`). One column family: keys for the hard state,
the snapshot, c and the header, and one per entry. One `WriteBatch` per
applied batch (`rocksdb_write`, `rocksdb/c.h:484`), c inside, `DeleteRange`
for a cut or a dropped prefix (`:844`), RocksDB's WAL disabled (`:2081`):
legal exactly because our WAL is the log above it. The durability point is
a flush that waits (`:701`, `:2153`); with one column family, a crash
leaves the base as of a whole batch at or after the last such flush. Small
write buffers, as the lab runs five servers per process; cargo unit tests
use an in-memory base that drops unflushed batches. Plain files would need the
same machinery (hard state, cut, prefix, entries and c changed atomically:
chunk files and a renamed manifest, a small LSM), and rewriting the whole
base at each checkpoint is quadratic without snapshots.

**The simulated disk.**

- **Which build.** Disk mode is a build, not a run-time choice:
  `-DMAKO_RAFT_DISK=ON` turns on the Cargo feature `raft_disk`, as
  `-DRAFT_TEST=ON` turns on `raft_test` (`CMakeLists.txt:1226-1233`).
  Off, the disk code is compiled out and the binary is today's memory
  mode, so its speed cannot move; the core is the same in both builds.
  The shell tests `cfg!(feature = "raft_disk")` in expressions where it
  can, so every build compiles the disk path. Environment variables only
  tune a disk build: the directory, the delay, the checkpoint thresholds.
- **Where.** The local disk, by default
  `/var/tmp/raft-wal-${USER}/<run>/<site>-<partition>/` (ext4; an environment
  variable may move it). Not tmpfs, which is RAM the tests need (on 2026-10-09
  tmpfs leftovers ran the host out of memory); never under `/tmp/${USER}_*`,
  which `examples/run_rocksdb_test.sh:27` deletes whole and `ci/ci.sh:84` in
  part. The store opens only on a local filesystem type, so it can never land
  in the NFS home. Each run's directory is locked by its launcher, deleted at
  exit, and swept by the next launcher if a kill left it behind (plan §1).
- **Cost.** On the local disk (`/var/tmp`, ext4) write plus `fdatasync`
  takes about 180 µs for 4 KiB and 1.6 ms for 1 MiB (p50, zoo-003,
  2026-10-09): a real sync, to which the injected delay, slept on the
  flusher, adds. On tmpfs (still selectable) `fdatasync` takes about 1 µs,
  so there the delay alone models the device; sweep 0.2, 1 and 5 ms. A reply waits for two flushes, the
  leader's and the follower's. The base adds no latency, but entries are
  written again, and rewritten by RocksDB's flushes and compactions (cheap
  for sequential keys, not free); the delay models only the WAL.
- **Space.** The local disk has 1.7 TB free (2026-10-09); tmpfs is RAM. The
  WAL stays under about 256 MB plus a segment. The base is the Raft log:
  without snapshots it grows (three replicas at the unthrottled 4 KB rate,
  about 37,800 entries/s each, write about 0.5 GB/s), and RAM, as the core
  keeps the whole log, and restart, which decodes every entry and re-applies
  1 to commit to Mako (no state file), are the real limits. With snapshots,
  one image and 10,000 entries by default (`MAKO_RAFT_SNAPSHOT_INTERVAL`).
- **Tests kill processes.** Persistence is tested on processes, each
  holding one Raft server and its RPC server, never in the lab, whose five
  replicas share one process. A driver kills them with SIGKILL, at random
  and at named crash points (inside a batch, around `fdatasync`, in a
  rotation, around a checkpoint, inside recovery), and restarts each from
  its directory. A killed process keeps what it wrote, as the page cache
  survives, so a crash point can first simulate a power cut: it truncates
  the open segment to its last `fdatasync`, and undoes a file creation or
  rename that no directory `fsync` covered. Each node logs what every
  output reveals (term, vote, log tail) just before sending it; after a
  restart its state must cover every such line, which checks the WAL rule
  itself. The cargo unit tests check the same orders more cheaply.

## 6. What Verus adds

**Without snapshots, the Raft model needs no change**: a restart is its
"step aside" move, which keeps term, vote, log and commit and resets only
the role and the votes granted (`core/src/coupling.rs:468-473`). What the
proof forces is in the design. Commit is saved: the invariant equates the
core's state with the replay of its *ghost log*, a proof-only list of its
Raft moves (`:305-308`), and no move lowers a commit index. Restart is a
core event. Whole steps reach the disk in order before anything they
produced leaves, as the cluster proof needs each received message's send
in its sender's ghost log: so the leader flushes before it sends. And
persist notes are exact, as follows.

**`Restore` takes the previous run's ghost log, `prev`, as a ghost
argument** (it is not on disk) and sets the ghost log to
`step_aside_log(prev)`, reusing the step-aside lemma, with the ghost match
and next indexes of `replay(prev)` (`core/src/coupling.rs:278-298`); about
1-2 days. `prev` is cut at the last step before the first lost record's
step, not at the last durable record: steps with no record still send
(heartbeats with nothing new, and the followers' success replies to them),
their outputs leave once their tail is durable, and the cluster proof needs
those sends ([host-contract.md](host-contract.md) §2). `replay(prev)` agrees
with the store only if a step with no persist note changes no saved field
and every note is exact, so exactness is now required: proved (2-4 days) or
trusted. glr did the same for raft-rs, `new_from_store(store, prev)`
(`glr@40786a2b:docs/ghost-log/raftrs/composition.md:150-173`). Optional and
cheap: a spec function applying a record, and the fold lemma (c+1 to d
over the replay of 1 to c is the replay of 1 to d), make §3's argument a
theorem.

**Snapshots stay outside the proof**: the spec does not model them, the
invariant requires `snapterm_ == 0` (`core/src/coupling.rs:309`), and
`MAKO_RAFT_VERIFIED_GATES=1` turns them off. Bringing them in takes spec
moves for compaction and InstallSnapshot, and both paths moved into `step`.
`ObserveTerm` stays outside too: the spec's step-down move needs a modeled
message carrying the term (`core/src/coupling.rs:614`, `:636`), and
InstallSnapshot is not one, so the event's coupling premise is false
(`core/src/coupling.rs:2585-2613`) and Verus checks only the core's
invariant for it; with snapshots off it never steps.

**Still trusted:** the WAL (a batch is all or nothing, durable once
`fdatasync` returns, its segment's header and directory entry synced first);
the base engine (a crash leaves a whole-batch prefix at or after the last
waiting flush); the applier (durable records only, whole, in order, gapless,
c in the same batch); segment deletion (after that flush, records at or
below c); the merge at recovery; the images and Mako's two callbacks; the
queue, held replies and waits; decoding; the abort on error. Once built,
this changes §1 items 4-5 (item 5: "none persisted"), §3 and §6's closing
paragraph of [host-contract.md](host-contract.md); B6 retires in disk mode.

## 7. Decisions

| # | Decision | Recommended |
|---|---|---|
| 1 | Stay inside what the proof covers: commit saved, whole steps, exact persist notes, no send before its flush | Yes. The price: one flush on each side of a round that only announces a new commit, off the commit's latency path. |
| 2 | Where the flush runs | A flusher thread, from the start: the 5 ms delay to sweep equals the 5 ms heartbeat (`server.h:169-173`). Replies are held, fibers yield, the apply thread and InstallSnapshot block (§3). |
| 3 | Engine | WAL: our own Rust segments. Base: RocksDB through its C API, called from Rust, its WAL off, one column family, c in every batch, a waiting flush as the durability point. |
| 4 | Who builds the persist note | The core, proved exact (or trusted, §6): it already knows where a cut begins (`first_write_index`). Not a before-and-after diff in the shell: a cut followed by an append of the same length leaves the log's length unchanged, so only an entry-by-entry scan under `mtx_` would find where the cut began. The two snapshot paths build theirs by hand; term and vote change only in core steps (`ObserveTerm` for the InstallSnapshot paths, §3). |
| 5 | How far the apply thread may go | A leader's durable last index, free with two or more servers; a follower's apply is memory a restart rebuilds. The durable commit index only if Mako's acknowledgements must be inside the proof (one more flush per commit). |
| 6 | Mako's `RocksDBPersistence` | Leave it alone at first (other data, directory and lifecycle); decide later whether Raft's log replaces it, which changes Mako's disk watermark. |
| 7 | Snapshots and InstallSnapshot | On, as files (§3): an image is synced before the record that names it, and deleted once the base durably names a newer one, or at recovery if the state does not name it. The WAL never waits for one. Off under the verified gate until the spec models them (§6). |
| 8 | What restarts first | Followers. Mako picks the apply callback by role, and its control entries (the advancer marker, epoch no-ops, reset) run again on re-apply; whole-cluster restarts wait until those are checked. |
| 9 | The unused C++ `LogStorage` files | Leave them; a separate cleanup. |
| 10 | A missing store at startup | Start empty only on a launch that creates the cluster. Otherwise a missing base or segment, a WAL gap or a mismatched header fails closed, as does a missing image the state names. Creation is atomic: built in `<store>.creating/`, synced, renamed into place, so a crash while creating leaves no half-made store; a creating launch deletes a leftover `.creating` and starts over. |
| 17 | Where stores live, and who removes them | The local disk (`/var/tmp`), never tmpfs or NFS; the store refuses a non-local filesystem. Launchers lock their run directory, delete it at exit, and first sweep the run directories no live launcher holds. |
| 11 | Who writes the image file | Raft, from the bytes `create()` returns, as today's callbacks have it. If Mako's image is too large to hold in memory, `create()` writes the file itself and returns its name and checksum, and a leader reads that file to send it. |
| 12 | Leader-change callbacks: before or after the flush | Before, as today: each case is safe (§3), and the atomic copies already show state before the flush. Move them after the wait if a callback ever starts revealing state. |
| 13 | When the base checkpoints | Every 256 MB of WAL or 10 s; the threshold must exceed the 64 MB segment: only closed segments are deleted, so below it the open segment alone would keep the threshold crossed and force a checkpoint after every batch. |
| 14 | A base error | Stop the applier, and so truncation: the WAL still holds every record. A WAL error aborts. |
| 15 | How disk mode is chosen | A build switch, `-DMAKO_RAFT_DISK=ON` (the Cargo feature `raft_disk`). Off is today's memory-mode binary, the disk code compiled out; the core is the same in both. Environment variables only tune a disk build. |
| 16 | How persistence is tested | By killing processes, each one Raft server with its RPC server, with SIGKILL at random and at crash points, some simulating a power cut, then restarting them; the recovered state must cover every output a node logged before sending it. Not in the lab. Cargo unit tests over in-memory backends check the same orders. |
