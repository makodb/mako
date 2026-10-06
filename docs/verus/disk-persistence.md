# Disk persistence on verus-raft: a design, and what Verus adds

This describes branch `verus-raft` at `d9c2cb245` (2026-10-06); the commits
after it change only this document. It builds on the untracked assessment
`/home/users/zyang2/mako-srpc-adopt/docs/raft-disk-assessment.md`
(2026-10-03), which was written against `srpc-subtree-forward` at
`8fc195615`, the Rust Raft before the Verus work; this branch is that tree
plus 82 commits. It answers the owner's questions (§0): how to add disk
persistence to the current code, assuming the known bugs (B1, B4, B7, B8,
B14, B16-B19; their titles are in Appendix B) are fixed, and whether Verus
makes it harder. B6, the memory-only state, is the gap this closes. Nothing
here was built, run or verified, except the flush timings of §3.7. Effort
figures are estimates in agent-days, builds and benchmarks included.
"Inference" marks a conclusion drawn from the code rather than read in it.
Every label and technical term is defined in the glossary (Appendix B).

Paths follow [code-structure.md](code-structure.md): relative to
`src/deptran/raft/` (`src/server_h.rs` is the shell, `core/src/node.rs` the
core), except those beginning `src/deptran/`, `src/srpc/`, `src/rusty-rustc/`,
`src/mako/`, `scripts/`, `docs/`, `ci/`, `examples/`, `docker_build.sh` or
`CMakeLists.txt`, which are from the repository root. `origin/mako-dev:`
paths are mako-dev's old C++ Raft at `3e102604d`, read with `git show`;
`d288748f5^:` paths are the srpc-lineage C++ Raft that commit `d288748f5`
removed. `glr:` paths are in the ghost-log-refinement repository, read at
`40786a2b`, glr's main (`origin/main`); the cited lines are unchanged at
`0e48c6c2`, the head of its `work/mako-raft-log-refinement` branch.
`glr@d7e04ed7:` marks our spec pin, whose lines are those of commit
`d7e04ed7` (of the pin's two patches, only S1 touches `raft.rs`, and only
`LRejectAppendEntries`, `glr@d7e04ed7:src/protocol/Raft/raft.rs:593-619`,
which nothing here relies on; `verus/spec/SPEC_VERSION.toml:4-8`).
`rocksdb/` paths are the RocksDB 9.11.2 headers Mako builds against, under
`/home/users/zyang2/.local/mako-deps/usr/include/`. "assessment §N" is the
assessment's section N; its line numbers refer to the other tree.

## 0. Short answers to the owner's questions

Two words recur. A **step** is one call of the verified core,
`RaftCore::step` (`core/src/event.rs:370-371`; a network message enters
through `step_checked`, `:327`, which calls it): one complete handler run,
such as "answer this vote request" or "accept these entries and reply", run
to completion while the shell holds the server's lock `mtx_`. The proof
divides a step into zero or more moves of its Raft model; a vote at a higher
term, for example, is two moves: "adopt the new term", then "grant" (§1.4).
A **flush** means here: write the records queued since the last flush to
the log file, then wait for `fdatasync` (and, on the simulated disk, the
injected delay). A flush takes **W + F**: W to write the records, which
grows with their size, and F to wait. Since `fdatasync` on tmpfs takes
about 1 µs, F is in effect the injected delay (Q2, §3.7).

**Q1. "Would having verus add more difficulty?"** Yes, but modestly, and
mostly as design constraints rather than proof work.

- **The Raft model we prove against needs no change.** It already has a move
  called "step aside": become a follower and forget the election state, but
  keep the term, the vote, the log and the commit index. If those four are
  on disk, a restart is exactly that move. glr (Q5) used the same reading
  for raft-rs in its phase A11 (2026-09-28); our pin has the move
  (`glr@d7e04ed7:src/protocol/Raft/raft.rs:210-236`). So the plan's "spec v3,
  4-8 days" (`docs/verus/modification-plan.md:837-852`) is not needed (§4.1,
  §4.2).
