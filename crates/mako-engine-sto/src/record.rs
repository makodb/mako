//! Canonical cache commit records: sizing and allocation-free encoding.
//!
//! The framing is [`mako_engine_api::record_format`]; this module produces
//! the canonical durable bytes for the final operations. Historical records
//! from the retired engine retain the same framing and replay semantics.

use std::ptr;

use mako_engine_api::record_format::{
    CRC_BYTES, HEADER_BYTES, MAGIC_V5, MAGIC_V6, OP_HEADER_BYTES, TAG_DELETE, TAG_PUT, VERSION_V5,
    VERSION_V6,
};
use mako_engine_api::CommitRecordChecksum;

/// Kind of one canonical final-effect mutation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum OpKind {
    Put,
    Delete,
}

/// One canonical mutation borrowed from the transaction's staged write set.
#[derive(Debug, Clone, Copy)]
pub(crate) struct OpRef<'a> {
    pub(crate) kind: OpKind,
    pub(crate) table_id: u64,
    pub(crate) key: &'a [u8],
    pub(crate) value: &'a [u8],
}

pub(crate) const fn trailer_bytes(checksum: CommitRecordChecksum) -> usize {
    match checksum {
        CommitRecordChecksum::Crc32c => CRC_BYTES,
        CommitRecordChecksum::None => 0,
    }
}

