//! Versioned transaction commit records with an explicit integrity mode.
//!
//! A record is both the recovery description of one cache transaction and
//! the source of the single RocksDB batch that materializes that transaction.
//! The backend keyspace is private to `mako-cache`; raw application keys are
//! never used as RocksDB keys directly.
//!
//! The value format is deliberately small and fixed-width where practical:
//!
//! ```text
//! mode_magic[8] | version:u16 | sequence:u64 | mako_timestamp:u32 | op_count:u32
//! repeated op_count times:
//!   tag:u8 | table_id:u64 | key_len:u32 | value_len:u32 | key | value
//! v3 only: crc32c:u32
//! ```
//!
//! All integers are big-endian. A delete has a zero `value_len`; a put may
//! have an empty value. The default v3 format's CRC covers every preceding
//! byte, including the header. The self-describing v4 format deliberately has
//! no checksum trailer and uses a distinct magic, so accidental damage cannot
//! turn a v3 record into an unchecked v4 record merely by changing its version
//! field. It exists for deployments which explicitly trade payload corruption
//! detection for foreground commit latency. Decoding accepts mixed v3/v4 logs
//! and rejects non-canonical input, including unclaimed bytes after the last
//! declared operation.

use std::collections::HashSet;
use std::fmt;
use std::mem::{ManuallyDrop, MaybeUninit};
use std::num::NonZeroU64;
use std::ptr::NonNull;

use mako_local::MakoTimestamp;
use mrx_core::BlobOp;

/// The table used by the first, single-table cache API.
pub const DEFAULT_TABLE_ID: u64 = 1;

const CRC32C_MAGIC: &[u8; 8] = b"MAKOCMT\0";
const UNCHECKED_MAGIC: &[u8; 8] = b"MAKONOC\0";
const FORMAT_VERSION: u16 = 3;
const UNCHECKED_FORMAT_VERSION: u16 = 4;
const PUT_TAG: u8 = 1;
const DELETE_TAG: u8 = 2;

const VERSION_OFFSET: usize = CRC32C_MAGIC.len();
const SEQUENCE_OFFSET: usize = VERSION_OFFSET + 2;
const MAKO_TIMESTAMP_OFFSET: usize = SEQUENCE_OFFSET + 8;
const OP_COUNT_OFFSET: usize = MAKO_TIMESTAMP_OFFSET + 4;
const HEADER_LEN: usize = OP_COUNT_OFFSET + 4;
const OP_HEADER_LEN: usize = 1 + 8 + 4 + 4;
const CRC_LEN: usize = 4;
const MIN_RECORD_LEN: usize = HEADER_LEN + CRC_LEN;
const MIN_UNCHECKED_RECORD_LEN: usize = HEADER_LEN;

#[inline]
const fn expected_magic(version: u16) -> Option<&'static [u8; 8]> {
    match version {
        FORMAT_VERSION => Some(CRC32C_MAGIC),
        UNCHECKED_FORMAT_VERSION => Some(UNCHECKED_MAGIC),
        _ => None,
    }
}

// The leading NUL and binary keyspace version make this visibly an internal
// namespace in RocksDB dumps. The kind byte makes log and materialized data
// keys disjoint, while the fixed-width table/sequence fields make each
// mapping injective.
const LOG_KEY_PREFIX: &[u8] = b"\0mako-cache\0\x01L";
const DATA_KEY_PREFIX: &[u8] = b"\0mako-cache\0\x01D";

/// Monotonic cache commit sequence, distinct from Mako's transaction timestamp.
///
/// Zero is not a valid sequence. Construction is crate-private so only the
/// cache's reservation allocator can mint sequence numbers.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct CommitSeq(NonZeroU64);

impl CommitSeq {
    /// Construct a sequence, returning `None` for the reserved value zero.
    pub(crate) const fn new(raw: u64) -> Option<Self> {
        match NonZeroU64::new(raw) {
            Some(raw) => Some(Self(raw)),
            None => None,
        }
    }

    /// Return the integer representation.
    pub const fn get(self) -> u64 {
        self.0.get()
    }
}

/// One materialized mutation in a cache transaction.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Mutation {
    /// Store a raw user value.
    Put {
        /// Stable logical table identifier.
        table_id: u64,
        /// Raw application key.
        key: Vec<u8>,
        /// Raw application value; no MassTrans trailer is present.
        value: Vec<u8>,
    },
    /// Remove an application key.
    Delete {
        /// Stable logical table identifier.
        table_id: u64,
        /// Raw application key.
        key: Vec<u8>,
    },
}

impl Mutation {
    fn table_id(&self) -> u64 {
        match self {
            Self::Put { table_id, .. } | Self::Delete { table_id, .. } => *table_id,
        }
    }

    fn key(&self) -> &[u8] {
        match self {
            Self::Put { key, .. } | Self::Delete { key, .. } => key,
        }
    }

    fn value(&self) -> Option<&[u8]> {
        match self {
            Self::Put { value, .. } => Some(value),
            Self::Delete { .. } => None,
        }
    }
}

/// Classification of one key found in the private RocksDB keyspace.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum BackendKey<'a> {
    /// A complete transaction record, identified by its commit sequence.
    Log(CommitSeq),
    /// A materialized application key.
    Data {
        /// Stable logical table identifier.
        table_id: u64,
        /// Raw application key suffix.
        key: &'a [u8],
    },
    /// A legacy, malformed, or otherwise unknown key.
    Foreign,
}

/// Classify a private backend key without allocating.
///
/// Log keys must have exactly one eight-byte sequence suffix. Data keys have
/// an eight-byte table identifier followed by an arbitrary (possibly empty)
/// raw application key. Zero is never accepted as a log sequence.
pub(crate) fn classify_backend_key(key: &[u8]) -> BackendKey<'_> {
    if key.starts_with(LOG_KEY_PREFIX) {
        let suffix = &key[LOG_KEY_PREFIX.len()..];
        if suffix.len() != 8 {
            return BackendKey::Foreign;
        }
        let raw = u64::from_be_bytes(suffix.try_into().expect("length checked"));
        return match CommitSeq::new(raw) {
            Some(sequence) => BackendKey::Log(sequence),
            None => BackendKey::Foreign,
        };
    }

    if key.starts_with(DATA_KEY_PREFIX) {
        let suffix = &key[DATA_KEY_PREFIX.len()..];
        let Some(table_bytes) = suffix.get(..8) else {
            return BackendKey::Foreign;
        };
        let table_id = u64::from_be_bytes(table_bytes.try_into().expect("length checked"));
        return BackendKey::Data {
            table_id,
            key: &suffix[8..],
        };
    }

    BackendKey::Foreign
}

/// A complete write set whose storage is prepared before native commit.
///
/// Construction performs every validation, length calculation, and heap
/// allocation needed by the eventual backend record. [`Self::bind`] only
/// writes fixed-width scalar fields into this existing storage, so it is safe
/// to call at Mako's post-validation serialization point while native write
/// locks are held.
#[derive(Debug, Eq, PartialEq)]
pub(crate) struct PreparedCommitRecord {
    mutations: Vec<Mutation>,
    encoded: Vec<u8>,
    log_key: Vec<u8>,
    data_keys: Vec<Vec<u8>>,
}

impl PreparedCommitRecord {
    /// Validate and preallocate a complete transaction write set.
    ///
    /// `max_bytes` applies to the final encoded log value, including its Mako
    /// timestamp and checksum. Duplicate `(table_id, key)` mutations are
    /// rejected rather than relying on backend batch ordering.
    pub(crate) fn prepare(mutations: Vec<Mutation>, max_bytes: usize) -> Result<Self, RecordError> {
        validate_unique_mutations(&mutations)?;
        let encoded_len = encoded_len(&mutations)?;
        if encoded_len > max_bytes {
            return Err(RecordError::RecordTooLarge {
                size: encoded_len,
                max: max_bytes,
            });
        }

        let encoded = encode_prepared(&mutations, encoded_len)?;
        let log_key = make_unbound_log_key()?;
        let data_keys = make_data_keys(&mutations)?;

        Ok(Self {
            mutations,
            encoded,
            log_key,
            data_keys,
        })
    }

    /// Bind ordering metadata after native validation succeeds.
    ///
    /// This operation is constant-time and allocation-free. Checksumming the
    /// potentially large encoded write set is deliberately deferred to
    /// [`BoundCommitRecord::finalize`], which may run after native locks have
    /// been released but before the queue entry becomes ready for write-back.
    pub(crate) fn bind(
        mut self,
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
    ) -> BoundCommitRecord {
        self.encoded[SEQUENCE_OFFSET..MAKO_TIMESTAMP_OFFSET]
            .copy_from_slice(&sequence.get().to_be_bytes());
        self.encoded[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET]
            .copy_from_slice(&mako_timestamp.get().to_be_bytes());
        let suffix = self
            .log_key
            .get_mut(LOG_KEY_PREFIX.len()..)
            .expect("prepared log key has a sequence suffix");
        suffix.copy_from_slice(&sequence.get().to_be_bytes());

        BoundCommitRecord {
            sequence,
            mako_timestamp,
            mutations: self.mutations,
            encoded: self.encoded,
            log_key: self.log_key,
            data_keys: self.data_keys,
        }
    }
}

