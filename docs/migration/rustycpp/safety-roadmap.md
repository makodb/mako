# SRPC/RPC Memory Safety Roadmap with RustyCpp

> **Historical snapshot.** This roadmap predates the canonical-Rust Goal 0
> ownership model and is not the current status or execution order. See
> `docs/dev/goal0_completion_plan.md` and `src/srpc/RUST_CANARY.md` for the live
> plan. In particular, `srpc.basetypes` is now owned by
> `src/srpc/src/basetypes.rs`; the `basetypes.hpp/cpp` actions below are
> superseded.

## Current Status
✅ **Phase 1 Complete**: RustyCpp infrastructure is already integrated in CMake
- `enable_borrow_checking()` is configured
- Build target for borrow checking exists
- Currently only enabled for `dbtest` target

## Roadmap Overview
Transform SRPC/RPC library to be memory-safe using RustyCpp borrow checking, eliminating undefined behavior and memory bugs.

---

## Phase 2: Core Foundation (Current Phase)
**Goal**: Make base types and fundamental utilities memory-safe  
**Timeline**: 1-2 weeks  
**Priority**: CRITICAL - Everything depends on these

### 2.1 Base Types Module (`src/srpc/base/`)
| File | Status | Key Changes Needed | Unsafe Blocks |
|------|--------|-------------------|---------------|
| `basetypes.hpp/cpp` | 🔴 Not Started | - Convert `RefCounted` to `std::shared_ptr`<br>- Fix `Counter` atomic operations<br>- Make `NoCopy` use delete operators | None expected |
| `threading.hpp/cpp` | 🔴 Not Started | - RAII wrappers for pthread primitives<br>- `std::lock_guard` instead of manual lock/unlock<br>- Fix `Pthread_*` macros | FFI with pthread |
| `misc.hpp/cpp` | 🔴 Not Started | - Replace C-style casts<br>- Fix `Timer` class lifetime | None expected |
| `debugging.hpp/cpp` | 🔴 Not Started | - Make stack trace safe<br>- Fix `verify()` macro | Signal handlers |
| `logging.hpp/cpp` | 🔴 Not Started | - Remove static mutable state<br>- Thread-safe initialization<br>- Fix `Log::info` vs `Log_info` usage | None expected |

### 2.2 Memory & Marshaling (`src/srpc/misc/`)
| File | Status | Key Changes Needed | Unsafe Blocks |
|------|--------|-------------------|---------------|
| `marshal.hpp/cpp` | 🔴 Not Started | - Use `std::span` for buffers<br>- Clear ownership model<br>- Safe type punning | Performance critical |
| `alock.hpp/cpp` | 🔴 Not Started | - Complex lock state machine<br>- Fix manual memory management | Lock-free ops |
| `recorder.hpp/cpp` | 🔴 Not Started | - Fix circular buffer<br>- Clear ownership | None expected |

**Success Criteria**:
- [ ] All base type files compile with `#pragma safe`
- [ ] No memory leaks in unit tests
- [ ] Performance regression < 2%

---

## Phase 3: Event System & Networking
**Goal**: Safe event loop and network handling  
**Timeline**: 1-2 weeks  
**Priority**: HIGH

### 3.1 Reactor Pattern (`src/srpc/reactor/`)
| File | Status | Key Changes Needed | Unsafe Blocks |
|------|--------|-------------------|---------------|
| `reactor.h/cc` | 🔴 Not Started | - Fix `Pollable` ownership<br>- Coroutine scheduler safety<br>- Event lifetime management | epoll/kqueue FFI |
| `event.h/cc` | 🔴 Not Started | - Smart pointers for events<br>- Fix callback captures<br>- QuorumEvent children ownership | None expected |
| `coroutine.cc` | 🔴 Not Started | - Stack allocation safety<br>- Context switching | setjmp/longjmp |
| `epoll_wrapper.h/cc` | 🔴 Not Started | - RAII for file descriptors<br>- Safe event structure | System calls |

### 3.2 RPC Core (`src/srpc/rpc/`)
| File | Status | Key Changes Needed | Unsafe Blocks |
|------|--------|-------------------|---------------|
| `server.hpp/cpp` | 🔴 Not Started | - Fix `ServerConnection` lifetime<br>- ThreadPool integration<br>- Remove `verify(0)` debug code | None expected |
| `client.hpp/cpp` | 🔴 Not Started | - Connection pooling safety<br>- Future lifetime management<br>- Request/Reply ownership | None expected |
| `utils.hpp/cpp` | 🔴 Not Started | - Safe network utilities | Socket operations |

**Success Criteria**:
- [ ] Server can accept connections safely
- [ ] Client can make RPC calls without leaks
- [ ] Benchmark (`rpcbench`) still works

---

## Phase 4: Integration & Migration
**Goal**: Update all dependent code to use safe APIs  
**Timeline**: 1 week  
**Priority**: MEDIUM

### 4.1 Code Generation Updates
- [ ] Update RPC code generator templates (`src/srpc/pylib/simplerpcgen/`)
- [ ] Ensure generated code is borrow-check compliant
- [ ] Fix service registration patterns

### 4.2 Test Migration
- [ ] Update test files in `test/` directory
- [ ] Fix `rpcbench` for full safety
- [ ] Migrate example code

**Success Criteria**:
- [ ] All generated code passes borrow checking
- [ ] All tests pass
- [ ] Examples work correctly

---

