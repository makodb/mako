//! `mrx-core` — the backend-agnostic core of the Masstree-over-RocksDB
//! write-back cache, ported from the C++ implementation in
//! `src/mako/storage/masstree_rocks_index.hh`.
//!
//! # What this crate is and is not
//!
//! This crate contains **all** the cache logic and **no** FFI. The two
//! things it needs from the outside world are behind traits:
//!
//! * [`KeyIndex`] — an ordered key → `u64` directory (masstree in
//!   production, a `BTreeMap` in tests).
//! * [`Blobs`] — a durable byte store (RocksDB in production, a
//!   `HashMap` in tests).
//!
//! That split exists so the entire cache can be tested, mutation-tested,
//! and model-checked with no C++, no RocksDB, and no `unsafe` in the test
//! path. The C++ implementation it is ported from is mutation-verified
//! 5/5 and serves as a differential oracle.
//!
//! # The two invariants everything else follows from
//!
//! 1. **A write acks before it is durable.** Crash inside that window
//!    loses the un-flushed tail; that is the trade, not a defect.
//! 2. **The index holds every key; only values are evictable.** An index
//!    miss is therefore *authoritative absence*, which is what lets the
//!    existence-reporting writes (`put` returns "newly inserted",
//!    `insert` is put-if-absent, `remove` returns "existed") be
//!    lock-free instead of two-tier read-modify-writes under per-key
//!    locks.
//!
//! # Durability
//!
//! One scalar: the watermark `W`. A published value is durable iff
//! `version <= W`. There is no per-value durable flag.
//!
//! **`W` is a LOW-water mark**, not "the newest version written back".
//! Writeback drains an unordered chunk of the dirty map, never a version
//! prefix, so the only sound reading is "every version at or below this
//! is discharged". A consequence, observed and documented in the C++
//! version: under sustained write overload `W` stops advancing entirely,
//! because the oldest undischarged obligation never retires. Writes stay
//! acked and readable, but nothing becomes durable, `sync()` blocks, and
//! eviction stalls with it. A "fix" that lets `W` advance under backlog
//! is a data-loss bug wearing a performance hat.

// DENY, not forbid, so that exactly one module can opt out and the
// compiler enforces that the list stays exactly one. `durability`
// carries three lines of unsafe for the lock-free ticket ring; every
// other file in this crate is unsafe-free and stays that way.
//
// If a second `#![allow(unsafe_code)]` ever appears in this crate, that
// is the moment to argue about it, not after.
#![deny(unsafe_code)]
#![warn(missing_docs)]

pub mod durability;
pub mod fakes;
pub mod record;
pub mod runtime;
pub mod store;
pub mod table;
pub mod value;

pub use durability::{Floors, Ticket, TicketLog, VersionCounter, Watermark, WriterSlot};
pub use record::{Kind, Record};
pub use runtime::Runtime;
pub use store::{Chunk, Store, WriteOutcome};
pub use table::EntryTable;
pub use value::Entry;

/// Version numbers start at 1, never 0.
///
/// This is load-bearing in two places and is the single easiest thing to
/// get wrong in the port. `W` is computed as `min_floor - 1`, and the
/// barrier's target is `version_ctr - 1`:
///
/// * `AtomicU64::new(0)` — the default anyone reaches for — makes
///   `min_floor - 1` wrap to `u64::MAX` **in release** (Rust wraps in
///   release and panics only in debug), so every value is instantly
///   "durable" and the sweeper evicts the only copy of everything.
/// * the same wrap makes the barrier target `u64::MAX`, so `sync()`
///   blocks forever.
///
/// Because the panic is debug-only, `cargo test` would catch it and the
/// shipped staticlib would not. Hence: start at 1, saturate on the
/// subtraction, and assert it.
pub const FIRST_VERSION: u64 = 1;

/// A value on a cache line of its own.
///
/// For scalars that EVERY writer hits on EVERY write. Without this the
/// store's version counter, its watermark, and the ticket log's mutex
/// word all sit inside one 64-byte line, so a writer drawing a version
/// invalidates the line under the flusher's watermark write and vice
/// versa — coherence traffic between threads that never actually share
/// data.
///
/// Note this is the OPPOSITE case to [`WriterSlot`], where padding was
/// measured and rejected: those are per-thread and read in bulk by the
/// flusher, so spreading them multiplied its scan. These are genuinely
/// shared single words.
#[derive(Debug, Default)]
#[repr(align(64))]
pub struct CacheLine<T>(pub T);

impl<T> std::ops::Deref for CacheLine<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.0
    }
}

/// A monotonically increasing version stamped on every published value.
pub type Version = u64;

