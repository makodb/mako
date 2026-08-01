# RPC Reliability Features

This document describes the reliability features implemented in the rrr/rpc module to handle server/client crashes, automatic reconnection, and fault tolerance.

## Overview

The RPC layer provides several mechanisms to improve reliability:

1. **Connection State Management** - Tracks connection lifecycle with a state machine
2. **Automatic Reconnection** - Configurable reconnection with exponential backoff
3. **Circuit Breaker** - Prevents cascade failures by failing fast
4. **Request Buffering** - Queues requests during disconnection for replay
5. **Timeout and Retry** - Configurable per-request timeout and retry logic
6. **Health Monitoring** - Heartbeat and connection validation
7. **Graceful Shutdown** - Clean server shutdown with request draining
8. **Connection Metrics** - Tracks latency, success/failure rates

## Connection State Machine

Every client connection follows a defined state machine:

```
    ┌─────────┐
    │   NEW   │
    └────┬────┘
         │ connect()
         v
    ┌─────────────┐
    │ CONNECTING  │
    └──────┬──────┘
           │ success/failure
     ┌─────┴─────┐
     v           v
┌──────────┐ ┌────────┐
│CONNECTED │ │ FAILED │
└────┬─────┘ └────────┘
     │ close()/error
     v
┌──────────────┐
│DISCONNECTING │
└──────┬───────┘
       │
       v
┌──────────────┐
│ DISCONNECTED │
└──────────────┘
```

### Usage

```cpp
#include "rpc/connection_state.hpp"

// State machine is internal to ClientConnection
auto client = Client::create(poll_thread);
client->connect("127.0.0.1:8080");

// Check connection state
auto state = client->connection_state();
if (state == ConnectionState::CONNECTED) {
    // Ready to send requests
}
```

## Reconnection Policy

Reconnection is configured via `ReconnectPolicy`:

```cpp
#include "rpc/reconnect_policy.hpp"

// Preset policies
auto aggressive = ReconnectPolicy::AGGRESSIVE();  // Fast retries, more attempts
auto conservative = ReconnectPolicy::CONSERVATIVE();  // Slower, fewer attempts
auto no_retry = ReconnectPolicy::NO_RETRY();  // No automatic reconnection

// Custom policy
ReconnectPolicy custom;
custom.max_retries = 5;
custom.base_delay_ms = 100;
custom.max_delay_ms = 10000;
custom.backoff_multiplier = 2.0;
custom.jitter_factor = 0.1;

// Apply to client
client->set_reconnect_policy(custom);

// Trigger reconnection
client->reconnect([](bool success) {
    if (success) {
        Log_info("Reconnected successfully");
    }
});
```

### Exponential Backoff

Delays between reconnection attempts follow exponential backoff:

```
delay = min(base_delay * multiplier^attempt, max_delay) + jitter
```

## Circuit Breaker

The circuit breaker prevents cascade failures by tracking error rates:

```cpp
#include "rpc/circuit_breaker.hpp"

// States: CLOSED (allowing requests), OPEN (failing fast), HALF_OPEN (testing)

// Preset configurations
auto sensitive = CircuitBreakerConfig::sensitive();  // Opens after 3 failures
auto relaxed = CircuitBreakerConfig::relaxed();      // Opens after 10 failures
auto disabled = CircuitBreakerConfig::disabled();    // Never opens

// Custom configuration
CircuitBreakerConfig config;
config.failure_threshold = 5;      // Open after 5 consecutive failures
config.success_threshold = 3;      // Close after 3 consecutive successes
config.half_open_timeout_ms = 5000; // Try again after 5 seconds

// Usage is automatic within ClientConnection
// After N failures, requests fail fast with RpcError::CIRCUIT_OPEN
```

## Request Buffering

Requests can be buffered during temporary disconnections:

```cpp
#include "rpc/request_queue.hpp"

// Configure buffering behavior
BufferingConfig config;
config.enabled = true;
config.behavior = DisconnectBehavior::QUEUE;  // Queue requests when disconnected
config.max_pending = 1000;  // Maximum queued requests
config.default_ttl_ms = 30000;  // Request TTL (30 seconds)
config.overflow = OverflowStrategy::DROP_OLDEST;

client->set_buffering_config(config);

// Requests made while disconnected are queued
auto fu = client->request(rpc_id, [](BinaryWriteArchive& m) {
    m << arg1 << arg2;
});

// Queued requests are automatically replayed on reconnection
client->reconnect();
```

