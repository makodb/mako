//! Recovery state that survives transaction-log reclamation.
//!
//! All integers use big-endian encoding. Each envelope includes an independent
//! format version and CRC32C over every preceding byte. Checkpoints always use
//! checksums, including when native transaction payload checksums are disabled.
//! These encodings are constructed by the background writer.

use mako_local::MakoTimestamp;
use mrx_core::BlobError;

use crate::record::{
    crc32c, make_log_key, CommitSeq, DEFAULT_TABLE_ID, LANE_KEY_PREFIX, LOG_LANE_SHIFT,
    LOG_LOCAL_MASK,
};

pub(crate) use crate::record::FORMAT_KEY;

const FORMAT_MAGIC: &[u8; 8] = b"MAKOCHK\0";
const ROW_MAGIC: &[u8; 8] = b"MAKOROW\0";
const LANE_MAGIC: &[u8; 8] = b"MAKOLAN\0";
const KEYSPACE_VERSION: u16 = 2;
const ENVELOPE_VERSION: u16 = 1;
const NAMESPACE: &[u8] = b"mako-cache-single-table\0";
const PUT: u8 = 1;
const TOMBSTONE: u8 = 2;
const ROW_HEADER_LEN: usize = 8 + 2 + 1 + 8 + 16 + 8;
const LANE_LEN: usize = 8 + 2 + 8 + 8 + 16 + 8 + 4;

fn invalid(message: &str) -> BlobError {
    BlobError(format!("invalid cache checkpoint: {message}"))
}

fn allocate(len: usize) -> Result<Vec<u8>, BlobError> {
    let mut bytes = Vec::new();
    bytes
        .try_reserve_exact(len)
        .map_err(|_| invalid("allocation failed"))?;
    Ok(bytes)
}

fn append_crc(bytes: &mut Vec<u8>) {
    bytes.extend_from_slice(&crc32c(bytes).to_be_bytes());
}

fn check_envelope<'a>(bytes: &'a [u8], magic: &[u8; 8]) -> Result<&'a [u8], BlobError> {
    if bytes.len() < 8 + 2 + 4 || bytes.get(..8) != Some(magic.as_slice()) {
        return Err(invalid("truncated envelope or wrong magic"));
    }
    let end = bytes.len() - 4;
    let expected = u32::from_be_bytes(bytes[end..].try_into().expect("four-byte trailer"));
    if crc32c(&bytes[..end]) != expected {
        return Err(invalid("checksum mismatch"));
    }
    Ok(&bytes[..end])
}

/// Fixed namespace identity and layout. A different namespace/layout requires
/// rebuilding the cache rather than interpreting the old rows as user values.
pub(crate) fn encode_format() -> Result<Vec<u8>, BlobError> {
    let mut bytes = allocate(8 + 2 + NAMESPACE.len() + 8 + 4)?;
    bytes.extend_from_slice(FORMAT_MAGIC);
    bytes.extend_from_slice(&KEYSPACE_VERSION.to_be_bytes());
    bytes.extend_from_slice(NAMESPACE);
    bytes.extend_from_slice(&DEFAULT_TABLE_ID.to_be_bytes());
    append_crc(&mut bytes);
    Ok(bytes)
}

pub(crate) fn validate_format(bytes: &[u8]) -> Result<(), BlobError> {
    check_envelope(bytes, FORMAT_MAGIC)?;
    if bytes != encode_format()? {
        return Err(invalid("unsupported format or namespace; rebuild required"));
    }
    Ok(())
}

/// Permanent coverage and HLC floor for one physical log stream.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct LaneMetadata {
    pub applied: u64,
    pub reclaimed: u64,
    pub max_timestamp: Option<MakoTimestamp>,
    /// Sum of retained log key and value lengths, excluding metadata and rows.
    pub retained_bytes: u64,
}

impl LaneMetadata {
    fn validate(self) -> Result<(), BlobError> {
        if self.applied > LOG_LOCAL_MASK || self.reclaimed > self.applied {
            return Err(invalid("lane frontiers exceed coverage or sequence range"));
        }
        if (self.applied == 0) != self.max_timestamp.is_none() {
            return Err(invalid("lane HLC presence disagrees with applied frontier"));
        }
        if (self.reclaimed == self.applied) != (self.retained_bytes == 0) {
            return Err(invalid("retained bytes disagree with lane suffix"));
        }
        Ok(())
    }

