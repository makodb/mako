//! Interop: this crate's wire layer against the LIVE production C++ server.
//!
//! The golden corpus proves the encoders agree on bytes. This proves the
//! whole request/reply contract agrees with the real dispatcher — framing,
//! header field order and widths, xid echo, error codes — by driving the
//! unmodified `rpcbench -s` process through real RPCs.
//!
//! ```text
//! build_clang22/rpcbench -s 127.0.0.1:18848 &
//! SRPC_INTEROP_ADDR=127.0.0.1:18848 cargo test -p srpc --test interop_cpp_server
//! ```
//!
//! Skipped (not failed) when `SRPC_INTEROP_ADDR` is unset, so the suite
//! stays green without a server.
//!
//! Uses `std::net::TcpStream` deliberately: this is a `#[cfg(test)]`
//! integration test, which never translates to C++, so it needs none of
//! the syscall kernels the real transport will (those land with the
//! epoll poll thread, where they are actually required).
//!
//! ## The contract, as implemented here
//!
//! ```text
//! request : [i32 size LE][v64 xid][i32 rpc_id LE][args...]
//! reply   : [i32 size LE][v64 xid][v32 error][v64 server_instance_id][results...]
//! ```
//!
//! `size` excludes itself; its high bit is the extended-header flag, which
//! the C++ transport hardcodes to false in BOTH directions — so it is
//! always clear on send, and `server_instance_id` is always present on
//! receive regardless of it. Note the asymmetry that catches
//! reimplementations: `xid` is a varint but `rpc_id` is a RAW 4-byte i32,
//! and the reply's `error` is a v32 varint, not a fixed i32.

use srpc::wire::{frame, Deserialize, FrameReader, ReadArchive, Serialize, WriteArchive, V32, V64};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

/// rpc_ids from `src/rrr/tests/benchmark_service.h`.
///
/// rpcgen assigns each method a random i32 ONCE and then preserves it by
/// re-parsing the previously generated header, so these are frozen
/// checked-in constants rather than a derivable range. A Rust client
/// learns them the only way anything does: by using the same literals.
/// `ids_match_the_generated_header` guards them against drift.
mod ids {
    pub const FAST_PRIME: i32 = 0x4f4daa5a;
    pub const FAST_DOT_PROD: i32 = 0x36ff5226;
    pub const FAST_ADD: i32 = 0x3a24232d;
    pub const FAST_NOP: i32 = 0x4b921bd9;
    pub const FAST_VEC: i32 = 0x23928fcb;
    pub const NOP: i32 = 0x327203ee;
    pub const DEFERRED_ECHO: i32 = 0x412ef56f;
    /// Not registered by any service — expected to come back ENOENT.
    pub const UNKNOWN: i32 = 0x1111_1111;
}

/// Linux errno the C++ server reports for an unregistered rpc_id.
const ENOENT: i32 = 2;

struct Peer {
    sock: TcpStream,
    next_xid: i64,
    /// The crate's own frame reader, so this exercises the ported
    /// decoder against the real server rather than a test-local one.
    reader: FrameReader,
}

impl Peer {
    fn connect(addr: &str) -> Peer {
        let sock = TcpStream::connect(addr)
            .unwrap_or_else(|e| panic!("connect {addr}: {e} (is `rpcbench -s {addr}` running?)"));
        // Generous: a stall here means a protocol desync, and we would
        // rather fail the test than hang the suite.
        sock.set_read_timeout(Some(Duration::from_secs(10)))
            .unwrap();
        Peer {
            sock,
            next_xid: 0,
            reader: FrameReader::new(),
        }
    }

    /// Send one request and return the reply's `(error, results)`.
    ///
    /// `fill` writes the arguments; the caller decodes the results, since
    /// only it knows their types.
    fn call<F: FnOnce(&mut WriteArchive)>(&mut self, rpc_id: i32, fill: F) -> (i32, Vec<u8>) {
        let xid = self.next_xid;
        self.next_xid += 1;

        let mut body = WriteArchive::new();
        V64(xid).serialize(&mut body);
        // RAW 4-byte i32, NOT a varint — the asymmetry with xid above.
        rpc_id.serialize(&mut body);
        fill(&mut body);

        let mut framed: Vec<u8> = Vec::new();
        assert!(
            frame::encode_into(&mut framed, body.as_bytes(), false),
            "payload rejected by the frame encoder"
        );
        self.sock.write_all(&framed).expect("send request");

        let payload = self.read_frame();
        let mut ar = ReadArchive::new(&payload);
        let got_xid = V64::deserialize(&mut ar).expect("reply xid").0;
        assert_eq!(got_xid, xid, "server must echo the request xid");
        let error = V32::deserialize(&mut ar).expect("reply error code").0;
        // Always present, whatever the extended-header flag says.
        let _instance_id = V64::deserialize(&mut ar).expect("server instance id").0;
        let consumed = payload.len() - ar.remaining();
        (error, payload[consumed..].to_vec())
    }

