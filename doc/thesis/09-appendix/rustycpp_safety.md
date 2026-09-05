# RustyCpp Safety Annotations in Raft Code

## 1. Overview

The Raft implementation uses RustyCpp for memory safety and ownership
tracking.  Every function and significant code block has safety
annotations (`@safe` or `@unsafe`), and Rust-style smart pointers
replace raw pointers and STL ownership types.

The inventory below covers the core files discussed in this appendix. It is
an architectural snapshot, not a generated count of every annotation in the
current Raft directory.

## 2. Annotation Summary by File

| File | @safe | @unsafe | rusty:: Types | Notes |
|------|-------|---------|---------------|-------|
| `server.cc` (1829 lines) | 8 | 33 | Arc, Box | Persistence methods all @unsafe |
| `server.h` (637 lines) | 5 | 26 | Arc, Box, Function | Metadata keys, raw pointer casts |
| `commo.cc` (287 lines) | 5 | 0 | Arc | RPC communication all @safe |
| `commo.h` (128 lines) | 5 | 2 | Arc, Option | QuorumEvent inline @unsafe |
| `service.cc` (112 lines) | 5 | 0 | -- | All RPC handlers @safe |
| `service.h` (83 lines) | 1 | 0 | -- | Class declaration @safe |
| `frame.cc` (206 lines) | 7 | 0 | Arc, Box | Factory methods all @safe |
| `frame.h` (49 lines) | 1 | 0 | Arc, Box, Cell, Option | Shared state |
| `coordinator.cc` (199 lines) | 5 | 2 | Arc, Cell | Output params @unsafe |
| `coordinator.h` (82 lines) | 3 | 0 | Arc, Cell, Option | Slot management |

## 3. Which Methods Are @safe and Why

### 3.1 All @safe — RPC Service Layer

All 5 RPC handlers in `service.cc` are `@safe`:

| Method | Line | Why Safe |
|--------|------|----------|
| `RaftServiceImpl()` | 14 | Constructor with no pointer operations |
| `HandleVote()` | 22 | Uses lambda instead of `std::bind` to avoid pointer ops |
| `HandleAppendEntries()` | 36 | Delegates to server via reference |
| `HandleEmptyAppendEntries()` | 69 | Delegates to server via reference |
| `HandleTimeoutNow()` | 101 | Delegates to server via reference |

### 3.2 All @safe — Frame Factory Methods

All 6 frame methods are `@safe` (lines 23-206 of `frame.cc`):
`RaftFrame()`, `~RaftFrame()`, `CreateCoordinator()`,
`CreateScheduler()`, `CreateCommo()`,
`CreateRpcServices()`.

These construct new objects using `rusty::make_box` and smart pointers.

### 3.3 All @safe — Communication Layer

All 5 commo methods are `@safe` (lines 20-227 of `commo.cc`):
`RaftCommo()`, `SendAppendEntries2()`, `SendAppendEntries()`,
`BroadcastVote()`, `SendTimeoutNow()`.

These create `rusty::Arc<Future>` objects for async RPC callbacks.

### 3.4 @safe — Read-Only Accessors

| Method | File:Line | Why Safe |
|--------|-----------|----------|
| `IsLeader()` | server.h:287 | Returns boolean state |
| `GetElectionTimeout()` | server.cc:347 | Returns integer |
| `IsDisconnected()` | server.cc:438 | Checks proxy map |
| `GetUncommittedCount()` | server.cc:197 | Read-only log query |
| `GetInstance()` | server.h:386 | Returns existing reference |
| `SetLogStorage()` | server.h:430 | Simple setter |
| `GetLogStorage()` | server.h:439 | Simple getter |
| `SetSnapshotManager()` | server.h:478 | Simple setter |
| `GetSnapshotManager()` | server.h:487 | Simple getter |

## 4. Which Methods Are @unsafe and Why

### 4.1 Persistence Methods (I/O)

All persistence methods are `@unsafe` because they call the
`LogStorage` API which performs disk I/O:

| Method | File:Line | Reason |
|--------|-----------|--------|
| `PersistTermAndVote()` | server.cc:35 | LogStorage `set_metadata()` |
| `PersistVote()` | server.cc:46 | LogStorage `set_metadata()` |
| `PersistCommitIndex()` | server.cc:56 | LogStorage `set_metadata()` |
| `PersistLogEntry()` | server.cc:66 | LogStorage `put()` |
| `PersistLogEntries()` | server.cc:82 | LogStorage `put_batch()` |
| `RecoverFromStorage()` | server.cc:104 | LogStorage `get_metadata()` + `get_range()` |
| `ReplayCommittedEntries()` | server.cc:153 | Calls `app_next_` callback |
| `CompactLog()` | server.cc:205 | LogStorage `remove_range()` |

### 4.2 State Mutation

| Method | File:Line | Reason |
|--------|-----------|--------|
| `doVote()` | server.h:160 | Modifies `currentTerm_`, `vote_for_` |
| `OnRequestVote()` | server.h:503 | Calls `doVote()` and `std::function` callback |
| `OnAppendEntries()` | server.h:512 | Modifies log entries and commit index |
| `OnTimeoutNow()` | server.h:543 | Triggers immediate election |
| `setIsLeader()` | server.cc:443 | Modifies leader state |
| `Start()` | server.h:302 | Initiates consensus participation |
| `resetTimer()` | server.h:228 | Calls `timer_->start()` |
| `removeCmd()` | server.h:567 | Modifies log |

### 4.3 RPC and Connection Management

| Method | File:Line | Reason |
|--------|-----------|--------|
| `Disconnect()` | server.h:550 | Modifies connection state and proxy maps |
| `Reconnect()` | server.h:553 | Calls Disconnect and resetTimer |
| `commo()` | server.h:155 | Returns raw pointer cast |
| `GetState()` | server.h:305 | Dereferences raw pointers |
| `GetRaftInstance()` | server.h:405 | Returns mutable reference |

### 4.4 Random Number Generation

| Method | File:Line | Reason |
|--------|-----------|--------|
| `randDuration()` | server.h:238 | Calls `RandomGenerator::rand_double` (not annotated) |

### 4.5 Monitoring

| Method | File:Line | Reason |
|--------|-----------|--------|
| `heartbeat()` | server.cc:627 | Complex function with multiple inline @unsafe blocks |
| `StartElectionTimer()` | server.cc:1187 | Calls `Fiber::create_run()` |
| `StartLeadershipTransferMonitoring()` | server.cc:1588 | Thread creation |

## 5. RustyCpp Types Used in Raft

### 5.1 `rusty::Arc<T>` — Thread-Safe Shared Ownership

Used for objects shared across threads or coroutines:

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Arc<Cell<slotid_t>> slot_hint_` | frame.h:19, coordinator.h:41 | Shared slot counter between frame and coordinator |
| `Arc<PollThread>` | commo.h:62, raft_worker.h:72 | Shared poll thread reference |
| `Arc<Future>` | commo.cc:46,116 | Async RPC future handles |
| `Arc<ServerStatus>` | raft_worker.h:79 | Shared server status |
| `Arc<OneTimeJob>` | testconf.cc:295 | Test helper |

### 5.2 `rusty::Box<T>` — Single Ownership

Used for owned resources with automatic cleanup:

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Box<Timer> timer_` | server.h:85 | Election timer owned by RaftServer |
| `Box<RaftServiceImpl>` | frame.cc:200 | RPC service instance |

Initialised via `rusty::Box<Timer>::make(Timer())` (server.cc:317).

### 5.3 `rusty::Cell<T>` — Interior Mutability

