# RRR RPC Library User Guide

## Table of Contents
- [Overview](#overview)
- [Quick Start](#quick-start)
- [Service Definition Language](#service-definition-language)
- [Code Generation](#code-generation)
- [Client Programming](#client-programming)
- [Server Programming](#server-programming)
- [Advanced Features](#advanced-features)
- [Best Practices](#best-practices)
- [Troubleshooting](#troubleshooting)

## Overview

The RRR (Repeatable Research Runtime) RPC library is a high-performance, code-generation based RPC framework designed for distributed systems. 

## Quick Start

### 1. Define Your Service

Create a `.rpc` file defining your service interface:

```rpc
// counter.rpc
namespace example;

abstract service Counter {
    defer increment(i32 delta | i32 new_value);
    defer get_value( | i32 current_value);
    fast reset();  // Fast methods run in network thread
}
```

### 2. Generate Code

Run the RPC code generator:

```bash
./bin/rpcgen --cpp counter.rpc
```

This generates `counter.h` with service interfaces and proxy classes.

### 3. Implement Server

```cpp
#include "counter.h"

class CounterServiceImpl : public example::CounterService {
    int counter_ = 0;
public:
    void increment(const rrr::i32& delta, rrr::i32* new_value) override {
        counter_ += delta;
        *new_value = counter_;
    }
    
    void get_value(rrr::i32* current_value) override {
        *current_value = counter_;
    }
    
    void reset() override {
        counter_ = 0;
    }
};

int main() {
    // Create server infrastructure
    rrr::PollMgr* poll = new rrr::PollMgr(1);
    rrr::ThreadPool* tp = new rrr::ThreadPool(4);
    rrr::Server* server = new rrr::Server(poll, tp);
    
    // Register service
    CounterServiceImpl* service = new CounterServiceImpl();
    server->reg(service);
    
    // Start server
    server->start("0.0.0.0:8000");
    
    // Run event loop
    while (true) {
        poll->poll(1000);  // Poll with 1 second timeout
    }
}
```

### 4. Implement Client

```cpp
#include "counter.h"

int main() {
    // Create client infrastructure
    rrr::PollMgr* poll = new rrr::PollMgr(1);
    rrr::Client* client = new rrr::Client(poll);
    
    // Connect to server
    int ret = client->connect("localhost:8000");
    if (ret != 0) {
        printf("Connection failed\n");
        return 1;
    }
    
    // Create proxy
    example::CounterProxy proxy(client);
    
    // Make RPC calls
    rrr::i32 result;
    proxy.increment(5, &result);
    printf("New value: %d\n", result.get());

    // Async call with Arc<Future>
    auto fut_result = proxy.async_get_value(&result);
    if (fut_result.is_ok()) {
        auto fut = fut_result.unwrap();
        fut->wait();
        printf("Current value: %d\n", result.get());
        // Arc automatically manages lifetime
    }

    // Cleanup
    client->close_and_release();
    delete poll;
}
```

## Service Definition Language

### Basic Syntax

```rpc
namespace <namespace_name>;

struct <struct_name> {
    <type> <field_name>;
    ...
}

abstract service <service_name> {
    <method_attr> <method_name>(<input_params> | <output_params>);
}
```

### Data Types

#### Built-in Types
- `i8`, `i16`, `i32`, `i64` - Fixed-size signed integers
- `v32`, `v64` - Variable-length integers (space-efficient)
- `double` - Double precision floating point
- `string` - UTF-8 string

#### Container Types
- `vector<T>` - Dynamic array
- `map<K,V>` - Hash map
- `set<T>` - Hash set
- `pair<T1,T2>` - Pair of values

#### Custom Types
```rpc
struct User {
    i64 id;
    string name;
    vector<string> emails;
}
```

### Method Attributes

- **`defer`** - Execute in thread pool (default, for CPU-intensive operations)
- **`fast`** - Execute in network thread (for quick, non-blocking operations)
- **`raw`** - Direct access to marshaled request/response

### Examples

```rpc
namespace myapp;

struct Order {
    i64 order_id;
    i64 user_id;
    double amount;
    string status;
}

abstract service OrderService {
    // Create new order, return order ID
    defer create_order(i64 user_id, double amount | i64 order_id);
    
    // Get order details
    defer get_order(i64 order_id | Order order);
    
    // Update order status (fast operation)
    fast update_status(i64 order_id, string new_status | i32 error_code);
    
    // List user's orders
    defer list_orders(i64 user_id | vector<Order> orders);
    
    // Batch operation
    defer process_batch(vector<i64> order_ids | map<i64, string> results);
}
```

## Code Generation

### Running the Generator

```bash
# Generate C++ code
./bin/rpcgen --cpp myservice.rpc

# Generate Python code (if available)
./bin/rpcgen --python myservice.rpc

# Specify output directory
./bin/rpcgen --cpp --out-dir ./generated myservice.rpc
```

### Generated Files

For `myservice.rpc`, the generator creates:
- `myservice.h` - Service interfaces, proxy classes, and marshaling code
- `myservice.py` - Python bindings (if requested)

### Generated Classes

```cpp
// Abstract service interface
class MyService : public rrr::Service {
public:
    enum {
        METHOD1 = 0xXXXXXXXX,  // Auto-generated method IDs
        METHOD2 = 0xYYYYYYYY,
    };
    virtual void method1(...) = 0;
};

// Client proxy
class MyServiceProxy {
public:
    MyServiceProxy(rrr::Client* cl);
    
    // Synchronous methods
    void method1(...);
    
    // Asynchronous methods
    rrr::Future* async_method1(...);
};

// Marshaling support for custom structs
struct MyStruct : public rrr::Marshallable {
    rrr::Marshal& marshal(rrr::Marshal&) const;
    rrr::Marshal& unmarshal(rrr::Marshal&);
};
```

## Client Programming

### Basic Client Setup

```cpp
// Create poll manager (handles I/O events)
rrr::PollMgr* poll_mgr = new rrr::PollMgr(1);

// Create client
rrr::Client* client = new rrr::Client(poll_mgr);

// Connect to server
int ret = client->connect("server.example.com:8000");
if (ret != 0) {
    // Handle connection error
}

// Create proxy
MyServiceProxy proxy(client);
```

### Synchronous Calls

```cpp
// Simple call
rrr::i32 result;
proxy.get_value(&result);

// Call with multiple parameters
Order order;
proxy.get_order(order_id, &order);

// Error handling
try {
    proxy.risky_operation();
} catch (rrr::RpcException& e) {
    printf("RPC failed: %s\n", e.what());
}
```

### Asynchronous Calls

```cpp
// Single async call
rrr::i32 result;
rrr::Future* future = proxy.async_get_value(&result);

// Do other work...

// Wait for completion
future->wait();
future->release();  // Always release futures

// Multiple async calls
rrr::FutureGroup fg;
for (int i = 0; i < 10; i++) {
    fg.add(proxy.async_increment(1, &results[i]));
}
fg.wait_all();  // Wait for all to complete
```

### Connection Pooling

```cpp
// Create connection pool (4 connections per server)
rrr::ClientPool* pool = new rrr::ClientPool(poll_mgr, 4);

// Get client from pool
rrr::Client* client = pool->get_client("server:8000");

// Use client normally
MyServiceProxy proxy(client);
proxy.method();
```

### Advanced Client Patterns

```cpp
// Retry logic
int retry_rpc(MyServiceProxy& proxy, int max_retries = 3) {
    for (int i = 0; i < max_retries; i++) {
        try {
            return proxy.operation();
        } catch (rrr::RpcException& e) {
            if (i == max_retries - 1) throw;
            usleep(100000 * (1 << i));  // Exponential backoff
        }
    }
}

// Scatter-gather pattern
void scatter_gather(vector<string> servers) {
    rrr::FutureGroup fg;
    vector<rrr::i32> results(servers.size());
    
    for (size_t i = 0; i < servers.size(); i++) {
        rrr::Client* cl = new rrr::Client(poll_mgr);
        cl->connect(servers[i]);
        MyServiceProxy* proxy = new MyServiceProxy(cl);
        fg.add(proxy->async_get_value(&results[i]));
    }
    
    fg.wait_all();
    // Process results...
}
```

## Server Programming

### Basic Server Setup

```cpp
// Create poll manager
rrr::PollMgr* poll_mgr = new rrr::PollMgr(1);

// Create thread pool for defer methods
rrr::ThreadPool* thread_pool = new rrr::ThreadPool(8);

// Create server
rrr::Server* server = new rrr::Server(poll_mgr, thread_pool);

// Register services
MyServiceImpl* service = new MyServiceImpl();
server->reg(service);

// Start listening
server->start("0.0.0.0:8000");

// Run event loop
while (running) {
    poll_mgr->poll(1000);  // 1 second timeout
}
```

### Service Implementation

```cpp
class MyServiceImpl : public MyService {
private:
    mutex mtx_;
    map<i64, Order> orders_;
    
public:
    void create_order(const rrr::i64& user_id, 
                     const rrr::double& amount,
                     rrr::i64* order_id) override {
        lock_guard<mutex> lock(mtx_);
        *order_id = generate_order_id();
        orders_[*order_id] = Order{*order_id, user_id, amount, "pending"};
    }
    
    void get_order(const rrr::i64& order_id,
                   Order* order) override {
        lock_guard<mutex> lock(mtx_);
        auto it = orders_.find(order_id);
        if (it != orders_.end()) {
            *order = it->second;
        } else {
            throw rrr::RpcException("Order not found");
        }
    }
};
```

### Multi-Service Server

```cpp
// Register multiple services on same server
server->reg(new OrderServiceImpl());
server->reg(new UserServiceImpl());
server->reg(new PaymentServiceImpl());

// Services are dispatched based on method IDs
```

### Connection Management

```cpp
class MyServiceImpl : public MyService {
public:
    // Called when client connects
    void client_connected(rrr::Client* client) {
        printf("Client connected: %s\n", client->addr().c_str());
    }
    
    // Called when client disconnects
    void client_disconnected(rrr::Client* client) {
        printf("Client disconnected: %s\n", client->addr().c_str());
    }
};
```

## Advanced Features

### Custom Marshaling

```cpp
struct ComplexType : public rrr::Marshallable {
    int field1;
    string field2;
    vector<int> field3;
    
    rrr::Marshal& marshal(rrr::Marshal& m) const override {
        return m << field1 << field2 << field3;
    }
    
    rrr::Marshal& unmarshal(rrr::Marshal& m) override {
        return m >> field1 >> field2 >> field3;
    }
};
```

### Raw Method Handler

```cpp
class MyServiceImpl : public MyService {
public:
    void raw_handler(rrr::Request* req, rrr::Reply* reply) {
        // Direct access to marshaled data
        rrr::Marshal* in = &req->m;
        rrr::Marshal* out = &reply->m;
        
        i32 input;
        *in >> input;
        
        i32 result = process(input);
        *out << result;
    }
};
```

### Event-Driven Programming

```cpp
// Custom event
class MyEvent : public rrr::Event {
public:
    void trigger() {
        // Handle event
    }
};

// Timeout event
rrr::TimeoutEvent* timeout = new rrr::TimeoutEvent(5000, []{
    printf("Timeout occurred\n");
});
poll_mgr->add(timeout);

// Quorum event (triggers when N events complete)
rrr::QuorumEvent* quorum = new rrr::QuorumEvent(3);
for (auto& future : futures) {
    quorum->add_child(future);
}
quorum->wait();
```

### Performance Tuning

```cpp
// Configure thread pool size
rrr::ThreadPool* tp = new rrr::ThreadPool(
    16,     // number of threads
    1000    // queue size
);

// Configure poll manager
rrr::PollMgr* pm = new rrr::PollMgr(
    2,      // number of poll threads
    10000   // max events per poll
);

// Connection options
client->set_timeout(5000);        // 5 second timeout
client->set_retry_count(3);       // retry failed RPCs
client->set_keepalive(true);      // enable TCP keepalive
```

## Best Practices

### 1. Service Design

- **Keep methods focused**: Each RPC method should do one thing well
- **Use defer for heavy operations**: Don't block the network thread
- **Batch when possible**: Reduce round trips with batch operations
- **Version your services**: Plan for backward compatibility

### 2. Error Handling

```cpp
// Define error codes
enum ErrorCode {
    OK = 0,
    NOT_FOUND = 1,
    INVALID_PARAM = 2,
    INTERNAL_ERROR = 3
};

// Return error codes
void method(const Input& in, Output* out, rrr::i32* error) {
    if (!validate(in)) {
        *error = INVALID_PARAM;
        return;
    }
    // Process...
    *error = OK;
}
```

### 3. Threading

- **Use thread-safe data structures** in services
- **Minimize lock contention** with fine-grained locking
- **Consider per-thread state** for read-heavy workloads
- **Use fast methods** for simple lookups and updates

### 4. Connection Management

- **Reuse connections**: Don't create new connections per RPC
- **Use connection pools** for high-throughput scenarios
- **Handle disconnections gracefully**: Implement retry logic
- **Monitor connection health**: Implement heartbeats if needed

### 5. Memory Management

```cpp
// Always release futures
rrr::Future* f = proxy.async_method();
f->wait();
f->release();  // Don't forget!

// Use RAII for automatic cleanup
class FutureGuard {
    rrr::Future* f_;
public:
    FutureGuard(rrr::Future* f) : f_(f) {}
    ~FutureGuard() { if (f_) f_->release(); }
};
```

## Troubleshooting

### Common Issues

#### Connection Refused
```cpp
// Check server is running
// Verify host:port is correct
// Check firewall settings
```

#### Method Not Found
```cpp
// Ensure service is registered: server->reg(service)
// Verify .rpc file matches implementation
// Regenerate code after .rpc changes
```

#### Marshaling Errors
```cpp
// Check parameter types match .rpc definition
// Ensure custom types implement Marshallable
// Verify byte order for cross-platform communication
```

#### Memory Leaks
```cpp
// Always release futures
// Delete services when shutting down
// Use valgrind to detect leaks
```

### Debugging Tips

1. **Enable logging**:
```cpp
rrr::Log::set_level(rrr::Log::DEBUG);
```

2. **Monitor RPC statistics**:
```cpp
server->get_stats().print();
```

3. **Use packet capture**:
```bash
tcpdump -i lo -w rpc.pcap port 8000
```

4. **Add request tracing**:
```cpp
class TracingService : public MyService {
    void method() override {
        auto start = chrono::steady_clock::now();
        // Process...
        auto end = chrono::steady_clock::now();
        log_latency(end - start);
    }
};
```

## Performance Considerations

### Latency Optimization

- **Use fast methods** for low-latency operations
- **Minimize marshaling** overhead with efficient data structures
- **Batch small requests** to amortize overhead
- **Consider UDP** for lossy but fast communication

### Throughput Optimization

- **Use connection pooling** to increase parallelism
- **Tune thread pool size** based on workload
- **Enable TCP_NODELAY** for small messages
- **Consider compression** for large payloads

### Memory Optimization

- **Use variable-length integers** (v32, v64) when possible
- **Reuse buffers** in hot paths
- **Limit request/response size** to prevent OOM
- **Monitor memory usage** with built-in stats

## Integration Examples

### With Janus/Mako

```cpp
// Transaction coordinator using RRR
class TxCoordinator {
    rrr::PollMgr* poll_;
    map<int, rrr::Client*> replicas_;
    
    void prepare_transaction(TxnId tid) {
        rrr::FutureGroup fg;
        for (auto& [id, client] : replicas_) {
            ReplicaProxy proxy(client);
            fg.add(proxy.async_prepare(tid));
        }
        fg.wait_all();
    }
};
```

### With External Systems

```cpp
// Bridge to external messaging system
class MessageBridge : public MessageService {
    KafkaProducer* kafka_;
    
    void send_message(const string& topic, 
                     const string& msg) override {
        kafka_->send(topic, msg);
    }
};
```

## Conclusion

The RRR RPC library provides a robust, high-performance foundation for building distributed systems. Its code generation approach eliminates boilerplate while maintaining type safety, and its event-driven architecture enables efficient resource utilization.

For more examples, see:
- `src/rrr/rpc_test/` - Simple test cases
- `src/deptran/` - Production usage in transaction processing
- `examples/` - Standalone examples

For questions or issues, consult the source code in `src/rrr/` or review the implementation patterns in the Janus/Mako codebase.