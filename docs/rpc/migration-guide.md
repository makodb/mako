# RPC Reliability Migration Guide

This guide helps you migrate existing code to use the new RPC reliability features introduced in the RPC Reliability Enhancement project.

## Overview

The RPC reliability enhancement adds robust connection management, automatic reconnection, circuit breaker patterns, request buffering, and comprehensive error handling to the rrr/rpc module. **All changes are backward compatible** - existing code will continue to work without modification.

## Breaking Changes

**None.** All existing APIs remain unchanged. The enhancements are purely additive.

## Backward Compatibility Notes (Wire/API)

### Wire protocol (response header extension)

SRPC responses now support an extended header carrying `server_instance_id` for
automatic restart detection. The extension is flagged via the high bit in the
frame `<size>` field (`kResponseHeaderExtFlag`) and parsed by
`response_has_extended_header()` / `response_payload_size()`.

- **New clients** are backward compatible with old and new servers.
- **Old clients** (that do not mask/parse the response-size extension flag) are
  not guaranteed to interoperate with new servers.

Recommended mixed-version rollout order:

1. Upgrade clients first.
2. Upgrade servers after client rollout is complete.

Recommended rollback order:

1. Roll back servers first.
2. Roll back clients after servers are reverted.

### API surface compatibility

No public SRPC API signatures were removed in this reliability series, but
several behaviors became stricter/safer:

- `request_with_options()` retry/reconnect/time-budget settings are now
  enforced at runtime (previously mostly declarative).
- Reconnect policy (`ReconnectPolicy`) now drives real reconnect backoff/retry.
- `ServerConnection::run_async()` executes inline callback behavior instead of
  aborting; `content_size()` and `handle_free()` are now safe compatibility
  paths instead of crash stubs.

No source-level migration is required for existing callers, but operators
should follow the wire upgrade order above for mixed-version deployments.

### Typed RPC API (completed)

All RPC services now use typed request/response structs exclusively. The
`rpcgen` code generator produces only typed APIs — no legacy pointer-style
wrappers. Generated service classes do not inherit `rrr::Service`; they
register via `ServiceTypedBoxAdapter` / `Server::reg_service(Box<T>)`.

The `--legacy-compat` flag and `SRPC_LEGACY_COMPAT` CMake option have been
removed. All callsites across `src/deptran/` use typed `Rpc*Request`/
`Rpc*Response` structs.

## New Dependencies

### rusty-cpp (Required)

The reliability features use rusty-cpp for thread-safe smart pointers and interior mutability:

```cpp
#include <rusty/cell.hpp>   // Cell<T> for interior mutability
#include <rusty/arc.hpp>    // Arc<T> for shared ownership
#include <rusty/mutex.hpp>  // Mutex<T> for thread-safe access
#include <rusty/box.hpp>    // Box<T> for heap allocation
#include <rusty/option.hpp> // Option<T> for optional values
```

Ensure the `third-party/rusty-cpp` submodule is initialized:

```bash
git submodule update --init third-party/rusty-cpp
```

## New Headers

| Header | Purpose |
|--------|---------|
| `rpc/connection_state.hpp` | Connection state machine |
| `rpc/reconnect_policy.hpp` | Reconnection configuration |
| `rpc/circuit_breaker.hpp` | Circuit breaker pattern |
| `rpc/request_queue.hpp` | Request buffering |
| `rrr.request_options` | Per-request options C++ module |
| `rpc/heartbeat.hpp` | Keep-alive management |
| `rpc/connection_metrics.hpp` | Connection statistics |
| `rpc/errors.hpp` | Structured error types |
| `rpc/callbacks.hpp` | Event callbacks |
| `rpc/idempotency.hpp` | Duplicate request detection |
| `rpc/completion_tracker.hpp` | Request completion tracking |
| `rpc/load_balancer.hpp` | Load balancing strategies |

## Migration Examples

### 1. Basic Usage (No Changes Required)

Existing code continues to work:

```cpp
// Before and after - no changes needed
auto client = Client::create(poll_thread);
client->connect("127.0.0.1:8080");
auto future = client->request(RPC_ID, [](BinaryWriteArchive& m) { m << data; });
future.unwrap()->wait();
client->close();
```

### 2. Adding Automatic Reconnection

Enable automatic reconnection for resilience:

```cpp
#include "rpc/reconnect_policy.hpp"

auto client = Client::create(poll_thread);

// Configure reconnection policy
ReconnectPolicy policy = ReconnectPolicy::aggressive();
policy.max_retries = 10;
policy.initial_delay_ms = 50;
client->set_reconnect_policy(policy);

client->connect("127.0.0.1:8080");
// Client will automatically attempt reconnection on disconnect
```

