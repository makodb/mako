# srpc's async runtime

The stackless executor lives in canonical `reactor/reactor.rs`. It accepts
standard Rust futures and runs the same scheduling policy in the Rust and C++
lanes. The Rust lane uses `std::future::Future`, `std::pin::Pin`, and
`std::task::{Poll, Context, Wake, Waker}` directly.

## Executor interface

`reactor_spawn_stackless_task_with_result` accepts a
`Pin<Box<dyn Future<Output = T>>>` and a completion callback `FnMut(T)`.
Callers pass `Box::pin(future)`. `TaskVoid` names the corresponding boxed
future with output `()` for `reactor_spawn_stackless_task_impl`.

Spawn polls once immediately. A ready future delivers its value immediately;
a pending future enters the task table. Completion consumes the `Poll::Ready`
payload, so the output does not need `Default` or `Clone`.

| Piece | Canonical implementation |
| --- | --- |
| Task table | `stackless_tasks_: RefCell<Vec<StacklessTaskEntry>>` |
| Ready queue | `ready_stackless_tasks_: RefCell<VecDeque<usize>>` |
| Poll pass | `process_stackless_tasks()`, called by `run_loop(..)` |
| Wake ingress | `StacklessWakeTarget`, implementing `std::task::Wake` |
| Cancellation | `StacklessCancelReport` and `stackless_cancel_report` |

Each wake target owns an `Arc` to its ingress queue and ticket. It carries no
Reactor pointer. A wake may arrive on another thread; it queues the ticket for
the owning reactor. Closing a ticket before slot reuse prevents an old retained
waker from scheduling a new task in that slot. Teardown stops ingress admission
before destroying task closures and releasing bindings.

The registry stores an owned `Waker`. Each synchronous poll creates a temporary
`Context::from_waker(&waker)`. A future that needs notification after returning
`Pending` clones `context.waker()`. That owned clone can outlive the poll, the
future, and reactor teardown; ticket and ingress checks decide whether it can
still enqueue work.

## Rust and C++ lowering

Under rustc, an `async fn` produces an ordinary Rust future. There is no facade
`Task`, `Poll`, `Context`, or waker bridge in the Rust lane.

The transpiler maps the supported owning type
`Pin<Box<dyn Future<Output = T>>>` to `rusty::Task<T>`, with unit output mapped to
`Task<void>`. Standard `Poll` variants lower to the runtime's ready/pending
storage, and `Box::pin` either carries an existing coroutine task or stores a
concrete pollable future in a coroutine frame. Standard `Wake` methods retain
their owning or borrowed `Arc<Self>` receivers in the generated C++.

The C++ task promise owns a snapshot of its polling waker and context. This
keeps the context available to suspended C++ awaiters through task destruction,
while canonical Rust only borrows a context for the synchronous poll. Pending
runtime polls do not construct an unused `T`.

## What drives the executor

`PollThread` spawns a `PollThreadWorker`. Its canonical `pollworker_poll_loop`
dispatches epoll readiness, drains commands, processes deferred removals, and
then calls `Reactor::get_reactor().run_loop(false, true)` once per pass. The
reactor drains wake ingress, polls ready tasks, and returns completed slots to
the free list. Native callers may also pump `run_loop` directly.

The executor is cooperative and local to each reactor thread. A poll that does
not return blocks that reactor's pass. Tasks do not migrate between reactors.
The poll loop uses a 1 ms epoll timeout; it has no timer wheel or separate wake
notification file descriptor.

SRPC's I/O path still uses epoll and stackful fibers. Stackless futures run
alongside that path; they do not replace the fiber blocking operations with an
async I/O driver.
