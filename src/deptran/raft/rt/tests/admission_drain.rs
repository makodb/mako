//! The shutdown drain is a barrier (bugs-found B23): once admission is closed
//! and a drain has returned true, no request reaches the service, and a drain
//! waits for a handler still inside it. RaftWorker::ShutDown deletes the Raft
//! server after that drain, so a request let through afterwards would run a
//! handler on a freed server.
//!
//! The service is the generated rpc::register and rpc::dispatch over a probe
//! handler, on a real srpc server, sent to over TCP.
#![allow(unsafe_code)]

use std::ffi::CString;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use raft_rt::rpc::{
    self, AppendEntriesRequestRef, AppendEntriesResponse, EmptyAppendEntriesRequest,
    EmptyAppendEntriesResponse, InstallSnapshotRequest, InstallSnapshotResponse, RaftHandler, RaftProxy,
    VoteRequest, VoteResponse,
};
use srpc::client::Client;
use srpc::reactor::PollThread;
use srpc::server::{Request, Server, Service, WeakServerConnection, SERVER_ERR_TRY_AGAIN};

/// What the handler saw. `hold` keeps a Vote inside its handler until
/// cleared; `drained` is set by a test once its drain has returned.
#[derive(Default)]
struct Probe {
    entered: AtomicUsize,
    inside: AtomicUsize,
    drained: AtomicBool,
    after_drain: AtomicUsize,
    hold: Mutex<bool>,
    released: Condvar,
}

impl Probe {
    fn release(&self) {
        *self.hold.lock().unwrap() = false;
        self.released.notify_all();
    }
}

struct Handler(Arc<Probe>);

impl RaftHandler for Handler {
    fn vote(&self, req: &VoteRequest) -> Result<VoteResponse, i32> {
        let p = &self.0;
        p.entered.fetch_add(1, Ordering::SeqCst);
        if p.drained.load(Ordering::SeqCst) {
            p.after_drain.fetch_add(1, Ordering::SeqCst);
        }
        p.inside.fetch_add(1, Ordering::SeqCst);
        let mut held = p.hold.lock().unwrap();
        while *held {
            held = p.released.wait(held).unwrap();
        }
        drop(held);
        p.inside.fetch_sub(1, Ordering::SeqCst);
        Ok(VoteResponse { max_ballot: req.cur_term, vote_granted: 1 })
    }
    fn append_entries(&self, _: &AppendEntriesRequestRef<'_>) -> Result<AppendEntriesResponse, i32> {
        Ok(AppendEntriesResponse::default())
    }
    fn empty_append_entries(&self, _: &EmptyAppendEntriesRequest)
        -> Result<EmptyAppendEntriesResponse, i32> {
        Ok(EmptyAppendEntriesResponse::default())
    }
    fn install_snapshot(&self, _: InstallSnapshotRequest) -> Result<InstallSnapshotResponse, i32> {
        Ok(InstallSnapshotResponse::default())
    }
}

impl Service for Handler {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        rpc::register(server, svc_index)
    }
    fn __dispatch__(&self, rpc_id: i32, req: Box<Request>, sconn: WeakServerConnection) {
        rpc::dispatch(self, rpc_id, &req, &sconn);
    }
}

/// A server with admission open, on an ephemeral port, and its address.
fn serve(probe: &Arc<Probe>) -> (Server, CString) {
    let mut server = Server::new(Some(PollThread::create()));
    server.reg_service(Box::new(Handler(probe.clone())));
    // SAFETY: the literal is NUL-terminated and valid for the call.
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let addr = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    (server, addr)
}

fn connect(addr: &CString) -> Arc<Client> {
    let client = Client::create(PollThread::create());
    assert_eq!(client.connect(addr.as_ptr(), false), 0, "could not connect to {addr:?}");
    client
}

/// Sends one Vote; the reply's code arrives on the returned channel.
fn vote(client: &Client, term: i64) -> mpsc::Receiver<i32> {
    let (tx, rx) = mpsc::channel();
    let req = VoteRequest { cur_term: term, ..Default::default() };
    let sent = RaftProxy { client }.vote_async(&req, Some(Box::new(move |code, _, _| {
        let _ = tx.send(code);
    })));
    assert!(sent.is_ok(), "vote_async refused to send");
    rx
}

const REPLY: Duration = Duration::from_secs(10);

