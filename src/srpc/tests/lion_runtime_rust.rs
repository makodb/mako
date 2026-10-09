// A Lion runtime driven by SRPC's OS backend (plan item S1): the executor and
// reactor are Lion's (third-party/lion, built without its `mio` feature), and
// every wait, registration and cross-thread interrupt goes through
// `impl lion_reactor::os::OsBackend for SrpcEpollBackend`
// (reactor/epoll_wrapper.rs) to srpc_epoll.c.
//
// Every wait that could hang on a lost wakeup runs under a reactor-timer
// deadline, so a regression fails the test instead of wedging the binary.
// Foreign-thread wake latency lives in its own binary
// (tests/lion_foreign_wake_rust.rs), so these tests' threads do not compete
// with its measurement.

use lion_executor::os::{OsBackend, OsEvent, OsInterrupt, RawFd};
use lion_executor::{spawn, spawn_local, Runtime, RuntimeBuilder};
use lion_reactor::{AsyncFd, Duration, Instant, Interest, IoResult, ReactorHandle, ResourceId, Waker};
use srpc::epoll_wrapper::SrpcEpollBackend;
use std::cell::Cell;
use std::future::Future;
use std::io::{ErrorKind, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Instant as StdInstant;

const LIMIT_MS: u64 = 20_000;

fn srpc_runtime() -> Runtime {
    let backend = SrpcEpollBackend::new().expect("SRPC epoll backend");
    RuntimeBuilder::new().os_backend(Box::new(backend)).build().expect("Lion runtime over SRPC's backend")
}

// ---------------------------------------------------------------------------
// Test-local futures over Lion's reactor (lion-utility's `sleep`/`timeout` are
// outside SRPC's dependency graph).

// A one-shot reactor timer: registers `cx.waker()` until the deadline passes.
struct Sleep {
    deadline: Instant,
    rid: Option<ResourceId>,
}

fn sleep_ms(ms: u64) -> Sleep {
    Sleep { deadline: Instant::now() + Duration::from_millis(ms), rid: None }
}

impl Future for Sleep {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        let handle = ReactorHandle::new();
        if let Some(rid) = self.rid.take() {
            handle.deregister_timer(rid);
        }
        let now = ReactorHandle::cached_now().unwrap_or_else(Instant::now);
        if now.inner >= self.deadline.inner {
            return Poll::Ready(());
        }
        match handle.register_timer(self.deadline, Waker::from_std(cx.waker().clone())) {
            IoResult::Ok(rid) => self.rid = Some(rid),
            IoResult::Err(e) => panic!("register_timer: {e:?}"),
        }
        Poll::Pending
    }
}

impl Drop for Sleep {
    fn drop(&mut self) {
        if let Some(rid) = self.rid.take() {
            ReactorHandle::new().deregister_timer(rid);
        }
    }
}

// Fails the test instead of hanging when `fut` misses a wakeup: the reactor
// timer of `limit` is a wake source of its own.
struct Deadline<F> {
    fut: Pin<Box<F>>,
    limit: Sleep,
}

fn deadline<F: Future>(ms: u64, fut: F) -> Deadline<F> {
    Deadline { fut: Box::pin(fut), limit: sleep_ms(ms) }
}

impl<F: Future> Future for Deadline<F> {
    type Output = F::Output;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<F::Output> {
        if let Poll::Ready(v) = self.fut.as_mut().poll(cx) {
            return Poll::Ready(v);
        }
        if Pin::new(&mut self.limit).poll(cx).is_ready() {
            panic!("no wakeup within the deadline (a lost wakeup?)");
        }
        Poll::Pending
    }
}

// Counts what reaches SRPC's backend through the trait, then forwards to it.
struct Counted {
    inner: SrpcEpollBackend,
    stats: Arc<Stats>,
}

#[derive(Default)]
struct Stats {
    registers: AtomicUsize,
    deregisters: AtomicUsize,
    waits: AtomicUsize,
    timed_waits: AtomicUsize,
    events: AtomicUsize,
}

