use mako_rocksdb_store::{BlobOp, Durability, RocksBlobs};
use std::{path::Path, sync::Arc};
use sto_rocksdb::{
    distributed::{Prefix, Row},
    Database, Position, Table, METADATA_COLUMN_FAMILY,
};
fn schema() -> Vec<Table> {
    vec![
        Table {
            id: 1,
            name: "default".into(),
        },
        Table {
            id: 2,
            name: "orders".into(),
        },
    ]
}
fn fixture(path: &Path) {
    let db = RocksBlobs::open_strict(path, Durability::Sync).unwrap();
    db.ensure_family("orders").unwrap();
    db.write_families(&[
        (
            "default",
            BlobOp::Put {
                key: b"",
                val: b"empty",
            },
        ),
        (
            "default",
            BlobOp::Put {
                key: b"\0\xff",
                val: b"binary\0\xff",
            },
        ),
        (
            "orders",
            BlobOp::Put {
                key: b"\0\xff",
                val: b"other",
            },
        ),
    ])
    .unwrap();
}
fn open(path: &Path) -> Arc<Database> {
    Database::open_distributed_with_tables(path, 7, true, &schema()).unwrap()
}
fn read(db: &Arc<Database>, table: u64, key: &[u8]) -> Option<Vec<u8>> {
    let mut worker = db.attach_worker().unwrap();
    let mut tx = worker.begin().unwrap();
    tx.get(table, key).unwrap()
}
#[test]
fn native_families_round_trip_and_checkpoint_publication() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("native");
    fixture(&path);
    let db = open(&path);
    assert_ne!(db.initial_state_id().unwrap(), [0; 32]);
    let initial = db.initial_state_id().unwrap();
    assert_eq!(read(&db, 1, b""), Some(b"empty".to_vec()));
    assert_eq!(read(&db, 2, b"\0\xff"), Some(b"other".to_vec()));
    assert_eq!(db.count(1).unwrap(), 2);
    assert_eq!(db.distributed_page(1, None, 10).unwrap().0.len(), 2);
    let prefix = Prefix {
        position: Position { index: 1, term: 1 },
        fingerprint: [5; 32],
    };
    db.apply_distributed(
        Some(&prefix),
        &[
            Row {
                table: 1,
                key: b"\0\xff".to_vec(),
                value: None,
                time_term: 10,
            },
            Row {
                table: 2,
                key: b"\0\xff".to_vec(),
                value: Some(b"updated".to_vec()),
                time_term: 10,
            },
        ],
        &vec![(b"protocol".to_vec(), b"state".to_vec())],
    )
    .unwrap();
    assert_eq!(db.count(1).unwrap(), 1);
    assert!(db
        .distributed_page(1, None, 10)
        .unwrap()
        .0
        .iter()
        .any(|r| r.key == b"\0\xff" && r.value.is_none()));
    let checkpoint = root.path().join("checkpoint");
    db.create_checkpoint(&checkpoint, prefix.position).unwrap();
    drop(db);
    let raw = RocksBlobs::open_existing_strict(&path, Durability::Sync).unwrap();
    assert_eq!(raw.get_cf("default", b"").unwrap(), Some(b"empty".to_vec()));
    assert_eq!(raw.get_cf("default", b"\0\xff").unwrap(), None);
    assert_eq!(
        raw.get_cf("orders", b"\0\xff").unwrap(),
        Some(b"updated".to_vec())
    );
    assert!(raw.has_family(METADATA_COLUMN_FAMILY));
    drop(raw);
    let target = root.path().join("target");
    fixture(&target);
    let db = open(&target);
    db.publish_checkpoint(&checkpoint, prefix.position).unwrap();
    assert_eq!(db.initial_state_id().unwrap(), initial);
    drop(db);
    let raw = RocksBlobs::open_existing_strict(&target, Durability::Sync).unwrap();
    assert_eq!(
        raw.get_cf("orders", b"\0\xff").unwrap(),
        Some(b"updated".to_vec())
    );
    drop(raw);
    let db = open(&target);
    assert_eq!(db.applied().unwrap(), prefix.position);
    assert_eq!(
        db.distributed_metadata().unwrap(),
        vec![(b"protocol".to_vec(), b"state".to_vec())]
    );
}
#[test]
fn unsupported_mako_directory_is_rejected_without_conversion_or_overwrite() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("old");
    std::fs::create_dir(&path).unwrap();
    std::fs::write(path.join("CURRENT"), b"generation-0").unwrap();
    let generation = path.join("generation-0");
    let old = RocksBlobs::open_strict(&generation, Durability::Sync).unwrap();
    old.write_families(&[(
        "default",
        BlobOp::Put {
            key: &[0, 0],
            val: b"STORDB01",
        },
    )])
    .unwrap();
    drop(old);
    assert!(Database::open_distributed_with_tables(&path, 7, true, &schema()).is_err());
    assert_eq!(
        std::fs::read(path.join("CURRENT")).unwrap(),
        b"generation-0"
    );
    assert!(!path.symlink_metadata().unwrap().file_type().is_symlink());
    assert!(!sto_rocksdb::control_path(&path).join("ADOPTING").exists());
    let old = RocksBlobs::open_existing_strict(&generation, Durability::Sync).unwrap();
    assert_eq!(
        old.get_cf("default", &[0, 0]).unwrap().as_deref(),
        Some(b"STORDB01".as_slice())
    );
}

