//! Redis records use bounded physical rows. Full keys disambiguate hash
//! collisions; tombstoned directory entries keep collision chains intact.
//! All directory and chunk operations belong to the caller's STO transaction.
use crate::wire::{Error, ErrorCode, Result};

pub(super) const MAX_KEY: usize = 8192;
pub(super) const MAX_VALUE: usize = 16 * 1024 * 1024;
const CHUNK: usize = 32 * 1024;
const SLOTS: u8 = 64;
const ABSENT: u32 = u32::MAX;

pub(super) trait Rows {
    fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>>;
    fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()>;
    fn remove(&mut self, key: &[u8]) -> Result<()>;
}
fn invalid() -> Error {
    Error::new(ErrorCode::Fatal, "invalid Redis directory record")
}
fn conflict() -> Error {
    Error::new(
        ErrorCode::Conflict,
        "Redis chunks changed during optimistic read",
    )
}
fn hash(key: &[u8]) -> u64 {
    key.iter().fold(0xcbf29ce484222325, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x100000001b3)
    })
}
fn physical(hash: u64, slot: u8, part: Option<usize>) -> Vec<u8> {
    let mut key = b"\0mako-redis\x01".to_vec();
    key.extend_from_slice(&hash.to_be_bytes());
    key.push(slot);
    key.extend_from_slice(&(part.map_or(0, |part| part + 1) as u16).to_be_bytes());
    key
}
struct Record<'a> {
    key: &'a [u8],
    length: Option<usize>,
    inline: &'a [u8],
}
fn decode(bytes: &[u8]) -> Result<Record<'_>> {
    if bytes.len() < 12 || &bytes[..4] != b"MKR1" {
        return Err(invalid());
    }
    let key_len = u32::from_le_bytes(bytes[4..8].try_into().unwrap()) as usize;
    let length = u32::from_le_bytes(bytes[8..12].try_into().unwrap());
    let length = (length != ABSENT).then_some(length as usize);
    if key_len > MAX_KEY || length.is_some_and(|n| n > MAX_VALUE) {
        return Err(invalid());
    }
    let inline_len = length.filter(|n| *n <= CHUNK).unwrap_or(0);
    if bytes.len() != 12 + key_len + inline_len {
        return Err(invalid());
    }
    Ok(Record {
        key: &bytes[12..12 + key_len],
        length,
        inline: &bytes[12 + key_len..],
    })
}
fn encode(key: &[u8], value: Option<&[u8]>) -> Vec<u8> {
    let mut record = b"MKR1".to_vec();
    record.extend_from_slice(&(key.len() as u32).to_le_bytes());
    record.extend_from_slice(&value.map_or(ABSENT, |v| v.len() as u32).to_le_bytes());
    record.extend_from_slice(key);
    if let Some(value) = value.filter(|value| value.len() <= CHUNK) {
        record.extend_from_slice(value);
    }
    record
}
fn locate(rows: &mut impl Rows, key: &[u8], hash: u64) -> Result<Option<(u8, Option<Vec<u8>>)>> {
    for slot in 0..SLOTS {
        let record = rows.get(&physical(hash, slot, None))?;
        if record
            .as_ref()
            .is_none_or(|bytes| decode(bytes).is_ok_and(|record| record.key == key))
        {
            return Ok(Some((slot, record)));
        }
        // Validate even nonmatching entries. A malformed chain cannot establish
        // authoritative absence for a colliding key.
        decode(record.as_ref().unwrap())?;
    }
    Ok(None)
}
fn chunks(length: Option<usize>) -> usize {
    length
        .filter(|n| *n > CHUNK)
        .map_or(0, |n| n.div_ceil(CHUNK))
}
pub(super) fn get(rows: &mut impl Rows, key: &[u8]) -> Result<Option<Vec<u8>>> {
    get_hashed(rows, key, hash(key))
}
fn get_hashed(rows: &mut impl Rows, key: &[u8], hash: u64) -> Result<Option<Vec<u8>>> {
    let Some((slot, Some(bytes))) = locate(rows, key, hash)? else {
        return Ok(None);
    };
    let record = decode(&bytes)?;
    let Some(length) = record.length else {
        return Ok(None);
    };
    if length <= CHUNK {
        return Ok(Some(record.inline.to_vec()));
    }
    let mut value = Vec::with_capacity(length);
    for part in 0..chunks(Some(length)) {
        let chunk = rows
            .get(&physical(hash, slot, Some(part)))?
            .ok_or_else(conflict)?;
        if chunk.len() != CHUNK.min(length - value.len()) {
            return Err(conflict());
        }
        value.extend(chunk);
    }
    Ok(Some(value))
}
pub(super) fn set(rows: &mut impl Rows, key: &[u8], value: Option<&[u8]>) -> Result<bool> {
    set_hashed(rows, key, value, hash(key))
}
fn set_hashed(rows: &mut impl Rows, key: &[u8], value: Option<&[u8]>, hash: u64) -> Result<bool> {
    if key.len() > MAX_KEY || value.is_some_and(|v| v.len() > MAX_VALUE) {
        return Err(Error::new(ErrorCode::Capacity, "Redis record limit"));
    }
    let Some((slot, prior)) = locate(rows, key, hash)? else {
        if value.is_none() {
            return Ok(false);
        }
        return Err(Error::new(ErrorCode::Capacity, "Redis hash bucket full"));
    };
    let old_length = prior
        .as_ref()
        .map(|bytes| decode(bytes).map(|r| r.length))
        .transpose()?
        .flatten();
    if value.is_none() && old_length.is_none() {
        return Ok(false);
    }
    let new_chunks = chunks(value.map(<[u8]>::len));
    if new_chunks != 0 {
        for (part, chunk) in value.unwrap().chunks(CHUNK).enumerate() {
            rows.put(&physical(hash, slot, Some(part)), chunk)?;
        }
    }
    for part in new_chunks..chunks(old_length) {
        rows.remove(&physical(hash, slot, Some(part)))?;
    }
    rows.put(&physical(hash, slot, None), &encode(key, value))?;
    Ok(old_length.is_some())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;
    #[derive(Default)]
    struct Memory(BTreeMap<Vec<u8>, Vec<u8>>);
    impl Rows for Memory {
        fn get(&mut self, key: &[u8]) -> Result<Option<Vec<u8>>> {
            Ok(self.0.get(key).cloned())
        }
        fn put(&mut self, key: &[u8], value: &[u8]) -> Result<()> {
            self.0.insert(key.to_vec(), value.to_vec());
            Ok(())
        }
        fn remove(&mut self, key: &[u8]) -> Result<()> {
            self.0.remove(key);
            Ok(())
        }
    }
    #[test]
    fn forced_hash_collisions_and_deleted_slots_preserve_full_key_identity() {
        let mut rows = Memory::default();
        let a = vec![0; MAX_KEY];
        let b = b"\0mako-redis\x01";
        set_hashed(&mut rows, &a, Some(b"a"), 7).unwrap();
        set_hashed(&mut rows, b, Some(b"b"), 7).unwrap();
        assert_eq!(get_hashed(&mut rows, &a, 7).unwrap(), Some(b"a".to_vec()));
        assert!(set_hashed(&mut rows, &a, None, 7).unwrap());
        assert_eq!(get_hashed(&mut rows, b, 7).unwrap(), Some(b"b".to_vec()));
        set_hashed(&mut rows, b"third", Some(b""), 7).unwrap();
        set_hashed(&mut rows, &a, Some(b"restored"), 7).unwrap();
        assert_eq!(
            get_hashed(&mut rows, &a, 7).unwrap(),
            Some(b"restored".to_vec())
        );
        assert_eq!(get_hashed(&mut rows, b"third", 7).unwrap(), Some(vec![]));
    }
    #[test]
    fn chunk_growth_shrink_delete_and_missing_chunk_detection() {
        let mut rows = Memory::default();
        for size in [0, CHUNK, CHUNK + 1, 10 * 1024 * 1024, 1] {
            let value = (0..size).map(|i| i as u8).collect::<Vec<_>>();
            set(&mut rows, b"key", Some(&value)).unwrap();
            assert_eq!(get(&mut rows, b"key").unwrap(), Some(value));
            assert_eq!(rows.0.len(), 1 + chunks(Some(size)));
        }
        assert!(set(&mut rows, b"key", None).unwrap());
        assert_eq!(get(&mut rows, b"key").unwrap(), None);
        assert_eq!(rows.0.len(), 1);
        set(&mut rows, b"key", Some(&vec![3; CHUNK + 1])).unwrap();
        rows.0.remove(&physical(hash(b"key"), 0, Some(1)));
        assert_eq!(
            get(&mut rows, b"key").unwrap_err().code,
            ErrorCode::Conflict
        );
    }
}
