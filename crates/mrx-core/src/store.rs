//! The cache itself.
//!
//! Ported from `src/mako/storage/masstree_rocks_index.hh`, which is
//! mutation-verified 5/5 and serves as the oracle for this code.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, RwLock};

use crate::durability::{Floors, Ticket, TicketLog, VersionCounter, Watermark, WriterSlot,
                        NO_FLOOR};
use crate::value::{Entry, Val, ValState};
use crate::{Blobs, BlobOp, Config, EntryWord, KeyIndex, Version};

/// What a write did.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WriteOutcome {
    /// The write was applied.
    pub wrote: bool,
    /// The key was live before the write.
    pub existed: bool,
}

/// Which write is being performed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WriteMode {
    /// Always write.
    Put,
    /// Write only if absent.
    Insert,
    /// Write a tombstone, only if live.
    Remove,
}

/// Per-entry overhead that eviction can never reclaim.
///
/// Eviction frees a value's *payload*, never the entry or the record
/// header, so capacity must be compared against `resident - floor`.
/// Comparing against the total makes any capacity below the floor
/// permanently "over" and degrades the sweeper into perpetual churn — a
/// defect the C++ original shipped and had to fix.
const ENTRY_OVERHEAD: u64 = 64;

/// A Masstree-over-blob-store write-back cache.
pub struct Store<K: KeyIndex, B: Blobs> {
    cfg: Config,
    index: K,
    blobs: B,

    /// Entries, addressed by the word stored in the index (`idx + 1`, so
    /// that word 0 stays reserved for "absent").
    ///
    /// Entries are immortal: this only ever grows. `Arc<Entry>` means the
    /// entries themselves never move even when the table reallocates.
    entries: RwLock<Vec<Arc<Entry>>>,

    counter: VersionCounter,
    watermark: Watermark,
    log: TicketLog,
    writers: Vec<WriterSlot>,
    next_writer: AtomicU64,

    /// Flusher-only: entry index → **oldest** undischarged version.
    ///
    /// Both the coalescer (a hot key occupies one slot however many
    /// tickets it generates) and the honesty mechanism (an entry stays
    /// here, pinning the watermark, until a real write discharges it).
    dirty: Mutex<HashMap<u32, Version>>,

    resident_bytes: AtomicU64,
    floor_bytes: AtomicU64,

    io_failing: AtomicBool,
    stopping: AtomicBool,

    sync_lock: Mutex<()>,
    sync_cv: Condvar,
}

impl<K: KeyIndex, B: Blobs> Store<K, B> {
    /// Open a store over an index and a durable byte store.
    ///
    /// Establishes the key-resident invariant first: every key already in
    /// the durable store gets an entry with its value evicted. Until that
    /// completes, an index miss cannot be trusted as absence.
    pub fn open(cfg: Config, index: K, blobs: B) -> Result<Self, crate::BlobError> {
        let n_writers = 64;
        let s = Self {
            log: TicketLog::new(cfg.log_slots),
            writers: (0..n_writers).map(|_| WriterSlot::new()).collect(),
            next_writer: AtomicU64::new(0),
            cfg,
            index,
            blobs,
            entries: RwLock::new(Vec::new()),
            counter: VersionCounter::new(),
            watermark: Watermark::new(),
            dirty: Mutex::new(HashMap::new()),
            resident_bytes: AtomicU64::new(0),
            floor_bytes: AtomicU64::new(0),
            io_failing: AtomicBool::new(false),
            stopping: AtomicBool::new(false),
            sync_lock: Mutex::new(()),
            sync_cv: Condvar::new(),
        };
        s.load_keys()?;
        Ok(s)
    }

    fn load_keys(&self) -> Result<(), crate::BlobError> {
        let mut keys: Vec<Vec<u8>> = Vec::new();
        self.blobs.for_each_key(&mut |k| keys.push(k.to_vec()))?;
        for k in keys {
            // Version 0: at or below any watermark, i.e. durable by
            // provenance — correct, since the bytes came FROM the store.
            let idx = self.intern(&k, Val::evicted(0));
            let _ = idx;
        }
        Ok(())
    }