    /// Read exactly one frame, blocking until it is complete.
    fn read_frame(&mut self) -> Vec<u8> {
        let mut scratch = [0u8; 4096];
        loop {
            match self.reader.next_frame() {
                Ok(Some((header, payload))) => {
                    assert!(
                        !header.extended_header_flag,
                        "the C++ transport hardcodes the extended-header flag false"
                    );
                    return payload;
                }
                Ok(None) => {}
                Err(()) => panic!("malformed frame from the server"),
            }
            let n = self.sock.read(&mut scratch).expect("read reply");
            assert!(n > 0, "server closed the connection mid-frame");
            self.reader.append(&scratch[..n]);
        }
    }
}

fn addr() -> Option<String> {
    std::env::var("SRPC_INTEROP_ADDR").ok()
}

macro_rules! peer_or_skip {
    () => {
        match addr() {
            Some(a) => Peer::connect(&a),
            None => {
                eprintln!("SRPC_INTEROP_ADDR unset — skipping interop test");
                return;
            }
        }
    };
}

/// `fast_add(v32 a, v32 b | v32 a_add_b)` — varints in both directions.
#[test]
fn fast_add_round_trips_varints() {
    let mut p = peer_or_skip!();
    let (err, results) = p.call(ids::FAST_ADD, |ar| {
        V32(7).serialize(ar);
        V32(35).serialize(ar);
    });
    assert_eq!(err, 0, "fast_add should succeed");
    let mut ar = ReadArchive::new(&results);
    assert_eq!(V32::deserialize(&mut ar).unwrap().0, 42);

    // Negative operands exercise the varint sign path.
    let (err, results) = p.call(ids::FAST_ADD, |ar| {
        V32(-1000).serialize(ar);
        V32(1).serialize(ar);
    });
    assert_eq!(err, 0);
    let mut ar = ReadArchive::new(&results);
    assert_eq!(V32::deserialize(&mut ar).unwrap().0, -999);
}

/// `fast_prime(i32 n | i8 flag)` — RAW scalars, proving they are not
/// varint-encoded.
#[test]
fn fast_prime_uses_raw_scalars() {
    let mut p = peer_or_skip!();
    // compute_prime short-circuits n <= 3 to 1, so 1 reports "prime"
    // and 0/negative report -1. Asserting the implementation, not
    // number theory.
    for (n, expected) in [(7i32, 1i8), (8, 0), (2, 1), (1, 1), (0, -1), (-5, -1)] {
        let (err, results) = p.call(ids::FAST_PRIME, |ar| n.serialize(ar));
        assert_eq!(err, 0, "fast_prime({n})");
        assert_eq!(results.len(), 1, "an i8 is exactly one raw byte");
        let mut ar = ReadArchive::new(&results);
        assert_eq!(i8::deserialize(&mut ar).unwrap(), expected, "prime({n})");
    }
}

/// `fast_nop(string)` and `nop(string)` — the reply body is header-only.
/// The pair also proves the fast and slow dispatch paths agree on the wire.
#[test]
fn nop_variants_reply_with_an_empty_body() {
    let mut p = peer_or_skip!();
    for id in [ids::FAST_NOP, ids::NOP] {
        let (err, results) = p.call(id, |ar| "hello".serialize(ar));
        assert_eq!(err, 0, "nop {id:#x}");
        assert!(results.is_empty(), "nop returns nothing, got {results:?}");
    }
    // An empty string is a length-prefixed zero, not an absent field.
    let (err, _) = p.call(ids::FAST_NOP, |ar| "".serialize(ar));
    assert_eq!(err, 0);
}

/// `fast_dot_prod(point3, point3 | double)` — struct fields serialize in
/// declaration order with no length prefix, and doubles are raw.
#[test]
fn fast_dot_prod_handles_structs_and_doubles() {
    let mut p = peer_or_skip!();
    let (err, results) = p.call(ids::FAST_DOT_PROD, |ar| {
        for v in [1.0f64, 2.0, 3.0, 4.0, 5.0, 6.0] {
            v.serialize(ar);
        }
    });
    assert_eq!(err, 0);
    assert_eq!(results.len(), 8, "one raw f64");
    let mut ar = ReadArchive::new(&results);
    // 1*4 + 2*5 + 3*6
    assert_eq!(f64::deserialize(&mut ar).unwrap(), 32.0);
}

