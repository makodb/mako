//! Worker-affine transaction execution and the native STO commit protocol.

use std::{
    any::TypeId,
    hash::{Hash, Hasher},
    marker::PhantomData,
    num::NonZeroUsize,
    panic::{catch_unwind, AssertUnwindSafe},
    rc::Rc,
    sync::Arc,
};

use crate::{
    adapter::{FinishDisposition, ResourceKey, TerminalReadBatchCapability, TransactionalResource},
    error::{
        AbortInfo, AbortReason, AccessError, AcquireError, AdapterFault, AdapterPhase, BeginError,
        CheckError, CommitFailure, CommitInfo, CommitOutcome, DefiniteOutcome, FailurePhase,
        IndeterminateInfo, InternalError, InvalidUse, ItemInitError, PoisonInfo, PrepareError,
        Unsupported,
    },
    hook::{CommitHook, CommitHookError},
    item::{
        BatchFinishStage, Entry, ErasedItem, ErasedItemBatch, ItemBox, ItemData, TypedItemBatch,
    },
    lock::{
        FinishContext, LockDisposition, LockPlan, LockPlanStorage, PreflightFreeValidationContext,
    },
    runtime::{IsolationMode, RegisteredResource, Runtime, WorkerContext},
    terminal_read::{ErasedTerminalReadBatch, TerminalReadEntry, TypedTerminalReadBatch},
};

/// Marker for a general active transaction.
#[derive(Debug)]
pub struct Active;

/// Marker for a newly begun transaction restricted to one terminal read batch.
#[derive(Debug)]
pub struct TerminalReadOpen;

/// Marker for a complete terminal read batch that permits only commit or abort.
#[derive(Debug)]
pub struct TerminalReadReady;

/// Reusable open-addressed scratch for an exact hashed uniqueness proof.
///
/// Entries carry a proof generation, so a repeated call does not clear the
/// retained allocation. Hashes select candidate buckets only. Core compares
/// full keys with [`Eq`] before reporting a duplicate, so distinct keys remain
/// distinct even when every hash collides.
#[derive(Default)]
pub struct UniqueItemKeyIndex {
    entries: Vec<UniqueItemKeyIndexEntry>,
    generation: u32,
}

#[derive(Clone, Copy, Default)]
struct UniqueItemKeyIndexEntry {
    generation: u32,
    item_slot: usize,
}

impl UniqueItemKeyIndex {
    const MIN_BUCKETS: usize = 8;

    /// Creates empty uniqueness scratch.
    pub const fn new() -> Self {
        Self {
            entries: Vec::new(),
            generation: 0,
        }
    }

    /// Creates scratch sized for at least `key_capacity` keys.
    pub fn with_capacity(key_capacity: usize) -> Self {
        let mut index = Self::new();
        index
            .try_reserve_for_len(key_capacity)
            .unwrap_or_else(|_| panic!("unique-item-key index capacity is too large"));
        index
    }

    /// Returns the key count that the retained table can hold without growing.
    pub fn capacity(&self) -> usize {
        self.entries.len() / 2
    }

    /// Fallibly reserves enough buckets to prove `needed` keys unique.
    ///
    /// The table stays at or below one-half load. A failed growth leaves the
    /// prior allocation and every later proof usable.
    pub fn try_reserve_for_len(
        &mut self,
        needed: usize,
    ) -> Result<(), std::collections::TryReserveError> {
        if needed <= self.capacity() {
            return Ok(());
        }

        let minimum_buckets = needed.saturating_mul(2);
        let required_buckets = minimum_buckets
            .max(Self::MIN_BUCKETS)
            .checked_next_power_of_two()
            .unwrap_or(usize::MAX);
        let additional = required_buckets.saturating_sub(self.entries.len());
        self.entries.try_reserve_exact(additional)?;
        self.entries
            .resize(required_buckets, UniqueItemKeyIndexEntry::default());
        Ok(())
    }

    fn begin(&mut self, needed: usize) -> Result<(), std::collections::TryReserveError> {
        self.try_reserve_for_len(needed)?;
        self.generation = self.generation.wrapping_add(1);
        if self.generation == 0 {
            for entry in &mut self.entries {
                entry.generation = 0;
            }
            self.generation = 1;
        }
        Ok(())
    }
}

impl std::fmt::Debug for UniqueItemKeyIndex {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("UniqueItemKeyIndex")
            .field("capacity", &self.capacity())
            .finish()
    }
}

/// Exact proof that a borrowed key batch contains no duplicate logical keys.
///
/// Every constructor confirms duplicates with full [`Eq`] equality. The
/// indexed hash constructor uses hashes only to select candidate buckets, so
/// colliding distinct keys remain unique. The keys must retain their logical
/// meaning while this value is alive, as required by the [`ResourceKey`]
/// contract.
#[derive(Clone, Copy)]
pub struct UniqueItemKeys<'keys, K: ResourceKey> {
    keys: &'keys [K],
}

impl<'keys, K: ResourceKey> UniqueItemKeys<'keys, K> {
    /// Returns an exact uniqueness proof, or `None` when any key repeats.
    ///
    /// This allocation-free constructor is intended for small transaction
    /// batches. It performs pairwise equality checks so the proof is
    /// independent of `Hash` quality and cannot admit a collision alias.
    pub fn try_new(keys: &'keys [K]) -> Option<Self> {
        for (index, key) in keys.iter().enumerate() {
            if keys[..index].iter().any(|prior| prior == key) {
                return None;
            }
        }
        Some(Self { keys })
    }

    /// Returns an exact uniqueness proof using a reusable caller-owned index.
    ///
    /// `order` is cleared, grown fallibly, and filled with every input index.
    /// On a normal return it is a permutation sorted by the corresponding key,
    /// including when this method reports a duplicate. Callers that need to
    /// inspect equal-key groups may therefore reuse the same ordering instead
    /// of sorting the batch a second time.
    ///
    /// This constructor performs `O(n log n)` ordering comparisons and `O(n)`
    /// exact equality checks. It never hashes a key. [`ResourceKey`] requires
    /// `Ord` and `Eq` to agree, so adjacent equality in the sorted permutation
    /// is a complete proof. Retaining `order` across calls also makes repeated
    /// batches allocation-free once it has sufficient capacity.
    ///
    /// [`Self::try_new`] remains preferable for very small batches where its
    /// allocation-free pairwise scan costs less than initializing and sorting
    /// an index.
    pub fn try_new_indexed(
        keys: &'keys [K],
        order: &mut Vec<usize>,
    ) -> Result<Option<Self>, std::collections::TryReserveError> {
        order.clear();
        order.try_reserve_exact(keys.len())?;
        order.extend(0..keys.len());
        order.sort_unstable_by(|left, right| keys[*left].cmp(&keys[*right]));

        if order
            .windows(2)
            .any(|adjacent| keys[adjacent[0]] == keys[adjacent[1]])
        {
            Ok(None)
        } else {
            Ok(Some(Self { keys }))
        }
    }

    /// Returns an exact uniqueness proof using a reusable hash index.
    ///
    /// The index grows fallibly to keep its load at or below one half. Later
    /// calls reuse that allocation and start a new logical generation in
    /// constant time. [`ResourceKey`] requires equal keys to hash equally;
    /// every candidate match is nevertheless confirmed with full [`Eq`], so a
    /// hash collision cannot reject a distinct key or admit a duplicate.
    ///
    /// Unlike [`Self::try_new_indexed`], this method does not construct a sorted
    /// permutation. Callers that need duplicate-group ordering may build it
    /// only on the cold `None` result.
    pub fn try_new_hashed(
        keys: &'keys [K],
        index: &mut UniqueItemKeyIndex,
    ) -> Result<Option<Self>, std::collections::TryReserveError> {
        index.begin(keys.len())?;
        if keys.is_empty() {
            return Ok(Some(Self { keys }));
        }

        debug_assert!(!index.entries.is_empty());
        debug_assert!(keys.len() <= index.entries.len() / 2);
        let generation = index.generation;
        let bucket_mask = index.entries.len() - 1;
        for (item_slot, key) in keys.iter().enumerate() {
            let mut hasher = ItemHasher::for_unique_key();
            key.hash(&mut hasher);
            let mut bucket = hasher.finish() as usize & bucket_mask;
            loop {
                let entry = &mut index.entries[bucket];
                if entry.generation != generation {
                    *entry = UniqueItemKeyIndexEntry {
                        generation,
                        item_slot,
                    };
                    break;
                }
                if keys[entry.item_slot] == *key {
                    return Ok(None);
                }
                bucket = (bucket + 1) & bucket_mask;
            }
        }
        Ok(Some(Self { keys }))
    }

    /// Returns the proven-unique keys in their original order.
    pub const fn as_slice(&self) -> &'keys [K] {
        self.keys
    }

    /// Returns the number of proven-unique keys.
    pub const fn len(&self) -> usize {
        self.keys.len()
    }

    /// Returns whether the proven batch is empty.
    pub const fn is_empty(&self) -> bool {
        self.keys.is_empty()
    }
}

impl<K: ResourceKey> std::fmt::Debug for UniqueItemKeys<'_, K> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("UniqueItemKeys")
            .field("keys", &self.keys)
            .finish()
    }
}

/// Flow control for an operation over an exactly unique item batch.
///
/// [`Self::Stop`] keeps the item whose operation returned it and ends the
/// batch before core initializes the next key. This lets streaming callers
/// retain an exact processed prefix rather than adding an unseen suffix to
/// the transaction.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ItemBatchControl {
    Continue,
    Stop,
}

/// Result of attempting a streaming append to the homogeneous typed lane.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum ItemBatchOutcome {
    /// The transaction shape cannot append this binding directly. No item was
    /// initialized and no operation ran, so the caller may use scalar lookup.
    Ineligible,
    /// Every proven-unique key was initialized and operated on in input order.
    Complete { appended: usize },
    /// The operation requested a stop after the reported prefix was appended.
    Stopped { appended: usize },
}

impl ItemBatchOutcome {
    /// Returns the number of items initialized and operated on by this call.
    pub const fn appended(self) -> usize {
        match self {
            Self::Ineligible => 0,
            Self::Complete { appended } | Self::Stopped { appended } => appended,
        }
    }

    /// Returns whether an operation requested an early stop.
    pub const fn stopped(self) -> bool {
        matches!(self, Self::Stopped { .. })
    }
}

struct ItemHasher {
    state: u64,
}

impl ItemHasher {
    const MULTIPLIER: u64 = 0x517c_c1b7_2722_0a95;

    #[inline]
    const fn for_unique_key() -> Self {
        Self {
            state: Self::MULTIPLIER,
        }
    }

    #[inline]
    fn for_resource<A: TransactionalResource>(resource: &RegisteredResource<A>) -> Self {
        Self {
            state: resource.object_id().get()
                ^ (u64::from(resource.resource_class().get()) << 32)
                ^ Self::MULTIPLIER,
        }
    }

    /// The same word-at-a-time recurrence used by rustc's fast non-cryptographic
    /// hasher. Transaction identities are bounded and always verified with the
    /// full erased identity, so collision resistance is a correctness property
    /// of the index rather than something this hot-path hash must provide.
    #[inline]
    fn mix(&mut self, value: u64) {
        self.state = (self.state.rotate_left(5) ^ value).wrapping_mul(Self::MULTIPLIER);
    }
}

impl Hasher for ItemHasher {
    #[inline]
    fn finish(&self) -> u64 {
        self.state ^ (self.state >> 32)
    }

    #[inline]
    fn write(&mut self, bytes: &[u8]) {
        let mut chunks = bytes.chunks_exact(8);
        for chunk in &mut chunks {
            self.mix(u64::from_le_bytes(
                chunk.try_into().expect("eight-byte hash chunk"),
            ));
        }
        let remainder = chunks.remainder();
        if !remainder.is_empty() {
            let mut tail = [0_u8; 8];
            tail[..remainder.len()].copy_from_slice(remainder);
            self.mix(u64::from_le_bytes(tail) ^ ((remainder.len() as u64) << 56));
        }
        self.mix(bytes.len() as u64);
    }

    #[inline]
    fn write_u32(&mut self, value: u32) {
        self.mix(u64::from(value));
    }

    #[inline]
    fn write_u64(&mut self, value: u64) {
        self.mix(value);
    }

    #[inline]
    fn write_usize(&mut self, value: usize) {
        self.mix(value as u64);
    }
}

#[derive(Clone, Copy, Default)]
struct ItemIndexEntry {
    identity_hash: u64,
    generation: u32,
    item_slot: u32,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct ItemIndexVacancy {
    bucket: usize,
    table_len: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ItemIndexProbe {
    Occupied(usize),
    Vacant(ItemIndexVacancy),
}

/// Worker-local open-addressed item index.
///
/// Every logical item occupies one table entry, including items whose identity
/// hashes collide. Entries are tagged with the current transaction generation,
/// so beginning a transaction is O(1) and never clears the table. Growth is
/// bounded by the worker's peak transaction size and disappears from the
/// steady-state path once that peak is stable.
#[derive(Default)]
struct ItemIndex {
    entries: Vec<ItemIndexEntry>,
    generation: u32,
    len: usize,
}

impl ItemIndex {
    const MIN_CAPACITY: usize = 8;

    fn begin_transaction(&mut self) {
        self.len = 0;
        self.generation = self.generation.wrapping_add(1);
        if self.generation == 0 {
            // This is reachable only after 2^32 transactions on one attached
            // worker. Reset the tags so generation one remains unambiguous.
            for entry in &mut self.entries {
                entry.generation = 0;
            }
            self.generation = 1;
        }
    }

    #[inline]
    fn find(&self, identity_hash: u64, mut matches: impl FnMut(usize) -> bool) -> Option<usize> {
        match self.probe(identity_hash, &mut matches) {
            ItemIndexProbe::Occupied(item_slot) => Some(item_slot),
            ItemIndexProbe::Vacant(_) => None,
        }
    }

    /// Performs the exact identity lookup and retains the first vacant bucket.
    ///
    /// A fresh insertion can reuse this bucket as long as reserving space does
    /// not grow the table. This avoids walking the same collision cluster a
    /// second time after the caller has initialized and operated on the item.
    #[inline]
    fn probe(&self, identity_hash: u64, mut matches: impl FnMut(usize) -> bool) -> ItemIndexProbe {
        if self.entries.is_empty() {
            return ItemIndexProbe::Vacant(ItemIndexVacancy {
                bucket: 0,
                table_len: 0,
            });
        }

        let mask = self.entries.len() - 1;
        let mut bucket = identity_hash as usize & mask;
        loop {
            let entry = &self.entries[bucket];
            if entry.generation != self.generation {
                return ItemIndexProbe::Vacant(ItemIndexVacancy {
                    bucket,
                    table_len: self.entries.len(),
                });
            }
            let item_slot = entry.item_slot as usize;
            if entry.identity_hash == identity_hash && matches(item_slot) {
                return ItemIndexProbe::Occupied(item_slot);
            }
            bucket = (bucket + 1) & mask;
        }
    }

    #[inline]
    fn try_reserve_for_insert(&mut self) -> Result<(), crate::error::CapacityError> {
        let needed = self
            .len
            .checked_add(1)
            .ok_or(crate::error::CapacityError::ItemLimit)?;
        self.try_reserve_for_len(needed)
    }

    /// Reserves one insertion and returns the exact bucket to populate.
    ///
    /// `vacancy` comes from an immediately preceding lookup while this index
    /// is exclusively borrowed. Growth is the only operation that can
    /// invalidate it, and in that case the already-proven-fresh hash needs
    /// only a vacant-bucket walk in the replacement table.
    #[inline]
    fn try_reserve_vacancy(
        &mut self,
        identity_hash: u64,
        vacancy: ItemIndexVacancy,
    ) -> Result<usize, crate::error::CapacityError> {
        self.try_reserve_for_insert()?;
        if vacancy.table_len == self.entries.len() && vacancy.table_len != 0 {
            debug_assert_ne!(
                self.entries[vacancy.bucket].generation, self.generation,
                "a retained item-index vacancy remains empty"
            );
            return Ok(vacancy.bucket);
        }
        Ok(self.vacant_bucket(identity_hash))
    }

    #[inline]
    fn try_reserve_for_len(&mut self, needed: usize) -> Result<(), crate::error::CapacityError> {
        if needed <= self.entries.len() / 2 {
            return Ok(());
        }
        self.try_grow(needed)
    }

    #[cold]
    fn try_grow(&mut self, needed: usize) -> Result<(), crate::error::CapacityError> {
        let minimum_capacity = needed
            .checked_mul(2)
            .ok_or(crate::error::CapacityError::ItemLimit)?;
        let new_capacity = minimum_capacity
            .max(Self::MIN_CAPACITY)
            .checked_next_power_of_two()
            .ok_or(crate::error::CapacityError::ItemLimit)?;
        let mut replacement = Vec::new();
        replacement
            .try_reserve_exact(new_capacity)
            .map_err(|_| crate::error::CapacityError::ItemLimit)?;
        replacement.resize(new_capacity, ItemIndexEntry::default());

        for entry in self
            .entries
            .iter()
            .copied()
            .filter(|entry| entry.generation == self.generation)
        {
            Self::insert_into(&mut replacement, entry);
        }
        self.entries = replacement;
        Ok(())
    }

    #[inline]
    fn insert(&mut self, identity_hash: u64, item_slot: usize) {
        debug_assert!(self.len < self.entries.len() / 2);
        let bucket = self.vacant_bucket(identity_hash);
        self.insert_at(bucket, identity_hash, item_slot);
    }

    #[inline]
    fn insert_at(&mut self, bucket: usize, identity_hash: u64, item_slot: usize) {
        debug_assert!(self.len < self.entries.len() / 2);
        debug_assert_ne!(self.entries[bucket].generation, self.generation);
        self.entries[bucket] = ItemIndexEntry {
            identity_hash,
            generation: self.generation,
            item_slot: u32::try_from(item_slot)
                .expect("runtime item limit keeps item slots within u32"),
        };
        self.len += 1;
    }

    #[inline]
    fn vacant_bucket(&self, identity_hash: u64) -> usize {
        debug_assert!(!self.entries.is_empty());
        let mask = self.entries.len() - 1;
        let mut bucket = identity_hash as usize & mask;
        while self.entries[bucket].generation == self.generation {
            bucket = (bucket + 1) & mask;
        }
        bucket
    }

    #[inline]
    fn insert_into(entries: &mut [ItemIndexEntry], entry: ItemIndexEntry) {
        let mask = entries.len() - 1;
        let mut bucket = entry.identity_hash as usize & mask;
        while entries[bucket].generation == entry.generation {
            bucket = (bucket + 1) & mask;
        }
        entries[bucket] = entry;
    }

    #[cfg(test)]
    fn capacity(&self) -> usize {
        self.entries.len() / 2
    }
}

#[derive(Clone, Copy)]
struct CommitShape {
    has_writes: bool,
    has_predicates: bool,
}

// Consecutive operations commonly use the same resource. Retain only that
// binding so the common check stays one comparison and the transaction frame
// does not grow for a speculative multi-resource cache.
#[derive(Default)]
struct ValidatedBindings {
    current: Option<NonZeroUsize>,
}

impl ValidatedBindings {
    #[inline(always)]
    fn activate(&self, binding: NonZeroUsize) -> bool {
        self.current == Some(binding)
    }

    #[inline(always)]
    fn admit(&mut self, binding: NonZeroUsize) {
        self.current = Some(binding);
    }

    #[cfg(test)]
    fn entries(&self) -> impl Iterator<Item = NonZeroUsize> + '_ {
        self.current.into_iter()
    }
}

pub(crate) struct TransactionScratch {
    // Keep one worker-affine runtime handle paired with the reusable frame.
    // Moving it between scratch and the active frame avoids a shared Arc
    // increment/decrement on every transaction boundary.
    runtime: Arc<Runtime>,
    items: Vec<Option<Box<dyn ErasedItem>>>,
    // The leading ordinary boxes whose retained resource handles have already
    // been disposed. This avoids repeating erased calls and unwind boundaries
    // after a transaction switches to the separate typed-batch pool.
    ordinary_disposed_prefix: usize,
    unique_batch: Option<Box<dyn ErasedItemBatch>>,
    item_index: ItemIndex,
    lock_storage: LockPlanStorage,
}

impl TransactionScratch {
    pub(crate) fn new(runtime: Arc<Runtime>) -> Self {
        Self {
            runtime,
            items: Vec::new(),
            ordinary_disposed_prefix: 0,
            unique_batch: None,
            item_index: ItemIndex::default(),
            lock_storage: LockPlanStorage::default(),
        }
    }

