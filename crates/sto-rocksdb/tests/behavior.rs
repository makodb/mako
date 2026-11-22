use std::sync::Arc;
use sto_rocksdb::{
    Command, CommandBody, Database, Error, OutcomeKind, Position, RequestId, Transaction, Worker,
};

fn request(sequence: u64) -> RequestId {
    RequestId {
        client: 41,
        sequence,
    }
}
fn position(index: u64) -> Position {
    Position { index, term: 3 }
}
fn table(db: &Database, index: u64, name: &str) -> (u64, Vec<u8>) {
    let command = Command::create_table(db.group(), request(index), name.to_owned())
        .unwrap()
        .encode()
        .unwrap();
    let result = db
        .apply(position(index), &command)
        .unwrap()
        .outcome
        .unwrap();
    let OutcomeKind::Table(id) = result.kind else {
        panic!("catalog outcome")
    };
    (id, command)
}
fn commit(db: &Database, txn: Transaction<'_>, sequence: u64, index: u64) -> Vec<u8> {
    let mut prepared = txn.prepare(request(sequence)).unwrap();
    prepared.certify().unwrap();
    prepared.mark_submitted().unwrap();
    let bytes = prepared.take_bytes().unwrap();
    prepared.mark_committed().unwrap();
    db.apply(position(index), &bytes).unwrap();
    assert_eq!(prepared.complete_applied().unwrap(), position(index));
    bytes
}
// A runtime permits one attached worker per owning thread. Construct and drop
// the worker inside its lane; neither workers nor borrowed transactions move.
fn on_worker<R: Send>(db: &Arc<Database>, operation: impl FnOnce(&mut Worker) -> R + Send) -> R {
    std::thread::scope(|scope| {
        scope
            .spawn(move || {
                let mut worker = db.attach_worker().unwrap();
                operation(&mut worker)
            })
            .join()
            .unwrap()
    })
}
fn read(db: &Arc<Database>, table: u64, key: &[u8]) -> Option<Vec<u8>> {
    on_worker(db, |worker| {
        let mut txn = worker.begin().unwrap();
        let value = txn.get(table, key).unwrap();
        let mut prepared = txn.prepare(request(9000)).unwrap();
        assert!(prepared.is_read_only());
        prepared.certify().unwrap();
        prepared.complete_read_only().unwrap();
        value
    })
}

#[test]
fn real_rows_intents_rollback_and_multiple_tables() {
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (first, _) = table(&db, 1, "first");
    let (second, _) = table(&db, 2, "second");
    let mut worker = db.attach_worker().unwrap();
    let mut txn = worker.begin().unwrap();
    assert!(!txn.exists(first, b"a").unwrap());
    txn.insert(first, b"a", b"one").unwrap();
    txn.put(first, b"a", b"two").unwrap();
    txn.put(second, b"a", b"other").unwrap();
    assert_eq!(txn.get(first, b"a").unwrap(), Some(b"two".to_vec()));
    assert_eq!(txn.get(second, b"a").unwrap(), Some(b"other".to_vec()));
    commit(&db, txn, 3, 3);
    assert_eq!(db.count(first).unwrap(), 1);
    assert_eq!(db.count(second).unwrap(), 1);
    let mut txn = worker.begin().unwrap();
    txn.delete(first, b"a").unwrap();
    assert!(!txn.exists(first, b"a").unwrap());
    txn.insert(first, b"a", b"replacement").unwrap();
    txn.rollback();
    assert_eq!(read(&db, first, b"a"), Some(b"two".to_vec()));
    let mut txn = worker.begin().unwrap();
    txn.delete(first, b"a").unwrap();
    commit(&db, txn, 4, 4);
    assert_eq!(read(&db, first, b"a"), None);
    assert_eq!(db.count(first).unwrap(), 0);
    assert_eq!(db.lookup_table("first").unwrap().unwrap().id, first);
    let (duplicate, _) = table(&db, 5, "first");
    assert_eq!(duplicate, first);
    assert_eq!(db.list_tables().unwrap().len(), 2);
}

