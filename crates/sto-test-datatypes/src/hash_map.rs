//! A fixed-bucket transactional hash map used to test the public STO adapter.
//!
//! Each bucket is one logical transaction item and one physical version lock.
//! Reads take an immutable [`BTreeMap`] snapshot of the selected bucket, so a
//! missing key is covered by the same observation as a present key. Writes
//! clone and modify that snapshot during transaction execution. Commit only
//! swaps the prepared [`Arc`] while holding the bucket lock.
//!
//! This deliberately simple conflict model admits false conflicts between
//! different keys that hash to the same bucket. Keys in different buckets can
//! validate and commit independently.

use std::{
    collections::{hash_map::DefaultHasher, BTreeMap},
    fmt,
    hash::{Hash, Hasher},
    num::NonZeroUsize,
    sync::Arc,
};

use arc_swap::ArcSwap;
use sto_core::{
    AccessError, Active, AdapterFault, AdapterFaultKind, AdapterPhase, AtomicVersion,
    CapacityError, CheckError, Conflict, Entry, ExecutionCheckContext, FinishContext,
    FinishDisposition, FinishItem, InstallContext, InstallItem, LockClass, LockIdentity,
    LockNamespaceId, LockRequest, LockUse, NoPredicate, ObjectId, ObservationOrder, ObservationRef,
    OccVersion, OpacityToken, PredicateContext, PreflightContext, PreflightItem, PrepareError,
    RegisteredResource, RegistrationError, ResourceClass, ResourceKey, Runtime, Transaction,
    TransactionalResource, ValidationContext, VersionLock,
};

const HASH_MAP_RESOURCE_CLASS_VALUE: u32 = 1;
const HASH_MAP_LOCK_CLASS_VALUE: u32 = 1;
const DEFAULT_BUCKET_COUNT: usize = 64;

/// Failure to allocate or register a transactional hash map.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum HashMapCreateError {
    /// The fixed bucket array or one of its checked identities exceeded a
    /// finite limit.
    Capacity(CapacityError),
    /// STO object or resource registration failed.
    Registration(RegistrationError),
}

impl fmt::Display for HashMapCreateError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Capacity(error) => write!(formatter, "hash-map capacity error: {error}"),
            Self::Registration(error) => write!(formatter, "hash-map registration error: {error}"),
        }
    }
}

impl std::error::Error for HashMapCreateError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Capacity(error) => Some(error),
            Self::Registration(error) => Some(error),
        }
    }
}

impl From<CapacityError> for HashMapCreateError {
    fn from(error: CapacityError) -> Self {
        Self::Capacity(error)
    }
}

impl From<RegistrationError> for HashMapCreateError {
    fn from(error: RegistrationError) -> Self {
        Self::Registration(error)
    }
}

/// A fixed-bucket transactional hash map.
///
/// Every bucket owns one immutable committed map snapshot and one OCC version.
/// Operations on different keys in the same bucket therefore conflict even
/// when the keys are unequal. Increasing the bucket count reduces these false
/// conflicts at the cost of more per-map metadata.
pub struct TxnHashMap<K, V>
where
    K: ResourceKey + Hash,
    V: Clone + Send + Sync + 'static,
{
    resource: RegisteredResource<HashMapAdapter<K, V>>,
}

