#![allow(unsafe_code)]

use srpc::client::{deserialize_from, Client, FutureAttr};
use srpc::fiber::this_fiber;
use srpc::reactor::{reactor_spawn_stackless_task_with_result, Fiber, PollThread, Reactor};
use srpc::serializable::{BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize};
use srpc::server::{Request, Server, Service, WeakServerConnection};
use std::cell::{Cell, RefCell};
use std::ffi::CString;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc};
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant};

const RPC_ID: i32 = 0x00e0_2001;

struct Echo;

impl Service for Echo {
    fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
        server.reg_fast_rpc(RPC_ID, index)
    }

    fn __dispatch__(&self, _: i32, mut request: Box<Request>, connection: WeakServerConnection) {
        let mut value = 0i64;
        // Request owns this buffer until deserialization completes.
        let mut archive = BinaryReadArchive::new(unsafe {
            srpc::serializable::make_source_proxy_buffer(&raw mut request.src)
        });
        Deserialize::deserialize(&mut value, &mut archive);
        connection.upgrade().unwrap().reply(&request, 0, Some(Box::new(move |out| {
            Serialize::serialize(&(value * 2), out);
        })));
    }
}

struct WakeGate {
    ready: Arc<AtomicBool>,
    sender: mpsc::Sender<Waker>,
}

impl Future for WakeGate {
    type Output = i64;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<i64> {
        if self.ready.load(Ordering::Acquire) {
            Poll::Ready(7)
        } else {
            self.sender.send(cx.waker().clone()).unwrap();
            Poll::Pending
        }
    }
}

#[test]
fn runtime_parity_transcript() {
    let reactor = Reactor::get_reactor();
    let order = Rc::new(RefCell::new(Vec::new()));
    let timer_done = Rc::new(Cell::new(false));
    let deadline_respected = Rc::new(Cell::new(false));
    let timer_order = order.clone();
    let done = timer_done.clone();
    let respected = deadline_respected.clone();
    Fiber::create_run(move || {
        timer_order.borrow_mut().push("start");
        let started = Instant::now();
        this_fiber::sleep_ms(10);
        respected.set(started.elapsed() >= Duration::from_millis(10));
        timer_order.borrow_mut().push("resume");
        done.set(true);
    });
    let timer_suspended = !timer_done.get();
    let peer_order = order.clone();
    Fiber::create_run(move || peer_order.borrow_mut().push("peer"));
    let deadline = Instant::now() + Duration::from_secs(2);
    while !timer_done.get() && Instant::now() < deadline {
        reactor.run_loop(false, true);
        std::thread::yield_now();
    }
    assert!(timer_done.get());
    assert_eq!(*order.borrow(), ["start", "peer", "resume"]);
    assert!(timer_suspended && deadline_respected.get());

    let owner = std::thread::current().id();
    let ready = Arc::new(AtomicBool::new(false));
    let (wake_tx, wake_rx) = mpsc::channel();
    let wake_result = Rc::new(Cell::new(0i64));
    let completion_owner = Rc::new(Cell::new(false));
    let result = wake_result.clone();
    let on_owner = completion_owner.clone();
    reactor_spawn_stackless_task_with_result(&reactor,
        Box::pin(WakeGate { ready: ready.clone(), sender: wake_tx }),
        move |value| {
            result.set(value);
            on_owner.set(std::thread::current().id() == owner);
        });
    let wake = wake_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    let pending_before_wake = wake_result.get() == 0;
    let foreign_thread = std::thread::spawn(move || {
        let foreign = std::thread::current().id() != owner;
        ready.store(true, Ordering::Release);
        wake.wake();
        foreign
    }).join().unwrap();
    let deadline = Instant::now() + Duration::from_secs(2);
    while wake_result.get() == 0 && Instant::now() < deadline {
        reactor.run_loop(false, true);
        std::thread::yield_now();
    }
    assert_eq!(wake_result.get(), 7);
    assert!(pending_before_wake && foreign_thread && completion_owner.get());

    let poll = PollThread::create();
    let mut server = Server::new(Some(poll.clone()));
    server.reg_service(Box::new(Echo));
    assert_eq!(unsafe { server.start(c"127.0.0.1:0".as_ptr()) }, 0);
    let address = CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    let client = Client::create(poll.clone());
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    let success = client.request(RPC_ID, &FutureAttr::default(), |out: &mut BinaryWriteArchive| {
        Serialize::serialize(&21i64, out);
    }).unwrap();
    let success_error = success.get_error_code();
    let mut reply = 0i64;
    deserialize_from(success.get_reply(), &mut reply);
    let failure = client.request(RPC_ID + 1, &FutureAttr::default(), |_| {}).unwrap();
    let failure_error = failure.get_error_code();
    client.close();
    drop(client);
    drop(server);
    poll.shutdown();
    assert_eq!((reply, success_error, failure_error), (42, 0, 2));

    println!("SRPC_RUNTIME_PARITY {{\"version\":1,\"timer_order\":{:?},\"timer_suspended\":{},\"deadline_respected\":{},\"pending_before_wake\":{},\"foreign_thread\":{},\"completion_on_owner\":{},\"wake_value\":{},\"rpc_reply\":{},\"rpc_success_error\":{},\"rpc_missing_error\":{}}}",
        *order.borrow(), timer_suspended, deadline_respected.get(), pending_before_wake,
        foreign_thread, completion_owner.get(), wake_result.get(), reply, success_error, failure_error);
}
