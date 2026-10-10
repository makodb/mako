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
| | `shell/server_cc.rs:481`, `:666`, `:1233`, `:1243` | M0 | trace stages 8 (commit advanced), 3/4 (append sent) | as above |
| | `shell/server_h.rs:1443`, `:3345`, `:3894` | M0 | trace stages 10 (apply pop), 9 (apply enqueue) | as above |
| | `rt/src/trace.rs` (new), `rt/src/lib.rs:7` | M0 | the Rust lane's hook slots, null unless installed; raft-rt's standalone cargo tests link no C++ | `cargo test: raft-rt` passes |
| | `rt/src/service.rs:120`, `:134-137` | M0 | trace stages 5/6 (follower accepted), through the hooks | traced run |
| | `rt/src/transport.rs:373-377` | M0 | trace stage 7 (leader got a success reply), through the hooks | traced run |
| | `raft_lane_rust.cc:25-33`, `transport_exports.h:31-34` | M0 | `Serve` installs the hooks only when tracing is on | traced run |
| | `raft_worker.cc:50-53`, `:797`, `:850`, `:864-867`, `:1011`, `:1243`; `raft_worker.h:98` | M0 | trace stages 0-2 (enqueue, submit, Start returned), 11 (Next) | traced run |
| | `server.cc:503-513` | M0 | `raft_lab_commit_log_enabled` (`MAKO_RAFT_LAB_COMMIT_LOG=1`), lab builds only (`RAFT_TEST_CORO`) | lab runs, three lanes |
| | `shell/lab.rs:25`, `:49-50`, `:180`, `:189`, `:196`, `:200-268` | M0 | `LABLEADER` per waitOneLeader result; `LABCOMMIT` dump of the committed table per case (row-by-row copy, see bugs-found B10) | lab runs; comparator calibration |
| | `shell/lab_cases.rs:31`, `:36` | M0 | dump at every case end (`passed`/`failed`) | as above |
| | `raft_bench.cc:126-172`, `:213-215`, `:262-263`, `:289`, `:316`, `:345`, `:370-374` | M0 | `--failover-out`: realtime stamps, record writer, option plumbing | G7 functional run; G7 A/A |
| | `raft_bench.cc:1423-1425`, `:1458-1464` | M0 | leadership callback stamps `new_leader_us`; apply callback stamps `first_commit_us` for a tagged seq-0 probe | as above |
| | `raft_bench.cc:1639-1690` | M0 | follower loop: arm, propose tagged probes once leader, write record, end markers | as above |
| | repo `examples/raft_bench.sh` (kill mode) | M0 | `leader_loss_us` stamp before SIGKILL, `--failover-out` plumbing, the run's JSON record | as above |

## Phase 1 (one `RaftCore`; F1, F3-F5)