### Disconnect Behaviors

- `QUEUE` - Queue requests for later replay (default)
- `FAIL_FAST` - Immediately fail with ENOTCONN

## Timeout and Retry

Requests can be configured with timeout and retry options:

```cpp
#include "rpc/request_options.hpp"

// Preset options
auto fast = RequestOptions::fast();            // 100ms timeout, 2 retries
auto patient = RequestOptions::patient();      // 10s timeout, 5 retries
auto no_timeout = RequestOptions::no_timeout(); // Wait indefinitely

// Custom options
RequestOptions opts;
opts.timeout_ms = 1000;          // 1 second per-attempt timeout
opts.total_timeout_ms = 5000;    // 5 seconds total (across all retries)
opts.max_retries = 3;            // Up to 3 retry attempts
opts.idempotent = true;          // Safe to retry (required for retries)
opts.base_delay_ms = 100;        // Initial backoff delay
opts.max_delay_ms = 2000;        // Maximum backoff delay
opts.jitter_factor = 0.1;        // 10% jitter

// Send request with options
auto fu = client->request_with_options(rpc_id, opts, [](BinaryWriteArchive& m) {
    m << arg1 << arg2;
});

// Wait with configured timeout
bool success = fu->wait_with_options();

// Check timeout type if failed
if (fu->timed_out()) {
    auto timeout_type = fu->get_timeout_type();
    // RESPONSE_TIMEOUT, TOTAL_TIMEOUT, etc.
}
```

### Timeout Types

- `NONE` - No timeout occurred
- `CONNECT_TIMEOUT` - Failed to establish connection
- `REQUEST_TIMEOUT` - Request send timed out
- `RESPONSE_TIMEOUT` - Waiting for response timed out
- `TOTAL_TIMEOUT` - Total operation time exceeded

## Health Monitoring

### Heartbeat

Track connection liveness with heartbeats:

```cpp
#include "rpc/heartbeat.hpp"

HeartbeatConfig config;
config.interval_ms = 5000;        // Send heartbeat every 5 seconds
config.timeout_ms = 15000;        // Consider dead after 15 seconds
config.enabled = true;

auto manager = HeartbeatManager(config);

// Check if heartbeat should be sent
if (manager.should_send_heartbeat()) {
    send_heartbeat();
    manager.on_heartbeat_sent();
}

// Record pong response
manager.on_pong_received();

// Check for timeout
if (manager.check_timeout()) {
    // Connection is dead
}
```

### Connection Validation

Validate connections proactively:

```cpp
// Apply TCP keepalive options
KeepaliveConfig keepalive;
keepalive.enabled = true;
keepalive.idle_time_sec = 60;      // Start probing after 60s idle
keepalive.interval_sec = 10;       // Probe every 10 seconds
keepalive.probe_count = 5;         // 5 failed probes = dead

client->set_keepalive_config(keepalive);

// Check if connection is idle
if (client->is_idle(30000)) {  // 30 seconds
    // Connection hasn't been used recently
}

// Validate connection is still healthy
if (!client->validate_connection()) {
    client->reconnect();
}
```

## Connection Metrics

Track connection statistics:

```cpp
#include "rpc/connection_metrics.hpp"

// Access metrics from client
const auto& metrics = client->metrics();

// Request counts
uint64_t sent = metrics.requests_sent();
uint64_t completed = metrics.requests_completed();
uint64_t failed = metrics.requests_failed();
uint64_t timed_out = metrics.requests_timed_out();

// Bytes transferred
uint64_t bytes_sent = metrics.bytes_sent();
uint64_t bytes_received = metrics.bytes_received();

// Latency (microseconds)
uint64_t min_latency = metrics.min_latency_us();
uint64_t max_latency = metrics.max_latency_us();
uint64_t avg_latency = metrics.avg_latency_us();

// Connection events
uint64_t reconnects = metrics.reconnect_count();
uint64_t connect_time = metrics.connect_time_us();

// Reset metrics
metrics.reset();
```

## Graceful Shutdown

Servers can shut down gracefully:

```cpp
#include "rpc/server.hpp"

// Stop accepting new connections
server->stop_accepting();

// Wait for pending requests to complete (up to 30 seconds)
bool drained = server->drain(30000);

// Full graceful shutdown
server->graceful_shutdown(30000);

// Shutdown hooks
server->add_shutdown_hook([]() {
    Log_info("Server shutting down");
    cleanup_resources();
});
```

