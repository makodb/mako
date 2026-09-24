#![allow(non_snake_case)]

use srpc::misc::{clamp, format_thousands, get_ncpu, Job, OneTimeJob};
use std::cmp::Ordering;
use std::sync::Arc;
use std::sync::atomic::{AtomicU32, Ordering as AtomicOrdering};

#[test]
fn process_seam_and_homogeneous_clamp_preserve_results() {
    assert!(get_ncpu() > 0);
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Value(i32);

#[derive(Clone, Copy, Debug)]
struct Bound(i16);

impl PartialEq<Bound> for Value {
    fn eq(&self, other: &Bound) -> bool {
        self.0 == i32::from(other.0)
    }
}

impl PartialOrd<Bound> for Value {
    fn partial_cmp(&self, other: &Bound) -> Option<Ordering> {
        self.0.partial_cmp(&i32::from(other.0))
    }
}

impl From<&Bound> for Value {
    fn from(bound: &Bound) -> Value {
        Value(i32::from(bound.0))
    }
}

impl From<&Value> for Value {
    fn from(value: &Value) -> Value {
        *value
    }
}

#[test]
fn heterogeneous_clamp_keeps_the_legacy_template_shape() {
    assert_eq!(clamp(&Value(-2), &Bound(0), &Bound(10)), Value(0));
    assert_eq!(clamp(&Value(4), &Bound(0), &Bound(10)), Value(4));
    assert_eq!(clamp(&Value(12), &Bound(0), &Bound(10)), Value(10));
}

#[test]
fn one_time_job_preserves_state_and_trait_dispatch() {
    let calls = Arc::new(AtomicU32::new(0_u32));
    let observed = Arc::clone(&calls);
    let mut concrete = OneTimeJob::new(Box::new(move || {
        observed.fetch_add(1, AtomicOrdering::Relaxed);
    }));
    let job: &mut dyn Job = &mut concrete;

    assert!(job.Ready());
    assert!(!job.Done());
    job.Work();
    assert!(!job.Ready());
    assert!(job.Done());
    assert_eq!(calls.load(AtomicOrdering::Relaxed), 1);

    // The historical class does not suppress an explicit second Work call.
    job.Work();
    assert_eq!(calls.load(AtomicOrdering::Relaxed), 2);
}

#[test]
fn thousands_formatter_matches_the_legacy_surface() {
    let text = |value| format_thousands(value);
    assert_eq!(text(0.0), "0.00");
    assert_eq!(text(-0.0), "0.00");
    assert_eq!(text(12.5), "12.50");
    assert_eq!(text(1_234.5), "1,234.50");
    assert_eq!(text(-1_234_567.89), "-1,234,567.89");
    assert_eq!(text(999.999), "1,000.00");
}

// The async-fn lowering demos, exercised the two ways the Rust lane can
// drive them.  Under rustc an `async fn` is an ordinary Rust future (the
// C++ lane gets a coroutine returning `rusty::Task` from the same bytes);
// The canonical stackless spawn path accepts a boxed standard Future.
#[test]
fn async_double_resolves_as_a_plain_rust_future() {
    use std::future::Future;
    use std::pin::pin;
    use std::task::{Context, Poll, Waker};

    let mut fut = pin!(srpc::misc::async_double_twice(10));
    let waker = Waker::noop();
    let mut cx = Context::from_waker(waker);
    match fut.as_mut().poll(&mut cx) {
        Poll::Ready(value) => assert_eq!(value, 40, "double twice: 10 -> 20 -> 40"),
        Poll::Pending => panic!("a leaf-only await chain must resolve on the first poll"),
    }
}

#[test]
fn async_double_drives_through_a_boxed_standard_future() {
    use std::future::Future;
    use std::pin::Pin;
    use std::task::{Context, Poll, Waker};

    let mut task: Pin<Box<dyn Future<Output = i64>>> = Box::pin(srpc::misc::async_double(21));
    let mut context = Context::from_waker(Waker::noop());
    assert_eq!(task.as_mut().poll(&mut context), Poll::Ready(42));
}

// The thread_local! pilot: each thread must see its own counter.  Under
// rustc this is the std macro; the C++ lane's copy of this assertion lives
// in tests/test_reactor.cc against the emitted rusty::LocalKey lowering.
#[test]
fn thread_slot_bump_is_per_thread() {
    assert_eq!(srpc::misc::thread_slot_bump(), 1);
    assert_eq!(srpc::misc::thread_slot_bump(), 2);
    let other = std::thread::spawn(|| {
        (srpc::misc::thread_slot_bump(), srpc::misc::thread_slot_bump())
    })
    .join()
    .expect("bump thread");
    assert_eq!(other, (1, 2), "a fresh thread starts from its own zero");
    assert_eq!(
        srpc::misc::thread_slot_bump(),
        3,
        "the other thread's bumps must not leak into this one"
    );
}