- **Four design choices become fixed** if restarts are to stay inside what
  the proof covers. All four come from one rule, the subject of Q7 and §1:
  1. save the commit index too, and load it back instead of starting at 0;
  2. save a step's changes, and every earlier step's, before sending
     anything that step produced, even a follower's reply that only
     acknowledges a higher commit index, and a leader's round that only
     announces one;
  3. restart the core through one new input, `Restore`;
  4. the leader does not send entries before they are on its own disk (the
     assessment's S3 is excluded). Asynchronous I/O stays allowed; sending
     before the flush does not.
- **New proof work is small.** Show that `Restore` yields a core the proof
  accepts (1-2 days; it reuses the step-aside lemma). Optionally, for 2-4
  more days, prove that each step reports exactly what it changed and asks
  for a flush whenever it sends, so that a forgotten save becomes a proof
  failure instead of a silent bug. Whether the shell really waits for the
  disk before sending stays review and tests (§4.3-§4.5).
- **Where Verus helps.** The Verus refactoring already made `step` the only
  writer of term, vote, commit and log inside the verified configuration:
  one place to hook persistence instead of the assessment's nine term/vote
  sites in two files (§2.4, Appendix A). And `Restore` must establish the
  core's invariants, which forces some of the store checks the assessment
  recommended (§4.3).
- **Run-time cost of staying inside.** The same as plain save-before-reply
  Raft, plus one flush on each side of a round that carries only a new
  commit index (§4.2). Under load that announcement rides on a round that
  carries new entries, which flushes anyway. Under light load (G1, G3, G5)
  it costs one flush on the leader and one on each follower, after the
  client already has its answer. Against the fastest design that the proof
  does not cover (early-send, the assessment's S3), about one F more per
  commit (§3.9).

**Q2. "How much time does it take to flush to /tmp?"** About as long as
writing the bytes, plus the injected delay; the wait itself is about 1 µs.
Measured on zoo-003 (2026-10-06, §3.7), in two parts:

1. **The wait, `fdatasync`: about 1 µs** (0.5-1.0 µs at the median, under
   10 µs at p99.9, whatever the size). tmpfs keeps files in RAM, so there is
   no device to wait for: the time is the system call.
2. **The write, W, which grows with the record:** at the median about 3 µs
   for 4 KB, 0.10-0.16 ms for 256 KB, 0.4-0.7 ms for 1 MiB and 0.7-1.1 ms
   for six 286 KB entries. Appending to a tmpfs file costs 5-7 times a plain
   memory copy of the same bytes (1 MiB: 0.07-0.1 ms).

So a flush to `/tmp` takes W + F, where F is the injected delay: since the
wait costs almost nothing on tmpfs, the design adds a delay per flush to
model a disk (§3.7). For the large payloads of G3-G6, W is comparable to
the delays proposed for F, 0.2 and 1 ms. For reference, `fdatasync` on ext4
at `/var/tmp` takes 0.16-0.31 ms at the median.

**Q3. "We simulate disk write ... home directory is NFS, flushing tons of
bytes to it will cause problems."** Agreed, and the design follows it. The
store never goes to the home directory: `/home/users`, which also holds this
worktree and its build directories, is NFS, and at G2's rate one replica
would write about 155 MB/s to the file server. The store lives in `/tmp`
(tmpfs, in RAM) under its own prefix, at an absolute default path. Three
consequences: full-rate runs fill `/tmp`'s free 24 GB in under a minute, so
they must be short or use a timing-only store; a reboot of the machine
erases every store; and Docker's `/tmp` is not tmpfs (§3.7).

**Q4. "Do you think it's necessary to write a storage manager for the raft,
so that disk persistence can be realized, or we just mechanically
substitute everything memory r/w with disk r/w? If the latter is not
possible, why?"** A storage manager, but a small one. Mechanical
substitution is excluded on this branch by design: the fields are written
inside the verified core, whose rule is that it does no I/O and never waits.
It would need a trusted store call at each write site inside the core (the
core already calls the shell in this way, to read a message's entries) and
a wait for the disk under the lock at every write. It would also be wrong on
any branch:

- one step changes several saved values together, so per-field writes leave
  crash states no server was ever in (Q7);
- the real rule is about order: a step's changes must be on disk before any
  message that depends on them leaves; substituted code, which writes where
  a field changes and knows nothing of messages, can only obey it by
  waiting for the disk at every write, under the lock, one flush per field
  and per entry;
- reads must stay in memory (the disk is read only at restart), and
  recovery must be written anyway.

mako-dev's old C++ Raft was close to mechanical and shows each problem
(§2.3). The manager has three jobs: record each step's changes as one
record; order and flush the records before any dependent message leaves;
recover at restart. About 1,100-1,750 lines of code and 3.5-6.5 agent-days
(§2.4).

**Q5. "What does glr mean?"** Ghost-log refinement: the verification method
our proof uses, and its repository
(github.com/ZhangZihao270/ghost-log-refinement;
`glr:docs/ghost-log/README.md:1-8`). The method leaves the code as it is
and adds proof-only bookkeeping, the ghost log, a diary that records each
change as it happens (`:24-34`). A proof then shows that every completed
stretch of the diary is one legal move of a hand-written Raft model
(`:36-42`), and combines the servers' diaries, plus "every message received
was sent", into safety for the whole cluster (`:43-67`). "glr" in this
document means the method, its repository, or the group that develops it.
Its phase A11 verified the node side of crash-restart for its port of
raft-rs: the persistence certificates and the restart constructor. The
host's part there, persisting in order and restarting from the persisted
certificate, stays trusted (§4.1).

**Q6. "What is memtable flush?"** RocksDB keeps each write in two places: in
its own write-ahead log file, the safety copy, and in an in-memory sorted
table, the memtable. A memtable flush writes that table out as a new sorted
file on disk (an SST file). For writes not already synced, it is what makes
them durable, at the cost of a whole file per call. The usual way to make a
write durable is to fsync RocksDB's log, which `sync = 1` does on every
write. The old store set `sync = 1`, so its writes were durable when they
returned, and its `sync()`, which Raft called after most writes, was a
memtable flush that made nothing durable that was not already: a redundant,
costly operation, not a durability hole. What the old code lacked was
atomicity across fields, the right order (it wrote the commit index before
the entries), some writes altogether (truncations, two term sites), and, in
async mode, a write before its reply (§2.3, §3.7).

**Q7. "Why does the restart server must see something between steps of the
core be a condition you want to emphasize?"** Because it is the simplest
rule that makes a restart both safe for Raft and covered by the proof, and
it is easy to break without noticing. Strictly, the proof asks for
something a little weaker (§1.3); resuming between whole steps is the form
of it that the shell can see and enforce.

- **Many steps change several saved values at once.** A vote at a higher
  term changes the term and the vote; one AppendEntries can cut the log,
  append entries and raise the commit index. If the disk can be caught
  halfway through a step, a restarted server can come back in a state no
  server is ever in: a new term with the old vote, a commit index over
  entries that were never written.
- **Some of those states break Raft:** a second vote in one term, or
  entries applied as committed that the cluster never committed. The others
  are harmless only by a separate argument for each state (§1.2, §1.3).
- **Most of them are not covered by the proof.** The proof describes a step
  as a series of whole model moves and covers a restart from the state after
  any of them, provided nothing already sent depends on a later one. A
  state halfway through a move, such as a new term with the old vote, comes
  after no move at all (§1.4). A few points inside a step do end a move (the
  state after the step-down that precedes a vote, for example) and would
  do, but they buy nothing: the shell sees only whole steps, and a step's
  messages wait for all of it anyway (§1.3).
- **The rule:** save each step's changes as one all-or-nothing record,
  write records in step order, and let nothing a step produced leave the
  server before its record is on disk. Every crash then leaves the state
  after some whole step, which always ends a move, and nothing the server
  said before the crash is forgotten.
- **It is easy to break:** the old C++ broke it four ways (§2.3), and a
  check at load cannot catch the worst case (scenario D, §1.2).
- **It costs nothing extra here:** it is the standard write-ahead-log rule,
  and the core already runs each step whole and reports its effects in one
  output (§1.5).

**The design in short** (§3).

- One log file per server, in `/tmp`, holds the term, the vote, the commit
  index and every entry (base 1).
- Each core step reports what it changed and whether it sent anything; the
  shell queues that as one record, under `mtx_`, in step order.
- Before anything leaves the server, the shell releases `mtx_`, writes the
  queue and flushes it on the sending thread. The leader flushes once per
  round (group commit), a follower once per AppendEntries.
- The apply thread applies only entries that are on disk.
- A restart reads the file, checks it, runs `Restore` between `Configure`
  and `EnterGates` with RPCs closed, and hands the committed entries to the
  apply thread to rebuild Masstree.
- Any write or flush error stops the process. Snapshots stay off.

**Effort (estimates).**

| | Days |
|---|---|
| Without Verus: Raft side (§5, P0-P3, P5, P6, P8) | 7.75-14.25 |
| Without Verus: Mako's re-apply hazards (P7, uncertain) | 2-5 |
| Added by Verus, the core's persist record trusted (assumed form) | 2-3.5 |
| Added by Verus, the core's persist record proved (check form) | 4-7.5 |
| Optional: `log_grows` on `step` | 0.5-1.5 |

Against the plain total with P7 (midpoint 14.5 days), Verus adds about a
fifth with the record trusted (midpoint 2.75 days) and about two fifths with
it proved (5.75 days). In both forms the shell's half of the durability
rule, writing and flushing before it sends, stays trusted (§4.5).

## 1. Why a restart resumes between two whole steps (Q7)

**The condition.** After a crash, the restarted server starts from the
state its disk held after some complete step k, never from a state partway
through a step, and nothing it sent before the crash may depend on a step
after k. This is the simplest rule that suffices, not the weakest: the exact
condition (§1.3) also admits a few points inside a step, and §1.4 gives the
proof's reason for it. Two rules of the design (§3.3) give it:

- **R2:** each step's changes form one record, written all or nothing, and
  records are written in step order;
- **R1:** a step's record, and every earlier one, is on disk before anything
  that step produced leaves the server.

Today nobody sees a state partway through a step: a step runs to completion
under `mtx_`, so every other thread sees only its start and its end
(`docs/verus/host-contract.md:51-58`). A crash adds a new observer, the next
incarnation of the process, which sees whatever the disk holds. The record
does for the disk what `mtx_` does for the other threads.

```
               step k+1: accept AppendEntries(prev 5; 6', 7', 8'; leader commit 8)
  step k |---------------------------------------------------------------------| step k+2
         term:=3  vote:=none  cut log at 6  append 6' 7' 8'  commit:=8  reply
         ^                                                                     ^
         the design restarts here                                 or here; not in between
```

### 1.1 Steps that change more than one saved value

With snapshots off, every write of the term, the vote, the commit index or
the log happens inside `step` (`core/src/event.rs:371`), except the
InstallSnapshot term write, which the design removes by refusing first
(§3.3; Appendix A lists the sites). These steps write more than one of
them:

| Step | Saved values written, in code order | What it sends |
|---|---|---|
| a RequestVote at a higher term (`do_vote`, `core/src/node.rs:587-696`) | term (`:663`), vote cleared (`:665`); on a grant, vote := the candidate (`:685`) | the reply, at the new term |
| an accepted AppendEntries (`raft_on_append_entries`, `:1749-2207`) | at a higher term, term (`:1901`) and vote cleared (`:1902`); then the log cut at the first conflict (`:2094`), the entries appended one by one (`:2127`), the commit raised (`:2147`) | the success reply (`:2154-2159`) |
| an AppendEntries refused after a higher term | term (`:1901`), vote cleared (`:1902`) | the refusal, at the new term (`:1928-1933`) |
| a reply with a higher term (`heartbeat_apply_append_reply`) | term, vote cleared (`core/src/heartbeat.rs:1359-1360`) | nothing |
| a campaign settled by a higher reply term (`election_settle`) | term, vote cleared (`core/src/node.rs:889-890`) | nothing |
| a campaign start (`start_election`) | term + 1 (`:759`), vote := self (`:761`) | the RequestVote broadcast, decided at `src/server_h.rs:3173-3185`, sent at `:3220-3226` |

One AppendEntries carries up to 256 entries or 16 MiB by default
(`server.cc:226-240`). Three single-value steps matter as well, because what
they send depends on the change: a heartbeat tick raises the leader's
commit index (`core/src/heartbeat.rs:122`, via `:358`) and every send of
that tick carries it (`:917`); a round end raises it (`:1689`) and the
follow-up round carries it (`src/server_cc.rs:324-329`); a proposal appends
(`core/src/node.rs:510`) and a later tick sends the entry. Several steps can
also share one critical section: a won election appends the leader's no-op
in the settlement's section (`src/server_h.rs:1543-1544` -> `:3498-3506`).

### 1.2 What a split write can leave on disk, and what breaks

"Split" means the values one step changes reach the disk separately. What a
crash can then leave depends on how the separate writes are made:

- **(a) writes that reach the disk in code order:** each flushed on its own
  under `mtx_`; or appended in code order to one log, or written to a store
  that keeps one thread's writes in order, and flushed later. A crash leaves
  the step's writes up to some point in code order. (This is not §3.4's
  flush-every-change, which flushes each step's whole record as soon as the
  step changes anything.)
- **(b) writes that can reach the disk out of code order:** made from another
  thread, or outside `mtx_` with nothing to keep them in order, or batched
  out of order, or sent to a store that does not keep the order of writes.
  A crash can leave any subset of them.

| Step | Crash state on disk | Possible under | Raft | The proof |
|---|---|---|---|---|
| any step-down | new term, old vote | (a), (b) | safe (inference); but the server refuses every other candidate in that term | not covered |
| any step-down | old term, vote cleared | (b) | Election Safety broken (scenario A) | not covered |
| vote at a higher term | new term, no vote, grant missing | (a), (b) | safe: the reply has not left | covered: the state after the step-down |
| campaign | new term and old vote, or old term and self-vote | (a); (b) | safe while the broadcast waits for the record (inference); Election Safety broken if it does not (scenario B) | not covered |
| accepted append | log cut, nothing appended | (a), (b) | safe (inference) | not covered |
| accepted append | some or all new entries, old commit | (a), (b) | safe: Raft's commit index may be recomputed | not covered, unless the step raised no commit |
| accepted append | the first i new entries, with the commit they justify | one record per entry | safe | covered (§1.3) |
| accepted append | new entries written, old tail not deleted | (b), or a delete never written | Log Matching broken (scenario C) | not covered |
| accepted append | commit raised, entries missing or stale | (b), or the wrong order | State Machine Safety broken (scenario D) | not covered |
| leader tick | commit not on disk, sends already out | early-send | safe (inference): the entries are on a majority | not covered: the sends carry a commit the restarted leader never had |

Every state (a) can leave is safe for Raft (inference), but only because code
order happens to be a safe order at every site, which the old C++ did not
keep (§2.3), and (a) still leaves states the proof does not cover. (a) need
not cost a flush per value: per-field records appended in code order to one
checksummed log and flushed at the four barriers of §3.3 cost one flush per
barrier, as this design does, and recovery up to the first bad checksum
still leaves a prefix in code order. Such a design loses the proof's
coverage, and its Raft safety rests on code order staying safe at every
write site as the code changes. Only designs that let writes reach the disk
out of order are (b), and under (b) Raft safety breaks.

The scenarios use three servers, X, Y and Z; scenario A needs five.

**A. A second vote in one term** (five servers). Y voted for X in term 5,
and X won. A RequestVote from Z, now campaigning in term 6, makes Y adopt
term 6, which clears its vote (`core/src/node.rs:663-665`). Suppose the
cleared vote reaches the disk and the new term does not, and Y crashes. It
restarts in term 5 with no vote, so it grants any up-to-date term-5
candidate that asks (`:1583-1586`). If a fourth server is still campaigning
in term 5 with two votes (a candidate waits up to 1 s for votes,
`rt/src/seam.rs:276-284`), Y's second vote gives it a majority of five
while X also holds one: two leaders in one term (inference). Both can then
write different entries with the same index and term, and Log Matching,
which identifies an entry by its index and term, no longer protects the
logs.

**B. A broadcast before the campaign's record is on disk.** This breaks R1:
the broadcast leaves before the campaign's record is durable.
1. X, in term 5, campaigns: term 6, a vote for itself, the broadcast. Y
   grants, and X leads term 6 with {X, Y}.
2. X crashes before the record reaches its disk, and restarts in term 5.
3. Z campaigns in term 6 with a log as up to date as X's and asks X. X adopts
   term 6, which clears its vote (`core/src/node.rs:663-665`), and grants. Z
   also leads term 6, with {X, Z}.

Here the whole record was lost, so the harm comes from the early broadcast,
not from a split. A split does the same damage: if X's disk got "term 6"
but not "voted for X" (an R2 break as well), X restarts in term 6 with its
term-5 vote, and still grants Z if that vote was none or Z; any other vote
makes it refuse (`:1523-1526`). With R1 the broadcast waits for the record,
so no peer hears of a campaign that a crash could undo, and the split is
then harmless (the campaign row of the table).

**C. The old tail not deleted.**
1. A follower holds entries 1-10. Entries 6-10 are from term 2 and were
   never committed: their leader crashed after reaching only this follower.
2. The term-3 leader sends prev 5 with entries 6' and 7' of term 3. The step
   cuts 6-10 and appends 6' and 7'.
3. The new entries reach the disk; the deletion of 8-10 does not.
4. The disk now holds 1-5, then 6' and 7' (term 3), then 8, 9 and 10 (term
   2): terms that go down along the log, which no Raft log ever has.

`Restore`'s check that entry terms never decrease (§3.6) refuses this store,
so the replica cannot rejoin (§3.7, Failure). Without that check the
follower would claim a log through index 10 that no leader holds. The old
C++ left exactly this on disk, crash or not, whenever a conflict's
replacing entries ended below the old last index, until later appends
overwrote the stale slots (§2.3).

**D. The commit written before the entries.**
1. The same follower; this time the term-3 leader sends prev 5 with 6', 7'
   and 8', and leader commit 8.
2. The step cuts 6-10, appends 6'-8' and raises the commit to 8
   (`core/src/node.rs:2143-2147`).
3. The commit reaches the disk first, and the process dies before the
   entries do. The disk holds commit 8 over the old entries 6-10 of term 2.
4. Every check of §3.6 passes: no gap, terms that never decrease and are at
   most the stored term, and commit 8 at most the last index, 10.
5. The restart hands (0, 8] to the apply thread (§3.6), which applies the
   old 6, 7 and 8 as committed. The cluster committed 6', 7' and 8': State
   Machine Safety is gone.
6. The leader can never repair this follower: rewriting 6-8 now touches
   entries the follower considers committed, and it refuses
   (`core/src/node.rs:2053-2082`).

No check at load can catch D; only the write order prevents it. The old C++
used this order (§2.3). A variant with a shorter log (commit 8 over a log
ending at 5) breaks `inv` (`core/src/node.rs:149`), so `Restore` refuses it
and the replica is out for good.

### 1.3 Smaller records: when they would be harmless, and the general condition

A split crash state is harmless when some legal Raft run, consistent with
everything the server has already sent, ends in that state: as if the step
had not started, had been a smaller step, or a message had been lost. Four
rows of §1.2 are like that, each by its own argument (inference in each
case):

- **A shorter log.** A conflict with an elected leader shows the cut
  entries were never committed (Leader Completeness), and the leader will
  resend them. The removed C++ relied on this argument in a comment
  (`d288748f5^:src/deptran/raft/server.cc:804-807`).
- **A new term with the old vote.** The reply had not left, so the server
  cast no other vote in the new term; the leftover vote is the only one it
  holds there.
- **A missing commit raise.** Raft's commit index can be relearned;
  raft.tla resets it to 0 on restart
  (`glr:docs/ghost-log/raftrs/roadmap.md:521-522`).
- **An unheard campaign.** A higher term no peer has seen promises nothing.

"Harmless" is not enough, for two reasons. Each argument must be redone for
every step and every new field. And none of these states is covered by the
proof, which restarts a server only from the replay of a closed prefix of
its own diary (glr B46; §1.4, fact 3). No prefix holds a log cut without
the append: the spec has no action that cuts a log without appending (a
conflict replaces the tail and appends in one action,
`glr@d7e04ed7:src/protocol/Raft/raft.rs:735-754`). No prefix holds a new
term with the old vote: in this core's diary the term rises only in a
step-down segment, which also clears the vote
(`core/src/coupling.rs:614-621`), or in a campaign segment, which also sets
the self-vote (`:885-894`). The spec can reach "new term, old vote" from
another history (`LGrantVote` raises the term and keeps a vote for the same
candidate, `glr@d7e04ed7:src/protocol/Raft/raft.rs:104-131`), but not this
server's. And the spec's campaign sets the term and the self-vote together
(`:72-100`).

**The general condition.** A record smaller than one step would be
acceptable when both of these hold:

1. every state the disk can be left in is one the server's own history
   passes through between two whole Raft actions; in proof terms, the replay
   of a prefix of whole segments (§1.4);
2. nothing already sent depends on the part not yet on disk.

Inside this core's steps the points that satisfy condition 1 are exactly
the ends of segments: after the step-down that comes before a vote or an
append (`core/src/coupling.rs:714-717`, `:1865-1868`); after each entry of
an accepted batch, since the coupling records each entry as its own
follower-append action with the commit that entry justifies
(`:1705-1718`); and after the leader's commit advance, before its sends
(`:2315-2319`; `core/src/heartbeat.rs:788-798`). For scenario D's append,
with old commit 5, the diary reads:

```
Recv(L, AE{prev 5, 6'})   Set(log, 1-5 6')        Set(commit, 6)  Send(L, ok 6)  Close
Recv(L, AE{prev 6, 7'})   Set(log, 1-5 6' 7')     Set(commit, 7)  Send(L, ok 7)  Close
Recv(L, AE{prev 7, 8'})   Set(log, 1-5 6' 7' 8')  Set(commit, 8)  Send(L, ok 8)  Close
```

So "1-5 and 6', with commit 6" is a legal restart point; "1-5, nothing
appended" and "old 6-10 with commit 8" are not. Only the last answer goes on
the wire (`docs/verus/host-contract.md:77`); the first two count as lost
messages (`:62-63`).

These finer points buy nothing, so the design keeps the simplest rule that
suffices, whole steps. A step's emission waits for its whole record anyway,
so smaller records only add crash states, and the shell sees whole steps,
not segments. Larger records are fine: group commit puts several steps in
one write and one flush, and any prefix still ends at a step boundary.
Every step boundary qualifies, not only critical-section boundaries.

### 1.4 The proof-level reason

**The ghost log.** Beside the core's state the proof keeps a diary that
exists only when Verus checks the code (`core/src/coupling.rs:1-17`;
`core/src/node.rs:105-116`). It has five kinds of entry
(`glr@d7e04ed7:src/protocol/Raft/ghost_log.rs:178-195`):

- `Recv(from, message)`: a message arrived and starts an action;
- `Tick`: a timer or a local decision starts an action;
- `Set(field, value)`: a field the model tracks got a new value;
- `Send(to, message)`: the action sends a message;
- `Close(label)`: the entries since the last `Close` claim to be one atomic
  action of the model (`:194`).

The stretch that ends at a `Close` is a **segment**. In scenario A, if Y
grants Z's term-6 request, Y's step is recorded like this
(`core/src/coupling.rs:614-621`, `:714-726`):

```
Recv(Z, RequestVote{term 6})
  Set(term, 6) Set(role, Follower) Set(has_voted, false) Set(voted_for, -) Set(votes, {})  Close(StepDown)
  Set(has_voted, true) Set(voted_for, Z)  Send(Z, VoteResponse{6, granted})                Close(GrantVote)
```

Four facts give the condition, and show that every step boundary meets it:

1. **Only closed segments must be legal moves.** The open segment is
   unconstrained (`glr@d7e04ed7:src/protocol/Raft/ghost_log.rs:414-420`);
   `no_open_writes`, "the open segment holds no writes yet", is
   "deliberately broken mid-action" and restored by `Close` (`:422-429`).
2. **At every step boundary the diary is fully closed and its replay equals
   the core's state** (`ginv`, `core/src/coupling.rs:305-315`; kept by
   `step`, `core/src/event.rs:373-379`; "at every step boundary",
   `core/src/coupling.rs:14-17`), so a step boundary is always a legal
   restart point. Inside a step this holds only at some segment ends: the
   leader's commit advance re-establishes it before the tick's sends
   (`core/src/heartbeat.rs:87-94`, `:122-129`, called at `:358`), while most
   handlers change the fields first and append their segments at the end
   (an accepted append at `core/src/node.rs:2161-2205`, a campaign at
   `:783-790`). What matters for a restart is not when the code writes the
   diary but which states the finished diary passes through at its
   `Close`s.
3. **A restart continues the diary.** In glr's A11 the restarted server's
   diary is `prev` followed by one step-aside segment
   (`glr:src/ports/raftrs/raw_node.rs:860-867`, `:877-891`). `prev` must be
   fully closed and replay to exactly what the store holds: term, vote,
   commit and log (`restart_cert_ok`, `:785-808`; the closure at `:794`,
   the store at `:798-802`). Our `Restore` is the same construction (O1,
   §4.3). Most states partway through a step are the replay of no closed
   prefix of the server's own diary. In scenario A, "term 6 with the old
   vote X" is not: every closed prefix with term 6 has the vote cleared
   (after the step-down) or set to Z (after the grant), and a prefix may
   end only at a `Close`. The exceptions are the segment ends inside a step:
   after a step-down, after each entry of a batch with the commit that
   entry justifies, after a leader's commit advance (§1.3). They are legal
   restart points, which the design does not use. A `prev` that is not the
   previous incarnation's real diary can still satisfy `ginv`, which checks
   each server's diary on its own: Y's real diary followed by an invented
   `Recv(X, RequestVote{6})` and a grant replays to "term 6, voted X". But
   it records a message no server sent, or lacks a `Send` the server
   emitted, so the spliced diaries break the cluster theorem's other
   premise, `causal` (`core/src/coupling.rs:2685`; glr B46,
   `glr:docs/ghost-log/raftrs/coupling.md:353`). That does not make the
   server wrong; it makes it unverified.
4. **Every received message must have its `Send` in a surviving diary.** The
   cluster theorem assumes causality: each segment opened by a `Recv` runs
   after the matching `Send` (`docs/verus/host-contract.md:36-63`;
   `core/src/coupling.rs:2666-2704`). A crash keeps a prefix of the diary,
   and a prefix keeps a `Send` only together with everything before it:
   every earlier segment and the `Set`s of its own segment. So a message may
   leave only once its step and all earlier ones are on disk: R1. glr made
   the same rule a contract for raft-rs (glr B45,
   `glr:docs/ghost-log/raftrs/coupling.md:391`), and its composition joins
   the incarnations' diaries into one without changing the theorem
   (`glr:docs/ghost-log/raftrs/composition.md:150-174`).

Fact 4 has three consequences here:

- **A follower's commit raise.** The follower-append segment sets the log
  and the commit and sends the reply in one segment
  (`core/src/coupling.rs:1691-1698`;
  `glr@d7e04ed7:src/protocol/Raft/raft.rs:489-520`, commit at `:508`). So
  even a heartbeat that only raises the commit must put the raise on disk
  before replying. Raft itself does not need this; the proof does (§4.2).
- **A leader's commit advance.** It is a segment of its own, followed by one
  segment per send (`core/src/coupling.rs:2315-2319`, `:2503-2515`). A send
  carries a commit no higher than the leader's (`glr@d7e04ed7:.../raft.rs:363`),
  and an append that carries an entry carries exactly the leader's
  (`:368`), so the sends cannot survive without the advance.
- **A campaign.** The term, the self-vote and the broadcast are one segment
  (`core/src/coupling.rs:885-894`).

The model already assumes this crash model: each action is atomic, its
writes are durable "the moment they are written", and a restart happens
only between actions (`glr:docs/ghost-log/raftrs/roadmap.md:514-522`). The
two rules make the implementation match it. They are trusted, not checked
at load: `Restore`'s checks cannot tell Y's "term 6, voted X" from a legal
state, and scenario D passes all of them. So the condition is a premise of
O1 (§4.3), and that is why it is a rule of the design rather than a check.

### 1.5 Why this design gets it for free

This is the standard write-ahead-log rule: one atomic, checksummed record
per unit of work, written in order; a torn record dropped whole; a unit's
record durable before any of its effects become visible. raft-rs's own
contract is the same: persist, then send
(`glr:docs/ghost-log/raftrs/coupling.md:552-553`). Here every piece exists
or is planned:

- **The unit exists:** a step runs to completion under `mtx_`
  (`core/src/event.rs:370`; `docs/verus/host-contract.md:51-58`).
- **The step already reports its effects as data,** in one `CoreOutput`
  (`core/src/output.rs:13-20`, `:124-133`); the record is one more field of
  it (§3.2).
- **Order and timing:** the queue under `mtx_` keeps step order, and the
  write and the flush happen before the emission (R1, R2, §3.3; the store
  lock is taken before `mtx_` is released, §3.4).
- **Atomicity comes from the framing:** each record carries its length and a
  CRC32C (§3.7). Recovery reads up to the first bad checksum and drops the
  torn tail; skipping a bad record in the middle would apply later steps to
  a state missing an earlier one. So the disk always holds "steps 1 to k,
  complete" for some k.
- **The cost is one write and one flush per barrier** (group commit).
  Per-field writes cost one flush per value under `mtx_`, or need the same
  queue and barriers, and either way still leave the split states of §1.2.

## 2. A small storage manager, not a mechanical substitution (Q4)

"Mechanical substitution" means: after every change to a saved field in
memory, write that field to disk; read literally, also read fields from
disk. A **storage manager** here means the code that owns the disk copy: it
decides the bytes each step writes, writes them in order, makes them durable
before any message that depends on them leaves, and rebuilds memory from
them at restart. It need not be a class; in raft-rs it is the exchange
between the core and its host around each Ready (§4.1).

### 2.1 Why substitution does not work

1. **It is excluded on this branch, by design.** The saved fields are
   written inside the verified core (`core/src/node.rs:663-685`,
   `:759-761`, `:889-890`, `:1901-1902`, `:2094-2147`;
   `core/src/heartbeat.rs:122`, `:1359-1360`), and the core does no I/O:
   it is "plain Rust in the Verus subset -- no unsafe, FFI, locks, atomics,
   hashing, logging or clock reads" (`core/src/lib.rs:1-5`), it depends
   only on Verus's crates (`core/Cargo.toml:14-21`), and it "takes no lock,
   does no I/O, reads no clock and cannot wait"
   (`docs/verus/code-structure.md:32-37`). The shell sees whole steps
   through one wrapper (`src/server_h.rs:1407-1443`), never single field
   writes. A write at `core/src/node.rs:663` would need a
   trusted store call inside `step`. The core could make one: it already
   calls shell code during a step, through a trait with a trusted contract,
   to read a message's entries (`InboundBatch`, `core/src/node.rs:1704-1739`,
   "The host contract (unverified: the shell implements this)" at `:1718`;
   the shell's `WireBatch` implements it over C++ kernels,
   `src/server_h.rs:3307`, `:3353-3365`). With R4 (an I/O error aborts,
   §3.3) such a call returns nothing the core branches on, so the core stays
   a function of its events (`docs/verus/code-structure.md:63-65`). But the
   call breaks the core's design rule, puts a wait for the disk under `mtx_`
   at every write (item 3), and the items below show it would be wrong
   anyway (inference).