/// The opaque word this crate stores in the [`KeyIndex`].
///
/// In production it is a pointer to a leaked, immortal entry, so the
/// index never has to interpret it. It is deliberately a plain `u64` so
/// that the choice of pointer-vs-handle stays a decision of this crate
/// alone and never reaches the FFI boundary.
pub type EntryWord = u64;

/// An ordered key → word directory.
///
/// Deliberately has **no `remove`**. Trap 1 in the design: a delete must
/// publish a tombstone rather than erase the key, because erasing it
/// would break authoritative absence, and reclaiming it races a
/// concurrent insert (masstree has no conditional remove). Leaving
/// `remove` off the trait makes that structural rather than a rule
/// someone has to remember.
pub trait KeyIndex: Send + Sync {
    /// Look up a key. Returns the stored word, or `None` if absent.
    fn get(&self, key: &[u8]) -> Option<EntryWord>;

    /// Insert `word` if the key is absent.
    ///
    /// Returns the word now associated with the key: `word` itself if
    /// this call installed it, or the existing word if another writer
    /// won. Folding "insert if absent" and "read the winner" into one
    /// call removes a race and saves a second traversal.
    fn get_or_insert(&self, key: &[u8], word: EntryWord) -> EntryWord;

    /// Visit up to `budget` keys at or after `from`, in ascending order.
    ///
    /// Fills `out` and returns the number appended. Buffer-filling
    /// rather than callback-driven on purpose: in production this call
    /// runs inside an RCU epoch that pins a per-core spinlock, and
    /// letting arbitrary caller code run there stalls epoch advancement
    /// process-wide — a bug the C++ version had and had to fix.
    fn scan_chunk(&self, from: &[u8], budget: usize, out: &mut Vec<(Vec<u8>, EntryWord)>)
        -> usize;

    /// Descending mirror of [`KeyIndex::scan_chunk`].
    fn rscan_chunk(&self, from: &[u8], budget: usize, out: &mut Vec<(Vec<u8>, EntryWord)>)
        -> usize;

    /// Hint that several calls are about to happen back to back on this
    /// thread, and that the index may amortise per-call setup across them.
    ///
    /// Must be paired with [`KeyIndex::unpin`]; [`Pin`] does that. The
    /// default is a no-op, which is correct for any index with no
    /// per-call setup to amortise — the in-memory fakes, for instance.
    ///
    /// **A pin must not be held across IO, a blocking call, or arbitrary
    /// caller code.** For masstree it opens an RCU region, and an open
    /// region stalls epoch advancement — all reclamation — process-wide,
    /// not merely for the pinning thread. The only sanctioned use is a
    /// short run of adjacent index calls.
    fn pin(&self) {}

    /// Release a pin taken by [`KeyIndex::pin`].
    fn unpin(&self) {}

    /// Approximate key count. Counts tombstones, so it overcounts live
    /// keys — see trap 1.
    fn len(&self) -> usize;

    /// Whether the index is empty (by the same overcounting measure).
    fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

/// So a caller can keep a handle to the index it hands the store.
impl<T: KeyIndex + ?Sized> KeyIndex for std::sync::Arc<T> {
    fn get(&self, key: &[u8]) -> Option<EntryWord> {
        (**self).get(key)
    }
    fn get_or_insert(&self, key: &[u8], word: EntryWord) -> EntryWord {
        (**self).get_or_insert(key, word)
    }
    fn scan_chunk(
        &self,
        from: &[u8],
        budget: usize,
        out: &mut Vec<(Vec<u8>, EntryWord)>,
    ) -> usize {
        (**self).scan_chunk(from, budget, out)
    }
    fn rscan_chunk(
        &self,
        from: &[u8],
        budget: usize,
        out: &mut Vec<(Vec<u8>, EntryWord)>,
    ) -> usize {
        (**self).rscan_chunk(from, budget, out)
    }
    fn pin(&self) {
        (**self).pin();
    }
    fn unpin(&self) {
        (**self).unpin();
    }
    fn len(&self) -> usize {
        (**self).len()
    }
}

/// Holds a [`KeyIndex::pin`] for a scope.
///
/// Exists so the unpin cannot be missed on an early return — and `intern`
/// has one on its hot path.
pub struct Pin<'a, K: KeyIndex + ?Sized>(&'a K);

impl<'a, K: KeyIndex + ?Sized> Pin<'a, K> {
    /// Pin `index` until this guard drops.
    pub fn new(index: &'a K) -> Self {
        index.pin();
        Self(index)
    }
}

impl<K: KeyIndex + ?Sized> Drop for Pin<'_, K> {
    fn drop(&mut self) {
        self.0.unpin();
    }
}

