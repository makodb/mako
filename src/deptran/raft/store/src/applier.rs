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
    handle: Option<JoinHandle<Result<u64, String>>>,
}

struct State {
    base: Box<dyn Base>,
    fs: Arc<dyn StoreFs>,
    wal_dir: PathBuf,
    id: Identity,
    durable: Arc<DurableState>,
    cfg: ApplierConfig,
    c: u64,
    bytes: u64,
    since: Instant,
}

impl State {
    fn fold(&mut self, first: u64, records: &[Vec<u8>]) -> Result<(), String> {
        let mut ops = Vec::new();
        for (k, bytes) in records.iter().enumerate() {
            let seq = first + k as u64;
            if seq <= self.c {
                continue;
            }
            assert_eq!(seq, self.c + 1, "the applier folds records in order");
            let rec = decode(bytes, &BytesCodec).map_err(|e| format!("record {seq}: {e}"))?;
            ops_for(&rec, seq, &mut ops);
            self.c = seq;
            self.bytes += bytes.len() as u64;
        }
        if ops.is_empty() {
            return Ok(());
        }
        crash_point("base.write");
        self.base.write(&ops).map_err(|e| format!("base write: {e}"))
    }

    fn catch_up(&mut self, to: u64) -> Result<(), String> {
        if to <= self.c {
            return Ok(());
        }
        let recs = wal::read_range(&*self.fs, &self.wal_dir, &self.id, self.c + 1, to)?;
        let first = recs[0].0;
        let bytes: Vec<Vec<u8>> = recs.into_iter().map(|r| r.1).collect();
        self.fold(first, &bytes)
    }

    fn maybe_checkpoint(&mut self, force: bool) -> Result<(), String> {
        if !force && self.bytes < self.cfg.checkpoint_bytes
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
        let mut st = State { base, fs, wal_dir, id, durable, cfg, c, bytes: 0, since: Instant::now() };
        let handle = std::thread::Builder::new()
            .name("raft-applier".into())
            .spawn(move || -> Result<u64, String> {
                let run = |st: &mut State| -> Result<(), String> {
                    loop {
                        match rx.recv_timeout(Duration::from_millis(100)) {
                            Ok((first, records)) => {
                                if first > st.c + 1 {
                                    st.catch_up(first - 1)?;
                                }
                                st.fold(first, &records)?;
                            }
                            Err(RecvTimeoutError::Timeout) => {
                                let d = st.durable.seq();
                                st.catch_up(d)?;
                            }
                            Err(RecvTimeoutError::Disconnected) => {
                                let d = st.durable.seq();
                                st.catch_up(d)?;
                                return st.maybe_checkpoint(true);
                            }
                        }
                        st.maybe_checkpoint(false)?;
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
        (Applier { handle: Some(handle) }, ApplierTap { tx })
    }

    /// Waits for the applier, after every tap is dropped; its final c, or why
    /// it stopped.
    pub fn join(mut self) -> Result<u64, String> {
        self.handle.take().map(|h| h.join().unwrap_or_else(|_| Err("applier panicked".into()))).unwrap_or(Ok(0))
    }
}