2. **It is the wrong unit for crashes.** One step changes several values
   together (§1.1), so per-field writes leave crash states no server was
   ever in (§1.2). Example: a server at term 4 that voted for X grants C at
   term 5 in one step (`core/src/node.rs:663`, `:665`, `:685`). Written
   field by field, the disk passes through "term 5, voted X" (term first) or
   "term 4, voted C" (vote first). Neither is a state the server was ever
   in, and whether each is harmless has to be argued site by site (§1.3);
   the old C++ happened to write the term first (§2.3). An
   AppendEntries that truncates and appends (`:2094`, `:2127`) passes
   through a shorter log, or, if slots are overwritten in place, through a
   log whose terms go down (scenario C).
3. **It is the wrong unit for order and cost.** The rule is about order: a
   step's changes, and every earlier step's, are durable before anything
   the step produced leaves the server (R1). Per-write code can obey that
   only by waiting for the disk at every write, and the old C++'s sync mode
   mostly did. What it cannot do is wait once, just before the message
   leaves: that needs code that knows where messages leave, which on this
   branch is always a critical-section boundary (`src/server_h.rs:4181-4186`,
   `:4264-4272`, `:3173-3185` -> `:3220-3226`; `src/server_cc.rs:87-125` ->
   `:139-178`). Waiting at every write costs:
   - **on the leader,** `Start` holds `mtx_` on the submit thread
     (`src/server_h.rs:3886-3887`; the thread is created at
     `raft_worker.cc:743-753`) while `append_local` writes the entry
     (`core/src/node.rs:510`). A flush there holds `mtx_` for F per
     proposal, and meanwhile the poll thread, which runs every handler and
     both fibers, blocks in `lock()` (`docs/verus/code-structure.md:494-496`).
     Throughput is capped at 1/F: 5,000 proposals/s at F = 0.2 ms, 87% below
     G2's 37,760/s; 1,000/s at 1 ms; 200/s at 5 ms;
   - **on a follower,** one flush per entry plus up to four for the other
     values, on the poll thread: a full batch of 256 entries at F = 0.2 ms
     takes about 51 ms, against a 5 ms heartbeat (`server.h:169-173`) and a
     reply deadline of min(heartbeat, 100 ms) (`src/server_cc.rs:201-210`)
     (arithmetic);
   - **with a manager,** one flush per AppendEntries on a follower and one
     per round on the leader (group commit, §3.4).

   Moving the wait out of the lock, as the old C++ follower did (§2.3), is
   no longer mechanical: it needs an ordering guarantee so that a newer
   step's write cannot land before an older one's (§3.4).
4. **Reads must stay in memory.** The core reads the saved fields on every
   path: every reply copies the term (`core/src/node.rs:1805`, `:1836`,
   `:1932`, `:2066`, `:2155`), log matching reads the log (`:1874-1886`),
   the vote check compares logs (`:1554`), the leader's payloads clone
   command handles out of the log (`core/src/heartbeat.rs:580`, `:696`,
   `:707`), and the apply hand-off reads committed entries
   (`src/server_h.rs:3075-3144`). A log entry holds `cmd_: C`
   (`core/src/log.rs:37-48`), which in production is a 24-byte opaque
   carrier of a C++ object (`src/rusty-rustc/src/lib.rs:390-393`): its bytes
   are pointers and mean nothing after a restart. Writing an entry needs
   `raft_command_encode` (`server.cc:1414`); reading it back needs
   `raft_command_from_bytes` (`server.cc:438`) and `raft_entry_from_command`
   (`src/server_h.rs:3446-3459`). So the disk is a write-ahead copy, read
   only at restart. raft-rs works the same way: its `Storage` trait gives
   the core only reads (`glr:src/ports/raftrs/storage.rs:123`), and the host
   sends messages "AFTER the HardState, Entries and Snapshot are persisted"
   (`glr:src/ports/raftrs/raw_node.rs:288-289`).
5. **Recovery must be written anyway.** None of this comes from substituting
   writes: open the store and check its header against the site, partition
   and configuration; read records up to the first bad checksum; decode the
   commands; check what the core's invariants need (§4.3); install the state
   through one new event, `Restore`, since writing core fields directly
   would add a `T` line, which ends a replay
   (`docs/verus/code-structure.md:893-907`); hand (0, commit] to the apply
   thread and hold the campaign until it catches up (§3.6).
6. **Overwriting in place cannot be atomic.** Entries vary from 4 KB to
   1 MiB (§3.9). A file laid out like memory cannot rewrite entry i with an
   entry of another size without moving its neighbours, and an overwrite
   cut short by a crash leaves a slot that is neither the old entry nor the
   new one. An append-only record log with a length and a checksum per
   record never overwrites; a torn last record fails its checksum and is cut
   at recovery (§3.7).
7. **Putting the state itself on disk fails too.** Mapping the structures
   into a file, so that every memory write is a disk write, does not help.
   A mapped file holds whatever memory held when the process died: possibly
   a state partway through a step (the split states of §1.2), plus C++
   handles that mean nothing after a restart (item 4). `msync` at each
   write is a flush at each write. And the log's vectors live on the heap,
   outside the file, unless a custom allocator puts them there, which
   `#![forbid(unsafe_code)]` rules out (`core/src/lib.rs:5`) (inference).

### 2.2 The closest to mechanical: Option S

Option S (§3.2) has the step wrapper compare the state before and after
each call, so it changes no core code. It is a storage manager too, only one
that guesses what changed: the rule then rests on shell code alone (§4.5),
and finding the lowest changed log index would need a scan of the log's
uncommitted tail, since the core does not export it (inference).

### 2.3 The worked example: mako-dev's C++ Raft

mako-dev's old C++ Raft is a near-mechanical substitution: one `Persist*`
call beside each change in memory (`origin/mako-dev:src/deptran/raft/server.cc:166-269`).
It runs only with `MAKO_RAFT_PERSISTENCE=1` (`:849-852`), off by default;
the variable appears only in documents, `macros.h`, `server.cc`, `test.cc`,
`testconf.cc` and `PERSISTENCE_TEST_STATUS.txt`, so no CI script sets it.
Its building blocks:

- `PersistTermAndVoteToLogStorage` writes the term and the vote as two
  separate puts, then calls `sync()` (`:171-182`);
- `PersistLogEntryToLogStorage` writes one entry, then calls `sync()`
  (`:229-245`);
- `PersistCommitIndexToLogStorage` writes three puts and no `sync()`
  (`:198-211`);
- the batch writer `PersistLogEntriesToLogStorage` (`:248`) and
  `PersistVoteToLogStorage` (`:185`) are never called.

Below, `server.cc` and `server.h` are mako-dev's (`origin/mako-dev:src/deptran/raft/`).

