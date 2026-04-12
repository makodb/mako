# The SRPC Book

A comprehensive developer guide for RRR — the **S**imple **RPC** framework powering Mako.

RRR stands for "Repeatable Research Runtime." It provides high-performance RPC, stackful coroutines (fibers), an event-driven reactor, and binary serialization — all with Rust-inspired memory safety.

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Architecture Overview](#2-architecture-overview)
3. [Fibers (Stackful Coroutines)](#3-fibers-stackful-coroutines)
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
| **Stackful Coroutines** | Lightweight fibers with custom x86_64 assembly context switching |
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
    errors.hpp          # Error code definitions
    utils.hpp/cpp       # RPC utilities

  reactor/            # Event loop and coroutine system
    reactor.h           # Core event loop scheduler (482 lines)
    fiber.h             # Modern fiber API (this_fiber namespace)
    fiber_impl.h/cc     # Fiber implementation + context switching
    fiber_context_x86_64.cc # Assembly context switching
    coroutine.h         # Legacy coroutine interface (alias for Fiber)
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

## 3. Fibers (Stackful Coroutines)

Fibers are the fundamental concurrency primitive in RRR. They are **stackful coroutines** — lightweight execution contexts that can pause and resume from any function depth.

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

**Backward compatibility:** `Coroutine` is an alias for `Fiber`. Use `Fiber` in new code — it more accurately describes our stackful execution model (C++20 "coroutines" are stackless).

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
Recycled (if REUSE_CORO enabled) or Destroyed
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
auto reactor = Reactor::GetReactor();

// Create fibers
reactor->CreateRunCoroutine([]() {
    // Your concurrent task
    this_fiber::yield();  // Let others run
});

// Run event loop
reactor->Loop(true);   // Run forever
reactor->Loop(false);  // Process once
```

### Event Loop Operation

Each iteration of `Reactor::Loop()`:

1. **Process commands** from other threads (add/remove Pollables, Jobs)
2. **epoll_wait()** to get ready file descriptors
3. **Handle I/O** on ready FDs (call `Pollable::handle_read/write`)
4. **Check events** — collect ready events from the waiting queue
5. **Check timeouts** — move expired events to ready queue
6. **Process composite events** (WaitAll, WaitAny, QuorumEvent)
7. **Resume fibers** whose events are ready
8. **Repeat**

### Thread-Local Design (Critical)

```cpp srpc-no-compile
// WRONG - undefined behavior!
std::thread t([reactor]() {
    reactor->CreateRunCoroutine([]() { /* ... */ });  // BAD!
});

// RIGHT - each thread has its own reactor
std::thread t([]() {
    auto reactor = Reactor::GetReactor();  // Thread-local
    reactor->CreateRunCoroutine([]() { /* ... */ });
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
            reactor_->Loop(false);  // Process pending events
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
auto event = Reactor::CreateSpEvent<IntEvent>();
event->target_ = 42;  // Ready when value_ == 42

// In another fiber:
event->Set(42);  // Triggers the event

// In waiting fiber:
event->Wait();  // Resumes when value_ == target_
```

#### TimeoutEvent — Time-based trigger

```cpp srpc-no-compile
auto timeout = Reactor::CreateSpEvent<TimeoutEvent>(1000000);  // 1 second
timeout->Wait();
std::cout << "1 second elapsed!" << std::endl;
```

#### WaitAny (OrEvent) — Any of multiple events

```cpp srpc-no-compile
auto e1 = Reactor::CreateSpEvent<IntEvent>();
auto e2 = Reactor::CreateSpEvent<IntEvent>();
auto any = Reactor::CreateSpEvent<WaitAny>(e1, e2);
any->Wait();  // Continues when EITHER event triggers
```

#### WaitAll (AndEvent) — All events must be ready

```cpp srpc-no-compile
auto e1 = Reactor::CreateSpEvent<IntEvent>();
auto e2 = Reactor::CreateSpEvent<IntEvent>();
auto all = Reactor::CreateSpEvent<WaitAll>(e1, e2);
all->Wait();  // Continues when BOTH events trigger
```

#### WaitN (NEvent) — N of M events

```cpp srpc-no-compile
auto events = {e1, e2, e3, e4, e5};
auto quorum = Reactor::CreateSpEvent<WaitN>(events, 3);
quorum->Wait();  // Continues when 3 of 5 events trigger
```

### Wait with Timeout

```cpp srpc-no-compile
auto event = Reactor::CreateSpEvent<IntEvent>();
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

Any object that needs I/O multiplexing implements `Pollable`:

```cpp srpc-no-compile
class Pollable {
    virtual int fd() const = 0;             // File descriptor
    virtual int poll_mode() const = 0;      // READ, WRITE, or both
    virtual bool handle_read() = 0;         // Called when FD is readable
    virtual int handle_write() = 0;         // Called when FD is writable
    virtual void handle_error() = 0;        // Called on error
    virtual void close() = 0;              // Cleanup
};
```

### Epoll Abstraction

RRR wraps Linux epoll and macOS kqueue behind a unified `Epoll` class:

```cpp srpc-no-compile
class Epoll {
    void add(Pollable* p);        // Register FD for monitoring
    void remove(Pollable* p);     // Unregister FD
    void update(Pollable* p);     // Change poll mode
    int wait(epoll_event* events, int maxevents, int timeout_ms);
};
```

### PollThread / PollThreadWorker

**PollThread** (client-facing handle): Send commands from any thread.

```cpp srpc-no-compile
Arc<PollThread> pt = PollThread::create();
pt->add(Arc<Pollable>(connection));     // Register for I/O
pt->remove(*connection);                // Unregister
pt->request_close(fd);                  // Close and drop
pt->shutdown();                         // Stop poll loop
```

**PollThreadWorker** (internal, runs in dedicated thread): Owns the epoll and all Pollable objects.

```cpp srpc-no-compile
class PollThreadWorker {
    Epoll poll_;
    unordered_map<int, Arc<Pollable>> fd_to_pollable_;
    mpsc::Receiver<PollCommand> receiver_;  // Commands from main thread

    void poll_loop() {
        while (!stop_) {
            process_channel_commands();  // Add/remove/close/update_mode
            epoll_wait(...);
            for each ready fd:
                call handle_read() / handle_write();
            trigger_jobs();
        }
    }
};
```

### Cross-Thread Communication

The poll thread and main thread communicate via an mpsc (multi-producer, single-consumer) channel:

| Command | Description |
|---------|-------------|
| `CmdAddPollable` | Register new Pollable with epoll |
| `CmdRemovePollable` | Remove from epoll (keep Arc alive) |
| `CmdClosePollable` | Remove + drop Arc |
| `CmdUpdateMode` | Change poll_mode for an FD |
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
  |-- ClientConnection.handle_read()                                |
  |-- Match xid to Future                                           |
  |-- Set reply data, signal fiber                                  |
  |-- Fiber resumes with result                                     |
```

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
auto fu_result = client->request(RPC_METHOD_ID, [&](Marshal& m) {
    m << arg1 << arg2;
});

if (fu_result.is_ok()) {
    auto fu = fu_result.unwrap();
    fu->wait();
    int result = 0;
    fu->get_reply() >> result;
}
```

### Async RPC

```cpp srpc-compile-client
auto fu_result = client->request(RPC_METHOD_ID, [&](Marshal& m) {
    m << arg1;
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
        req->m >> arg;
        int result = compute(arg);

        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<ServerConnection&>(*sconn).reply(*req, 0, [&](Marshal& out) {
                out << result;
            });
        }
    }
};
```

### Server Lifecycle

```cpp srpc-compile-server
auto poll_thread = PollThread::create();
Server server(rusty::Some(poll_thread.clone()));
server.reg_service(rusty::make_box<MyService>());

// Start listening
int rc = server.start("0.0.0.0:8100");

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
    // Map rpc_id -> Service implementation
    // Uses rusty::Arc for thread-safe sharing
    // Uses rusty::RefCell for single-threaded dispatch
};
```

---

## 10. Serialization (Marshal)

The `Marshal` class provides binary serialization/deserialization:

### Basic Usage

```cpp srpc-no-compile
Marshal m;

// Serialize
m << (i32)42;
m << (i64)1234567890LL;
m << std::string("hello");
m << (double)3.14;

// Deserialize
i32 x; i64 y; std::string s; double d;
m >> x >> y >> s >> d;
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

### Custom Types (Marshallable)

To serialize custom types, implement `Marshallable`:

```cpp srpc-no-compile
struct MyData : public Marshallable {
    i32 id;
    std::string name;

    Marshal& to_marshal(Marshal& m) const override {
        m << id << name;
        return m;
    }

    Marshal& from_marshal(Marshal& m) override {
        m >> id >> name;
        return m;
    }
};
```

### Bookmarks

For recording sizes without seeking:

```cpp srpc-no-compile
Marshal m;
auto bookmark = m.set_bookmark(sizeof(i32));  // Reserve space
m << data1 << data2 << data3;
i32 payload_size = m.get_and_reset_write_cnt();
m.write_bookmark(bookmark, &payload_size);  // Fill in size
```

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
    // 'defer' = dispatched to thread pool
    defer get_user(i32 id | UserInfo user);

    // 'fast' = handled on network thread (low latency)
    fast ping(| i32 status);
};
```

### Code Generation

The `rpcgen` tool generates client and server stubs:

```bash
# Generate C++ code from .rpc definition
python3 pylib/simplerpc/rpcgen.py my_service.rpc
```

This produces:
- Client proxy class with compatibility pointer-style sync/async methods
- Server dispatch skeleton
- Marshal/unmarshal code for custom structs
- Per-method typed scaffolding structs (`MethodRequest`/`MethodResponse`) synthesized from RPC input/output lists

Current generated C++ service/proxy boundaries still use out-parameters
(`T* out`) for return values in compatibility mode.
Generated services now also expose typed virtual overloads
(`Result<MethodResponse, rrr::i32> Method(const MethodRequest&)`) for non-raw
methods; in compatibility mode those overloads bridge to pointer handlers
(with `defer` methods currently returning `ENOTSUP` until typed async flow lands).
Generated proxies now expose typed sync overloads with the same request/response
shape for non-raw methods; they currently run through the existing async/future
pipeline and return `Err(i32)` on transport or RPC error codes.
Generated proxies now also expose typed async overloads for non-raw methods:
`async_Method(const MethodRequest&, const FutureAttr&)` returns
`Result<MethodTypedFuture, rrr::i32>`, and `MethodTypedFuture::resolve()`
decodes the underlying wire reply into `MethodResponse`.
Legacy pointer-style proxy async/sync signatures remain available as compatibility
wrappers; for non-raw methods they now build typed request structs and delegate
to the typed async/sync overloads.

### Generated Client Usage

```cpp srpc-compile-codegen
MyServiceProxy proxy(client.get());

// Synchronous call
UserInfo user;
if (proxy.get_user(1001, &user) == 0) {
    // user populated
}

// Asynchronous call
auto fu_result = proxy.async_get_user(1001);
if (fu_result.is_ok()) {
    auto fu = fu_result.unwrap();
    fu->wait();
    fu->get_reply() >> user;
}

// The compatibility sync wrapper returns i32 status.
// Generated typed sync and typed async request/response overloads are also available.
```

### Planned Typed Request/Response API (Migration Target)

The target interface style is one request type plus one response type per RPC
method. This removes raw out-parameters from the public generated API and
matches common RPC APIs (gRPC/Thrift style).

IDL ergonomics remain simple: users can still list primitive output fields in
`.rpc`; `rpcgen` should synthesize request/response structs automatically.

```cpp srpc-no-compile
struct GetUserRequest {
    i32 id;
};

struct GetUserResponse {
    UserInfo user;
};

template <typename T>
using RpcResult = rusty::Result<T, rrr::i32>;

class MyServiceService: public rrr::Service {
public:
    // Target service boundary (no output pointers)
    virtual RpcResult<GetUserResponse> get_user(const GetUserRequest& req) = 0;
};

class MyServiceProxy {
public:
    // Target client boundary
    RpcResult<GetUserResponse> get_user(const GetUserRequest& req);
};
```

Migration plan:
- Keep old pointer-style generated signatures as compatibility wrappers.
- Add typed request/response signatures in parallel.
- Migrate callsites incrementally, then retire pointer-style APIs.

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

---

## 15. Performance Tuning

### RPC Benchmark Tool

```bash
# Start server
./build/rpc_bench -s -p 8100

# Run client benchmark
./build/rpc_bench -c -h 127.0.0.1 -p 8100 \
    -t 4           \  # 4 threads
    -n 100         \  # 100 outstanding requests per thread
    -m 64          \  # 64 byte messages
    -d 30             # 30 second duration
```

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
- **`fast`**: Handler runs on the network thread. Use for trivial operations (ping, status) to avoid thread pool overhead.
- **`defer`**: Handler runs in a thread pool. Use for anything that might block or take significant time.

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
    static Reactor* GetReactor();        // Thread-local main reactor
    static Reactor* GetDiskReactor();    // Thread-local disk reactor

    // Fiber management
    Rc<Fiber> CreateRunCoroutine(Func&& f);
    void ContinueCoro(Rc<Fiber> fiber);

    // Event creation
    template<typename T, typename... Args>
    static shared_ptr<T> CreateSpEvent(Args&&... args);

    // Event loop
    void Loop(bool forever = false);
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

// Alias
using Coroutine = Fiber;
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
    Marshal& get_reply();           // Access reply data
    int get_error_code();           // 0 = success
    bool timed_out();               // Did it timeout?
    static void safe_release(...);  // Compatibility no-op (Arc handles lifetime)
};
```

### PollThread

```cpp srpc-no-compile
class PollThread {
    static Arc<PollThread> create();
    void add(Arc<Pollable> p);
    void remove(Pollable& p);
    void request_close(int fd);
    void update_mode(int fd, int new_mode);
    void shutdown();
};
```

---

## 17. Pitfalls and Best Practices

### Do

- **One reactor per thread** — always use `Reactor::GetReactor()`
- **Yield in long loops** — let other fibers run
- **Use events for coordination** — don't busy-wait
- **Use RAII guards** — `SpinMutexGuard`, `RefCell::borrow()`, etc.
- **Prefer `Fiber` over `Coroutine`** — in new code
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
reactor->CreateRunCoroutine([]() {
    while (true) {
        compute();  // Never yields!
    }
});

// GOOD - cooperative
reactor->CreateRunCoroutine([]() {
    while (true) {
        compute();
        this_fiber::yield();  // Let others run
    }
});
```

**Cross-thread event access:**
```cpp srpc-no-compile
// BAD
auto event = Reactor::CreateSpEvent<IntEvent>();
std::thread t([event]() {
    event->Set(1);  // CRASH - wrong thread!
});

// GOOD - use mpsc channel or shared atomic
```

**Forgetting the event loop:**
```cpp srpc-no-compile
// BAD - fiber never executes
reactor->CreateRunCoroutine([]() { /* ... */ });
// No loop() call!

// GOOD
reactor->CreateRunCoroutine([]() { /* ... */ });
reactor->Loop(true);
```

---

## 18. Troubleshooting

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Fibers never run | No event loop running | Call `reactor->Loop()` |
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
