# Diff ledger

One row per changed hunk in `src/deptran/raft/{src,rt/src,core}/`,
`server.cc`, `raft_worker.cc`, `raft_main_helper.cc` (and, for Phase 0's
harness, `raft_bench.cc`), as [modification-plan.md](modification-plan.md)
§A.3 requires. Kinds are §A.3's M0-M12 (labelled "move, don't rewrite"
transformations) and §A.2's F1-F11 (numbered behaviour changes). Every
changed line carries `// [M<n>]` or `// [fix, F<n>]` on or just above it.
Line numbers are after the change. Paths are relative to
`src/deptran/raft/`.

## Phase 0 (all M0: env- or flag-gated instrumentation, inert when unset; no decision reads it)

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `fe9763f80` | `server.cc:6-8` | M0 | `<signal.h>`, `<time.h>`, `<unistd.h>` for the trace kit | builds, three lanes |
| | `server.cc:1620-1722` | M0 | trace kit kernels `raft_trace_at`, `raft_trace_through`, `raft_trace_now_us`, `raft_trace_enabled` behind `MAKO_RAFT_TRACE_FILE`: one cached flag when unset, no clock read | traced run reproduces the 12-stage breakdown (Phase 0 report); A/A G1/G2 on `verus-p0` |
| | `src/server_cc.rs:481`, `:666`, `:1233`, `:1243` | M0 | trace stages 8 (commit advanced), 3/4 (append sent) | as above |
| | `src/server_h.rs:1443`, `:3345`, `:3894` | M0 | trace stages 10 (apply pop), 9 (apply enqueue) | as above |
| | `rt/src/trace.rs` (new), `rt/src/lib.rs:7` | M0 | the Rust lane's hook slots, null unless installed; raft-rt's standalone cargo tests link no C++ | `cargo test: raft-rt` passes |
| | `rt/src/service.rs:120`, `:134-137` | M0 | trace stages 5/6 (follower accepted), through the hooks | traced run |
| | `rt/src/transport.rs:373-377` | M0 | trace stage 7 (leader got a success reply), through the hooks | traced run |
| | `raft_lane_rust.cc:25-33`, `transport_exports.h:31-34` | M0 | `Serve` installs the hooks only when tracing is on | traced run |
| | `raft_worker.cc:50-53`, `:797`, `:850`, `:864-867`, `:1011`, `:1243`; `raft_worker.h:98` | M0 | trace stages 0-2 (enqueue, submit, Start returned), 11 (Next) | traced run |
| | `server.cc:503-513` | M0 | `raft_lab_commit_log_enabled` (`MAKO_RAFT_LAB_COMMIT_LOG=1`), lab builds only (`RAFT_TEST_CORO`) | lab runs, three lanes |
| | `src/lab.rs:25`, `:49-50`, `:180`, `:189`, `:196`, `:200-268` | M0 | `LABLEADER` per waitOneLeader result; `LABCOMMIT` dump of the committed table per case (row-by-row copy, see bugs-found B10) | lab runs; comparator calibration |
| | `src/lab_cases.rs:31`, `:36` | M0 | dump at every case end (`passed`/`failed`) | as above |
| | `raft_bench.cc:126-172`, `:213-215`, `:262-263`, `:289`, `:316`, `:345`, `:370-374` | M0 | `--failover-out`: realtime stamps, record writer, option plumbing | G7 functional run; G7 A/A |
| | `raft_bench.cc:1423-1425`, `:1458-1464` | M0 | leadership callback stamps `new_leader_us`; apply callback stamps `first_commit_us` for a tagged seq-0 probe | as above |
| | `raft_bench.cc:1639-1690` | M0 | follower loop: arm, propose tagged probes once leader, write record, end markers | as above |
| | repo `examples/raft_bench.sh` (kill mode) | M0 | `leader_loss_us` stamp before SIGKILL, `--failover-out` plumbing, the run's JSON record | as above |

## Phase 1 (one `RaftCore`; F1, F3-F5)

