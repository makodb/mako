use super::*;
use crate::directory::RouteTable;

const TABLE:u64=31;
const ALIAS:u64=312;
const LOCAL:Grant=Grant {owner:0,ownership_version:0};
const REMOTE:Grant=Grant {owner:1,ownership_version:0};
fn tables() -> Vec<RouteTable> {
    vec![RouteTable {table:TABLE,boundaries:vec![
        Boundary {start:vec![],grant:LOCAL},
        Boundary {start:b"m".to_vec(),grant:REMOTE},
    ]}]
}
fn routes() -> Arc<Snapshot> {Arc::new(Snapshot {version:0,tables:tables()})}
fn plan() -> MigrationPlan {
    MigrationPlan {generation:1,nonce:TxnId {client:900,sequence:1},source:0,destination:1,
        range:KeyRange {table:TABLE,lo:b"b".to_vec(),hi:Some(b"d".to_vec())},
        old:tables().remove(0).boundaries}
}
fn grant() -> RoutedGrant {RoutedGrant {grant:LOCAL,snapshot_version:0}}

#[test]
fn old_capture_authorizes_new_accesses_after_freeze_but_fresh_capture_rejects() {
    let mut p=Participant::new(0,tables()).unwrap();
    let old=AdmissionSnapshot::capture(&p,routes());
    let captured=old.route(TABLE,b"c").unwrap();
    assert_eq!(p.deliver(&plan(),Command::Freeze).status,Status::Ok);
    let frozen=AdmissionSnapshot::capture(&p,routes());
    // No access or per-key registration occurred before Freeze. The entire
    // retained immutable view, not a previously accessed key, is protected.
    assert_eq!(old.point(TABLE,b"c",captured),Status::Ok);
    assert_eq!(old.range(TABLE,b"b",Some(b"d"),captured),Status::Ok);
    assert_eq!(frozen.point(TABLE,b"c",captured),Status::Retry);
    assert_eq!(frozen.range(TABLE,b"b",Some(b"d"),captured),Status::Retry);
    assert_eq!(frozen.point(TABLE,b"a",captured),Status::Ok);
    assert_eq!(frozen.point(TABLE,b"d",captured),Status::Ok);
    // Cancel publishes a new Serving observation but cannot mutate a retained
    // Frozen view. This is a metadata test, not evidence of an engine drain.
    assert_eq!(p.deliver(&plan(),Command::Abort).status,Status::Ok);
    let cancelled=AdmissionSnapshot::capture(&p,routes());
    assert_eq!(cancelled.point(TABLE,b"c",captured),Status::Ok);
    assert_eq!(frozen.point(TABLE,b"c",captured),Status::Retry);
}

#[test]
fn whole_ranges_detect_interior_frozen_holes_and_remote_suffixes() {
    let mut p=Participant::new(0,tables()).unwrap();
    assert_eq!(p.deliver(&plan(),Command::Freeze).status,Status::Ok);
    let view=AdmissionSnapshot::capture(&p,routes());
    assert_eq!(view.point(TABLE,b"a",grant()),Status::Ok);
    assert_eq!(view.range(TABLE,b"a",Some(b"e"),grant()),Status::Retry);
    assert_eq!(view.range(TABLE,b"a",Some(b"b"),grant()),Status::Ok);
    assert_eq!(view.range(TABLE,b"d",Some(b"m"),grant()),Status::Ok);
    assert_eq!(view.range(TABLE,b"d",None,grant()),Status::Retry);
    assert_eq!(view.range(TABLE,b"d",Some(b"n"),grant()),Status::Retry);
    assert_eq!(view.range(TABLE,b"d",Some(b"d"),grant()),Status::Invalid);
    assert_eq!(view.range(TABLE,b"e",Some(b"d"),grant()),Status::Invalid);
}