    /// Find or create the entry for a key, returning its index.
    fn intern(&self, key: &[u8], seed: Arc<Val>) -> u32 {
        if let Some(w) = self.index.get(key) {
            return (w - 1) as u32;
        }
        let e = Arc::new(Entry::new(key, seed));
        let idx = {
            let mut t = self.entries.write().expect("entries lock poisoned");
            t.push(e);
            (t.len() - 1) as u32
        };
        let word: EntryWord = idx as u64 + 1;
        let won = self.index.get_or_insert(key, word);
        if won != word {
            // Another thread interned it first. Ours is orphaned, which
            // is harmless: entries are immortal anyway and nothing else
            // can reach it.
            return (won - 1) as u32;
        }
        self.floor_bytes.fetch_add(
            ENTRY_OVERHEAD + key.len() as u64,
            Ordering::Relaxed,
        );
        self.resident_bytes.fetch_add(
            ENTRY_OVERHEAD + key.len() as u64,
            Ordering::Relaxed,
        );
        idx
    }

    fn entry(&self, idx: u32) -> Arc<Entry> {
        let t = self.entries.read().expect("entries lock poisoned");
        Arc::clone(&t[idx as usize])
    }

    /// This thread's writer slot.
    ///
    /// Slots are owned by the store and never recycled, and the
    /// thread-local cache holds only an index with **no `Drop`**. A
    /// `thread_local!` that dropped its slot would destroy a dying
    /// thread's partial batch *and* release its floors, which is dropped
    /// obligations and watermark overshoot in one move.
    fn writer(&self) -> &WriterSlot {
        thread_local! {
            static SLOT: std::cell::Cell<Option<usize>> = const { std::cell::Cell::new(None) };
        }
        let n = self.writers.len();
        let idx = SLOT.with(|s| match s.get() {
            Some(i) => i,
            None => {
                let i = (self.next_writer.fetch_add(1, Ordering::Relaxed) as usize) % n;
                s.set(Some(i));
                i
            }
        });
        &self.writers[idx]
    }

    // ---------------------------------------------------------------
    // Reads
    // ---------------------------------------------------------------

    /// Read a key.
    ///
    /// An index miss is **authoritative absence** and never consults the
    /// durable store. Only a non-resident value does.
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, crate::BlobError> {
        let Some(word) = self.index.get(key) else {
            return Ok(None);
        };
        let e = self.entry((word - 1) as u32);
        let cur = e.load_touched();
        match &cur.state {
            ValState::Tombstone => Ok(None),
            ValState::Resident(b) => Ok(Some(b.clone())),
            ValState::Evicted => self.fill(&e, &cur),
        }
    }

    /// Fetch an evicted value and install it.
    ///
    /// The install must lose to any newer write. It compares against the
    /// *exact* record read before going to the durable store, so a writer
    /// that published in the meantime — even one already flushed and
    /// re-evicted — replaced that record and the install fails.
    fn fill(
        &self,
        e: &Arc<Entry>,
        seen: &Arc<Val>,
    ) -> Result<Option<Vec<u8>>, crate::BlobError> {
        let Some(bytes) = self.blobs.get(e.key())? else {
            // The index says this key exists but the store has no row.
            // Report absent rather than inventing data.
            return Ok(None);
        };
        let installed = Val::resident(seen.version, bytes.clone());
        if e.compare_publish(seen, Arc::clone(&installed)) {
            self.account(seen.payload_bytes(), installed.payload_bytes());
            return Ok(Some(bytes));
        }
        // Someone won. Serve whatever is published now, without
        // installing our possibly-stale bytes.
        let now = e.load();
        match &now.state {
            ValState::Tombstone => Ok(None),
            ValState::Resident(b) => Ok(Some(b.clone())),
            // Re-evicted already; our bytes are still that version's, but
            // the caller asked for a value, so hand back what we read.
            ValState::Evicted => Ok(Some(bytes)),
        }
    }

    // ---------------------------------------------------------------
    // Writes
    // ---------------------------------------------------------------

    /// Write, reporting whether the key was newly inserted.
    pub fn put(&self, key: &[u8], val: &[u8]) -> WriteOutcome {
        self.write(key, Some(val), WriteMode::Put)
    }

    /// Write only if absent, reporting whether it was applied.
    pub fn insert(&self, key: &[u8], val: &[u8]) -> WriteOutcome {
        self.write(key, Some(val), WriteMode::Insert)
    }

    /// Publish a tombstone, reporting whether the key existed.
    ///
    /// The key stays in the index forever. Erasing it would break
    /// authoritative absence, and reclaiming it races a concurrent
    /// insert.
    pub fn remove(&self, key: &[u8]) -> WriteOutcome {
        self.write(key, None, WriteMode::Remove)
    }

