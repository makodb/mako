// A PollThread is one OS thread running one Lion runtime (S3 of
// docs/dev/lion-runtime-plan.md): a driver task does the owner-side work
// run_loop does elsewhere, woken by each source of work instead of by a 1 ms
// epoll timeout; each registered pollable has a transport task over Lion's
// AsyncFd (since S5 a TCP connection has its own reader and writer tasks
// instead, see tcp_transport_rust.rs); stackless tasks run as Lion
// spawn_local tasks.
//
// These tests drive the real crate: real PollThreads, real sockets, real
// fibers. Each wake path has a test that times out if that wake is dropped
// (the negative controls are recorded in the plan's S3 note). The latency
// tests print their distributions and assert only loose bounds: the host
// running the gate is shared and often heavily loaded.
//
// Every test takes SERIAL: the idle-CPU and latency measurements must not
// compete with the other tests' threads, and the descriptor count in the
// shutdown test must not see another test's sockets.

#![allow(unsafe_code)]

use srpc::callback_wrapper::detail::CallbackWrapper;
use srpc::channel::{
    ChannelConnectionBase, ChannelConnectionProxy, ChannelError, ChannelFrame,
    NullableChannelConnectionProxy, OnAcceptCallback, OnClosedCallback, OnErrorCallback,
    OnFrameCallback,
};
use srpc::fiber_channel::FiberChannel;
use srpc::misc::{Job, OneTimeJob};
use srpc::reactor::{
    create_sp_int_event, reactor_spawn_stackless_task_impl,
    reactor_spawn_stackless_task_with_result, stackless_cancel_report, Fiber, PollThread,
    Reactor, StacklessPollFn,
};
use srpc::tcp_channel::TcpFactory;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex, MutexGuard, PoisonError};
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant};

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL.lock().unwrap_or_else(PoisonError::into_inner)
}

const LIMIT: Duration = Duration::from_secs(10);

// A OneTimeJob running `f` once on the poll thread.
fn job<F: FnOnce() + Send + 'static>(f: F) -> Arc<dyn Job> {
    let slot = Mutex::new(Some(f));
    Arc::new(OneTimeJob::new(Box::new(move || {
        let taken = slot.lock().unwrap().take();
        if let Some(f) = taken {
            f();
        }
    })))
}

// Run `f` on the poll thread and wait for it to return.
fn run_on<R: Send + 'static, F: FnOnce() -> R + Send + 'static>(pt: &PollThread, f: F) -> R {
    let (tx, rx) = mpsc::channel();
    pt.add(job(move || {
        tx.send(f()).unwrap();
    }));
    rx.recv_timeout(LIMIT).expect("the job never ran on the poll thread")
}

// The current Lion task's id, as a number (the type itself is unnameable).
fn current_lion_task() -> Option<u64> {
    lion_executor::tls::get_current_task().map(|id| id.0)
}

// Prints a latency distribution and returns its median.
fn summarize(what: &str, mut samples: Vec<Duration>) -> Duration {
    samples.sort();
    let at = |p: f64| samples[((samples.len() - 1) as f64 * p).round() as usize];
    println!(
        "{what}: n={} min={:?} p50={:?} p90={:?} p99={:?} max={:?}",
        samples.len(),
        samples[0],
        at(0.5),
        at(0.9),
        at(0.99),
        samples[samples.len() - 1]
    );
    at(0.5)
}

// Nanoseconds on CPU and context switches of one thread of this process.
fn thread_cpu_and_switches(tid: u64) -> (u64, u64) {
    let schedstat = std::fs::read_to_string(format!("/proc/self/task/{tid}/schedstat")).unwrap();
    let cpu_ns: u64 = schedstat.split_whitespace().next().unwrap().parse().unwrap();
    let status = std::fs::read_to_string(format!("/proc/self/task/{tid}/status")).unwrap();
    let mut switches = 0u64;
    for line in status.lines() {
        if line.starts_with("voluntary_ctxt_switches:") || line.starts_with("nonvoluntary_ctxt_switches:") {
            switches += line.split_whitespace().nth(1).unwrap().parse::<u64>().unwrap();
        }
    }
    (cpu_ns, switches)
}

fn open_fds() -> usize {
    std::fs::read_dir("/proc/self/fd").unwrap().count()
}

// ---------------------------------------------------------------------------
// Idle and command wake

