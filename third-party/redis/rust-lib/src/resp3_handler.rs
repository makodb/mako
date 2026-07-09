use bytes::BytesMut;
use redis_protocol::error::RedisProtocolError;
use redis_protocol::resp3::{
    decode::streaming::decode_bytes_mut, types::BytesFrame, types::DecodedFrame,
};

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
        Self {
            buf: BytesMut::with_capacity(capacity),
        }
    }

    pub fn print_buffer(&self) {
        let raw: &[u8] = &*self.buf;
        println!("Buffer:\n{}", String::from_utf8_lossy(raw));
    }

    /// Buffer reads raw bytes (to be parsed).
    pub fn read_bytes(&mut self, data: &[u8]) {
        self.buf.extend_from_slice(data);
    }

    pub fn buffered(&self) -> &[u8] {
        &self.buf
    }

    pub fn consume(&mut self, len: usize) {
        let _ = self.buf.split_to(len);
    }

    fn maybe_rewrite_inline_command(&mut self) {
        if self.buf.is_empty()
            || matches!(
                self.buf[0],
                b'*' | b'+'
                    | b'-'
                    | b':'
                    | b'$'
                    | b'_'
                    | b'%'
                    | b'~'
                    | b'>'
                    | b'='
                    | b','
                    | b'#'
                    | b'('
                    | b'!'
            )
        {
            return;
        }
        let Some(line_end) = self.buf.iter().position(|b| *b == b'\n') else {
            return;
        };
        let mut line = self.buf.split_to(line_end + 1);
        while matches!(line.last(), Some(b'\n' | b'\r')) {
            line.truncate(line.len() - 1);
        }
        let parts: Vec<&[u8]> = line
            .as_ref()
            .split(|b| *b == b' ' || *b == b'\t')
            .filter(|part| !part.is_empty())
            .collect();
        if parts.is_empty() {
            return;
        }
        let mut encoded = BytesMut::new();
        encoded.extend_from_slice(format!("*{}\r\n", parts.len()).as_bytes());
        for part in parts {
            encoded.extend_from_slice(format!("${}\r\n", part.len()).as_bytes());
            encoded.extend_from_slice(part);
            encoded.extend_from_slice(b"\r\n");
        }
        encoded.extend_from_slice(&self.buf);
        self.buf = encoded;
    }

    /// Attempt to parse the next available frame.
    ///
    /// - Returns Ok(Some(frame)) when a full frame is parsed.
    /// - Returns Ok(None) if more data is needed.
    /// - Clears the buffer and return Ok(None) if an error was raised during parsing.
    pub fn next_frame(&mut self) -> Result<Option<DecodedFrame<BytesFrame>>, RedisProtocolError> {
        self.maybe_rewrite_inline_command();
        match decode_bytes_mut(&mut self.buf) {
            Ok(Some((frame, _consumed, _leftover))) => Ok(Some(frame)),
            Ok(None) => Ok(None),
            Err(e) => {
                eprintln!(
                    "Parse error: {} ---- the message below will be garbage collected",
                    e
                );
                eprintln!("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
                self.print_buffer();
                eprintln!("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
                self.buf.clear();
                Ok(None)
            }
        }
    }
}
