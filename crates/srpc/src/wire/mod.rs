//! The rrr wire format.
//!
//! Byte-exact port of `src/rrr/misc/serializable.cpp` +
//! `src/rrr/base/basetypes.cpp` (SparseInt / v32 / v64). The format:
//!
//! * **Fixed scalars** (`i8..i64`, `u8..u64`, `f64`): raw
//!   little-endian bytes of the native representation.
//! * **`V32` / `V64` varints**: SparseInt prefix coding — the first
//!   byte's leading-ones count encodes the total length (UTF-8
//!   style), payload big-endian under the tag bits, sign-extended on
//!   decode. See [`varint`] for the exact layout and the legacy
//!   8-length quirk.
//! * **Strings**: `[V64 len][bytes]`.
//! * **Sequences** (`Vec<T>` etc.): `[V64 len][element...]`.
//! * **Request body**: `[V64 xid][i32 rpc_id][args...]` (framing —
//!   the 4-byte `i32` payload-size header with its high extended-flag
//!   bit — lives in the frame codec, ported in a later milestone).
//! * **Envelope**: `[V32 kind][payload]`.

pub mod archive;
pub mod frame;
pub mod internal_protocol;
pub mod serde;
pub mod varint;

pub use archive::{ReadArchive, WireError, WriteArchive};
pub use frame::{FrameDecodeStatus, FrameHeader, FrameReader};
pub use internal_protocol::{
    encode_response_size, response_has_extended_header, response_payload_size,
};
pub use serde::{Deserialize, Serialize, V32, V64};