/// An allocation-free bound record awaiting its checksum.
#[derive(Debug, Eq, PartialEq)]
pub(crate) struct BoundCommitRecord {
    sequence: CommitSeq,
    mako_timestamp: MakoTimestamp,
    mutations: Vec<Mutation>,
    encoded: Vec<u8>,
    log_key: Vec<u8>,
    data_keys: Vec<Vec<u8>>,
}

impl BoundCommitRecord {
    /// Finish the checksum without allocating and produce a backend record.
    pub(crate) fn finalize(mut self) -> CommitRecord {
        let checksum_offset = self.encoded.len() - CRC_LEN;
        let checksum = crc32c(&self.encoded[..checksum_offset]);
        self.encoded[checksum_offset..].copy_from_slice(&checksum.to_be_bytes());

        CommitRecord {
            sequence: self.sequence,
            mako_timestamp: self.mako_timestamp,
            mutations: self.mutations,
            encoded: self.encoded,
            log_key: self.log_key,
            data_keys: self.data_keys,
        }
    }
}

/// Preallocated storage for the cold legacy write-back producer.
///
/// The legacy API prepares this box before entering native commit, then turns
/// the value into its final record in the same allocation after binding.  The
/// transient variant permits moving the prepared value out without allocating
/// another `Box` at a point where Silo may already have installed the write.
#[derive(Debug, Eq, PartialEq)]
pub(crate) enum LegacyCommitRecord {
    Prepared(PreparedCommitRecord),
    Ready(CommitRecord),
    Transitioning,
}

impl LegacyCommitRecord {
    pub(crate) fn prepare(mutations: Vec<Mutation>, max_bytes: usize) -> Result<Self, RecordError> {
        PreparedCommitRecord::prepare(mutations, max_bytes).map(Self::Prepared)
    }

    /// Bind and checksum this record without changing its heap allocation.
    pub(crate) fn finalize_in_place(&mut self, sequence: CommitSeq, mako_timestamp: MakoTimestamp) {
        let prepared = match std::mem::replace(self, Self::Transitioning) {
            Self::Prepared(prepared) => prepared,
            Self::Ready(_) => panic!("a legacy record may be finalized only once"),
            Self::Transitioning => panic!("a legacy record cannot re-enter finalization"),
        };
        *self = Self::Ready(prepared.bind(sequence, mako_timestamp).finalize());
    }

    fn ready(&self) -> &CommitRecord {
        match self {
            Self::Ready(record) => record,
            Self::Prepared(_) => panic!("a prepared legacy record is not publishable"),
            Self::Transitioning => panic!("legacy record finalization did not complete"),
        }
    }
}

/// One complete v3 or v4 record produced directly by the trusted native
/// transaction adapter.
///
/// Native code fills the encoded bytes after Silo has validated the canonical
/// write set and before it installs any write.  The foreground cache path
/// retains those bytes without decoding or copying them; validation and
/// construction of materialized RocksDB keys happen on the background replay
/// thread.
#[derive(Debug, Eq, PartialEq)]
pub(crate) struct NativeCommitRecord {
    sequence: CommitSeq,
    mako_timestamp: MakoTimestamp,
    encoded: NativeRecordBytes,
}

/// One native one-Put commit whose canonical recovery bytes are intentionally
/// deferred to the background writer.
///
/// The sequence-indexed C++ holder owns the exact value allocation originally
/// staged for STO.  This descriptor owns no pointer into that holder: the
/// consumer must acquire a fresh exact-generation view for each replay
/// attempt, and the holder is released only after successful backend
/// retirement.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct DeferredOnePutRecord {
    sequence: CommitSeq,
    mako_timestamp: MakoTimestamp,
    exact_record_bytes: u32,
}

impl DeferredOnePutRecord {
    /// Exact unchecked-v4 extent represented by one native holder view.
    pub(crate) fn encoded_len_for(key_len: usize, value_len: usize) -> Result<usize, RecordError> {
        HEADER_LEN
            .checked_add(OP_HEADER_LEN)
            .and_then(|size| size.checked_add(key_len))
            .and_then(|size| size.checked_add(value_len))
            .ok_or(RecordError::LengthOverflow)
    }

    /// Adopt the scalar witness returned by the trusted native holder
    /// terminal. The complete view is revalidated during materialization.
    pub(crate) fn new(
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
        exact_record_bytes: usize,
    ) -> Self {
        let exact_record_bytes =
            u32::try_from(exact_record_bytes).expect("the trusted one-Put record extent fits u32");
        Self {
            sequence,
            mako_timestamp,
            exact_record_bytes,
        }
    }

    pub(crate) const fn sequence(self) -> CommitSeq {
        self.sequence
    }

    pub(crate) const fn mako_timestamp(self) -> MakoTimestamp {
        self.mako_timestamp
    }

    pub(crate) const fn encoded_len(self) -> usize {
        self.exact_record_bytes as usize
    }

    /// Copy one exact holder view into the canonical unchecked-v4 recovery
    /// representation on the background thread.
    pub(crate) fn materialize(
        self,
        table_id: u64,
        key: &[u8],
        value: &[u8],
        max_bytes: usize,
    ) -> Result<CommitRecord, RecordError> {
        if u32::try_from(key.len()).is_err() {
            return Err(RecordError::KeyTooLarge(key.len()));
        }
        if u32::try_from(value.len()).is_err() {
            return Err(RecordError::ValueTooLarge(value.len()));
        }
        let actual = Self::encoded_len_for(key.len(), value.len())?;
        let expected = self.encoded_len();
        if actual != expected {
            return Err(RecordError::WrongEncodedLength { expected, actual });
        }
        if actual > max_bytes {
            return Err(RecordError::RecordTooLarge {
                size: actual,
                max: max_bytes,
            });
        }

        let owned_key = copy_bytes(key)?;
        let owned_value = copy_bytes(value)?;
        let mut mutations = Vec::new();
        mutations
            .try_reserve_exact(1)
            .map_err(|_| RecordError::AllocationFailed)?;
        mutations.push(Mutation::Put {
            table_id,
            key: owned_key,
            value: owned_value,
        });

        let mut encoded = Vec::new();
        encoded
            .try_reserve_exact(actual)
            .map_err(|_| RecordError::AllocationFailed)?;
        encoded.extend_from_slice(UNCHECKED_MAGIC);
        encoded.extend_from_slice(&UNCHECKED_FORMAT_VERSION.to_be_bytes());
        encoded.extend_from_slice(&self.sequence.get().to_be_bytes());
        encoded.extend_from_slice(&self.mako_timestamp.get().to_be_bytes());
        encoded.extend_from_slice(&1_u32.to_be_bytes());
        encoded.push(PUT_TAG);
        encoded.extend_from_slice(&table_id.to_be_bytes());
        encoded.extend_from_slice(&(key.len() as u32).to_be_bytes());
        encoded.extend_from_slice(&(value.len() as u32).to_be_bytes());
        encoded.extend_from_slice(key);
        encoded.extend_from_slice(value);
        debug_assert_eq!(encoded.len(), actual);

        let log_key = make_log_key(self.sequence)?;
        let data_keys = make_data_keys(&mutations)?;
        Ok(CommitRecord {
            sequence: self.sequence,
            mako_timestamp: self.mako_timestamp,
            mutations,
            encoded,
            log_key,
            data_keys,
        })
    }
}

/// Ownership of bytes filled by the trusted native serializer.
///
/// Small production records normally point into `Writeback`'s fixed arena;
/// oversized records and unit-test records retain an ordinary vector. The
/// exact sequence's ring turn keeps an arena block unavailable until the
/// background backend call has completed.
#[derive(Debug)]
enum NativeRecordBytes {
    Owned(Vec<u8>),
    Arena {
        bytes: NonNull<u8>,
        len: usize,
        block: usize,
    },
}

// SAFETY: an Arena token identifies a uniquely occupied ring block until it is
// consumed for retirement. The backing arena is stable and outlives every
// queued record, and bytes are immutable after native's exact completion
// witness. Owned vectors have the ordinary Send/Sync properties of Vec<u8>.
unsafe impl Send for NativeRecordBytes {}
unsafe impl Sync for NativeRecordBytes {}

