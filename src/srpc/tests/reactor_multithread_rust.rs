// Independent OS threads own independent canonical reactors and task queues.

use std::future::Future;
use std::pin::Pin;
use std::sync::mpsc;
use std::task::{Context, Poll};

use srpc::reactor::{reactor_spawn_stackless_task_with_result, Reactor};

/// Ready on the second poll; wakes itself during the first.
struct PendingOnce {
    polls: u32,
    value: i64,
}

impl Future for PendingOnce {
    type Output = i64;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<i64> {
        self.polls += 1;
        if self.polls == 1 {
            cx.waker().wake_by_ref();
            Poll::Pending
        } else {
            Poll::Ready(self.value)
        }
    }
}

fn drive_one_reactor(seed: i64) -> (i64, usize) {
    let reactor = Reactor::get_reactor();
    let task = Box::pin(async move {
        let v = PendingOnce {
            polls: 0,
            value: seed,
        }
        .await;
        srpc::misc::async_double(v).await
    });
    let (tx, rx) = mpsc::channel::<i64>();
    reactor_spawn_stackless_task_with_result(&reactor, task, move |value| {
        tx.send(value).expect("receiver alive");
    });
    assert!(rx.try_recv().is_err(), "suspended, not completed");
    reactor.run_loop(false, true);
    let got = rx.try_recv().expect("one pump completes the woken task");
    // Return the reactor's identity too, so the test can prove the two
    // threads did not share an instance.
    (got, std::rc::Rc::as_ptr(&reactor) as usize)
}

#[test]
fn two_threads_run_independent_reactors_in_one_process() {
    let here = drive_one_reactor(21);

    let there = std::thread::spawn(|| drive_one_reactor(100))
        .join()
        .expect("second reactor thread");

    assert_eq!(here.0, 42, "this thread's task: 21 -> 42");
    assert_eq!(there.0, 200, "the other thread's task: 100 -> 200");
    assert_ne!(
        here.1, there.1,
        "each thread must have constructed its own reactor instance"
    );

    // And the original thread's reactor still works after the other thread
    // has come and gone -- its state was never shared or torn down remotely.
    let again = drive_one_reactor(3);
    assert_eq!(again.0, 6);
    assert_eq!(again.1, here.1, "same thread, same reactor");
}
