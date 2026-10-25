//! Conditional-backend regressions: actual journal bytes survive file reopen.
use crate as native;
#[path = "durable_test_store.rs"]
pub(crate) mod store;
#[path = "durable_test_engine.rs"]
pub(crate) mod engine;
#[path = "durable_test_protocol.rs"]
mod protocol;
use native::durable_kv::{journal_key, Journal, JournalError, Namespace, HEAD_TAG};
use store::{Directory, DiskKv, Fault};

fn namespace(owner: u32, coordinator: bool) -> Namespace { Namespace { owner, coordinator } }

#[test]
fn unknown_append_replays_only_the_settled_atomic_prefix() {
    for (fault, committed) in [
        (Fault::NotCommitted, false),
        (Fault::UnknownAbort, false),
        (Fault::UnknownCommit, true),
        (Fault::UnknownDelayedCommit, true),
    ] {
        let directory = Directory::new();
        let ns = namespace(0, true);
        let mut kv = DiskKv::open(directory.path());
        let mut journal = Journal::create(&mut kv, ns, b"bootstrap".to_vec()).ok().unwrap();
        let old = journal.activate(&mut kv).ok().unwrap();
        kv.fault = fault;
        assert!(journal.append(&mut kv, b"decision".to_vec()).is_err());
        assert!(journal.admission().is_none());
        assert!(!journal.check(&old));
        drop(journal);
        drop(kv);

        let mut kv = DiskKv::open(directory.path());
        let mut recovered = Journal::recover(&mut kv, ns).ok().unwrap();
        assert!(recovered.admission().is_none());
        assert_eq!(recovered.count(), if committed { 2 } else { 1 });
        assert_eq!(recovered.read_record(&mut kv, 0).ok().unwrap(), b"bootstrap");
        if committed {
            assert_eq!(recovered.read_record(&mut kv, 1).ok().unwrap(), b"decision");
        }
        let new = recovered.activate(&mut kv).ok().unwrap();
        assert!(new.incarnation > old.incarnation);
        assert!(!recovered.check(&old));
        assert!(recovered.check(&new));
        recovered.append(&mut kv, b"after-recovery".to_vec()).ok().unwrap();
        assert_eq!(recovered.read_record(&mut kv, if committed { 2 } else { 1 })
            .ok().unwrap(), b"after-recovery");
    }
}

#[test]
fn failed_settlement_and_unknown_admission_never_open_serving() {
    let directory = Directory::new();
    let ns = namespace(3, false);
    let mut kv = DiskKv::open(directory.path());
    let mut journal = Journal::create(&mut kv, ns, b"participant".to_vec()).ok().unwrap();
    kv.fault = Fault::UnknownDelayedCommit;
    assert!(matches!(journal.activate(&mut kv), Err(JournalError::Unknown)));
    assert!(journal.admission().is_none());
    drop(journal);
    drop(kv);

    let mut kv = DiskKv::open(directory.path());
    kv.fail_settle = true;
    assert!(matches!(Journal::recover(&mut kv, ns), Err(JournalError::Io)));
    let mut recovered = Journal::recover(&mut kv, ns).ok().unwrap();
    let admission = recovered.activate(&mut kv).ok().unwrap();
    // The unknown activation committed incarnation 1; reopening cannot reuse it.
    assert_eq!(admission.incarnation, 2);
    assert!(!recovered.check(&native::durable_kv::Admission { namespace: ns, incarnation: 1 }));
}

