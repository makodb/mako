#![allow(unsafe_code)]

use srpc::client::{deserialize_from, BufferingConfig, Client, Future, FutureAttr};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::reactor::PollThread;
use srpc::request_queue::{kRequestQueueExpiredError, kRequestQueueRejectedError};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, ServerReplyFn, Service, WeakServerConnection};
use std::ffi::CString;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const RPC: i32 = 0x00e0_0091;

struct Echo(Arc<Mutex<Vec<i64>>>);

impl Service for Echo {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(RPC, index)
    }

    fn __dispatch__(&self, _: i32, mut request: Box<Request>, connection: WeakServerConnection) {
        let mut value = 0i64;
        let mut archive = BinaryReadArchive::new(unsafe {
            srpc::serializable::make_source_proxy_buffer(&raw mut request.src)
        });
        Deserialize::deserialize(&mut value, &mut archive);
        self.0.lock().unwrap().push(value);
        let reply: ServerReplyFn = Some(Box::new(move |output: &mut BinaryWriteArchive| {
            Serialize::serialize(&(value * 2), output);
        }));
        connection.upgrade().unwrap().reply(&request, 0, reply);
    }
}

type Fixture = (Server, Arc<Client>, Arc<PollThread>, Arc<Mutex<Vec<i64>>>);

fn disconnected(name: &str, config: BufferingConfig) -> Fixture {
    let calls = Arc::new(Mutex::new(Vec::new()));
    let switchboard = Arc::new(InMemorySwitchboard::new());
    let poll = PollThread::create();
    let mut server = Server::new(Some(poll.clone()));
    server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard.clone(),
    )))));
    server.reg_service(Box::new(Echo(calls.clone())));
    let address = CString::new(format!("inmemory://{name}")).unwrap();
    assert_eq!(unsafe { server.start(address.as_ptr()) }, 0);
    let client = Client::create(poll.clone());
    client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(InMemoryFactory::new(
        switchboard,
    )))));
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    client.set_buffering_config(&config);
    client.connection().unwrap().close();
    assert!(!client.connected());
    (server, client, poll, calls)
}

fn queue(client: &Client, value: i64, writes: &Arc<AtomicUsize>) -> Arc<Future> {
    let writes = writes.clone();
    client.request(RPC, &FutureAttr::default(), move |archive| {
        writes.fetch_add(1, Ordering::SeqCst);
        Serialize::serialize(&value, archive);
    }).unwrap()
}

fn finish(server: Server, client: Arc<Client>, poll: Arc<PollThread>) {
    client.close();
    drop(client);
    drop(server);
    poll.shutdown();
}

#[test]
fn reconnect_replays_owned_bytes_in_order_without_reinvoking_writers() {
    let (server, client, poll, calls) = disconnected("replay-fifo", BufferingConfig::defaults());
    let writes = Arc::new(AtomicUsize::new(0));
    let futures: Vec<_> = [11, 22, 33].map(|value| queue(&client, value, &writes)).into();
    assert_eq!(writes.load(Ordering::SeqCst), 3, "writers run when queued");
    assert!(calls.lock().unwrap().is_empty());
    assert_eq!(client.pending_request_count(), 3);
    assert_eq!(client.reconnect(Default::default()), 0);
    assert_eq!(writes.load(Ordering::SeqCst), 3, "replay must reuse the encoded body");
    assert_eq!(*calls.lock().unwrap(), [11, 22, 33]);
    for (future, expected) in futures.iter().zip([22, 44, 66]) {
        assert_eq!(future.get_error_code(), 0);
        let mut value = 0i64;
        deserialize_from(future.get_reply(), &mut value);
        assert_eq!(value, expected);
    }
    assert_eq!(client.pending_request_count(), 0);
    let connection = client.connection().unwrap();
    assert_eq!(connection.replay_pending_requests(), 0, "a second replay sends nothing");
    assert_eq!(connection.pending_future_count(), 0);
    assert_eq!(connection.metrics().in_flight_requests(), 0);
    drop(connection);
    finish(server, client, poll);
}

#[test]
fn reconnect_expires_stale_payloads_without_sending_them() {
    let mut config = BufferingConfig::defaults();
    config.default_ttl_ms = 1;
    let (server, client, poll, calls) = disconnected("replay-expiry", config);
    let future = queue(&client, 9, &Arc::new(AtomicUsize::new(0)));
    std::thread::sleep(Duration::from_millis(3));
    assert_eq!(client.reconnect(Default::default()), 0);
    assert_eq!(future.get_error_code(), kRequestQueueExpiredError);
    assert!(calls.lock().unwrap().is_empty());
    assert_eq!(client.pending_request_count(), 0);
    assert_eq!(client.connection().unwrap().pending_future_count(), 0);
    finish(server, client, poll);
}

#[test]
fn overflow_rejects_the_evicted_future_and_replays_only_the_survivor() {
    let mut config = BufferingConfig::defaults();
    config.max_pending = 1;
    let (server, client, poll, calls) = disconnected("replay-overflow", config);
    let writes = Arc::new(AtomicUsize::new(0));
    let first = queue(&client, 9, &writes);
    let second = queue(&client, 10, &writes);
    assert_eq!(first.get_error_code(), kRequestQueueRejectedError);
    assert!(!second.ready());
    assert_eq!(client.reconnect(Default::default()), 0);
    assert_eq!(second.get_error_code(), 0);
    assert_eq!(*calls.lock().unwrap(), [10]);
    assert_eq!(writes.load(Ordering::SeqCst), 2);
    assert_eq!(client.pending_request_count(), 0);
    finish(server, client, poll);
}
