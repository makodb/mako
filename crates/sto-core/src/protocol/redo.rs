//! Checked conversion of the existing STO callback log to owned logical redo.
//! Native-endian legacy framing is consumed only at that local callback. Raft
//! persists the versioned Rust wire encoding, never native pointers or structs.
use crate::protocol::wire::{Error, ErrorCode, Result, MAX_BYTES, MAX_KEY, MAX_VALUE};
use crate::clock::distributed::DistributedTimeTerm;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Row {
    pub table: u16,
    pub key: Vec<u8>,
    pub value: Option<Vec<u8>>,
    pub time_term: u32,
}
impl Row {
    pub fn validate(&self) -> Result<()> {
        if self.table == 0
            || self.table >= 0x8000
            || self.key.len() > MAX_KEY
            || self
                .value
                .as_ref()
                .is_some_and(|value| value.len() > MAX_VALUE)
            || DistributedTimeTerm::decode_legacy(self.time_term).is_err()
        {
            return Err(invalid("invalid redo row"));
        }
        Ok(())
    }
    /// Same compare/apply rule used by STO's replay datatype.
    pub fn supersedes(&self, prior: u32) -> bool {
        self.time_term % 10 > prior % 10 || self.time_term / 10 > prior / 10
    }
}
fn invalid(message: &str) -> Error {
    Error::new(ErrorCode::Invalid, message)
}
struct Input<'a>(&'a [u8]);
impl<'a> Input<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8]> {
        if n > self.0.len() {
            return Err(invalid("truncated redo log"));
        }
        let (head, tail) = self.0.split_at(n);
        self.0 = tail;
        Ok(head)
    }
    fn u16(&mut self) -> Result<u16> {
        Ok(u16::from_ne_bytes(self.take(2)?.try_into().unwrap()))
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_ne_bytes(self.take(4)?.try_into().unwrap()))
    }
}
pub fn decode_legacy(bytes: &[u8], expected: u32) -> Result<Vec<Row>> {
    decode_legacy_bounded(bytes, expected, MAX_BYTES)
}

/// Volatile native intents can span many bounded RPC operations. Their owned
/// local log has a separate aggregate limit from one transport/durable frame.
pub fn decode_legacy_bounded(bytes: &[u8], expected: u32, limit: usize) -> Result<Vec<Row>> {
    if bytes.len() < 18 || bytes.len() > limit {
        return Err(invalid("redo log length"));
    }
    let mut input = Input(&bytes[..bytes.len() - 8]);
    let mut rows = Vec::new();
    let mut last = 0;
    while !input.0.is_empty() {
        let time_term = input.u32()?;
        DistributedTimeTerm::decode_legacy(time_term)
            .map_err(|_| invalid("redo source timestamp"))?;
        let count = usize::from(input.u16()?);
        let length = input.u32()? as usize;
        let mut records = Input(input.take(length)?);
        if count == 0 || count > length / 6 {
            return Err(invalid("redo record count"));
        }
        for _ in 0..count {
            let length = usize::from(records.u16()?);
            let key = records.take(length)?;
            let length = usize::from(records.u16()?);
            let value = records.take(length)?;
            let table = records.u16()?;
            let row = Row {
                table: table & 0x7fff,
                key: key.to_vec(),
                value: (table & 0x8000 == 0).then(|| value.to_vec()),
                time_term,
            };
            row.validate()?;
            if table & 0x8000 != 0 && value != [0] {
                return Err(invalid("redo delete encoding"));
            }
            rows.push(row);
        }
        if !records.0.is_empty() {
            return Err(invalid("redo record payload length"));
        }
        last = time_term;
    }
    let mut footer = Input(&bytes[bytes.len() - 8..]);
    if footer.u32()? != last || last != expected {
        return Err(invalid("redo decision mismatch"));
    }
    footer.u32()?; // Optional latency tracker, not replicated application state.
    Ok(rows)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn log() -> Vec<u8> {
        let mut bytes = 230u32.to_ne_bytes().to_vec();
        bytes.extend_from_slice(&2u16.to_ne_bytes());
        bytes.extend_from_slice(&16u32.to_ne_bytes());
        for (key, value, table) in [(b'a', b'b', 1u16), (b'c', 0, 0x8002)] {
            bytes.extend_from_slice(&1u16.to_ne_bytes());
            bytes.push(key);
            bytes.extend_from_slice(&1u16.to_ne_bytes());
            bytes.push(value);
            bytes.extend_from_slice(&table.to_ne_bytes());
        }
        bytes.extend_from_slice(&230u32.to_ne_bytes());
        bytes.extend_from_slice(&9u32.to_ne_bytes());
        bytes
    }
    #[test]
    fn redo_validates_entire_unit_before_exposing_rows() {
        let bytes = log();
        let rows = decode_legacy(&bytes, 230).unwrap();
        assert_eq!(rows[0].value.as_deref(), Some(b"b".as_slice()));
        assert_eq!(rows[1].value, None);
        assert_eq!(rows[1].table, 2);
        for end in 0..bytes.len() {
            assert!(decode_legacy(&bytes[..end], 230).is_err());
        }
        let mut broken = bytes.clone();
        broken[6..10].copy_from_slice(&15u32.to_ne_bytes());
        assert!(decode_legacy(&broken, 230).is_err());
        assert!(decode_legacy(&bytes, 240).is_err());
        let mut broken = bytes;
        broken[0..4].copy_from_slice(&0u32.to_ne_bytes());
        assert!(decode_legacy(&broken, 230).is_err());
    }
}
