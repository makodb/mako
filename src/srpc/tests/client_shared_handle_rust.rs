// One `Client` handle shared by the thread that owns it and the poll thread
// that delivers its replies.
//
// rpcbench's pipeline is the shape under test: the client thread issues
// `request_async` in a loop while each reply callback -- running on the
// client's poll thread -- issues the next request through the same
// `Arc<Client>`. `Client::connection()` used to read a `RefCell`, whose borrow
// counter is a plain integer in both languages. Two threads borrowing it at
// once could lose an update and leave the counter at -1, and the next borrow
// then panicked "already mutably borrowed" with no `borrow_mut` in sight.
// The generated C++ `rusty::Arc` and `rusty::Function` erase Rust's auto
// traits, so C++ callers reached that state with no compile error. Rust
// rejected the same capture only because `Client` was not `Sync`.
//
// The two-thread test runs over TCP on purpose: the in-memory channel
// delivers replies inline on the calling thread, so it can never put a second
// thread inside the client. That inline delivery is what the re-entry test
// uses instead: a callback that calls back into the client it came from.

#![allow(unsafe_code)]

use std::ffi::CString;
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{mpsc, Arc};
use std::time::{Duration, Instant};

use srpc::client::{AsyncReplyCallback, Client, ClientPool};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::reactor::PollThread;
use srpc::serializable::{BinaryWriteArchive, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};

const NOP_RPC: i32 = 0x00E0_00A1;

// Chains issued from the poll thread, and the hops each one makes.
const CHAINS: i64 = 32;
const HOPS_PER_CHAIN: i64 = 1_500;
// Requests the owning thread issues while the chains run, and the most it
// keeps in flight so the 16,384-entry async slot table never wraps.
const CALLER_REQUESTS: i64 = 20_000;
const CALLER_WINDOW: i64 = 256;

struct NopService;

impl Service for NopService {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(NOP_RPC, index)
    }

    fn __dispatch__(&self, rpc_id: i32, request: Box<Request>, connection: WeakServerConnection) {
        assert_eq!(rpc_id, NOP_RPC);
        let connection = connection.upgrade().expect("live server connection");
        let writer: ServerReplyFn = Some(Box::new(|archive: &mut BinaryWriteArchive| {
            Serialize::serialize(&1i64, archive);
        }));
        connection.reply(&request, 0, writer);
    }
}

#[derive(Default)]
struct Counters {
    chain_hops: AtomicI64,
    caller_completed: AtomicI64,
    errors: AtomicI64,
    chains_finished: AtomicI64,
}

// Issue one hop of a chain. The reply callback runs on the client's poll
// thread and issues the next hop through the same shared client handle.
fn chain(client: Arc<Client>, counters: Arc<Counters>, hops_left: i64, done: mpsc::Sender<()>) {
    let next_client = client.clone();
    let next_counters = counters.clone();
    let callback: AsyncReplyCallback = Some(Box::new(move |error, _payload, _size| {
        if error != 0 {
            next_counters.errors.fetch_add(1, Ordering::Relaxed);
            let _ = done.send(());
            return;
        }
        next_counters.chain_hops.fetch_add(1, Ordering::Relaxed);
        if hops_left > 1 {
            chain(next_client.clone(), next_counters.clone(), hops_left - 1, done.clone());
        } else {
            next_counters.chains_finished.fetch_add(1, Ordering::Relaxed);
            let _ = done.send(());
        }
    }));
    if client.request_async(NOP_RPC, |_| {}, callback).is_err() {
        counters.errors.fetch_add(1, Ordering::Relaxed);
    }
}

#[test]
fn client_handles_are_send_and_sync() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<Client>();
    assert_send_sync::<Arc<Client>>();
    assert_send_sync::<ClientPool>();
}

