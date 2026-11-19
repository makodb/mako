# srpc's async runtime

Stackless tasks are standard Rust futures, spawned through canonical
`reactor/reactor.rs`. On a `PollThread` they run on that thread's Lion runtime
(`lion-executor`, whose executor is verified in Verus); on any other thread
they run on the `Reactor`'s own executor, described below. Both lanes run the
same code: the C++ lane transpiles SRPC's canonical Rust and Lion's executable
code alike. The Rust lane uses `std::future::Future`, `std::pin::Pin`, and
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

Two executors run stackless tasks, depending on the thread (plan item S3 in
`docs/dev/lion-runtime-plan.md`):

- **On a `PollThread`**, the thread runs a Lion runtime over
  `SrpcEpollBackend`. The spawn functions poll a task once inline, as before;
  a task still pending becomes a Lion `spawn_local` task, and the waker of
  that first poll forwards to it. Lion wakes it from any thread through its
  cross-thread queue and the backend's eventfd. A task the runtime drops at
  shutdown is counted in `stackless_cancel_report().teardown_tasks` and logged
  at ERROR.
- **Everywhere else**, the task registers with the thread's `Reactor`. Its
  `run_loop` drains wake ingress, polls ready tasks, and returns completed
  slots to the free list. A thread with no loop must pump `run_loop` itself,
  as `create_run` does.

A `PollThread`'s driver task does the owner-side work: it drains commands and
jobs and calls `run_loop(false, true)` when woken, and sleeps on a Lion timer
until the next event deadline. Every source of work wakes it; there is no
polling interval. The exception is a `Job` whose `Ready()` is false: it has
no wake, so the driver re-checks it every millisecond while it waits.

The executors are cooperative and local to their thread. A poll that does not
return blocks the thread. Tasks do not migrate between threads.

SRPC's TCP transport runs as Lion tasks on the `PollThread` (a reader and a
writer task per connection over `lion_reactor::AsyncFd`, an accept task per
listener), over SRPC's own epoll backend. Stackful fibers remain SRPC's: they
block on SRPC events, and a fiber resumes only in its owner thread's drain.
Stackless futures and fibers run side by side; the fiber blocking operations
are not replaced by async I/O.
