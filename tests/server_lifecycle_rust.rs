//! Shutdown and restart contracts formerly exercised only by omitted C++ suites.
use srpc::reactor::PollThread;
use srpc::server::{server_generate_instance_id, Server, ShutdownPhase};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

#[test]
fn shutdown_hooks_run_in_order_despite_panics_and_signal_completion() {
    let mut server = Server::new(None);
    let calls = Arc::new(Mutex::new(Vec::new()));
    assert!(server.phase() == ShutdownPhase::RUNNING);
    assert_eq!(server.pending_request_count(), 0);
    for index in 0..3 {
        let calls = calls.clone();
        server.add_shutdown_hook(Box::new(move || {
            calls.lock().unwrap().push(index);
            if index == 1 {
                panic!("intentional shutdown hook failure");
            }
        }));
    }
    server.stop_accepting();
    server.stop_accepting();
    assert!(server.phase() == ShutdownPhase::STOP_ACCEPTING);
    assert!(server.drain(0));
    assert!(server.phase() == ShutdownPhase::DRAINING);
    server.graceful_shutdown(0);
    assert!(server.phase() == ShutdownPhase::STOPPED);
    assert_eq!(*calls.lock().unwrap(), [0, 1, 2]);
    server.wait_for_shutdown();
}

#[test]
fn drain_timeout_preserves_pending_count_and_shutdown_still_finishes() {
    let mut server = Server::new(None);
    server.increment_pending();
    assert!(!server.drain(0));
    assert_eq!(server.pending_request_count(), 1);
    assert!(server.phase() == ShutdownPhase::DRAINING);
    server.graceful_shutdown(0);
    assert!(server.phase() == ShutdownPhase::STOPPED);
    assert_eq!(server.pending_request_count(), 1);
    server.decrement_pending();
    assert_eq!(server.pending_request_count(), 0);
    assert!(server.drain(0));
}

#[test]
fn instance_ids_are_nonzero_stable_and_unique_across_threads() {
    let server = Server::new(None);
    let id = server.instance_id();
    assert_ne!(id, 0);
    assert_eq!(id, server.instance_id());
    let threads: Vec<_> = (0..16)
        .map(|_| std::thread::spawn(server_generate_instance_id))
        .collect();
    let ids: std::collections::HashSet<_> =
        threads.into_iter().map(|t| t.join().unwrap()).collect();
    assert_eq!(ids.len(), 16);
    assert!(!ids.contains(&0));
    assert!(!ids.contains(&id));
}

#[test]
fn drain_waits_for_a_real_inflight_request() {
    use srpc::client::{Client, FutureAttr};
    use srpc::server::{Request, Service, WeakServerConnection};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::mpsc::{channel, RecvTimeoutError};
    struct Slow {
        entered: std::sync::mpsc::Sender<()>,
        release: Arc<AtomicBool>,
    }
    impl Service for Slow {
        fn __reg_to__(&mut self, server: &mut Server, index: usize) -> i32 {
            server.reg_rpc(0x00e0_7102, index)
        }
        fn __dispatch__(&self, _: i32, request: Box<Request>, connection: WeakServerConnection) {
            self.entered.send(()).unwrap();
            let deadline = Instant::now() + Duration::from_secs(5);
            while !self.release.load(Ordering::Acquire) {
                assert!(Instant::now() < deadline, "request was never released");
                srpc::fiber::this_fiber::sleep_ms(1);
            }
            connection.upgrade().unwrap().reply(&request, 0, None);
        }
    }
    let server_poll = PollThread::create();
    let client_poll = PollThread::create();
    let (entered, waiting) = channel();
    let release = Arc::new(AtomicBool::new(false));
    let mut server = Server::new(Some(server_poll.clone()));
    server.reg_service(Box::new(Slow {
        entered,
        release: release.clone(),
    }));
    // The literal is NUL terminated and lives through start.
    #[allow(unsafe_code)]
    let started = unsafe { server.start(c"127.0.0.1:0".as_ptr()) };
    assert_eq!(started, 0);
    let address = std::ffi::CString::new(format!("127.0.0.1:{}", server.get_bound_port())).unwrap();
    let client = Client::create(client_poll.clone());
    assert_eq!(client.connect(address.as_ptr(), true), 0);
    let future = client
        .request(0x00e0_7102, &FutureAttr::default(), |_| {})
        .unwrap();
    waiting.recv_timeout(Duration::from_secs(2)).unwrap();
    assert_eq!(server.pending_request_count(), 1);
    let (draining, started) = channel();
    let (finished, done) = channel();
    let release_thread = std::thread::spawn(move || {
        started.recv_timeout(Duration::from_secs(2)).unwrap();
        let early_completion = done.recv_timeout(Duration::from_millis(50));
        release.store(true, Ordering::Release);
        assert_eq!(
            early_completion,
            Err(RecvTimeoutError::Timeout),
            "drain must wait while the handler still owns its request"
        );
        done.recv_timeout(Duration::from_secs(2)).unwrap();
    });
    draining.send(()).unwrap();
    let drained = server.drain(2000);
    finished.send(()).unwrap();
    release_thread.join().unwrap();
    assert!(drained);
    assert_eq!(server.pending_request_count(), 0);
    future.wait();
    assert_eq!(future.get_error_code(), 0);
    assert_eq!(server.pending_request_count(), 0);
    assert!(server.drain(0));
    server.graceful_shutdown(0);
    assert!(server.phase() == ShutdownPhase::STOPPED);
    drop(client);
    client_poll.shutdown();
    drop(server);
    server_poll.shutdown();
}