#[test]
fn authority_namespaces_and_admission_tokens_cannot_alias() {
    let directory = Directory::new();
    let mut kv = DiskKv::open(directory.path());
    let ns_a = namespace(7, true);
    let ns_b = namespace(7, false);
    let ns_c = namespace(8, false);
    let mut a = Journal::create(&mut kv, ns_a, b"coordinator".to_vec()).ok().unwrap();
    let mut b = Journal::create(&mut kv, ns_b, b"local-participant".to_vec()).ok().unwrap();
    let mut c = Journal::create(&mut kv, ns_c, b"other-participant".to_vec()).ok().unwrap();
    let aa = a.activate(&mut kv).ok().unwrap();
    let ab = b.activate(&mut kv).ok().unwrap();
    let ac = c.activate(&mut kv).ok().unwrap();
    assert!(!a.check(&ab) && !b.check(&aa) && !b.check(&ac));
    a.append(&mut kv, b"committed-owner-change".to_vec()).ok().unwrap();
    drop((a, b, c, kv));

    let mut kv = DiskKv::open(directory.path());
    let mut a = Journal::recover(&mut kv, ns_a).ok().unwrap();
    let mut b = Journal::recover(&mut kv, ns_b).ok().unwrap();
    let mut c = Journal::recover(&mut kv, ns_c).ok().unwrap();
    assert_eq!(a.count(), 2);
    assert_eq!(b.count(), 1);
    assert_eq!(c.count(), 1);
    assert_eq!(a.read_record(&mut kv, 1).ok().unwrap(), b"committed-owner-change");
    assert_eq!(b.read_record(&mut kv, 0).ok().unwrap(), b"local-participant");
    assert_eq!(c.read_record(&mut kv, 0).ok().unwrap(), b"other-participant");
}

#[test]
fn missing_or_malformed_committed_record_never_becomes_fresh_state() {
    for corrupt in [None, Some(vec![0xff, 0x00]), Some(vec![0; 16])] {
        let directory = Directory::new();
        let ns = namespace(0, true);
        let mut kv = DiskKv::open(directory.path());
        Journal::create(&mut kv, ns, b"bootstrap".to_vec()).ok().unwrap();
        kv.corrupt(journal_key(ns, true, 0), corrupt);
        drop(kv);
        let mut kv = DiskKv::open(directory.path());
        assert!(matches!(Journal::recover(&mut kv, ns), Err(JournalError::Corrupt)));
        assert!(matches!(Journal::create(&mut kv, ns, b"wrong-genesis".to_vec()),
            Err(JournalError::Exists)));
    }
}

#[test]
fn admission_exhaustion_does_not_wrap_or_overwrite_records() {
    let directory = Directory::new();
    let ns = namespace(0, true);
    let mut kv = DiskKv::open(directory.path());
    Journal::create(&mut kv, ns, b"retained-bootstrap".to_vec()).ok().unwrap();
    let mut head = Vec::new();
    head.extend_from_slice(&HEAD_TAG.to_le_bytes());
    head.extend_from_slice(&u64::MAX.to_le_bytes());
    head.extend_from_slice(&1u64.to_le_bytes());
    kv.corrupt(journal_key(ns, false, 0), Some(head));
    let before = kv.all_bytes().clone();
    let mut journal = Journal::recover(&mut kv, ns).ok().unwrap();
    assert!(matches!(journal.activate(&mut kv), Err(JournalError::Exhausted)));
    assert!(journal.admission().is_none());
    assert_eq!(kv.all_bytes(), &before);
}

fn at_phase(cluster: &mut protocol::Cluster, wanted: native::types::Phase) {
    for _ in 0..24 {
        if cluster.phase() == Some(wanted) { return; }
        assert!(cluster.drive_one().unwrap());
    }
    panic!("did not reach the requested native phase");
}

#[test]
fn every_native_phase_recovers_without_resetting_nonce_or_generation() {
    use native::types::{Phase, TxnId};
    use native::durable_coordinator::Operation;
    use native::migration::Request;
    for phase in [Phase::Copy, Phase::Freezing, Phase::Final, Phase::Retiring, Phase::Committed, Phase::Aborted] {
        let directory = Directory::new();
        let mut cluster = protocol::Cluster::open(directory.path(), true);
        cluster.seed();
        assert_eq!(cluster.begin(protocol::NONCE, 0, 1).unwrap(), 1);
        if phase == Phase::Aborted {
            cluster.execute(Operation::Request(Request::Abort)).unwrap();
        } else { at_phase(&mut cluster, phase); }
        let old_admission = cluster.admission;
        drop(cluster);

        let mut cluster = protocol::Cluster::open(directory.path(), false);
        assert!(cluster.phase() == Some(phase));
        assert!(cluster.coordinator.actor(&old_admission).is_err());
        assert_eq!(cluster.begin(protocol::NONCE, 0, 1).unwrap(), 1);
        assert!(cluster.begin(protocol::NONCE, 1, 0).is_err());
        cluster.finish().unwrap();
        let owner = if phase == Phase::Aborted { 0 } else { 1 };
        cluster.check_destination(owner);
        let nonce = TxnId { client: protocol::NONCE.client, sequence: 2 };
        assert_eq!(cluster.begin(nonce, owner, 1 - owner).unwrap(), 2);
        cluster.finish().unwrap();
        cluster.check_destination(1 - owner);
    }
}