/// The review's reproduction: a Vote sent after a successful drain is
/// refused, and never reaches the handler.
#[test]
fn a_request_after_a_drain_is_refused() {
    let probe = Arc::new(Probe::default());
    let (server, addr) = serve(&probe);
    let client = connect(&addr);
    assert_eq!(vote(&client, 1).recv_timeout(REPLY).unwrap(), 0, "admission open: the vote is handled");
    assert_eq!(probe.entered.load(Ordering::SeqCst), 1);

    server.set_admission_ready(false);
    assert!(server.drain(1000));
    assert_eq!(vote(&client, 2).recv_timeout(REPLY).unwrap(), SERVER_ERR_TRY_AGAIN);
    assert_eq!(probe.entered.load(Ordering::SeqCst), 1, "a request reached the handler after the drain");
    client.close();
}

/// A drain waits for a handler that is still inside: it returns only after
/// the handler has left (the request's count is released with its reply).
#[test]
fn a_drain_waits_for_a_handler_inside() {
    let probe = Arc::new(Probe::default());
    *probe.hold.lock().unwrap() = true;
    let (server, addr) = serve(&probe);
    let client = connect(&addr);
    let reply = vote(&client, 1);
    let start = Instant::now();
    while probe.inside.load(Ordering::SeqCst) == 0 {
        assert!(start.elapsed() < REPLY, "the vote never reached its handler");
        std::thread::sleep(Duration::from_millis(1));
    }

    let releaser = {
        let probe = probe.clone();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(300));
            probe.release();
        })
    };
    server.set_admission_ready(false);
    let t0 = Instant::now();
    assert!(server.drain(10_000), "the drain timed out");
    assert!(t0.elapsed() >= Duration::from_millis(250), "the drain returned while a handler was inside");
    assert_eq!(probe.inside.load(Ordering::SeqCst), 0, "the drain returned while a handler was inside");
    releaser.join().unwrap();
    assert_eq!(reply.recv_timeout(REPLY).unwrap(), 0);
    client.close();
}

/// A drain that times out says so: a handler is still inside.
#[test]
fn a_drain_that_times_out_reports_it() {
    let probe = Arc::new(Probe::default());
    *probe.hold.lock().unwrap() = true;
    let (server, addr) = serve(&probe);
    let client = connect(&addr);
    let reply = vote(&client, 1);
    let start = Instant::now();
    while probe.inside.load(Ordering::SeqCst) == 0 {
        assert!(start.elapsed() < REPLY, "the vote never reached its handler");
        std::thread::sleep(Duration::from_millis(1));
    }
    server.set_admission_ready(false);
    assert!(!server.drain(100), "a drain with a handler inside reported success");
    probe.release();
    assert_eq!(reply.recv_timeout(REPLY).unwrap(), 0);
    client.close();
}

/// Under load from several clients, closing admission and draining is a
/// barrier: no request enters the handler once the drain has returned, and
/// every later request is refused.
#[test]
fn no_request_enters_after_a_drain_under_load() {
    let probe = Arc::new(Probe::default());
    let (server, addr) = serve(&probe);
    let stop = Arc::new(AtomicBool::new(false));
    let refused = Arc::new(AtomicUsize::new(0));
    let senders: Vec<_> = (0..4)
        .map(|t| {
            let (addr, stop, refused) = (addr.clone(), stop.clone(), refused.clone());
            std::thread::spawn(move || {
                let client = connect(&addr);
                let mut term = 0i64;
                while !stop.load(Ordering::SeqCst) {
                    term += 1;
                    match vote(&client, t * 1_000_000 + term).recv_timeout(REPLY) {
                        Ok(0) => {}
                        Ok(code) if code == SERVER_ERR_TRY_AGAIN => {
                            refused.fetch_add(1, Ordering::SeqCst);
                        }
                        other => panic!("unexpected reply {other:?}"),
                    }
                }
                client.close();
            })
        })
        .collect();

    let start = Instant::now();
    while probe.entered.load(Ordering::SeqCst) < 200 {
        assert!(start.elapsed() < REPLY, "the clients' votes are not being handled");
        std::thread::sleep(Duration::from_millis(1));
    }
    server.set_admission_ready(false);
    assert!(server.drain(5_000), "the drain timed out");
    probe.drained.store(true, Ordering::SeqCst);
    // Keep sending into the closed gate for a while.
    let refused_at_drain = refused.load(Ordering::SeqCst);
    let start = Instant::now();
    while refused.load(Ordering::SeqCst) < refused_at_drain + 200 {
        assert!(start.elapsed() < REPLY, "requests after the drain are neither refused nor handled");
        std::thread::sleep(Duration::from_millis(1));
    }
    stop.store(true, Ordering::SeqCst);
    for s in senders {
        s.join().unwrap();
    }
    assert_eq!(probe.after_drain.load(Ordering::SeqCst), 0, "requests entered the handler after the drain");
}
