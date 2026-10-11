//! Consumer-visible coverage of the executable certified admission path.
use super::*;

const TABLE: u64 = 31;
const LOCAL: Grant = Grant { owner: 0, ownership_version: 0 };
const REMOTE: Grant = Grant { owner: 1, ownership_version: 0 };
fn tables() -> Vec<RouteTable> {
    vec![RouteTable { table: TABLE, boundaries: vec![
        Boundary { start: vec![], grant: LOCAL },
        Boundary { start: b"m".to_vec(), grant: REMOTE },
    ] }]
}
fn snapshot(version: u64) -> Snapshot { Snapshot { version, tables: tables() } }
fn participant() -> Participant { Participant::new(0,tables()).unwrap() }
fn transaction(client: u64) -> TxnId { TxnId { client, sequence: 1 } }
fn untrusted(grant: Grant) -> RoutedGrant { RoutedGrant { grant, snapshot_version: NO_SNAPSHOT } }
fn plan() -> MigrationPlan {
    MigrationPlan { generation: 1, nonce: transaction(900), source: 0, destination: 1,
        range: KeyRange { table: TABLE, lo: b"b".to_vec(), hi: Some(b"d".to_vec()) },
        old: tables().remove(0).boundaries }
}

#[test]
fn certified_point_and_interval_accept_without_per_request_metadata_validation() {
    let mut p = participant();
    let s = snapshot(0);
    assert_eq!(p.fast_snapshot_version(),None);
    assert!(p.certify_snapshot(&s));
    assert_eq!(p.fast_snapshot_version(),Some(0));
    let id = transaction(1);
    assert_eq!(p.begin(id),Status::Ok);
    let routed = s.capture(TABLE,b"a").unwrap();
    // Equal non-sentinel version, matching owner, and no previous hold select
    // the executable branch that registers directly with LeaseRegistry.
    assert_eq!(p.acquire_routed(id,TABLE,b"a",routed),Status::Ok);
    assert!(p.held(id,TABLE,b"a") == Some(LOCAL));
    assert_eq!(p.acquire_range_routed(id,TABLE,b"b",Some(b"d"),s.capture_range(TABLE,b"b",Some(b"d")).unwrap()),Status::Ok);
    assert!(p.held_range(id,TABLE,b"b",Some(b"d")) == Some(LOCAL));
    assert_eq!(p.fast_snapshot_version(),Some(0));
}

#[test]
fn freeze_invalidates_same_version_before_new_admission_but_keeps_held_lease() {
    let mut p = participant();
    let s = snapshot(0);
    let routed = s.capture(TABLE,b"c").unwrap();
    assert!(p.certify_snapshot(&s));
    let held = transaction(2);
    assert_eq!(p.begin(held),Status::Ok);
    assert_eq!(p.acquire_routed(held,TABLE,b"c",routed),Status::Ok);
    let plan = plan();
    assert_eq!(p.deliver(&plan,Command::Freeze).status,Status::Ok);
    assert_eq!(p.fast_snapshot_version(),None);
    assert!(!p.certify_snapshot(&s));
    let fresh = transaction(3);
    assert_eq!(p.begin(fresh),Status::Ok);
    assert_eq!(p.acquire_routed(fresh,TABLE,b"c",routed),Status::Retry);
    assert_eq!(p.acquire_range_routed(fresh,TABLE,b"b",Some(b"d"),routed),Status::Retry);
    assert_eq!(p.acquire_routed(held,TABLE,b"c",routed),Status::Ok);
    assert_eq!(p.deliver(&plan,Command::Retire).status,Status::Retry);
    assert_eq!(p.finish(held),Status::Ok);
    assert_eq!(p.deliver(&plan,Command::Retire).status,Status::Ok);
    assert_eq!(p.deliver(&plan,Command::Commit).status,Status::Ok);
    assert_eq!(p.acquire_routed(fresh,TABLE,b"c",routed),Status::Retry);
    assert_eq!(p.acquire_routed(fresh,TABLE,b"a",s.capture(TABLE,b"a").unwrap()),Status::Ok);
}

