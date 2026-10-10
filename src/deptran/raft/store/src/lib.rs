//! The Rust Raft's on-disk store (docs/verus/disk-persistence.md §3-§5;
//! docs/verus/disk-persistence-plan.md P2).
//!
//! Every Raft step that changes saved state becomes one [`Record`], numbered
//! in step order by the [`RecordQueue`] under the server's `mtx_`. The
//! flusher thread ([`flusher`]) writes each queued group of records to the
//! [`Wal`] as one checksummed batch, syncs it, and publishes the durable
//! sequence number ([`DurableState`]). A restart replays the WAL
//! ([`wal::recover`]) into a [`SavedState`]. The log is redo-only: a record
//! states the step's outcome, never what it replaced.
//!
//! The store is generic over the payload type `P` (an entry's command); the
//! shell supplies a [`Codec`]. All file access goes through [`StoreFs`], so
//! the tests run on [`MemFs`], which models what a crash or a power cut
//! keeps.

pub mod applier;
pub mod base;
pub mod crash;
pub mod crc;
pub mod create;
pub mod flusher;
pub mod fs;
pub mod images;
pub mod local;
pub mod queue;
pub mod record;
pub mod segment;
pub mod state;
pub mod stats;
pub mod wal;
#[cfg(feature = "rocksdb")]
pub mod rocks;

mod bytes;

pub use create::{open_store, open_store_with_base, store_path, BaseFactory, Opened};
pub use flusher::{Durable, DurableState, Flusher, HeldReplies};
pub use fs::{MemFs, RealFs, StoreFile, StoreFs};
pub use queue::RecordQueue;
pub use record::{BytesCodec, Codec, Hard, Record, SnapRef};
pub use segment::Identity;
pub use state::SavedState;
pub use wal::{Wal, WalOptions};
