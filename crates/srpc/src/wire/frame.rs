//! Frame codec — the 4-byte size prefix that delimits messages on the
//! transport stream.
//!
//! Byte-exact port of `src/rrr/rpc/frame_codec.cpp` +
//! `src/rrr/rpc/internal_protocol.cpp` (whose DSL blocks are the
//! Rust-syntax source of these functions). Wire layout:
//!
//! ```text
//! [ i32 encoded_size (host byte order = little-endian) ][ payload... ]
//!   encoded_size = (payload_size & 0x7FFF_FFFF) | (ext_flag << 31)
//! ```
//!
//! * `payload_size` excludes the 4-byte header itself; max 2 GiB − 1.
//! * The high bit is the **extended-header flag**: the RPC layer
//!   interprets it as "the response payload starts with
//!   `<server_instance_id>` after `<error_code>`". Request frames must
//!   always have it clear.
//! * The size is written in host byte order (little-endian on every
//!   target we build for) — matching the C++ `memcpy` of the `i32`.

/// On-wire size of the frame header.
pub const FRAME_HEADER_SIZE: usize = 4;

/// Maximum payload size (low 31 bits of the header i32).
pub const MAX_FRAME_PAYLOAD_SIZE: i32 = 0x7fff_ffff;

const RESPONSE_HEADER_EXT_FLAG: u32 = 0x8000_0000;
const RESPONSE_SIZE_MASK: u32 = 0x7fff_ffff;

/// Result of peeking at a (possibly incomplete) frame header.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameDecodeStatus {
    NeedMoreBytes,
    Complete,
    Malformed,
}

/// Decoded view of the 4-byte size prefix.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FrameHeader {
    pub payload_size: i32,
    pub extended_header_flag: bool,
}

impl FrameHeader {
    /// Header + payload, in bytes.
    pub fn total_frame_size(&self) -> i32 {
        FRAME_HEADER_SIZE as i32 + self.payload_size
    }
}

pub fn response_has_extended_header(encoded_size: i32) -> bool {
    ((encoded_size as u32) & RESPONSE_HEADER_EXT_FLAG) != 0
}

pub fn response_payload_size(encoded_size: i32) -> i32 {
    ((encoded_size as u32) & RESPONSE_SIZE_MASK) as i32
}

pub fn encode_response_size(payload_size: i32, extended_header: bool) -> i32 {
    let base: u32 = (payload_size as u32) & RESPONSE_SIZE_MASK;
    let out: u32 = if extended_header {
        base | RESPONSE_HEADER_EXT_FLAG
    } else {
        base
    };
    out as i32
}

/// Write the 4-byte header into `out_buf`. Fails (returns false, like
/// the C++ kernel) on a negative or over-limit payload size.
pub fn write_header(
    out_buf: &mut [u8; FRAME_HEADER_SIZE],
    payload_size: i32,
    extended_header_flag: bool,
) -> bool {
    if payload_size < 0 {
        return false;
    }
    if payload_size > MAX_FRAME_PAYLOAD_SIZE {
        return false;
    }
    let encoded = encode_response_size(payload_size, extended_header_flag);
    out_buf.copy_from_slice(&encoded.to_le_bytes());
    true
}

/// Peek at the size prefix in `buf` (the full payload need not be
/// buffered). `Complete` only means "header decoded"; the caller still
/// compares against [`FrameHeader::total_frame_size`].
pub fn peek_header(buf: &[u8]) -> (FrameDecodeStatus, FrameHeader) {
    let mut header = FrameHeader::default();
    if buf.len() < FRAME_HEADER_SIZE {
        return (FrameDecodeStatus::NeedMoreBytes, header);
    }
    let mut raw = [0u8; FRAME_HEADER_SIZE];
    raw.copy_from_slice(&buf[..FRAME_HEADER_SIZE]);
    let encoded = i32::from_le_bytes(raw);

    let ext = response_has_extended_header(encoded);
    let payload = response_payload_size(encoded);
    if payload < 0 {
        return (FrameDecodeStatus::Malformed, header);
    }
    header.payload_size = payload;
    header.extended_header_flag = ext;
    (FrameDecodeStatus::Complete, header)
}

/// Append `[header][payload]` to `out` (the `frame_codec_encode_into`
/// kernel). Returns false and leaves `out` unchanged on an invalid
/// payload size.
pub fn encode_into(out: &mut Vec<u8>, payload: &[u8], extended_header_flag: bool) -> bool {
    if payload.len() > MAX_FRAME_PAYLOAD_SIZE as usize {
        return false;
    }
    let payload_size = payload.len() as i32;
    let mut header = [0u8; FRAME_HEADER_SIZE];
    if !write_header(&mut header, payload_size, extended_header_flag) {
        return false;
    }
    out.extend_from_slice(&header);
    out.extend_from_slice(payload);
    true
}

