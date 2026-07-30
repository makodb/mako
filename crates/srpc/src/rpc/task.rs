//! A stackless executor: `RpcFuture`, `Task`, and `block_on`.
//!
//! ## Why this exists, in one number
//!
//! At depth 1 the blocking client runs at 74–76% of the C++ (see
//! `docs/dev/srpc_s5_perf_gate.md`). The cause is not the wire or the
//! transport: [`Future::wait`] parks the calling thread on a condvar,
//! so every request costs a park/unpark pair plus the poll thread's
//! signal. The C++ pays none of that because it resumes a coroutine
//! **on the poll thread**, inline in the reply callback.
//!
//! That is what a [`Task`] reproduces. Its waker does not hand the task
//! to a scheduler — it re-polls the task then and there, on whatever
//! thread completed the future, which for an RPC reply is the poll
//! thread. `rpcbench.cc` calls this the "self-referencing waker"; the
//! shape is the same.
//!
//! ## Zero dependencies means the vtable is written by hand
//!
//! No `futures` crate, so [`RawWaker`] and [`RawWakerVTable`] are built
//! directly. The three operations must keep the refcount exact: `clone`
//! adds one, `drop` removes one, and `wake` CONSUMES its reference
//! while `wake_by_ref` does not. Getting `wake` vs `wake_by_ref` wrong
//! is a leak or a double-free, and neither shows up in a passing test —
//! which is why `waker_refcounts_are_balanced` checks the count
//! directly rather than inferring it from behaviour.
//!
//! ## Re-entrancy
//!
//! Because the waker re-polls inline, a task's continuation runs while
//! the reply callback is on the stack. It may issue more RPCs (a
//! different lock), but it must not block waiting on the poll thread —
//! it *is* the poll thread. `Future::complete` releases the state lock
//! before waking for this reason.
//!
//! ## This is the crate's SECOND unsafe file
//!
//! `sys` was the only one, and the rule was "every unsafe block lives
//! there, so the FFI boundary is one file to audit". That rule now has
//! an exception, and it is a different KIND of unsafe: not a syscall
//! boundary but a language-level contract (`RawWaker`'s vtable), which
//! cannot be expressed in safe Rust and cannot live in `sys` without
//! putting task machinery in the syscall module. The audit surface is
//! the four functions below and nothing else.

#![allow(unsafe_code)]

use crate::rpc::client::Future as RpcReply;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};

/// Awaits an in-flight RPC.
pub struct RpcFuture {
    reply: Arc<RpcReply>,
}

impl RpcFuture {
    pub fn new(reply: Arc<RpcReply>) -> RpcFuture {
        RpcFuture { reply }
    }
}

impl Future for RpcFuture {
    type Output = Result<Vec<u8>, i32>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        // `register_waker` reports "already done" under the same lock
        // that completion takes. Checking done-ness separately would
        // race: the reply can land between the check and the register,
        // and the task would then wait on a wake that already fired.
        if self.reply.register_waker(cx.waker()) {
            return Poll::Ready(self.reply.take_result());
        }
        Poll::Pending
    }
}

/// A spawned unit of work, driven by re-polling.
struct TaskInner {
    future: Mutex<Option<Pin<Box<dyn Future<Output = ()> + Send>>>>,
    done: AtomicBool,
    /// For `block_on`-style joins.
    finished: Mutex<bool>,
    signal: Condvar,
}

impl TaskInner {
    /// Poll once. Called first by `spawn`, then by the waker on each
    /// completion — on whatever thread completed it.
    fn poll_once(self: &Arc<Self>) {
        if self.done.load(Ordering::Acquire) {
            return;
        }
        // Take the future out for the duration of the poll, so a
        // re-entrant wake (a future that completes during its own poll)
        // finds the slot empty and returns instead of recursing into a
        // deadlock on this mutex.
        let mut slot = match self.future.try_lock() {
            Ok(g) => g,
            Err(_) => return,
        };
        let Some(mut fut) = slot.take() else {
            return;
        };
        drop(slot);

        let waker = waker_for(Arc::clone(self));
        let mut cx = Context::from_waker(&waker);
        match fut.as_mut().poll(&mut cx) {
            Poll::Ready(()) => {
                self.done.store(true, Ordering::Release);
                *self.finished.lock().unwrap() = true;
                self.signal.notify_all();
            }
            Poll::Pending => {
                *self.future.lock().unwrap() = Some(fut);
                // A wake that arrives DURING a poll finds the slot
                // empty and does nothing; the future is back now, so a
                // task that completed mid-poll is re-driven by the next
                // wake rather than stalling on one it could not observe.
            }
        }
    }
}