    /// Drops every adapter binding retained by this idle worker scratch under
    /// an unwind boundary. On failure the caller must poison the runtime and
    /// quarantine the returned scratch rather than run any more destructors.
    #[allow(
        clippy::result_large_err,
        reason = "the unwind path must return the allocation itself for no-allocation quarantine"
    )]
    pub(crate) fn dispose_retained_resources(mut self) -> Result<(), Self> {
        debug_assert!(self.ordinary_disposed_prefix <= self.items.len());
        for item_slot in self.ordinary_disposed_prefix..self.items.len() {
            let Some(item) = self.items[item_slot].as_mut() else {
                continue;
            };
            let disposal = catch_unwind(AssertUnwindSafe(|| {
                item.dispose_retained_resource();
            }));
            if disposal.is_err() {
                return Err(self);
            }
        }
        if let Some(batch) = self.unique_batch.as_mut() {
            let disposal = catch_unwind(AssertUnwindSafe(|| {
                batch.dispose_retained_resources();
            }));
            if disposal.is_err() {
                return Err(self);
            }
        }
        Ok(())
    }
}

impl std::fmt::Debug for TransactionScratch {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("TransactionScratch")
            .field("runtime_id", &self.runtime.id())
            .field("item_capacity", &self.items.capacity())
            .field("ordinary_disposed_prefix", &self.ordinary_disposed_prefix)
            .field("has_unique_batch_pool", &self.unique_batch.is_some())
            .field("item_index_capacity", &self.item_index.entries.capacity())
            .finish()
    }
}

/// Worker-affine storage used only by the restricted terminal-read protocol.
///
/// Keeping this pool separate preserves the general transaction frame's
/// by-value layout while allowing a terminal batch to retain one reusable
/// typed allocation and resource binding at a stable address.
pub(crate) struct TerminalReadScratch {
    runtime: Arc<Runtime>,
    terminal_read_batch: Option<Box<dyn ErasedTerminalReadBatch>>,
}

impl TerminalReadScratch {
    pub(crate) fn new(runtime: Arc<Runtime>) -> Self {
        Self {
            runtime,
            terminal_read_batch: None,
        }
    }

    /// Disposes the one optional retained terminal binding under an unwind
    /// boundary. The boxed scratch itself is returned for no-allocation
    /// quarantine if adapter-owned destruction panics.
    pub(crate) fn dispose_retained_resource(mut self: Box<Self>) -> Result<(), Box<Self>> {
        if let Some(batch) = self.terminal_read_batch.as_mut() {
            let disposal = catch_unwind(AssertUnwindSafe(|| {
                batch.dispose_retained_resource();
            }));
            if disposal.is_err() {
                return Err(self);
            }
        }
        Ok(())
    }
}

impl std::fmt::Debug for TerminalReadScratch {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("TerminalReadScratch")
            .field("runtime_id", &self.runtime.id())
            .field(
                "has_terminal_read_batch_pool",
                &self.terminal_read_batch.is_some(),
            )
            .finish()
    }
}

struct TransactionFrame {
    runtime: Arc<Runtime>,
    isolation: IsolationMode,
    items: Vec<Option<Box<dyn ErasedItem>>>,
    ordinary_disposed_prefix: usize,
    unique_batch: Option<Box<dyn ErasedItemBatch>>,
    batch_active: bool,
    // Current-generation index entries cover exactly the live slot prefix
    // `[0, item_index.len)`. When typed storage is active, the remaining
    // `[item_index.len, item_count)` slots may be a proven-unique unindexed
    // suffix. Ordinary storage is fully indexed after each successful access.
    item_index: ItemIndex,
    item_count: usize,
    // A cached address is either retained by a live item or protected by the
    // current public access scope's borrowed resource. Successful empty
    // session/batch scopes restore the prior retained address; failures and
    // unwinds doom the attempt before another validation can consult it.
    validated_bindings: ValidatedBindings,
    // Consecutive read-then-write operations commonly touch the same logical
    // item. Store slot + 1 so this exact-identity cache remains one word and
    // can bypass hashing plus open-addressed probing on the second access.
    // The cache is attempt-local and never enters worker scratch.
    last_accessed_item_slot: Option<NonZeroUsize>,
    lock_storage: LockPlanStorage,
    doomed: bool,
}

impl TransactionFrame {
    fn new(isolation: IsolationMode, mut scratch: TransactionScratch) -> Self {
        scratch.item_index.begin_transaction();
        debug_assert!(scratch
            .unique_batch
            .as_ref()
            .is_none_or(|batch| batch.active_len() == 0));
        debug_assert!(scratch.ordinary_disposed_prefix <= scratch.items.len());
        Self {
            runtime: scratch.runtime,
            isolation,
            items: scratch.items,
            ordinary_disposed_prefix: scratch.ordinary_disposed_prefix,
            unique_batch: scratch.unique_batch,
            batch_active: false,
            item_index: scratch.item_index,
            item_count: 0,
            validated_bindings: ValidatedBindings::default(),
            last_accessed_item_slot: None,
            lock_storage: scratch.lock_storage,
            doomed: false,
        }
    }

    fn into_scratch(self) -> TransactionScratch {
        debug_assert!(!self.batch_active);
        debug_assert!(self
            .unique_batch
            .as_ref()
            .is_none_or(|batch| batch.active_len() == 0));
        debug_assert!(self.ordinary_disposed_prefix <= self.items.len());
        TransactionScratch {
            runtime: self.runtime,
            items: self.items,
            ordinary_disposed_prefix: self.ordinary_disposed_prefix,
            unique_batch: self.unique_batch,
            item_index: self.item_index,
            lock_storage: self.lock_storage,
        }
    }

    fn commit_shape(&self) -> CommitShape {
        if self.batch_active {
            let (has_writes, has_predicates) = self
                .unique_batch
                .as_ref()
                .expect("an active unique batch retains its storage")
                .commit_shape();
            return CommitShape {
                has_writes,
                has_predicates,
            };
        }
        let mut shape = CommitShape {
            has_writes: false,
            has_predicates: false,
        };
        for item in self.items[..self.item_count]
            .iter()
            .map(|slot| slot.as_ref().expect("commit owns every live item slot"))
        {
            shape.has_writes |= item.has_intent();
            shape.has_predicates |= item.has_predicate();
            if shape.has_writes && shape.has_predicates {
                break;
            }
        }
        shape
    }

    #[inline]
    fn is_preflight_free_read_only(&self) -> bool {
        if self.batch_active {
            return self
                .unique_batch
                .as_ref()
                .expect("an active unique batch retains its storage")
                .is_preflight_free_read_only();
        }
        self.items[..self.item_count].iter().all(|slot| {
            slot.as_ref()
                .expect("commit owns every live item slot")
                .is_preflight_free_read_candidate()
        })
    }
}

/// Minimal active state for the terminal-read-only protocol.
///
/// It deliberately contains no ordinary item pool, item index, unique batch,
/// or lock-plan storage. Moving this frame at begin/end therefore moves only a
/// thin scratch pointer and terminal protocol metadata.
struct TerminalReadFrame {
    scratch: Box<TerminalReadScratch>,
    terminal_read_active: bool,
    doomed: bool,
}

impl TerminalReadFrame {
    fn new(scratch: Box<TerminalReadScratch>) -> Self {
        debug_assert!(scratch
            .terminal_read_batch
            .as_ref()
            .is_none_or(|batch| batch.active_len() == 0));
        Self {
            scratch,
            terminal_read_active: false,
            doomed: false,
        }
    }

    #[inline(always)]
    fn runtime(&self) -> &Arc<Runtime> {
        &self.scratch.runtime
    }

    fn active_len(&self) -> usize {
        self.scratch
            .terminal_read_batch
            .as_ref()
            .map_or(0, |batch| batch.active_len())
    }

    fn into_scratch(self) -> Box<TerminalReadScratch> {
        debug_assert!(!self.terminal_read_active);
        debug_assert!(self
            .scratch
            .terminal_read_batch
            .as_ref()
            .is_none_or(|batch| batch.active_len() == 0));
        self.scratch
    }
}

/// One active transaction borrowing exactly one attached worker.
///
/// Completion consumes this value. Dropping it performs a definite abort.
/// Active transaction state cannot cross a thread boundary:
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<sto_core::Transaction<'static>>();
/// ```
pub struct Transaction<'worker, State = Active> {
    worker: Option<&'worker mut WorkerContext>,
    frame: Option<TransactionFrame>,
    state: PhantomData<fn() -> State>,
    not_send_sync: PhantomData<Rc<()>>,
}

/// One restricted terminal-read transaction borrowing an attached worker.
///
/// The distinct handle keeps terminal-only storage and code out of
/// [`Transaction<Active>`]. Completion consumes this value, and dropping it
/// performs a definite drop-only abort. Like the general transaction, it is
/// structurally neither `Send` nor `Sync`.
///
/// ```compile_fail
/// fn require_send<T: Send>() {}
/// require_send::<sto_core::TerminalReadTransaction<'static>>();
/// ```
pub struct TerminalReadTransaction<'worker, State = TerminalReadOpen> {
    worker: Option<&'worker mut WorkerContext>,
    frame: Option<TerminalReadFrame>,
    state: PhantomData<fn() -> State>,
    not_send_sync: PhantomData<Rc<()>>,
}

/// Scoped access to one validated typed resource within an active transaction.
///
/// A session amortizes transaction-state and resource-capability validation
/// across several [`Self::with_resolved_item`] calls. It is created only by
/// [`Transaction::with_item_session`], cannot escape that method's closure,
/// and is structurally neither `Send` nor `Sync`.
pub struct ResolvedItemSession<'session, A: TransactionalResource> {
    frame: &'session mut TransactionFrame,
    resource: &'session RegisteredResource<A>,
    binding_retained: bool,
    failure: Option<AccessError>,
    not_send_sync: PhantomData<Rc<()>>,
}

impl<A: TransactionalResource> ResolvedItemSession<'_, A> {
    /// Returns whether this session can still take the direct unique-batch
    /// append lane.
    ///
    /// This is a cheap structural eligibility check intended to precede an
    /// expensive exact uniqueness proof. `true` does not prove that any
    /// particular keys are unique or that their initialization will succeed;
    /// [`Self::try_with_unique_item_batch`] remains authoritative. An empty
    /// transaction is eligible. So is an active typed batch of the same
    /// adapter type when it has not previously used this exact
    /// registered-resource binding. The batch may already have an indexed
    /// prefix; the new proven-unique group remains an unindexed suffix until a
    /// later scalar lookup needs it. A failed session is never eligible.
    #[inline]
    pub fn can_start_unique_item_batch(&self) -> bool {
        self.failure.is_none() && can_append_unique_item_batch(self.frame, self.resource)
    }

    /// Resolves, looks up or creates, and accesses one logical item.
    ///
    /// This has the same resolver and capacity contract as
    /// [`Transaction::with_resolved_item`]. The session has already validated
    /// its resource capability, so that check and the outer transaction
    /// failure boundary are not repeated for every item.
    ///
    /// The first failed access dooms the entire session. Later accesses return
    /// [`InvalidUse::TransactionDoomed`], and the enclosing session call
    /// reports the first error even if its closure catches it.
    #[inline]
    pub fn with_resolved_item<C, R>(
        &mut self,
        lookup: impl FnOnce() -> Result<Option<(A::Key, C)>, AccessError>,
        create: impl FnOnce() -> Result<(A::Key, C), AccessError>,
        operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>, C) -> Result<R, AccessError>,
    ) -> Result<R, AccessError> {
        if self.failure.is_some() {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        let result = with_resolved_item_inner_validated(
            self.frame,
            self.resource,
            lookup,
            create,
            operation,
        );
        if result.is_ok() {
            self.binding_retained = true;
        }
        if let Err(error) = result {
            self.failure = Some(error);
            if access_error_poisons_runtime(error) {
                self.frame.runtime.poison();
            }
        }
        result
    }

    /// Tries to append an exactly unique batch while this transaction remains
    /// in the homogeneous typed lane.
    ///
    /// `Ok(true)` means every operation ran and the unique batch was appended
    /// without populating the item index. The first group may start an empty
    /// transaction; later groups must use the same adapter type and a distinct
    /// registered-resource binding. `Ok(false)` means the current binding was
    /// already used, the adapter type differs, or an intervening access
    /// materialized the transaction into ordinary heterogeneous storage. No
    /// operation ran and no state changed, so the caller may immediately fall
    /// back to [`Self::with_resolved_item`] within this session. Errors retain
    /// the session's ordinary first-error, doom, and runtime-poisoning
    /// behavior.
    #[inline]
    pub fn try_with_unique_item_batch(
        &mut self,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(usize, &mut Entry<'entry, A>) -> Result<(), AccessError>,
    ) -> Result<bool, AccessError> {
        if self.failure.is_some() {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        let retains_binding = !keys.is_empty();
        let result = try_append_unique_item_batch_inner_validated(
            self.frame,
            self.resource,
            keys,
            operation,
        );
        if result == Ok(true) && retains_binding {
            self.binding_retained = true;
        }
        if let Err(error) = result {
            self.failure = Some(error);
            if access_error_poisons_runtime(error) {
                self.frame.runtime.poison();
            }
        }
        result
    }

    /// Tries to append an exact prefix of a proven-unique batch.
    ///
    /// This has the same eligibility, failure, and fallback contract as
    /// [`Self::try_with_unique_item_batch`]. Operations run in key order.
    /// Returning [`ItemBatchControl::Stop`] keeps the current item and returns
    /// [`ItemBatchOutcome::Stopped`] before the next item initializes. This is
    /// useful for scans whose callback-visible prefix must exactly match the
    /// transaction read set. Capacity and storage for the next item are
    /// checked immediately before that item initializes, so an error retains
    /// the same successfully operated prefix as repeated scalar appends.
    #[inline]
    pub fn try_with_unique_item_batch_while(
        &mut self,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(
            usize,
            &mut Entry<'entry, A>,
        ) -> Result<ItemBatchControl, AccessError>,
    ) -> Result<ItemBatchOutcome, AccessError> {
        if self.failure.is_some() {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        let retains_binding = !keys.is_empty();
        let result = try_append_unique_item_batch_while_inner_validated(
            self.frame,
            self.resource,
            keys,
            operation,
        );
        if result
            .as_ref()
            .is_ok_and(|outcome| retains_binding && *outcome != ItemBatchOutcome::Ineligible)
        {
            self.binding_retained = true;
        }
        if let Err(error) = result {
            self.failure = Some(error);
            if access_error_poisons_runtime(error) {
                self.frame.runtime.poison();
            }
        }
        result
    }
}

impl<State> std::fmt::Debug for Transaction<'_, State> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Transaction")
            .field("active", &self.frame.is_some())
            .field(
                "items",
                &self.frame.as_ref().map_or(0, |frame| frame.item_count),
            )
            .field(
                "doomed",
                &self.frame.as_ref().is_some_and(|frame| frame.doomed),
            )
            .finish_non_exhaustive()
    }
}

impl<State> std::fmt::Debug for TerminalReadTransaction<'_, State> {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("TerminalReadTransaction")
            .field("active", &self.frame.is_some())
            .field(
                "items",
                &self.frame.as_ref().map_or(0, TerminalReadFrame::active_len),
            )
            .field(
                "doomed",
                &self.frame.as_ref().is_some_and(|frame| frame.doomed),
            )
            .finish_non_exhaustive()
    }
}

impl<'worker, State> TerminalReadTransaction<'worker, State> {
    fn transition<Next>(mut self) -> TerminalReadTransaction<'worker, Next> {
        TerminalReadTransaction {
            worker: self.worker.take(),
            frame: self.frame.take(),
            state: PhantomData,
            not_send_sync: PhantomData,
        }
    }
}

impl WorkerContext {
    /// Begins a transaction with the runtime's configured default isolation.
    pub fn begin(&mut self) -> Result<Transaction<'_, Active>, BeginError> {
        let isolation = self.runtime.config().default_isolation();
        self.begin_with(isolation)
    }

    /// Begins a transaction with an explicitly selected isolation profile.
    pub fn begin_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<Transaction<'_, Active>, BeginError> {
        self.begin_isolation(isolation)?;
        let scratch = self
            .transaction_scratch
            .take()
            .expect("idle worker retains transaction scratch");
        debug_assert!(Arc::ptr_eq(&scratch.runtime, &self.runtime));
        Ok(Transaction {
            worker: Some(self),
            frame: Some(TransactionFrame::new(isolation, scratch)),
            state: PhantomData,
            not_send_sync: PhantomData,
        })
    }

    /// Begins a transaction whose only data access is one terminal read batch.
    ///
    /// The returned open typestate exposes no general item API. Successfully
    /// recording its batch consumes it and returns [`TerminalReadReady`], which
    /// in turn exposes only commit and abort.
    pub fn begin_terminal_read_batch(
        &mut self,
    ) -> Result<TerminalReadTransaction<'_, TerminalReadOpen>, BeginError> {
        let isolation = self.runtime.config().default_isolation();
        self.begin_terminal_read_batch_with(isolation)
    }

    /// Begins a terminal read batch with an explicitly selected isolation.
    pub fn begin_terminal_read_batch_with(
        &mut self,
        isolation: IsolationMode,
    ) -> Result<TerminalReadTransaction<'_, TerminalReadOpen>, BeginError> {
        self.begin_isolation(isolation)?;
        let scratch = self
            .terminal_read_scratch
            .take()
            .expect("idle worker retains terminal-read scratch");
        debug_assert!(Arc::ptr_eq(&scratch.runtime, &self.runtime));
        Ok(TerminalReadTransaction {
            worker: Some(self),
            frame: Some(TerminalReadFrame::new(scratch)),
            state: PhantomData,
            not_send_sync: PhantomData,
        })
    }
}