impl NativeRecordBytes {
    fn as_slice(&self) -> &[u8] {
        match self {
            Self::Owned(bytes) => bytes,
            Self::Arena { bytes, len, .. } => {
                // SAFETY: NativeRecordBytes::from_arena is called only after
                // native's exact completion witness. Its unique arena token
                // keeps this initialized block unavailable for mutation.
                unsafe { std::slice::from_raw_parts(bytes.as_ptr(), *len) }
            }
        }
    }

    fn len(&self) -> usize {
        self.as_slice().len()
    }

    fn into_recycled(self) -> RecycledNativeRecord {
        match self {
            Self::Owned(bytes) => {
                let mut bytes = ManuallyDrop::new(bytes);
                let ptr = bytes.as_mut_ptr().cast::<MaybeUninit<u8>>();
                let capacity = bytes.capacity();
                // SAFETY: MaybeUninit<u8> has the same layout as u8. A zero
                // logical length prevents future users from observing stale
                // initialized bytes before resizing the reusable buffer.
                let bytes = unsafe { Vec::from_raw_parts(ptr, 0, capacity) };
                RecycledNativeRecord::Owned(bytes)
            }
            Self::Arena { block, .. } => RecycledNativeRecord::Arena(block),
        }
    }
}

impl PartialEq for NativeRecordBytes {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

impl Eq for NativeRecordBytes {}

/// Storage returned to the writeback allocator after a successful replay.
#[derive(Debug)]
pub(crate) enum RecycledNativeRecord {
    Arena(usize),
    Owned(Vec<MaybeUninit<u8>>),
    /// Release the exact native holder generation after successful replay.
    Holder(CommitSeq),
}

impl NativeCommitRecord {
    /// Adopt bytes initialized by the build-private native serializer.
    ///
    /// The serializer and Rust wrapper share an exact build fingerprint and a
    /// completion witness.  These constant-time checks deliberately fail fast
    /// on a broken private contract: after `sequence` has been inserted into
    /// the ordered queue, treating malformed bytes as an ordinary abort would
    /// create a durability hole.
    pub(crate) fn from_native(
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
        encoded: Vec<u8>,
    ) -> Self {
        Self::from_storage(sequence, mako_timestamp, NativeRecordBytes::Owned(encoded))
    }

    /// Adopt a fully initialized block from the writeback arena.
    ///
    /// # Safety
    ///
    /// `encoded` must point to `len` initialized bytes in the uniquely owned
    /// arena block identified by `block`. The arena must remain stable and live
    /// until this record is consumed by the owning writeback queue. The bytes
    /// must be the canonical native record for exactly `sequence` and
    /// `mako_timestamp`, with the integrity-mode magic/version selected during
    /// preflight. The build fingerprint and native completion witness are the
    /// production proof of that private contract; background materialization
    /// still performs the complete untrusted-record validation before replay.
    #[inline]
    pub(crate) unsafe fn from_native_arena(
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
        encoded: NonNull<u8>,
        len: usize,
        block: usize,
    ) -> Self {
        // Do not reread the record header on the acknowledgement path. Native
        // has just initialized this exact target under the private fingerprinted
        // ABI and returned its terminal completion witness. Any caller that
        // fabricates that proof violates this function's safety contract.
        Self {
            sequence,
            mako_timestamp,
            encoded: NativeRecordBytes::Arena {
                bytes: encoded,
                len,
                block,
            },
        }
    }

    fn from_storage(
        sequence: CommitSeq,
        mako_timestamp: MakoTimestamp,
        encoded: NativeRecordBytes,
    ) -> Self {
        let bytes = encoded.as_slice();
        assert!(
            bytes.len() >= MIN_UNCHECKED_RECORD_LEN,
            "native commit record is shorter than its fixed header"
        );
        let version = u16::from_be_bytes(
            bytes[VERSION_OFFSET..SEQUENCE_OFFSET]
                .try_into()
                .expect("fixed version field"),
        );
        let magic = expected_magic(version).expect("native commit record has the wrong version");
        assert_eq!(
            bytes.get(..CRC32C_MAGIC.len()),
            Some(magic.as_slice()),
            "native commit record has the wrong magic for its integrity mode"
        );
        if version == FORMAT_VERSION {
            assert!(
                bytes.len() >= MIN_RECORD_LEN,
                "native v3 commit record has no CRC32C trailer"
            );
        }
        assert_eq!(
            u64::from_be_bytes(
                bytes[SEQUENCE_OFFSET..MAKO_TIMESTAMP_OFFSET]
                    .try_into()
                    .expect("fixed sequence field"),
            ),
            sequence.get(),
            "native commit record has the wrong cache sequence"
        );
        assert_eq!(
            u32::from_be_bytes(
                bytes[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET]
                    .try_into()
                    .expect("fixed timestamp field"),
            ),
            mako_timestamp.get(),
            "native commit record has the wrong Mako timestamp"
        );

        Self {
            sequence,
            mako_timestamp,
            encoded,
        }
    }

    pub(crate) const fn sequence(&self) -> CommitSeq {
        self.sequence
    }

    pub(crate) fn encoded_len(&self) -> usize {
        self.encoded.len()
    }

    /// Decode and materialize RocksDB replay state off the acknowledgement
    /// path. Checksummed v3 records are verified here even though the trusted
    /// native producer already computed the CRC. Explicitly unchecked v4
    /// records receive the same structural validation but cannot detect
    /// arbitrary payload corruption.
    pub(crate) fn materialize(&self, max_bytes: usize) -> Result<CommitRecord, RecordError> {
        let log_key = make_log_key(self.sequence)?;
        let record = CommitRecord::decode(&log_key, self.encoded.as_slice(), max_bytes)?;
        if record.mako_timestamp() != self.mako_timestamp {
            return Err(RecordError::WrongMakoTimestamp {
                expected: self.mako_timestamp.get(),
                record: record.mako_timestamp().get(),
            });
        }
        Ok(record)
    }

    fn into_recycled(self) -> RecycledNativeRecord {
        self.encoded.into_recycled()
    }
}

/// Record representation stored in an ordered write-back slot.
///
/// Legacy/unit-test producers may still supply an already materialized record.
/// The production cache uses `Native`, which keeps foreground acknowledgement
/// to one native serialization and defers decoding/key construction to replay.
#[derive(Debug, Eq, PartialEq)]
pub(crate) enum QueuedCommitRecord {
    /// The already-materialized representation is retained only by the legacy
    /// producer and unit tests. Keep its five vectors out of every production
    /// native queue value: otherwise Rust sizes the whole enum for this cold
    /// variant and moves more than two cache lines on each native commit.
    Materialized(Box<LegacyCommitRecord>),
    Native(NativeCommitRecord),
    /// A copy-free foreground holder awaiting background v4 encoding.
    Holder(DeferredOnePutRecord),
}

impl QueuedCommitRecord {
    #[inline]
    pub(crate) fn sequence(&self) -> CommitSeq {
        match self {
            Self::Materialized(record) => record.ready().sequence(),
            Self::Native(record) => record.sequence(),
            Self::Holder(record) => record.sequence(),
        }
    }

    #[cfg(test)]
    pub(crate) fn mako_timestamp(&self) -> MakoTimestamp {
        match self {
            Self::Materialized(record) => record.ready().mako_timestamp(),
            Self::Native(record) => record.mako_timestamp,
            Self::Holder(record) => record.mako_timestamp(),
        }
    }

    pub(crate) fn encoded_len(&self) -> usize {
        match self {
            Self::Materialized(record) => record.ready().encoded.len(),
            Self::Native(record) => record.encoded_len(),
            Self::Holder(record) => record.encoded_len(),
        }
    }

    pub(crate) const fn deferred_holder(&self) -> Option<DeferredOnePutRecord> {
        match self {
            Self::Holder(record) => Some(*record),
            Self::Materialized(_) | Self::Native(_) => None,
        }
    }

    pub(crate) fn materialize(&self, max_bytes: usize) -> Result<CommitRecord, RecordError> {
        match self {
            Self::Materialized(record) => Ok(record.ready().clone()),
            Self::Native(record) => record.materialize(max_bytes),
            Self::Holder(_) => {
                panic!("a deferred holder record requires its exact native pool view")
            }
        }
    }

    /// Consume a successfully replayed queue record and return reusable native
    /// storage. Legacy materialized records have no native buffer to recycle.
    pub(crate) fn into_recycled_native(self) -> Option<RecycledNativeRecord> {
        match self {
            Self::Materialized(_) => None,
            Self::Native(record) => Some(record.into_recycled()),
            Self::Holder(record) => Some(RecycledNativeRecord::Holder(record.sequence())),
        }
    }
}

/// A sealed, owned commit record.
///
/// Besides the encoded recovery value, this owns every materialized RocksDB
/// key. Consequently no key or value bytes need to be copied when the
/// background writer constructs the transaction's atomic backend batch.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommitRecord {
    sequence: CommitSeq,
    mako_timestamp: MakoTimestamp,
    mutations: Vec<Mutation>,
    encoded: Vec<u8>,
    log_key: Vec<u8>,
    data_keys: Vec<Vec<u8>>,
}

