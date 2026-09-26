//! How fast one large AppendEntries crosses real loopback TCP on the Rust
//! lane. Ignored by default (a measurement, not a check):
//!   cargo test --release -p raft-rt --test large_frame_bench -- --ignored --nocapture
#![allow(unsafe_code)]

use std::ffi::CString;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use raft_rt::rpc::{
    self, AppendEntriesRequest, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotResponse, RaftHandler,
    VoteRequest, VoteResponse,
};
use raft_rt::transport::RaftTransport;
use srpc::reactor::PollThread;
use srpc::server::{Request, Server, Service, WeakServerConnection};

struct Sink(Arc<Mutex<usize>>);
impl RaftHandler for Sink {
    fn vote(&self, _: &VoteRequest) -> Result<VoteResponse, i32> { Ok(VoteResponse::default()) }
    fn append_entries(&self, req: &AppendEntriesRequest) -> Result<AppendEntriesResponse, i32> {
        *self.0.lock().unwrap() += req.cmd.len();
        Ok(AppendEntriesResponse { follower_append_ok: 1, follower_current_term: 1,
                                   follower_last_log_index: 0 })
    }
    fn empty_append_entries(&self, _: &EmptyAppendEntriesRequest)
        -> Result<EmptyAppendEntriesResponse, i32> { Ok(EmptyAppendEntriesResponse::default()) }
    fn install_snapshot(&self, _: &InstallSnapshotRequest)
        -> Result<InstallSnapshotResponse, i32> { Ok(InstallSnapshotResponse::default()) }
}
impl Service for Sink {
    fn __reg_to__(&mut self, server: &mut Server, i: usize) -> i32 { rpc::register(server, i) }
    fn __dispatch__(&self, id: i32, req: Box<Request>, s: WeakServerConnection) {
        rpc::dispatch(self, id, &req, &s);
    }
}

#[test]
#[ignore]
fn large_append_entries_throughput() {
    let got = Arc::new(Mutex::new(0usize));
    let mut server = Server::new(Some(PollThread::create()));
    server.reg_service(Box::new(Sink(got.clone())));
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let addr = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    let mut t = RaftTransport::new();
    assert!(unsafe { t.add_peer_with_timeout(1, 2, addr.as_ptr(), Duration::from_secs(5)) });
    for size in [64 * 1024usize, 1 << 20, 4 << 20, 16 << 20] {
        let n = (64usize << 20) / size;
        let start = Instant::now();
        for _ in 0..n {
            let req = AppendEntriesRequest { cmd: vec![7u8; size], ..Default::default() };
            let r = t.send_append_entries(2, req).expect("sent");
            while !r.is_ready() { std::thread::sleep(Duration::from_micros(50)); }
            assert!(r.peek().unwrap().is_ok());
        }
        let secs = start.elapsed().as_secs_f64();
        println!("sequential {:>9} B x {:>4}: {:8.1} MB/s  {:8.2} ms/frame",
                 size, n, (n * size) as f64 / secs / 1e6, secs * 1e3 / n as f64);
        // pipelined: all in flight at once
        let start = Instant::now();
        let replies: Vec<_> = (0..n).map(|_| t.send_append_entries(2,
            AppendEntriesRequest { cmd: vec![7u8; size], ..Default::default() }).expect("sent")).collect();
        for r in &replies { while !r.is_ready() { std::thread::sleep(Duration::from_micros(50)); } }
        let secs = start.elapsed().as_secs_f64();
        println!("pipelined  {:>9} B x {:>4}: {:8.1} MB/s", size, n, (n * size) as f64 / secs / 1e6);
    }
}