#[test]
fn uncertain_native_decision_is_resolved_before_route_publication() {
    use native::types::Phase;
    use native::durable_coordinator::{ResumeAction, ResumeWork};
    use native::migration::Request;
    for (fault, committed) in [
        (Fault::NotCommitted, false), (Fault::UnknownAbort, false),
        (Fault::UnknownCommit, true), (Fault::UnknownDelayedCommit, true),
    ] {
        let directory = Directory::new();
        let mut cluster = protocol::Cluster::open(directory.path(), true);
        cluster.seed();
        cluster.begin(protocol::NONCE, 0, 1).unwrap();
        at_phase(&mut cluster, Phase::Retiring);
        assert!(cluster.drive_one().unwrap()); // Actual source Retired receipt.
        assert!(matches!(cluster.coordinator.resume(&cluster.admission).ok().unwrap(),
            ResumeWork::Active { action: ResumeAction::Advance(Request::Commit), .. }));
        cluster.owners[0].kv.fault = fault;
        assert!(cluster.drive_one().is_err());
        assert!(cluster.coordinator.actor(&cluster.admission).is_err());
        drop(cluster);

        let mut cluster = protocol::Cluster::open(directory.path(), false);
        assert!(cluster.phase() == Some(if committed { Phase::Committed } else { Phase::Retiring }));
        assert_eq!(cluster.route(b"m\0").owner, if committed { 1 } else { 0 });
        cluster.finish().unwrap();
        cluster.check_destination(1);
    }
}

#[test]
fn uncertain_copy_never_recovers_ready_from_a_partial_destination() {
    use native::types::{Phase, Role};
    use native::durable_coordinator::{ResumeAction, ResumeWork};
    for fault in [Fault::UnknownAbort, Fault::UnknownCommit, Fault::UnknownDelayedCommit] {
        let directory = Directory::new();
        let mut cluster = protocol::Cluster::open(directory.path(), true);
        cluster.seed();
        cluster.begin(protocol::NONCE, 0, 1).unwrap();
        at_phase(&mut cluster, Phase::Final);
        assert!(matches!(cluster.coordinator.resume(&cluster.admission).ok().unwrap(),
            ResumeWork::Active { action: ResumeAction::FinalCopy { .. }, .. }));
        cluster.owners[1].engine.row_fault = fault;
        assert!(cluster.drive_one().is_err());
        let owner = &cluster.owners[1];
        assert!(owner.participant.meta(&owner.admission, protocol::TABLE, b"m\0").is_err());
        drop(cluster);

        let mut cluster = protocol::Cluster::open(directory.path(), false);
        let owner = &cluster.owners[1];
        let metadata = owner.participant.meta(&owner.admission, protocol::TABLE, b"m\0")
            .ok().unwrap().unwrap();
        assert!(metadata.role == Role::Stage);
        cluster.finish().unwrap();
        cluster.check_destination(1);
    }
}