impl<'worker> TerminalReadTransaction<'worker, TerminalReadOpen> {
    /// Definitely aborts preparatory work that failed before the terminal
    /// batch was handed to core, and returns `error` unchanged.
    ///
    /// This is the failure boundary for datatype adapters that perform
    /// directory or cache preparation before calling
    /// [`Self::with_terminal_read_batch`]. It applies the same runtime
    /// quarantine policy as an access error raised inside that method:
    /// adapter faults, poisoned dependencies, and internal errors poison the
    /// runtime, while ordinary conflicts and capacity failures do not.
    #[cold]
    #[inline(never)]
    pub fn abort_with_access_error(mut self, error: AccessError) -> AccessError {
        let worker = self
            .worker
            .take()
            .expect("an open terminal transaction retains its worker");
        let frame = self
            .frame
            .take()
            .expect("an open terminal transaction retains its frame");
        if access_error_poisons_runtime(error) {
            frame.runtime().poison();
        }
        let _ = abort_terminal_read_without_locks(worker, frame, AbortReason::Explicit);
        error
    }

    /// Records this transaction's one terminal homogeneous read batch.
    ///
    /// The transaction must have been created by
    /// [`WorkerContext::begin_terminal_read_batch`]. Each callback receives a
    /// restricted [`TerminalReadEntry`] and must record exactly one ordinary
    /// observation. Duplicate keys are permitted: without writes or later
    /// lookup they are independent conservative observations of the same
    /// logical item.
    ///
    /// Success consumes the open handle and returns a ready handle that permits
    /// only commit or abort. An error consumes and definitely aborts the open
    /// transaction before returning. An unwind likewise reaches transaction
    /// Drop with the complete prefix and at most one pending key retained.
    pub fn with_terminal_read_batch<A>(
        mut self,
        resource: &RegisteredResource<A>,
        keys: &[A::Key],
        mut operation: impl for<'entry> FnMut(
            usize,
            &mut TerminalReadEntry<'entry, A>,
        ) -> Result<(), AccessError>,
    ) -> Result<TerminalReadTransaction<'worker, TerminalReadReady>, AccessError>
    where
        A: TransactionalResource,
    {
        let frame = self
            .frame
            .as_mut()
            .expect("an open terminal transaction retains its frame");
        debug_assert!(!frame.terminal_read_active);
        debug_assert_eq!(frame.active_len(), 0);

        frame.doomed = true;
        let result = (|| {
            resource.validate_for_runtime(frame.runtime().id())?;
            append_terminal_read_batch(frame, resource, keys, &mut operation)
        })();
        match result {
            Ok(()) => {
                frame.doomed = false;
                Ok(self.transition())
            }
            Err(error) => {
                if access_error_poisons_runtime(error) {
                    frame.runtime().poison();
                }
                Err(error)
            }
        }
    }
}

impl<'worker> Transaction<'worker, Active> {
    /// Returns this transaction's selected isolation profile.
    pub fn isolation(&self) -> IsolationMode {
        self.frame
            .as_ref()
            .map(|frame| frame.isolation)
            .unwrap_or(IsolationMode::Serializable)
    }

    /// Returns whether a prior failed or unwound access made commit impossible.
    pub fn is_doomed(&self) -> bool {
        self.frame.as_ref().is_none_or(|frame| frame.doomed)
    }

    /// Returns whether this attempt already retains an item for the exact
    /// registered resource binding.
    ///
    /// This is a structural query for datatype implementations that can use a
    /// conservative table-level observation only before any local state for
    /// that table exists. It does not create an item, validate the resource,
    /// or change the attempt's failure state. Cloned handles to the same
    /// binding compare equal; another registration never does.
    #[doc(hidden)]
    pub fn has_items_for<A>(&self, resource: &RegisteredResource<A>) -> bool
    where
        A: TransactionalResource,
    {
        let Some(frame) = self.frame.as_ref() else {
            return false;
        };
        let binding_identity = resource.binding_identity();
        if frame.batch_active {
            return frame
                .unique_batch
                .as_ref()
                .and_then(|batch| batch.as_any().downcast_ref::<TypedItemBatch<A>>())
                .is_some_and(|batch| batch.contains_active_binding(resource));
        }
        frame.items[..frame.item_count].iter().any(|slot| {
            slot.as_ref()
                .expect("an active transaction item slot remains occupied")
                .retains_binding_identity(binding_identity)
        })
    }

    /// Looks up or creates one typed logical item, then scopes adapter access
    /// to `operation`.
    #[inline]
    pub fn with_item<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        key: A::Key,
        operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TransactionalResource,
    {
        let Some(frame) = self.frame.as_mut() else {
            return Err(InvalidUse::TransactionFinished.into());
        };
        if frame.doomed {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        // Arm before every fallible runtime, allocation, initialization, and
        // adapter-operation step. An unwind naturally leaves this set.
        frame.doomed = true;
        let result = with_item_inner(frame, resource, key, operation);
        if result.is_ok() {
            frame.doomed = false;
        } else if matches!(
            result,
            Err(AccessError::Fault(_) | AccessError::Poisoned(_) | AccessError::Internal(_))
        ) {
            frame.runtime.poison();
        }
        result
    }

    /// Resolves a datatype-specific stable item identity under the same
    /// failure boundary as the item operation, then looks up or creates that
    /// item.
    ///
    /// This is useful for index-backed resources whose public key is not the
    /// cheapest or most stable transaction identity. For example, a tree can
    /// resolve a byte string to a compact record ID and use that ID as its STO
    /// item key. A resolution error dooms the transaction exactly like an
    /// error from [`Self::with_item`].
    ///
    /// `lookup` must be nonmutating. When it returns `None`, core checks item
    /// capacity before invoking `create`, so an index-backed adapter cannot
    /// accidentally publish a new identity for a transaction that has no room
    /// to retain it. A successful lookup may still reuse an existing
    /// transaction item when the transaction is otherwise at capacity.
    #[inline]
    pub fn with_resolved_item<A, C, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        lookup: impl FnOnce() -> Result<Option<(A::Key, C)>, AccessError>,
        create: impl FnOnce() -> Result<(A::Key, C), AccessError>,
        operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>, C) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TransactionalResource,
    {
        let Some(frame) = self.frame.as_mut() else {
            return Err(InvalidUse::TransactionFinished.into());
        };
        if frame.doomed {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        frame.doomed = true;
        let result = (|| {
            // Validate the typed capability before either resolver can perform
            // a datatype-specific operation. The inner helper accepts this
            // proof by construction and does not repeat the checks.
            validate_item_resource(frame, resource)?;
            with_resolved_item_inner_validated(frame, resource, lookup, create, operation)
        })();
        if result.is_ok() {
            frame.doomed = false;
        } else if matches!(
            result,
            Err(AccessError::Fault(_) | AccessError::Poisoned(_) | AccessError::Internal(_))
        ) {
            frame.runtime.poison();
        }
        result
    }

    /// Runs several resolved-item accesses for one typed resource under a
    /// single transaction failure boundary.
    ///
    /// The resource capability is validated before `operation` runs. Each
    /// [`ResolvedItemSession::with_resolved_item`] call otherwise preserves
    /// the lookup, create, capacity, item-reuse, and error semantics of
    /// [`Self::with_resolved_item`]. If any access fails, the transaction stays
    /// doomed and this method reports the first such error even when
    /// `operation` catches it. An unwind likewise leaves the transaction
    /// doomed.
    #[inline]
    pub fn with_item_session<A, R>(
        &mut self,
        resource: &RegisteredResource<A>,
        operation: impl for<'session> FnOnce(
            &mut ResolvedItemSession<'session, A>,
        ) -> Result<R, AccessError>,
    ) -> Result<R, AccessError>
    where
        A: TransactionalResource,
    {
        let Some(frame) = self.frame.as_mut() else {
            return Err(InvalidUse::TransactionFinished.into());
        };
        if frame.doomed {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        // Keep the boundary armed for the whole batch. In particular, a panic
        // in a resolver or caller operation must not briefly make the
        // transaction committable between item accesses.
        frame.doomed = true;
        let result = (|| {
            let previous_validated_binding = frame.validated_bindings.current;
            validate_item_resource(frame, resource)?;
            let mut session = ResolvedItemSession {
                frame,
                resource,
                binding_retained: false,
                failure: None,
                not_send_sync: PhantomData,
            };
            let operation_result = operation(&mut session);
            let result = session.failure.map_or(operation_result, Err);
            if result.is_ok() && !session.binding_retained {
                session.frame.validated_bindings.current = previous_validated_binding;
            }
            result
        })();
        if result.is_ok() {
            frame.doomed = false;
        } else if result
            .as_ref()
            .is_err_and(|error| access_error_poisons_runtime(*error))
        {
            frame.runtime.poison();
        }
        result
    }

    /// Appends one exactly unique group to a homogeneous typed transaction.
    ///
    /// The [`UniqueItemKeys`] proof lets core omit per-item hashing, index
    /// probing, identity dispatch, and index insertion. Each key still gets
    /// the ordinary typed [`Entry`] surface in input order, so adapters may
    /// record observations, predicates, or intents normally. The first group
    /// starts an empty transaction. Further groups may remain contiguous when
    /// they use the same adapter type and a distinct exact
    /// [`RegisteredResource`] binding; distinct bindings make their full item
    /// identities disjoint even when keys repeat across groups. Earlier typed
    /// items may already form an exact indexed prefix. A new group stays as an
    /// unindexed suffix until a later scalar access needs it. If an access
    /// through another adapter type follows these groups, core materializes
    /// the typed storage and indexes only the missing suffix before performing
    /// that access.
    ///
    /// A binding may appear in at most one direct group. Reusing a binding or
    /// changing adapter type is misuse for this direct API; a resolved session
    /// reports the same structural ineligibility as `Ok(false)` so it can take
    /// scalar fallback. Capacity is checked against the total live prefix
    /// before any new item initializes.
    /// Misuse, capacity failure, an item initialization error, or an operation
    /// error dooms the transaction just like [`Self::with_item`]. An unwind
    /// likewise leaves it doomed.
    #[inline]
    pub fn with_unique_item_batch<A>(
        &mut self,
        resource: &RegisteredResource<A>,
        keys: UniqueItemKeys<'_, A::Key>,
        operation: impl for<'entry> FnMut(usize, &mut Entry<'entry, A>) -> Result<(), AccessError>,
    ) -> Result<(), AccessError>
    where
        A: TransactionalResource,
    {
        let Some(frame) = self.frame.as_mut() else {
            return Err(InvalidUse::TransactionFinished.into());
        };
        if frame.doomed {
            return Err(InvalidUse::TransactionDoomed.into());
        }

        // Arm before capability checks, allocation, key cloning,
        // initialization, and every adapter operation. A panic at any point
        // leaves normal transaction Drop responsible for definite abort.
        frame.doomed = true;
        let result = (|| {
            let previous_validated_binding = frame.validated_bindings.current;
            let retains_binding = !keys.is_empty();
            validate_item_resource(frame, resource)?;
            if !try_append_unique_item_batch_inner_validated(frame, resource, keys, operation)? {
                return Err(InvalidUse::UniqueBatchRequiresEmptyTransaction.into());
            }
            if !retains_binding {
                frame.validated_bindings.current = previous_validated_binding;
            }
            Ok(())
        })();
        if result.is_ok() {
            frame.doomed = false;
        } else if result
            .as_ref()
            .is_err_and(|error| access_error_poisons_runtime(*error))
        {
            frame.runtime.poison();
        }
        result
    }

    /// Attempts the full preflight/lock/validate/install/publication protocol.
    pub fn commit(mut self) -> Result<CommitOutcome, CommitFailure> {
        self.commit_with_optional_hook(None)
    }

    /// Commits with an upper-layer metadata and pre-install coordination hook.
    ///
    /// The hook is skipped for a read-only transaction. For a writing
    /// transaction its reservation callback runs after all planned locks are
    /// acquired, and its pre-install callback runs after final validation but
    /// before installation can begin.
    pub fn commit_with_hook<H: CommitHook>(
        mut self,
        hook: &mut H,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.commit_with_optional_hook(Some(hook))
    }

    fn commit_with_optional_hook(
        &mut self,
        hook: Option<&mut dyn CommitHook>,
    ) -> Result<CommitOutcome, CommitFailure> {
        let worker = self
            .worker
            .take()
            .expect("active transaction always retains its worker");
        let frame = self
            .frame
            .take()
            .expect("active transaction always retains its frame");
        let mut driver = CommitDriver::new(worker, frame, hook);
        match catch_unwind(AssertUnwindSafe(|| driver.run())) {
            Ok(result) => result,
            Err(_) => match catch_unwind(AssertUnwindSafe(|| driver.contain_unexpected_unwind())) {
                Ok(result) => result,
                Err(_) => driver.emergency_failure(),
            },
        }
    }

    /// Explicitly aborts and finishes every inserted item exactly once.
    pub fn abort(mut self) -> AbortInfo {
        let worker = self
            .worker
            .take()
            .expect("active transaction always retains its worker");
        let frame = self
            .frame
            .take()
            .expect("active transaction always retains its frame");
        abort_without_locks(worker, frame, AbortReason::Explicit)
    }
}

impl TerminalReadTransaction<'_, TerminalReadReady> {
    /// Certifies and commits the terminal read batch without constructing a
    /// lock plan or invoking any general item callback.
    pub fn commit(mut self) -> Result<CommitOutcome, CommitFailure> {
        let worker = self
            .worker
            .take()
            .expect("a ready terminal transaction retains its worker");
        let frame = self
            .frame
            .take()
            .expect("a ready terminal transaction retains its frame");
        let mut driver = TerminalReadCommitDriver::new(worker, frame);
        match catch_unwind(AssertUnwindSafe(|| driver.run())) {
            Ok(result) => result,
            Err(_) => match catch_unwind(AssertUnwindSafe(|| driver.contain_unexpected_unwind())) {
                Ok(result) => result,
                Err(_) => driver.emergency_failure(),
            },
        }
    }

    /// Explicitly aborts a ready terminal batch using drop-only cleanup.
    pub fn abort(mut self) -> AbortInfo {
        let worker = self
            .worker
            .take()
            .expect("a ready terminal transaction retains its worker");
        let frame = self
            .frame
            .take()
            .expect("a ready terminal transaction retains its frame");
        abort_terminal_read_without_locks(worker, frame, AbortReason::Explicit)
    }
}

impl<State> Drop for Transaction<'_, State> {
    fn drop(&mut self) {
        let (Some(worker), Some(frame)) = (self.worker.take(), self.frame.take()) else {
            return;
        };
        let _ = abort_without_locks(worker, frame, AbortReason::Explicit);
    }
}

impl<State> Drop for TerminalReadTransaction<'_, State> {
    fn drop(&mut self) {
        let (Some(worker), Some(frame)) = (self.worker.take(), self.frame.take()) else {
            return;
        };
        let _ = abort_terminal_read_without_locks(worker, frame, AbortReason::Explicit);
    }
}

#[inline]
fn with_item_inner<A, R>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    key: A::Key,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    validate_item_resource(frame, resource)?;
    with_item_inner_validated(frame, resource, key, operation)
}

#[inline(always)]
fn with_resolved_item_inner_validated<A, C, R>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    lookup: impl FnOnce() -> Result<Option<(A::Key, C)>, AccessError>,
    create: impl FnOnce() -> Result<(A::Key, C), AccessError>,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>, C) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    let resolved = lookup()?;
    let (key, context) = if let Some(existing) = resolved {
        existing
    } else {
        if frame.item_count >= frame.runtime.config().max_items_per_transaction() {
            return Err(crate::error::CapacityError::ItemLimit.into());
        }
        create()?
    };
    with_item_inner_validated(frame, resource, key, |entry| operation(entry, context))
}

#[inline(always)]
fn access_error_poisons_runtime(error: AccessError) -> bool {
    matches!(
        error,
        AccessError::Fault(_) | AccessError::Poisoned(_) | AccessError::Internal(_)
    )
}

#[inline(always)]
fn validate_item_resource<A>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
) -> Result<(), AccessError>
where
    A: TransactionalResource,
{
    let binding_identity = resource.binding_identity();
    if frame.validated_bindings.activate(binding_identity) {
        return Ok(());
    }
    validate_item_resource_miss(frame, resource, binding_identity)
}

#[inline(always)]
fn validate_item_resource_miss<A>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    binding_identity: NonZeroUsize,
) -> Result<(), AccessError>
where
    A: TransactionalResource,
{
    if let Err(error) = resource.validate_for_runtime(frame.runtime.id()) {
        return item_resource_validation_failure(error);
    }
    frame.validated_bindings.admit(binding_identity);
    Ok(())
}

#[cold]
#[inline(never)]
fn item_resource_validation_failure(error: InvalidUse) -> Result<(), AccessError> {
    Err(error.into())
}

/// Returns whether a proven-unique group can be appended without constructing
/// the item index.
///
/// Uniqueness is local to one [`UniqueItemKeys`] value. The live items make it
/// transaction-wide only when every appended group owns a distinct exact
/// resource binding, because the binding is part of each full item identity.
/// Earlier scalar lookup may have indexed an exact typed prefix. Appending a
/// new resource group is still safe: its binding proves it cannot alias that
/// prefix, and the new group remains an unindexed typed suffix until lookup
/// needs it.
#[inline]
fn can_append_unique_item_batch<A>(
    frame: &TransactionFrame,
    resource: &RegisteredResource<A>,
) -> bool
where
    A: TransactionalResource,
{
    if frame.item_count == 0 {
        return !frame.batch_active && frame.item_index.len == 0;
    }
    if !frame.batch_active {
        return false;
    }
    debug_assert!(
        frame.item_index.len <= frame.item_count,
        "the item index covers an exact typed prefix"
    );
    debug_assert!(
        frame.unique_batch.is_some(),
        "an active typed batch retains its storage"
    );
    let Some(erased) = frame.unique_batch.as_ref() else {
        return false;
    };
    if erased.active_len() != frame.item_count {
        debug_assert_eq!(erased.active_len(), frame.item_count);
        return false;
    }
    erased
        .as_any()
        .downcast_ref::<TypedItemBatch<A>>()
        .is_some_and(|batch| !batch.contains_active_binding(resource))
}

#[inline]
fn with_item_inner_validated<A, R>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    key: A::Key,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    // Keep an otherwise homogeneous scalar transaction in the same contiguous
    // typed storage used by the proven-unique batch lane. This is especially
    // valuable for callers that discover keys one operation at a time: phase
    // dispatch happens once per transaction instead of once per item, while
    // the ordinary exact item index still handles repeats and hash collisions.
    // A second adapter type materializes the prefix before taking the fully
    // heterogeneous path, preserving the existing general contract.
    if frame.batch_active {
        // Downcast the private batch once. The former adapter_type_id probe
        // followed by a second Any downcast executed twice per scalar access
        // in the homogeneous hot path.
        {
            let TransactionFrame {
                runtime,
                items,
                ordinary_disposed_prefix,
                unique_batch,
                item_index,
                item_count,
                last_accessed_item_slot,
                ..
            } = frame;
            let erased = unique_batch.as_mut().ok_or_else(|| {
                runtime.poison();
                AdapterFault::invariant(AdapterPhase::Execute)
            })?;
            if let Some(batch) = erased.as_any_mut().downcast_mut::<TypedItemBatch<A>>() {
                return with_active_typed_batch_item_inner_validated(
                    runtime,
                    items,
                    ordinary_disposed_prefix,
                    item_index,
                    item_count,
                    last_accessed_item_slot,
                    batch,
                    resource,
                    key,
                    operation,
                );
            }
        }
        materialize_unique_batch(frame)?;
    } else if frame.item_count == 0 {
        return with_typed_batch_item_inner_validated(frame, resource, key, operation);
    }

    with_ordinary_item_inner_validated(frame, resource, key, operation)
}

