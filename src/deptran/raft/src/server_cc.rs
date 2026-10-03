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
    fn raft_command_has_value(cmd: *const rusty::RaftCommand) -> bool;
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
        };
    }

    core.round_.begin(core.current_term_, core.heartbeat_round_);

    // Sized here rather than in the prologue because the round state is the
    // loop's, not the server's. Idempotent: resize only runs when the two
    // tables disagree, so in-flight slots survive every later round.
    if core.pending_rpcs_.len() != core.peers_.len() {
        core.pending_rpcs_.resize(core.peers_.len());
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
    }
}

// Whether PHASE 0 declined to advance the read-index generation because the
// counter is saturated. Split out so the caller can log it at ERROR without
// the DSL body paying for an unconditional log_line every round.
pub fn heartbeat_round_saturated(round_counter: u64) -> bool {
    !raft_server_read_index_round_can_advance(round_counter)
}

// ==========================================================================
// PHASE 0 and PHASE 3.
//
// Their decisions are heartbeat_phase0_locked and heartbeat_phase3_locked.
// What stayed C++ around them was the mutex, the apply
// queue, the replication wake and the debug logging -- all of which a DSL
// body can now express, so the C++ halves are gone.
//
// The round members no longer come from a std::vector rebuilt out of
// current_config_ every round; they are server.core.config_members_, filled once
// during Setup. Same contents, sorted and duplicate-free, which is what both
// the round's membership and the ledger's set-equality check expect.
// ==========================================================================
pub fn heartbeat_phase0_body(server: &mut RaftServerBase) -> bool {
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let leader: bool = server.IsLeaderLocked();
        if leader && heartbeat_round_saturated(server.core.heartbeat_round_) {
            rusty::raft_log_error_2(
                "[READ-INDEX] site={} heartbeat round saturated in term {}",
                server.site_id_, server.core.current_term_);
        }
        if leader {
            let mut ord: usize = 0;
            while ord < server.core.peers_.len() {
                rusty::raft_log_debug_2(
                    "[COMMIT-CALC] match_index_[{}] = {}",
                    server.peer_site_at(ord),
                    server.core.peers_.match_index(ord));
                ord += 1;
            }
        }

        let site_id: u16 = server.site_id_;
        let members: rusty::Vec<u16> = server.core.config_members_.clone();
        let outcome: Phase0Outcome = heartbeat_phase0_locked(
            &mut server.core, &members, site_id, leader);  // [move, M2]

        if outcome.restart() {
            // Was `continue`; the Rust driver starts the next round when
            // this returns true.
            return true;
        }
        if outcome.commit_advanced() {
            // The apply queue is I/O, so the hand-off stays a call; the
            // DECISION to commit was made above.
            server.EnqueueCommittedEntries(outcome.commit_from(),
                                           outcome.commit_to());
        }
    }

    let members: rusty::Vec<u16> = server.core.config_members_.clone();
    let opened: bool = server.core.authority_rounds_.open(
        server.core.round_.round_id(), &members,
        HeartbeatAuthority::new(server.core.round_.term(), server.core.round_.nservers(),
                                server.site_id_));
    server.core.round_.set_authority_inserted(opened);
    // heartbeat_round_ never wraps. The only possible duplicate is the
    // deliberately fail-closed UINT64_MAX saturation generation, which
    // open() declines rather than overwriting.
    if !server.core.round_.authority_inserted() {
        assert!(server.core.round_.round_id() == u64::MAX);  // [move, M10]
    }
    true
}

// ==========================================================================
// PHASE 1 -- choose each follower's payload and send it
// ==========================================================================