#[test]
fn late_ready_after_abort_recovers_the_abort_and_cleans_the_private_copy() {
    use native::durable_coordinator::{Operation, Response};
    use native::migration::Request;
    use native::types::{Certificate, Command, Phase, Role, Status};
    let directory = Directory::new();
    let mut cluster = protocol::Cluster::open(directory.path(), true);
    cluster.seed();
    assert_eq!(cluster.begin(protocol::NONCE, 0, 1).unwrap(), 1);
    at_phase(&mut cluster, Phase::Final);
    let destination = &mut cluster.owners[1];
    let result = destination.participant.deliver(&mut destination.kv, &destination.admission, 1, Command::Final)
        .ok().unwrap();
    assert!(result.status == Status::Ok);
    cluster.execute(Operation::Request(Request::Abort)).unwrap();
    {
        let (left, right) = cluster.owners.split_at_mut(1);
        let source = &left[0];
        let destination = &mut right[0];
        let capture = source.participant.source(&source.engine, source.admission, 1).ok().unwrap();
        let result = destination.participant.execute_final(&mut destination.kv, &mut destination.engine,
            &destination.admission, 1, &capture).ok().unwrap();
        assert!(result.certificate == Some(Certificate::Ready));
        assert_eq!(destination.engine.value(protocol::TABLE, b"m\0", b"m\0"), Some(&b"first\0value"[..]));
    }
    let _late_reply = cluster.execute(Operation::Receive { generation: 1, owner: 1, certificate: Certificate::Ready });
    assert!(cluster.phase() == Some(Phase::Aborted));
    assert_eq!(cluster.route(b"m\0").owner, 0);
    assert_eq!(cluster.route(b"m\0").ownership_version, 0);
    drop(cluster);

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    assert!(cluster.phase() == Some(Phase::Aborted));
    let destination = &cluster.owners[1];
    assert!(destination.participant.meta(&destination.admission, protocol::TABLE, b"m\0")
        .ok().unwrap().unwrap().role == Role::Ready);
    cluster.finish().unwrap();
    cluster.check_destination(0);
    assert_eq!(cluster.owners[1].engine.value(protocol::TABLE, b"m\0", b"m\0"), None);
    match cluster.execute(Operation::Reply(protocol::NONCE)).unwrap() {
        Response::Replied(outcome) => { assert_eq!(outcome.generation, 1); assert!(!outcome.committed); }
        _ => panic!("aborted migration did not retain its terminal reply"),
    }
}

#[test]
fn unknown_cleanup_resumes_and_old_completion_cannot_delete_returned_ownership() {
    use native::types::{Certificate, Phase, TxnId};
    let directory = Directory::new();
    let mut cluster = protocol::Cluster::open(directory.path(), true);
    cluster.seed();
    let other_table = protocol::TABLE + 1;
    cluster.owners[0].engine.seed(other_table, protocol::row(b"m\0", b"other-source"));
    cluster.owners[1].engine.seed(other_table, protocol::row(b"m\0", b"other-destination"));
    let untouched_versions = [
        cluster.owners[0].engine.physical_version(other_table, b"m\0", b"m\0"),
        cluster.owners[1].engine.physical_version(other_table, b"m\0", b"m\0"),
    ];
    cluster.begin(protocol::NONCE, 0, 1).unwrap();
    at_phase(&mut cluster, Phase::Committed);
    cluster.owners[0].engine.row_fault = Fault::UnknownDelayedCommit;
    assert!(cluster.drive_one().is_err());
    drop(cluster);

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    assert!(cluster.phase() == Some(Phase::Committed));
    cluster.finish().unwrap();
    cluster.check_destination(1);
    assert_eq!(cluster.begin(TxnId { client: protocol::NONCE.client, sequence: 2 }, 1, 0).unwrap(), 2);
    cluster.finish().unwrap();
    cluster.check_destination(0);
    let owner = &mut cluster.owners[0];
    let before = owner.engine.physical_version(protocol::TABLE, b"m\0", b"m\0");
    let result = owner.participant.execute_cleanup(&mut owner.kv, &mut owner.engine, &owner.admission, 1)
        .ok().unwrap();
    assert!(result.certificate == Some(Certificate::SourceDone));
    assert_eq!(owner.engine.physical_version(protocol::TABLE, b"m\0", b"m\0"), before);
    cluster.check_destination(0);
    for (index, owner) in cluster.owners.iter().enumerate() {
        let expected: &[u8] = if index == 0 { b"other-source" } else { b"other-destination" };
        assert_eq!(owner.engine.value(other_table, b"m\0", b"m\0"), Some(expected));
        assert_eq!(owner.engine.physical_version(other_table, b"m\0", b"m\0"), untouched_versions[index]);
    }
}