    pub(crate) fn encode(self) -> Result<Vec<u8>, BlobError> {
        self.validate()?;
        let mut bytes = allocate(LANE_LEN)?;
        bytes.extend_from_slice(LANE_MAGIC);
        bytes.extend_from_slice(&ENVELOPE_VERSION.to_be_bytes());
        bytes.extend_from_slice(&self.applied.to_be_bytes());
        bytes.extend_from_slice(&self.reclaimed.to_be_bytes());
        bytes.extend_from_slice(
            &self
                .max_timestamp
                .map_or([0; 16], MakoTimestamp::to_be_bytes),
        );
        bytes.extend_from_slice(&self.retained_bytes.to_be_bytes());
        append_crc(&mut bytes);
        Ok(bytes)
    }

    pub(crate) fn decode(bytes: &[u8]) -> Result<Self, BlobError> {
        if bytes.len() != LANE_LEN {
            return Err(invalid("lane metadata has wrong length"));
        }
        let body = check_envelope(bytes, LANE_MAGIC)?;
        if u16::from_be_bytes(body[8..10].try_into().unwrap()) != ENVELOPE_VERSION {
            return Err(invalid("unsupported lane metadata version"));
        }
        let timestamp_bytes: [u8; 16] = body[26..42].try_into().unwrap();
        let max_timestamp = if timestamp_bytes == [0; 16] {
            None
        } else {
            Some(
                MakoTimestamp::from_be_bytes(timestamp_bytes)
                    .ok_or_else(|| invalid("lane HLC has zero origin"))?,
            )
        };
        let metadata = Self {
            applied: u64::from_be_bytes(body[10..18].try_into().unwrap()),
            reclaimed: u64::from_be_bytes(body[18..26].try_into().unwrap()),
            max_timestamp,
            retained_bytes: u64::from_be_bytes(body[42..50].try_into().unwrap()),
        };
        metadata.validate()?;
        Ok(metadata)
    }
}

/// A borrowed current value or deletion marker with its winning identity.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct Row<'a> {
    pub timestamp: MakoTimestamp,
    pub sequence: CommitSeq,
    pub value: Option<&'a [u8]>,
}

pub(crate) fn encode_row(
    timestamp: MakoTimestamp,
    sequence: CommitSeq,
    value: Option<&[u8]>,
) -> Result<Vec<u8>, BlobError> {
    lane_tag(sequence)?;
    let payload = value.unwrap_or_default();
    let length = ROW_HEADER_LEN
        .checked_add(payload.len())
        .and_then(|n| n.checked_add(4))
        .ok_or_else(|| invalid("row length overflow"))?;
    let payload_len = u64::try_from(payload.len()).map_err(|_| invalid("value length overflow"))?;
    let mut bytes = allocate(length)?;
    bytes.extend_from_slice(ROW_MAGIC);
    bytes.extend_from_slice(&ENVELOPE_VERSION.to_be_bytes());
    bytes.push(if value.is_some() { PUT } else { TOMBSTONE });
    bytes.extend_from_slice(&sequence.get().to_be_bytes());
    bytes.extend_from_slice(&timestamp.to_be_bytes());
    bytes.extend_from_slice(&payload_len.to_be_bytes());
    bytes.extend_from_slice(payload);
    append_crc(&mut bytes);
    Ok(bytes)
}

pub(crate) fn decode_row(bytes: &[u8]) -> Result<Row<'_>, BlobError> {
    if bytes.len() < ROW_HEADER_LEN + 4 {
        return Err(invalid("truncated row"));
    }
    let body = check_envelope(bytes, ROW_MAGIC)?;
    if u16::from_be_bytes(body[8..10].try_into().unwrap()) != ENVELOPE_VERSION {
        return Err(invalid("unsupported row version"));
    }
    let sequence = CommitSeq::new(u64::from_be_bytes(body[11..19].try_into().unwrap()))
        .ok_or_else(|| invalid("row source sequence is zero"))?;
    lane_tag(sequence)?;
    let timestamp = MakoTimestamp::from_be_bytes(body[19..35].try_into().unwrap())
        .ok_or_else(|| invalid("row HLC has zero origin"))?;
    let payload_len = u64::from_be_bytes(body[35..43].try_into().unwrap());
    let payload = &body[ROW_HEADER_LEN..];
    if u64::try_from(payload.len()).ok() != Some(payload_len) {
        return Err(invalid("row payload length mismatch"));
    }
    let value = match body[10] {
        PUT => Some(payload),
        TOMBSTONE if payload.is_empty() => None,
        TOMBSTONE => return Err(invalid("tombstone contains a value")),
        _ => return Err(invalid("unknown row kind")),
    };
    Ok(Row {
        timestamp,
        sequence,
        value,
    })
}

