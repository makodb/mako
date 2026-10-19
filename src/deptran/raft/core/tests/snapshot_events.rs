// [fix, F25, F27, F28, F29] The snapshot events (coupling design Part B,
// E1-E6): creation, the install's contact and outcome, the install's reply,
// and leaving the gates. Each step's persist note is exact (common::step);
// the raise a newer term makes (F20's, formerly the ObserveTerm event) is
// covered here through the two messages that carry one.

mod common;
use common::*;
use raft_core::*;

fn elect(mut core: RaftCore<Cmd>) -> RaftCore<Cmd> {
    let (c, _) = step::<Batch>(&mut core, Event::StartElection {
        timer_guarded: false, expected_generation: 0, now: 0, stopped: false });
    let c = c.into_campaign();
    step::<Batch>(&mut core, Event::SettleElection {
        term: c.term_, loc_id: 0, voters: &[2], granted: &[true], reply_terms: &[c.term_ as i64],
        n_total: 3, timed_out: false, stopped: false, looping: true, failover: true,
        election_debug: false });
    assert!(core.is_leader_);
    core
}

fn contact(core: &mut RaftCore<Cmd>, term: u64, leader: u16, index: u64, snap_term: u64)
    -> ((u64, bool, bool), Option<PersistNote>) {
    let (r, n) = step::<Batch>(core, Event::RecvInstallSnapshot {
        term, leader_site_id: leader, last_included_index: index, last_included_term: snap_term,
        applied: 0, stopped: false, failover: true });
    (r.into_install_contact(), n)
}

fn install(core: &mut RaftCore<Cmd>, index: u64, term: u64) -> ((bool, bool), Option<PersistNote>) {
    let (r, n) = step::<Batch>(core, Event::InstallSnapshot { index, term });
    (r.into_installed(), n)
}

fn reply(core: &mut RaftCore<Cmd>, send_term: u64, follower_term: u64, index: u64)
    -> (bool, Option<PersistNote>) {
    // ordinal 0 is site 2 for site 1 of {1, 2, 3}
    let (r, n) = step::<Batch>(core, Event::RecvSnapshotReply {
        ord: 0, site_id: 2, snap_last_idx: index, send_term, follower_term, stopped: false,
        failover: true });
    (r.into_snapshot_reply(), n)
}

/// A follower of leader 2 in term 1 holding 1..=3 (terms 1), commit 1.
fn follower_with_log() -> RaftCore<Cmd> {
    let mut core = configured_ungated();
    let (r, _) = append(&mut core, 1, 2, (0, 0), &[(1, 11), (1, 12), (1, 13)], 1);
    let (_, ok, _, _) = r.into_append();
    assert_eq!(ok, 1);
    core
}

#[test]
fn a_reply_with_a_newer_term_deposes_a_leader() {
    let mut core = elect(configured_ungated());
    let t = core.current_term_;
    let (credited, n) = reply(&mut core, t, t + 3, 0);
    assert!(!credited && !core.is_leader_);
    assert_eq!((core.current_term_, core.vote_for_), (t + 3, RAFT_SERVER_INVALID_SITE_ID));
    assert_eq!(core.current_leader_id_, RAFT_SERVER_INVALID_SITE_ID);
    let n = n.unwrap();
    assert!(n.hard_ && n.term_ == t + 3 && n.vote_ == RAFT_SERVER_INVALID_SITE_ID && n.log_from_ == 0);
}

#[test]
fn a_reply_at_or_below_the_term_saves_nothing() {
    let mut core = elect(configured_ungated());
    let t = core.current_term_;
    for follower_term in [t, t - 1, 0] {
        let (_, n) = reply(&mut core, t, follower_term, 0);
        assert!(n.is_none());
        assert!(core.is_leader_ && core.current_term_ == t && core.vote_for_ == 1);
    }
    // sent in an older term, or naming another site: not credited
    assert!(!reply(&mut core, t - 1, t, 0).0);
    let (r, _) = step::<Batch>(&mut core, Event::RecvSnapshotReply {
        ord: 0, site_id: 3, snap_last_idx: 0, send_term: t, follower_term: t, stopped: false,
        failover: true });
    assert!(!r.into_snapshot_reply());
}

#[test]
fn an_install_contact_with_a_newer_term_ends_a_campaign() {
    let mut core = configured_ungated();
    step::<Batch>(&mut core, Event::StartElection {
        timer_guarded: false, expected_generation: 0, now: 0, stopped: false });
    assert!(core.election_in_progress_);
    let t = core.current_term_;
    // index 0: covered by any progress, acknowledged without an install
    let ((term, accepted, inst), n) = contact(&mut core, t + 1, 2, 0, 0);
    assert_eq!((term, accepted, inst), (t + 1, true, false));
    assert!(!core.election_in_progress_ && !core.req_voting_ && !core.is_leader_);
    assert_eq!((core.current_term_, core.current_leader_id_), (t + 1, 2));
    assert!(n.unwrap().hard_);
}