#[test]
fn recovery_retains_old_leases_until_exact_durable_transaction_settlement() {
    use native::types::{Grant, KeyRange, Phase, Status, TxnId};
    let directory = Directory::new();
    let mut cluster = protocol::Cluster::open(directory.path(), true);
    cluster.seed();
    let id = TxnId { client: 500, sequence: 1 };
    let owner = &mut cluster.owners[0];
    assert!(owner.participant.begin(&mut owner.kv, &owner.admission, id).ok().unwrap() == Status::Ok);
    assert!(owner.participant.acquire(&mut owner.kv, &owner.admission, id,
        KeyRange { table: protocol::TABLE, lo: b"m\0".to_vec(), hi: None }, true,
        Grant { owner: 0, ownership_version: 0 }).ok().unwrap() == Status::Ok);
    assert!(owner.engine.transaction(id, None,
        vec![(protocol::TABLE, protocol::row(b"m\0", b"settled-after-crash"), false)]));
    cluster.begin(protocol::NONCE, 0, 1).unwrap();
    at_phase(&mut cluster, Phase::Freezing);
    assert!(cluster.drive_one().is_err()); // Frozen, but the old lease is held.
    drop(cluster);

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    let owner = &mut cluster.owners[0];
    let (id, _) = owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
        .ok().unwrap().unwrap();
    assert!(!matches!(owner.participant.begin(&mut owner.kv, &owner.admission, id), Ok(Status::Ok)));
    assert!(owner.participant.drain(&owner.admission, 1).ok().unwrap().status != Status::Ok);
    assert!(owner.participant.finish(&mut owner.kv, &mut owner.engine, &owner.admission, id).is_err());
    drop(cluster);

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    let owner = &mut cluster.owners[0];
    let (id, _) = owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
        .ok().unwrap().unwrap();
    assert!(owner.participant.drain(&owner.admission, 1).ok().unwrap().status != Status::Ok);
    owner.engine.decide(id, true);
    assert!(owner.participant.finish(&mut owner.kv, &mut owner.engine, &owner.admission, id)
        .ok().unwrap() == Status::Ok);
    assert!(owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
        .ok().unwrap().is_none());
    cluster.finish().unwrap();
    assert_eq!(cluster.route(b"m\0").owner, 1);
    assert_eq!(cluster.read(b"m\0").unwrap(), Some(b"settled-after-crash".to_vec()));
}

#[test]
fn lost_lease_reply_without_engine_work_requires_a_durable_identity_fence() {
    use native::types::{Grant, KeyRange, Phase, Status, TxnId};
    let directory = Directory::new();
    let mut cluster = protocol::Cluster::open(directory.path(), true);
    cluster.seed();
    let id = TxnId { client: 502, sequence: 1 };
    let owner = &mut cluster.owners[0];
    assert!(owner.participant.begin(&mut owner.kv, &owner.admission, id).ok().unwrap() == Status::Ok);
    owner.kv.fault = Fault::UnknownDelayedCommit;
    assert!(owner.participant.acquire(&mut owner.kv, &owner.admission, id,
        KeyRange { table: protocol::TABLE, lo: b"m\0".to_vec(), hi: None }, true,
        Grant { owner: 0, ownership_version: 0 }).is_err());
    drop(cluster);

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    cluster.begin(protocol::NONCE, 0, 1).unwrap();
    at_phase(&mut cluster, Phase::Freezing);
    assert!(cluster.drive_one().is_err());
    let owner = &mut cluster.owners[0];
    let (id, _) = owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
        .ok().unwrap().unwrap();
    assert!(owner.participant.finish(&mut owner.kv, &mut owner.engine, &owner.admission, id)
        .ok().unwrap() == Status::Ok);
    // A late request cannot recreate the absent transaction after settlement.
    assert!(!owner.engine.transaction(id, Some(true),
        vec![(protocol::TABLE, protocol::row(b"m\0", b"late-old-write"), false)]));
    drop(cluster);

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    let owner = &mut cluster.owners[0];
    assert!(owner.participant.next_recovery_transaction(&mut owner.kv, &owner.admission, 0)
        .ok().unwrap().is_none());
    assert!(!cluster.owners[0].engine.transaction(id, Some(true),
        vec![(protocol::TABLE, protocol::row(b"m\0", b"late-after-restart"), false)]));
    cluster.finish().unwrap();
    cluster.check_destination(1);
}

#[test]
fn unknown_bootstrap_is_either_absent_or_a_complete_initial_prefix() {
    for (fault, committed) in [
        (Fault::NotCommitted, false), (Fault::UnknownAbort, false),
        (Fault::UnknownCommit, true), (Fault::UnknownDelayedCommit, true),
    ] {
        let directory = Directory::new();
        let ns = namespace(0, true);
        let mut kv = DiskKv::open(directory.path());
        kv.fault = fault;
        assert!(Journal::create(&mut kv, ns, b"original-bootstrap".to_vec()).is_err());
        drop(kv);
        let mut kv = DiskKv::open(directory.path());
        let mut journal = if committed {
            assert!(matches!(Journal::create(&mut kv, ns, b"replacement".to_vec()), Err(JournalError::Exists)));
            Journal::recover(&mut kv, ns).ok().unwrap()
        } else {
            assert!(matches!(Journal::recover(&mut kv, ns), Err(JournalError::Missing)));
            Journal::create(&mut kv, ns, b"original-bootstrap".to_vec()).ok().unwrap()
        };
        assert_eq!(journal.read_record(&mut kv, 0).ok().unwrap(), b"original-bootstrap");
        assert_eq!(journal.activate(&mut kv).ok().unwrap().incarnation, 1);
    }
}

