# Manual Figure Drawing Instructions

The generated Graphviz diagrams are only rough placeholders. Use this file as
the drawing brief for making clean thesis figures manually in PowerPoint,
Keynote, Figma, draw.io, or LaTeX/TikZ.

The style should follow systems papers like ROLIS and Mako:

- Prefer simple block diagrams over decorative art.
- Use 2-4 colors consistently.
- Keep text inside boxes short.
- Every arrow should mean data flow, control flow, or leadership transfer.
- Every figure should have one clear takeaway.
- Avoid source-code class names unless the figure is explicitly about
  implementation.

Suggested color mapping:

- Mako worker/execution path: blue.
- Replication/consensus layer: green.
- Replay/apply path: orange.
- Bottleneck or problem state: red.
- Disk/persistence path: gray or purple, used sparingly.

## Figure 1: Mako Baseline Replication Path

**Purpose:** Explain the original system shape before introducing Raft.

**Main takeaway:** Mako workers produce transaction logs on a leader process;
the replication backend orders those logs; followers replay committed logs.

**Layout:**

Draw left-to-right.

Left block:

```text
Leader process
Mako worker threads
```

Inside or next to it, show 3-4 small worker boxes labeled:

```text
worker 1
worker 2
worker 3
...
```

Middle blocks:

```text
Transaction log entries
Replication interface
Original Paxos backend
```

Right block:

```text
Follower p1
Replay committed logs
```

Second right block:

```text
Follower p2
Replay committed logs
```

**Arrows:**

- workers -> transaction log entries: label `produce logs`.
- transaction log entries -> replication interface: label `submit`.
- replication interface -> Paxos backend: label `dispatch`.
- Paxos backend -> followers: label `committed log order`.
- followers -> local database state: optional short arrow labeled `replay`.

**Do not show:**

- Raft yet.
- Preferred leader yet.
- Detailed RPC names.
- File names or C++ class names.

**Caption draft:**

```text
Mako's original replication path. Leader-side worker threads execute
transactions and submit serialized transaction logs to the replication layer;
followers consume the committed log order and replay it into local state.
```

## Figure 2: Paxos, Multi-Raft, and Single-Raft Topologies

**Purpose:** Explain the architectural comparison.

**Main takeaway:** Multi-Raft preserves the Paxos-shaped parallelism, while
Single-Raft consolidates all partition logs into one Raft group.

**Layout:**

Draw three side-by-side panels.

Panel A title:

```text
Original Paxos
```

Panel A boxes:

```text
worker/partition 1 -> Paxos stream 1
worker/partition 2 -> Paxos stream 2
worker/partition N -> Paxos stream N
```

Panel B title:

```text
Multi-Raft
```

Panel B boxes:

```text
worker/partition 1 -> Raft group 1
worker/partition 2 -> Raft group 2
worker/partition N -> Raft group N
```

Panel C title:

```text
Single-Raft
```

Panel C boxes:

```text
worker/partition 1
worker/partition 2
worker/partition N
        \ | /
   one Raft group
commands carry partition ID
```

**Visual emphasis:**

- Use repeated parallel lanes for Paxos and Multi-Raft.
- Use fan-in for Single-Raft.
- Make Single-Raft visually simpler but also visibly centralized.

**Do not show:**

- ReplayPool here.
- Disk.
- Preferred leader.

**Caption draft:**

```text
Replication topologies evaluated in this thesis. Multi-Raft keeps the
per-partition replication shape of the original Paxos design, whereas
Single-Raft consolidates all partition commands into one Raft log.
```

## Figure 3: Preferred Leader Election for Mako

**Purpose:** Give preferred leader real credit as a Mako-specific systems
contribution.

**Main takeaway:** Mako submits transaction logs through one expected local
leader. Preferred leader election keeps Raft aligned with that path while
still preserving normal failover.

**Layout:**

Use a left-to-right layout with three visual regions.

Left region:

```text
Mako workers
leader-side transaction submission
```

Draw two or three small worker boxes inside this region.

Middle region:

```text
Preferred leader
localhost
receives Mako transaction logs
```

Right region:

```text
Backup replica p1
can lead if preferred fails
```

```text
Backup replica p2
can lead if preferred fails
```

Below the cluster, draw three leadership placement cases. These are cases, not
a mandatory timeline, so do not connect them with strong arrows:

```text
normal: preferred replica leads
failure: backup can lead
recovery: preferred returns caught up
```

**Arrows:**

- Mako workers -> preferred leader: label `submit transaction logs`.
- Preferred leader -> backup replicas: label `Raft log replication`.
- The case strip explains normal/failure/recovery behavior. It should not look
  like every run must pass through all three states in order.

**Safety annotation:**

Add a small note box:

```text
Raft safety rules are unchanged
preference affects leader placement only
```

**Do not show:**

- Preferred leader as a new consensus protocol.
- Guaranteed leadership under failure.
- Generic Raft election state machine.
- Detailed RPC names such as `AppendEntries`.
- Timeout constants or implementation method names.

**Use these labels:**

```text
Mako workers
Preferred leader
localhost
Backup replica p1
Backup replica p2
submit transaction logs
Raft log replication
Raft safety rules are unchanged
```

**Avoid these labels in the figure body:**

```text
AppendEntries
RequestVote
ElectionTimeout
candidate state
term/vote metadata
```

**Caption draft:**

```text
Preferred leader election aligns Raft leadership with Mako's leader-oriented
submission path. The preferred replica is favored when it is healthy and
caught up, while ordinary Raft failover remains available.
```

## Figure 4: Single-Raft Bottleneck Before ReplayPool

