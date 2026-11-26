//! Atomic byte-store operations shared by RocksDB row and log adapters.

/// A durable byte store: the system of record.
pub trait Blobs: Send + Sync {
    /// Read one key.
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError>;

    /// Apply a batch atomically: either the whole batch lands or none of it
    /// does.
    ///
    /// Operations apply in slice order. If a key occurs more than once, the
    /// last operation for that key determines its final state. Callers
    /// rely on this rule when one physical batch contains several logical
    /// transactions.
    ///
    /// Backends used for checkpoint-based log reclamation must be configured
    /// so crash recovery preserves a coherent prefix of serialized writes. A
    /// reclamation batch must not survive without its prerequisite checkpoint.
    /// This additional requirement does not apply to configurations intended
    /// only as a disposable cache; callers needing it must enforce the choice
    /// of backend and durability settings.
    ///
    /// An error need not reveal which atomic outcome occurred, so callers may
    /// retry the identical batch. Implementations must make that retry
    /// idempotent for these Put/Delete operations.
    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError>;

    /// Iterate every key, in ascending order, for the open-time load.
    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError>;

    /// Visit key/value pairs in ascending key order. References are valid only
    /// during the callback. An error stops iteration and propagates unchanged.
    /// Callers requiring a stable view must exclude concurrent backend writes.
    ///
    /// This compatibility default collects keys, then fetches one value at a
    /// time after key iteration releases its locks. Production adapters should
    /// override it with a streaming iterator so startup memory does not include
    /// a second copy of every key. Implementations must release iterator state
    /// on callback errors and unwinding panics.
    fn for_each_entry(&self, f: &mut EntryVisitor<'_>) -> Result<(), BlobError> {
        let mut keys = Vec::new();
        self.for_each_key(&mut |key| keys.push(key.to_vec()))?;
        for key in keys {
            let value = self.get(&key)?.ok_or_else(|| {
                BlobError("key disappeared during exclusive backend iteration".into())
            })?;
            f(&key, &value)?;
        }
        Ok(())
    }

    /// Visit at most `max_entries` rows in ascending bytewise key order,
    /// starting at the first key greater than or equal to `start`.
    /// Returning `false` stops successfully; an error propagates unchanged.
    /// A zero limit performs no backend access. References are valid only
    /// during the callback, including when it stops or panics. Callers needing
    /// a stable view must exclude concurrent backend writes.
    ///
    /// The compatibility implementation enumerates keys but retains at most
    /// `max_entries`, then reads values outside enumeration locks. Adapters
    /// should override this with one bounded seek/scan, as RocksDB does, to
    /// avoid a separate point lookup for every reclaimed log entry.
    fn for_each_entry_from(
        &self,
        start: &[u8],
        max_entries: usize,
        visitor: &mut BoundedEntryVisitor<'_>,
    ) -> Result<(), BlobError> {
        if max_entries == 0 {
            return Ok(());
        }
        let mut keys = std::collections::BTreeSet::new();
        self.for_each_key(&mut |key| {
            if key >= start
                && (keys.len() < max_entries
                    || keys
                        .last()
                        .is_some_and(|last: &Vec<u8>| key < last.as_slice()))
            {
                keys.insert(key.to_vec());
                if keys.len() > max_entries {
                    keys.pop_last();
                }
            }
        })?;
        for key in keys {
            let value = self.get(&key)?.ok_or_else(|| {
                BlobError("key disappeared during exclusive backend iteration".into())
            })?;
            if !visitor(&key, &value)? {
                break;
            }
        }
        Ok(())
    }

    /// Observe database file sizes, if this backend can measure them.
    /// This may perform filesystem I/O and must not run on transaction paths
    /// or inside nonblocking health snapshots. The result is an observation,
    /// not a storage limit; compaction may change it during measurement.
    fn disk_usage_bytes(&self) -> Result<Option<u64>, BlobError> {
        Ok(None)
    }
}

/// Forward the byte-store contract through a shared backend handle.
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
    fn for_each_entry(&self, f: &mut EntryVisitor<'_>) -> Result<(), BlobError> {
        (**self).for_each_entry(f)
    }
    fn for_each_entry_from(
        &self,
        start: &[u8],
        max_entries: usize,
        visitor: &mut BoundedEntryVisitor<'_>,
    ) -> Result<(), BlobError> {
        (**self).for_each_entry_from(start, max_entries, visitor)
    }
    fn disk_usage_bytes(&self) -> Result<Option<u64>, BlobError> {
        (**self).disk_usage_bytes()
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

/// Callback shape of [`Blobs::for_each_entry`]: visits one `(key, value)` row.
pub type EntryVisitor<'a> = dyn FnMut(&[u8], &[u8]) -> Result<(), BlobError> + 'a;

/// Callback shape of [`Blobs::for_each_entry_from`]: `Ok(false)` stops early.
pub type BoundedEntryVisitor<'a> = dyn FnMut(&[u8], &[u8]) -> Result<bool, BlobError> + 'a;

/// A failure from the durable store.
#[derive(Debug, Clone)]
pub struct BlobError(pub String);

impl std::fmt::Display for BlobError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "blob store error: {}", self.0)
    }
}

