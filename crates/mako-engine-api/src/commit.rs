//! Commit outcome vocabulary and the canonical cache-record framing.

use std::num::NonZeroU64;
use std::ptr::NonNull;

use crate::error::{Error, Result};
use crate::timestamp::MakoTimestamp;

/// Whether an engine commit definitely became visible.
///
/// This is deliberately separate from handle cleanup. A transaction can be
/// installed successfully and then encounter a failure while destroying its
/// small facade handle; a write-back cache must still publish that committed
/// transaction to its durability log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "commit visibility must be handled before releasing durability coverage"]
pub enum CommitDisposition {
    /// Validation succeeded and the transaction's writes are visible.
    Committed,
    /// The transaction definitely did not become visible.
    Aborted(Error),
    /// The engine boundary could not prove whether installation occurred.
    Unknown(Error),
}

/// The two independently relevant results of consuming a transaction.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "commit visibility and handle cleanup are independent outcomes"]
pub struct CommitReport {
    /// Whether the transaction became visible.
    pub disposition: CommitDisposition,
    /// Whether the now-terminal facade handle was destroyed cleanly.
    pub cleanup: Result<()>,
}

/// Integrity mode for a cache commit record.
///
/// CRC32C is the default and produces the v5 format. `None` produces a
/// self-describing v6 record without a checksum trailer. It avoids checksum
/// work on the foreground commit path, but replay can then validate only the
/// record's structure, not arbitrary payload corruption.
#[doc(hidden)]
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum CommitRecordChecksum {
    /// Emit v6 without an integrity checksum.
    None = 0,
    /// Emit v5 with a CRC-32C trailer.
    #[default]
    Crc32c = 1,
}

/// Exact sizing for one canonical cache commit record.
///
/// Returned only for a trusted transaction whose write plan has been sealed.
/// The byte count includes the selected format's complete header and, for
/// CRC32C/v5, its checksum trailer; `op_count == 0` identifies a logical
/// read-only transaction that must use the ordinary no-record commit terminal.
#[doc(hidden)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CommitRecordPreflight {
    exact_record_bytes: usize,
    op_count: u32,
    checksum: CommitRecordChecksum,
}

impl CommitRecordPreflight {
    /// Describe a sealed plan. Engines construct this; callers only read it.
    #[doc(hidden)]
    pub const fn from_parts(
        exact_record_bytes: usize,
        op_count: u32,
        checksum: CommitRecordChecksum,
    ) -> Self {
        Self {
            exact_record_bytes,
            op_count,
            checksum,
        }
    }

    /// Complete encoded record size, including the header and optional CRC.
    pub const fn exact_record_bytes(self) -> usize {
        self.exact_record_bytes
    }

    /// Number of canonical final-effect mutations in the record.
    pub const fn op_count(self) -> u32 {
        self.op_count
    }

    /// Integrity mode sealed into this record plan.
    pub const fn checksum(self) -> CommitRecordChecksum {
        self.checksum
    }

    /// Whether canonicalization found no externally visible mutation.
    pub const fn is_empty(self) -> bool {
        self.op_count == 0
    }
}

/// Result of a commit terminal that serializes a canonical record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "a bound record must be published or pinned before acknowledgement"]
pub struct CommitRecordReport {
    /// Commit visibility and facade cleanup outcomes.
    pub commit: CommitReport,
    /// Whether the engine's terminal status, bind callback, and 0/1 completion
    /// witness form one internally consistent result.
    ///
    /// A false value is a contract violation, not an ordinary engine error.
    /// When no record was bound, a durability adapter has no ordered slot it
    /// can pin and must fail-stop the process.
    pub completion_contract_valid: bool,
    /// Whether the synchronous acquire callback returned a sequence and storage.
    pub record_bound: bool,
    /// Whether the engine initialized the complete record and issued its witness.
    pub record_written: bool,
}

/// Result of a commit terminal that seals a preselected one-put holder.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[must_use = "a bound holder must be published, released, or pinned"]
pub struct CommitHolderReport {
    /// Commit visibility and facade cleanup outcomes.
    pub commit: CommitReport,
    /// Whether the engine's terminal and holder-state words form one
    /// internally consistent result.
    pub completion_contract_valid: bool,
    /// Whether the engine accepted this exact preselected generation into
    /// Mako's commit order.
    pub holder_bound: bool,
    /// Whether the engine sealed the complete key/value payload in the holder.
    pub holder_sealed: bool,
}

/// Raw writable storage for an allocation-free record terminal.
///
/// This is an optimization seam. It deliberately carries no Rust lifetime: an
/// engine writes the record after the binding callback returns, while the same
/// synchronous terminal call is still active. Callers must therefore uphold
/// the lifetime and aliasing contract of [`Self::from_raw_parts`].
#[doc(hidden)]
#[derive(Debug, Clone, Copy)]
pub struct CommitRecordTarget {
    sequence: NonZeroU64,
    bytes: NonNull<u8>,
    exact_record_bytes: usize,
}

