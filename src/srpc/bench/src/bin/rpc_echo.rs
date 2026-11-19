// Rust-lane RPC echo benchmark: one fast-handler RPC over TCP between two
// PollThreads in one process, through the public Server/Client API.
//
// rpcbench (tests/rpcbench.cc) is the C++-lane benchmark: it needs the C++
// build, and it covers the dispatch modes. This one needs only Cargo and
// measures one shape: a fast handler that runs inline in the server poll
// thread's TCP reader task and replies, and a client that issues requests from
// its own thread and waits on the reply future's condition variable. Both
// PollThreads run Lion runtimes over SrpcEpollBackend, so both poll threads'
// wake paths are on the critical path: the server's reader and writer tasks
// and the client's reader task. A request leaves through the TCP cork: the
// client's thread writes it through when the connection has been idle for
// kTcpWriteThroughIdleUs (rpc/tcp_channel.rs), and otherwise queues it for the
// client poll thread's writer task.
//
// Two measurements per run, each on a fresh server and client:
//   * latency    -- one request outstanding at a time; per-request round trip
//                   from `request` to the reply, reported as p50/p90/p99.
//   * throughput -- WINDOW requests kept outstanding from one thread for
//                   SECONDS; completed replies per second.
//
// The throughput line also reports the process's CPU time (user plus
// system, all threads) per completed request over that phase.
//
// One run prints one `RPC_ECHO` line. Compare builds with
// scripts/run_rpc_echo_bench.sh --compare <ref-a> <ref-b>, which alternates
// runs of the two builds in one sitting; read the spread across runs, not the
// best number (see CLAUDE.md on rpcbench).
//
// Environment: RPC_ECHO_SECONDS (default 3), RPC_ECHO_WINDOW (default 64),
// RPC_ECHO_LATENCY_N (default 5000).

use std::collections::VecDeque;
use std::ffi::CString;
use std::time::{Duration, Instant};

use srpc::client::{deserialize_from, Client, FutureAttr};
use srpc::reactor::PollThread;
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, Service, WeakServerConnection};

const ECHO_RPC: i32 = 0x00E0_7E01;

struct Echo;

impl Service for Echo {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(ECHO_RPC, index)
    }

    #[allow(unsafe_code)]
    fn __dispatch__(&self, _: i32, mut request: Box<Request>, connection: WeakServerConnection) {
        let mut value = 0i64;
        // SAFETY: the request owns this buffer until deserialization completes.
        let mut archive = BinaryReadArchive::new(unsafe {
            srpc::serializable::make_source_proxy_buffer(&raw mut request.src)
        });
        Deserialize::deserialize(&mut value, &mut archive);
        connection.upgrade().unwrap().reply(&request, 0, Some(Box::new(move |out: &mut BinaryWriteArchive| {
            Serialize::serialize(&value, out);
        })));
    }
}

fn env_u64(name: &str, default: u64) -> u64 {
    std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
}

struct Pair {
    client: std::sync::Arc<Client>,
    server: Server,
    client_poll: std::sync::Arc<PollThread>,
    server_poll: std::sync::Arc<PollThread>,
}

#[allow(unsafe_code)]
fn connect() -> Pair {
    let server_poll = PollThread::create();
    let client_poll = PollThread::create();
    let mut server = Server::new(Some(server_poll.clone()));
    server.reg_service(Box::new(Echo));
    // SAFETY: the literal is NUL terminated and outlives the call.
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let address = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    let client = Client::create(client_poll.clone());
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    Pair { client, server, client_poll, server_poll }
}

fn disconnect(pair: Pair) {
    pair.client.close();
    drop(pair.client);
    drop(pair.server);
    pair.client_poll.shutdown();
    pair.server_poll.shutdown();
}

fn round_trip(client: &Client, value: i64) {
    let future = client
        .request(ECHO_RPC, &FutureAttr::default(), |out: &mut BinaryWriteArchive| {
            Serialize::serialize(&value, out);
        })
        .expect("request");
    future.wait();
    assert_eq!(future.get_error_code(), 0);
    let mut echoed = 0i64;
    deserialize_from(future.get_reply(), &mut echoed);
    assert_eq!(echoed, value);
}

// User plus system CPU time of the whole process so far, in microseconds
// (from /proc/self/stat, in clock ticks of 10 ms).
fn process_cpu_us() -> f64 {
    let stat = std::fs::read_to_string("/proc/self/stat").unwrap();
    // The fields after the parenthesized command name; utime and stime are
    // fields 14 and 15 of the whole line.
    let rest = &stat[stat.rfind(')').unwrap() + 2..];
    let fields: Vec<&str> = rest.split_whitespace().collect();
    let ticks: u64 = fields[11].parse::<u64>().unwrap() + fields[12].parse::<u64>().unwrap();
    ticks as f64 * 10_000.0
}

fn percentile(sorted: &[Duration], p: f64) -> f64 {
    sorted[((sorted.len() - 1) as f64 * p).round() as usize].as_secs_f64() * 1e6
}

fn main() {
    let seconds = env_u64("RPC_ECHO_SECONDS", 3);
    let window = env_u64("RPC_ECHO_WINDOW", 64) as usize;
    let latency_n = env_u64("RPC_ECHO_LATENCY_N", 5000) as usize;

    // Latency: one request in flight.
    let pair = connect();
    for i in 0..200 {
        round_trip(&pair.client, i);
    }
    let mut samples: Vec<Duration> = Vec::with_capacity(latency_n);
    for i in 0..latency_n {
        let start = Instant::now();
        round_trip(&pair.client, i as i64);
        samples.push(start.elapsed());
    }
    samples.sort();
    disconnect(pair);

    // Throughput: `window` requests in flight from one thread.
    let pair = connect();
    let mut in_flight = VecDeque::with_capacity(window);
    let mut completed: u64 = 0;
    let mut next: i64 = 0;
    let issue = |client: &Client, value: i64| {
        client
            .request(ECHO_RPC, &FutureAttr::default(), |out: &mut BinaryWriteArchive| {
                Serialize::serialize(&value, out);
            })
            .expect("request")
    };
    for _ in 0..window {
        in_flight.push_back(issue(&pair.client, next));
        next += 1;
    }
    let start = Instant::now();
    let cpu_start = process_cpu_us();
    let limit = Duration::from_secs(seconds);
    while start.elapsed() < limit {
        let oldest = in_flight.pop_front().unwrap();
        oldest.wait();
        assert_eq!(oldest.get_error_code(), 0);
        completed += 1;
        in_flight.push_back(issue(&pair.client, next));
        next += 1;
    }
    let elapsed = start.elapsed().as_secs_f64();
    let cpu_us = process_cpu_us() - cpu_start;
    while let Some(future) = in_flight.pop_front() {
        future.wait();
    }
    disconnect(pair);

    println!(
        "RPC_ECHO qps={:.0} window={} cpu_us_per_req={:.2} p50_us={:.1} p90_us={:.1} p99_us={:.1} latency_n={}",
        completed as f64 / elapsed,
        window,
        cpu_us / completed as f64,
        percentile(&samples, 0.5),
        percentile(&samples, 0.9),
        percentile(&samples, 0.99),
        latency_n
    );
}