impl OsBackend for Counted {
    fn register(&mut self, fd: RawFd, token: usize, interest: Interest) -> std::io::Result<()> {
        self.stats.registers.fetch_add(1, Ordering::SeqCst);
        OsBackend::register(&mut self.inner, fd, token, interest)
    }
    fn reregister(&mut self, fd: RawFd, token: usize, interest: Interest) -> std::io::Result<()> {
        OsBackend::reregister(&mut self.inner, fd, token, interest)
    }
    fn deregister(&mut self, fd: RawFd) -> std::io::Result<()> {
        self.stats.deregisters.fetch_add(1, Ordering::SeqCst);
        OsBackend::deregister(&mut self.inner, fd)
    }
    fn wait(&mut self, events: &mut Vec<OsEvent>, timeout: Option<std::time::Duration>) -> std::io::Result<()> {
        self.stats.waits.fetch_add(1, Ordering::SeqCst);
        if timeout.is_some_and(|t| !t.is_zero()) {
            self.stats.timed_waits.fetch_add(1, Ordering::SeqCst);
        }
        let before = events.len();
        let result = OsBackend::wait(&mut self.inner, events, timeout);
        self.stats.events.fetch_add(events.len() - before, Ordering::SeqCst);
        result
    }
    fn interrupt(&self) -> Arc<dyn OsInterrupt> {
        OsBackend::interrupt(&self.inner)
    }
}

fn counted_runtime() -> (Runtime, Arc<Stats>) {
    let stats = Arc::new(Stats::default());
    let backend = Counted { inner: SrpcEpollBackend::new().expect("SRPC epoll backend"), stats: stats.clone() };
    let rt = RuntimeBuilder::new().os_backend(Box::new(backend)).build().expect("Lion runtime over SRPC's backend");
    (rt, stats)
}

// ---------------------------------------------------------------------------

// SRPC builds Lion without its `mio` feature, so Lion has no OS backend of its
// own: every runtime in these tests runs on SRPC's epoll backend or none.
#[test]
fn lion_has_no_default_backend_without_mio() {
    match Runtime::new() {
        Ok(_) => panic!("Runtime::new found a default OS backend; is Lion's mio feature on?"),
        Err(e) => assert_eq!(e.kind(), ErrorKind::Unsupported),
    }
}

#[test]
fn spawned_and_local_tasks_complete() {
    let rt = srpc_runtime();
    let total = rt.block_on(deadline(LIMIT_MS, async {
        let sent = spawn(async { 40 });
        // Not Send: only spawn_local accepts it.
        let local = Rc::new(Cell::new(2));
        let reader = local.clone();
        let local_task = spawn_local(async move { reader.get() });
        sent.await.expect("spawned task") + local_task.await.expect("local task")
    }));
    assert_eq!(total, 42);
}

#[test]
fn handle_spawns_run_under_an_embedder_tick_loop() {
    let rt = srpc_runtime();
    let done = Rc::new(Cell::new(0));
    for i in 0..3 {
        let done = done.clone();
        rt.handle().spawn_local(async move {
            sleep_ms(5 * i).await;
            done.set(done.get() + 1);
        });
    }
    let start = StdInstant::now();
    while done.get() < 3 {
        rt.tick_with_timeout(std::time::Duration::from_millis(50));
        assert!(start.elapsed() < std::time::Duration::from_secs(5), "tasks never completed");
    }
}

#[test]
fn lion_timers_fire_in_order_through_the_srpc_backend() {
    let (rt, stats) = counted_runtime();
    let start = StdInstant::now();
    let order = rt.block_on(deadline(LIMIT_MS, async {
        let fired = Rc::new(std::cell::RefCell::new(Vec::new()));
        let mut tasks = Vec::new();
        for ms in [60_u64, 20, 40] {
            let fired = fired.clone();
            tasks.push(spawn_local(async move {
                sleep_ms(ms).await;
                fired.borrow_mut().push(ms);
            }));
        }
        for task in tasks {
            task.await.expect("timer task");
        }
        let order = fired.borrow().clone();
        order
    }));
    let elapsed = start.elapsed();
    assert_eq!(order, vec![20, 40, 60]);
    assert!(elapsed >= std::time::Duration::from_millis(55), "timers fired early: {elapsed:?}");
    assert!(elapsed < std::time::Duration::from_secs(5), "timers fired late: {elapsed:?}");
    // The runtime slept in SRPC's epoll_wait with a timer-bounded timeout.
    assert!(stats.timed_waits.load(Ordering::SeqCst) >= 1, "no bounded wait reached the backend");
}

// Writes every byte of `data` to `stream`, waiting for writability with the
// AsyncFd whenever the socket buffer is full.
async fn write_all(fd: &AsyncFd, stream: &UnixStream, mut data: &[u8]) {
    while !data.is_empty() {
        let n = fd.write_io(|_| (&*stream).write(data)).await.expect("write");
        data = &data[n..];
    }
}

