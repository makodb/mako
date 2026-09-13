//! Zero-copy one-put holders and the stable control blocks lent to the
//! trusted terminals.
//!
//! The pool mirrors `mako_rust_fast_one_put_holder_pool` in
//! `mako_local_abi.cc`: a power-of-two ring of holders where lane sequence
//! `N` exclusively leases holder `(N - 1) & mask`. Fields are deliberately
//! non-atomic. The cache's publication protocol (Release of the queue frontier
//! or exact cell turn after sealing; Acquire before viewing; release before
//! the applied frontier advances) orders every access to one holder.

use std::cell::UnsafeCell;
use std::fmt;
use std::num::{NonZeroU32, NonZeroU64};
use std::ptr::NonNull;

use mako_engine_api::{Error, MakoTimestamp, Result, MAX_KEY_BYTES, MAX_VALUE_BYTES};

pub(crate) const INLINE_KEY_BYTES: usize = 32;

/// One holder generation.
pub(crate) struct Holder {
    sealed: bool,
    key_len: u32,
    key_inline: [u8; INLINE_KEY_BYTES],
    key_overflow: Vec<u8>,
    /// Raw application value; the allocation is retained across generations.
    value: Vec<u8>,
    sequence: u64,
    table_id: u64,
    timestamp_stamp: u64,
}

impl Holder {
    const fn free() -> Self {
        Self {
            sealed: false,
            key_len: 0,
            key_inline: [0; INLINE_KEY_BYTES],
            key_overflow: Vec::new(),
            value: Vec::new(),
            sequence: 0,
            table_id: 0,
            timestamp_stamp: 0,
        }
    }

    pub(crate) fn is_free(&self) -> bool {
        !self.sealed && self.sequence == 0 && self.timestamp_stamp == 0
    }

    /// Stage the key before validation; allocation happens here, never in a
    /// lock-held commit callback.
    pub(crate) fn stage_key(&mut self, key: &[u8]) -> Result<()> {
        let len = u32::try_from(key.len()).map_err(|_| Error::ValueTooLarge)?;
        if key.len() <= INLINE_KEY_BYTES {
            self.key_inline[..key.len()].copy_from_slice(key);
        } else {
            self.key_overflow.clear();
            self.key_overflow
                .try_reserve_exact(key.len())
                .map_err(|_| Error::OutOfMemory)?;
            self.key_overflow.extend_from_slice(key);
        }
        self.key_len = len;
        Ok(())
    }

    /// Copy a short key into inline storage (no allocation).
    pub(crate) fn stage_inline_key(&mut self, key: &[u8]) {
        debug_assert!(key.len() <= INLINE_KEY_BYTES);
        self.key_inline[..key.len()].copy_from_slice(key);
        self.key_len = key.len() as u32;
    }

    /// Adopt a key prepared before commit (no allocation).
    pub(crate) fn adopt_overflow_key(&mut self, key: Vec<u8>) {
        debug_assert!(key.len() > INLINE_KEY_BYTES);
        self.key_len = key.len() as u32;
        self.key_overflow = key;
    }

    /// Transfer the accepted value and seal every scalar.
    pub(crate) fn seal(&mut self, value: &mut Vec<u8>, sequence: u64, table_id: u64, stamp: u64) {
        debug_assert!(!self.sealed);
        debug_assert!(sequence != 0);
        debug_assert!(mako_clock::valid_stamp(stamp));
        std::mem::swap(&mut self.value, value);
        self.sequence = sequence;
        self.table_id = table_id;
        self.timestamp_stamp = stamp;
        self.sealed = true;
    }

    fn key(&self) -> &[u8] {
        let len = self.key_len as usize;
        if len <= INLINE_KEY_BYTES {
            &self.key_inline[..len]
        } else {
            &self.key_overflow[..len]
        }
    }
}

/// Fixed-capacity storage for callback-free one-put write-back.
pub struct TrustedOnePutHolderPool {
    holders: Box<[UnsafeCell<Holder>]>,
    mask: usize,
    capacity: usize,
}

impl fmt::Debug for TrustedOnePutHolderPool {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TrustedOnePutHolderPool")
            .field("capacity", &self.capacity)
            .finish_non_exhaustive()
    }
}

// SAFETY: sharing the pool handle reads or writes no holder. Every holder
// access goes through the unsafe generation-ownership protocol documented on
// the terminal, view, and release methods.
unsafe impl Send for TrustedOnePutHolderPool {}
// SAFETY: as above.
unsafe impl Sync for TrustedOnePutHolderPool {}