#[test]
fn snapshot_repeatable_reads_and_absence_aba_conflict() {
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (table, _) = table(&db, 1, "rows");
    let mut old_worker = db.attach_worker().unwrap();
    let mut old = old_worker.begin().unwrap();
    assert_eq!(old.get(table, b"aba").unwrap(), None);
    on_worker(&db, |writer| {
        let mut insert = writer.begin().unwrap();
        insert.put(table, b"aba", b"temporary").unwrap();
        commit(&db, insert, 2, 2);
        let mut delete = writer.begin().unwrap();
        delete.delete(table, b"aba").unwrap();
        commit(&db, delete, 3, 3);
    });
    assert_eq!(old.get(table, b"aba").unwrap(), None);
    old.put(table, b"other", b"must-not-appear").unwrap();
    let mut prepared = old.prepare(request(4)).unwrap();
    assert!(matches!(prepared.certify(), Err(Error::Conflict)));
    drop(prepared);
    assert_eq!(read(&db, table, b"other"), None);
    assert_eq!(db.count(table).unwrap(), 0);
}

#[test]
fn scans_merge_sealed_intents_forward_reverse_and_detect_phantoms() {
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (table, _) = table(&db, 1, "rows");
    let mut writer = db.attach_worker().unwrap();
    let mut seed = writer.begin().unwrap();
    for key in [b"a", b"c", b"e"] {
        seed.put(table, key, key).unwrap();
    }
    commit(&db, seed, 2, 2);
    let mut txn = writer.begin().unwrap();
    txn.put(table, b"b", b"B").unwrap();
    txn.delete(table, b"c").unwrap();
    txn.put(table, b"d", b"D").unwrap();
    let ascending = txn.scan(table, b"b", Some(b"e"), 20).unwrap();
    assert_eq!(
        ascending,
        vec![
            (b"b".to_vec(), b"B".to_vec()),
            (b"d".to_vec(), b"D".to_vec())
        ]
    );
    let mut descending = ascending.clone();
    descending.reverse();
    assert_eq!(
        txn.reverse_scan(table, b"b", Some(b"e"), 20).unwrap(),
        descending
    );
    assert_eq!(txn.reverse_scan(table, b"", None, 1).unwrap()[0].0, b"e");
    txn.rollback();
    drop(writer);
    let mut scanner_worker = db.attach_worker().unwrap();
    let mut scanner = scanner_worker.begin().unwrap();
    assert!(scanner.scan(table, b"x", Some(b"z"), 5).unwrap().is_empty());
    on_worker(&db, |writer| {
        let mut mutation = writer.begin().unwrap();
        mutation.put(table, b"y", b"phantom").unwrap();
        commit(&db, mutation, 3, 3);
    });
    let mut prepared = scanner.prepare(request(4)).unwrap();
    assert!(matches!(prepared.certify(), Err(Error::Conflict)));
}

#[test]
fn independent_prepared_writers_pipeline_but_pending_writer_blocks_scan() {
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (table, _) = table(&db, 1, "rows");
    std::thread::scope(|scope| {
        let mut owners = Vec::new();
        for (key, sequence) in [(b"a", 2), (b"b", 3)] {
            let database = Arc::clone(&db);
            let (ready_tx, ready_rx) = std::sync::mpsc::sync_channel(0);
            let (resolve_tx, resolve_rx) = std::sync::mpsc::sync_channel(0);
            let (finished_tx, finished_rx) = std::sync::mpsc::sync_channel(0);
            scope.spawn(move || {
                let mut worker = database.attach_worker().unwrap();
                let mut txn = worker.begin().unwrap();
                txn.put(table, key, key).unwrap();
                let mut retained = txn.prepare(request(sequence)).unwrap();
                retained.certify().unwrap();
                retained.mark_submitted().unwrap();
                ready_tx.send(retained.take_bytes().unwrap()).unwrap();
                resolve_rx.recv().unwrap();
                retained.mark_committed().unwrap();
                retained.complete_applied().unwrap();
                finished_tx.send(()).unwrap();
            });
            owners.push((ready_rx, resolve_tx, finished_rx));
        }
        // Both owner lanes certify and retain their guards before either apply.
        let first_bytes = owners[0].0.recv().unwrap();
        let second_bytes = owners[1].0.recv().unwrap();
        let mut scanner_worker = db.attach_worker().unwrap();
        let mut scanner = scanner_worker.begin().unwrap();
        scanner.scan(table, b"", None, 100).unwrap();
        assert!(matches!(scanner.prepare(request(4)), Err(Error::Conflict)));
        // Apply runs independently of both retained owner-lane guards.
        db.apply(position(2), &first_bytes).unwrap();
        db.apply(position(3), &second_bytes).unwrap();
        // Resolve in reverse order while retaining the first attempt's guards.
        owners[1].1.send(()).unwrap();
        owners[1].2.recv().unwrap();
        owners[0].1.send(()).unwrap();
        owners[0].2.recv().unwrap();
    });
    assert_eq!(db.count(table).unwrap(), 2);
}

