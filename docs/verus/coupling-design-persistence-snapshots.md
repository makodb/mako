# Coupling design: persistence and snapshots (step 1a)

> **Status (2026-10-19).** Implemented and proved. The restructuring: E1 and
> E2 as F25, E3 as F26, E4 and E5 as F27, E6 as F28 (which deleted
> `ObserveTerm`), E7 as F30 (modification-plan.md A.2). The proof: Part A's
> P1, P3 and P4 (P2, the notes' exactness, stays checked rather than proved),
> spec v4 with W1, and Part B's coupling of every snapshot event inside the
> gate (F31), after which the gate admits snapshots (F32, which also removed
> F29's `LeaveGates`). One departure from the text below: the gate keeps
> refusing a memory build that restarts from a snapshot image, as F30's
> approval decided (the open question on that, near the end). The text below
> is the design as reviewed.

Step 1a of the plan to prove Raft's persistence and snapshot paths against the
group's spec v2 ([spec-changes.md](spec-changes.md)): which code path is which
spec action or ghost-log entry, the ghost state and invariant to add, the
behaviour-preserving restructuring step 1b needs, the premises (what becomes
proved, what stays trusted), the proof obligations, the fit with the spec, and
the tests. Written 2026-10-19 by a read-only workflow (four studies of glr and
of Mako, two drafts, one adversarial review against the code and glr at
`raft-spec-v2-export`); the review's 24 corrections are folded in and listed
at the end.

**Order.** Persistence first, with snapshots still off under the gate; then
the snapshot gate (Part B), whose decisions are mostly behaviour or spec
changes and wait for the owner.

**Defaults taken for Part A** (the owner may overrule):
- the `CertProtocol` instance for `RaftP` goes in as spec patch 0004 (export-only,
  like 0003), since Rust's orphan rule keeps it out of Mako;
- `Event::Restore` carries the store's `d` as an exec field the core ignores;
- the incarnation join and the store fold stay on paper;
- `certs_ok` lives inside `log_ok` (glr's precedent), moved out only if the solver
  needs it;
- the leader's apply wait stays outside premise D (it is not a message).

The one Part-A-adjacent decision left to the owner is B55 (bugs-found.md): a
gated server does not refuse InstallSnapshot.

## Part A: Persistence coupling (corrected)

### A1. Mapping

| Code path | Ghost side |
|---|---|
| `RaftCore::new` (node.rs:184-226) | `g_log_` empty, `g_seq_ = 0` |
| `finish_persist` leaves a note (output.rs:168-177, called at event.rs:603-607) | `g_seq_ += 1`, with no log entry. Under premise N this is the number the queue gives the record. |
| end of `step`, when `final.ginv()` and (`final.gated_` or a Restore was taken) | push `Mark(Ready{number: g_seq_, term, vote: rank(vote)+1 or 0, commit})`. Every Send the step recorded comes before this mark. |
| `disk_note` -> `push_note` -> `RecordQueue::push` (server_h.rs:1619-1625, disk.rs:409-411, queue.rs:37-46; first number d+1, disk.rs:345) | WAL record number `g_seq_` (premise N) |
| waits: tick (server_cc.rs:150, 162-165), campaign (server_h.rs:3603-3613), held replies (service.rs:105-118, flusher.rs:125-143) | premise D |
| flusher publishes d (flusher.rs:200-213) | nothing during the run. A crash at durable d is the cut `durable_cert(old ++ Persisted(d))`. |
| `Restore` (event.rs:577-601, node.rs:571-640). It is stepped whenever the store was not freshly created (disk.rs:375, server_h.rs:2179-2196). | `g_log_ = step_aside_log(restore_prev(old,d,n)) ++ Ready(d)`, and `g_seq_ = d` |
| `OpenDiskStore`, `wal::recover` (server_h.rs:2107-2175, wal.rs:186) | trusted fact T2 |
| snapshot record pushes (server_h.rs:2882, 3243) | outside the gate; Part B routes them through notes |

There are no Persisted marks during the run: the core never reads durability. A `Persisted(d)` appended at the crash covers every Ready numbered at most d. So the cut is disk-persistence.md §6's: the last step before the step of the first lost record. That includes steps with no record whose sends left. No new event, lock or timing change is needed.

### A2. Ghost state and invariant

- **`g_seq_: Ghost<int>`**, declared `#[cfg(verus_keep_ghost)]` like `g_log_` (node.rs:109-116).
- **`log_ok(l,c)` gains `certs_ok::<RaftP>(c,(),l)`** (glr ghost_log_certs.rs:132-143). Exactly six proofs establish `log_ok`:
  - the four primitives (coupling.rs:55-160), each given `broadcast use lemma_certs_push_auto` (:160-171);
  - `lemma_new_ginv` (:363-364) and `lemma_load_config` (:417-418), each given `lemma_certs_empty`.

  Every other `log_ok` goes through the primitives, including `lemma_step_aside` (:565-578) and the 27 `g_log_` assignments. `restore_prev_ok`'s `log_ok(prev)` (:490-493) now carries `certs_ok(prev)`.
- **New `ginv` conjunct `ready_seq_ok(g_log_, g_seq_)`:**
  - Ready numbers never decrease, and consecutive ones differ by at most 1.
  - The last Ready number is `g_seq_`.
  - With no Ready mark, `g_seq_ == 0`.
  - `first_ready(l)` is a spec function giving the first Ready mark's number.
- **New spec functions:**
  - `saved_view` (term, vote, commit, log view);
  - `apply_note`, the spec form of `Shadow::apply` (replay/src/lib.rs:846-862);
  - `crash_cert(old,d) = durable_cert::<RaftP>(old.push(Mark(Persisted(d))))`;
  - `restore_prev(old,d,n) = if durable_pos(old.push(Mark(Persisted(d)))) >= 0 { crash_cert(old,d).log } else { load_config_log(n) }`.

  The fallback is sound: only gated steps send, and the end of EnterGates pushes the first Ready mark, so a log with no Ready mark sent nothing.

### A3. Restructuring for step 1b

- **Ghost only:** the field; closed accessors and ensures in output.rs; a trailing proof block in `step`; the Restore arm; the coupling.rs additions.
- **Exec, required:** `Event::Restore` gains `d: u64`. `RestoreDiskState` already holds `r.d` (server_h.rs:2181, 2214), and the replay parser reads it. `restore` ignores it, so there is no runtime effect. With an existential d, `g_seq_` would be an unspecified witness and N could not link Ready numbers to WAL numbers.
- **Exec, optional:** a number on `PersistNote`, with a `debug_assert` in `disk_note` that `push_note` returned it. This would check N instead of trusting it.
- **Gate gap (owner decision):** an InstallSnapshot that reaches a gated server is not refused at entry. It steps the uncoupled `ObserveTerm` and writes `current_leader_id_`, `req_voting_` and `election_in_progress_` outside step (server_h.rs:2691-2718). There are two ways to close this:
  - refuse the RPC at entry when `gated_` is set (this changes behaviour only for a misconfigured peer);
  - or add the host premise "no member enables snapshots".

### A4. Premises and host contract

**Proved:**
- **(P1) Coverage.** If the step pushed a Ready mark, then `last_ready_pos == len-1` and `last_cert.number == g_seq_`. For every event except Restore (which replaces the log), `log_grows(old.g_log_, last_cert.log)` also holds.
- **(P2) Note exactness.**
  - `final.g_seq_ == old.g_seq_ + (note ? 1 : 0)`.
  - With no note, `saved_view` is unchanged.
  - With a note, `saved_view(final) == apply_note(saved_view(old), note, final.raft_log_)`.

  This retires the trusted fact at host-contract.md:71-75. log.rs already has the view-level ensures: `append` (:322-331), `truncate_from` (:460-472), `reset` (:656-665) and `get` (:274-278). node.rs:2211-2214 already asserts the tail after truncation.
- **(P3) `lemma_crash_cut`.** Given `ready_seq_ok(old,s)` and `first_ready(old) <= d <= s`:
  - the cut is the last Ready mark numbered at most d, and its number is exactly d;
  - every Ready numbered at most d has its certificate's log as a prefix of the cut;
  - the cut is `cert_ok`, by `lemma_certs_ok_at` and `lemma_durable_cert_ok` (:319-343).

  It needs its own induction, because `lemma_durable_pos_upto_from` (:373) is private. The lower bound is necessary: an incarnation restored at 5 has no mark numbered 3.
- **(P4) Restore.** The premise `restore_cert_ok(core, d, term, vote, log, commit)` is: there exists `old` such that
  - `log_ok(old)` and `fully_closed(old)`;
  - `ready_seq_ok(old, s)`, and if `old` has a Ready mark then `first_ready(old) <= d <= s`;
  - `restore_prev_ok(core, restore_prev(old,d,n), …)`.

  The arm sets `g_log_ = step_aside_log(prev)` and `g_seq_ = d`, then pushes `Ready(d)`. `certs_ok` survives by `lemma_certs_grow`; `ready_seq_ok` survives because the cut ends at d or d-1. The arm ensures that `crash_cert(g_log_, d).log` extends `prev`.

**Host contract, §1 item 5 becomes:**
- **D:** an output a step produced leaves only after the WAL is durable through that step's Ready number. The tail, read after the section's last push, bounds that number from above.
- **N:** under the gate, the queued records are exactly the steps' notes, in step order, numbered from the restored d+1.
  - Every core step goes through the wrappers: `core().step` and `step_checked` are called only at server_h.rs:1587, 1598, 1639 and 1651.
  - Saved fields are also written outside step on the snapshot paths (server_h.rs:2446-2466, 2828-2869, 3232-3233). Under the gate these are excluded by:
    - the fail-closed check at OpenDiskStore (server_h.rs:2147-2154);
    - no snapshot manager being set (server_h.rs:2273-2291);
    - the `verified_gates_` early returns at server_h.rs:1522 and 3378.
- **T2:** recovery returns records 1..d whole. d is at least every durable number the previous run published, and at least that run's starting d. The state equals the fold of `apply_note` over those records.
- **The incarnation join stays on paper.**
  - By D, every send that left came from a step numbered at most d.
  - By P3, that step is in `prev`, and the new `g_log_` extends `prev`.
  - So `theorem_mako_safety` applies unchanged.
  - As today, the existential `old` is instantiated with the real log in the argument.
  - glr has no crash-aware composition at the tag: 4e2a796f and 31dbcaa4 are not ancestors.

**§3 Restore row:** the premise is `restore_cert_ok`. `RestoreDiskState` passes the store's d. That `old` is the previous run's log is a lifecycle fact, and is trusted.

### A5. Proof obligations, in order

1. **Patch 0004** (A6), then rerun `verify_spec.sh`. All of the current 1916 must still pass.
2. **`certs_ok` in `log_ok`**, at the six sites listed in A2.
3. **The note side:** `g_seq_`, the spec accessors and ensures in output.rs, and the note-exactness ensures (P2).
4. **The Ready push.** A Mako `lemma_cert_now_ok` (glr's is in the compiled-out port, ports/raftrs/coupling.rs:2524) derives `cert_ok` from `ginv`. Then `lemma_push_mark` (ghost_log.rs:900), `lemma_certs_push_ready`, `ready_seq_ok` and P1.
5. **P3**, `lemma_crash_cut`.
6. **P4**, the Restore premise and arm.
7. **Docs.**

### A6. Spec fit

Patch 0004 adds `impl CertProtocol for RaftP` under protocol/Raft:
- `Durable = (u64,u64,u64)` and `Ctx = ()`;
- `cert_local_ok` is `wf`, `routed`, equal term and commit, `has_voted == (v != 0)`, and `voted_for == if v == 0 {0} else {v-1}`;
- `lemma_cert_local_empty` comes from `lemma_routed_empty`.

**Why a patch is needed.** Mako cannot write the instance itself: glr is a separate crate (`--extern glr`, verify_core.sh:40-47), so the orphan rule forbids it (not compiled).

**Abstract and correct.** The vote encoding is forced by `LMark::Ready`'s u64 fields (ghost_log.rs:168-171). The patch changes no protocol rule.

**Conflict with the port.** It conflicts with the raft-rs instance (ports/raftrs/certs.rs:35-63, which has `Durable = HardState` and `Ctx = Set<u64>`), but only when the ports compile.

**Marks are passive:**
- `boundaries` and `group_trigger` skip them (ghost_log_core.rs:664-700);
- the network only grows (ghost_log_compose.rs:398);
- Mako's coupling never reads `boundaries`.

### A7. Tests and replay

- **Unchanged and still valid:** the core's exec code apart from `Restore.d`, the cargo tests, raftLabTest and the recordings (with `d` added to the parser).
- **raft_kill check (9)** already asserts `d >= tail` for every revealed output (check.py:175-207). That is D's test, and no new logging is needed. Add only the check that the recovered state equals the fold of the logged P notes.
- **Verification bookkeeping:**
  - update `verified_functions.txt`; `core_trusted.txt` must not grow;
  - verify_core.sh has no count to update;
  - regenerate `SPEC_VERSION.toml`.
- **Memory builds:** one verification run covers both builds, because the core has no `raft_disk` cfg.

### A8. Risks

- **Solver cost.** `certs_ok` inside `log_ok` could slow the solver. The fallback is a separate conjunct proved once per step.
- **Outputs D does not cover:**
  - the leader's apply waits on `durable.last` (server_h.rs:3036-3050);
  - leader-change callbacks fire before the waits (server_h.rs:1783-1806);
  - outside the gate, the InstallSnapshot send happens before the wait (server_cc.rs:126-150).

## Part B: Snapshot coupling (corrected)

### B1. Mapping

| Code path | Ghost side |
|---|---|
| CreateSnapshotLocked (server_h.rs:3173-3256); CompactLog/CompactLogLocked (:1489-1527) | E1/E2: a stutter; `g_prefix_` grows |
| TickHeartbeat `SnapshotSend` (heartbeat.rs:992-1009) | in the tick group: `Send(f, Snapshot{T, snapidx_, snapterm_, log_view.take(snapidx_)})`, then `Close(SendSnapshot)`. A failed or suppressed wire send counts as lost. |
| refusals before contact (server_h.rs:2607-2675) | nothing (a dropped packet) |
| stale term, reply with own term (:2616-2624) | `Recv`, then `Close(ShareCommit{0,0})` sending `CommitInfo{T_f,0,0}`. ShareCommit fits a Recv trigger (ghost_log.rs:413). |
| higher term (:2691); same-term candidate (:2717-2718) | `step_down_seg` in the Recv group; or `step_aside_log`, a Tick group before the Recv |
| conflict / no successor / no manager / payload failure (:2743-2826) | nothing more; a reply of 0 means no reply (server.cc:1253) |
| covered acknowledgement (:2756-2764) | `Recv`, decline, reply `AppendResponse{T, true, index}` (needs W1) |
| install, matching boundary / not matching (:2828-2869) | in E5's own group: `Recv(src,m)` again (the network keeps packets), then `Set(CommitIndex)` [plus `Set(Log, prefix)` when not matching], then `Close(InstallSnapshot)` |
| reply handling (:2526-2566) | higher term: `Recv(f,m)` + `step_down_seg`; accepted: `har_log` when snap_idx is above the spec's match, otherwise nothing |
| startup recovery (:2446-2466) + RestoreDiskState | E7 |

### B2. Ghost state and invariant

- **Ghost field `g_prefix_`.** `log_view() = g_prefix_ ++ live.map(entry_view)`.
- **`ginv` replaces `snapterm_ == 0` (coupling.rs:309)** with:
  - `|g_prefix_| == base-1`;
  - `snapidx_ == 0 ⇒ snapterm_ == 0`;
  - `snapidx_ > 0 ⇒ log_view[snapidx_-1].term == snapterm_`;
  - every `g_prefix_` entry is NoChange with term ≥ 0;
  - `execute_index_ <= commit_index_`.
- **`inv_config` drops the gated clause** (node.rs:172-176) and keeps node.rs:148-150.
- **`restore_prev_ok` generalized:**
  - `s.log.len() == snap + |entries|`;
  - `s.log.skip(snap) == restore_log(rev)`;
  - the boundary term matches;
  - `commit >= snap`.
- **Install ticket.** `g_install_: Ghost<Option<(int, LRaftMessage)>>` is set by E4 and cleared at the start of every other event. E5's facts come from E5's premise, not from a `ginv` conjunct, so no other handler proof changes.

### B3. Events and restructuring

- **E1 `CreateSnapshot{index, term}`.**
  - The choice moves into a pure query `snapshot_point()` (server_h.rs:3182-3215).
  - The shell saves the image, then steps E1.
  - E1 writes `snapidx_`/`snapterm_` and compacts through `min(index, commit, snapidx)`.
  - The note carries `SnapRef{keep:true}`.
- **E2 `Compact{up_to}`** takes CompactLogLocked's `compact_through`.
- **E3.** `SnapshotSend` gains `index_ = snapidx_` and `term_ = snapterm_`.
- **The send kernel** takes `(index, term)` and refuses unless `latest()` is that image:
  - the store changes with `snapidx_` under `mtx_` (server_h.rs:3220-3233, server.cc:883-913), and fail-stops on code 2;
  - the image Arc and the reply context are resolved under `mtx_`;
  - the send runs after the tick's `wait_durable`, and the wait's filter (server_cc.rs:162) must also count snapshot sends;
  - the inline no-peer delivery then runs without `mtx_`, which is harmless because `follower_term` is 0 there (server.cc:1229-1254).
- **E4 `RecvInstallSnapshot{…, applied, Ghost(prefix)}`** owns:
  - the admission checks, the stale reply and the term raise;
  - the leader hint;
  - StepDown/SetFollower and the two flag writes, which `run_locked_actions` does not read (server_h.rs:1733-1773);
  - the conflict, covered and successor decisions, and `retain_suffix`.

  It returns `Refuse | Ack(term) | Install`, and closes every group it opens.
- **E5 `InstallSnapshot`,** stepped right after the payload kernel returns 3, in the same `mtx_` hold.
  - It writes `snapidx_`, `snapterm_`, the log (compact or reset) and `commit_index_`.
  - The apply-queue purge, which reads none of these, follows it.
  - Its note carries the hard state and `SnapRef{keep: retain}`.
- **E6 `RecvSnapshotReply{…, Ghost(m)}`** replaces `ObserveTerm` and `accept_through` (server_h.rs:2526-2566).
- **E7 `Restore` gains `snap_index`, `snap_term`.** The core writes of startup recovery move into it, including the term raise (:2464-2466). This reorders them: today they run before `LoadCurrentConfig` and `publish_mirrors` (server_h.rs:2023-2039), and Configure's coupling requires `state_view == init_state` and `snapterm_ == 0`. Nothing observes the reordering before RPC admission.
- **`PersistNote.snap` (required).** `record_from_note` writes the image first. This replaces the hand pushes, which would break N under the gate.
- **The gate.** `enter_gates` drops both `!snapshots_enabled` and `whole_log` (node.rs:316-318). The shell drops the `verified_gates_` returns (server_h.rs:1522, 3378). `VERIFIED_GATES=1` together with `SNAPSHOTS=1` then no longer fails closed (server_h.rs:2062-2069).
- **`ObserveTerm` is deleted:** from the core, the replay crate (replay/src/lib.rs:269, 548, 632, 772) and the tests (replay/tests/persist_lines.rs:52, core/tests/observe_term.rs).
- **Record granularity.** Merging today's ObserveTerm and StepDown steps into E4 turns two WAL records into one. The state is unchanged.

The ghost fields of E4 and E6 are the first ghost fields on `Event` (event.rs:24ff). They are `#[cfg(verus_keep_ghost)]`. The shell and the replay crate omit them, and the host contract states their value.

### B4. Premises

| Event | Premise | Host side |
|---|---|---|
| E1 | `index == snapshot_point()`, nothing stepped in between | apply gate and `mtx_` held across query, save and E1 |
| E3 | the gate | — |
| E4 | the gate; the prefix has `len == index >= 1`, last term `snap_term`, NoChange; values below the limit; `applied <= commit_index_` | the prefix is the sender's Send (§1 item 1). NoChange follows from `entry_view` (coupling.rs:239-245) and the `g_prefix_` conjunct. |
| E5 | `g_install_ == Some((src,m))`; `m.term == current_term_`; Follower; `index > commit_index_` | E4 returned Install in the same hold; the payload kernel changes no core state |
| E6 | `msg_term(m) == follower_term`; `m.match == snap_idx` when `m` is an AppendResponse | reply context (server.cc:1208-1222); the follower picks `m = index` |
| E7 | the generalized `restore_prev_ok` (with A4's `restore_cert_ok`) | A4 |
| Applied | `index <= commit_index_` | the apply thread applies committed entries |

**Becomes proved:** creation, compaction, send, install and reply; the ObserveTerm row disappears.

**Stays trusted:**
- the image is the state machine after the prefix;
- the image is written before its record (images.rs:48-54);
- the WAL.

### B5. Proof obligations

1. W1 on mako-spec, the group's proof rerun, then pin v3.
2. A stronger ensures on `compact_through` (log.rs:588-593 has only `wf`).
3. Lemmas:
   - `prefix_config` on NoChange entries is `(range(0,n), ∅)`, and `last_conf_index` is 0;
   - the compactions keep the view;
   - the snapterm lemma.
4. Generalize the base-1 proofs:
   - the follower's prev check (node.rs:1978-2002); `compacted_prefix_miss` is an S1 refusal;
   - the leader's prev term (heartbeat.rs:1012-1032);
   - `election_last_log_term` (node.rs:672-690);
   - commit advance, Propose, Restore.
5. E1/E2 stutter.
6. E3 `lemma_send_snapshot`, and `tick_sends_log` (coupling.rs:2584).
7. E4 per outcome.
8. E5 (ii) and (iii).
9. E6 through `lemma_har_ginv` and `lemma_reply_stutter` (:2166, :2247). `accept_through` takes a max (progress.rs:375-389).
10. E7.
11. EnterGates.

### B6. Spec fit

LSendSnapshot fits as it is. LInstallSnapshot fits except for its reply: it fixes `match_index = s_.commit_index` (raft.rs:415-416), and HandleAppendResponse binds `new_match == resp_match <= log.len()` (ghost_log.rs:270-276; raft.rs:496-503).

**W1:** the reply may carry `m` with `m == s_.commit_index || (m == index && index <= s_.commit_index)`, and `success = m > 0`.
- **Abstract:** this is the Raft paper's term-only reply.
- **Correct (argued):** `agrees_or_conflicted`, `VotesAgree` and `AllVotesAgree` are all downward closed in the match (log_invariants.rs:110-117, 470-528).
- **Known repair:** `lemma_responses_agree_new_success_install` asserts `mi == s_.commit_index` (log_induction_agree.rs:381-440) and needs a `lemma_prefix_eq_shrink` step.

### B7. Tests and replay

- New `core/tests/snapshot_events.rs`.
- `persist_note.rs` and the replay Shadow gain SnapRef.
- Record E1-E7 and remove the five taints (server_h.rs:1508, 2445, 2548, 2606, 3231).
- Replay the lab snapshot cases under the gate.
- RaftLabTest with snapshots on.
- raft_kill with snapshots on.
- The perf points `stall_*` and `s286k_r190_p6`.

### B8. Risks

- **Proof churn.** Step 4 touches most handler proofs; a base-1 fast path helps.
- **Ghost-only packets.** The ShareCommit model adds CommitInfo packets that exist only in the ghost log.
- **Latency.** Catch-up costs one more fsync.
- **Memory builds.** Whether a memory build ever finds a snapshot at startup is unverified.
- **Embedder effect.** `PublishAppliedIndexLocked` publishes before the record is durable. That is an embedder effect, not a message.

## Part C: Step-1b list, in order

1. glr patch 0004 (CertProtocol instance); rerun the spec proof; update SPEC_VERSION and spec-changes.md.
2. Core ghost:
   - `g_seq_`;
   - `certs_ok` in `log_ok` (six sites);
   - `ready_seq_ok`;
   - the output.rs ensures;
   - the Ready push and P1/P2 in `step`;
   - `crash_cert`, `restore_prev`, `restore_cert_ok`, `lemma_crash_cut`;
   - the Restore arm.
3. `Event::Restore.d` (exec field, unused at run time), passed by `RestoreDiskState` and parsed by the replay crate.
4. Optional: the note number plus a `debug_assert` in `disk_note`.
5. The gate gap: an entry refusal of InstallSnapshot when gated, or the host premise.
6. Docs (host-contract §1 item 5, §1 item 3, the §3 Restore row; disk-persistence.md §6), check.py's fold check, `verified_functions.txt`.
7. glr W1 patch, rerun, pin v3.
8. The `compact_through` ensures.
9. `snapshot_point()`, E1, E2; CreateSnapshotLocked and CompactLogLocked call them.
10. `PersistNote.snap`; `record_from_note` writes the image and the SnapRef; delete the hand pushes (server_h.rs:2876-2893, 3237-3248).
11. E3; the kernel takes `(index, term)` and checks `latest()`; split it into resolve and send; send after the wait, with the wait's filter including snapshots.
12. E4 and E5; rewrite `OnInstallSnapshotLocked` around them.
13. E6; rewrite `InstallSnapshotReplyAcceptedLocked`.
14. E7, with the startup recovery's core writes moved into it.
15. The Applied premise (or a guard).
16. Delete `ObserveTerm` (core, replay, tests).
17. `enter_gates` and `inv_config`; drop the `verified_gates_` returns.
18. Replay events; remove the taints.
19. Docs (host-contract §3, coupling-table, spec-changes v3).

## Decisions for the owner

- Patch 0004 (impl CertProtocol for RaftP in protocol/Raft, export-only like 0003) or re-proving about 12 certificate lemmas inside Mako. Also, whether to ask upstream glr to move the raft-rs port onto one shared instance, since the two instances conflict when the ports compile.
- How Restore learns d: an exec field the core ignores (recommended; the shell has r.d), or a #[cfg(verus_keep_ghost)] ghost field. Either one replaces the existential. Separately: whether to add an exec number to PersistNote with a debug_assert, so that premise N is checked rather than trusted.
- The InstallSnapshot hole under today's snapshot-off gate: refuse the RPC at entry when gated_ (this changes behaviour only for a misconfigured peer), or add a host-contract premise that no member enables snapshots.
- Incarnation join: keep it on paper (recommended), or ask glr for a crash-aware composition theorem (theorem_compose_env cannot express rollback and is not at the tag).
- Store fold: keep 'records 1..d fold to the recovered state' on paper, or add a ghost note sequence and a fold lemma so that only raft-store's write and recover stay trusted.
- Where certs_ok lives: inside log_ok (glr precedent; touches 6 proofs) or as a separate ginv conjunct, if solver cost requires it.
- The leader's apply waits on durable.last, which rests on an unchecked 'a leader never cuts its log' argument. Leave it outside D (it is not a message), or switch it to a record-number wait (a timing change).
- Spec W1 (the install reply may report the snapshot index when it is at or below the commit index), or a trusted cross-node premise 'follower commit <= leader log length'.
- Move the InstallSnapshot send after the tick's durability wait. This is a timing change, needed by the durability rule once snapshots are gated, and costs one fsync of catch-up latency.
- RecvSnapshotReply: add checks that follower_term == send_term and that the server is leader (they never fire for an honest follower), or keep them as premises. Also: F9-style admission (snap_idx > last means no reply), or a premise that the leader's log never shrinks.
- Applied: a premise index <= commit_index_, or a refusing guard that never fires.
- The E4-to-E5 ticket: a ghost ticket plus a call-order premise (recommended, cleared by other events), or an exec ticket with a runtime_assert.
- A memory build that finds a snapshot at startup under the gate: fail closed, or rely on host contract item 5 (no in-place restart).
- The image is the core's snapshot: a kernel check of (index, term) against latest(), or a trusted host-contract fact.
- Ordering of the two efforts: land the persistence coupling with snapshots still off, then the snapshot gate. Until the second lands, MAKO_RAFT_VERIFIED_GATES=1 with snapshots on keeps failing closed.

## Corrections the review made to the drafts

- [persist P3] The precondition 'ready_seq_ok(old,s) and d <= s' does not make the cut's number exactly d: it also needs d >= the first Ready number. Counterexample: an incarnation restored at d0=5 has no mark numbered 3. durable_cert takes the last covered Ready mark (glr ghost_log_certs.rs:105-124), and a log can begin at a number above d.
- [persist P4] restore_cert_ok cannot be satisfied when the previous incarnation crashed before its first Ready mark (e.g. after its store was created but before EnterGates). Restore is stepped for every store that was not freshly created (shell/disk.rs:375; server_h.rs:2179-2196). Then crash_cert = Cert::empty(), whose replay has config ∅, which fails restore_prev_ok's s.config == range(0,n) (coupling.rs:506). Fix: when durable_pos < 0, fall back to prev = load_config_log(n). This is sound because no Send precedes the first Ready mark.
- [persist §3/open decision] With d existential in Restore's premise, g_seq_ = d is an unspecified witness, so premise N cannot tie Ready numbers to WAL numbers. d must be an argument. The shell already has it as r.d (server_h.rs:2181, 2214), so an exec field on Event::Restore that the core ignores is enough. This is a required change, not an optional one.
- [persist P1] log_grows(old.g_log, last_cert.log) is false for Restore, whose arm replaces g_log_ (event.rs:592). Restore must be excluded from that clause.
- [persist step 2] 'which lemmas establish log_ok without the primitives is unverified' is checkable. Only lemma_new_ginv (coupling.rs:363-364) and lemma_load_config (:417-418) call glr's base lemmas outside the four primitives (:68, 92, 117, 157). lemma_step_aside uses the primitives (:565-578). The 27 g_log_ assignment sites are confirmed: 20 in node.rs, 4 in heartbeat.rs, 3 in event.rs.
- [persist risk] 'log.rs view specs across truncation: existence unverified, largest piece of work' is wrong. append (log.rs:322-331), truncate_from (:460-472), reset (:656-665) and get (:274-278) all have view-level ensures, and node.rs:2211-2214 already asserts the tail after truncation. Only compact_through lacks one (:588-593 ensures wf only), and that matters only for snapshots.
- [persist §7] check.py check (9) already asserts d >= tail for every revealed output (scripts/raft_kill/check.py:175-207). The reveal lines already carry the tail (server_cc.rs:166-171; rt/src/service.rs:110-114), and the tail bounds the Ready number from above. So D is already tested, and no Ready number is needed in the reveal lines.
- [persist §7] verify_core.sh has no expected count to update. The gate is verus_gate.py, checking verified_functions.txt and core_trusted.txt (verify_core.sh:6-12, 53-55).
- [persist N] 'every step goes through the wrappers: unverified beyond server_h.rs:1586-1643' can be checked. core().step and core().step_checked are called only at server_h.rs:1587, 1598, 1639 and 1651. Saved fields are also written directly on the snapshot paths (server_h.rs:2446-2466, 2828-2869, 3232-3233). Under the gate these are excluded only by: the fail-closed check at OpenDiskStore (:2147-2154), no snapshot manager (:2273-2291), and the verified_gates_ returns (:1522, :3378).
- [persist §4 'other events unchanged'] Under the current gate, an InstallSnapshot from an authoritative peer is not refused at entry. It steps the uncoupled ObserveTerm (:2691; coupled=false at coupling.rs:2695) and writes current_leader_id_, req_voting_ and election_in_progress_ outside step (:2697, 2717-2718). host-contract.md §3 (ObserveTerm row) relies on no member enabling snapshots, and that premise is unstated.
- [persist §5 step 4] lemma_cert_now_ok exists only in the compiled-out raft-rs port (glr src/ports/raftrs/coupling.rs:2524). Mako must write its own, which is easy: cert_ok follows from ginv's log_ok, fully_closed and replay == state_view.
- [persist mapping] The first record number comes from disk.rs:345 (RecordQueue::new(d+1)), not :347.
- [snap §3] E4 cannot leave the Recv group open across the payload kernel: ginv requires fully_closed at every step boundary (coupling.rs:306-308). E5 must open its own Recv(src,m) group. That stays causal because the distributed model's network only grows (glr ghost_log_compose.rs:398). The design leaves this two-group shape implicit.
- [snap §2] A g_install_ conjunct in ginv must survive every handler. It is cheaper to clear g_install_ at the start of every other event and put E5's facts in E5's premise. Nothing else steps between E4 and E5 in one mtx_ hold (server_h.rs:2806-2869).
- [snap open decision 9] The persist-note extension with the snapshot is required, not optional, once snapshots are gated and the persistence premise N holds. The hand-pushed SnapRef records (server_h.rs:2882, 3243) would otherwise be queued records that come from no note.
- [snap E7] Startup snapshot recovery runs before LoadCurrentConfig and publish_mirrors (server_h.rs:2023-2031 vs 2039). Configure's coupling requires pre.ginv(): state_view == init_state and snapterm_ == 0 (coupling.rs:306-317, 390-404). Moving the writes into Restore therefore reorders them after Configure and after the mirror publish; this must be listed as a reorder. The term raise (:2464-2466) must also be merged with Restore's term < current_term_ refusal (node.rs:592-594).
- [snap timing change] The tick's wait is skipped when tick.sends_ is empty (server_cc.rs:162), so the condition must also count snapshot sends. After the move, the inline no-peer delivery runs outside mtx_ (server.cc:1229-1254); this is harmless because follower_term is 0 on that path. A declined tick returns before any SnapshotSend is pushed (heartbeat.rs:849-855), so it never carries one.
- [snap §3] Deleting Event::ObserveTerm also touches the replay crate (replay/src/lib.rs:269, 548, 632, 772), replay/tests/persist_lines.rs:52 and core/tests/observe_term.rs. These are missing from the restructuring list.
- [snap §3] enter_gates must also drop the whole_log requirement (node.rs:316-318), not just accept snapshots_enabled. Dropping the verified_gates_ returns changes behaviour: today MAKO_RAFT_VERIFIED_GATES=1 with snapshots on fails closed (server_h.rs:2062-2069).
- [snap §3] Merging today's ObserveTerm step and StepDown/SetFollower step into E4 merges their two WAL records into one, because disk_note runs once per step (server_h.rs:1588, 1603). The state is unchanged, but record granularity and the recordings change.
- [snap §3] No Event variant has ghost fields today (event.rs:24ff). Ghost(prefix) and Ghost(m) would be the first, declared #[cfg(verus_keep_ghost)] like RaftCore's (node.rs:109-116). Cargo-built callers (the shell and replay) omit them.
- [snap risk/decision on NoChange] The prefix's NoChange follows from genuine packets (§1 item 1), because every Mako Send prefix is a log_view whose entry_view always sets change = NoChange (coupling.rs:239-245), plus the new g_prefix_ conjunct. It does not need 'no node closes ProposeConfChange'.
- [snap §2] cluster_c's init_config is its cfg0 parameter (ghost_log_compose.rs:55-57). It is range(0,n) because Mako's c_view passes that (coupling.rs:258-260); it is not a fact of cluster_c itself.
- [snap W1] The proof site is known. lemma_responses_agree_new_success_install asserts mi == s_.commit_index (glr log_induction_agree.rs:381-440) and needs a lemma_prefix_eq_shrink step. VotesAgree and AllVotesAgree (log_invariants.rs:495-528) are also downward closed, which supports the argument.

## Not verified

- Orphan rule: reasoned, not compiled. glr is linked as a separate crate (verify_core.sh:40-47 --extern glr), so impl CertProtocol for RaftP in Mako should be E0117.
- W1's correctness and its proof cost until the group's proof (1916 obligations) is rerun. Only the one assertion site at log_induction_agree.rs:381-440 was located.
- Solver cost and trigger behaviour of the opaque certs_ok inside log_ok, and of a Ready mark pushed on every gated step.
- That Ready marks at step ends disturb no Mako lemma. Checked only that coupling.rs never reads boundaries(); nothing was run.
- That every SnapshotStore::save happens under mtx_ together with snapidx_. Checked for the create path (server_h.rs:3220-3233) and the install payload (server.cc:868-913), not for every startup-recovery path.
- Whether a memory build's startup snapshot recovery is ever reached outside in-process lab restarts.
- Whether the authority check refuses an InstallSnapshot to a same-term leader (raft_server_leader_rpc_sender_is_authoritative was not read).
- That the kill-test recordings carry P lines for every incarnation in a form check.py can fold against the recovered state.
- Whether publish_mirrors at server_h.rs:2028-2031 has any observer before RestoreDiskState, which matters for the E7 reorder (rpc_ready_ is set later, at :2076).
- Effort estimates (for example 2-4 days for P2) were not assessed.
