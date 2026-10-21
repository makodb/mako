// The bounds walk every peer-supplied body passes before it is decoded.
// srpc's streaming decoders abort the process on a
// short read and trust a byte string's v64 length into an allocation, so
// `wire_fits` must refuse exactly the bodies those decoders cannot finish:
// every truncation of a real encoding, and a byte-string length that is
// negative or runs past the end. A whole encoding must still fit.

use raft_rt::rpc::{
    AppendEntriesResponse, EmptyAppendEntriesRequest, InstallSnapshotRequest,
    InstallSnapshotResponse, VoteRequest, VoteResponse, WireBytes, WireFits,
};
use srpc::serializable::{make_sink_proxy_buffer, BinaryWriteArchive, BufferSink, Serialize};

fn encode<T: Serialize>(value: &T) -> Vec<u8> {
    let mut sink = BufferSink { bytes: Vec::new() };
    {
        // SAFETY: `sink` outlives the archive that borrows it.
        let mut ar = BinaryWriteArchive {
            sink_: unsafe { make_sink_proxy_buffer(&raw mut sink) },
        };
        value.serialize(&mut ar);
    }
    sink.bytes
}

/// A whole encoding fits; every proper prefix of it does not.
fn only_whole<T: Serialize + WireFits>(value: &T) {
    let bytes = encode(value);
    assert!(T::wire_fits(&bytes), "the whole encoding ({} bytes)", bytes.len());
    for cut in 0..bytes.len() {
        assert!(!T::wire_fits(&bytes[..cut]), "a body cut to {cut} of {} bytes", bytes.len());
    }
}

#[test]
fn every_truncation_is_refused() {
    only_whole(&VoteRequest { lst_log_idx: 1, lst_log_term: 2, site_id: 3, cur_term: 4 });
    only_whole(&VoteResponse { max_ballot: 9, vote_granted: 1 });
    only_whole(&EmptyAppendEntriesRequest { slot: 1, ballot: 2, leader_current_term: 3, leader_site_id: 4,
        leader_prev_log_index: 5, leader_prev_log_term: 6, leader_commit_index: 7 });
    only_whole(&AppendEntriesResponse { follower_append_ok: 1, follower_current_term: 2, follower_last_log_index: 3 });
    only_whole(&InstallSnapshotResponse { term_out: 5 });
    for len in [0usize, 1, 63, 64, 300, 70_000] {
        only_whole(&InstallSnapshotRequest { term: 1, leader_id: 2, last_included_index: 3,
            last_included_term: 4, data: WireBytes(vec![7u8; len]) });
    }
}

#[test]
fn a_byte_string_length_past_the_body_is_refused() {
    let req = InstallSnapshotRequest { term: 1, leader_id: 2, last_included_index: 3,
        last_included_term: 4, data: WireBytes(b"abcd".to_vec()) };
    let mut bytes = encode(&req);
    // The four fixed fields, then the v64 length: one byte, 4.
    assert_eq!(bytes[32], 4);
    for claimed in [5u8, 0x3f] {
        bytes[32] = claimed;
        assert!(!InstallSnapshotRequest::wire_fits(&bytes), "length {claimed} over 4 bytes");
    }
    // A one-byte v64 is signed: 0x40..=0x7f are negative lengths.
    bytes[32] = 0x7f;
    assert!(!InstallSnapshotRequest::wire_fits(&bytes), "length -1");
    // A nine-byte v64 claiming 2^40 bytes in a 45-byte body.
    let mut huge = bytes[..32].to_vec();
    huge.push(0xff);
    huge.extend_from_slice(&(1u64 << 40).to_be_bytes());
    huge.extend_from_slice(b"abcd");
    assert!(!InstallSnapshotRequest::wire_fits(&huge), "length 2^40");
}