#[test]
fn matching_foreign_snapshot_label_never_proves_owner_incarnation_or_coordinate() {
    let p=Participant::new(0,tables()).unwrap();
    let view=AdmissionSnapshot::capture(&p,routes());
    let local=view.route(TABLE,b"a").unwrap();
    assert_eq!(view.point(TABLE,b"z",local),Status::Retry);
    assert_eq!(view.point(999,b"a",local),Status::Retry);
    assert_eq!(view.range(999,b"a",None,local),Status::Retry);
    let wrong_version=RoutedGrant {grant:Grant {owner:0,ownership_version:7},snapshot_version:0};
    assert_eq!(view.point(TABLE,b"a",wrong_version),Status::Retry);
    let wrong_owner=RoutedGrant {grant:REMOTE,snapshot_version:0};
    assert_eq!(view.point(TABLE,b"a",wrong_owner),Status::Retry);
    let foreign_label=RoutedGrant {grant:LOCAL,snapshot_version:743};
    assert_eq!(view.point(TABLE,b"a",foreign_label),Status::Ok);
    assert_eq!(view.point(TABLE,b"z",foreign_label),Status::Retry);
}

#[test]
fn authoritative_copy_includes_physical_aliases_and_absences_not_routing_domain() {
    let mut metadata=tables();
    metadata.push(RouteTable {table:ALIAS,boundaries:vec![Boundary {start:vec![],grant:LOCAL}]});
    let p=Participant::new(0,metadata).unwrap();
    let view=AdmissionSnapshot::capture(&p,routes());
    assert_eq!(view.route(ALIAS,b"row").err(),Some(Status::NotFound));
    assert_eq!(view.point(ALIAS,b"row",grant()),Status::Ok);
    assert_eq!(view.range(ALIAS,b"",None,grant()),Status::Ok);
    // Conversely, a routing-only table does not synthesize local authority.
    let missing=Participant::new(0,vec![]).unwrap();
    let routing_only=AdmissionSnapshot::capture(&missing,routes());
    assert!(routing_only.route(TABLE,b"a").is_ok());
    assert_eq!(routing_only.point(TABLE,b"a",grant()),Status::Retry);
    assert_eq!(routing_only.range(TABLE,b"",None,grant()),Status::Retry);
}

#[test]
fn late_request_uses_receiver_capture_not_sender_snapshot_number() {
    let mut source=Participant::new(0,tables()).unwrap();
    let sender=AdmissionSnapshot::capture(&source,routes());
    let request=sender.route(TABLE,b"c").unwrap();
    assert_eq!(source.deliver(&plan(),Command::Freeze).status,Status::Ok);
    let late_receiver=AdmissionSnapshot::capture(&source,routes());
    assert_eq!(late_receiver.point(TABLE,b"c",request),Status::Retry);
    assert_eq!(late_receiver.range(TABLE,b"b",Some(b"d"),request),Status::Retry);
    // A receiver that already captured before the cut remains usable, provided
    // its STO guard remains announced; no commit-time epoch equality is needed.
    assert_eq!(sender.point(TABLE,b"c",request),Status::Ok);
}

#[test]
fn background_control_guards_track_entire_role_and_round_without_leases() {
    let mut source=Participant::new(0,tables()).unwrap();
    let mut destination=Participant::new(1,tables()).unwrap();
    let migration=plan();
    assert!(source.background_source(&migration));
    assert!(!source.frozen_source(&migration));
    assert!(!destination.background_destination(&migration));
    assert_eq!(destination.deliver(&migration,Command::Start).status,Status::Ok);
    assert!(destination.background_destination(&migration));
    assert_eq!(source.deliver(&migration,Command::Freeze).status,Status::Ok);
    assert!(source.frozen_source(&migration));
    assert!(!source.background_source(&migration));
    assert_eq!(destination.deliver(&migration,Command::Final).status,Status::Ok);
    assert!(!destination.background_destination(&migration));
    assert_eq!(source.deliver(&migration,Command::Abort).status,Status::Ok);
    assert!(!source.frozen_source(&migration));
    assert!(!source.background_source(&migration));
}