#[test]
fn initial_state_binds_family_placement_and_values_independent_of_schema_order() {
    let root = tempfile::tempdir().unwrap();
    let mut identities = Vec::new();
    for case in ["original", "reordered", "moved", "changed"] {
        let path = root.path().join(case);
        fixture(&path);
        if case == "moved" || case == "changed" {
            let raw = RocksBlobs::open_existing_strict(&path, Durability::Sync).unwrap();
            raw.write_families(&[
                ("default", BlobOp::Delete { key: b"" }),
                (
                    if case == "moved" { "orders" } else { "default" },
                    BlobOp::Put {
                        key: b"",
                        val: if case == "moved" {
                            b"empty"
                        } else {
                            b"changed"
                        },
                    },
                ),
            ])
            .unwrap();
        }
        let mut tables = schema();
        if case == "reordered" {
            tables.reverse();
        }
        let db = Database::open_distributed_with_tables(&path, 7, true, &tables).unwrap();
        identities.push(db.initial_state_id().unwrap());
    }
    assert_eq!(identities[0], identities[1]);
    assert_ne!(identities[0], identities[2]);
    assert_ne!(identities[0], identities[3]);
    assert_ne!(identities[2], identities[3]);
}
#[test]
fn unsupported_rows_reserved_families_and_wrong_schema_are_rejected() {
    let root = tempfile::tempdir().unwrap();
    for case in [
        "large-key",
        "large-value",
        "reserved",
        "schema",
        "live-writer",
    ] {
        let path = root.path().join(case);
        fixture(&path);
        let db = RocksBlobs::open_existing_strict(&path, Durability::Sync).unwrap();
        if case == "reserved" {
            db.ensure_family(METADATA_COLUMN_FAMILY).unwrap();
        }
        if case.starts_with("large") {
            db.write_families(&[(
                "orders",
                BlobOp::Put {
                    key: &vec![b'k'; if case == "large-key" { 1025 } else { 1 }],
                    val: &vec![b'v'; if case == "large-value" { 65536 } else { 1 }],
                },
            )])
            .unwrap();
        }
        if case == "live-writer" {
            assert!(Database::open_distributed_with_tables(&path, 7, true, &schema()).is_err());
        }
        drop(db);
        if case == "live-writer" {
            drop(open(&path));
            continue;
        }
        let tables = if case == "schema" {
            vec![schema()[0].clone()]
        } else {
            schema()
        };
        assert!(
            Database::open_distributed_with_tables(&path, 7, true, &tables).is_err(),
            "{case}"
        );
        assert!(!path.symlink_metadata().unwrap().file_type().is_symlink());
        let raw = RocksBlobs::open_existing_strict(&path, Durability::Sync).unwrap();
        assert_eq!(
            raw.get_cf("orders", b"\0\xff").unwrap(),
            Some(b"other".to_vec())
        );
        drop(raw);
        assert!(!sto_rocksdb::control_path(&path).join("ADOPTING").exists());
        if case == "schema" {
            assert_eq!(read(&open(&path), 2, b"\0\xff"), Some(b"other".to_vec()));
        }
    }
}
#[test]
#[cfg(feature = "test-hooks")]
fn adoption_crash_child() {
    if let Some(path) = std::env::var_os("STO_ROCKS_ADOPTION_PATH") {
        let _db = open(Path::new(&path));
        panic!("crash point missed");
    }
}
#[test]
#[cfg(feature = "test-hooks")]
fn interrupted_adoption_resumes_and_keeps_native_data() {
    for point in ["moved", "metadata", "ready", "selector"] {
        let root = tempfile::tempdir().unwrap();
        let path = root.path().join("db");
        fixture(&path);
        let status = std::process::Command::new(std::env::current_exe().unwrap())
            .args(["--exact", "adoption_crash_child", "--nocapture"])
            .env("STO_ROCKS_ADOPTION_PATH", &path)
            .env("STO_ROCKS_ADOPTION_CRASH", point)
            .status()
            .unwrap();
        assert_eq!(status.code(), Some(86), "{point}");
        let db = open(&path);
        assert_eq!(read(&db, 1, b""), Some(b"empty".to_vec()));
        assert_eq!(read(&db, 2, b"\0\xff"), Some(b"other".to_vec()));
        assert_eq!(db.count(1).unwrap(), 2);
        assert_eq!(db.distributed_page(1, None, 10).unwrap().0.len(), 2);
    }
}
