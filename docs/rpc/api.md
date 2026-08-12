# RPC Reliability API Reference

This document provides a complete API reference for the RPC reliability features in the rrr/rpc module.

## Table of Contents

1. [Connection State](#connection-state)
2. [Reconnection Policy](#reconnection-policy)
3. [Circuit Breaker](#circuit-breaker)
4. [Request Queue](#request-queue)
5. [Request Options](#request-options)
6. [Heartbeat](#heartbeat)
7. [Connection Metrics](#connection-metrics)
8. [Errors](#errors)
9. [Callbacks](#callbacks)
10. [Client and ClientConnection](#client-and-clientconnection)
11. [Server](#server)

---

## Connection State

**Header:** `src/rrr/rpc/connection_state.hpp`

### ConnectionState Enum

```cpp
enum class ConnectionState : uint8_t {
    NEW,          // Initial state, not yet connected
    CONNECTING,   // Connection attempt in progress
    CONNECTED,    // Successfully connected
    DISCONNECTING,// Disconnect in progress
    DISCONNECTED, // Cleanly disconnected
    FAILED        // Connection failed or error occurred
};
```

### ConnectionStateMachine Class

Manages connection state transitions with validation and callbacks.

```cpp
class ConnectionStateMachine {
public:
    // Constructors
    ConnectionStateMachine();
    ConnectionStateMachine(ConnectionState initial);

    // State access
    ConnectionState state() const;

    // State checks
    bool is_connected() const;
    bool is_failed() const;
    bool can_reconnect() const;

    // State transitions
    bool start_connecting();    // NEW/DISCONNECTED/FAILED -> CONNECTING
    bool mark_connected();      // CONNECTING -> CONNECTED
    bool mark_failed();         // Any -> FAILED
    bool start_disconnecting(); // CONNECTED/CONNECTING -> DISCONNECTING
    bool mark_disconnected();   // DISCONNECTING -> DISCONNECTED

    // Callbacks
    void set_on_connected(std::function<void()> callback);
    void set_on_disconnected(std::function<void()> callback);
    void set_on_failed(std::function<void()> callback);
};
```

### Helper Functions

```cpp
std::string_view connection_state_to_string(ConnectionState state);
```

---

## Reconnection Policy

**Header:** `src/rrr/rpc/reconnect_policy.hpp`

### ReconnectPolicy Struct

Configuration for automatic reconnection behavior.

```cpp
struct ReconnectPolicy {
    // Configuration
    uint32_t max_retries = 5;           // Max reconnection attempts (0 = infinite)
    uint32_t base_delay_ms = 100;       // Initial delay before first retry
    uint32_t max_delay_ms = 30000;      // Maximum delay cap
    double backoff_multiplier = 2.0;    // Exponential backoff multiplier
    double jitter_factor = 0.1;         // Random jitter (0.0 - 1.0)
    bool enabled = true;                // Enable/disable reconnection

    // Presets
    static ReconnectPolicy AGGRESSIVE();    // Fast retries, more attempts
    static ReconnectPolicy CONSERVATIVE();  // Slower, fewer attempts
    static ReconnectPolicy NO_RETRY();      // Disabled

    // Methods
    bool can_retry(uint32_t attempt) const;
};
```

### ReconnectCalculator Class

Calculates backoff delays with jitter.

```cpp
class ReconnectCalculator {
public:
    ReconnectCalculator(const ReconnectPolicy& policy);

    // Reset retry counter
    void reset();

    // Calculate next delay (ms) and increment counter
    uint32_t next_delay();

    // Peek at next delay without incrementing
    uint32_t peek_delay() const;

    // Check if more retries allowed
    bool can_retry() const;

    // Get current retry count
    uint32_t retry_count() const;
};
```

---

## Circuit Breaker

**Header:** `src/rrr/rpc/circuit_breaker.hpp`

### CircuitState Enum

```cpp
enum class CircuitState : uint8_t {
    CLOSED,    // Normal operation, requests allowed
    OPEN,      // Failing fast, requests rejected
    HALF_OPEN  // Testing recovery, limited requests
};
```

### CircuitBreakerConfig Struct

```cpp
struct CircuitBreakerConfig {
    uint32_t failure_threshold = 5;     // Failures to open circuit
    uint32_t success_threshold = 3;     // Successes to close circuit
    uint64_t half_open_timeout_ms = 5000; // Time before probing
    bool enabled = true;

    // Presets
    static CircuitBreakerConfig sensitive();  // Opens after 3 failures
    static CircuitBreakerConfig relaxed();    // Opens after 10 failures
    static CircuitBreakerConfig disabled();   // Never opens
};
```

### CircuitBreaker Class

```cpp
class CircuitBreaker {
public:
    CircuitBreaker();
    CircuitBreaker(const CircuitBreakerConfig& config);

    // State access
    CircuitState state() const;
    bool is_open() const;

    // Request gating
    bool allow_request();       // True if request should proceed

    // Record outcomes
    void record_success();
    void record_failure();

    // Manual control
    void reset();               // Force to CLOSED state

    // Configuration
    void set_config(const CircuitBreakerConfig& config);
    const CircuitBreakerConfig& config() const;
};
```

---

## Request Queue

**Header:** `src/rrr/rpc/request_queue.hpp`

### OverflowStrategy Enum

```cpp
enum class OverflowStrategy : uint8_t {
    DROP_OLDEST,   // Remove oldest when full
    DROP_NEWEST,   // Reject new when full
    FAIL_FAST      // Fail enqueue when full
};
```

### QueuedRequest Struct

```cpp
struct QueuedRequest {
    i64 xid;                              // Request ID
    i32 rpc_id;                           // RPC method ID
    uint64_t timestamp_ms;                // Enqueue time
    uint16_t retry_count;                 // Retry attempts
    std::shared_ptr<Marshal> payload;     // Serialized request
    std::function<void(int)> callback;    // Completion callback
    uint32_t ttl_ms;                      // Time-to-live

    bool is_expired() const;
};
```

### RequestQueueConfig Struct

```cpp
struct RequestQueueConfig {
    size_t max_size = 1000;              // Maximum queue size
    uint32_t default_ttl_ms = 30000;     // Default TTL (30 seconds)
    OverflowStrategy overflow = OverflowStrategy::DROP_OLDEST;
    bool enabled = true;

    // Presets
    static RequestQueueConfig defaults();
    static RequestQueueConfig small();    // max_size = 100
    static RequestQueueConfig large();    // max_size = 10000
    static RequestQueueConfig disabled(); // enabled = false
};
```

### RequestQueue Class

```cpp
class RequestQueue {
public:
    RequestQueue();
    RequestQueue(const RequestQueueConfig& config);

    // Queue operations
    bool enqueue(QueuedRequest request);
    rusty::Option<QueuedRequest> dequeue();
    rusty::Option<QueuedRequest> peek() const;

    // State
    bool empty() const;
    bool full() const;
    size_t size() const;

    // Maintenance
    size_t remove_expired();      // Remove and notify expired requests
    void clear_all(int error);    // Clear all with error callback

    // Configuration
    void update_config(const RequestQueueConfig& config);
};
```

---

## Request Options

**Module:** `rrr.request_options` (canonical source:
`src/rrr/src/request_options.rs`; C++ interface generated by rusty-cpp)

### TimeoutType Enum

```cpp
enum class TimeoutType {
    NONE,             // No timeout occurred
    CONNECT_TIMEOUT,  // Connection establishment timeout
    REQUEST_TIMEOUT,  // Request send timeout
    RESPONSE_TIMEOUT, // Response wait timeout
    TOTAL_TIMEOUT     // Overall operation timeout
};
```

The supported ABI pins the default enum backing to signed 32-bit storage.

### RequestOptions Struct

```cpp
struct RequestOptions {
    // Timeout configuration
    uint64_t timeout_ms;                 // Per-attempt timeout
    uint64_t total_timeout_ms;           // Total operation timeout (0 = no limit)

    // Retry configuration
    uint16_t max_retries;                // Max retry attempts
    uint16_t base_delay_ms;              // Base backoff delay
    uint16_t max_delay_ms;               // Maximum backoff delay
    float jitter_factor;                 // Backoff jitter

    // Idempotency
    bool idempotent;                     // Safe to retry

    // Presets
    static RequestOptions new_();                 // 1s timeout, no retry
    static RequestOptions defaults();             // Same as new_()
    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms);
    static RequestOptions idempotent_retry(uint16_t max_retries);
    static RequestOptions no_timeout();          // Wait indefinitely
    static RequestOptions fast();                // 100ms, 2 retries
    static RequestOptions patient();             // 10s, 5 retries

    // Helper methods
    bool can_retry(uint16_t current_retry_count) const;
    uint64_t calculate_delay_ms(uint16_t attempt) const;
    bool is_total_timeout_exceeded(uint64_t elapsed_ms) const;
    uint64_t remaining_time_ms(uint64_t elapsed_ms) const;
};
```

### Helper Functions

```cpp
std::string_view timeout_type_to_string(TimeoutType ty);
```

---

## Heartbeat

**Header:** `src/rrr/rpc/heartbeat.hpp`

### HeartbeatConfig Struct

```cpp
struct HeartbeatConfig {
    uint64_t interval_ms = 5000;       // Heartbeat interval
    uint64_t timeout_ms = 15000;       // Consider dead after this
    bool enabled = true;

    // Presets
    static HeartbeatConfig aggressive();  // 1s interval, 3s timeout
    static HeartbeatConfig relaxed();     // 10s interval, 30s timeout
    static HeartbeatConfig disabled();    // enabled = false
};
```

### HeartbeatManager Class

```cpp
class HeartbeatManager {
public:
    HeartbeatManager();
    HeartbeatManager(const HeartbeatConfig& config);

    // State checks
    bool should_send_heartbeat() const;
    bool check_timeout() const;

    // Events
    void on_heartbeat_sent();
    void on_pong_received();
    void on_activity();           // Reset timeout on any activity

    // Callbacks
    void set_on_timeout(std::function<void()> callback);

    // Configuration
    void set_config(const HeartbeatConfig& config);
    const HeartbeatConfig& config() const;

    // Reset
    void reset();
};
```

---

## Connection Metrics

**Header:** `src/rrr/rpc/connection_metrics.hpp`

### ConnectionMetrics Class

```cpp
class ConnectionMetrics {
public:
    ConnectionMetrics();

    // Request counts
    uint64_t requests_sent() const;
    uint64_t requests_completed() const;
    uint64_t requests_failed() const;
    uint64_t requests_timed_out() const;

    // Bytes transferred
    uint64_t bytes_sent() const;
    uint64_t bytes_received() const;

    // Latency (microseconds)
    uint64_t min_latency_us() const;
    uint64_t max_latency_us() const;
    uint64_t avg_latency_us() const;

    // Connection events
    uint64_t reconnect_count() const;
    uint64_t connect_time_us() const;

    // Recording methods (called internally)
    void record_request_sent();
    void record_request_completed(uint64_t latency_us);
    void record_request_failed();
    void record_request_timeout();
    void record_bytes_sent(uint64_t bytes);
    void record_bytes_received(uint64_t bytes);
    void record_reconnect();
    void record_connect_time(uint64_t time_us);

    // Reset all metrics
    void reset();
};
```

---

## Errors

**Header:** `src/rrr/rpc/errors.hpp`

### RpcErrorCategory Enum

```cpp
enum class RpcErrorCategory : uint8_t {
    NONE,        // No error
    CONNECTION,  // Network-level errors
    PROTOCOL,    // Protocol-level errors
    APPLICATION, // Application-level errors
    TIMEOUT,     // Timeout errors
    INTERNAL     // Internal errors
};
```

### RpcError Enum

```cpp
enum class RpcError : int32_t {
    OK = 0,

    // Connection errors
    CONNECTION_REFUSED = -1001,
    CONNECTION_RESET = -1002,
    CONNECTION_CLOSED = -1003,
    HOST_UNREACHABLE = -1004,
    NETWORK_UNREACHABLE = -1005,

    // Protocol errors
    INVALID_MESSAGE = -2001,
    PROTOCOL_MISMATCH = -2002,
    DESERIALIZATION_FAILED = -2003,

    // Application errors
    SERVICE_UNAVAILABLE = -3001,
    METHOD_NOT_FOUND = -3002,
    INVALID_ARGUMENT = -3003,

    // Timeout errors
    CONNECT_TIMEOUT = -4001,
    REQUEST_TIMEOUT = -4002,
    RESPONSE_TIMEOUT = -4003,

    // Internal errors
    CIRCUIT_OPEN = -5001,
    QUEUE_FULL = -5002,
    INTERNAL_ERROR = -5003
};
```

### Error Handling Model

RPC surfaces structured `RpcError` values and helper predicates
(`is_connection_error`, `is_timeout_error`, `is_retryable_error`) instead of
an RPC-specific exception type.

### Helper Functions

```cpp
RpcErrorCategory error_category(RpcError error);
const char* error_to_string(RpcError error);
const char* category_to_string(RpcErrorCategory category);

bool is_connection_error(RpcError error);
bool is_timeout_error(RpcError error);
bool is_retryable_error(RpcError error);
```

---

## Callbacks

**Header:** `src/rrr/rpc/callbacks.hpp`

### ConnectionCallbacks Struct

```cpp
struct ConnectionCallbacks {
    std::function<void()> on_connected;
    std::function<void()> on_disconnected;
    std::function<void(RpcError)> on_error;
    std::function<void()> on_reconnecting;
    std::function<void()> on_reconnected;
};
```

### CallbackManager Class

```cpp
class CallbackManager {
public:
    // Register callbacks (returns ID for removal)
    size_t add_on_connected(std::function<void()> cb);
    size_t add_on_disconnected(std::function<void()> cb);
    size_t add_on_error(std::function<void(RpcError)> cb);
    size_t add_on_reconnecting(std::function<void()> cb);
    size_t add_on_reconnected(std::function<void()> cb);

    // Remove callbacks
    void remove_callback(size_t id);
    void clear_all();

    // Invoke callbacks (called internally)
    void invoke_connected();
    void invoke_disconnected();
    void invoke_error(RpcError error);
    void invoke_reconnecting();
    void invoke_reconnected();
};
```

---

## Client and ClientConnection

**Header:** `src/rrr/rpc/client.hpp`

### Future Class

```cpp
class Future {
public:
    // Factory
    static rusty::Arc<Future> create(i64 xid, const FutureAttr& attr = FutureAttr());

    // Waiting
    void wait() const;                      // Wait indefinitely
    void timed_wait(double sec) const;      // Wait with timeout
    bool wait_with_options() const;         // Wait using stored options

    // State checks
    bool ready() const;
    bool timed_out() const;

    // Results
    rusty::RefMut<Marshal> get_reply() const;
    i32 get_error_code() const;
    i64 get_xid() const;

    // Timeout/retry support
    RequestOptions get_options() const;
    void set_options(const RequestOptions& opts);
    TimeoutType get_timeout_type() const;
    uint16_t get_retry_count() const;
    uint16_t increment_retry_count();
    bool should_retry() const;
};
```

### ClientConnection Class

```cpp
class ClientConnection : public Pollable {
public:
    // Connection
    int connect(const char* addr);
    void close();
    int reconnect(std::function<void(bool)> on_complete = nullptr);

    // Requests
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const;

    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const;

    // State
    ConnectionState connection_state() const;
    bool valid() const;

    // Configuration
    void set_reconnect_policy(const ReconnectPolicy& policy);
    const ReconnectPolicy& reconnect_policy() const;
    void set_buffering_config(const BufferingConfig& config) const;
    void set_keepalive_config(const KeepaliveConfig& config);

    // Health
    bool is_idle(uint64_t idle_ms) const;
    bool validate_connection() const;

    // Metrics
    const ConnectionMetrics& metrics() const;

    // Server restart detection
    void set_on_server_restart(std::function<void(uint64_t, uint64_t)> callback);
};
```

### Client Class

Wrapper around ClientConnection with safe connection management.

```cpp
class Client {
public:
    // Factory
    static rusty::Arc<Client> create(rusty::Arc<PollThread> poll_thread);

    // Connection
    int connect(const char* addr, bool client = true) const;
    void close() const;
    int reconnect(std::function<void(bool)> on_complete = nullptr) const;

    // Requests (delegates to ClientConnection)
    template<typename F>
    FutureResult request(i32 rpc_id, const FutureAttr& attr, F&& write_fn) const;

    template<typename F>
    FutureResult request_with_options(i32 rpc_id, const RequestOptions& options,
                                      const FutureAttr& attr, F&& write_fn) const;

    // State
    ConnectionState connection_state() const;
    bool valid() const;

    // Configuration (delegates to ClientConnection)
    void set_reconnect_policy(const ReconnectPolicy& policy) const;
    void set_buffering_config(const BufferingConfig& config) const;
    void set_keepalive_config(const KeepaliveConfig& config) const;

    // Health
    bool is_idle(uint64_t idle_ms) const;
    bool validate_connection() const;

    // Metrics
    const ConnectionMetrics* metrics() const;
};
```

### ClientPool Class

```cpp
class ClientPool {
public:
    // Get client (creates if needed)
    rusty::Arc<Client> get_client(const std::string& addr);

    // Health management
    size_t get_healthy_client_count(const std::string& addr);
    size_t remove_unhealthy_clients(const std::string& addr);
    size_t close_idle_clients(const std::string& addr, uint64_t current_time_ms);
    size_t remove_all_unhealthy();
    size_t close_all_idle(uint64_t current_time_ms);

    // Pool statistics
    size_t total_client_count();
    size_t address_count();

    // Bulk reconnection result
    struct BulkReconnectResult {
        size_t total;       // Total clients attempted
        size_t succeeded;   // Number that reconnected successfully
        size_t failed;      // Number that failed to reconnect
        size_t skipped;     // Number skipped (already connected)
    };

    // Bulk reconnection configuration
    struct BulkReconnectConfig {
        uint32_t max_concurrent = 10;     // Max concurrent reconnections
        uint32_t delay_between_ms = 10;   // Delay between batches
        bool skip_connected = true;       // Skip already connected clients

        static BulkReconnectConfig defaults();  // max_concurrent=10, delay=10ms
        static BulkReconnectConfig fast();      // max_concurrent=50, delay=0
        static BulkReconnectConfig gentle();    // max_concurrent=5, delay=50ms
    };

    // Bulk reconnection
    BulkReconnectResult reconnect_all(const std::string& addr,
                                      const BulkReconnectConfig& config = BulkReconnectConfig::defaults());
    BulkReconnectResult reconnect_all(const BulkReconnectConfig& config = BulkReconnectConfig::defaults());
};
```

---

## Server

**Header:** `src/rrr/rpc/server.hpp`

### Server Class

```cpp
class Server : public Pollable {
public:
    // Lifecycle
    void start(const char* bind_addr);
    void close();

    // Graceful shutdown
    void stop_accepting();              // Stop accepting new connections
    bool drain(uint64_t timeout_ms);    // Wait for pending requests
    void graceful_shutdown(uint64_t drain_timeout_ms);

    // Request tracking
    void increment_pending();
    void decrement_pending();
    int pending_request_count() const;

    // Shutdown hooks
    void add_shutdown_hook(std::function<void()> hook);

    // Instance identification
    uint64_t instance_id() const;

    // Service registration
    void reg(Service* svc);
};
```

---

## Usage Examples

### Basic Request with Timeout

```cpp
auto client = Client::create(poll_thread);
client->connect("127.0.0.1:8080");

// Request with 500ms timeout
auto opts = RequestOptions::defaults();
opts.timeout_ms = 500;

auto fu = client->request_with_options(RPC_METHOD, opts, [](BinaryWriteArchive& m) {
    m << arg1 << arg2;
});

if (fu.is_ok()) {
    bool success = fu.ok()->wait_with_options();
    if (!success && fu.ok()->timed_out()) {
        // Handle timeout
    }
}
```

### Idempotent Request with Retry

```cpp
auto opts = RequestOptions::idempotent_retry(3);  // 3 retries

auto fu = client->request_with_options(RPC_IDEMPOTENT_METHOD, opts, [](BinaryWriteArchive& m) {
    m << request_data;
});
```

### Reconnection with Buffering

```cpp
// Configure reconnection
client->set_reconnect_policy(ReconnectPolicy::AGGRESSIVE());

// Configure buffering
BufferingConfig buffering;
buffering.enabled = true;
buffering.behavior = DisconnectBehavior::QUEUE;
client->set_buffering_config(buffering);

// Requests made during disconnection are queued and replayed
```

### Server Graceful Shutdown

```cpp
server->add_shutdown_hook([]() {
    Log_info("Cleaning up resources...");
});

// Graceful shutdown with 30 second drain timeout
server->graceful_shutdown(30000);
```

---

## See Also

- [RPC Reliability Features](rpc_reliability.md) - Architecture overview
- [Transport Backends](transport_backends.md) - Transport layer options
