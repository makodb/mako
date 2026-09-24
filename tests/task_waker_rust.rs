use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Wake, Waker};

struct RetainWake {
    slot: Arc<Mutex<Option<Waker>>>,
}

impl Future for RetainWake {
    type Output = ();

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<()> {
        *self.slot.lock().unwrap() = Some(context.waker().clone());
        Poll::Pending
    }
}

struct CountWake {
    calls: Arc<AtomicUsize>,
}

impl Wake for CountWake {
    fn wake(self: Arc<Self>) {
        self.calls.fetch_add(1, Ordering::Relaxed);
    }
}

#[test]
fn retained_native_waker_owns_its_callback_after_task_and_context_drop() {
    let calls = Arc::new(AtomicUsize::new(0));
    let callback_lifetime = Arc::downgrade(&calls);
    let slot = Arc::new(Mutex::new(None));
    {
        let waker = Waker::from(Arc::new(CountWake { calls }));
        let mut context = Context::from_waker(&waker);
        let mut task = Box::pin(RetainWake { slot: slot.clone() });
        assert!(task.as_mut().poll(&mut context).is_pending());
    }
    // The retained standard waker keeps its target alive beyond the poll.
    assert!(callback_lifetime.upgrade().is_some());
    let retained = slot.lock().unwrap().take().unwrap();
    let observed = callback_lifetime.clone();
    std::thread::spawn(move || {
        for _ in 0..1000 {
            retained.wake_by_ref();
        }
        assert_eq!(observed.upgrade().unwrap().load(Ordering::Relaxed), 1000);
    })
    .join()
    .unwrap();
    assert!(callback_lifetime.upgrade().is_none());
}
