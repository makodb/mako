// The Raft server's second half: the C ABI exports, the inbound RPC bodies
// and the heartbeat phases. rustc compiles this into libraft.a; nothing here
// is transpiled, so edit it directly.

#[allow(dead_code, non_snake_case)]
fn IsPreferredLeaderConfigured(preferred_leader_site_id: u16) -> bool {
    preferred_leader_site_id != u16::MAX
}

use crate::server_h::raft_server_read_index_round_can_advance;
use crate::server_h::raft_server_log_index_above;
use crate::server_h::raft_server_log_entry_is_current_term;
use crate::server_h::raft_server_observed_higher_term;
use crate::server_h::raft_server_append_acknowledged_through;
use crate::server_h::raft_server_log_index_has_successor;
use crate::server_h::raft_server_follower_next_index;
use crate::server_h::raft_server_append_sent_end;
use crate::server_h::raft_server_append_entry_count_fits;
use crate::server_h::raft_server_append_batch_count_is_valid;
use crate::server_h::BackoffKind;
use crate::server_h::RAFT_SERVER_INVALID_SITE_ID;
use crate::server_h::RaftCore;
use crate::server_h::CoreOutput;  // [move, M3]
use crate::server_h::CoreAction;  // [move, M5]
use crate::server_h::AppendResponses;  // [move, M5]
// [move, M1] the heartbeat round's state, moved into server_h.rs with RaftCore
use crate::server_h::AuthorityReply;
use crate::server_h::HeartbeatAuthority;
use crate::server_h::PendingAppend;

use crate::server_h::RaftServerBase;
use crate::server_pods_h::AppendRespView;
use crate::server_pods_h::RaftServerHandle;
use crate::scheduler_h::RaftSpecific;
use crate::server_h::RaftEntry;
use crate::server_h::RaftLockGuard;
// Every C++ kernel this carrier calls, in one place. improper_ctypes is
// allowed because each of these passes an opaque handle by pointer and
// nothing is laid out across the boundary -- the same case as the allow
// on server.h's bridge block.
#[allow(improper_ctypes)]
unsafe extern "C" {
    fn raft_monotonic_now_us() -> u64;
    fn raft_fiber_sleep_us(micros: u64);
    fn raft_trace_through(stage: i32, through: u64, t_us: u64);  // [M0] trace kit
    fn raft_append_response_read(response: *const rusty::RaftResponsePtr)
        -> AppendRespView;
    // Drops this server's row from the C++ commo table; called from
    // raft_server_delete. See RaftServerBase::set_commo.
    fn raft_unbind_commo(server: *mut RaftServerHandle);
    fn raft_snapshot_manager_is_set(
        manager: *const rusty::RaftSnapshotManagerPtr) -> bool;
    fn raft_phase1_load_and_send_snapshot(
        server: *mut RaftServerHandle,
        snapshot_manager: *const rusty::RaftSnapshotManagerPtr,
        lifetime: *const rusty::RaftAsyncCallbackLifetimePtr,
        self_site_id: u16, partition_id: u32, send_term: u64,
        site_id: u16, ord: usize) -> bool;
    fn raft_batch_optimization_enabled() -> bool;
    fn raft_append_entries_batch_max() -> u64;
    fn raft_append_entries_batch_max_bytes() -> u64;
    // [move, M6] raft_command_payload_bytes, raft_command_kind and
    // raft_command_is_tpc_commit are read once per entry into RaftEntry now
    // (raft_command_meta). The leader's batch: a TpcCommitCommand is copied
    // and stamped with its log term in C++ (a Marshallable) by the facade's
    // `raft_stamped_commit`, pushed into batch_buffer_ in Rust, and the
    // buffer's Arcs are moved into one TpcBatchCommand at the end.
    fn raft_batch_finalize(entries: *mut rusty::RaftTpcCommitPtr,
                           count: usize, cmd_out: *mut rusty::RaftCommand);
    // The command copy INTO Rust's slot; see server.h for why never by value.
    fn raft_command_clone_into(src: *const rusty::RaftCommand,
                               dst: *mut rusty::RaftCommand);
    fn raft_phase1_send_append(server: *mut RaftServerHandle,
                               self_site_id: u16, site_id: u16,
                               partition_id: u32, is_leader: bool, term: u64,
                               prev_log_index: u64, prev_log_term: u64,
                               commit_index: u64,
                               cmd: *const rusty::RaftCommand,
                               cmd_log_term: u64,
                               out: *mut rusty::RaftResponsePtr);
}

// ==========================================================================
// THE ROUND SCOPE, AND THE COMMIT RULE BOTH PHASE 0 AND PHASE 3 APPLY
// ==========================================================================

// The commit-index advance, which PHASE 0 and PHASE 3 perform identically:
// PHASE 0 before the round's RPCs go out, PHASE 3 after their replies have
// been processed. It was the same fifteen lines twice.
//
// Returns the range the caller must hand to EnqueueCommittedEntries. The
// caller does the enqueue because that is the apply queue, i.e. I/O; the
// decision is here.
#[repr(C)]
pub struct CommitAdvance {
    advanced_: bool,
    from_: u64,
    to_: u64,
}

impl CommitAdvance {
    pub fn advanced(&self) -> bool {
        self.advanced_
    }

    pub fn from_index(&self) -> u64 {
        self.from_
    }

    pub fn to_index(&self) -> u64 {
        self.to_
    }
}

