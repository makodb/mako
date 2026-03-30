use bytes::BytesMut;
use redis_protocol::resp3::{decode::streaming::decode_bytes_mut, types::BytesFrame, types::DecodedFrame};
use redis_protocol::error::RedisProtocolError;

/// BytesFrame from redis_protocol: an enum type that includes all RESP3 types:
// pub enum BytesFrame {
//     BlobString,
//     BlobError,
//     SimpleString,
//     SimpleError,
//     Boolean,
//     Null,
//     Number,
//     Double,
//     BigNumber,
//     VerbatimString,
//     Array,
//     Map,
//     Set,
//     Push,
//     Hello,
//     ChunkedString(Bytes),
// }


/// Handler that buffers incoming bytes and parses complete RESP3 frames.
pub struct Resp3Handler {
    buf: BytesMut,
}

impl Resp3Handler {
    /// Create a new handler with a buffer of given capacity.
    pub fn new(capacity: usize) -> Self {
        Self { buf: BytesMut::with_capacity(capacity) }
    }

    pub fn print_buffer(&self) {
        let raw: &[u8] = &*self.buf;
        println!("Buffer:\n{}", String::from_utf8_lossy(raw));
    }

    /// Buffer reads raw bytes (to be parsed).
    pub fn read_bytes(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    /// Attempt to parse the next available frame.
    ///
    /// - Returns Ok(Some(frame)) when a full frame is parsed.
    /// - Returns Ok(None) if more data is needed.
    /// - Clears the buffer and return Ok(None) if an error was raised during parsing.
    pub fn next_frame(&mut self) -> Result<Option<DecodedFrame<BytesFrame>>, RedisProtocolError> {
        match decode_bytes_mut(&mut self.buf) {
            Ok(Some((frame, _consumed, _leftover))) => {
                Ok(Some(frame))
            }
            Ok(None) => {
                Ok(None)
            }
            Err(e) => {
                eprintln!("Parse error: {} ---- the message below will be garbage collected", e);
                eprintln!("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
                self.print_buffer();
                eprintln!("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
                self.buf.clear();
                Ok(None)
            }
        }
    }
}