## Server Restart Detection

Detect when a server has restarted:

```cpp
// Each server has a unique instance ID generated on startup
uint64_t server_id = server->instance_id();

// Clients can detect restarts
client->set_on_server_restart([](uint64_t old_id, uint64_t new_id) {
    Log_warn("Server restarted: {} -> {}", old_id, new_id);
    // Clear any cached state, re-establish session, etc.
});
```

## Error Handling

Structured error types for better error handling:

```cpp
#include "rpc/errors.hpp"

// Error categories
RpcErrorCategory::CONNECTION   // Network-level errors
RpcErrorCategory::PROTOCOL     // Protocol-level errors
RpcErrorCategory::APPLICATION  // Application-level errors
RpcErrorCategory::TIMEOUT      // Timeout errors
RpcErrorCategory::INTERNAL     // Internal errors

// Check error type
if (is_connection_error(error)) {
    // Try reconnecting
}

if (is_retryable_error(error)) {
    // Safe to retry
}

// Detailed error codes
RpcError::CONNECTION_REFUSED
RpcError::CONNECTION_RESET
RpcError::TIMEOUT
RpcError::CIRCUIT_OPEN
RpcError::QUEUE_FULL
// ... and more
```

## Client Pool

Enhanced client pool with health awareness:

```cpp
#include "rpc/client.hpp"

// Pool configuration
PoolConfig config;
config.min_clients = 2;
config.max_clients = 10;
config.idle_timeout_ms = 300000;  // 5 minutes
config.health_check_enabled = true;

// Get client from pool
auto client = pool.get_client("127.0.0.1:8080");

// Health management
size_t healthy = pool.get_healthy_client_count("127.0.0.1:8080");
pool.remove_unhealthy_clients("127.0.0.1:8080");
pool.close_idle_clients("127.0.0.1:8080", current_time_ms);
```

### Bulk Reconnection

Reconnect multiple clients efficiently:

```cpp
// Reconnect all clients for an address
auto result = pool.reconnect_all("127.0.0.1:8080");
Log_info("Reconnected {}/{} clients", result.succeeded, result.total);

// Reconnect all clients in pool
auto result = pool.reconnect_all();

// Use gentle config for rate limiting
auto config = ClientPool::BulkReconnectConfig::gentle();
config.max_concurrent = 5;    // Max 5 concurrent reconnections
config.delay_between_ms = 50; // 50ms between batches

auto result = pool.reconnect_all(config);
```

## Best Practices

### For Clients

1. **Set appropriate timeouts** - Use `RequestOptions` to configure per-request timeouts
2. **Mark idempotent operations** - Enable retries for safe-to-retry operations
3. **Handle circuit breaker** - Check for `RpcError::CIRCUIT_OPEN` and back off
4. **Use buffering wisely** - Enable for transient failures, disable for real-time data
5. **Monitor metrics** - Track latency and failure rates for alerting

### For Servers

1. **Use graceful shutdown** - Allow pending requests to complete before stopping
2. **Track pending requests** - Use `increment_pending()` / `decrement_pending()`
3. **Register shutdown hooks** - Clean up resources properly
4. **Generate unique instance IDs** - Helps clients detect restarts

### General

1. **Configure reconnection policy** - Use aggressive for fast recovery, conservative for stability
2. **Set circuit breaker thresholds** - Tune based on expected failure rates
3. **Enable TCP keepalive** - Detect dead connections faster
4. **Log connection events** - Use callbacks to log state changes

## Files Reference

| File | Description |
|------|-------------|
| `connection_state.hpp` | Connection state machine |
| `reconnect_policy.hpp` | Reconnection policy and backoff |
| `circuit_breaker.hpp` | Circuit breaker pattern |
| `request_queue.hpp` | Request queue for buffering |
| `request_options.hpp` | Timeout and retry configuration |
| `heartbeat.hpp` | Heartbeat/keep-alive mechanism |
| `connection_metrics.hpp` | Connection metrics tracking |
| `errors.hpp` | Structured error types |
| `callbacks.hpp` | Connection event callbacks |

## See Also

- [Transport Backends](transport_backends.md) - Overview of RPC transport options
- [Phase Implementation Plans](rpc/) - Detailed design documents for each phase