Used for shared mutable state (trivially-copyable types):

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Arc<Cell<slotid_t>> slot_hint_` | coordinator.h:41 | Slot counter with `.get()` and `.set()` |

The `Cell<T>` pattern allows safe mutation through shared references,
matching Rust's `Cell<T>` semantics for `Copy` types.

### 5.4 `rusty::Option<T>` — Optional Values

Used for values that may or may not be present:

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Option<Arc<PollThread>>` | commo.h:62, raft_worker.h:72 | Optional poll thread |
| `Option<Arc<ServerStatus>>` | raft_worker.h:79 | Optional server status |

Operations: `.is_some()`, `.as_ref().unwrap()`, `.clone()`.

### 5.5 `rusty::Function<Sig>` — Type-Erased Callable

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Function<void()> cb` | server.h:510, 548 | Callback parameters for OnRequestVote, OnTimeoutNow |

### 5.6 `rusty::Mutex<T>` — Thread-Safe Lock

Used in the persistence layer (not directly in raft/ but in the
`LogStorage` implementations):

| Usage | File | Purpose |
|-------|------|---------|
| `Mutex<std::map<...>>` | memory_log_storage.hpp | Thread-safe in-memory log |

## 6. Borrow Checking Configuration

**File**: `CMakeLists.txt` (lines 745-798)

### 6.1 Checked Files

All Raft source files under `src/deptran/raft/*.cc` are included in
the `raft_borrow` build target, **except**:

### 6.2 Excluded Files

| File | Reason |
|------|--------|
| `testconf.cc` | Test infrastructure with class inheritance patterns incompatible with @safe code |
| `test.cc` | Test harness depending on testconf.h |
| `raft_main_helper.cc` | Third-party headers (YAML, etc.) generate 1000+ false positive violations |

### 6.3 Build Command

```bash
cd build && make borrow_check_raft
```

Or for all checked files:

```bash
make borrow_check_all_dbtest
```

## 7. Key Safety Patterns

### 7.1 Arc<Cell<T>> for Shared Mutable State

```cpp
// frame.h:19 — shared between RaftFrame and CoordinatorRaft
rusty::Arc<rusty::Cell<slotid_t>> slot_hint_;

// coordinator.cc:27 — read/write via Cell operations
slotid_t GetNextSlot() {
    auto current = slot_hint_->get();    // @safe read
    slot_hint_->set(current + 1);         // @safe write
    return current;
}
```

### 7.2 Box<T> for Owned Resources

```cpp
// server.h:85 — timer owned by RaftServer
rusty::Box<Timer> timer_;

// server.cc:317 — initialisation in constructor
timer_(rusty::Box<Timer>::make(Timer()))
```

The `Box<Timer>` ensures the timer is automatically destroyed when the
RaftServer is destroyed, preventing memory leaks.

### 7.3 Lambda Over std::bind

```cpp
// service.cc:22 — @safe refactored to use lambda
// Before (unsafe): std::bind(&RaftServer::OnRequestVote, svr, ...)
// After (safe):
auto callback = [svr](auto&&... args) {
    svr->OnRequestVote(std::forward<decltype(args)>(args)...);
};
```

Lambdas are preferred over `std::bind` because they avoid implicit
pointer operations that the borrow checker cannot verify.

### 7.4 Inline @unsafe Blocks

```cpp
// coordinator.cc:115
// @safe - Uses @unsafe blocks for pointer operations
void AppendEntries(...) {
    // ... safe code ...
    // @unsafe { output parameter write }
    *output_param = value;
    // ... safe code ...
}
```

Individual unsafe operations within an otherwise-safe function are
marked with inline `// @unsafe { reason }` comments.

## 8. Safety Statistics

| Category | Files highlighted in this appendix |
|----------|------------------------------------------|
| Files whose listed methods are all @safe | service.cc, frame.cc |
| Files with @unsafe methods or blocks | server.cc, server.h, coordinator.cc, commo.h |

The @unsafe annotations cluster in two areas:
1. **Persistence** (8 methods): I/O through LogStorage
2. **State mutation** (16 methods): Modifying consensus state, timers,
   connections, and log entries

The RPC service layer, frame, and communication layer are entirely
`@safe`.