#[test]
fn decided_replay_dedup_outcomes_and_applied_noops_survive_reopen() {
    let leader_dir = tempfile::tempdir().unwrap();
    let follower_dir = tempfile::tempdir().unwrap();
    let leader = Database::open(leader_dir.path(), 7, true).unwrap();
    let follower = Database::open(follower_dir.path(), 7, true).unwrap();
    let (table, catalog) = table(&leader, 1, "rows");
    follower.apply(position(1), &catalog).unwrap();
    let mut worker = leader.attach_worker().unwrap();
    let mut txn = worker.begin().unwrap();
    txn.put(table, b"x", b"first").unwrap();
    txn.put(table, b"x", b"final").unwrap();
    let command = commit(&leader, txn, 2, 2);
    let decoded = Command::decode(&command).unwrap();
    let CommandBody::Transaction {
        mutations,
        count_deltas,
    } = decoded.body
    else {
        panic!("transaction")
    };
    assert_eq!(mutations.len(), 1);
    assert_eq!(count_deltas[&table], 1);
    follower.apply(position(2), &command).unwrap();
    assert!(follower.apply(position(2), &command).unwrap().duplicate);
    let duplicate = follower.apply(position(3), &command).unwrap();
    assert_eq!(duplicate.outcome.unwrap().position, position(2));
    follower.advance_applied(position(4)).unwrap();
    assert_eq!(follower.count(table).unwrap(), 1);
    drop(follower);
    let follower = Database::open(follower_dir.path(), 7, true).unwrap();
    assert_eq!(follower.applied().unwrap(), position(4));
    assert_eq!(
        follower.outcome(request(2)).unwrap().unwrap().position,
        position(2)
    );
    assert_eq!(read(&follower, table, b"x"), Some(b"final".to_vec()));
    let conflict = Command::create_table(7, request(2), "different".into())
        .unwrap()
        .encode()
        .unwrap();
    assert!(matches!(
        follower.apply(position(5), &conflict),
        Err(Error::RequestReuse)
    ));
    assert!(follower.is_fenced());
    assert_eq!(follower.applied().unwrap(), position(4));
}

#[test]
fn unknown_is_not_rollback_and_only_definite_discard_releases_reservation() {
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (table, _) = table(&db, 1, "rows");
    let mut first_worker = db.attach_worker().unwrap();
    let mut first = first_worker.begin().unwrap();
    first.put(table, b"x", b"first").unwrap();
    let mut prepared = first.prepare(request(2)).unwrap();
    prepared.certify().unwrap();
    prepared.mark_submitted().unwrap();
    prepared.mark_unknown().unwrap();
    std::thread::scope(|scope| {
        let database = Arc::clone(&db);
        let (conflict_tx, conflict_rx) = std::sync::mpsc::sync_channel(0);
        let (discard_tx, discard_rx) = std::sync::mpsc::sync_channel(0);
        let second = scope.spawn(move || {
            let mut worker = database.attach_worker().unwrap();
            let mut txn = worker.begin().unwrap();
            txn.put(table, b"x", b"second").unwrap();
            assert!(matches!(txn.prepare(request(3)), Err(Error::Conflict)));
            conflict_tx.send(()).unwrap();
            discard_rx.recv().unwrap();
            let mut txn = worker.begin().unwrap();
            txn.put(table, b"x", b"second").unwrap();
            commit(&database, txn, 3, 2);
        });
        conflict_rx.recv().unwrap();
        prepared.reject().unwrap();
        drop(prepared);
        assert!(!db.is_fenced());
        discard_tx.send(()).unwrap();
        second.join().unwrap();
    });
    assert_eq!(read(&db, table, b"x"), Some(b"second".to_vec()));
}

