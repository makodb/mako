//! Held replies (design §3): a reply goes only once the WAL is durable through
//! its tail, never waits for a later flush than the one that covers it (a
//! hold racing a publish included), and keeps hold order.

use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use raft_store::flusher::FlusherConfig;
use raft_store::{open_store, BytesCodec, Durable, DurableState, Flusher, HeldReplies, Identity, MemFs,
                 Record, RecordQueue, StoreFs, WalOptions};

const ID: Identity = Identity { site: 0, partition: 0, fingerprint: 1, format: 1 };

#[test]
fn a_durable_tail_sends_at_once_and_order_is_kept() {
    let durable = Arc::new(DurableState::new(Durable { seq: 5, last: 5, commit: 0 }));
    let held = HeldReplies::new(durable.clone());
    let sent = Arc::new(Mutex::new(Vec::new()));
    let s = sent.clone();
    held.hold(5, Box::new(move || s.lock().unwrap().push(5)));
    assert_eq!(*sent.lock().unwrap(), vec![5]);
    for (tag, tail) in [(7u64, 7u64), (6, 6), (9, 9), (8, 7)] {
        let s = sent.clone();
        held.hold(tail, Box::new(move || s.lock().unwrap().push(tag)));
    }
    held.release(7);
    assert_eq!(*sent.lock().unwrap(), vec![5, 7, 6, 8]);
    assert_eq!(held.len(), 1);
    held.release(9);
    assert_eq!(*sent.lock().unwrap(), vec![5, 7, 6, 8, 9]);
}

#[test]
fn holds_racing_the_flusher_are_never_stranded() {
    // Many threads push a record and hold a reply on its number while the
    // flusher publishes; every reply must go, and only after its record is
    // durable.
    let fs = MemFs::new();
    fs.mkdir_p(Path::new("/d"));
    let fsd: Arc<dyn StoreFs> = Arc::new(fs.clone());
    let o = open_store::<Vec<u8>>(fsd, Path::new("/d/0-0"), ID, WalOptions::default(), true, &BytesCodec, 0)
        .unwrap();
    let queue = Arc::new(RecordQueue::new(1));
    let durable = Arc::new(DurableState::new(Durable { seq: 0, last: 0, commit: 0 }));
    let held = Arc::new(HeldReplies::new(durable.clone()));
    let h2 = held.clone();
    let flusher = Flusher::spawn(o.wal, queue.clone(), Arc::new(BytesCodec), durable.clone(),
                                 FlusherConfig { delay: Duration::from_micros(50), tap: None },
                                 Box::new(move |d: Durable| h2.release(d.seq)));
    let sent = Arc::new(AtomicU64::new(0));
    let early = Arc::new(AtomicU64::new(0));
    let threads: Vec<_> = (0..8).map(|_| {
        let (q, held, durable, sent, early) = (queue.clone(), held.clone(), durable.clone(), sent.clone(),
                                               early.clone());
        std::thread::spawn(move || {
            for _ in 0..500 {
                let tail = q.push(Record::default());
                let d = durable.clone();
                let (sent, early) = (sent.clone(), early.clone());
                held.hold(tail, Box::new(move || {
                    if d.seq() < tail {
                        early.fetch_add(1, Ordering::SeqCst);
                    }
                    sent.fetch_add(1, Ordering::SeqCst);
                }));
            }
        })
    }).collect();
    for t in threads {
        t.join().unwrap();
    }
    let deadline = Instant::now() + Duration::from_secs(10);
    while sent.load(Ordering::SeqCst) < 4000 && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(sent.load(Ordering::SeqCst), 4000, "{} replies stranded", held.len());
    assert_eq!(early.load(Ordering::SeqCst), 0, "a reply left before its record was durable");
    queue.close();
    flusher.join();
}
