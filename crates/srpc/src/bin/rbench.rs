//! Rust rpcbench client — the S5 cross-stack perf gate.
//!
//! Mirrors `src/rrr/tests/rpcbench.cc` in client mode. It is a CLIENT
//! only: there is no Rust server yet (S6), so this measures the Rust
//! client stack against the C++ client stack, both driving the same
//! unmodified C++ server. That isolates exactly one variable.
//!
//! ## The harness semantics are copied, not improved
//!
//! Every one of these is a decision the C++ made that changes the
//! number being reported. Mirroring them is the whole point — a
//! "better" harness measures a different quantity and the comparison
//! becomes meaningless:
//!
//!   - **Counting is OK RESPONSES**, not sends. A failed reply does not
//!     count, so an error storm reads as low throughput, not high.
//!   - **The first sample is discarded.** `-n 8` yields 7 samples;
//!     the first tick covers connection setup.
//!   - **`-o depth` is outstanding requests per THREAD**, so total
//!     concurrency is `threads * depth`.
//!   - **Nagle stays ON.** Nothing in `src/rrr` sets `TCP_NODELAY`, so
//!     nothing here does either. Enabling it "because obviously" would
//!     invalidate the comparison.
//!   - **`fast` mode is `fast_nop(request_str)`** where `request_str`
//!     is `-b` bytes. The payload rides in the ARGUMENT; the reply is
//!     small.
//!
//! ## What this cannot decide
//!
//! The pinned baseline records depth-1 run-to-run noise at 5–18%, at or
//! beyond the 10% parity criterion. Depth 1 is therefore directional
//! only; the gate is the depth-100 cells. Recorded here so the gate is
//! not quietly redefined later to fit a result.
//!
//! ```text
//! rpcbench -s 127.0.0.1:19400 &
//! rbench -c 127.0.0.1:19400 -n 8 -t 4 -o 100 -b 10
//! ```

use srpc::rpc::client::ClientConnection;
use srpc::rpc::server::{Registry, Server};
use srpc::rpc::task::{spawn, RpcFuture};
use srpc::runtime::poll_thread::PollThread;
use srpc::wire::{Serialize, WriteArchive};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

/// `BenchmarkService::FAST_NOP`, from `benchmark_service.h`. rpcgen
/// assigns these once and then preserves them; never re-roll one.
const FAST_NOP: i32 = 0x4b921bd9;

struct Config {
    addr: String,
    serve: bool,
    seconds: u32,
    threads: usize,
    depth: usize,
    bytes: usize,
    /// `block` = one park per request; `await` = the executor.
    mode: String,
}

fn usage() -> ! {
    eprintln!(
        "usage: rbench -c ADDR [-n SECONDS] [-t THREADS] [-o DEPTH] [-b BYTES]\n\
         \x20      rbench -s ADDR   (serve fast_nop, for the C++ client to drive)\n\
         \n\
         Mirrors rpcbench's client mode (fast/fast_nop only).\n\
         Counting, sampling and Nagle match the C++ harness — see the\n\
         module docs before changing any of them."
    );
    std::process::exit(2);
}

fn parse_args() -> Config {
    let mut cfg = Config {
        addr: String::new(),
        serve: false,
        seconds: 8,
        threads: 4,
        depth: 1,
        bytes: 10,
        mode: "block".to_string(),
    };
    let argv: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < argv.len() {
        let need = |i: usize| -> String {
            if i + 1 >= argv.len() {
                usage();
            }
            argv[i + 1].clone()
        };
        match argv[i].as_str() {
            "-c" => cfg.addr = need(i),
            "-s" => {
                cfg.addr = need(i);
                cfg.serve = true;
            }
            "-n" => cfg.seconds = need(i).parse().unwrap_or_else(|_| usage()),
            "-t" => cfg.threads = need(i).parse().unwrap_or_else(|_| usage()),
            "-o" => cfg.depth = need(i).parse().unwrap_or_else(|_| usage()),
            "-b" => cfg.bytes = need(i).parse().unwrap_or_else(|_| usage()),
            "-m" => cfg.mode = need(i),
            _ => usage(),
        }
        i += 2;
    }
    if cfg.addr.is_empty() || cfg.threads == 0 || cfg.depth == 0 {
        usage();
    }
    cfg
}

/// `fast_nop`'s single argument: a string of `bytes` characters,
/// serialized exactly as the C++ proxy does.
fn nop_args(bytes: usize) -> Vec<u8> {
    let mut ar = WriteArchive::new();
    let s = "x".repeat(bytes);
    s.serialize(&mut ar);
    ar.into_bytes()
}