#[inline]
fn with_ordinary_item_inner_validated<A, R>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    key: A::Key,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    materialize_item_index(frame)?;
    let object_id = resource.object_id();
    let resource_class = resource.resource_class();
    let adapter_type_id = TypeId::of::<A>();
    let key_type_id = TypeId::of::<A::Key>();

    let cached_item_slot = frame
        .last_accessed_item_slot
        .map(|encoded| encoded.get() - 1);
    let cached_identity_matches = cached_item_slot.is_some_and(|item_slot| {
        frame
            .items
            .get(item_slot)
            .and_then(Option::as_ref)
            .is_some_and(|item| {
                item.matches_identity(
                    object_id,
                    resource_class,
                    adapter_type_id,
                    key_type_id,
                    &key,
                )
            })
    });
    if let Some(item_slot) = cached_item_slot.filter(|_| cached_identity_matches) {
        let item = frame
            .items
            .get_mut(item_slot)
            .and_then(Option::as_mut)
            .expect("last-accessed item cache references an empty slot");
        let Some(typed) = item.as_any_mut().downcast_mut::<ItemBox<A>>() else {
            frame.runtime.poison();
            return Err(AdapterFault::new(
                AdapterPhase::Execute,
                crate::error::AdapterFaultKind::TypeMismatch,
            )
            .into());
        };
        return operation(&mut Entry::new(typed));
    }

    let identity_hash = item_hash(resource, &key);
    let existing = frame.item_index.find(identity_hash, |item_slot| {
        frame
            .items
            .get(item_slot)
            .and_then(Option::as_ref)
            .is_some_and(|item| {
                item.matches_identity(
                    object_id,
                    resource_class,
                    adapter_type_id,
                    key_type_id,
                    &key,
                )
            })
    });
    if let Some(item_slot) = existing {
        frame.last_accessed_item_slot = Some(encode_item_slot(item_slot));
        let item = frame
            .items
            .get_mut(item_slot)
            .and_then(Option::as_mut)
            .expect("item index references an empty slot");
        let Some(typed) = item.as_any_mut().downcast_mut::<ItemBox<A>>() else {
            frame.runtime.poison();
            return Err(AdapterFault::new(
                AdapterPhase::Execute,
                crate::error::AdapterFaultKind::TypeMismatch,
            )
            .into());
        };
        return operation(&mut Entry::new(typed));
    }

    let maximum_items = frame.runtime.config().max_items_per_transaction();
    if frame.item_count >= maximum_items {
        return Err(crate::error::CapacityError::ItemLimit.into());
    }
    let TransactionFrame {
        runtime,
        items,
        ordinary_disposed_prefix,
        item_index,
        item_count,
        last_accessed_item_slot,
        ..
    } = frame;
    let item_slot = *item_count;
    if item_slot == items.len() {
        items
            .try_reserve(1)
            .map_err(|_| crate::error::CapacityError::ItemLimit)?;
    }
    item_index.try_reserve_for_insert()?;

    if let Some(pooled) = items.get_mut(item_slot) {
        let item = pooled
            .as_mut()
            .expect("an allocated pooled slot remains occupied");
        if let Some(typed) = item.as_any_mut().downcast_mut::<ItemBox<A>>() {
            if !typed.retains_binding(resource) && dispose_pooled_resource(runtime, typed).is_err()
            {
                return Err(pooled_resource_panic());
            }
            let local = initialize_local(runtime, resource, &key)?;
            typed.reinitialize(resource, key, local);
            *ordinary_disposed_prefix = 0;
            item_index.insert(identity_hash, item_slot);
            *item_count += 1;
            *last_accessed_item_slot = Some(encode_item_slot(item_slot));
            return operation(&mut Entry::new(typed));
        }

        if dispose_pooled_resource(runtime, item.as_mut()).is_err() {
            return Err(pooled_resource_panic());
        }
        let local = initialize_local(runtime, resource, &key)?;
        *pooled = Some(Box::new(ItemBox::new(resource.clone(), key, local)));
    } else {
        let local = initialize_local(runtime, resource, &key)?;
        items.push(Some(Box::new(ItemBox::new(resource.clone(), key, local))));
    }
    *ordinary_disposed_prefix = 0;
    item_index.insert(identity_hash, item_slot);
    *item_count += 1;
    *last_accessed_item_slot = Some(encode_item_slot(item_slot));

    let item = items[item_slot]
        .as_mut()
        .expect("newly inserted item remains occupied");
    let Some(typed) = item.as_any_mut().downcast_mut::<ItemBox<A>>() else {
        runtime.poison();
        return Err(AdapterFault::new(
            AdapterPhase::Execute,
            crate::error::AdapterFaultKind::TypeMismatch,
        )
        .into());
    };
    operation(&mut Entry::new(typed))
}

#[inline]
fn with_typed_batch_item_inner_validated<A, R>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    key: A::Key,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    let TransactionFrame {
        runtime,
        items,
        ordinary_disposed_prefix,
        unique_batch,
        batch_active,
        item_index,
        item_count,
        last_accessed_item_slot,
        ..
    } = frame;

    let batch = if !*batch_active {
        debug_assert_eq!(*item_count, 0);
        debug_assert_eq!(item_index.len, 0);
        if runtime.config().max_items_per_transaction() == 0 {
            return Err(crate::error::CapacityError::ItemLimit.into());
        }
        let batch = prepare_typed_unique_batch::<A>(runtime, unique_batch, 1)?;
        ensure_ordinary_pool_prefix_disposed(runtime, items, ordinary_disposed_prefix, 1)?;
        *batch_active = true;
        batch
    } else {
        let erased = unique_batch.as_mut().ok_or_else(|| {
            runtime.poison();
            AdapterFault::invariant(AdapterPhase::Execute)
        })?;
        erased
            .as_any_mut()
            .downcast_mut::<TypedItemBatch<A>>()
            .ok_or_else(|| {
                runtime.poison();
                AdapterFault::invariant(AdapterPhase::Execute)
            })?
    };

    with_active_typed_batch_item_inner_validated(
        runtime,
        items,
        ordinary_disposed_prefix,
        item_index,
        item_count,
        last_accessed_item_slot,
        batch,
        resource,
        key,
        operation,
    )
}

#[allow(clippy::too_many_arguments)]
#[inline(always)]
fn with_active_typed_batch_item_inner_validated<A, R>(
    runtime: &Runtime,
    items: &mut [Option<Box<dyn ErasedItem>>],
    ordinary_disposed_prefix: &mut usize,
    item_index: &mut ItemIndex,
    item_count: &mut usize,
    last_accessed_item_slot: &mut Option<NonZeroUsize>,
    batch: &mut TypedItemBatch<A>,
    resource: &RegisteredResource<A>,
    key: A::Key,
    operation: impl for<'entry> FnOnce(&mut Entry<'entry, A>) -> Result<R, AccessError>,
) -> Result<R, AccessError>
where
    A: TransactionalResource,
{
    // The last-access cache carries full binding-and-key equality, so it is
    // authoritative even when the item lies in the unindexed typed suffix.
    let cached_item_slot = last_accessed_item_slot.map(|encoded| encoded.get() - 1);
    let cached_identity_matches = cached_item_slot.is_some_and(|item_slot| {
        item_slot < batch.active_len()
            && batch
                .active_item(item_slot)
                .matches_typed_identity(resource, &key)
    });
    if let Some(item_slot) = cached_item_slot.filter(|_| cached_identity_matches) {
        let (item, intent) = batch.active_item_parts_mut(item_slot);
        let result = operation(&mut Entry::new_batch(item, intent));
        if result.is_ok() {
            batch.note_active_item_shape(item_slot);
        }
        return result;
    }

    // A clear binding-summary bit proves that no live item can share this full
    // identity, because the exact registered binding is part of that identity.
    // Append such a scalar as another unindexed singleton. A possible filter
    // collision is resolved by exact binding equality before this branch.
    let (indexed_vacancy, new_binding) = if !batch.contains_active_binding(resource) {
        (None, true)
    } else {
        // Direct unique groups and distinct-binding scalars intentionally append
        // as an unindexed suffix. Before a lookup that may alias a live binding,
        // extend the exact typed index in place without draining contiguous items.
        if item_index.len != *item_count {
            debug_assert!(item_index.len < *item_count);
            let indexed_prefix_len = item_index.len;
            item_index.try_reserve_for_len(*item_count)?;
            for item_slot in indexed_prefix_len..*item_count {
                item_index.insert(
                    batch.active_item(item_slot).typed_identity_hash(),
                    item_slot,
                );
            }
        }

        let identity_hash = item_hash(resource, &key);
        let vacancy = match item_index.probe(identity_hash, |item_slot| {
            batch
                .active_item(item_slot)
                .matches_typed_identity(resource, &key)
        }) {
            ItemIndexProbe::Occupied(item_slot) => {
                *last_accessed_item_slot = Some(encode_item_slot(item_slot));
                let (item, intent) = batch.active_item_parts_mut(item_slot);
                let result = operation(&mut Entry::new_batch(item, intent));
                if result.is_ok() {
                    batch.note_active_item_shape(item_slot);
                }
                return result;
            }
            ItemIndexProbe::Vacant(vacancy) => vacancy,
        };
        (Some((identity_hash, vacancy)), false)
    };

    if *item_count >= runtime.config().max_items_per_transaction() {
        return Err(crate::error::CapacityError::ItemLimit.into());
    }
    let item_slot = *item_count;
    let indexed_insertion = match indexed_vacancy {
        Some((identity_hash, vacancy)) => Some((
            identity_hash,
            item_index.try_reserve_vacancy(identity_hash, vacancy)?,
        )),
        None => None,
    };
    // Every pooled item was originally created only after both parallel
    // vectors had room for it. Vec clear/drain keeps that capacity, so the
    // common reactivation path cannot allocate. Reserve only when extending
    // the worker's typed high-water mark.
    if item_slot >= batch.pooled_len() {
        batch.try_reserve_for_len(item_slot + 1)?;
    }
    ensure_ordinary_pool_prefix_disposed(runtime, items, ordinary_disposed_prefix, item_slot + 1)?;
    let activated_slot = if new_binding {
        activate_typed_batch_item::<A, true>(runtime, batch, item_count, resource, key)
    } else {
        activate_typed_batch_item::<A, false>(runtime, batch, item_count, resource, key)
    }?;
    debug_assert_eq!(activated_slot, item_slot);
    let (item, intent) = batch.active_item_parts_mut(item_slot);
    let result = operation(&mut Entry::new_batch(item, intent));
    if result.is_ok() {
        batch.note_active_item_shape(item_slot);
        if let Some((identity_hash, insertion_bucket)) = indexed_insertion {
            item_index.insert_at(insertion_bucket, identity_hash, item_slot);
        }
        *last_accessed_item_slot = Some(encode_item_slot(item_slot));
    }
    result
}

#[inline(always)]
fn encode_item_slot(item_slot: usize) -> NonZeroUsize {
    let encoded = item_slot
        .checked_add(1)
        .expect("a live transaction item slot fits in usize");
    NonZeroUsize::new(encoded).expect("an encoded transaction item slot is nonzero")
}

/// Lazily indexes a directly appended unique suffix before ordinary lookup.
///
/// The item index always covers an exact live prefix. A unique group may append
/// after that prefix without hashing, leaving the remainder of the typed batch
/// unindexed. Reserving the final table size first means capacity failure
/// cannot expose a partially materialized index to another successful access.
#[inline]
fn materialize_item_index(frame: &mut TransactionFrame) -> Result<(), AccessError> {
    materialize_unique_batch(frame)?;
    if frame.item_index.len == frame.item_count {
        return Ok(());
    }
    debug_assert!(frame.item_index.len < frame.item_count);

    let TransactionFrame {
        items,
        item_index,
        item_count,
        ..
    } = frame;
    let indexed_prefix_len = item_index.len;
    item_index.try_reserve_for_len(*item_count)?;
    for (item_slot, item) in items
        .iter()
        .enumerate()
        .take(*item_count)
        .skip(indexed_prefix_len)
    {
        let identity_hash = item
            .as_ref()
            .expect("live unindexed item slot remains occupied")
            .identity_hash();
        item_index.insert(identity_hash, item_slot);
    }
    Ok(())
}

/// Converts the active typed batch into the ordinary erased-item prefix.
///
/// Every recoverable allocation is reserved before either pool changes. The
/// ordinary prefix's retained bindings are then disposed under their existing
/// per-item unwind boundaries. The unused ordinary tail remains pooled, which
/// preserves the direct lane's resource-lifetime and future-rebinding order.
/// Once draining starts, only infallible moves and the workspace's existing
/// infallible `Box` allocation policy remain. The batch is completely
/// materialized before user-defined key hashing begins, so a hashing unwind
/// leaves Drop with one coherent ordinary item sequence.
fn materialize_unique_batch(frame: &mut TransactionFrame) -> Result<(), AccessError> {
    if !frame.batch_active {
        return Ok(());
    }
    frame.last_accessed_item_slot = None;
    debug_assert!(
        frame.item_index.len <= frame.item_count,
        "the active typed batch index covers an exact prefix"
    );
    let item_count = frame.item_count;
    frame.item_index.try_reserve_for_len(item_count)?;
    if item_count > frame.items.len() {
        frame
            .items
            .try_reserve_exact(item_count - frame.items.len())
            .map_err(|_| crate::error::CapacityError::ItemLimit)?;
        frame.items.resize_with(item_count, || None);
    }

    ensure_ordinary_pool_prefix_disposed(
        &frame.runtime,
        &mut frame.items,
        &mut frame.ordinary_disposed_prefix,
        item_count,
    )?;

    frame
        .unique_batch
        .as_mut()
        .expect("an active unique batch retains its storage")
        .drain_active_into(&mut frame.items[..item_count]);
    // The newly materialized live prefix now retains resources again. Any
    // sanitized ordinary tail is no longer a leading prefix and therefore
    // cannot be skipped by this compact marker.
    frame.ordinary_disposed_prefix = 0;
    frame.batch_active = false;
    Ok(())
}

/// Shared unique-batch implementation for a direct transaction call and an
/// already validated resolved-item session. Ineligibility is deliberately a
/// successful, mutation-free result so a session can preserve composition by
/// taking its ordinary exact-lookup fallback.
#[inline]
fn try_append_unique_item_batch_inner_validated<A>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    keys: UniqueItemKeys<'_, A::Key>,
    mut operation: impl for<'entry> FnMut(usize, &mut Entry<'entry, A>) -> Result<(), AccessError>,
) -> Result<bool, AccessError>
where
    A: TransactionalResource,
{
    try_append_unique_item_batch_controlled_inner_validated::<A, true>(
        frame,
        resource,
        keys,
        |index, entry| {
            operation(index, entry)?;
            Ok(ItemBatchControl::Continue)
        },
    )
    .map(|outcome| outcome != ItemBatchOutcome::Ineligible)
}

/// Streaming form of [`try_append_unique_item_batch_inner_validated`].
///
/// A successful stop retains the current item and leaves every following key
/// untouched. Ineligibility is mutation-free so a resolved session can take
/// its scalar path immediately.
#[inline]
fn try_append_unique_item_batch_while_inner_validated<A>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    keys: UniqueItemKeys<'_, A::Key>,
    operation: impl for<'entry> FnMut(
        usize,
        &mut Entry<'entry, A>,
    ) -> Result<ItemBatchControl, AccessError>,
) -> Result<ItemBatchOutcome, AccessError>
where
    A: TransactionalResource,
{
    try_append_unique_item_batch_controlled_inner_validated::<A, false>(
        frame, resource, keys, operation,
    )
}

#[inline]
fn try_append_unique_item_batch_controlled_inner_validated<
    A: TransactionalResource,
    const PREFLIGHT_COMPLETE_BATCH: bool,
>(
    frame: &mut TransactionFrame,
    resource: &RegisteredResource<A>,
    keys: UniqueItemKeys<'_, A::Key>,
    mut operation: impl for<'entry> FnMut(
        usize,
        &mut Entry<'entry, A>,
    ) -> Result<ItemBatchControl, AccessError>,
) -> Result<ItemBatchOutcome, AccessError> {
    // Preserve the original first-group hot path: the more expensive typed
    // and exact-binding checks are needed only for a nonempty append.
    let first_item_slot = frame.item_count;
    if first_item_slot == 0 {
        debug_assert!(!frame.batch_active);
        debug_assert_eq!(frame.item_index.len, 0);
    } else if !can_append_unique_item_batch(frame, resource) {
        return Ok(ItemBatchOutcome::Ineligible);
    }

    let total_item_count = first_item_slot
        .checked_add(keys.len())
        .ok_or(crate::error::CapacityError::ItemLimit)?;
    if PREFLIGHT_COMPLETE_BATCH
        && total_item_count > frame.runtime.config().max_items_per_transaction()
    {
        return Err(crate::error::CapacityError::ItemLimit.into());
    }
    if keys.is_empty() {
        return Ok(ItemBatchOutcome::Complete { appended: 0 });
    }

    let TransactionFrame {
        runtime,
        items,
        ordinary_disposed_prefix,
        unique_batch,
        batch_active,
        item_index,
        item_count,
        ..
    } = frame;
    let batch = if first_item_slot == 0 {
        debug_assert!(!*batch_active);
        debug_assert_eq!(item_index.len, 0);
        let reserve_len = if PREFLIGHT_COMPLETE_BATCH {
            total_item_count
        } else {
            first_item_slot
        };
        prepare_typed_unique_batch(runtime, unique_batch, reserve_len)?
    } else {
        debug_assert!(*batch_active);
        debug_assert!(item_index.len <= *item_count);
        let erased = unique_batch.as_mut().ok_or_else(|| {
            runtime.poison();
            AdapterFault::invariant(AdapterPhase::Execute)
        })?;
        let batch = erased
            .as_any_mut()
            .downcast_mut::<TypedItemBatch<A>>()
            .ok_or_else(|| {
                runtime.poison();
                AdapterFault::invariant(AdapterPhase::Execute)
            })?;
        if PREFLIGHT_COMPLETE_BATCH {
            batch.try_reserve_for_len(total_item_count)?;
        }
        batch
    };

    // Before the typed lane existed, the transaction reused these same
    // ordinary worker-pool slots. Replacing a retained adapter in any newly
    // covered slot must still run its destructor under per-item containment
    // before item initialization. The complete form prepares the full prefix;
    // the streaming form prepares only the item about to run.
    if PREFLIGHT_COMPLETE_BATCH {
        ensure_ordinary_pool_prefix_disposed(
            runtime,
            items,
            ordinary_disposed_prefix,
            total_item_count,
        )?;
    }

    *batch_active = true;
    let maximum_items = runtime.config().max_items_per_transaction();
    for (index, key) in keys.as_slice().iter().enumerate() {
        if !PREFLIGHT_COMPLETE_BATCH {
            if *item_count >= maximum_items {
                return Err(crate::error::CapacityError::ItemLimit.into());
            }
            let next_item_count = (*item_count)
                .checked_add(1)
                .ok_or(crate::error::CapacityError::ItemLimit)?;
            if next_item_count > batch.pooled_len() {
                batch.try_reserve_for_len(next_item_count)?;
            }
            ensure_ordinary_pool_prefix_disposed(
                runtime,
                items,
                ordinary_disposed_prefix,
                next_item_count,
            )?;
        }
        let key = key.clone();
        let item_slot = if index == 0 {
            activate_typed_batch_item::<A, true>(runtime, batch, item_count, resource, key)
        } else {
            activate_typed_batch_item::<A, false>(runtime, batch, item_count, resource, key)
        }?;
        debug_assert_eq!(item_slot, first_item_slot + index);
        let (item, intent) = batch.active_item_parts_mut(item_slot);
        let control = operation(index, &mut Entry::new_batch(item, intent))?;
        batch.note_active_item_shape(item_slot);
        if control == ItemBatchControl::Stop {
            return Ok(ItemBatchOutcome::Stopped {
                appended: index + 1,
            });
        }
    }
    Ok(ItemBatchOutcome::Complete {
        appended: keys.len(),
    })
}

