// Rust-lane microbenchmark for srpc's hot leaf codecs.
//
// This exists because docs/verification.md quoted a "+12%" regression on
// frame_codec_write_header from a harness that was never committed. Nobody --
// including its author -- could re-take or check that measurement. This is that
// harness, written to stay: the numbers it prints can be re-derived by anyone,
// on any commit, which is the whole point.
//
// What it measures, and why these three:
//   * frame_codec_write_header  -- the function the +12% claim is about.
//   * sparseint_dump64/load64   -- the sparse-int codec, whose length-8 defect
//     the Verus work fixed; the fix changed the shape of both bodies, so both
//     directions are timed, per length class.
//
// Reading the output: compare the MIN across runs, and treat the spread as the
// noise floor. On a shared machine an effect smaller than the spread is not an
// effect. To A/B a change, run it on both commits in the same sitting -- the
// absolute ns/op is machine- and thermal-dependent and means little on its own.

use std::hint::black_box;
use std::time::Instant;

use srpc::basetypes::{sparseint_dump64, sparseint_load64, sparseint_val_size};
use srpc::frame_codec::frame_codec_write_header;

// There is no build.rs, so the plain-C kernels under base/ and reactor/ are
// never compiled or linked. Anything pulling in basetypes therefore has to
// supply the C seam itself or fail to LINK. These are stubs: nothing here calls
// them, they exist to satisfy the symbol references.
#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_clock_monotonic_us() -> u64 {
    0
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_clock_realtime_coarse_us() -> u64 {
    0
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_gettimeofday_us() -> u64 {
    0
}

#[allow(unsafe_code)]
#[unsafe(no_mangle)]
pub extern "C" fn srpc_sleep_us(_microseconds: u64) {}

// Matches what docs/verification.md's original (lost) harness reported, so the
// historical +12% figure and anything measured now are at least the same shape.
const ITERS: u64 = 20_000_000;
const RUNS: usize = 4;

// Time `body` over ITERS iterations, RUNS times, after a warmup pass. The
// iteration index is handed to the body so inputs can vary: a loop that feeds
// one constant lets the optimiser hoist the call out entirely and times an
// empty loop.
fn measure(label: &str, iters: u64, mut body: impl FnMut(u64)) {
    for i in 0..(iters / 20).max(1) {
        body(i);
    }

    let mut ns_per_op = Vec::with_capacity(RUNS);
    for _ in 0..RUNS {
        let started = Instant::now();
        for i in 0..iters {
            body(i);
        }
        ns_per_op.push(started.elapsed().as_nanos() as f64 / iters as f64);
    }

    let min = ns_per_op.iter().copied().fold(f64::INFINITY, f64::min);
    let max = ns_per_op.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let runs: Vec<String> = ns_per_op.iter().map(|n| format!("{n:.3}")).collect();
    println!(
        "  {label:<34} min {min:>6.3} ns/op   spread {:>5.3}   runs [{}]",
        max - min,
        runs.join(", ")
    );
}

// One representative value per length class the codec actually produces,
// discovered rather than hard-coded. Scanning powers of two also demonstrates
// the property the Verus proof pins: sparseint_val_size never returns 8, so no
// 8-byte class appears in this list.
fn representative_per_length_class() -> Vec<(usize, i64)> {
    let mut found: Vec<(usize, i64)> = Vec::new();
    for shift in 0..63 {
        let value = 1i64 << shift;
        let class = sparseint_val_size(value);
        if !found.iter().any(|(seen, _)| *seen == class) {
            found.push((class, value));
        }
    }
    found.sort_by_key(|(class, _)| *class);
    found
}

fn main() {
    let classes = representative_per_length_class();
    let class_list: Vec<String> = classes.iter().map(|(n, _)| n.to_string()).collect();

    println!("srpc Rust-lane microbenchmark");
    println!("  {ITERS} iterations x {RUNS} runs, release + lto + codegen-units=1");
    println!("  sparse-int length classes present: [{}]", class_list.join(", "));
    println!();

    println!("frame_codec");
    let mut header = [0u8; 4];
    measure("write_header", ITERS, |i| {
        // Vary the payload size across the whole legal range rather than
        // reusing one, so this cannot degenerate into a constant-folded store.
        let payload = (i & 0x00FF_FFFF) as i32;
        let ok = frame_codec_write_header(black_box(&mut header), black_box(payload), false);
        black_box(ok);
    });
    println!();

    println!("basetypes sparse-int (one representative value per length class)");
    let mut encoded = [0u8; 9];
    for (class, value) in &classes {
        let (class, value) = (*class, *value);
        measure(&format!("dump64  class {class} ({value})"), ITERS, |_| {
            let written = sparseint_dump64(black_box(value), black_box(&mut encoded));
            black_box(written);
        });
    }
    println!();

    for (class, value) in &classes {
        let (class, value) = (*class, *value);
        let mut source = [0u8; 9];
        let written = sparseint_dump64(value, &mut source);
        assert_eq!(written, class, "dump64 wrote {written} bytes for class {class}");
        // Round-trip guard: a benchmark timing a function that returns the
        // wrong answer is worse than no benchmark. This is the T4 property,
        // checked once per class before it is timed.
        assert_eq!(sparseint_load64(&source), value, "round trip failed at class {class}");

        measure(&format!("load64  class {class} ({value})"), ITERS, |_| {
            let decoded = sparseint_load64(black_box(&source));
            black_box(decoded);
        });
    }
}
