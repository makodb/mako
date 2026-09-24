// Tier 2.4 of docs/testing-plan.md: the RPC Future's timeout contract.
//
// The bare `Future::wait()` is hard-capped at one second and then latches
// permanently -- "the single most surprising thing about the client" (the
// book, ch.8) and, until now, untested. (future_rust.rs covers the *fiber*
// FiberFuture in reactor/future.rs, a different type.) wait_with_options and
// the retry chain are covered by client_retry_rust.rs; this file pins the
// plain-wait cap and its one-way latch.
//
// The cap test costs one real second by construction (the 1s deadline is a
// private constant on Future), which is negligible next to the battery's
// 600s-timeout suites.

use std::ffi::CString;
use std::sync::Arc;
use std::time::{Duration, Instant};

use srpc::client::{Client, FutureAttr, CLIENT_ERR_TIMED_OUT};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};

use srpc::reactor::PollThread;

const ECHO_RPC_ID: i32 = 0x00E0_0046;
const BLACK_HOLE_RPC_ID: i32 = 0x00E0_0047;

struct TimeoutProbeService;

impl Service for TimeoutProbeService {
    fn __reg_to__(&mut self, server: &mut Server, svc_index: usize) -> i32 {
        let r = server.reg_fast_rpc(ECHO_RPC_ID, svc_index);
        if r != 0 {
            return r;
        }
        server.reg_fast_rpc(BLACK_HOLE_RPC_ID, svc_index)
    }

    #[allow(unsafe_code)]
    fn __dispatch__(&self, rpc_id: i32, mut req: Box<Request>, sconn: WeakServerConnection) {
        if rpc_id == BLACK_HOLE_RPC_ID {
            return; // received, never replied -> the caller's Future must time out
        }
        let mut value = 0i64;
        {
            let mut ar = BinaryReadArchive::new(unsafe {
                srpc::serializable::make_source_proxy_buffer(&raw mut req.src)
            });
            Deserialize::deserialize(&mut value, &mut ar);
        }
        let sconn = sconn.upgrade().expect("live connection");
        let writer: ServerReplyFn = Some(Box::new(move |ar: &mut BinaryWriteArchive| {
            Serialize::serialize(&(value * 2), ar);
        }));
        sconn.reply(&req, 0, writer);
    }
}

#[allow(unsafe_code)]
fn connected_pair(tag: &str) -> (Server, Arc<Client>) {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let addr = CString::new(format!("inmemory://{tag}")).expect("addr");
    // SAFETY: rustc-lane poll threads are inert; in-memory never schedules on them.
    let mut server = Server::new(Some(PollThread::create()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(TimeoutProbeService));
    assert_eq!(unsafe { server.start(addr.as_ptr()) }, 0);
    let client = Client::create(PollThread::create());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(addr.as_ptr(), true), 0);
    (server, client)
}

#[test]
fn plain_wait_caps_at_one_second_and_latches_timed_out() {
    let (server, client) = connected_pair("wait-cap");

    let fu = client
        .request(BLACK_HOLE_RPC_ID, &FutureAttr::default(), |ar| {
            Serialize::serialize(&1_i64, ar);
        })
        .expect("request accepted");

    let start = Instant::now();
    fu.wait();
    let elapsed = start.elapsed();

    // The cap is ~1s: it must not return immediately, and must not hang past
    // a generous ceiling.
    assert!(
        elapsed >= Duration::from_millis(800),
        "wait() must block ~1s on a black-holed request, blocked {elapsed:?}"
    );
    assert!(
        elapsed < Duration::from_secs(5),
        "wait() must not hang past the 1s cap, blocked {elapsed:?}"
    );
    assert_eq!(
        fu.get_error_code(),
        CLIENT_ERR_TIMED_OUT,
        "the cap latches ETIMEDOUT (110)"
    );
    // The one-way latch: a timed-out future never becomes ready, and repeated
    // reads stay ETIMEDOUT (and return immediately, not after another second).
    assert!(!fu.ready(), "a timed-out future stays not-ready");
    let recheck = Instant::now();
    assert_eq!(fu.get_error_code(), CLIENT_ERR_TIMED_OUT, "latch is stable");
    assert!(!fu.ready(), "latch is one-way");
    assert!(
        recheck.elapsed() < Duration::from_millis(200),
        "reads after the latch return immediately"
    );

    drop(server);
    drop(client);
}

#[test]
fn a_replied_request_resolves_well_under_the_cap() {
    let (server, client) = connected_pair("wait-fast");

    let start = Instant::now();
    let fu = client
        .request(ECHO_RPC_ID, &FutureAttr::default(), |ar| {
            Serialize::serialize(&21_i64, ar);
        })
        .expect("request accepted");
    fu.wait();
    let elapsed = start.elapsed();

    assert_eq!(fu.get_error_code(), 0, "a real reply resolves success");
    assert!(fu.ready(), "a resolved future is ready");
    assert!(
        elapsed < Duration::from_millis(500),
        "an in-memory reply resolves far under the 1s cap, took {elapsed:?}"
    );
    let mut doubled = 0i64;
    srpc::client::deserialize_from(fu.get_reply(), &mut doubled);
    assert_eq!(doubled, 42);

    drop(server);
    drop(client);
}