| Change | Memory write | Persisted | Lock | Relative to the message that depends on it |
|---|---|---|---|---|
| campaign: term + 1, vote for self | `server.cc:1897-1898` | `PersistState`, `:1901` | `mtx_` | before the broadcast (`:1924`): correct |
| RequestVote at a higher term | `server.h:361-364` | `PersistState`, `:369` | `mtx_` | before the reply: correct |
| vote grant, sync mode | `server.h:376` | `PersistState`, `:429` | `mtx_` | before the reply: correct |
| vote grant, async mode | `server.h:376` | on a new thread (`:384-425`) | none | after the reply |
| AppendEntries at a higher term | `server.cc:2359-2360` | `PersistState`, `:2363` | `mtx_` | before the reply: correct |
| follower commit | `server.cc:2423` | `PersistCommitIndex`, `:2425` | `mtx_` | before the entries it covers: wrong order |
| follower entries, sync mode | `:2393`, `:2408` | one put and flush per entry (`:2494-2496`), then the commit again (`:2497`), after unlocking (`:2444`) | none | before the handler returns, so before the reply: correct |
| follower entries, async mode | the same | on a new thread (`:2449-2488`) | none | after the reply |
| follower truncation | only the tail index moves back (`:2393`, `:2408`) | never | -- | the old tail stays on disk |
| leader append | `server.h:629-635` | `PersistLogEntry`, `:640` | `mtx_` (`:624`) | before any send: correct |
| leader commit | `server.cc:1363`, `:1727` | `PersistCommitIndex`, `:1364`, `:1728` | `mtx_` | before the next round |
| higher term in a reply | `server.cc:1613` (AppendEntries), `:1440` (InstallSnapshot) | never: `stepDown` persists nothing (`:3252-3277`) | `mtx_` | later replies carry a term that is not on disk |

**What it got right.** Every RocksDB write used `sync = 1`
(`origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:146-147`, used by
`put` `:251`, `remove` `:280`, `put_batch` `:357`, `remove_range` `:408`
and `set_metadata` `:556`), so each single write was durable when it
returned (`rocksdb/options.h:1974-1991`). In sync mode the campaign, votes
and entries were persisted before their messages left; the leader persisted
an entry before sending it; and the term was written before the vote, the
order whose split states are harmless (§1.2).

**What it got wrong.**

1. **No atomicity across fields.** Term and vote are two durable puts, the
   commit three, each entry one. In sync mode every term and vote write is
   made under `mtx_`, the term's put before the vote's, each synced
   (`server.h:369`, `:429`; `server.cc:1901`, `:2363`;
   `origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:147`, `:556`),
   so a crash leaves only the term-first states of §1.2; scenario A's "old
   term, vote cleared", and the campaign's "old term and self-vote", need
   async mode's unlocked re-reads (item 8). Scenarios C and D are reachable
   in both modes: the cut is never written (item 3), and the commit is
   written before the entries (item 2). The row "log cut, nothing appended"
   never is, since no cut is written.
2. **The wrong order on the follower.** The commit becomes durable under the
   lock (`server.cc:2425`) before the entries it covers (`:2494`): scenario
   D. With a shorter log the stored commit lies past the last stored entry;
   recovery does not clamp it (`:296-299`; only the speculative indices are
   clamped, `:334-343`), and the startup hand-off (`:999-1001`) stops at the
   gap and only logs it (`:717-740`).
3. **Truncation is never written.** A conflict overwrites slots in place and
   moves only the tail index back (`:2393`, `:2408`); recovery takes the
   largest stored key as the last index (`:317`;
   `origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:456-495`). So a
   conflict whose replacing entries end below the old last index leaves the
   stale tail on disk, and a restart before later appends overwrite it,
   crash or not, resurrects that tail: scenario C (inference: read, not
   run). A heartbeat's success then reports the whole stale tail
   (`server.cc:2433`) and the leader takes it as the match index
   (`:1661-1666`), so it may count stale entries as replicated (inference).
   verus-raft's core blocks this second step: a follower acknowledges only
   the end the RPC proved (`core/src/node.rs:2156-2159`), and "a heartbeat
   cannot adopt an unknown follower suffix"
   (`core/src/heartbeat.rs:1404-1406`).
4. **Missed write sites.** Two term writes persist nothing (`server.cc:1440`,
   `:1613`). This is the typical failure of one call per change: the change
   sites are scattered.
5. **`sync()` was a memtable flush** (Q6), called after every term/vote
   write (`server.cc:180`) and every entry write (`:243`): costly, and the
   wrong operation (§3.7).
6. **Async mode replies before the write** (`server.h:384-425`;
   `server.cc:2449-2488`), which is outside what glr's contract allows (glr
   B45, `glr:docs/ghost-log/raftrs/coupling.md:391`).
7. **Failures are ignored.** Return values are discarded
   (`server.cc:178-180`, `:242-243`); a store that cannot be created leaves
   the server running without persistence (`:878-879`); a failed recovery
   only logs (`:895-897`); `verify_on_recovery` is declared and never read
   (`origin/mako-dev:src/deptran/raft/recovery_manager.hpp:48`); recovery
   checks nothing (`server.cc:272-353`).
8. **Writes read live fields without the lock.** `PersistState(term,
   voted_for)` ignores its arguments and writes the current fields
   (`server.h:499-505` -> `server.cc:171-182`); in async mode it runs on
   another thread, so a pair read before a newer one was written can land
   after it, rolling the disk back to an older term and allowing a second
   vote in the newer term, as in scenario A (inference; assessment §1.3
   describes the same hazard for the candidate).
9. **The lab could not see any of this.** `Kill` disconnects the server,
   waits 450 ms and deletes it (`origin/mako-dev:src/deptran/raft/testconf.cc:567-598`);
   it never stops a server between two writes of one step (inference).

Cost per operation in sync mode, counted from the code: a leader proposal
pays one WAL fdatasync and one memtable flush under `mtx_`; a follower
AppendEntries with k entries, a higher term and a commit raise pays k + 8
synced puts and k + 1 memtable flushes; a vote granted at a higher term
pays 4 synced puts and 2 memtable flushes.

The srpc-lineage C++ that `d288748f5` removed was more careful: one
write-and-sync helper with a sticky "persistence healthy" flag
(`d288748f5^:src/deptran/raft/server.h:1845-1868`), term and vote in one
batch (`d288748f5^:src/deptran/raft/server.cc:670-687`), and a follower's
truncation written as a range delete then the new entries, under one final
sync (`:840-863`). That is still two writes, and its comment accepts the
crash state between them by the shorter-log argument of §1.3 (`:804-807`).

### 2.4 The manager this design needs

**Three jobs.**

1. **Record.** Per step, record what changed, as one record: term, vote and
   commit when they changed, plus "replace the log from index i with these
   entries" (§3.2; R2).
2. **Order and flush.** Queue the records under `mtx_` in step order. At
   each of the four places where something leaves the server, after
   releasing `mtx_` and while holding the store lock, write the queue and
   flush it. Publish `durable_last` and `durable_commit`; bound the apply
   thread (R3); abort the process on any error (R4) (§3.3-§3.5).
3. **Recover.** Item 5 of §2.1 (§3.6, §3.7).

**What already exists on this branch.**

- One place where every saved value is written: `step`
  (`core/src/event.rs:371`), reached only through the shell's wrapper
  (`src/server_h.rs:1407-1443`).
- Effects returned as data in `CoreOutput` (`core/src/output.rs:13-20`,
  `:124-133`), the natural carrier for the record.
- Every emission at a critical-section boundary (§2.1 item 3).
- The codec kernels (`server.cc:438`, `:1414`) and the entry builder
  (`src/server_h.rs:3446-3459`).
- The apply hand-off (`src/server_h.rs:3075-3144`).
- A startup window with RPCs closed (`src/server_h.rs:1747-1748`).
- A precedent for one line per step: the replay recorder, written from the
  same wrapper (`src/server_h.rs:3384-3420`). It records inputs and command
  digests, not state, so it cannot rebuild the log.

**What is new, and its size (estimates).**

| Piece | Where | Code lines | Test lines | Agent-days |
|---|---|---|---|---|
| the record: a field of `CoreOutput`; three last-recorded copies; `log_from` at `core/src/node.rs:510` and `:2094`; `sync` on emitting steps | `core/src/{output,node,heartbeat,event}.rs` | 80-150 | the replay check | 0.5-1.5, with `Restore` (P2) |
| `Restore` and its checks; recorder and replayer support | `core/src/{event,node}.rs`, `replay/src/lib.rs` | 120-200 | 50-100 | (in P2) |
| order and flush: the queue, the store lock, four barriers, the watermarks, R3, R5, the abort, the knobs | `src/server_h.rs`, `src/server_cc.rs` | 250-400 | the lab cases (P5) | 1.5-2.5, with the restore path (P3) |
| the restore path: open, scan, decode, `step(Restore)`, the apply backlog, the campaign hold | `src/server_h.rs:1744-1869` | 120-200 | -- | (in P3) |
| the WAL crate: header; records with length and CRC32C; append, flush and injected delay; files; the recovery scan; the fault-injecting and timing-only backends | a new crate | 500-800 | 400-700 | 1.5-2.5 (P1) |
| **total** | | **about 1,100-1,750** | **about 450-800** | **3.5-6.5**, plus 1-2 for proofs O1 and O2 (and 2-4 for the check form) |

This is smaller than the assessment's Option C (assessment §2) because
snapshots stay off (no snapshot store), sync is strict (no asynchronous
watermarks), the core reports its own changes (no wrapper around every
mutator), and a Rust WAL needs no C++ kernels. For comparison, mako-dev's
approach was about 190 lines of `Persist*` and recovery functions
(`origin/mako-dev:src/deptran/raft/server.cc:166-353`) on top of a 716-line
RocksDB store, and §2.3 lists what those lines left out.

## 3. The design on the current core/shell split

### 3.1 What is durable

| State | Field | Durable | Why |
|---|---|---|---|
| term | `current_term_` (`core/src/node.rs:69`) | yes | every reply carries it; a forgotten term allows a second vote (`docs/verus/bugs-found.md:180-183`) |
| vote | `vote_for_` (`:55`) | yes | the same |
| log | `raft_log_` (`:49`), base 1 under the gate (`:172-176`) | yes, from index 1 | an acknowledged entry must survive |
| commit index | `commit_index_` (`:70`) | yes, in the same records | the proof needs it (§4.2); it also tells startup what to apply |
| applied index | `execute_index_` (`:71`), `appliedIndexForWait_` (`src/server_h.rs:951`) | no | Masstree is memory-only; restarts at 0 |
| snapshot boundary | `snapidx_`, `snapterm_` (`:73-74`) | no | snapshots stay off; disk mode is to refuse `MAKO_RAFT_SNAPSHOTS` (proposed, P3; the knob is read at `server.cc:798-802`) |
| role, peers, round state, timers, leader hint | the rest of `RaftCore` | no | volatile in Raft; the spec's step-aside drops the role and the votes but keeps the match and next tables, which the core holds only as ghost state (`g_match_`, `g_next_`, O1) and `LBecomeLeader` resets before use (`glr@d7e04ed7:src/protocol/Raft/raft.rs:200-201`, `:226-232`; `glr:docs/ghost-log/raftrs/roadmap.md:519-520`) |
| membership | `config_members_`, read from yaml (`src/server_h.rs:3531-3547`) | a fingerprint in the store's header | a store written under another configuration fails closed |

### 3.2 Decisions in the core, I/O in the shell

A core call already returns its effects as data: actions and log lines in
`CoreOutput` (`core/src/output.rs:25-39`, `:126-133`). Add one more output,
the step's **persist record**:

- `hard`: term, vote and commit, when any of them differs from the core's
  copy of what was last recorded (three new fields in `RaftCore`);
- `log_from`: the lowest log index the step changed; the shell copies the
  entries from there to the last index;
- `sync`: did this step send anything (a vote or append reply, the
  campaign's broadcast, a tick with sends)? If so, the shell flushes before
  the send. A one-server cluster never sends after its election: it has no
  peers (`core/src/heartbeat.rs:102`), so a tick's send loop never runs
  (`:884`). There `sync` is also set when a step advances the commit index;
  otherwise nothing would ever flush, and the apply thread would wait for
  ever (§3.5).

Only two places change the log in the verified configuration, and both know
the index: `append_local` (`core/src/node.rs:510`) and the AppendEntries
handler, which truncates at `first_write_index` and appends from there
(`:2094`, `:2127`). An entry already held is not rewritten: the scan stops
at the first conflict (`:2031-2036`), the cut starts there (`:2094`), and
only indexes from there on are appended (`:2125-2127`). Each site pushes
`log_from`, as handlers push actions now. Term, vote and commit need no
marks: comparing with the copies catches a write anywhere, the vote-only
grant at `:685` included. The alternative, Option S, has the step wrapper
(`src/server_h.rs:1407-1443`) diff the state around each call; it changes no
core code, but then the rule rests on shell code only (§2.2, §4.5).

### 3.3 The rule and the four barriers

- **R1.** A step's record, and every record before it, is durable before
  anything that step emits leaves the server.
- **R2.** All of a step's changes form one record (term, vote and commit,
  and any truncation together with the entries that replace it), written
  atomically; records are written in step order.
- **R3.** The apply thread applies only through the durable last index (§3.5).
- **R4.** A write or fdatasync error aborts the process; nothing is retried.
- **R5.** Checked at run time, aborting: no reply acknowledges beyond the
  durable last index; with two or more servers, neither does a leader's
  commit index (§3.5).

§1 explains why R1 and R2 are the condition that matters: together they
make every crash leave the state after some whole step.

| Emission | Decided in | Leaves at | Barrier |
|---|---|---|---|
| vote reply | `on_request_vote_body`, critical section `src/server_h.rs:4181-4186` | returned to `rt/src/service.rs:108-113`, queued by `rt/src/rpc.rs:476-491` | after `:4186`, before the return |
| AppendEntries reply, both RPC ids | `on_append_entries_body`, `:4264-4272` | `rt/src/service.rs:118-152` | after `:4272` |
| campaign broadcast | `RequestVoteImpl`, `:3173-3185` | `raft_broadcast_vote_and_wait`, `:3220-3226` | between them |
| AppendEntries sends | `heartbeat_tick_body`, `src/server_cc.rs:87-125` | `:139-178` | between them, when the tick has sends |
| InstallSnapshot | `OnInstallSnapshotLocked`, `src/server_h.rs:2226` | -- | none: with snapshots off, refuse before the term write at `:2306-2307` (move the storage check at `:2402-2412` up) |