#[test]
fn checkpoint_staging_is_not_publication_and_old_snapshots_are_fenced() {
    let leader_dir = tempfile::tempdir().unwrap();
    let follower_dir = tempfile::tempdir().unwrap();
    let artifact = tempfile::tempdir().unwrap();
    let leader = Database::open(leader_dir.path(), 7, true).unwrap();
    let follower = Database::open(follower_dir.path(), 7, true).unwrap();
    let (table, catalog) = table(&leader, 1, "rows");
    follower.apply(position(1), &catalog).unwrap();
    let mut old_worker = follower.attach_worker().unwrap();
    let mut old = old_worker.begin().unwrap();
    assert_eq!(old.get(table, b"x").unwrap(), None);
    let mut writer = leader.attach_worker().unwrap();
    let mut txn = writer.begin().unwrap();
    txn.put(table, b"x", b"checkpoint").unwrap();
    commit(&leader, txn, 2, 2);
    let path = artifact.path().join("checkpoint");
    leader.create_checkpoint(&path, position(2)).unwrap();
    follower.validate_checkpoint(&path, position(2)).unwrap();
    assert_eq!(follower.applied().unwrap(), position(1));
    // The host would fsync snapshot authority between these calls.
    follower.publish_checkpoint(&path, position(2)).unwrap();
    assert!(
        matches!(
            Database::open(follower_dir.path(), 7, true),
            Err(Error::Storage(_))
        ),
        "the logical DB remains exclusively owned across generation replacement"
    );
    assert_eq!(
        follower.reclaim_generations().unwrap(),
        0,
        "an old native snapshot must retain its generation files"
    );
    assert_eq!(old.get(table, b"x").unwrap(), None);
    assert!(matches!(old.prepare(request(8)), Err(Error::Conflict)));
    drop(old_worker);
    assert_eq!(
        follower.reclaim_generations().unwrap(),
        1,
        "the unselected generation becomes reclaimable after its final lease"
    );
    assert_eq!(read(&follower, table, b"x"), Some(b"checkpoint".to_vec()));
    follower.publish_checkpoint(&path, position(2)).unwrap();
    drop(follower);
    let follower = Database::open(follower_dir.path(), 7, true).unwrap();
    follower.publish_checkpoint(&path, position(2)).unwrap();
    assert_eq!(follower.applied().unwrap(), position(2));
}

#[test]
fn durable_tombstone_generation_exhaustion_is_checked_not_wrapped() {
    use mrx_core::{BlobOp, Blobs};
    use mrx_rocks::{Durability, RocksBlobs};
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (table, _) = table(&db, 1, "rows");
    drop(db);
    let raw =
        RocksBlobs::open_existing_strict(&directory.path().join("generation-0"), Durability::Sync)
            .unwrap();
    let mut key = vec![0, 6];
    key.extend_from_slice(&table.to_be_bytes());
    key.extend_from_slice(b"spent");
    raw.write_batch(&[BlobOp::Put {
        key: &key,
        val: &u64::MAX.to_be_bytes(),
    }])
    .unwrap();
    drop(raw);
    let db = Database::open(directory.path(), 7, true).unwrap();
    let mut worker = db.attach_worker().unwrap();
    let mut txn = worker.begin().unwrap();
    txn.put(table, b"spent", b"never").unwrap();
    assert!(matches!(
        txn.prepare(request(2)),
        Err(Error::VersionExhausted)
    ));
    assert_eq!(db.count(table).unwrap(), 0);
    assert_eq!(db.applied().unwrap(), position(1));
}

#[cfg(feature = "test-hooks")]
#[test]
fn ambiguous_real_atomic_batch_fences_then_recovers_without_double_effects() {
    use sto_rocksdb::ApplyFault;
    for fault in [ApplyFault::BeforeBatch, ApplyFault::AfterBatch] {
        let leader_dir = tempfile::tempdir().unwrap();
        let replica_dir = tempfile::tempdir().unwrap();
        let leader = Database::open(leader_dir.path(), 7, true).unwrap();
        let replica = Database::open(replica_dir.path(), 7, true).unwrap();
        let (table, catalog) = table(&leader, 1, "rows");
        replica.apply(position(1), &catalog).unwrap();
        let mut worker = leader.attach_worker().unwrap();
        let mut txn = worker.begin().unwrap();
        txn.put(table, b"x", b"durable").unwrap();
        let bytes = commit(&leader, txn, 2, 2);
        replica.inject_apply_fault(fault);
        assert!(matches!(
            replica.apply(position(2), &bytes),
            Err(Error::Storage(_))
        ));
        assert!(replica.is_fenced());
        assert!(replica.attach_worker().is_err());
        drop(replica);
        let replica = Database::open(replica_dir.path(), 7, true).unwrap();
        replica.apply(position(2), &bytes).unwrap();
        assert_eq!(replica.count(table).unwrap(), 1);
        assert_eq!(read(&replica, table, b"x"), Some(b"durable".to_vec()));
    }
}

