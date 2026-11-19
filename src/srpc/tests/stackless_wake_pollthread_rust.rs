// The canonical poll worker pumps suspended stackless tasks without a test hook.

use std::future::Future;
use std::pin::Pin;
use std::sync::mpsc;
use std::sync::Arc;
use std::task::{Context, Poll};

use srpc::misc::{Job, OneTimeJob};
use srpc::reactor::{reactor_spawn_stackless_task_impl, reactor_spawn_stackless_task_with_result, Reactor};

use srpc::reactor::PollThread;

/// Ready on the second poll; wakes itself during the first, exercising the
/// pending-wake re-arm rather than a leisurely wake-after-park.
struct PendingOnce {
    polls: u32,
}

impl Future for PendingOnce {
    type Output = i64;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<i64> {
        self.polls += 1;
        if self.polls == 1 {
            cx.waker().wake_by_ref();
            Poll::Pending
        } else {
            Poll::Ready(7)
        }
    }
}

/// The composed workload both tests spawn: suspend once, then run the
/// canonical `async fn` chain (7 -> 14).
fn suspending_workload() -> Pin<Box<dyn Future<Output = i64>>> {
    Box::pin(async {
        let seven = PendingOnce { polls: 0 }.await;
        srpc::misc::async_double(seven).await
    })
}

#[test]
#[allow(unsafe_code)]
fn canonical_poll_thread_pumps_a_suspended_task() {
    let poll_thread = PollThread::create();

    let (tx, rx) = mpsc::channel::<i64>();
    let spawn_job: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        let reactor = Reactor::get_reactor();
        let tx = tx.clone();
        reactor_spawn_stackless_task_with_result(&reactor, suspending_workload(), move |value| {
            tx.send(value).expect("receiver alive");
        });
    })));
    poll_thread.add(spawn_job);

    let value = rx
        .recv_timeout(std::time::Duration::from_secs(5))
        .expect("the canonical worker must pump the suspended task to completion");
    assert_eq!(value, 14);

    poll_thread.shutdown();
}

struct ForeignWake {
    ready: Arc<std::sync::atomic::AtomicBool>,
    wake_sender: mpsc::Sender<(std::task::Waker, std::thread::ThreadId)>,
    published: bool,
}

impl Future for ForeignWake {
    type Output = Option<std::thread::ThreadId>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.ready.load(std::sync::atomic::Ordering::Acquire) {
            return Poll::Ready(Some(std::thread::current().id()));
        }
        if !self.published {
            self.wake_sender.send((cx.waker().clone(), std::thread::current().id())).unwrap();
            self.published = true;
        }
        Poll::Pending
    }
}

#[test]
fn foreign_wake_resumes_the_task_on_its_original_poll_thread() {
    let poll_thread = PollThread::create();
    let ready = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let readiness = ready.clone();
    let (wake_tx, wake_rx) = mpsc::channel();
    let (result_tx, result_rx) = mpsc::channel();
    let job: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        let task = Box::pin(ForeignWake {
            ready: readiness.clone(), wake_sender: wake_tx.clone(), published: false,
        });
        let result = result_tx.clone();
        reactor_spawn_stackless_task_with_result(&Reactor::get_reactor(), task, move |thread| {
            result.send(thread).unwrap();
        });
    })));
    poll_thread.add(job);
    let (wake, owner) = wake_rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap();
    assert_ne!(owner, std::thread::current().id());
    assert!(result_rx.try_recv().is_err());
    ready.store(true, std::sync::atomic::Ordering::Release);
    wake.wake();
    let resumed = result_rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap();
    assert_eq!(resumed, Some(owner));
    poll_thread.shutdown();
}

#[test]
fn retained_wake_after_poll_thread_shutdown_is_rejected_safely() {
    let poll_thread = PollThread::create();
    let (wake_tx, wake_rx) = mpsc::channel();
    let (result_tx, result_rx) = mpsc::channel();
    let job: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        let task = Box::pin(ForeignWake {
            ready: Arc::new(std::sync::atomic::AtomicBool::new(false)),
            wake_sender: wake_tx.clone(),
            published: false,
        });
        let result = result_tx.clone();
        reactor_spawn_stackless_task_with_result(&Reactor::get_reactor(), task, move |thread| {
            result.send(thread).unwrap();
        });
    })));
    poll_thread.add(job);
    let (wake, _) = wake_rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap();
    poll_thread.shutdown();
    drop(poll_thread);
    std::thread::spawn(move || {
        for _ in 0..1000 {
            wake.wake_by_ref();
        }
    }).join().unwrap();
    assert!(matches!(result_rx.try_recv(), Err(mpsc::TryRecvError::Disconnected)));
}


#[test]
fn void_task_foreign_wake_completes_on_owner_and_retained_wake_is_safe() {
    let poll_thread = PollThread::create();
    let ready = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let readiness = ready.clone();
    let (wake_tx, wake_rx) = mpsc::channel();
    let (result_tx, result_rx) = mpsc::channel();
    let job: Arc<dyn Job> = Arc::new(OneTimeJob::new(Box::new(move || {
        let waiting = ForeignWake {
            ready: readiness.clone(), wake_sender: wake_tx.clone(), published: false,
        };
        let result = result_tx.clone();
        let task = Box::pin(async move {
            let owner = waiting.await;
            result.send(owner).unwrap();
        });
        reactor_spawn_stackless_task_impl(&Reactor::get_reactor(), task);
    })));
    poll_thread.add(job);
    let (wake, owner) = wake_rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap();
    assert_ne!(owner, std::thread::current().id());
    assert!(result_rx.try_recv().is_err(), "void task must suspend until woken");
    ready.store(true, std::sync::atomic::Ordering::Release);
    wake.wake_by_ref();
    assert_eq!(result_rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap(), Some(owner));
    poll_thread.shutdown();
    drop(poll_thread);
    std::thread::spawn(move || {
        for _ in 0..1000 { wake.wake_by_ref(); }
    }).join().unwrap();
}