/// Exact encoded size of a record holding `ops`, or `None` on overflow.
pub(crate) fn exact_record_bytes(
    ops: &[OpRef<'_>],
    checksum: CommitRecordChecksum,
) -> Option<usize> {
    let mut total = HEADER_BYTES.checked_add(trailer_bytes(checksum))?;
    for op in ops {
        if op.key.len() > u32::MAX as usize || op.value.len() > u32::MAX as usize {
            return None;
        }
        total = total
            .checked_add(OP_HEADER_BYTES)?
            .checked_add(op.key.len())?
            .checked_add(op.value.len())?;
    }
    Some(total)
}

struct Cursor {
    base: *mut u8,
    offset: usize,
    end: usize,
}

impl Cursor {
    #[inline(always)]
    unsafe fn put(&mut self, bytes: &[u8]) -> bool {
        if bytes.len() > self.end - self.offset {
            return false;
        }
        // SAFETY: the caller guarantees `base` is valid for `end` bytes of
        // writes and the bound check above keeps this copy inside it.
        unsafe {
            ptr::copy_nonoverlapping(bytes.as_ptr(), self.base.add(self.offset), bytes.len())
        };
        self.offset += bytes.len();
        true
    }
}

/// Initialize exactly `exact` bytes of `record` with the canonical record.
///
/// Returns false, leaving the caller's completion witness unset, when the
/// operations do not fit `exact` precisely. `stamp` is the private 63-bit HLC
/// stamp; the header carries its public expansion.
///
/// # Safety
///
/// `record` must be valid for writes of `exact` bytes and must not alias any
/// operation's key or value.
pub(crate) unsafe fn encode_record(
    record: *mut u8,
    exact: usize,
    sequence: u64,
    stamp: u64,
    checksum: CommitRecordChecksum,
    ops: &[OpRef<'_>],
) -> bool {
    debug_assert!(sequence != 0);
    debug_assert!(mako_clock::valid_stamp(stamp));
    let Some(timestamp) = mako_clock::expand(stamp) else {
        return false;
    };
    if exact < HEADER_BYTES + trailer_bytes(checksum) || ops.is_empty() {
        return false;
    }
    let operations_end = exact - trailer_bytes(checksum);
    let mut cursor = Cursor {
        base: record,
        offset: 0,
        end: operations_end,
    };
    let (magic, version) = match checksum {
        CommitRecordChecksum::Crc32c => (&MAGIC_V5, VERSION_V5),
        CommitRecordChecksum::None => (&MAGIC_V6, VERSION_V6),
    };
    let Ok(op_count) = u32::try_from(ops.len()) else {
        return false;
    };
    // SAFETY: every `put` stays within `record[..operations_end]`, which the
    // caller made valid for writes.
    let header_ok = unsafe {
        cursor.put(magic)
            && cursor.put(&version.to_be_bytes())
            && cursor.put(&sequence.to_be_bytes())
            && cursor.put(&timestamp.physical_us().to_be_bytes())
            && cursor.put(&timestamp.logical().to_be_bytes())
            && cursor.put(&timestamp.origin().to_be_bytes())
            && cursor.put(&op_count.to_be_bytes())
    };
    if !header_ok || cursor.offset != HEADER_BYTES {
        return false;
    }
    for op in ops {
        let (Ok(key_len), Ok(value_len)) =
            (u32::try_from(op.key.len()), u32::try_from(op.value.len()))
        else {
            return false;
        };
        let tag = match op.kind {
            OpKind::Put => TAG_PUT,
            OpKind::Delete => TAG_DELETE,
        };
        // SAFETY: as above.
        let op_ok = unsafe {
            cursor.put(&[tag])
                && cursor.put(&op.table_id.to_be_bytes())
                && cursor.put(&key_len.to_be_bytes())
                && cursor.put(&value_len.to_be_bytes())
                && cursor.put(op.key)
                && cursor.put(op.value)
        };
        if !op_ok {
            return false;
        }
    }
    if cursor.offset != operations_end {
        return false;
    }
    if checksum == CommitRecordChecksum::Crc32c {
        // SAFETY: every byte before `operations_end` was just initialized.
        let covered = unsafe { std::slice::from_raw_parts(record.cast_const(), operations_end) };
        let crc = crc32c(covered);
        cursor.end = exact;
        // SAFETY: the trailer lies inside the caller's `exact` extent.
        if !unsafe { cursor.put(&crc.to_be_bytes()) } {
            return false;
        }
    }
    cursor.offset == exact
}

const fn crc32c_table() -> [u32; 256] {
    const REVERSED_CASTAGNOLI: u32 = 0x82F6_3B78;
    let mut table = [0u32; 256];
    let mut index = 0;
    while index < 256 {
        let mut value = index as u32;
        let mut bit = 0;
        while bit < 8 {
            let low_mask = 0u32.wrapping_sub(value & 1);
            value = (value >> 1) ^ (REVERSED_CASTAGNOLI & low_mask);
            bit += 1;
        }
        table[index] = value;
        index += 1;
    }
    table
}

static CRC32C_TABLE: [u32; 256] = crc32c_table();

/// CRC-32C (Castagnoli) over `bytes`, matching the native record trailer.
pub(crate) fn crc32c(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for byte in bytes {
        crc = CRC32C_TABLE[((crc ^ u32::from(*byte)) & 0xff) as usize] ^ (crc >> 8);
    }
    !crc
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32c_matches_the_reference_vector() {
        // RFC 3720 / common CRC-32C check value for "123456789".
        assert_eq!(crc32c(b"123456789"), 0xE306_9283);
        assert_eq!(crc32c(b""), 0);
    }

    #[test]
    fn one_put_v6_record_has_the_documented_layout() {
        let key = b"key";
        let value = b"value!";
        let ops = [OpRef {
            kind: OpKind::Put,
            table_id: 7,
            key,
            value,
        }];
        let exact = exact_record_bytes(&ops, CommitRecordChecksum::None).unwrap();
        assert_eq!(
            exact,
            HEADER_BYTES + OP_HEADER_BYTES + key.len() + value.len()
        );
        let mut out = vec![0u8; exact];
        let stamp = (2000u64 << mako_clock::LOGICAL_BITS) | 5;
        assert!(unsafe {
            encode_record(
                out.as_mut_ptr(),
                exact,
                42,
                stamp,
                CommitRecordChecksum::None,
                &ops,
            )
        });
        assert_eq!(&out[..8], b"MAKONOC\0");
        assert_eq!(&out[8..10], &6u16.to_be_bytes());
        assert_eq!(&out[10..18], &42u64.to_be_bytes());
        assert_eq!(&out[18..26], &2_000_000u64.to_be_bytes());
        assert_eq!(&out[26..30], &5u32.to_be_bytes());
        assert_eq!(&out[30..34], &1u32.to_be_bytes());
        assert_eq!(&out[34..38], &1u32.to_be_bytes());
        assert_eq!(out[38], TAG_PUT);
        assert_eq!(&out[39..47], &7u64.to_be_bytes());
        assert_eq!(&out[47..51], &3u32.to_be_bytes());
        assert_eq!(&out[51..55], &6u32.to_be_bytes());
        assert_eq!(&out[55..58], key);
        assert_eq!(&out[58..], value);
    }

    #[test]
    fn v5_record_carries_a_crc_over_every_preceding_byte() {
        let ops = [OpRef {
            kind: OpKind::Delete,
            table_id: 1,
            key: b"k",
            value: b"",
        }];
        let exact = exact_record_bytes(&ops, CommitRecordChecksum::Crc32c).unwrap();
        let mut out = vec![0u8; exact];
        let stamp = 1u64 << mako_clock::LOGICAL_BITS;
        assert!(unsafe {
            encode_record(
                out.as_mut_ptr(),
                exact,
                1,
                stamp,
                CommitRecordChecksum::Crc32c,
                &ops,
            )
        });
        assert_eq!(&out[..8], b"MAKOCMT\0");
        let body = &out[..exact - CRC_BYTES];
        assert_eq!(&out[exact - CRC_BYTES..], &crc32c(body).to_be_bytes());
        assert_eq!(out[38], TAG_DELETE);
    }

    #[test]
    fn encoding_refuses_a_wrong_extent() {
        let ops = [OpRef {
            kind: OpKind::Put,
            table_id: 1,
            key: b"k",
            value: b"v",
        }];
        let exact = exact_record_bytes(&ops, CommitRecordChecksum::None).unwrap();
        let mut out = vec![0u8; exact + 1];
        let stamp = 1u64 << mako_clock::LOGICAL_BITS;
        assert!(!unsafe {
            encode_record(
                out.as_mut_ptr(),
                exact + 1,
                1,
                stamp,
                CommitRecordChecksum::None,
                &ops,
            )
        });
        assert!(!unsafe {
            encode_record(
                out.as_mut_ptr(),
                exact - 1,
                1,
                stamp,
                CommitRecordChecksum::None,
                &ops,
            )
        });
    }
}
