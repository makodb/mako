# Host contract: what the Raft certificate assumes

Plan Phase 8 ([modification-plan.md](modification-plan.md)): "instantiate
composition with our `msg_view` and **our** host contract". This is that
contract: every assumption the proof of the Raft core rests on that the
core itself cannot check, with who or what guarantees it today. Paths are
`src/deptran/raft/`-relative.

## 0. What is proved, and from what

Verus checks, over the core crate (`core/`) and the frozen spec v1
(`verus/spec/SPEC_VERSION.toml`), with `scripts/verus/verify_core.sh`:

- **The coupling invariant** (`RaftCore::ginv`, `core/src/coupling.rs`): the
  core's ghost log is a certificate of its history (every closed segment is
  one atomic step of the spec, every label bound to its trigger, every send
  routed: the group's `log_inv`, `wf`, `routed`), it is fully closed, and
  replaying it gives the core's state as the spec sees it (`state_view`),
  together with V2 (a leader's match index never exceeds the spec's) and
  the facts listed in §5.
- **Every step keeps it** (`RaftCore::step`, `step_checked`,
  `core/src/event.rs`): from a core with `ginv`, under the event's premise
  (`RaftCore::coupled`, §3), the core after the call has `ginv`. A fresh
  core (`RaftCore::new`) has it (`lemma_new_ginv`).
- **The per-node certificate** (`lemma_node_cert`): such a core's ghost log
  is the group's `node_cert` for its n-member cluster and its rank.
- **The cluster theorem** (`theorem_mako_safety`): n such cores, under any
  schedule of their segments that is causal (§1), satisfy the distributed
  model's `RaftSafetyInvariant` after every step: at most one leader per
  term, matching logs, committed entries never lost or changed. Liveness is
  not proved.

Everything below is what "under the event's premise" and "causal" ask of the
shell, the transport and the deployment.

## 1. The method's assumptions (the composition's `causal`)

As in the plan's §1.3, from the group's composition
(glr `ghost_log_compose.rs`, `causal`; `docs/ghost-log/raftrs/composition.md` §3):

1. **Genuine packets.** Every message the shell hands a core equals, under
   the message views of §2, a message some verified core's ghost log
   recorded as sent to it. Each component of a batched append counts on
   its own (BR1/BR2).
2. **Truthful sender and routing.** The sender id is the real sender, and a
   reply is attributed to the RPC (and so the follower or voter) it answers:
   the vote tally's voter is the peer its callback was created for (F1); an
   append reply completes its follower's own slot.
3. **A static configuration shared by all servers**, containing each of them
   (the gate, F5).