Some answers need no flush. The "not ready" answers and fix F9's drops
(`src/server_h.rs:3928-3934`, `:3946-3952`, `:4232-4241`, `:4311-4322`)
carry only the asker's term or zeros, and `Start`'s APPENDED
(`:3884-3914`) promises nothing (assessment §4.4). A step that changes saved
state but sends nothing (a reply's step-down,
`core/src/heartbeat.rs:1359-1360`; a proposal; a round end's commit,
`:1689`) is queued and flushed by the next step that sends; with one server
a commit advance flushes at once (§3.2).

### 3.4 Threading

All four Raft RPCs are fast RPCs, run inline on the transport's poll thread
(`rt/src/rpc.rs:386-404`; `src/srpc/rpc/server.rs:1476-1479`), and the
heartbeat and election fibers run on the same thread: the owner-thread
startup job spawns them (`src/server_h.rs:1742`, `:1851-1867`), and a fiber
stays on the thread that created it (`rt/src/seam.rs:166-168`, `:187-211`).
srpc only queues a reply or request; the poll loop writes it later
(`src/srpc/rpc/server.rs:1246-1285`; `src/srpc/rpc/tcp_channel.rs:864-897`),
so "send, then flush" on one thread overlaps nothing. A follower has at most
one AppendEntries from its leader in flight (`core/src/heartbeat.rs:908-911`),
so it has nothing else to do while it waits for its disk.

| Shape | Who waits for F | Group commit | Covered by the proof | Verdict |
|---|---|---|---|---|
| flush-every-change: each step's whole record fsynced under `mtx_` as soon as the step changes anything, `Start` included (submit thread, `raft_worker.cc:743-753`) | every thread that wants `mtx_`, once per proposal | none: at most 1/F proposals per second | yes, as whole-step records; flushing each field instead (§1.2, shape (a)) would not be | reject |
| flush-under-lock: fsync under `mtx_` at the four barriers only | the poll thread, plus `Start` and `Applied` while it holds `mtx_` | per round (leader), per RPC (follower) | yes | correct, blocks two threads needlessly |
| **flush-after-unlock: queue records under `mtx_`; write and fdatasync after unlocking, on the emitting thread** | the poll thread only | the same | yes | **recommended** |
| disk-thread: a disk thread; deferred replies; sends after the completion | nothing | across rounds | yes | later, if measurements call for it |
| early-send (the assessment's S3): the leader sends before its own flush | nothing on the leader's commit path | across rounds | no (glr B45) | outside |

In words: under the lock each step appends its record to a queue. A thread
about to send takes the store lock and the queue before it lets go of
`mtx_`, then writes, flushes, and only then sends. Taking the store lock
while still inside `mtx_` keeps records in step order, so an old term and
vote can never land on disk after a newer one; this is the hazard of
assessment §1.3 and §8.12. Flush-after-unlock, at each barrier:

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

The store lock is a leaf, taken after `mtx_`. While the poll thread waits on
the disk, `Start` and `Applied` can still take `mtx_`; in production nothing
else emits, since every emitting step runs on the poll thread. Lab builds
also call `ServeAppendEntries` and `ServeVote` from the harness thread
(`src/lab.rs:508-536`; code-structure.md §7), so there a handler can wait
for the store lock under `mtx_` while the poll thread flushes: the order
still holds, but it is a wait under `mtx_`, in the builds §3.8 uses. The lab
either accepts that wait or routes its injections through the server's poll
thread.

Timing: the heartbeat is 5 ms (`server.h:169-173`) and replies are collected
for min(heartbeat, 100 ms) (`src/server_cc.rs:201-210`), so when F plus the
round trip exceeds 5 ms every round misses its own replies; raising
`MAKO_RAFT_HEARTBEAT_INTERVAL_US` (`src/server_h.rs:1753-1766`) raises the
deadline too. Election timeouts are 150-300 ms for the preferred leader and
0.5-2 s for the others (`server.cc:165-202`); they need the "persistence
floor" that `src/server_h.rs:1450-1452` says does not exist only if the
injected delay nears 150 ms.

### 3.5 Applying committed entries

Mako's leader callback acknowledges a transaction (it advances
`local_timestamp_`, `src/mako/mako.hh:417`) and does not replay it into
Masstree; the follower callback replays it (`:271`). So the leader's apply
is an external effect, and it must not get ahead of durability.

- **Two or more servers: nothing extra is needed.** An entry commits only
  after a majority acknowledged it, a follower acknowledges only what it
  flushed, and the leader flushes before it sends; so a committed entry is
  on disk on a majority, the leader included (inference). In detail: a
  commit index is a follower's acknowledged index
  (`core/src/progress.rs:292-361`), which is capped at the end of what this
  leader sent (`core/src/heartbeat.rs:1400-1414`), which R1 flushed before
  sending. Match indexes restart at 0 when a server becomes leader
  (`core/src/node.rs:1200-1202`), and a leader's log only grows while it
  leads (an accepted AppendEntries steps it down first, `:1949-1954`). The
  leader's implicit self-count at its in-memory tail
  (`core/src/heartbeat.rs:104`) is harmless here.
- **One server.** The candidate is the in-memory last index
  (`core/src/helpers.rs:326-341`), committed in the tick
  (`core/src/heartbeat.rs:358`, `:844`) before any flush. Applying it would
  acknowledge an entry durable nowhere.

R3 bounds the apply thread (which polls its queue every 1 ms,
`src/server_h.rs:2625`) by `durable_last`: no wait with two or more servers,
one flush with one (the committing step's, which `sync` requests there,
§3.2; without that trigger nothing would flush after the election and the
apply thread would wait for ever), and a follower waits for its own flush,
which costs nothing that matters. The verified commit rule does not change,
unlike the assessment's §7 item 3.

A stricter option, **Rule A**, applies only through the commit index of the
last completed flush. Every acknowledgement then rests on a durable commit
and so lies inside the theorem; without it the case is argued, as glr argues
it (`glr:docs/ghost-log/raftrs/roadmap.md:547-551`). The leader then waits
for the next tick's flush, which a commit advance requests at once
(`src/server_cc.rs:327-329`): about +F per commit. With one server that
tick has no sends, and the wait is for the committing step's own flush
(§3.2).

### 3.6 Restart

In `SetupInternal` (`src/server_h.rs:1744-1869`):

1. `rpc_ready_` is false (`:1747-1748`): the handlers answer "unavailable".
   Snapshot-manager initialization (`:1795-1804`) returns early with
   snapshots off (`:1904-1923`, after the uncovered-progress check at
   `:1907-1918`).
2. `LoadCurrentConfig` steps `Configure` (`:1811`, `:3545`).
3. **New.** Open the store under `MAKO_RAFT_DATA_DIR` (unset: disk mode off);
   check its header; read records to the first bad checksum, cutting a torn
   tail and failing closed if good records follow a bad one (the
   simulated-disk rule, §3.7); decode each command (`server.cc:438`) and
   build each entry (`src/server_h.rs:3446-3459`); under `mtx_`,
   `step(Restore{term, vote, commit, entries})` and `run_locked_actions`;
   publish `durable_last` and `durable_commit` as the loaded last index and
   commit (all of it is durable), or R3 holds back the re-apply backlog
   `Restore` queues.
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
(`src/server_h.rs:3075-3144`) turns into the re-apply backlog. These checks
catch a damaged or foreign store; they cannot catch a store that holds half
a step (scenario D, §1.2), which only R1 and R2 prevent.

**Mako's state** is rebuilt by that backlog through the follower callback.
Two Mako hazards remain (assessment §8.4-§8.6). The callback is chosen by the
role at apply time (`raft_worker.cc:1091-1147`): holding the campaign covers
the restart, but a server that wins later still applies earlier-term entries
through the leader callback, which does not replay them (true without a disk
too). And control entries run again in the new process: the advancer marker
starts a thread (`src/mako/mako.hh:198-202`), noops call `set_epoch` and
NFSSync's `set_key` and `wait_for_key` (`:224`, `:242`, `:283`), and `reset`
runs (`:335`); whether re-applying old epochs is safe is not verified.

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

### 3.7 Storage engine, the simulated disk, and failure policy

**The engine: a Rust write-ahead log** in a small crate beside the shell,
with no FFI. A header (magic, format version, site, partition, configuration
fingerprint, command format version), then records (length, CRC32C, a hard
state and/or "replace from index i with these entries"), appended to a
sequence of log files; one flush per barrier; a new file and its directory
are fsynced before use, which keeps the format correct on a real disk and
costs almost nothing on tmpfs. No preallocation: on tmpfs it would take RAM
up front (inference). The length and checksum make each record all or
nothing, which is what R2 needs (§1.5). Recovery stops at the first bad
checksum and fails closed if good records follow a bad one. That is the
rule for the simulated disk: the kernel keeps every completed write when a
process dies, so a process crash can tear only the last record, and a bad
record before good ones means a bug or corruption. On a real disk, a power
cut inside an unflushed batch can leave that pattern for records never
acknowledged, and the rule would have to change (inference). A
fault-injecting in-memory backend, which drops records not yet flushed on a
simulated crash, serves the tests.

**Why not RocksDB** (assessment §5; `txlog_core` already links it,
`CMakeLists.txt:1311-1321`): the entries are 4 KB to 1 MiB values written once
and read only at startup, since memory stays the read cache (assessment §3;
§2.1 item 4); an LSM tree writes each byte at least twice, plus compaction
(inference), and needs `extern "C"` kernels where the WAL needs none
(CLAUDE.md: new logic in Rust). If it is chosen anyway: one `WriteBatch` per
barrier with `sync = true`, `DeleteRange` for a replaced suffix, never
`rocksdb_flush` as a sync. Verus is indifferent: storage is trusted either
way.

**What a memtable flush is (Q6).** RocksDB writes each change to two places:
its own write-ahead log file, the safety copy, and the memtable, an
in-memory sorted table that reads consult first. RocksDB describes the
memtable's size option as the "amount of data to build up in memory (backed
by an unsorted log on disk) before converting to a sorted on-disk file"
(`rocksdb/options.h:170-186`; 64 MB by default and in the old store,
`origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:141`). Writing the
memtable out as a new sorted file (an SST file) is the **memtable flush**
(`DB::Flush`, "Flush all memtable data", `rocksdb/db.h:1718`). For writes
the log already holds safely, it is housekeeping: after a crash RocksDB
rebuilds the memtable by replaying its log, by default up to the first
damaged record (`rocksdb/options.h:431-435`, `:1277-1278`). For writes the
log does not hold safely, it is what makes them durable: RocksDB flushes on
close when there are "unpersisted data (i.e. with WAL disabled)", which
otherwise "WILL BE LOST" (`rocksdb/options.h:1319-1321`), and a backup of a
store without its log must flush first "to avoid losing unflushed key/value
pairs from the memtable" (`rocksdb/utilities/backup_engine.h:310-311`;
`rocksdb/options.h:1993-1997`).

The usual durability comes from the log. With `WriteOptions::sync = true` a
write returns only after RocksDB's log is fdatasynced: "similar crash
semantics to a 'write()' system call followed by 'fdatasync()'"
(`rocksdb/options.h:1974-1991`). RocksDB's explicit calls for the log are
`FlushWAL(true)` and `SyncWAL()` (`rocksdb/db.h:1738-1755`). After a synced
write, a memtable flush only moves data that is already safe into another
file.

