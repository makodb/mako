# Coupling table: Mako's Raft core against the spec's actions

The path-by-path work list for Phase 8, the analogue of the group's
`docs/ghost-log/raftrs/coupling.md` §3 (glr = `/home/users/zyang2/ghost-log-refinement`).
Written at Phase 3 (plan §5), against the core calls that phase introduced,
for the verified configuration (snapshots off and a whole log, failover on, a
static configuration: F5's gate). Spec: v1 (`raft-spec-v1`, upstream
`d7e04ed7` + S1), the actions of glr `src/protocol/Raft/raft.rs`.

The criterion is the group's closure lens: every write to a **marked** field
and every message sent must be attributed to a spec action; a write that no
action owns is a hard blocker; a call that writes only unmarked fields is a
stutter. Paths below are `src/deptran/raft/`-relative.

## 0. Conclusion

Every core path inside the gate closes against an existing action, under the
plan's view choices V1-V3 (§4.3), one bundle convention for batches (BR1/BR2,
as the group's port uses), and the boundary conditions of §4. **V3's open point
is settled: the "no" count reaches no marked field** (§3.2). No new spec change
is needed beyond S1. Open work for Phase 8 is listed in §5.

## 1. State field correspondence

| Spec `LState` field | Mako (`RaftCore` unless stated) | View | Written by |
|---|---|---|---|
| `current_term` | `current_term_` | direct | `start_election`, a higher term seen in `raft_on_append_entries` / `do_vote` / `heartbeat_apply_append_reply` / `election_settle` |
| `role` | `is_leader_`, `election_in_progress_`, `election_term_` | Leader ⇔ `is_leader_`; Candidate ⇔ `!is_leader_ ∧ election_in_progress_ ∧ election_term_ == current_term_`; Follower otherwise | `set_is_leader`, `step_down`, `start_election`, `election_settle`, the inbound handlers |
| `has_voted` / `voted_for` | `vote_for_` | `vote_for_ != INVALID ⇔ has_voted`, `voted_for = vote_for_` | `start_election` (self), `do_vote`, every higher-term step |
| `log` | `raft_log_` | entry i ↦ `(term, value_view(cmd))`, `cmd` opaque (`Cmd`, M11); base 1 under the gate | `append_local`, `WireBatch::append_into`, `truncate_from` |
| `commit_index` | `commit_index_` | direct | `raft_commit_advance` (leader), `raft_on_append_entries` (follower) |
| `votes_granted` | the campaign's `VoteSet` (local to `election_settle`) | role-frozen ghost set, as glr's V1: while Candidate, the granted voters fed so far | `election_settle` |
| `match_index` | `peers_.match_index(ord)`, keyed by `peer_sites_[ord]` | leader-frozen view, as glr's V2 | `heartbeat_apply_append_reply` (`accept_through`) |
| `next_index` | `peers_.next_index(ord)` | ghost shadow, not coupled (no guard reads it), as glr's V3 | backoff, repairs in `heartbeat_tick` |
| `config` | `config_members_` | the static universe | Setup (`LoadCurrentConfig`), `LLoadConfig` at construction |
| `conf_index` | none | 0 (static configuration gate) | — |
| `pending_reads`, `served_ctxs` | none modelled | Mako's read-index authority rounds are outside certificate v1 | — |

Unmarked (private; writes are stutters): `execute_index_`, the election
timer (`last_heartbeat_time_`, `election_timeout_us_`,
`election_timer_generation_`), `req_voting_`, `current_leader_id_`,
`heartbeat_round_`, `read_quorum_confirmed_*`, the round state
(`round_`, `authority_rounds_`, `pending_leader_term_`, and
`pending_rpcs_` except as below), `snapidx_`/`snapterm_` (0 under the gate),
the snapshot configuration, and every `VoteSet` count other than the granted
voters.

## 2. Message views

| Mako wire message | Spec message | Notes |
|---|---|---|
| RequestVote `{cur_term, site_id, lst_log_idx, lst_log_term}` | `RequestVote{term, candidate, last_log_index, last_log_term}` | one broadcast, N wire copies (E1) |
| vote reply `{max_ballot, vote_granted}` from peer p | `VoteResponse{term: max_ballot, granted, voter: p}` | the voter is the shell's routing (the callback's peer, F1); refusals at or below the campaign term are unmodelled input (V3) |
| AppendEntries with one raw entry | `AppendEntries{term, leader, prev_index, prev_term, entry_term, value, has_entry: true, leader_commit}` | `leader_commit` = F3's commit read in the same call |
| AppendEntries with a batch of k entries | k components (BR1): component i has prev = `prev + i`, the i-th entry, the same term, leader and commit | each component is one `LSendAppendEntries` segment; the follower decomposes it (BR2) |
| EmptyAppendEntries (heartbeat) | `AppendEntries{has_entry: false, leader_commit: min(lc, prev)}` | V1 clamps the commit to prev |
| append reply `{ok, term, last_log_index}` | `AppendResponse{term, success: ok, match_index: ok ? last_log_index : 0, follower}` | on success `last_log_index` is `accepted_through`, the end the RPC proved |
| the unavailable sentinel (0/0/0) | none | the shell reads it as "no reply" (`response_available` false): unmodelled |
| InstallSnapshot | none | gated out (snapshots off) |

