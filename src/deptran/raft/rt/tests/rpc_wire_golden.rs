// Golden wire vectors for the Raft RPC structs.
//
// WHY THESE AND NOT A ROUND-TRIP TEST. A round-trip proves the encoder and
// decoder agree with each other, but there the implementation is its own
// oracle: both halves can drift together, stay self-consistent, and still be
// wire-incompatible with the C++ peers this must interoperate with. Golden
// vectors pin the exact bytes instead.
//
// WHY PINNING RUST ALSO PINS C++. The C++ lane's srpc::Serialize_ is
// TRANSPILED from the same src/srpc/misc/serializable.rs these structs call,
// so a change that alters the wire alters both lanes together and is caught
// here. This is the argument src/srpc/tests/wire_golden_rust.rs makes for its
// own vectors, applied to the Raft slice.
//
// THE EXPECTED BYTES ARE DERIVED, NOT CAPTURED. Each scalar impl in
// serializable.rs copies the value's own bytes with no byte-order
// normalisation -- `write_bytes(self as *const u64 as *const u8, 8)` -- so a
// field's encoding is its width in native-endian order, and a struct's is its
// fields concatenated in declaration order with no tag, no length and no
// padding. The vectors below are written out from that rule by hand; if the
// encoder disagrees, the encoder is what changed.
//
// Fixed-width fields are native-endian by deliberate design (the srpc wire is
// not portable across endianness), so these assert on little-endian targets,
// which is what mako runs on.

use raft_rt::rpc::{
    AppendEntriesRequest, EmptyAppendEntriesRequest, InstallSnapshotRequest, VoteRequest,
    VoteResponse, WireBytes,
};
use srpc::serializable::{
    make_sink_proxy_buffer, BinaryWriteArchive, BufferSink, Serialize,
};

/// Serialize one value and hand back the bytes it wrote. Same shape as the
/// helper in src/srpc/tests/wire_golden_rust.rs, so both sets of vectors are
/// produced the same way.
fn encode<T: Serialize>(value: &T) -> Vec<u8> {
    let mut sink = BufferSink { bytes: Vec::new() };
    {
        // SAFETY: `sink` outlives the archive that borrows it.
        let mut ar = BinaryWriteArchive {
            sink_: unsafe { make_sink_proxy_buffer(&raw mut sink) },
        };
        value.serialize(&mut ar);
    }
    sink.bytes.clone()
}

#[cfg(target_endian = "little")]
#[test]
fn vote_request_is_its_fields_in_declaration_order() {
    let req = VoteRequest {
        lst_log_idx: 0x0102_0304_0506_0708,
        lst_log_term: 0x1112_1314_1516_1718,
        site_id: 0x2122,
        cur_term: 0x3132_3334_3536_3738,
    };
    let expected: Vec<u8> = [
        // lst_log_idx: u64, 8 bytes little-endian
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
        // lst_log_term: ballot_t = int64_t, 8 bytes
        0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
        // site_id: siteid_t = uint16_t, 2 bytes -- NOT widened
        0x22, 0x21,
        // cur_term: ballot_t = int64_t, 8 bytes
        0x38, 0x37, 0x36, 0x35, 0x34, 0x33, 0x32, 0x31,
    ]
    .to_vec();
    assert_eq!(encode(&req), expected, "VoteRequest wire bytes moved");
    assert_eq!(expected.len(), 8 + 8 + 2 + 8, "unexpected total width");
}

#[cfg(target_endian = "little")]
#[test]
fn vote_response_keeps_bool_t_one_byte_wide() {
    // bool_t is `#define bool_t int8_t` (src/deptran/constants.h:26). A Rust
    // `bool` would also be one byte today, but nothing in the language says
    // so, and widening it silently would desynchronise every Vote reply.
    let resp = VoteResponse { max_ballot: 0x4142_4344_4546_4748, vote_granted: 1 };
    let expected: Vec<u8> = [
        0x48, 0x47, 0x46, 0x45, 0x44, 0x43, 0x42, 0x41, // max_ballot: i64
        0x01, // vote_granted: i8
    ]
    .to_vec();
    assert_eq!(encode(&resp), expected);
    assert_eq!(expected.len(), 9, "bool_t must occupy exactly one byte");
}