// The in-memory channel runs the reply callback inline, inside the caller's
// `request_async`. A callback that re-enters the same client deadlocks if
// `Client` holds its connection lock across the call into the connection.
#[test]
fn an_inline_reply_callback_can_reenter_the_same_client() {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let address = CString::new("inmemory://client-shared-reentry").unwrap();
    let server_poll = PollThread::create();
    let client_poll = PollThread::create();
    let mut server = Server::new(Some(server_poll.clone()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(NopService));
    // SAFETY: `address` is NUL terminated and outlives the call.
    assert_eq!(unsafe { server.start(address.as_ptr()) }, 0);
    let client = Client::create(client_poll.clone());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(address.as_ptr(), true), 0);

    let counters = Arc::new(Counters::default());
    let (done_tx, done_rx) = mpsc::channel();
    let caller_client = client.clone();
    let caller_counters = counters.clone();
    let caller = std::thread::spawn(move || chain(caller_client, caller_counters, 3, done_tx));
    done_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("an inline callback re-entering its client must not deadlock");
    caller.join().unwrap();
    assert_eq!(counters.errors.load(Ordering::Relaxed), 0);
    assert_eq!(counters.chain_hops.load(Ordering::Relaxed), 3);

    drop(client);
    client_poll.shutdown();
    drop(server);
    server_poll.shutdown();
}

#[test]
fn poll_thread_reply_callbacks_and_the_owner_share_one_client() {
    let server_poll = PollThread::create();
    let client_poll = PollThread::create();
    let mut server = Server::new(Some(server_poll.clone()));
    server.reg_service(Box::new(NopService));
    // SAFETY: the address literal is NUL terminated and outlives the call.
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let address = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    let client = Client::create(client_poll.clone());
    assert_eq!(client.connect(address.as_ptr(), true), 0);

    let counters = Arc::new(Counters::default());
    let (done_tx, done_rx) = mpsc::channel();
    for _ in 0..CHAINS {
        chain(client.clone(), counters.clone(), HOPS_PER_CHAIN, done_tx.clone());
    }
    drop(done_tx);

    // Meanwhile the owning thread keeps entering the same client.
    let deadline = Instant::now() + Duration::from_secs(60);
    let mut issued = 0i64;
    while issued < CALLER_REQUESTS {
        assert!(Instant::now() < deadline, "caller loop stalled at {issued} requests");
        if issued - counters.caller_completed.load(Ordering::Relaxed) >= CALLER_WINDOW {
            std::thread::yield_now();
            continue;
        }
        let completed = counters.clone();
        let callback: AsyncReplyCallback = Some(Box::new(move |error, _payload, _size| {
            if error != 0 {
                completed.errors.fetch_add(1, Ordering::Relaxed);
            }
            completed.caller_completed.fetch_add(1, Ordering::Relaxed);
        }));
        assert_eq!(client.request_async(NOP_RPC, |_| {}, callback), Ok(()));
        issued += 1;
    }

    for finished in 0..CHAINS {
        let remaining = deadline.saturating_duration_since(Instant::now());
        done_rx
            .recv_timeout(remaining)
            .unwrap_or_else(|_| panic!("only {finished} of {CHAINS} poll-thread chains finished"));
    }
    while counters.caller_completed.load(Ordering::Relaxed) < CALLER_REQUESTS {
        assert!(Instant::now() < deadline, "caller replies stalled");
        std::thread::yield_now();
    }

    assert_eq!(counters.errors.load(Ordering::Relaxed), 0);
    assert_eq!(counters.chains_finished.load(Ordering::Relaxed), CHAINS);
    assert_eq!(counters.chain_hops.load(Ordering::Relaxed), CHAINS * HOPS_PER_CHAIN);
    assert_eq!(counters.caller_completed.load(Ordering::Relaxed), CALLER_REQUESTS);
    assert_eq!(client.metrics().requests_sent(), (CHAINS * HOPS_PER_CHAIN + CALLER_REQUESTS) as u64);
    assert_eq!(client.metrics().in_flight_requests(), 0);

    client.close();
    drop(client);
    client_poll.shutdown();
    drop(server);
    server_poll.shutdown();
}