**The old stores were durable, and wasteful.** mako-dev's store sets
`sync = 1` on its one write-options object
(`origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:146-147`), which
every write uses (§2.3); so did the store `d288748f5` removed
(`d288748f5^:src/deptran/raft/rocksdb_log_storage.hpp:236`) and this tree's
unused copy (`rocksdb_log_storage.hpp:236`). Their writes were durable when
they returned. Their `sync()` is `rocksdb_flush` with `wait = 1`, a memtable
flush (`origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:596-615`;
`rocksdb_log_storage.hpp:756-776`), and Raft called it after every term/vote
write and every entry write (`origin/mako-dev:src/deptran/raft/server.cc:180`,
`:243`; in the removed code through its write-and-sync helper,
`d288748f5^:src/deptran/raft/server.h:1858`). That is waste, not a
durability hole: each call writes a new SST holding little more than one
write, and level-0 files pile up (by default compaction starts at 4 such
files, writes slow at 20 and stop at 36, `rocksdb/options.h:246`,
`rocksdb/advanced_options.h:438`, `:445`; the old store overrides none of
these, `origin/mako-dev:src/deptran/raft/rocksdb_log_storage.hpp:138-143`)
(inference from RocksDB's design). The code's comments show the confusion:
mako-dev's commit write says "Don't sync for commitIndex"
(`origin/mako-dev:src/deptran/raft/server.cc:209`), yet it goes through the
same `sync = 1` options, so it skips only the memtable flush; the removed
code calls its two synced writes "unsynced"
(`d288748f5^:src/deptran/raft/server.cc:805-806`). What the old code lacked
is listed in §2.3: atomicity across fields, the right order, some writes
altogether, and, in async mode, a write before its reply. In this document
"flush" alone always means the WAL write and its fdatasync (§0); RocksDB's
operation is always called a memtable flush.

**Where the store lives: a simulated disk on tmpfs** (owner, 2026-10-06: "we
simulate disk write, we don't require hard disk write on the particular
machine"; the home directory is NFS, so nothing heavy goes there).

- **Where.** On tmpfs (`/tmp`), under its own prefix, for example
  `/tmp/raft-wal-$USER/<site>-<partition>/`. A knob overrides the directory;
  its default is an absolute `/tmp` path, never relative to the repository
  or a build directory, because both sit on NFS: `/home/users` is nfs4 from
  `130.245.173.100:/zoohome`, and so is this worktree (`findmnt`,
  2026-10-06). It must never sit under `/tmp/$USER_*`, which
  `examples/run_rocksdb_test.sh:27` deletes, nor under
  `/tmp/$USER_mako_rocksdb_shard*`, which `ci/ci.sh:84` deletes.
- **The flush cost.** On tmpfs `fdatasync` returns in about 1 µs (below). A
  disk's cost is therefore modelled by an injected delay per flush (a knob,
  default 0), applied where the fdatasync would block, so the barriers and
  group commit see a realistic F.
- **What this tests.** tmpfs files survive `kill -9`, which tests the
  restart path and the order of the writes. But on tmpfs `kill -9` cannot
  tell a written record from a flushed one, so a missing flush is invisible
  there; only the fault-injecting backend, whose simulated crash drops
  records not yet flushed, tests what a flush promises. The injected delay
  models timing.
- **What it does not test.** A device's durability, which is not a
  requirement; and a reboot. tmpfs is RAM, so rebooting zoo-003 erases every
  replica's store on that machine at once: only process crashes are in
  scope. (For reference: `/` is ext4 on a hardware RAID volume.)
- **Docker.** CLAUDE.md runs CI in Docker, but `docker_build.sh` mounts only
  the workspace into the container (`docker_build.sh:722`) and no tmpfs (the
  file has no `--tmpfs`), so `/tmp` inside the container is the container's
  own file system, not tmpfs (inference). Disk-mode runs go on the host, as
  the owner runs tests, or need a tmpfs mount.

**How long a flush takes (Q2).** Measured on zoo-003 (2026-10-06, load
average 0.1-0.5): one writer appends a record to a growing file with
`write`, then calls `fdatasync`, and both calls are timed. The first runs
timed only `fdatasync`, for 4 KB and 256 KB; later runs timed both, for
4 KB, 256 KB, 1 MiB and a record of six 286 KB entries, in several runs,
one of which cut the file back to empty every 64 MiB. The script and raw
data are not in the repository.

| tmpfs `/tmp`, median | 4 KB | 256 KB | 1 MiB | 6 × 286 KB |
|---|---|---|---|---|
| the write, W | 2.6-3.4 µs | 0.10-0.16 ms | 0.40-0.71 ms | 0.68-1.14 ms |
| `fdatasync` | 0.5-1.0 µs | 0.5-1.0 µs | 0.5-1.0 µs | 0.5-1.0 µs |

At p99.9, W runs 2-8 times its median and `fdatasync` stays under 10 µs. A
plain memory copy of 1 MiB takes 0.07-0.1 ms, so a tmpfs append costs 5-7
times a copy of the same bytes. For reference, `fdatasync` on ext4 at
`/var/tmp` takes 0.16 ms (4 KB) and 0.31 ms (256 KB) at the median, 0.66 ms
and 5.8 ms at p99.9.

On tmpfs `fdatasync` has no device behind it, so its time is the system
call, and a flush takes about W plus the injected delay. F is therefore the
injected delay, swept over, say, 0.2, 1 and 5 ms (§3.9). For the payloads
of G3-G6, W is comparable to the 0.2 and 1 ms delays, so it shows in their
results (the W terms of §3.9).

**Failure.** A full or over-quota `/tmp` makes a write fail (ENOSPC, or
EDQUOT: `/tmp` is mounted with `usrquota`, so a per-user limit may sit below
the free space; inference), and the process aborts: any write or fdatasync
error panics, which aborts the process (`Cargo.toml:80-84`) before the reply
or send. On a real disk a retry after a failed fsync would prove nothing
either, since Linux may have dropped the dirty pages. A store with a wrong
header or mid-file corruption fails closed, and that replica must not
rejoin empty under its old id (B6 again); the static configuration cannot
re-add it.

**Growth.** Compaction is off (`src/server_h.rs:1342-1349`), so the log
grows without bound: about 155 MB/s per replica at G2's rate (37,760 × 4 KB;
inference). The whole log is loaded into memory and re-applied at restart,
so restart time and memory grow with it.

On tmpfs the log is RAM. `/tmp` is 47 GB, about 24 GB of it free
(2026-10-06). At G2's rate three replicas write about 465 MB/s, which fills
the free space in under a minute; G4's rate, about 2.7 GB/s, fills it in
about 9 s. Disk-mode throughput runs must therefore either be short and
delete their stores, or use a **timing-only store**: it applies the injected
delay and keeps no payload, so it cannot serve a restart. Restart tests keep
the real files.

### 3.8 Testing

- **Lab, soft restart in process.** In plain words: restart one server
  inside the lab process, without a new process. On the victim's poll
  thread, under the lock, drop the records not yet flushed, replace the core
  with one rebuilt from the file, and reset the shell's bookkeeping. In
  detail: a job on the victim's poll thread, under `mtx_`, drops the
  fault-injecting store's unsynced records, replaces `core` with a fresh one
  stepped through `SetIdentity`, `Configure`, `Restore` and `EnterGates`,
  bumps the apply queue's epoch so queued entries are skipped
  (`src/server_h.rs:2641`), and resets the mirrors, the response slots and
  the lab learner's table for that server. It also sets
  `appliedIndexForWait_` to 0, which is not a mirror (`:1650-1664`): left as
  it was, the apply thread would skip the re-apply backlog as
  snapshot-covered (`:2640`, `:2646-2650`) and `on_applied` would refuse to
  move it back (`core/src/node.rs:1090-1097`); and it publishes
  `durable_last` and `durable_commit` as `Restore` loaded them (§3.6). It
  avoids re-pointing every holder of the server pointer (assessment §6) but
  skips `SetupInternal`.
- **Cases**, after MIT 6.824's persistence tests, as cases 1-11 follow its
  earlier ones (`src/lab_cases.rs:141-549`): restart a follower, the leader, a
  majority, all; no second vote in a term; a crash between write and
  fdatasync with no reply carrying that state; a torn tail; mid-file
  corruption and a foreign store refused; Figure 8 with crashes; and the
  split states of §1.2, each checked to be unreachable from the store. CI
  counts new `init2` ids by itself (`ci/ci.sh:485-490`).
- **Process restart.** A restart mode beside `examples/raft_bench.sh`'s
  leader kill (`:77`): `kill -9`, relaunch on the same directory, integrity
  counters at 0 (`docs/verus/modification-plan.md:1467-1476`); then a `dbtest`
  follower restart. `kill -9` keeps tmpfs and the page cache, so it tests
  ordering, not flushing; only the fault-injecting store tests what a flush
  promises.

### 3.9 Performance

Baselines: `docs/verus/modification-plan.md:1461-1464`,
`docs/performance/raft-rust-9a361eccd/compare-vs-412c225a.txt:15-18`,
`docs/verus/reports/phase-0.md:164-167`. Bytes per second are rate × payload
(inference); effects are a model to be measured, with W the time to write a
batch and F the wait that follows it (in effect the injected delay, §3.7),
so that a flush takes W + F.

| Point | Today | Bytes per replica | Effect of flush-after-unlock + the assessment's S2 | tmpfs (RAM), three replicas |
|---|---|---|---|---|
| G1, 4 KB at 240/s | p50 2.647 ms | ~1 MB/s | +2F per commit, plus a commit-only round off its path (below); Rule A +F more; early-send (outside) would be about +F | ~3 MB/s in all |
| G2, 4 KB unthrottled | 37,760/s | ~155 MB/s | one flush per round on the leader, per AppendEntries on a follower | ~465 MB/s in all: fills the free ~24 GB in under a minute |
| G3, 286 KB × 6 at 190/s | p50 3.343 ms | ~54 MB/s | +2F + 2W | ~160 MB/s in all |
| G4, 286 KB × 6 unthrottled | 3,132/s | ~896 MB/s | six groups, six flush streams (`examples/raft_bench.sh:73`, multi-group) | ~2.7 GB/s in all: about 9 s |
| G5, 1 MiB at 55/s | p50 8.767 ms | ~58 MB/s | +2F + 2W | ~174 MB/s in all |
| G6, 1 MiB unthrottled | 183/s | ~192 MB/s | W dominates | ~576 MB/s in all: under a minute |
| G7, leader kill | ~630 ms to first commit | -- | the new leader's no-op adds about 2F | -- |

The per-commit figures leave out one cost of staying inside the proof. In
plain words: when the leader advances its commit index at a round's end, it
sends a follow-up round at once to announce it, and the proof needs that
commit index on the leader's disk before the round leaves and on each
follower's disk before it replies. In detail: a commit advanced at a round's
end goes out at once in a follow-up round (`src/server_cc.rs:324-329`),
whose sends read the new commit (`core/src/heartbeat.rs:917`) and follow its
segment in the ghost log, so the leader flushes the commit record before
sending them, and each follower flushes its raised commit before replying
(§1.4, §4.2), even when no entry is new. Under G2, G4 and G6 the commit
rides on rounds that carry entries. Under G1, G3 and G5 most commits are
followed by such a commit-only round: F on the leader and F on each
follower, off that commit's latency path, but holding the poll threads and
each follower's one AppendEntries in flight, so they can delay the next
proposal's round (inference). Plain strict sync, with the commit a hint,
pays neither.

Group commit keeps throughput: one flush per proposal would cap a group at
1/F proposals per second (5,000/s at F = 0.2 ms, 87% below G2).

The flush timings are in §3.7. Since the disk is simulated, F is a
parameter: the injected delay, swept over, say, 0.2, 1 and 5 ms. Memory mode
must stay inside today's G1-G7 bounds against `verus-p0`
(`docs/verus/reports/phase-8.md` §4), the barrier costing one branch. Disk
mode needs its own baselines on tmpfs, one for each simulated F.

## 4. What Verus adds

### 4.1 How glr verified crash-restart (A11)

In plain words, glr's approach for raft-rs: a restart is the "step aside"
move; the proof tracks which prefixes of the node's history the host has
saved; a message may leave only once the prefix that produced it is saved;
a crash cuts the history back to the last saved prefix, which is still a
valid history because nothing after it ever left; and the cluster theorem
did not change. It took about two hours. Still trusted: that storage works,
and that the restarted node reads the prefix it saved. In detail:

- **No spec action.** raft-rs's restart (term, vote, log and commit back from
  storage; role, votes, progress and reads forgotten) is, field for field, the
  guard-free `LStepAside`; keeping the commit index leaves every
  commit-monotonicity lemma alone (`glr:docs/ghost-log/raftrs/roadmap.md:514-522`).
- **The persistence boundary is ghost state in the invariant:** the
  certificate of the last Ready the host reported persisted (`durable`) and
  those handed out since (`pending`), each a closed ghost log whose replayed
  term, vote and commit are the hard state on disk, each a prefix of the next
  (`glr:src/ports/raftrs/certs.rs:22-27`, `:39-49`, `:66-71`).
- **Persist, then send** (glr B45, `glr:docs/ghost-log/raftrs/coupling.md:391`).
  A crash truncates the history to the last persisted certificate, which
  holds every emitted `Send`, so it is itself a certificate (roadmap.md:526-534).
  `new_from_store` checks the store in exec, takes that certificate `prev` as
  a ghost argument, and starts its ghost log as `prev` plus one step-aside
  segment (`glr:src/ports/raftrs/raw_node.rs:751-757`, `:785-808`, `:860-867`, `:877`).
  The composition theorem is unchanged: a node's certificate across crashes
  is its last incarnation's log (`glr:docs/ghost-log/raftrs/composition.md:150-174`).
- **Cost:** planned at 4-6 weeks, green in about two hours (roadmap.md:506,
  :569-570); commit `e1f4df62`, 19 files, +902/-100, of which the spec layer
  took only +106 lines of generic lemmas in `ghost_log.rs`.
- **Still trusted or open in glr:** storage reliability (coupling.md:555-556,
  :625-627) and the store matching `prev` (glr B46, B47, :353-354). Its audit
  says the end-to-end chain is not closed: no Verus host carries the
  certificate across a crash, and the harness assumes `prev` with
  `Ghost::assume_new()` (`glr:docs/ghost-log/raftrs/audit-2026-09-30.md:16-18`,
  `:86-96`; roadmap.md:760-764). raft-rs's leader-side early send stays
  outside (glr B45).

### 4.2 Our pinned spec suffices, and what it forces

In plain words: the proof requires the core's commit index to equal the one
in its recorded history, and no model move lowers a commit index. So a
server restarted at commit 0 after reaching commit 7 is in a state its
history cannot produce, and the proof stops covering it. Hence a follower
saves a raised commit index before replying, and a leader saves it before
announcing it. In detail:

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
  is not used. So the raise must be durable before the reply: a flush the
  plain design skips. Likewise a leader's commit advance at a round's end
  precedes, in its ghost log, the follow-up round's sends
  (`src/server_cc.rs:324-329`), so the leader too flushes the commit before
  sending, even when no entry is new (§3.9).
- **Early-send (the assessment's S3) is outside.** An entry sent before the
  leader's flush is recorded in a step a crash may discard, leaving the
  follower's `Recv` with no surviving `Send` (§1.4, fact 4). Admitting it is
  glr's S8 on the openraft line (a durable prefix per node, `LRestart`, the
  leader counted only to its durable index), estimated at 3-4 weeks and not
  started (`git show origin/openraft:docs/ghost-log/openraft/roadmap.md`,
  lines 82, 143, 151-183).

### 4.3 New proof obligations