**Purpose:** Show the problem that motivated ReplayPool.

**Main takeaway:** A consolidated Single-Raft log can order entries quickly, but
heavy follower replay can serialize behind a narrow apply/replay path.

**Layout:**

Draw as a pipeline from left to right:

```text
many Mako workers
```

arrow to:

```text
single Raft log
```

arrow to:

```text
single apply/replay path
```

arrow to:

```text
follower database replay
```

At the single apply/replay path, draw it narrow or red.

Add a side note:

```text
as worker count grows,
replay work queues here
```

**Visual emphasis:**

- Workers should be wide/parallel.
- Single apply/replay path should be narrow.
- The figure should visually communicate fan-in and serialization.

**Do not show:**

- This as the final design.
- Multiple replay workers.
- Disk.

**Caption draft:**

```text
Naive Single-Raft consolidation risks a replay bottleneck: many workers feed
one ordered log, but expensive follower replay can accumulate behind a narrow
apply/replay path.
```

## Figure 5: Final ReplayPool Pipeline

**Purpose:** Explain the fix.

**Main takeaway:** Raft apply is lightweight staging; heavy replay work is
parallelized by ReplayPool.

**Layout:**

Draw left-to-right pipeline:

```text
Mako workers
```

arrow to:

```text
Raft commit path
```

arrow to:

```text
raft_apply
lightweight staging
```

arrow to:

```text
Mako callback
copy committed payload
```

arrow to:

```text
ReplayPool queue
```

Then fan out to:

```text
replay_1
replay_2
...
replay_N
```

Add one clear note:

```text
1:1 means MAKO_REPLAY_THREADS = worker_threads
```

**Visual emphasis:**

- `raft_apply` should be small and lightweight.
- ReplayPool should be the wide parallel part.
- Use green or blue for the final fixed path.

**Do not show:**

- `raft_apply` doing all heavy replay.
- One Raft group per worker.
- `1:1` as anything other than replay threads matching worker threads.

**Caption draft:**

```text
Final Single-Raft replay pipeline. The Raft apply path stages committed
payloads, while ReplayPool workers perform heavy follower-side replay in
parallel.
```

## Figure 6: Persistence/Simulated-Disk Proof Path

**Purpose:** Explain why the disk results have proof statistics.

**Main takeaway:** The simulated disk measures per-source storage work from
Mako data writes, Raft log writes, and Raft metadata writes, so disk slowdowns
can be tied to measured bytes, writes, and bytes per committed transaction.

**Layout:**

Draw left-to-right:

```text
committed transaction log
replicated payload
```

branches into:

```text
Mako data writes
tag: mako_data
```

```text
Raft log writes
tag: raft_log
```

```text
Raft metadata writes
tag: raft_metadata
```

all feed into:

```text
Simulated disk
simulated disk model
bandwidth + latency pressure
per-source accounting
```

then output to metric boxes:

```text
persisted bytes
modeled writes
bytes per transaction
source breakdown
```

**Visual emphasis:**

- Use reader-facing labels for the source boxes and put the exact CSV/log tags
  as smaller subtitles: `mako_data`, `raft_log`, `raft_metadata`.
- Show that this is instrumentation/modeling, not a real SSD benchmark.
- Use `FakeDisk` only when referencing the implementation or CSV/log counter
  names outside the figure.
- Do not emphasize max wait in the main figure; the thesis uses bytes, writes,
  and bytes per transaction as the cleaner proof.

**Caption draft:**

```text
Source-tagged simulated-disk instrumentation. Persistence writes are charged
to a simulated disk model and reported as bytes, writes, bytes per committed
transaction, and source breakdowns, allowing disk throughput changes to be
explained with measured storage work.
```

## Figure 8: Evaluation Storyboard

**Purpose:** Optional overview figure for the evaluation section.

**Main takeaway:** Each experiment answers one claim in the thesis.

**Layout:**

Draw as a four-row table or flow:

```text
No-disk four-way sweep
Claim: Raft can match Paxos-class throughput
```

```text
ReplayPool sensitivity
Claim: replay parallelism fixes Single-Raft bottleneck
```

```text
Disk persistence sweep
Claim: disk behavior is backed by bytes/write proof
```

```text
Variance reruns
Claim: headline results are stable
```

**Caption draft:**

```text
Evaluation roadmap. Each experiment is tied to one thesis claim, keeping the
results section organized around evidence rather than scripts.
```

## Practical Drawing Workflow

1. Start with PowerPoint, Keynote, Figma, or draw.io.
2. Use one slide/page per figure.
3. Use the same font and color palette across all figures.
4. Export each figure as PDF for the thesis.
5. Also export SVG or PNG for quick preview in Markdown.
6. Save editable sources under:

```text
doc/thesis/figures/manual_sources/
```

7. Save final exported figures under:

```text
doc/thesis/figures/final/
```

Suggested final filenames:

```text
fig01_mako_baseline_replication.pdf
fig02_replication_topologies.pdf
fig03_preferred_leader.pdf
fig04_single_raft_bottleneck.pdf
fig05_replay_pool_pipeline.pdf
fig06_simulated_disk_proof_path.pdf
fig08_evaluation_storyboard.pdf
```

## Quality Checklist

Before using a figure in the thesis:

- Can the reader understand the figure in 10 seconds?
- Does the caption state the claim?
- Are all arrows labeled or obvious?
- Is there less than one sentence of text inside each box?
- Is the figure about mechanism, not implementation trivia?
- Does it avoid overclaiming results not yet measured?
- Does it match the terminology in the thesis text?