/// Serve `fast_nop` and block. The point is to let the UNMODIFIED C++
/// `rpcbench -c` drive this crate's server — the reverse of the client
/// interop, and the only way to check the reply envelope against a peer
/// that did not come from the same source tree.
///
/// `fast fast_nop(string)` returns nothing, so the reply carries empty
/// results; the envelope around them is what is being tested.
fn serve(cfg: &Config) -> ! {
    let mut reg = Registry::new();
    reg.register(FAST_NOP, Box::new(|_args| Ok(Vec::new())));
    let server = Server::new(reg, 1);
    let poll = PollThread::start();
    match server.listen(&cfg.addr, &poll) {
        Ok(port) => eprintln!("rbench: serving fast_nop on {}:{}", cfg.addr, port),
        Err(e) => {
            eprintln!("listen {}: {e:?}", cfg.addr);
            std::process::exit(1);
        }
    }
    loop {
        std::thread::sleep(Duration::from_secs(3600));
    }
}

fn main() {
    let cfg = parse_args();
    if cfg.serve {
        serve(&cfg);
    }
    let stop = Arc::new(AtomicBool::new(false));
    let ok_count = Arc::new(AtomicU64::new(0));

    // One poll thread, matching the C++ server shape being measured.
    let poll = PollThread::start();

    let mut workers = Vec::new();
    let mut handles = Vec::new();
    let mut conns = Vec::new();
    for t in 0..cfg.threads {
        let conn = match ClientConnection::connect(&cfg.addr, &poll) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("connect {}: {e:?}", cfg.addr);
                std::process::exit(1);
            }
        };
        if cfg.mode == "await" {
            // The executor path: `depth` tasks per connection, each
            // looping call -> await -> count. The continuation runs on
            // the POLL THREAD inside the reply callback, so a request
            // costs no park/unpark — which is the entire reason S7
            // exists. Nothing is spawned on a user thread here.
            let args = nop_args(cfg.bytes);
            for _ in 0..cfg.depth {
                let conn = Arc::clone(&conn);
                let stop = Arc::clone(&stop);
                let ok_count = Arc::clone(&ok_count);
                let args = args.clone();
                handles.push(spawn(async move {
                    while !stop.load(Ordering::Relaxed) {
                        let Ok(fu) = conn.call(FAST_NOP, &args) else {
                            break;
                        };
                        if RpcFuture::new(fu).await.is_ok() {
                            ok_count.fetch_add(1, Ordering::Relaxed);
                        }
                    }
                }));
            }
            conns.push(conn);
            continue;
        }

        let stop = Arc::clone(&stop);
        let ok_count = Arc::clone(&ok_count);
        let args = nop_args(cfg.bytes);
        let depth = cfg.depth;
        workers.push(std::thread::spawn(move || {
            // A SLIDING WINDOW of `depth` outstanding requests: retire
            // the oldest, immediately issue one more. The C++ runs
            // `depth` independent coroutines per thread, which holds
            // occupancy at `depth` continuously.
            //
            // The obvious alternative — issue a batch of `depth`, drain
            // the whole batch, repeat — is NOT the same benchmark. Its
            // occupancy sawtooths from `depth` to zero, so the pipeline
            // stands empty for the tail of every batch, and it measures
            // materially lower. Getting this wrong is the
            // harness-semantics divergence that decides the result
            // silently, which is why it is spelled out here.
            let mut inflight: std::collections::VecDeque<_> =
                std::collections::VecDeque::with_capacity(depth);
            while !stop.load(Ordering::Relaxed) {
                while inflight.len() < depth {
                    match conn.call(FAST_NOP, &args) {
                        Ok(fu) => inflight.push_back(fu),
                        Err(_) => break,
                    }
                }
                let Some(fu) = inflight.pop_front() else {
                    break;
                };
                // Counting OK RESPONSES, as the C++ does. An error
                // reply is not progress.
                if fu.wait_timeout(Duration::from_secs(10)).is_ok() {
                    ok_count.fetch_add(1, Ordering::Relaxed);
                }
            }
            let _ = t;
            conn.close();
        }));
    }

    // The sampler: one tick per second, FIRST SAMPLE DISCARDED.
    let mut samples: Vec<u64> = Vec::new();
    let mut last = 0u64;
    for i in 0..cfg.seconds {
        std::thread::sleep(Duration::from_secs(1));
        let now = ok_count.load(Ordering::Relaxed);
        if i > 0 {
            let qps = now - last;
            println!("qps: {qps}");
            samples.push(qps);
        }
        last = now;
    }
    stop.store(true, Ordering::Relaxed);

    if samples.is_empty() {
        println!("avg qps: 0.00");
    } else {
        let sum: u64 = samples.iter().sum();
        println!("avg qps: {:.2}", sum as f64 / samples.len() as f64);
    }

    for w in workers {
        let _ = w.join();
    }
    for h in &handles {
        h.join();
    }
    for c in &conns {
        c.close();
    }
    poll.shutdown();
}
