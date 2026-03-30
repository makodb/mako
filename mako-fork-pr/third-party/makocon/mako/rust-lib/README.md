# rust-lib

A high-performance Redis-compatible server written in Rust, designed for maximum scalability using the **thread-per-core** architecture with **100% synchronous blocking I/O**.

## Architecture

### Thread-Per-Core Model (100% Synchronous)

This server uses a **thread-per-core** model where each OS thread is completely isolated with pure blocking I/O:

```
┌───────────────────┐  ┌───────────────────┐  ┌───────────────────┐
│   OS Thread 0     │  │   OS Thread 1     │  │   OS Thread 2     │
│                   │  │                   │  │                   │
│  Blocking Socket  │  │  Blocking Socket  │  │  Blocking Socket  │
│  (SO_REUSEPORT)   │  │  (SO_REUSEPORT)   │  │  (SO_REUSEPORT)   │
│                   │  │                   │  │                   │
│  accept() blocks  │  │  accept() blocks  │  │  accept() blocks  │
│  read() blocks    │  │  read() blocks    │  │  read() blocks    │
│  write() blocks   │  │  write() blocks   │  │  write() blocks   │
└─────────┬─────────┘  └─────────┬─────────┘  └─────────┬─────────┘
          │                      │                      │
          │      NO SHARED STATE BETWEEN THREADS        │
          │                      │                      │
          └──────────────────────┴──────────────────────┘
                                 │
                        Port 6380 (kernel)
```

### Key Design Decisions

| Component | Choice | Rationale |
|-----------|--------|-----------|
| **I/O Model** | 100% blocking | Simple, predictable, no async complexity |
| **Threading** | N × `std::thread` | Each thread has isolated socket, no shared state |
| **Accept** | `SO_REUSEPORT` | Kernel distributes connections across threads |
| **Processing** | Synchronous | One client at a time per thread, fully blocking |

### Why 100% Synchronous?

- **Simplicity** - No async runtime, no coroutines, no task scheduling
- **Predictable latency** - No context switching within a thread
- **Zero contention** - No shared data structures between threads
- **Linear scalability** - Throughput scales with core count
- **Better cache locality** - Each thread's data stays in its L1/L2 cache

This is a simplified version of the model used by **Seastar** (ScyllaDB) and similar systems.

### Thread Independence

Each thread operates **completely independently** - no coordination during request processing:

```
Thread 0                    Thread 1                    Thread 2
    │                           │                           │
    ▼                           ▼                           ▼
accept() ←── Client A      accept() ←── Client B      accept() ←── Client C
    │                           │                           │
    ▼                           ▼                           ▼
handle_client_sync()       handle_client_sync()       handle_client_sync()
    │                           │                           │
    ▼                           ▼                           ▼
cpp_execute_request()      cpp_execute_request()      cpp_execute_request()
    │                           │                           │
    ▼                           ▼                           ▼
(uses tl_arena_0)          (uses tl_arena_1)          (uses tl_arena_2)

     ↑                          ↑                          ↑
     └── COMPLETELY INDEPENDENT ─┴── NO SHARED STATE ──────┘
```

Each thread:
- Accepts **its own** connections (kernel distributes via SO_REUSEPORT)
- Uses **its own** thread-local C++ buffers (`tl_arena`, `tl_txn_obj_buf`)
- Executes **its own** database transactions
- Never waits for or coordinates with other threads

## System Flow

```
══════════════════════════════════════════════════════════════════════════════
                              INITIALIZATION PHASE
══════════════════════════════════════════════════════════════════════════════

C++ main()
    │
    ├── Setup database, RustWrapper, tables
    │
    └── rust_init(n_threads)
            │
            └── for thread_id in 0..n_threads:
                    │
                    └── std::thread::spawn()
                            │
                            ├── Create SO_REUSEPORT socket (BLOCKING mode)
                            │   (dedicated kernel accept queue)
                            │
                            ├── cpp_worker_thread_init(thread_id) ──────► C++
                            │       │
                            │       └── ensure_thread_info()
                            │               ├── mbta_type::thread_init()
                            │               ├── allocate tl_arena
                            │               └── setup thread-local buffers
                            │
                            ├── barrier.wait() (sync all threads)
                            │
                            └── Enter Accept Loop...

══════════════════════════════════════════════════════════════════════════════
                              REQUEST PHASE (per connection)
══════════════════════════════════════════════════════════════════════════════

Redis Client (redis-benchmark, redis-cli, etc.)
    │
    │  TCP connection to 127.0.0.1:6380
    ▼
Kernel (SO_REUSEPORT load balancing)
    │
    │  Distributes to one of N sockets
    ▼
Rust Worker Thread (e.g., thread-3)
    │
    ├── listener.accept()  ← BLOCKS until connection arrives
    │
    └── handle_client_sync(stream)  ← BLOCKS until client done
            │
            ├── stream.read()  ← BLOCKS until data arrives
            │
            ├── Parse RESP3 frame (GET key / SET key value)
            │
            ├── cpp_execute_request_sync(op, key, val) ──────► C++
            │       │                                    BLOCKS
            │       └── RustWrapper::execute_request()
            │               │
            │               ├── ensure_thread_info() (already done)
            │               │
            │               ├── db->new_txn()
            │               │
            │               ├── customerTable->get() or ->put()
            │               │
            │               ├── db->commit_txn()
            │               │
            │               └── return result ◄────────────────── C++
            │
            ├── Write RESP3 response to buffer
            │
            ├── writer.flush()  ← BLOCKS until sent
            │
            └── Loop for next command (pipelining supported)
```

## Protocol

- **RESP3 Protocol**: Parses Redis commands using streaming decoder

## Dependencies

- `socket2` - Low-level socket control for `SO_REUSEPORT`
- `redis-protocol` (6.0.0) - RESP3 protocol parsing
- `bytes` - Efficient byte buffer handling
- `itoa` - Fast integer-to-string conversion

## C/C++ FFI Interface

### Functions Exported by Rust (called by C++)

```c
// Initialize the server with N worker threads
// Each thread: own blocking socket (SO_REUSEPORT), 100% synchronous I/O
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

## Supported Commands

- `GET <key>` - Retrieve value for key
- `SET <key> <value>` - Store key-value pair

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