/// Tag zero is the untagged stream; remaining tags name native worker slots.
pub(crate) fn lane_key(tag: u16) -> Vec<u8> {
    let mut key = Vec::with_capacity(LANE_KEY_PREFIX.len() + 2);
    key.extend_from_slice(LANE_KEY_PREFIX);
    key.extend_from_slice(&tag.to_be_bytes());
    key
}

pub(crate) fn lane_tag(sequence: CommitSeq) -> Result<u16, BlobError> {
    let tag = (sequence.get() >> LOG_LANE_SHIFT) as u16;
    if usize::from(tag) > mako_local::MAX_WORKERS || sequence.get() & LOG_LOCAL_MASK == 0 {
        return Err(invalid("unknown lane or zero local sequence"));
    }
    Ok(tag)
}

pub(crate) fn log_key(tag: u16, local: u64) -> Result<Vec<u8>, BlobError> {
    if usize::from(tag) > mako_local::MAX_WORKERS || local == 0 || local > LOG_LOCAL_MASK {
        return Err(invalid("unknown lane or out-of-range local sequence"));
    }
    let sequence =
        CommitSeq::new((u64::from(tag) << LOG_LANE_SHIFT) | local).expect("nonzero local sequence");
    make_log_key(sequence).map_err(|error| invalid(&error.to_string()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::record::{classify_backend_key, BackendKey};

    fn stamp() -> MakoTimestamp {
        MakoTimestamp::new(1_234_567_890_000, 17, 9).unwrap()
    }

    fn seq() -> CommitSeq {
        CommitSeq::new((3 << LOG_LANE_SHIFT) | 42).unwrap()
    }

    fn repair_crc(bytes: &mut [u8]) {
        let end = bytes.len() - 4;
        let checksum = crc32c(&bytes[..end]);
        bytes[end..].copy_from_slice(&checksum.to_be_bytes());
    }

    #[test]
    fn checkpoint_rows_preserve_put_empty_put_and_tombstone() {
        for value in [Some(b"\0\xffvalue".as_slice()), Some(b"".as_slice()), None] {
            let bytes = encode_row(stamp(), seq(), value).unwrap();
            assert_eq!(
                decode_row(&bytes).unwrap(),
                Row {
                    timestamp: stamp(),
                    sequence: seq(),
                    value
                }
            );
        }
    }

    #[test]
    fn every_checkpoint_byte_and_truncation_is_checked() {
        let lane = LaneMetadata {
            applied: 42,
            reclaimed: 17,
            max_timestamp: Some(stamp()),
            retained_bytes: 900,
        };
        for (bytes, decode) in [
            (
                encode_format().unwrap(),
                validate_format as fn(&[u8]) -> Result<(), BlobError>,
            ),
            (
                lane.encode().unwrap(),
                (|b: &[u8]| LaneMetadata::decode(b).map(|_| ()))
                    as fn(&[u8]) -> Result<(), BlobError>,
            ),
            (
                encode_row(stamp(), seq(), Some(b"payload")).unwrap(),
                (|b: &[u8]| decode_row(b).map(|_| ())) as fn(&[u8]) -> Result<(), BlobError>,
            ),
        ] {
            for len in 0..bytes.len() {
                assert!(
                    decode(&bytes[..len]).is_err(),
                    "accepted truncation at {len}"
                );
            }
            for index in 0..bytes.len() {
                let mut damaged = bytes.clone();
                damaged[index] ^= 1;
                assert!(
                    decode(&damaged).is_err(),
                    "accepted flipped byte at {index}"
                );
            }
            let mut trailing = bytes.clone();
            trailing.push(0);
            assert!(decode(&trailing).is_err());
        }
    }

    #[test]
    fn lane_metadata_covers_empty_partial_and_fully_reclaimed_lanes() {
        for lane in [
            LaneMetadata::default(),
            LaneMetadata {
                applied: 42,
                reclaimed: 17,
                max_timestamp: Some(stamp()),
                retained_bytes: 900,
            },
            LaneMetadata {
                applied: LOG_LOCAL_MASK,
                reclaimed: LOG_LOCAL_MASK,
                max_timestamp: Some(stamp()),
                retained_bytes: 0,
            },
        ] {
            assert_eq!(LaneMetadata::decode(&lane.encode().unwrap()).unwrap(), lane);
        }
        for lane in [
            LaneMetadata {
                applied: 1,
                ..LaneMetadata::default()
            },
            LaneMetadata {
                reclaimed: 1,
                ..LaneMetadata::default()
            },
            LaneMetadata {
                max_timestamp: Some(stamp()),
                ..LaneMetadata::default()
            },
            LaneMetadata {
                retained_bytes: 1,
                ..LaneMetadata::default()
            },
            LaneMetadata {
                applied: 1,
                reclaimed: 1,
                max_timestamp: Some(stamp()),
                retained_bytes: 1,
            },
            LaneMetadata {
                applied: LOG_LOCAL_MASK + 1,
                reclaimed: 0,
                max_timestamp: Some(stamp()),
                retained_bytes: 1,
            },
        ] {
            assert!(lane.encode().is_err(), "accepted invalid metadata {lane:?}");
        }
    }

    #[test]
    fn canonical_decoders_reject_semantically_invalid_rechecksummed_rows() {
        let row = encode_row(stamp(), seq(), Some(b"payload")).unwrap();
        for (offset, replacement) in [
            (8, vec![0, 99]),
            (10, vec![99]),
            (10, vec![TOMBSTONE]),
            (11, vec![0; 8]),
            (31, vec![0; 4]),
            (35, vec![255; 8]),
        ] {
            let mut changed = row.clone();
            changed[offset..offset + replacement.len()].copy_from_slice(&replacement);
            repair_crc(&mut changed);
            assert!(
                decode_row(&changed).is_err(),
                "accepted changed field at {offset}"
            );
        }
    }

    #[test]
    fn lane_decoder_rejects_invalid_frontiers_hlc_and_byte_accounting() {
        let lane = LaneMetadata {
            applied: 42,
            reclaimed: 17,
            max_timestamp: Some(stamp()),
            retained_bytes: 900,
        }
        .encode()
        .unwrap();
        for (offset, replacement) in [
            (8, vec![0, 99]),
            (10, (LOG_LOCAL_MASK + 1).to_be_bytes().to_vec()),
            (18, 43_u64.to_be_bytes().to_vec()),
            (26, vec![0; 16]),
            (38, vec![0; 4]),
            (42, 0_u64.to_be_bytes().to_vec()),
        ] {
            let mut changed = lane.clone();
            changed[offset..offset + replacement.len()].copy_from_slice(&replacement);
            repair_crc(&mut changed);
            assert!(
                LaneMetadata::decode(&changed).is_err(),
                "accepted changed field at {offset}"
            );
        }
        let mut changed_namespace = encode_format().unwrap();
        changed_namespace[10] ^= 1;
        repair_crc(&mut changed_namespace);
        assert!(validate_format(&changed_namespace).is_err());
    }

    #[test]
    fn checkpoint_keys_are_disjoint_and_reject_legacy_layout() {
        assert_eq!(classify_backend_key(FORMAT_KEY), BackendKey::Format);
        for tag in [0, 1, mako_local::MAX_WORKERS as u16] {
            assert_eq!(classify_backend_key(&lane_key(tag)), BackendKey::Lane(tag));
            let key = log_key(tag, 1).unwrap();
            let BackendKey::Log(sequence) = classify_backend_key(&key) else {
                panic!("expected log")
            };
            assert_eq!(lane_tag(sequence).unwrap(), tag);
            let mut legacy = key;
            legacy[b"\0mako-cache\0".len()] = 1;
            assert_eq!(classify_backend_key(&legacy), BackendKey::Foreign);
        }
        assert!(log_key(0, 0).is_err());
        assert!(log_key(0, LOG_LOCAL_MASK + 1).is_err());
        assert!(log_key(mako_local::MAX_WORKERS as u16 + 1, 1).is_err());
        assert_eq!(
            classify_backend_key(&lane_key(mako_local::MAX_WORKERS as u16 + 1)),
            BackendKey::Foreign
        );
        assert!(lane_tag(CommitSeq::new(1 << LOG_LANE_SHIFT).unwrap()).is_err());
        let mut extra = lane_key(1);
        extra.push(0);
        assert_eq!(classify_backend_key(&extra), BackendKey::Foreign);
    }
}