fn append_terminal_read_batch<A>(
    frame: &mut TerminalReadFrame,
    resource: &RegisteredResource<A>,
    keys: &[A::Key],
    operation: &mut impl for<'entry> FnMut(
        usize,
        &mut TerminalReadEntry<'entry, A>,
    ) -> Result<(), AccessError>,
) -> Result<(), AccessError>
where
    A: TransactionalResource,
{
    debug_assert_eq!(frame.active_len(), 0);
    debug_assert!(!frame.terminal_read_active);

    if keys.len() > frame.runtime().config().max_items_per_transaction() {
        return Err(crate::error::CapacityError::ItemLimit.into());
    }
    if keys.is_empty() {
        return Ok(());
    }

    let capability = match catch_unwind(AssertUnwindSafe(|| {
        resource.adapter().terminal_read_batch_capability()
    })) {
        Ok(Some(capability)) => capability,
        Ok(None) => return Err(Unsupported::Capability("terminal read batch").into()),
        Err(_) => {
            frame.runtime().poison();
            return Err(AdapterFault::new(
                AdapterPhase::Execute,
                crate::error::AdapterFaultKind::Panic,
            )
            .into());
        }
    };

    let TerminalReadFrame {
        scratch,
        terminal_read_active,
        ..
    } = frame;
    let TerminalReadScratch {
        runtime,
        terminal_read_batch,
    } = scratch.as_mut();
    let batch = prepare_typed_terminal_read_batch(
        runtime,
        terminal_read_batch,
        resource,
        capability,
        keys.len(),
    )?;

    // Arm before cloning the first key. Drop can now find the typed pool after
    // an unwind at any point in item construction or the user callback.
    *terminal_read_active = true;
    for (index, key) in keys.iter().enumerate() {
        batch.push_pending_key(key.clone());
        let mut entry = batch.pending_entry();
        operation(index, &mut entry)?;
        if !entry.has_read() {
            return Err(InvalidUse::IllegalItemState.into());
        }
    }
    Ok(())
}

fn prepare_typed_terminal_read_batch<'batch, A: TransactionalResource>(
    runtime: &Runtime,
    batch_slot: &'batch mut Option<Box<dyn ErasedTerminalReadBatch>>,
    resource: &RegisteredResource<A>,
    capability: &'static TerminalReadBatchCapability<A>,
    needed: usize,
) -> Result<&'batch mut TypedTerminalReadBatch<A>, AccessError> {
    let replace = batch_slot.as_mut().is_some_and(|batch| {
        batch
            .as_any_mut()
            .downcast_mut::<TypedTerminalReadBatch<A>>()
            .is_none()
    });

    if replace || batch_slot.is_none() {
        let mut replacement = TypedTerminalReadBatch::<A>::new();
        replacement.try_reserve_for_len(needed)?;
        if replace {
            let batch = batch_slot
                .as_mut()
                .expect("a terminal replacement candidate remains present");
            if dispose_pooled_terminal_resource(runtime, batch.as_mut()).is_err() {
                return Err(pooled_resource_panic());
            }
        }
        *batch_slot = Some(Box::new(replacement));
    }

    let batch = batch_slot
        .as_mut()
        .expect("terminal batch storage was installed");
    if batch.active_len() != 0 {
        runtime.poison();
        return Err(AdapterFault::invariant(AdapterPhase::Execute).into());
    }
    let batch = batch
        .as_any_mut()
        .downcast_mut::<TypedTerminalReadBatch<A>>()
        .ok_or_else(|| {
            runtime.poison();
            AdapterFault::new(
                AdapterPhase::Execute,
                crate::error::AdapterFaultKind::TypeMismatch,
            )
        })?;
    if !replace {
        batch.try_reserve_for_len(needed)?;
    }

    if batch.retains_binding(resource) {
        if !batch.retains_capability(capability) {
            runtime.poison();
            return Err(AdapterFault::invariant(AdapterPhase::Execute).into());
        }
    } else {
        if batch.has_retained_binding() && dispose_pooled_terminal_resource(runtime, batch).is_err()
        {
            return Err(pooled_resource_panic());
        }
        batch.retain_binding(resource, capability);
    }
    Ok(batch)
}

#[inline(always)]
fn ensure_ordinary_pool_prefix_disposed(
    runtime: &Runtime,
    items: &mut [Option<Box<dyn ErasedItem>>],
    disposed_prefix: &mut usize,
    prefix_len: usize,
) -> Result<(), AccessError> {
    debug_assert!(*disposed_prefix <= items.len());
    let target = prefix_len.min(items.len());
    if *disposed_prefix >= target {
        return Ok(());
    }
    dispose_ordinary_pool_prefix_slow(runtime, items, disposed_prefix, target)
}

#[cold]
#[inline(never)]
fn dispose_ordinary_pool_prefix_slow(
    runtime: &Runtime,
    items: &mut [Option<Box<dyn ErasedItem>>],
    disposed_prefix: &mut usize,
    target: usize,
) -> Result<(), AccessError> {
    debug_assert!(*disposed_prefix <= items.len());
    debug_assert!(target <= items.len());
    let start = *disposed_prefix;
    debug_assert!(start < target);
    for (offset, pooled) in items[start..target].iter_mut().enumerate() {
        let disposal = pooled.as_mut().map_or(Ok(()), |item| {
            dispose_pooled_resource(runtime, item.as_mut())
        });
        // `dispose_retained_resource` takes the handle before it can unwind,
        // so this slot is sanitized on both the success and panic paths.
        *disposed_prefix = start + offset + 1;
        if disposal.is_err() {
            return Err(pooled_resource_panic());
        }
    }
    Ok(())
}

/// Initializes and activates one typed item without carrying the caller's
/// operation or result type through the resource-rebinding path.
///
/// The common same-binding prefix is deliberately small enough to inline into
/// the typed access path. Rebinding and high-water extension remain outlined,
/// preserving their adapter-lifetime ordering without inflating every caller.
/// On success the item is abort-visible before control returns to the caller.
#[inline]
fn activate_typed_batch_item<A, const NEW_BINDING: bool>(
    runtime: &Runtime,
    batch: &mut TypedItemBatch<A>,
    item_count: &mut usize,
    resource: &RegisteredResource<A>,
    key: A::Key,
) -> Result<usize, AccessError>
where
    A: TransactionalResource,
{
    let item_slot = *item_count;
    debug_assert_eq!(batch.active_len(), item_slot);
    if item_slot < batch.pooled_len() {
        let retains_binding = {
            let pooled = batch
                .pooled_item_mut(item_slot)
                .expect("typed batch pooled slot remains allocated");
            pooled.retains_binding(resource)
        };
        if !retains_binding {
            return activate_rebound_typed_batch_item::<A, NEW_BINDING>(
                runtime, batch, item_count, resource, key,
            );
        }

        let direct_capability = if NEW_BINDING {
            direct_capability_for_new_binding(runtime, resource)?
        } else {
            None
        };
        {
            let pooled = batch
                .pooled_item_mut(item_slot)
                .expect("typed batch pooled slot remains allocated");
            let local = initialize_local(runtime, resource, &key)?;
            pooled.reinitialize_same_binding(resource, key, local);
        }
        batch.activate_reinitialized::<NEW_BINDING>(direct_capability);
    } else {
        return activate_rebound_typed_batch_item::<A, NEW_BINDING>(
            runtime, batch, item_count, resource, key,
        );
    }
    *item_count += 1;
    Ok(item_slot)
}

/// Handles the less common typed-pool binding replacement and high-water
/// extension paths. A different retained binding is disposed under its panic
/// boundary before the new adapter's item initializer runs, matching the
/// ordinary pool's observable lifetime order.
#[inline(never)]
fn activate_rebound_typed_batch_item<A, const NEW_BINDING: bool>(
    runtime: &Runtime,
    batch: &mut TypedItemBatch<A>,
    item_count: &mut usize,
    resource: &RegisteredResource<A>,
    key: A::Key,
) -> Result<usize, AccessError>
where
    A: TransactionalResource,
{
    let item_slot = *item_count;
    debug_assert_eq!(batch.active_len(), item_slot);
    if item_slot < batch.pooled_len() {
        let pooled = batch
            .pooled_item_mut(item_slot)
            .expect("typed batch pooled slot remains allocated");
        debug_assert!(!pooled.retains_binding(resource));
        if dispose_pooled_typed_resource(runtime, pooled).is_err() {
            return Err(pooled_resource_panic());
        }
        let direct_capability = if NEW_BINDING {
            direct_capability_for_new_binding(runtime, resource)?
        } else {
            None
        };
        let local = initialize_local(runtime, resource, &key)?;
        pooled.reinitialize(resource, key, local);
        batch.activate_reinitialized::<NEW_BINDING>(direct_capability);
    } else {
        let direct_capability = if NEW_BINDING {
            direct_capability_for_new_binding(runtime, resource)?
        } else {
            None
        };
        let local = initialize_local(runtime, resource, &key)?;
        batch.push_active::<NEW_BINDING>(
            ItemData::new(resource.clone(), key, local),
            direct_capability,
        );
    }
    *item_count += 1;
    Ok(item_slot)
}

#[inline]
fn direct_capability_for_new_binding<A: TransactionalResource>(
    runtime: &Runtime,
    resource: &RegisteredResource<A>,
) -> Result<Option<&'static crate::direct_commit::DirectCommitCapability<A>>, AccessError> {
    match catch_unwind(AssertUnwindSafe(|| {
        resource.adapter().direct_commit_capability()
    })) {
        Ok(capability) => Ok(capability),
        Err(_) => {
            runtime.poison();
            Err(
                AdapterFault::new(AdapterPhase::Execute, crate::error::AdapterFaultKind::Panic)
                    .into(),
            )
        }
    }
}

fn prepare_typed_unique_batch<'batch, A: TransactionalResource>(
    runtime: &Runtime,
    batch_slot: &'batch mut Option<Box<dyn ErasedItemBatch>>,
    needed: usize,
) -> Result<&'batch mut TypedItemBatch<A>, AccessError> {
    let replace = batch_slot
        .as_ref()
        .is_some_and(|batch| batch.adapter_type_id() != TypeId::of::<A>());

    if replace || batch_slot.is_none() {
        // Reserve the complete candidate before disposing an incompatible
        // retained pool. A recoverable allocation failure is therefore
        // mutation-free with respect to adapter/resource lifetime.
        let mut replacement = TypedItemBatch::<A>::new();
        replacement.try_reserve_for_len(needed)?;

        if replace {
            let batch = batch_slot
                .as_mut()
                .expect("a replacement candidate remains present");
            if dispose_pooled_batch_resources(runtime, batch.as_mut()).is_err() {
                return Err(pooled_resource_panic());
            }
        }
        *batch_slot = Some(Box::new(replacement));
    }

    let batch = batch_slot
        .as_mut()
        .expect("typed batch storage was installed");
    if batch.active_len() != 0 {
        runtime.poison();
        return Err(AdapterFault::invariant(AdapterPhase::Execute).into());
    }
    let batch = batch
        .as_any_mut()
        .downcast_mut::<TypedItemBatch<A>>()
        .ok_or_else(|| {
            runtime.poison();
            AdapterFault::new(
                AdapterPhase::Execute,
                crate::error::AdapterFaultKind::TypeMismatch,
            )
        })?;
    if !replace {
        batch.try_reserve_for_len(needed)?;
    }
    Ok(batch)
}

#[inline]
fn initialize_local<A: TransactionalResource>(
    runtime: &Runtime,
    resource: &RegisteredResource<A>,
    key: &A::Key,
) -> Result<A::Local, AccessError> {
    match catch_unwind(AssertUnwindSafe(|| resource.adapter().new_local(key))) {
        Ok(Ok(local)) => Ok(local),
        Ok(Err(error)) => Err(item_init_access_error(error)),
        Err(_) => {
            runtime.poison();
            Err(AdapterFault::new(
                AdapterPhase::ItemInit,
                crate::error::AdapterFaultKind::Panic,
            )
            .into())
        }
    }
}

#[inline]
pub(crate) fn item_hash<A: TransactionalResource>(
    resource: &RegisteredResource<A>,
    key: &A::Key,
) -> u64 {
    let mut hasher = ItemHasher::for_resource(resource);
    key.hash(&mut hasher);
    hasher.finish()
}

fn dispose_pooled_resource(runtime: &Runtime, item: &mut dyn ErasedItem) -> Result<(), ()> {
    match catch_unwind(AssertUnwindSafe(|| item.dispose_retained_resource())) {
        Ok(()) => Ok(()),
        Err(_) => {
            runtime.poison();
            Err(())
        }
    }
}

fn dispose_pooled_typed_resource<A: TransactionalResource>(
    runtime: &Runtime,
    item: &mut ItemData<A>,
) -> Result<(), ()> {
    match catch_unwind(AssertUnwindSafe(|| item.dispose_retained_resource())) {
        Ok(()) => Ok(()),
        Err(_) => {
            runtime.poison();
            Err(())
        }
    }
}

fn dispose_pooled_batch_resources(
    runtime: &Runtime,
    batch: &mut dyn ErasedItemBatch,
) -> Result<(), ()> {
    match catch_unwind(AssertUnwindSafe(|| batch.dispose_retained_resources())) {
        Ok(()) => Ok(()),
        Err(_) => {
            runtime.poison();
            Err(())
        }
    }
}

fn dispose_pooled_terminal_resource(
    runtime: &Runtime,
    batch: &mut dyn ErasedTerminalReadBatch,
) -> Result<(), ()> {
    match catch_unwind(AssertUnwindSafe(|| batch.dispose_retained_resource())) {
        Ok(()) => Ok(()),
        Err(_) => {
            runtime.poison();
            Err(())
        }
    }
}

#[cold]
fn pooled_resource_panic() -> AccessError {
    AdapterFault::new(
        AdapterPhase::ItemInit,
        crate::error::AdapterFaultKind::Panic,
    )
    .into()
}

#[cfg(test)]
mod item_index_tests {
    use super::{
        Active, ErasedTerminalReadBatch, ItemIndex, ItemIndexEntry, ItemIndexProbe,
        TerminalReadFrame, TerminalReadScratch, Transaction, TransactionFrame, TransactionScratch,
        UniqueItemKeyIndex, UniqueItemKeys, ValidatedBindings,
    };
    use crate::{
        AccessError, CapacityError, CheckError, ExecutionCheckContext, FinishContext,
        FinishDisposition, FinishItem, InstallContext, InstallItem, InvalidUse, ItemInitError,
        NoPredicate, ObservationOrder, OpacityToken, PredicateContext, PreflightContext,
        PreflightItem, PrepareError, RegisteredResource, ResourceClass, Runtime, RuntimeConfig,
        TransactionalResource, TxnCell, ValidationContext, WorkerContext,
    };
    use std::{
        num::NonZeroUsize,
        panic::{catch_unwind, AssertUnwindSafe},
        sync::Arc,
    };

    struct CacheObservation;

    impl OpacityToken for CacheObservation {
        fn observation_order(&self) -> ObservationOrder {
            ObservationOrder::Unordered
        }
    }

    struct CacheAdapter;

    impl TransactionalResource for CacheAdapter {
        type Key = u64;
        type Local = ();
        type Observation = CacheObservation;
        type Predicate = NoPredicate;
        type Intent = ();
        type Prepared = ();

        fn new_local(&self, _key: &Self::Key) -> Result<Self::Local, ItemInitError> {
            Ok(())
        }

        fn preflight(
            &self,
            _key: &Self::Key,
            _item: PreflightItem<'_, Self>,
            _cx: &mut PreflightContext<'_>,
        ) -> Result<Self::Prepared, PrepareError> {
            Ok(())
        }

        fn revalidate_read(
            &self,
            _key: &Self::Key,
            _observation: &Self::Observation,
            _cx: &ExecutionCheckContext<'_>,
        ) -> Result<(), CheckError> {
            Ok(())
        }

        fn revalidate_predicate(
            &self,
            _key: &Self::Key,
            predicate: &Self::Predicate,
            _cx: &ExecutionCheckContext<'_>,
        ) -> Result<ObservationOrder, CheckError> {
            match *predicate {}
        }

        fn upgrade_predicate(
            &self,
            _key: &Self::Key,
            predicate: &Self::Predicate,
            _prepared: &Self::Prepared,
            _cx: &PredicateContext<'_>,
        ) -> Result<Self::Observation, CheckError> {
            match *predicate {}
        }

        fn validate_read(
            &self,
            _key: &Self::Key,
            _observation: &Self::Observation,
            _prepared: &Self::Prepared,
            _cx: &ValidationContext<'_>,
        ) -> Result<(), CheckError> {
            Ok(())
        }

        fn install(
            &self,
            _key: &Self::Key,
            _item: InstallItem<'_, Self>,
            _prepared: &mut Self::Prepared,
            _cx: &mut InstallContext<'_>,
        ) {
        }

        fn finish(
            &self,
            _key: &Self::Key,
            _item: FinishItem<'_, Self>,
            _prepared: Option<&mut Self::Prepared>,
            _disposition: FinishDisposition,
            _cx: &mut FinishContext<'_>,
        ) {
        }
    }

    fn cache_resource(runtime: &Arc<Runtime>) -> RegisteredResource<CacheAdapter> {
        runtime
            .register_object()
            .unwrap()
            .register_resource(ResourceClass::new(91).unwrap(), CacheAdapter)
            .unwrap()
    }

    fn cached_bindings(transaction: &Transaction<'_, Active>) -> Vec<NonZeroUsize> {
        transaction
            .frame
            .as_ref()
            .expect("transaction is active")
            .validated_bindings
            .entries()
            .collect()
    }