    fn write(&self, key: &[u8], val: Option<&[u8]>, mode: WriteMode) -> WriteOutcome {
        let idx = self.intern(key, Val::tombstone(0));
        let e = self.entry(idx);
        let w = self.writer();

        // Arm BEFORE drawing: the version exists from the instant it is
        // drawn, and is invisible to both batch and log until submitted.
        w.arm(self.counter.peek());

        let (outcome, ver, freed, added) = e.with_slot(|cur| {
            let live = cur.is_live();
            let refuse = match mode {
                WriteMode::Insert => live,
                WriteMode::Remove => !live,
                WriteMode::Put => false,
            };
            if refuse {
                return (
                    None,
                    (
                        WriteOutcome { wrote: false, existed: live },
                        0,
                        0,
                        0,
                    ),
                );
            }
            let ver = self.counter.draw();
            let nv = match (mode, val) {
                (WriteMode::Remove, _) => Val::tombstone(ver),
                (_, Some(v)) => Val::resident(ver, v.to_vec()),
                (_, None) => Val::tombstone(ver),
            };
            let freed = cur.payload_bytes();
            let added = nv.payload_bytes();
            (
                Some(Arc::clone(&nv)),
                (
                    WriteOutcome { wrote: true, existed: live },
                    ver,
                    freed,
                    added,
                ),
            )
        });

        if !outcome.wrote {
            w.disarm();
            return outcome;
        }
        self.account(freed, added);

        // submit() releases the announce floor only once the ticket is
        // covered by the batch.
        if let Some(full) = w.submit(Ticket { entry: idx, version: ver }, self.cfg.batch) {
            self.log.append(&full);
            w.clear_staged();
        }
        outcome
    }

    fn account(&self, freed: u64, added: u64) {
        if added >= freed {
            self.resident_bytes.fetch_add(added - freed, Ordering::Relaxed);
        } else {
            self.resident_bytes.fetch_sub(freed - added, Ordering::Relaxed);
        }
    }

    // ---------------------------------------------------------------
    // Scans
    // ---------------------------------------------------------------

    /// Ascending scan from `from`, calling `f` for each live key.
    ///
    /// `f` returns `false` to stop. Because the cache holds every key,
    /// this is a scan of the *cache*, not a merge of two sorted streams:
    /// there is no second cursor over the durable store and no dedup. Only
    /// values may need fetching.
    pub fn scan(
        &self,
        from: &[u8],
        mut f: impl FnMut(&[u8], &[u8]) -> bool,
    ) -> Result<(), crate::BlobError> {
        self.scan_dir(from, true, &mut f)
    }

    /// Descending mirror of [`Store::scan`].
    pub fn rscan(
        &self,
        from: &[u8],
        mut f: impl FnMut(&[u8], &[u8]) -> bool,
    ) -> Result<(), crate::BlobError> {
        self.scan_dir(from, false, &mut f)
    }

    fn scan_dir(
        &self,
        from: &[u8],
        ascending: bool,
        f: &mut dyn FnMut(&[u8], &[u8]) -> bool,
    ) -> Result<(), crate::BlobError> {
        let mut cursor = from.to_vec();
        let mut first = true;
        loop {
            let mut chunk: Vec<(Vec<u8>, EntryWord)> = Vec::new();
            let n = if ascending {
                self.index.scan_chunk(&cursor, self.cfg.scan_chunk, &mut chunk)
            } else {
                self.index.rscan_chunk(&cursor, self.cfg.scan_chunk, &mut chunk)
            };
            if n == 0 {
                return Ok(());
            }
            // Chunks are half-open at the seam: the first key of a
            // follow-on chunk is the last key of the previous one, since
            // the index is seeked inclusively.
            let skip = if first { 0 } else { 1 };
            first = false;
            let last = chunk[n - 1].0.clone();
            for (k, word) in chunk.into_iter().skip(skip) {
                let e = self.entry((word - 1) as u32);
                let cur = e.load_touched();
                let bytes = match &cur.state {
                    ValState::Tombstone => continue,
                    ValState::Resident(b) => b.clone(),
                    ValState::Evicted => match self.fill(&e, &cur)? {
                        Some(b) => b,
                        None => continue,
                    },
                };
                if !f(&k, &bytes) {
                    return Ok(());
                }
            }
            if n < self.cfg.scan_chunk {
                return Ok(());
            }
            if last == cursor && skip == 1 {
                return Ok(()); // no forward progress; end of range
            }
            cursor = last;
        }
    }