impl<K, V> TxnHashMap<K, V>
where
    K: ResourceKey + Hash,
    V: Clone + Send + Sync + 'static,
{
    /// Number of buckets created by [`Self::new`].
    pub const DEFAULT_BUCKET_COUNT: usize = DEFAULT_BUCKET_COUNT;

    /// Registers an empty map with 64 fixed buckets.
    pub fn new(runtime: &Arc<Runtime>) -> Result<Self, HashMapCreateError> {
        Self::with_bucket_count(
            runtime,
            NonZeroUsize::new(DEFAULT_BUCKET_COUNT)
                .expect("the default transactional hash-map bucket count is nonzero"),
        )
    }

    /// Registers an empty map with the requested fixed number of buckets.
    pub fn with_bucket_count(
        runtime: &Arc<Runtime>,
        bucket_count: NonZeroUsize,
    ) -> Result<Self, HashMapCreateError> {
        let object = runtime.register_object()?;
        let namespace = LockNamespaceId::new(object.object_id().get())
            .expect("nonzero ObjectId always forms a LockNamespaceId");
        let resource_class = ResourceClass::new(HASH_MAP_RESOURCE_CLASS_VALUE)
            .expect("the private TxnHashMap resource class is nonzero");
        let lock_class = LockClass::new(HASH_MAP_LOCK_CLASS_VALUE)
            .expect("the private TxnHashMap lock class is nonzero");

        let mut buckets = Vec::new();
        buckets
            .try_reserve_exact(bucket_count.get())
            .map_err(|_| CapacityError::BufferLimit)?;
        for index in 0..bucket_count.get() {
            let lock_key = u64::try_from(index).map_err(|_| CapacityError::KeyLimit)?;
            let version = Arc::new(AtomicVersion::default());
            let lock = Arc::new(VersionLock::new(Arc::clone(&version)));
            let lock_identity =
                LockIdentity::new(object.runtime_id(), namespace, lock_class, lock_key);
            buckets.push(HashMapBucket {
                values: ArcSwap::from_pointee(BTreeMap::new()),
                version,
                lock,
                lock_identity,
            });
        }

        let adapter = HashMapAdapter {
            buckets: buckets.into_boxed_slice(),
        };
        let resource = object.register_resource(resource_class, adapter)?;
        Ok(Self { resource })
    }

    /// Returns the map's fixed bucket count.
    pub fn bucket_count(&self) -> usize {
        self.resource.adapter().buckets.len()
    }

    /// Returns the stable STO object identity of this map.
    pub fn object_id(&self) -> ObjectId {
        self.resource.object_id()
    }

    /// Returns the bucket selected for `key`.
    ///
    /// This is mainly useful for deterministic conflict tests. Hashing a key
    /// must remain stable for the key's lifetime, as required by
    /// [`ResourceKey`].
    pub fn bucket_index(&self, key: &K) -> usize {
        let mut hasher = DefaultHasher::new();
        key.hash(&mut hasher);
        let bucket_count = u64::try_from(self.bucket_count())
            .expect("a process-addressable bucket count fits in u64");
        usize::try_from(hasher.finish() % bucket_count)
            .expect("the reduced bucket index fits in usize")
    }

    /// Reads the transaction-local value for `key`.
    ///
    /// A missing result records a bucket observation, so a later insertion in
    /// the same bucket invalidates the transaction.
    pub fn get(
        &self,
        transaction: &mut Transaction<'_, Active>,
        key: &K,
    ) -> Result<Option<V>, AccessError> {
        let adapter = self.resource.adapter();
        transaction.with_resolved_item(
            &self.resource,
            || {
                let bucket_index = self.bucket_index(key);
                Ok(Some((bucket_index, bucket_index)))
            },
            || unreachable!("TxnHashMap bucket resolution always succeeds"),
            |entry, bucket_index| {
                let snapshot = adapter.snapshot(bucket_index, entry)?;
                Ok(snapshot.get(key).cloned())
            },
        )
    }

    /// Tests whether the transaction-local map contains `key`.
    pub fn contains_key(
        &self,
        transaction: &mut Transaction<'_, Active>,
        key: &K,
    ) -> Result<bool, AccessError> {
        let adapter = self.resource.adapter();
        transaction.with_resolved_item(
            &self.resource,
            || {
                let bucket_index = self.bucket_index(key);
                Ok(Some((bucket_index, bucket_index)))
            },
            || unreachable!("TxnHashMap bucket resolution always succeeds"),
            |entry, bucket_index| {
                let snapshot = adapter.snapshot(bucket_index, entry)?;
                Ok(snapshot.contains_key(key))
            },
        )
    }

    /// Inserts or replaces `key`, returning its prior transaction-local value.
    pub fn insert(
        &self,
        transaction: &mut Transaction<'_, Active>,
        key: K,
        value: V,
    ) -> Result<Option<V>, AccessError> {
        let adapter = self.resource.adapter();
        transaction.with_resolved_item(
            &self.resource,
            move || {
                let bucket_index = self.bucket_index(&key);
                Ok(Some((bucket_index, (bucket_index, key, value))))
            },
            || unreachable!("TxnHashMap bucket resolution always succeeds"),
            |entry, (bucket_index, key, value)| {
                let snapshot = adapter.snapshot(bucket_index, entry)?;
                let mut replacement = snapshot.as_ref().clone();
                let previous = replacement.insert(key, value);
                entry.stage(Arc::new(replacement))?;
                Ok(previous)
            },
        )
    }

    /// Removes `key`, returning its prior transaction-local value.
    ///
    /// Removing a key that was absent before any staged update remains a read
    /// only operation. It still records the bucket observation needed to
    /// protect the missing result.
    pub fn remove(
        &self,
        transaction: &mut Transaction<'_, Active>,
        key: &K,
    ) -> Result<Option<V>, AccessError> {
        let adapter = self.resource.adapter();
        transaction.with_resolved_item(
            &self.resource,
            || {
                let bucket_index = self.bucket_index(key);
                Ok(Some((bucket_index, bucket_index)))
            },
            || unreachable!("TxnHashMap bucket resolution always succeeds"),
            |entry, bucket_index| {
                let had_intent = entry.intent().is_some();
                let snapshot = adapter.snapshot(bucket_index, entry)?;
                let mut replacement = snapshot.as_ref().clone();
                let previous = replacement.remove(key);
                if previous.is_some() || had_intent {
                    entry.stage(Arc::new(replacement))?;
                }
                Ok(previous)
            },
        )
    }

    /// Returns the transaction-local number of entries.
    ///
    /// This observes every bucket. A concurrent change to any bucket therefore
    /// invalidates the result before commit.
    pub fn len(&self, transaction: &mut Transaction<'_, Active>) -> Result<usize, AccessError> {
        let adapter = self.resource.adapter();
        let mut len = 0usize;
        for bucket_index in 0..adapter.buckets.len() {
            transaction.with_item(
                &self.resource,
                bucket_index,
                |entry| -> Result<(), AccessError> {
                    let bucket_len = adapter.snapshot(bucket_index, entry)?.len();
                    len = len
                        .checked_add(bucket_len)
                        .ok_or(CapacityError::BufferLimit)?;
                    Ok(())
                },
            )?;
        }
        Ok(len)
    }

    /// Returns whether the transaction-local map has no entries.
    ///
    /// The successful empty result observes every bucket. A false result needs
    /// only the observed nonempty bucket as its witness.
    pub fn is_empty(&self, transaction: &mut Transaction<'_, Active>) -> Result<bool, AccessError> {
        let adapter = self.resource.adapter();
        for bucket_index in 0..adapter.buckets.len() {
            let bucket_is_empty = transaction.with_item(
                &self.resource,
                bucket_index,
                |entry| -> Result<bool, AccessError> {
                    Ok(adapter.snapshot(bucket_index, entry)?.is_empty())
                },
            )?;
            if !bucket_is_empty {
                return Ok(false);
            }
        }
        Ok(true)
    }

    /// Copies the complete transaction-local map in key order.
    ///
    /// This observes every bucket and performs all key and value cloning before
    /// commit begins.
    pub fn to_btree_map(
        &self,
        transaction: &mut Transaction<'_, Active>,
    ) -> Result<BTreeMap<K, V>, AccessError> {
        let adapter = self.resource.adapter();
        let mut result = BTreeMap::new();
        for bucket_index in 0..adapter.buckets.len() {
            transaction.with_item(
                &self.resource,
                bucket_index,
                |entry| -> Result<(), AccessError> {
                    let mut bucket_copy = adapter.snapshot(bucket_index, entry)?.as_ref().clone();
                    result.append(&mut bucket_copy);
                    Ok(())
                },
            )?;
        }
        Ok(result)
    }
}

