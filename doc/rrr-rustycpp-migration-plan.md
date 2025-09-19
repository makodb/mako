# RRR/RPC RustyCpp Memory Safety Migration Plan

## Overview
This document outlines the plan to make the RRR/RPC library memory-safe using rusty-cpp borrow checking. The goal is to mark all functions as safe and only use `unsafe` when absolutely necessary.

## Phase 1: Assessment and Preparation (Week 1)

### 1.1 Inventory Current State
- [ ] Count total files in `src/rrr/`
- [ ] Identify all pointer usage patterns
- [ ] Document existing memory management patterns
- [ ] List all classes with manual memory management
- [ ] Identify shared ownership patterns

### 1.2 Set Up RustyCpp Infrastructure
- [ ] Enable borrow checking for RRR in CMakeLists.txt
- [ ] Create test harness for incremental checking
- [ ] Set up CI/CD integration for borrow checking
- [ ] Document RustyCpp annotations needed

### 1.3 Establish Safety Guidelines
- [ ] Document when to use `unsafe`
- [ ] Create patterns for safe alternatives
- [ ] Define ownership transfer conventions
- [ ] Create code review checklist

## Phase 2: Core Infrastructure (Week 2-3)

### 2.1 Base Types (`src/rrr/base/`)
Priority: **Critical** - Everything depends on these

#### Files to migrate:
- `basetypes.hpp/cpp` - RefCounted, Counter, NoCopy
- `threading.hpp/cpp` - Mutex, CondVar, Thread safety
- `misc.hpp/cpp` - Utility functions
- `debugging.hpp/cpp` - Assert, verify macros
- `logging.hpp/cpp` - Thread-safe logging

#### Key challenges:
- RefCounted uses manual reference counting
- Threading primitives need careful lifetime management
- Global state in logging

#### Proposed solutions:
- Convert RefCounted to use `std::shared_ptr`
- Use RAII wrappers for pthread primitives
- Make logging use thread-local storage where possible

### 2.2 Marshaling (`src/rrr/misc/marshal.hpp/cpp`)
Priority: **Critical** - Core serialization

#### Key challenges:
- Raw pointer manipulation for performance
- Buffer ownership during marshaling
- Endianness conversions with pointer casts

#### Proposed solutions:
- Use `std::span` for buffer views
- Clear ownership model: Marshal owns buffer
- Safe abstractions for type punning

### 2.3 Memory Management (`src/rrr/misc/`)
Priority: **High**

#### Files to migrate:
- `alock.hpp/cpp` - Custom allocator/locks
- `alarm.hpp` - Timer management
- `rand.hpp/cpp` - Random number generation
- `recorder.hpp/cpp` - Event recording

#### Key challenges:
- ALock has complex state machine
- Custom memory pools
- Lock-free data structures

## Phase 3: Networking Layer (Week 3-4)

### 3.1 Reactor Pattern (`src/rrr/reactor/`)
Priority: **Critical** - Event loop foundation

#### Files to migrate:
- `reactor.h/cc` - Main event loop
- `event.h/cc` - Event handling
- `coroutine.cc` - Coroutine support
- `epoll_wrapper.h/cc` - Platform abstraction

#### Key challenges:
- Callbacks with lifetime issues
- Coroutine stack management
- File descriptor ownership

#### Proposed solutions:
- Use `std::function` with clear lifetime
- Stack allocation with RAII guards
- RAII wrapper for file descriptors

### 3.2 RPC Core (`src/rrr/rpc/`)
Priority: **Critical** - Main RPC functionality

#### Files to migrate:
- `server.hpp/cpp` - Server implementation
- `client.hpp/cpp` - Client implementation
- `utils.hpp/cpp` - RPC utilities

#### Key challenges:
- Connection lifetime management
- Request/Reply object ownership
- Thread pool integration
- Async callback lifetimes

#### Proposed solutions:
- Clear ownership: Server owns connections
- Request/Reply use smart pointers
- Callback captures by value or weak_ptr

## Phase 4: Integration and Testing (Week 4-5)

### 4.1 Integration Points
- [ ] Update generated code templates
- [ ] Fix service registration patterns
- [ ] Update benchmark code
- [ ] Migrate example code

### 4.2 Testing Strategy
- [ ] Unit tests for each component
- [ ] Integration tests for RPC calls
- [ ] Stress tests for memory safety
- [ ] Performance regression tests

## Phase 5: Advanced Features (Week 5-6)

### 5.1 Optional Optimizations
- [ ] Lock-free queues with safe interfaces
- [ ] Memory pools with borrow checking
- [ ] Zero-copy optimizations