#[test]
fn an_idle_poll_thread_does_not_spin() {
    let _serial = serial();
    let pt = PollThread::create();
    // With a registered, idle connection: its transport tasks must wait for
    // the next edge rather than re-poll a readiness they already consumed.
    let peer = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let connected = TcpFactory::new(pt.clone()).connect(&peer.local_addr().unwrap().to_string());
    assert_eq!(connected.error, ChannelError::None);
    let (_peer_stream, _) = peer.accept().unwrap();
    run_on(&pt, || ());
    let tid = pt.poll_thread_id_bits_.load(Ordering::Acquire);
    std::thread::sleep(Duration::from_millis(50));
    let (cpu0, switches0) = thread_cpu_and_switches(tid);
    std::thread::sleep(Duration::from_secs(1));
    let (cpu1, switches1) = thread_cpu_and_switches(tid);
    let switches = switches1 - switches0;
    let cpu_us = (cpu1 - cpu0) / 1000;
    println!("idle PollThread over 1 s: {switches} context switches, {cpu_us} us on CPU");
    // Lion parks an idle runtime for up to 100 ms, so about 10 wakeups per
    // second; the retired poll loop woke on a 1 ms epoll timeout, about 1000.
    assert!(switches <= 60, "an idle PollThread woke {switches} times in 1 s");
    assert!(cpu_us < 20_000, "an idle PollThread used {cpu_us} us of CPU in 1 s");
    drop(connected);
    pt.shutdown();
}

#[test]
fn a_foreign_job_wakes_the_idle_driver_promptly() {
    let _serial = serial();
    let pt = PollThread::create();
    run_on(&pt, || ());
    let mut samples = Vec::new();
    for _ in 0..200 {
        // Let the driver go back to sleep, so each sample is an idle wake.
        std::thread::sleep(Duration::from_millis(2));
        let (tx, rx) = mpsc::channel();
        let start = Instant::now();
        pt.add(job(move || tx.send(Instant::now()).unwrap()));
        let ran = rx.recv_timeout(LIMIT).expect("a queued job did not wake the driver");
        samples.push(ran - start);
    }
    let p50 = summarize("foreign Job -> runs on the poll thread", samples);
    assert!(p50 < Duration::from_millis(1), "median command wake {p50:?}");
    pt.shutdown();
}

// Job::Ready has no wake: a job that waits is re-checked by the driver every
// millisecond, the retired loop's rate, and only while it waits.
#[test]
fn a_waiting_job_is_rechecked_until_it_becomes_ready() {
    let _serial = serial();
    struct Gated {
        ready: Arc<AtomicBool>,
        ran: mpsc::Sender<Instant>,
    }
    unsafe impl Job for Gated {
        fn Ready(&mut self) -> bool {
            self.ready.load(Ordering::Acquire)
        }
        fn Work(&mut self) {
            self.ran.send(Instant::now()).unwrap();
        }
        fn Done(&mut self) -> bool {
            false
        }
    }
    let pt = PollThread::create();
    let ready = Arc::new(AtomicBool::new(false));
    let (tx, rx) = mpsc::channel();
    pt.add(Arc::new(Gated { ready: ready.clone(), ran: tx }));
    assert!(rx.recv_timeout(Duration::from_millis(50)).is_err(), "a job ran before it was ready");
    let start = Instant::now();
    ready.store(true, Ordering::Release);
    let ran = rx.recv_timeout(LIMIT).expect("a job that became ready never ran");
    println!("waiting job: ran {:?} after it became ready", ran - start);
    pt.shutdown();
}

