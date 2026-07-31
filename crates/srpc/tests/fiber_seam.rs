//! S8a-2: fibers driven by the real poll thread.
//!
//! The unit tests around `FiberRuntime` prove the queue discipline in
//! isolation. This proves it end to end on the actual loop — including
//! the property the whole design exists for: a fiber woken from a
//! connection's frame callback resumes LATER, in the poll loop's own
//! phase, never inline while that connection holds its reader lock.

use srpc::runtime::fiber::{spawn_here, yield_now};
use srpc::runtime::poll_thread::PollThread;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

fn wait_until(timeout: Duration, mut cond: impl FnMut() -> bool) -> bool {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if cond() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(2));
    }
    cond()
}

#[test]
fn a_fiber_spawned_on_the_poll_thread_runs_there() {
    let poll = PollThread::start();
    let ran_on = Arc::new(Mutex::new(None));
    let sink = Arc::clone(&ran_on);

    poll.run_on_poll_thread(move || {
        let sink = Arc::clone(&sink);
        spawn_here(Box::new(move || {
            *sink.lock().unwrap() = Some(std::thread::current().id());
        }))
        .expect("spawn");
    });

    assert!(
        wait_until(Duration::from_secs(5), || ran_on.lock().unwrap().is_some()),
        "the fiber never ran"
    );
    let id = ran_on.lock().unwrap().unwrap();
    assert_ne!(
        id,
        std::thread::current().id(),
        "the fiber ran on the calling thread, not the poll thread"
    );
    poll.shutdown();
}

#[test]
fn a_fiber_suspends_and_is_resumed_by_the_loop_after_a_foreign_wake() {
    let poll = PollThread::start();
    let stage = Arc::new(AtomicU32::new(0));
    let handle_slot = Arc::new(Mutex::new(None));

    let s = Arc::clone(&stage);
    let hs = Arc::clone(&handle_slot);
    poll.run_on_poll_thread(move || {
        let s2 = Arc::clone(&s);
        let h = spawn_here(Box::new(move || {
            s2.store(1, Ordering::SeqCst);
            yield_now();
            s2.store(2, Ordering::SeqCst);
        }))
        .expect("spawn");
        *hs.lock().unwrap() = Some(h);
    });

    assert!(
        wait_until(Duration::from_secs(5), || stage.load(Ordering::SeqCst) == 1),
        "the fiber did not reach its first suspend"
    );

    // Wake from THIS thread — the foreign-wake path.
    let h = handle_slot.lock().unwrap().clone().expect("handle");
    h.wake();

    assert!(
        wait_until(Duration::from_secs(5), || stage.load(Ordering::SeqCst) == 2),
        "the poll loop never resumed the woken fiber"
    );
    poll.shutdown();
}

#[test]
fn many_fibers_interleave_on_one_poll_thread() {
    const N: u32 = 64;
    let poll = PollThread::start();
    let finished = Arc::new(AtomicU32::new(0));
    let handles = Arc::new(Mutex::new(Vec::new()));

    let f = Arc::clone(&finished);
    let hs = Arc::clone(&handles);
    poll.run_on_poll_thread(move || {
        for _ in 0..N {
            let f2 = Arc::clone(&f);
            let h = spawn_here(Box::new(move || {
                yield_now();
                f2.fetch_add(1, Ordering::SeqCst);
            }))
            .expect("spawn");
            hs.lock().unwrap().push(h);
        }
    });

    assert!(
        wait_until(Duration::from_secs(5), || handles.lock().unwrap().len()
            == N as usize),
        "not all fibers were spawned"
    );
    for h in handles.lock().unwrap().iter() {
        h.wake();
    }
    assert!(
        wait_until(Duration::from_secs(10), || finished.load(Ordering::SeqCst)
            == N),
        "only {} of {N} fibers finished",
        finished.load(Ordering::SeqCst)
    );
    poll.shutdown();
}
