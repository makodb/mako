# RPC Reliability Enhancement Plan

## Overview

This document provides the master plan for enhancing the srpc/rpc module (`src/srpc/rpc/`) to support server/client crash handling, automatic reconnection, and improved fault tolerance.

## Scope

**In Scope**: srpc/rpc module - the TCP-based RPC framework in `src/srpc/rpc/`

**Out of Scope**: eRPC (RDMA backend in `src/mako/lib/erpc_backend.h`) - eRPC has its own session management and reliability mechanisms provided by the eRPC library. The enhancements in this plan are specific to the srpc/rpc TCP transport.

## Current Architecture

### Key Files
| File | Purpose |
|------|---------|
| `src/srpc/rpc/client.hpp` | Client-side RPC: `Future`, `ClientConnection`, `Client`, `ClientPool` |
| `src/srpc/rpc/client.cpp` | Client implementation |
| `src/srpc/rpc/server.hpp` | Server-side RPC: `Service`, `ServerConnection`, `ServerListener`, `Server` |
| `src/srpc/rpc/server.cpp` | Server implementation |
| `src/srpc/reactor/reactor.h` | Poll thread and async I/O management |

### Current Limitations
1. **No automatic reconnection** - client must manually call `connect()` after failure
2. **No message durability** - in-flight messages lost on disconnect
3. **No crash recovery** - no way to detect if request was processed before crash
4. **No health monitoring** - no heartbeat to detect stale connections
5. **Limited error semantics** - errors don't distinguish network issues from server unavailability

## Architecture Design

### New Components

```
┌─────────────────────────────────────────────────────────────────┐
│                         Client Layer                             │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │   Client    │  │ ClientPool  │  │    LoadBalancer         │  │
│  │             │  │ (Enhanced)  │  │ (ROUND_ROBIN, LEAST_*)  │  │
│  └──────┬──────┘  └──────┬──────┘  └───────────┬─────────────┘  │
│         │                │                      │                │
│  ┌──────▼────────────────▼──────────────────────▼─────────────┐  │
│  │              ClientConnection (Enhanced)                    │  │
│  │  ┌────────────────┐  ┌────────────────┐  ┌──────────────┐  │  │
│  │  │ ConnectionState│  │ ReconnectMgr   │  │ CircuitBreaker│ │  │
│  │  │    Machine     │  │                │  │               │  │  │
│  │  └────────────────┘  └────────────────┘  └──────────────┘  │  │
│  │  ┌────────────────┐  ┌────────────────┐  ┌──────────────┐  │  │
│  │  │ RequestQueue   │  │ HeartbeatMgr   │  │ Metrics      │  │  │
│  │  │                │  │                │  │              │  │  │
│  │  └────────────────┘  └────────────────┘  └──────────────┘  │  │
│  └─────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                         Server Layer                             │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────────┐  ┌─────────────────────┐  │
│  │   Server    │  │ ServerListener  │  │ ServerConnection    │  │
│  │ (Enhanced)  │  │                 │  │                     │  │
│  └──────┬──────┘  └────────┬────────┘  └──────────┬──────────┘  │
│         │                  │                       │             │
│  ┌──────▼──────────────────▼───────────────────────▼──────────┐  │
│  │                    Shared Components                        │  │
│  │  ┌────────────────┐  ┌────────────────┐  ┌──────────────┐  │  │
│  │  │ IdempotencyCache│ │ CompletionLog  │  │ InstanceID   │  │  │
│  │  │                │  │                │  │              │  │  │
│  │  └────────────────┘  └────────────────┘  └──────────────┘  │  │
│  └─────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### Connection State Machine

```
                    ┌─────────┐
                    │   NEW   │
                    └────┬────┘
                         │ connect()
                         ▼
                    ┌─────────────┐
          ┌────────│ CONNECTING  │────────┐
          │        └─────────────┘        │
          │ success                       │ failure
          ▼                               ▼
    ┌───────────┐                    ┌─────────┐
    │ CONNECTED │◄───────────────────│  FAILED │
    └─────┬─────┘     reconnect()    └────┬────┘
          │                               │
          │ disconnect/error              │ max retries
          ▼                               ▼
    ┌───────────────┐               ┌───────────────┐
    │ DISCONNECTING │               │  TERMINATED   │
    └───────┬───────┘               └───────────────┘
            │
            ▼
    ┌──────────────┐
    │ DISCONNECTED │─────┐
    └──────────────┘     │ auto_reconnect
                         │
                         ▼
                  ┌─────────────┐
                  │ RECONNECTING│
                  └─────────────┘
```

### Circuit Breaker State Machine

```
                    ┌────────┐
          ┌────────│ CLOSED │◄─────────┐
          │        └────┬───┘          │
          │             │              │
          │ failure_count               │ success_count
          │ >= threshold               │ >= threshold
          │             │              │
          ▼             │              │
    ┌─────────┐         │        ┌───────────┐
    │  OPEN   │─────────┴───────►│ HALF_OPEN │
    └────┬────┘   timeout        └─────┬─────┘
         │                              │
         │         failure              │
         └──────────────────────────────┘