## 3. Path-by-path inventory

Notation: `[x]` a marked write, `(x)` an unmarked write, `→ M` a message sent,
`⇒ A` the segment closes action A.

### 3.1 Inbound RequestVote (`raft_on_request_vote`, `do_vote`)

| Branch | Effect | Attribution |
|---|---|---|
| stopped | reply `{term: current_term, granted: 0}` | `LRejectVote`, which has no guard: any refusal at the server's own term, state unchanged |
| malformed (`can_term < 0`, `lst_log_term < 0`) or not a voter | reply refusal | `LRejectVote`; a non-voter's request is outside the network model (boundary §4) |
| unavailable (`ServeVote`, before the core: disconnected or not RPC-ready) | the shell replies `{term: can_term, granted: 0}` without calling the core | no segment: a shell-made message, outside the ghost log; at the candidate it is V3's unmodelled input |
| `can_term < current_term` | reply refusal at own term | `LRejectVote` (stale term) |
| `can_term > current_term` (inside `do_vote`) | `[current_term := can_term] [voted_for := none]` (current_leader_id) then `set_is_leader(false)` / `step_down` | ⇒ `LStepDown(new_term)`, one segment before the vote decision |
| already voted for another in this term | reply refusal | `LRejectVote` (already voted) |
| log not up to date | reply refusal | `LRejectVote` (`candidate_log_ok` false) |
| grant | `[voted_for := can_id]`, reply `{term, granted: 1}`, RESET_ELECTION (unmarked) | ⇒ `LGrantVote` |

### 3.2 The campaign (`start_election`, `election_settle`)

| Branch | Effect | Attribution |
|---|---|---|
| `start_election` admitted | `[current_term += 1] [voted_for := self] [role := Candidate]` (election_in_progress_, election_term_, req_voting_, hint, RESET_ELECTION) → RequestVote | ⇒ `LTimeout` |
| refused admission (stopped, leader, campaign running, timer stale) | none, or `(req_voting_)` | stutter |
| `election_settle`: each granted reply at the campaign term | `[votes_granted ∪= {voter}]` | ⇒ `LReceiveVoteGranted` (Recv-opened) |
| each refusal at or below the campaign term | the `VoteSet`'s "no" count, local to the call | **unmarked** (V3): Tick-opened, no `Recv` |
| a reply with a higher term (ADVANCE_HIGHER_TERM) | `[current_term := term] [voted_for := none]`, `set_is_leader(false)`/`step_down` | ⇒ `LStepDown` (Recv-opened: a higher-term reply is always genuine) |
| yes quorum, still the campaign's term, not stopped | `[role := Leader]` (`set_is_leader(true)`: heartbeat round reset, peer table rebuilt, unmarked), APPEND_NOOP | ⇒ `LBecomeLeader`, then the no-op's append ⇒ `LClientRequest` |
| no quorum | `(election_in_progress_ := false)`, `set_is_leader(false)` (a no-op on a non-leader) | the role leaves Candidate ⇒ `LStepAside` (no guard) |
| timeout | the same as a no quorum | ⇒ `LStepAside` |
| stale result (IGNORE_STALE) | none | stutter |