#[test]
fn ready_recovers_only_if_its_post_copy_completion_record_committed() {
    use native::types::{Command, Phase, Role, Status};
    for (fault, committed) in [
        (Fault::NotCommitted, false), (Fault::UnknownAbort, false),
        (Fault::UnknownCommit, true), (Fault::UnknownDelayedCommit, true),
    ] {
        let directory = Directory::new();
        let mut cluster = protocol::Cluster::open(directory.path(), true);
        cluster.seed();
        cluster.begin(protocol::NONCE, 0, 1).unwrap();
        at_phase(&mut cluster, Phase::Final);
        let owner = &mut cluster.owners[1];
        assert!(owner.participant.deliver(&mut owner.kv, &owner.admission, 1, Command::Final)
            .ok().unwrap().status == Status::Ok);
        // Acknowledge reservation, then lose the completion-record result.
        owner.kv.skip_fault = 1;
        owner.kv.fault = fault;
        let (first, second) = cluster.owners.split_at_mut(1);
        let source = first[0].participant.source(&first[0].engine, first[0].admission, 1)
            .ok().unwrap();
        let owner = &mut second[0];
        assert!(owner.participant.execute_final(&mut owner.kv, &mut owner.engine,
            &owner.admission, 1, &source).is_err());
        assert_eq!(cluster.owners[1].engine.value(protocol::TABLE, b"m\0", b"m\0"), Some(&b"first\0value"[..]));
        assert_eq!(cluster.owners[1].engine.value(protocol::TABLE, b"m-stale", b"m-stale"), None);
        assert!(cluster.owners[1].participant.meta(&cluster.owners[1].admission, protocol::TABLE, b"m\0").is_err());
        drop(cluster);

        let mut cluster = protocol::Cluster::open(directory.path(), false);
        let owner = &cluster.owners[1];
        let meta = owner.participant.meta(&owner.admission, protocol::TABLE, b"m\0").ok().unwrap().unwrap();
        assert!(meta.role == if committed { Role::Ready } else { Role::Stage });
        assert_eq!(cluster.route(b"m\0").owner, 0);
        cluster.finish().unwrap();
        cluster.check_destination(1);
    }
}

#[test]
fn failed_engine_replay_cannot_open_a_metadata_only_participant() {
    use native::durable_participant::DurableParticipant;
    use native::types::{Phase, Role};
    let directory = Directory::new();
    let mut cluster = protocol::Cluster::open(directory.path(), true);
    cluster.seed();
    cluster.begin(protocol::NONCE, 0, 1).unwrap();
    at_phase(&mut cluster, Phase::Retiring);
    let old_admission = cluster.owners[1].admission;
    drop(cluster);

    let base = directory.path().join("owner-1");
    let mut kv = DiskKv::open(&base.join("system"));
    let mut engine = engine::DiskEngine::open(&base.join("data"), 1);
    engine.fail_recovery = true;
    assert!(DurableParticipant::recover(&mut kv, &mut engine, 1).is_err());
    let mut participant = DurableParticipant::recover(&mut kv, &mut engine, 1).ok().unwrap();
    assert!(participant.meta(&old_admission, protocol::TABLE, b"m\0").is_err());
    let admission = participant.activate(&mut kv, &mut engine).ok().unwrap();
    assert!(participant.meta(&admission, protocol::TABLE, b"m\0").ok().unwrap().unwrap().role == Role::Ready);
    assert_eq!(engine.value(protocol::TABLE, b"m\0", b"m\0"), Some(&b"first\0value"[..]));
    drop((participant, engine, kv));

    let mut cluster = protocol::Cluster::open(directory.path(), false);
    cluster.finish().unwrap();
    cluster.check_destination(1);
}
