// The heartbeat round as core calls: the tick (PHASE 0 and PHASE 1's
// decisions), each reply, and the round end (PHASE 3); the commit rule.
//
// [move, M1] From src/deptran/raft/src/server_cc.rs (Phase 6); the
// command is the type parameter C (M11) and logging is records in the
// output (M7).

#[allow(unused_imports)]
use crate::*;
#[allow(unused_imports)]
use vstd::pervasive::runtime_assert;
use vstd::prelude::*;

verus! {

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
    // [M12] Whether it advanced, and to where (ghost).
    pub closed spec fn spec_advanced(&self) -> bool {
        self.advanced_
    }

    pub closed spec fn spec_to(&self) -> u64 {
        self.to_
    }

    pub fn advanced(&self) -> (r: bool)
        ensures r == self.spec_advanced(),  // [M12]
    {
        self.advanced_
    }

    pub fn from_index(&self) -> u64 {
        self.from_
    }

    pub fn to_index(&self) -> (r: u64)
        ensures r == self.spec_to(),  // [M12]
    {
        self.to_
    }
}

// Same aliasing note as heartbeat_phase3_locked: peers and log are reached
// through `consensus` because both are its fields.
pub fn raft_commit_advance<C: Clone>(
    consensus: &mut RaftCore<C>,
    nservers: usize,
) -> (r: CommitAdvance)
    requires
        old(consensus).inv(),
        nservers == old(consensus).round_.spec_nservers(),
        nservers > 0,
        // the gate (F5): the configuration contains this server
        old(consensus).config_members_@.contains(old(consensus).site_id_),
    ensures
        final(consensus).inv(),
        // only the commit index moves (and, Verus only, the ghost log)
        *final(consensus) == (RaftCore {
            commit_index_: final(consensus).commit_index_,
            g_log_: final(consensus).g_log_,
            ..*old(consensus)
        }),
        // [M12] an advance sets the commit index to its target
        r.spec_advanced() ==> final(consensus).commit_index_ == r.spec_to(),
        r.spec_advanced() ==> final(consensus).commit_index_ > old(consensus).commit_index_,
        !r.spec_advanced() ==> final(consensus).commit_index_ == old(consensus).commit_index_,
        // [M12] a leader's advance is LAdvanceCommitIndex; no advance is
        // unseen by the spec
        old(consensus).ginv() && old(consensus).gated_ && old(consensus).is_leader_ ==> {
            &&& final(consensus).ginv()
            &&& final(consensus).g_log_@ == (if r.spec_advanced() {
                    crate::coupling::advance_log(old(consensus).g_log_@, r.spec_to() as int)
                } else {
                    old(consensus).g_log_@
                })
        },
{
    let ghost pre = *consensus;
    // nservers is the value latched in PHASE 0. Reusing it in PHASE 3 is
    // sound only because current_config_ has exactly one write, during
    // Setup, and progress_ is never erased, so the size is invariant across
    // the round. Assert it rather than trusting the phases to stay in step.
    // Peer table and round membership agree.
    runtime_assert(consensus.peers_.len() == nservers - 1);  // [move, M10]
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
    runtime_assert(candidate.is_some());  // [move, M10]
    if !raft_server_log_entry_is_current_term(
        candidate.unwrap().term(),
        consensus.current_term_,
    ) {
        // Raft commits a prior-term entry only via one from the current term.
        return CommitAdvance { advanced_: false, from_: 0, to_: 0 };
    }
    let from = consensus.commit_index_;
    consensus.commit_index_ = candidate_index;
    proof {
        if pre.ginv() && pre.gated_ && pre.is_leader_ {
            // [M12] the majority (with this server; nservers is the
            // configuration's size, its round latched) and the gate's log
            pre.raft_log_.lemma_wf_bounds();
            consensus.g_log_@ = crate::coupling::advance_log(pre.g_log_@, candidate_index as int);
            crate::coupling::lemma_advance_ginv(&pre, consensus, candidate_index);
        }
    }
    // [M0] The trace kit's stage-8 stamp (commit advanced) is the shell's
    // now: it stamps when it carries out the APPLY_RANGE this advance pushes.
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
    pub closed spec fn spec_restart(&self) -> bool {
        self.restart_
    }

    pub fn restart(&self) -> (r: bool)
        ensures r == self.spec_restart(),
    {
        self.restart_
    }

    // [M12] Whether the commit advanced (ghost).
    pub closed spec fn spec_commit_advanced(&self) -> bool {
        self.commit_advanced_
    }

    pub fn commit_advanced(&self) -> (r: bool)
        ensures r == self.spec_commit_advanced(),  // [M12]
    {
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
pub fn heartbeat_phase0_locked<C: Clone>(
    core: &mut RaftCore<C>,  // [move, M2] the round state is core's now
    members: &[u16],
    site_id: u16,
    is_leader: bool,
) -> (r: Phase0Outcome)
    requires
        old(core).inv(),
        members@ == old(core).config_members_@,
        site_id == old(core).site_id_,
        // the host contract: is_leader is IsLeaderLocked, which reads the
        // core's role
        is_leader ==> old(core).is_leader_,
        // the gate (F5): the configuration contains this server
        old(core).config_members_@.contains(old(core).site_id_),
    ensures
        final(core).inv(),
        r.spec_restart() == !is_leader,
        final(core).config_members_ == old(core).config_members_,
        final(core).site_id_ == old(core).site_id_,
        final(core).is_leader_ == old(core).is_leader_,
        final(core).current_term_ == old(core).current_term_,
        is_leader ==> {
            // the round opened: generation, term and membership latched
            &&& final(core).round_.spec_round_id() == old(core).heartbeat_round_
            &&& final(core).round_.spec_term() == final(core).current_term_
            &&& final(core).round_.spec_nservers() > 0
            &&& final(core).pending_rpcs_.spec_len() == final(core).peers_.spec_len()
            // the epoch is current, and its generation id is fresh
            &&& final(core).pending_leader_term_ == Some(final(core).current_term_)
            &&& (old(core).heartbeat_round_ == u64::MAX
                || final(core).authority_rounds_.spec_ids_below(old(core).heartbeat_round_))
            &&& final(core).heartbeat_round_ == (if old(core).heartbeat_round_ == u64::MAX {
                    u64::MAX } else { (old(core).heartbeat_round_ + 1) as u64 })
        },
        // [M12] the round state is unseen by the spec; the commit advance is
        // LAdvanceCommitIndex (to the new commit index)
        old(core).ginv() && old(core).gated_ ==> {
            &&& final(core).ginv()
            &&& final(core).g_log_@ == (if r.spec_commit_advanced() {
                    crate::coupling::advance_log(old(core).g_log_@, final(core).commit_index_ as int)
                } else {
                    old(core).g_log_@
                })
        },
        // [M12] what the spec sees moves only by the advance
        final(core).raft_log_ == old(core).raft_log_,
        final(core).vote_for_ == old(core).vote_for_,
        final(core).election_in_progress_ == old(core).election_in_progress_,
        final(core).election_term_ == old(core).election_term_,
        final(core).peers_ == old(core).peers_,
        final(core).peer_sites_ == old(core).peer_sites_,
        final(core).snapterm_ == old(core).snapterm_,
        final(core).gated_ == old(core).gated_,
        final(core).g_votes_ == old(core).g_votes_,
        final(core).g_match_ == old(core).g_match_,
        final(core).g_next_ == old(core).g_next_,
        !r.spec_commit_advanced() ==> final(core).commit_index_ == old(core).commit_index_,
        r.spec_commit_advanced() ==> final(core).commit_index_ > old(core).commit_index_,
{
    if !is_leader {
        core.pending_rpcs_.abandon();
        core.authority_rounds_.abandon();
        core.pending_leader_term_ = None;
        return Phase0Outcome {
            restart_: true,
            commit_advanced_: false,
            commit_from_: 0,
            commit_to_: 0,
            slots_reset_: true,  // [move, M5]
        };
    }
    let mut slots_reset: bool = false;  // [move, M5]
    let ghost h0 = core.heartbeat_round_;

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
        core.pending_leader_term_ = Some(core.round_.term());
        slots_reset = true;  // [move, M5]
    }

    // the generation id this round opens is fresh in its epoch
    assert(h0 == u64::MAX || core.authority_rounds_.spec_ids_below(h0));
    if raft_server_read_index_round_can_advance(core.heartbeat_round_) {
        core.heartbeat_round_ += 1;
        proof { core.authority_rounds_.lemma_ids_below_mono(h0, core.heartbeat_round_); }
    }
    // Saturation is fail-closed for new reads: the round never wraps, so no
    // post-baseline proof can be forged from an old generation. The caller
    // reports it; see round_saturated below.

    let ghost pre = *core;
    let mut i = 0;
    while i < members.len()
        invariant
            i <= members@.len(),
            members@ == pre.config_members_@,
            sites_sorted(members@),
            core.round_.wf(),
            core.round_.spec_nservers() == i,
            forall|x: u16| core.round_.spec_is_member(x) == members@.subrange(0, i as int).contains(x),
            core.round_.spec_term() == pre.round_.spec_term(),
            core.round_.spec_round_id() == pre.round_.spec_round_id(),
            *core == (RaftCore { round_: core.round_, ..pre }),
        decreases members@.len() - i,
    {
        proof {
            // sorted: the next member is not admitted yet
            if members@.subrange(0, i as int).contains(members@[i as int]) {
                let k = choose|k: int| 0 <= k < i && members@.subrange(0, i as int)[k] == members@[i as int];
                assert(members@[k] == members@[i as int]);
            }
            assert(members@.subrange(0, i + 1) == members@.subrange(0, i as int).push(members@[i as int]));
        }
        core.round_.admit(members[i]);
        proof {
            let s0 = members@.subrange(0, i as int);
            let s1 = members@.subrange(0, i + 1);
            assert forall|x: u16| core.round_.spec_is_member(x) == s1.contains(x) by {
                if s1.contains(x) {
                    let k = choose|k: int| 0 <= k < s1.len() && s1[k] == x;
                    if k < i {
                        assert(s0[k] == x);
                    }
                }
                if s0.contains(x) {
                    let k = choose|k: int| 0 <= k < s0.len() && s0[k] == x;
                    assert(s1[k] == x);
                }
                if x == members@[i as int] {
                    assert(s1[i as int] == x);
                }
            }
        }
        i += 1;
    }
    proof {
        assert(members@.subrange(0, members@.len() as int) == members@);
    }
    // The heartbeat round admitted a quorum containing this site.
    runtime_assert(core.round_.nservers() != 0 && core.round_.is_member(site_id));  // [move, M10]
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
#[derive(Structural)]  // [M12] ghost: `==` is equality to the verifier
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
pub struct AppendSend<C> {
    pub ord_: usize,
    pub site_id_: u16,
    pub payload_: AppendPayload,
    pub term_: u64,
    pub prev_log_index_: u64,
    pub prev_log_term_: u64,
    // [fix, F3] read in the same call that chose prev and the entries.
    pub commit_index_: u64,
    // RAW_ENTRY: the entry's log term; otherwise 0, as the C++ sent it.
    pub entry_term_: u64,
    pub sent_end_index_: u64,
    pub sent_round_: u64,
    // RAW_ENTRY: the one command; BATCH: one per entry, with its term.
    pub cmds_: Vec<C>,
    pub terms_: Vec<i64>,
}

impl<C> AppendSend<C> {
    fn heartbeat(ord: usize, site_id: u16, term: u64, prev_log_index: u64,
                 commit_index: u64, sent_round: u64) -> (r: AppendSend<C>)
        ensures  // [M12] the heartbeat at prev
            r.site_id_ == site_id,
            r.payload_ == AppendPayload::HEARTBEAT,
            r.term_ == term,
            r.prev_log_index_ == prev_log_index,
            r.prev_log_term_ == 0,
            r.commit_index_ == commit_index,
            r.sent_end_index_ == prev_log_index,
    {
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
            cmds_: Vec::new(),
            terms_: Vec::new(),
        }
    }
}

/// An InstallSnapshot heartbeat_tick asked for: the follower is behind the
/// log's base. The shell runs the kernel before the guard drops, where the
/// per-follower section ran it.
pub struct SnapshotSend {
    pub ord_: usize,
    pub site_id_: u16,
    pub term_: u64,
}

/// What heartbeat_tick decided.
pub struct HeartbeatTick<C> {
    // Not leading: the round ends here (bugs-found B12).
    pub declined_: bool,
    // The core cleared its in-flight slots (resized, or abandoned on a lost
    // or changed leadership epoch); the shell clears its handles to match.
    pub slots_reset_: bool,
    pub slot_count_: usize,
    pub round_id_: u64,
    // Whether the round's authority generation already has a quorum.
    pub has_authority_: bool,
    pub snapshots_: Vec<SnapshotSend>,
    pub sends_: Vec<AppendSend<C>>,
}

impl<C> HeartbeatTick<C> {
    fn new() -> (r: HeartbeatTick<C>)
        ensures r.sends_@.len() == 0,  // [M12]
    {
        HeartbeatTick {
            declined_: false,
            slots_reset_: false,
            slot_count_: 0,
            round_id_: 0,
            has_authority_: false,
            snapshots_: Vec::new(),
            sends_: Vec::new(),
        }
    }
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
fn heartbeat_select_payload<C: Clone>(core: &mut RaftCore<C>, ord: usize, site_id: u16,
                            prev_log_index: u64, batching: bool,
                            max_batch_entries: u64, max_batch_bytes: u64,
                            send: &mut AppendSend<C>,  // [move, M7]
                            out: &mut CoreOutput) -> (r: bool)
    requires
        old(core).inv(),
        ord < old(core).peers_.spec_len(),
        // [M12] the caller's prev is the follower's next less one, and the
        // send so far is the heartbeat at prev
        old(core).peers_.spec_next(ord as int) as int == prev_log_index as int + 1,
        prev_log_index as int <= old(core).raft_log_.spec_last_index(),
        old(send).prev_log_index_ == prev_log_index,
        old(send).payload_ == AppendPayload::HEARTBEAT,
    ensures
        *final(core) == *old(core),
        // [M12] the payload: none (the heartbeat at prev), or entries of the
        // log from prev + 1 to the end it records; the RPC's other fields
        // kept
        final(send).site_id_ == old(send).site_id_,
        final(send).term_ == old(send).term_,
        final(send).prev_log_index_ == old(send).prev_log_index_,
        final(send).prev_log_term_ == old(send).prev_log_term_,
        final(send).commit_index_ == old(send).commit_index_,
        !r && final(send).payload_ == AppendPayload::HEARTBEAT
            ==> final(send).sent_end_index_ == old(send).sent_end_index_,
        !r && final(send).payload_ != AppendPayload::HEARTBEAT ==> {
            &&& (prev_log_index as int) < final(send).sent_end_index_ as int
            &&& final(send).sent_end_index_ as int <= old(core).raft_log_.spec_last_index()
        },
{
    let mut skip_follower: bool = false;

    if !batching {
        out.log(RAFT_LOG_DEBUG,
            "[BATCH_CHECK] site={} follower={} next_index={} core.raft_log_.base()={} core.raft_log_.last_index()={}",
            &[(core.site_id_).arg(),
             (site_id).arg(),
             (core.peers_.next_index(ord)).arg(),
             (core.raft_log_.base()).arg(),
             (core.raft_log_.last_index()).arg()]);
        if core.peers_.next_index(ord) <= core.raft_log_.last_index() {
            if !raft_server_append_entry_count_fits(prev_log_index, 1) {
                out.log(RAFT_LOG_ERROR,
                    "[HEARTBEAT-SEND] Log index exhausted after {}, skipping follower {}",
                    &[(prev_log_index).arg(),
                     (site_id).arg()]);
                skip_follower = true;
            } else {
                let next: u64 = core.peers_.next_index(ord);
                let slot = core.raft_log_.get(next);
                let usable: bool = slot.is_some() && slot.unwrap().has_value();
                if !usable {
                    out.log(RAFT_LOG_ERROR,
                        "[HEARTBEAT-SEND] Missing log entry {}, skipping follower {}",
                        &[(next).arg(),
                         (site_id).arg()]);
                    skip_follower = true;
                } else {
                    let entry: &RaftEntry<C> = slot.unwrap();
                    send.entry_term_ = entry.term() as u64;
                    send.cmds_.push(entry.cmd().clone());  // [move, M11]
                    send.payload_ = AppendPayload::RAW_ENTRY;
                    send.sent_end_index_ =
                        raft_server_append_sent_end(prev_log_index, 1);
                    // The kind tag identifies the payload better than the
                    // inner shared_ptr's raw address ever did.
                    let kind: i32 = entry.kind();
                    out.log(RAFT_LOG_DEBUG,
                        "[APPEND_SEND] site={} sending entry {} to follower {} cmd_kind={}",
                        &[(core.site_id_).arg(),
                         (next).arg(),
                         (site_id).arg(),
                         (kind).arg()]);
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
    out.log(RAFT_LOG_DEBUG,
        "[BATCH_CHECK] site={} follower={} next_index={} core.raft_log_.base()={} core.raft_log_.last_index()={}",
        &[(core.site_id_).arg(),
         (site_id).arg(),
         (core.peers_.next_index(ord)).arg(),
         (core.raft_log_.base()).arg(),
         (core.raft_log_.last_index()).arg()]);
    if !raft_server_append_entry_count_fits(prev_log_index, 1) {
        out.log(RAFT_LOG_ERROR,
            "[HEARTBEAT-BATCH] Log index exhausted after {}, skipping follower {}",
            &[(prev_log_index).arg(),
             (site_id).arg()]);
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
        out.log(RAFT_LOG_ERROR,
            "[HEARTBEAT-BATCH] Non-contiguous source for follower {}: prev={} start={} min_active={}; refusing to compress a hole",
            &[(site_id).arg(),
             (prev_log_index).arg(),
             (batch_start_idx).arg(),
             (core.raft_log_.base()).arg()]);
        skip_follower = true;
    } else if !skip_follower {
        let mut idx: u64 = batch_start_idx;
        while idx <= core.raft_log_.last_index() && batched < max_batch_entries
            invariant_except_break
                !skip_follower,
                // [M12] the batch so far: the entries prev + 1 .. idx - 1
                batched as int == idx as int - batch_start_idx as int,
                send.payload_ == AppendPayload::HEARTBEAT,
                send.sent_end_index_ == old(send).sent_end_index_,
            invariant
                core.inv(),
                (prev_log_index as int) < u64::MAX,
                // [M12]
                batch_start_idx as int == prev_log_index as int + 1,
                idx as int <= core.raft_log_.spec_last_index() + 1,
                send.site_id_ == old(send).site_id_,
                send.term_ == old(send).term_,
                send.prev_log_index_ == old(send).prev_log_index_,
                send.prev_log_term_ == old(send).prev_log_term_,
                send.commit_index_ == old(send).commit_index_,
            ensures
                // [M12] a raw entry, at prev + 1, ends the loop
                send.payload_ == AppendPayload::RAW_ENTRY ==> {
                    &&& batched == 0
                    &&& send.sent_end_index_ as int == prev_log_index as int + 1
                    &&& prev_log_index as int + 1 <= core.raft_log_.spec_last_index()
                },
                send.payload_ != AppendPayload::RAW_ENTRY ==> {
                    &&& send.payload_ == AppendPayload::HEARTBEAT
                    &&& send.sent_end_index_ == old(send).sent_end_index_
                    &&& (skip_follower || prev_log_index as int + batched as int <= core.raft_log_.spec_last_index())
                },
                send.site_id_ == old(send).site_id_,
                send.term_ == old(send).term_,
                send.prev_log_index_ == old(send).prev_log_index_,
                send.prev_log_term_ == old(send).prev_log_term_,
                send.commit_index_ == old(send).commit_index_,
            decreases core.raft_log_.spec_last_index() + 1 - idx,
        {
            let entry = core.raft_log_.get(idx);
            let usable: bool = entry.is_some() && entry.unwrap().has_value();
            if !usable {
                out.log(RAFT_LOG_ERROR,
                    "[HEARTBEAT-BATCH] Missing log entry {} for follower {}; refusing to compress a hole",
                    &[(idx).arg(),
                     (site_id).arg()]);
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
                send.cmds_.push(entry.unwrap().cmd().clone());  // [move, M11]
                send.terms_.push(entry_term);
                batched += 1;
            } else {
                let kind: i32 = entry.unwrap().kind();
                if batched == 0 {
                    out.log(RAFT_LOG_INFO,
                        "[BATCH_SKIP] site={} idx={}: log entry is not TpcCommitCommand (kind={}), using raw log",
                        &[(core.site_id_).arg(),
                         (idx).arg(),
                         (kind).arg()]);
                    send.cmds_.push(entry.unwrap().cmd().clone());  // [move, M11]
                    send.payload_ = AppendPayload::RAW_ENTRY;
                    send.entry_term_ = entry_term as u64;
                    send.sent_end_index_ =
                        raft_server_append_sent_end(prev_log_index, 1);
                } else {
                    out.log(RAFT_LOG_INFO,
                        "[BATCH_STOP] site={} idx={}: ending batch before non-TpcCommitCommand kind={}",
                        &[(core.site_id_).arg(),
                         (idx).arg(),
                         (kind).arg()]);
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
        out.log(RAFT_LOG_ERROR,
            "[HEARTBEAT-BATCH] Invalid encoded count {} after previous index {}; skipping follower {}",
            &[(batched).arg(),
             (prev_log_index).arg(),
             (site_id).arg()]);
        skip_follower = true;
    }
    if !skip_follower && batched > 0 {
        send.payload_ = AppendPayload::BATCH;
        send.sent_end_index_ = raft_server_append_sent_end(prev_log_index,
                                                           batched);
        let batch_end_idx: u64 = send.sent_end_index_;
        let truncated: bool = batch_end_idx < core.raft_log_.last_index();
        out.log(RAFT_LOG_INFO,
            "[BATCH_SEND] site={} sending batch of {} entries to follower {} (from={} to={}{})",
            &[(core.site_id_).arg(),
             (batched).arg(),
             (site_id).arg(),
             (batch_start_idx).arg(),
             (batch_end_idx).arg(),
             (if truncated { ", truncated" } else { "" }).arg()]);
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
pub fn heartbeat_tick<C: Clone>(core: &mut RaftCore<C>, is_leader: bool,
                      snapshot_configured: bool, batching: bool,
                      max_batch_entries: u64, max_batch_bytes: u64,
                      out: &mut CoreOutput) -> (r: HeartbeatTick<C>)
    requires
        old(core).inv(),
        // the host contract: is_leader is IsLeaderLocked, which reads the
        // core's role
        is_leader ==> old(core).is_leader_,
        // the gate (F5): the configuration contains this server
        old(core).config_members_@.contains(old(core).site_id_),
    ensures
        final(core).inv(),
        // [M12] PHASE 0's commit advance (LAdvanceCommitIndex, when the
        // commit index moved), then each follower's AppendEntries, one
        // LSendAppendEntries segment per component (BR1), built from the
        // leader's log (coupling::send_msg); a declined tick sends nothing
        old(core).ginv() && old(core).gated_ ==> {
            &&& final(core).ginv()
            &&& final(core).g_log_@ == crate::coupling::tick_sends_log(
                    (if final(core).commit_index_ != old(core).commit_index_ {
                        crate::coupling::advance_log(old(core).g_log_@, final(core).commit_index_ as int)
                    } else {
                        old(core).g_log_@
                    }),
                    r.sends_@, final(core).log_view(), final(core).config_members_@,
                    final(core).my_rank(), r.sends_@.len() as int)
        },
{
    let ghost pre = *core;
    let ghost on = pre.ginv() && pre.gated_;
    let mut tick: HeartbeatTick<C> = HeartbeatTick::new();

    // ---- PHASE 0 ----
    if is_leader && heartbeat_round_saturated(core.heartbeat_round_) {
        out.log(RAFT_LOG_ERROR,
            "[READ-INDEX] site={} heartbeat round saturated in term {}",
            &[(core.site_id_).arg(),
             (core.current_term_).arg()]);
    }
    if is_leader {
        let mut ord: usize = 0;
        while ord < core.peers_.len()
            invariant
                core.inv(),
                ord <= core.peers_.spec_len(),
            decreases core.peers_.spec_len() - ord,
        {
            out.log(RAFT_LOG_DEBUG,
                "[COMMIT-CALC] match_index_[{}] = {}",
                &[(core.peer_site_at(ord)).arg(),
                 (core.peers_.match_index(ord)).arg()]);
            ord += 1;
        }
    }

    let site_id: u16 = core.site_id_;
    let members: Vec<u16> = core.config_members_.clone();
    assert(members@ =~= core.config_members_@);
    let ghost h0 = core.heartbeat_round_;
    let outcome: Phase0Outcome = heartbeat_phase0_locked(
        core, &members, site_id, is_leader);
    tick.slots_reset_ = outcome.slots_reset();
    tick.slot_count_ = core.pending_rpcs_.len();
    if outcome.restart() {
        tick.declined_ = true;
        proof {
            // [M12] nothing sent
            assert(tick.sends_@ =~= Seq::<AppendSend<C>>::empty());
        }
        return tick;
    }
    if outcome.commit_advanced() {
        out.push(CoreAction::apply_range(outcome.commit_from(),
                                         outcome.commit_to()));
    }

    // The authority generation, opened under the guard now (the fiber that
    // owns the round state used to open it after releasing mtx_).
    proof {
        // below the saturated counter, this round's id is fresh
        if h0 != u64::MAX {
            core.authority_rounds_.lemma_ids_below_mono(h0, core.heartbeat_round_);
            core.authority_rounds_.lemma_ids_below_excludes(h0);
        }
    }
    let opened: bool = core.authority_rounds_.open(
        core.round_.round_id(), &members,
        HeartbeatAuthority::new(core.round_.term(), core.round_.nservers(),
                                site_id));
    core.round_.set_authority_inserted(opened);
    // heartbeat_round_ never wraps. The only possible duplicate is the
    // deliberately fail-closed UINT64_MAX saturation generation, which
    // open() declines rather than overwriting.
    if !core.round_.authority_inserted() {
        runtime_assert(core.round_.round_id() == u64::MAX);  // [move, M10]
    }
    tick.round_id_ = core.round_.round_id();
    // [M12] the state the sends are built from: PHASE 0's
    let ghost mid = *core;
    proof {
        if on {
            assert(tick.sends_@ =~= Seq::<AppendSend<C>>::empty());
        }
    }

    // ---- PHASE 1's decisions ----
    //
    // The ordinal is used for ITERATION ONLY; every read and write of a
    // follower's next index goes through core.peers_.next_index(ord).
    let ghost round0 = core.round_;
    let ghost n0 = core.peers_.spec_len();
    let mut ord: usize = 0;
    while ord < core.peers_.len()
        invariant
            core.inv(),
            core.peers_.spec_len() == n0,
            core.pending_rpcs_.spec_len() == n0,
            ord <= n0,
            core.round_ == round0,
            core.authority_rounds_.spec_has_id(round0.spec_round_id()),
            // [M12] the spec's state is PHASE 0's; the ghost log, PHASE 0's
            // and then the sends so far
            crate::coupling::spec_fields_same(core, &mid),
            mid.inv(),
            on ==> mid.ginv(),
            on ==> mid.gated_,
            on ==> core.ginv(),
            on ==> core.g_log_@ == crate::coupling::tick_sends_log(mid.g_log_@, tick.sends_@,
                mid.log_view(), mid.config_members_@, mid.my_rank(), tick.sends_@.len() as int),
        decreases n0 - ord,
    {
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
            out.log(RAFT_LOG_WARN,
                "[APPEND_ENTRIES] Repairing wrapped next_index for follower {} at leader last index {}",
                &[(peer).arg(),
                 (core.raft_log_.last_index()).arg()]);
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
            out.log(RAFT_LOG_INFO,
                "[APPEND_ENTRIES] ERROR: prevLogIndex ({}) > core.raft_log_.last_index() ({}), fixing next_index",
                &[(prev_log_index).arg(),
                 (core.raft_log_.last_index()).arg()]);
            let last: u64 = core.raft_log_.last_index();
            let repaired: u64 = if raft_server_log_index_has_successor(last) {
                raft_server_follower_next_index(last)
            } else {
                last
            };
            core.peers_.set_next_index(ord, repaired);
            prev_log_index = core.peers_.next_index(ord) - 1;
        }
        let mut send: AppendSend<C> = AppendSend::heartbeat(
            ord, peer, core.round_.term(), prev_log_index, send_commit_index,
            core.round_.round_id());

        let mut skip_follower: bool = false;
        if prev_log_index > core.raft_log_.last_index() {
            out.log(RAFT_LOG_INFO,
                "[APPEND_ENTRIES] WARNING: Cannot send AppendEntries to follower {}: prevLogIndex ({}) > core.raft_log_.last_index() ({}), skipping",
                &[(peer).arg(),
                 (prev_log_index).arg(),
                 (core.raft_log_.last_index()).arg()]);
            core.peers_.set_next_index(ord, 1);
            skip_follower = true;
        } else if core.peers_.next_index(ord) < core.raft_log_.base()
            && snapshot_configured
        {
            // The follower is behind the log's base, so send it a snapshot
            // instead of entries it can no longer be given. Sent or not,
            // the normal AppendEntries is skipped for this follower.
            out.log(RAFT_LOG_INFO,
                "[HEARTBEAT-SNAPSHOT] Site {}: Follower {} next_index={} < core.raft_log_.base()={}, sending InstallSnapshot",
                &[(site_id).arg(),
                 (peer).arg(),
                 (core.peers_.next_index(ord)).arg(),
                 (core.raft_log_.base()).arg()]);
            tick.snapshots_.push(SnapshotSend {
                ord_: ord,
                site_id_: peer,
                term_: core.current_term_,
            });
            skip_follower = true;
        } else {
            runtime_assert(prev_log_index <= core.raft_log_.last_index());  // [move, M10]
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
                    out.log(RAFT_LOG_ERROR,
                        "[HEARTBEAT-SEND] [CRITICAL] log entry {} is absent! Skipping follower {}",
                        &[(prev_log_index).arg(),
                         (peer).arg()]);
                    skip_follower = true;
                } else {
                    send.prev_log_term_ = instance.unwrap().term() as u64;
                }
            }
            if !skip_follower {
                skip_follower = heartbeat_select_payload(
                    core, ord, peer, prev_log_index, batching,
                    max_batch_entries, max_batch_bytes, &mut send, out);  // [move, M7]
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
            runtime_assert(launched);
        }
        let ghost sv = send;  // [M12]
        let ghost sends0 = tick.sends_@;  // [M12]
        tick.sends_.push(send);
        proof {
            if on {
                // [M12] this send's components, from PHASE 0's log
                let cfg = mid.config_members_@;
                let log = mid.log_view();
                let me = mid.my_rank();
                let f = crate::coupling::rank(cfg, sv.site_id_);
                let has_entry = sv.payload_ != AppendPayload::HEARTBEAT;
                let k = crate::coupling::send_count(sv);
                let lb = core.g_log_@;
                crate::coupling::lemma_my_rank(&mid);
                crate::coupling::lemma_rank_bounds(cfg, sv.site_id_);
                mid.raft_log_.lemma_wf_bounds();
                assert forall|j: int| 0 <= j < log.len()
                    implies (#[trigger] log[j]).change == glr::protocol::Raft::types::LConfChange::NoChange by {
                    assert(log[j] == crate::coupling::entry_view(mid.raft_log_.view()[j]));
                }
                if sv.prev_log_index_ > 0 {
                    assert(mid.raft_log_.view()[sv.prev_log_index_ - 1].spec_term() >= 0);
                    assert(log[sv.prev_log_index_ - 1] == crate::coupling::entry_view(mid.raft_log_.view()[sv.prev_log_index_ - 1]));
                }
                crate::coupling::lemma_send_segs(lb, mid.c_view(), log, me, f, sv.term_ as int,
                    sv.prev_log_index_ as int, sv.prev_log_term_ as int, sv.commit_index_ as int,
                    has_entry, k);
                crate::coupling::lemma_tick_sends_push(mid.g_log_@, sends0, sv, log, cfg, me,
                    sends0.len() as int);
                let ghost before = *core;
                core.g_log_@ = crate::coupling::send_segs(lb, log, me, f, sv.term_ as int,
                    sv.prev_log_index_ as int, sv.prev_log_term_ as int, sv.commit_index_ as int,
                    has_entry, k);
                crate::coupling::lemma_same_fields_ginv(&before, core);
            }
        }
        ord += 1;
    }
    tick.has_authority_ =
        core.authority_rounds_.has_quorum(core.round_.round_id());
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
    // The follower's ordinal (ghost).
    pub closed spec fn spec_ordinal(&self) -> usize {
        self.ordinal_
    }

    // [M12] The term it was sent at (ghost).
    pub closed spec fn spec_term(&self) -> u64 {
        self.term_
    }

    pub fn new(follower: u16, term: u64, round: u64, end_index: u64, ordinal: usize) -> (r: SentAppend)
        ensures
            r.spec_ordinal() == ordinal,
            r.spec_term() == term,  // [M12]
    {
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
    // The reply's term (ghost).
    pub closed spec fn spec_term(&self) -> u64 {
        self.term_
    }

    // [M12] Whether there was a reply, its status and its reported index
    // (ghost).
    pub closed spec fn spec_available(&self) -> bool {
        self.available_
    }

    pub closed spec fn spec_status(&self) -> bool {
        self.status_
    }

    pub closed spec fn spec_last(&self) -> u64 {
        self.last_log_index_
    }

    pub fn new(available: bool, status: bool, term: u64, last_log_index: u64) -> (r: AppendReply)
        ensures
            r.spec_term() == term,
            // [M12]
            r.spec_available() == available,
            r.spec_status() == status,
            r.spec_last() == last_log_index,
    {
        AppendReply { available_: available, status_: status, term_: term,
                      last_log_index_: last_log_index }
    }
}

#[allow(non_camel_case_types)]
#[cfg_attr(not(any()), derive(Clone, Copy, Debug, Eq, PartialEq))]
#[derive(Structural)]  // [M12] ghost: `==` is equality to the verifier
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
    // What the reply meant, and the backoff it took (ghost).
    pub closed spec fn spec_action(&self) -> AppendReplyAction {
        self.action_
    }

    pub closed spec fn spec_rung(&self) -> BackoffKind {
        self.rung_
    }

    pub closed spec fn spec_old_next(&self) -> u64 {
        self.old_next_
    }

    pub closed spec fn spec_new_next(&self) -> u64 {
        self.new_next_
    }

    pub fn action(&self) -> (r: AppendReplyAction)
        ensures r == self.spec_action(),
    {
        self.action_
    }

    // BACKED_OFF only.
    pub fn rung(&self) -> (r: BackoffKind)
        ensures r == self.spec_rung(),
    {
        self.rung_
    }

    pub fn old_next(&self) -> (r: u64)
        ensures r == self.spec_old_next(),
    {
        self.old_next_
    }

    pub fn new_next(&self) -> (r: u64)
        ensures r == self.spec_new_next(),
    {
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

fn append_reply_nothing(action: AppendReplyAction) -> (r: AppendReplyOutcome)
    ensures
        r.spec_action() == action,
        r.spec_rung() == BackoffKind::FLOOR,
{
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
pub fn heartbeat_apply_append_reply<C: Clone>(
    core: &mut RaftCore<C>,  // [move, M2] the ledger is core's now
    sent: &SentAppend,
    reply: &AppendReply,
    log_last_index: u64,
    is_leader: bool,
) -> (r: AppendReplyOutcome)
    requires
        old(core).inv(),
        sent.spec_ordinal() <= old(core).peers_.spec_len(),
        // the host contract: a term off the wire is below the ceiling
        (reply.spec_term() as int) < raft_index_limit(),
    ensures
        final(core).inv(),
        final(core).pending_rpcs_ == old(core).pending_rpcs_,
        final(core).round_ == old(core).round_,
        final(core).peers_.spec_len() == old(core).peers_.spec_len(),
        r.spec_action() == AppendReplyAction::ACCEPTED ==> sent.spec_ordinal() < final(core).peers_.spec_len(),
        r.spec_action() == AppendReplyAction::BACKED_OFF && r.spec_rung() == BackoffKind::FAST
            ==> r.spec_old_next() > r.spec_new_next(),
        // [M12] what the reply wrote: a higher term (STEP_DOWN); a leader's
        // match raised by max() to at most the reported index (ACCEPTED, a
        // success at the current term); nothing else the spec sees
        r.spec_action() == AppendReplyAction::STEP_DOWN ==> {
            &&& reply.spec_available()
            &&& reply.spec_term() > old(core).current_term_
            &&& final(core).current_term_ == reply.spec_term()
            &&& final(core).vote_for_ == RAFT_SERVER_INVALID_SITE_ID
        },
        r.spec_action() != AppendReplyAction::STEP_DOWN ==> {
            &&& final(core).current_term_ == old(core).current_term_
            &&& final(core).vote_for_ == old(core).vote_for_
        },
        r.spec_action() == AppendReplyAction::ACCEPTED ==> {
            &&& reply.spec_available()
            &&& reply.spec_status()
            &&& reply.spec_term() == old(core).current_term_
            &&& is_leader
            &&& (final(core).peers_.spec_match(sent.spec_ordinal() as int)
                    == old(core).peers_.spec_match(sent.spec_ordinal() as int)
                || final(core).peers_.spec_match(sent.spec_ordinal() as int) <= reply.spec_last())
        },
        forall|o: int| 0 <= o < old(core).peers_.spec_len()
            && (r.spec_action() != AppendReplyAction::ACCEPTED || o != sent.spec_ordinal())
            ==> #[trigger] final(core).peers_.spec_match(o) == old(core).peers_.spec_match(o),
        final(core).is_leader_ == old(core).is_leader_,
        final(core).election_in_progress_ == old(core).election_in_progress_,
        final(core).election_term_ == old(core).election_term_,
        final(core).raft_log_ == old(core).raft_log_,
        final(core).commit_index_ == old(core).commit_index_,
        final(core).config_members_ == old(core).config_members_,
        final(core).site_id_ == old(core).site_id_,
        final(core).snapterm_ == old(core).snapterm_,
        final(core).peer_sites_ == old(core).peer_sites_,
        final(core).gated_ == old(core).gated_,
        final(core).g_log_ == old(core).g_log_,
        final(core).g_votes_ == old(core).g_votes_,
        final(core).g_match_ == old(core).g_match_,
        final(core).g_next_ == old(core).g_next_,
{
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
    pub stepped_down_: bool,
    // The reply's RPC went out in an earlier round, so its follower's slot
    // opened after PHASE 1 and another round should be prompted.
    pub completed_previous_round_: bool,
    pub has_authority_: bool,
}

// [move, M5] PHASE 2's per-slot body, as one core call under mtx_: what the
// completed reply in slot `ord` means, applied, and the slot released. The
// reply arrives as the three scalars the shell read out of its handle's wire
// view ([move, M11]); `stopped` and `failover` are the shell's, for a
// step-down.
#[allow(clippy::too_many_arguments)]
pub fn heartbeat_on_reply<C: Clone>(core: &mut RaftCore<C>, ord: usize,
                          resp_status: bool, resp_term: u64,  // [move, M11]
                          resp_last_log_index: u64, is_leader: bool,  // [move, M11]
                          stopped: bool, failover: bool,
                          out: &mut CoreOutput) -> ReplyResult
    requires
        old(core).inv(),
        // the host contract: the shell's response slots are the core's, and
        // a term off the wire is below the ceiling
        ord < old(core).pending_rpcs_.spec_len(),
        (resp_term as int) < raft_index_limit(),
    ensures
        final(core).inv(),
        // [M12] the reply's group (coupling::reply_log_ok), from the
        // follower its slot was sent to: a higher term's StepDown, a success
        // past the spec's match its HandleAppendResponse, or nothing the
        // spec sees. The host contract: is_leader is IsLeaderLocked, and a
        // success never reports past the leader's log (F9's step_checked
        // drops one that does)
        old(core).ginv() && old(core).gated_ && is_leader == old(core).is_leader_
            && (resp_status ==> resp_last_log_index as int <= old(core).raft_log_.spec_last_index()) ==> {
            &&& final(core).ginv()
            &&& crate::coupling::reply_log_ok(final(core).g_log_@, old(core).g_log_@,
                    crate::coupling::rank(old(core).config_members_@,
                        old(core).pending_rpcs_.spec_follower(ord as int)),
                    crate::coupling::reply_msg(old(core).config_members_@,
                        old(core).pending_rpcs_.spec_follower(ord as int), resp_status, resp_term,
                        resp_last_log_index),
                    old(core).g_match_@, old(core).g_next_@)
        },
{
    let ghost pre = *core;
    let ghost on = pre.ginv() && pre.gated_ && is_leader == pre.is_leader_
        && (resp_status ==> resp_last_log_index as int <= pre.raft_log_.spec_last_index());
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
        !(!resp_status && resp_term == 0 && resp_last_log_index == 0);  // [move, M11]
    let resp_ord: usize = core.peer_ordinal(follower_id);
    let log_last_index: u64 = core.raft_log_.last_index();
    let outcome: AppendReplyOutcome = heartbeat_apply_append_reply(
        core,
        &SentAppend::new(follower_id, sent_term, sent_round, sent_end_index,
                         resp_ord),
        &AppendReply::new(response_available, resp_status, resp_term,  // [move, M11]
                          resp_last_log_index),  // [move, M11]
        log_last_index,
        is_leader);

    let action: AppendReplyAction = outcome.action();
    if action == AppendReplyAction::STEP_DOWN {
        out.log(RAFT_LOG_INFO,
            "[STEPDOWN] Site {}: AppendEntries response from follower {} carried higher term {} > {}",
            &[(core.site_id_).arg(),
             (follower_id).arg(),
             (resp_term).arg(),
             (outcome.previous_term()).arg()]);
        let now_term: u64 = core.current_term_;
        core.log_term_change("AppendEntries response carried newer term",
                             outcome.previous_term(), now_term, follower_id, out);  // [move, M7]
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
            out.log(RAFT_LOG_INFO,
                "[LOG-RECONCILE] Site {}: Fast backoff for follower {}: next_index {} -> {} (gap: {}, follower reported last: {})",
                &[(core.site_id_).arg(),
                 (follower_id).arg(),
                 (outcome.old_next()).arg(),
                 (outcome.new_next()).arg(),
                 (outcome.old_next() - outcome.new_next()).arg(),
                 (resp_last_log_index).arg()]);
        } else if rung == BackoffKind::TERM_CONFLICT {
            out.log(RAFT_LOG_INFO,
                "[LOG-RECONCILE] Site {}: Term-conflict backoff for follower {}: next_index {} -> {}",
                &[(core.site_id_).arg(),
                 (follower_id).arg(),
                 (outcome.old_next()).arg(),
                 (outcome.new_next()).arg()]);
        } else if rung == BackoffKind::EXPONENTIAL {
            out.log(RAFT_LOG_INFO,
                "[LOG-RECONCILE] Site {}: Exponential backoff for follower {}: next_index {} -> {} (halved)",
                &[(core.site_id_).arg(),
                 (follower_id).arg(),
                 (outcome.old_next()).arg(),
                 (outcome.new_next()).arg()]);
        } else if rung == BackoffKind::LINEAR {
            out.log(RAFT_LOG_DEBUG,
                "[LOG-RECONCILE] Site {}: Linear backoff for follower {}: next_index {} -> {}",
                &[(core.site_id_).arg(),
                 (follower_id).arg(),
                 (outcome.old_next()).arg(),
                 (outcome.new_next()).arg()]);
        }
        // BackoffKind::FLOOR logs nothing, as before.
    } else if action == AppendReplyAction::ACCEPTED {
        out.log(RAFT_LOG_DEBUG,
            "[APPEND_RPC] Leader {} accepted follower {} proof: kind={} reported={} sent_end={} acknowledged={} next={} match={}",
            &[(core.site_id_).arg(),
             (follower_id).arg(),
             (if cmd_has_value { "entries" } else { "heartbeat" }).arg(),
             (resp_last_log_index).arg(),
             (sent_end_index).arg(),
             (outcome.acknowledged()).arg(),
             (core.peers_.next_index(resp_ord)).arg(),
             (core.peers_.match_index(resp_ord)).arg()]);
    } else if action == AppendReplyAction::CONTRADICTORY {
        out.log(RAFT_LOG_WARN,
            "[APPEND_RPC] Ignoring contradictory success from follower {}: reported_end={} sent_end={}",
            &[(follower_id).arg(),
             (resp_last_log_index).arg(),
             (sent_end_index).arg()]);
    } else if action == AppendReplyAction::UNKNOWN_FOLLOWER {
        out.log(RAFT_LOG_DEBUG,
            "[APPEND_RPC] Ignoring replication response from removed follower {}",
            &[(follower_id).arg()]);
    }
    // AppendReplyAction::IGNORED does nothing, as before.

    let completed_previous_round: bool = sent_round != core.round_.round_id();
    core.pending_rpcs_.release(ord);
    proof {
        if on {
            let cfg = pre.config_members_@;
            let f = crate::coupling::rank(cfg, follower_id);
            let m = crate::coupling::reply_msg(cfg, follower_id, resp_status, resp_term,
                resp_last_log_index);
            if action == AppendReplyAction::STEP_DOWN {
                core.g_log_@ = crate::coupling::step_down_seg(
                    pre.g_log_@.push(glr::protocol::Raft::ghost_log::Entry::Recv(f, m)), resp_term as int);
                core.g_votes_@ = Set::<int>::empty();
                crate::coupling::lemma_recv_step_down(&pre, core, f, m);
            } else if action == AppendReplyAction::ACCEPTED
                && resp_last_log_index as int > crate::coupling::match_of(pre.g_match_@, f)
            {
                core.g_log_@ = crate::coupling::har_log(pre.g_log_@, f, resp_term as int,
                    resp_last_log_index as int, pre.g_match_@, pre.g_next_@);
                core.g_match_@ = pre.g_match_@.insert(f as u64, resp_last_log_index);
                core.g_next_@ = pre.g_next_@.insert(f as u64,
                    glr::protocol::Raft::raft::u64_inc(resp_last_log_index));
                crate::coupling::lemma_har_ginv(&pre, core, resp_ord as int, resp_term,
                    resp_last_log_index);
            } else {
                crate::coupling::lemma_reply_stutter(&pre, core,
                    action == AppendReplyAction::ACCEPTED, resp_ord as int, resp_last_log_index);
            }
        }
    }
    ReplyResult {
        stepped_down_: stepped_down,
        completed_previous_round_: completed_previous_round,
        has_authority_:
            core.authority_rounds_.has_quorum(core.round_.round_id()),
    }
}

// [move, M5] The shell's half of a leadership loss during collection: both
// halves of every in-flight slot, and the authority evidence, dropped.
pub fn heartbeat_abandon_round<C: Clone>(core: &mut RaftCore<C>)
    requires old(core).inv(),
    ensures
        final(core).inv(),
        old(core).ginv() ==> final(core).ginv(),  // [M12] unseen by the spec
{
    core.pending_rpcs_.abandon();
    core.authority_rounds_.abandon();
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
    pub fn commit(&self) -> (r: &CommitAdvance)
        ensures *r == self.spec_commit(),  // [M12]
    {
        &self.commit_
    }

    // [M12] The commit advance (ghost).
    pub closed spec fn spec_commit(&self) -> CommitAdvance {
        self.commit_
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
pub fn heartbeat_phase3_locked<C: Clone>(
    core: &mut RaftCore<C>,  // [move, M2] the ledger is core's now
    nservers: usize,
    members: &[u16],
    is_leader: bool,
) -> (r: Phase3Outcome)
    requires
        old(core).inv(),
        nservers == old(core).round_.spec_nservers(),
        nservers > 0,
        // the gate (F5): the configuration contains this server
        old(core).config_members_@.contains(old(core).site_id_),
    ensures
        final(core).inv(),
        // [M12] a leader's advance is LAdvanceCommitIndex (bugs-found B17:
        // the advance does not check is_leader, so this needs the server to
        // lead); the read-index settlement is unseen by the spec
        old(core).ginv() && old(core).gated_ && old(core).is_leader_ ==> {
            &&& final(core).ginv()
            &&& final(core).g_log_@ == (if r.spec_commit().spec_advanced() {
                    crate::coupling::advance_log(old(core).g_log_@, final(core).commit_index_ as int)
                } else {
                    old(core).g_log_@
                })
        },
{
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
pub fn heartbeat_round_end<C: Clone>(core: &mut RaftCore<C>, is_leader: bool,
                           out: &mut CoreOutput) -> (r: bool)
    requires
        old(core).inv(),
        // the host contract: a round end follows a tick that opened the
        // round (the round's membership is latched)
        old(core).round_.spec_nservers() > 0,
        // the gate (F5): the configuration contains this server
        old(core).config_members_@.contains(old(core).site_id_),
    ensures
        final(core).inv(),
        // [M12] the commit advance is LAdvanceCommitIndex, to the new commit
        // index. The premise: is_leader is IsLeaderLocked, and the server
        // leads (bugs-found B17: the shell checks leadership before taking
        // mtx_, and the advance does not check it)
        old(core).ginv() && old(core).gated_ && is_leader == old(core).is_leader_ && is_leader ==> {
            &&& final(core).ginv()
            &&& final(core).g_log_@ == (if r {
                    crate::coupling::advance_log(old(core).g_log_@, final(core).commit_index_ as int)
                } else {
                    old(core).g_log_@
                })
        },
{
    let members: Vec<u16> = core.config_members_.clone();
    let nservers: usize = core.round_.nservers();
    let outcome: Phase3Outcome = heartbeat_phase3_locked(
        core, nservers, &members, is_leader);  // [move, M2]
    let mut advanced: bool = false;
    if outcome.commit().advanced() {
        out.log(RAFT_LOG_DEBUG,
            "[PHASE3-COMMIT] Advancing core.commit_index_ {} -> {}",
            &[(outcome.commit().from_index()).arg(),
             (outcome.commit().to_index()).arg()]);
        out.push(CoreAction::apply_range(outcome.commit().from_index(),
                                         outcome.commit().to_index()));
        advanced = true;
    }
    if outcome.confirmed() {
        out.log(RAFT_LOG_DEBUG,
            "[READ-INDEX] site={} confirmed round={} term={}",
            &[(core.site_id_).arg(),
             (core.read_quorum_confirmed_round_).arg(),
             (core.read_quorum_confirmed_term_).arg()]);
    }
    advanced
}

} // verus!