**V3, settled.** A "no" reply reaches `VoteSet::no_` (a local count) and,
through `outcome.no_`, only the "no quorum" branch, whose sole marked effect is
the role leaving Candidate: `LStepAside`, which has no guard. Its term can
raise `VoteSet::highest_term_` only to the campaign term itself (the forged
unavailable reply carries the candidate's own term), and ADVANCE_HIGHER_TERM
needs a term above `current_term_`, so it never fires on a refusal at or below
the campaign term. No marked field depends on the "no" count; F2 is not needed.

### 3.3 Inbound AppendEntries (`raft_on_append_entries`)

| Branch | Effect | Attribution |
|---|---|---|
| stopped | reply refusal | `LRejectAppendEntries` (S1: any server may refuse) |
| not a voter, stale term, or unauthoritative sender | reply refusal, nothing written | `LRejectAppendEntries` (S1) |
| `leader_term > current_term` | `[current_term := leader_term] [voted_for := none]`, then `step_down`/`set_is_leader(false)` | ⇒ `LStepDown`, its own segment before the append's |
| any accepted-term contact | `(current_leader_id_, RESET_ELECTION)`; from Candidate, the role leaves Candidate | ⇒ `LStepAside` when the role changes, else unmarked |
| prev mismatch, index or decode failure (F4: entry term below 1) | reply refusal with the local tail as a hint | `LRejectAppendEntries` |
| refused committed conflict | reply refusal before any marked write | `LRejectAppendEntries` (V2: the existing guard admits it) |
| accept, entry-less | `[commit_index := max(commit, min(lc, prev))]` | ⇒ `LFollowerHeartbeat` (V1's view of the commit) |
| accept with k entries | truncate, append, `[commit_index := …]`, APPLY_RANGE (unmarked) | ⇒ k `LFollowerAppendEntries` segments (BR2), or `LFollowerStaleAppend` for components already present |

### 3.4 The heartbeat round (`heartbeat_tick`, `heartbeat_on_reply`, `heartbeat_round_end`)

| Branch | Effect | Attribution |
|---|---|---|
| tick declined (not leading) | abandon the round state (unmarked) | stutter |
| PHASE 0's commit advance | `[commit_index := m]` when the majority match is a current-term entry | ⇒ `LAdvanceCommitIndex` |
| one follower's AppendEntries | the slot's protocol half (unmarked), → AppendEntries (k components) | ⇒ k `LSendAppendEntries` segments, one per component |
| repairs of a wrapped or overlong `next_index` | `next_index` (ghost shadow, not coupled) | unmarked |
| InstallSnapshot | gated out | — |
| reply: higher term (STEP_DOWN) | `[current_term] [voted_for := none]`, `step_down` | ⇒ `LStepDown` |
| reply: refusal (BACKED_OFF) | `next_index` backoff | ⇒ `LHandleAppendReject` |
| reply: success (ACCEPTED) | `[match_index(f) := acknowledged]` | ⇒ `LHandleAppendResponse` |
| reply: stale term, contradictory, unknown follower, or unavailable | none | stutter |
| `heartbeat_round_end` | `[commit_index := m]` | ⇒ `LAdvanceCommitIndex`; read-index settlement unmarked |

### 3.5 Other core calls

| Call | Effect | Attribution |
|---|---|---|
| `append_local` from Start (propose), as leader | `[log ++ (current_term, cmd)]` | ⇒ `LClientRequest` |
| `raft_election_tick` | reads only | stutter |
| `reset_election_timer` | timer fields | unmarked |

## 4. Boundary conditions (the shell's obligations, declared at entry)

1. Inbound messages come from configured voters with genuine routing: a vote
   reply's voter is the peer its callback was created for (F1); an append's
   sender is checked against the configuration before any marked write.
2. Terms on the wire are non-negative; entry terms are at least 1 (F4).
3. The unavailable sentinel and refusals at or below the campaign term never
   open a `Recv` (V3).
4. The configuration is static and contains this server; snapshots are off
   (F5's gate).
5. Each core call runs under `mtx_` with no other access to the core between
   its start and its actions (Phase 4 enforces this through `with_core`).

## 5. Phase 8: done, and where it departed from this table

Every path above is proved in Phase 8 (`core/src/coupling.rs`; the Phase 8
report). Departures from the table:

1. **Backoff is a stutter, not `LHandleAppendReject`.** `next_index` is a
   ghost shadow (V3) no guard reads, so a refusal's backoff writes nothing
   the spec sees; the reply is not recorded. Likewise the stale, contradictory
   and removed-follower replies (§3.4).
2. **`LHandleAppendResponse` takes the reported match**, recorded only when
   it rises past the spec's (V2 as an inequality: the exec raises a match by
   `max()` to at most the reported index, so it never passes the spec's).
   F9's `step_checked` supplies `match <= the leader's log`.
3. **The votes of a won campaign are recorded at settlement**, one Recv group
   per granted reply (open point 2 of the earlier list), then BecomeLeader in
   a Tick group; refusals and a lost campaign's votes are not recorded (V3).
4. **A candidate refused for a committed conflict also steps aside**
   (`raft_on_append_entries` makes it a follower before refusing): an
   `LStepAside` segment after the `LRejectAppendEntries` one.
5. **The round end's advance needs leadership as a premise** (bugs-found B17):
   PHASE 3 does not check it.
6. **A heartbeat's commit view (V1)** is `min(commit, prev)` on both sides, so
   a heartbeat at prev 0 carries commit 0 and `heartbeat_commit_ok` holds
   trivially.

The host contract the certificate rests on is
[host-contract.md](host-contract.md).