    #[test]
    fn unique_key_index_generation_wrap_clears_stale_buckets() {
        let first = [1_u64, 2, 3];
        let mut index = UniqueItemKeyIndex::with_capacity(first.len());
        assert!(UniqueItemKeys::try_new_hashed(&first, &mut index)
            .unwrap()
            .is_some());
        assert_eq!(
            index
                .entries
                .iter()
                .filter(|entry| entry.generation == index.generation)
                .count(),
            first.len()
        );

        index.generation = u32::MAX;
        let second = [4_u64, 5, 6, 7];
        assert!(UniqueItemKeys::try_new_hashed(&second, &mut index)
            .unwrap()
            .is_some());
        assert_eq!(index.generation, 1);
        assert_eq!(
            index
                .entries
                .iter()
                .filter(|entry| entry.generation == index.generation)
                .count(),
            second.len()
        );
    }

    #[test]
    fn exact_resource_item_query_covers_typed_and_materialized_storage() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let first = cache_resource(&runtime);
        let first_clone = first.clone();
        let second = cache_resource(&runtime);
        let cell = TxnCell::new(&runtime, 7_u64).unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        assert!(!transaction.has_items_for(&first));
        assert!(!transaction.has_items_for(&second));
        transaction.with_item(&first, 1, |_| Ok(())).unwrap();
        assert!(transaction.frame.as_ref().unwrap().batch_active);
        assert!(transaction.has_items_for(&first));
        assert!(transaction.has_items_for(&first_clone));
        assert!(!transaction.has_items_for(&second));

        transaction.with_item(&second, 2, |_| Ok(())).unwrap();
        assert!(transaction.frame.as_ref().unwrap().batch_active);
        assert!(transaction.has_items_for(&first));
        assert!(transaction.has_items_for(&second));

        assert_eq!(cell.get(&mut transaction).unwrap(), 7);
        assert!(!transaction.frame.as_ref().unwrap().batch_active);
        assert!(transaction.has_items_for(&first));
        assert!(transaction.has_items_for(&second));
        transaction.abort();
    }

    #[test]
    fn compact_index_entry_remains_two_machine_words() {
        assert_eq!(std::mem::size_of::<ItemIndexEntry>(), 16);
        assert_eq!(std::mem::align_of::<ItemIndexEntry>(), 8);
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn validated_binding_cache_is_one_word_without_growing_worker_scratch() {
        assert_eq!(std::mem::size_of::<Option<NonZeroUsize>>(), 8);
        assert_eq!(std::mem::size_of::<ValidatedBindings>(), 8);
        assert_eq!(std::mem::align_of::<ValidatedBindings>(), 8);
        assert_eq!(std::mem::size_of::<TransactionScratch>(), 168);
        assert_eq!(std::mem::size_of::<TransactionFrame>(), 200);
        assert_eq!(std::mem::size_of::<Option<TransactionFrame>>(), 200);
        assert_eq!(std::mem::size_of::<Transaction<'static, Active>>(), 208);
        assert_eq!(std::mem::size_of::<WorkerContext>(), 208);
    }

    #[test]
    fn validated_binding_cache_keeps_only_the_most_recent_binding() {
        let identities = [1, 2].map(|value| NonZeroUsize::new(value).unwrap());
        let mut bindings = ValidatedBindings::default();

        assert!(!bindings.activate(identities[0]));
        bindings.admit(identities[0]);
        assert!(bindings.activate(identities[0]));
        assert!(!bindings.activate(identities[1]));
        bindings.admit(identities[1]);
        assert_eq!(bindings.entries().collect::<Vec<_>>(), vec![identities[1]]);
        assert!(!bindings.activate(identities[0]));
        assert!(bindings.activate(identities[1]));
    }

    #[test]
    fn empty_session_and_batch_restore_the_prior_live_binding_cache() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let retained = cache_resource(&runtime);
        let resource = cache_resource(&runtime);
        let binding = resource.binding_identity();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        transaction.with_item(&retained, 1, |_| Ok(())).unwrap();
        let prior = cached_bindings(&transaction);
        transaction
            .with_item_session(&resource, |_session| Ok(()))
            .unwrap();
        assert_eq!(cached_bindings(&transaction), prior);
        transaction.abort();

        let mut transaction = worker.begin().unwrap();
        let empty = [];
        let empty = UniqueItemKeys::try_new(&empty).unwrap();
        transaction
            .with_item_session(&resource, |session| {
                assert!(session.try_with_unique_item_batch(empty, |_, _| Ok(()))?);
                Ok(())
            })
            .unwrap();
        assert!(cached_bindings(&transaction).is_empty());

        let empty = [];
        let empty = UniqueItemKeys::try_new(&empty).unwrap();
        transaction
            .with_unique_item_batch(&resource, empty, |_, _| Ok(()))
            .unwrap();
        assert!(cached_bindings(&transaction).is_empty());

        // Neither empty scope retained this binding in an item, so its
        // allocation may disappear without leaving a reusable address behind.
        drop(resource);
        assert!(!cached_bindings(&transaction).contains(&binding));
        transaction.abort();
    }

    #[test]
    fn failed_or_unwound_access_is_doomed_before_a_cached_address_can_be_reused() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let resource = cache_resource(&runtime);
        let binding = resource.binding_identity();
        let mut worker = runtime.attach().unwrap();

        let mut failed = worker.begin().unwrap();
        assert_eq!(
            failed.with_resolved_item(
                &resource,
                || Err(InvalidUse::IllegalItemState.into()),
                || panic!("a failed lookup must not create an item"),
                |_, ()| Ok(())
            ),
            Err(AccessError::InvalidUse(InvalidUse::IllegalItemState))
        );
        assert_eq!(cached_bindings(&failed), vec![binding]);
        assert!(failed.is_doomed());
        failed.abort();

        let mut unwound = worker.begin().unwrap();
        let panic = catch_unwind(AssertUnwindSafe(|| {
            let _: Result<(), AccessError> =
                unwound.with_item(&resource, 1, |_| panic!("injected operation panic"));
        }));
        assert!(panic.is_err());
        assert_eq!(cached_bindings(&unwound), vec![binding]);
        assert!(unwound.is_doomed());
        unwound.abort();
    }

    #[test]
    fn four_resource_round_robin_keeps_latest_binding_and_new_attempt_resets_it() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let resources: Vec<_> = (0..4).map(|_| cache_resource(&runtime)).collect();
        let identities: Vec<_> = resources
            .iter()
            .map(RegisteredResource::binding_identity)
            .collect();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        for (key, resource) in resources.iter().enumerate() {
            transaction
                .with_item(resource, key as u64, |_| Ok(()))
                .unwrap();
        }
        for (key, resource) in resources.iter().enumerate().cycle().take(12) {
            transaction
                .with_item(resource, key as u64, |_| Ok(()))
                .unwrap();
            assert_eq!(
                transaction
                    .frame
                    .as_ref()
                    .unwrap()
                    .validated_bindings
                    .current,
                Some(resource.binding_identity())
            );
        }
        assert_eq!(
            cached_bindings(&transaction),
            vec![*identities.last().unwrap()]
        );
        transaction.abort();

        let transaction = worker.begin().unwrap();
        assert!(cached_bindings(&transaction).is_empty());
        transaction.abort();
    }

    #[test]
    fn wrong_runtime_binding_is_not_admitted_or_allowed_to_run() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let foreign_runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let local = cache_resource(&runtime);
        let foreign = cache_resource(&foreign_runtime);
        let foreign_binding = foreign.binding_identity();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        transaction.with_item(&local, 1, |_| Ok(())).unwrap();
        let before = cached_bindings(&transaction);
        let operation_ran = std::cell::Cell::new(false);
        assert_eq!(
            transaction.with_item(&foreign, 1, |_| {
                operation_ran.set(true);
                Ok(())
            }),
            Err(AccessError::InvalidUse(InvalidUse::WrongRuntime))
        );
        assert!(!operation_ran.get());
        assert!(!cached_bindings(&transaction).contains(&foreign_binding));
        assert_eq!(cached_bindings(&transaction), before);
        assert!(transaction.is_doomed());
        transaction.abort();
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn terminal_read_frame_remains_independent_and_minimal() {
        assert_eq!(
            std::mem::size_of::<Option<Box<dyn ErasedTerminalReadBatch>>>(),
            16
        );
        assert_eq!(std::mem::size_of::<TerminalReadScratch>(), 24);
        assert_eq!(std::mem::size_of::<Box<TerminalReadScratch>>(), 8);
        assert_eq!(std::mem::size_of::<Option<Box<TerminalReadScratch>>>(), 8);
        assert_eq!(std::mem::size_of::<TerminalReadFrame>(), 16);
        assert_eq!(std::mem::size_of::<Option<TerminalReadFrame>>(), 16);
    }

    #[test]
    fn same_adapter_bindings_remain_typed_until_adapter_type_changes() {
        let runtime = Runtime::new(RuntimeConfig::default()).unwrap();
        let first = TxnCell::new(&runtime, 11_u64).unwrap();
        let second = TxnCell::new(&runtime, 22_u64).unwrap();
        let different_adapter = TxnCell::new(&runtime, 33_i64).unwrap();
        let mut worker = runtime.attach().unwrap();
        let mut transaction = worker.begin().unwrap();

        assert_eq!(first.get(&mut transaction).unwrap(), 11);
        assert_eq!(second.get(&mut transaction).unwrap(), 22);
        assert_eq!(first.get(&mut transaction).unwrap(), 11);

        let frame = transaction.frame.as_ref().expect("transaction is active");
        assert!(frame.batch_active);
        assert_eq!(frame.item_count, 2);
        assert_eq!(
            frame
                .unique_batch
                .as_ref()
                .expect("the typed lane retains its batch")
                .active_len(),
            2
        );
        assert!(frame.items.is_empty());

        assert_eq!(different_adapter.get(&mut transaction).unwrap(), 33);

        let frame = transaction.frame.as_ref().expect("transaction is active");
        assert!(!frame.batch_active);
        assert_eq!(frame.item_count, 3);
        assert_eq!(
            frame
                .unique_batch
                .as_ref()
                .expect("materialized storage remains pooled")
                .active_len(),
            0
        );
        assert_eq!(frame.items.len(), 3);
        assert!(frame.items.iter().all(Option::is_some));

        transaction.abort();
    }

    #[test]
    fn colliding_entries_remain_distinct_across_generation_reuse() {
        let mut index = ItemIndex::default();
        index.begin_transaction();
        for slot in 0..32 {
            index.try_reserve_for_insert().unwrap();
            index.insert(7, slot);
        }
        for wanted in 0..32 {
            assert_eq!(index.find(7, |slot| slot == wanted), Some(wanted));
        }
        assert_eq!(index.find(7, |_| false), None);

        let allocation = index.entries.as_ptr();
        let capacity = index.capacity();
        index.begin_transaction();
        assert_eq!(index.find(7, |_| true), None);

        for slot in 32..64 {
            index.try_reserve_for_insert().unwrap();
            index.insert(7, slot);
        }
        assert_eq!(index.entries.as_ptr(), allocation);
        assert_eq!(index.capacity(), capacity);
        for wanted in 32..64 {
            assert_eq!(index.find(7, |slot| slot == wanted), Some(wanted));
        }
    }

    #[test]
    fn fresh_index_probe_reuses_vacancy_and_recomputes_it_after_growth() {
        let mut index = ItemIndex::default();
        index.begin_transaction();
        for slot in 0..3 {
            index.try_reserve_for_insert().unwrap();
            index.insert(7, slot);
        }

        let ItemIndexProbe::Vacant(stable) = index.probe(7, |_| false) else {
            panic!("a distinct colliding identity must find a vacant bucket");
        };
        let stable_bucket = index.try_reserve_vacancy(7, stable).unwrap();
        assert_eq!(stable_bucket, stable.bucket);
        index.insert_at(stable_bucket, 7, 3);

        let ItemIndexProbe::Vacant(before_growth) = index.probe(7, |_| false) else {
            panic!("a fresh identity must retain the next collision vacancy");
        };
        let old_table_len = index.entries.len();
        let grown_bucket = index.try_reserve_vacancy(7, before_growth).unwrap();
        assert!(index.entries.len() > old_table_len);
        assert_eq!(grown_bucket, index.vacant_bucket(7));
        index.insert_at(grown_bucket, 7, 4);

        for wanted in 0..5 {
            assert_eq!(index.find(7, |slot| slot == wanted), Some(wanted));
        }
        assert_eq!(index.find(7, |_| false), None);
    }

    #[test]
    fn impossible_index_growth_reports_the_public_item_limit() {
        let mut index = ItemIndex::default();
        index.begin_transaction();
        index.len = usize::MAX;
        assert_eq!(
            index.try_reserve_for_insert(),
            Err(CapacityError::ItemLimit)
        );
    }
}