#[test]
fn mismatched_version_and_untrusted_grants_use_exact_fallback() {
    let mut p = participant();
    let old = snapshot(0);
    assert!(p.certify_snapshot(&snapshot(7)));
    let id = transaction(4);
    assert_eq!(p.begin(id),Status::Ok);
    assert_eq!(p.acquire_routed(id,TABLE,b"a",old.capture(TABLE,b"a").unwrap()),Status::Ok);
    assert_eq!(p.acquire_routed(id,TABLE,b"e",untrusted(LOCAL)),Status::Ok);
    assert_eq!(p.acquire_routed(id,TABLE,b"f",untrusted(Grant { owner: 0, ownership_version: 7 })),Status::Retry);
    assert_eq!(p.acquire_routed(id,TABLE,b"z",untrusted(LOCAL)),Status::Retry);
    assert_eq!(p.acquire_routed(id,999,b"a",untrusted(LOCAL)),Status::NotFound);
    assert!(!p.certify_snapshot(&snapshot(NO_SNAPSHOT)));
    assert_eq!(p.fast_snapshot_version(),None);
}

#[test]
fn certified_admission_cannot_reopen_terminal_transaction_identity() {
    let mut p = participant();
    let s = snapshot(0);
    assert!(p.certify_snapshot(&s));
    let id = transaction(5);
    let routed = s.capture(TABLE,b"a").unwrap();
    assert_eq!(p.begin(id),Status::Ok);
    assert_eq!(p.acquire_routed(id,TABLE,b"a",routed),Status::Ok);
    assert_eq!(p.finish(id),Status::Ok);
    assert_eq!(p.acquire_routed(id,TABLE,b"a",routed),Status::Invalid);
    assert_eq!(p.acquire_range_routed(id,TABLE,b"a",Some(b"b"),routed),Status::Invalid);
    assert_eq!(p.begin(id),Status::Invalid);
    let next = TxnId { client: id.client, sequence: 2 };
    assert_eq!(p.begin(next),Status::Ok);
    assert_eq!(p.acquire_routed(next,TABLE,b"a",routed),Status::Ok);
}

#[test]
fn certification_checks_full_intervals_not_only_routing_boundaries() {
    let mut p = participant();
    let s = snapshot(0);
    assert!(p.certify_snapshot(&s));
    // Freeze lies strictly inside one snapshot range; certifying only the
    // snapshot's boundary key would incorrectly overlook it.
    let plan = plan();
    assert_eq!(p.deliver(&plan,Command::Freeze).status,Status::Ok);
    assert!(!p.certify_snapshot(&s));
    let id = transaction(6);
    assert_eq!(p.begin(id),Status::Ok);
    assert_eq!(p.acquire_range_routed(id,TABLE,b"a",Some(b"e"),untrusted(LOCAL)),Status::Retry);
    assert_eq!(p.acquire_range_routed(id,TABLE,b"e",Some(b"m"),untrusted(LOCAL)),Status::Ok);
    assert_eq!(p.acquire_range_routed(id,TABLE,b"e",None,untrusted(LOCAL)),Status::Retry);
    assert_eq!(p.acquire_range_routed(id,TABLE,b"e",Some(b"e"),untrusted(LOCAL)),Status::Invalid);
    assert_eq!(p.deliver(&plan,Command::Abort).status,Status::Ok);
    assert!(p.certify_snapshot(&s));
    let fresh = transaction(7);
    assert_eq!(p.begin(fresh),Status::Ok);
    assert_eq!(p.acquire_range_routed(fresh,TABLE,b"a",Some(b"m"),s.capture_range(TABLE,b"a",Some(b"m")).unwrap()),Status::Ok);
}

#[test]
fn certification_rejects_wrong_incarnation_and_missing_local_table() {
    let mut p = participant();
    let mut wrong = snapshot(3);
    wrong.tables[0].boundaries[0].grant.ownership_version = 3;
    assert!(!p.certify_snapshot(&wrong));
    assert_eq!(p.fast_snapshot_version(),None);
    let mut missing = snapshot(0);
    missing.tables[0].table = 999;
    assert!(!p.certify_snapshot(&missing));
    assert_eq!(p.fast_snapshot_version(),None);
    assert!(p.certify_snapshot(&snapshot(0)));
}