// Same aliasing note as heartbeat_phase3_locked: peers and log are reached
// through `consensus` because both are its fields.
pub fn raft_commit_advance(
    consensus: &mut RaftCore,
    nservers: usize,
) -> CommitAdvance {
    // nservers is the value latched in PHASE 0. Reusing it in PHASE 3 is
    // sound only because current_config_ has exactly one write, during
    // Setup, and progress_ is never erased, so the size is invariant across
    // the round. Assert it rather than trusting the phases to stay in step.
    // Peer table and round membership agree.
    assert!(consensus.peers_.len() == nservers - 1);  // [move, M10]
    let candidate_index = consensus.peers_
        .majority_match_index(nservers, consensus.raft_log_.last_index());
    if !raft_server_log_index_above(candidate_index, consensus.commit_index_) {
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    // The candidate is <= last_index() and > commit_index_, so the entry
    // provably exists. This says so rather than leaving a null dereference
    // to express it.
    let candidate = consensus.raft_log_.get(candidate_index);
    // The committable index is present in the log.
    assert!(candidate.is_some());  // [move, M10]
    if !raft_server_log_entry_is_current_term(
        candidate.unwrap().term(),
        consensus.current_term_,
    ) {
        // Raft commits a prior-term entry only via one from the current term.
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    let from = consensus.commit_index_;
    consensus.commit_index_ = candidate_index;
    unsafe { raft_trace_through(8, candidate_index, 0) };  // [M0] trace kit
    CommitAdvance { advanced_: true, from_: from, to_: candidate_index }
}

// ==========================================================================
// PHASE 0 -- advance the read-index round, recompute the commit index
// ==========================================================================

#[repr(C)]
pub struct Phase0Outcome {
    restart_: bool,
    commit_advanced_: bool,
    commit_from_: u64,
    commit_to_: u64,
    // [move, M5] The in-flight slots were cleared (resized or abandoned), so
    // the shell clears its response handles to match.
    slots_reset_: bool,
}

impl Phase0Outcome {
    // The round is over before it began -- this server is not the leader.
    // The caller returns, and the driver starts the next round.
    pub fn restart(&self) -> bool {
        self.restart_
    }

    pub fn commit_advanced(&self) -> bool {
        self.commit_advanced_
    }

    pub fn commit_from(&self) -> u64 {
        self.commit_from_
    }

    pub fn commit_to(&self) -> u64 {
        self.commit_to_
    }

    pub fn slots_reset(&self) -> bool {
        self.slots_reset_
    }
}

// TODO(raft-server-struct): remove this allow once RaftServer is itself a DSL
// struct. The ten parameters are exactly the pieces of RaftServer's state that
// PHASE 0 touches; they are separate arguments only because the orphan-impl
// rule forbids `impl RaftServer`, so this cannot yet be a method taking
// &mut self. Grouping them into a carrier struct now would invent a type whose
// only purpose is to be dissolved by that change. Before removing the allow,
// verify the parameter list really has collapsed into self rather than being
// hidden behind a wrapper.
#[allow(clippy::too_many_arguments)]
// Same aliasing note as heartbeat_phase3_locked.
pub fn heartbeat_phase0_locked(
    core: &mut RaftCore,  // [move, M2] the round state is core's now
    members: &[u16],
    site_id: u16,
    is_leader: bool,
) -> Phase0Outcome {
    if !is_leader {
        core.pending_rpcs_.abandon();
        core.authority_rounds_.abandon();
        core.pending_leader_term_ = rusty::None;
        return Phase0Outcome {
            restart_: true,
            commit_advanced_: false,
            commit_from_: 0,
            commit_to_: 0,
            slots_reset_: true,  // [move, M5]
        };
    }
    let mut slots_reset: bool = false;  // [move, M5]

    core.round_.begin(core.current_term_, core.heartbeat_round_);

    // Sized here rather than in the prologue because the round state is the
    // loop's, not the server's. Idempotent: resize only runs when the two
    // tables disagree, so in-flight slots survive every later round.
    if core.pending_rpcs_.len() != core.peers_.len() {
        core.pending_rpcs_.resize(core.peers_.len());
        slots_reset = true;  // [move, M5]
    }

    // Leadership may be lost and regained between two observations by this
    // fiber. Never let a prior term's physical RPC occupy a slot or collide
    // with the new leader epoch's round counter reset.
    let epoch_changed = core.pending_leader_term_.is_none()
        || *core.pending_leader_term_.as_ref().unwrap() != core.round_.term();
    if epoch_changed {
        core.pending_rpcs_.abandon();
        core.authority_rounds_.abandon();
        core.pending_leader_term_ = rusty::Some(core.round_.term());
        slots_reset = true;  // [move, M5]
    }

    if raft_server_read_index_round_can_advance(core.heartbeat_round_) {
        core.heartbeat_round_ += 1;
    }
    // Saturation is fail-closed for new reads: the round never wraps, so no
    // post-baseline proof can be forged from an old generation. The caller
    // reports it; see round_saturated below.

    let mut i = 0;
    while i < members.len() {
        core.round_.admit(members[i]);
        i += 1;
    }
    // The heartbeat round admitted a quorum containing this site.
    assert!(core.round_.nservers() != 0 && core.round_.is_member(site_id));  // [move, M10]
    // [move, M2] read before core is lent whole
    let nservers: usize = core.round_.nservers();
    let advance = raft_commit_advance(core, nservers);
    core.round_.publish_commit_index(core.commit_index_);

    Phase0Outcome {
        restart_: false,
        commit_advanced_: advance.advanced(),
        commit_from_: advance.from_index(),
        commit_to_: advance.to_index(),
        slots_reset_: slots_reset,  // [move, M5]
    }
}

// Whether PHASE 0 declined to advance the read-index generation because the
// counter is saturated. Split out so the caller can log it at ERROR without
// the DSL body paying for an unconditional log_line every round.
pub fn heartbeat_round_saturated(round_counter: u64) -> bool {
    !raft_server_read_index_round_can_advance(round_counter)
}

// ==========================================================================
// THE HEARTBEAT ROUND, CUT AT ITS SUSPENSIONS ([move, M5], Phase 3)
//
// One round is three kinds of core call, each made under mtx_, none of
// which waits:
//   heartbeat_tick       PHASE 0, and PHASE 1's decisions: the round opens,
//                        the commit rule runs, and each follower's
//                        AppendEntries is chosen (prev, the commit index, the
//                        entries' handles) and its in-flight slot placed.
//   heartbeat_on_reply   one completed reply: PHASE 2's per-slot body.
//   heartbeat_round_end  PHASE 3.
// The shell keeps what waits or does I/O: the wake, the InstallSnapshot
// kernel, building each payload from the handles the core copied out and
// sending it ([fix, F7]), the response handles, and PHASE 2's 1 ms poll loop
// with its round deadline and early-quorum exit.
//
// The round members no longer come from a std::vector rebuilt out of
// current_config_ every round; they are core.config_members_, filled once
// during Setup. Same contents, sorted and duplicate-free, which is what both
// the round's membership and the ledger's set-equality check expect.
// ==========================================================================

/// How one follower's AppendEntries carries its payload.
#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum AppendPayload {
    // No entries: the RPC proves only prev.
    HEARTBEAT = 0,
    // One entry, sent as its own command: batching is off, or the next entry
    // is not a TpcCommitCommand.
    RAW_ENTRY = 1,
    // TpcCommitCommands, each stamped with its log term and finalized into
    // one TpcBatchCommand by the shell, after the guard ([fix, F7]).
    BATCH = 2,
}

/// One follower's AppendEntries, as heartbeat_tick decided it under mtx_.
/// The command handles were copied out under the guard, so the shell never
/// reads the log to send this.
pub struct AppendSend {
    ord_: usize,
    site_id_: u16,
    payload_: AppendPayload,
    term_: u64,
    prev_log_index_: u64,
    prev_log_term_: u64,
    // [fix, F3] read in the same call that chose prev and the entries.
    commit_index_: u64,
    // RAW_ENTRY: the entry's log term; otherwise 0, as the C++ sent it.
    entry_term_: u64,
    sent_end_index_: u64,
    sent_round_: u64,
    // RAW_ENTRY: the one command; BATCH: one per entry, with its term.
    cmds_: rusty::Vec<rusty::RaftCommand>,
    terms_: rusty::Vec<i64>,
}

impl AppendSend {
    fn heartbeat(ord: usize, site_id: u16, term: u64, prev_log_index: u64,
                 commit_index: u64, sent_round: u64) -> AppendSend {
        AppendSend {
            ord_: ord,
            site_id_: site_id,
            payload_: AppendPayload::HEARTBEAT,
            term_: term,
            prev_log_index_: prev_log_index,
            prev_log_term_: 0,
            commit_index_: commit_index,
            entry_term_: 0,
            // Until a payload is selected this is a heartbeat, and proves
            // only the prefix named by prev_log_index.
            sent_end_index_: raft_server_append_sent_end(prev_log_index, 0),
            sent_round_: sent_round,
            cmds_: rusty::Vec::new(),
            terms_: rusty::Vec::new(),
        }
    }
}

/// An InstallSnapshot heartbeat_tick asked for: the follower is behind the
/// log's base. The shell runs the kernel before the guard drops, where the
/// per-follower section ran it.
pub struct SnapshotSend {
    ord_: usize,
    site_id_: u16,
    term_: u64,
}

/// What heartbeat_tick decided.
pub struct HeartbeatTick {
    // Not leading: the round ends here (bugs-found B12).
    declined_: bool,
    // The core cleared its in-flight slots (resized, or abandoned on a lost
    // or changed leadership epoch); the shell clears its handles to match.
    slots_reset_: bool,
    slot_count_: usize,
    round_id_: u64,
    // Whether the round's authority generation already has a quorum.
    has_authority_: bool,
    snapshots_: rusty::Vec<SnapshotSend>,
    sends_: rusty::Vec<AppendSend>,
}

impl HeartbeatTick {
    fn new() -> HeartbeatTick {
        HeartbeatTick {
            declined_: false,
            slots_reset_: false,
            slot_count_: 0,
            round_id_: 0,
            has_authority_: false,
            snapshots_: rusty::Vec::new(),
            sends_: rusty::Vec::new(),
        }
    }
}

// [move, M11] A command handle's copy: a refcount bump on its inner Arc,
// made by the kernel, never the carrier's clone (see server.h). The handle is
// opaque to the core; this copies it without looking inside.
fn raft_command_handle_clone(cmd: &rusty::RaftCommand) -> rusty::RaftCommand {
    let mut copy: rusty::RaftCommand = Default::default();
    unsafe {
        raft_command_clone_into(cmd as *const rusty::RaftCommand,
                                &mut copy as *mut rusty::RaftCommand);
    }
    copy
}

// [move, M5] heartbeat_phase1_select_payload's decisions: what this
// AppendEntries carries -- nothing (a heartbeat), one raw entry, or a batch
// of TpcCommitCommands -- with the entries' handles copied into `send`.
// Returns true when the follower must be skipped. The payload is built from
// those handles after the guard ([fix, F7]); everything this reads about an
// entry is RaftEntry's cached metadata (M6).
//
// The two arms were an #ifdef RAFT_BATCH_OPTIMIZATION / #ifndef pair. They
// are an if/else on a constant now, because the preprocessor has no DSL
// spelling; the compiler folds the dead arm exactly as the preprocessor
// removed it.
#[allow(clippy::too_many_arguments)]
fn heartbeat_select_payload(core: &mut RaftCore, ord: usize, site_id: u16,
                            prev_log_index: u64, batching: bool,
                            max_batch_entries: u64, max_batch_bytes: u64,
                            send: &mut AppendSend) -> bool {
    let mut skip_follower: bool = false;

    if !batching {
        rusty::raft_log_debug_5(
            "[BATCH_CHECK] site={} follower={} next_index={} core.raft_log_.base()={} core.raft_log_.last_index()={}",
            core.site_id_, site_id, core.peers_.next_index(ord),
            core.raft_log_.base(),
            core.raft_log_.last_index());
        if core.peers_.next_index(ord) <= core.raft_log_.last_index() {
            if !raft_server_append_entry_count_fits(prev_log_index, 1) {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-SEND] Log index exhausted after {}, skipping follower {}",
                    prev_log_index, site_id);
                skip_follower = true;
            } else {
                let next: u64 = core.peers_.next_index(ord);
                let slot = core.raft_log_.get(next);
                let usable: bool = slot.is_some() && slot.unwrap().has_value();
                if !usable {
                    rusty::raft_log_error_2(
                        "[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                        next, site_id);
                    skip_follower = true;
                } else {
                    let entry: &RaftEntry = slot.unwrap();
                    send.entry_term_ = entry.term() as u64;
                    send.cmds_.push(raft_command_handle_clone(entry.cmd()));
                    send.payload_ = AppendPayload::RAW_ENTRY;
                    send.sent_end_index_ =
                        raft_server_append_sent_end(prev_log_index, 1);
                    // The kind tag identifies the payload better than the
                    // inner shared_ptr's raw address ever did.
                    let kind: i32 = entry.kind();
                    rusty::raft_log_debug_4(
                        "[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        core.site_id_, next, site_id, kind);
                }
            }
        }
        return skip_follower;
    }

    // The batch is bounded by BYTES too: the transport refuses a frame past
    // its 64 MiB limit, and an entry count alone lets large entries exceed
    // it. `batched` counts the TpcCommitCommands taken, which is what the
    // C++'s buffer length counted.
    let mut batched: u64 = 0;
    let mut batch_bytes: u64 = 0;
    let batch_start_idx: u64 = core.peers_.next_index(ord);
    rusty::raft_log_debug_5(
        "[BATCH_CHECK] site={} follower={} next_index={} core.raft_log_.base()={} core.raft_log_.last_index()={}",
        core.site_id_, site_id, core.peers_.next_index(ord),
        core.raft_log_.base(), core.raft_log_.last_index());
    if !raft_server_append_entry_count_fits(prev_log_index, 1) {
        rusty::raft_log_error_2(
            "[HEARTBEAT-BATCH] Log index exhausted after {}, skipping follower {}",
            prev_log_index, site_id);
        skip_follower = true;
    }
    let first_encoded_index: u64 = if skip_follower {
        0
    } else {
        raft_server_append_sent_end(prev_log_index, 1)
    };
    if !skip_follower
        && (batch_start_idx != first_encoded_index
            || batch_start_idx < core.raft_log_.base())
    {
        rusty::raft_log_error_4(
            "[HEARTBEAT-BATCH] Non-contiguous source for follower {}: prev={} start={} min_active={}; refusing to compress a hole",
            site_id, prev_log_index, batch_start_idx,
            core.raft_log_.base());
        skip_follower = true;
    } else if !skip_follower {
        let mut idx: u64 = batch_start_idx;
        while idx <= core.raft_log_.last_index() && batched < max_batch_entries {
            let entry = core.raft_log_.get(idx);
            let usable: bool = entry.is_some() && entry.unwrap().has_value();
            if !usable {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-BATCH] Missing log entry {} for follower {}; refusing to compress a hole",
                    idx, site_id);
                skip_follower = true;
                break;
            }
            let entry_term: i64 = entry.unwrap().term();
            // Stop before this entry would push the batch past the byte bound
            // -- but always carry at least one, so progress never stalls.
            let entry_bytes: u64 = entry.unwrap().payload_bytes();
            if batched != 0
                && batch_bytes.saturating_add(entry_bytes) > max_batch_bytes
            {
                break;
            }
            batch_bytes = batch_bytes.saturating_add(entry_bytes);
            if entry.unwrap().is_tpc_commit() {
                send.cmds_.push(raft_command_handle_clone(entry.unwrap().cmd()));
                send.terms_.push(entry_term);
                batched += 1;
            } else {
                let kind: i32 = entry.unwrap().kind();
                if batched == 0 {
                    rusty::raft_log_info_3(
                        "[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log",
                        core.site_id_, idx, kind);
                    send.cmds_.push(raft_command_handle_clone(entry.unwrap().cmd()));
                    send.payload_ = AppendPayload::RAW_ENTRY;
                    send.entry_term_ = entry_term as u64;
                    send.sent_end_index_ =
                        raft_server_append_sent_end(prev_log_index, 1);
                } else {
                    rusty::raft_log_info_3(
                        "[BATCH_STOP] site={} idx={}: ending batch before non-TpcCommitCommand kind={}",
                        core.site_id_, idx, kind);
                }
                break;
            }
            if !raft_server_log_index_has_successor(idx) {
                break;
            }
            idx += 1;
        }
    }

    if !skip_follower && batched > 0
        && !raft_server_append_batch_count_is_valid(prev_log_index, batched)
    {
        rusty::raft_log_error_3(
            "[HEARTBEAT-BATCH] Invalid encoded count {} after previous index {}; skipping follower {}",
            batched, prev_log_index, site_id);
        skip_follower = true;
    }
    if !skip_follower && batched > 0 {
        send.payload_ = AppendPayload::BATCH;
        send.sent_end_index_ = raft_server_append_sent_end(prev_log_index,
                                                           batched);
        let batch_end_idx: u64 = send.sent_end_index_;
        let truncated: bool = batch_end_idx < core.raft_log_.last_index();
        rusty::raft_log_info_6(
            "[BATCH_SEND] site={} sending batch of {} entries to follower {} (from={} to={}{})",
            core.site_id_, batched, site_id, batch_start_idx,
            batch_end_idx, if truncated { ", truncated" } else { "" });
    }
    skip_follower
}