// --- the hand-written waker vtable ------------------------------------

const VTABLE: RawWakerVTable =
    RawWakerVTable::new(waker_clone, waker_wake, waker_wake_by_ref, waker_drop);

/// # Safety
/// `data` is always an `Arc<TaskInner>` leaked by [`waker_for`].
unsafe fn waker_clone(data: *const ()) -> RawWaker {
    let arc = unsafe { Arc::from_raw(data as *const TaskInner) };
    // One for the clone being produced, one to put back.
    let cloned = Arc::clone(&arc);
    std::mem::forget(arc);
    RawWaker::new(Arc::into_raw(cloned) as *const (), &VTABLE)
}

/// `wake` CONSUMES its reference — the Arc is dropped here, not leaked.
unsafe fn waker_wake(data: *const ()) {
    let arc = unsafe { Arc::from_raw(data as *const TaskInner) };
    arc.poll_once();
}

/// `wake_by_ref` does NOT consume — put the reference back.
unsafe fn waker_wake_by_ref(data: *const ()) {
    let arc = unsafe { Arc::from_raw(data as *const TaskInner) };
    arc.poll_once();
    std::mem::forget(arc);
}

unsafe fn waker_drop(data: *const ()) {
    drop(unsafe { Arc::from_raw(data as *const TaskInner) });
}

fn waker_for(inner: Arc<TaskInner>) -> Waker {
    let raw = RawWaker::new(Arc::into_raw(inner) as *const (), &VTABLE);
    // @unsafe { the vtable above upholds the RawWaker contract }
    unsafe { Waker::from_raw(raw) }
}

// --- public API --------------------------------------------------------

/// A handle to a spawned task.
pub struct JoinHandle {
    inner: Arc<TaskInner>,
}

impl JoinHandle {
    pub fn is_done(&self) -> bool {
        self.inner.done.load(Ordering::Acquire)
    }

    /// Block until the task finishes.
    ///
    /// This parks — but ONCE for the whole task, not once per awaited
    /// RPC, which is the difference the executor buys.
    pub fn join(&self) {
        let mut guard = self.inner.finished.lock().unwrap();
        while !*guard {
            guard = self.inner.signal.wait(guard).unwrap();
        }
    }
}

/// Start a task. It is polled immediately on the calling thread and
/// thereafter re-polled by its waker, on whichever thread completes the
/// future it was waiting on.
pub fn spawn<F>(future: F) -> JoinHandle
where
    F: Future<Output = ()> + Send + 'static,
{
    let inner = Arc::new(TaskInner {
        future: Mutex::new(Some(Box::pin(future))),
        done: AtomicBool::new(false),
        finished: Mutex::new(false),
        signal: Condvar::new(),
    });
    inner.poll_once();
    JoinHandle { inner }
}