### 3. Adding Connection Monitoring

Track connection state changes:

```cpp
#include "rpc/callbacks.hpp"

auto client = Client::create(poll_thread);

// Set up callbacks for connection events
client->add_on_connected([]() {
    Log_info("Connected to server");
});

client->add_on_disconnected([]() {
    Log_warn("Disconnected from server");
});

client->add_on_reconnected([](bool success) {
    Log_info("Reconnected to server (success={})", success ? 1 : 0);
});

client->connect("127.0.0.1:8080");
```

### 4. Using Circuit Breaker

Prevent cascading failures:

```cpp
#include "rpc/circuit_breaker.hpp"

CircuitBreakerConfig config;
config.failure_threshold = 5;   // Open after 5 failures
config.success_threshold = 3;   // Close after 3 successes
config.timeout_ms = 30000;      // Try again after 30s

CircuitBreaker breaker(config);

// Before each request
if (!breaker.allow_request()) {
    // Circuit is open, fail fast
    return Error::CIRCUIT_OPEN;
}

// After request completes
if (success) {
    breaker.record_success();
} else {
    breaker.record_failure();
}
```

### 5. Request Options

Configure per-request behavior:

```cpp
import rrr.request_options;

RequestOptions opts = RequestOptions::with_retry(3, 5000);

auto future = client->request_with_options(RPC_ID, opts, writer);
```

### 6. Connection Metrics

Monitor connection health:

```cpp
auto& metrics = client->metrics();

Log_info("Requests sent: {}", metrics.requests_sent());
Log_info("Requests completed: {}", metrics.requests_completed());
Log_info("Requests failed: {}", metrics.requests_failed());
Log_info("Reconnect count: {}", metrics.reconnect_count());
Log_info("Avg latency: {} us", metrics.avg_latency_us());
```

### 7. Graceful Server Shutdown

Implement graceful shutdown:

```cpp
// Server side
server->stop_accepting();  // Stop accepting new connections

// Wait for in-flight requests to complete (5 second timeout)
if (!server->drain(5000)) {
    Log_warn("Some requests did not complete within timeout");
}

// Or use combined method
server->graceful_shutdown(5000);
```

### 8. Load Balancing with ClientPool

Use load-balanced connections:

```cpp
#include "rpc/load_balancer.hpp"

PoolConfig config = PoolConfig::defaults();
config.load_balancing = LoadBalancingStrategy::LEAST_LATENCY;

ClientPool pool(poll_thread, config);
pool.add_addr("server1:8080");
pool.add_addr("server2:8080");
pool.add_addr("server3:8080");

// Get client using load balancing
auto client = pool.get_client();
```

## Incremental Adoption

You can adopt features incrementally:

1. **Phase 1**: Add reconnection policy for basic resilience
2. **Phase 2**: Add callbacks for monitoring
3. **Phase 3**: Add circuit breaker for failure isolation
4. **Phase 4**: Add metrics for observability
5. **Phase 5**: Add request options for fine-grained control
6. **Phase 6**: Add graceful shutdown for clean deployments

## Testing Changes

The test suite includes comprehensive coverage:

```bash
# Run all RPC reliability tests
ctest -R "test_rpc"

# Run stress tests (longer runtime)
ctest -L stress

# Run chaos engineering tests
ctest -L chaos
```

## Performance Considerations

- **Memory**: Each client now tracks metrics and state, adding ~1KB per connection
- **CPU**: Connection monitoring adds minimal overhead (<1% in benchmarks)
- **Latency**: No measurable impact on request latency in normal operation

## Troubleshooting

### Connection State Issues

```cpp
// Check connection state
if (!client->connected()) {
    ConnectionState state = client->connection_state();
    Log_warn("Not connected, state: {}",
             connection_state_to_string(state));
}
```

### Circuit Breaker Open

```cpp
if (!breaker.allow_request()) {
    Log_warn("Circuit breaker open. State: {}, Failures: {}",
             circuit_state_to_string(breaker.state()),
             breaker.failure_count());
}
```

### Request Timeout

```cpp
if (future->timed_wait(timeout_ms)) {
    // Completed within timeout
} else {
    // Timed out
    Log_warn("Request timed out after {} ms", timeout_ms);
}
```

## See Also

- [RPC API Reference](rpc_api.md) - Complete API documentation
- [RPC Reliability Guide](rpc_reliability.md) - Architecture and design
- [Transport Backends](transport_backends.md) - Backend comparison
