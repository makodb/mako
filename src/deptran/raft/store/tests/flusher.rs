//! The flusher under concurrent pushers (plan P3/P4 shape): group commit,
//! the published durable state, and a recovery equal to the pushes.

use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use raft_store::flusher::FlusherConfig;
use raft_store::{open_store, BytesCodec, Durable, DurableState, Flusher, Hard, Identity, MemFs, Record, RecordQueue, StoreFs, WalOptions};

const ID: Identity = Identity { site: 0, partition: 0, fingerprint: 1, format: 1 };

#[test]
fn concurrent_pushes_recover_in_queue_order() {
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/d"));
    let fsd: Arc<dyn StoreFs> = Arc::new(fs.clone());
    let opts = WalOptions { segment_bytes: 4096 };
    let o = open_store::<Vec<u8>>(fsd.clone(), Path::new("/d/0-0"), ID, opts, true, &BytesCodec, 0).unwrap();
    let queue = Arc::new(RecordQueue::new(1));
    let durable = Arc::new(DurableState::new(Durable { seq: 0, last: 0, commit: 0 }));
    let published = Arc::new(Mutex::new(Vec::new()));
    let p2 = published.clone();
    let flusher = Flusher::spawn(
        o.wal,
        queue.clone(),
        Arc::new(BytesCodec),
        durable.clone(),
        Durable { seq: 0, last: 0, commit: 0 },
        FlusherConfig { delay: Duration::from_micros(200), tap: None },
        Box::new(move |d| p2.lock().unwrap().push(d)),
    );
    // The "mtx_": pushes and the log they describe change together.
    let log = Arc::new(Mutex::new(0u64));
    let threads: Vec<_> = (0..4)
        .map(|t| {
            let (q, log, durable) = (queue.clone(), log.clone(), durable.clone());
            std::thread::spawn(move || {
                for i in 0..250u64 {
                    let seq = {
                        let mut last = log.lock().unwrap();
                        *last += 1;
                        q.push(Record {
                            replace_from: Some(*last),
                            entries: vec![(1, vec![t as u8; (i % 50) as usize])],
                            hard: Some(Hard { term: 1, vote: 0, commit: *last / 2 }),
                            ..Default::default()
                        })
                    };
                    if i % 10 == 0 {
                        assert!(durable.wait_seq(seq, Duration::from_secs(10)), "record {seq} never durable");
                    }
                }
            })
        })
        .collect();
    for t in threads {
        t.join().unwrap();
    }
    queue.close();
    flusher.join();
    let d = durable.get();
    assert_eq!(d, Durable { seq: 1000, last: 1000, commit: 500 });
    let pubs = published.lock().unwrap();
    assert!(pubs.len() < 1000, "no group commit: {} flushes for 1000 records", pubs.len());
    assert!(pubs.windows(2).all(|w| w[0].seq < w[1].seq));
    drop(pubs);
    let r = open_store::<Vec<u8>>(fsd, Path::new("/d/0-0"), ID, opts, false, &BytesCodec, 0).unwrap();
    assert_eq!((r.d, r.state.last(), r.state.hard.commit), (1000, 1000, 500));
}

#[test]
fn real_disk_roundtrip() {
    // RealFs on the local disk: create, append, reopen, and the lock.
    let root = std::env::temp_dir().join(format!("raft-store-test-{}", std::process::id()));
    std::fs::create_dir_all(&root).unwrap();
    let fs: Arc<dyn StoreFs> = Arc::new(raft_store::RealFs::new());
    let store = root.join("0-0");
    let mut o = open_store::<Vec<u8>>(fs.clone(), &store, ID, WalOptions { segment_bytes: 256 }, true, &BytesCodec, 0).unwrap();
    for k in 1..=20u64 {
        let mut b = Vec::new();
        let r: Record<Vec<u8>> = Record { replace_from: Some(k), entries: vec![(1, vec![k as u8; 40])], ..Default::default() };
        raft_store::record::encode(&r, &BytesCodec, &mut b);
        o.wal.append(k, &[b]).unwrap();
    }
    // A second opener is refused while the first holds the lock.
    let second = open_store::<Vec<u8>>(fs.clone(), &store, ID, WalOptions::default(), false, &BytesCodec, 0);
    assert!(second.err().unwrap().contains("locked"));
    drop(o);
    let r = open_store::<Vec<u8>>(fs, &store, ID, WalOptions::default(), false, &BytesCodec, 0).unwrap();
    assert_eq!((r.d, r.state.last()), (20, 20));
    drop(r);
    std::fs::remove_dir_all(&root).unwrap();
}
