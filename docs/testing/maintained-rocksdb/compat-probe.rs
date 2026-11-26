use std::path::Path;
use raft_store::{base::{Base, Op}, rocks::RocksBase};
use row_store::{BlobOp, Blobs, Durability, RocksBlobs};
fn main() {
    let args: Vec<_> = std::env::args().collect();
    let root = Path::new(&args[2]);
    if args[1] == "write" {
        std::fs::create_dir(root).unwrap();
        let rows = RocksBlobs::open_strict(&root.join("rows"), Durability::Sync).unwrap();
        rows.write_batch(&[
            BlobOp::Put { key: b"\0row\xff", val: b"binary\0value" },
            BlobOp::Put { key: b"empty", val: b"" },
            BlobOp::Put { key: b"deleted", val: b"old" },
        ]).unwrap();
        rows.checkpoint(&root.join("checkpoint")).unwrap();
        rows.write_batch(&[
            BlobOp::Delete { key: b"deleted" },
            BlobOp::Put { key: b"wal-suffix", val: b"after-checkpoint" },
        ]).unwrap();
        let mut log = RocksBase::open_journal(&root.join("log"), true, true).unwrap();
        log.persist(&[Op::Put(b"entry\0".to_vec(), b"old".to_vec()),
            Op::Put(b"entry\x01".to_vec(), b"retained\0".to_vec())]).unwrap();
        log.flush().unwrap();
        log.persist(&[Op::DeleteRange(b"entry\0".to_vec(), b"entry\x01".to_vec()),
            Op::Put(b"entry\x02".to_vec(), b"wal-suffix".to_vec())]).unwrap();
        println!("legacy SST/checkpoint/WAL fixture written; exiting without destructors");
        std::process::exit(0);
    }
    let rows = RocksBlobs::open_existing_strict(&root.join("rows"), Durability::Sync).unwrap();
    assert_eq!(rows.get(b"\0row\xff").unwrap(), Some(b"binary\0value".to_vec()));
    assert_eq!(rows.get(b"empty").unwrap(), Some(vec![]));
    assert_eq!(rows.get(b"deleted").unwrap(), None);
    assert_eq!(rows.get(b"wal-suffix").unwrap(), Some(b"after-checkpoint".to_vec()));
    let checkpoint = RocksBlobs::open_existing_strict(&root.join("checkpoint"), Durability::Sync).unwrap();
    assert_eq!(checkpoint.get(b"deleted").unwrap(), Some(b"old".to_vec()));
    assert_eq!(checkpoint.get(b"wal-suffix").unwrap(), None);
    let log = RocksBase::open_journal(&root.join("log"), false, true).unwrap();
    assert_eq!(log.scan(b"entry").unwrap(), vec![
        (b"entry\x01".to_vec(), b"retained\0".to_vec()),
        (b"entry\x02".to_vec(), b"wal-suffix".to_vec()),
    ]);
    println!("legacy binary/empty rows, deletion, checkpoint and log WAL suffix preserved");
}
