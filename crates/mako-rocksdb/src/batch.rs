use crate::{ColumnFamily, Error, Result, MAX_BATCH_BYTES, MAX_BATCH_OPERATIONS};

pub(crate) struct Entry {
    pub cf: Option<ColumnFamily>,
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
}
/// Atomic at `DB::write`, including across column families and shards.
#[derive(Default)]
pub struct WriteBatch {
    pub(crate) entries: Vec<Entry>,
    pub(crate) error: Option<Error>,
    bytes: usize,
}
impl WriteBatch {
    pub fn put<K: AsRef<[u8]>, V: AsRef<[u8]>>(&mut self, key: K, value: V) {
        self.add(None, key.as_ref(), Some(value.as_ref()));
    }
    pub fn put_cf<K: AsRef<[u8]>, V: AsRef<[u8]>>(&mut self, cf: &ColumnFamily, key: K, value: V) {
        self.add(Some(cf.clone()), key.as_ref(), Some(value.as_ref()));
    }
    pub fn delete<K: AsRef<[u8]>>(&mut self, key: K) {
        self.add(None, key.as_ref(), None);
    }
    pub fn delete_cf<K: AsRef<[u8]>>(&mut self, cf: &ColumnFamily, key: K) {
        self.add(Some(cf.clone()), key.as_ref(), None);
    }
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn clear(&mut self) {
        *self = Self::default();
    }
    fn add(&mut self, cf: Option<ColumnFamily>, key: &[u8], value: Option<&[u8]>) {
        if self.error.is_some() {
            return;
        }
        let valid: Result<()> = (|| {
            crate::validate_key(key)?;
            if let Some(value) = value {
                crate::validate_value(value)?;
            }
            let bytes = key.len() + value.map_or(0, <[u8]>::len);
            if self.entries.len() >= MAX_BATCH_OPERATIONS || bytes > MAX_BATCH_BYTES - self.bytes {
                return Err(Error::invalid(
                    "write batch exceeds operation or byte limit",
                ));
            }
            self.bytes += bytes;
            Ok(())
        })();
        if let Err(error) = valid {
            self.error = Some(error);
            return;
        }
        self.entries.push(Entry {
            cf,
            key: key.to_vec(),
            value: value.map(<[u8]>::to_vec),
        });
    }
}