// [move, M5] PHASE 0 and PHASE 1's decisions, in one core call under mtx_.
// The caller passes what it reads outside the core: whether it leads
// (IsLeaderLocked, which also reads looping_), whether a snapshot manager is
// set, and the batching configuration. The commit range PHASE 0 advanced is
// an APPLY_RANGE action in `out`.
//
// PHASE 1 used to release mtx_ between followers, re-check IsLeader() before
// each, and send each RPC before choosing the next. Here every follower is
// chosen in one call, so leadership cannot change between two of them; the
// F3 check stays, and the sends follow in the same follower order once the
// guard drops.
// unnecessary_unwrap: `is_none` then `unwrap` is the shape PHASE 1 always
// had, and `if let` is not an option here: the emitter renders its binding
// with a dot where the C++ needs an arrow.
#[allow(clippy::too_many_arguments, clippy::unnecessary_unwrap)]
pub fn heartbeat_tick(core: &mut RaftCore, is_leader: bool,
                      snapshot_configured: bool, batching: bool,
                      max_batch_entries: u64, max_batch_bytes: u64,
                      out: &mut CoreOutput) -> HeartbeatTick {
    let mut tick: HeartbeatTick = HeartbeatTick::new();

    // ---- PHASE 0 ----
    if is_leader && heartbeat_round_saturated(core.heartbeat_round_) {
        rusty::raft_log_error_2(
            "[READ-INDEX] site={} heartbeat round saturated in term {}",
            core.site_id_, core.current_term_);
    }
    if is_leader {
        let mut ord: usize = 0;
        while ord < core.peers_.len() {
            rusty::raft_log_debug_2(
                "[COMMIT-CALC] match_index_[{}] = {}",
                core.peer_site_at(ord),
                core.peers_.match_index(ord));
            ord += 1;
        }
    }

    let site_id: u16 = core.site_id_;
    let members: rusty::Vec<u16> = core.config_members_.clone();
    let outcome: Phase0Outcome = heartbeat_phase0_locked(
        core, &members, site_id, is_leader);
    tick.slots_reset_ = outcome.slots_reset();
    tick.slot_count_ = core.pending_rpcs_.len();
    if outcome.restart() {
        tick.declined_ = true;
        return tick;
    }
    if outcome.commit_advanced() {
        out.push(CoreAction::apply_range(outcome.commit_from(),
                                         outcome.commit_to()));
    }

    // The authority generation, opened under the guard now (the fiber that
    // owns the round state used to open it after releasing mtx_).
    let opened: bool = core.authority_rounds_.open(
        core.round_.round_id(), &members,
        HeartbeatAuthority::new(core.round_.term(), core.round_.nservers(),
                                site_id));
    core.round_.set_authority_inserted(opened);
    // heartbeat_round_ never wraps. The only possible duplicate is the
    // deliberately fail-closed UINT64_MAX saturation generation, which
    // open() declines rather than overwriting.
    if !core.round_.authority_inserted() {
        assert!(core.round_.round_id() == u64::MAX);  // [move, M10]
    }
    tick.round_id_ = core.round_.round_id();

    // ---- PHASE 1's decisions ----
    //
    // The ordinal is used for ITERATION ONLY; every read and write of a
    // follower's next index goes through core.peers_.next_index(ord).
    let mut ord: usize = 0;
    while ord < core.peers_.len() {
        let peer: u16 = core.peer_site_at(ord);
        if peer == site_id {
            ord += 1;
            continue;
        }
        if core.pending_rpcs_.occupied(ord) {
            ord += 1;
            continue;
        }
        // [fix, F3] Only a server still leading in the round's term may send
        // that term's append, built from the log it holds now.
        if !core.is_leader_ || core.current_term_ != core.round_.term() {
            break;
        }
        let send_commit_index: u64 = core.commit_index_;
        if core.peers_.next_index(ord) == 0 {
            rusty::raft_log_warn_2(
                "[APPEND_ENTRIES] Repairing wrapped next_index for follower {} at leader last index {}",
                peer, core.raft_log_.last_index());
            let last: u64 = core.raft_log_.last_index();
            let repaired: u64 = if raft_server_log_index_has_successor(last) {
                raft_server_follower_next_index(last)
            } else {
                last
            };
            core.peers_.set_next_index(ord, repaired);
        }
        let mut prev_log_index: u64 = core.peers_.next_index(ord) - 1;
        if prev_log_index > core.raft_log_.last_index() {
            rusty::raft_log_info_2(
                "[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > core.raft_log_.last_index() ({}), fixing next_index",
                prev_log_index, core.raft_log_.last_index());
            let last: u64 = core.raft_log_.last_index();
            let repaired: u64 = if raft_server_log_index_has_successor(last) {
                raft_server_follower_next_index(last)
            } else {
                last
            };
            core.peers_.set_next_index(ord, repaired);
            prev_log_index = core.peers_.next_index(ord) - 1;
        }
        let mut send: AppendSend = AppendSend::heartbeat(
            ord, peer, core.round_.term(), prev_log_index, send_commit_index,
            core.round_.round_id());

        let mut skip_follower: bool = false;
        if prev_log_index > core.raft_log_.last_index() {
            rusty::raft_log_info_3(
                "[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > core.raft_log_.last_index() ({}), skipping",
                peer, prev_log_index, core.raft_log_.last_index());
            core.peers_.set_next_index(ord, 1);
            skip_follower = true;
        } else if core.peers_.next_index(ord) < core.raft_log_.base()
            && snapshot_configured
        {
            // The follower is behind the log's base, so send it a snapshot
            // instead of entries it can no longer be given. Sent or not,
            // the normal AppendEntries is skipped for this follower.
            rusty::raft_log_info_4(
                "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < core.raft_log_.base()={}, sending InstallSnapshot",
                site_id, peer, core.peers_.next_index(ord),
                core.raft_log_.base());
            tick.snapshots_.push(SnapshotSend {
                ord_: ord,
                site_id_: peer,
                term_: core.current_term_,
            });
            skip_follower = true;
        } else {
            assert!(prev_log_index <= core.raft_log_.last_index());  // [move, M10]
            if prev_log_index == 0 {
                send.prev_log_term_ = 0;
            } else if prev_log_index == core.snapidx_ && core.snapidx_ > 0 {
                // Keep using snapshot boundary metadata after compaction.
                send.prev_log_term_ = core.snapterm_ as u64;
            } else {
                // Was GetRaftInstance, which default-inserted and so could
                // never return null -- the check below was dead, and a
                // genuinely missing prevLogIndex silently fabricated an empty
                // entry with term 0 and sent prevLogTerm = 0 rather than
                // skipping the follower.
                let instance = core.raft_log_.get(prev_log_index);
                if instance.is_none() {
                    rusty::raft_log_error_2(
                        "[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}",
                        prev_log_index, peer);
                    skip_follower = true;
                } else {
                    send.prev_log_term_ = instance.unwrap().term() as u64;
                }
            }
            if !skip_follower {
                skip_follower = heartbeat_select_payload(
                    core, ord, peer, prev_log_index, batching,
                    max_batch_entries, max_batch_bytes, &mut send);
            }
        }
        if skip_follower {
            ord += 1;
            continue;
        }

        // The slot's protocol half; the shell adds the response handle when
        // it sends.
        let has_entries: bool = send.payload_ != AppendPayload::HEARTBEAT;
        core.pending_rpcs_.place(ord, PendingAppend::new(
            peer, core.round_.term(), core.round_.round_id(),
            send.sent_end_index_, has_entries));
        if core.round_.authority_inserted() && core.round_.is_member(peer) {
            // Was a std::map iterator created in PHASE 0 and dereferenced
            // after the RPC sends; look it up by key.
            // [move, M10] The launch is a write, so it is not made inside the
            // assert's condition.
            let launched: bool =
                core.authority_rounds_.launch(core.round_.round_id(), peer);
            assert!(launched);
        }
        tick.sends_.push(send);
        ord += 1;
    }
    tick.has_authority_ =
        core.authority_rounds_.has_quorum(core.round_.round_id());
    tick
}