/// `fast_vec(i32 n | vector<i64>)` — container framing, and a reply large
/// enough to exercise multi-read frame assembly.
#[test]
fn fast_vec_returns_a_length_prefixed_container() {
    let mut p = peer_or_skip!();
    // n must be > 0: the handler asserts it, and rrr's verify() ABORTS
    // the whole server process on failure — a handler precondition is a
    // remotely-triggerable process kill (see the note in
    // docs/srpc-rust-port.md). Deliberately not exercised here.
    for n in [1i32, 4, 1000] {
        let (err, results) = p.call(ids::FAST_VEC, |ar| n.serialize(ar));
        assert_eq!(err, 0, "fast_vec({n})");
        let mut ar = ReadArchive::new(&results);
        let len = V64::deserialize(&mut ar).unwrap().0;
        assert_eq!(len, n as i64, "element count");
        let mut i = 0;
        while i < n {
            assert_eq!(i64::deserialize(&mut ar).unwrap(), 1, "element {i}");
            i += 1;
        }
    }
}

/// `defer deferred_echo(i32 val | i32 result)` — the deferred-reply path,
/// which answers on a different code path from the fast handlers.
#[test]
fn deferred_echo_replies_on_the_deferred_path() {
    let mut p = peer_or_skip!();
    let (err, results) = p.call(ids::DEFERRED_ECHO, |ar| 21i32.serialize(ar));
    assert_eq!(err, 0);
    let mut ar = ReadArchive::new(&results);
    assert_eq!(
        i32::deserialize(&mut ar).unwrap(),
        42,
        "the handler doubles"
    );
}

/// An unregistered rpc_id must come back ENOENT with the connection
/// intact — the error path, and proof that a reply's error code is a
/// varint rather than a fixed i32.
#[test]
fn unknown_rpc_id_returns_enoent_without_dropping_the_connection() {
    let mut p = peer_or_skip!();
    let (err, results) = p.call(ids::UNKNOWN, |_| {});
    assert_eq!(err, ENOENT, "unknown rpc_id should be ENOENT");
    assert!(results.is_empty());

    // The connection survives: the next call on the SAME socket works.
    let (err, results) = p.call(ids::FAST_ADD, |ar| {
        V32(1).serialize(ar);
        V32(1).serialize(ar);
    });
    assert_eq!(err, 0, "connection must stay usable after an error reply");
    let mut ar = ReadArchive::new(&results);
    assert_eq!(V32::deserialize(&mut ar).unwrap().0, 2);
}

/// Many requests on one connection, checking that replies are matched by
/// xid rather than by arrival order.
#[test]
fn many_calls_on_one_connection_stay_in_sync() {
    let mut p = peer_or_skip!();
    let mut i = 0;
    while i < 200 {
        let (err, results) = p.call(ids::FAST_ADD, |ar| {
            V32(i).serialize(ar);
            V32(i * 2).serialize(ar);
        });
        assert_eq!(err, 0, "call {i}");
        let mut ar = ReadArchive::new(&results);
        assert_eq!(V32::deserialize(&mut ar).unwrap().0, i * 3, "call {i}");
        i += 1;
    }
}

/// The rpc_ids are frozen constants scraped from the generated header. If
/// a regeneration ever re-rolls one, this fails here rather than as a
/// mysterious ENOENT at interop time.
#[test]
fn ids_match_the_generated_header() {
    let header = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../src/rrr/tests/benchmark_service.h"
    );
    let src = match std::fs::read_to_string(header) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("cannot read {header}: {e} — skipping id drift check");
            return;
        }
    };
    for (name, want) in [
        ("FAST_PRIME", ids::FAST_PRIME),
        ("FAST_DOT_PROD", ids::FAST_DOT_PROD),
        ("FAST_ADD", ids::FAST_ADD),
        ("FAST_NOP", ids::FAST_NOP),
        ("FAST_VEC", ids::FAST_VEC),
        ("NOP", ids::NOP),
        ("DEFERRED_ECHO", ids::DEFERRED_ECHO),
    ] {
        let needle = format!("{name} = {want:#010x},");
        assert!(
            src.contains(&needle),
            "{name} drifted: expected `{needle}` in benchmark_service.h"
        );
    }
}