## Phase 5: Validation & Documentation
**Goal**: Ensure production readiness  
**Timeline**: 1 week  
**Priority**: MEDIUM

### 5.1 Testing & Validation
- [ ] Full test suite passes with borrow checking
- [ ] Memory leak detection with Valgrind
- [ ] AddressSanitizer clean
- [ ] Performance benchmarks acceptable

### 5.2 Documentation
- [ ] Document all `unsafe` blocks with justification
- [ ] Update SRPC-RPC guide
- [ ] Create migration guide for users
- [ ] Document new safety patterns

---

## Implementation Guidelines

### Safe Conversion Patterns

#### Pattern 1: Raw Pointer to Smart Pointer
```cpp
// ❌ UNSAFE
class Server {
    ServerConnection* conn_;
    ~Server() { delete conn_; }
};

// ✅ SAFE
class Server {
    std::unique_ptr<ServerConnection> conn_;
    // No manual delete needed
};
```

#### Pattern 2: Manual Lock Management to RAII
```cpp
// ❌ UNSAFE
void process() {
    pthread_mutex_lock(&mutex_);
    // ... code ...
    pthread_mutex_unlock(&mutex_);  // May not be called if exception
}

// ✅ SAFE
void process() {
    std::lock_guard<std::mutex> lock(mutex_);
    // ... code ...
    // Automatically unlocked
}
```

#### Pattern 3: C-style Arrays to Containers
```cpp
// ❌ UNSAFE
char* buffer = new char[size];
// ... use buffer ...
delete[] buffer;  // Easy to forget

// ✅ SAFE
std::vector<char> buffer(size);
// Automatically cleaned up
```

#### Pattern 4: Callback Lifetime Management
```cpp
// ❌ UNSAFE
void register_callback(std::function<void()> cb) {
    callbacks_.push_back(cb);  // cb may capture dead references
}

// ✅ SAFE
void register_callback(std::function<void()> cb) {
    // Use weak_ptr or shared_ptr in captures
    // Document lifetime requirements
}
```

### When `unsafe` is Acceptable

✅ **ACCEPTABLE** uses of `unsafe`:
1. **System Calls**: epoll, socket operations
2. **C Library FFI**: pthread, libc functions  
3. **Performance Critical**: After profiling proves need
4. **Lock-free Algorithms**: Atomic operations

Each `unsafe` block MUST have:
```cpp
// SAFETY: Explanation of why this is safe
// - Invariant 1 that ensures safety
// - Invariant 2 that ensures safety
#pragma unsafe
{
    // Minimal unsafe code
}
#pragma safe
```

---

## Progress Tracking

### Week-by-Week Goals

**Week 1** (Current)
- [ ] Make `Counter` class safe
- [ ] Make `RefCounted` safe  
- [ ] Fix one complete base file
- [ ] Establish patterns

**Week 2**
- [ ] Complete all base types
- [ ] Start marshaling system
- [ ] Fix threading primitives

**Week 3**
- [ ] Complete reactor system
- [ ] Start RPC server safety

**Week 4**
- [ ] Complete RPC client/server
- [ ] Update code generation

**Week 5**
- [ ] Full integration testing
- [ ] Performance validation
- [ ] Documentation

### Metrics Dashboard

| Metric | Current | Target | Status |
|--------|---------|--------|--------|
| Files with `#pragma safe` | 0/30 | 30/30 | 🔴 |
| Unsafe blocks | Unknown | < 20 | 🔴 |
| Test pass rate | 100% | 100% | 🟢 |
| Memory leaks | Unknown | 0 | 🔴 |
| Performance impact | 0% | < 5% | 🟢 |

---

## Quick Reference

### File Priority Order
1. **Start Here**: `src/srpc/base/basetypes.hpp`
2. **Then**: `src/srpc/base/threading.hpp`
3. **Then**: `src/srpc/misc/marshal.hpp`
4. **Then**: `src/srpc/reactor/reactor.h`
5. **Finally**: `src/srpc/rpc/server.hpp`

### Common Issues & Solutions

| Issue | Solution |
|-------|----------|
| "use of moved value" | Use `std::shared_ptr` or clone |
| "lifetime does not live long enough" | Extend lifetime or use heap allocation |
| "multiple mutable references" | Use `RefCell` pattern or redesign |
| "unsafe cast" | Use `std::bit_cast` (C++20) or `memcpy` |

### Build Commands

```bash
# Enable borrow checking for SRPC
cmake .. -DENABLE_BORROW_CHECKING=ON

# Build with borrow checking
make -j8

# Run borrow check only
make borrow_check_srpc

# Run tests with sanitizers
./test_srpc --asan --ubsan
```

---

## Next Immediate Actions

1. **Today**: 
   - [ ] Add SRPC to CMake borrow checking targets
   - [ ] Start with `src/srpc/base/basetypes.hpp`
   - [ ] Create first safe class (Counter)

2. **This Week**:
   - [ ] Complete base types module
   - [ ] Document patterns that work
   - [ ] Create unit tests for safe code

3. **First Milestone**:
   - [ ] One complete module passes borrow checking
   - [ ] No performance regression
   - [ ] Clear patterns established

---

## Notes & Lessons Learned

### What Works Well
- (To be filled as we progress)

### What Doesn't Work
- (To be filled as we progress)

### Patterns to Avoid
- (To be filled as we progress)

---

*Last Updated: [Auto-update on commit]*  
*Owner: Development Team*  
*Review: Weekly on Fridays*