#[cfg(target_endian = "little")]
#[test]
fn empty_append_entries_request_is_forty_six_bytes() {
    let req = EmptyAppendEntriesRequest {
        slot: 1,
        ballot: 2,
        leader_current_term: 3,
        leader_site_id: 4,
        leader_prev_log_index: 5,
        leader_prev_log_term: 6,
        leader_commit_index: 7,
    };
    // six 8-byte fields plus one 2-byte siteid_t, in declaration order
    let bytes = encode(&req);
    assert_eq!(bytes.len(), 8 * 6 + 2);
    assert_eq!(&bytes[0..8], &1u64.to_le_bytes());
    assert_eq!(&bytes[8..16], &2i64.to_le_bytes());
    assert_eq!(&bytes[16..24], &3u64.to_le_bytes());
    assert_eq!(&bytes[24..26], &4u16.to_le_bytes(), "siteid_t sits mid-struct");
    assert_eq!(&bytes[26..34], &5u64.to_le_bytes());
}

#[cfg(target_endian = "little")]
#[test]
fn install_snapshot_request_length_prefixes_its_string() {
    // `data` is std::string, the one variable-length field in the Raft slice,
    // and it is the reason InstallSnapshot can be framed while AppendEntries
    // cannot: String carries its own length, janus::Command does not.
    let short = InstallSnapshotRequest {
        term: 1,
        leader_id: 2,
        last_included_index: 3,
        last_included_term: 4,
        data: WireBytes::default(),
    };
    let long = InstallSnapshotRequest { data: WireBytes(b"abcd".to_vec()), ..short.clone() };
    let empty_len = encode(&short).len();
    let four_len = encode(&long).len();
    assert_eq!(
        four_len - empty_len,
        4,
        "a four-byte payload must add exactly four bytes; the length prefix \
         itself must not change width"
    );
}

#[cfg(target_endian = "little")]
#[test]
fn append_entries_writes_its_payload_raw_between_the_fixed_fields() {
    // `cmd` is the janus::Command envelope, opaque to Rust and UNFRAMED on the
    // wire: the C++ lane writes the envelope's own bytes with no length
    // prefix, and from_body bounds it by arithmetic -- offset 50 to len - 8.
    // A v64 length prefix here (what serializing the Vec<u8> would add)
    // shifted every byte after it and misparsed on both lanes.
    let cmd: Vec<u8> = vec![0xde, 0xad, 0xbe, 0xef, 0x00, 0x7f, 0x80];
    let req = AppendEntriesRequest {
        slot: u64::MAX,
        ballot: -1,
        leader_current_term: 5,
        leader_site_id: 2,
        leader_prev_log_index: 9,
        leader_prev_log_term: 4,
        leader_commit_index: 8,
        cmd: cmd.clone(),
        leader_next_log_term: 5,
    };
    let bytes = encode(&req);
    assert_eq!(bytes.len(), 50 + cmd.len() + 8, "no framing around cmd");
    assert_eq!(&bytes[0..8], &u64::MAX.to_le_bytes());
    assert_eq!(&bytes[8..16], &(-1i64).to_le_bytes());
    assert_eq!(&bytes[24..26], &2u16.to_le_bytes());
    assert_eq!(&bytes[50..50 + cmd.len()], cmd.as_slice(), "cmd verbatim at 50");
    assert_eq!(&bytes[50 + cmd.len()..], &5u64.to_le_bytes());
    // And the decoder reads back exactly what went out.
    assert_eq!(AppendEntriesRequest::from_body(&bytes), Some(req));
}

#[test]
fn a_snapshot_payload_need_not_be_utf8() {
    // InstallSnapshot's `data` is a C++ std::string -- bytes. Decoding it as a
    // Rust String would reject this as InvalidUtf8.
    use srpc::serializable::{make_source_proxy_buffer, BinaryReadArchive, BufferSource,
                             Deserialize};
    let req = InstallSnapshotRequest {
        term: 1,
        leader_id: 2,
        last_included_index: 3,
        last_included_term: 4,
        data: WireBytes(vec![0xff, 0xfe, 0x00, 0xc3]),
    };
    let bytes = encode(&req);
    let mut src = BufferSource::new(bytes.as_ptr(), bytes.len());
    let mut ar = BinaryReadArchive::new(unsafe { make_source_proxy_buffer(&raw mut src) });
    let mut back = InstallSnapshotRequest::default();
    back.deserialize(&mut ar);
    assert!(!ar.failed());
    assert_eq!(back, req);
}
