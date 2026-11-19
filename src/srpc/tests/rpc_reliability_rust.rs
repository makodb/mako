//! Canonical RPC policy cases recovered from the historical C++ integration suites.
#![allow(unsafe_code)]

use srpc::circuit_breaker::{CircuitBreakerConfig, CircuitState};
use srpc::client::{Client, FutureAttr, CLIENT_ERR_NOT_CONNECTED};
use srpc::inmemory_channel::{make_inmemory_factory_proxy, InMemoryFactory, InMemorySwitchboard};
use srpc::reactor::PollThread;
use srpc::request_options::{RequestOptions, TimeoutType};
use srpc::serializable::Serialize;
use srpc::server::{DeferredReply, Request, Server, Service, WeakServerConnection};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

const HOLD: i32 = 0x00e0_7200;
const DEFER: i32 = HOLD + 1;
const DROP: i32 = HOLD + 2;
const FAIL: i32 = HOLD + 3;

#[derive(Default)]
struct State {
    calls: AtomicUsize,
    cleanups: AtomicUsize,
    async_calls: AtomicUsize,
    fail: AtomicBool,
}

struct Probe(Arc<State>);
impl Service for Probe {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        for rpc in HOLD..=FAIL {
            let error = server.reg_fast_rpc(rpc, index);
            if error != 0 {
                return error;
            }
        }
        0
    }
    fn __dispatch__(&self, rpc: i32, request: Box<Request>, connection: WeakServerConnection) {
        self.0.calls.fetch_add(1, Ordering::SeqCst);
        if rpc == HOLD {
            return;
        }
        if rpc == FAIL {
            // The breaker counts timeout/connection errors, not arbitrary
            // application errors. Return ETIMEDOUT through the real RPC reply.
            let error = if self.0.fail.load(Ordering::SeqCst) {
                110
            } else {
                0
            };
            connection.upgrade().unwrap().reply(&request, error, None);
            return;
        }
        let cleanup = self.0.clone();
        let mut deferred = DeferredReply::new(
            request,
            connection,
            Box::new(|out| Serialize::serialize(&123i64, out)),
            Box::new(move || {
                cleanup.cleanups.fetch_add(1, Ordering::SeqCst);
            }),
        );
        if rpc == DEFER {
            let state = self.0.clone();
            assert_eq!(
                deferred.run_async(Box::new(move || {
                    state.async_calls.fetch_add(1, Ordering::SeqCst);
                })),
                0
            );
            deferred.reply();
            deferred.reply();
            deferred.reply_error(5);
        }
    }
}

struct Fixture {
    client: Arc<Client>,
    server: Option<Server>,
    poll: Arc<PollThread>,
    state: Arc<State>,
}
impl Fixture {
    fn new() -> Self {
        let board = Arc::new(InMemorySwitchboard::new());
        let poll = PollThread::create();
        let state = Arc::new(State::default());
        let mut server = Server::new(Some(poll.clone()));
        server.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(
            InMemoryFactory::new(board.clone()),
        ))));
        server.reg_service(Box::new(Probe(state.clone())));
        assert_eq!(
            unsafe { server.start(c"inmemory://reliability".as_ptr()) },
            0
        );
        let client = Client::create(poll.clone());
        client.set_channel_factory(Some(make_inmemory_factory_proxy(Arc::new(
            InMemoryFactory::new(board),
        ))));
        assert_eq!(client.connect(c"inmemory://reliability".as_ptr(), true), 0);
        Self {
            client,
            server: Some(server),
            poll,
            state,
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        self.client.close();
        drop(self.server.take());
        self.poll.shutdown();
    }
}

#[test]
fn deferred_reply_is_once_only_and_drop_releases_request_and_cleanup() {
    let fixture = Fixture::new();
    let reply = fixture
        .client
        .request(DEFER, &FutureAttr::default(), |_| {})
        .unwrap();
    assert!(reply.ready());
    assert_eq!(reply.get_error_code(), 0);
    let mut value = 0i64;
    srpc::client::deserialize_from(reply.get_reply(), &mut value);
    assert_eq!(value, 123);
    assert_eq!(fixture.state.async_calls.load(Ordering::SeqCst), 1);
    assert_eq!(fixture.state.cleanups.load(Ordering::SeqCst), 1);
    let dropped = fixture
        .client
        .request(DROP, &FutureAttr::default(), |_| {})
        .unwrap();
    assert!(!dropped.ready());
    assert_eq!(fixture.state.cleanups.load(Ordering::SeqCst), 2);
    assert_eq!(fixture.server.as_ref().unwrap().pending_request_count(), 0);
    fixture.client.close();
    assert_eq!(dropped.get_error_code(), CLIENT_ERR_NOT_CONNECTED);
}

#[test]
fn non_idempotent_timeout_never_retries_and_total_budget_caps_idempotent_retries() {
    for idempotent in [false, true] {
        let fixture = Fixture::new();
        let mut options = RequestOptions::defaults();
        options.idempotent = idempotent;
        options.timeout_ms = 20;
        options.max_retries = 50;
        options.total_timeout_ms = 80;
        options.base_delay_ms = 5;
        options.max_delay_ms = 5;
        options.jitter_factor = 0.0;
        let start = Instant::now();
        let future = fixture
            .client
            .request_with_options(HOLD, &options, |_| {})
            .unwrap();
        let mut wait = options;
        wait.timeout_ms = 2000;
        future.set_options(&wait);
        future.wait_with_options();
        assert_ne!(future.get_error_code(), 0);
        assert!(start.elapsed() < Duration::from_secs(2));
        let calls = fixture.state.calls.load(Ordering::SeqCst);
        if idempotent {
            assert!(
                (1..=4).contains(&calls),
                "total budget must stop the retry chain: {calls}"
            );
            assert!(future.get_timeout_type() == TimeoutType::TOTAL_TIMEOUT);
        } else {
            assert_eq!(calls, 1);
            assert_eq!(future.get_retry_count(), 0);
            assert!(future.get_timeout_type() == TimeoutType::RESPONSE_TIMEOUT);
        }
    }
}

#[test]
fn circuit_breaker_rejects_without_dispatch_and_recovers_through_a_probe() {
    let fixture = Fixture::new();
    fixture.state.fail.store(true, Ordering::SeqCst);
    let mut config = CircuitBreakerConfig::defaults();
    config.failure_threshold = 2;
    config.success_threshold = 1;
    config.timeout_ms = 20;
    fixture.client.set_circuit_breaker(&config);
    for _ in 0..2 {
        let future = fixture
            .client
            .request(FAIL, &FutureAttr::default(), |_| {})
            .unwrap();
        assert_eq!(future.get_error_code(), 110);
    }
    assert!(fixture.client.circuit_breaker_state() == CircuitState::OPEN);
    let rejected = fixture.client.request(FAIL, &FutureAttr::default(), |_| {});
    match rejected {
        Ok(future) => assert_ne!(future.get_error_code(), 0),
        Err(error) => assert_ne!(error, 0),
    }
    assert_eq!(fixture.state.calls.load(Ordering::SeqCst), 2);
    fixture.state.fail.store(false, Ordering::SeqCst);
    std::thread::sleep(Duration::from_millis(30));
    let recovered = fixture
        .client
        .request(FAIL, &FutureAttr::default(), |_| {})
        .unwrap();
    assert_eq!(recovered.get_error_code(), 0);
    assert!(fixture.client.circuit_breaker_state() == CircuitState::CLOSED);
    assert_eq!(fixture.state.calls.load(Ordering::SeqCst), 3);
}
