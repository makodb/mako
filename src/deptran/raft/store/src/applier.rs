//! The applier thread (design §3-§4 "The base and the applier"; plan P7).
//!
//! The flusher offers each synced batch (its first number and encoded
//! records) on a bounded queue and drops it when the queue is full; the
//! applier folds records in order into the base, one atomic batch each with
//! c inside, so the base is always the replay of 1..=c, of durable records
//! only. A batch that starts past c + 1 (an offer was dropped) is preceded
//! by a catch-up read from the segments. A checkpoint, every
//! `checkpoint_bytes` of records or `checkpoint_secs`, flushes the base and
//! then deletes the segments it covers. A base error stops the applier and
//! so the deletion: the WAL still holds every record (design Decision 14).

use std::path::PathBuf;
use std::sync::mpsc::{sync_channel, Receiver, RecvTimeoutError, SyncSender, TrySendError};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use crate::base::{ops_for, Base};
use crate::crash::crash_point;
use crate::flusher::DurableState;
use crate::fs::StoreFs;
use crate::record::{decode, BytesCodec};
use crate::segment::Identity;
use crate::wal;

pub struct ApplierConfig {
    pub checkpoint_bytes: u64,
    pub checkpoint_secs: u64,
    /// The offer queue's length (tests use 1 to force catch-ups).
    pub queue: usize,
}

impl Default for ApplierConfig {
    fn default() -> Self {
        ApplierConfig { checkpoint_bytes: 256 << 20, checkpoint_secs: 10, queue: 64 }
    }
}

/// One synced batch: its first record number and the encoded records.
type Offer = (u64, Vec<Vec<u8>>);

/// The flusher's side: offers a synced batch, never blocking.
#[derive(Clone)]
pub struct ApplierTap {
    tx: SyncSender<Offer>,
}

impl ApplierTap {
    pub fn offer(&self, first: u64, records: Vec<Vec<u8>>) {
        match self.tx.try_send((first, records)) {
            Ok(()) | Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => {}
        }
    }
}

pub struct Applier {
    handle: JoinHandle<Result<u64, String>>,
    stop: Arc<std::sync::atomic::AtomicBool>,
}

/// Records folded per base write in a catch-up: the applier rechecks its
/// stop flag between chunks (and may checkpoint), so a large backlog (a
/// saturated run) never holds a shutdown.
const CATCH_UP_CHUNK: u64 = 1024;

struct State {
    base: Box<dyn Base>,
    fs: Arc<dyn StoreFs>,
    wal_dir: PathBuf,
    id: Identity,
    durable: Arc<DurableState>,
    cfg: ApplierConfig,
    stop: Arc<std::sync::atomic::AtomicBool>,
    c: u64,
    /// The highest entry index the base may hold (base::ops_for).
    top: u64,
    bytes: u64,
    since: Instant,
}

impl State {
    /// Folds `records` (numbered from `first`) into the base, one write; c
    /// and top move only once it succeeded.
    fn fold(&mut self, first: u64, records: &[Vec<u8>]) -> Result<(), String> {
        let mut ops = Vec::new();
        let (mut c, mut top, mut bytes) = (self.c, self.top, 0);
        for (k, rec) in records.iter().enumerate() {
            let seq = first + k as u64;
            if seq <= c {
                continue;
            }
            assert_eq!(seq, c + 1, "the applier folds records in order");
            let r = decode(rec, &BytesCodec).map_err(|e| format!("record {seq}: {e}"))?;
            ops_for(&r, seq, &mut top, &mut ops);
            c = seq;
            bytes += rec.len() as u64;
        }
        if ops.is_empty() {
            return Ok(());
        }
        crash_point("base.write");
        self.base.write(&ops).map_err(|e| format!("base write: {e}"))?;
        (self.c, self.top, self.bytes) = (c, top, self.bytes + bytes);
        Ok(())
    }

    fn stopping(&self) -> bool {
        self.stop.load(std::sync::atomic::Ordering::Acquire)
    }

