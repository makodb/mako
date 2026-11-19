// The revival half of the tier-2 reconnect story, in its OWN test binary:
// under one binary's parallel harness the sibling tests' teardown close-jobs
// interleave with this test's server restart on the shared facade poll
// threads, and the flake pointed at real ordering hazards worth isolating
// rather than papering over in-process.  (The switchboard identity-check fix
// in rpc/inmemory_channel.rs came out of exactly this test; see its comment.)

// (Preamble shared by copy with tests/client_retry_rust.rs.)
// The tier-2 payoff: request_with_options RETRIES under rustc, on a real
// spawned coordinator thread.  Until this increment the facade's
// thread::spawn dropped its body, so the whole options/retry family was
// deliberately left un-pub -- flipping it earlier would have shipped an API
// that accepts options and silently never retries.  The enabling changes:
// Future carries the ClientConnection-style notify-before-read Send/Sync
// contract, the callback aliases carry Send bounds, and the facade spawn is
// a real std::thread.
//
// The black-hole service receives requests and never replies, so each
// attempt times out on its per-attempt budget and the coordinator walks the
// whole chain -- observable, bounded (small budgets), and deterministic in
// outcome if not in timing.

use std::ffi::CString;
use std::sync::mpsc;
use std::sync::Arc;

use srpc::client::Client;
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};

use srpc::reactor::PollThread;

const ECHO_RPC_ID: i32 = 0x00E0_0044;
const BLACK_HOLE_RPC_ID: i32 = 0x00E0_0045;

struct RetryProbeService;

impl Service for RetryProbeService {
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
            // Receive and never reply: the attempt's future must time out.
            return;
        }
        assert_eq!(rpc_id, ECHO_RPC_ID);
        let mut value = 0i64;
        {
            let mut ar = BinaryReadArchive::new(unsafe {
                srpc::serializable::make_source_proxy_buffer(&raw mut req.src)
            });
            Deserialize::deserialize(&mut value, &mut ar);
        }
        let sconn = sconn.upgrade().expect("connection alive during inline dispatch");
        let writer: ServerReplyFn = Some(Box::new(move |ar: &mut BinaryWriteArchive| {
            Serialize::serialize(&(value + 1), ar);
        }));
        sconn.reply(&req, 0, writer);
    }
}

#[allow(unsafe_code)]
#[allow(dead_code)] // shared-by-copy preamble; the revival test builds its pair inline
fn connected_pair(tag: &str) -> (Server, Arc<Client>) {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let addr = CString::new(format!("inmemory://{tag}")).expect("addr");

    // SAFETY: rustc-lane poll threads; the in-memory channel never schedules
    // onto them.
    let mut server = Server::new(Some(PollThread::create()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(RetryProbeService));
    // SAFETY: `addr` is NUL-terminated and outlives the call.
    assert_eq!(unsafe { server.start(addr.as_ptr()) }, 0);

    let client = Client::create(PollThread::create());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(addr.as_ptr(), true), 0);
    (server, client)
}

#[test]
#[allow(unsafe_code)]
fn explicit_reconnect_recovers_after_the_server_returns() {
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let probe_switchboard = switchboard.clone();
    let addr = CString::new("inmemory://reconnect").expect("addr");

    // SAFETY: rustc-lane poll threads; the in-memory channel never schedules
    // onto them (three create sites in this test).
    let mut server = Server::new(Some(PollThread::create()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(RetryProbeService));
    assert_eq!(unsafe { server.start(addr.as_ptr()) }, 0);

    let client = Client::create(PollThread::create());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    assert_eq!(client.connect(addr.as_ptr(), true), 0);
    // Only the EXPLICIT path under test: without this, the default policy's
    // auto-reconnect thread can race the revived server below.
    client.set_reconnect_policy(&srpc::reconnect_policy::ReconnectPolicy::no_retry());

    // Reconnecting while CONNECTED is refused by contract, and the completion
    // callback still fires with `false`.
    let (tx0, rx0) = mpsc::channel::<bool>();
    let refused_cb: srpc::client::OnReconnectCompleteCallbackFn = Some(Box::new(
        move |ok: bool| {
            let _ = tx0.send(ok);
        },
    ));
    assert_eq!(client.reconnect(refused_cb), 22, "EINVAL from CONNECTED");
    assert!(!rx0.recv_timeout(std::time::Duration::from_secs(1)).unwrap());

    // Kill the server. The listener close rides a poll-thread job (the
    // deferred close-job protocol), so under a loaded parallel test run the
    // disconnect lands a tick later -- wait for it, bounded.
    drop(server);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    while client.connected() {
        assert!(std::time::Instant::now() < deadline, "disconnect must land");
        std::thread::sleep(std::time::Duration::from_millis(5));
    }

    // Bring a server back on the same switchboard address, then reconnect --
    // the completion arrives from the coordinator's real spawned thread.
    let mut revived = Server::new(Some(PollThread::create()));
    revived.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    revived.reg_service(Box::new(RetryProbeService));
    let revived_start = unsafe { revived.start(addr.as_ptr()) };
    let addr_string = "inmemory://reconnect".to_string();
    eprintln!(
        "PROBE revived.start={} entry_upgrades={}",
        revived_start,
        probe_switchboard.find_listener(&addr_string).is_some()
    );
    assert_eq!(revived_start, 0);

    // The OLD listener's deferred close-job can still be unregistering the
    // switchboard address when the revived server registers it, so the first
    // dial may land in the gap. Retry the explicit reconnect, bounded.
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    let ok = loop {
        let (tx, rx) = mpsc::channel::<bool>();
        let on_complete: srpc::client::OnReconnectCompleteCallbackFn = Some(Box::new(
            move |completed: bool| {
                let _ = tx.send(completed);
            },
        ));
        let ret = client.reconnect(on_complete);
        let completed = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("reconnect completion must arrive");
        if ret == 0 && completed {
            break true;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "reconnect must eventually reach the revived server (last ret={ret}, entry_upgrades={})",
            probe_switchboard.find_listener(&addr_string).is_some()
        );
        std::thread::sleep(std::time::Duration::from_millis(10));
    };
    assert!(ok, "reconnect to the revived server succeeds");
    assert!(client.connected());

    drop(revived);
    drop(client);
}