// Jobs run in the order they were submitted, whatever their addresses: a
// batch queued behind a blocked job is submitted in the reverse of its
// allocation order, so an address-ordered queue would run it backwards.
#[test]
fn jobs_run_in_submission_order() {
    let _serial = serial();
    const JOBS: usize = 64;
    let pt = PollThread::create();
    let (entered_tx, entered_rx) = mpsc::channel();
    let release = Arc::new(std::sync::Barrier::new(2));
    let blocker_release = release.clone();
    pt.add(job(move || {
        entered_tx.send(()).unwrap();
        blocker_release.wait();
    }));
    entered_rx.recv_timeout(LIMIT).unwrap();
    let order = Arc::new(Mutex::new(Vec::new()));
    let jobs: Vec<Arc<dyn Job>> = (0..JOBS)
        .map(|i| {
            let order = order.clone();
            job(move || order.lock().unwrap().push(i))
        })
        .collect();
    for queued in jobs.iter().rev() {
        pt.add(queued.clone());
    }
    let (done_tx, done_rx) = mpsc::channel();
    pt.add(job(move || done_tx.send(()).unwrap()));
    release.wait();
    done_rx.recv_timeout(LIMIT).unwrap();
    let ran = order.lock().unwrap().clone();
    let submitted: Vec<usize> = (0..JOBS).rev().collect();
    assert_eq!(ran, submitted, "jobs did not run in submission order");
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Stackless tasks on Lion

// Pending until `target` passes what it last saw; reports each step.
struct Stepper {
    target: Arc<AtomicU64>,
    seen: u64,
    last: u64,
    waker: Arc<Mutex<Option<Waker>>>,
    steps: mpsc::Sender<(u64, Instant, Option<u64>)>,
}

impl Future for Stepper {
    type Output = u64;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<u64> {
        *self.waker.lock().unwrap() = Some(cx.waker().clone());
        let target = self.target.load(Ordering::Acquire);
        if target > self.seen {
            self.seen = target;
            self.steps.send((target, Instant::now(), current_lion_task())).unwrap();
            if target == self.last {
                return Poll::Ready(target);
            }
        }
        Poll::Pending
    }
}

#[test]
fn a_stackless_task_runs_on_lion_and_a_foreign_wake_resumes_it_promptly() {
    let _serial = serial();
    const STEPS: u64 = 200;
    let pt = PollThread::create();
    let target = Arc::new(AtomicU64::new(0));
    let waker: Arc<Mutex<Option<Waker>>> = Arc::new(Mutex::new(None));
    let (steps_tx, steps_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let stepper = Stepper {
        target: target.clone(),
        seen: 0,
        last: STEPS,
        waker: waker.clone(),
        steps: steps_tx,
    };
    let spawner_task = run_on(&pt, move || {
        let reactor = Reactor::get_reactor();
        reactor_spawn_stackless_task_with_result(&reactor, Box::pin(stepper), move |value: u64| {
            done_tx.send((value, std::thread::current().id())).unwrap();
        });
        current_lion_task()
    });
    // The spawning job ran inside the driver, itself a Lion task.
    assert!(spawner_task.is_some(), "the driver is not a Lion task");
    let mut samples = Vec::new();
    let mut task_ids = Vec::new();
    for step in 1..=STEPS {
        std::thread::sleep(Duration::from_millis(1));
        let w = waker.lock().unwrap().clone().expect("the task published no waker");
        target.store(step, Ordering::Release);
        let start = Instant::now();
        w.wake();
        let (seen, at, task) = steps_rx.recv_timeout(LIMIT).expect("a foreign wake did not resume the task");
        assert_eq!(seen, step);
        samples.push(at - start);
        task_ids.push(task);
    }
    let (value, thread) = done_rx.recv_timeout(LIMIT).unwrap();
    assert_eq!(value, STEPS);
    assert_ne!(thread, std::thread::current().id());
    // Every resumed poll ran inside one Lion task, which is not the driver:
    // the task was spawned onto Lion, not onto the Reactor's own executor
    // (whose polls would run inside the driver's).
    let first = task_ids[0];
    assert!(first.is_some(), "a resumed poll ran outside any Lion task");
    assert!(task_ids.iter().all(|id| *id == first), "the task moved between Lion tasks");
    assert_ne!(first, spawner_task, "the stackless task was polled by the driver, not as its own Lion task");
    let p50 = summarize("foreign stackless wake -> task resumed", samples);
    assert!(p50 < Duration::from_millis(1), "median stackless wake {p50:?}");
    pt.shutdown();
}

#[test]
fn a_stackless_task_that_completes_at_once_delivers_inline() {
    let _serial = serial();
    let pt = PollThread::create();
    let delivered = run_on(&pt, || {
        let reactor = Reactor::get_reactor();
        let got = Arc::new(Mutex::new(None));
        let sink = got.clone();
        reactor_spawn_stackless_task_with_result(&reactor, Box::pin(async { 21i64 * 2 }), move |v: i64| {
            *sink.lock().unwrap() = Some(v);
        });
        let inline = *got.lock().unwrap();
        inline
    });
    assert_eq!(delivered, Some(42), "a ready task must complete inside the spawn call");
    pt.shutdown();
}

// A future that keeps only the waker of its first poll -- the one inside the
// spawn call, before the task is on Lion -- is still woken on Lion through it.
struct FirstWakerOnly {
    ready: Arc<AtomicBool>,
    published: Option<mpsc::Sender<Waker>>,
}

impl Future for FirstWakerOnly {
    type Output = Option<u64>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<u64>> {
        if let Some(publish) = self.published.take() {
            publish.send(cx.waker().clone()).unwrap();
        }
        if self.ready.load(Ordering::Acquire) {
            return Poll::Ready(current_lion_task());
        }
        Poll::Pending
    }
}

#[test]
fn the_waker_of_the_inline_first_poll_reaches_the_lion_task() {
    let _serial = serial();
    let pt = PollThread::create();
    let ready = Arc::new(AtomicBool::new(false));
    let (waker_tx, waker_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let future = FirstWakerOnly { ready: ready.clone(), published: Some(waker_tx) };
    let spawner = run_on(&pt, move || {
        reactor_spawn_stackless_task_with_result(&Reactor::get_reactor(), Box::pin(future), move |task: Option<u64>| {
            let _ = done_tx.send(task);
        });
        current_lion_task()
    });
    let first_waker = waker_rx.recv_timeout(LIMIT).unwrap();
    // Let the task take its first Lion poll, which does not publish.
    std::thread::sleep(Duration::from_millis(20));
    assert!(done_rx.try_recv().is_err());
    ready.store(true, Ordering::Release);
    first_waker.wake();
    let task = done_rx.recv_timeout(LIMIT).expect("the first poll's waker did not reach the Lion task");
    assert!(task.is_some() && task != spawner, "completed outside its own Lion task");
    pt.shutdown();
}

// A poller registered directly with the Reactor on a PollThread runs on the
// Reactor's own executor, whose wake ingress wakes the driver.
#[test]
fn a_directly_registered_stackless_poller_is_woken_through_the_driver() {
    let _serial = serial();
    let pt = PollThread::create();
    let ready = Arc::new(AtomicBool::new(false));
    let waker: Arc<Mutex<Option<Waker>>> = Arc::new(Mutex::new(None));
    let (done_tx, done_rx) = mpsc::channel();
    let (poll_ready, poll_waker) = (ready.clone(), waker.clone());
    run_on(&pt, move || {
        let reactor = Reactor::get_reactor();
        let poller: StacklessPollFn = Some(Box::new(move |cx: &mut Context<'_>| -> bool {
            *poll_waker.lock().unwrap() = Some(cx.waker().clone());
            if poll_ready.load(Ordering::Acquire) {
                done_tx.send(Instant::now()).unwrap();
                return true;
            }
            false
        }));
        let idx = reactor.register_stackless_poller(poller);
        reactor.enqueue_stackless_task(idx);
    });
    let deadline = Instant::now() + LIMIT;
    let w = loop {
        if let Some(w) = waker.lock().unwrap().clone() {
            break w;
        }
        assert!(Instant::now() < deadline, "the poller was never polled");
        std::thread::sleep(Duration::from_millis(1));
    };
    std::thread::sleep(Duration::from_millis(5));
    ready.store(true, Ordering::Release);
    let start = Instant::now();
    w.wake();
    let done = done_rx.recv_timeout(LIMIT).expect("a wake queued on the stackless ingress did not wake the driver");
    println!("direct poller: resumed {:?} after a foreign wake", done - start);
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Events and fibers woken from outside the driver

// A fiber waits on an event that a Lion task (not the driver) sets: the
// WAIT->READY edge queues it on the empty ready queue, which wakes the driver.
#[test]
fn an_event_set_from_a_lion_task_resumes_its_waiting_fiber() {
    let _serial = serial();
    let pt = PollThread::create();
    let target = Arc::new(AtomicU64::new(0));
    let waker: Arc<Mutex<Option<Waker>>> = Arc::new(Mutex::new(None));
    let (resumed_tx, resumed_rx) = mpsc::channel();
    let (task_target, task_waker) = (target.clone(), waker.clone());
    run_on(&pt, move || {
        let event = create_sp_int_event(1);
        let waited = event.clone();
        Fiber::create_run(move || {
            waited.wait();
            resumed_tx.send(Instant::now()).unwrap();
        });
        // Sets the event once woken; runs as its own Lion task by then.
        struct SetOnWake {
            target: Arc<AtomicU64>,
            waker: Arc<Mutex<Option<Waker>>>,
            event: Arc<srpc::reactor::IntEvent>,
        }
        impl Future for SetOnWake {
            type Output = ();
            fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
                *self.waker.lock().unwrap() = Some(cx.waker().clone());
                if self.target.load(Ordering::Acquire) == 0 {
                    return Poll::Pending;
                }
                self.event.set(1);
                Poll::Ready(())
            }
        }
        let task = SetOnWake { target: task_target, waker: task_waker, event };
        reactor_spawn_stackless_task_impl(&Reactor::get_reactor(), Box::pin(task));
    });
    std::thread::sleep(Duration::from_millis(5));
    let w = waker.lock().unwrap().clone().unwrap();
    target.store(1, Ordering::Release);
    let start = Instant::now();
    w.wake();
    let resumed = resumed_rx.recv_timeout(LIMIT).expect("the ready queue's edge did not wake the driver");
    println!("event set by a Lion task: fiber resumed {:?} after the wake", resumed - start);
    pt.shutdown();
}

// A fiber started outside the driver (here from a Lion task) sleeps: its
// deadline is earlier than anything the idle driver sleeps until, so pushing
// it wakes the driver to arm its timer.
#[test]
fn a_sleep_started_outside_the_driver_arms_its_timer() {
    let _serial = serial();
    let pt = PollThread::create();
    let target = Arc::new(AtomicU64::new(0));
    let waker: Arc<Mutex<Option<Waker>>> = Arc::new(Mutex::new(None));
    let (slept_tx, slept_rx) = mpsc::channel();
    let (task_target, task_waker) = (target.clone(), waker.clone());
    run_on(&pt, move || {
        struct SleepOnWake {
            target: Arc<AtomicU64>,
            waker: Arc<Mutex<Option<Waker>>>,
            slept: Option<mpsc::Sender<Duration>>,
        }
        impl Future for SleepOnWake {
            type Output = ();
            fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
                *self.waker.lock().unwrap() = Some(cx.waker().clone());
                if self.target.load(Ordering::Acquire) == 0 {
                    return Poll::Pending;
                }
                let slept = self.slept.take().unwrap();
                Fiber::create_run(move || {
                    let start = Instant::now();
                    srpc::fiber::this_fiber::sleep_ms(20);
                    slept.send(start.elapsed()).unwrap();
                });
                Poll::Ready(())
            }
        }
        let task = SleepOnWake { target: task_target, waker: task_waker, slept: Some(slept_tx) };
        reactor_spawn_stackless_task_impl(&Reactor::get_reactor(), Box::pin(task));
    });
    std::thread::sleep(Duration::from_millis(5));
    let w = waker.lock().unwrap().clone().unwrap();
    target.store(1, Ordering::Release);
    w.wake();
    let slept = slept_rx.recv_timeout(LIMIT).expect("a deadline pushed outside the driver never fired");
    println!("sleep_ms(20) started in a Lion task took {slept:?}");
    assert!(slept >= Duration::from_millis(20));
    pt.shutdown();
}

#[test]
fn fiber_sleeps_on_a_poll_thread_meet_their_deadlines() {
    let _serial = serial();
    let pt = PollThread::create();
    let (tx, rx) = mpsc::channel();
    pt.add(job(move || {
        Fiber::create_run(move || {
            let mut results: Vec<(u64, Duration)> = Vec::new();
            for _round in 0..8 {
                for us in [300u64, 1_000, 2_500, 5_000, 10_000, 20_000] {
                    let start = Instant::now();
                    srpc::fiber::this_fiber::sleep_us(us);
                    results.push((us, start.elapsed()));
                }
            }
            tx.send(results).unwrap();
        });
    }));
    let results = rx.recv_timeout(Duration::from_secs(30)).expect("the sleeping fiber never finished");
    let mut lateness = Vec::new();
    for (us, elapsed) in results.iter() {
        let requested = Duration::from_micros(*us);
        assert!(*elapsed >= requested, "a {requested:?} sleep ended early, after {elapsed:?}");
        lateness.push(*elapsed - requested);
    }
    let p50 = summarize("fiber sleep lateness on a PollThread", lateness);
    assert!(p50 < Duration::from_millis(3), "median sleep lateness {p50:?}");
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// FiberChannel: a frame published on another thread pings the owner

struct StubState {
    closed: bool,
    on_frame: OnFrameCallback,
    on_closed: OnClosedCallback,
    on_error: OnErrorCallback,
}

#[derive(Clone)]
struct StubHandle(Arc<Mutex<StubState>>);

impl StubHandle {
    fn deliver(&self, payload: &[u8]) {
        let callback = self.0.lock().unwrap().on_frame.clone();
        if callback.has_value() {
            (callback.callable())(&ChannelFrame { payload: payload.as_ptr(), size: payload.len() });
        }
    }
}

struct StubConnection(StubHandle);

impl ChannelConnectionBase for StubConnection {
    unsafe fn send_frame(&self, _frame: &ChannelFrame) -> ChannelError {
        ChannelError::None
    }
    fn flush(&self) {}
    fn close(&self) {
        self.0 .0.lock().unwrap().closed = true;
    }
    fn is_closed(&self) -> bool {
        self.0 .0.lock().unwrap().closed
    }
    fn peer_address(&self) -> String {
        "stub".to_owned()
    }
    fn set_on_frame(&mut self, callback: OnFrameCallback) {
        self.0 .0.lock().unwrap().on_frame = callback;
    }
    fn set_on_closed(&mut self, callback: OnClosedCallback) {
        self.0 .0.lock().unwrap().on_closed = callback;
    }
    fn set_on_error(&mut self, callback: OnErrorCallback) {
        self.0 .0.lock().unwrap().on_error = callback;
    }
}

#[test]
fn a_foreign_fiber_channel_frame_resumes_its_receiver_promptly() {
    let _serial = serial();
    const FRAMES: usize = 200;
    let pt = PollThread::create();
    let handle = StubHandle(Arc::new(Mutex::new(StubState {
        closed: false,
        on_frame: OnFrameCallback::default(),
        on_closed: OnClosedCallback::default(),
        on_error: OnErrorCallback::default(),
    })));
    let (received_tx, received_rx) = mpsc::channel();
    let stub = handle.clone();
    run_on(&pt, move || {
        let proxy: ChannelConnectionProxy = Box::new(StubConnection(stub));
        let mut wrapper = Box::pin(FiberChannel::new(proxy));
        unsafe { wrapper.as_mut().get_unchecked_mut() }.bind_callbacks();
        Fiber::create_run(move || {
            for _ in 0..FRAMES {
                let frame = wrapper.as_ref().get_ref().recv_frame().unwrap();
                received_tx.send((frame.bytes[0], Instant::now())).unwrap();
            }
        });
    });
    let mut samples = Vec::new();
    for i in 0..FRAMES {
        std::thread::sleep(Duration::from_millis(1));
        let start = Instant::now();
        handle.deliver(&[i as u8]);
        let (byte, at) = received_rx.recv_timeout(LIMIT).expect("a foreign frame's ping did not wake the driver");
        assert_eq!(byte, i as u8);
        samples.push(at - start);
    }
    let p50 = summarize("foreign FiberChannel frame -> receiver resumed", samples);
    assert!(p50 < Duration::from_millis(1), "median FiberChannel wake {p50:?}");
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// TCP through the transport tasks (the pollable adapter until S5)

type ProxySlot = Arc<Mutex<Option<ChannelConnectionProxy>>>;

fn frame_payload(frame: &ChannelFrame) -> Vec<u8> {
    if frame.size == 0 {
        return Vec::new();
    }
    unsafe { std::slice::from_raw_parts(frame.payload, frame.size) }.to_vec()
}

fn pattern(seed: usize, len: usize) -> Vec<u8> {
    (0..len).map(|i| (i.wrapping_mul(31) ^ seed.wrapping_mul(131) ^ (i >> 9)) as u8).collect()
}

#[test]
fn tcp_frames_echo_through_the_transport_tasks() {
    let _serial = serial();
    let server_pt = PollThread::create();
    let client_pt = PollThread::create();

    // Server: echo every frame back on the connection that carried it,
    // from inside the reader task.
    let accepted: Arc<Mutex<Vec<ProxySlot>>> = Arc::new(Mutex::new(Vec::new()));
    let (server_closed_tx, server_closed_rx) = mpsc::channel();
    let server_closed_tx = Mutex::new(server_closed_tx);
    let accepted_sink = accepted.clone();
    let factory = TcpFactory::new(server_pt.clone());
    let mut listener = factory.make_listener().unwrap();
    let on_accept: OnAcceptCallback = CallbackWrapper::from_callable(Box::new(
        move |connection: NullableChannelConnectionProxy| {
            let mut proxy = connection.unwrap();
            let slot: ProxySlot = Arc::new(Mutex::new(None));
            let echo = slot.clone();
            proxy.set_on_frame(OnFrameCallback::from_callable(Box::new(move |frame: &ChannelFrame| {
                let guard = echo.lock().unwrap();
                let result = unsafe { guard.as_ref().unwrap().send_frame(frame) };
                assert_eq!(result, ChannelError::None, "the echo was refused");
            })));
            let closed = server_closed_tx.lock().unwrap().clone();
            proxy.set_on_closed(OnClosedCallback::from_callable(Box::new(move |_reason: ChannelError| {
                let _ = closed.send(());
            })));
            *slot.lock().unwrap() = Some(proxy);
            accepted_sink.lock().unwrap().push(slot);
        },
    ));
    listener.set_on_accept(on_accept);
    assert_eq!(listener.listen("127.0.0.1:0"), ChannelError::None);
    let address = listener.local_address();

    // Client: frames sent from this thread, echoes collected in order.
    let client_factory = TcpFactory::new(client_pt.clone());
    let connected = client_factory.connect(&address);
    assert_eq!(connected.error, ChannelError::None);
    let mut client = connected.connection.unwrap();
    let (echo_tx, echo_rx) = mpsc::channel::<Vec<u8>>();
    let echo_tx = Mutex::new(echo_tx);
    client.set_on_frame(OnFrameCallback::from_callable(Box::new(move |frame: &ChannelFrame| {
        echo_tx.lock().unwrap().send(frame_payload(frame)).unwrap();
    })));

    // Sizes from empty to 1 MiB: the large ones fill the socket buffers, so
    // both sides' writers stop on EAGAIN and resume on the next write edge.
    let sizes = [0usize, 1, 7, 4096, 65_536, 1 << 20, 300_000, 3, 1 << 20, 2 << 20, 17];
    let window = 4usize;
    let mut sent = 0usize;
    let mut received = 0usize;
    let mut expected = std::collections::VecDeque::new();
    let start = Instant::now();
    while received < sizes.len() {
        while sent < sizes.len() && sent - received < window {
            let payload = pattern(sent, sizes[sent]);
            let frame = ChannelFrame { payload: payload.as_ptr(), size: payload.len() };
            assert_eq!(unsafe { client.send_frame(&frame) }, ChannelError::None);
            expected.push_back(payload);
            sent += 1;
        }
        let echoed = echo_rx.recv_timeout(LIMIT).expect("an echo did not arrive (a lost write or read wake?)");
        assert_eq!(echoed, expected.pop_front().unwrap(), "echo {received} differs");
        received += 1;
    }
    println!("TCP echo of {} frames ({} bytes) took {:?}", sizes.len(), sizes.iter().sum::<usize>(), start.elapsed());

    // Many small frames from another thread, pipelined.
    let mut many = 0usize;
    for i in 0..2_000usize {
        let payload = pattern(i, i % 97);
        let frame = ChannelFrame { payload: payload.as_ptr(), size: payload.len() };
        assert_eq!(unsafe { client.send_frame(&frame) }, ChannelError::None);
        expected.push_back(payload);
    }
    while let Some(want) = expected.pop_front() {
        assert_eq!(echo_rx.recv_timeout(LIMIT).expect("a pipelined echo was lost"), want);
        many += 1;
    }
    assert_eq!(many, 2_000);

    // Closing the client from this thread shuts its socket down; the server's
    // reader task reads EOF and retires the transport.
    client.close();
    server_closed_rx.recv_timeout(LIMIT).expect("the server never saw the client close");
    listener.close();
    drop(listener);
    for slot in accepted.lock().unwrap().iter() {
        if let Some(proxy) = slot.lock().unwrap().take() {
            proxy.close();
        }
    }
    drop(client);
    client_pt.shutdown();
    server_pt.shutdown();
}

// A peer that stops reading fills the socket buffers: the writer's
// transport task must then wait for the next write edge, not spin on EAGAIN,
// and must resume when the peer drains.
#[test]
fn a_stalled_writer_waits_for_the_write_edge_and_resumes() {
    let _serial = serial();
    use std::io::Read;
    let pt = PollThread::create();
    let peer = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let connected = TcpFactory::new(pt.clone()).connect(&peer.local_addr().unwrap().to_string());
    assert_eq!(connected.error, ChannelError::None);
    let (mut peer_stream, _) = peer.accept().unwrap();
    let client = connected.connection.unwrap();
    // Queue far more than the socket buffers hold (the outbound high-water
    // mark is 4 MiB, so stay under it).
    let payload = pattern(7, 1 << 20);
    let mut expected_bytes = 0usize;
    for _ in 0..3 {
        let frame = ChannelFrame { payload: payload.as_ptr(), size: payload.len() };
        assert_eq!(unsafe { client.send_frame(&frame) }, ChannelError::None);
        expected_bytes += 4 + payload.len();
    }
    // Wait until the transport task has written what the kernel accepts.
    std::thread::sleep(Duration::from_millis(100));
    let tid = pt.poll_thread_id_bits_.load(Ordering::Acquire);
    let (cpu0, _) = thread_cpu_and_switches(tid);
    std::thread::sleep(Duration::from_millis(300));
    let (cpu1, _) = thread_cpu_and_switches(tid);
    let stalled_us = (cpu1 - cpu0) / 1000;
    println!("stalled writer: {stalled_us} us of poll-thread CPU in 300 ms");
    assert!(stalled_us < 30_000, "a writer blocked on a full socket spun for {stalled_us} us in 300 ms");
    // Drain the peer: every byte must arrive.
    peer_stream.set_read_timeout(Some(LIMIT)).unwrap();
    let mut got = vec![0u8; expected_bytes];
    peer_stream.read_exact(&mut got).expect("the stalled writer never resumed");
    assert_eq!(&got[4..4 + payload.len()], &payload[..]);
    drop(client);
    pt.shutdown();
}

// The pollable adapter (S3's PollFdTask) no longer carries TCP, but
// PollThread::add_proxy still takes any pollable. A socketpair end: its
// read edge reaches handle_read, and request_close retires and closes it.
struct SocketPollable {
    stream: std::os::unix::net::UnixStream,
    seen: Arc<Mutex<Vec<u8>>>,
    closed: Arc<AtomicBool>,
}

impl srpc::pollable_proxy::PollableBase for SocketPollable {
    fn fd(&self) -> i32 {
        use std::os::fd::AsRawFd;
        self.stream.as_raw_fd()
    }
    fn poll_mode(&self) -> i32 {
        1
    }
    fn content_size(&mut self) -> usize {
        0
    }
    fn handle_read(&mut self) -> bool {
        use std::io::Read;
        let mut buffer = [0u8; 256];
        let mut any = false;
        while let Ok(n) = self.stream.read(&mut buffer) {
            if n == 0 {
                break;
            }
            self.seen.lock().unwrap().extend_from_slice(&buffer[..n]);
            any = true;
        }
        any
    }
    fn handle_write(&mut self) -> i32 {
        -1
    }
    fn handle_error(&mut self) {}
    fn close(&mut self) {
        self.closed.store(true, Ordering::Release);
    }
    fn check_pending_write_update(&self) -> bool {
        false
    }
    fn is_closed(&self) -> bool {
        self.closed.load(Ordering::Acquire)
    }
}

#[test]
fn a_custom_pollable_still_runs_through_the_adapter() {
    let _serial = serial();
    use std::io::{Read, Write};
    use std::os::fd::AsRawFd;
    let pt = PollThread::create();
    let (local, mut peer) = std::os::unix::net::UnixStream::pair().unwrap();
    local.set_nonblocking(true).unwrap();
    let fd = local.as_raw_fd();
    let seen = Arc::new(Mutex::new(Vec::new()));
    let closed = Arc::new(AtomicBool::new(false));
    pt.add_proxy(Box::new(SocketPollable { stream: local, seen: seen.clone(), closed: closed.clone() }));
    for chunk in [&b"adapter "[..], &b"still "[..], &b"reads"[..]] {
        peer.write_all(chunk).unwrap();
        std::thread::sleep(Duration::from_millis(5));
    }
    let start = Instant::now();
    while seen.lock().unwrap().as_slice() != b"adapter still reads" {
        assert!(start.elapsed() < LIMIT, "the adapter never delivered the read edges: {:?}", seen.lock().unwrap());
        std::thread::sleep(Duration::from_millis(2));
    }
    pt.request_close(fd);
    let start = Instant::now();
    while !closed.load(Ordering::Acquire) {
        assert!(start.elapsed() < LIMIT, "request_close never closed the pollable");
        std::thread::sleep(Duration::from_millis(2));
    }
    // Retirement dropped the pollable, and with it the socket.
    peer.set_read_timeout(Some(LIMIT)).unwrap();
    let mut rest = Vec::new();
    assert_eq!(peer.read_to_end(&mut rest).unwrap(), 0);
    pt.shutdown();
}

// ---------------------------------------------------------------------------
// Shutdown

struct DropFlag(Arc<AtomicBool>);

impl Drop for DropFlag {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Release);
    }
}

struct Forever(#[allow(dead_code)] DropFlag);

impl Future for Forever {
    type Output = ();
    fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<()> {
        Poll::Pending
    }
}

#[test]
fn shutdown_with_live_fibers_tasks_and_descriptors_releases_them() {
    let _serial = serial();
    // Settle any descriptor a previous test's threads are still closing.
    std::thread::sleep(Duration::from_millis(50));
    let fds_before = open_fds();
    let cancelled_before = stackless_cancel_report::<()>().teardown_tasks;
    let task_dropped = Arc::new(AtomicBool::new(false));
    let callback_dropped = Arc::new(AtomicBool::new(false));
    {
        let peer = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = peer.local_addr().unwrap().to_string();
        let pt = PollThread::create();
        // A registered connection, whose only remaining owner is its
        // registration once the proxy below is dropped.
        let connected = TcpFactory::new(pt.clone()).connect(&address);
        assert_eq!(connected.error, ChannelError::None);
        let (peer_stream, _) = peer.accept().unwrap();
        drop(connected.connection);
        let flag = task_dropped.clone();
        let callback_flag = callback_dropped.clone();
        run_on(&pt, move || {
            // A fiber on an event nobody sets, and one on a long sleep.
            let event = create_sp_int_event(1);
            Fiber::create_run(move || event.wait());
            Fiber::create_run(|| srpc::fiber::this_fiber::sleep_ms(60_000));
            // Stackless tasks that never complete, with and without a
            // completion callback; the callback's capture must be released.
            reactor_spawn_stackless_task_impl(&Reactor::get_reactor(), Box::pin(Forever(DropFlag(flag))));
            let callback_capture = DropFlag(callback_flag);
            reactor_spawn_stackless_task_with_result(
                &Reactor::get_reactor(),
                Box::pin(Forever(DropFlag(Arc::new(AtomicBool::new(false))))),
                move |_: ()| {
                    let _keep = &callback_capture;
                },
            );
        });
        // And a job that is never ready, which keeps the 1 ms re-check armed.
        struct Never;
        unsafe impl Job for Never {
            fn Ready(&mut self) -> bool {
                false
            }
            fn Work(&mut self) {}
            fn Done(&mut self) -> bool {
                false
            }
        }
        pt.add(Arc::new(Never));
        std::thread::sleep(Duration::from_millis(20));
        let start = Instant::now();
        pt.shutdown();
        let took = start.elapsed();
        println!("shutdown with live fibers, a task, a job and a descriptor took {took:?}");
        assert!(took < Duration::from_secs(2), "shutdown took {took:?}");
        drop(pt);
        drop(peer_stream);
    }
    assert!(task_dropped.load(Ordering::Acquire), "the pending stackless task was not dropped at shutdown");
    assert!(callback_dropped.load(Ordering::Acquire), "a completion callback outlived its PollThread");
    assert_eq!(
        stackless_cancel_report::<()>().teardown_tasks,
        cancelled_before + 2,
        "the dropped tasks were not reported as cancelled"
    );
    // The runtime's epoll and eventfd descriptors, and the registered socket,
    // are all closed once the thread is joined.
    assert_eq!(open_fds(), fds_before, "descriptors leaked across a PollThread's life");
}

#[test]
fn poll_threads_come_and_go_without_leaking_descriptors() {
    let _serial = serial();
    std::thread::sleep(Duration::from_millis(50));
    let fds_before = open_fds();
    for _ in 0..50 {
        let pt = PollThread::create();
        run_on(&pt, || ());
        pt.shutdown();
    }
    assert_eq!(open_fds(), fds_before);
}
