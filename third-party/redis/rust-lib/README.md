# rust-lib

The Rust Redis-compatible protocol and connection layer used by `makoCon`.
It uses a thread-per-core worker model with nonblocking sockets and synchronous
Mako transactions.

## Architecture

`rust_init(n_threads)` creates one nonblocking TCP listener and N worker
threads. The workers share the listener and take turns accepting connections,
so the first N persistent connections are assigned to distinct workers instead
of relying on kernel `SO_REUSEPORT` hashing. A connection remains owned by the
worker that accepted it.

Each worker:

- polls the shared listener, its client sockets, and a private wake socket;
- parses buffered RESP2/RESP3 commands and writes nonblocking replies;
- uses its own C++ thread-local Mako transaction state; and
- can service multiple persistent clients without blocking on an idle client.

The worker wake sockets deliver cross-worker notifications. For example,
`PUBLISH` queues a reply for each subscriber and wakes the worker that owns the
subscriber connection. Shared registries still use synchronization, so the
workers are not fully isolated.

## Connection Flow

```text
Redis clients
     |
shared nonblocking listener
     |
round-robin accept turn
     |
worker poll loop --- private wake socket
     |
RESP parse and reply buffering
     |
synchronous C++ Mako transaction
```

Pipelining is supported: a worker parses all complete frames already buffered
for a client before returning to its poll loop. Redis command execution remains
synchronous inside the owning worker.

## Protocol

- Streaming RESP2/RESP3 command parsing via `redis-protocol`.
- Protocol-specific replies, including RESP3 maps, doubles, and nulls.
- Redis connection, transaction, collection, and Pub/Sub compatibility owned by
  the Rust command layer.

## Dependencies

- `socket2` - Listener creation and socket configuration.
- `redis-protocol` (6.0.0) - RESP3 protocol parsing.
- `bytes` - Byte buffers.
- `itoa` - Integer formatting.

## C/C++ FFI Interface

### Functions Exported by Rust (called by C++)

```c
// Initialize the server with N worker threads
// Workers share one listener and service nonblocking client sockets.
// Returns true on success
bool rust_init(size_t n_threads);
```

### Functions C++ Must Implement (called by Rust)

```c
// Called by Rust when each worker thread starts
// Must initialize C++ thread-local state for this thread
void cpp_worker_thread_init(size_t thread_id);

// Execute a GET/SET request synchronously
// op: 1=GET, 2=SET
// Returns true on success, out_ptr/out_len set for GET results
bool cpp_execute_request_sync(
    uint32_t op,
    const uint8_t* key_ptr, size_t key_len,
    const uint8_t* val_ptr, size_t val_len,
    uint8_t** out_ptr, size_t* out_len
);

// Free buffer returned by cpp_execute_request_sync
void cpp_free_buf(uint8_t* ptr, size_t len);
```

### Usage Example (makoCon.cc)

```cpp
#include <mako.hh>

RustWrapper* g_rust_wrapper_instance = nullptr;

extern "C" {
    // Rust calls this when each worker thread starts
    void cpp_worker_thread_init(size_t thread_id) {
        if (g_rust_wrapper_instance) {
            g_rust_wrapper_instance->ensure_thread_info();
        }
    }

    // Rust calls this for each GET/SET request
    bool cpp_execute_request_sync(uint32_t op, ...) {
        // ... handle request using thread-local state ...
    }

    void cpp_free_buf(uint8_t* ptr, size_t len) {
        if (ptr) std::free(ptr);
    }
}

extern "C" bool rust_init(size_t n_threads);

int main() {
    // 1. Setup database
    abstract_db *db = new mbta_wrapper;
    db->init();

    // 2. Setup wrapper (stores global instance pointer)
    RustWrapper* wrapper = new RustWrapper();
    wrapper->db = db;
    wrapper->customerTable = db->open_index("customer_0");

    // 3. Start Rust server (spawns N threads, each calls cpp_worker_thread_init)
    rust_init(8);

    // Server now accepting on 127.0.0.1:6380
    while (true) { std::this_thread::sleep_for(std::chrono::seconds(1)); }
}
```

## Command Scope And Validation

The server now covers the scoped Redis connection, string, keyspace,
transaction, expiry, set, list, hash, sorted-set, blocking, and Pub/Sub command
families. The maintained command tiers, correctness suites, dated results, and
intentional divergences are documented in
`third-party/redis/compat/README.md`.

## Building

```bash
cd rust-lib
cargo build --release
```

Output: `target/release/librust_redis.a` (static library)

## Configuration

| Setting | Value | Notes |
|---------|-------|-------|
| Listen address | `127.0.0.1:6380` | Hardcoded |
| Read buffer | 16 KB | Matches Redis `PROTO_IOBUF_LEN` |
| Write buffer | 16 KB | Matches Redis `PROTO_REPLY_CHUNK_BYTES` |
| Listen backlog | 1024 | Per-socket pending connection queue |
