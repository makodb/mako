//! Serialize / Deserialize — the wire codec surface.
//!
//! Port of the serde free-function layer in
//! `src/rrr/misc/serializable.cpp` (`rrr::Serialize_::serialize` /
//! `rrr::Deserialize_::deserialize`), with the same trait shape the
//! C++ DSL blocks already declare (`impl Serialize for v32 { fn
//! serialize(&self, ar) }`).
//!
//! Encodings (byte-exact with the C++ implementation):
//! * fixed-width scalars: raw little-endian native bytes;
//! * [`V32`] / [`V64`]: SparseInt varints ([`super::varint`]);
//! * `String` / `&str`: `[V64 len][utf8 bytes]`;
//! * `Vec<T>`, slices: `[V64 len][element...]`;
//! * `(A, B)` pairs: `first` then `second`.

use super::archive::{ReadArchive, WireError, WriteArchive};
use super::varint;
use super::varint::VARINT_BUF_LEN;

/// Types that can be encoded onto the rrr wire.
pub trait Serialize {
    fn serialize(&self, ar: &mut WriteArchive);
}

/// Types that can be decoded from the rrr wire.
pub trait Deserialize: Sized {
    fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError>;
}

// ---------------------------------------------------------------------------
// Varint wrappers (rrr::v32 / rrr::v64).

/// Variable-length i32 (the C++ `rrr::v32`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct V32(pub i32);

/// Variable-length i64 (the C++ `rrr::v64`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct V64(pub i64);

impl Serialize for V32 {
    fn serialize(&self, ar: &mut WriteArchive) {
        let mut b = [0u8; VARINT_BUF_LEN];
        let bsize = varint::dump32(self.0, &mut b);
        ar.write_bytes(&b[..bsize]);
    }
}

impl Serialize for V64 {
    fn serialize(&self, ar: &mut WriteArchive) {
        let mut b = [0u8; VARINT_BUF_LEN];
        let bsize = varint::dump64(self.0, &mut b);
        ar.write_bytes(&b[..bsize]);
    }
}

/// Shared varint decode: read the first byte, size the tail from it,
/// read the tail into a zero-padded buffer (mirrors the C++ decode
/// path's zero-initialized `VarintBuf`, which is what makes the
/// legacy 8-length quirk decode deterministically).
fn read_varint_buf(ar: &mut ReadArchive<'_>) -> Result<[u8; VARINT_BUF_LEN], WireError> {
    let mut b = [0u8; VARINT_BUF_LEN];
    ar.read_exact(&mut b[..1])?;
    let total = varint::buf_size(b[0]);
    if total > 1 {
        let (_, tail) = b.split_at_mut(1);
        ar.read_exact(&mut tail[..total - 1])?;
    }
    Ok(b)
}

impl Deserialize for V32 {
    fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError> {
        let b = read_varint_buf(ar)?;
        Ok(V32(varint::load32(&b)))
    }
}

impl Deserialize for V64 {
    fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError> {
        let b = read_varint_buf(ar)?;
        Ok(V64(varint::load64(&b)))
    }
}

// ---------------------------------------------------------------------------
// Fixed-width scalars: raw little-endian native bytes.

macro_rules! fixed_scalar {
    ($t:ty) => {
        impl Serialize for $t {
            fn serialize(&self, ar: &mut WriteArchive) {
                ar.write_bytes(&self.to_le_bytes());
            }
        }
        impl Deserialize for $t {
            fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError> {
                let mut b = [0u8; core::mem::size_of::<$t>()];
                ar.read_exact(&mut b)?;
                Ok(<$t>::from_le_bytes(b))
            }
        }
    };
}

fixed_scalar!(i8);
fixed_scalar!(i16);
fixed_scalar!(i32);
fixed_scalar!(i64);
fixed_scalar!(u8);
fixed_scalar!(u16);
fixed_scalar!(u32);
fixed_scalar!(u64);
fixed_scalar!(f64);

// ---------------------------------------------------------------------------
// Strings: [V64 len][utf8 bytes].

impl Serialize for str {
    fn serialize(&self, ar: &mut WriteArchive) {
        V64(self.len() as i64).serialize(ar);
        if !self.is_empty() {
            ar.write_bytes(self.as_bytes());
        }
    }
}

impl Serialize for String {
    fn serialize(&self, ar: &mut WriteArchive) {
        self.as_str().serialize(ar);
    }
}

impl Deserialize for String {
    fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError> {
        let len = V64::deserialize(ar)?.0;
        if len < 0 {
            return Err(WireError::Underflow);
        }
        let mut bytes = vec![0u8; len as usize];
        ar.read_exact(&mut bytes)?;
        // The C++ side treats string payloads as raw bytes; rrr
        // strings are byte strings in practice. Lossless carrier:
        match String::from_utf8(bytes) {
            Ok(s) => Ok(s),
            Err(_) => Err(WireError::Underflow),
        }
    }
}