// [move, M5] PHASE 0 and PHASE 1 around their core call: the decision under
// mtx_ and its actions (with any InstallSnapshot kernel) before the guard
// drops, then each AppendEntries built and sent in follower order
// ([fix, F7]). Returns the tick for the rest of the round.
pub fn heartbeat_tick_body(server: &mut RaftServerBase) -> HeartbeatTick {
    let mut out: CoreOutput = CoreOutput::new();
    let tick: HeartbeatTick = {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let is_leader: bool = server.IsLeaderLocked();
        let snapshot_configured: bool = unsafe {
            raft_snapshot_manager_is_set(&server.snapshot_manager_)
        };
        let batching: bool = unsafe { raft_batch_optimization_enabled() };
        let max_batch_entries: u64 = unsafe { raft_append_entries_batch_max() };
        let max_batch_bytes: u64 = unsafe { raft_append_entries_batch_max_bytes() };
        let decided: HeartbeatTick = heartbeat_tick(
            &mut server.core, is_leader, snapshot_configured, batching,
            max_batch_entries, max_batch_bytes, &mut out);
        server.run_locked_actions(&out);
        // The InstallSnapshot kernel keeps its place under the guard. Its
        // completion callback takes the SAME mutex, which is why PHASE 1
        // reaches followers by ordinal and never holds a reference across it.
        let mut i: usize = 0;
        while i < decided.snapshots_.len() {
            let snapshot: &SnapshotSend = &decided.snapshots_[i];
            let sent: bool = unsafe {
                raft_phase1_load_and_send_snapshot(
                    server.handle(),
                    &server.snapshot_manager_
                        as *const rusty::RaftSnapshotManagerPtr,
                    &server.async_callback_lifetime_
                        as *const rusty::RaftAsyncCallbackLifetimePtr,
                    server.site_id_, server.partition_id_, snapshot.term_,
                    snapshot.site_id_, snapshot.ord_)
            };
            if !sent {
                rusty::raft_log_warn_2(
                    "[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                    server.site_id_, snapshot.site_id_);
            }
            i += 1;
        }
        decided
    };
    server.run_unlocked_actions(&out);
    if tick.slots_reset_ {
        server.append_responses_.reset(tick.slot_count_);
    }
    if tick.declined_ {
        return tick;
    }

    // [fix, F7] Each payload is built from the handles the core copied out
    // under the guard -- a batch's entries stamped with their log terms and
    // finalized into one TpcBatchCommand -- and sent, in follower order.
    let partition_id: u32 = server.partition_id_;
    let mut i: usize = 0;
    while i < tick.sends_.len() {
        let send: &AppendSend = &tick.sends_[i];
        // An empty Command (has_value() == false) signals a heartbeat.
        let mut cmd: rusty::RaftCommand = Default::default();
        if send.payload_ == AppendPayload::RAW_ENTRY {
            cmd = raft_command_handle_clone(&send.cmds_[0]);
        } else if send.payload_ == AppendPayload::BATCH {
            // A fresh buffer per follower, as the C++ local was.
            server.batch_buffer_.clear();
            let mut k: usize = 0;
            while k < send.cmds_.len() {
                let stamped: rusty::RaftTpcCommitPtr = unsafe {
                    rusty::raft_stamped_commit(
                        &send.cmds_[k] as *const rusty::RaftCommand,
                        send.terms_[k])
                };
                server.batch_buffer_.push(stamped);
                k += 1;
            }
            unsafe {
                raft_batch_finalize(server.batch_buffer_.as_mut_ptr(),
                                    server.batch_buffer_.len(),
                                    &mut cmd as *mut rusty::RaftCommand);
            }
        }
        let mut sent_response: rusty::RaftResponsePtr = Default::default();
        unsafe { raft_trace_through(3, send.sent_end_index_, 0) };  // [M0] trace kit
        unsafe {
            raft_phase1_send_append(
                server.handle(), server.site_id_, send.site_id_, partition_id,
                true, send.term_, send.prev_log_index_, send.prev_log_term_,
                send.commit_index_,  // [fix, F3]
                &cmd as *const rusty::RaftCommand, send.entry_term_,
                &mut sent_response as *mut rusty::RaftResponsePtr);
        }
        unsafe { raft_trace_through(4, send.sent_end_index_, 0) };  // [M0] trace kit
        server.append_responses_.place(send.ord_, sent_response,
                                       send.sent_round_);
        i += 1;
    }
    tick
}

// ==========================================================================
// PHASE 2 -- poll the replies through one round deadline and apply them
// ==========================================================================

// PHASE 2's decision core: what one AppendEntries reply means.
//
// PHASE 2 is a polling loop over the in-flight slots. The loop itself, its
// round deadline and its Fiber::sleep stay in C++ -- suspension is the one
// thing genuinely shaped by the fiber runtime. What each reply MEANS is not,
// and that is this.
//
// The wire reply is read out by the caller and arrives here as three scalars.
// That is the same "convert at the edge" split the rest of the file uses: the
// srpc response object never crosses, only what it says.
//
// The caller keeps four things because none of them are decisions:
// LogTermChange and the backoff-rung logging (both pure logging, and both
// need their level short-circuit), PeerOrdinal (a scan over peer_sites_,
// which is C++), and stepDown -- which reaches setIsLeader and the election
// timer, i.e. the reactor. This returns STEP_DOWN and lets the caller do it.
#[repr(C)]
pub struct SentAppend {
    follower_: u16,
    term_: u64,
    round_: u64,
    end_index_: u64,
    // peers.len() when this follower is no longer a peer at all.
    ordinal_: usize,
}

impl SentAppend {
    pub fn new(follower: u16, term: u64, round: u64, end_index: u64, ordinal: usize) -> SentAppend {
        SentAppend { follower_: follower, term_: term, round_: round,
                     end_index_: end_index, ordinal_: ordinal }
    }

    pub fn round(&self) -> u64 {
        self.round_
    }
}

#[repr(C)]
pub struct AppendReply {
    available_: bool,
    status_: bool,
    term_: u64,
    last_log_index_: u64,
}

impl AppendReply {
    pub fn new(available: bool, status: bool, term: u64, last_log_index: u64) -> AppendReply {
        AppendReply { available_: available, status_: status, term_: term,
                      last_log_index_: last_log_index }
    }
}

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[repr(i32)]
pub enum AppendReplyAction {
    // Nothing was learned: the RPC failed, or the reply belongs to a term or
    // a leadership epoch that is no longer current.
    IGNORED = 0,
    // The follower proved a newer term. Term, vote and leader hint have been
    // updated here; the caller must perform the step-down itself.
    STEP_DOWN = 1,
    // Rejected. The backoff ladder ran and reports which rung it took.
    BACKED_OFF = 2,
    // Accepted, and replication progress advanced.
    ACCEPTED = 3,
    // Success that does not cover the payload it was sent. AppendEntries
    // acceptance is atomic, so this proves nothing and must not be counted.
    CONTRADICTORY = 4,
    // The follower has no replication indices: it is not one this server
    // leads. Higher-term evidence was already handled above.
    UNKNOWN_FOLLOWER = 5,
}

#[repr(C)]
pub struct AppendReplyOutcome {
    action_: AppendReplyAction,
    rung_: BackoffKind,
    old_next_: u64,
    new_next_: u64,
    acknowledged_: u64,
    previous_term_: u64,
}

impl AppendReplyOutcome {
    pub fn action(&self) -> AppendReplyAction {
        self.action_
    }

    // BACKED_OFF only.
    pub fn rung(&self) -> BackoffKind {
        self.rung_
    }

    pub fn old_next(&self) -> u64 {
        self.old_next_
    }

    pub fn new_next(&self) -> u64 {
        self.new_next_
    }

    // ACCEPTED only.
    pub fn acknowledged(&self) -> u64 {
        self.acknowledged_
    }

    // STEP_DOWN only: the term this server held before the reply displaced it.
    pub fn previous_term(&self) -> u64 {
        self.previous_term_
    }
}

fn append_reply_nothing(action: AppendReplyAction) -> AppendReplyOutcome {
    AppendReplyOutcome {
        action_: action,
        rung_: BackoffKind::FLOOR,
        old_next_: 0,
        new_next_: 0,
        acknowledged_: 0,
        previous_term_: 0,
    }
}

// Takes the log's tail rather than the log: this decides what a reply
// proves, and the only thing it needs from the log is where the log ends.
// Narrowing the parameter is also what keeps the argument list inside
// clippy's limit without an allow.
// `peers` is reached through `consensus` rather than passed alongside it.
// The C++ call site passed `core` and `core.peers_` as two arguments --
// two mutable borrows of overlapping state, which only compiled because the
// caller was C++. A Rust caller cannot spell that, and PHASE 2 is a Rust
// caller now.
pub fn heartbeat_apply_append_reply(
    core: &mut RaftCore,  // [move, M2] the ledger is core's now
    sent: &SentAppend,
    reply: &AppendReply,
    log_last_index: u64,
    is_leader: bool,
) -> AppendReplyOutcome {
    // Retire the RPC and, if it proves this exact generation, count the vote.
    // One physical RPC exists per follower per generation, but the evidence
    // stays a set so a future transport still cannot double-count a voter.
    let evidence = AuthorityReply::new(
        sent.round_,
        sent.follower_,
        sent.term_,
        reply.term_,
        core.current_term_,
        is_leader,
        reply.available_,
    );
    core.authority_rounds_.record_reply(&evidence);

    if !reply.available_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }

    // A higher term is authoritative regardless of the accompanying status
    // bit. The responding follower proves a newer term, not its leader.
    if raft_server_observed_higher_term(reply.term_, core.current_term_) {
        let previous_term = core.current_term_;
        core.current_term_ = reply.term_;
        core.vote_for_ = u16::MAX;
        // Neither leading nor knowing a leader, so the hint is cleared. The
        // responding follower proved a newer term, not that it is the leader
        // of that term. (With both flags false the shared predicate returns
        // the invalid id whatever ids it is handed, so it is spelled out
        // here rather than called with two arguments that do not matter.)
        core.current_leader_id_ = RAFT_SERVER_INVALID_SITE_ID;
        let mut out = append_reply_nothing(AppendReplyAction::STEP_DOWN);
        out.previous_term_ = previous_term;
        return out;
    }

    // A reply from a send term this server has left proves nothing about now.
    if core.current_term_ != sent.term_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    // A valid follower processes AppendEntries in the leader's term before
    // replying, so a lower response term cannot prove this send.
    if reply.term_ != sent.term_ {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    if !is_leader {
        return append_reply_nothing(AppendReplyAction::IGNORED);
    }
    if sent.ordinal_ == core.peers_.len() {
        return append_reply_nothing(AppendReplyAction::UNKNOWN_FOLLOWER);
    }

    if !reply.status_ {
        let old_next = core.peers_.next_index(sent.ordinal_);
        let rung = core.peers_
            .back_off_after_reject(sent.ordinal_, reply.last_log_index_);
        let new_next = core.peers_.next_index(sent.ordinal_);
        let mut out = append_reply_nothing(AppendReplyAction::BACKED_OFF);
        out.rung_ = rung;
        out.old_next_ = old_next;
        out.new_next_ = new_next;
        return out;
    }

    if reply.last_log_index_ < sent.end_index_ {
        return append_reply_nothing(AppendReplyAction::CONTRADICTORY);
    }

    // Successful responses are monotonic and prove no index beyond the exact
    // payload end. In particular a heartbeat cannot adopt an unknown
    // follower suffix.
    let acknowledged = raft_server_append_acknowledged_through(
        reply.last_log_index_, sent.end_index_, log_last_index);
    core.peers_.accept_through(
        sent.ordinal_,
        acknowledged,
        raft_server_log_index_has_successor(acknowledged),
        raft_server_follower_next_index(acknowledged),
    );
    let mut out = append_reply_nothing(AppendReplyAction::ACCEPTED);
    out.acknowledged_ = acknowledged;
    out
}

// What heartbeat_on_reply decided about one reply, for the collection loop.
pub struct ReplyResult {
    stepped_down_: bool,
    // The reply's RPC went out in an earlier round, so its follower's slot
    // opened after PHASE 1 and another round should be prompted.
    completed_previous_round_: bool,
    has_authority_: bool,
}

// [move, M5] PHASE 2's per-slot body, as one core call under mtx_: what the
// completed reply in slot `ord` means, applied, and the slot released. The
// reply arrives as the wire view the shell read out of its handle; `stopped`
// and `failover` are the shell's, for a step-down.
#[allow(clippy::too_many_arguments)]
pub fn heartbeat_on_reply(core: &mut RaftCore, ord: usize,
                          resp: &AppendRespView, is_leader: bool,
                          stopped: bool, failover: bool,
                          out: &mut CoreOutput) -> ReplyResult {
    // Bound once per reply, not per use: every read below is the same shape
    // it was when this was a map value.
    let follower_id: u16 = core.pending_rpcs_.follower(ord);
    let sent_term: u64 = core.pending_rpcs_.sent_term(ord);
    let sent_round: u64 = core.pending_rpcs_.sent_round(ord);
    let sent_end_index: u64 = core.pending_rpcs_.sent_end_index(ord);
    let cmd_has_value: bool = core.pending_rpcs_.has_entries(ord);

    let mut stepped_down: bool = false;
    // What the reply MEANS is heartbeat_apply_append_reply. It reads the wire
    // response as three scalars -- the srpc object itself never crosses --
    // and returns what to do about it.
    let response_available: bool =
        !(!resp.status_ && resp.term_ == 0 && resp.last_log_index_ == 0);
    let resp_ord: usize = core.peer_ordinal(follower_id);
    let log_last_index: u64 = core.raft_log_.last_index();
    let outcome: AppendReplyOutcome = heartbeat_apply_append_reply(
        core,
        &SentAppend::new(follower_id, sent_term, sent_round, sent_end_index,
                         resp_ord),
        &AppendReply::new(response_available, resp.status_, resp.term_,
                          resp.last_log_index_),
        log_last_index,
        is_leader);

    let action: AppendReplyAction = outcome.action();
    if action == AppendReplyAction::STEP_DOWN {
        rusty::raft_log_info_4(
            "[STEPDOWN] Site {}: AppendEntries response from follower {} carried higher term {} > {}",
            core.site_id_, follower_id, resp.term_, outcome.previous_term());
        let now_term: u64 = core.current_term_;
        core.log_term_change("AppendEntries response carried newer term",
                             outcome.previous_term(), now_term, follower_id);
        // The step-down's effects are actions ([move, M3]); the decision to
        // take it was made above.
        core.step_down(stopped, failover, out);
        core.req_voting_ = false;
        core.election_in_progress_ = false;
        stepped_down = true;
    } else if action == AppendReplyAction::BACKED_OFF {
        // The five-rung ladder is FollowerProgress::back_off_after_reject; it
        // reports which rung it took so the diagnostics stay as specific as
        // they were when the branches were inline.
        let rung: BackoffKind = outcome.rung();
        if rung == BackoffKind::FAST {
            rusty::raft_log_info_6(
                "[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})",
                core.site_id_, follower_id, outcome.old_next(),
                outcome.new_next(),
                outcome.old_next() - outcome.new_next(),
                resp.last_log_index_);
        } else if rung == BackoffKind::TERM_CONFLICT {
            rusty::raft_log_info_4(
                "[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}",
                core.site_id_, follower_id, outcome.old_next(),
                outcome.new_next());
        } else if rung == BackoffKind::EXPONENTIAL {
            rusty::raft_log_info_4(
                "[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)",
                core.site_id_, follower_id, outcome.old_next(),
                outcome.new_next());
        } else if rung == BackoffKind::LINEAR {
            rusty::raft_log_debug_4(
                "[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}",
                core.site_id_, follower_id, outcome.old_next(),
                outcome.new_next());
        }
        // BackoffKind::FLOOR logs nothing, as before.
    } else if action == AppendReplyAction::ACCEPTED {
        rusty::raft_log_debug_8(
            "[APPEND_RPC] Leader {} accepted follower {} proof: kind={} reported={} sent_end={} acknowledged={} next={} match={}",
            core.site_id_, follower_id,
            if cmd_has_value { "entries" } else { "heartbeat" },
            resp.last_log_index_, sent_end_index,
            outcome.acknowledged(),
            core.peers_.next_index(resp_ord),
            core.peers_.match_index(resp_ord));
    } else if action == AppendReplyAction::CONTRADICTORY {
        rusty::raft_log_warn_3(
            "[APPEND_RPC] Ignoring contradictory success from follower {}: reported_end={} sent_end={}",
            follower_id, resp.last_log_index_, sent_end_index);
    } else if action == AppendReplyAction::UNKNOWN_FOLLOWER {
        rusty::raft_log_debug_1(
            "[APPEND_RPC] Ignoring replication response from removed follower {}",
            follower_id);
    }
    // AppendReplyAction::IGNORED does nothing, as before.

    let completed_previous_round: bool = sent_round != core.round_.round_id();
    core.pending_rpcs_.release(ord);
    ReplyResult {
        stepped_down_: stepped_down,
        completed_previous_round_: completed_previous_round,
        has_authority_:
            core.authority_rounds_.has_quorum(core.round_.round_id()),
    }
}

// [move, M5] The shell's half of a leadership loss during collection: both
// halves of every in-flight slot, and the authority evidence, dropped.
pub fn heartbeat_abandon_round(core: &mut RaftCore) {
    core.pending_rpcs_.abandon();
    core.authority_rounds_.abandon();
}

// ==========================================================================
// PHASE 2: poll responses through one SHORT round deadline and process them.
//
// Formerly RaftServer::HeartbeatPhase2. Never call wait_timeout on an
// individual response: that permanently marks its event TIMEOUT and loses a
// legitimate late persistence reply. Polling also gives every parallel RPC
// the same bounded round budget.
//
// [move, M5] This is the shell's loop now: it polls its response handles,
// hands each completed reply to heartbeat_on_reply under mtx_, and keeps the
// deadline, the 1 ms step and the early-quorum exit. `round_id` and
// `has_authority` are what the round's tick reported.
// ==========================================================================
#[allow(clippy::manual_clamp)]
pub fn heartbeat_collect_body(server: &mut RaftServerBase, round_id: u64,
                              has_authority: bool) {
    const RESPONSE_POLL_STEP_US: u64 = 1000;
    // max(1, min(100000, heartbeat_interval_us_)). Spelled out rather than
    // with clamp: this lowers to C++, where uint64_t has no such member.
    let response_round_timeout_us: u64 =
        if server.heartbeat_interval_us_ > 100000 {
            100000
        } else if server.heartbeat_interval_us_ < 1 {
            1
        } else {
            server.heartbeat_interval_us_
        };
    let response_deadline_us: u64 =
        unsafe { raft_monotonic_now_us() } + response_round_timeout_us;
    let mut stop_response_processing: bool = false;
    let mut retry_released_follower: bool = false;
    // The ledger changes only in the core calls below, so the round's
    // authority is whatever the last of them reported.
    let mut current_round_has_authority: bool = has_authority;

    while !stop_response_processing {
        let mut waiting_for_current_round: bool = false;
        let mut pending_ord: usize = 0;
        while pending_ord < server.append_responses_.len() {
            if !server.IsLeader() {
                stop_response_processing = true;
                break;
            }
            if !server.append_responses_.occupied(pending_ord) {
                pending_ord += 1;
                continue;
            }
            let sent_round: u64 = server.append_responses_.sent_round(pending_ord);
            let resp: AppendRespView = unsafe {
                raft_append_response_read(
                    server.append_responses_.response(pending_ord)
                        as *const rusty::RaftResponsePtr)
            };
            if !resp.completed_ {
                if sent_round == round_id {
                    waiting_for_current_round = true;
                }
                pending_ord += 1;
                continue;
            }

            let mut out: CoreOutput = CoreOutput::new();
            let reply: ReplyResult = {
                let _lock = RaftLockGuard::new(&mut server.mtx_);
                let is_leader: bool = server.IsLeaderLocked();
                let stopped: bool = server.stopped_now();
                let failover: bool = server.failover_;
                let decided: ReplyResult = heartbeat_on_reply(
                    &mut server.core, pending_ord, &resp, is_leader, stopped,
                    failover, &mut out);
                server.run_locked_actions(&out);
                decided
            };
            server.run_unlocked_actions(&out);  // [fix, F6]
            server.append_responses_.release(pending_ord);
            retry_released_follower =
                retry_released_follower || reply.completed_previous_round_;
            current_round_has_authority = reply.has_authority_;
            if reply.stepped_down_ {
                stop_response_processing = true;
                break;
            }
            pending_ord += 1;
        }

        if stop_response_processing || !waiting_for_current_round
            || current_round_has_authority
        {
            break;
        }
        let now_us: u64 = unsafe { raft_monotonic_now_us() };
        if now_us >= response_deadline_us {
            break;
        }
        let remaining_us: u64 = response_deadline_us - now_us;
        let step_us: u64 = if remaining_us < RESPONSE_POLL_STEP_US {
            remaining_us
        } else {
            RESPONSE_POLL_STEP_US
        };
        unsafe {
            raft_fiber_sleep_us(step_us);
        }
    }

    if stop_response_processing {
        {
            let _lock = RaftLockGuard::new(&mut server.mtx_);
            heartbeat_abandon_round(&mut server.core);
        }
        let slots: usize = server.append_responses_.len();
        server.append_responses_.reset(slots);
    } else if retry_released_follower {
        // A completion from an older round opened a per-follower slot after
        // PHASE 1. Prompt another round instead of waiting a full interval.
        server.RequestReplication();
    }
}

// ==========================================================================
// PHASE 3 -- recompute the commit index and publish read-index authority
// ==========================================================================

// PHASE 3 of the heartbeat round: the whole locked section. Recomputes the
// commit index now that this round's replies have been processed, then
// publishes read-index authority -- deliberately in that order, because a
// delayed reply is evidence for the exact term, generation and membership
// snapshot that launched it and must never be relabelled as the current
// round.
//
// Both halves were already Rust in their parts: raft_commit_advance above
// and AuthorityLedger::settle. This is the body that joins them.
#[repr(C)]
pub struct Phase3Outcome {
    commit_: CommitAdvance,
    confirmed_: bool,
}

impl Phase3Outcome {
    pub fn commit(&self) -> &CommitAdvance {
        &self.commit_
    }

    // True when a read-index generation reached quorum this round, in which
    // case the confirmed term and round have already been stored.
    pub fn confirmed(&self) -> bool {
        self.confirmed_
    }
}

// peers and log are reached through `consensus`, for the same reason
// heartbeat_apply_append_reply's are: the C++ call site passed `core`,
// `core.peers_` and `core.raft_log_` as three arguments, which is one
// mutable borrow overlapping two shared ones. A Rust caller cannot spell it.
pub fn heartbeat_phase3_locked(
    core: &mut RaftCore,  // [move, M2] the ledger is core's now
    nservers: usize,
    members: &[u16],
    is_leader: bool,
) -> Phase3Outcome {
    let commit = raft_commit_advance(core, nservers);
    let outcome = core.authority_rounds_.settle(
        is_leader,
        core.current_term_,
        members,
        core.read_quorum_confirmed_term_,
        core.read_quorum_confirmed_round_,
    );
    let mut confirmed = false;
    if outcome.confirmed() {
        core.read_quorum_confirmed_term_ = outcome.term();
        core.read_quorum_confirmed_round_ = outcome.round_id();
        confirmed = true;
    }
    Phase3Outcome { commit_: commit, confirmed_: confirmed }
}

// [move, M5] PHASE 3 as one core call under mtx_: recompute the commit index
// now that this round's replies have been processed, then publish read-index
// authority -- deliberately in that order, because a delayed reply is
// evidence for the exact term, generation and membership snapshot that
// launched it and must never be relabelled as the current round. The commit
// range is an APPLY_RANGE action. Returns whether the commit index advanced.
pub fn heartbeat_round_end(core: &mut RaftCore, is_leader: bool,
                           out: &mut CoreOutput) -> bool {
    let members: rusty::Vec<u16> = core.config_members_.clone();
    let nservers: usize = core.round_.nservers();
    let outcome: Phase3Outcome = heartbeat_phase3_locked(
        core, nservers, &members, is_leader);  // [move, M2]
    let mut advanced: bool = false;
    if outcome.commit().advanced() {
        rusty::raft_log_debug_2(
            "[PHASE3-COMMIT] Advancing core.commit_index_ {} -> {}",
            outcome.commit().from_index(), outcome.commit().to_index());
        out.push(CoreAction::apply_range(outcome.commit().from_index(),
                                         outcome.commit().to_index()));
        advanced = true;
    }
    if outcome.confirmed() {
        rusty::raft_log_debug_3(
            "[READ-INDEX] site={} confirmed round={} term={}",
            core.site_id_, core.read_quorum_confirmed_round_,
            core.read_quorum_confirmed_term_);
    }
    advanced
}

// PHASE 3 around its core call.
pub fn heartbeat_round_end_body(server: &mut RaftServerBase) {
    if !server.IsLeader() {
        return;
    }
    let mut out: CoreOutput = CoreOutput::new();
    let commit_advanced_after_send: bool = {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let is_leader: bool = server.IsLeaderLocked();
        let advanced: bool = heartbeat_round_end(&mut server.core, is_leader,
                                                 &mut out);
        server.run_locked_actions(&out);
        advanced
    };
    server.run_unlocked_actions(&out);

    // The AppendEntries messages for this round carried the OLD commit
    // index. Latch exactly one prompt follow-up round so followers learn the
    // phase-3 commit without waiting out the periodic heartbeat.
    if commit_advanced_after_send {
        server.RequestReplication();
    }
}

// ==========================================================================
// THE ROUND DRIVER
//
// Sequences the round's three core calls and the waits between them. It
// lives here, beside them, rather than next to RaftServerBase: a DSL block can
// only call what precedes it, and the phase bodies are in this file.
// ==========================================================================

// The driver holds only the server. The round state it used to own
// ([move, M1]) is the core's: RaftCore::pending_rpcs_, authority_rounds_,
// pending_leader_term_ and round_, with the response handles beside them in
// RaftServerBase::append_responses_ ([move, M5]); all reset when a run of the
// loop begins and again when it ends.
pub struct HeartbeatDriver {
    server_: *mut RaftServerBase,
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl HeartbeatDriver {
    pub fn new(server: *mut RaftServerBase) -> HeartbeatDriver {
        HeartbeatDriver { server_: server }
    }

    // decide -> emit -> collect -> decide: the tick (PHASE 0 and 1), the
    // collection loop (PHASE 2) and the round end (PHASE 3).
    pub fn run(&mut self) {
        let server: &mut RaftServerBase = unsafe { &mut *self.server_ };
        // [move, M1] a fresh round state per run, as the driver's own was
        server.core.reset_round_state();
        server.append_responses_ = AppendResponses::new();  // [move, M5]
        server.HeartbeatPrologue();
        while server.HeartbeatLooping() {
            // The wake gate returns false on shutdown rather than on timeout.
            if !server.HeartbeatWait() {
                break;
            }
            // [move, M5] The tick declines the round when leadership is not
            // held, and the driver waits for the next one, as the C++
            // `continue` did (bugs-found B12).
            let tick: HeartbeatTick = heartbeat_tick_body(server);
            if tick.declined_ {
                continue;
            }
            heartbeat_collect_body(server, tick.round_id_, tick.has_authority_);
            heartbeat_round_end_body(server);
        }
        // [move, M1] The in-flight handles are released when the loop ends, as
        // they were when the driver's own round state went out of scope.
        // Before the epilogue, not after: once it reports the loop stopped,
        // shutdown may free the server.
        server.core.reset_round_state();
        server.append_responses_ = AppendResponses::new();  // [move, M5]
        server.HeartbeatEpilogue();
    }
}

// The whole loop, so the C++ side is one fiber spawn rather than a method.
#[allow(clippy::not_unsafe_ptr_arg_deref)]
pub fn heartbeat_loop_body(server: *mut RaftServerBase) {
    let mut driver = HeartbeatDriver::new(server);
    driver.run();
}

// ==========================================================================
// THE C ABI OVER RaftServerBase
//
// One extern "C" function per behaviour the hand-written C++ reaches -- the
// RaftServer shim's interface forwarders and the kernels' callbacks -- each
// forwarding to the method. Generated by scripts/raft_gen_exports.py; the
// matching prototypes are server_exports.h. These are the crate's exported
// symbols, and nothing else of the struct is visible to C++.
// ==========================================================================
use crate::scheduler_h::RaftStartResult;
use crate::scheduler_h::TxLogServer;
use crate::server_h::GateWakeJob;
use crate::server_h::ElectionTimerLoop;
// --- GENERATED EXPORTS BEGIN (scripts/raft_gen_exports.py; do not edit by hand) ---
// --- Lifetime. Rust allocates and frees: the struct is a Box the shim
// holds as a raw pointer between these two calls.
/// # Safety
/// The returned pointer is owned by the caller until raft_server_delete.
#[no_mangle]
pub unsafe extern "C" fn raft_server_new() -> *mut RaftServerBase {
    let s: *mut RaftServerBase = Box::into_raw(Box::new(RaftServerBase::new()));
    (*s).ConstructRuntime();
    s
}

/// # Safety
/// `s` came from raft_server_new and is not used afterwards.
#[no_mangle]
pub unsafe extern "C" fn raft_server_delete(s: *mut RaftServerBase) {
    (*s).Shutdown();
    // Drops this server's row from the C++ commo table -- see set_commo, and
    // commo_of in server.cc. Here rather than in the shim's destructor so the
    // key is released in the same function that frees what it keys on, and
    // while the pointer is still live: Shutdown reaches no kernel that
    // resolves the communicator.
    raft_unbind_commo(s as *mut RaftServerHandle);
    drop(rusty::Box::from_raw(s));
}

// --- The two fiber loops, entered from the spawn kernels.
/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_heartbeat_loop(s: *mut RaftServerBase) {
    heartbeat_loop_body(s)
}

/// # Safety
/// `s` is a live `RaftServerBase`; runs on the calling fiber until shutdown.
#[no_mangle]
pub unsafe extern "C" fn raft_server_run_election_timer_loop(s: *mut RaftServerBase,
                                                              wait_int_us: u64) {
    let timer: ElectionTimerLoop = ElectionTimerLoop::new(s, wait_int_us);
    timer.run()
}

// --- The wake job, entered from the reactor's OneTimeJob (raft_queue_wake_job).
/// # Safety
/// `token` is the Box<GateWakeJob> RaftServerBase::queue_wake_job made raw,
/// handed back exactly once.
#[no_mangle]
pub unsafe extern "C" fn raft_wake_job_run(token: *mut core::ffi::c_void) {
    let job: rusty::Box<GateWakeJob> = rusty::Box::from_raw(token as *mut GateWakeJob);
    job.run();
}

// --- The replication interface: TxLogServer and RaftSpecific.

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_site_identity(s: *mut RaftServerBase,
                                                       loc_id: u32,
                                                       site_id: u16,
                                                       partition_id: u32) {
    (*s).set_site_identity(loc_id, site_id, partition_id)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_commo(s: *mut RaftServerBase,
                                               commo: *mut rusty::Communicator) {
    (*s).set_commo(commo)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_reg_learner_action(s: *mut RaftServerBase,
                                                        learner_action: *const rusty::LearnerAction) {
    (*s).reg_learner_action(&*learner_action)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_ensure_setup(s: *mut RaftServerBase) {
    (*s).EnsureSetup()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_wait_for_startup(s: *mut RaftServerBase) -> bool {
    (*s).WaitForStartup()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_prepare_for_shutdown(s: *mut RaftServerBase) {
    (*s).PrepareForShutdown()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_leader(s: *mut RaftServerBase) -> bool {
    (*s).IsLeader()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_get_leader_hint(s: *mut RaftServerBase) -> u16 {
    (*s).GetLeaderHint()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_preferred_leader(s: *mut RaftServerBase,
                                                          site_id: u16) {
    (*s).SetPreferredLeader(site_id)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_register_leader_change_callback(s: *mut RaftServerBase,
                                                                     cb: *const rusty::RaftLeaderChangeCb) {
    (*s).RegisterLeaderChangeCallback(&*cb)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_is_rpc_ready(s: *const RaftServerBase) -> bool {
    (*s).IsRpcReady()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_site_id(s: *const RaftServerBase) -> u16 {
    (*s).SiteId()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_partition_id(s: *const RaftServerBase) -> u32 {
    (*s).PartitionId()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_commit_index(s: *const RaftServerBase) -> u64 {
    (*s).CommitIndex()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_start(s: *mut RaftServerBase,
                                           cmd: *const rusty::RaftCommand,
                                           index: *mut u64,
                                           term: *mut u64) -> RaftStartResult {
    (*s).Start(&*cmd, index, term)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_serve_vote(s: *mut RaftServerBase,
                                                lst_log_idx: u64,
                                                lst_log_term: i64,
                                                can_id: u16,
                                                can_term: i64,
                                                reply_term: *mut i64,
                                                vote_granted: *mut i8) {
    (*s).ServeVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term, vote_granted)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_serve_append_entries(s: *mut RaftServerBase,
                                                          leader_current_term: u64,
                                                          leader_site_id: u16,
                                                          leader_prev_log_index: u64,
                                                          leader_prev_log_term: u64,
                                                          leader_commit_index: u64,
                                                          cmd: *const rusty::RaftCommand,
                                                          leader_next_log_term: u64,
                                                          follower_append_ok: *mut u64,
                                                          follower_current_term: *mut u64,
                                                          follower_last_log_index: *mut u64) {
    (*s).ServeAppendEntries(leader_current_term, leader_site_id, leader_prev_log_index, leader_prev_log_term, leader_commit_index, &*cmd, leader_next_log_term, follower_append_ok, follower_current_term, follower_last_log_index)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_serve_install_snapshot(s: *mut RaftServerBase,
                                                            term: u64,
                                                            leader_id: u64,
                                                            last_included_index: u64,
                                                            last_included_term: u64,
                                                            data: *const rusty::RaftByteString,
                                                            term_out: *mut u64) {
    (*s).ServeInstallSnapshot(term, leader_id, last_included_index, last_included_term, &*data, term_out)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_set_state_machine_snapshot_callbacks(s: *mut RaftServerBase,
                                                                          create_cb: *const rusty::RaftCreateSnapshotCb,
                                                                          prepare_cb: *const rusty::RaftPrepareSnapshotCb) -> u64 {
    (*s).SetStateMachineSnapshotCallbacks(&*create_cb, &*prepare_cb)
}

// --- What the kernels in server.cc call back into.

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_apply_thread_loop(s: *mut RaftServerBase) {
    (*s).ApplyThreadLoop()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_bind_replication_wake_owner(s: *mut RaftServerBase,
                                                                 owner: *const rusty::RaftPollThreadPtr) {
    let owner_copy: rusty::RaftPollThreadPtr = (*owner).clone();
    (*s).BindReplicationWakeOwner(owner_copy)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_fail_stop(s: *mut RaftServerBase) {
    (*s).FailStop()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_initialize_snapshot_manager_locked(s: *mut RaftServerBase) -> bool {
    (*s).InitializeSnapshotManagerLocked()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_install_snapshot_reply_accepted(s: *mut RaftServerBase,
                                                                     site_id: u16,
                                                                     ord: usize,
                                                                     snap_last_idx: u64,
                                                                     send_term: u64,
                                                                     follower_term: u64) {
    (*s).InstallSnapshotReplyAccepted(site_id, ord, snap_last_idx, send_term, follower_term)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_on_install_snapshot_locked(s: *mut RaftServerBase,
                                                                term: u64,
                                                                leader_id: u64,
                                                                last_included_index: u64,
                                                                last_included_term: u64,
                                                                data: *const rusty::RaftByteString,
                                                                term_out: *mut u64) {
    (*s).OnInstallSnapshotLocked(term, leader_id, last_included_index, last_included_term, data, term_out)
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_setup_internal(s: *mut RaftServerBase) -> bool {
    (*s).SetupInternal()
}

/// # Safety
/// `s` is a live `RaftServerBase`; every pointer argument is live for the call.
#[no_mangle]
pub unsafe extern "C" fn raft_server_start_election_timer(s: *mut RaftServerBase) {
    (*s).StartElectionTimer()
}
// --- GENERATED EXPORTS END ---
