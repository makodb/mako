# Thesis LaTeX Edit TODO

Use this file to collect thesis edits before applying them in batches. Keep notes concrete: include the target file/chapter, the problem, and the intended change.

## Workflow

1. Add new notes under `Inbox`.
2. Move related notes into a batch section when ready.
3. Apply one batch at a time.
4. After each batch, run:

```bash
cd /home/users/mmakadia/mako/doc/thesis/latex
make check
```

5. Move completed items to `Completed` with a short note.

## Inbox

- [ ] Review Chapter 5 for repeated definitions now that Chapters 1-4 explain the core Mako/Raft terminology.
- [ ] Check Chapter 5 onward for the same preferred-leader wording used in the abstract and Chapters 1-4: preferred leader election is used to align Raft leadership with Mako's leader-oriented submission path for both Multi-Raft and Single-Raft, not as a new Raft safety requirement.
- [ ] Check Chapter 5 onward for the same ReplayPool wording used in the abstract and Chapters 1-4: ReplayPool is the Single-Raft replay-path fix, not a Multi-Raft requirement.
- [ ] Confirm all figure references are introduced before the figure appears or immediately near it.
- [ ] Figure 3.1 manual PDF still visually says “One group per process” inside the Single-Raft panel. The LaTeX prose and caption are corrected, but the manual source/export should be updated to say “one consolidated Raft group” and “one local replica per process.”
- [ ] Fix the existing Chapter 7 underfull box warning if it is visually noticeable in the PDF.
- [ ] Audit and revise the phrase “one Raft group per process” for Single-Raft in Chapter 5 onward. The abstract and Chapters 1-3 now use more precise wording, but later chapters and captions still need the same pass.
- [ ] Search Chapter 6 and Chapter 8 for persistence wording and make sure NVMe is mentioned alongside Cloud-SSD where appropriate. Avoid implying the thesis only evaluated Cloud-SSD.

## General Audit Checklist

- [ ] During later PDF review, continue looking for orphaned first lines at the bottom of a page, especially after tables/figures. The Chapter 2 table case has been handled with a local `\Needspace` guard; later chapters still need visual review.

## Batch 1: Opening Arc

- [x] Rewrite abstract so new readers understand Mako, Raft replacement, preferred leader, ReplayPool, and the core results.
- [x] Expand Chapter 1 into problem, design overview, contributions, evidence preview, and roadmap.
- [x] Expand Chapter 2 into detailed Mako/Paxos background with transaction, commit, replay, worker lanes, figure explanation, and replication modes.
- [x] Correct preferred leader wording: both Raft designs need preferred leader placement; Single-Raft additionally needs ReplayPool.

## Batch 2: Next Candidate

- [x] Audit Chapter 3 for clarity and consistency with Chapters 1-2.
- [x] In Chapter 3 and the topology figure caption, replace or qualify “one Raft group per process” so Single-Raft is described precisely as a process-wide Raft worker/log/replica participating in the consolidated group.
- [x] Chapter 3: remove low-level helper/function names from the prose and replace them with conceptual descriptions of setup, callback registration, log submission, and shutdown.
- [x] Chapter 3: audit remaining code-level identifiers and function names; keep only thesis-level implementation concepts.
- [x] Chapter 3.5 Log Submission Path: rewrite the section in simpler terms around the transaction-log journey and Single-Raft partition-lane routing.
- [x] Chapter 3.6 Commit and Callback Delivery: rewrite the section in deeper but simpler terms around replication, majority commit, Mako callback delivery, and role-aware behavior.
- [x] Chapter 3.6 second pass: rewrite commit/callback delivery as a novice-friendly two-question lifecycle with a concrete lane-3 example for Multi-Raft versus Single-Raft routing.
- [x] Audit Chapter 4 to give preferred leader enough credit without overclaiming safety changes.
- [ ] Audit Chapter 5 to make the Single-Raft replay bottleneck and ReplayPool story easy to follow.

## Batch 3: Opening Precision Edits

- [x] Abstract: replace imprecise Single-Raft wording with “one process-wide Raft log” and “one replica of the consolidated Raft group running in each process.”
- [x] Abstract: mention simulated NVMe and Cloud-SSD persistence models before quoting the Cloud-SSD flattening result.
- [x] Chapter 1.2: explicitly say Chapter 4 explains preferred leader election in depth.
- [x] Chapter 1.2: replace “one Raft group per process” with precise Single-Raft wording.
- [x] Chapter 1.2: briefly define “apply path” and “replay work” before using them in the Single-Raft/ReplayPool overview.
- [x] Chapter 1.3 and evidence preview: explain that simulated disk is intentional because real SSD/cloud-storage behavior varies, and mention both NVMe and Cloud-SSD.
- [x] Chapter 2.2: explain worker lanes and replication streams concretely, including how consensus orders records within a stream and why multiple streams preserve parallelism.
- [x] Chapter 2 table: replace “one Raft group per process” with process-wide Raft log wording for Single-Raft rows.
- [x] General opening-layout pass: add a local `\Needspace` guard after the Chapter 2 modes table so the following explanatory paragraph does not start as an isolated first line at the bottom of a page.

## Completed

- 2026-05-10: Opening arc rebuilt successfully with `make check`; PDF remains PDF/A-2b with embedded fonts.
- 2026-05-10: Applied opening precision edits for the abstract, Chapter 1, and Chapter 2; later-chapter wording audits remain in Batch 2.
- 2026-05-10: Verified opening precision edits with `make check`, source search, and PDF text extraction for the edited abstract/Chapter 1/Chapter 2 pages.
- 2026-05-10: Rewrote Chapter 3 as a detailed Raft integration chapter covering runtime dispatch, topology, RaftWorker setup, submission, command wrapping, and callback delivery.
- 2026-05-10: Applied Chapter 3 readability pass after student/faculty audits: removed API inventory language, simplified log submission and callback delivery, clarified Single-Raft leadership/routing, and softened preferred-leader wording through Chapter 3.
- 2026-05-10: Rewrote Chapter 3.6 again around a novice-friendly commit-then-route lifecycle, including a lane-3 example to explain Multi-Raft group routing versus Single-Raft partition-lane-id routing.
- 2026-05-10: Rewrote Chapter 4 as a full preferred-leader chapter: placement problem, Mako/Paxos contrast, preference-not-authority design, startup/failover/failback mechanism, safety boundary, non-claims, and validation matrix.
- 2026-05-10: Applied Chapter 4 preferred-leader cleanup: replaced lab process names with submission/preferred/backup replica terminology and added the missing motivation that Mako's generated logs enter a local submission path without remote elected-leader forwarding.
- 2026-05-11: Updated abstract through Chapter 4 for the preferred-leader recovery work: startup placement remains limited, backups can lead during ordinary failover, failback is guarded by preferred-replica catch-up, and Chapter 4 now cites the preferred-leader audit/failback tests plus the fresh Single-Raft/Multi-Raft no-regression sweep without replacing Chapter 8 result data.
