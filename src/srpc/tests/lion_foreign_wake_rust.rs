// Cross-thread wakes of a Lion runtime parked in SRPC's OS backend (plan item
// S1). A waker woken, or a task spawned, from another thread goes through
// Lion's cross-thread queue and then `OsInterrupt::signal`, which
// `impl lion_reactor::os::OsInterrupt for SrpcEpollInterrupt`
// (reactor/epoll_wrapper.rs) forwards to SRPC's eventfd; the backend's wait
// sees the eventfd and returns. Without that signal the wake would surface
// only when the park times out: after up to 100 ms under `block_on`, and
// never within the bound under a long `tick_with_timeout`.
//
// Kept in its own test binary, as Lion keeps its own foreign-wake test, so no
// other test's threads compete for the CPU while the latency is measured.

use lion_executor::{spawn, Runtime, RuntimeBuilder};
use srpc::epoll_wrapper::SrpcEpollBackend;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};
use std::time::{Duration, Instant};

fn srpc_runtime() -> Runtime {
    let backend = SrpcEpollBackend::new().expect("SRPC epoll backend");
    RuntimeBuilder::new().os_backend(Box::new(backend)).build().expect("Lion runtime over SRPC's backend")
}

#[derive(Default)]
struct Slot {
    woken: bool,
    waker: Option<Waker>,
}

// Pending until another thread sets `woken`; stores the task's waker for it.
struct WaitForSlot(Arc<Mutex<Slot>>);

impl Future for WaitForSlot {
    type Output = Instant;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Instant> {
        let mut slot = self.0.lock().unwrap();
        if slot.woken {
            Poll::Ready(Instant::now())
        } else {
            slot.waker = Some(cx.waker().clone());
            Poll::Pending
        }
    }
}

// Sets the slot and wakes its waiter from this thread after `delay`; returns
// when it woke it.
fn wake_later(slot: Arc<Mutex<Slot>>, delay: Duration) -> std::thread::JoinHandle<Instant> {
    std::thread::spawn(move || {
        std::thread::sleep(delay);
        let mut s = slot.lock().unwrap();
        s.woken = true;
        let waker = s.waker.take().expect("task parked without a waker");
        drop(s);
        let woke_at = Instant::now();
        waker.wake();
        woke_at
    })
}

// Under block_on the idle park is 100 ms; a wake that only surfaced when the
// park timed out would show tens of milliseconds of latency. The wakes land
// at spread points inside a park.
#[test]
fn foreign_thread_wake_interrupts_an_idle_park() {
    const TRIALS: u64 = 8;
    const LIMIT: Duration = Duration::from_millis(25);
    let rt = srpc_runtime();
    let mut latencies = Vec::new();
    for trial in 0..TRIALS {
        let latency = rt.block_on(async move {
            let slot = Arc::new(Mutex::new(Slot::default()));
            let task = spawn(WaitForSlot(slot.clone()));
            let waker_thread = wake_later(slot, Duration::from_millis(150 + 13 * trial));
            let polled_at = task.await.expect("woken task");
            let woke_at = waker_thread.join().unwrap();
            polled_at.saturating_duration_since(woke_at)
        });
        latencies.push(latency);
    }
    let max = latencies.iter().max().copied().unwrap();
    eprintln!("foreign wake -> poll latencies: {latencies:?}");
    assert!(
        max < LIMIT,
        "a foreign-thread wake took {max:?} to reach its task (limit {LIMIT:?}); the park was not interrupted"
    );
}

// An embedder's loop parked with a 10 s bound: only the interrupt can end the
// park early, so the wake must arrive long before the bound.
#[test]
fn foreign_wake_ends_a_long_bounded_park() {
    let rt = srpc_runtime();
    let slot = Arc::new(Mutex::new(Slot::default()));
    let done = Arc::new(AtomicBool::new(false));
    {
        let (slot, done) = (slot.clone(), done.clone());
        rt.handle().spawn_local(async move {
            WaitForSlot(slot).await;
            done.store(true, Ordering::SeqCst);
        });
    }
    // Let the task run once and store its waker.
    rt.tick_with_timeout(Duration::ZERO);
    let waker_thread = wake_later(slot, Duration::from_millis(50));
    let start = Instant::now();
    while !done.load(Ordering::SeqCst) {
        rt.tick_with_timeout(Duration::from_secs(10));
    }
    let woke_at = waker_thread.join().unwrap();
    let elapsed = start.elapsed();
    let latency = Instant::now().saturating_duration_since(woke_at);
    assert!(
        elapsed < Duration::from_secs(2),
        "the wake took {elapsed:?} to end a 10 s park (latency after the wake {latency:?})"
    );
}

// ExecutorHandle::spawn from another thread also interrupts the park.
#[test]
fn foreign_spawn_ends_a_long_bounded_park() {
    let rt = srpc_runtime();
    let handle = rt.handle().clone();
    let ran = Arc::new(AtomicBool::new(false));
    let ran2 = ran.clone();
    let spawner = std::thread::spawn(move || {
        std::thread::sleep(Duration::from_millis(50));
        handle.spawn(async move { ran2.store(true, Ordering::SeqCst) });
    });
    let start = Instant::now();
    while !ran.load(Ordering::SeqCst) {
        rt.tick_with_timeout(Duration::from_secs(10));
    }
    spawner.join().unwrap();
    let elapsed = start.elapsed();
    assert!(elapsed < Duration::from_secs(2), "a foreign spawn took {elapsed:?} to end a 10 s park");
}