// Chooses what this AppendEntries carries: nothing (a heartbeat), one raw
// entry, or a TpcBatchCommand. Returns true when the follower must be
// skipped. CALLER MUST HOLD mtx_.
//
// The two arms were an #ifdef RAFT_BATCH_OPTIMIZATION / #ifndef pair. They
// are an if/else on a compile-time-constant function now, because the
// preprocessor has no DSL spelling; the compiler folds the dead arm exactly
// as the preprocessor removed it.
#[allow(clippy::too_many_arguments)]
pub fn heartbeat_phase1_select_payload(server: &mut RaftServerBase,
                                       ord: usize, site_id: u16,
                                       prev_log_index: u64,
                                       cmd: &mut rusty::RaftCommand,
                                       cmd_log_term: &mut u64,
                                       sent_end_index: &mut u64) -> bool {
    let mut skip_follower: bool = false;

    if !unsafe { raft_batch_optimization_enabled() } {
        rusty::raft_log_debug_5(
            "[BATCH_CHECK] site={} follower={} next_index={} core.raft_log_.base()={} core.raft_log_.last_index()={}",
            server.site_id_, site_id, server.core.peers_.next_index(ord),
            server.core.raft_log_.base(),
            server.core.raft_log_.last_index());
        if server.core.peers_.next_index(ord)
            <= server.core.raft_log_.last_index()
        {
            if !raft_server_append_entry_count_fits(prev_log_index, 1) {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-SEND] Log index exhausted after {}, skipping follower {}",
                    prev_log_index, site_id);
                skip_follower = true;
            } else {
                let next: u64 = server.core.peers_.next_index(ord);
                let slot = server.core.raft_log_.get(next);
                let usable: bool = slot.is_some()
                    && slot.unwrap().has_value();  // [move, M6]
                if !usable {
                    rusty::raft_log_error_2(
                        "[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                        next, site_id);
                    skip_follower = true;
                } else {
                    let entry: &RaftEntry = slot.unwrap();
                    *cmd_log_term = entry.term() as u64;
                    // Copying a Command is a refcount bump on its inner Arc:
                    // a kernel, never the carrier's clone.
                    unsafe {
                        raft_command_clone_into(
                            entry.cmd() as *const rusty::RaftCommand,
                            cmd as *mut rusty::RaftCommand);
                    }
                    *sent_end_index =
                        raft_server_append_sent_end(prev_log_index, 1);
                    // The kind tag identifies the payload better than the
                    // inner shared_ptr's raw address ever did.
                    let kind: i32 = entry.kind();  // [move, M6]
                    rusty::raft_log_debug_4(
                        "[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        server.site_id_, next, site_id, kind);
                }
            }
        }
        return skip_follower;
    }

    // A fresh buffer per follower, as the C++ local was.
    server.batch_buffer_.clear();
    let max_batch_entries: u64 = unsafe { raft_append_entries_batch_max() };
    // A batch is bounded by BYTES too: the transport refuses a frame past its
    // 64 MiB limit, and an entry count alone lets large entries exceed it.
    let max_batch_bytes: u64 = unsafe { raft_append_entries_batch_max_bytes() };
    let mut batch_bytes: u64 = 0;
    let batch_start_idx: u64 = server.core.peers_.next_index(ord);
    rusty::raft_log_debug_5(
        "[BATCH_CHECK] site={} follower={} next_index={} core.raft_log_.base()={} core.raft_log_.last_index()={}",
        server.site_id_, site_id, server.core.peers_.next_index(ord),
        server.core.raft_log_.base(), server.core.raft_log_.last_index());
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
            || batch_start_idx < server.core.raft_log_.base())
    {
        rusty::raft_log_error_4(
            "[HEARTBEAT-BATCH] Non-contiguous source for follower {}: prev={} start={} min_active={}; refusing to compress a hole",
            site_id, prev_log_index, batch_start_idx,
            server.core.raft_log_.base());
        skip_follower = true;
    } else if !skip_follower {
        let mut idx: u64 = batch_start_idx;
        while idx <= server.core.raft_log_.last_index()
            && (server.batch_buffer_.len() as u64) < max_batch_entries
        {
            let entry = server.core.raft_log_.get(idx);
            let usable: bool = entry.is_some()
                && entry.unwrap().has_value();  // [move, M6]
            if !usable {
                rusty::raft_log_error_2(
                    "[HEARTBEAT-BATCH] Missing log entry {} for follower {}; refusing to compress a hole",
                    idx, site_id);
                skip_follower = true;
                break;
            }
            let entry_term: i64 = entry.unwrap().term();
            let entry_cmd: *const rusty::RaftCommand =
                entry.unwrap().cmd() as *const rusty::RaftCommand;
            // Stop before this entry would push the batch past the byte bound
            // -- but always carry at least one, so progress never stalls.
            let entry_bytes: u64 = entry.unwrap().payload_bytes();  // [move, M6]
            if !server.batch_buffer_.is_empty()
                && batch_bytes.saturating_add(entry_bytes) > max_batch_bytes
            {
                break;
            }
            batch_bytes = batch_bytes.saturating_add(entry_bytes);
            let is_commit: bool = entry.unwrap().is_tpc_commit();  // [move, M6]
            if is_commit {
                let stamped: rusty::RaftTpcCommitPtr = unsafe {
                    rusty::raft_stamped_commit(entry_cmd, entry_term)
                };
                server.batch_buffer_.push(stamped);
            } else {
                // Looked up again rather than held across the push above:
                // `entry` borrows the log, and handing the push a *mut to the
                // server ends that borrow. This branch is the rare one -- an
                // entry that is not a TpcCommitCommand -- so the second
                // lookup costs nothing on the batching path.
                let slot = server.core.raft_log_.get(idx);
                let kind: i32 = slot.unwrap().kind();  // [move, M6]
                let batched: u64 = server.batch_buffer_.len() as u64;
                if batched == 0 {
                    rusty::raft_log_info_3(
                        "[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log",
                        server.site_id_, idx, kind);
                    unsafe {
                        raft_command_clone_into(
                            slot.unwrap().cmd() as *const rusty::RaftCommand,
                            cmd as *mut rusty::RaftCommand);
                    }
                    *cmd_log_term = entry_term as u64;
                    *sent_end_index =
                        raft_server_append_sent_end(prev_log_index, 1);
                } else {
                    rusty::raft_log_info_3(
                        "[BATCH_STOP] site={} idx={}: ending batch before non-TpcCommitCommand kind={}",
                        server.site_id_, idx, kind);
                }
                break;
            }
            if !raft_server_log_index_has_successor(idx) {
                break;
            }
            idx += 1;
        }
    }

    let encoded_entry_count: u64 = server.batch_buffer_.len() as u64;
    if !skip_follower && encoded_entry_count > 0
        && !raft_server_append_batch_count_is_valid(prev_log_index,
                                                    encoded_entry_count)
    {
        rusty::raft_log_error_3(
            "[HEARTBEAT-BATCH] Invalid encoded count {} after previous index {}; skipping follower {}",
            encoded_entry_count, prev_log_index, site_id);
        skip_follower = true;
    }
    if !skip_follower && encoded_entry_count > 0 {
        unsafe {
            raft_batch_finalize(server.batch_buffer_.as_mut_ptr(),
                                server.batch_buffer_.len(),
                                cmd as *mut rusty::RaftCommand);
        }
        *sent_end_index = raft_server_append_sent_end(prev_log_index,
                                                      encoded_entry_count);
        let batch_end_idx: u64 = *sent_end_index;
        let truncated: bool =
            batch_end_idx < server.core.raft_log_.last_index();
        rusty::raft_log_info_6(
            "[BATCH_SEND] site={} sending batch of {} entries to follower {} (from={} to={}{})",
            server.site_id_, encoded_entry_count, site_id, batch_start_idx,
            batch_end_idx, if truncated { ", truncated" } else { "" });
    }
    skip_follower
}