impl CommitRecord {
    /// Decode and validate a record stored under `log_key`.
    ///
    /// The sequence in the exact private log key must match the encoded
    /// sequence. This prevents a valid record from being moved to a different
    /// commit position. The returned record owns all decoded bytes.
    pub(crate) fn decode(
        log_key: &[u8],
        value: &[u8],
        max_bytes: usize,
    ) -> Result<Self, RecordError> {
        let key_sequence = match classify_backend_key(log_key) {
            BackendKey::Log(sequence) => sequence,
            BackendKey::Data { .. } | BackendKey::Foreign => return Err(RecordError::ForeignKey),
        };

        if value.len() > max_bytes {
            return Err(RecordError::RecordTooLarge {
                size: value.len(),
                max: max_bytes,
            });
        }
        if value.len() < MIN_UNCHECKED_RECORD_LEN {
            return Err(RecordError::Truncated);
        }

        let encoded_version = u16::from_be_bytes(
            value[VERSION_OFFSET..SEQUENCE_OFFSET]
                .try_into()
                .expect("fixed version field"),
        );
        let magic = expected_magic(encoded_version)
            .ok_or(RecordError::UnsupportedVersion(encoded_version))?;
        if value.get(..CRC32C_MAGIC.len()) != Some(magic.as_slice()) {
            return Err(RecordError::BadMagic);
        }
        let operations_end = match encoded_version {
            FORMAT_VERSION => {
                if value.len() < MIN_RECORD_LEN {
                    return Err(RecordError::Truncated);
                }
                let checksum_offset = value.len() - CRC_LEN;
                let stored_checksum = u32::from_be_bytes(
                    value[checksum_offset..]
                        .try_into()
                        .expect("four-byte checksum suffix"),
                );
                let calculated_checksum = crc32c(&value[..checksum_offset]);
                if stored_checksum != calculated_checksum {
                    return Err(RecordError::BadChecksum {
                        expected: stored_checksum,
                        actual: calculated_checksum,
                    });
                }
                checksum_offset
            }
            UNCHECKED_FORMAT_VERSION => value.len(),
            _ => unreachable!("known record version selected above"),
        };

        let mut cursor = Cursor::new(&value[..operations_end]);
        let _magic = cursor.take(CRC32C_MAGIC.len())?;
        let version = cursor.read_u16()?;
        debug_assert_eq!(version, encoded_version);

        let raw_sequence = cursor.read_u64()?;
        let sequence = CommitSeq::new(raw_sequence).ok_or(RecordError::InvalidSequence)?;
        if sequence != key_sequence {
            return Err(RecordError::WrongSequence {
                key: key_sequence.get(),
                record: sequence.get(),
            });
        }

        let mako_timestamp =
            MakoTimestamp::new(cursor.read_u32()?).ok_or(RecordError::InvalidMakoTimestamp)?;
        let op_count = cursor.read_u32()? as usize;
        // Even an empty-key delete needs a complete operation header. This
        // check prevents a corrupt count from provoking a huge allocation.
        if op_count > cursor.remaining() / OP_HEADER_LEN {
            return Err(RecordError::Truncated);
        }

        let mut parsed = Vec::new();
        parsed
            .try_reserve_exact(op_count)
            .map_err(|_| RecordError::AllocationFailed)?;

        for _ in 0..op_count {
            let tag = cursor.read_u8()?;
            if tag != PUT_TAG && tag != DELETE_TAG {
                return Err(RecordError::UnknownTag(tag));
            }
            let table_id = cursor.read_u64()?;
            let key_len = cursor.read_u32()? as usize;
            let value_len = cursor.read_u32()? as usize;
            let key = cursor.take(key_len)?;

            match tag {
                PUT_TAG => {
                    let value = cursor.take(value_len)?;
                    parsed.push(ParsedMutation::Put {
                        table_id,
                        key,
                        value,
                    });
                }
                DELETE_TAG => {
                    if value_len != 0 {
                        return Err(RecordError::DeleteWithValue(value_len));
                    }
                    parsed.push(ParsedMutation::Delete { table_id, key });
                }
                _ => unreachable!("operation tag checked above"),
            }
        }

        if cursor.remaining() != 0 {
            return Err(RecordError::TrailingBytes(cursor.remaining()));
        }
        validate_unique_parsed(&parsed)?;

        let mut mutations = Vec::new();
        mutations
            .try_reserve_exact(parsed.len())
            .map_err(|_| RecordError::AllocationFailed)?;
        for mutation in parsed {
            mutations.push(mutation.to_owned()?);
        }

        let encoded = copy_bytes(value)?;
        let canonical_log_key = make_log_key(sequence)?;
        let data_keys = make_data_keys(&mutations)?;
        Ok(Self {
            sequence,
            mako_timestamp,
            mutations,
            encoded,
            log_key: canonical_log_key,
            data_keys,
        })
    }

    /// Commit sequence carried by this record.
    pub const fn sequence(&self) -> CommitSeq {
        self.sequence
    }

    /// Mako transaction timestamp carried by this record.
    pub const fn mako_timestamp(&self) -> MakoTimestamp {
        self.mako_timestamp
    }

    /// Complete transaction write set in application-key form.
    pub fn mutations(&self) -> &[Mutation] {
        &self.mutations
    }

    /// Serialized recovery value, with the record's selected integrity mode.
    #[cfg(test)]
    pub fn encoded(&self) -> &[u8] {
        &self.encoded
    }

    /// Exact private RocksDB key for the serialized recovery value.
    #[cfg(test)]
    pub fn log_key(&self) -> &[u8] {
        &self.log_key
    }

    /// Build the one atomic backend batch for this transaction.
    ///
    /// The log-record put is first, followed by materialized data mutations
    /// in their encoded order. Every byte slice points into `self`.
    #[cfg(test)]
    pub(crate) fn backend_ops(&self) -> Vec<BlobOp<'_>> {
        debug_assert_eq!(self.mutations.len(), self.data_keys.len());
        let mut ops = Vec::with_capacity(self.mutations.len() + 1);
        self.append_backend_ops(&mut ops);
        ops
    }

    /// Number of backend operations contributed by this logical transaction.
    pub(crate) fn backend_op_count(&self) -> usize {
        self.mutations.len() + 1
    }

    /// Append this transaction's log record and materialized mutations to a
    /// larger atomic backend batch.
    ///
    /// Callers append records in dense cache-sequence order. Consequently a
    /// later transaction updating the same key naturally wins inside the one
    /// RocksDB `WriteBatch`, while every individual recovery record remains
    /// present.
    pub(crate) fn append_backend_ops<'a>(&'a self, ops: &mut Vec<BlobOp<'a>>) {
        debug_assert_eq!(self.mutations.len(), self.data_keys.len());
        ops.push(BlobOp::Put {
            key: &self.log_key,
            val: &self.encoded,
        });

        for (mutation, data_key) in self.mutations.iter().zip(&self.data_keys) {
            match mutation {
                Mutation::Put { value, .. } => ops.push(BlobOp::Put {
                    key: data_key,
                    val: value,
                }),
                Mutation::Delete { .. } => ops.push(BlobOp::Delete { key: data_key }),
            }
        }
    }
}

/// Why a commit record could not be sealed or decoded.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum RecordError {
    /// A zero commit sequence appeared in encoded input.
    InvalidSequence,
    /// A Mako timestamp was zero or exceeded its representable base range.
    InvalidMakoTimestamp,
    /// The supplied RocksDB key is not an exact private log key.
    ForeignKey,
    /// The record was stored under a different commit sequence.
    WrongSequence {
        /// Sequence encoded in the RocksDB log key.
        key: u64,
        /// Sequence encoded in the record value.
        record: u64,
    },
    /// The native record carries a different timestamp from its bound queue
    /// reservation.
    WrongMakoTimestamp {
        /// Timestamp assigned at the post-validation bind point.
        expected: u32,
        /// Timestamp encoded in the native record.
        record: u32,
    },
    /// A trusted native holder's scalar extent disagreed with its view.
    WrongEncodedLength {
        /// Extent sealed by the foreground terminal.
        expected: usize,
        /// Extent derived from the background holder view.
        actual: usize,
    },
    /// The exact native holder generation was absent or malformed.
    NativeHolderUnavailable {
        /// Dense sequence whose holder could not be viewed.
        sequence: u64,
    },
    /// The encoded value exceeds its configured bound.
    RecordTooLarge {
        /// Actual or required encoded byte count.
        size: usize,
        /// Configured maximum byte count.
        max: usize,
    },
    /// The mutation count does not fit the on-disk field.
    TooManyMutations(usize),
    /// A key length does not fit the on-disk field.
    KeyTooLarge(usize),
    /// A value length does not fit the on-disk field.
    ValueTooLarge(usize),
    /// Length arithmetic overflowed the host address space.
    LengthOverflow,
    /// Memory needed to own the sealed record could not be reserved.
    AllocationFailed,
    /// Input ended before a declared field or required checksum was complete.
    Truncated,
    /// The record magic does not identify this format.
    BadMagic,
    /// The format version is not supported by this reader.
    UnsupportedVersion(u16),
    /// The stored and calculated CRC32C values differ.
    BadChecksum {
        /// CRC32C stored at the end of the record.
        expected: u32,
        /// CRC32C calculated from the preceding bytes.
        actual: u32,
    },
    /// An operation tag is not defined by this version.
    UnknownTag(u8),
    /// A delete encoded a nonzero value length.
    DeleteWithValue(usize),
    /// More bytes appeared before the checksum than the operation count owns.
    TrailingBytes(usize),
    /// Two operations target the same logical table and raw key.
    DuplicateMutation {
        /// Table containing the duplicate key.
        table_id: u64,
    },
}