// ---------------------------------------------------------------------------
// Sequences: [V64 len][element...].

impl<T: Serialize> Serialize for [T] {
    fn serialize(&self, ar: &mut WriteArchive) {
        V64(self.len() as i64).serialize(ar);
        let mut i = 0;
        while i < self.len() {
            self[i].serialize(ar);
            i += 1;
        }
    }
}

impl<T: Serialize> Serialize for Vec<T> {
    fn serialize(&self, ar: &mut WriteArchive) {
        self.as_slice().serialize(ar);
    }
}

impl<T: Deserialize> Deserialize for Vec<T> {
    fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError> {
        let len = V64::deserialize(ar)?.0;
        if len < 0 {
            return Err(WireError::Underflow);
        }
        let mut v: Vec<T> = Vec::with_capacity(len as usize);
        let mut i: i64 = 0;
        while i < len {
            v.push(T::deserialize(ar)?);
            i += 1;
        }
        Ok(v)
    }
}

// ---------------------------------------------------------------------------
// Pairs: first then second (std::pair<T1, T2>).

impl<A: Serialize, B: Serialize> Serialize for (A, B) {
    fn serialize(&self, ar: &mut WriteArchive) {
        self.0.serialize(ar);
        self.1.serialize(ar);
    }
}

impl<A: Deserialize, B: Deserialize> Deserialize for (A, B) {
    fn deserialize(ar: &mut ReadArchive<'_>) -> Result<Self, WireError> {
        let a = A::deserialize(ar)?;
        let b = B::deserialize(ar)?;
        Ok((a, b))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn roundtrip<T: Serialize + Deserialize + PartialEq + core::fmt::Debug>(v: T) {
        let mut w = WriteArchive::new();
        v.serialize(&mut w);
        let bytes = w.into_bytes();
        let mut r = ReadArchive::new(&bytes);
        let back = T::deserialize(&mut r).expect("decode");
        assert_eq!(back, v);
        assert_eq!(r.remaining(), 0, "no trailing bytes");
    }

    #[test]
    fn scalars_roundtrip() {
        roundtrip(0i32);
        roundtrip(-1i32);
        roundtrip(i32::MAX);
        roundtrip(i64::MIN);
        roundtrip(255u8);
        roundtrip(65535u16);
        roundtrip(u64::MAX);
        // Not f64::consts::PI: this literal must match the C++ golden
        // corpus generator's `3.14159` byte-for-byte.
        #[allow(clippy::approx_constant)]
        roundtrip(3.14159f64);
    }

    #[test]
    fn scalars_are_little_endian_fixed() {
        let mut w = WriteArchive::new();
        0x01020304i32.serialize(&mut w);
        assert_eq!(w.as_bytes(), &[0x04, 0x03, 0x02, 0x01]);
    }

    #[test]
    fn varints_roundtrip() {
        roundtrip(V32(0));
        roundtrip(V32(i32::MAX));
        roundtrip(V32(i32::MIN));
        roundtrip(V64(0));
        roundtrip(V64(i64::MAX));
        roundtrip(V64(i64::MIN));
        roundtrip(V64(281474976710655)); // 7-byte boundary
    }

    #[test]
    fn strings() {
        roundtrip(String::new());
        roundtrip(String::from("a"));
        roundtrip(String::from("hello rrr wire"));
        // len rides as a V64: 1-byte for short strings.
        let mut w = WriteArchive::new();
        "ab".serialize(&mut w);
        assert_eq!(w.as_bytes(), &[0x02, b'a', b'b']);
    }

    #[test]
    fn vectors_and_pairs() {
        roundtrip::<Vec<i32>>(vec![]);
        roundtrip(vec![1i32, -2, 3]);
        roundtrip(vec![String::from("x"), String::from("yz")]);
        roundtrip((V64(7), String::from("kv")));
        // [V64 len][elements]: 3 fixed i32s after a 1-byte len.
        let mut w = WriteArchive::new();
        vec![1i32, 2, 3].serialize(&mut w);
        assert_eq!(w.len(), 1 + 3 * 4);
        assert_eq!(w.as_bytes()[0], 0x03);
    }

    #[test]
    fn request_body_shape() {
        // [V64 xid][i32 rpc_id][args...] — the client request layout
        // (clientconn_request_via_channel).
        let mut w = WriteArchive::new();
        V64(1).serialize(&mut w);
        0x1234i32.serialize(&mut w);
        String::from("payload").serialize(&mut w);
        let bytes = w.into_bytes();
        let mut r = ReadArchive::new(&bytes);
        assert_eq!(V64::deserialize(&mut r).unwrap(), V64(1));
        assert_eq!(i32::deserialize(&mut r).unwrap(), 0x1234);
        assert_eq!(String::deserialize(&mut r).unwrap(), "payload");
        assert_eq!(r.remaining(), 0);
    }
}