#[test]
fn install_recovers_staging_and_durable_selector_cutpoints() {
    use std::io::Write;
    let leader_dir = tempfile::tempdir().unwrap();
    let replica_dir = tempfile::tempdir().unwrap();
    let artifacts = tempfile::tempdir().unwrap();
    let leader = Database::open(leader_dir.path(), 7, true).unwrap();
    let replica = Database::open(replica_dir.path(), 7, true).unwrap();
    let (table, catalog) = table(&leader, 1, "rows");
    replica.apply(position(1), &catalog).unwrap();
    let mut worker = leader.attach_worker().unwrap();
    let mut txn = worker.begin().unwrap();
    txn.put(table, b"x", b"installed").unwrap();
    commit(&leader, txn, 2, 2);
    let checkpoint = artifacts.path().join("snapshot");
    leader.create_checkpoint(&checkpoint, position(2)).unwrap();
    let partial = replica_dir.path().join("staging-2-3");
    std::fs::create_dir(&partial).unwrap();
    std::fs::write(partial.join("torn-copy"), b"incomplete").unwrap();
    replica
        .validate_checkpoint(&checkpoint, position(2))
        .unwrap();
    drop(replica);
    // A crash after staging cannot change the authoritative selected DB.
    let replica = Database::open(replica_dir.path(), 7, true).unwrap();
    assert_eq!(replica.applied().unwrap(), position(1));
    let mut selector = std::fs::File::create(replica_dir.path().join("CURRENT-next")).unwrap();
    selector.write_all(b"install-2-3").unwrap();
    selector.sync_all().unwrap();
    std::fs::rename(
        replica_dir.path().join("CURRENT-next"),
        replica_dir.path().join("CURRENT"),
    )
    .unwrap();
    std::fs::File::open(replica_dir.path())
        .unwrap()
        .sync_all()
        .unwrap();
    // Simulate the next cutpoint: durable authority/selector published, but the
    // process died before its in-memory RocksDB handle swap.
    assert_eq!(replica.applied().unwrap(), position(1));
    drop(replica);
    let replica = Database::open(replica_dir.path(), 7, true).unwrap();
    replica
        .publish_checkpoint(&checkpoint, position(2))
        .unwrap();
    assert_eq!(read(&replica, table, b"x"), Some(b"installed".to_vec()));
}

#[cfg(feature = "test-hooks")]
#[test]
fn native_checkpoint_larger_than_sixty_four_mebibytes_installs() {
    let leader_dir = tempfile::tempdir().unwrap();
    let replica_dir = tempfile::tempdir().unwrap();
    let artifacts = tempfile::tempdir().unwrap();
    let leader = Database::open(leader_dir.path(), 7, true).unwrap();
    let (table, _) = table(&leader, 1, "large");
    let mut worker = leader.attach_worker().unwrap();
    // Deterministic incompressible data ensures the actual native checkpoint,
    // not merely the logical row lengths, exceeds the historical RPC cap.
    let mut value = vec![0u8; 8 * 1024 * 1024];
    let mut state = 0x6a09_e667_f3bc_c909u64;
    for index in 2u64..=10 {
        for chunk in value.chunks_exact_mut(8) {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            chunk.copy_from_slice(&state.to_le_bytes());
        }
        let mut txn = worker.begin().unwrap();
        txn.put(table, &index.to_be_bytes(), &value).unwrap();
        commit(&leader, txn, index, index);
    }
    let checkpoint = artifacts.path().join("large");
    leader.create_checkpoint(&checkpoint, position(10)).unwrap();
    let bytes: u64 = std::fs::read_dir(&checkpoint)
        .unwrap()
        .map(|entry| {
            let metadata = entry.unwrap().metadata().unwrap();
            if metadata.is_file() {
                metadata.len()
            } else {
                0
            }
        })
        .sum();
    assert!(
        bytes > 64 * 1024 * 1024,
        "native checkpoint must exercise the size boundary"
    );
    let replica = Database::open(replica_dir.path(), 7, true).unwrap();
    replica
        .validate_checkpoint(&checkpoint, position(10))
        .unwrap();
    replica
        .publish_checkpoint(&checkpoint, position(10))
        .unwrap();
    assert_eq!(replica.count(table).unwrap(), 9);
    assert_eq!(read(&replica, table, &10u64.to_be_bytes()), Some(value));
}