The M2 rename is checked mechanically: `git show c5fa64ec5^:<f> | sed
's/\bstate_\b/core/g; s/RaftConsensusState/RaftCore/g'` differs from
`c5fa64ec5:<f>` only in the M1/M9 rows below (`<f>` = `shell/server_h.rs`,
`shell/server_cc.rs`).

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `c5fa64ec5` | `shell/server_h.rs` (244 hunks), `shell/server_cc.rs` (49), `shell/lab_snapshot_cases.rs:1259-1260` | M2 | `RaftConsensusState` → `RaftCore`, `RaftServerBase::state_` → `core`, in code, comments and log text | the sed check above |
| | `shell/server_h.rs:5-7` | M1 | imports for the moved authority ledger | builds |
| | `shell/server_h.rs:920-1520` | M1, M9 | `PendingAppend`, `PendingTable`, `HeartbeatAuthority`, `AuthorityGeneration`, `AuthorityReply`, `AuthorityOutcome`, `AuthorityLedger`, `HeartbeatRoundScope` moved from `server_cc.rs` with bodies unchanged; their `rusty::BTreeSet<u16>` site sets become `SiteSet`, a sorted duplicate-free `Vec<u16>` (same membership and order; only insert, remove, contains, len, is_empty and clear were used; `insert` keeps a hand-written swap under `#[allow(clippy::manual_swap)]`, since both lanes' `Vec` support only push, pop and indexing) | text diff of the moved block against the removed one: only the `SiteSet` lines differ |
| | `shell/server_h.rs:1584-1593`, `:1628-1631`, `:1634-1642` | M1 | `RaftCore` gains the heartbeat round state (`pending_rpcs_`, `authority_rounds_`, `pending_leader_term_`, `round_`) and `reset_round_state()` | Tier 1 |
| | `shell/server_cc.rs:9`, `:23-26`, `:89` | M1 | the moved types and their imports leave | builds |
| | `shell/server_cc.rs:199-254` (`heartbeat_phase0_locked`), `:284-333` (`heartbeat_phase0_body`), `:570-733` (`heartbeat_phase1_body`), `:878-939` (`heartbeat_apply_append_reply`), `:956-1045` (`heartbeat_phase2_body`), `:1121-1233` (`heartbeat_phase3_locked`, `heartbeat_phase3_body`) | M2 | the round-state parameters become the core's fields (`round.x` → `server.core.round_.x`, `pending_rpcs` → `server.core.pending_rpcs_`, ...); one `let nservers` added in phase 0 because the core is lent whole to `raft_commit_advance` | Tier 1 |
| | `shell/server_cc.rs:1269-1309` | M1 | `HeartbeatRoundState` deleted; `HeartbeatDriver` holds only the server and resets the core's round state when its run begins and before its epilogue, where the driver's own state was created and dropped (one heartbeat loop per server) | Tier 1 |
| | `server.cc:1285-1287`, `server.h:198`, `:202`, `verus/commit_rule.rs:139`, repo `scripts/raft_field_census.py` | M1 | comments and the field census follow the rename | census exits 0 |
| `b75f285e2` | `rt/src/transport.rs:564`, `:581-583`, `:607-610`, `:615-623`, `:659` | F1 | the vote tally counts each voter once per campaign: the broadcast callback carries its peer's site id, `TallyState` keeps the voters counted | unit test `a_repeated_reply_counts_once` |
| | `rt/src/transport.rs:949`, `:952-953`, `:958-986` | F1 | test helper `fed` gives each reply its own peer; tests `a_stale_term_reply_is_counted_as_cast` (pins that a reply's term is not compared with the campaign's) and `a_repeated_reply_counts_once` | `cargo test` (raft-rt) |
| `372b73e6f` | `shell/server_cc.rs:594-596`, `:599-605`, `:725` | F3 | phase 1 re-checks, inside the locked block that builds the message, that the server still leads in the round's term, and sends the `commit_index_` read there instead of phase 0's snapshot | Tier 1; the two values agree today (plan A.2) |
| `a38c5012e` | `shell/server_h.rs:4907-4927` | F4 | `AeDecodePayload` refuses a batch with any entry term below 1, and a single entry of term below 1 | lab case 12 |
| | `shell/lab.rs:444-495`, `shell/lab_cases.rs:659-717`, `:732` | F4 | lab helpers `serve_append`, `site_id_of`, `log_tail`; lab case 12 (an entry-less control append is accepted, the same append with a term-0 entry is refused and the log is unchanged) | lab |
| `771d238c5` | `shell/server_h.rs:2282-2283`, `:3176-3201`, `:4902-4920`; `server.cc:925` | F5 | `verified_config_ok()` (snapshots off and a whole log, failover on, a static configuration containing this server), checked at the end of `SetupInternal`: a normal run logs a warning, `MAKO_RAFT_VERIFIED_GATES=1` fails closed; the knob is parsed like the other env knobs | Tier 1 (the lab's snapshot cases run outside the gate and log the warning) |
| `15160ea4d` | `shell/lab.rs:468-480`, `shell/lab_cases.rs:715-759`, `:761-762`, `:778` | none (test only) | lab case 13 pins today's behaviour: an unavailable replica answers "no" at the candidate's own term, and its term does not move (V3, plan §4.3) | lab |
| `590370719` | repo `docs/migration/raft/cpp-rust-correspondence.md` | none (generated) | line counts regenerated; `raft_correspondence_check` fails the build when stale | `gen_correspondence.py --check` |

Plan deviation: the plan's Phase 1 asks for lab cases for a duplicated and a
stale-term vote reply. The lab cannot inject a reply into a live campaign, so
both are raft-rt unit tests on the tally itself (`b75f285e2`).

## Phase 2 (side effects become returned actions; F6)

Line numbers are the start of each item after its commit (`shell/server_h.rs`
unless stated).

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `3ab039029` | 22 `raft_verify` sites and 7 `if !cond { panic!(..) }` checks in `shell/server_h.rs` and `shell/server_cc.rs` | M10 | `assert!(cond)`, same condition, the message kept as a comment; `AuthorityLedger::launch`'s result bound to a local before the assert (a write never sits inside an assert) | clippy, Tier 1 |
| | `server.cc`, `server.h`, both extern blocks, repo `scripts/gen_correspondence.py` | M10 | the now-unused `raft_verify` kernel deleted | builds |
| `4a8d2dc81` | `:563` (`RaftEntry`), `:4874` (`raft_entry_from_command`); `server.cc:1469` (`raft_command_meta`) | M6 | `RaftEntry` caches `has_value`, `is_tpc_commit`, `kind`, `payload_bytes`, asked once through one kernel when the entry is made; the three construction sites use it | Tier 1 |
| | `shell/server_cc.rs` batch selection; `EnqueueCommittedEntries`; the follower's conflict search; the two snapshot-boundary checks | M6 | every per-entry metadata query reads the cached field | Tier 1 |
| `2d657ae4c` | `:1559` (`CoreActionKind`, `TimerResetReason`, `CoreAction`, `CoreOutput`) | M3 | the actions a core call pushes, carried out in push order by the shell | Tier 1 |
| | `:1833` (`rebuild_peer_tables`, `peer_site_at`, `is_config_member`, `peer_ordinal`), `RaftCore` fields `config_members_`, `peer_sites_` | M1 | moved from `RaftServerBase`, which keeps delegates | Tier 1 |
| | `:1897` (`append_local`), `:1913` (`reset_election_timer`), `:1931` (`set_is_leader`), `:2032` (`step_down`) | M3, M4 | `setIsLeader`/`stepDown` as core methods pushing APPEND_NOOP, RESET_ELECTION and ROLE_SET; the timer reset's clock and sample are parameters; `AppendLocal`'s append | Tier 1; lab case 14 |
| | `:3285` (`run_locked_actions`), `resetTimerLocked` | M3, M4 | the shell's executor; `resetTimerLocked` samples, then calls the core | Tier 1 |
| | `:5014` (`RequestVoteSettleLocked`), `:6091` (`on_request_vote_locked`), `:6523` (`on_append_entries_locked`), `InstallSnapshotReplyAccepted(Locked)`, `OnInstallSnapshotLocked`, `ConstructRuntime`, `shell/server_cc.rs` phase 2's step-down | M3 | each critical section that can change the role runs its actions before releasing `mtx_`; the follower's InstallSnapshot runs them where `stepDown` ran them, ahead of the install | Tier 1 |
| `a85a19993` | `:2684` (`LeaderNotices`), `:3357` (`run_unlocked_actions`), `:3387` (`fire_leader_notices`), `:5740` (`RegisterLeaderChangeCallback`), `install_out_`, every role-changing caller | F6 | the role's log entry and the leader-change callback run after `mtx_` is released, in transition order, each once, from a copy taken under the notice queue's lock; registration takes that lock (bugs-found B11) | lab case 14 |
| | `shell/lab_cases.rs:769`, `shell/lab.rs:173`, `server.cc:517` | F6 | lab case 14 and its recorder kernel | lab |

## Phase 3 (cut the fibers; F7)

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `937a34cd8` | `shell/server_cc.rs:303` (`AppendPayload`, `AppendSend`, `SnapshotSend`, `HeartbeatTick`), `:419` (`heartbeat_select_payload`), `:584` (`heartbeat_tick`) | M5, M11 | PHASE 0 and PHASE 1's decisions as one core call: every follower's AppendEntries chosen, the entries' handles copied out under the guard (`raft_command_handle_clone`, M11), the in-flight slot's protocol half placed | Tier 1 |
| | `shell/server_cc.rs:772` (`heartbeat_tick_body`) | M5, F7 | the shell: actions and InstallSnapshot kernels under the guard, then each payload built (a batch stamped and finalized) and sent in follower order after it | Tier 1 |
| | `shell/server_cc.rs:1109` (`heartbeat_on_reply`), `:1232` (`heartbeat_collect_body`), `:1401` (`heartbeat_round_end` and its body), `:1467` (the driver) | M5 | PHASE 2's per-reply body and PHASE 3 as core calls; the poll loop, deadline and early-quorum exit stay the shell's; a declined round ends at the tick (B12) | Tier 1 |
| | `shell/server_h.rs:966` (`PendingAppend`), `:2697` (`AppendResponses`), `:1906` (`log_term_change`) | M5, M1 | the in-flight slot's protocol half in the core, the response handles in the shell; LogTermChange's body in the core | Tier 1 |
| `496db6cd2` | `shell/server_h.rs:1925` (`election_last_log_term`), `:1950` (`do_vote`), `:5344` (`WireBatch`), `:6180` (`raft_on_request_vote`), `:6434` (`raft_on_append_entries`) | M5, M11 | the inbound handlers take the core; the payload is a `WireBatch` over the wire kernels (AeDecodePayload's and AeApplyIncoming's bodies) | Tier 1 |
| `25aacb728` | `shell/server_h.rs:1695` (`VoteOutcome`, `VoteSet`, `CampaignStart`), `:2103` (`start_election`), `:2170` (`election_settle`), `:5363` (`RequestVoteImpl`), `:6228` (`raft_election_tick`) | M5, M4, F1 | the campaign as two core calls; the tally counted by the core from the replies each lane's wait gathered (F1 now on every lane); the election gather a core function | Tier 1 |
| | `rt/src/transport.rs:614`, `rt/src/seam.rs:309`; `commo.h:66`, `commo.cc` (BroadcastVote), `server_seam_cpp.cc:159` | M5 | each lane records every vote reply (voter, vote, term) and exposes it through four seam kernels replacing `raft_vote_quorum_snapshot` | Tier 1, raft-rt tests |


## Phase 4 (serialization and mirrors; F8)

Line numbers are each commit's own (`shell/server_h.rs`).

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

## Phase 6 (the core crate, verified; F5's gate in the core, F9)

Paths are `src/deptran/raft/`-relative; line numbers are at the phase's
last code commit (`ce967f6f7`). From `3d768bdcf` on, every line of
`core/src` is also checked mechanically: `scripts/verus/ledger_lint.py`
classifies each as ghost, comment, structural, moved (present in the shell
at the phase's start, `43c57e3ac`, after the recorded renames) or labelled
(a tag on the statement or `(whole item)` / `(whole file)`), and
`core_check.sh` fails a commit with any other line.

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `43c57e3ac` | CMake, `ci/ci.sh`, `scripts/verus/tier1.sh`, deleted lane files | none (user decision Q9) | the hybrid and cpp lanes removed: the Verus worktree is rust-lane only | Tier 1 (rust): lab 30/30, four suites pass |
| `896cdfc85` | `core/` (new crate `raft-core`), `shell/server_h.rs` (`pub use raft_core::*`, aliases), `shell/server_cc.rs` | M1 | the core's types and calls moved out of the shell verbatim, paths and visibility aside; quorum.hpp's two helpers copied in | Tier 1; clippy both feature sets |
| | `core/src/node.rs` (`InboundBatch`), `shell/server_h.rs` (`WireBatch`), `src/rusty-rustc` (`Clone for RaftCommand`) | M11 | the command is the type parameter `C`; the inbound payload is `W: InboundBatch<C>`; `append_into`'s loop runs in the core over `entry_at(k)`; the reply handler takes the reply's three scalars | as above |
| | `core/src/logging.rs` (new), `core/src/output.rs`, every core log call | M7 | the core pushes log records into `CoreOutput`, filtered by level at the push; the shell prints them first in `run_locked_actions` | as above |
| | `shell/server_h.rs` (`run_locked_actions`, APPLY_RANGE) | M0 | the trace kit's stage-8 stamp moves from the commit rule to the shell's executor | traced runs (Phase 6 checkpoint) |
| `53bb170b4` | `core/src/{helpers,logging,output,progress,pending,authority,election}.rs` | M12 | inside `verus!`, with the specs panic and overflow freedom need | `verify_core.sh` |
| | `core/src/progress.rs:67-74` (`back_off_after_reject`) | M10 | `follower_last_log_index + 1` is `wrapping_add(1)`: release builds always wrapped it | as above |
| `2fc0dcdba` | `core/src/log.rs` | M12, M11 | `RaftLog`'s block layout as its invariant, its index arithmetic proved below the 2^62 ceiling; `div_ceil` behind the trusted `blocks_for` | as above |
| `92f571d3b`, `1121af91e` | `core/src/node.rs:110` (`inv`), every `RaftCore` method | M12, M10 | `RaftCore`'s invariant; `assert!` → `runtime_assert` (the same check at run time, a proof obligation for Verus); `now - last_heartbeat_time_` → `wrapping_sub` in the campaign and the election tick | as above |
| `0166c0a89` | `core/src/authority.rs` (`SiteSet::insert`, `remove`) | M9, M12 | the sorted position, then `Vec::insert` / `Vec::remove` there: the sequence the hand-written bubble and shift made; set semantics specified | as above |
| `de877ebc6`, `cb15e7c62` | `core/src/authority.rs`, `core/src/node.rs` | M12 | the ledger's round ids specified; the invariant covers peers, rounds and epochs | as above |
| `b269b4589` | `core/src/node.rs:1319` (`raft_on_append_entries`) | M12 | the append handler verifies; `core_trusted.txt` holds only `blocks_for` | 261 verified |
| `192e15c39` | `core/src/heartbeat.rs`, `progress.rs`, `authority.rs`, `node.rs` | M12 | the heartbeat round verifies; `AppendReplyAction` and `BackoffKind` derive `Structural` (expands to nothing under cargo) | 301 verified; clippy |
| `62a237637` | `core/src/node.rs`, `shell/server_h.rs:967` | M1 | `snapshot_threshold_` and the two snapshot-callback tokens move to `RaftServerBase`: the core never read them | field census exits 0 |
| `7aa01a586` | `core/src/node.rs:204` (`set_identity`), `shell/server_h.rs:3705` (`set_site_identity`) | M1 | `set_site_identity`'s core half as a core call | 305 verified |
| | `core/src/node.rs:224` (`configure`), `shell/server_h.rs:3531` (`LoadCurrentConfig`) | M1, F5 | `LoadCurrentConfig`'s write as a core call (M1) that also builds the peer table with next index 1 (F5: the core is consistent from Setup on; `HeartbeatPrologue`'s rebuild still runs and finds the same table) | as above; smoke suite |
| | `core/src/node.rs:259` (`enter_gates`), `:99` (`gated_`), `shell/server_h.rs:3519` (`verified_config_ok`) | F5 | `verified_config_ok`'s decision moved into the core (the plan's `new_gated`); a pass sets `gated_`, whose facts the invariant carries | as above |
| `4962bd6d8` | `core/src/event.rs` (new: `Event` `:24`, `Reply` `:110`, `admits` `:240`, `step` `:356`) | M5 | the core's one entry point: each event dispatched to the core call the shell made directly, same arguments, same critical section; the two RPC handlers' out-params become reply fields | 319 verified; build; smoke suite |
| | `shell/server_h.rs:1407` (`RaftServerBase::step`), every shell call site in `shell/server_h.rs` and `shell/server_cc.rs:97`, `:252`, `:297`, `:317` | M5 | the shell calls the core only through `step` | as above |
| | `core/src/node.rs:104` (`decoded_terms_`), `raft_on_append_entries` | M1 | the decode scratch moves from `RaftServerBase` into the core | as above |
| | `shell/server_cc.rs:364`, `:390` (`HeartbeatDriver::run`) | B15 (plan §3.1 rule 1) | the two round-state resets take `mtx_` (they did not) | as above |
| `affc46aa7` | `shell/server_h.rs:972` (`recorder_`), `:3379` (`CoreRecorder`), `:3426` (`command_digest`), `step` | M0 | the replay recorder behind `MAKO_RAFT_REPLAY_DIR`: one record per step; `T` lines at the snapshot paths' direct writes (`CompactLogLocked` only when it can move the log) | 40,524 recorded steps replay byte for byte |
| | `replay/` (new crate `raft-replay`), `Cargo.toml`, `core/src/output.rs` (`log_level`) | M0 | the record format, the parser, `replay()`, and `core_replay` (A.4 item 4's test) | its unit tests; `core_replay` |
| `9665c8f3e` | `core/src/event.rs:285` (`message_admitted`), `:315` (`step_checked`) | F9 | drop a message from this server or outside the configuration, at term 0, or (an append) with prev index 0 but a prev term or the reverse; read a success reply beyond the leader's log as no reply | 321 verified; `core/tests/step_checked.rs` |
| | `shell/server_h.rs:1427` (`step_checked`), `:4192` (`on_request_vote_locked`), `:4280` (`on_append_entries_locked`), `shell/server_cc.rs:252` | F9 | inbound requests and replies go through `step_checked`; a dropped request is answered as an unavailable replica answers (append 0/0/0, vote "no" at the candidate's term) | lab case 15 |
| | `shell/lab_cases.rs:771` (case 15), `core/tests/step_checked.rs` | F9 | the new behaviour, end to end and for the reply case in the core | Tier 1 |
| `3d768bdcf`, `0986e4b7f`, `ce967f6f7` | `scripts/verus/ledger_lint.py`, `core_check.sh`; tags in `core/src` | none (tooling, comments) | the ledger lint, run by `core_check.sh` with the correspondence doc's check | negative tests in the commit |

## Phase 8 (the coupling and the proof; ghost only)

Every core change is M12: ghost code (specs, proof blocks, ghost fields
under `cfg(verus_keep_ghost)`, the Verus-only module `core/src/coupling.rs`)
and comments. `core_check.sh` runs the ghost-only lint against Phase 6's last
commit (`51d100d1f`, `scripts/verus/ghost_only_base.txt`): every changed
line of `core/src` is ghost, comment, structural, or the same code as a
removed line (a named return, a contract moving a brace). At `998b86820`:
0 executable changes. No shell change.

| Commit | File (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `dfe6ba6d8` | `scripts/verus/verify_core.sh`, `core/src/coupling.rs` (new), `core/Cargo.toml` | M12 | the proof side imports spec v1 (Verus `--export`/`--import` of the frozen tag); the coupling module exists only for Verus | `verify_core.sh` |
| `0feae5ac7` | `core/src/log.rs` | M12 | `RaftLog::view()`: the live entries in index order; `get`, `append`, `truncate_from`, `new` state their effect on it | as above |
| `274a72654` | `scripts/verus/core_check.sh`, `ghost_only_base.txt` | tooling | the ghost-only lint (the plan's diff-2 lint) on every commit | -- |
| `5f97ae6fe` | `core/src/coupling.rs`, `core/src/node.rs` (ghost fields `g_log_`, `g_votes_`, `g_match_`, `g_next_`) | M12 | the coupling: `state_view`, `ginv`, the four ghost-log operations; `RaftCore::new` is fresh; `configure` records LoadConfig | 336 verified |
| `426eb7e1d`, `4d8357131`, `a09862d2f` | `scripts/verus/ledger_lint.py`; tags in `core/src/node.rs` | tooling; comments | the lint's ghost scanner fixed three ways (a struct literal or a block in a clause; a multi-line `let ghost`; a bodiless trait `spec fn`, which since Phase 6 had made `raft_on_append_entries`' body count as ghost); Verus-only modules are ghost; six Phase 6 lines of that body tagged with their Phase 6 items (M1, M7, M11) | the scanner's ghost lines diffed before and after |
| `6b5030ff4` | `core/src/{node,heartbeat,event}.rs` | M12 | the stutters; StepAside (`SetFollower`, `StepDown`); ClientRequest (`append_local`) | 341 verified |
| `033078d5b` | `core/src/node.rs` (`raft_on_request_vote`, `do_vote`) | M12 | an inbound RequestVote: StepDown, then GrantVote or RejectVote; `ginv` gains `snapterm_ == 0`, no member the sentinel, a leader or candidate voted for itself | 343 verified |
| `cc73145a0` | `core/src/node.rs` (`start_election`), `core/src/election.rs` | M12 | a started campaign is LTimeout | 346 verified |
| `e2f63615e` | `core/src/node.rs` (`election_settle`, `inv_config`), `core/src/election.rs` (`VoteSet`) | M12 | a settled campaign: ReceiveVoteGranted per grant, BecomeLeader on the exec's quorum, StepDown on a higher reply, StepAside; `inv_config` gains `election_term_ >= 0` | 355 verified |
| `762573fc4` | `core/src/node.rs` (`raft_on_append_entries`, `InboundBatch`), `core/src/log.rs` | M12 | an inbound AppendEntries: StepDown, then RejectAppendEntries, or one FollowerAppendEntries per component (BR2); the trait gains `spec_entries` and contracts; `ginv` gains every entry's value and term (B16) | 363 verified |
| `1d8babc9c` | `core/src/{progress,node,coupling}.rs` | M12 | V2 in `ginv`; the peer table's matches and the majority count (Phase 3's `commit_rule.rs`); `inv_peers` gains the peers' order and membership | 369 verified |
| `e60880223` | `core/src/{heartbeat,pending}.rs` | M12 | a heartbeat reply: StepDown, HandleAppendResponse, or unseen | 375 verified |
| `37319d389` | `core/src/heartbeat.rs` (`raft_commit_advance`, phases 0 and 3, `heartbeat_round_end`); `docs/verus/bugs-found.md` | M12; doc | the commit advance is AdvanceCommitIndex (`lemma_commit_quorum`); B17 recorded | 379 verified |
| `ba5e0ed1b` | `core/src/heartbeat.rs` (`heartbeat_tick`, `heartbeat_select_payload`), `core/src/node.rs` | M12 | a tick's AppendEntries: SendAppendEntries per component (BR1), built from the leader's log | 384 verified |
| `998b86820` | `core/src/event.rs` (`step`, `step_checked`, `message_admitted`), `core/src/coupling.rs` | M12 | `step` and `step_checked` keep `ginv` under `coupled(ev)`; `lemma_node_cert`; `theorem_mako_safety` | 387 verified |


## Bug fixes after Phase 8 (F12-F19; the user's approval, 2026-10-06)

The behaviour freeze is lifted for these numbered items only
([modification-plan.md](modification-plan.md) A.2, F12-F19). The ghost-only lint of Phase 8
is retired (`scripts/verus/ghost_only_base.txt` is empty): executable core lines may change
again, and the ledger lint still requires a tag on every one.

| Commit | File:line (after) | Kind | What | Evidence |
|---|---|---|---|---|
| `a586a7f51` | `core/src/heartbeat.rs:1692-1700` (`heartbeat_phase3_locked`) | F12 | the commit advance only when `core.is_leader_`; otherwise no advance (bugs-found B17) | `core/tests/b17_round_end.rs` passes, no longer ignored; 387 verified |
| | `core/src/heartbeat.rs:1676-1690`, `:1727-1735`; `core/src/coupling.rs:2611` | M12 | phase 3's and the round end's contracts lose the `is_leader` premise; `coupled(RoundEnd)` is the gate | as above |
| | `core/src/heartbeat.rs:1455`, `:1468`; `core/src/coupling.rs:2608`, `:2622` | M12 | the reply's premise is `is_leader ==> core.is_leader_` (bugs-found B18) | as above |
| | `scripts/verus/ghost_only_base.txt`, `docs/verus/modification-plan.md` A.2 | tooling, plan | the ghost-only lint retired; F12-F19 registered | ledger lint 0 unregistered |
| `924a4dfdd` | `shell/server_h.rs:3895-3908` (`Start`) | F13 | under `mtx_`, after the leadership check, a command without a value is refused (REJECTED, warned) before it is appended (bugs-found B16) | lab case 16 `test_empty_command_refused` (`shell/lab_cases.rs:839-871`), with the `start_empty` helper (`shell/lab.rs:503-514`) |
| `21c287832` | `shell/server_h.rs:3290-3304` (`WireBatch`), `:3332-3353` (`decode_terms`), `:4323-4324` (`on_append_entries_locked`) | F14 | the batch carries the append's own term; a batch entry above it, or a single entry whose `leader_next_log_term` is above it, is refused like an undecodable payload (the rest of B4) | lab case 12's second probe (`shell/lab_cases.rs:712-730`) |
| `ad7af330e` | `core/src/election.rs:108-112` (`VoteSet::outcome`), `rt/src/transport.rs:643-649` (`TallyState::no`) | F15 | a campaign is lost once `no > (n - 1) - n/2`: two refusals of three replicas, three of five (B1) | `rt` unit tests `three_replicas_lose_once_both_peers_reject`, `five_replicas_lose_once_three_peers_reject`; `rt/tests/transport_roundtrip.rs:142-155` `a_rejected_three_replica_campaign_is_lost_at_once`; 387 verified |
| `00881d428` | `replay/src/lib.rs:869` | tidy | two redundant borrows in a test's `format!` (clippy `--all-targets`) | clippy clean |
| `cd46bbf20` | `core/src/node.rs:1176-1184` (`set_is_leader`) | F16 | the stale-publication guard is `if stopped`; its dead term half is gone (B8) | 387 verified; ledger lint 0 unregistered |
| | `shell/server_h.rs:908-911` (field), `Get`/`SetHeartbeatInterval`; `shell/server_cc.rs:201-208` (`heartbeat_collect_body`); `rt/src/snapshot.rs:412` | F17 | `heartbeat_interval_us_` is an `AtomicU64`, Relaxed; the collect phase reads it once per round (B14) | lab suite 32/32 |
| | repo `src/deptran/raft/raft_worker.{h,cc}` (`Submit`, `OutstandingOwnLogs`), `src/deptran/raft_main_helper.cc:978-988` (`get_outstanding_logs`) | F18 | the worker records each accepted submission's index and prunes those at or below the commit index; the metric is that count (B7) | Tier 1 replication suites |
| `74dab7c9a` | `shell/server_h.rs` (`ShellCell`; `RaftServerBase`'s fields; `core()`, `mtx()`, `recorder()`, `install_out()`, `append_responses()`, `batch_buffer()`; every method's receiver) | F19 | every entry takes `&RaftServerBase`; a field that changes after the server is shared is an atomic or a `ShellCell` reached under its stated lock or owner; `EnsureSetup` is one atomic swap; three two-`core()` expressions take one reference (B19) | workspace tests; `tests/server_is_send.rs` with `ShellCell: Sync` only for `T: Send`; lab suite 32/32 |
| | repo `src/deptran/scheduler.h` (DSL and its generated C++), `shell/scheduler_h.rs` (extracted) | F19 | `RaftSpecific`'s twelve methods take `&self`; the C++ virtuals become `const`; `TxLogServer` unchanged (PaxosServer implements it) | `scripts/raft_dsl.sh --check` (the build's source gate) |
| | `shell/server_h.rs` (`bind_commo`, `register_learner_action`), `scripts/raft_gen_exports.py` (`SHARED_TWINS`); regenerated `shell/server_cc.rs` exports, `server_exports.h`, the `RaftServer` shim | F19 | the exports of `&self` methods take `*const RaftServerBase`; `set_commo` and `reg_learner_action` export `&self` twins | `scripts/raft_field_census.py` exits 0 |
| | `rt/src/service.rs` (`RaftRpcService::server`) | F19 | the service hands handlers `&RaftServerBase`; its `mut_from_ref` allow is gone | `rt` tests |