#[test]
fn an_install_keeps_the_log_after_a_matching_boundary() {
    let mut core = follower_with_log();
    let ((term, accepted, inst), _) = contact(&mut core, 1, 2, 2, 1);
    assert_eq!((term, accepted, inst), (1, true, true));
    let ((taken, kept), n) = install(&mut core, 2, 1);
    assert!(taken && kept);
    assert_eq!((core.snapidx_, core.snapterm_, core.commit_index_), (2, 1, 2));
    assert_eq!((core.raft_log_.base(), core.raft_log_.last_index()), (3, 3));
    let n = n.unwrap();
    let s = n.snap_.unwrap();
    assert!(n.hard_ && n.commit_ == 2 && s.index_ == 2 && s.term_ == 1 && s.keep_);
}

#[test]
fn an_install_drops_the_log_after_a_mismatched_boundary() {
    let mut core = follower_with_log();
    // leader 2 in term 2, whose snapshot at 2 is of term 2
    let ((term, accepted, inst), _) = contact(&mut core, 2, 2, 2, 2);
    assert_eq!((term, accepted, inst), (2, true, true));
    let ((taken, kept), n) = install(&mut core, 2, 2);
    assert!(taken && !kept);
    assert_eq!((core.raft_log_.base(), core.raft_log_.last_index()), (3, 2));
    assert!(!n.unwrap().snap_.unwrap().keep_);
}

#[test]
fn stale_conflicting_and_refused_installs() {
    let mut core = follower_with_log();
    // at or below commit: acknowledged, nothing installed
    assert_eq!(contact(&mut core, 1, 2, 1, 1).0, (1, true, false));
    // a stale term answers with this server's
    let ((term, accepted, _), n) = contact(&mut core, 0, 2, 3, 0);
    assert_eq!((term, accepted), (1, false));
    assert!(n.is_none());
    // an impossible boundary term, an unknown sender, this server itself
    assert_eq!(contact(&mut core, 1, 2, 3, 2).0, (0, false, false));
    assert_eq!(contact(&mut core, 1, 7, 3, 1).0, (0, false, false));
    assert_eq!(contact(&mut core, 1, 1, 3, 1).0, (0, false, false));
    // E5 only where E4 admitted: at or below commit, or a future term
    assert_eq!(install(&mut core, 1, 1).0, (false, false));
    assert_eq!(install(&mut core, 3, 5).0, (false, false));
}

#[test]
fn inside_the_gates_snapshots_are_refused_until_it_leaves() {
    let mut core = configured();
    assert!(core.gated_);
    let before = saved(&core);
    // a stale term is still answered; everything else is refused, unchanged
    assert_eq!(contact(&mut core, 0, 2, 3, 0).0, (core.current_term_, false, false));
    assert_eq!(contact(&mut core, 4, 2, 3, 1).0, (0, false, false));
    assert_eq!(install(&mut core, 3, 1).0, (false, false));
    assert!(!reply(&mut core, 0, 9, 0).0);
    let (r, _) = step::<Batch>(&mut core, Event::CreateSnapshot { index: 0, term: 0 });
    assert!(!r.into_created());
    assert_eq!(saved(&core), before);
    // leaving the gates: the contact is taken
    let (r, n) = step::<Batch>(&mut core, Event::LeaveGates);
    r.into_done();
    assert!(n.is_none() && !core.gated_);
    assert_eq!(contact(&mut core, 4, 2, 0, 0).0, (4, true, false));
}

#[test]
fn a_snapshot_at_the_applied_index() {
    let mut core = follower_with_log();
    // nothing applied: no snapshot point
    assert!(core.snapshot_point().is_none());
    let (r, _) = step::<Batch>(&mut core, Event::Applied { index: 1, published: 0 });
    assert!(r.into_applied());
    assert_eq!(core.snapshot_point(), Some((1, 1)));
    // not the core's point: refused
    let (r, n) = step::<Batch>(&mut core, Event::CreateSnapshot { index: 1, term: 2 });
    assert!(!r.into_created() && n.is_none());
    let (r, n) = step::<Batch>(&mut core, Event::CreateSnapshot { index: 1, term: 1 });
    assert!(r.into_created());
    let s = n.unwrap().snap_.unwrap();
    assert!(s.index_ == 1 && s.term_ == 1 && s.keep_);
    assert_eq!((core.snapidx_, core.raft_log_.base(), core.raft_log_.last_index()), (1, 2, 3));
    // the boundary's own term once compacted
    assert_eq!(core.snapshot_point(), Some((1, 1)));
}