fn item_init_access_error(error: ItemInitError) -> AccessError {
    match error {
        ItemInitError::Capacity(capacity) => capacity.into(),
        ItemInitError::Fault(fault) => fault.into(),
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CommitBoundary {
    Reversible,
    Irrevocable,
    Published,
}

struct CommitDriver<'worker, 'hook> {
    worker: &'worker mut WorkerContext,
    frame: Option<TransactionFrame>,
    locks: Option<LockPlan>,
    // The concrete plan is retained inside the exact typed batch so its
    // allocation can be reused without adding another transaction-frame word.
    direct_lane: bool,
    commit_id: Option<crate::identity::OccCommitId>,
    boundary: CommitBoundary,
    phase: FailurePhase,
    completed: bool,
    hook: Option<&'hook mut dyn CommitHook>,
}

impl<'worker, 'hook> CommitDriver<'worker, 'hook> {
    fn new(
        worker: &'worker mut WorkerContext,
        frame: TransactionFrame,
        hook: Option<&'hook mut dyn CommitHook>,
    ) -> Self {
        Self {
            worker,
            frame: Some(frame),
            locks: None,
            direct_lane: false,
            commit_id: None,
            boundary: CommitBoundary::Reversible,
            phase: FailurePhase::Preflight,
            completed: false,
            hook,
        }
    }

    fn run(&mut self) -> Result<CommitOutcome, CommitFailure> {
        self.phase = FailurePhase::Preflight;
        if self.frame().doomed {
            let poison = self
                .worker
                .runtime
                .ensure_healthy(FailurePhase::Execution)
                .err();
            return self.abort_commit(AbortReason::Doomed, poison);
        }
        if let Err(info) = self.frame().runtime.ensure_healthy(FailurePhase::Preflight) {
            return self.abort_commit(AbortReason::Doomed, Some(info));
        }

        // The empty transaction and a transaction made entirely from the
        // explicit prepared-free ordinary-read capability own no physical
        // lock uses. Certify them directly, preserving the same final
        // validation cut while leaving the worker's pooled lock plan intact.
        if self.frame().is_preflight_free_read_only() {
            return self.commit_preflight_free_read_only();
        }

        // A homogeneous typed batch is used by scalar point accesses as well
        // as explicit unique batches. Eligibility therefore checks the stable
        // capability of every live resource binding; batch_active or a shared
        // adapter TypeId alone is not a proof and scans/directories fall back.
        // Execution has already folded each distinct exact binding's stable
        // capability, so this selection is constant-time.
        let direct_selected = if self.frame().batch_active {
            self.frame
                .as_mut()
                .expect("commit driver retains its transaction frame")
                .unique_batch
                .as_mut()
                .expect("an active unique batch retains its storage")
                .select_direct_commit()
        } else {
            false
        };
        if direct_selected {
            self.direct_lane = true;
            return self.run_direct();
        }

        let runtime_id = self.frame().runtime.id();
        let max_locks = self.frame().runtime.config().max_locks_per_transaction();
        // Consume the per-owner generation before invoking any adapter
        // preflight callback. Even if preflight aborts or unwinds after
        // retaining a LockUse, no later plan can reuse that token's identity.
        let plan_nonce = match self.worker.reserve_lock_plan_nonce() {
            Ok(nonce) => nonce,
            Err(error) => return self.abort_commit(error.into(), None),
        };
        let storage = std::mem::take(
            &mut self
                .frame
                .as_mut()
                .expect("commit driver owns its frame")
                .lock_storage,
        );
        let lock_plan = match LockPlan::with_storage(runtime_id, max_locks, plan_nonce, storage) {
            Ok(plan) => plan,
            Err(error) => return self.abort_commit(error.into(), None),
        };
        self.locks = Some(lock_plan);

        if self.frame().batch_active {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let mut cx = locks.preflight_context()?;
                frame
                    .unique_batch
                    .as_mut()
                    .expect("an active unique batch retains its storage")
                    .preflight(&mut cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_prepare_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Preflight,
                            "preflight callback panicked",
                        )),
                        Some(self.poison(FailurePhase::Preflight, "preflight callback panicked")),
                    );
                }
            }
        } else {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let mut cx = locks.preflight_context()?;
                for item_slot in 0..frame.item_count {
                    frame.items[item_slot]
                        .as_mut()
                        .expect("commit owns every live item slot")
                        .preflight(&mut cx)?;
                }
                Ok::<(), PrepareError>(())
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_prepare_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Preflight,
                            "preflight callback panicked",
                        )),
                        Some(self.poison(FailurePhase::Preflight, "preflight callback panicked")),
                    );
                }
            }
        }

        // Preflight cannot add/remove an intent or observation through its
        // restricted item view, so this shape remains stable for the rest of
        // the attempt. Cache it once instead of rescanning every item at each
        // commit phase.
        let commit_shape = self.frame().commit_shape();

        self.phase = FailurePhase::Acquire;
        let owner = self.worker.owner;
        let acquire = catch_unwind(AssertUnwindSafe(|| self.locks_mut().acquire_all(owner)));
        match acquire {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return self.handle_acquire_error(error),
            Err(_) => return self.handle_acquire_panic(),
        }

        if commit_shape.has_writes && self.hook.is_some() {
            self.phase = FailurePhase::UpperMetadata;
            let reservation = catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::reserve_upper_metadata)
            }));
            match reservation {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.abort_for_hook_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::UpperMetadata,
                            "upper metadata reservation panicked",
                        )),
                        Some(self.poison(
                            FailurePhase::UpperMetadata,
                            "upper metadata reservation panicked",
                        )),
                    )
                }
            }
        }

        if commit_shape.has_predicates {
            self.phase = FailurePhase::PredicateUpgrade;
            if self.frame().batch_active {
                let result = catch_unwind(AssertUnwindSafe(|| {
                    let (frame, locks) = self.parts_mut();
                    let cx = locks.predicate_context().map_err(CheckError::from)?;
                    frame
                        .unique_batch
                        .as_mut()
                        .expect("an active unique batch retains its storage")
                        .upgrade_predicates(&cx)
                }));
                match result {
                    Ok(Ok(())) => {}
                    Ok(Err(error)) => {
                        return self.handle_check_error(error, FailurePhase::PredicateUpgrade)
                    }
                    Err(_) => {
                        return self.abort_commit(
                            AbortReason::Internal(InternalError::new(
                                FailurePhase::PredicateUpgrade,
                                "predicate callback panicked",
                            )),
                            Some(self.poison(
                                FailurePhase::PredicateUpgrade,
                                "predicate callback panicked",
                            )),
                        )
                    }
                }
            } else {
                let result = catch_unwind(AssertUnwindSafe(|| {
                    let (frame, locks) = self.parts_mut();
                    let cx = locks.predicate_context().map_err(CheckError::from)?;
                    for item_slot in 0..frame.item_count {
                        frame.items[item_slot]
                            .as_mut()
                            .expect("commit owns every live item slot")
                            .upgrade_predicate(&cx)?;
                    }
                    Ok::<(), CheckError>(())
                }));
                match result {
                    Ok(Ok(())) => {}
                    Ok(Err(error)) => {
                        return self.handle_check_error(error, FailurePhase::PredicateUpgrade)
                    }
                    Err(_) => {
                        return self.abort_commit(
                            AbortReason::Internal(InternalError::new(
                                FailurePhase::PredicateUpgrade,
                                "predicate callback panicked",
                            )),
                            Some(self.poison(
                                FailurePhase::PredicateUpgrade,
                                "predicate callback panicked",
                            )),
                        )
                    }
                }
            }
        }

        if commit_shape.has_writes {
            self.phase = FailurePhase::CommitMetadata;
            self.commit_id = match self.frame().runtime.reserve_commit_id() {
                Ok(commit_id) => Some(commit_id),
                Err(error) => return self.abort_commit(error.into(), None),
            };
        }

        self.phase = FailurePhase::Validation;
        if self.frame().batch_active {
            let commit_id = self.commit_id;
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let cx = locks
                    .validation_context(commit_id)
                    .map_err(CheckError::from)?;
                frame
                    .unique_batch
                    .as_ref()
                    .expect("an active unique batch retains its storage")
                    .validate(&cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_check_error(error, FailurePhase::Validation),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Validation,
                            "validation callback panicked",
                        )),
                        Some(self.poison(FailurePhase::Validation, "validation callback panicked")),
                    )
                }
            }
        } else {
            let commit_id = self.commit_id;
            let result = catch_unwind(AssertUnwindSafe(|| {
                let (frame, locks) = self.parts_mut();
                let cx = locks
                    .validation_context(commit_id)
                    .map_err(CheckError::from)?;
                for item_slot in 0..frame.item_count {
                    frame.items[item_slot]
                        .as_ref()
                        .expect("commit owns every live item slot")
                        .validate(&cx)?;
                }
                Ok::<(), CheckError>(())
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_check_error(error, FailurePhase::Validation),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Validation,
                            "validation callback panicked",
                        )),
                        Some(self.poison(FailurePhase::Validation, "validation callback panicked")),
                    )
                }
            }
        }

        if commit_shape.has_writes && self.hook.is_some() {
            self.phase = FailurePhase::PreinstallHook;
            let acceptance = catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::pre_install)
            }));
            match acceptance {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.abort_for_hook_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::PreinstallHook,
                            "pre-install hook panicked",
                        )),
                        Some(
                            self.poison(FailurePhase::PreinstallHook, "pre-install hook panicked"),
                        ),
                    )
                }
            }
        }

        self.boundary = CommitBoundary::Irrevocable;
        if commit_shape.has_writes {
            self.phase = FailurePhase::Install;
            if self.frame().batch_active {
                let commit_id = self.commit_id;
                let result = catch_unwind(AssertUnwindSafe(|| {
                    let (frame, locks) = self.parts_mut();
                    let mut cx = locks
                        .install_context(commit_id)
                        .expect("held plan must construct an install context");
                    frame
                        .unique_batch
                        .as_mut()
                        .expect("an active unique batch retains its storage")
                        .install(&mut cx);
                }));
                if result.is_err() {
                    return self.indeterminate(FailurePhase::Install, "install callback panicked");
                }
            } else {
                let commit_id = self.commit_id;
                let result = catch_unwind(AssertUnwindSafe(|| {
                    let (frame, locks) = self.parts_mut();
                    let mut cx = locks
                        .install_context(commit_id)
                        .expect("held plan must construct an install context");
                    for item_slot in 0..frame.item_count {
                        if !frame.items[item_slot]
                            .as_ref()
                            .expect("commit owns every live item slot")
                            .has_intent()
                        {
                            continue;
                        }
                        frame.items[item_slot]
                            .as_mut()
                            .expect("commit owns every live item slot")
                            .install(&mut cx);
                    }
                }));
                if result.is_err() {
                    return self.indeterminate(FailurePhase::Install, "install callback panicked");
                }
            }
        }

        self.phase = FailurePhase::Release;
        let disposition = LockDisposition::Committed {
            occ_commit_id: self.commit_id,
        };
        let released = catch_unwind(AssertUnwindSafe(|| {
            self.locks_mut().release_all(disposition)
        }));
        match released {
            Ok(Ok(())) => {
                self.boundary = CommitBoundary::Published;
                if self.drop_released_lock_plan().is_err() {
                    return self.finish_committed_with_poison(PoisonInfo::new(
                        FailurePhase::Release,
                        "released lock-plan destruction panicked",
                    ));
                }
            }
            Ok(Err(_)) | Err(_) => {
                return self.indeterminate_after_release_failure(
                    FailurePhase::Release,
                    "lock release failed or panicked",
                )
            }
        }

        self.phase = FailurePhase::Finish;
        let commit_info = CommitInfo::new(self.commit_id);
        if let Err(info) = self.finish_items(FinishDisposition::Committed, FailurePhase::Finish) {
            self.complete_worker();
            return Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(commit_info),
                info,
            });
        }

        self.recycle_frame();
        self.complete_worker();
        Ok(CommitOutcome::Committed(commit_info))
    }

    /// Executes the same commit boundary and hook protocol as the general
    /// lane while keeping one concrete lock-frame vector inside the typed
    /// batch. The selected capability excludes predicates, maps every write
    /// to exactly one distinct lock, and leaves the generic preparation
    /// sidecar empty.
    fn run_direct(&mut self) -> Result<CommitOutcome, CommitFailure> {
        debug_assert!(self.direct_lane);
        debug_assert!(self.locks.is_none());
        debug_assert!(self.frame().batch_active);
        let commit_shape = self.frame().commit_shape();
        debug_assert!(commit_shape.has_writes);
        debug_assert!(!commit_shape.has_predicates);

        self.phase = FailurePhase::Preflight;
        let runtime_id = self.frame().runtime.id();
        let max_locks = self.frame().runtime.config().max_locks_per_transaction();
        let preflight = catch_unwind(AssertUnwindSafe(|| {
            self.direct_batch_mut()
                .direct_preflight(runtime_id, max_locks)
        }));
        match preflight {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return self.handle_prepare_error(error),
            Err(_) => {
                return self.abort_commit(
                    AbortReason::Internal(InternalError::new(
                        FailurePhase::Preflight,
                        "direct preflight callback panicked",
                    )),
                    Some(self.poison(
                        FailurePhase::Preflight,
                        "direct preflight callback panicked",
                    )),
                )
            }
        }

        self.phase = FailurePhase::Acquire;
        let owner = self.worker.owner;
        let acquire = catch_unwind(AssertUnwindSafe(|| {
            self.direct_batch_mut().direct_acquire_all(owner)
        }));
        match acquire {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return self.handle_acquire_error(error),
            Err(_) => return self.handle_acquire_panic(),
        }

        if self.hook.is_some() {
            self.phase = FailurePhase::UpperMetadata;
            let reservation = catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::reserve_upper_metadata)
            }));
            match reservation {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.abort_for_hook_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::UpperMetadata,
                            "upper metadata reservation panicked",
                        )),
                        Some(self.poison(
                            FailurePhase::UpperMetadata,
                            "upper metadata reservation panicked",
                        )),
                    )
                }
            }
        }

        self.phase = FailurePhase::CommitMetadata;
        self.commit_id = match self.frame().runtime.reserve_commit_id() {
            Ok(commit_id) => Some(commit_id),
            Err(error) => return self.abort_commit(error.into(), None),
        };

        self.phase = FailurePhase::Validation;
        let commit_id = self.commit_id;
        let validation = catch_unwind(AssertUnwindSafe(|| {
            self.direct_batch().direct_validate(commit_id)
        }));
        match validation {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return self.handle_check_error(error, FailurePhase::Validation),
            Err(_) => {
                return self.abort_commit(
                    AbortReason::Internal(InternalError::new(
                        FailurePhase::Validation,
                        "direct validation callback panicked",
                    )),
                    Some(self.poison(
                        FailurePhase::Validation,
                        "direct validation callback panicked",
                    )),
                )
            }
        }

        if self.hook.is_some() {
            self.phase = FailurePhase::PreinstallHook;
            let acceptance = catch_unwind(AssertUnwindSafe(|| {
                self.hook
                    .as_deref_mut()
                    .map_or(Ok(()), CommitHook::pre_install)
            }));
            match acceptance {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.abort_for_hook_error(error),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::PreinstallHook,
                            "pre-install hook panicked",
                        )),
                        Some(
                            self.poison(FailurePhase::PreinstallHook, "pre-install hook panicked"),
                        ),
                    )
                }
            }
        }

        self.boundary = CommitBoundary::Irrevocable;
        self.phase = FailurePhase::Install;
        let commit_id = self.commit_id;
        let installation = catch_unwind(AssertUnwindSafe(|| {
            self.direct_batch_mut().direct_install(commit_id);
        }));
        if installation.is_err() {
            return self.indeterminate(FailurePhase::Install, "direct install callback panicked");
        }

        self.phase = FailurePhase::Release;
        let disposition = LockDisposition::Committed {
            occ_commit_id: self.commit_id,
        };
        let released = catch_unwind(AssertUnwindSafe(|| {
            self.direct_batch_mut().direct_release_all(disposition)
        }));
        match released {
            Ok(Ok(())) => {
                self.boundary = CommitBoundary::Published;
                if self.drop_released_direct_plan().is_err() {
                    return self.finish_committed_with_poison(PoisonInfo::new(
                        FailurePhase::Release,
                        "released direct-plan destruction panicked",
                    ));
                }
            }
            Ok(Err(_)) | Err(_) => {
                return self.indeterminate_after_release_failure(
                    FailurePhase::Release,
                    "direct lock release failed or panicked",
                )
            }
        }

        self.phase = FailurePhase::Finish;
        let commit_info = CommitInfo::new(self.commit_id);
        if let Err(info) = self.finish_items(FinishDisposition::Committed, FailurePhase::Finish) {
            self.complete_worker();
            return Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(commit_info),
                info,
            });
        }

        self.recycle_frame();
        self.complete_worker();
        Ok(CommitOutcome::Committed(commit_info))
    }

    fn commit_preflight_free_read_only(&mut self) -> Result<CommitOutcome, CommitFailure> {
        debug_assert!(self.frame().is_preflight_free_read_only());
        debug_assert!(self.locks.is_none());
        debug_assert!(self.commit_id.is_none());

        self.phase = FailurePhase::Validation;
        let mut validation_scope = ();
        let cx = PreflightFreeValidationContext::without_locks(None, &mut validation_scope);
        if self.frame().batch_active {
            let result = catch_unwind(AssertUnwindSafe(|| {
                self.frame
                    .as_mut()
                    .expect("commit driver owns its frame")
                    .unique_batch
                    .as_mut()
                    .expect("an active unique batch retains its storage")
                    .validate_preflight_free_reads(&cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_check_error(error, FailurePhase::Validation),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Validation,
                            "preflight-free validation callback panicked",
                        )),
                        Some(self.poison(
                            FailurePhase::Validation,
                            "preflight-free validation callback panicked",
                        )),
                    )
                }
            }
        } else {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let frame = self.frame.as_mut().expect("commit driver owns its frame");
                for item_slot in 0..frame.item_count {
                    frame.items[item_slot]
                        .as_mut()
                        .expect("commit owns every live item slot")
                        .validate_preflight_free_read(&cx)?;
                }
                Ok::<(), CheckError>(())
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(error)) => return self.handle_check_error(error, FailurePhase::Validation),
                Err(_) => {
                    return self.abort_commit(
                        AbortReason::Internal(InternalError::new(
                            FailurePhase::Validation,
                            "preflight-free validation callback panicked",
                        )),
                        Some(self.poison(
                            FailurePhase::Validation,
                            "preflight-free validation callback panicked",
                        )),
                    )
                }
            }
        }

        // A read-only commit has no installation or publication callback.
        // Successful final certification is therefore its definite commit
        // boundary; cleanup panics after this assignment cannot turn it into
        // an abort or an indeterminate outcome.
        self.boundary = CommitBoundary::Published;
        self.phase = FailurePhase::Finish;
        let commit_info = CommitInfo::new(None);
        if let Err(info) = self.finish_items(FinishDisposition::Committed, FailurePhase::Finish) {
            self.complete_worker();
            return Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(commit_info),
                info,
            });
        }

        self.recycle_frame();
        self.complete_worker();
        Ok(CommitOutcome::Committed(commit_info))
    }

    fn handle_prepare_error(
        &mut self,
        error: PrepareError,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            PrepareError::Conflict(conflict) => self.abort_commit(conflict.into(), None),
            PrepareError::Capacity(capacity) => self.abort_commit(capacity.into(), None),
            PrepareError::Fault(fault) => self.abort_commit(
                AbortReason::Internal(InternalError::new(
                    FailurePhase::Preflight,
                    "adapter fault during preflight",
                )),
                Some(self.poison(
                    adapter_failure_phase(fault),
                    "adapter fault during preflight",
                )),
            ),
        }
    }

    fn abort_for_hook_error(
        &mut self,
        error: CommitHookError,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            CommitHookError::Rejected => self.abort_commit(AbortReason::HookRejected, None),
            CommitHookError::Capacity(capacity) => self.abort_commit(capacity.into(), None),
        }
    }

    fn handle_acquire_error(
        &mut self,
        error: AcquireError,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            AcquireError::Conflict(conflict) => self.abort_commit(conflict.into(), None),
            AcquireError::Fault(fault) => self.abort_commit(
                AbortReason::Internal(InternalError::new(
                    FailurePhase::Acquire,
                    "adapter fault during lock acquisition",
                )),
                Some(self.poison(
                    adapter_failure_phase(fault),
                    "adapter fault during acquisition",
                )),
            ),
        }
    }

    fn handle_acquire_panic(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = FailurePhase::Acquire;
        let reason = AbortReason::Internal(InternalError::new(
            phase,
            "lock acquisition callback panicked",
        ));
        let poison = self.poison(phase, "lock acquisition callback panicked");

        if self.direct_lane {
            let _ = catch_unwind(AssertUnwindSafe(|| {
                self.direct_batch_mut()
                    .direct_recover_after_callback_panic(LockDisposition::Aborted)
            }));
            // The in-progress concrete frame may have acquired before it
            // unwound. Retain the complete typed batch and skip finish exactly
            // as the general plan does for an uncertain erased frame.
            self.quarantine_items();
            self.complete_worker();
            return Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(reason),
                info: poison,
            });
        }

        if let Some(mut locks) = self.locks.take() {
            let _ = catch_unwind(AssertUnwindSafe(|| {
                locks.recover_after_callback_panic(LockDisposition::Aborted)
            }));
            // The in-progress callback frame is uncertain even when recovery
            // released every other definitely acquired guard. Retain the
            // complete plan and all item state: `finish` is a post-unlock
            // callback and therefore cannot run under that uncertainty.
            std::mem::forget(locks);
        }
        self.quarantine_items();
        self.complete_worker();
        Err(CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Aborted(reason),
            info: poison,
        })
    }

    fn handle_check_error(
        &mut self,
        error: CheckError,
        phase: FailurePhase,
    ) -> Result<CommitOutcome, CommitFailure> {
        match error {
            CheckError::Conflict(conflict) => self.abort_commit(conflict.into(), None),
            CheckError::Fault(fault) => self.abort_commit(
                AbortReason::Internal(InternalError::new(phase, "adapter validation fault")),
                Some(self.poison(adapter_failure_phase(fault), "adapter validation fault")),
            ),
        }
    }

    fn abort_commit(
        &mut self,
        reason: AbortReason,
        mut poison: Option<PoisonInfo>,
    ) -> Result<CommitOutcome, CommitFailure> {
        if self.boundary != CommitBoundary::Reversible {
            return self.indeterminate(
                FailurePhase::Install,
                "abort requested after irreversible boundary",
            );
        }

        if self.direct_lane {
            if self.direct_batch().direct_requires_release() {
                let released = catch_unwind(AssertUnwindSafe(|| {
                    self.direct_batch_mut()
                        .direct_release_all(LockDisposition::Aborted)
                }));
                if !matches!(released, Ok(Ok(()))) {
                    let _ = catch_unwind(AssertUnwindSafe(|| {
                        self.direct_batch_mut()
                            .direct_recover_after_callback_panic(LockDisposition::Aborted)
                    }));
                    poison = Some(self.poison(
                        FailurePhase::Release,
                        "abort direct-lock release failed or panicked",
                    ));
                    self.quarantine_items();
                    self.complete_worker();
                    return Err(CommitFailure::Poisoned {
                        outcome: DefiniteOutcome::Aborted(reason),
                        info: poison.expect("abort release failure poisons runtime"),
                    });
                }
            }
            if self.drop_released_direct_plan().is_err() {
                poison = Some(self.poison(
                    FailurePhase::Release,
                    "aborted direct-plan destruction panicked",
                ));
            }
        }

        if let Some(mut locks) = self.locks.take() {
            if locks.requires_release() {
                let released = catch_unwind(AssertUnwindSafe(|| {
                    locks.release_all(LockDisposition::Aborted)
                }));
                if !matches!(released, Ok(Ok(()))) {
                    let _ = catch_unwind(AssertUnwindSafe(|| {
                        locks.recover_after_callback_panic(LockDisposition::Aborted)
                    }));
                    std::mem::forget(locks);
                    poison = Some(self.poison(
                        FailurePhase::Release,
                        "abort lock release failed or panicked",
                    ));
                    self.quarantine_items();
                    self.complete_worker();
                    return Err(CommitFailure::Poisoned {
                        outcome: DefiniteOutcome::Aborted(reason),
                        info: poison.expect("abort release failure poisons runtime"),
                    });
                }
            }
            match teardown_lock_plan(locks) {
                Ok(storage) => {
                    self.frame
                        .as_mut()
                        .expect("reversible abort retains its transaction frame")
                        .lock_storage = storage;
                }
                Err(()) => {
                    poison = Some(self.poison(
                        FailurePhase::Release,
                        "aborted lock-plan destruction panicked",
                    ));
                }
            }
        }

        if let Err(finish_poison) =
            self.finish_items(FinishDisposition::Aborted, FailurePhase::Finish)
        {
            poison = Some(finish_poison);
        }
        self.recycle_frame();
        self.complete_worker();

        match poison {
            Some(info) => Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(reason),
                info,
            }),
            None => Ok(CommitOutcome::Aborted(reason)),
        }
    }

    fn indeterminate(
        &mut self,
        phase: FailurePhase,
        reason: &'static str,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.worker.runtime.mark_indeterminate();
        if self.direct_lane {
            let disposition = LockDisposition::Indeterminate {
                occ_commit_id: self.commit_id,
            };
            let released = catch_unwind(AssertUnwindSafe(|| {
                self.direct_batch_mut().direct_release_all(disposition)
            }));
            match released {
                Ok(Ok(())) => {
                    let _ = self.drop_released_direct_plan();
                }
                Ok(Err(_)) | Err(_) => {
                    let _ = catch_unwind(AssertUnwindSafe(|| {
                        self.direct_batch_mut()
                            .direct_recover_after_callback_panic(disposition)
                    }));
                }
            }
        }
        if let Some(mut locks) = self.locks.take() {
            let disposition = LockDisposition::Indeterminate {
                occ_commit_id: self.commit_id,
            };
            let released = catch_unwind(AssertUnwindSafe(|| locks.release_all(disposition)));
            match released {
                Ok(Ok(())) => {
                    // An indeterminate transaction never places its plan back
                    // into the worker pool, even if all releases returned. The
                    // runtime and transaction frame are quarantined below.
                    let _ = teardown_lock_plan(locks);
                }
                Ok(Err(_)) | Err(_) => {
                    let _ = catch_unwind(AssertUnwindSafe(|| {
                        locks.recover_after_callback_panic(disposition)
                    }));
                    std::mem::forget(locks);
                }
            }
        }
        self.quarantine_items();
        self.complete_worker();
        Err(CommitFailure::Indeterminate(IndeterminateInfo::new(
            phase,
            self.commit_id,
            reason,
        )))
    }

    fn indeterminate_after_release_failure(
        &mut self,
        phase: FailurePhase,
        reason: &'static str,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.worker.runtime.mark_indeterminate();
        if self.direct_lane {
            let disposition = LockDisposition::Indeterminate {
                occ_commit_id: self.commit_id,
            };
            let _ = catch_unwind(AssertUnwindSafe(|| {
                self.direct_batch_mut()
                    .direct_recover_after_callback_panic(disposition)
            }));
        }
        if let Some(mut locks) = self.locks.take() {
            let disposition = LockDisposition::Indeterminate {
                occ_commit_id: self.commit_id,
            };
            let _ = catch_unwind(AssertUnwindSafe(|| {
                locks.recover_after_callback_panic(disposition)
            }));
            std::mem::forget(locks);
        }
        self.quarantine_items();
        self.complete_worker();
        Err(CommitFailure::Indeterminate(IndeterminateInfo::new(
            phase,
            self.commit_id,
            reason,
        )))
    }

    fn drop_released_lock_plan(&mut self) -> Result<(), ()> {
        let Some(locks) = self.locks.take() else {
            return Ok(());
        };
        let storage = teardown_lock_plan(locks)?;
        self.frame
            .as_mut()
            .expect("released plan retains its transaction frame")
            .lock_storage = storage;
        Ok(())
    }

    fn drop_released_direct_plan(&mut self) -> Result<(), ()> {
        if !self.direct_lane {
            return Ok(());
        }
        self.direct_batch_mut().teardown_direct_plan()
    }

    fn finish_committed_with_poison(
        &mut self,
        mut info: PoisonInfo,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.worker.runtime.poison();
        self.phase = FailurePhase::Finish;
        if let Err(finish_poison) =
            self.finish_items(FinishDisposition::Committed, FailurePhase::Finish)
        {
            info = finish_poison;
        }
        self.recycle_frame();
        self.complete_worker();
        Err(CommitFailure::Poisoned {
            outcome: DefiniteOutcome::Committed(CommitInfo::new(self.commit_id)),
            info,
        })
    }

    fn contain_unexpected_unwind(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = self.phase;
        match self.boundary {
            CommitBoundary::Reversible => {
                let reason = AbortReason::Internal(InternalError::new(
                    phase,
                    "unexpected commit-driver unwind",
                ));
                let info = self.poison(phase, "unexpected commit-driver unwind");
                self.abort_commit(reason, Some(info))
            }
            CommitBoundary::Irrevocable => {
                self.indeterminate(phase, "unexpected unwind after irreversible boundary")
            }
            CommitBoundary::Published => self.finish_committed_with_poison(
                self.poison(phase, "unexpected unwind after complete publication"),
            ),
        }
    }

    fn emergency_failure(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = self.phase;
        if let Some(locks) = self.locks.take() {
            std::mem::forget(locks);
        }
        self.quarantine_items();
        self.complete_worker();

        match self.boundary {
            CommitBoundary::Reversible => {
                self.worker.runtime.poison();
                let reason = AbortReason::Internal(InternalError::new(
                    phase,
                    "panic containment itself failed before installation",
                ));
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Aborted(reason),
                    info: PoisonInfo::new(phase, "panic containment itself failed"),
                })
            }
            CommitBoundary::Irrevocable => {
                self.worker.runtime.mark_indeterminate();
                Err(CommitFailure::Indeterminate(IndeterminateInfo::new(
                    phase,
                    self.commit_id,
                    "panic containment itself failed after installation began",
                )))
            }
            CommitBoundary::Published => {
                self.worker.runtime.poison();
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Committed(CommitInfo::new(self.commit_id)),
                    info: PoisonInfo::new(
                        phase,
                        "panic containment itself failed after publication",
                    ),
                })
            }
        }
    }

    fn finish_items(
        &mut self,
        disposition: FinishDisposition,
        phase: FailurePhase,
    ) -> Result<(), PoisonInfo> {
        let Some(frame) = self.frame.as_mut() else {
            return Ok(());
        };
        if frame.batch_active {
            let mut stage = BatchFinishStage::Callback;
            let cleanup = catch_unwind(AssertUnwindSafe(|| {
                let batch = frame
                    .unique_batch
                    .as_mut()
                    .expect("an active unique batch retains its storage");
                let mut cx = FinishContext::new();
                batch.finish_and_teardown(disposition, &mut cx, &mut stage);
            }));
            if cleanup.is_err() {
                frame.runtime.poison();
                let retained = self
                    .frame
                    .take()
                    .expect("frame exists during typed batch cleanup");
                std::mem::forget(retained);
                let reason = match stage {
                    BatchFinishStage::Callback => "finish callback panicked",
                    BatchFinishStage::Teardown => "adapter-owned state drop panicked",
                };
                return Err(PoisonInfo::new(phase, reason));
            }
            frame.batch_active = false;
            return Ok(());
        }
        let mut stage = BatchFinishStage::Callback;
        let cleanup = catch_unwind(AssertUnwindSafe(|| {
            let mut cx = FinishContext::new();
            for item_slot in (0..frame.item_count).rev() {
                stage = BatchFinishStage::Callback;
                let item = frame.items[item_slot]
                    .as_mut()
                    .expect("finish owns every remaining item slot");
                item.finish(disposition, &mut cx);
                stage = BatchFinishStage::Teardown;
                frame.items[item_slot]
                    .as_mut()
                    .expect("finished item remains owned during teardown")
                    .teardown_after_finish();
                // Keep the now-empty typed item in its worker-affine slot. The
                // next transaction can reinitialize it in place.
            }
        }));
        if cleanup.is_err() {
            frame.runtime.poison();
            let retained = self.frame.take().expect("frame exists during cleanup");
            std::mem::forget(retained);
            let reason = match stage {
                BatchFinishStage::Callback => "finish callback panicked",
                BatchFinishStage::Teardown => "adapter-owned state drop panicked",
            };
            return Err(PoisonInfo::new(phase, reason));
        }
        Ok(())
    }

    fn frame(&self) -> &TransactionFrame {
        self.frame.as_ref().expect("commit driver owns its frame")
    }

    fn locks_mut(&mut self) -> &mut LockPlan {
        self.locks
            .as_mut()
            .expect("commit driver owns its lock plan")
    }

    fn parts_mut(&mut self) -> (&mut TransactionFrame, &mut LockPlan) {
        (
            self.frame.as_mut().expect("commit driver owns its frame"),
            self.locks
                .as_mut()
                .expect("commit driver owns its lock plan"),
        )
    }

    fn direct_batch(&self) -> &dyn ErasedItemBatch {
        self.frame()
            .unique_batch
            .as_deref()
            .expect("direct commit owns an active typed batch")
    }

    fn direct_batch_mut(&mut self) -> &mut dyn ErasedItemBatch {
        self.frame
            .as_mut()
            .expect("direct commit driver owns its frame")
            .unique_batch
            .as_deref_mut()
            .expect("direct commit owns an active typed batch")
    }

    fn poison(&self, phase: FailurePhase, reason: &'static str) -> PoisonInfo {
        self.worker.runtime.poison();
        PoisonInfo::new(phase, reason)
    }

    fn quarantine_items(&mut self) {
        if let Some(frame) = self.frame.take() {
            std::mem::forget(frame);
        }
    }

    fn recycle_frame(&mut self) {
        if let Some(frame) = self.frame.take() {
            self.worker
                .recycle_transaction_scratch(frame.into_scratch());
        }
    }

    fn complete_worker(&mut self) {
        if !self.completed {
            self.worker.finish_transaction();
            self.completed = true;
        }
    }
}