impl TrustedOnePutHolderPool {
    /// Allocate an independent holder pool for one publication ring.
    ///
    /// `capacity` must be a nonzero power of two equal to the physical ring
    /// capacity. The reserve hints preallocate payload storage.
    pub fn new(capacity: usize, key_reserve_bytes: u32, value_reserve_bytes: u32) -> Result<Self> {
        if !capacity.is_power_of_two() || capacity == 0 {
            return Err(Error::InvalidArgument);
        }
        if key_reserve_bytes as usize > MAX_KEY_BYTES
            || value_reserve_bytes as usize > MAX_VALUE_BYTES
        {
            return Err(Error::ValueTooLarge);
        }
        let mut holders = Vec::new();
        holders
            .try_reserve_exact(capacity)
            .map_err(|_| Error::OutOfMemory)?;
        for _ in 0..capacity {
            let mut holder = Holder::free();
            if key_reserve_bytes as usize > INLINE_KEY_BYTES {
                holder
                    .key_overflow
                    .try_reserve_exact(key_reserve_bytes as usize)
                    .map_err(|_| Error::OutOfMemory)?;
            }
            if value_reserve_bytes != 0 {
                holder
                    .value
                    .try_reserve_exact(value_reserve_bytes as usize)
                    .map_err(|_| Error::OutOfMemory)?;
            }
            holders.push(UnsafeCell::new(holder));
        }
        Ok(Self {
            holders: holders.into_boxed_slice(),
            mask: capacity - 1,
            capacity,
        })
    }

    /// Number of generations addressable before a ring position repeats.
    pub const fn capacity(&self) -> usize {
        self.capacity
    }

    /// Process allocator origin of every holder timestamp.
    pub const fn timestamp_origin(&self) -> u32 {
        mako_clock::ORIGIN.get()
    }

    /// Raw holder for `sequence`. Ownership of the generation, not this masked
    /// lookup, keeps two sequences from aliasing one holder.
    #[inline(always)]
    pub(crate) fn holder_ptr(&self, sequence: u64) -> *mut Holder {
        debug_assert!(sequence != 0);
        self.holders[(sequence.wrapping_sub(1) as usize) & self.mask].get()
    }

    /// Borrow one sealed generation after queue publication.
    ///
    /// # Safety
    ///
    /// The caller must be the sole consumer of `expected_sequence`, must have
    /// Acquired its publication, and must not overlap another view or release
    /// of the same generation.
    pub unsafe fn get_view(
        &self,
        expected_sequence: NonZeroU64,
    ) -> Result<TrustedOnePutHolderView<'_>> {
        let pointer = self.holder_ptr(expected_sequence.get());
        // SAFETY: the caller's publication Acquire and sole-consumer ownership
        // make this shared read race-free.
        let holder = unsafe { &*pointer };
        if !holder.sealed || holder.sequence != expected_sequence.get() {
            return Err(Error::Busy);
        }
        let mako_timestamp = mako_clock::expand(holder.timestamp_stamp).ok_or(Error::Internal)?;
        if holder.key().len() > MAX_KEY_BYTES || holder.value.len() > MAX_VALUE_BYTES {
            return Err(Error::Internal);
        }
        Ok(TrustedOnePutHolderView {
            pool: self,
            sequence: expected_sequence,
            table_id: holder.table_id,
            mako_timestamp,
            key: NonNull::from(holder.key()),
            value: NonNull::from(holder.value.as_slice()),
        })
    }

    /// Compact spelling of [`Self::get_view`].
    ///
    /// # Safety
    ///
    /// As [`Self::get_view`].
    #[inline(always)]
    pub unsafe fn view(
        &self,
        expected_sequence: NonZeroU64,
    ) -> Result<TrustedOnePutHolderView<'_>> {
        // SAFETY: delegated unchanged.
        unsafe { self.get_view(expected_sequence) }
    }

    /// Release a generation after backend retirement.
    ///
    /// # Safety
    ///
    /// Every use of the view's bytes must be complete, and the caller must
    /// Release-publish the matching reuse frontier only after this returns.
    pub unsafe fn release(&self, view: TrustedOnePutHolderView<'_>) -> Result<()> {
        if !std::ptr::eq(self, view.pool) {
            return Err(Error::InvalidArgument);
        }
        let pointer = self.holder_ptr(view.sequence.get());
        // SAFETY: the consumer's exclusive ownership of this sealed generation
        // ends with this call; no producer can touch it before the frontier.
        let holder = unsafe { &mut *pointer };
        if !holder.sealed || holder.sequence != view.sequence.get() {
            return Err(Error::Busy);
        }
        holder.timestamp_stamp = 0;
        holder.sequence = 0;
        holder.sealed = false;
        Ok(())
    }
}