    // ---------------------------------------------------------------
    // Bulk operations
    // ---------------------------------------------------------------

    /// Delete every key, then make that durable.
    ///
    /// Runs through the ordinary write path so every tombstone gets a
    /// version and a ticket. Returns `false` if the deletions could not be
    /// made durable — in which case the cache still reads as empty but the
    /// durable store has not caught up, so the caller must treat it as a
    /// failure rather than a completed truncate.
    pub fn clear(&self) -> bool {
        if self.io_failing.load(Ordering::Acquire) {
            return false;
        }
        let mut cursor: Vec<u8> = Vec::new();
        let mut first = true;
        loop {
            let mut chunk: Vec<(Vec<u8>, EntryWord)> = Vec::new();
            let n = self.index.scan_chunk(&cursor, self.cfg.scan_chunk, &mut chunk);
            if n == 0 {
                break;
            }
            let last = chunk[n - 1].0.clone();
            let skip = if first { 0 } else { 1 };
            first = false;
            for (k, _) in chunk.into_iter().skip(skip) {
                self.remove(&k);
            }
            if n < self.cfg.scan_chunk {
                break;
            }
            cursor = last;
        }
        self.drain_fully()
    }

    // ---------------------------------------------------------------
    // The flusher
    // ---------------------------------------------------------------

    /// One flusher cycle. Returns how much progress it made.
    ///
    /// Split deliberately into *drain* and *writeback*. Draining folds
    /// tickets into the dirty map at memory speed and frees log space
    /// immediately, so producers essentially never hit backpressure;
    /// writeback then discharges a bounded chunk of that map.
    pub fn flush_cycle(&self) -> usize {
        let mut progressed = 0;

        // --- drain ---------------------------------------------------
        let mut taken: Vec<Ticket> = Vec::new();
        progressed += self.log.drain(self.cfg.drain_bound, &mut taken);
        if !taken.is_empty() {
            let mut d = self.dirty.lock().expect("dirty lock poisoned");
            for t in &taken {
                // KEEP THE OLDEST. `insert` would keep the newest and
                // discharge the older obligation early — precisely the
                // hot-key bug where flush() reports durable for a key
                // that never reached the store.
                let slot = d.entry(t.entry).or_insert(t.version);
                *slot = (*slot).min(t.version);
            }
        }

        // --- steal partial batches -----------------------------------
        // Bounds the straggler window: an acked write sitting in an idle
        // (or dead) thread's batch would otherwise pin the watermark
        // forever. try_append, never append: blocking here would deadlock
        // the flusher against the backpressure only it can relieve.
        for w in &self.writers {
            if let Some(batch) = w.steal() {
                if self.log.try_append(&batch) {
                    w.clear_staged();
                } else {
                    // No room; put it back so nothing is lost. staged_min
                    // still covers these versions meanwhile.
                    self.log.append(&batch);
                    w.clear_staged();
                }
            }
        }

        // --- writeback -----------------------------------------------
        let candidates: Vec<(u32, Version)> = {
            let d = self.dirty.lock().expect("dirty lock poisoned");
            d.iter()
                .take(self.cfg.writeback_chunk)
                .map(|(k, v)| (*k, *v))
                .collect()
        };
        if !candidates.is_empty() {
            let mut keys: Vec<Vec<u8>> = Vec::new();
            let mut vals: Vec<Option<Vec<u8>>> = Vec::new();
            let mut wrote: Vec<u32> = Vec::new();
            for (idx, owed) in &candidates {
                let e = self.entry(*idx);
                // Re-read the CURRENT value: writing bytes snapshotted at
                // drain time is the bug above. `owed` is the version that
                // created the obligation and is deliberately NOT what gets
                // written — it is only good for asserting that versions
                // move forward.
                let cur = e.load();
                debug_assert!(
                    cur.version >= *owed,
                    "entry {idx} went backwards: {} < {owed}",
                    cur.version
                );
                match &cur.state {
                    ValState::Resident(b) => {
                        keys.push(e.key().to_vec());
                        vals.push(Some(b.clone()));
                        wrote.push(*idx);
                    }
                    ValState::Tombstone => {
                        keys.push(e.key().to_vec());
                        vals.push(None);
                        wrote.push(*idx);
                    }
                    // Non-resident means already durable (eviction
                    // requires version <= W), so the obligation is met
                    // with no write. It still must be DISCHARGED, or the
                    // entry wedges in the map and pins the watermark
                    // forever.
                    ValState::Evicted => wrote.push(*idx),
                }
            }
            let ops: Vec<BlobOp<'_>> = keys
                .iter()
                .zip(vals.iter())
                .map(|(k, v)| match v {
                    Some(b) => BlobOp::Put { key: k, val: b },
                    None => BlobOp::Delete { key: k },
                })
                .collect();

            match self.blobs.write_batch(&ops) {
                Ok(()) => {
                    let mut d = self.dirty.lock().expect("dirty lock poisoned");
                    // Erase ONLY after the write succeeded. Draining the
                    // map while building the batch discharges obligations
                    // before knowing they landed.
                    for idx in &wrote {
                        d.remove(idx);
                    }
                    progressed += wrote.len();
                    self.io_failing.store(false, Ordering::Release);
                }
                Err(_) => {
                    // Obligations stay in the map, the watermark stays
                    // pinned below them, and the same entries retry next
                    // cycle. Transient failure self-heals with no loss.
                    self.io_failing.store(true, Ordering::Release);
                }
            }
        }

        self.recompute_watermark();
        self.sync_cv.notify_all();
        progressed
    }

