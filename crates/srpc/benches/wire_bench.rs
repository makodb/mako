//! Wire-layer micro-benchmark — the Rust mirror of
//! `src/rrr/tests/bench_marshal.cc`, scenario-for-scenario, for the
//! Goal-1 performance-parity gate (docs/srpc-rust-port.md).
//!
//! Same methodology: fixed iteration counts, warmup = iters/100 capped
//! at 1024, `ns/op` and `ops/sec` reported in the same table shape.
//! Run with `cargo bench -p srpc` (harness = false; this is a plain
//! main). Pin to a core (`taskset -c N`) when comparing against the
//! C++ numbers, and compare like-for-like build modes (cargo bench is
//! release/opt-level 3; the C++ bench builds -O3).

use srpc::wire::frame::{encode_into, FrameReader};
use srpc::wire::{Deserialize, ReadArchive, Serialize, WriteArchive};
use std::time::Instant;

struct Scenario {
    name: &'static str,
    iters: usize,
    body: fn(usize),
}

fn run(s: &Scenario) {
    // Warmup — touch the code path once so caches/branch predictors settle.
    (s.body)(std::cmp::min(s.iters / 100, 1024));
    let t0 = Instant::now();
    (s.body)(s.iters);
    let total_ns = t0.elapsed().as_nanos() as u64;
    let ns_per_op = total_ns as f64 / s.iters as f64;
    let ops_per_sec = 1e9 / ns_per_op;
    println!(
        "{:<55} | {:>9} | {:>12} | {:>8.1} | {:>12.0}",
        s.name, s.iters, total_ns, ns_per_op, ops_per_sec
    );
}

fn blob(n: usize, fill: u8) -> Vec<u8> {
    vec![fill; n]
}

fn str100() -> String {
    "y".repeat(100)
}

/// Frame-reader scenarios for the first-swap perf question.
///
/// The C++ FrameStreamReader splits a ZERO-COPY peek (`next_frame` ->
/// a `FrameView` aliasing the buffer) from a separate `consume_frame()`.
/// The Rust port fuses them: `next_frame()` copies each payload out
/// into a fresh `Vec`. That is one allocation + copy per INBOUND RPC on
/// the hot path, and the conversion ledger flags it as a bench gate
/// before the `rrr.frame_codec` swap can land.
///
/// `with_next_frame()` already provides the zero-copy shape, so these
/// price the difference directly instead of arguing about it.
/// Const-generic over the payload size so each stays a plain `fn(usize)`
/// (the Scenario table takes a fn pointer, not a capturing closure).
fn frame_owned_vec<const N: usize>(n: usize) {
    let mut wire = Vec::new();
    assert!(encode_into(&mut wire, &blob(N, 0xab), false));
    let mut r = FrameReader::new();
    let mut acc = 0usize;
    let mut i = 0usize;
    while i < n {
        r.append(&wire);
        let (_h, p) = r.next_frame().unwrap().unwrap();
        acc += p.len();
        i += 1;
    }
    std::hint::black_box(acc);
}

fn frame_zero_copy<const N: usize>(n: usize) {
    let mut wire = Vec::new();
    assert!(encode_into(&mut wire, &blob(N, 0xab), false));
    let mut r = FrameReader::new();
    let mut acc = 0usize;
    let mut i = 0usize;
    while i < n {
        r.append(&wire);
        r.with_next_frame(|_h, p| acc += p.len()).unwrap();
        i += 1;
    }
    std::hint::black_box(acc);
}