### 5.2 Documentation
- [ ] Update RRR-RPC guide
- [ ] Create migration guide
- [ ] Document unsafe blocks
- [ ] Performance impact analysis

## Implementation Strategy

### Order of Attack
1. **Start Bottom-Up**: Base types first, then build up
2. **Incremental Migration**: One file at a time
3. **Maintain Compatibility**: Keep API stable
4. **Test Continuously**: Run tests after each file

### Common Patterns to Apply

#### 1. Raw Pointer → Smart Pointer
```cpp
// Before
class Connection {
    Request* pending_request_;
    
    ~Connection() {
        delete pending_request_;
    }
};

// After
class Connection {
    std::unique_ptr<Request> pending_request_;
    // Destructor not needed
};
```

#### 2. Manual Ref Counting → shared_ptr
```cpp
// Before
class RefCounted {
    int ref_count_;
    void add_ref();
    void release();
};

// After
using ObjectPtr = std::shared_ptr<Object>;
```

#### 3. C Arrays → std::vector/std::array
```cpp
// Before
char buffer[1024];
int* values = new int[size];

// After
std::array<char, 1024> buffer;
std::vector<int> values(size);
```

#### 4. Unsafe Casts → Safe Alternatives
```cpp
// Before
int* p = (int*)buffer;

// After
int value;
std::memcpy(&value, buffer, sizeof(int));
```

### When to Use `unsafe`

Acceptable uses of `unsafe`:
1. **FFI Boundaries**: Interfacing with C libraries
2. **Performance Critical**: Proven bottlenecks only
3. **Lock-free Algorithms**: Where atomics are needed
4. **Platform Code**: System calls, epoll, etc.

Each `unsafe` block must have:
- Comment explaining why it's needed
- Proof of safety
- Test coverage

### Milestone Checkpoints

#### Checkpoint 1 (End of Week 1)
- [ ] All base types compile with borrow checking
- [ ] No regressions in existing tests
- [ ] Documentation updated

#### Checkpoint 2 (End of Week 3)
- [ ] Marshal and reactor systems safe
- [ ] RPC client can make safe calls
- [ ] Benchmark still runs

#### Checkpoint 3 (End of Week 5)
- [ ] Entire RRR library passes borrow checking
- [ ] All tests pass
- [ ] Performance within 5% of original

## Risk Mitigation

### Potential Risks
1. **Performance Regression**: Mitigation: Profile continuously
2. **API Breaking Changes**: Mitigation: Maintain compatibility layer
3. **Complex Lifetime Issues**: Mitigation: Redesign if needed
4. **Hidden Bugs Exposed**: Mitigation: Fix as we go

### Rollback Plan
- Keep original code in separate branch
- Feature flag for safe/unsafe mode
- Incremental deployment

## Success Metrics

### Primary Goals
- [ ] 100% of RRR files pass borrow checking
- [ ] < 5% performance impact
- [ ] No API breaking changes
- [ ] Zero memory leaks in tests

### Stretch Goals
- [ ] Remove all `unsafe` blocks
- [ ] Improve performance with better patterns
- [ ] Create reusable safety patterns

## Tools and Resources

### Required Tools
- rusty-cpp checker (already integrated)
- Valgrind for memory verification
- AddressSanitizer for runtime checks
- Benchmark suite for performance

### Documentation Needed
- RustyCpp annotation guide
- Common patterns cookbook
- Troubleshooting guide
- Performance tuning guide

## Team Considerations

### Code Review Process
1. Every PR must pass borrow checking
2. Document all `unsafe` usage
3. Performance impact must be measured
4. Tests must cover changed code

### Knowledge Sharing
- Weekly progress updates
- Document learned patterns
- Create internal best practices
- Share problematic cases

## Timeline Summary

| Week | Focus | Deliverable |
|------|-------|-------------|
| 1 | Assessment & Base Types | Base types safe |
| 2 | Marshaling & Memory | Core utilities safe |
| 3 | Reactor Pattern | Event loop safe |
| 4 | RPC Core | Client/Server safe |
| 5 | Integration & Testing | Full system working |
| 6 | Optimization & Docs | Production ready |

## Next Steps

1. **Immediate Actions**:
   - Enable borrow checking for first file
   - Create test harness
   - Start with `basetypes.hpp`

2. **First PR Goals**:
   - Make Counter class safe
   - Make RefCounted safe
   - Update documentation

3. **Success Criteria for Phase 1**:
   - One complete module passes checking
   - No performance regression
   - Clear patterns established

---

This is a living document and should be updated as we learn more about the specific challenges in the codebase.