#[test]
fn delayed_owner_completion_cannot_regress_newer_replica_publication() {
    let directory = tempfile::tempdir().unwrap();
    let db = Database::open(directory.path(), 7, true).unwrap();
    let (table, _) = table(&db, 1, "rows");
    let mut writer = db.attach_worker().unwrap();
    let mut txn = writer.begin().unwrap();
    txn.put(table, b"x", b"first").unwrap();
    let mut retained = txn.prepare(request(2)).unwrap();
    retained.certify().unwrap();
    retained.mark_submitted().unwrap();
    retained.mark_committed().unwrap();
    let bytes = retained.take_bytes().unwrap();
    db.apply(position(2), &bytes).unwrap();

    // A newly elected leader may have decided the successor while this old
    // owning lane has not yet processed its own successful-apply notification.
    let mut successor = Command::decode(&bytes).unwrap();
    successor.request = request(3);
    let CommandBody::Transaction {
        mutations,
        count_deltas,
    } = &mut successor.body
    else {
        panic!("transaction command");
    };
    mutations[0].generation += 1;
    mutations[0].was_present = true;
    mutations[0].value = Some(b"successor".to_vec());
    count_deltas.clear();
    db.apply(position(3), &successor.encode().unwrap()).unwrap();
    std::thread::scope(|scope| {
        let database = Arc::clone(&db);
        let (observed_tx, observed_rx) = std::sync::mpsc::sync_channel(0);
        let (published_tx, published_rx) = std::sync::mpsc::sync_channel(0);
        let reader = scope.spawn(move || {
            let mut worker = database.attach_worker().unwrap();
            let mut txn = worker.begin().unwrap();
            assert_eq!(txn.get(table, b"x").unwrap(), Some(b"successor".to_vec()));
            observed_tx.send(()).unwrap();
            published_rx.recv().unwrap();
            // Retain the same canonical target across delayed publication;
            // rehydrating from disk would conceal a regressing version store.
            let mut retained = txn.prepare(request(4)).unwrap();
            retained.certify().unwrap();
            retained.complete_read_only().unwrap();
        });
        observed_rx.recv().unwrap();
        retained.complete_applied().unwrap();
        drop(retained);
        published_tx.send(()).unwrap();
        reader.join().unwrap();
    });
    assert_eq!(db.count(table).unwrap(), 1);
}

#[test]
fn durability_identity_survives_reopen_and_checkpoint_install() {
    for sync_writes in [true, false] {
        let source_dir = tempfile::tempdir().unwrap();
        let target_dir = tempfile::tempdir().unwrap();
        let wrong_dir = tempfile::tempdir().unwrap();
        let artifacts = tempfile::tempdir().unwrap();
        let source = Database::open(source_dir.path(), 7, sync_writes).unwrap();
        let (table, _) = table(&source, 1, "rows");
        on_worker(&source, |worker| {
            let mut txn = worker.begin().unwrap();
            txn.put(table, b"x", b"before").unwrap();
            commit(&source, txn, 2, 2);
        });
        let checkpoint = artifacts.path().join("snapshot");
        source.create_checkpoint(&checkpoint, position(2)).unwrap();
        drop(source);
        assert!(Database::open(source_dir.path(), 7, !sync_writes).is_err());
        let source = Database::open(source_dir.path(), 7, sync_writes).unwrap();
        assert_eq!(read(&source, table, b"x"), Some(b"before".to_vec()));
        let wrong = Database::open(wrong_dir.path(), 7, !sync_writes).unwrap();
        assert!(wrong.validate_checkpoint(&checkpoint, position(2)).is_err());
        assert_eq!(wrong.applied().unwrap(), Position::default());
        let target = Database::open(target_dir.path(), 7, sync_writes).unwrap();
        target
            .validate_checkpoint(&checkpoint, position(2))
            .unwrap();
        target.publish_checkpoint(&checkpoint, position(2)).unwrap();
        on_worker(&target, |worker| {
            let mut txn = worker.begin().unwrap();
            txn.put(table, b"x", b"after").unwrap();
            commit(&target, txn, 3, 3);
        });
        drop(target);
        assert!(Database::open(target_dir.path(), 7, !sync_writes).is_err());
        let target = Database::open(target_dir.path(), 7, sync_writes).unwrap();
        assert_eq!(target.applied().unwrap(), position(3));
        assert_eq!(read(&target, table, b"x"), Some(b"after".to_vec()));
    }
}
