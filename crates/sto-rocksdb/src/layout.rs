//! Private logical addresses for metadata and rows. Only raw user keys/values
//! reach native user column families. The address prefix is never persisted
//! there. Catalog, versions and protocol records live in META.
use mako_rocksdb_store::{BlobError, BlobOp, Blobs, BoundedEntryVisitor, Durability, RocksBlobs};
use std::{
    collections::BTreeMap,
    path::Path,
    sync::{Arc, RwLock},
};

pub const META: &str = "__mako_internal_v2";
pub(crate) struct Store {
    pub native: Arc<RocksBlobs>,
    names: RwLock<BTreeMap<u64, String>>,
}
pub(crate) struct Snapshot {
    native: mako_rocksdb_store::Snapshot,
    names: BTreeMap<u64, String>,
}
fn row(key: &[u8]) -> Option<(u64, &[u8])> {
    (key.first() == Some(&1) && key.len() >= 9)
        .then(|| (u64::from_be_bytes(key[1..9].try_into().unwrap()), &key[9..]))
}
fn family(names: &BTreeMap<u64, String>, id: u64) -> Result<&str, BlobError> {
    names
        .get(&id)
        .map(String::as_str)
        .ok_or_else(|| BlobError(format!("missing column family for table {id}")))
}
impl Store {
    pub fn open_existing_strict(path: &Path, durability: Durability) -> Result<Self, BlobError> {
        Self::from_native(Arc::new(RocksBlobs::open_existing_strict(
            path, durability,
        )?))
    }

    pub fn from_native(native: Arc<RocksBlobs>) -> Result<Self, BlobError> {
        if !native.has_family(META) {
            return Err(BlobError("missing Mako metadata family".into()));
        }
        let snapshot = native.snapshot()?;
        let mut names = BTreeMap::new();
        snapshot.scan_cf(
            META,
            &[0, 3],
            Some(&[0, 4]),
            false,
            usize::MAX,
            &mut |key, value| {
                if key.len() != 10 {
                    return Err(BlobError("invalid table catalog key".into()));
                }
                let id = u64::from_be_bytes(key[2..].try_into().unwrap());
                let name = String::from_utf8(value.to_vec())
                    .map_err(|_| BlobError("invalid family name".into()))?;
                if name == META || !native.has_family(&name) {
                    return Err(BlobError("missing or reserved user family".into()));
                }
                names.insert(id, name);
                Ok(true)
            },
        )?;
        Ok(Self {
            native,
            names: RwLock::new(names),
        })
    }
    pub fn snapshot(&self) -> Result<Snapshot, BlobError> {
        Ok(Snapshot {
            native: self.native.snapshot()?,
            names: self.names.read().unwrap().clone(),
        })
    }
    pub fn has_snapshot_leases(&self) -> bool {
        Arc::strong_count(&self.native) > 1
    }
    pub fn checkpoint(&self, path: &Path) -> Result<(), BlobError> {
        self.native.checkpoint(path)
    }
}
impl Blobs for Store {
    fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        if let Some((id, raw)) = row(key) {
            self.native
                .get_cf(family(&self.names.read().unwrap(), id)?, raw)
        } else {
            self.native.get_cf(META, key)
        }
    }
    fn write_batch(&self, ops: &[BlobOp<'_>]) -> Result<(), BlobError> {
        let mut names = self.names.write().unwrap();
        // Column-family creation is idempotent and precedes catalog publication.
        // A crash can leave an empty family; replay of the catalog command adopts
        // it. No row can be served before its catalog record commits.
        for op in ops {
            if let BlobOp::Put { key, val } = op {
                if key.starts_with(&[0, 3]) && key.len() == 10 {
                    let id = u64::from_be_bytes(key[2..].try_into().unwrap());
                    let name = std::str::from_utf8(val)
                        .map_err(|_| BlobError("invalid family name".into()))?;
                    if name == META || name.contains('\0') {
                        return Err(BlobError("reserved or invalid family name".into()));
                    }
                    if names.get(&id).is_some_and(|prior| prior != name)
                        || names
                            .iter()
                            .any(|(other, prior)| *other != id && prior == name)
                    {
                        return Err(BlobError("column-family identity changed".into()));
                    }
                    self.native.ensure_family(name)?;
                    names.insert(id, name.to_owned());
                }
            }
        }
        let native = ops
            .iter()
            .map(|op| {
                let (key, value) = match op {
                    BlobOp::Put { key, val } => (*key, Some(*val)),
                    BlobOp::Delete { key } => (*key, None),
                };
                let (name, key) = match row(key) {
                    Some((id, raw)) => (family(&names, id)?, raw),
                    None => (META, key),
                };
                Ok((
                    name,
                    match value {
                        Some(val) => BlobOp::Put { key, val },
                        None => BlobOp::Delete { key },
                    },
                ))
            })
            .collect::<Result<Vec<_>, BlobError>>()?;
        self.native.write_families(&native)
    }
    fn for_each_key(&self, f: &mut dyn FnMut(&[u8])) -> Result<(), BlobError> {
        self.for_each_entry_from(b"", usize::MAX, &mut |k, _| {
            f(k);
            Ok(true)
        })
    }
    fn for_each_entry_from(
        &self,
        start: &[u8],
        limit: usize,
        visitor: &mut BoundedEntryVisitor<'_>,
    ) -> Result<(), BlobError> {
        self.snapshot()?.scan(start, None, false, limit, visitor)
    }
    fn disk_usage_bytes(&self) -> Result<Option<u64>, BlobError> {
        self.native.disk_usage_bytes()
    }
}
impl Snapshot {
    pub fn get(&self, key: &[u8]) -> Result<Option<Vec<u8>>, BlobError> {
        match row(key) {
            Some((id, raw)) => self.native.get_cf(family(&self.names, id)?, raw),
            None => self.native.get_cf(META, key),
        }
    }
    pub fn scan(
        &self,
        start: &[u8],
        end: Option<&[u8]>,
        reverse: bool,
        limit: usize,
        visitor: &mut BoundedEntryVisitor<'_>,
    ) -> Result<(), BlobError> {
        if let Some((id, raw)) = row(start) {
            let stop = end
                .and_then(row)
                .and_then(|(end_id, key)| (id == end_id).then_some(key));
            self.native.scan_cf(
                family(&self.names, id)?,
                raw,
                stop,
                reverse,
                limit,
                &mut |key, val| {
                    let mut address = Vec::with_capacity(9 + key.len());
                    address.extend_from_slice(&start[..9]);
                    address.extend_from_slice(key);
                    visitor(&address, val)
                },
            )
        } else {
            self.native
                .scan_cf(META, start, end, reverse, limit, visitor)
        }
    }
}
