// [fix, F20] ObserveTerm (docs/verus/disk-persistence-plan.md P1): a newer
// term raises the term, clears the vote, drops the role and ends a campaign,
// with a persist note; an equal or older one changes nothing.

mod common;
use common::*;
use raft_core::*;

fn leader() -> RaftCore<Cmd> {
    let mut core = configured();
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

#[test]
fn a_newer_term_deposes_a_leader() {
    let mut core = leader();
    let t = core.current_term_;
    let (r, n) = step::<Batch>(&mut core, Event::ObserveTerm { term: t + 3, stopped: false, failover: true });
    r.into_done();
    assert!(!core.is_leader_);
    assert_eq!((core.current_term_, core.vote_for_), (t + 3, RAFT_SERVER_INVALID_SITE_ID));
    assert_eq!(core.current_leader_id_, RAFT_SERVER_INVALID_SITE_ID);
    let n = n.unwrap();
    assert!(n.hard_ && n.term_ == t + 3 && n.vote_ == RAFT_SERVER_INVALID_SITE_ID && n.log_from_ == 0);
}

#[test]
fn a_newer_term_ends_a_campaign() {
    let mut core = configured();
    step::<Batch>(&mut core, Event::StartElection {
        timer_guarded: false, expected_generation: 0, now: 0, stopped: false });
    assert!(core.election_in_progress_);
    let t = core.current_term_;
    step::<Batch>(&mut core, Event::ObserveTerm { term: t + 1, stopped: false, failover: true });
    assert!(!core.election_in_progress_ && !core.req_voting_);
    assert_eq!(core.current_term_, t + 1);
}

#[test]
fn an_equal_or_older_term_changes_nothing() {
    let mut core = leader();
    let t = core.current_term_;
    for term in [t, t - 1, 0] {
        let (_, n) = step::<Batch>(&mut core, Event::ObserveTerm { term, stopped: false, failover: true });
        assert!(n.is_none());
        assert!(core.is_leader_ && core.current_term_ == t && core.vote_for_ == 1);
    }
}
