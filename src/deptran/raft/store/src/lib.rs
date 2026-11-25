//! Shared durable storage for local and replicated ordering.
//!
//! Production `raft-host` and its single-disk driver use [`journal`]: identical
//! RocksDB entries and metadata, atomic WAL-backed batches, and fenced authority.
//! [`base::MemBase`] supplies a separate crash model for those transitions.
//!
//! The earlier segmented [`Wal`], flusher and recovery modules remain for their
//! component tests and inspection of the retired format. The production host
//! does not use that WAL. Synchronous writes supply the durability guarantee;
//! explicit async mode retains no machine/power-crash acknowledgement guarantee.

pub mod applier;
pub mod base;
pub mod crash;
pub mod crc;
pub mod create;
pub mod flusher;
pub mod fs;
pub mod images;
pub mod journal;
pub mod local;
pub mod queue;
pub mod record;
#[cfg(feature = "rocksdb")]
pub mod rocks;
pub mod segment;
pub mod state;
pub mod stats;
pub mod wal;

mod bytes;

pub use create::{open_store, open_store_with_base, store_path, BaseFactory, Opened};
pub use flusher::{Durable, DurableState, Flusher, HeldReplies};
pub use fs::{MemFs, RealFs, StoreFile, StoreFs};
pub use queue::RecordQueue;
pub use record::{BytesCodec, Codec, Hard, Record, SnapRef};
pub use segment::Identity;
pub use state::SavedState;
pub use wal::{Wal, WalOptions};