| Obligation | Content | Estimate |
|---|---|---|
| O1: `Restore` keeps `ginv` | given the previous incarnation's saved history `prev` (a proof-only argument), the new history is `prev` plus one step-aside segment, and the core's fields match what the file holds; the clauses are below the table | 1-2 d |
| O2: `Restore` keeps `inv` | the checks of §3.6 give `inv_config` (`core/src/node.rs:145-178`): the commit index at most the last index (`:149`), the term below the index limit (`:147`), base 1 (`:172-176`); panic freedom | in O1 |
| O3 (check form): the record is exact | `step` ensures that the record applied to the last recorded state gives the new term, vote, commit and log view. The AppendEntries site states its log effect in its loop invariant (`core/src/node.rs:2101-2121`); `append_local` proves its effect only inside (`RaftLog::append`'s ensures, `core/src/log.rs:323`; the assert at `core/src/node.rs:517`) and does not export it (`:501-506`), so O3 adds that ensures | 1-2 d |
| O4 (check form): `sync` covers every send | if the step's ghost segment holds a `Send`, `sync` is set | 0.5-1 d |
| O5: solver budget and frame conditions | the copies change only at the end of `step`, so existing frame conditions hold (inference); `raft_on_append_entries` runs at `rlimit(40)` (`core/src/node.rs:1748`), and glr raised its append handler to `rlimit(200)` for A11 (`glr:docs/ghost-log/raftrs/port-audit.md:843`) | 0.5-1 d |
| O6 (optional): `log_grows` on `step` | turns "the durable certificate is a prefix of the live log" from prose into a theorem; 26 ghost-log assignments in the core | 0.5-1.5 d |

**O1 in detail.** From a core `Configure` set up, given the ghost `prev`
(fully closed, `log_ok` with the same constants, replaying to the stored
term, vote by rank, log view and commit, to the configuration `range(0, n)`
with `conf_index` 0, and with no reads; `log_ok` alone does not pin the
configuration, since the pinned ghost log admits `LoadConfig` and
`ApplyConfChange` segments, while `state_view` fixes both,
`core/src/coupling.rs:295-296`, and `step_aside_log` keeps `prev`'s,
`:468-473`; glr's `restart_cert_ok` carries the same clauses, its B47,
`glr:src/ports/raftrs/raw_node.rs:803-807`), the new core has
`g_log_ = step_aside_log(prev)`, no votes, and `g_match_`, `g_next_` from
`replay(prev)`. It reuses `lemma_step_aside` (`core/src/coupling.rs:475-507`);
the restore loop needs an invariant over the log view; `prev` is an erased
ghost field of the event, the actual previous log as glr's `new_from_store`
takes it. `ginv` also requires every entry to have a value and a term of at
least 0 (`log_entries_ok`, `core/src/coupling.rs:313`, `:329-334`), so O1
needs `Restore`'s value and term checks. And O1 rests on a premise no check
can establish: that the store is the replay of a closed prefix, which R1 and
R2 make true (§1.4).

The assumed form is O1 and O2 plus the host contract (§4.4) and making the
commit index durable: 2-3.5 days. The check form adds O3-O5 (2-4 days):
4-7.5 days in all; the core-reported record it relies on (§3.2) is in P2's
plain estimate.

### 4.4 Host-contract changes

| Where | Today | With persistence |
|---|---|---|
| host-contract.md §1 item 4 (`docs/verus/host-contract.md:51-58`) | a send is made after the call that recorded it returns; `sched` is the real interleaving of the servers' calls | in plain words: a message leaves only after its step's record, and every earlier record, is on disk, and records reach the disk in step order; the cluster theorem's schedule then contains only steps that survived crashes. In detail: ...and after that call's record and every earlier one are durable (R1); records are written in call order (R2); `sched` is that interleaving restricted to the segments that survive in the spliced logs, since a crash discards the unsynced tail and `Restore` replaces the new incarnation's `LoadConfig` segment (`core/src/node.rs:298-301`), while the theorem reads only the latest cores' logs (`core/src/coupling.rs:2674-2685`); that `causal` holds for the splice is argued, not proved, as glr argues it (`glr:docs/ghost-log/raftrs/composition.md:166-173`) |
| host-contract.md §1 item 5 (`:59-61`) | storage: none persisted; no replica restarts in place | storage trusted: a record is atomic, durable when fdatasync returns, read back as written; a crash leaves a prefix of the records |
| host-contract.md §3, a new row | -- | `Restore`: on a core `Configure` just set up, from this server's own store, whose contents are the replay of the previous incarnation's last durable ghost log (glr B46), written under the same configuration, so that its replay's configuration is `range(0, n)` with `conf_index` 0 (glr B47) |
| host-contract.md §7, the codec | a command's value view survives the wire codec | ...and the disk encoding (`server.cc:1414`, `:438`) |
| fix F5's gate (`src/server_h.rs:1816-1841`) | snapshots off, failover on, static configuration | a restart is covered only in disk mode |
| B6 (`docs/verus/bugs-found.md:176-189`) | a trusted assumption | retired for disk-mode runs; kept for memory-mode runs |
| InstallSnapshot | writes term and vote outside `step`, then refuses (`src/server_h.rs:2306-2307`, `:2402-2412`) | refuses first while snapshots are off |

Still outside: a flushed write the store loses (on the simulated disk, a WAL
bug; a real device is out of scope); a restart with another configuration
or another server's store; the snapshot paths; and, unless Rule A is
chosen, the leader's acknowledgement to Mako (§3.5).

### 4.5 Where Verus catches persistence bugs, and where it does not

With the check form:

| Bug | Where it would be on this branch | How it shows up |
|---|---|---|
| a vote-only write not persisted (the assessment's T4: no term change to hook) | `core/src/node.rs:685` | O3 fails: the record no longer yields the new state |
| a truncation without the matching on-disk delete (mako-dev's C++ never deletes the old tail, §2.3; the removed C++ deleted it in a separate write, `d288748f5^:src/deptran/raft/server.cc:840-863`) | `:2094` then `:2127` | O3 fails: the recorded log differs from the log view |
| a new log mutation that forgets to report itself | any future site | O3 fails |
| a step that sends without asking for a flush (candidate before the broadcast, a refusal after a term step) | the four barriers | O4 fails |
| a corrupt or foreign store: a commit past the last index, a term at the limit (O2, `inv`), an entry without a value (O1, `ginv`) | the restore | O1/O2 cannot be proved without the checks of §3.6 |
| a panic on the restart path | the restore | panic freedom; glr's work there found a raft-rs restart crash loop (`glr:docs/ghost-log/raftrs/findings.md:34`) |

It does not check: the shell's queue, lock order and barrier placement; the
WAL code and the store (simulated; a real device is out of scope); that the
store holds whole steps, which is the premise of O1 (§1.4); the store checks
the invariants do not force (entry-term order and bounds, the vote's
membership), which the trusted ghost premise of O1 covers instead; the
decoding at load; the abort on error; Mako's re-apply; peer redial; the
snapshot paths (26 direct writes, code-structure.md §3.2 item 4). Under
strict sync the assessment's "memory means durable" sites (assessment §4.4) hold
trivially, so the proof finds nothing there. With the assumed form, Verus
checks only O1 and O2, and the durability rule rests on review and tests as
it would without Verus.

## 5. Phase plan

Estimates in agent-days. The Verus column is what the certificate adds;
`scripts/verus/verify_core.sh` and the replay check run at the end of every
phase that touches the core.

| Phase | Work | Plain | Verus |
|---|---|---|---|
| P0 | fdatasync and W are measured for one writer on tmpfs (§3.7, 2026-10-06); choose the simulated delays and bound tmpfs use (three writers, run length, the timing-only store) | 0.25 | -- |
| P1 | the WAL crate: records, recovery, the fault-injecting backend, crash-at-every-offset tests | 1.5-2.5 | -- (trusted) |
| P2 | core: `Restore` and its checks; the persist record in `CoreOutput`; recorder and replayer support | 0.5-1.5 | O1, O2 (1-2); the host contract, B6 and the gate text (0.5-1) |
| P3 | shell: the queue and the four barriers (flush-after-unlock); R3 and R5; InstallSnapshot refuses first; the restore in `SetupInternal`, the re-apply backlog, the campaign hold; knobs; abort on error; disk mode refuses snapshots | 1.5-2.5 | the commit index durable and restored (0.5) |
| P4 | -- | -- | check form: O3-O5 (2-4); O6 optional (0.5-1.5) |
| P5 | lab soft restart and the cases of §3.8 | 1.5-3 | -- |
| P6 | `raft_bench` restart mode; a `dbtest` follower restart | 1.5-2.5 | -- |
| P7 | Mako: apply by entry origin, control entries on re-apply (uncertain) | 2-5 | -- |
| P8 | memory mode against today's gates; disk-mode baselines (on the host, §3.7) | 1-2 | -- |
| later | the disk-thread shape: a disk thread and deferred replies (srpc's `DeferredReply`, `src/srpc/rpc/server.rs:443-516`), inside the certificate | 3-5 | small (inference) |

Plain total: 7.75-14.25 days for P0-P3, P5, P6 and P8; 9.75-19.25 with P7.
P1-P3 are the storage manager of §2.4 (3.5-6.5 days).

Order: P0 first, because the simulated F decides whether flush-after-unlock
is enough. O1 goes with P2, since a failing proof may reshape `Restore`; P4
comes before P5, so the record's shape is settled before tests are written
against it.

## 6. Risks and open decisions

Risks:

1. **The simulated F decides the results.** The disk is simulated (§3.7), so
   disk-mode numbers hold for the chosen delay, not for a device. At
   F = 10 ms, G1's latency grows several times and rounds miss their 5 ms
   deadline (inference). Sweep F.
2. **tmpfs is RAM.** At G2, G4 and G6's rates, three replicas write
   0.5-2.7 GB/s in all, and about 24 GB is free (less if a per-user quota
   applies). Full-rate disk-mode runs must be short and clean up after
   themselves, or use the timing-only store (§3.7). Nothing goes to the NFS
   home directory.
3. **Unbounded log.** Restart time and memory grow with the log. Durable
   snapshots are needed for long runs, and they need the spec retarget (1-3
   days, inference), the snapshot coupling (3-5 days,
   `docs/verus/modification-plan.md:819`), and moving the snapshot protocol,
   about 950 lines, into the core (code-structure.md:39-44).
4. **Mako's re-apply hazards** may block full-cluster restarts (P7).
5. **Over-reading the result.** "Persistence verified" would be conditional
   on the shell's ordering and on the store, which is simulated, as in glr.
6. **Proof budget** in the two large handlers (O5).
7. **A lost or corrupt store** takes its replica out for good: rejoining
   empty under the old id is unsafe, and there is no reconfiguration. A
   reboot of zoo-003 erases every store on it at once, since `/tmp` is
   tmpfs: for a group whose replicas all run there, that is the loss of the
   whole group's state.

Decisions for the owner:

| # | Decision | Options | Recommendation |
|---|---|---|---|
| 1 | Stay inside the certificate? | inside (no early-send, durable commit, a flush on each side of a commit-only round) / an uncertified fast mode | inside; measure; then the disk-thread shape, which stays inside |
| 2 | The core's persist record trusted or proved? | assumed form / check form (+2-4 days); the shell's write-and-flush before sending stays trusted in both | check form: it is where Verus catches persistence bugs |
| 3 | Apply bound | the durable last index (free with two or more servers) / Rule A (+F; acknowledgements inside the theorem) | the durable last index; Rule A if the theorem must cover acknowledgements |
| 4 | Storage engine | Rust WAL / RocksDB through kernels | Rust WAL |
| 5 | Mako's re-apply hazards | fix in Mako / restart followers only at first | followers only at first |
| 6 | Snapshots | now / later | later, after P8, as their own phase |
| 7 | Disk mode in CI | from the start / later | from the start (assessment §8.16), run on the host or with a tmpfs mount, since Docker's `/tmp` is not tmpfs (§3.7) |

## Appendix A. The assessment, and what changed on verus-raft

Most of the assessment still holds. Three things changed: the protocol
state now has one writer, `step`, instead of nine term/vote sites in two
files; every send runs on the poll thread; and the commit index must now be
saved, because the proof needs it (§4.2). Two corrections to the
assessment's §2: its citation for the flush the removed code paid on every
write, `d288748f5^:server.cc:1849-1861`, points at compaction code (the
write-and-sync helper is `d288748f5^:src/deptran/raft/server.h:1845-1868`,
with `storage.sync()` at `:1858`); and its "`sync()` is a memtable flush,
not an fsync" (assessment line 20) is true, but the writes were durable
anyway (`sync = 1`, §3.7).

| Assessment | What it said | On verus-raft | Evidence |
|---|---|---|---|
| §1.1 durable state | term, vote, log, snapshot meta; commit an optional hint | same fields, now in the core; commit becomes required for a certified restart (§4.2) | `core/src/node.rs:49`, `:55`, `:69-74` |
| §1.2 term/vote sites | 9 sites in 2 files, no setter; T4 (a grant) logs no term change | 6 sites, all inside `step`; T4 is `core/src/node.rs:685`. Outside `step` only the snapshot paths, including the InstallSnapshot term write that runs even with snapshots off | `core/src/node.rs:663-665`, `:685`, `:759-761`, `:889-890`, `:1901-1902`; `core/src/heartbeat.rs:1359-1360`; `src/server_h.rs:2306-2307` |
| §1.2 log mutators | 4 mutators, 6 sites | 3 sites in the verified configuration; compaction removes nothing under the gate | `core/src/node.rs:510`, `:2094`, `:2127`; `core/src/log.rs:314`, `:404`, `:532`, `:600` |
| §1.3 barriers | vote reply, AppendEntries body (both ids), InstallSnapshot, candidate before broadcast, leader self-count | three of them (vote reply, AppendEntries reply, campaign broadcast) plus the leader's AppendEntries sends (the assessment's S2 flush), each at a critical-section boundary (§3.3); InstallSnapshot needs none, since it refuses first while snapshots are off; the self-count is still implicit | `core/src/heartbeat.rs:104`; `core/src/helpers.rs:326-341` |
| §1.3, §8.12 candidate gap | persist T1 under `mtx_`, or a late write can roll back a newer term | closed by queueing records under `mtx_` in step order (§3.4) | -- |
| §1.4 read-back | load before snapshot-manager init, with its own validation | load after `Configure` (whose premise is a fresh core, and the vote's rank needs the configuration), before `EnterGates`; `inv` and `ginv` force part of the validation (§4.3) | `src/server_h.rs:1795-1830`; `core/src/coupling.rs:2588-2591` |
| §2 storage manager | Option C: a Rust storage trait and a log manager as the one choke point | the choke point exists: the `step` wrapper. The "log manager" reduces to the persist record, the queue and the barriers (§2.4) | `src/server_h.rs:1407-1443` |
| §2 the old RocksDB store | `sync()` is a memtable flush, not an fsync, paid on every write; the removed follower wrote truncate and append as two writes | true, but every write was durable (`sync = 1`); the flush was waste, not a hole (§3.7). The two writes are confirmed; mako-dev's C++ never deletes the old tail at all (§2.3) | `d288748f5^:src/deptran/raft/rocksdb_log_storage.hpp:236`; `d288748f5^:src/deptran/raft/server.cc:840-863`; `origin/mako-dev:src/deptran/raft/server.cc:2393`, `:2408` |
| §4.1 batching | one RPC in flight per follower; ack = min(reported, sent end, last); CONTRADICTORY; commit computed twice per round | unchanged, now in the core | `core/src/heartbeat.rs:908-911`, `:1400-1414`, `:358`, `:1689` |
| §4.2 S1, S2, S3 | S2, with the leader counted at a durable index | S1 and S2 fit the certificate; S3 (early-send) is outside it (glr B45). Without S3 the explicit self-count is not needed (§3.5) | §4.2 |
| §4.3, §4.4 watermarks | no durable index; "memory means durable" sites hold trivially under strict sync | unchanged; `Start` still reports APPENDED from memory | `src/server_h.rs:3884-3914` |
| §5 layout | a DB per (site, partition), never under `/tmp/$USER_*`; term is u64 in state, i64 in entries | both still true; a Rust WAL is recommended instead (§3.7) | `ci/ci.sh:84`; `examples/run_rocksdb_test.sh:27`; `core/src/node.rs:69`; `core/src/log.rs:38` |
| §6 restart tests | none; the lab only disconnects; `shardFaultTolerance` disabled; `recover_fresh` the precedent | unchanged; `recover_fresh` writes the core directly, a `T` line | `src/lab.rs:671-702`; `ci/ci.sh:615-627`; `src/lab_snapshot_cases.rs:1246-1290` |
| §6 in-process restart | rebuild the server object, re-point every holder; `PrepareForShutdown` on a fiber | a soft restart that replaces `core` avoids the re-pointing (§3.8); `PrepareForShutdown` runs on the shutdown thread | `raft_worker.cc:611`; code-structure.md:944-945 |
| §7 items 3-4 | leader self-count and follower ack from a durable index | now changes to the verified core; item 4 also needs deferred replies or a spec change. Neither is needed under strict sync without S3 (§3.5) | `core/src/heartbeat.rs:63-135` |
| §7 threading | handlers hold `mtx_` for the whole body; re-locking aborts | the same in substance: one critical section per handler, then the unlocked actions | `src/server_h.rs:4181-4186`, `:4264-4272`; `server.h:241-250` |
| §8.1 blocking I/O on the reactor | which thread runs the fibers was unverified | the poll thread | `rt/src/seam.rs:166-168`, `:187-211`; `src/server_h.rs:1742`, `:1851-1867` |
| §8.2, §8.3 apply gaps | recovery never enqueues (applied, commit]; a gap is only logged | still true; `Restore` pushes the range itself (§3.6) | `src/server_h.rs:3075-3144` |
| §8.4-§8.6 Mako's replay (re-apply here) | control entries not idempotent; apply depends on the role | unchanged | `raft_worker.cc:1091-1147`; `src/mako/mako.hh:198-335` |
| §8.8-§8.10 | lanes; `raft_test` divergence; compaction needs a durable snapshot | lanes removed; the other two unchanged (compaction is skipped under the gates) | `CMakeLists.txt:465-479`; `src/server_h.rs:3499-3501`, `:1342-1349` |
| §8.14 redial | unverified whether Raft uses srpc's reconnect policy | it does (§3.6) | `src/srpc/rpc/client.rs:1586`, `:1659` |

## Appendix B. Glossary and label index

**Raft, and how the server runs**

| Term | Meaning |
|---|---|
| term, vote, log, commit index | Raft's state: the election period a server is in, whom it voted for in that term, its list of entries, and how far the log is known to be committed. The first four rows of §3.1 are the saved values. |
| applied index | How far committed entries have been handed to Mako. Not saved here (§3.1). |
| Election Safety, Log Matching, Leader Completeness, State Machine Safety | Raft's safety properties (Raft paper, Figure 3): at most one leader per term; two logs with an entry of the same index and term agree up to it; a committed entry is in every later leader's log; no two servers apply different entries at one index. |
| step, step boundary | One call of `RaftCore::step` (or `step_checked`, which network messages enter through and which calls it): one handler run, to completion under `mtx_`, which records zero or more model moves (segments, §1.4). A step boundary is the moment between two calls (§1). |
| emission, emits | Anything a step makes visible outside the server: an RPC reply, the campaign's broadcast, the leader's AppendEntries, and on the leader Mako's acknowledgement of a committed entry (§3.5). |
| crash state | What the disk holds when the process dies; the restart begins from it. |
| split write, split state | A step's changes reaching the disk as separate writes, and the partial state a crash between them leaves (§1.2). Scenarios A-D are four examples. |
| incarnation | One run of the server process between a start and a crash. |
| self-count | The leader counting its own copy of an entry toward a majority, without an explicit acknowledgement (`core/src/heartbeat.rs:104`). |
| strict sync | Every change a message depends on is on disk before the message leaves; nothing is acknowledged from memory alone. |
| group commit | One flush covers every record queued since the last flush. |
| barrier | A point where a thread waits for its flush before it sends: one of the four rows of §3.3. |
| watermark; durable last index, `durable_last`, `durable_commit` | The highest log index, and the commit index, known to be on disk. Published after each flush (§3.4). |
| mechanical substitution | Writing each saved field to disk wherever memory changes it (and reading fields from disk). Rejected in §2. |
| storage manager | The code that owns the disk copy: what each step writes, in what order, flushed when, and how memory is rebuilt at restart (§2.4). |
| soft restart | Restarting one server inside the lab process by replacing its core (§3.8). |
| re-apply backlog | The committed entries (0, commit] a restart hands to the apply thread so Mako rebuilds Masstree. |
| Masstree | Mako's in-memory key-value store; not saved, so a restart rebuilds it from the log. |
| raft-rs; Ready | TiKV's Raft library, glr's second case study. A Ready is its batch of changes to persist and messages to send, handed to the host. |
| openraft | Another Raft library; glr's S8 plans a spec change on its line (§4.2). |

**The code**

| Term | Meaning |
|---|---|
| core, shell, runtime | `core/`: the verified protocol logic, with no I/O. `src/`: the shell, which holds `mtx_`, carries out the core's actions and talks to Mako. `rt/`: the runtime, with the poll thread, RPCs and fibers. |
| `mtx_`, critical section | The server's one lock, and a stretch of code that holds it. |
| poll thread, fiber | The transport's thread, which runs every RPC handler; and the cooperative tasks it also runs (the heartbeat and election loops). |
| `CoreOutput`, actions | What a core call returns for the shell to carry out: actions such as "hand these entries to the apply thread", and log lines. |
| persist record | What a step reports for saving: `hard` (term, vote, commit), `log_from` and `sync` (§3.2). |
| `Restore`, `Configure`, `EnterGates` | Core inputs (events). `Configure` sets the membership, `EnterGates` checks the verified configuration, and `Restore` (new) loads the saved state at restart. |
| the gate, verified configuration | The setting the proof covers: snapshots off, a whole log from index 1, failover on, a static membership (fix F5). |
| APPENDED, CONTRADICTORY | Status codes: `Start` appended an entry in memory (a promise of nothing more); a follower's success reply that claims less than was sent. |
| replay recorder, replay check, `T` line | Instrumentation that records each core call as a line and re-runs the lines through a fresh core; a `T` line marks a write to the core that bypassed `step`, and ends the check. |
| kernels, FFI | Small C++ functions the Rust shell calls across the language boundary; FFI is that foreign-function interface. |
| Option C, Option S | The assessment's recommended storage layer (a Rust storage trait and a log manager); and this document's alternative in which the step wrapper diffs the state instead of the core reporting it (§2.2, §3.2). |

**The proof**

| Term | Meaning |
|---|---|
| glr | Ghost-log refinement: the verification method, its repository, and the group that develops it (§0, Q5). |
| spec, spec action | The hand-written Raft model the proof targets; an action is one atomic move of one server in it, such as `LTimeout` (start a campaign). |
| spec pin, `glr@d7e04ed7`, patch S1 | The frozen version of the model our proof is checked against: commit `d7e04ed7` plus two patches, S1 ("a follower may refuse any AppendEntries") and one that makes a module public (`verus/spec/SPEC_VERSION.toml:4-8`). |
| `LStepAside`, guard-free | The model move "become a follower at the same term, forget the role and the vote count, keep term, vote, log and commit index". Guard-free: allowed at any time. A restart is read as this move. |
| `LStepDown`, `LTimeout`, `LFollowerAppendEntries`, `LBecomeLeader` | Other model moves: adopt a higher term and clear the vote; start a campaign; accept an AppendEntries and reply; become leader (resetting the match and next tables). |
| ghost, ghost state, ghost premise | Proof-only values and assumptions; cargo erases them, so they never run. |
| ghost log (diary), segment, closed, fully closed | The proof-only diary of `Recv`/`Tick`, `Set`, `Send` and `Close` entries. A segment runs to a `Close` and stands for one model move; the log is fully closed when it ends at a `Close` (§1.4). |
| `replay(...)` | The proof function that folds a ghost log into the model state it describes. Not the replay recorder. |
| certificate | A closed ghost log in which every closed segment is a legal model move; "inside the certificate" means the run is one the theorem covers. |
| `ginv`, `inv` | The core's two invariants. `ginv`: the ghost log is fully closed, replaying it gives the core's state, and every entry has a value and a term of at least 0 (`core/src/coupling.rs:305-315`). `inv`: the log is well formed, the commit index is at most the last index, the term is below the limit, the base is 1 under the gate (`core/src/node.rs:122-178`). |
| coupling | The proof code that ties the core's real fields to the model's state (`core/src/coupling.rs`). |
| host contract | What the proof assumes about the code around the core: the shell, the network, the disk (`docs/verus/host-contract.md`). |
| causal, `sched`, splice | The theorem's premise that every received message was sent; the order of all servers' segments (glr: "some linearization of all nodes' closes", `glr:docs/ghost-log/raftrs/composition.md:120`); joining an incarnation's diary to the next one's. |
| `prev`, composition theorem | The previous incarnation's last saved diary, a proof-only argument of a restart; the cluster-wide theorem built from all servers' certificates. |
| exec | Executable code, as opposed to ghost code. |
| `LoadConfig`, `conf_index`, `range(0, n)` | Model terms for the membership: the move that loads the configuration, the index of the last applied configuration change, and the membership as ranks 0 to n-1. |
| `rlimit`, frame conditions | Verus's solver budget per function; the clauses that say what a function leaves unchanged. |
| assumed form, check form | Assumed form: only O1 and O2 are proved, and the core's persist record is trusted. Check form: the core is also proved to report each step's changes exactly and to request a flush whenever the step sends (O3-O5). That the shell writes and flushes before sending stays trusted in both forms (§4.3, §4.5). |
| O1-O6 | The new proof obligations (§4.3). |
| A11 | glr's phase that verified the node side of crash-restart for raft-rs (2026-09-28): the persistence certificates and the restart constructor. The host's part stays trusted (§4.1). |
| glr B42, B45, B46, B47, B49 | glr's numbered conditions for raft-rs's host (`glr:docs/ghost-log/raftrs/coupling.md:352-354`, `:391-392`). B45: send only after persisting; B46: restart from a store the node persisted; B47: the stored configuration agrees with it. Ours are written without "glr". |
| S8 | glr's planned openraft spec change admitting early sends (§4.2). |

**Storage and disks**

| Term | Meaning |
|---|---|
| fsync, fdatasync | System calls that wait until a file's data is on the device. On tmpfs they return at once. |
| flush | In this document: write the queued records to the log file, then fdatasync it and wait the injected delay; it takes W + F. Never RocksDB's memtable flush. |
| F, W | The wait of one flush after its write: fdatasync (about 1 µs on tmpfs) plus the injected delay, so in effect the injected delay; and the time to write one batch, which grows with its bytes (§3.7). Not to be confused with the fixes F1-F9. |
| write-ahead log, WAL | An append-only file of records, written before the effects they describe. The design's store is one (§3.7); RocksDB keeps its own. |
| record, CRC32C, torn tail | One step's changes in the log file; the checksum each record carries; a half-written last record, cut at recovery. |
| tmpfs, NFS, `usrquota` | A file system kept in RAM (`/tmp` on zoo-003), which survives a process crash but not a reboot; the network file system of `/home/users`; the mount option that allows per-user limits. |
| injected delay | The knob that stands in for a disk's flush time. |
| fault-injecting backend, timing-only store | Two test stores: one drops records not yet flushed on a simulated crash; the other only waits the injected delay and keeps no data. |
| LSM, memtable, memtable flush, SST | RocksDB's design: writes go to an in-memory sorted table (the memtable) and to RocksDB's own log; a memtable flush writes the table out as a sorted file (an SST). It makes durable the writes the log does not already hold safely; after synced writes, as all of the old store's were, it is redundant (§3.7). |
| `sync = 1` | RocksDB's per-write option that fdatasyncs its log before the write returns: what made the old store's writes durable. |
| WriteBatch, DeleteRange | RocksDB's atomic multi-write, and its range delete. |
| agent-days | The unit of the effort estimates. |

**Labels**

| Label | Meaning |
|---|---|
| R1-R5 | The persistence rules (§3.3). |
| scenarios A-D | The split-write examples of §1.2. |
| flush-every-change, flush-under-lock, flush-after-unlock, disk-thread, early-send | The threading shapes of §3.4. The previous version of this document called them A1, A2, B1, B2 and S3. |
| S1, S2, S3 (the assessment's) | The assessment's options for the leader's own log: flush per proposal; flush per round before sending; send before flushing (= early-send). Not the spec's patch S1. |
| Rule A | Apply only through the commit index of the last completed flush (§3.5). |
| G1-G7 | The plan's benchmark points (`docs/verus/modification-plan.md:1483-1491`): 4 KB at 240/s and unthrottled (G1, G2), 6 groups of 286 KB at 190/s and unthrottled (G3, G4), 1 MiB at 55/s and unthrottled (G5, G6), and a leader kill (G7). |
| P0-P8 | The phases of §5. |
| T1, T4 | The assessment's labels for term/vote write sites: T1 the campaign start, T4 a vote grant. |
| fix F1-F9 | The verification work's numbered behaviour changes (`docs/verus/code-structure.md:978`): F1 one vote per voter, F3 the leadership re-check where the message is built, F4 entry terms below 1 refused, F5 the verified-configuration gate, F9 message admission. |
| B1-B19 | The bug log (`docs/verus/bugs-found.md`). Those named here: B1 the candidate's "no" quorum can never form early; B4 AppendEntries entry terms are not validated; B6 memory-only Raft state, in-place restart unsafe; B7 `get_outstanding_logs` mixes two counters; B8 `setIsLeader`'s stale-publication term check is a tautology; B14 the heartbeat interval is a plain field written while the loops read it; B16 an entry with no value is never replicated; B17 the round end can commit after leadership is lost; B18 shutdown takes a leader outside its reply and round-end premises; B19 every thread holds its own `&mut RaftServerBase`. glr's conditions are written "glr B45" etc. |
