//! Binary archives — the byte-buffer sinks/sources the serde layer
//! writes to and reads from.
//!
//! Port of `BinaryWriteArchive` / `BinaryReadArchive` over the
//! in-memory `BufferSink` / `BufferSource` (`src/rrr/misc/
//! serializable.cpp`). The C++ archives are polymorphic over
//! sink/source proxies (fd-backed, Marshal-backed, buffer-backed);
//! the wire *format* only needs the buffer pair, so that is what this
//! milestone ports. The C++ read path `verify()`s (aborts) on
//! underflow; the crate surfaces [`WireError`] instead — the
//! transpiled C++ consumption maps `Err` back to `verify`.

/// Wire-level failure. The only failure the wire layer itself can
/// produce is running out of bytes mid-decode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WireError {
    /// The source ran dry before the value was fully decoded.
    Underflow,
}

/// Append-only byte sink (the `BufferSink`-backed write archive).
pub struct WriteArchive {
    bytes: Vec<u8>,
}

impl Default for WriteArchive {
    fn default() -> Self {
        Self::new()
    }
}

impl WriteArchive {
    pub fn new() -> WriteArchive {
        WriteArchive { bytes: Vec::new() }
    }

    pub fn write_bytes(&mut self, p: &[u8]) {
        self.bytes.extend_from_slice(p);
    }

    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// The accumulated wire bytes.
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// Drop accumulated bytes, keeping the allocation (buffer-reuse
    /// pattern the transport pumps rely on — mirrors
    /// `sink.bytes.clear()` on the C++ side).
    pub fn clear(&mut self) {
        self.bytes.clear();
    }

    /// Consume the archive, yielding the wire bytes.
    pub fn into_bytes(self) -> Vec<u8> {
        self.bytes
    }
}

/// Cursor over received wire bytes (the `BufferSource`-backed read
/// archive).
pub struct ReadArchive<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> ReadArchive<'a> {
    pub fn new(buf: &'a [u8]) -> ReadArchive<'a> {
        ReadArchive { buf, pos: 0 }
    }

    /// Bytes not yet consumed.
    pub fn remaining(&self) -> usize {
        self.buf.len() - self.pos
    }

    /// Copy exactly `out.len()` bytes into `out`, or fail without
    /// consuming anything (mirrors `read_exact`'s all-or-nothing
    /// contract).
    pub fn read_exact(&mut self, out: &mut [u8]) -> Result<(), WireError> {
        if self.remaining() < out.len() {
            return Err(WireError::Underflow);
        }
        let n = out.len();
        out.copy_from_slice(&self.buf[self.pos..self.pos + n]);
        self.pos += n;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn write_then_read() {
        let mut w = WriteArchive::new();
        w.write_bytes(&[1, 2, 3]);
        w.write_bytes(&[4]);
        assert_eq!(w.as_bytes(), &[1, 2, 3, 4]);

        let bytes = w.into_bytes();
        let mut r = ReadArchive::new(&bytes);
        let mut out = [0u8; 3];
        r.read_exact(&mut out).unwrap();
        assert_eq!(out, [1, 2, 3]);
        assert_eq!(r.remaining(), 1);

        let mut over = [0u8; 2];
        assert_eq!(r.read_exact(&mut over), Err(WireError::Underflow));
        // All-or-nothing: the failed read consumed nothing.
        assert_eq!(r.remaining(), 1);
    }
}
