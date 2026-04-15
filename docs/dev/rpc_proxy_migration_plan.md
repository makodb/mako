# RPC Inheritance-to-Proxy Migration Plan

## Overview

Replace all C++ virtual inheritance in `src/rrr/rpc/` with the [proxy library](https://github.com/ngcpp/proxy) — a C++20 header-only library that provides polymorphism without inheritance via type erasure and compile-time facades.

**Why proxy?**
- No vtable overhead — proxy uses small object optimization and inline storage
- Non-intrusive — implementations don't inherit from an interface base class
- Explicit allocation control (no hidden heap allocations for small types)
- Better composability — facades can be combined without diamond inheritance

## Inheritance Audit Summary

| # | Base Class | Virtual Methods | Implementations | Storage Pattern | Files |
|---|-----------|----------------|-----------------|-----------------|-------|
| 1 | `Pollable` | 10 | 3 (ServerListener, ServerConnection, ClientConnection) | `Arc<Pollable>` | 3 |
| 2 | `Service` | 2 | Many (generated RPC services) | `Box<Service>` in `RefCell` | 1 + all services |
| 3 | `LogStorage` | 18 | 2 (InMemoryLogStorage, RocksDBLogStorage) | `shared_ptr<LogStorage>` | 4 |
| 4 | `SnapshotManager` | 10 | 1 (FileSnapshotManager) | `shared_ptr<SnapshotManager>` | 2 |
| 5 | `SnapshotReader` | 4 | 1 (FileSnapshotReader) | `unique_ptr<SnapshotReader>` | 2 |
| 6 | `SnapshotWriter` | 4 | 1 (FileSnapshotWriter) | `unique_ptr<SnapshotWriter>` | 2 |
| 7 | `Marshallable` | 4 | Many | `shared_ptr<Marshallable>` | 1 + many |
| 8 | `NoCopy` | 0 | 2 (Server, Counter) | N/A (semantic only) | 1 |
| 9 | `RefCounted` | 1 | Legacy | N/A (manual ref count) | 1 |
| 10 | `RpcException` | 1 | N/A (inherits `std::exception`) | thrown/caught | 1 |

**Total: ~54 virtual methods across 10 hierarchies, 12 files to change.**

## Migration Strategy

### What to Migrate

Migrate hierarchies 1-6 (Pollable, Service, LogStorage, SnapshotManager/Reader/Writer). These are the core RPC interfaces with clear facade boundaries.

### What to Leave Alone

- **NoCopy** (#8): Not polymorphic. Replace with `= delete` on copy constructor directly (no proxy needed).
- **RefCounted** (#9): Legacy, being replaced by `rusty::Arc`. Not a proxy candidate.
- **RpcException** (#10): Inherits `std::exception`. Standard library integration — leave as inheritance.
- **Marshallable** (#7): Deeply embedded across the codebase (not just RPC). Migrate separately in a later phase.

## Phased Plan

### Phase 1: Add proxy library and migrate Pollable (smallest, most impactful)

**Files**: `epoll_wrapper.h`, `server.hpp`, `client.hpp`

**Before (inheritance):**
```cpp
class Pollable {
public:
    virtual int fd() const = 0;
    virtual int poll_mode() const = 0;
    virtual bool handle_read() = 0;
    virtual int handle_write() = 0;
    virtual void handle_error() = 0;
    virtual void close() = 0;
    virtual size_t content_size() = 0;
    virtual bool check_pending_write_update() const = 0;
    virtual bool is_closed() const = 0;
    virtual ~Pollable() = default;
};

class ServerConnection : public Pollable { ... };
```

**After (proxy):**
```cpp
PRO_DEF_MEM_DISPATCH(MemFd, fd);
PRO_DEF_MEM_DISPATCH(MemPollMode, poll_mode);
PRO_DEF_MEM_DISPATCH(MemHandleRead, handle_read);
PRO_DEF_MEM_DISPATCH(MemHandleWrite, handle_write);
PRO_DEF_MEM_DISPATCH(MemHandleError, handle_error);
PRO_DEF_MEM_DISPATCH(MemClose, close);
PRO_DEF_MEM_DISPATCH(MemContentSize, content_size);
PRO_DEF_MEM_DISPATCH(MemCheckPending, check_pending_write_update);
PRO_DEF_MEM_DISPATCH(MemIsClosed, is_closed);

struct PollableFacade : pro::facade_builder
    ::add_convention<MemFd, int() const>
    ::add_convention<MemPollMode, int() const>
    ::add_convention<MemHandleRead, bool()>
    ::add_convention<MemHandleWrite, int()>
    ::add_convention<MemHandleError, void()>
    ::add_convention<MemClose, void()>
    ::add_convention<MemContentSize, size_t()>
    ::add_convention<MemCheckPending, bool() const>
    ::add_convention<MemIsClosed, bool() const>
    ::build {};

// ServerConnection no longer inherits Pollable
class ServerConnection { ... };  // Same methods, no `: public Pollable`

// Usage: pro::proxy<PollableFacade> instead of Arc<Pollable>
pro::proxy<PollableFacade> p = pro::make_proxy<PollableFacade>(ServerConnection(...));
p->fd();           // Dispatched via proxy, not vtable
p->handle_read();  // Same
```

**Steps:**
1. Add proxy as a git submodule in `third-party/proxy/`.
2. Define `PollableFacade` in `epoll_wrapper.h`.
3. Remove `Pollable` base class.
4. Remove `: public Pollable` from ServerListener, ServerConnection, ClientConnection.
5. Change `Arc<Pollable>` to `pro::proxy<PollableFacade>` in Epoll and PollThread.
6. Update all call sites.
7. Run tests.

**Estimated LOC**: ~200 (mostly mechanical find-replace)

### Phase 2: Migrate Service interface

**Files**: `server.hpp`, all generated service implementations

**Before:**
```cpp
class Service {
    virtual int __reg_to__(Server&, size_t) = 0;
    virtual void __dispatch__(i32 rpc_id, Box<Request> req, WeakServerConnection) = 0;
};
```

**After:**
```cpp
PRO_DEF_MEM_DISPATCH(MemRegTo, __reg_to__);
PRO_DEF_MEM_DISPATCH(MemDispatch, __dispatch__);

struct ServiceFacade : pro::facade_builder
    ::add_convention<MemRegTo, int(Server&, size_t)>
    ::add_convention<MemDispatch, void(i32, Box<Request>, WeakServerConnection)>
    ::build {};
```

**Steps:**
1. Define `ServiceFacade` in `server.hpp`.
2. Replace `Box<Service>` with `pro::proxy<ServiceFacade>`.
3. Update rpcgen code generator to emit classes without `: public Service`.
4. Update `Server::reg_service()` to accept `pro::proxy<ServiceFacade>`.
5. Run tests.

**Estimated LOC**: ~100 (but rpcgen changes affect all generated code)

### Phase 3: Migrate LogStorage

**Files**: `log_storage.hpp`, `memory_log_storage.hpp`, `rocksdb_log_storage.hpp`, `recovery_manager.hpp`

**Before:**
```cpp
class LogStorage {
    virtual Option<LogEntry> get(slotid_t) const = 0;
    virtual bool put(const LogEntry&) = 0;
    // ... 16 more methods
};
class InMemoryLogStorage : public LogStorage { ... };
class RocksDBLogStorage : public LogStorage { ... };
```

**After:**
```cpp
PRO_DEF_MEM_DISPATCH(MemGet, get);
PRO_DEF_MEM_DISPATCH(MemPut, put);
// ... one per method

struct LogStorageFacade : pro::facade_builder
    ::add_convention<MemGet, Option<LogEntry>(slotid_t) const>
    ::add_convention<MemPut, bool(const LogEntry&)>
    // ... 16 more
    ::build {};
```

**Steps:**
1. Define `LogStorageFacade` with all 18 methods.
2. Remove `LogStorage` base class.
3. Remove `: public LogStorage` from InMemoryLogStorage and RocksDBLogStorage.
4. Change `shared_ptr<LogStorage>` to `pro::proxy<LogStorageFacade>` in RecoveryManager.
5. Run tests.

**Estimated LOC**: ~250

### Phase 4: Migrate SnapshotManager/Reader/Writer

**Files**: `snapshot_manager.hpp`, `file_snapshot_manager.hpp`

Three facades: `SnapshotManagerFacade` (10 methods), `SnapshotReaderFacade` (4 methods), `SnapshotWriterFacade` (4 methods).

**Steps:**
1. Define three facades.
2. Remove base classes.
3. Remove inheritance from File* implementations.
4. Change storage types to `pro::proxy<*Facade>`.
5. Run tests.

**Estimated LOC**: ~200

### Phase 5: Cleanup NoCopy

Replace `class X : public NoCopy` with direct deleted copy constructors:
```cpp
// Before
class Server : public NoCopy { ... };

// After
class Server {
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ...
};
```

**Estimated LOC**: ~20

## Prerequisites

- **C++20**: The proxy library requires C++20. Verify the project compiles with `-std=c++20` (currently uses C++17).
- **Compiler**: GCC 13.1+, Clang 16+, or MSVC 19.31+. Check current compiler version.
- **Header-only**: No build system changes beyond adding the submodule and include path.

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| C++20 requirement | Check compiler version; may need CMake flag change |
| proxy object size | Verify small object optimization kicks in for our types |
| Generated code (rpcgen) | Phase 2 requires rpcgen changes — test generated output carefully |
| Thread safety | proxy is not thread-safe by default; ensure Arc/Mutex wrapping as before |
| Marshallable deep dependency | Defer to later phase — too many dependents outside RPC |

## Estimated Total Effort

| Phase | LOC | Days |
|-------|-----|------|
| Phase 1: Pollable | ~200 | 1-2 |
| Phase 2: Service | ~100 | 1 |
| Phase 3: LogStorage | ~250 | 1-2 |
| Phase 4: Snapshot* | ~200 | 1 |
| Phase 5: NoCopy | ~20 | 0.5 |
| **Total** | **~770** | **4-6 days** |
