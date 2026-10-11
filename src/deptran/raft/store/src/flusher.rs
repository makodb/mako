//! The flusher thread and the durable state it publishes (design §3, "The
//! flusher thread").
//!
//! The flusher takes every queued record, encodes the batch, appends and
//! syncs it, sleeps the injected delay, and then publishes `Durable`: the
//! last durable record, and the log's last index and commit as of it. Waiters
//! block on [`DurableState`]; the shell's `on_durable` hook (design §3: send the
//! held replies) runs after each publish. An I/O error aborts the process
//! (design Decision 14): the crates abort on panic.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::queue::RecordQueue;
use crate::record::{self, Codec};
use crate::wal::Wal;

/// What is on disk: records 1..=seq, a log through `last`, commit `commit`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Durable {
    pub seq: u64,
    pub last: u64,
    pub commit: u64,
}

/// The published durable state; read lock-free, waited on by condvar.
pub struct DurableState {
    seq: AtomicU64,
    last: AtomicU64,
    commit: AtomicU64,
    /// When the last publish happened (stats only).
    pub published_us: AtomicU64,
    m: Mutex<()>,
    cv: Condvar,
}

impl DurableState {
    pub fn new(d: Durable) -> Self {
        DurableState {
            seq: AtomicU64::new(d.seq),
            last: AtomicU64::new(d.last),
            commit: AtomicU64::new(d.commit),
            published_us: AtomicU64::new(0),
            m: Mutex::new(()),
            cv: Condvar::new(),
        }
    }

    pub fn seq(&self) -> u64 {
        self.seq.load(Ordering::Acquire)
    }

    pub fn last(&self) -> u64 {
        self.last.load(Ordering::Acquire)
    }

    pub fn get(&self) -> Durable {
        // `seq` last: it is stored last, so a reader that sees it sees the rest.
        let seq = self.seq();
        Durable { seq, last: self.last(), commit: self.commit.load(Ordering::Acquire) }
    }

    fn publish(&self, d: Durable) {
        let _g = self.m.lock().unwrap_or_else(|e| e.into_inner());
        self.last.store(d.last, Ordering::Release);
        self.commit.store(d.commit, Ordering::Release);
        self.published_us.store(crate::stats::now_us(), Ordering::Release);
        self.seq.store(d.seq, Ordering::Release);
        self.cv.notify_all();
    }

    fn wait_until(&self, done: impl Fn(&Self) -> bool, timeout: Duration) -> bool {
        if done(self) {
            return true;
        }
        let deadline = Instant::now() + timeout;
        let mut g = self.m.lock().unwrap_or_else(|e| e.into_inner());
        while !done(self) {
            let now = Instant::now();
            if now >= deadline {
                return false;
            }
            g = self.cv.wait_timeout(g, deadline - now).unwrap_or_else(|e| e.into_inner()).0;
        }
        true
    }

    /// Waits until record `seq` is durable; false on timeout.
    pub fn wait_seq(&self, seq: u64, timeout: Duration) -> bool {
        self.wait_until(|s| s.seq() >= seq, timeout)
    }

    /// Waits until the log is durable through `index`; false on timeout. Only
    /// a leader may use this: a leader never cuts its log, so a durable last
    /// index at or above `index` means entry `index` is on disk.
    pub fn wait_last(&self, index: u64, timeout: Duration) -> bool {
        self.wait_until(|s| s.last() >= index, timeout)
    }
}

/// A held reply's send.
pub type SendReply = Box<dyn FnOnce() + Send>;

/// Replies held until the WAL is durable through their tail (design §3,
/// "What waits for the disk"). A reply whose tail is already
/// durable goes at once. The flusher releases the rest after each publish,
/// in hold order. `hold` reads the durable number under the list's mutex and
/// the flusher takes that mutex only after publishing, so a reply never
/// waits for a later flush than the one that covers it.
pub struct HeldReplies {
    durable: Arc<DurableState>,
    held: Mutex<Vec<(u64, SendReply)>>,
}