impl<K, V> Clone for TxnHashMap<K, V>
where
    K: ResourceKey + Hash,
    V: Clone + Send + Sync + 'static,
{
    fn clone(&self) -> Self {
        Self {
            resource: self.resource.clone(),
        }
    }
}

impl<K, V> fmt::Debug for TxnHashMap<K, V>
where
    K: ResourceKey + Hash,
    V: Clone + Send + Sync + 'static,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TxnHashMap")
            .field("runtime_id", &self.resource.runtime_id())
            .field("object_id", &self.resource.object_id())
            .field("bucket_count", &self.bucket_count())
            .finish_non_exhaustive()
    }
}

struct HashMapBucket<K, V> {
    values: ArcSwap<BTreeMap<K, V>>,
    version: Arc<AtomicVersion>,
    lock: Arc<VersionLock>,
    lock_identity: LockIdentity,
}

struct HashMapAdapter<K, V> {
    buckets: Box<[HashMapBucket<K, V>]>,
}

struct HashMapLocal<K, V> {
    first_snapshot: Option<Arc<BTreeMap<K, V>>>,
    displaced: Option<Arc<BTreeMap<K, V>>>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct HashMapObservation {
    version: OccVersion,
}

impl OpacityToken for HashMapObservation {
    fn observation_order(&self) -> ObservationOrder {
        ObservationOrder::Ordered(self.version)
    }
}

struct HashMapPrepared {
    lock_use: Option<LockUse<VersionLock>>,
}

impl<K, V> HashMapAdapter<K, V>
where
    K: ResourceKey + Hash,
    V: Clone + Send + Sync + 'static,
{
    fn snapshot(
        &self,
        bucket_index: usize,
        entry: &mut Entry<'_, Self>,
    ) -> Result<Arc<BTreeMap<K, V>>, AccessError> {
        if let Some(intent) = entry.intent() {
            return Ok(Arc::clone(intent));
        }
        if let Some(snapshot) = entry.local().first_snapshot.as_ref() {
            return Ok(Arc::clone(snapshot));
        }

        let bucket = self.bucket_for_access(bucket_index)?;
        let observed = bucket
            .version
            .observe()
            .map_err(|_| AccessError::from(Conflict::LockBusy))?;
        let snapshot = bucket.values.load_full();
        if !bucket.version.validate(observed) {
            return Err(Conflict::ReadValidation.into());
        }

        entry.record_read(HashMapObservation { version: observed })?;
        entry.local_mut().first_snapshot = Some(Arc::clone(&snapshot));
        Ok(snapshot)
    }

    fn bucket_for_access(&self, bucket_index: usize) -> Result<&HashMapBucket<K, V>, AccessError> {
        self.buckets.get(bucket_index).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Execute,
                AdapterFaultKind::Other("TxnHashMap item key is out of bounds"),
            )
            .into()
        })
    }

    fn validate_observation(
        &self,
        bucket_index: usize,
        observation: &HashMapObservation,
        prepared: &HashMapPrepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        let bucket = self.buckets.get(bucket_index).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Validation,
                AdapterFaultKind::Other("TxnHashMap item key is out of bounds"),
            )
        })?;
        let valid = if let Some(lock_use) = prepared.lock_use.as_ref() {
            let guard = cx.guard(lock_use)?;
            if !guard.is_for(&bucket.version) || guard.owner() != cx.owner() {
                return Err(AdapterFault::new(
                    AdapterPhase::Validation,
                    AdapterFaultKind::LockIdentityMismatch,
                )
                .into());
            }
            bucket.version.validate_own(observation.version, cx.owner())
        } else {
            bucket.version.validate(observation.version)
        };

        if valid {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
    }
}