/// Run a future to completion on the calling thread.
pub fn block_on<F: Future<Output = T> + Send + 'static, T: Send + 'static>(future: F) -> T {
    let slot: Arc<Mutex<Option<T>>> = Arc::new(Mutex::new(None));
    let out = Arc::clone(&slot);
    let handle = spawn(async move {
        *out.lock().unwrap() = Some(future.await);
    });
    handle.join();
    let v = slot.lock().unwrap().take();
    v.expect("a finished task must have produced its output")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;

    #[test]
    fn a_ready_future_completes_without_parking() {
        let hits = Arc::new(AtomicUsize::new(0));
        let seen = Arc::clone(&hits);
        let h = spawn(async move {
            seen.fetch_add(1, Ordering::SeqCst);
        });
        assert!(h.is_done(), "an immediately-ready task finishes in spawn");
        assert_eq!(hits.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn block_on_returns_the_value() {
        assert_eq!(block_on(async { 6 * 7 }), 42);
    }

    #[test]
    fn a_task_resumes_on_the_thread_that_completes_it() {
        // The property the whole stage exists for: the continuation runs
        // on the COMPLETING thread, not on a parked waiter.
        let reply = Arc::new(RpcReply::new(1));
        let resumed_on = Arc::new(Mutex::new(None::<std::thread::ThreadId>));
        let record = Arc::clone(&resumed_on);
        let awaited = Arc::clone(&reply);

        let h = spawn(async move {
            let _ = RpcFuture::new(awaited).await;
            *record.lock().unwrap() = Some(std::thread::current().id());
        });
        assert!(!h.is_done(), "should be pending until the reply lands");

        let completer = std::thread::spawn(move || {
            reply.complete_for_test(0, b"ok".to_vec());
            std::thread::current().id()
        });
        let completer_id = completer.join().unwrap();
        h.join();

        assert_eq!(
            *resumed_on.lock().unwrap(),
            Some(completer_id),
            "the continuation must run on the completing thread"
        );
    }

    #[test]
    fn an_awaited_reply_delivers_its_payload() {
        let reply = Arc::new(RpcReply::new(2));
        let got = Arc::new(Mutex::new(None));
        let sink = Arc::clone(&got);
        let awaited = Arc::clone(&reply);
        let h = spawn(async move {
            *sink.lock().unwrap() = Some(RpcFuture::new(awaited).await);
        });
        reply.complete_for_test(0, b"payload".to_vec());
        h.join();
        assert_eq!(got.lock().unwrap().clone(), Some(Ok(b"payload".to_vec())));
    }

    #[test]
    fn an_error_reply_surfaces_as_an_error() {
        let reply = Arc::new(RpcReply::new(3));
        let got = Arc::new(Mutex::new(None));
        let sink = Arc::clone(&got);
        let awaited = Arc::clone(&reply);
        let h = spawn(async move {
            *sink.lock().unwrap() = Some(RpcFuture::new(awaited).await);
        });
        reply.complete_for_test(7, Vec::new());
        h.join();
        assert_eq!(got.lock().unwrap().clone(), Some(Err(7)));
    }

    #[test]
    fn a_reply_that_lands_before_the_poll_is_not_lost() {
        // The race the `register_waker`-reports-done contract exists
        // for: complete the future BEFORE anything awaits it. A task
        // that parks here would park forever.
        let reply = Arc::new(RpcReply::new(4));
        reply.complete_for_test(0, b"early".to_vec());
        let got = Arc::new(Mutex::new(None));
        let sink = Arc::clone(&got);
        let h = spawn(async move {
            *sink.lock().unwrap() = Some(RpcFuture::new(reply).await);
        });
        assert!(h.is_done(), "an already-complete reply must not park");
        assert_eq!(got.lock().unwrap().clone(), Some(Ok(b"early".to_vec())));
    }

    #[test]
    fn waker_refcounts_are_balanced() {
        // A leak or a double-free here does not change behaviour in any
        // of the tests above, so the count is checked directly.
        let reply = Arc::new(RpcReply::new(5));
        let awaited = Arc::clone(&reply);
        let h = spawn(async move {
            let _ = RpcFuture::new(awaited).await;
        });
        reply.complete_for_test(0, Vec::new());
        h.join();
        // The task holds one; nothing else should.
        assert_eq!(
            Arc::strong_count(&h.inner),
            1,
            "waker clones outlived the task — clone/drop are unbalanced"
        );
    }

    #[test]
    fn sequential_awaits_in_one_task_all_run() {
        let a = Arc::new(RpcReply::new(6));
        let b = Arc::new(RpcReply::new(7));
        let order = Arc::new(Mutex::new(Vec::new()));
        let log = Arc::clone(&order);
        let (wa, wb) = (Arc::clone(&a), Arc::clone(&b));
        let h = spawn(async move {
            let _ = RpcFuture::new(wa).await;
            log.lock().unwrap().push(1);
            let _ = RpcFuture::new(wb).await;
            log.lock().unwrap().push(2);
        });
        a.complete_for_test(0, Vec::new());
        assert!(!h.is_done(), "still waiting on the second reply");
        b.complete_for_test(0, Vec::new());
        h.join();
        assert_eq!(*order.lock().unwrap(), vec![1, 2]);
    }
}