// ==========================================================================
// PHASE 1: send every AppendEntries RPC in parallel, non-blocking.
//
// Formerly RaftServer::HeartbeatPhase1. The ordinal is used for ITERATION
// ONLY; every read and write of a follower's next index goes through
// core.peers_.next_index(ord).
//
// That is not style. The body reaches commo()->SendInstallSnapshot inside
// the lock scope, and that call's completion callback takes the SAME mutex
// and writes next_index -- so a callback that completes synchronously
// re-enters and mutates the table while a dereferenced cursor into it is
// live. Holding no dereferenced cursor across that call is also what makes
// the loop expressible in Rust at all: a `&mut` into a table cannot be held
// across a call that takes `&mut` to the same table.
// ==========================================================================
// The zero-initialisation of prev_log_index and sent_end_index is the C++
// original's, kept so the emitted C++ declares initialised locals rather
// than uninitialised ones. `is_none` then `unwrap` is likewise the shape the
// original had; rewriting it as a match would obscure the correspondence.
#[allow(unused_assignments, clippy::unnecessary_unwrap)]
pub fn heartbeat_phase1_body(server: &mut RaftServerBase) {
    let partition_id: u32 = server.partition_id_;
    let mut ord: usize = 0;
    while ord < server.core.peers_.len() {
        let site_id: u16 = server.peer_site_at(ord);
        if site_id == server.site_id_ {
            ord += 1;
            continue;
        }
        if !server.IsLeader() {
            break;  // Stop sending if we lost leadership.
        }
        if server.core.pending_rpcs_.occupied(ord) {
            ord += 1;
            continue;
        }

        let mut prev_log_index: u64 = 0;
        let mut prev_log_term: u64 = 0;
        // An empty Command (has_value() == false) signals a heartbeat.
        let mut cmd: rusty::RaftCommand = Default::default();
        let mut cmd_log_term: u64 = 0;
        let mut sent_end_index: u64 = 0;
        let mut skip_follower: bool = false;
        // [fix, F3] The commit index this append carries, read in the same
        // locked section that picks prev and the entries.
        let mut send_commit_index: u64 = 0;
        {
            let _lock = RaftLockGuard::new(&mut server.mtx_);
            // [fix, F3] The IsLeader() check above released the lock. Only a
            // server still leading in the round's term may send that term's
            // append, built from the log it holds now.
            if !server.core.is_leader_ || server.core.current_term_ != server.core.round_.term() {
                break;
            }
            send_commit_index = server.core.commit_index_;
            if server.core.peers_.next_index(ord) == 0 {
                rusty::raft_log_warn_2(
                    "[APPEND_ENTRIES] Repairing wrapped next_index for follower {} at leader last index {}",
                    site_id, server.core.raft_log_.last_index());
                let last: u64 = server.core.raft_log_.last_index();
                let repaired: u64 = if raft_server_log_index_has_successor(last) {
                    raft_server_follower_next_index(last)
                } else {
                    last
                };
                server.core.peers_.set_next_index(ord, repaired);
            }
            prev_log_index = server.core.peers_.next_index(ord) - 1;
            if prev_log_index > server.core.raft_log_.last_index() {
                rusty::raft_log_info_2(
                    "[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > core.raft_log_.last_index() ({}), fixing next_index",
                    prev_log_index, server.core.raft_log_.last_index());
                let last: u64 = server.core.raft_log_.last_index();
                let repaired: u64 = if raft_server_log_index_has_successor(last) {
                    raft_server_follower_next_index(last)
                } else {
                    last
                };
                server.core.peers_.set_next_index(ord, repaired);
                prev_log_index = server.core.peers_.next_index(ord) - 1;
            }
            // Until a payload is selected this is a heartbeat, and proves
            // only the prefix named by prev_log_index.
            sent_end_index = raft_server_append_sent_end(prev_log_index, 0);

            let snapshot_configured: bool = unsafe {
                raft_snapshot_manager_is_set(&server.snapshot_manager_)
            };
            if prev_log_index > server.core.raft_log_.last_index() {
                rusty::raft_log_info_3(
                    "[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > core.raft_log_.last_index() ({}), skipping",
                    site_id, prev_log_index,
                    server.core.raft_log_.last_index());
                server.core.peers_.set_next_index(ord, 1);
                skip_follower = true;
            } else if server.core.peers_.next_index(ord)
                < server.core.raft_log_.base()
                && snapshot_configured
            {
                // The follower is behind the log's base, so send it a
                // snapshot instead of entries it can no longer be given.
                rusty::raft_log_info_4(
                    "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < core.raft_log_.base()={}, sending InstallSnapshot",
                    server.site_id_, site_id,
                    server.core.peers_.next_index(ord),
                    server.core.raft_log_.base());
                let sent: bool = unsafe {
                    raft_phase1_load_and_send_snapshot(
                        server.handle(),
                        &server.snapshot_manager_
                            as *const rusty::RaftSnapshotManagerPtr,
                        &server.async_callback_lifetime_
                            as *const rusty::RaftAsyncCallbackLifetimePtr,
                        server.site_id_, server.partition_id_,
                        server.core.current_term_, site_id, ord)
                };
                if !sent {
                    rusty::raft_log_warn_2(
                        "[HEARTBEAT-SNAPSHOT] Site {}: Failed to load snapshot for follower {}, skipping",
                        server.site_id_, site_id);
                }
                // Both outcomes skip the normal AppendEntries for this
                // follower: sent, or failed to load.
                skip_follower = true;
            } else {
                assert!(
                    prev_log_index
                        <= server.core.raft_log_.last_index());  // [move, M10]
                if prev_log_index == 0 {
                    prev_log_term = 0;
                } else if prev_log_index == server.core.snapidx_
                    && server.core.snapidx_ > 0
                {
                    // Keep using snapshot boundary metadata after compaction.
                    prev_log_term = server.core.snapterm_ as u64;
                } else {
                    // Was GetRaftInstance, which default-inserted and so
                    // could never return null -- the check below was dead,
                    // and a genuinely missing prevLogIndex silently
                    // fabricated an empty entry with term 0 and sent
                    // prevLogTerm = 0 rather than skipping the follower.
                    let instance = server.core.raft_log_.get(prev_log_index);
                    if instance.is_none() {
                        rusty::raft_log_error_2(
                            "[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}",
                            prev_log_index, site_id);
                        skip_follower = true;
                    } else {
                        prev_log_term = instance.unwrap().term() as u64;
                    }
                }

                if !skip_follower {
                    skip_follower = heartbeat_phase1_select_payload(
                        server, ord, site_id, prev_log_index, &mut cmd,
                        &mut cmd_log_term, &mut sent_end_index);
                }
            }
        }
        if skip_follower {
            ord += 1;
            continue;
        }

        let is_leader: bool = server.IsLeader();
        let mut sent_response: rusty::RaftResponsePtr = Default::default();
        unsafe { raft_trace_through(3, sent_end_index, 0) };  // [M0] trace kit
        unsafe {
            raft_phase1_send_append(
                server.handle(), server.site_id_, site_id,
                partition_id,
                is_leader, server.core.round_.term(), prev_log_index, prev_log_term,
                send_commit_index,  // [fix, F3] was server.core.round_.commit_index()
                &cmd as *const rusty::RaftCommand, cmd_log_term,
                &mut sent_response as *mut rusty::RaftResponsePtr);
        }
        unsafe { raft_trace_through(4, sent_end_index, 0) };  // [M0] trace kit

        server.core.pending_rpcs_.place(ord, PendingAppend::new(
            site_id, server.core.round_.term(), server.core.round_.round_id(), sent_end_index,
            sent_response, cmd));
        if server.core.round_.authority_inserted() && server.core.round_.is_member(site_id) {
            // Was a std::map iterator created in PHASE 0 and dereferenced
            // here, after the RPC sends. Nothing between the two points
            // mutates authority_rounds, so it was valid -- but a cursor held
            // across a phase boundary and across a synchronous completion
            // callback is the hazard class this file removed for next_index,
            // so look it up by key.
            // [move, M10] The launch is a write, so it is not made inside the
            // assert's condition.
            let launched: bool =
                server.core.authority_rounds_.launch(server.core.round_.round_id(), site_id);
            assert!(launched);
        }
        ord += 1;
    }
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

// [move, M3] PHASE 2's step-down, as a function of this module: the
// transpiled C++ lane passes a local CoreOutput by reference to a
// same-module call, but not to a method of a type another module defines
// (bugs-found B13's class).
fn heartbeat_step_down(core: &mut RaftCore, stopped: bool, failover: bool,
                       out: &mut CoreOutput) {
    core.step_down(stopped, failover, out);
}

// ==========================================================================
// PHASE 2: poll responses through one SHORT round deadline and process them.
//
// Formerly RaftServer::HeartbeatPhase2. Never call wait_timeout on an
// individual response: that permanently marks its event TIMEOUT and loses a
// legitimate late persistence reply. Polling also gives every parallel RPC
// the same bounded round budget.
//
// The round's pending slots and authority ledger are fields of the core
// ([move, M1]), reached through `server`.
// ==========================================================================
#[allow(clippy::manual_clamp)]
pub fn heartbeat_phase2_body(server: &mut RaftServerBase) {
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

    while !stop_response_processing {
        let mut waiting_for_current_round: bool = false;
        let mut pending_ord: usize = 0;
        while pending_ord < server.core.pending_rpcs_.len() {
            if !server.IsLeader() {
                stop_response_processing = true;
                break;
            }
            if !server.core.pending_rpcs_.occupied(pending_ord) {
                pending_ord += 1;
                continue;
            }

            // Bound once per slot per poll pass, not per use: every read
            // below is the same shape it was when this was a map value.
            let follower_id: u16 = server.core.pending_rpcs_.follower(pending_ord);
            let sent_term: u64 = server.core.pending_rpcs_.sent_term(pending_ord);
            let sent_round: u64 = server.core.pending_rpcs_.sent_round(pending_ord);
            let sent_end_index: u64 = server.core.pending_rpcs_.sent_end_index(pending_ord);
            let cmd_has_value: bool = unsafe {
                raft_command_has_value(
                    server.core.pending_rpcs_.cmd(pending_ord) as *const rusty::RaftCommand)
            };
            let resp: AppendRespView = unsafe {
                raft_append_response_read(
                    server.core.pending_rpcs_.response(pending_ord)
                        as *const rusty::RaftResponsePtr)
            };
            if !resp.completed_ {
                if sent_round == server.core.round_.round_id() {
                    waiting_for_current_round = true;
                }
                pending_ord += 1;
                continue;
            }

            let mut stepped_down: bool = false;
            // [move, M3] The step-down's actions, before the guard drops.
            let mut out: CoreOutput = CoreOutput::new();
            {
                let _lock = RaftLockGuard::new(&mut server.mtx_);
                // What the reply MEANS is heartbeat_apply_append_reply. It
                // reads the wire response as three scalars -- the srpc object
                // itself never crosses -- and returns what to do about it.
                let response_available: bool =
                    !(!resp.status_ && resp.term_ == 0
                      && resp.last_log_index_ == 0);
                let resp_ord: usize = server.PeerOrdinal(follower_id);
                let log_last_index: u64 = server.core.raft_log_.last_index();
                let is_leader: bool = server.IsLeaderLocked();
                let outcome: AppendReplyOutcome = heartbeat_apply_append_reply(
                    &mut server.core,  // [move, M2] carries the ledger
                    &SentAppend::new(follower_id, sent_term, sent_round,
                                     sent_end_index, resp_ord),
                    &AppendReply::new(response_available, resp.status_,
                                      resp.term_, resp.last_log_index_),
                    log_last_index,
                    is_leader);

                let action: AppendReplyAction = outcome.action();
                if action == AppendReplyAction::STEP_DOWN {
                    rusty::raft_log_info_4(
                        "[STEPDOWN] Site {}: AppendEntries response from follower {} carried higher term {} > {}",
                        server.site_id_, follower_id, resp.term_,
                        outcome.previous_term());
                    server.LogTermChange(
                        "AppendEntries response carried newer term",
                        outcome.previous_term(), server.core.current_term_,
                        follower_id);
                    // The step-down's effects are actions now ([move, M3]);
                    // the decision to take it was made above.
                    let stopped: bool = server.stopped_now();
                    let failover: bool = server.failover_;
                    heartbeat_step_down(&mut server.core, stopped, failover,
                                        &mut out);
                    server.core.req_voting_ = false;
                    server.core.election_in_progress_ = false;
                    stepped_down = true;
                } else if action == AppendReplyAction::BACKED_OFF {
                    // The five-rung ladder is
                    // FollowerProgress::back_off_after_reject; it reports
                    // which rung it took so the diagnostics stay as specific
                    // as they were when the branches were inline.
                    let rung: BackoffKind = outcome.rung();
                    if rung == BackoffKind::FAST {
                        rusty::raft_log_info_6(
                            "[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next(),
                            outcome.old_next() - outcome.new_next(),
                            resp.last_log_index_);
                    } else if rung == BackoffKind::TERM_CONFLICT {
                        rusty::raft_log_info_4(
                            "[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    } else if rung == BackoffKind::EXPONENTIAL {
                        rusty::raft_log_info_4(
                            "[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    } else if rung == BackoffKind::LINEAR {
                        rusty::raft_log_debug_4(
                            "[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}",
                            server.site_id_, follower_id, outcome.old_next(),
                            outcome.new_next());
                    }
                    // BackoffKind::FLOOR logs nothing, as before.
                } else if action == AppendReplyAction::ACCEPTED {
                    rusty::raft_log_debug_8(
                        "[APPEND_RPC] Leader {} accepted follower {} proof: kind={} reported={} sent_end={} acknowledged={} next={} match={}",
                        server.site_id_, follower_id,
                        if cmd_has_value { "entries" } else { "heartbeat" },
                        resp.last_log_index_, sent_end_index,
                        outcome.acknowledged(),
                        server.core.peers_.next_index(resp_ord),
                        server.core.peers_.match_index(resp_ord));
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
                server.run_locked_actions(&out);  // [move, M3]
            }

            let completed_previous_round: bool =
                sent_round != server.core.round_.round_id();
            server.core.pending_rpcs_.release(pending_ord);
            retry_released_follower =
                retry_released_follower || completed_previous_round;
            if stepped_down {
                stop_response_processing = true;
                break;
            }
            pending_ord += 1;
        }

        let current_round_has_authority: bool =
            server.core.authority_rounds_.has_quorum(server.core.round_.round_id());
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
        server.core.pending_rpcs_.abandon();
        server.core.authority_rounds_.abandon();
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

pub fn heartbeat_phase3_body(server: &mut RaftServerBase) {
    if !server.IsLeader() {
        return;
    }
    let mut commit_advanced_after_send: bool = false;
    {
        let _lock = RaftLockGuard::new(&mut server.mtx_);
        let members: rusty::Vec<u16> = server.core.config_members_.clone();
        let nservers: usize = server.core.round_.nservers();
        let is_leader: bool = server.IsLeaderLocked();
        let outcome: Phase3Outcome = heartbeat_phase3_locked(
            &mut server.core, nservers, &members, is_leader);  // [move, M2]

        if outcome.commit().advanced() {
            rusty::raft_log_debug_2(
                "[PHASE3-COMMIT] Advancing core.commit_index_ {} -> {}",
                outcome.commit().from_index(), outcome.commit().to_index());
            server.EnqueueCommittedEntries(outcome.commit().from_index(),
                                           outcome.commit().to_index());
            commit_advanced_after_send = true;
        }
        if outcome.confirmed() {
            rusty::raft_log_debug_3(
                "[READ-INDEX] site={} confirmed round={} term={}",
                server.site_id_, server.core.read_quorum_confirmed_round_,
                server.core.read_quorum_confirmed_term_);
        }
    }

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
// Sequences the four phases above. It lives here, beside them, rather than
// next to RaftServerBase: a DSL block can only call what precedes it, and
// the phase bodies are in this file. Keeping driver and phases together is
// what lets the round state be an owned struct instead of a `*mut c_void`
// passed through four trampolines.
// ==========================================================================

// The driver holds only the server. The round state it used to own
// ([move, M1]) is the core's: RaftCore::pending_rpcs_, authority_rounds_,
// pending_leader_term_ and round_, reset when a run of the loop begins and
// again when it ends.
pub struct HeartbeatDriver {
    server_: *mut RaftServerBase,
}

#[allow(clippy::not_unsafe_ptr_arg_deref)]
impl HeartbeatDriver {
    pub fn new(server: *mut RaftServerBase) -> HeartbeatDriver {
        HeartbeatDriver { server_: server }
    }

    // decide -> emit -> collect -> decide, which is the shape the C++ already
    // had as four comment-delimited phases. It is now the shape of the Rust
    // that sequences them, and the phases are called directly.
    pub fn run(&mut self) {
        let server: &mut RaftServerBase = unsafe { &mut *self.server_ };
        // [move, M1] a fresh round state per run, as the driver's own was
        server.core.reset_round_state();
        server.HeartbeatPrologue();
        while server.HeartbeatLooping() {
            // The wake gate returns false on shutdown rather than on timeout.
            if !server.HeartbeatWait() {
                break;
            }
            // PHASE 0 declines the round when leadership is not held. The C++
            // spelled that `continue`.
            if !heartbeat_phase0_body(server) {
                continue;
            }
            heartbeat_phase1_body(server);
            heartbeat_phase2_body(server);
            heartbeat_phase3_body(server);
        }
        // [move, M1] The in-flight handles are released when the loop ends, as
        // they were when the driver's own round state went out of scope.
        // Before the epilogue, not after: once it reports the loop stopped,
        // shutdown may free the server.
        server.core.reset_round_state();
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