fn teardown_lock_plan(mut locks: LockPlan) -> Result<LockPlanStorage, ()> {
    let teardown = catch_unwind(AssertUnwindSafe(|| locks.teardown_adapter_state()));
    if !matches!(teardown, Ok(Ok(()))) {
        std::mem::forget(locks);
        return Err(());
    }
    catch_unwind(AssertUnwindSafe(|| locks.into_reusable_storage()))
        .map_err(|_| ())
        .and_then(std::convert::identity)
}

impl Drop for CommitDriver<'_, '_> {
    fn drop(&mut self) {
        if self.completed {
            return;
        }
        let fallback = catch_unwind(AssertUnwindSafe(|| {
            let _ = self.contain_unexpected_unwind();
        }));
        if fallback.is_err() || !self.completed {
            let _ = self.emergency_failure();
        }
    }
}

/// Commit driver for the terminal read typestate.
///
/// This intentionally does not share the general driver's lock planning,
/// shape selection, item dispatch, or finish path.
struct TerminalReadCommitDriver<'worker> {
    worker: &'worker mut WorkerContext,
    frame: Option<TerminalReadFrame>,
    boundary: CommitBoundary,
    phase: FailurePhase,
    completed: bool,
}

impl<'worker> TerminalReadCommitDriver<'worker> {
    fn new(worker: &'worker mut WorkerContext, frame: TerminalReadFrame) -> Self {
        debug_assert!(
            !frame.terminal_read_active
                || frame
                    .scratch
                    .terminal_read_batch
                    .as_ref()
                    .is_some_and(|batch| batch.is_complete())
        );
        Self {
            worker,
            frame: Some(frame),
            boundary: CommitBoundary::Reversible,
            phase: FailurePhase::Validation,
            completed: false,
        }
    }

    fn run(&mut self) -> Result<CommitOutcome, CommitFailure> {
        if self.frame().doomed {
            let poison = self
                .worker
                .runtime
                .ensure_healthy(FailurePhase::Execution)
                .err();
            return self.abort_commit(AbortReason::Doomed, poison);
        }
        if let Err(info) = self
            .frame()
            .runtime()
            .ensure_healthy(FailurePhase::Validation)
        {
            return self.abort_commit(AbortReason::Doomed, Some(info));
        }

        self.phase = FailurePhase::Validation;
        if self.frame().terminal_read_active {
            let mut validation_scope = ();
            let cx = PreflightFreeValidationContext::without_locks(None, &mut validation_scope);
            let result = catch_unwind(AssertUnwindSafe(|| {
                self.frame
                    .as_ref()
                    .expect("terminal driver owns its frame")
                    .scratch
                    .terminal_read_batch
                    .as_ref()
                    .expect("an active terminal batch retains its storage")
                    .validate(&cx)
            }));
            match result {
                Ok(Ok(())) => {}
                Ok(Err(CheckError::Conflict(conflict))) => {
                    return self.abort_commit(conflict.into(), None)
                }
                Ok(Err(CheckError::Fault(fault))) => {
                    let reason = AbortReason::Internal(InternalError::new(
                        FailurePhase::Validation,
                        "adapter fault during terminal read validation",
                    ));
                    let info = self.poison(
                        adapter_failure_phase(fault),
                        "adapter fault during terminal read validation",
                    );
                    return self.abort_commit(reason, Some(info));
                }
                Err(_) => {
                    let reason = AbortReason::Internal(InternalError::new(
                        FailurePhase::Validation,
                        "terminal read validation callback panicked",
                    ));
                    let info = self.poison(
                        FailurePhase::Validation,
                        "terminal read validation callback panicked",
                    );
                    return self.abort_commit(reason, Some(info));
                }
            }
        }

        // With no installation or publication callback, successful final
        // certification is the definite commit boundary.
        self.boundary = CommitBoundary::Published;
        self.phase = FailurePhase::Finish;
        let commit_info = CommitInfo::new(None);
        if let Err(info) = self.teardown_terminal_batch() {
            self.complete_worker();
            return Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(commit_info),
                info,
            });
        }
        self.recycle_frame();
        self.complete_worker();
        Ok(CommitOutcome::Committed(commit_info))
    }

    fn abort_commit(
        &mut self,
        reason: AbortReason,
        mut poison: Option<PoisonInfo>,
    ) -> Result<CommitOutcome, CommitFailure> {
        self.phase = FailurePhase::Finish;
        if let Err(info) = self.teardown_terminal_batch() {
            poison = Some(info);
        }
        self.recycle_frame();
        self.complete_worker();
        match poison {
            Some(info) => Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Aborted(reason),
                info,
            }),
            None => Ok(CommitOutcome::Aborted(reason)),
        }
    }

    fn teardown_terminal_batch(&mut self) -> Result<(), PoisonInfo> {
        let Some(frame) = self.frame.as_mut() else {
            return Ok(());
        };
        if !frame.terminal_read_active {
            return Ok(());
        }
        let cleanup = catch_unwind(AssertUnwindSafe(|| {
            frame
                .scratch
                .terminal_read_batch
                .as_mut()
                .expect("an active terminal batch retains its storage")
                .teardown_drop_only_reverse();
        }));
        if cleanup.is_err() {
            frame.runtime().poison();
            let retained = self
                .frame
                .take()
                .expect("terminal cleanup retains its frame");
            std::mem::forget(retained);
            return Err(PoisonInfo::new(
                FailurePhase::Finish,
                "terminal key or observation drop panicked",
            ));
        }
        frame.terminal_read_active = false;
        Ok(())
    }

    fn contain_unexpected_unwind(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = self.phase;
        match self.boundary {
            CommitBoundary::Reversible => {
                let reason = AbortReason::Internal(InternalError::new(
                    phase,
                    "unexpected terminal-read commit-driver unwind",
                ));
                let info = self.poison(phase, "unexpected terminal-read commit-driver unwind");
                self.abort_commit(reason, Some(info))
            }
            CommitBoundary::Published => {
                let mut info =
                    self.poison(phase, "unexpected terminal-read unwind after certification");
                if let Err(teardown) = self.teardown_terminal_batch() {
                    info = teardown;
                }
                self.recycle_frame();
                self.complete_worker();
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Committed(CommitInfo::new(None)),
                    info,
                })
            }
            CommitBoundary::Irrevocable => unreachable!(
                "terminal read transactions have no irreversible installation boundary"
            ),
        }
    }

    fn emergency_failure(&mut self) -> Result<CommitOutcome, CommitFailure> {
        let phase = self.phase;
        self.worker.runtime.poison();
        if let Some(frame) = self.frame.take() {
            std::mem::forget(frame);
        }
        self.complete_worker();
        let info = PoisonInfo::new(phase, "terminal-read panic containment itself failed");
        match self.boundary {
            CommitBoundary::Published => Err(CommitFailure::Poisoned {
                outcome: DefiniteOutcome::Committed(CommitInfo::new(None)),
                info,
            }),
            CommitBoundary::Reversible | CommitBoundary::Irrevocable => {
                Err(CommitFailure::Poisoned {
                    outcome: DefiniteOutcome::Aborted(AbortReason::Internal(InternalError::new(
                        phase,
                        "terminal-read panic containment itself failed",
                    ))),
                    info,
                })
            }
        }
    }

    fn frame(&self) -> &TerminalReadFrame {
        self.frame
            .as_ref()
            .expect("terminal commit driver owns its frame")
    }

    fn poison(&self, phase: FailurePhase, reason: &'static str) -> PoisonInfo {
        self.worker.runtime.poison();
        PoisonInfo::new(phase, reason)
    }

    fn recycle_frame(&mut self) {
        if let Some(frame) = self.frame.take() {
            self.worker
                .recycle_terminal_read_scratch(frame.into_scratch());
        }
    }

    fn complete_worker(&mut self) {
        if !self.completed {
            self.worker.finish_transaction();
            self.completed = true;
        }
    }
}

impl Drop for TerminalReadCommitDriver<'_> {
    fn drop(&mut self) {
        if self.completed {
            return;
        }
        let fallback = catch_unwind(AssertUnwindSafe(|| {
            let _ = self.contain_unexpected_unwind();
        }));
        if fallback.is_err() || !self.completed {
            let _ = self.emergency_failure();
        }
    }
}

fn abort_without_locks(
    worker: &mut WorkerContext,
    mut frame: TransactionFrame,
    reason: AbortReason,
) -> AbortInfo {
    let cleanup = catch_unwind(AssertUnwindSafe(|| {
        if frame.batch_active {
            let batch = frame
                .unique_batch
                .as_mut()
                .expect("an active unique batch retains its storage");
            let mut cx = FinishContext::new();
            let mut stage = BatchFinishStage::Callback;
            batch.finish_and_teardown(FinishDisposition::Aborted, &mut cx, &mut stage);
            frame.batch_active = false;
        } else {
            let mut cx = FinishContext::new();
            for item_slot in (0..frame.item_count).rev() {
                let item = frame.items[item_slot]
                    .as_mut()
                    .expect("finish owns every remaining item slot");
                item.finish(FinishDisposition::Aborted, &mut cx);
                item.teardown_after_finish();
                // Retain the empty typed item for the worker's next transaction.
            }
        }
    }));
    if cleanup.is_err() {
        frame.runtime.poison();
        std::mem::forget(frame);
    } else {
        worker.recycle_transaction_scratch(frame.into_scratch());
    }
    worker.finish_transaction();
    AbortInfo::new(reason)
}

fn abort_terminal_read_without_locks(
    worker: &mut WorkerContext,
    mut frame: TerminalReadFrame,
    reason: AbortReason,
) -> AbortInfo {
    let cleanup = catch_unwind(AssertUnwindSafe(|| {
        if frame.terminal_read_active {
            frame
                .scratch
                .terminal_read_batch
                .as_mut()
                .expect("an active terminal batch retains its storage")
                .teardown_drop_only_reverse();
            frame.terminal_read_active = false;
        }
    }));
    if cleanup.is_err() {
        frame.runtime().poison();
        std::mem::forget(frame);
    } else {
        worker.recycle_terminal_read_scratch(frame.into_scratch());
    }
    worker.finish_transaction();
    AbortInfo::new(reason)
}

fn adapter_failure_phase(fault: AdapterFault) -> FailurePhase {
    match fault.phase() {
        AdapterPhase::ItemInit => FailurePhase::Execution,
        AdapterPhase::Execute => FailurePhase::Execution,
        AdapterPhase::Preflight => FailurePhase::Preflight,
        AdapterPhase::Acquire => FailurePhase::Acquire,
        AdapterPhase::ExecutionCheck => FailurePhase::Execution,
        AdapterPhase::PredicateUpgrade => FailurePhase::PredicateUpgrade,
        AdapterPhase::Validation => FailurePhase::Validation,
        AdapterPhase::Install => FailurePhase::Install,
        AdapterPhase::Release => FailurePhase::Release,
        AdapterPhase::Finish => FailurePhase::Finish,
    }
}
