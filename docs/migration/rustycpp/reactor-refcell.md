# Reactor RefCell Refactoring Plan

## Problem
reactor.cc uses C++ `mutable` keyword to allow modification of fields in const methods.
This is NOT safe - C++ mutable bypasses const-correctness without any runtime safety checks.

## Solution
Replace `mutable T` fields with `rusty::RefCell<T>` to get proper interior mutability with
runtime borrow checking.

## Fields to Refactor

### Reactor class (reactor.h lines 150-169)

| Current | New Type | Notes |
|---------|----------|-------|
| `mutable int server_id_{0}` | `rusty::Cell<int>` | Simple Copy type, use Cell |
| `mutable rusty::VecDeque<...> all_events_` | `rusty::RefCell<rusty::VecDeque<...>>` | Container |
| `mutable rusty::VecDeque<...> waiting_events_` | `rusty::RefCell<rusty::VecDeque<...>>` | Container |
| `mutable rusty::VecDeque<...> timeout_events_` | `rusty::RefCell<rusty::VecDeque<...>>` | Container |
| `mutable rusty::VecDeque<...> composite_events_` | `rusty::RefCell<rusty::VecDeque<...>>` | Container |
| `mutable std::vector<...> network_events_` | `rusty::RefCell<std::vector<...>>` | Container |
| `mutable rusty::VecDeque<...> ready_network_events_` | `rusty::RefCell<rusty::VecDeque<...>>` | Container |
| `mutable rusty::BTreeSet<...> coros_` | `rusty::RefCell<rusty::BTreeSet<...>>` | Container |
| `mutable std::vector<...> available_coros_` | `rusty::RefCell<std::vector<...>>` | Container |
| `mutable std::unordered_map<...> processors_` | `rusty::RefCell<std::unordered_map<...>>` | Container |
| `mutable std::unordered_map<...> opened_files_` | `rusty::RefCell<std::unordered_map<...>>` | Container |

### PollThread class (already safe)
- `mutable rusty::sync::mpsc::Sender<PollCommand>` - Already rusty type with interior mutability
- `mutable std::atomic<...>` - Atomics are already thread-safe

## Access Pattern Changes

### Read access
```cpp
// Before (mutable field)
auto len = timeout_events_.len();

// After (RefCell)
auto guard = timeout_events_.borrow();
auto len = guard->len();
```

### Write access
```cpp
// Before (mutable field)
timeout_events_.push_back(event);

// After (RefCell)
auto guard = timeout_events_.borrow_mut();
guard->push_back(event);
```

## Task Breakdown

### Task 1: server_id_ to Cell (~20 LOC)
- Change type in reactor.h
- Update access sites (get/set pattern)

### Task 2: Event containers to RefCell (~100 LOC)
- all_events_, waiting_events_, timeout_events_, composite_events_
- Update loop(), check_timeout(), and related methods

### Task 3: Network event containers to RefCell (~50 LOC)
- network_events_, ready_network_events_
- **COMPLETED: Removed as dead code** - These fields were declared but never used anywhere in the codebase

### Task 4: Coroutine containers to RefCell (~80 LOC)
- coros_, available_coros_
- Update create_run_coroutine(), recycle(), continue_coro()

### Task 5: Map containers to RefCell (~30 LOC)
- processors_, opened_files_
- **COMPLETED: Removed as dead code** - These fields were declared but never used anywhere in the codebase

### Task 6: Remove @unsafe blocks, add to borrow checking (~20 LOC)
- Remove @unsafe annotations from refactored methods
- Add reactor.cc to SRPC_BORROW_SRC
- **COMPLETED**: Added reactor.cc to borrow checking (now 15 SRPC files pass). Fixed Rc::clone() false positive with @unsafe annotation.

## Estimated Total: ~300 LOC changes

## Benefits
1. Runtime borrow checking catches double-borrow bugs
2. Methods can be @safe instead of @unsafe
3. reactor.cc can be under borrow checking
4. Follows Rust idioms for interior mutability