    fn recompute_watermark(&self) {
        // ORDER IS LOAD-BEARING: counter, then writer floors, then the
        // log, then the dirty map. See Floors::min_over.
        let c = self.counter.peek();
        let mut m = Floors::min_over(&self.writers, c);
        let log_min = self.log.min_version();
        if log_min < m {
            m = log_min;
        }
        {
            let d = self.dirty.lock().expect("dirty lock poisoned");
            for v in d.values() {
                if *v < m {
                    m = *v;
                }
            }
        }
        if m == NO_FLOOR {
            m = c;
        }
        self.watermark.advance_to_floor(m);
    }

    /// The current durability watermark.
    pub fn watermark(&self) -> Version {
        self.watermark.get()
    }

    /// Block until every write acked before this call is durable.
    ///
    /// Returns `false` if it gave up because the durable store is failing
    /// or the store is shutting down — in both cases some acked writes
    /// are not durable.
    pub fn sync(&self) -> bool {
        let target = self.counter.peek().saturating_sub(1);
        if target == 0 {
            return true;
        }
        loop {
            if self.watermark.get() >= target {
                return true;
            }
            if self.io_failing.load(Ordering::Acquire)
                || self.stopping.load(Ordering::Acquire)
            {
                return self.watermark.get() >= target;
            }
            let g = self.sync_lock.lock().expect("sync lock poisoned");
            let _unused = self
                .sync_cv
                .wait_timeout(g, std::time::Duration::from_millis(1))
                .expect("sync condvar poisoned");
        }
    }

    /// Drive the flusher until everything currently owed is discharged.
    ///
    /// Used by tests and by shutdown; a real deployment runs
    /// [`Store::flush_cycle`] on its own thread.
    pub fn drain_fully(&self) -> bool {
        // A *transient* failure must not be reported as a failed flush:
        // the obligations stay in the dirty map and the same entries
        // retry, so giving up on the first error turns a self-healing
        // hiccup into a caller-visible durability failure.
        let mut consecutive_failures = 0usize;
        for spin in 0..1_000_000u64 {
            let target = self.counter.peek().saturating_sub(1);
            if self.watermark.get() >= target {
                return true;
            }
            if self.flush_cycle() > 0 {
                consecutive_failures = 0;
                continue;
            }
            if self.io_failing.load(Ordering::Acquire) {
                consecutive_failures += 1;
                if consecutive_failures >= self.cfg.flush_retry_limit {
                    return false;
                }
            } else {
                // Made no progress with healthy IO: something else is
                // holding the floor — an in-flight write, or another
                // flusher doing the work. Wait rather than declare
                // failure; pinning is the fail-stop direction.
                consecutive_failures = 0;
            }
            if spin < 64 {
                std::thread::yield_now();
            } else {
                std::thread::sleep(std::time::Duration::from_micros(100));
            }
        }
        false
    }

    // ---------------------------------------------------------------
    // Eviction
    // ---------------------------------------------------------------

    /// Bytes eviction could still reclaim.
    pub fn evictable_bytes(&self) -> u64 {
        let total = self.resident_bytes.load(Ordering::Relaxed);
        let floor = self.floor_bytes.load(Ordering::Relaxed);
        total.saturating_sub(floor)
    }