/// A durable byte store: the system of record.
pub trait Blobs: Send + Sync {
    /// Read one key.
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError>;

    /// Apply a batch atomically-enough: either the whole batch lands or
    /// none of it does.
    ///
    /// The cache relies on all-or-nothing per batch, not on ordering
    /// between batches.
    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError>;

    /// Iterate every key, in ascending order, for the open-time load.
    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError>;
}

/// So a caller can keep a handle to the durable store — which the crash
/// and fault-injection tests need, since they assert against the *durable*
/// ground truth rather than through the cache.
impl<T: Blobs + ?Sized> Blobs for std::sync::Arc<T> {
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        (**self).get(key)
    }
    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
        (**self).write_batch(ops)
    }
    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        (**self).for_each_key(f)
    }
}

/// One operation in a [`Blobs::write_batch`].
#[derive(Debug)]
pub enum BlobOp<'a> {
    /// Store these bytes under this key.
    Put {
        /// The key.
        key: &'a [u8],
        /// The value bytes.
        val: &'a [u8],
    },
    /// Remove this key.
    Delete {
        /// The key.
        key: &'a [u8],
    },
}

/// A failure from the durable store.
#[derive(Debug, Clone)]
pub struct BlobError(pub String);

impl std::fmt::Display for BlobError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "blob store error: {}", self.0)
    }
}

impl std::error::Error for BlobError {}

/// Tunables.
///
/// Every one of these is a parameter rather than a constant, from the
/// first commit, for two reasons. Model checking needs absurdly small
/// values (a 4-slot log, 2-entry batches) to enumerate interleavings at
/// all, and retrofitting constants-into-parameters later is a rewrite.
/// Also: the C++ default log is 1 << 20 slots × 24 bytes ≈ 25 MB per
/// store, allocated at open — twenty tables is 500 MB before a single
/// key exists, so it had better be tunable.
#[derive(Debug, Clone)]
pub struct Config {
    /// Slots in the dirty-ticket ring. Power of two.
    pub log_slots: usize,
    /// Tickets a writer buffers before appending to the ring.
    pub batch: usize,
    /// Dirty entries written back per flusher cycle.
    pub writeback_chunk: usize,
    /// Keys visited per scan chunk.
    pub scan_chunk: usize,
    /// Keys visited per CLOCK sweep chunk.
    pub sweep_chunk: usize,
    /// Consecutive failed flusher cycles before a barrier gives up.
    ///
    /// Exists so a *transient* durable-store failure is not reported as a
    /// failed flush. Obligations stay in the dirty map and the same
    /// entries retry, so giving up on the first error would turn a
    /// self-healing hiccup into a caller-visible durability failure.
    pub flush_retry_limit: usize,
    /// Byte ceiling for the *evictable* value tier. `None` disables
    /// eviction entirely and starts no sweeper.
    ///
    /// Compared against resident bytes **minus the un-evictable floor**
    /// (entry records and value headers, which are never reclaimed).
    /// Comparing against the total instead makes any capacity below the
    /// floor permanently "over" and degrades the sweeper into perpetual
    /// churn — a defect the C++ version shipped and had to fix.
    pub capacity_bytes: Option<u64>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            log_slots: 1 << 20,
            batch: 64,
            writeback_chunk: 4096,
            scan_chunk: 512,
            sweep_chunk: 256,
            flush_retry_limit: 64,
            capacity_bytes: None,
        }
    }
}

impl Config {
    /// A tiny configuration for model checking and interleaving tests.
    pub fn tiny() -> Self {
        Self {
            log_slots: 4,
            batch: 2,
            writeback_chunk: 2,
            scan_chunk: 4,
            sweep_chunk: 2,
            flush_retry_limit: 4,
            capacity_bytes: None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_start_at_one_so_the_watermark_cannot_wrap() {
        // The whole point of FIRST_VERSION. If this is ever 0, `W =
        // min_floor - 1` wraps to u64::MAX in release and every value
        // becomes instantly "durable".
        assert_eq!(FIRST_VERSION, 1);
        let w = FIRST_VERSION.saturating_sub(1);
        assert_eq!(w, 0, "an empty store must have W == 0, not u64::MAX");
    }

    #[test]
    fn tiny_config_is_actually_tiny_enough_to_enumerate() {
        let c = Config::tiny();
        assert!(c.log_slots.is_power_of_two());
        assert!(c.log_slots <= 8 && c.batch <= 4);
    }

    #[test]
    fn default_log_is_power_of_two() {
        assert!(Config::default().log_slots.is_power_of_two());
    }
}