impl fmt::Display for RecordError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidSequence => write!(f, "commit sequence zero is reserved"),
            Self::InvalidMakoTimestamp => write!(f, "invalid Mako base timestamp"),
            Self::ForeignKey => write!(f, "backend key is not an exact mako-cache log key"),
            Self::WrongSequence { key, record } => write!(
                f,
                "log-key sequence {key} does not match record sequence {record}"
            ),
            Self::WrongMakoTimestamp { expected, record } => write!(
                f,
                "bound Mako timestamp {expected} does not match record timestamp {record}"
            ),
            Self::WrongEncodedLength { expected, actual } => write!(
                f,
                "trusted holder record extent {expected} does not match derived extent {actual}"
            ),
            Self::NativeHolderUnavailable { sequence } => {
                write!(
                    f,
                    "native one-Put holder for sequence {sequence} is unavailable"
                )
            }
            Self::RecordTooLarge { size, max } => {
                write!(f, "commit record is {size} bytes; maximum is {max}")
            }
            Self::TooManyMutations(count) => {
                write!(f, "mutation count {count} exceeds the u32 format limit")
            }
            Self::KeyTooLarge(size) => {
                write!(f, "key length {size} exceeds the u32 format limit")
            }
            Self::ValueTooLarge(size) => {
                write!(f, "value length {size} exceeds the u32 format limit")
            }
            Self::LengthOverflow => write!(f, "commit-record length arithmetic overflow"),
            Self::AllocationFailed => write!(f, "could not allocate the owned commit record"),
            Self::Truncated => write!(f, "commit record is truncated"),
            Self::BadMagic => write!(f, "commit record has the wrong magic"),
            Self::UnsupportedVersion(version) => {
                write!(f, "unsupported commit-record version {version}")
            }
            Self::BadChecksum { expected, actual } => write!(
                f,
                "commit-record CRC32C mismatch: stored {expected:#010x}, calculated {actual:#010x}"
            ),
            Self::UnknownTag(tag) => write!(f, "unknown commit-record operation tag {tag}"),
            Self::DeleteWithValue(size) => {
                write!(f, "delete operation encodes a {size}-byte value")
            }
            Self::TrailingBytes(size) => {
                write!(f, "commit record has {size} unclaimed trailing bytes")
            }
            Self::DuplicateMutation { table_id } => {
                write!(f, "duplicate mutation in table {table_id}")
            }
        }
    }
}

impl std::error::Error for RecordError {}

#[derive(Clone, Copy)]
enum ParsedMutation<'a> {
    Put {
        table_id: u64,
        key: &'a [u8],
        value: &'a [u8],
    },
    Delete {
        table_id: u64,
        key: &'a [u8],
    },
}

impl<'a> ParsedMutation<'a> {
    fn table_id(self) -> u64 {
        match self {
            Self::Put { table_id, .. } | Self::Delete { table_id, .. } => table_id,
        }
    }

    fn key_bytes(self) -> &'a [u8] {
        match self {
            Self::Put { key, .. } | Self::Delete { key, .. } => key,
        }
    }

    fn to_owned(self) -> Result<Mutation, RecordError> {
        match self {
            Self::Put {
                table_id,
                key,
                value,
            } => Ok(Mutation::Put {
                table_id,
                key: copy_bytes(key)?,
                value: copy_bytes(value)?,
            }),
            Self::Delete { table_id, key } => Ok(Mutation::Delete {
                table_id,
                key: copy_bytes(key)?,
            }),
        }
    }
}

fn validate_unique_mutations(mutations: &[Mutation]) -> Result<(), RecordError> {
    let mut seen: HashSet<(u64, &[u8])> = HashSet::new();
    seen.try_reserve(mutations.len())
        .map_err(|_| RecordError::AllocationFailed)?;
    for mutation in mutations {
        if !seen.insert((mutation.table_id(), mutation.key())) {
            return Err(RecordError::DuplicateMutation {
                table_id: mutation.table_id(),
            });
        }
    }
    Ok(())
}

fn validate_unique_parsed(mutations: &[ParsedMutation<'_>]) -> Result<(), RecordError> {
    let mut seen: HashSet<(u64, &[u8])> = HashSet::new();
    seen.try_reserve(mutations.len())
        .map_err(|_| RecordError::AllocationFailed)?;
    for mutation in mutations {
        if !seen.insert((mutation.table_id(), mutation.key_bytes())) {
            return Err(RecordError::DuplicateMutation {
                table_id: mutation.table_id(),
            });
        }
    }
    Ok(())
}

fn encoded_len(mutations: &[Mutation]) -> Result<usize, RecordError> {
    let count = mutations.len();
    if u32::try_from(count).is_err() {
        return Err(RecordError::TooManyMutations(count));
    }

    let mut total = MIN_RECORD_LEN;
    for mutation in mutations {
        let key_len = mutation.key().len();
        if u32::try_from(key_len).is_err() {
            return Err(RecordError::KeyTooLarge(key_len));
        }
        let value_len = mutation.value().map_or(0, <[u8]>::len);
        if u32::try_from(value_len).is_err() {
            return Err(RecordError::ValueTooLarge(value_len));
        }
        total = total
            .checked_add(OP_HEADER_LEN)
            .and_then(|n| n.checked_add(key_len))
            .and_then(|n| n.checked_add(value_len))
            .ok_or(RecordError::LengthOverflow)?;
    }
    Ok(total)
}

fn encode_prepared(mutations: &[Mutation], encoded_len: usize) -> Result<Vec<u8>, RecordError> {
    let mut encoded = Vec::new();
    encoded
        .try_reserve_exact(encoded_len)
        .map_err(|_| RecordError::AllocationFailed)?;
    encoded.extend_from_slice(CRC32C_MAGIC);
    encoded.extend_from_slice(&FORMAT_VERSION.to_be_bytes());
    encoded.extend_from_slice(&0_u64.to_be_bytes());
    encoded.extend_from_slice(&0_u32.to_be_bytes());
    encoded.extend_from_slice(&(mutations.len() as u32).to_be_bytes());

    for mutation in mutations {
        match mutation {
            Mutation::Put {
                table_id,
                key,
                value,
            } => {
                encoded.push(PUT_TAG);
                encoded.extend_from_slice(&table_id.to_be_bytes());
                encoded.extend_from_slice(&(key.len() as u32).to_be_bytes());
                encoded.extend_from_slice(&(value.len() as u32).to_be_bytes());
                encoded.extend_from_slice(key);
                encoded.extend_from_slice(value);
            }
            Mutation::Delete { table_id, key } => {
                encoded.push(DELETE_TAG);
                encoded.extend_from_slice(&table_id.to_be_bytes());
                encoded.extend_from_slice(&(key.len() as u32).to_be_bytes());
                encoded.extend_from_slice(&0_u32.to_be_bytes());
                encoded.extend_from_slice(key);
            }
        }
    }

    // `BoundCommitRecord::finalize` overwrites this placeholder after the
    // sequence and Mako timestamp have been bound.
    encoded.extend_from_slice(&0_u32.to_be_bytes());
    debug_assert_eq!(encoded.len(), encoded_len);
    Ok(encoded)
}

fn make_log_key(sequence: CommitSeq) -> Result<Vec<u8>, RecordError> {
    let mut key = make_unbound_log_key()?;
    key[LOG_KEY_PREFIX.len()..].copy_from_slice(&sequence.get().to_be_bytes());
    Ok(key)
}

fn make_unbound_log_key() -> Result<Vec<u8>, RecordError> {
    let len = LOG_KEY_PREFIX
        .len()
        .checked_add(8)
        .ok_or(RecordError::LengthOverflow)?;
    let mut key = Vec::new();
    key.try_reserve_exact(len)
        .map_err(|_| RecordError::AllocationFailed)?;
    key.extend_from_slice(LOG_KEY_PREFIX);
    key.extend_from_slice(&0_u64.to_be_bytes());
    Ok(key)
}

fn make_data_keys(mutations: &[Mutation]) -> Result<Vec<Vec<u8>>, RecordError> {
    let mut keys = Vec::new();
    keys.try_reserve_exact(mutations.len())
        .map_err(|_| RecordError::AllocationFailed)?;
    for mutation in mutations {
        let raw = mutation.key();
        let len = DATA_KEY_PREFIX
            .len()
            .checked_add(8)
            .and_then(|n| n.checked_add(raw.len()))
            .ok_or(RecordError::LengthOverflow)?;
        let mut key = Vec::new();
        key.try_reserve_exact(len)
            .map_err(|_| RecordError::AllocationFailed)?;
        key.extend_from_slice(DATA_KEY_PREFIX);
        key.extend_from_slice(&mutation.table_id().to_be_bytes());
        key.extend_from_slice(raw);
        keys.push(key);
    }
    Ok(keys)
}

fn copy_bytes(bytes: &[u8]) -> Result<Vec<u8>, RecordError> {
    let mut copy = Vec::new();
    copy.try_reserve_exact(bytes.len())
        .map_err(|_| RecordError::AllocationFailed)?;
    copy.extend_from_slice(bytes);
    Ok(copy)
}

struct Cursor<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Cursor<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn remaining(&self) -> usize {
        self.bytes.len() - self.offset
    }

    fn take(&mut self, len: usize) -> Result<&'a [u8], RecordError> {
        let end = self.offset.checked_add(len).ok_or(RecordError::Truncated)?;
        let bytes = self
            .bytes
            .get(self.offset..end)
            .ok_or(RecordError::Truncated)?;
        self.offset = end;
        Ok(bytes)
    }

    fn read_u8(&mut self) -> Result<u8, RecordError> {
        Ok(self.take(1)?[0])
    }

    fn read_u16(&mut self) -> Result<u16, RecordError> {
        Ok(u16::from_be_bytes(
            self.take(2)?.try_into().expect("length checked"),
        ))
    }

    fn read_u32(&mut self) -> Result<u32, RecordError> {
        Ok(u32::from_be_bytes(
            self.take(4)?.try_into().expect("length checked"),
        ))
    }

    fn read_u64(&mut self) -> Result<u64, RecordError> {
        Ok(u64::from_be_bytes(
            self.take(8)?.try_into().expect("length checked"),
        ))
    }
}