/// Exact-generation borrowed view of one sealed holder.
pub struct TrustedOnePutHolderView<'pool> {
    pool: &'pool TrustedOnePutHolderPool,
    sequence: NonZeroU64,
    table_id: u64,
    mako_timestamp: MakoTimestamp,
    key: NonNull<[u8]>,
    value: NonNull<[u8]>,
}

impl fmt::Debug for TrustedOnePutHolderView<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TrustedOnePutHolderView")
            .field("sequence", &self.sequence)
            .field("table_id", &self.table_id)
            .field("mako_timestamp", &self.mako_timestamp)
            .finish_non_exhaustive()
    }
}

impl TrustedOnePutHolderView<'_> {
    /// Lane-local sequence of this generation.
    pub const fn sequence(&self) -> NonZeroU64 {
        self.sequence
    }

    /// Stable logical table id.
    pub const fn table_id(&self) -> u64 {
        self.table_id
    }

    /// Accepted Mako timestamp.
    pub const fn mako_timestamp(&self) -> MakoTimestamp {
        self.mako_timestamp
    }

    /// Raw application key.
    pub fn key(&self) -> &[u8] {
        // SAFETY: the holder stays sealed and immutable for the view's life.
        unsafe { self.key.as_ref() }
    }

    /// Raw application value.
    pub fn value(&self) -> &[u8] {
        // SAFETY: as `key`.
        unsafe { self.value.as_ref() }
    }

    /// Release this generation after its backend batch succeeded.
    ///
    /// # Safety
    ///
    /// As [`TrustedOnePutHolderPool::release`].
    pub unsafe fn release_after_backend(self) -> Result<()> {
        let pool = self.pool;
        // SAFETY: delegated unchanged.
        unsafe { pool.release(self) }
    }
}

impl mako_engine_api::HolderView for TrustedOnePutHolderView<'_> {
    fn sequence(&self) -> NonZeroU64 {
        self.sequence
    }
    fn table_id(&self) -> u64 {
        self.table_id
    }
    fn mako_timestamp(&self) -> MakoTimestamp {
        self.mako_timestamp
    }
    fn key(&self) -> &[u8] {
        Self::key(self)
    }
    fn value(&self) -> &[u8] {
        Self::value(self)
    }
}

impl mako_engine_api::HolderPool for TrustedOnePutHolderPool {
    type Engine = crate::MakoStoEngine;
    type View<'pool>
        = TrustedOnePutHolderView<'pool>
    where
        Self: 'pool;

    fn new(capacity: usize, key_reserve_bytes: u32, value_reserve_bytes: u32) -> Result<Self> {
        Self::new(capacity, key_reserve_bytes, value_reserve_bytes)
    }
    fn capacity(&self) -> usize {
        self.capacity
    }
    unsafe fn view(&self, expected_sequence: NonZeroU64) -> Result<Self::View<'_>> {
        // SAFETY: forwarded unchanged.
        unsafe { self.get_view(expected_sequence) }
    }
    unsafe fn release(&self, view: Self::View<'_>) -> Result<()> {
        // SAFETY: forwarded unchanged.
        unsafe { Self::release(self, view) }
    }
}

/// Stable lane-wide inputs of the fused SPSC one-put terminal.
pub struct TrustedSpscOnePutHolderControl {
    pub(crate) pool: *const TrustedOnePutHolderPool,
    pub(crate) acknowledged: *mut u64,
    pub(crate) unhealthy: *const u8,
    pub(crate) capacity: u64,
    pub(crate) max_record_bytes: u32,
}

impl fmt::Debug for TrustedSpscOnePutHolderControl {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("TrustedSpscOnePutHolderControl")
            .field("capacity", &self.capacity)
            .field("max_record_bytes", &self.max_record_bytes)
            .finish_non_exhaustive()
    }
}

// SAFETY: sharing the control performs no access; the unsafe terminal
// contract serializes every use through the unique lane producer.
unsafe impl Send for TrustedSpscOnePutHolderControl {}
// SAFETY: as above.
unsafe impl Sync for TrustedSpscOnePutHolderControl {}

impl TrustedSpscOnePutHolderControl {
    /// Snapshot stable lane-wide inputs.
    ///
    /// # Safety
    ///
    /// `pool`, `acknowledged` (an aligned `AtomicU64`), and `unhealthy` (an
    /// `AtomicBool`) must stay live and stable until the last terminal call
    /// using this value returns.
    pub unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        acknowledged: *mut u64,
        unhealthy: *const u8,
        capacity: NonZeroU64,
        max_record_bytes: NonZeroU32,
    ) -> Self {
        debug_assert!(!acknowledged.is_null());
        debug_assert!(!unhealthy.is_null());
        debug_assert!(capacity.get() <= pool.capacity as u64);
        Self {
            pool,
            acknowledged,
            unhealthy,
            capacity: capacity.get(),
            max_record_bytes: max_record_bytes.get(),
        }
    }

    /// Maximum record extent supplied to the engine.
    pub const fn max_record_bytes(&self) -> u32 {
        self.max_record_bytes
    }
}