impl std::error::Error for BlobError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct MemBlobs(std::sync::Mutex<std::collections::BTreeMap<Vec<u8>, Vec<u8>>>);
    impl MemBlobs {
        fn seeded(rows: impl IntoIterator<Item = (Vec<u8>, Vec<u8>)>) -> Self {
            Self(std::sync::Mutex::new(rows.into_iter().collect()))
        }
    }
    impl Blobs for MemBlobs {
        fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
            Ok(self.0.lock().unwrap().get(key).cloned())
        }
        fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
            let mut rows = self.0.lock().unwrap();
            for op in ops {
                match op {
                    BlobOp::Put { key, val } => {
                        rows.insert(key.to_vec(), val.to_vec());
                    }
                    BlobOp::Delete { key } => {
                        rows.remove(*key);
                    }
                }
            }
            Ok(())
        }
        fn for_each_key(&self, visitor: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
            for key in self.0.lock().unwrap().keys() {
                visitor(key);
            }
            Ok(())
        }
    }

    #[test]
    fn legacy_entry_visitor_propagates_get_and_callback_failures() {
        struct LegacyBackend(MemBlobs);
        impl Blobs for LegacyBackend {
            fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
                if key == b"bad" {
                    Err(BlobError("read failed".into()))
                } else {
                    self.0.get(key)
                }
            }
            fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
                self.0.write_batch(ops)
            }
            fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
                self.0.for_each_key(f)
            }
        }
        let backend = LegacyBackend(MemBlobs::seeded([
            (b"a".to_vec(), b"1".to_vec()),
            (b"bad".to_vec(), b"2".to_vec()),
        ]));
        assert_eq!(backend.disk_usage_bytes().unwrap(), None);
        let mut visits = 0;
        let error = backend
            .for_each_entry(&mut |_, _| {
                visits += 1;
                Ok(())
            })
            .unwrap_err();
        assert_eq!(error.0, "read failed");
        assert_eq!(visits, 1);
        assert_eq!(
            backend
                .for_each_entry(&mut |_, _| Err(BlobError("callback failed".into())))
                .unwrap_err()
                .0,
            "callback failed"
        );
        let mut rows = Vec::new();
        backend
            .for_each_entry_from(b"a", 1, &mut |key, value| {
                rows.push((key.to_vec(), value.to_vec()));
                Ok(true)
            })
            .unwrap();
        assert_eq!(rows, vec![(b"a".to_vec(), b"1".to_vec())]);
        // Stopping after the first row must not read the failing successor.
        backend
            .for_each_entry_from(b"a", 10, &mut |_, _| Ok(false))
            .unwrap();
        assert_eq!(
            backend
                .for_each_entry_from(b"a\0", 10, &mut |_, _| Ok(true))
                .unwrap_err()
                .0,
            "read failed"
        );
        assert_eq!(
            backend
                .for_each_entry_from(b"a", 10, &mut |_, _| {
                    Err(BlobError("range callback failed".into()))
                })
                .unwrap_err()
                .0,
            "range callback failed"
        );
    }

    #[test]
    fn shared_backend_forwards_streaming_visitor_without_key_collection() {
        struct StreamingOnly;
        impl Blobs for StreamingOnly {
            fn get(&self, _: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
                panic!("streaming visitor must not call get");
            }
            fn write_batch(&self, _: &[BlobOp<'_>]) -> Result<(), BlobError> {
                Ok(())
            }
            fn for_each_key(&self, _: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
                panic!("streaming visitor must not collect keys");
            }
            fn for_each_entry(&self, f: &mut EntryVisitor<'_>) -> Result<(), BlobError> {
                f(b"key", b"value")
            }
            fn for_each_entry_from(
                &self,
                start: &[u8],
                max_entries: usize,
                visitor: &mut BoundedEntryVisitor<'_>,
            ) -> Result<(), BlobError> {
                assert_eq!(start, b"key");
                assert_eq!(max_entries, 7);
                visitor(b"key", b"value")?;
                Ok(())
            }
        }
        let backend = std::sync::Arc::new(StreamingOnly);
        let mut seen = false;
        backend
            .for_each_entry(&mut |key, value| {
                assert_eq!((key, value), (b"key".as_slice(), b"value".as_slice()));
                seen = true;
                Ok(())
            })
            .unwrap();
        assert!(seen);
        let mut ranged = false;
        backend
            .for_each_entry_from(b"key", 7, &mut |key, value| {
                assert_eq!((key, value), (b"key".as_slice(), b"value".as_slice()));
                ranged = true;
                Ok(false)
            })
            .unwrap();
        assert!(ranged);
    }

    #[test]
    fn zero_limit_compatibility_scan_does_not_access_backend() {
        struct NoAccess;
        impl Blobs for NoAccess {
            fn get(&self, _: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
                panic!("zero limit must not read");
            }
            fn write_batch(&self, _: &[BlobOp<'_>]) -> Result<(), BlobError> {
                panic!("zero limit must not write");
            }
            fn for_each_key(&self, _: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
                panic!("zero limit must not enumerate");
            }
        }
        NoAccess
            .for_each_entry_from(b"start", 0, &mut |_, _| panic!("zero limit callback"))
            .unwrap();
    }
}