```

## Phase Details

### Phase 1: Connection State Management (~550-850 LOC)

**Goal**: Establish foundation for tracking and managing connection lifecycle.

| Task | Priority | LOC | Dependencies |
|------|----------|-----|--------------|
| 1.1 Connection State Machine | High | 150-200 | None |
| 1.2 Reconnection Policy | High | 100-150 | None |
| 1.3 Automatic Reconnection | Medium | 200-300 | 1.1, 1.2 |
| 1.4 Circuit Breaker | Medium | 150-200 | 1.1 |

### Phase 2: Message Durability (~700-900 LOC)

**Goal**: Ensure requests are not lost during disconnection and can be retried safely.

| Task | Priority | LOC | Dependencies |
|------|----------|-----|--------------|
| 2.1 Request Queue | Medium | 200-250 | None |
| 2.2 Request Buffering | Medium | 150-200 | 1.3, 2.1 |
| 2.3 Idempotency Support | Low | 200-250 | 2.2 |
| 2.4 Timeout and Retry | Medium | 150-200 | 1.2, 2.3 |

### Phase 3: Health Monitoring (~350-500 LOC)

**Goal**: Proactively detect connection issues before I/O failures.

| Task | Priority | LOC | Dependencies |
|------|----------|-----|--------------|
| 3.1 Heartbeat Mechanism | High | 150-200 | 1.3 |
| 3.2 Connection Metrics | Low | 100-150 | None |
| 3.3 Proactive Validation | Medium | 100-150 | 3.1 |

### Phase 4: Server-Side Crash Handling (~400-550 LOC)

**Goal**: Enable graceful shutdown and restart detection.

| Task | Priority | LOC | Dependencies |
|------|----------|-----|--------------|
| 4.1 Graceful Shutdown | Medium | 150-200 | None |
| 4.2 Restart Detection | Medium | 100-150 | 4.1 |
| 4.3 Completion Tracking | Low | 150-200 | 2.3, 4.2 |

### Phase 5: Client Pool Enhancements (~450-600 LOC)

**Goal**: Improve connection pooling with health awareness and load balancing.

| Task | Priority | LOC | Dependencies |
|------|----------|-----|--------------|
| 5.1 Health-Aware Pool | Medium | 200-250 | 1.1, 3.2 |
| 5.2 Load Balancing | Low | 150-200 | 3.2, 5.1 |
| 5.3 Bulk Reconnection | Low | 100-150 | 1.3, 5.1 |

### Phase 6: Error Handling (~250-350 LOC)

**Goal**: Provide structured errors and callbacks for observability.

| Task | Priority | LOC | Dependencies |
|------|----------|-----|--------------|
| 6.1 Structured Errors | High | 150-200 | None |
| 6.2 Error Callbacks | Medium | 100-150 | 6.1 |

### Phase 7: Testing

**Goal**: Comprehensive test coverage including chaos engineering.

| Category | Test Files | Focus Areas |
|----------|------------|-------------|
| Unit | 7 files | Individual component behavior |
| Integration | 5 files | Component interaction, crash recovery |
| Stress | 2 files | High load, long-running stability |
| Chaos | 2 files | Random failures, recovery verification |

### Phase 8: Documentation

- API documentation with usage examples
- Architecture documentation with diagrams
- Migration guide for breaking changes

## Implementation Order

```
Week 1-2: Phase 1 (Connection State Management)
  ├── 1.1 + 1.2 in parallel
  └── 1.3 + 1.4 after

Week 3-4: Phase 2 (Message Durability)
  ├── 2.1 first
  └── 2.2 → 2.3 → 2.4 sequentially

Week 5: Phase 3 (Health Monitoring)
  ├── 3.2 can start early
  └── 3.1 → 3.3 after Phase 1.3

Week 6: Phase 4 (Server-Side)
  └── 4.1 → 4.2 → 4.3 sequentially

Week 7: Phase 5 + 6 (Pool + Errors)
  ├── 6.1 first (no deps)
  └── 5.1 → 5.2, 5.3, 6.2 after

Week 8: Phase 7 + 8 (Testing + Docs)
  └── Finalize all tests and documentation
```

## RustyCpp Compliance

All new code must follow RustyCpp safety requirements:

### Required Types
```cpp
// Use these instead of STL equivalents
rusty::Box<T>      // instead of std::unique_ptr<T>
rusty::Arc<T>      // instead of std::shared_ptr<T>
rusty::Rc<T>       // for single-thread shared ownership
rusty::Cell<T>     // for interior mutability (Copy types)
rusty::RefCell<T>  // for interior mutability (complex types)
rusty::Option<T>   // instead of std::optional<T>
```

### Safety Annotations
```cpp
// @safe - Pure function, no side effects
ConnectionState get_state() const {
    return state_.get();
}

// @unsafe - Calls non-borrow-checked code
void log_error(const char* msg) {
    std::cerr << msg << std::endl;  // @unsafe
}
```

## Total Estimated LOC

| Phase | LOC Range |
|-------|-----------|
| Phase 1 | 550-850 |
| Phase 2 | 700-900 |
| Phase 3 | 350-500 |
| Phase 4 | 400-550 |
| Phase 5 | 450-600 |
| Phase 6 | 250-350 |
| **Total** | **2700-3750** |

Plus test code (estimated 2000-3000 LOC additional).

## Success Criteria

1. **Automatic Recovery**: Clients automatically reconnect after server crash
2. **No Data Loss**: In-flight requests are either completed or properly failed
3. **Graceful Degradation**: System remains responsive during failures
4. **Observable**: All failures and recovery events are logged and metricated
5. **Configurable**: All behaviors can be tuned via configuration
6. **Tested**: All test suites pass, including chaos tests
7. **Documented**: Complete API and architecture documentation

## References

- Client RPC: `src/srpc/rpc/client.hpp`, `src/srpc/rpc/client.cpp`
- Server RPC: `src/srpc/rpc/server.hpp`, `src/srpc/rpc/server.cpp`
- Poll thread: `src/srpc/reactor/reactor.h`
- RustyCpp guidelines: `CLAUDE.md`