The M2 rename is checked mechanically: `git show c5fa64ec5^:<f> | sed
's/\bstate_\b/core/g; s/RaftConsensusState/RaftCore/g'` differs from
`c5fa64ec5:<f>` only in the M1/M9 rows below (`<f>` = `src/server_h.rs`,
`src/server_cc.rs`).

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `c5fa64ec5` | `src/server_h.rs` (244 hunks), `src/server_cc.rs` (49), `src/lab_snapshot_cases.rs:1259-1260` | M2 | `RaftConsensusState` → `RaftCore`, `RaftServerBase::state_` → `core`, in code, comments and log text | the sed check above |
| | `src/server_h.rs:5-7` | M1 | imports for the moved authority ledger | builds |
| | `src/server_h.rs:920-1520` | M1, M9 | `PendingAppend`, `PendingTable`, `HeartbeatAuthority`, `AuthorityGeneration`, `AuthorityReply`, `AuthorityOutcome`, `AuthorityLedger`, `HeartbeatRoundScope` moved from `server_cc.rs` with bodies unchanged; their `rusty::BTreeSet<u16>` site sets become `SiteSet`, a sorted duplicate-free `Vec<u16>` (same membership and order; only insert, remove, contains, len, is_empty and clear were used; `insert` keeps a hand-written swap under `#[allow(clippy::manual_swap)]`, since both lanes' `Vec` support only push, pop and indexing) | text diff of the moved block against the removed one: only the `SiteSet` lines differ |
| | `src/server_h.rs:1584-1593`, `:1628-1631`, `:1634-1642` | M1 | `RaftCore` gains the heartbeat round state (`pending_rpcs_`, `authority_rounds_`, `pending_leader_term_`, `round_`) and `reset_round_state()` | Tier 1 |
| | `src/server_cc.rs:9`, `:23-26`, `:89` | M1 | the moved types and their imports leave | builds |
| | `src/server_cc.rs:199-254` (`heartbeat_phase0_locked`), `:284-333` (`heartbeat_phase0_body`), `:570-733` (`heartbeat_phase1_body`), `:878-939` (`heartbeat_apply_append_reply`), `:956-1045` (`heartbeat_phase2_body`), `:1121-1233` (`heartbeat_phase3_locked`, `heartbeat_phase3_body`) | M2 | the round-state parameters become the core's fields (`round.x` → `server.core.round_.x`, `pending_rpcs` → `server.core.pending_rpcs_`, ...); one `let nservers` added in phase 0 because the core is lent whole to `raft_commit_advance` | Tier 1 |
| | `src/server_cc.rs:1269-1309` | M1 | `HeartbeatRoundState` deleted; `HeartbeatDriver` holds only the server and resets the core's round state when its run begins and before its epilogue, where the driver's own state was created and dropped (one heartbeat loop per server) | Tier 1 |
| | `server.cc:1285-1287`, `server.h:198`, `:202`, `verus/commit_rule.rs:139`, repo `scripts/raft_field_census.py` | M1 | comments and the field census follow the rename | census exits 0 |
| `b75f285e2` | `rt/src/transport.rs:564`, `:581-583`, `:607-610`, `:615-623`, `:659` | F1 | the vote tally counts each voter once per campaign: the broadcast callback carries its peer's site id, `TallyState` keeps the voters counted | unit test `a_repeated_reply_counts_once` |
| | `rt/src/transport.rs:949`, `:952-953`, `:958-986` | F1 | test helper `fed` gives each reply its own peer; tests `a_stale_term_reply_is_counted_as_cast` (pins that a reply's term is not compared with the campaign's) and `a_repeated_reply_counts_once` | `cargo test` (raft-rt) |
| `372b73e6f` | `src/server_cc.rs:594-596`, `:599-605`, `:725` | F3 | phase 1 re-checks, inside the locked block that builds the message, that the server still leads in the round's term, and sends the `commit_index_` read there instead of phase 0's snapshot | Tier 1; the two values agree today (plan A.2) |
| `a38c5012e` | `src/server_h.rs:4907-4927` | F4 | `AeDecodePayload` refuses a batch with any entry term below 1, and a single entry of term below 1 | lab case 12 |
| | `src/lab.rs:444-495`, `src/lab_cases.rs:659-717`, `:732` | F4 | lab helpers `serve_append`, `site_id_of`, `log_tail`; lab case 12 (an entry-less control append is accepted, the same append with a term-0 entry is refused and the log is unchanged) | lab |
| `771d238c5` | `src/server_h.rs:2282-2283`, `:3176-3201`, `:4902-4920`; `server.cc:925` | F5 | `verified_config_ok()` (snapshots off and a whole log, failover on, a static configuration containing this server), checked at the end of `SetupInternal`: a normal run logs a warning, `MAKO_RAFT_VERIFIED_GATES=1` fails closed; the knob is parsed like the other env knobs | Tier 1 (the lab's snapshot cases run outside the gate and log the warning) |
| `15160ea4d` | `src/lab.rs:468-480`, `src/lab_cases.rs:715-759`, `:761-762`, `:778` | none (test only) | lab case 13 pins today's behaviour: an unavailable replica answers "no" at the candidate's own term, and its term does not move (V3, plan §4.3) | lab |
| `590370719` | repo `docs/migration/raft/cpp-rust-correspondence.md` | none (generated) | line counts regenerated; `raft_correspondence_check` fails the build when stale | `gen_correspondence.py --check` |

Plan deviation: the plan's Phase 1 asks for lab cases for a duplicated and a
stale-term vote reply. The lab cannot inject a reply into a live campaign, so
both are raft-rt unit tests on the tally itself (`b75f285e2`).

## Phase 2 (side effects become returned actions; F6)

Line numbers are the start of each item after its commit (`src/server_h.rs`
unless stated).

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `3ab039029` | 22 `raft_verify` sites and 7 `if !cond { panic!(..) }` checks in `src/server_h.rs` and `src/server_cc.rs` | M10 | `assert!(cond)`, same condition, the message kept as a comment; `AuthorityLedger::launch`'s result bound to a local before the assert (a write never sits inside an assert) | clippy, Tier 1 |
| | `server.cc`, `server.h`, both extern blocks, repo `scripts/gen_correspondence.py` | M10 | the now-unused `raft_verify` kernel deleted | builds |
| `4a8d2dc81` | `:563` (`RaftEntry`), `:4874` (`raft_entry_from_command`); `server.cc:1469` (`raft_command_meta`) | M6 | `RaftEntry` caches `has_value`, `is_tpc_commit`, `kind`, `payload_bytes`, asked once through one kernel when the entry is made; the three construction sites use it | Tier 1 |
| | `src/server_cc.rs` batch selection; `EnqueueCommittedEntries`; the follower's conflict search; the two snapshot-boundary checks | M6 | every per-entry metadata query reads the cached field | Tier 1 |
| `2d657ae4c` | `:1559` (`CoreActionKind`, `TimerResetReason`, `CoreAction`, `CoreOutput`) | M3 | the actions a core call pushes, carried out in push order by the shell | Tier 1 |
| | `:1833` (`rebuild_peer_tables`, `peer_site_at`, `is_config_member`, `peer_ordinal`), `RaftCore` fields `config_members_`, `peer_sites_` | M1 | moved from `RaftServerBase`, which keeps delegates | Tier 1 |
| | `:1897` (`append_local`), `:1913` (`reset_election_timer`), `:1931` (`set_is_leader`), `:2032` (`step_down`) | M3, M4 | `setIsLeader`/`stepDown` as core methods pushing APPEND_NOOP, RESET_ELECTION and ROLE_SET; the timer reset's clock and sample are parameters; `AppendLocal`'s append | Tier 1; lab case 14 |
| | `:3285` (`run_locked_actions`), `resetTimerLocked` | M3, M4 | the shell's executor; `resetTimerLocked` samples, then calls the core | Tier 1 |
| | `:5014` (`RequestVoteSettleLocked`), `:6091` (`on_request_vote_locked`), `:6523` (`on_append_entries_locked`), `InstallSnapshotReplyAccepted(Locked)`, `OnInstallSnapshotLocked`, `ConstructRuntime`, `src/server_cc.rs` phase 2's step-down | M3 | each critical section that can change the role runs its actions before releasing `mtx_`; the follower's InstallSnapshot runs them where `stepDown` ran them, ahead of the install | Tier 1 |
| `a85a19993` | `:2684` (`LeaderNotices`), `:3357` (`run_unlocked_actions`), `:3387` (`fire_leader_notices`), `:5740` (`RegisterLeaderChangeCallback`), `install_out_`, every role-changing caller | F6 | the role's log entry and the leader-change callback run after `mtx_` is released, in transition order, each once, from a copy taken under the notice queue's lock; registration takes that lock (bugs-found B11) | lab case 14 |
| | `src/lab_cases.rs:769`, `src/lab.rs:173`, `server.cc:517` | F6 | lab case 14 and its recorder kernel | lab |

## Phase 3 (cut the fibers; F7)

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `937a34cd8` | `src/server_cc.rs:303` (`AppendPayload`, `AppendSend`, `SnapshotSend`, `HeartbeatTick`), `:419` (`heartbeat_select_payload`), `:584` (`heartbeat_tick`) | M5, M11 | PHASE 0 and PHASE 1's decisions as one core call: every follower's AppendEntries chosen, the entries' handles copied out under the guard (`raft_command_handle_clone`, M11), the in-flight slot's protocol half placed | Tier 1 |
| | `src/server_cc.rs:772` (`heartbeat_tick_body`) | M5, F7 | the shell: actions and InstallSnapshot kernels under the guard, then each payload built (a batch stamped and finalized) and sent in follower order after it | Tier 1 |
| | `src/server_cc.rs:1109` (`heartbeat_on_reply`), `:1232` (`heartbeat_collect_body`), `:1401` (`heartbeat_round_end` and its body), `:1467` (the driver) | M5 | PHASE 2's per-reply body and PHASE 3 as core calls; the poll loop, deadline and early-quorum exit stay the shell's; a declined round ends at the tick (B12) | Tier 1 |
| | `src/server_h.rs:966` (`PendingAppend`), `:2697` (`AppendResponses`), `:1906` (`log_term_change`) | M5, M1 | the in-flight slot's protocol half in the core, the response handles in the shell; LogTermChange's body in the core | Tier 1 |
| `496db6cd2` | `src/server_h.rs:1925` (`election_last_log_term`), `:1950` (`do_vote`), `:5344` (`WireBatch`), `:6180` (`raft_on_request_vote`), `:6434` (`raft_on_append_entries`) | M5, M11 | the inbound handlers take the core; the payload is a `WireBatch` over the wire kernels (AeDecodePayload's and AeApplyIncoming's bodies) | Tier 1 |
| `25aacb728` | `src/server_h.rs:1695` (`VoteOutcome`, `VoteSet`, `CampaignStart`), `:2103` (`start_election`), `:2170` (`election_settle`), `:5363` (`RequestVoteImpl`), `:6228` (`raft_election_tick`) | M5, M4, F1 | the campaign as two core calls; the tally counted by the core from the replies each lane's wait gathered (F1 now on every lane); the election gather a core function | Tier 1 |
| | `rt/src/transport.rs:614`, `rt/src/seam.rs:309`; `commo.h:66`, `commo.cc` (BroadcastVote), `server_seam_cpp.cc:159` | M5 | each lane records every vote reply (voter, vote, term) and exposes it through four seam kernels replacing `raft_vote_quorum_snapshot` | Tier 1, raft-rt tests |


## Phase 4 (serialization and mirrors; F8)

Line numbers are each commit's own (`src/server_h.rs`).

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `c62fc2519` | `:3294` (`commit_index_mirror_`, `is_leader_mirror_`, `leader_hint_mirror_`, `term_mirror_`), `:3904` (`publish_mirrors`), `:3819` (its call at the end of `run_locked_actions`), `:4064` (after snapshot recovery), `:6255` (after the follower's InstallSnapshot) | F8 | the four mirrors, published under `mtx_` at the end of every critical section that runs a core decision and after the shell's own writes of those fields | Tier 1 ×2 |
| | `:5993` (`IsLeader`), `:6002` (`GetLeaderHint`), `:6050` (`CommitIndex`) | F8 | read the mirrors with no `mtx_`; `CommitIndex`'s unlocked read of the core field (B2) is gone | Tier 1 ×2 |
| `366b25935` | `:2313` (`RaftCore::on_applied`), `:3634` (`PublishAppliedIndexLocked`) | M3 | the applied index's no-going-back decision in the core; `appliedIndexForWait_` stays the shell's mirror | Tier 1 ×2 |
| | `:3270` (`preferred_leader_site_id_` an `AtomicU64`), `:3557` (`preferred_leader`), `:3567`, `:3750`, `:6029` (`SetPreferredLeader`) | M1 | the preferred leader a shell atomic written with no `mtx_`; its readers go through `preferred_leader()` | Tier 1 ×2 |
| `22a54332c` | `:3319` (`verified_gates_`), `:4118` (set at setup), `:3683` (`CompactLog`), `:5236` (`MaybeCreateSnapshot`) | F5 | under `MAKO_RAFT_VERIFIED_GATES=1` the two entry points that could shorten the log return at once | Tier 1 ×2 |

Plan deviation: `with_core` as the compiler-enforced only accessor is not
done here. It needs `RaftCore` in a module of its own (a C++20 module graph
may not be cyclic, so the shell cannot import a wrapper that imports the
core), which is Phase 6's crate split. `scripts/verus/core_access_census.py`
lists the shell functions that still name core fields, as Phase 6's work
list.