4. **Segments are atomic and globally ordered.** Each core call runs to
   completion under `mtx_` (decision Q1; `RaftLockGuard` around every
   `step` but the three made before anything else can reach the server:
   `SetIdentity`, `Configure` and the lab constructor's `SetFollower`,
   [code-structure.md](code-structure.md) §3.2), so a server's segments
   happen in its ghost log's order, and a send is made after the call that
   recorded it returns (the shell sends from the call's output). The
   schedule `sched` is the real interleaving of the servers' calls.
5. **Per-node trusted base**: the transport, the codec, the kernels, storage
   (none persisted: no replica restarts in place under its old id, plan
   §4.4.2).
6. **Nothing else about the network**: loss, duplication, reordering and
   delay are allowed; a message the core refuses counts as dropped.

## 2. The message views

What a wire message is to the spec, on both sides. Causality needs the two
sides to agree: the receiver records exactly the message the sender
recorded. The views are spec functions in `core/src/coupling.rs`; "rank" is a
site's index in the sorted configuration, the spec's server id.

| Message | Sender records (`Send`) | Receiver records (`Recv`) | What the wire must carry |
|---|---|---|---|
| RequestVote | `campaign_msg`: the campaign's term, the candidate's rank, its log's last index and last term (`start_election`, one broadcast) | `request_vote_msg`: `can_term`, `rank(can_id)`, `lst_log_idx`, `lst_log_term` (`raft_on_request_vote`) | `CampaignStart`'s term, `site_id_`, `lst_idx_`, `lst_term_`, unchanged |
| vote reply | the voter's GrantVote / RejectVote answer: its term, the decision, its rank (`vote_group`) | `vote_reply_view(voter, granted, reply_term)` (`election_settle`) | the reply term and decision; the voter is the callback's peer (F1) |
| AppendEntries | one component per entry (`send_msg`): the leader's entry `prev + i + 1` behind its predecessor, the leader's term and rank, the commit read in the same call (F3); without entries, prev and its term with commit `min(commit, prev)` (V1) (`heartbeat_tick`) | the same, per component (`comp_msg`, `empty_msg`) from the RPC's fields and the payload's entries (`raft_on_append_entries`) | `AppendSend`'s term, site, prev, prev term and commit, and the entries' terms and commands (`InboundBatch::spec_entries` is the leader's entries; a command's value view survives the codec and clones) |
| append reply | the follower's answer per component: success with match `prev + i + 1`, or a refusal at its term with match 0 (`ae_log`); only the last component's answer goes on the wire | `reply_msg(follower, status, term, last)`: match = `last` on success, 0 on refusal (`heartbeat_on_reply`) | `ok`, the term, and on success `accepted_through = prev + count`; the follower is the slot's |

**Never recorded as received** (unmodelled input, so not part of
causality): the unavailable answers (`ServeVote`'s "no" at the candidate's
term, `ServeAppendEntries`' 0/0/0), F9's drops (`step_checked`), vote
refusals at or below the campaign term (V3: the tally reads them, but no
marked field depends on them), and append replies that are stale, from a
removed follower, contradictory, or refusals (backoff moves only the next
shadow, V3).

## 3. Per-event premises

`step` requires `admits(ev)` (the Phase 6 host contract, panic freedom) and,
for the coupling, `coupled(ev)`. `step_checked` discharges the message
admission itself (`coupled_checked`).

| Event | Premise | Who guarantees it |
|---|---|---|
| `SetIdentity` | the core is fresh (empty ghost log): the identity before anything else; the configuration still empty | the worker sets the identity before Setup |
| `Configure` | the core is fresh; the membership once, sorted, duplicate-free (`admits`); no member is the sentinel site 65535 | Setup's `LoadCurrentConfig`; the config kernel lists real site ids |
| `EnterGates`, `RebuildPeers`, `AbandonRound`, `ResetRoundState`, `ResetElectionTimer`, `Applied`, `SetFollower`, `StepDown` | none beyond `admits` | -- |
| `Propose` | the core leads; the command has a value | `Start` returns REJECTED unless `IsLeaderLocked()` under `mtx_` (`src/server_h.rs:3884`); the no-op is `APPEND_NOOP`'s action after becoming leader (`:1543`, `:3498`); every caller proposes a non-empty command (bugs-found B16) |
| `StartElection` | the gate | F5's `enter_gates` |
| `SettleElection` | the gate; failover on; `n_total` is the configuration's size; every reply from another member; a grant is at the campaign's term | `RequestVoteImpl` (`src/server_h.rs:3155`): `n_total` is the lane's quorum size; the replies are this campaign's quorum object's (F1: the callback's peer); a voter grants only at the request's term |
| `RecvRequestVote` | the gate; the candidate another member | `step_checked`'s admission (F9) |
| `RecvAppendEntries` | the gate; the sender another server | `step_checked`'s admission (F9) |
| `TickHeartbeat` | the gate; `is_leader` is the core's role (`admits`) | `heartbeat_tick_body` reads `IsLeaderLocked()` under the same lock (`src/server_cc.rs:85`) |
| `RecvAppendReply` | the gate; `is_leader` is the core's role; a success reports no more than the leader's log | the collection loop reads `IsLeaderLocked()` under the lock (`src/server_cc.rs:246`), which is the core's role only while `looping_` holds: **not once shutdown has begun, bugs-found B18** (§6); `step_checked` reads a success beyond the log as no reply (F9) |
| `RoundEnd` | the gate; `is_leader` is the core's role, **and the server leads** | the driver checks the mirror before taking the lock and reads `IsLeaderLocked()` under it (`src/server_cc.rs:310`, `:316`); in production nothing steps the core down in between (inference, bugs-found B17), but nothing checks it: **violable in lab builds (B17) and once shutdown has begun (B18)** (§6) |

Terms and indices off the wire and in the log stay below 2^62
(`raft_index_limit`, the Phase 6 contract): a log that long would hold
4.6e18 entries.

## 4. The inbound payload (`InboundBatch`, the shell's `WireBatch`)

The trait's contract (`core/src/node.rs`), trusted because the shell
implements it (`src/server_h.rs:3307`):

- `spec_entries()` is the payload's entries as the leader encoded them;
  `has_cmd` is the flag the batch was built from; no payload is no entries.
- `decode_terms`: an accepted decode reads exactly the entries' terms, each
  at least 1 (F4), and ends below the index ceiling; every entry has a value.
- `entry_at(k)` is entry k.

## 5. Facts the coupling carries (proved, from the premises above)

Kept by every step, so not assumptions, but they explain the premises:
`snapterm_ == 0` (snapshots are off under the gate, and only they write
it); no configured site is the sentinel; a leader or candidate voted for
itself; a candidate holds its own vote; every log entry has a value and a
term at least 0; V2.

## 6. Known gaps

- **B17 (safety, latent race; recorded, not fixed: the user's decision,
  2026-10-04).** PHASE 3 advances the commit index without
  checking that the server still leads, and `heartbeat_round_end_body`
  checks leadership before taking `mtx_`. If a step-down and a newer
  leader's appends ran in that window, the round end would count the old
  term's match indices against an entry of the new term. In production on
  this lane they cannot (inference): they run on the poll thread, which the
  driver holds or blocks from the check to the lock. Lab builds' direct
  handler calls (`src/lab.rs:520`, `:534`) can. The `RoundEnd` premise ("the
  server leads") is what excludes it, so a run that hits the race is outside
  the certificate. The fix would be one branch (advance only while
  leading); `core/tests/b17_round_end.rs` reproduces the bug at the core.
- **B18 (proof coverage, recorded).** `IsLeaderLocked()` is `looping_ &&
  core.is_leader_` (`src/server_h.rs:1236-1240`), and shutdown clears
  `looping_` (`PrepareForShutdown` under `mtx_`; `FailStop` without it). A
  leader's reply or round end stepped after that passes `is_leader = false`
  while the core still leads, which breaks the `RecvAppendReply` and
  `RoundEnd` premises. The core then does nothing wrong (the reply is
  ignored unless its higher term steps the core down; the round end commits
  as the leader the core still is and confirms no read authority), but the
  step is outside the certificate. Weakening the two premises (ghost only,
  with those two proofs redone) would cover it; not attempted.
- **B16 (liveness, latent; recorded, not fixed: the user's decision,
  2026-10-04).** An entry without a value is never replicated. No caller
  proposes one, so `Propose`'s premise holds today; nothing enforces it.

## 7. Trusted code

Inside the core, one function: `blocks_for` (`u64::div_ceil`, which vstd
does not specify; `scripts/verus/core_trusted.txt`). The value view of a
command, `value_view`, is uninterpreted: the proof needs only that every
server reads a command the same way (the codec's fidelity, §2).
