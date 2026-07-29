# The SRPC Book

A comprehensive developer guide for RRR — the **S**imple **RPC** framework powering Mako.

RRR stands for "Repeatable Research Runtime." It provides high-performance RPC, stackful fibers, an event-driven reactor, and binary serialization — all with Rust-inspired memory safety.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture Overview](#2-architecture-overview)
3. [Fibers (Stackful)](#3-fibers-stackful)
4. [The Reactor Pattern](#4-the-reactor-pattern)
5. [Event System](#5-event-system)
6. [I/O Layer: Polling and Connections](#6-io-layer-polling-and-connections)
7. [RPC Protocol](#7-rpc-protocol)
8. [RPC Client](#8-rpc-client)
9. [RPC Server](#9-rpc-server)
10. [Serialization (Marshal)](#10-serialization-marshal)
11. [Reliability Features](#11-reliability-features)
12. [Service Definition and Code Generation](#12-service-definition-and-code-generation)
13. [Threading and Synchronization](#13-threading-and-synchronization)
14. [Memory Safety (RustyCpp)](#14-memory-safety-rustycpp)
15. [Performance Tuning](#15-performance-tuning)
16. [API Reference](#16-api-reference)
17. [Pitfalls and Best Practices](#17-pitfalls-and-best-practices)
18. [Troubleshooting](#18-troubleshooting)

---

## 1. Introduction

RRR is a custom-built RPC and concurrency framework designed for high-performance distributed systems research. It is the networking and concurrency backbone of the Mako distributed transactional datastore.

### Why a Custom Framework?

Off-the-shelf RPC frameworks (gRPC, Thrift) are designed for general-purpose use. RRR is purpose-built for:

- **Ultra-low latency**: Sub-100us round-trip with TCP/IP; ~10us with RDMA
- **Massive concurrency**: 100,000+ concurrent operations via lightweight fibers
- **Zero-copy where possible**: Minimal allocation and copying on hot paths
- **Lock-free design**: Cooperative scheduling eliminates most synchronization
- **Research flexibility**: Easy to modify, extend, and instrument

### Key Features

| Feature | Description |
|---------|-------------|
| **Stackful Fibers** | Lightweight fibers with custom x86_64 assembly context switching |
| **Reactor Pattern** | Event-driven I/O via epoll (Linux) / kqueue (macOS) |
| **Binary RPC** | Compact wire format with code generation from `.rpc` definitions |
| **Connection Pooling** | Health-aware pools with load balancing |
| **Reliability** | Reconnection, circuit breaker, request buffering, heartbeat |
| **Memory Safety** | RustyCpp smart pointers and borrow checking throughout |

### Performance

Typical numbers (TCP/IP backend):

| Metric | Value |
|--------|-------|
| Fast-mode RPC | 500K+ requests/sec |
| Standard RPC | 100-200K requests/sec |
| Round-trip latency | <100 us |
| Fiber context switch | ~10-100 ns |
| Fiber stack size | 1 MiB (configurable) |
| Concurrent fibers | 100,000+ per thread |

---

## 2. Architecture Overview

RRR is organized in layers:

```
+-----------------------------------------------------+
|  User Application (RPC Services / Clients)           |
+-----------------------------------------------------+
|  RPC Layer                                           |
|  - ClientConnection, ServerConnection                |
|  - Connection pooling, health checking               |
|  - Future-based async results                        |
|  - Request queuing, retry logic                      |
+-----------------------------------------------------+
|  Reactor / Event Loop                                |
|  - Fiber scheduling and management                   |
|  - Event synchronization (Event, WaitAll, WaitAny)   |
|  - Timeout handling                                  |
+-----------------------------------------------------+
|  I/O Layer                                           |
|  - Pollable interface                                |
|  - epoll/kqueue abstraction                          |
|  - PollThread / PollThreadWorker                     |
|  - Cross-thread command channel (mpsc)               |
+-----------------------------------------------------+
|  Base Utilities                                      |
|  - Marshal (binary serialization)                    |
|  - SpinMutex, Cell, RefCell, Arc, Rc                 |
|  - Logging, timing, random, stats                    |
+-----------------------------------------------------+
|  System Layer (POSIX sockets, epoll, pthreads)       |
+-----------------------------------------------------+
```

### Directory Structure

```
src/rrr/
  rpc/                # RPC client/server implementation
    client.hpp          # Client, Future, and connection APIs
    server.hpp          # Server, listener, dispatch (684 lines)
    callbacks.hpp       # Connection lifecycle callbacks
    circuit_breaker.hpp # Fail-fast fault tolerance
    connection_state.hpp# Connection state machine
    connection_metrics.hpp # Performance metrics
    heartbeat.hpp       # Keep-alive probes
    load_balancer.hpp   # Pool load-balancing strategies
    reconnect_policy.hpp# Reconnection strategies
    request_queue.hpp   # Pending request buffering
    request_options.hpp # Per-request configuration
    pollable_proxy.h    # Pollable proxy facade + typed Arc adapter helpers
    errors.hpp          # Error code definitions
    utils.hpp/cpp       # RPC utilities

  reactor/            # Event loop and fiber system
    reactor.h           # Core event loop scheduler (482 lines)
    fiber.h             # Modern fiber API (this_fiber namespace)
    fiber_impl.h/cc     # Fiber implementation + context switching
    fiber_context_x86_64.cc # Assembly context switching
    event.h             # Event synchronization primitives
    future.h            # Async result container
    epoll_wrapper.h     # Linux epoll / macOS kqueue abstraction
    quorum_event.h      # Multi-condition synchronization

  base/               # Core utilities
    threading.hpp       # SpinLock, SpinMutex
    basetypes.hpp       # Type aliases, NoCopy, SparseInt
    logging.hpp         # Log framework (FATAL/ERROR/WARN/INFO/DEBUG)
    misc.hpp            # General utilities

  misc/               # Serialization and extras
    marshal.hpp         # Binary serialization/deserialization
    alock.hpp           # Async queued lock with timeout
    alarm.hpp           # Timer utilities
    stat.hpp            # Statistics tracking

  pylib/              # Python bindings
    simplerpc/          # Python RPC client/server

  rrr.hpp             # C++ umbrella header
```

---

## 3. Fibers (Stackful)

Fibers are the fundamental concurrency primitive in RRR. They are **stackful execution contexts** that can pause and resume from any function depth.

### Why Fibers Instead of Threads?

| | Threads | Fibers |
|---|---------|--------|
| Stack size | 1-8 MB | 1 MiB (configurable) |
| Context switch | ~1-10 us (kernel) | ~10-100 ns (user-space) |
| Synchronization | Mutexes, atomics needed | None needed (cooperative) |
| Practical limit | ~10,000 | 100,000+ |
| Parallelism | True (multi-core) | Concurrent (single thread) |

### Key Insight: No Locks Needed

Fibers within the same reactor thread **never run simultaneously**. This eliminates race conditions, deadlocks, and the need for synchronization:

```cpp srpc-no-compile
// SAFE - no locks needed within a reactor
class Counter {
    int value = 0;
public:
    void increment() { value++; }  // No race condition possible
    int get() const { return value; }
};
```

### Fiber API

The modern API uses `Fiber` class and `this_fiber` namespace:

```cpp srpc-no-compile
#include "reactor/fiber.h"
using namespace rrr;

// Create and run a fiber
auto fiber = Fiber::create_run([]() {
    auto id = this_fiber::get_id();

    // Yield CPU to other fibers
    this_fiber::yield();

    // Sleep (fiber-aware, doesn't block thread)
    this_fiber::sleep_ms(100);

    // Check if we're in fiber context
    assert(this_fiber::in_fiber_context());
});
```

**Backward compatibility:** Legacy aliases still exist for older APIs. Use `Fiber` naming in new code.

### Fiber Lifecycle

```
Created  (Fiber::create_run)
  |
  v
Running  (executing user function)
  |-- yield() --> Suspended (waiting for event/timer)
  |                   |
  |                   v
  |               Resumed (event ready, reactor continues)
  |                   |
  |<------------------+
  v
Finished (function returns)
  |
  v
Recycled (if fiber reuse is enabled) or Destroyed
```

### Implementation Details

Fibers use custom x86_64 assembly for context switching:

```cpp srpc-no-compile
// CPU context saved/restored per fiber
struct FiberContext {
    void* rsp, *rip;                    // Stack and instruction pointers
    uintptr_t rbx, rbp;                 // Callee-saved registers
    uintptr_t r12, r13, r14, r15;
};

// Assembly function swaps CPU state between fibers
extern "C" void fiber_swap_context(FiberContext* from, FiberContext* to);
```

Each fiber has its own 1 MiB stack allocated on the heap. Context switches save the current register state and restore the target fiber's state — taking only ~10-100 nanoseconds.

---

## 4. The Reactor Pattern

The **Reactor** is the event loop that manages all fibers and I/O within a thread.

### Core Concepts

- **One reactor per thread** (thread-local singleton)
- **Never share reactors across threads**
- Two reactor instances per thread: a main reactor and a disk reactor
- The reactor schedules fibers, checks events, and processes I/O

### Basic Usage

```cpp srpc-no-compile
// Get the thread-local reactor
auto reactor = Reactor::get_reactor();

// Create fibers
reactor->create_run_fiber([]() {
    // Your concurrent task
    this_fiber::yield();  // Let others run
});

// Run event loop
reactor->loop(true);   // Run forever
reactor->loop(false);  // Process once
```

### Event Loop Operation

Each iteration of `Reactor::loop()`:

1. **Process commands** from other threads (add/remove Pollables, Jobs)
2. **epoll_wait()** to get ready file descriptors
3. **Handle I/O** on ready FDs (proxy-dispatch `handle_read/write` by FD)
4. **Check events** — collect ready events from the waiting queue
5. **Check timeouts** — move expired events to ready queue
6. **Process composite events** (WaitAll, WaitAny, QuorumEvent)
7. **Resume fibers** whose events are ready
8. **Repeat**

### Thread-Local Design (Critical)

```cpp srpc-no-compile
// WRONG - undefined behavior!
std::thread t([reactor]() {
    reactor->create_run_fiber([]() { /* ... */ });  // BAD!
});

// RIGHT - each thread has its own reactor
std::thread t([]() {
    auto reactor = Reactor::get_reactor();  // Thread-local
    reactor->create_run_fiber([]() { /* ... */ });
});
```

### The Scheduler: PollThread

Fibers that yield are dead until the reactor resumes them. The reactor's event loop acts as the scheduler:

1. Checks which events are ready (satisfied or timed out)
2. Calls `Resume()` on fibers waiting for those events
3. Runs resumed fibers until they yield again

In the actual codebase, `PollThread` owns a reactor and runs the event loop in a dedicated thread:

```cpp srpc-no-compile
class PollThread {
    Reactor* reactor_;
    void Run() {
        while (running_) {
            reactor_->loop(false);  // Process pending events
        }
    }
};
```

When you use the RPC framework, `PollThread` handles fiber scheduling automatically.

---

## 5. Event System

Events are the synchronization primitives for fibers. They allow fibers to wait for conditions without blocking the thread.

### Event Base Class

```cpp srpc-no-compile
class Event {
    enum Status { INIT, WAIT, READY, DONE, TIMEOUT };

    virtual bool Test() = 0;  // Check if condition is met
    void Wait();              // Block fiber until ready or timeout
    void Wait(uint64_t timeout_us);  // With timeout
};
```

### Event Types

#### IntEvent — Integer-based condition

```cpp srpc-no-compile
auto event = Reactor::create_sp_event<IntEvent>();
event->target_ = 42;  // Ready when value_ == 42

// In another fiber:
event->Set(42);  // Triggers the event

// In waiting fiber:
event->Wait();  // Resumes when value_ == target_
```

#### TimeoutEvent — Time-based trigger

```cpp srpc-no-compile
auto timeout = Reactor::create_sp_event<TimeoutEvent>(1000000);  // 1 second
timeout->Wait();
std::cout << "1 second elapsed!" << std::endl;
```

#### WaitAny (OrEvent) — Any of multiple events

```cpp srpc-no-compile
auto e1 = Reactor::create_sp_event<IntEvent>();
auto e2 = Reactor::create_sp_event<IntEvent>();
auto any = Reactor::create_sp_event<WaitAny>(e1, e2);
any->Wait();  // Continues when EITHER event triggers
```

#### WaitAll (AndEvent) — All events must be ready

```cpp srpc-no-compile
auto e1 = Reactor::create_sp_event<IntEvent>();
auto e2 = Reactor::create_sp_event<IntEvent>();
auto all = Reactor::create_sp_event<WaitAll>(e1, e2);
all->Wait();  // Continues when BOTH events trigger
```

#### WaitN (NEvent) — N of M events

```cpp srpc-no-compile
auto events = {e1, e2, e3, e4, e5};
auto quorum = Reactor::create_sp_event<WaitN>(events, 3);
quorum->Wait();  // Continues when 3 of 5 events trigger
```

### Wait with Timeout

```cpp srpc-no-compile
auto event = Reactor::create_sp_event<IntEvent>();
event->Wait(1000000);  // 1 second timeout

if (event->status_ == Event::TIMEOUT) {
    // Handle timeout
} else if (event->status_ == Event::DONE) {
    // Handle success
}
```

### Event Rules

1. **Events are single-use** — don't `Wait()` twice on the same event
2. **One waiter per event** — only one fiber can wait on a given event
3. **Events belong to a reactor** — never access from another thread

---

## 6. I/O Layer: Polling and Connections

### Pollable Interface

Legacy path: objects can still implement `Pollable` directly. The
trait now lives as an inline-DSL `pub trait` in
`src/rrr/reactor/epoll_wrapper.cc`:

```rust srpc-no-compile
#[cfg(rusty_cpp_rust)]
pub trait Pollable {
    fn fd(&self) -> i32;
    fn poll_mode(&self) -> i32;
    fn content_size(&mut self) -> usize;
    fn handle_read(&mut self) -> bool;
    fn handle_write(&mut self) -> i32;
    fn handle_error(&mut self);
    fn close(&mut self);
    fn check_pending_write_update(&self) -> bool;
    fn is_closed(&self) -> bool;
}
```

The rusty-cpp transpiler emits the matching `class Pollable { ...
virtual ... = 0; }` GEN block into the same file at build time.

Migration note: proxy scaffolding for `Pollable` now lives in `src/rrr/rpc/pollable_proxy.h`
(`PollableFacade` and typed-arc adapter support). Poll-thread
command payloads, storage, and event dispatch run through proxy-backed state
(`pro::proxy<PollableFacade>`), and epoll integration is fd-based (no
`Pollable*` userdata/update assumptions). `ServerListener`, `ServerConnection`,
and `ClientConnection` now use direct typed-proxy construction and no longer
inherit `Pollable`.

### Epoll Abstraction

RRR wraps Linux epoll and macOS kqueue behind a unified `Epoll` class:

```cpp srpc-no-compile
class Epoll {
    void add(int fd, int mode);                    // Register FD for monitoring
    void remove(int fd);                           // Unregister FD
    void update(int fd, int new_mode, int old_mode); // Change poll mode
    void wait(fn(int fd, int ready_events));       // Report readiness by FD
};
```

### PollThread / PollThreadWorker

**PollThread** (client-facing handle): Send commands from any thread.

```cpp srpc-no-compile
Arc<PollThread> pt = PollThread::create();
pt->add_proxy(make_pollable_proxy_from_typed_arc(connection)); // Direct proxy path
pt->request_close(connection->fd());    // Unregister + close
pt->shutdown();                         // Stop poll loop
```

**PollThreadWorker** (internal, runs in dedicated thread): Owns the epoll and all Pollable objects.

```cpp srpc-no-compile
class PollThreadWorker {
    Epoll poll_;
    unordered_map<int, PollableProxy> fd_to_pollable_;
    mpsc::Receiver<PollCommand> receiver_;  // Commands from main thread

    void poll_loop() {
        while (!stop_) {
            process_channel_commands();  // Add/remove/close/update_mode
            epoll_wait(...);
            for each ready fd:
                lookup proxy by fd, then call handle_read() / handle_write();
            trigger_jobs();
        }
    }
};
```

### Cross-Thread Communication

The poll thread and main thread communicate via an mpsc (multi-producer, single-consumer) channel:

| Command | Description |
|---------|-------------|
| `CmdAddPollable` | Register new Pollable proxy with epoll |
| `CmdRemovePollable` | Remove from epoll and worker ownership without forcing `close()` |
| `CmdClosePollable` | Remove + close via proxy dispatch, then drop ownership |
| `CmdUpdateMode` | Change poll_mode for an FD (fd-based lookup, no raw pointer payload) |
| `CmdAddJob` / `CmdRemoveJob` | Periodic job management |
| `CmdShutdown` | Stop poll loop |

This design avoids locks: the poll thread is the sole consumer of its command channel and the sole owner of all Pollable state.

### Job System

For periodic tasks within the poll loop:

```cpp srpc-no-compile
class Job {
    virtual bool Ready() = 0;   // Should this job run now?
    virtual void Work() = 0;    // Execute the job
    virtual bool Done() = 0;    // Is this job finished?
};

// OneTimeJob: runs once
// FrequentJob: runs periodically with configurable period
```

---

## 7. RPC Protocol

### Wire Format

```
REQUEST:
  +--------+------+---------+---------------------------+
  | size   | xid  | rpc_id  | arg1 | arg2 | ... | argN |
  | 4 bytes| 8 B  | 8 bytes | (marshalled arguments)   |
  +--------+------+---------+---------------------------+
  Note: <size> does NOT include itself

RESPONSE:
  +---------+------+------------+-------------------------------+-----------------------------+
  | size*   | xid  | error_code | [server_instance_id (optional)] | ret1 | ret2 | ... | retN |
  | 4 bytes | 8 B  | 4 bytes    | present when size high-bit set | (marshalled return values) |
  +---------+------+------------+-------------------------------+-----------------------------+
```

`size*` uses the high bit (`kResponseHeaderExtFlag`) as an extension flag.
- Flag clear: legacy response header (`xid`, `error_code`).
- Flag set: extended response header includes `server_instance_id` for restart detection.

### Wire Compatibility Note

SRPC now emits extended response headers with `server_instance_id`.

- New clients are backward compatible with old/new servers.
- Old clients that do not mask the size high-bit are not guaranteed to interoperate with new servers.
- Recommended rollout order: upgrade clients first, then servers.
- Recommended rollback order: rollback servers first, then clients.

### Request/Response Flow

```
Client                           Network                          Server
  |                                                                 |
  |-- Create Future(xid) ------>                                    |
  |-- Marshal args ------------> [size|xid|rpc_id|args] ---------->|
  |-- Return Future (async) --->                                    |
  |                                                                 |
  |                                  ServerConnection.handle_read() |
  |                                  Parse: size, xid, rpc_id      |
  |                                  Lookup Service by rpc_id       |
  |                                  Call Service.__dispatch__()    |
  |                                  Handler processes request      |
  |                                                                 |
  |                              <-- [size|xid|error|rets] --------|
  |-- TcpConnection on_frame -> ClientConnection                    |
  |   .decode_response_and_notify(bytes, size)                      |
  |-- Match xid to Future                                           |
  |-- Set reply data, signal fiber                                  |
  |-- Fiber resumes with result                                     |
```

Workstream K, sub-leaf 4g3c3 — `ClientConnection` no longer owns
the socket fd or the `Pollable` I/O methods. The channel layer's
`TcpConnection` registers with the poll thread, drives
`handle_read` / `handle_write`, and forwards decoded frames to
`ClientConnection::decode_response_and_notify` via the
`bind_channel_direct(...)` `on_frame` callback. The legacy
`ClientConnection::handle_read` body, the `socket_` / `in_` / `out_`
fields, the `pending_write_update_` flag, and the
`apply_keepalive_options` / `validate_connection` socket probes
have all been removed; the `Pollable` overrides remain as no-op
stubs only because deptran's host-scoped retention map
(`Reactor::clients_`) still wraps `ClientConnection` in
`PollableProxy`.

Workstream K, sub-leaf 4g3d — `Client::fd()` (the public RPC
client's file-descriptor accessor) is gone. Users that need a
peer identifier should use `Client::host()` instead.
`ClientConnection::fd()` survives only as a no-op (returns -1)
to keep `PollableTypedArcAdapter<ClientConnection>` compilable
for the deptran retention proxy. The RPC client's translation
unit also dropped its socket-path system headers
(`<sys/socket.h>`, `<sys/un.h>`, `<unistd.h>`, `<netdb.h>`,
`<netinet/tcp.h>`, `<sys/types.h>`, `<string.h>`) — none of
their symbols are reachable from the RPC layer post-4g3c3.

Workstream K, sub-leaf 4g4 — the `SRPC_USE_CHANNEL` /
`SRPC_DISABLE_CHANNEL` migration env vars and the
`srpc_use_channel()` / `srpc_set_use_channel_for_testing(...)` /
`srpc_reset_use_channel_for_testing()` helpers are deleted.
Channel mode is unconditional; `Client::connect(addr, ...)`
auto-installs a default TCP `ChannelFactoryProxy` whenever the
caller hasn't already bound one via
`set_channel_factory(...)`. The standalone migration-switch test
(`test_rpc_client_channel_switch`) is removed. External callers
that set the env vars or invoked the helpers should remove
those references.

Workstream K, server sub-leaves 5a–5g3 — RPC server migrated to
the channel layer end-to-end. `Server` now exposes
`set_channel_factory(ChannelFactoryProxy)` and
`is_channel_factory_bound()` (5a). `ServerConnection` exposes
`bind_channel(ChannelConnectionProxy)` and `is_channel_mode()`
(5b–5d): `reply<F>(req, error_code, write_fn)` builds the
response body in a scratch `Marshal` and dispatches via
`proxy->send_frame(...)` (5b); the proxy's `on_frame` callback
calls `decode_request_and_dispatch(bytes, size)` which mirrors
the legacy per-packet dispatch path (5c); `on_closed` /
`on_error` route to the existing `close()` path (5d).

`Server::start(addr)` calls `factory->make_listener()`,
installs an `on_accept(ChannelConnectionProxy)` callback that
constructs a `ServerConnection` bound to the new proxy and parks
it in `channel_sconns_`, then calls `listener->listen(addr)`
(5e). When no factory is explicitly bound, `Server::start`
auto-installs a default `TcpFactory(poll_thread_)` (5f), so
channel mode is the only path. `~Server` actively closes each
accepted channel-mode `ServerConnection` (driving the bound
proxy's `close()`) before clearing `channel_sconns_`, then
schedules the channel listener's close on the poll thread via a
`OneTimeJob` to avoid the `CmdAddPollable` race (5f).

The legacy `ServerListener` class is gone (5g1) along with
`Server::server_listener_`, the `ServerListener` socket fallback
in `start()`, and the legacy listener cleanup branches in
`~Server` / `stop_accepting`. `Server::get_bound_port()` is
re-implemented atop `ChannelListenerProxy::local_address()`.
`ServerConnection`'s legacy fd-path Pollable methods
(`handle_read` / `handle_write` / `handle_error` / `poll_mode` /
`content_size` / `check_pending_write_update` / `fd`) are
stubbed to no-ops (kept for `PollableProxy` facade ABI
conformance) and the underlying fields (`Marshal in_`,
`SpinMutex<Marshal> out_`, `int socket_`,
`Cell<bool> pending_write_update_`) are deleted (5g2). Unused
socket-path system headers (`<sys/socket.h>`, `<netdb.h>`,
`<sys/un.h>`, `<sys/select.h>`, `<sys/types.h>`,
`<netinet/tcp.h>`, `<unistd.h>`, `<pthread.h>`, `<string.h>`)
are dropped from `server.{hpp,cpp}` (5g3); only `<errno.h>`
remains (for `EINVAL` / `ENOENT` constants in the dispatch
path). `Server` is now reduced to its dispatch + lifecycle
state plus the channel binding.

Workstream K, sub-leaves 6a–6d — in-memory channel backend.
`src/rrr/rpc/inmemory_channel.{hpp,cpp}` adds a deterministic
in-process channel implementation conforming to the same
`ChannelConnectionFacade` / `ChannelListenerFacade` /
`ChannelFactoryFacade` contracts as the TCP backend. Components:

  * `InMemorySwitchboard` — thread-safe address → `Weak<InMemoryListener>`
    map. Thin lookup surface (`bind`, `unbind`, `lookup`); the address
    space is whatever string the caller chooses (e.g.
    `"inmemory://server-1"`).
  * `InMemoryConnectionState` — shared `Arc<>` state between the two
    sides of an in-memory channel pair: per-side callback storage
    (`on_frame` / `on_closed` / `on_error`), closed flags, and
    test-only fault-injection counters (drop / error). Mutated under
    a single `SpinMutex`.
  * `InMemoryChannel` — one side of a pair. `send_frame(...)` copies
    the bytes (the channel-layer contract requires the buffer to be
    valid only during the callback) and synchronously invokes the
    *peer's* `on_frame` callback. `close()` fires the peer's
    `on_closed` (not self's, mirroring TCP's "remote saw FIN"
    semantics). `is_closed()` reports the joint state
    (`a_closed || b_closed`).
  * `InMemoryListener` — registers itself with the switchboard at
    `listen(addr)` and unregisters at `close()`. `on_accept` fires
    synchronously inside `connect(...)` after the channel pair is
    constructed.
  * `InMemoryFactory` — `connect(addr)` looks up the listener for
    `addr`, builds a fresh `Arc<InMemoryConnectionState>`, splits it
    into two `InMemoryChannel`s with a synthesized client peer
    address, fires the listener's `on_accept` with the server-side
    proxy, and returns the client-side proxy. If no listener is
    registered, returns `ChannelError::ConnectionRefused`.

Fault-injection knobs (`inject_drop_next_sends(N)`,
`inject_send_error(err, N)`, `clear_fault_injection()`) live on
`InMemoryChannel` and are exposed through a test-only
`make_channel_pair_for_testing(a_addr, b_addr)` helper. Drops fire
before errors when both are queued; `closed` always wins.

The end-to-end test `src/rrr/tests/rpc_inmemory_channel_e2e_test.cc`
drives a real `Server` + `Client` through the `InMemoryFactory`
without any real sockets — useful as a deterministic foundation for
RPC-layer reliability tests (reconnect coverage, partition-induced
timeout coverage) that previously had to rely on TCP timing.

### Error Codes

| Code | Meaning |
|------|---------|
| 0 | Success |
| ENOENT | RPC method not found |
| EINVAL | Bad packet format |
| ETIMEDOUT | Request timed out |
| EAGAIN | Request rejected by queue policy (overflow/fail-fast) |

---

## 8. RPC Client

### Client (Single Connection)

`Client` is the public single-connection API (`ClientConnection` is an internal
pollable implementation detail):

```cpp srpc-no-compile
auto poll_thread = PollThread::create();
auto client = Client::create(poll_thread.clone());

int rc = client->connect("10.0.1.100:8100");
if (rc != 0) {
    // handle connect error
}

// Synchronous-style call using FutureResult
auto fu_result = client->request(RPC_METHOD_ID, FutureAttr{}, [&](BinaryWriteArchive& m) {
    rrr::Serialize_::serialize(arg1, m);
    rrr::Serialize_::serialize(arg2, m);
});

if (fu_result.is_ok()) {
    auto fu = fu_result.unwrap();
    fu->wait();
    int result = 0;
    rrr::deserialize_from(fu->get_reply(), result);
}
```

### Async RPC

```cpp srpc-compile-client
auto fu_result = client->request(RPC_METHOD_ID, FutureAttr{}, [&](BinaryWriteArchive& m) {
    rrr::Serialize_::serialize(arg1, m);
});
// ... do other work ...
if (fu_result.is_ok()) {
    auto fu = fu_result.unwrap();
    fu->wait();  // Block only when result is needed
}
```

### ClientPool (Connection Pool)

`Client` represents a single connection. `ClientPool` manages multiple connections
per address and applies pool-wide load-balancing policy:

```cpp srpc-no-compile
ClientPool pool;

PoolConfig cfg = PoolConfig::defaults();
cfg.load_balancing = LoadBalancingStrategy::ROUND_ROBIN;
cfg.max_connections = 8;
pool.set_pool_config(cfg);

auto client_opt = pool.get_client("10.0.1.100:8100");
if (client_opt.is_some()) {
    auto client = client_opt.unwrap();
    // Use selected client
}

// Strategies: RANDOM, ROUND_ROBIN, LEAST_CONNECTIONS, LEAST_LATENCY
```

`LEAST_CONNECTIONS` is based on explicit per-connection in-flight counts (`ConnectionMetrics::in_flight_requests()`), not derived sent-minus-completed math.

### Request Options

Per-request configuration:

```cpp srpc-no-compile
RequestOptions opts;
opts.timeout_ms = 5000;       // 5 second timeout
opts.max_retries = 3;         // Retry up to 3 times
opts.idempotent = true;       // Safe to retry
opts.total_timeout_ms = 15000; // Total time budget
```

### Keepalive Configuration

```cpp srpc-no-compile
KeepaliveConfig keepalive;
keepalive.idle_sec = 60;       // Seconds before first probe
keepalive.interval_sec = 10;   // Seconds between probes
keepalive.count = 3;           // Probes before declaring dead

// Presets available:
KeepaliveConfig::aggressive();  // Quick detection
KeepaliveConfig::relaxed();     // Low overhead
KeepaliveConfig::disabled();    // No keepalive
```

---

## 9. RPC Server

### Service Implementation

Implement the `Service` interface to handle RPCs:

```cpp srpc-compile-server
class MyService : public Service {
public:
    enum : i32 { RPC_DO_WORK = 0x1001 };

    int __reg_to__(Server& svr, size_t svc_index) override {
        return svr.reg_rpc(RPC_DO_WORK, svc_index);
    }

    void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection weak_sconn) override {
        if (rpc_id != RPC_DO_WORK) {
            return;
        }
        int arg;
        rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));
        rrr::Deserialize_::deserialize(arg, __req_ar__);
        int result = compute(arg);

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).reply(*req, 0, [&](BinaryWriteArchive& out) {
                rrr::Serialize_::serialize(result, out);
            });
        }
    }
};
```

Migration note: `Server` now stores services internally as `pro::proxy<ServiceFacade>`.
Legacy `Service` inheritance remains fully supported via a compatibility adapter,
so existing generated/handwritten services continue to work unchanged.
The server also accepts non-inheritance typed services, as long as they expose
`__reg_to__(Server&, size_t)` and `__dispatch__(i32, rusty::Box<Request>, WeakServerConnection)`.

```cpp srpc-no-compile
class MyTypedService {
public:
    int __reg_to__(Server& svr, size_t svc_index);
    void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection weak_sconn);
};

auto server = Server::new_(rusty::None);
server.reg_service(rusty::make_box<MyTypedService>());
```

### Server Lifecycle

```cpp srpc-compile-server
auto poll_thread = PollThread::create();
auto server = Server::new_(rusty::Some(poll_thread.clone()));
server.reg_service(rusty::make_box<MyService>());

// Start listening
int rc = server.start(reinterpret_cast<const int8_t*>("0.0.0.0:8100"));

// ... server runs ...

// Graceful shutdown
server.graceful_shutdown(30000);
poll_thread->shutdown();
```

### Graceful Shutdown

The server supports a phased shutdown process:

```
RUNNING -> STOP_ACCEPTING -> DRAINING -> CLOSING -> STOPPED
```

1. **STOP_ACCEPTING**: Reject new connections
2. **DRAINING**: Wait for pending requests to complete (with timeout)
3. **CLOSING**: Close all connections, execute shutdown hooks
4. **STOPPED**: All resources released

### Server API Compatibility Behavior

Recent reliability hardening removed crash-style server stubs:

- `ServerConnection::run_async()` executes callback inline; empty callbacks return error instead of aborting.
- `DeferredReply::run_async()` follows the same inline/explicit-error behavior.
- `ServerConnection::content_size()` returns currently buffered bytes.
- `ServerConnection::handle_free()` is an explicit no-op compatibility path (no abort).

### Dispatch Context

The `RpcServiceContext` maps RPC IDs to service implementations:

```cpp srpc-no-compile
class RpcServiceContext {
    // Map rpc_id -> service index (rpc_id_sp_map)
    // Services are stored as Vec<RefCell<ServiceProxy>>
    // (legacy Service subclasses are adapted to ServiceProxy on registration)
};
```

---

## 10. Serialization (Marshal)

The `Marshal` class provides binary serialization/deserialization:

### Basic Usage

```cpp srpc-no-compile
Marshal m;

// Serialize
rrr::Serialize_::serialize((i32)42, m);
rrr::Serialize_::serialize((i64)1234567890LL, m);
rrr::Serialize_::serialize(std::string("hello"), m);
rrr::Serialize_::serialize((double)3.14, m);

// Deserialize
i32 x; i64 y; std::string s; double d;
rrr::Deserialize_::deserialize(x, m);
rrr::Deserialize_::deserialize(y, m);
rrr::Deserialize_::deserialize(s, m);
rrr::Deserialize_::deserialize(d, m);
```

### Supported Types

| Type | C++ | Wire Size |
|------|-----|-----------|
| `i8` | `int8_t` | 1 byte |
| `i16` | `int16_t` | 2 bytes |
| `i32` | `int32_t` | 4 bytes |
| `i64` | `int64_t` | 8 bytes |
| `v32` | variable-length 32-bit | 1-5 bytes |
| `v64` | variable-length 64-bit | 1-10 bytes |
| `double` | `double` | 8 bytes |
| `string` | `std::string` | length-prefixed |
| containers | `vector`, `map`, `set`, `pair` | element-wise |

### Variable-Length Integers (SparseInt)

For compactness, `v32` and `v64` use variable-length encoding — small values use fewer bytes:

```cpp srpc-no-compile
v32 small_val(5);    // Encoded in 1 byte
v32 large_val(1000); // Encoded in 2 bytes
```

### Custom Types (Serializable)

To serialize a custom type, define three things on it: a `kind()`
discriminator, `save(BinaryWriteArchive&)`, and `load(BinaryReadArchive&)`.
No inheritance is required — the type stays a plain struct/class, and
the `pro::proxy<SerializableFacade>` machinery (the proxy library
facade in `third-party/proxy/`) does the type erasure at registration
time:

```cpp srpc-no-compile
struct MyTypedData {
    static constexpr int32_t kMarshallKind = 420999;
    i32 id;
    std::string name;

    int32_t kind() const { return kMarshallKind; }

    void save(rrr::BinaryWriteArchive& ar) const {
        rrr::Serialize_::serialize(id, ar);
        rrr::Serialize_::serialize(name, ar);
    }

    void load(rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(id, ar);
        rrr::Deserialize_::deserialize(name, ar);
    }
};

// Register so the `SerializableEnvelope` factory path can recover the
// type by kind on the receive side. Lives in any .cc file:
static int volatile reg_my_typed_data =
    rrr::SerializableRegistry::reg<MyTypedData>(MyTypedData::kMarshallKind);
```

For closed-set command types (the in-tree `MakoCommands` TypeList) the
kind is derived from the type's TypeList position — drop the
`kMarshallKind` constant + manual `kind()` method and inherit
`Serializable<T, MakoCommands>` instead.  See the "Closed-set
polymorphism" subsection below for the full pattern.

All in-tree deptran payload types (`VecRecData`, `ViewData`,
`KeyCmdBatchData`, `VecPieceData`, `TpcPrepareCommand`,
`TpcCommitCommand`, `TpcEmptyCommand`, `TpcNoopCommand`,
`TpcBatchCommand`, `ReplicatedDBCommand`, `EmptyGraph`, `RccGraph`,
`BulkPrepareLog`, `PaxosPrepCmd`, `HeartBeatLog`, `SyncLogRequest`,
`SyncLogResponse`, `SyncNoOpRequest`, `LogEntry`, `BulkPaxosCmd`,
`SimpleRWCommand`) use the Serializable path.  Wire format is
`[v32 kind][payload bytes]` for closed-set types and
`[v32 ANY_MESSAGE][v64-prefixed type_name][payload bytes]` for the
open-set `AnyMessage` envelope.

> **Historical note** — the prior abstract base classes
> `rrr::Marshallable` (virtual `to_marshal`/`from_marshal`) and
> `rrr::MarshallDeputy` (kind-tagged envelope around
> `shared_ptr<Marshallable>`) are gone as of Workstream N L10f-2 step 5
> (2026-05-05).  Production code uses `janus::Command`
> (`SerializableEnvelope<MakoCommands>`) for closed-set commands and
> `rrr::AnyMessage` for open-set graph payloads; both serialize
> through `pro::proxy<SerializableFacade>` value members with no
> intermediate adapter or `shared_ptr<Marshallable>` storage.

When extracting a typed payload from a `Command` envelope, use
`Command::unpack<T>()` (returns `T*` or `nullptr` on type mismatch)
or `Command::unpack_shared<T>()` (returns a `shared_ptr<T>` aliased
into the envelope's storage so the receiver can pin lifetime):

```cpp srpc-no-compile
janus::Command cmd;
rrr::Deserialize_::deserialize(cmd, m);  // wire decode populates kind + payload via factory + load

if (auto* view = cmd.unpack<ViewData>()) {
    // use view
}

// Aliased shared_ptr — extends the envelope's storage refcount.
auto sp = cmd.unpack_shared<ViewData>();
```

For RCC graph envelopes (`RccGraph`, `EmptyGraph`) the access pattern
uses `AnyMessage` directly (Workstream N L7).  See the AnyMessage
subsection below.

### Bookmarks

For recording sizes without seeking:

```cpp srpc-no-compile
Marshal m;
auto bookmark = m.set_bookmark(sizeof(i32));  // Reserve space
rrr::Serialize_::serialize(data1, m);
rrr::Serialize_::serialize(data2, m);
rrr::Serialize_::serialize(data3, m);
i32 payload_size = m.get_and_reset_write_cnt();
m.write_bookmark(bookmark, &payload_size);  // Fill in size
```

### Sink/Source Archive (Workstream N — in-flight)

A new serde / cereal-style serialization layer is being introduced in
parallel to `Marshal`. It decouples **format** (how bytes are laid out)
from **target** (where bytes go), so the same `BinaryWriteArchive` can
write into a memory buffer, an fd, a TCP channel, or a hash without
copying through `Marshal` first.

```cpp srpc-no-compile
#include <rrr/misc/serializable.hpp>

// Sink: holds the bytes (Layer 1 — concrete; Layer 2 — `pro::proxy`)
rrr::BufferSink sink;

// Archive: knows the wire format (Layer 3)
rrr::BinaryWriteArchive writer(&sink);
rrr::Serialize_::serialize((rrr::i32)42, writer);
rrr::Serialize_::serialize(std::string("hello"), writer);

// Source: drains from a byte view
rrr::BufferSource source(sink.bytes.data(), sink.bytes.len());
rrr::BinaryReadArchive reader(&source);
rrr::i32 x; std::string s;
reader >> x >> s;
```

The archive supports the same primitive set as `Marshal`
(`int8..int64`, `uint8..uint64`, `double`, `v32`, `v64`, `std::string`,
`std::string_view`) plus the same containers (`std::pair`,
`rusty::Vec`, `std::vector`, `std::list`, `std::set`,
`std::unordered_set`, `std::map`, `std::unordered_map`, plus
`rusty::BTreeSet`, `rusty::HashSet`, `rusty::BTreeMap`,
`rusty::HashMap`).

Wire format is **byte-for-byte identical** to `Marshal`'s output for
all overlapping types — switching transports does not change the
on-the-wire bytes.

Sinks ship for in-memory buffers (`BufferSink`) and for raw file
descriptors (`FdSink` / `FdSource`). The fd variants drive a
synchronous full-write / full-read loop with EINTR retry — useful for
log replay paths, snapshots, or any consumer that wants to bypass the
`Marshal`-based intermediate buffer:

```cpp srpc-no-compile
int fd = ::open("/tmp/snap.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
{
  rrr::FdSink sink(fd);
  rrr::BinaryWriteArchive writer(&sink);
  writer << (rrr::i32)42 << std::string("snapshot payload");
}
::close(fd);
```

#### Polymorphic command types — `SerializableProxy` + registry

For tag-dispatched polymorphism — the role formerly filled by
`Marshallable` + `MarshallDeputy::reg_initializer` before L10f-2
step 5 — Layer 4 of the archive system is `SerializableProxy`:

```cpp srpc-no-compile
struct MyCommand {
  int32_t id;
  std::string name;

  // Required by SerializableFacade.
  static constexpr int32_t kKind = 0xCAFE;
  int32_t kind() const { return kKind; }
  void save(rrr::BinaryWriteArchive& ar) const { ar << id << name; }
  void load(rrr::BinaryReadArchive& ar) { ar >> id >> name; }
};

// Static-initializer registration.
static int _reg =
    rrr::SerializableRegistry::reg<MyCommand>(MyCommand::kKind);

// Read-side: factory create + load.
rrr::SerializableProxy proxy =
    rrr::SerializableRegistry::create(kind_from_wire);
proxy->load(reader);
```

`SerializableProxy::save` emits only the payload bytes (no kind
prefix); the kind tag is framed by the enclosing envelope —
`SerializableEnvelope<TypeList>` (e.g. `janus::Command`) for
closed-set commands, or `rrr::AnyMessage` for open-set graph
payloads.  Both envelopes wrap the proxy as a value member; there is
no `shared_ptr<Marshallable>` storage layer.

#### Open-set polymorphism — `AnyMessage` (Workstream N L7)

For payload types where the universe of values is open (callers may
not know about every possible carried type at compile time, or the
type set evolves independently of a central registry), the open-set
counterpart to the closed-set TypeList pattern lives in
`misc/any_message.hpp`:

```cpp srpc-no-compile
struct GraphPayload {
  int32_t node_count;
  std::string label;
  void save(rrr::BinaryWriteArchive& ar) const { ar << node_count << label; }
  void load(rrr::BinaryReadArchive& ar) { ar >> node_count >> label; }
  int32_t kind() const { return 0; }  // unused — AnyMessage owns the tag
};

// Register under a stable string name (anywhere, at static init).
static int _reg = rrr::reg_any_message_as<GraphPayload>("my.GraphPayload");

// Sender: pack typed value into AnyMessage and ride it directly on
// an RPC field of type `rrr::AnyMessage`.
auto val = std::make_shared<GraphPayload>();
val->node_count = 42;
val->label = "x";
rrr::AnyMessage outgoing = *rrr::AnyMessage::pack(val);

// Receiver: dispatch by carried type.
if (incoming.is_a<GraphPayload>()) {
  auto p = incoming.unpack<GraphPayload>();
  // ... use p ...
}
```

Wire layout: `[v32: ANY_MESSAGE=24] [v64-prefixed string: type_name]
[payload bytes]`.  The `ANY_MESSAGE` discriminator (24) is the fixed
kind; `type_name` is the runtime discriminator.  After Workstream N L9,
the kind itself rides in 1 byte (v32 single-byte range covers values
0-63), so the envelope's framing overhead is 1 byte (v32 kind) +
length-prefixed type_name string.  Aliasing semantics: mutations on
the caller's `shared_ptr<T>` after `pack` remain visible to the
encoded payload (the holder-shaped proxy retains the original
refcount).

When to choose AnyMessage vs. closed-set TypeList:
- **AnyMessage** for graph / data payloads (`RccGraph`, `EmptyGraph`),
  versioned schemas, or any case where a service that doesn't know
  about a new type can still receive and dispatch the envelope. The
  Rust analogue is `typetag` (string tags via the `inventory` pattern);
  the protobuf analogue is `google.protobuf.Any` (type-URL).
- **Closed-set TypeList** for command types where the full set is
  known up-front and wire compactness matters. The Rust analogue is
  `enum Foo { ... }` + bincode (declaration-order discriminants); the
  protobuf analogue is `oneof` (field numbers).

#### Closed-set polymorphism — TypeList + MakoCommands (Workstream N L8/L9)

Command types where the universe is closed (the receiver knows the
full set at compile time, and wire compactness matters more than
extensibility) live in the `janus::MakoCommands` TypeList in
`src/deptran/mako_commands.h`. Each type's wire kind = its 1-indexed
position in the list, derived via the `Serializable<T, MakoCommands>`
CRTP base — no per-type kind constant, no central int enum, no
hashing.

```cpp srpc-no-compile
// Inherit Serializable<T, MakoCommands> for kind() / static_kind()
// from the TypeList position.
class TpcCommitCommand : public rrr::Serializable<TpcCommitCommand,
                                                  janus::MakoCommands> {
 public:
  txnid_t tx_id_ = 0;
  janus::Command cmd_{};   // nested polymorphic field rides Command directly

  void save(rrr::BinaryWriteArchive& ar) const;
  void load(rrr::BinaryReadArchive& ar);
};

// Register at static init — the no-arg overload picks up the kind
// from `T::static_kind()` (the TypeList position).
static int _reg = rrr::SerializableRegistry::reg<TpcCommitCommand>();
```

Adding a new closed-set Command type:
1. Add a forward declaration to `src/deptran/mako_commands.h`.
2. Append the type at the END of the `MakoCommands` TypeList there
   (appending preserves existing types' kind values; reordering or
   inserting in the middle is a wire-format break).
3. Define `class T : public rrr::Serializable<T, janus::MakoCommands>`.
4. Add `static int _reg = rrr::SerializableRegistry::reg<T>();` in
   T's .cc.

After Workstream N L9, the `Command::kind_` field serializes as
`rrr::v32` (variable-length SparseInt) on the wire instead of raw
4-byte int32.  SparseInt's first-byte encoding has a 6-bit signed
payload, so values in [-64, 63] fit in 1 byte.  With closed-set kinds
in [1, 19] and ANY_MESSAGE=24, every production polymorphic envelope
saves 3 bytes for the kind prefix vs. the pre-L9 4-byte encoding —
non-trivial savings on the Paxos LogEntry / TpcCommit / heartbeat
hot path.

#### Marshal ↔ Archive bridges (transitional)

For incremental migration, the new system bridges the existing
`Marshal` buffer abstraction. `MarshalSink` lets new
`BinaryWriteArchive`-based code emit bytes directly into a Marshal
that legacy code is also writing to:

```cpp srpc-no-compile
rrr::Marshal m;
rrr::Serialize_::serialize(static_cast<rrr::i32>(1), m);   // legacy

{
  rrr::MarshalSink sink(&m);
  rrr::BinaryWriteArchive writer(&sink);
  writer << static_cast<rrr::i32>(2);  // new code, same buffer
}

rrr::Serialize_::serialize(std::string("trailing"), m);  // legacy
```

`MarshalSource` is the dual: a `BinaryReadArchive` over a
`Marshal` that was filled by the legacy `operator<<` path decodes
the bytes exactly as `Marshal::operator>>` would (Phase 1's
byte-for-byte commitment). Use these adapters when integrating the
new archive layer with the existing TCP TX/RX path.

#### Marshallable ↔ Serializable bridge adapter (retired)

The `marshal_serializable_bridge.hpp` header (528 LOC) carried a
`SerializableMarshallableAdapter` that wrapped a
`pro::proxy<SerializableFacade>` so it satisfied the legacy
`Marshallable` virtual interface; together with
`as_marshallable`, `wrap_typed_marshallable`,
`wrap_serializable[_aliased]`, `marshallable_cast<T>` /
`serializable_cast<T>` overloads, and
`reg_serializable_in_deputy<T>`, it was the migration-period glue
that let Serializable types flow through APIs typed against
`shared_ptr<Marshallable>`.  Workstream N L10f-2 step 5 retired the
entire bridge — every production payload registers via
`SerializableRegistry::reg<T>()` and rides `janus::Command` /
`rrr::AnyMessage` directly.

If you encounter a stale reference to one of these names in third-
party docs or comments, the modern equivalent is:

| Retired symbol                         | Replacement                                      |
|----------------------------------------|--------------------------------------------------|
| `reg_serializable_in_deputy<T>(kind)`  | `SerializableRegistry::reg<T>(kind)`             |
| `reg_serializable_in_deputy<T>()`      | `SerializableRegistry::reg<T>()` (TypeList kind) |
| `wrap_typed_marshallable<T>(sp)`       | `Command{sp}` or `Command::pack_aliased<T>(sp)`  |
| `wrap_serializable_aliased(proxy)`     | `Command::pack_aliased<T>(sp)`                   |
| `as_marshallable(proxy)`               | (gone — use `Command` value type)                |
| `marshallable_cast<T>(md)`             | `cmd.unpack<T>()` / `unpack_shared<T>()`         |
| `serializable_cast<T>(md)`             | `cmd.unpack<T>()`                                |
| `MarshallDeputy(shared_ptr<T>)`        | `Command{sp}`                                    |
| `md.set_marshallable(sp)`              | `cmd = sp` (Command's templated `operator=`)     |
| `Command::inner_marshallable()`        | `cmd.unpack_shared<T>()` (typed)                 |

#### `rpcgen --archive` emission (default ON)

`bin/rpcgen` emits BinaryWriteArchive / BinaryReadArchive operator<<>>
overloads alongside the existing Marshal& ones for generated headers.
Both forms compile and produce identical wire bytes.  As of Phase 3e
(2026-04-29) this is the default — every in-tree `.rpc` file now
references types with archive operators (`janus::Command` /
`rrr::AnyMessage` via the L10c/L10f migrations, `Value` /
`SimpleCommand` / `TxWorkspace` via Phase 4d-6, `TxReply` and
`ParentEdge<RccTx>` via Phase 3e), so the additive emission compiles
cleanly.  Use `--no-archive` to opt out (e.g. when generating against
a custom `.rpc` that uses user types without archive overloads).

The four in-tree generated headers (`rcc_rpc.h`, `helloworld.h`,
`network.h`, `benchmark_service.h`) all carry archive operators;
`rpcgen_compile_test.py` exercises both modes.

Status: the archive layer landed in five broad strokes.

1. **Foundation (Phases 1–3)** — primitive / container archive ops,
   `SerializableProxy` + registry, `rpcgen --archive` (now the
   default), `Marshal↔Archive` adapters (`MarshalSink` / `MarshalSource`),
   archive ops on `Value` / `SimpleCommand` / `TxWorkspace` /
   `TxReply` / `ParentEdge<RccTx>` so every in-tree `.rpc` references
   archive-aware types.
2. **Reactor TX/RX migration (Phase 3d)** — channel-mode response
   demux moved to `BufferSource`, request/reply lambdas typed as
   `BinaryWriteArchive&`, `DeferredReply`'s stored callback flipped
   to `rusty::Function<void(BinaryWriteArchive&)>`, the Python C
   extension migrated, and `if constexpr` dual-signature dispatch
   collapsed back to a single archive-only path.  Every write-side
   caller in the production path now uses `BinaryWriteArchive&`.
3. **Per-payload Serializable migration (Phase 4)** — every in-tree
   deptran payload (`EmptyGraph`, the simple paxos control types,
   `procedure.h` types, `SyncLogResponse`, `RccGraph`, `VecPieceData`,
   `LogEntry`, `BulkPaxosCmd`, `SimpleRWCommand`,
   `ReplicatedDBCommand`, the TPC commands) defines `save` / `load` /
   `kind` directly with no `Marshallable` inheritance.
4. **Dead-code sweep (Phase 5)** — the bypass-to-socket fast path,
   `MarshallableProxy` / `MarshallableSharedPtrAdapter`,
   `TypedMarshallableAdapter`, the `Marshallable::entity_size` /
   `write_to_fd` virtuals, the `RPC_STATISTICS` marshal-out / marshal-
   in counters, `MarInitializerState`, and dead
   `CmdData::to_marshal` / `from_marshal` overrides — ~1410 LOC of
   transitional / dead infrastructure removed.
5. **Workstream N L10 — Marshallable + MarshallDeputy retirement.**
   - **L10c** retired the `MarshallDeputy` envelope from production
     polymorphic command fields: graphs migrated to `AnyMessage`
     (L10c-graphs), and the closed-set `[v32 kind][payload]` envelope
     became `janus::Command` (`SerializableEnvelope<MakoCommands>`).
   - **L10d-prep** deleted the lazy `MarshallDeputy::serializable()`
     cache and the `MarshallableSerializableAdapter` reverse-direction
     bridge (~622 LOC).
   - **L10f-2 step 5** (the final cut, 2026-05-05) flipped
     `Command::inner_` and `AnyMessage::payload_` from
     `shared_ptr<Marshallable>` to `pro::proxy<SerializableFacade>`
     value members; retired `marshal_serializable_bridge.hpp` (528
     LOC); deleted the `Marshallable` abstract base and the
     `MarshallDeputy` concrete envelope from `marshal.hpp` /
     `marshal.cpp` (~500 LOC); migrated every
     `reg_serializable_in_deputy<T>` callsite to
     `SerializableRegistry::reg<T>()`; flipped raft's in-process
     `AppendEntriesReq.cmd` field type from `MarshallDeputy` to
     `::janus::Command`.

Result: every payload type in the tree implements `save` / `load` /
`kind` directly; closed-set polymorphism rides
`SerializableEnvelope<TypeList>` (typed as `janus::Command` for
Mako); open-set polymorphism rides `rrr::AnyMessage`; both envelopes
back their storage with `pro::proxy<SerializableFacade>` value
members and have no `shared_ptr<Marshallable>` layer.  The only
`Marshal&` references that remain in source are the intentional
byte-format-parity tests in `rpc_marshal_archive_test.cc` and the
`Marshal` buffer abstraction itself (still used as the underlying
storage that `MarshalSink` / `MarshalSource` adapt to the archive
API).

For deeper background see
[`docs/dev/marshal_archive_design.md`](dev/marshal_archive_design.md)
and [`docs/dev/l10f-2-command-inner-design.md`](dev/l10f-2-command-inner-design.md).

---

## 11. Reliability Features

RRR includes comprehensive fault tolerance for production deployments.

### Implemented vs Planned (Shipping Status)

The table below reflects the current shipping headers under `src/rrr/rpc`:

| Capability | Status | Shipping API |
| --- | --- | --- |
| Connection state machine | Implemented | `ConnectionState` transitions in `ClientConnection` |
| Auto reconnect with backoff/jitter | Implemented | `ReconnectPolicy` (`max_retries`, `initial_delay_ms`, `jitter_enabled`) |
| Request buffering during disconnects | Implemented | `BufferingConfig`, `DisconnectBehavior`, `OverflowStrategy` |
| Circuit breaker fail-fast | Implemented | `CircuitBreakerConfig`, `CircuitBreaker` |
| Heartbeat timeout detection | Implemented | `HeartbeatConfig`, `HeartbeatManager` |
| Server restart auto-detection | Implemented | Response header extension + `client.set_on_server_restart(...)` |
| Lifecycle callbacks | Implemented | `client.add_on_connected` / `add_on_disconnected` / `add_on_error` / `add_on_reconnecting` / `add_on_reconnected` |
| Connection reliability metrics | Implemented | `ConnectionMetrics` (`in_flight_requests`, retries/timeouts/reconnects) |
| Planned-only reliability APIs in this chapter | Planned | None currently; new entries will be marked `Planned` until headers and tests land |

### Connection State Machine

```
NEW --> CONNECTING --> CONNECTED --> DISCONNECTING --> DISCONNECTED
              |             |                              |
              +-> FAILED <--+          FAILED <-----------+
                    |                    |
                    +-- (reconnect) -----+
```

States tracked via `rusty::Cell<ConnectionState>` for thread-safe interior mutability.

### Automatic Reconnection

```cpp srpc-compile
ReconnectPolicy policy;
policy.max_retries = 10;            // 0 for unlimited
policy.initial_delay_ms = 100;      // Initial delay
policy.max_delay_ms = 30000;        // Max delay (30s)
policy.backoff_multiplier = 2.0;    // Exponential backoff
policy.jitter_enabled = true;       // Randomize delay to avoid herd effects
```

### Circuit Breaker

Prevents cascade failures using the CLOSED/OPEN/HALF_OPEN pattern:

```cpp srpc-compile
CircuitBreakerConfig cb;
cb.failure_threshold = 5;       // Open after 5 consecutive failures
cb.success_threshold = 2;       // Close after 2 successes in half-open
cb.timeout_ms = 5000;           // Try again after 5 seconds
```

### Request Buffering

Queue requests during temporary disconnections:

```cpp srpc-no-compile
BufferingConfig buffering;
buffering.behavior = DisconnectBehavior::QUEUE;  // or FAIL_FAST
buffering.max_pending = 1000;
buffering.default_ttl_ms = 5000;  // Expire after 5 seconds
buffering.overflow = OverflowStrategy::DROP_OLDEST;
// Options: DROP_OLDEST, DROP_NEWEST, FAIL_FAST
```

Queued requests are automatically replayed when the connection is restored.

### Heartbeat / Keep-Alive

Detect stale connections:

```cpp srpc-compile
HeartbeatConfig heartbeat;
heartbeat.interval_ms = 5000;   // Ping every 5 seconds
heartbeat.timeout_ms = 15000;   // Dead after 15 seconds of silence
```

### Connection Metrics

Track per-connection performance:

```cpp srpc-compile-client
const ConnectionMetrics& metrics = client.metrics();
auto sent = metrics.requests_sent();
auto completed = metrics.requests_completed();
auto timed_out = metrics.requests_timed_out();
auto in_flight = metrics.in_flight_requests();
auto avg_latency_us = metrics.avg_latency_us();
auto reconnects = metrics.reconnect_count();
auto retries = metrics.retry_attempts();
auto queue_drops = metrics.queue_dropped_requests();
auto circuit_rejects = metrics.circuit_open_rejections();
auto circuit_open = metrics.circuit_open_transitions();
auto circuit_half_open = metrics.circuit_half_open_transitions();
auto circuit_closed = metrics.circuit_closed_transitions();
```

### Connection Callbacks

Hook into connection lifecycle events:

```cpp srpc-compile-client
client.add_on_connected([]() { /* ... */ });
client.add_on_disconnected([]() { /* ... */ });
client.add_on_error([](RpcError err, const std::string& msg) { /* ... */ });
client.add_on_reconnecting([]() { /* ... */ });
client.add_on_reconnected([](bool success) { /* ... */ });
```

### Error Types

Structured error categories:

```cpp srpc-no-compile
enum class RpcError {
    // Connection errors
    NOT_CONNECTED, CONNECTION_REFUSED, CONNECTION_RESET,
    // Protocol errors
    INVALID_MESSAGE, UNKNOWN_RPC_ID, MARSHALLING_ERROR,
    // Application errors
    RPC_FAILED, SERVICE_UNAVAILABLE, INVALID_ARGUMENT,
    // Timeout errors
    CONNECT_TIMEOUT, REQUEST_TIMEOUT, RESPONSE_TIMEOUT, HEARTBEAT_TIMEOUT,
    // Internal errors
    INTERNAL_ERROR, CIRCUIT_OPEN
};
```

`TOTAL_TIMEOUT` is represented by `TimeoutType::TOTAL_TIMEOUT` in `request_options.hpp`.
SRPC handles failures through `RpcError` values and helper predicates rather
than an RPC-specific exception class.

---

## 12. Service Definition and Code Generation

### Service Definition Language

Define RPC services in `.rpc` files:

```
// my_service.rpc

// Custom struct
struct UserInfo {
    i32 id;
    string name;
    double balance;
};

// Service definition
service MyService {
    // 'defer' = deferred reply API (you decide when to reply)
    defer get_user(i32 id | UserInfo user);

    // 'fast' = handled on network thread (low latency)
    fast ping(| i32 status);
};
```

### RPC Execution Attributes (`fast`, `defer`, `fiber`, `async`)

These are server-side execution attributes in `.rpc` files. They control how
the generated server wrapper runs your handler.

Important distinction:
- Server-side `async` (IDL attribute) is about handler execution style.
- Client-side `async_Method(...)` / `await_Method(...)` proxy APIs are generated
  for non-raw RPCs regardless of server attribute.

```srpc-no-compile
service ExecModes {
    fast   ping(| i32 ok);
    defer  write(i64 key, string val | i32 status);
    fiber  lock_then_read(i64 key | string val);
    async  fetch_remote(i64 key | string val);
    read_local(i64 key | string val);  // default (no attribute)
};
```

Generated server handler signatures:

```cpp srpc-no-compile
// fast / default / fiber
virtual Result<RpcPingResponse, i32> ping(const RpcPingRequest& req);
virtual Result<RpcReadLocalResponse, i32> read_local(const RpcReadLocalRequest& req);
virtual Result<RpcLockThenReadResponse, i32> lock_then_read(const RpcLockThenReadRequest& req);

// defer
virtual void write(const RpcWriteRequest& req,
                   RpcWriteResponse& resp,
                   rrr::DeferredReply defer);

// async (server-side stackless task)
virtual rusty::Task<Result<RpcFetchRemoteResponse, i32>>
fetch_remote(const RpcFetchRemoteRequest& req);
```

Execution model summary:

| Attribute | Registration path | Handler runtime model | Reply model | Typical use |
|-----------|-------------------|-----------------------|-------------|-------------|
| `fast` | `reg_fast_rpc` | Runs directly on network/poll thread | Immediate typed `Result` | Very small, non-blocking handlers; minimum overhead |
| `defer` | `reg_rpc` | Entered from regular RPC path; handler receives `DeferredReply` token | Explicit `defer.reply()` / `defer.reply_error()` when ready | Work whose reply is naturally delayed (callbacks, external completion) |
| `fiber` | `reg_rpc` + generated `Fiber::create_run(...)` in wrapper | Runs in a stackful fiber context | Immediate typed `Result` from that fiber | Logic that benefits from `this_fiber::yield()` / event waits |
| `async` | `reg_fast_rpc` + `spawn_stackless_task_with_result(...)` | C++20 stackless coroutine (`rusty::Task`) polled by reactor | Reply when task becomes ready | `co_await`-style flow without stackful fiber overhead |

Notes:
- In current implementation, both `fast` and `async` enter from the fast
  dispatch path (network thread), while default / `defer` / `fiber` enter from
  the regular RPC dispatch path.
- `async` handlers that complete on first poll can reply inline; pending tasks
  are resumed by reactor wakeups.
- `defer` provides deferred reply semantics; it does not implicitly guarantee
  thread-pool offload by itself.

### Code Generation

The `rpcgen` tool generates client and server stubs:

```bash srpc-no-compile
# Generate C++ code from .rpc definition
bin/rpcgen --cpp my_service.rpc
```

This produces:
- Server dispatch skeleton
- Marshal/unmarshal code for custom structs
- Per-method typed scaffolding structs (`Rpc<MethodPascalCase>Request`/`Rpc<MethodPascalCase>Response`) synthesized from RPC input/output lists
- Typed service signatures for non-raw methods:
  `Result<MethodResponse, rrr::i32> Method(const MethodRequest&)`
- Typed proxy sync/async APIs for non-raw methods:
  `Method(const MethodRequest&)` and
  `async_Method(const MethodRequest&, const FutureAttr&)`
- Typed proxy coroutine APIs for non-raw methods:
  `await_Method(const MethodRequest&, const FutureAttr&)` and
  `co_await` support on `MethodTypedFuture`

Current codegen is typed-only for non-raw RPC methods:
- Generated service wrappers decode `MethodRequest`, call the typed handler, and
  map `Err(i32)` to RPC error replies.
- Generated proxy sync/async methods use typed request/response objects end-to-end.
- `raw` handlers remain raw (`void Method(Box<Request>, WeakServerConnection)`).
- Generated service classes do not inherit `rrr::Service`. They register via
  `Server::reg_service(Box<T>)` using the `ServiceLike` concept.
- All in-tree generated headers (`rcc_rpc.h`, `network.h`, `helloworld.h`) use
  typed-only mode and all callsites use typed APIs.

### Generated Client Usage

```cpp srpc-no-compile
MyServiceProxy proxy(client.get());

// Synchronous typed call
MyServiceProxy::RpcGetUserRequest req;
req.id = 1001;
auto result = proxy.get_user(req);
if (result.is_ok()) {
    auto resp = result.unwrap();
    // resp.user populated
}

// Asynchronous typed call
auto fu_result = proxy.async_get_user(req);
if (fu_result.is_ok()) {
    auto resolved = fu_result.unwrap().resolve();
    if (resolved.is_ok()) {
        auto resp = resolved.unwrap();
        // resp.user populated
    }
}

// Coroutine typed call (C++20 co_await)
rusty::Task<void> get_user_async(MyServiceProxy& proxy, const MyServiceProxy::RpcGetUserRequest& req) {
    auto awaited = co_await proxy.await_get_user(req);
    if (awaited.is_ok()) {
        auto resp = awaited.unwrap();
        (void)resp;
    }
    co_return;
}
```

### Typed Request/Response API

The interface style is one request type plus one response type per non-raw RPC
method. This removes out-parameters from the generated public API and matches
common RPC APIs (gRPC/Thrift style).

IDL ergonomics remain simple: users still list primitive output fields in
`.rpc`; `rpcgen` synthesizes request/response structs automatically.

```cpp srpc-no-compile
struct GetUserRequest {
    i32 id;
};

struct GetUserResponse {
    UserInfo user;
};

template <typename T>
using RpcResult = rusty::Result<T, rrr::i32>;

class MyServiceService {
public:
    // Service boundary (no output pointers)
    virtual RpcResult<GetUserResponse> get_user(const GetUserRequest& req) = 0;
};

class MyServiceProxy {
public:
    // Client boundary
    RpcResult<GetUserResponse> get_user(const GetUserRequest& req);
};
```

---

## 13. Threading and Synchronization

### SpinLock and SpinMutex

For low-latency critical sections:

```cpp srpc-no-compile
// SpinLock - raw lock
SpinLock lock;
lock.lock();    // Busy-wait until acquired
lock.unlock();  // Release

// SpinMutex<T> - Rust-like RAII wrapper
SpinMutex<int> counter(0);
{
    auto guard = counter.lock().unwrap();
    *guard += 1;  // Access through guard
}  // Auto-released on scope exit
```

### When to Use What

| Primitive | When |
|-----------|------|
| No synchronization | Within a single reactor (fibers are cooperative) |
| `SpinMutex<T>` | Very short critical sections across threads |
| `rusty::Mutex<T>` | Longer critical sections (OS-level, may sleep) |
| `rusty::Cell<T>` | Single-thread interior mutability (trivially copyable) |
| `rusty::RefCell<T>` | Single-thread interior mutability (complex types) |
| `rusty::Arc<T>` | Shared ownership across threads |
| `rusty::Rc<T>` | Shared ownership within one thread |

### Thread Safety Guidelines

1. **Fibers in the same reactor**: No synchronization needed
2. **Cross-reactor communication**: Use mpsc channels or Arc<Mutex<T>>
3. **Poll thread <-> main thread**: Use PollThread command channel (mpsc)
4. **Never share Reactor, Event, or Rc<Fiber> across threads**

---

## 14. Memory Safety (RustyCpp)

All new RRR code must use RustyCpp types and annotations.

### Required Types

| Use | Instead of | Purpose |
|-----|-----------|---------|
| `rusty::Box<T>` | `std::unique_ptr<T>` | Single ownership |
| `rusty::Arc<T>` | `std::shared_ptr<T>` | Thread-safe shared |
| `rusty::Rc<T>` | `std::shared_ptr<T>` | Single-thread shared |
| `rusty::Cell<T>` | mutable field | Interior mutability (Copy) |
| `rusty::RefCell<T>` | mutable field | Interior mutability (complex) |
| `SpinMutex<T>` | `std::mutex` + raw access | Mutex with RAII guard |

### Annotations

```cpp srpc-no-compile
// @safe - No side effects, borrow-checked
bool is_connected() const { return state_.get() == CONNECTED; }

// @unsafe - Calls non-borrow-checked code
void write_to_socket() {
    ::write(fd_, buf, len);  // @unsafe { POSIX I/O }
}
```

### Interior Mutability Patterns

```cpp srpc-no-compile
class ServerConnection {
    // Trivially copyable -> Cell
    rusty::Cell<ConnectionState> state_{ConnectionState::NEW};

    // Complex type, single-threaded -> RefCell
    rusty::RefCell<Marshal> output_buf_;

    // Complex type, multi-threaded -> SpinMutex
    SpinMutex<Marshal> shared_output_;
};
```

### Weak References for Cycle Prevention

Events hold weak references to fibers to avoid reference cycles:

```cpp srpc-no-compile
// Event -> Weak<Fiber> (not Rc<Fiber>)
// If the fiber is destroyed, the weak ref expires gracefully
```

### Inline Rust DSL

Most newly-migrated rrr types are authored as inline Rust DSL blocks
that the `rusty-cpp-transpiler` (third-party/rusty-cpp) regenerates
into C++ at build time. The pattern:

```cpp srpc-no-compile
// 1. Source-of-truth Rust block, guarded so a stock C++ compiler skips it.
#if RUSTYCPP_RUST
struct MyConfig {
    timeout_ms: i32,
    retries: i32,
}

impl MyConfig {
    fn new(timeout_ms: i32, retries: i32) -> MyConfig {
        MyConfig { timeout_ms, retries }
    }
}
#endif

// 2. Transpiler-emitted C++ inside a GEN region. The `rust_sha256`
//    field captures the Rust block's hash so stale GEN output is
//    detectable.
/*RUSTYCPP:GEN-BEGIN id=mymod.myconfig version=1 rust_sha256=...*/
struct MyConfig {
    int32_t timeout_ms;
    int32_t retries;

    static MyConfig new_(int32_t timeout_ms, int32_t retries);
};

inline MyConfig MyConfig::new_(int32_t timeout_ms, int32_t retries) {
    return MyConfig{.timeout_ms = timeout_ms, .retries = retries};
}
/*RUSTYCPP:GEN-END id=mymod.myconfig*/
```

Regenerate after editing the Rust block:

```bash srpc-no-compile
third-party/rusty-cpp/target/release/rusty-cpp-transpiler \
    inline-rust --rewrite --files src/rrr/path/to/file.cpp
```

What the DSL currently expresses, in rough order of usage:

- **Plain structs / POD aggregates** — fields plus an `impl T { fn new(...) -> T { T { ... } } }` static factory.
- **`pub trait T { fn method(&self) -> ...; }`** — emits a C++ abstract base class (pure virtual, virtual dtor, copy/move disabled) so concrete C++ implementors keep working unchanged. Examples: `Pollable`, `PollableBase`, `SerializableBase`, `Service`, `Job`, `Alarm`, `SinkBase`, `SourceBase`.
- **Concrete classes with state + methods** — `Client`, `Server`, `CircuitBreaker`, `HeartbeatManager`, etc. The DSL impl block carries the method bodies; large method bodies stay in out-of-class `T::method(...) { ... }` C++ definitions because the DSL doesn't translate complex syscall / cast-heavy code yet.

Constructs the DSL grammar does **not** accept (these stay manual C++ and show up as `needs-transpiler` or `trivial-blocked` in `docs/rrr-inventory.md`):

- `void*` / `va_list` / C-style array params
- Template methods (and class templates beyond a couple of pilot shapes)
- Default-argument syntax on member functions
- Operator overloading
- Custom destructors that aren't trivially-defaulted
- `impl Trait for Type` — parses but the emitter does not yet write `: public Trait` + `override` for the implementor, so trait *implementors* stay manual C++ while the *trait base* migrates cleanly.

The `tools/rrr-inventory.py` script scans `src/rrr` and produces a
per-decl bucket (trivial / trivial-blocked / refactor-then-dsl /
needs-transpiler / boundary / already-dsl) along with a blocker
histogram — see `docs/rrr-inventory.md`.

---

## 15. Performance Tuning

### RPC Benchmark Tool

```bash
# Build benchmark binary
cmake --build build --target rpcbench -j

# Terminal 1: start one server
./build/rpcbench -s 127.0.0.1:18848 -f

# Terminal 2 (topology A): one client process with 10 client threads
./build/rpcbench -c 127.0.0.1:18848 -f -t 10 -n 10 -o 1000

# Terminal 2 (topology B): 10 independent client processes
for i in $(seq 1 10); do
  ./build/rpcbench -c 127.0.0.1:18848 -f -t 1 -n 10 -o 1000 &
done
wait
```

### Measured Run (1 Server, 10 Clients, Fast Mode)

Run date (UTC): `2026-04-18T01:04:56Z`

Environment:

| Parameter | Value |
|-----------|-------|
| Host kernel | Linux 6.17.4-2-pve x86_64 |
| CPU | AMD Ryzen Threadripper 2990WX 32-Core Processor |
| Logical CPUs | 64 |
| Benchmark binary | `build/rpcbench` |

Benchmark config from `rpcbench` logs:

| Parameter | Value |
|-----------|-------|
| Server address | `127.0.0.1:18848` |
| Duration (`-n`) | 10 seconds |
| Packet byte size (`-b`) | 10 |
| Epoll instances (`-e`) | 2 |
| Outgoing requests (`-o`) | 1000 |
| Worker threads (`-w`) | 16 |
| Fast mode (`-f`) | true |
| Vector mode (`-v`) | 0 |

Throughput results:

1. One client process, 10 client threads (`-t 10`)
- Per-second QPS samples: `2355764, 2184594, 2335724, 2253167, 2124776, 2209057, 2148797, 2161505, 2213496`
- Average QPS: `2220764.50`
- Min/Max sampled QPS: `2124776 / 2355764`
- Server max CPU (`pidstat`, `%CPU`): `82.00`

2. Ten client processes, each with one client thread (`10 x (-t 1)`)
- Aggregate average QPS (sum of 10 client averages): `2323925.64`
- Per-client average QPS range: `229820.88 .. 235125.00`
- Server max CPU (`pidstat`, `%CPU`): `86.00`

Result summary:
- After the fiber-reuse fix in the client callback path, single-process (`-t 10`)
  is close to multi-process throughput (~4.7% lower), indicating the prior process-local
  bottleneck is largely removed.

### Await API Benchmark (`-t 10`, `-o 1`, 60s Trials)

Run date (UTC): `2026-04-18`

Measured topology:
- One server process (`rpcbench -s ... -f`)
- One client process with 10 client threads (`-t 10`)
- Single outstanding RPC per client thread (`-o 1`)
- Duration per trial (`-n`) = 60s
- Compared client modes:
  - callback mode (default)
  - await mode (`-a`)

Server CPU measurement:
- `pidstat -u -p <server_pid> 1`
- Reported as average/max `%CPU` over the run

| Mode | Trial | Throughput (avg qps) | Server CPU avg % | Server CPU max % |
|------|-------|-----------------------|------------------|------------------|
| callback | 1 | `69842.22` | `96.79` | `101.00` |
| callback | 2 | `75653.46` | `96.77` | `101.00` |
| await (`-a`) | 1 | `70659.36` | `96.77` | `101.00` |
| await (`-a`) | 2 | `72430.24` | `96.76` | `101.00` |

Aggregate summary:
- Callback mean throughput: `72747.84 qps`
- Await mean throughput: `71544.80 qps`
- Await vs callback: `-1.65%`
- Server CPU utilization is effectively identical between modes in this setup.

### Await API Benchmark (`-t 10`, `-o 1000`, 60s Trials)

Run date (UTC): `2026-04-18`
Code commit (HEAD): `3080e880e17f685b04bb72d9e930ae318191bcdd`

Measured topology:
- One server process (`rpcbench -s ... -f`)
- One client process with 10 client threads (`-t 10`)
- 1000 outstanding RPC per client thread (`-o 1000`)
- Duration per trial (`-n`) = 60s
- Compared client modes:
  - callback mode (default)
  - await mode (`-a`)

Server CPU measurement:
- `pidstat -u -p <server_pid> 1`
- Reported as average/max `%CPU` over the run

| Mode | Trial | Throughput (avg qps) | Server CPU avg % | Server CPU max % |
|------|-------|-----------------------|------------------|------------------|
| callback | 1 | `2401442.25` | `96.81` | `101.00` |
| callback | 2 | `2375803.75` | `96.79` | `101.00` |
| await (`-a`) | 1 | `2347543.00` | `96.60` | `101.00` |
| await (`-a`) | 2 | `2406166.25` | `96.63` | `101.00` |

Aggregate summary:
- Callback mean throughput: `2388623.00 qps`
- Await mean throughput: `2376854.63 qps`
- Await vs callback: `-0.49%`
- Server CPU utilization is effectively identical between modes in this setup.

### Mode Comparison Update (10 Client Processes, `-e 1`)

Run date (UTC): `2026-04-18T20:20:06Z`  
Code commit (HEAD): `e3d65943cda0a5b3be014dea35095a6060ab4d02`

Measured topology:
- One server process (`rpcbench -s ... -e 1`)
- Ten independent client processes (`10 x rpcbench -c ... -t 1 -e 1`)
- Modes compared via `-m fast|fiber|async`
- Packet size `-b 10` (default), worker threads `-w 16` (default)
- Duration per trial (`-n`) = 20s
- Server CPU measured with `pidstat -u -p <server_pid> 1`

#### `-o 1` (single outstanding RPC per client process)

| Mode | Trial | Throughput (avg qps, sum of 10 clients) | Server CPU avg % | Server CPU max % |
|------|-------|-------------------------------------------|------------------|------------------|
| fast | 1 | `74095.57` | `99.90` | `101.00` |
| fast | 2 | `70273.97` | `99.85` | `101.00` |
| fiber | 1 | `73691.00` | `99.90` | `101.00` |
| fiber | 2 | `69536.29` | `99.90` | `101.00` |
| async | 1 | `64415.00` | `99.95` | `101.00` |
| async | 2 | `75005.19` | `99.95` | `101.00` |

Aggregate summary (`-o 1`):
- Fast mean: `72184.77 qps`
- Fiber mean: `71613.64 qps` (`-0.79%` vs fast)
- Async mean: `69710.10 qps` (`-3.43%` vs fast, `-2.66%` vs fiber)
- Server CPU is saturated for all modes (~100% avg/max).

#### `-o 1000` (1000 outstanding RPCs per client process)

| Mode | Trial | Throughput (avg qps, sum of 10 clients) | Server CPU avg % | Server CPU max % |
|------|-------|-------------------------------------------|------------------|------------------|
| fast | 1 | `2327315.34` | `99.90` | `101.00` |
| fast | 2 | `2329882.27` | `99.90` | `101.00` |
| fiber | 1 | `1283867.41` | `100.00` | `101.00` |
| fiber | 2 | `1322332.60` | `99.95` | `101.00` |
| async | 1 | `1971063.29` | `99.95` | `101.00` |
| async | 2 | `1987283.78` | `99.95` | `101.00` |

Aggregate summary (`-o 1000`):
- Fast mean: `2328598.80 qps`
- Fiber mean: `1303100.00 qps` (`-44.04%` vs fast)
- Async mean: `1979173.54 qps` (`-15.01%` vs fast, `+51.88%` vs fiber)
- Server CPU is saturated for all modes (~100% avg/max).

### Tuning Parameters

| Parameter | Default | Tuning Advice |
|-----------|---------|---------------|
| Threads per shard | 1 | Match to CPU cores |
| Outstanding requests | 100 | Higher = more throughput, more latency |
| Message size | varies | Smaller = lower latency |
| Keepalive interval | 60s | Lower for faster failure detection |
| Reconnect base delay | 100ms | Lower for faster recovery |
| Circuit breaker threshold | 5 | Higher to tolerate transient failures |
| Request queue size | 1000 | Higher for bursty workloads |

### Fast vs Defer Dispatch

In service definitions:
- **`fast`**: Handler runs on the network thread. Use for trivial operations (ping, status) to minimize scheduling overhead.
- **`defer`**: Handler gets a `DeferredReply` token and decides when to reply (`defer.reply()` / `defer.reply_error()`). Use when completion is delayed by external work.

### Connection Pooling

```cpp srpc-no-compile
PoolConfig pool;
pool.min_connections = 2;     // Pre-warm connections
pool.max_connections = 10;    // Cap per address
pool.idle_timeout_ms = 60000; // Close idle after 1 minute
pool.health_check_interval_ms = 10000;  // Check health every 10s
```

---

## 16. API Reference

### Reactor

```cpp srpc-no-compile
class Reactor {
    static Rc<Reactor> get_reactor();    // Thread-local main reactor
    static Rc<Reactor> get_disk_reactor(); // Thread-local disk reactor

    // Fiber management
    Rc<Fiber> create_run_fiber(Function<void()> f);
    void continue_fiber(Rc<Fiber> fiber);

    // Event creation
    template<typename T, typename... Args>
    static shared_ptr<T> create_sp_event(Args&&... args);

    // Event loop
    void loop(bool forever = false);
};
```

### Fiber

```cpp srpc-no-compile
class Fiber {
    static Rc<Fiber> create_run(Func&& func);
    static Option<Rc<Fiber>> current_fiber();
    void yield_() const;
    void continue_() const;
    bool finished() const;
};
```

### this_fiber Namespace

```cpp srpc-no-compile
namespace this_fiber {
    uint64_t get_id();
    Option<Rc<Fiber>> current();
    bool in_fiber_context();
    void yield();
    void sleep_us(uint64_t microseconds);
    void sleep_ms(uint64_t milliseconds);
    void sleep_s(uint64_t seconds);
    void sleep_until_us(uint64_t absolute_time_us);
}
```

### Event

```cpp srpc-no-compile
class Event {
    enum Status { INIT, WAIT, READY, DONE, TIMEOUT };
    Status status_;
    virtual bool Test() = 0;
    void wait();
    void wait(uint64_t timeout_us);
};
```

### Marshal

```cpp srpc-no-compile
class Marshal {
    Marshal& operator<<(const T& val);   // Serialize
    Marshal& operator>>(T& val);          // Deserialize
    auto set_bookmark(size_t size);        // Reserve space
    void write_bookmark(auto bm, void* data);  // Fill reserved space
    size_t get_and_reset_write_cnt();      // Bytes written since last reset
    size_t content_size() const;           // Total buffered data
};
```

### Future

```cpp srpc-no-compile
class Future {
    static Arc<Future> create();
    void wait();                    // Block fiber
    void timed_wait(double sec);    // With timeout
    rusty::RefMut<ReplyBuffer> get_reply();  // Access reply data (cursor)
    int get_error_code();           // 0 = success
    bool timed_out();               // Did it timeout?
    static void safe_release(...);  // Compatibility no-op (Arc handles lifetime)
};
```

### PollThread

```cpp srpc-no-compile
class PollThread {
    static Arc<PollThread> create();
    void add_proxy(PollableProxy p);
    void remove(Pollable& p);
    void request_close(int fd);
    void update_mode(int fd, int new_mode);
    void shutdown();
};
```

---

## 17. Pitfalls and Best Practices

### Do

- **One reactor per thread** — always use `Reactor::get_reactor()`
- **Yield in long loops** — let other fibers run
- **Use events for coordination** — don't busy-wait
- **Use RAII guards** — `SpinMutexGuard`, `RefCell::borrow()`, etc.
- **Use `Fiber` naming consistently** — in new code
- **Use `this_fiber::sleep_ms()`** — never `std::this_thread::sleep_for()`
- **Close connections gracefully** — use the shutdown phases

### Don't

- **Never access events across threads** — undefined behavior
- **Never reuse events** — create new ones after Wait()
- **Never have multiple waiters on one event** — one fiber per event
- **Never use `std::unique_ptr` / `std::shared_ptr`** — use `rusty::Box` / `rusty::Arc`
- **Never forget the event loop** — fibers that yield without a running reactor are dead
- **Never block a fiber with OS calls** — use async I/O or defer to a thread
- **Never use `std::this_thread::sleep_for()`** — it blocks the entire thread, starving all other fibers

### Common Mistakes

**Fiber starvation:**
```cpp srpc-no-compile
// BAD - blocks all other fibers
reactor->create_run_fiber([]() {
    while (true) {
        compute();  // Never yields!
    }
});

// GOOD - cooperative
reactor->create_run_fiber([]() {
    while (true) {
        compute();
        this_fiber::yield();  // Let others run
    }
});
```

**Cross-thread event access:**
```cpp srpc-no-compile
// BAD
auto event = Reactor::create_sp_event<IntEvent>();
std::thread t([event]() {
    event->Set(1);  // CRASH - wrong thread!
});

// GOOD - use mpsc channel or shared atomic
```

**Forgetting the event loop:**
```cpp srpc-no-compile
// BAD - fiber never executes
reactor->create_run_fiber([]() { /* ... */ });
// No loop() call!

// GOOD
reactor->create_run_fiber([]() { /* ... */ });
reactor->loop(true);
```

---

## 18. Troubleshooting

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Fibers never run | No event loop running | Call `reactor->loop()` |
| Fiber hangs forever | Event never triggered | Add timeout to `Wait()` |
| Segfault in event | Cross-thread access | Keep events thread-local |
| Connection refused | Server not listening | Check port and host config |
| Address already in use | Lingering process | `pkill -9 dbtest; sleep 2` |
| High latency | Fiber starvation | Add yield() in hot loops |
| Memory leak | Missing Arc/Rc cleanup | Use RAII, check weak refs |

### Debugging

```bash
# Debug build
MODE=debug make -j$(nproc)

# Run under GDB
gdb --args ./build/dbtest ...

# Enable verbose RPC logging
export MAKO_LOG_LEVEL=debug

# Check RPC statistics at shutdown
# Look for "msg_size_req_sent", "msg_counter_req_sent" in logs
```

### Shutdown Issues

If the process hangs during shutdown, check the transport stop fix documentation (`docs/developer/transport-stop-fix.md`). Common causes:

1. **Non-atomic stop flag** — use `std::atomic<bool>`
2. **Non-idempotent Stop()** — use atomic compare-exchange
3. **In-flight RPCs after stop** — add post-wait stop checks
4. **Missing entry-point guards** — check stop flag before starting new RPCs

---

*This document consolidates the RRR/SRPC framework documentation from across the Mako project. For detailed implementation plans, phase documents, and migration guides, see `docs/rpc/`, `docs/developer/`, and `docs/migration/rustycpp/`.*

*For the ongoing manual-C++ → Rust DSL migration roadmap, see [`docs/TODO-rusty-rewrite.md`](TODO-rusty-rewrite.md) (the phased plan) and [`docs/rrr-inventory.md`](rrr-inventory.md) (the per-decl triage CSV + bucket summary, regeneratable via `python3 tools/rrr-inventory.py`).*