/// CRC-32C (Castagnoli), in its standard reflected representation.
fn crc32c(bytes: &[u8]) -> u32 {
    let mut crc = !0_u32;
    for &byte in bytes {
        let index = ((crc ^ u32::from(byte)) & 0xff) as usize;
        crc = CRC32C_TABLE[index] ^ (crc >> 8);
    }
    !crc
}

const CRC32C_TABLE: [u32; 256] = make_crc32c_table();

const fn make_crc32c_table() -> [u32; 256] {
    const REVERSED_CASTAGNOLI: u32 = 0x82f6_3b78;
    let mut table = [0_u32; 256];
    let mut index = 0usize;
    while index < table.len() {
        let mut value = index as u32;
        let mut bit = 0;
        while bit < 8 {
            let low_bit_mask = (value & 1).wrapping_neg();
            value = (value >> 1) ^ (REVERSED_CASTAGNOLI & low_bit_mask);
            bit += 1;
        }
        table[index] = value;
        index += 1;
    }
    table
}

#[cfg(test)]
mod tests {
    use super::*;
    use mako_local::MAX_MAKO_TIMESTAMP;

    fn seq(raw: u64) -> CommitSeq {
        CommitSeq::new(raw).expect("test sequence must be nonzero")
    }

    fn mako_timestamp(raw: u32) -> MakoTimestamp {
        MakoTimestamp::new(raw).expect("test Mako timestamp must be nonzero")
    }

    fn refresh_checksum(bytes: &mut [u8]) {
        let checksum_offset = bytes.len() - CRC_LEN;
        let checksum = crc32c(&bytes[..checksum_offset]);
        bytes[checksum_offset..].copy_from_slice(&checksum.to_be_bytes());
    }

    fn seal_at(sequence: u64, mako_timestamp_raw: u32, mutations: Vec<Mutation>) -> CommitRecord {
        PreparedCommitRecord::prepare(mutations, 16 * 1024)
            .unwrap()
            .bind(seq(sequence), mako_timestamp(mako_timestamp_raw))
            .finalize()
    }

    fn seal(sequence: u64, mutations: Vec<Mutation>) -> CommitRecord {
        seal_at(sequence, 1_000, mutations)
    }

    #[test]
    fn deferred_holder_materializes_canonical_unchecked_one_put() {
        let key = b"key";
        let value = b"value\0bytes";
        let exact = HEADER_LEN + OP_HEADER_LEN + key.len() + value.len();
        let deferred = DeferredOnePutRecord::new(seq(41), mako_timestamp(9_001), exact);
        let record = deferred.materialize(7, key, value, 16 * 1024).unwrap();

        assert_eq!(record.sequence(), seq(41));
        assert_eq!(record.mako_timestamp(), mako_timestamp(9_001));
        assert_eq!(&record.encoded()[..UNCHECKED_MAGIC.len()], UNCHECKED_MAGIC);
        assert_eq!(
            u16::from_be_bytes(
                record.encoded()[VERSION_OFFSET..SEQUENCE_OFFSET]
                    .try_into()
                    .unwrap()
            ),
            UNCHECKED_FORMAT_VERSION
        );
        assert_eq!(
            record.mutations(),
            &[Mutation::Put {
                table_id: 7,
                key: key.to_vec(),
                value: value.to_vec(),
            }]
        );
        assert_eq!(
            CommitRecord::decode(record.log_key(), record.encoded(), 16 * 1024).unwrap(),
            record
        );

        let queued = QueuedCommitRecord::Holder(deferred);
        assert_eq!(queued.sequence(), seq(41));
        assert_eq!(queued.encoded_len(), exact);
        assert!(matches!(
            queued.into_recycled_native(),
            Some(RecycledNativeRecord::Holder(sequence)) if sequence == seq(41)
        ));
    }

    #[test]
    fn deferred_holder_rejects_a_mismatched_scalar_extent() {
        let deferred =
            DeferredOnePutRecord::new(seq(7), mako_timestamp(8), HEADER_LEN + OP_HEADER_LEN + 3);
        assert_eq!(
            deferred.materialize(1, b"key", b"value", 1024),
            Err(RecordError::WrongEncodedLength {
                expected: HEADER_LEN + OP_HEADER_LEN + 3,
                actual: HEADER_LEN + OP_HEADER_LEN + 3 + 5,
            })
        );
    }

    fn unchecked_v4_bytes(record: &CommitRecord) -> Vec<u8> {
        let mut encoded = record.encoded()[..record.encoded().len() - CRC_LEN].to_vec();
        encoded[..UNCHECKED_MAGIC.len()].copy_from_slice(UNCHECKED_MAGIC);
        encoded[VERSION_OFFSET..SEQUENCE_OFFSET]
            .copy_from_slice(&UNCHECKED_FORMAT_VERSION.to_be_bytes());
        encoded
    }

    #[test]
    fn crc32c_matches_standard_check_vector() {
        assert_eq!(crc32c(b"123456789"), 0xe306_9283);
        assert_eq!(crc32c(b""), 0);
    }

    #[test]
    fn binary_empty_and_delete_round_trip_and_batch() {
        let mutations = vec![
            Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: vec![0, 0xff, b'k'],
                value: Vec::new(),
            },
            Mutation::Put {
                table_id: 9,
                key: Vec::new(),
                value: vec![0, 1, 0xff, 0],
            },
            Mutation::Delete {
                table_id: 2,
                key: vec![b'x', 0, b'y'],
            },
        ];
        let record = seal_at(42, 0x1234_5678, mutations.clone());
        let decoded = CommitRecord::decode(record.log_key(), record.encoded(), 16 * 1024).unwrap();

        assert_eq!(decoded.sequence(), seq(42));
        assert_eq!(decoded.mako_timestamp(), mako_timestamp(0x1234_5678));
        assert_eq!(decoded.mutations(), mutations.as_slice());
        assert_eq!(decoded.encoded(), record.encoded());
        assert_eq!(decoded.log_key(), record.log_key());