impl<K, V> TransactionalResource for HashMapAdapter<K, V>
where
    K: ResourceKey + Hash,
    V: Clone + Send + Sync + 'static,
{
    type Key = usize;
    type Local = HashMapLocal<K, V>;
    type Observation = HashMapObservation;
    type Predicate = NoPredicate;
    type Intent = Arc<BTreeMap<K, V>>;
    type Prepared = HashMapPrepared;

    fn new_local(&self, key: &Self::Key) -> Result<Self::Local, sto_core::ItemInitError> {
        if *key >= self.buckets.len() {
            return Err(AdapterFault::new(
                AdapterPhase::ItemInit,
                AdapterFaultKind::Other("TxnHashMap item key is out of bounds"),
            )
            .into());
        }
        Ok(HashMapLocal {
            first_snapshot: None,
            displaced: None,
        })
    }

    fn preflight(
        &self,
        key: &Self::Key,
        item: PreflightItem<'_, Self>,
        cx: &mut PreflightContext<'_>,
    ) -> Result<Self::Prepared, PrepareError> {
        let bucket = self.buckets.get(*key).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::Preflight,
                AdapterFaultKind::Other("TxnHashMap item key is out of bounds"),
            )
        })?;
        let lock_use = if item.intent().is_some() {
            Some(cx.require_lock(LockRequest::new(
                bucket.lock_identity.clone(),
                Arc::clone(&bucket.lock),
            ))?)
        } else {
            None
        };
        Ok(HashMapPrepared { lock_use })
    }

    fn revalidate_read(
        &self,
        key: &Self::Key,
        observation: &Self::Observation,
        _cx: &ExecutionCheckContext<'_>,
    ) -> Result<(), CheckError> {
        let bucket = self.buckets.get(*key).ok_or_else(|| {
            AdapterFault::new(
                AdapterPhase::ExecutionCheck,
                AdapterFaultKind::Other("TxnHashMap item key is out of bounds"),
            )
        })?;
        if bucket.version.validate(observation.version) {
            Ok(())
        } else {
            Err(Conflict::ReadValidation.into())
        }
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
        key: &Self::Key,
        observation: &Self::Observation,
        prepared: &Self::Prepared,
        cx: &ValidationContext<'_>,
    ) -> Result<(), CheckError> {
        self.validate_observation(*key, observation, prepared, cx)
    }

    fn install(
        &self,
        key: &Self::Key,
        mut item: InstallItem<'_, Self>,
        prepared: &mut Self::Prepared,
        cx: &mut InstallContext<'_>,
    ) {
        let bucket = self.buckets.get(*key).expect(
            "sto-test-datatypes TxnHashMap invariant: item key is out of bounds during install",
        );
        let lock_use = prepared
            .lock_use
            .as_ref()
            .expect("sto-test-datatypes TxnHashMap invariant: write has no planned bucket lock");
        let commit_id = cx
            .occ_commit_id()
            .expect("sto-test-datatypes TxnHashMap invariant: write has no OCC commit ID");
        let guard = cx
            .guard_mut(lock_use)
            .unwrap_or_else(|error| panic!("sto-test-datatypes TxnHashMap invariant: {error}"));
        if !guard.is_held() || !guard.is_for(&bucket.version) {
            panic!(
                "sto-test-datatypes TxnHashMap invariant: install received the wrong version guard"
            );
        }
        if commit_id.to_version() <= guard.before() {
            panic!(
                "sto-test-datatypes TxnHashMap invariant: OCC commit ID does not advance the bucket"
            );
        }
        if item.local_mut().displaced.is_some() {
            panic!("sto-test-datatypes TxnHashMap invariant: item installed more than once");
        }

        match item.observation() {
            ObservationRef::Unobserved => {
                panic!(
                    "sto-test-datatypes TxnHashMap invariant: replacement has no bucket observation"
                );
            }
            ObservationRef::Read(observation) | ObservationRef::UpgradedPredicate(observation) => {
                if observation.version != guard.before() {
                    panic!(
                        "sto-test-datatypes TxnHashMap invariant: read observation differs from lock guard"
                    );
                }
            }
            ObservationRef::Predicate(predicate) => match *predicate {},
        }

        let replacement = Arc::clone(item.intent());
        let displaced = bucket.values.swap(replacement);
        item.local_mut().displaced = Some(displaced);
    }

    fn finish(
        &self,
        _key: &Self::Key,
        mut item: FinishItem<'_, Self>,
        _prepared: Option<&mut Self::Prepared>,
        _disposition: FinishDisposition,
        _cx: &mut FinishContext<'_>,
    ) {
        item.local_mut().first_snapshot = None;
        item.local_mut().displaced = None;
        let _ = item.take_remaining_intent();
    }
}