impl HeldReplies {
    pub fn new(durable: Arc<DurableState>) -> Self {
        HeldReplies { durable, held: Mutex::new(Vec::new()) }
    }

    /// Sends `send` once records 1..=tail are durable: now, or from the
    /// flusher's release.
    pub fn hold(&self, tail: u64, send: SendReply) {
        let mut g = self.held.lock().unwrap_or_else(|e| e.into_inner());
        if self.durable.seq() >= tail {
            drop(g);
            send();
        } else {
            g.push((tail, send));
        }
    }

    /// Sends every held reply records 1..=seq cover, in hold order. The
    /// flusher calls it after publishing `seq`.
    pub fn release(&self, seq: u64) {
        let ready: Vec<(u64, SendReply)> =
            self.held.lock().unwrap_or_else(|e| e.into_inner()).extract_if(.., |(t, _)| *t <= seq).collect();
        for (_, send) in ready {
            send();
        }
    }

    /// How many replies wait (tests).
    pub fn len(&self) -> usize {
        self.held.lock().unwrap_or_else(|e| e.into_inner()).len()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// The flusher's settings.
pub struct FlusherConfig {
    /// The injected sleep after each sync (`MAKO_RAFT_FLUSH_DELAY_US`): a
    /// device's sync latency, on a store that has none.
    pub delay: Duration,
    /// The applier's queue (design §3): each synced batch is offered to it.
    pub tap: Option<crate::applier::ApplierTap>,
}

/// A running flusher; [`Flusher::join`] after closing its queue.
pub struct Flusher {
    handle: JoinHandle<()>,
}

impl Flusher {
    /// Starts the flusher thread. `start` is the durable state the WAL holds
    /// now (recovery's, or zeros for a new store); `wal.next_seq()` must be
    /// `start.seq + 1` and the queue must number from there too.
    pub fn spawn<P: Send + 'static>(
        mut wal: Wal,
        queue: Arc<RecordQueue<P>>,
        codec: Arc<dyn Codec<P>>,
        durable: Arc<DurableState>,
        cfg: FlusherConfig,
        mut on_durable: Box<dyn FnMut(Durable) + Send>,
    ) -> Flusher {
        let start = durable.get();
        assert_eq!(wal.next_seq(), start.seq + 1, "the WAL and the durable state disagree");
        let handle = std::thread::Builder::new()
            .name("raft-flusher".into())
            .spawn(move || {
                let mut last = start.last;
                let mut commit = start.commit;
                while let Some((first, batch)) = queue.take_all() {
                    let t0 = crate::stats::now_us();
                    let mut encoded = Vec::with_capacity(batch.len());
                    for rec in &batch {
                        let mut b = Vec::new();
                        record::encode(rec, &*codec, &mut b);
                        encoded.push(b);
                        last = rec.last_after(last);
                        if let Some(h) = &rec.hard {
                            commit = h.commit;
                        }
                    }
                    if let Err(e) = wal.append(first, &encoded) {
                        panic!("raft-store: WAL write failed at record {first}: {e}");
                    }
                    drop(batch);
                    if !cfg.delay.is_zero() {
                        std::thread::sleep(cfg.delay);
                    }
                    let d = Durable { seq: first + encoded.len() as u64 - 1, last, commit };
                    if crate::stats::on() {
                        crate::stats::add(crate::stats::FLUSH, crate::stats::now_us() - t0);
                        crate::stats::add(crate::stats::BATCH, encoded.len() as u64);
                    }
                    durable.publish(d);
                    on_durable(d);
                    if let Some(tap) = &cfg.tap {
                        tap.offer(first, encoded);
                    }
                }
            })
            .expect("spawn the raft flusher");
        Flusher { handle }
    }

    /// Waits for the thread to finish (its queue must be closed).
    pub fn join(self) {
        self.handle.join().expect("raft flusher panicked");
    }
}