    /// Whether eviction is configured at all.
    pub fn has_capacity(&self) -> bool {
        self.cfg.capacity_bytes.is_some()
    }

    /// Whether the value tier is over its ceiling.
    pub fn over_capacity(&self) -> bool {
        match self.cfg.capacity_bytes {
            None => false,
            Some(cap) => self.evictable_bytes() > cap,
        }
    }

    /// Try to evict one entry's value.
    ///
    /// Only a **resident** value at or below the watermark may go. A
    /// tombstone must never become evicted: that transition republishes
    /// the key as live-but-evicted, and the next read fills it from the
    /// durable store, resurrecting a deleted key.
    pub fn evict(&self, idx: u32) -> bool {
        let e = self.entry(idx);
        let cur = e.load();
        match cur.state {
            ValState::Resident(_) => {}
            ValState::Tombstone | ValState::Evicted => return false,
        }
        if cur.version > self.watermark.get() {
            return false; // the only copy
        }
        let marker = Val::evicted(cur.version);
        if e.compare_publish(&cur, marker) {
            self.account(cur.payload_bytes(), 0);
            true
        } else {
            false
        }
    }

    /// One bounded CLOCK pass. Returns entries evicted.
    pub fn sweep_chunk(&self, cursor: &mut u32) -> usize {
        let n = { self.entries.read().expect("entries lock poisoned").len() as u32 };
        if n == 0 {
            return 0;
        }
        let mut evicted = 0;
        for _ in 0..self.cfg.sweep_chunk.min(n as usize) {
            let idx = *cursor % n;
            *cursor = (*cursor + 1) % n;
            let e = self.entry(idx);
            // Second chance: a recently touched value survives, paying
            // for it with the bit.
            if e.take_referenced() {
                continue;
            }
            if self.evict(idx) {
                evicted += 1;
            }
            if !self.over_capacity() {
                break;
            }
        }
        evicted
    }

    /// Signal shutdown; releases anything blocked on backpressure.
    pub fn stop(&self) {
        self.stopping.store(true, Ordering::Release);
        self.log.stop();
        self.sync_cv.notify_all();
    }

    /// Whether the durable store is currently failing.
    pub fn io_failing(&self) -> bool {
        self.io_failing.load(Ordering::Acquire)
    }

    // ---------------------------------------------------------------
    // Introspection
    // ---------------------------------------------------------------

    /// Whether a key's value is currently in memory.
    ///
    /// `None` if the key is unknown or deleted. Exists so tests can assert
    /// eviction actually happened rather than inferring it from timing.
    pub fn is_resident(&self, key: &[u8]) -> Option<bool> {
        let word = self.index.get(key)?;
        let cur = self.entry((word - 1) as u32).load();
        match cur.state {
            ValState::Resident(_) => Some(true),
            ValState::Evicted => Some(false),
            ValState::Tombstone => None,
        }
    }

    /// The version currently published for a key, if it is known.
    pub fn version_of(&self, key: &[u8]) -> Option<Version> {
        let word = self.index.get(key)?;
        Some(self.entry((word - 1) as u32).load().version)
    }

    /// Try to evict one key's value by name. Test-facing wrapper around
    /// the eligibility rules.
    pub fn evict_key(&self, key: &[u8]) -> bool {
        match self.index.get(key) {
            Some(w) => self.evict((w - 1) as u32),
            None => false,
        }
    }

    /// Entries with undischarged writeback obligations.
    pub fn dirty_len(&self) -> usize {
        self.dirty.lock().expect("dirty lock poisoned").len()
    }

    /// Arm a writer floor by hand, as a stuck in-flight write would.
    ///
    /// Returns the slot index to release with
    /// [`Store::release_announce_hold`]. Only meaningful in tests: it is
    /// how the announce-floor properties are made deterministic instead of
    /// depending on catching a real few-instruction window.
    pub fn hold_announce(&self, at: Version) -> usize {
        let idx = (self.next_writer.fetch_add(1, Ordering::Relaxed) as usize)
            % self.writers.len();
        self.writers[idx].arm(at);
        idx
    }

    /// Release a hold taken with [`Store::hold_announce`].
    pub fn release_announce_hold(&self, idx: usize) {
        self.writers[idx].disarm();
    }

    /// Approximate key count, tombstones included.
    pub fn len(&self) -> usize {
        self.index.len()
    }

    /// Whether the index is empty.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}