    /// Folds records c+1..=to, read back from the segments, each segment
    /// once (one read a chunk made a long catch-up reread and checksum the
    /// same 64 MB segments over and over: about 15k records/s, slower than a
    /// saturated leader writes).
    fn catch_up(&mut self, to: u64) -> Result<(), String> {
        if to <= self.c || self.stopping() {
            return Ok(());
        }
        let (fs, dir, id) = (self.fs.clone(), self.wal_dir.clone(), self.id);
        let mut chunk: Vec<Vec<u8>> = Vec::new();
        let mut first = self.c + 1;
        let stopped = "stopped";
        let r = wal::for_each_record(&*fs, &dir, &id, self.c + 1, to, |seq, rec| {
            chunk.push(rec.to_vec());
            if chunk.len() as u64 == CATCH_UP_CHUNK {
                self.fold(first, &chunk)?;
                self.maybe_checkpoint()?;
                chunk.clear();
                first = seq + 1;
                if self.stopping() {
                    return Err(stopped.into());
                }
            }
            Ok(())
        });
        match r {
            Err(e) if e == stopped => return Ok(()),
            r => r?,
        }
        if !chunk.is_empty() {
            self.fold(first, &chunk)?;
            self.maybe_checkpoint()?;
        }
        Ok(())
    }

    fn maybe_checkpoint(&mut self) -> Result<(), String> {
        if self.bytes < self.cfg.checkpoint_bytes
            && self.since.elapsed() < Duration::from_secs(self.cfg.checkpoint_secs) {
            return Ok(());
        }
        crash_point("base.flush");
        self.base.flush().map_err(|e| format!("base flush: {e}"))?;
        let doomed = wal::covered_segments(&*self.fs, &self.wal_dir, self.c).map_err(|e| e.to_string())?;
        for p in &doomed {
            crash_point("base.delete");
            self.fs.remove_file(p).map_err(|e| format!("{}: {e}", p.display()))?;
        }
        if !doomed.is_empty() {
            self.fs.sync_dir(&self.wal_dir).map_err(|e| e.to_string())?;
        }
        // Images older than the one the base now durably names (plan P8).
        if let Some(images) = self.wal_dir.parent().map(|p| p.join("images")) {
            if let Some(b) = self.base.get(crate::base::KEY_SNAP).map_err(|e| e.to_string())? {
                if b.len() >= 8 && self.fs.exists(&images) {
                    let snap = u64::from_le_bytes(b[..8].try_into().unwrap());
                    crate::images::cleanup(&*self.fs, &images, snap, false).map_err(|e| e.to_string())?;
                }
            }
        }
        self.bytes = 0;
        self.since = Instant::now();
        Ok(())
    }
}

impl Applier {
    /// Starts the applier over `base`, which holds records 1..=c. Returns it
    /// and the tap the flusher offers batches through.
    pub fn spawn(base: Box<dyn Base>, c: u64, fs: Arc<dyn StoreFs>, wal_dir: PathBuf, id: Identity,
                 durable: Arc<DurableState>, cfg: ApplierConfig) -> (Applier, ApplierTap) {
        let (tx, rx): (SyncSender<Offer>, Receiver<Offer>) = sync_channel(cfg.queue.max(1));
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let mut st = State { base, fs, wal_dir, id, durable, cfg, stop: stop.clone(), c, top: u64::MAX, bytes: 0,
                             since: Instant::now() };
        let handle = std::thread::Builder::new()
            .name("raft-applier".into())
            .spawn(move || -> Result<u64, String> {
                let run = |st: &mut State| -> Result<(), String> {
                    loop {
                        if st.stopping() {
                            return Ok(());
                        }
                        match rx.recv_timeout(Duration::from_millis(100)) {
                            Ok((first, records)) => {
                                if first > st.c + 1 {
                                    st.catch_up(first - 1)?;
                                }
                                if st.stopping() {
                                    return Ok(());
                                }
                                st.fold(first, &records)?;
                            }
                            Err(RecvTimeoutError::Timeout) => {
                                let d = st.durable.seq();
                                st.catch_up(d)?;
                            }
                            // Shutdown: stop where the base is. The WAL holds
                            // every record after c, so a lagging base costs
                            // only replay time at the next start (design §3);
                            // folding a saturated run's backlog here would hold
                            // the shutdown for as long as the backlog takes.
                            Err(RecvTimeoutError::Disconnected) => return Ok(()),
                        }
                        st.maybe_checkpoint()?;
                    }
                };
                match run(&mut st) {
                    Ok(()) => Ok(st.c),
                    Err(why) => {
                        eprintln!("raft-store: applier stopped at record {}: {why} (the WAL keeps every record)", st.c);
                        Err(why)
                    }
                }
            })
            .expect("spawn the raft applier");
        (Applier { handle, stop }, ApplierTap { tx })
    }

    /// Waits for the applier, after every tap is dropped; its final c, or why
    /// it stopped.
    pub fn join(self) -> Result<u64, String> {
        // Stop where the base is: the WAL holds every record after c.
        self.stop.store(true, std::sync::atomic::Ordering::Release);
        self.handle.join().unwrap_or_else(|_| Err("applier panicked".into()))
    }
}
