//! The assembled cache: masstree in front of RocksDB, in Rust.
//!
//! `mrx-core` holds the logic and knows nothing about either backend;
//! `mrx-masstree` and `mrx-rocks` are the backends; this crate is the one
//! that says *which* backends, starts the background threads, and offers
//! an API shaped like the one callers already have.
//!
//! ```no_run
//! # fn main() -> Result<(), mrx::Error> {
//! let db = mrx::Db::open("/tmp/example", mrx::Options::default())?;
//! db.put(b"key", b"value")?;
//! assert_eq!(db.get(b"key")?.as_deref(), Some(&b"value"[..]));
//! db.close()?;   // waits until everything acked is durable
//! # Ok(())
//! # }
//! ```
//!
//! # What differs from RocksDB, and why it is not a drop-in
//!
//! The API is deliberately RocksDB-*shaped* rather than
//! RocksDB-*compatible*, because one guarantee genuinely differs and
//! hiding that would be worse than the inconvenience of a distinct type:
//!
//! **A write returns before it is durable.** `put` acks once the value is
//! visible in memory; it reaches RocksDB later. That is the entire point
//! of the cache. A crash between the two loses the write. Callers that
//! need RocksDB's guarantee call [`Db::flush`], which is a real barrier —
//! it returns only once everything acked before the call is durable.
//!
//! Two smaller differences follow from the design:
//!
//! * **Memory is bounded by [`Options::capacity_bytes`], not by RocksDB's
//!   block cache.** Keys are always resident; only values are evicted. A
//!   workload with a billion tiny keys is bounded by the key set, and no
//!   capacity setting changes that.
//! * **Deleted keys are never reclaimed from the index.** A tombstone is
//!   published instead. `len()` therefore counts deleted keys, and a
//!   delete-heavy workload grows the index monotonically until reopen.

#![forbid(unsafe_code)]
#![warn(missing_docs)]

use std::path::Path;
use std::sync::Arc;

use mrx_core::{Config, Runtime, Store};
use mrx_masstree::MasstreeIndex;
use mrx_rocks::RocksBlobs;

pub use mrx_core::{BlobError, WriteOutcome};
pub use mrx_rocks::Durability;

mod batch;
mod iter;

pub use batch::WriteBatch;
pub use iter::{Direction, Iter};

/// The concrete store this crate assembles.
pub type MrxStore = Store<MasstreeIndex, RocksBlobs>;

/// Anything that can go wrong opening or using a [`Db`].
#[derive(Debug)]
pub enum Error {
    /// The durable store failed.
    Blob(BlobError),
    /// The key index failed.
    Index(mrx_masstree::IndexError),
    /// A barrier could not be satisfied: some acked writes are not
    /// durable. Never returned for an ordinary read or write.
    NotDurable,
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Blob(e) => write!(f, "{e}"),
            Self::Index(e) => write!(f, "{e}"),
            Self::NotDurable => write!(
                f,
                "the durable store did not accept the pending writes; \
                 acknowledged writes are NOT durable"
            ),
        }
    }
}

impl std::error::Error for Error {}

impl From<BlobError> for Error {
    fn from(e: BlobError) -> Self {
        Self::Blob(e)
    }
}

impl From<mrx_masstree::IndexError> for Error {
    fn from(e: mrx_masstree::IndexError) -> Self {
        Self::Index(e)
    }
}

/// How to open a [`Db`].
#[derive(Debug, Clone)]
pub struct Options {
    /// Durability of each writeback batch.
    pub durability: Durability,
    /// Byte ceiling for the *evictable* value tier. `None` keeps every
    /// value in memory and starts no sweeper.
    ///
    /// Compared against resident bytes minus the un-evictable floor —
    /// entries and record headers, which eviction never reclaims. A
    /// capacity below that floor would otherwise be permanently "over"
    /// and the sweeper would churn forever.
    pub capacity_bytes: Option<u64>,
    /// The rest of the cache tunables. Defaults are fine unless you are
    /// model checking or memory-constrained; note the default ticket log
    /// is a few tens of MB per store, allocated at open.
    pub cache: Config,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            durability: Durability::Wal,
            capacity_bytes: None,
            cache: Config::default(),
        }
    }
}

/// A masstree-over-RocksDB write-back cache.
pub struct Db {
    store: Arc<MrxStore>,
    rt: std::sync::Mutex<Option<Runtime<MasstreeIndex, RocksBlobs>>>,
}

