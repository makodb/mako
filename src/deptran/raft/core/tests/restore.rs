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

/// A core whose snapshot boundary is at `s` (term `t`) and whose log is
/// empty after it, as snapshot recovery leaves it before Restore.
fn at_boundary(s: u64, t: i64) -> RaftCore<Cmd> {
    let mut core = configured_ungated();
    core.snapidx_ = s;
    core.snapterm_ = t;
    core.raft_log_.reset(s + 1);
    core
}

#[test]
fn a_state_after_a_snapshot_is_restored() {
    let mut core = at_boundary(10, 2);
    let (ok, out) = restore(&mut core, 5, 2, 12, vec![entry(2, 11), entry(3, 12), entry(4, 13)]);
    assert!(ok);
    assert_eq!((core.raft_log_.base(), core.raft_log_.last_index()), (11, 13));
    // The apply of 11..=12 is queued, from the boundary.
    assert_eq!((out.at(0).from(), out.at(0).to()), (10, 12));
}

#[test]
fn states_no_step_produces_are_refused_unchanged() {
    let ok = || vec![entry(1, 1), entry(2, 2), entry(2, 3)];
    let after = || vec![entry(2, 11), entry(3, 12)];
    let none = RAFT_SERVER_INVALID_SITE_ID;
    // (why, the core's term, its snapshot boundary (index, term, log base),
    //  the state: term, vote, commit, entries)
    type Case = (&'static str, u64, (u64, i64, u64), u64, u16, u64, Vec<RaftEntry<Cmd>>);
    let cases: Vec<Case> = vec![
        ("commit past the log", 0, (0, 0, 1), 3, 2, 4, ok()),
        ("vote for a non-member", 0, (0, 0, 1), 3, 9, 1, ok()),
        ("a vote at term 0", 0, (0, 0, 1), 0, 2, 0, vec![]),
        ("an entry without a value", 0, (0, 0, 1), 3, 2, 1,
         vec![entry(1, 1), RaftEntry::new(2, Cmd(2), false, false, 0, 0)]),
        ("an entry of term 0", 0, (0, 0, 1), 3, 2, 1, vec![entry(0, 1)]),
        ("falling terms", 0, (0, 0, 1), 3, 2, 1, vec![entry(2, 1), entry(1, 2)]),
        ("a last term above the term", 0, (0, 0, 1), 1, 2, 1, ok()),
        ("a term at the ceiling", 0, (0, 0, 1), RAFT_INDEX_LIMIT, 2, 1, ok()),
        ("a term below the current one", 4, (0, 0, 1), 3, 2, 0, vec![]),
        ("a commit below the boundary", 0, (10, 2, 11), 5, none, 9, after()),
        ("a first term below the boundary's", 0, (10, 3, 11), 5, none, 10, after()),
        ("a log not starting after the boundary", 0, (10, 2, 12), 5, none, 10, after()),
    ];
    for (why, current, (s, t, base), term, vote, commit, entries) in cases {
        let mut core = at_boundary(s, t);
        core.raft_log_.reset(base);
        core.current_term_ = current;
        let before = saved(&core);
        let (accepted, mut out) = restore(&mut core, term, vote, commit, entries);
        assert!(!accepted, "{why}: accepted");
        assert_eq!(saved(&core), before, "{why}: changed the core");
        assert!(out.is_empty() && out.take_persist().is_none(), "{why}: left output");
    }
}
