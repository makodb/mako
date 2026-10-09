// [fix, F22] Restore (docs/verus/disk-persistence-plan.md P1): a recovered
// state is loaded before EnterGates, the committed prefix queued for apply,
// no persist note left; a state no step produces is refused, unchanged.

mod common;
use common::*;
use raft_core::*;

fn restore(core: &mut RaftCore<Cmd>, term: u64, vote: u16, commit: u64, entries: Vec<RaftEntry<Cmd>>)
    -> (bool, CoreOutput) {
    let mut rev = entries;
    rev.reverse();
    let mut out = CoreOutput::new();
    let ok = core.step::<Batch>(Event::Restore { term, vote, commit, entries_rev: rev }, &mut out)
        .into_restored();
    (ok, out)
}

#[test]
fn a_state_is_restored_then_gated() {
    let mut core = configured_ungated();
    let entries = vec![entry(1, 11), entry(1, 12), entry(3, 13), entry(4, 14)];
    let (ok, mut out) = restore(&mut core, 5, 2, 3, entries);
    assert!(ok);
    assert_eq!((core.current_term_, core.vote_for_, core.commit_index_), (5, 2, 3));
    assert_eq!(core.raft_log_.last_index(), 4);
    assert_eq!(core.raft_log_.get(3).unwrap().cmd().0, 13);
    // The apply of 1..=3 is queued; no note (the store already holds this).
    assert_eq!(out.len(), 1);
    assert_eq!(out.at(0).kind(), CoreActionKind::APPLY_RANGE);
    assert_eq!((out.at(0).from(), out.at(0).to()), (0, 3));
    assert!(out.take_persist().is_none());
    assert!(core.step::<Batch>(Event::EnterGates { snapshots_enabled: false, failover: true },
                               &mut CoreOutput::new()).into_gates());
    // The restored server carries on: an append after its log is exact too.
    let (r, n) = append(&mut core, 5, 2, (4, 4), &[(5, 15)], 5);
    assert!(matches!(r, Reply::Append { ok: 1, .. }));
    assert_eq!(n.unwrap().log_from_, 5);
}

#[test]
fn an_empty_state_is_restored() {
    let mut core = configured_ungated();
    let (ok, out) = restore(&mut core, 0, RAFT_SERVER_INVALID_SITE_ID, 0, vec![]);
    assert!(ok);
    assert_eq!(out.at(0).to(), 0);
}

#[test]
fn states_no_step_produces_are_refused_unchanged() {
    let ok = || vec![entry(1, 1), entry(2, 2), entry(2, 3)];
    let cases: Vec<(&str, u64, u16, u64, Vec<RaftEntry<Cmd>>)> = vec![
        ("commit past the log", 3, 2, 4, ok()),
        ("vote for a non-member", 3, 9, 1, ok()),
        ("an entry without a value", 3, 2, 1,
         vec![entry(1, 1), RaftEntry::new(2, Cmd(2), false, false, 0, 0)]),
        ("an entry of term 0", 3, 2, 1, vec![entry(0, 1)]),
        ("falling terms", 3, 2, 1, vec![entry(2, 1), entry(1, 2)]),
        ("a last term above the term", 1, 2, 1, ok()),
        ("a term at the ceiling", RAFT_INDEX_LIMIT, 2, 1, ok()),
    ];
    for (why, term, vote, commit, entries) in cases {
        let mut core = configured_ungated();
        let before = saved(&core);
        let (accepted, mut out) = restore(&mut core, term, vote, commit, entries);
        assert!(!accepted, "{why}: accepted");
        assert_eq!(saved(&core), before, "{why}: changed the core");
        assert!(out.is_empty() && out.take_persist().is_none(), "{why}: left output");
    }
}

#[test]
fn a_term_below_the_current_one_is_refused() {
    let mut core = configured_ungated();
    core.current_term_ = 4; // as a lab case's startup bump would leave it
    let (ok, _) = restore(&mut core, 3, 2, 0, vec![]);
    assert!(!ok);
}