impl Db {
    /// Open, creating the database if it does not exist.
    ///
    /// Loads every existing key into the index before returning. Until
    /// that finishes an index miss cannot be trusted as absence, so the
    /// cost is not optional — it is proportional to the key count, not
    /// the data size, because no values are read.
    pub fn open<P: AsRef<Path>>(path: P, opts: Options) -> Result<Self, Error> {
        let blobs = RocksBlobs::open(path.as_ref(), opts.durability)?;
        let index = MasstreeIndex::new()?;
        let cfg = Config {
            capacity_bytes: opts.capacity_bytes,
            ..opts.cache
        };
        let store = Arc::new(Store::open(cfg, index, blobs)?);
        let rt = Runtime::start(Arc::clone(&store));
        Ok(Self {
            store,
            rt: std::sync::Mutex::new(Some(rt)),
        })
    }

    /// Read a key.
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, Error> {
        Ok(self.store.get(key)?)
    }

    /// Write a key. Returns once the value is visible, **not** once it is
    /// durable.
    pub fn put(&self, key: &[u8], value: &[u8]) -> Result<(), Error> {
        self.store.put(key, value);
        Ok(())
    }

    /// Write a key, reporting whether it already existed.
    pub fn put_reporting(&self, key: &[u8], value: &[u8]) -> WriteOutcome {
        self.store.put(key, value)
    }

    /// Write only if the key is absent. Returns whether it was written.
    pub fn insert(&self, key: &[u8], value: &[u8]) -> bool {
        self.store.insert(key, value).wrote
    }

    /// Delete a key. Returns whether it existed.
    pub fn delete(&self, key: &[u8]) -> Result<bool, Error> {
        Ok(self.store.remove(key).existed)
    }

    /// Apply a batch.
    ///
    /// **Not atomic**, unlike RocksDB's `write`. Each operation goes
    /// through the ordinary write path and becomes visible as it is
    /// applied, so a reader can observe a partially applied batch. The
    /// batch exists to amortise call overhead, not to provide isolation;
    /// building atomicity on top would mean a second commit protocol over
    /// the one the cache already has.
    pub fn write(&self, batch: WriteBatch) -> Result<(), Error> {
        for op in batch.into_ops() {
            match op {
                batch::Op::Put(k, v) => {
                    self.store.put(&k, &v);
                }
                batch::Op::Delete(k) => {
                    self.store.remove(&k);
                }
            }
        }
        Ok(())
    }

    /// Block until every write acked before this call is durable.
    ///
    /// The real barrier. Returns [`Error::NotDurable`] if it could not be
    /// satisfied, which means the durable store is failing — the data is
    /// still readable, but a crash now would lose it.
    pub fn flush(&self) -> Result<(), Error> {
        if self.store.sync() {
            Ok(())
        } else {
            Err(Error::NotDurable)
        }
    }

    /// Iterate forwards from `from` (use `b""` for the beginning).
    pub fn iter(&self, from: &[u8]) -> Iter<'_> {
        Iter::new(&self.store, from, Direction::Forward)
    }

    /// Iterate backwards from `from`, inclusive.
    pub fn iter_rev(&self, from: &[u8]) -> Iter<'_> {
        Iter::new(&self.store, from, Direction::Reverse)
    }

    /// Delete every key and make that durable.
    pub fn clear(&self) -> Result<(), Error> {
        if self.store.clear() {
            Ok(())
        } else {
            Err(Error::NotDurable)
        }
    }

    /// The durability watermark: every version at or below it is durable.
    pub fn watermark(&self) -> u64 {
        self.store.watermark()
    }

    /// Key count, **including deleted keys**. See the crate note.
    pub fn len(&self) -> usize {
        self.store.len()
    }

    /// Whether the index holds no keys at all.
    pub fn is_empty(&self) -> bool {
        self.store.is_empty()
    }

    /// The underlying store, for callers that need the full surface.
    pub fn store(&self) -> &Arc<MrxStore> {
        &self.store
    }

    /// Shut down cleanly: make everything acked durable, then stop.
    ///
    /// Prefer this to dropping. `Drop` does the same thing but has
    /// nowhere to report a failure, so it can only complain to stderr.
    pub fn close(self) -> Result<(), Error> {
        self.shutdown()
    }

    fn shutdown(&self) -> Result<(), Error> {
        let mut g = self.rt.lock().expect("runtime lock poisoned");
        match g.take() {
            None => Ok(()), // already closed
            Some(mut rt) => {
                if rt.shutdown() {
                    Ok(())
                } else {
                    Err(Error::NotDurable)
                }
            }
        }
    }
}

impl Drop for Db {
    fn drop(&mut self) {
        // Draining on drop is the maintenance-exit property, and it must
        // happen whether or not the caller remembered to call `close`.
        // A failure here cannot be returned, so it is at least said out
        // loud: silently exiting having dropped acknowledged writes is
        // the one outcome this design must never produce quietly.
        if self.shutdown().is_err() {
            eprintln!(
                "mrx: shutting down with acknowledged writes NOT durable -- \
                 the durable store refused them. Data has been lost."
            );
        }
    }
}