impl CommitRecordTarget {
    /// Describe storage into which an engine may initialize one complete record.
    ///
    /// # Safety
    ///
    /// `bytes` must remain allocated, stable, and exclusively writable for
    /// `exact_record_bytes` bytes until the synchronous record terminal that
    /// receives this target returns. The storage may be uninitialized on
    /// entry, and any callback that returns this target must not unwind. The
    /// storage must not be read as initialized unless the terminal's
    /// completion contract is valid and its written witness is true.
    pub const unsafe fn from_raw_parts(
        sequence: NonZeroU64,
        bytes: NonNull<u8>,
        exact_record_bytes: usize,
    ) -> Self {
        Self {
            sequence,
            bytes,
            exact_record_bytes,
        }
    }

    /// Lane sequence the record is bound to.
    pub const fn sequence(self) -> NonZeroU64 {
        self.sequence
    }

    /// Start of the exclusively writable storage.
    pub const fn bytes(self) -> NonNull<u8> {
        self.bytes
    }

    /// Exact record extent the storage was sized for.
    pub const fn exact_record_bytes(self) -> usize {
        self.exact_record_bytes
    }
}

/// Ownership result of one attempt at the fused one-put SPSC terminal.
///
/// Only the two `Untouched` variants retain the engine transaction. Every
/// other variant means the engine consumed it, including malformed results;
/// this distinction prevents a later Rust drop from touching a consumed handle.
/// `Outcome` is the engine's compact preselected-holder outcome type.
#[doc(hidden)]
#[derive(Debug)]
#[must_use = "the fused terminal result owns transaction and queue progress"]
pub enum FusedOnePutHolderAttempt<Outcome> {
    /// The engine committed and Release-published ACK as the canonical tail.
    Published,
    /// The transaction is still active and needs the general commit path.
    UntouchedGeneral,
    /// The transaction is active, but the queue needs a cold capacity refresh.
    UntouchedSlow {
        /// Exact unchecked-v6 record extent rederived by the engine.
        exact_record_bytes: std::num::NonZeroU32,
    },
    /// The engine committed behind a fail-stop barrier; Rust advanced the local cursor.
    CommittedUnpublished {
        /// Accepted Mako serialization timestamp.
        timestamp: MakoTimestamp,
        /// Exact unchecked-v6 record extent retained for cold pinning.
        exact_record_bytes: std::num::NonZeroU32,
    },
    /// The engine consumed the transaction with a non-ordinary terminal outcome.
    ConsumedOutcome {
        /// Full compact terminal result for fail-closed cold decoding.
        outcome: Outcome,
        /// Exact unchecked-v6 record extent retained for possible pinning.
        exact_record_bytes: std::num::NonZeroU32,
    },
    /// An explicit untouched result had malformed metadata; the handle is live.
    UntouchedMalformed,
    /// Any malformed non-untouched result; the handle is conservatively consumed.
    ConsumedMalformed,
}

/// Canonical commit-record framing shared by every engine and the cache.
///
/// ```text
/// magic[8]  "MAKOCMT\0" (v5, CRC32C) | "MAKONOC\0" (v6, no checksum)
/// version:u16 | sequence:u64 | physical_us:u64 | logical:u32 | origin:u32 | op_count:u32
/// repeat op_count: tag:u8 (1=put, 2=delete) | table_id:u64 | key_len:u32 | value_len:u32 | key | value
/// v5 only: crc32c:u32 over every preceding byte
/// ```
pub mod record_format {
    /// Magic bytes of a CRC32C-checked v5 record.
    pub const MAGIC_V5: [u8; 8] = *b"MAKOCMT\0";
    /// Magic bytes of an unchecked v6 record.
    pub const MAGIC_V6: [u8; 8] = *b"MAKONOC\0";
    /// Record version carrying a CRC32C trailer.
    pub const VERSION_V5: u16 = 5;
    /// Record version without a checksum trailer.
    pub const VERSION_V6: u16 = 6;
    /// Header bytes before the first operation.
    pub const HEADER_BYTES: usize = 38;
    /// Fixed bytes of one operation before its key and value.
    pub const OP_HEADER_BYTES: usize = 17;
    /// Trailer bytes of a v5 record.
    pub const CRC_BYTES: usize = 4;
    /// Tag of a put operation.
    pub const TAG_PUT: u8 = 1;
    /// Tag of a delete operation.
    pub const TAG_DELETE: u8 = 2;
    /// Exact v6 extent of a record holding one put, before key and value bytes.
    pub const UNCHECKED_ONE_PUT_OVERHEAD_BYTES: usize = HEADER_BYTES + OP_HEADER_BYTES;
}