fn pattern(i: usize) -> u8 {
    (i.wrapping_mul(31) ^ (i >> 7)) as u8
}

#[test]
fn async_fd_echo_over_a_socketpair() {
    // Several times a default Unix socket buffer each way, so both the writer and
    // the echo server block on a full buffer many times.
    const TOTAL: usize = 1 << 20;
    let (rt, stats) = counted_runtime();
    let echoed = rt.block_on(deadline(LIMIT_MS, async {
        let (client, server) = UnixStream::pair().expect("socketpair");
        client.set_nonblocking(true).unwrap();
        server.set_nonblocking(true).unwrap();

        let echo = spawn_local(async move {
            let fd = AsyncFd::new(server.as_raw_fd()).expect("register server fd");
            let mut buf = vec![0_u8; 16 * 1024];
            let mut total = 0_usize;
            loop {
                let n = fd.read_io(|_| (&server).read(&mut buf)).await.expect("server read");
                if n == 0 {
                    return total;
                }
                total += n;
                write_all(&fd, &server, &buf[..n]).await;
            }
        });

        // One AsyncFd shared by a writer task and a reader task: one waiter
        // per direction.
        let client = Rc::new(client);
        let fd = Rc::new(AsyncFd::new(client.as_raw_fd()).expect("register client fd"));
        let writer = {
            let (fd, client) = (fd.clone(), client.clone());
            spawn_local(async move {
                let data: Vec<u8> = (0..TOTAL).map(pattern).collect();
                for chunk in data.chunks(7 * 1024 + 13) {
                    write_all(&fd, &client, chunk).await;
                }
                client.shutdown(std::net::Shutdown::Write).expect("half-close");
            })
        };
        let reader = {
            let (fd, client) = (fd.clone(), client.clone());
            spawn_local(async move {
                let mut buf = vec![0_u8; 9 * 1024 + 5];
                let mut seen = 0_usize;
                while seen < TOTAL {
                    let n = fd.read_io(|_| (&*client).read(&mut buf)).await.expect("client read");
                    assert!(n > 0, "echo server closed after {seen} bytes");
                    for (k, byte) in buf[..n].iter().enumerate() {
                        assert_eq!(*byte, pattern(seen + k), "byte {}", seen + k);
                    }
                    seen += n;
                }
                seen
            })
        };
        writer.await.expect("writer");
        let seen = reader.await.expect("reader");
        assert_eq!(seen, TOTAL);
        let served = echo.await.expect("echo server");
        drop(fd);
        served
    }));
    assert_eq!(echoed, TOTAL);
    // Both fds went through SRPC's register/deregister, and SRPC's wait
    // delivered the readiness edges that drove the tasks.
    assert_eq!(stats.registers.load(Ordering::SeqCst), 2);
    assert_eq!(stats.deregisters.load(Ordering::SeqCst), 2);
    assert!(stats.events.load(Ordering::SeqCst) > 0, "no readiness event came from the backend");
}

#[test]
fn tick_with_zero_timeout_does_not_block() {
    let rt = srpc_runtime();
    let fired = Rc::new(Cell::new(false));
    {
        let fired = fired.clone();
        rt.handle().spawn_local(async move {
            sleep_ms(300).await;
            fired.set(true);
        });
    }
    // The first step polls the task, which arms its 300 ms timer; from then on
    // the runtime is idle, and only the bound on the park decides how long a
    // step takes.
    rt.tick_with_timeout(std::time::Duration::ZERO);
    let start = StdInstant::now();
    for _ in 0..100 {
        let step = StdInstant::now();
        rt.tick_with_timeout(std::time::Duration::ZERO);
        assert!(step.elapsed() < std::time::Duration::from_millis(50), "a ZERO step blocked for {:?}", step.elapsed());
    }
    assert!(start.elapsed() < std::time::Duration::from_millis(200), "100 ZERO steps took {:?}", start.elapsed());
    assert!(!fired.get());
    // A bounded step does park, for about its bound (and the timer still fires).
    let step = StdInstant::now();
    rt.tick_with_timeout(std::time::Duration::from_millis(30));
    let parked = step.elapsed();
    assert!(parked >= std::time::Duration::from_millis(25), "a 30 ms step returned after {parked:?}");
    let start = StdInstant::now();
    while !fired.get() {
        rt.tick_with_timeout(std::time::Duration::from_millis(100));
        assert!(start.elapsed() < std::time::Duration::from_secs(5), "the 300 ms timer never fired");
    }
}