        let ops = decoded.backend_ops();
        assert_eq!(ops.len(), 4);
        match &ops[0] {
            BlobOp::Put { key, val } => {
                assert_eq!(*key, decoded.log_key());
                assert_eq!(*val, decoded.encoded());
            }
            BlobOp::Delete { .. } => panic!("the first operation must persist the log record"),
        }
        match &ops[1] {
            BlobOp::Put { key, val } => {
                assert_eq!(*val, &[]);
                assert_eq!(
                    classify_backend_key(key),
                    BackendKey::Data {
                        table_id: DEFAULT_TABLE_ID,
                        key: &[0, 0xff, b'k']
                    }
                );
            }
            BlobOp::Delete { .. } => panic!("put became delete"),
        }
        match &ops[2] {
            BlobOp::Put { key, val } => {
                assert_eq!(*val, &[0, 1, 0xff, 0]);
                assert_eq!(
                    classify_backend_key(key),
                    BackendKey::Data {
                        table_id: 9,
                        key: &[]
                    }
                );
            }
            BlobOp::Delete { .. } => panic!("put became delete"),
        }
        match &ops[3] {
            BlobOp::Delete { key } => assert_eq!(
                classify_backend_key(key),
                BackendKey::Data {
                    table_id: 2,
                    key: &[b'x', 0, b'y']
                }
            ),
            BlobOp::Put { .. } => panic!("delete became put"),
        }
    }

    #[test]
    fn native_record_defers_materialization_without_changing_v3_bytes() {
        let mutations = vec![
            Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"native\0key".to_vec(),
                value: vec![0, 0xff, 7],
            },
            Mutation::Delete {
                table_id: 9,
                key: b"gone".to_vec(),
            },
        ];
        let expected = seal_at(73, 91, mutations.clone());
        let native = NativeCommitRecord::from_native(
            seq(73),
            mako_timestamp(91),
            expected.encoded().to_vec(),
        );
        let queued = QueuedCommitRecord::Native(native);

        assert_eq!(queued.sequence(), seq(73));
        assert_eq!(queued.mako_timestamp(), mako_timestamp(91));
        assert_eq!(queued.encoded_len(), expected.encoded().len());

        let replay = queued.materialize(16 * 1024).unwrap();
        assert_eq!(replay.sequence(), seq(73));
        assert_eq!(replay.mako_timestamp(), mako_timestamp(91));
        assert_eq!(replay.mutations(), mutations.as_slice());
        assert_eq!(replay.encoded(), expected.encoded());
    }

    #[test]
    fn unchecked_v4_is_self_describing_and_replays_beside_v3() {
        let expected = seal_at(
            74,
            92,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"unchecked".to_vec(),
                value: b"value".to_vec(),
            }],
        );
        let unchecked = unchecked_v4_bytes(&expected);
        assert_eq!(unchecked.len() + CRC_LEN, expected.encoded().len());

        let native =
            NativeCommitRecord::from_native(seq(74), mako_timestamp(92), unchecked.clone());
        let replay = native.materialize(16 * 1024).unwrap();
        assert_eq!(replay.mutations(), expected.mutations());
        assert_eq!(replay.encoded(), unchecked);

        // This is the intentional tradeoff: a structurally valid payload bit
        // change cannot be detected when the producer explicitly selected v4.
        let mut corrupted = unchecked;
        *corrupted.last_mut().expect("one-byte value") ^= 1;
        let decoded = CommitRecord::decode(expected.log_key(), &corrupted, 16 * 1024).unwrap();
        assert_ne!(decoded.mutations(), expected.mutations());
    }

    #[test]
    fn integrity_mode_magic_blocks_a_structurally_valid_downgrade() {
        let record = seal(
            75,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"mode".to_vec(),
                value: b"checked".to_vec(),
            }],
        );
        let mut forged = record.encoded().to_vec();
        forged[VERSION_OFFSET..SEQUENCE_OFFSET]
            .copy_from_slice(&UNCHECKED_FORMAT_VERSION.to_be_bytes());
        // Without the distinct magic, increasing this one-PUT value length by
        // four made the old decoder accept the former CRC trailer as ordinary
        // unchecked payload. Exercise that complete, structurally valid
        // downgrade rather than merely leaving four unclaimed bytes.
        let value_len_offset = HEADER_LEN + 1 + 8 + 4;
        let old_value_len = u32::from_be_bytes(
            forged[value_len_offset..value_len_offset + 4]
                .try_into()
                .unwrap(),
        );
        forged[value_len_offset..value_len_offset + 4]
            .copy_from_slice(&(old_value_len + CRC_LEN as u32).to_be_bytes());
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &forged, 16 * 1024),
            Err(RecordError::BadMagic)
        ));
    }

    #[test]
    fn arena_record_timestamp_mismatch_fails_during_background_materialization() {
        let expected = seal_at(
            76,
            94,
            vec![Mutation::Put {
                table_id: DEFAULT_TABLE_ID,
                key: b"timestamp".to_vec(),
                value: b"mismatch".to_vec(),
            }],
        );
        let mut unchecked = unchecked_v4_bytes(&expected);
        unchecked[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET].copy_from_slice(&95_u32.to_be_bytes());
        let bytes = NonNull::new(unchecked.as_mut_ptr()).unwrap();
        // SAFETY: this deliberately violates only the canonical-timestamp
        // portion of the private producer contract to prove the off-ACK
        // fail-stop validation remains active in release builds. The Vec keeps
        // the initialized extent live and stable through materialization.
        let native = unsafe {
            NativeCommitRecord::from_native_arena(
                seq(76),
                mako_timestamp(94),
                bytes,
                unchecked.len(),
                0,
            )
        };
        assert!(matches!(
            native.materialize(16 * 1024),
            Err(RecordError::WrongMakoTimestamp {
                expected: 94,
                record: 95,
            })
        ));
    }

    #[test]
    fn every_truncation_and_payload_corruption_is_rejected() {
        let record = seal(
            7,
            vec![Mutation::Put {
                table_id: 1,
                key: b"binary\0key".to_vec(),
                value: b"binary\0value".to_vec(),
            }],
        );

        for end in 0..record.encoded().len() {
            assert!(
                CommitRecord::decode(record.log_key(), &record.encoded()[..end], 16 * 1024)
                    .is_err(),
                "accepted truncation at byte {end}"
            );
        }

        let mut corrupt = record.encoded().to_vec();
        corrupt[HEADER_LEN + OP_HEADER_LEN] ^= 0x80;
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &corrupt, 16 * 1024),
            Err(RecordError::BadChecksum { .. })
        ));

        let mut corrupt_mako_timestamp = record.encoded().to_vec();
        corrupt_mako_timestamp[MAKO_TIMESTAMP_OFFSET] ^= 0x80;
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &corrupt_mako_timestamp, 16 * 1024),
            Err(RecordError::BadChecksum { .. })
        ));
    }

    #[test]
    fn duplicate_targets_are_rejected_on_prepare_and_decode() {
        let duplicates = vec![
            Mutation::Put {
                table_id: 4,
                key: b"same".to_vec(),
                value: b"first".to_vec(),
            },
            Mutation::Delete {
                table_id: 4,
                key: b"same".to_vec(),
            },
        ];
        assert!(matches!(
            PreparedCommitRecord::prepare(duplicates.clone(), 4096),
            Err(RecordError::DuplicateMutation { table_id: 4 })
        ));

        // The low-level encoder assumes validation, which lets this test
        // model a malicious on-disk record that did not pass through
        // `prepare`.
        let len = encoded_len(&duplicates).unwrap();
        let mut encoded = encode_prepared(&duplicates, len).unwrap();
        encoded[SEQUENCE_OFFSET..MAKO_TIMESTAMP_OFFSET].copy_from_slice(&1_u64.to_be_bytes());
        encoded[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET].copy_from_slice(&77_u32.to_be_bytes());
        refresh_checksum(&mut encoded);
        let key = make_log_key(seq(1)).unwrap();
        assert!(matches!(
            CommitRecord::decode(&key, &encoded, 4096),
            Err(RecordError::DuplicateMutation { table_id: 4 })
        ));

        // The same raw key in a different table remains a distinct target.
        let distinct_tables = vec![
            Mutation::Delete {
                table_id: 4,
                key: b"same".to_vec(),
            },
            Mutation::Delete {
                table_id: 5,
                key: b"same".to_vec(),
            },
        ];
        assert!(PreparedCommitRecord::prepare(distinct_tables, 4096).is_ok());
    }

    #[test]
    fn exact_key_sequence_magic_version_tag_and_trailing_bytes_are_checked() {
        let record = seal(
            11,
            vec![Mutation::Put {
                table_id: 1,
                key: b"k".to_vec(),
                value: b"v".to_vec(),
            }],
        );

        let wrong_key = make_log_key(seq(12)).unwrap();
        assert_eq!(
            CommitRecord::decode(&wrong_key, record.encoded(), 4096),
            Err(RecordError::WrongSequence {
                key: 12,
                record: 11
            })
        );

        let mut key_with_suffix = record.log_key().to_vec();
        key_with_suffix.push(0);
        assert!(matches!(
            CommitRecord::decode(&key_with_suffix, record.encoded(), 4096),
            Err(RecordError::ForeignKey)
        ));
        assert!(matches!(
            CommitRecord::decode(b"legacy raw key", record.encoded(), 4096),
            Err(RecordError::ForeignKey)
        ));
        assert!(matches!(
            CommitRecord::decode(&record.data_keys[0], record.encoded(), 4096),
            Err(RecordError::ForeignKey)
        ));

        let mut bad_magic = record.encoded().to_vec();
        bad_magic[0] ^= 1;
        refresh_checksum(&mut bad_magic);
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &bad_magic, 4096),
            Err(RecordError::BadMagic)
        ));

        let mut bad_version = record.encoded().to_vec();
        let version_offset = CRC32C_MAGIC.len();
        bad_version[version_offset..version_offset + 2].copy_from_slice(&5_u16.to_be_bytes());
        refresh_checksum(&mut bad_version);
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &bad_version, 4096),
            Err(RecordError::UnsupportedVersion(5))
        ));

        let mut legacy_silo_version = record.encoded().to_vec();
        legacy_silo_version[version_offset..version_offset + 2]
            .copy_from_slice(&2_u16.to_be_bytes());
        refresh_checksum(&mut legacy_silo_version);
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &legacy_silo_version, 4096),
            Err(RecordError::UnsupportedVersion(2))
        ));

        let mut zero_mako_timestamp = record.encoded().to_vec();
        zero_mako_timestamp[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET]
            .copy_from_slice(&0_u32.to_be_bytes());
        refresh_checksum(&mut zero_mako_timestamp);
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &zero_mako_timestamp, 4096),
            Err(RecordError::InvalidMakoTimestamp)
        ));

        let mut oversized_mako_timestamp = record.encoded().to_vec();
        oversized_mako_timestamp[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET]
            .copy_from_slice(&(MAX_MAKO_TIMESTAMP + 1).to_be_bytes());
        refresh_checksum(&mut oversized_mako_timestamp);
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &oversized_mako_timestamp, 4096),
            Err(RecordError::InvalidMakoTimestamp)
        ));

        let mut bad_tag = record.encoded().to_vec();
        bad_tag[HEADER_LEN] = 0xff;
        refresh_checksum(&mut bad_tag);
        assert!(matches!(
            CommitRecord::decode(record.log_key(), &bad_tag, 4096),
            Err(RecordError::UnknownTag(0xff))
        ));

        let mut trailing = record.encoded()[..record.encoded().len() - CRC_LEN].to_vec();
        trailing.push(0xaa);
        let checksum = crc32c(&trailing);
        trailing.extend_from_slice(&checksum.to_be_bytes());
        assert_eq!(
            CommitRecord::decode(record.log_key(), &trailing, 4096),
            Err(RecordError::TrailingBytes(1))
        );
    }

    #[test]
    fn key_mapping_is_injective_and_classifier_is_strict() {
        let a = seal(
            0x0102_0304_0506_0708,
            vec![Mutation::Delete {
                table_id: 1,
                key: vec![0, 1],
            }],
        );
        let b = seal(
            0x0102_0304_0506_0709,
            vec![Mutation::Delete {
                table_id: 0x0100,
                key: Vec::new(),
            }],
        );
        let c = seal(
            3,
            vec![Mutation::Delete {
                table_id: 2,
                key: vec![0, 1],
            }],
        );

        assert_ne!(a.log_key(), b.log_key());
        assert_ne!(a.log_key(), &a.data_keys[0]);
        assert_ne!(a.data_keys[0], b.data_keys[0]);
        assert_ne!(a.data_keys[0], c.data_keys[0]);
        assert_eq!(
            classify_backend_key(a.log_key()),
            BackendKey::Log(seq(0x0102_0304_0506_0708))
        );
        assert_eq!(
            classify_backend_key(&a.data_keys[0]),
            BackendKey::Data {
                table_id: 1,
                key: &[0, 1]
            }
        );

        let mut zero_log = LOG_KEY_PREFIX.to_vec();
        zero_log.extend_from_slice(&0_u64.to_be_bytes());
        assert_eq!(classify_backend_key(&zero_log), BackendKey::Foreign);

        let mut short_data = DATA_KEY_PREFIX.to_vec();
        short_data.extend_from_slice(&[0; 7]);
        assert_eq!(classify_backend_key(&short_data), BackendKey::Foreign);
        assert_eq!(classify_backend_key(b""), BackendKey::Foreign);
        assert_eq!(classify_backend_key(b"unprefixed"), BackendKey::Foreign);
    }

    #[test]
    fn max_size_includes_header_operations_and_checksum() {
        let mutations = vec![Mutation::Put {
            table_id: 1,
            key: b"key".to_vec(),
            value: b"value".to_vec(),
        }];
        let exact = encoded_len(&mutations).unwrap();

        let record = PreparedCommitRecord::prepare(mutations.clone(), exact)
            .unwrap()
            .bind(seq(1), mako_timestamp(9))
            .finalize();
        assert_eq!(record.encoded().len(), exact);
        assert_eq!(
            PreparedCommitRecord::prepare(mutations, exact - 1),
            Err(RecordError::RecordTooLarge {
                size: exact,
                max: exact - 1
            })
        );
        assert_eq!(
            CommitRecord::decode(record.log_key(), record.encoded(), exact - 1),
            Err(RecordError::RecordTooLarge {
                size: exact,
                max: exact - 1
            })
        );
    }

    #[test]
    fn empty_transaction_record_is_canonical() {
        let record = PreparedCommitRecord::prepare(Vec::new(), MIN_RECORD_LEN)
            .unwrap()
            .bind(seq(1), mako_timestamp(1))
            .finalize();
        assert_eq!(record.encoded().len(), MIN_RECORD_LEN);
        assert_eq!(record.mako_timestamp(), mako_timestamp(1));
        assert_eq!(record.backend_ops().len(), 1);
        assert_eq!(
            CommitRecord::decode(record.log_key(), record.encoded(), MIN_RECORD_LEN).unwrap(),
            record
        );
    }

    #[test]
    fn bind_and_finalize_reuse_every_preallocated_buffer() {
        let prepared = PreparedCommitRecord::prepare(
            vec![Mutation::Put {
                table_id: 3,
                key: b"key".to_vec(),
                value: b"value".to_vec(),
            }],
            4096,
        )
        .unwrap();

        let encoded_ptr = prepared.encoded.as_ptr();
        let encoded_capacity = prepared.encoded.capacity();
        let log_key_ptr = prepared.log_key.as_ptr();
        let log_key_capacity = prepared.log_key.capacity();
        let data_key_ptr = prepared.data_keys[0].as_ptr();
        let data_key_capacity = prepared.data_keys[0].capacity();

        assert_eq!(
            &prepared.encoded[SEQUENCE_OFFSET..MAKO_TIMESTAMP_OFFSET],
            &0_u64.to_be_bytes()
        );
        assert_eq!(
            &prepared.encoded[MAKO_TIMESTAMP_OFFSET..OP_COUNT_OFFSET],
            &0_u32.to_be_bytes()
        );

        let bound = prepared.bind(seq(55), mako_timestamp(MAX_MAKO_TIMESTAMP));
        assert_eq!(bound.encoded.as_ptr(), encoded_ptr);
        assert_eq!(bound.encoded.capacity(), encoded_capacity);
        assert_eq!(bound.log_key.as_ptr(), log_key_ptr);
        assert_eq!(bound.log_key.capacity(), log_key_capacity);
        assert_eq!(bound.data_keys[0].as_ptr(), data_key_ptr);
        assert_eq!(bound.data_keys[0].capacity(), data_key_capacity);

        let record = bound.finalize();
        assert_eq!(record.encoded.as_ptr(), encoded_ptr);
        assert_eq!(record.encoded.capacity(), encoded_capacity);
        assert_eq!(record.log_key.as_ptr(), log_key_ptr);
        assert_eq!(record.log_key.capacity(), log_key_capacity);
        assert_eq!(record.data_keys[0].as_ptr(), data_key_ptr);
        assert_eq!(record.data_keys[0].capacity(), data_key_capacity);
        assert_eq!(record.sequence(), seq(55));
        assert_eq!(record.mako_timestamp(), mako_timestamp(MAX_MAKO_TIMESTAMP));
        assert_eq!(
            CommitRecord::decode(record.log_key(), record.encoded(), 4096).unwrap(),
            record
        );
    }
}