impl mako_engine_api::SpscHolderControl for TrustedSpscOnePutHolderControl {
    type Engine = crate::MakoStoEngine;

    unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        acknowledged: *mut u64,
        unhealthy: *const u8,
        capacity: NonZeroU64,
        max_record_bytes: NonZeroU32,
    ) -> Self {
        // SAFETY: forwarded unchanged.
        unsafe { Self::new(pool, acknowledged, unhealthy, capacity, max_record_bytes) }
    }
    fn max_record_bytes(&self) -> u32 {
        self.max_record_bytes
    }
}

/// Stable queue layout for the callback-free concurrent arena terminal.
#[derive(Debug, Clone, Copy)]
pub struct TrustedNativeOrderedArenaControl {
    pub(crate) next_bound: *mut u64,
    pub(crate) unhealthy: *const u8,
    pub(crate) publication_base: *mut u8,
    pub(crate) arena_base: *mut u8,
    pub(crate) publication_mask: usize,
    pub(crate) publication_shift: u32,
    pub(crate) publication_stride: u32,
    pub(crate) arena_stride: u32,
    pub(crate) arena_block_bytes: u32,
}

impl TrustedNativeOrderedArenaControl {
    /// Describe one stable publication ring and its matching record arena.
    ///
    /// # Safety
    ///
    /// Every pointer must stay live and stable through each terminal call;
    /// see the C ABI engine's documentation for the exact layout contract.
    #[allow(clippy::too_many_arguments)]
    pub const unsafe fn from_raw_parts(
        next_bound: *mut u64,
        unhealthy: *const u8,
        publication_base: *mut u8,
        arena_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        arena_stride: u32,
        arena_block_bytes: u32,
    ) -> Self {
        Self {
            next_bound,
            unhealthy,
            publication_base,
            arena_base,
            publication_mask,
            publication_shift,
            publication_stride,
            arena_stride,
            arena_block_bytes,
        }
    }
}

impl mako_engine_api::NativeOrderedArenaControl for TrustedNativeOrderedArenaControl {
    unsafe fn from_raw_parts(
        next_bound: *mut u64,
        unhealthy: *const u8,
        publication_base: *mut u8,
        arena_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        arena_stride: u32,
        arena_block_bytes: u32,
    ) -> Self {
        // SAFETY: forwarded unchanged.
        unsafe {
            Self::from_raw_parts(
                next_bound,
                unhealthy,
                publication_base,
                arena_base,
                publication_mask,
                publication_shift,
                publication_stride,
                arena_stride,
                arena_block_bytes,
            )
        }
    }
}

/// Stable queue layout and holder pool for the callback-free concurrent
/// holder terminal.
#[derive(Debug, Clone, Copy)]
pub struct TrustedNativeOrderedHolderControl {
    pub(crate) pool: *const TrustedOnePutHolderPool,
    pub(crate) unhealthy: *const u8,
    pub(crate) publication_base: *mut u8,
    pub(crate) publication_mask: usize,
    pub(crate) publication_shift: u32,
    pub(crate) publication_stride: u32,
    pub(crate) max_record_bytes: u32,
}

impl TrustedNativeOrderedHolderControl {
    /// Describe the publication layout and holder pool.
    ///
    /// # Safety
    ///
    /// Every pointer and `pool` must stay live and stable through each call,
    /// and the ring capacity must equal `pool.capacity()`.
    pub unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        unhealthy: *const u8,
        publication_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        max_record_bytes: u32,
    ) -> Self {
        debug_assert!(!unhealthy.is_null());
        debug_assert!(!publication_base.is_null());
        debug_assert_eq!(publication_mask + 1, pool.capacity());
        Self {
            pool,
            unhealthy,
            publication_base,
            publication_mask,
            publication_shift,
            publication_stride,
            max_record_bytes,
        }
    }
}

impl mako_engine_api::NativeOrderedHolderControl for TrustedNativeOrderedHolderControl {
    type Engine = crate::MakoStoEngine;

    unsafe fn new(
        pool: &TrustedOnePutHolderPool,
        unhealthy: *const u8,
        publication_base: *mut u8,
        publication_mask: usize,
        publication_shift: u32,
        publication_stride: u32,
        max_record_bytes: u32,
    ) -> Self {
        // SAFETY: forwarded unchanged.
        unsafe {
            Self::new(
                pool,
                unhealthy,
                publication_base,
                publication_mask,
                publication_shift,
                publication_stride,
                max_record_bytes,
            )
        }
    }
}
