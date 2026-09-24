# Client-Server Roadmap

This document describes planned improvements for Mako's client-server architecture.

## Transaction Isolation (High Priority)

### Problem

The current implementation does not guarantee transaction isolation when multiple clients execute transactions concurrently. This can lead to:
- Lost updates when clients write to overlapping keys
- Dirty reads if a client reads uncommitted data
- Non-repeatable reads within a transaction

### Root Cause

All clients share the same server-side thread context. Without per-client transaction isolation, concurrent operations interleave unpredictably.

### Proposed Solution

1. **Per-Client Transaction Context**: Each client connection gets a dedicated `scoped_db_thread_ctx` that isolates its transaction state.

2. **Worker Slot Pool**: Implement bounded concurrency with `nthreads` worker slots per shard. Each slot:
   - Has its own thread context
   - Can serve one client transaction at a time
   - Returns to pool after transaction completes

3. **Capacity Management**: When all workers are busy, reject new connections with `SERVER_BUSY` response code.

### Design Sketch

```cpp
struct WorkerSlot {
    std::atomic<bool> in_use{false};
    scoped_db_thread_ctx* ctx;
    uint64_t current_txn_id;

    bool acquire() {
        return !in_use.exchange(true);
    }
    void release() {
        in_use.store(false);
    }
};

class WorkerPool {
    std::vector<WorkerSlot> slots_;  // size = nthreads
public:
    WorkerSlot* acquire_slot();
    void release_slot(WorkerSlot* slot);
};
```

### Safe Use Cases After Implementation

- Multiple concurrent clients performing read-write transactions
- OLTP workloads with proper isolation
- Production deployments

## SRPC RPC Framework Migration (Medium Priority)

### Current State

The client-server communication currently uses raw TCP sockets with custom message framing.

### Proposed Change

Migrate to the existing SRPC RPC framework (`src/srpc/rpc/`) to gain:
- Automatic connection management
- Timeout and retry handling
- Health checking
- Load balancing (via ClientPool)

### Benefits

1. **Code Reuse**: Leverage battle-tested RPC infrastructure
2. **Reliability**: Built-in reconnection, circuit breakers
3. **Observability**: Metrics integration (ConnectionMetrics)
4. **Future-Proofing**: Easier to add features like streaming

### Implementation Outline

1. Define `MakoClientService` in RPC IDL
2. Generate proxy/service stubs
3. Implement service handlers (reuse existing logic)
4. Update RemoteDB to use MakoClientProxy

### Migration Path

Phase 1: Add SRPC-based implementation alongside TCP
Phase 2: Test and validate correctness
Phase 3: Deprecate raw TCP implementation
Phase 4: Remove TCP code

## Performance Evaluation (Low Priority)

### Metrics to Track

- Throughput: ops/sec for Put, Get operations
- Latency: P50, P95, P99 for each operation type
- Memory: Per-client overhead
- Connection: Establishment time, reconnection success rate

### Benchmarking Plan

1. Single-client baseline (compare with local DB)
2. Multi-client scaling (1, 4, 8, 16 clients)
3. Cross-shard transactions
4. Network latency simulation

## Timeline (Tentative)

| Feature | Priority | Estimated Effort |
|---------|----------|------------------|
| Transaction Isolation | High | 300-400 LOC |
| SRPC Migration | Medium | 200-300 LOC |
| Performance Eval | Low | Testing only |

## References

- Current implementation: [Client-Server Architecture](client-server.md)
- SRPC RPC framework: `src/srpc/rpc/`
- Example code: `examples/simpleTransactionRep.cc`