fn main() {
    println!(
        "{:<55} | {:>9} | {:>12} | {:>8} | {:>12}",
        "scenario", "iters", "total_ns", "ns/op", "ops/sec"
    );

    run(&Scenario {
        name: "write+read i64 (fresh archives)",
        iters: 5_000_000,
        body: |n| {
            let mut i = 0usize;
            while i < n {
                let mut war = WriteArchive::new();
                let v = i as i64;
                v.serialize(&mut war);
                let bytes = war.as_bytes();
                let mut rar = ReadArchive::new(bytes);
                let out = i64::deserialize(&mut rar).unwrap();
                assert!(out == v);
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "write+read i64 (single archive, drains immediately)",
        iters: 5_000_000,
        body: |n| {
            let mut war = WriteArchive::new();
            let mut i = 0usize;
            while i < n {
                war.clear();
                let v = i as i64;
                v.serialize(&mut war);
                let mut rar = ReadArchive::new(war.as_bytes());
                let out = i64::deserialize(&mut rar).unwrap();
                assert!(out == v);
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "write 1024 i64 then read 1024 i64",
        iters: 50_000,
        body: |n| {
            const COUNT: usize = 1024;
            let mut k = 0usize;
            while k < n {
                let mut war = WriteArchive::new();
                let mut i = 0usize;
                while i < COUNT {
                    (i as i64).serialize(&mut war);
                    i += 1;
                }
                let mut rar = ReadArchive::new(war.as_bytes());
                let mut i = 0usize;
                while i < COUNT {
                    let out = i64::deserialize(&mut rar).unwrap();
                    assert!(out == i as i64);
                    i += 1;
                }
                k += 1;
            }
        },
    });

    run(&Scenario {
        name: "raw write(8) + read(8) (single archive)",
        iters: 5_000_000,
        body: |n| {
            let mut war = WriteArchive::new();
            let v: u64 = 0xDEAD_BEEF_CAFE_BABE;
            let mut i = 0usize;
            while i < n {
                war.clear();
                war.write_bytes(&v.to_le_bytes());
                let mut rar = ReadArchive::new(war.as_bytes());
                let mut out = [0u8; 8];
                rar.read_exact(&mut out).unwrap();
                assert!(u64::from_le_bytes(out) == v);
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "write 1KB blob + read 1KB blob",
        iters: 200_000,
        body: |n| {
            let kblob = blob(1024, 0xCD);
            let mut dst = vec![0u8; 1024];
            let mut i = 0usize;
            while i < n {
                let mut war = WriteArchive::new();
                war.write_bytes(&kblob);
                let mut rar = ReadArchive::new(war.as_bytes());
                rar.read_exact(&mut dst).unwrap();
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "write+read String(100)",
        iters: 1_000_000,
        body: |n| {
            let input = str100();
            let mut i = 0usize;
            while i < n {
                let mut war = WriteArchive::new();
                input.serialize(&mut war);
                let mut rar = ReadArchive::new(war.as_bytes());
                let out = String::deserialize(&mut rar).unwrap();
                assert!(out == input);
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "4*i32 + String(100) round-trip",
        iters: 500_000,
        body: |n| {
            let input = str100();
            let mut i = 0usize;
            while i < n {
                let mut war = WriteArchive::new();
                1i32.serialize(&mut war);
                2i32.serialize(&mut war);
                3i32.serialize(&mut war);
                4i32.serialize(&mut war);
                input.serialize(&mut war);
                let mut rar = ReadArchive::new(war.as_bytes());
                let _a = i32::deserialize(&mut rar).unwrap();
                let _b = i32::deserialize(&mut rar).unwrap();
                let _c = i32::deserialize(&mut rar).unwrap();
                let _d = i32::deserialize(&mut rar).unwrap();
                let so = String::deserialize(&mut rar).unwrap();
                assert!(so.len() == input.len());
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "write 4KB blob (single write) + read 4KB",
        iters: 100_000,
        body: |n| {
            let b = blob(4096, 0xAB);
            let mut dst = vec![0u8; 4096];
            let mut i = 0usize;
            while i < n {
                let mut war = WriteArchive::new();
                war.write_bytes(&b);
                let mut rar = ReadArchive::new(war.as_bytes());
                rar.read_exact(&mut dst).unwrap();
                i += 1;
            }
        },
    });

    run(&Scenario {
        name: "write 10x 1KB then drain 10x 1KB",
        iters: 50_000,
        body: |n| {
            let b = blob(1024, 0xEE);
            let mut dst = vec![0u8; 1024];
            let mut i = 0usize;
            while i < n {
                let mut war = WriteArchive::new();
                let mut k = 0usize;
                while k < 10 {
                    war.write_bytes(&b);
                    k += 1;
                }
                let mut rar = ReadArchive::new(war.as_bytes());
                let mut k = 0usize;
                while k < 10 {
                    rar.read_exact(&mut dst).unwrap();
                    k += 1;
                }
                i += 1;
            }
        },
    });
    // Frame reader: owned-Vec pop vs zero-copy peek, same payload.
    run(&Scenario {
        name: "frame next_frame OWNED Vec (16B payload)",
        iters: 2_000_000,
        body: frame_owned_vec::<16>,
    });
    run(&Scenario {
        name: "frame with_next_frame ZERO-COPY (16B payload)",
        iters: 2_000_000,
        body: frame_zero_copy::<16>,
    });
    run(&Scenario {
        name: "frame next_frame OWNED Vec (1KiB payload)",
        iters: 1_000_000,
        body: frame_owned_vec::<1024>,
    });
    run(&Scenario {
        name: "frame with_next_frame ZERO-COPY (1KiB payload)",
        iters: 1_000_000,
        body: frame_zero_copy::<1024>,
    });
    run(&Scenario {
        name: "frame next_frame OWNED Vec (16KiB payload)",
        iters: 200_000,
        body: frame_owned_vec::<16384>,
    });
    run(&Scenario {
        name: "frame with_next_frame ZERO-COPY (16KiB payload)",
        iters: 200_000,
        body: frame_zero_copy::<16384>,
    });
}