/// Streaming frame assembler (the `FrameStreamReader` role): feed
/// transport bytes with [`append`](FrameReader::append), pull complete
/// frames with [`next_frame`](FrameReader::next_frame).
pub struct FrameReader {
    buf: Vec<u8>,
    pos: usize,
}

impl Default for FrameReader {
    fn default() -> Self {
        Self::new()
    }
}

impl FrameReader {
    pub fn new() -> FrameReader {
        FrameReader {
            buf: Vec::new(),
            pos: 0,
        }
    }

    pub fn append(&mut self, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        self.buf.extend_from_slice(data);
    }

    /// Bytes buffered but not yet consumed as frames.
    pub fn buffered(&self) -> usize {
        self.buf.len() - self.pos
    }

    /// Decode the next complete frame, if fully buffered. Returns the
    /// header + payload (copied out; the C++ FrameView aliases the
    /// internal buffer, but a safe owned copy keeps this layer simple —
    /// the transport port revisits zero-copy if the benchmarks demand
    /// it). `Malformed` surfaces as `Err(())` like the C++ status.
    #[allow(clippy::result_unit_err)]
    pub fn next_frame(&mut self) -> Result<Option<(FrameHeader, Vec<u8>)>, ()> {
        let rem = &self.buf[self.pos..];
        let (status, header) = peek_header(rem);
        match status {
            FrameDecodeStatus::NeedMoreBytes => return Ok(None),
            FrameDecodeStatus::Malformed => return Err(()),
            FrameDecodeStatus::Complete => {}
        }
        let total = header.total_frame_size() as usize;
        if rem.len() < total {
            return Ok(None);
        }
        let payload = rem[FRAME_HEADER_SIZE..total].to_vec();
        self.pos += total;
        self.compact_if_needed();
        Ok(Some((header, payload)))
    }

    /// Drop consumed bytes once they dominate the buffer (mirrors
    /// `fsr_compact_if_needed`'s amortization intent).
    fn compact_if_needed(&mut self) {
        if self.pos > 4096 && self.pos * 2 >= self.buf.len() {
            self.buf.drain(..self.pos);
            self.pos = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_layout() {
        let mut h = [0u8; FRAME_HEADER_SIZE];
        assert!(write_header(&mut h, 5, false));
        assert_eq!(h, [0x05, 0x00, 0x00, 0x00]); // LE

        assert!(write_header(&mut h, 1, true));
        assert_eq!(h, [0x01, 0x00, 0x00, 0x80]); // ext flag = high bit

        assert!(!write_header(&mut h, -1, false));
    }

    #[test]
    fn peek_roundtrip() {
        let mut out = Vec::new();
        assert!(encode_into(&mut out, b"hello", false));
        assert_eq!(out.len(), FRAME_HEADER_SIZE + 5);
        let (st, h) = peek_header(&out);
        assert_eq!(st, FrameDecodeStatus::Complete);
        assert_eq!(h.payload_size, 5);
        assert!(!h.extended_header_flag);
        assert_eq!(h.total_frame_size(), 9);

        // Short buffer → NeedMoreBytes.
        let (st, _) = peek_header(&out[..3]);
        assert_eq!(st, FrameDecodeStatus::NeedMoreBytes);
    }

    #[test]
    fn stream_reassembly_across_splits() {
        // Two frames, delivered in awkward splits.
        let mut wire = Vec::new();
        assert!(encode_into(&mut wire, b"first", false));
        assert!(encode_into(&mut wire, b"2nd", true));

        let mut r = FrameReader::new();
        r.append(&wire[..2]); // partial header
        assert_eq!(r.next_frame(), Ok(None));
        r.append(&wire[2..7]); // header + partial payload
        assert_eq!(r.next_frame(), Ok(None));
        r.append(&wire[7..]);

        let (h1, p1) = r.next_frame().unwrap().unwrap();
        assert_eq!((h1.payload_size, h1.extended_header_flag), (5, false));
        assert_eq!(p1, b"first");

        let (h2, p2) = r.next_frame().unwrap().unwrap();
        assert_eq!((h2.payload_size, h2.extended_header_flag), (3, true));
        assert_eq!(p2, b"2nd");

        assert_eq!(r.next_frame(), Ok(None));
        assert_eq!(r.buffered(), 0);
    }

    #[test]
    fn empty_payload_frame() {
        let mut out = Vec::new();
        assert!(encode_into(&mut out, b"", false));
        assert_eq!(out, [0x00, 0x00, 0x00, 0x00]);
        let mut r = FrameReader::new();
        r.append(&out);
        let (h, p) = r.next_frame().unwrap().unwrap();
        assert_eq!(h.payload_size, 0);
        assert!(p.is_empty());
    }
}
