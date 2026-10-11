// [fix, F21] The persist note (docs/verus/disk-persistence.md §3): every
// step's note, applied to the saved state before it, gives the saved state
// after it, through elections, proposals, appends with conflicts, votes and
// commits; a step that changes nothing saved leaves none.

mod common;
use common::*;
use raft_core::*;

#[test]
fn the_design_s_follower_step() {
    // design §2: at term 4 a follower holds 1-10, of which 6-10 are of term
    // 2; the leader of term 5 sends prev 5, 6'-8' and commit 8.
    let mut core = configured();
    let old: Vec<(i64, u64)> = (1..=10).map(|i| (if i <= 5 { 1 } else { 2 }, i)).collect();
    let (_, n) = append(&mut core, 4, 2, (0, 0), &old, 5);
    let n = n.unwrap();
    assert!(n.hard_ && n.term_ == 4 && n.commit_ == 5 && n.log_from_ == 1);
    let (r, n) = append(&mut core, 5, 3, (5, 1), &[(5, 60), (5, 70), (5, 80)], 8);
    assert!(matches!(r, Reply::Append { ok: 1, .. }));
    let n = n.unwrap();
    assert!(n.hard_ && n.term_ == 5 && n.vote_ == RAFT_SERVER_INVALID_SITE_ID && n.commit_ == 8);
    assert_eq!(n.log_from_, 6);
    assert_eq!(core.raft_log_.last_index(), 8);
}

#[test]
fn a_cut_then_an_equal_length_append() {
    // Decision 4: the log's length does not change, only a scan finds where
    // the cut began; the note knows.
    let mut core = configured();
    append(&mut core, 2, 2, (0, 0), &[(1, 1), (1, 2), (2, 3), (2, 4), (2, 5)], 2);
    let (_, n) = append(&mut core, 3, 3, (2, 1), &[(3, 30), (3, 40), (3, 50)], 2);
    let n = n.unwrap();
    assert_eq!((n.log_from_, core.raft_log_.last_index()), (3, 5));
}

#[test]
fn refusals_and_repeats_leave_no_note() {
    let mut core = configured();
    append(&mut core, 2, 2, (0, 0), &[(1, 1), (2, 2)], 1);
    // A stale term: refused, nothing saved.
    let (_, n) = append(&mut core, 1, 3, (2, 2), &[(1, 9)], 1);
    assert!(n.is_none());
    // A heartbeat that repeats what the follower holds: nothing saved.
    let (_, n) = append(&mut core, 2, 2, (2, 2), &[], 1);
    assert!(n.is_none());
    // The same entries again: rewritten from 1 (the note covers them) or not
    // at all, but exact either way (the check in common::step).
    append(&mut core, 2, 2, (0, 0), &[(1, 1), (2, 2)], 1);
}

#[test]
fn an_election_proposals_and_a_commit() {
    let mut core = configured();
    // The campaign: term + 1, the self-vote.
    let (c, n) = step::<Batch>(&mut core, Event::StartElection {
        timer_guarded: false, expected_generation: 0, now: 0, stopped: false });
    let c = c.into_campaign();
    let n = n.unwrap();
    assert!(n.hard_ && n.term_ == c.term_ && n.vote_ == 1 && n.log_from_ == 0);
    let (won, _) = step::<Batch>(&mut core, Event::SettleElection {
        term: c.term_, loc_id: 0, voters: &[2], granted: &[true], reply_terms: &[c.term_ as i64],
        n_total: 3, timed_out: false, stopped: false, looping: true, failover: true,
        election_debug: false });
    assert!(won.into_settled() && core.is_leader_);
    // Proposals: the log from the new index, nothing else.
    for k in 0..3u64 {
        let (r, n) = step::<Batch>(&mut core, Event::Propose {
            cmd: Cmd(100 + k), has_value: true, is_tpc_commit: false, kind: 0, payload_bytes: 8 });
        let n = n.unwrap();
        assert!(!n.hard_);
        assert_eq!(n.log_from_, r.into_proposed() + 1);
    }
    // A round: the tick sends, a follower acknowledges all, the commit rises.
    let (t, _) = step::<Batch>(&mut core, Event::TickHeartbeat {
        is_leader: true, snapshot_configured: false, batching: true, max_batch_entries: 256,
        max_batch_bytes: 16 << 20 });
    let t = t.into_tick();
    let term = core.current_term_;
    let sent = t.sends_.iter().map(|s| s.sent_end_index_).min().unwrap();
    assert!(sent >= 1);
    for s in t.sends_.iter() {
        step::<Batch>(&mut core, Event::RecvAppendReply {
            ord: s.ord_, status: true, term, last_log_index: s.sent_end_index_, is_leader: true,
            stopped: false, failover: true });
    }
    step::<Batch>(&mut core, Event::RoundEnd { is_leader: true });
    // The commit rose to what both followers acknowledged, with its note.
    assert_eq!(core.commit_index_, sent);
}

#[test]
fn a_vote_is_saved() {
    let mut core = configured();
    let (r, n) = step::<Batch>(&mut core, Event::RecvRequestVote {
        stopped: false, candidate_is_current_voter: true, lst_log_idx: 0, lst_log_term: 0,
        can_id: 2, can_term: 3, failover: true, election_debug: false });
    assert!(matches!(r, Reply::Vote { granted: 1, .. }));
    let n = n.unwrap();
    assert!(n.hard_ && n.term_ == 3 && n.vote_ == 2);
    // The same request again: already voted, nothing new.
    let (_, n) = step::<Batch>(&mut core, Event::RecvRequestVote {
        stopped: false, candidate_is_current_voter: true, lst_log_idx: 0, lst_log_term: 0,
        can_id: 2, can_term: 3, failover: true, election_debug: false });
    assert!(n.is_none());
}
