# SRPC Unsafe Blocks and Functions

This document lists all `@unsafe` annotated blocks and functions in the `src/srpc/` directory, sorted by lines of code (LOC) in descending order.

**Generated**: 2026-01-03

> Historical migration inventory. `base/basetypes.cpp` has since been retired:
> canonical `src/srpc/src/basetypes.rs` owns SparseInt, and its four public
> raw-pointer codecs are explicit unsafe Rust functions with storage contracts.
> The line references below describe the former carrier and are not current
> ownership guidance.

## Summary Statistics

| Category | Count | Total LOC |
|----------|-------|-----------|
| marshal.hpp operators | 35+ | ~400 |
| SparseInt functions | 6 | ~180 |
| epoll_wrapper.h | 5 | ~165 |
| reactor.cc | 25+ | ~350 |
| logging.cpp | 13 | ~90 |
| threading.hpp | 20+ | ~200 |
| alock.cpp | 5 | ~80 |
| basetypes.cpp | 8 | ~130 |

---

## Detailed List (Sorted by LOC)

### 1. SparseInt::dump(i64, char*) - 82 LOC
**File**: `src/srpc/base/basetypes.cpp:121-203`
```
@unsafe - Uses raw pointer operations for performance
SAFETY: Caller must ensure buffer is large enough
```

### 2. Epoll::Wait<ModeUpdater> - 65 LOC
**File**: `src/srpc/reactor/epoll_wrapper.h:261-330`
```
@unsafe - Waits for events and dispatches to handlers directly
SAFETY: Uses system calls with timeout, raw pointer safe due to deferred removal
```

### 3. Epoll::Update - 66 LOC
**File**: `src/srpc/reactor/epoll_wrapper.h:188-256`
```
@unsafe - Updates poll mode for file descriptor
SAFETY: Uses system calls with proper event flag handling
```

### 4. PollThreadWorker::poll_loop - 63 LOC
**File**: `src/srpc/reactor/reactor.cc:455-518`
```
@unsafe - Main polling loop - processes epoll events and channel commands
SAFETY: Thread-safe due to channel-based communication
```

### 5. Queue<T>::pop - 11 LOC
**File**: `src/srpc/base/threading.hpp:452-463`
```
@unsafe - Thread-safe blocking pop
SAFETY: Returns by value (move), not by reference
```

### 6. SparseInt::dump(i32, char*) - 40 LOC
**File**: `src/srpc/base/basetypes.cpp:78-119`
```
@unsafe - Uses raw pointer operations for performance
SAFETY: Caller must ensure buffer is large enough
```

### 7. Epoll::Add - 36 LOC
**File**: `src/srpc/reactor/epoll_wrapper.h:119-157`
```
@unsafe - Adds file descriptor to epoll/kqueue
SAFETY: Uses system calls with proper error checking
```

### 8. PollThreadWorker::process_commands - 30 LOC
**File**: `src/srpc/reactor/reactor.cc:520-551`
```
@unsafe - calls try_recv and std::visit
SAFETY: Channel operations are thread-safe
```

### 9. ALock::Lock - 32 LOC
**File**: `src/srpc/misc/alock.cpp:26-58`
```
@unsafe - Creates std::function objects from lambdas
SAFETY: Lambda captures are properly scoped
```

### 10. SparseInt::load_i64 - 24 LOC
**File**: `src/srpc/base/basetypes.cpp:231-254`
```
@unsafe - Reads from raw pointer
SAFETY: Caller must ensure buffer contains valid SparseInt encoding
```

### 11. Epoll::Remove - 24 LOC
**File**: `src/srpc/reactor/epoll_wrapper.h:160-185`
```
@unsafe - Removes file descriptor from epoll/kqueue
SAFETY: Uses system calls, ignores errors for already removed fds
```

### 12. SparseInt::load_i32 - 23 LOC
**File**: `src/srpc/base/basetypes.cpp:206-229`
```
@unsafe - Reads from raw pointer
SAFETY: Caller must ensure buffer contains valid SparseInt encoding
```

### 13. PollThreadWorker::TriggerJob - 20 LOC
**File**: `src/srpc/reactor/reactor.cc:553-573`
```
@unsafe - uses std::set operations
SAFETY: Set operations are encapsulated
```

### 14. chunk::write_to_fd - 42 LOC
**File**: `src/srpc/misc/marshal.hpp:407-451`
```
@unsafe - Writes to file descriptor (I/O system call)
SAFETY: Proper null checking before operations
```

### 15. Log::log_v - 18 LOC
**File**: `src/srpc/base/logging.cpp:57-74`
```
@unsafe - Uses vsprintf to format strings into stack buffer
SAFETY: Fixed buffer size, format validation
```

### 16. do_close_pollable - 27 LOC
**File**: `src/srpc/reactor/reactor.cc:603-630`
```
@unsafe - Closes socket and drops Arc
SAFETY: Called only from poll thread, owns the Pollable via Arc
```

### 17. do_add_pollable - 17 LOC
**File**: `src/srpc/reactor/reactor.cc:575-592`
```
@unsafe - Uses raw pointer cast for epoll userdata
SAFETY: Pointer remains valid due to fd_to_pollable_ map ownership
```

### 18. chunk::read_from_fd - 24 LOC
**File**: `src/srpc/misc/marshal.hpp:453-477`
```
@unsafe - Reads from file descriptor (I/O system call)
SAFETY: Bounds checking on buffer size
```

### 19. MarshallDeputy::WriteToFd - 26 LOC
**File**: `src/srpc/misc/marshal.hpp:197-222`
```
@unsafe - Writes to file descriptor
SAFETY: Null checking before operations
```

### 20. operator<<(Marshal, std::string) - 18 LOC
**File**: `src/srpc/misc/marshal.hpp:780-800`
```
@unsafe - Writes string data to marshal buffer
SAFETY: Bounds checking via v64 length prefix
```

### 21. TimeoutALock::~TimeoutALock - 28 LOC
**File**: `src/srpc/misc/alock.cpp:682-709`
```
@unsafe - Uses std::function and std::vector
SAFETY: Cleanup of lock requests on destruction
```

### 22. SpinCondVar::wait - 10 LOC
**File**: `src/srpc/base/threading.hpp:346-356`
```
@unsafe - Calls std::atomic::store/load
SAFETY: Thread-safe atomic operations
```

### 23. SpinCondVar::timed_wait - 13 LOC
**File**: `src/srpc/base/threading.hpp:358-372`
```
@unsafe - Calls std::atomic::store/load
SAFETY: Thread-safe atomic operations with timeout
```

### 24. Epoll constructor - 9 LOC
**File**: `src/srpc/reactor/epoll_wrapper.h:87-96`
```
@unsafe - Creates epoll/kqueue file descriptor
SAFETY: Verifies creation succeeded
```

### 25. Log::fatal (overloads) - 8 LOC each
**File**: `src/srpc/base/logging.cpp:84-91, 126-133`
```
@unsafe - Variadic function that calls abort
SAFETY: Terminates program after logging
```

### 26. Log::{error,warn,info,debug} - 7 LOC each x 8
**File**: `src/srpc/base/logging.cpp:93-165`
```
@unsafe - Variadic functions using va_list
SAFETY: Properly initializes and cleans up va_list
```

### 27. Queue<T>::push - 6 LOC
**File**: `src/srpc/base/threading.hpp:415-421`
```
@unsafe - Thread-safe push with mutex protection
SAFETY: Mutex lock/unlock properly paired
```

### 28. Queue<T>::try_pop - 10 LOC
**File**: `src/srpc/base/threading.hpp:423-435`
```
@unsafe - Thread-safe try_pop with mutex protection
SAFETY: Returns via output parameter using move semantics
```

### 29. SpinMutex<T>::lock - 8 LOC
**File**: `src/srpc/base/threading.hpp:283-291`
```
@unsafe - Acquires lock and returns LockResult
SAFETY: SpinLock is always acquired before returning guard
```

### 30. Marshal operator<< (primitives) - 4-6 LOC each x 10
**File**: `src/srpc/misc/marshal.hpp:670-770`
```
@unsafe - Writes primitive types to marshal buffer
SAFETY: Fixed-size writes with verify()
```

### 31. Marshal operator>> (primitives) - 4-6 LOC each x 10
**File**: `src/srpc/misc/marshal.hpp:905-1000`
```
@unsafe - Reads primitive types from marshal buffer
SAFETY: Fixed-size reads with verify()
```

### 32. Marshal operator<< (containers) - 10-15 LOC each x 6
**File**: `src/srpc/misc/marshal.hpp:802-902`
```
@unsafe - Writes STL containers to marshal buffer
SAFETY: Length prefix prevents overflow
```

### 33. Marshal operator>> (containers) - 10-15 LOC each x 6
**File**: `src/srpc/misc/marshal.hpp:1023-1123`
```
@unsafe - Reads STL containers from marshal buffer
SAFETY: Length prefix bounds iteration
```

### 34. Timer::start/stop/elapsed - 4-10 LOC each
**File**: `src/srpc/base/basetypes.cpp:261-293`
```
@unsafe - Uses gettimeofday (external unsafe)
SAFETY: Properly passes address of timeval struct
```

### 35. Rand constructor - 10 LOC
**File**: `src/srpc/base/basetypes.cpp:295-305`
```
@unsafe - Uses gettimeofday, pthread_self, reinterpret_cast
SAFETY: Seed generation for RNG
```

### 36. SpinMutexGuard methods - 4-8 LOC each x 8
**File**: `src/srpc/base/threading.hpp:148-246`
```
@unsafe - Access to data through UnsafeCell
SAFETY: Guard only exists while lock is held
```

### 37. Log::set_level/set_file - 5 LOC each
**File**: `src/srpc/base/logging.cpp:24-37`
```
@unsafe - Uses mutex operations
SAFETY: Proper lock/unlock pairing
```

### 38. basename (static) - 16 LOC
**File**: `src/srpc/base/logging.cpp:39-55`
```
@unsafe - Returns pointer into input string
SAFETY: Returns valid pointer within input bounds
```

### 39. Reactor helper functions - 5-15 LOC each
**File**: `src/srpc/reactor/reactor.cc:114-186`
```
@unsafe - RefCell operations, Rc::clone
SAFETY: Single-threaded access pattern
```

### 40. do_update_mode - 20 LOC
**File**: `src/srpc/reactor/reactor.cc:632-654`
```
@unsafe - Uses raw pointer dereference for poll_ptr
SAFETY: poll_ptr guaranteed valid by caller
```

### 41. PollThread::update_mode - 8 LOC
**File**: `src/srpc/reactor/reactor.cc:806-814`
```
@unsafe - channel send and pointer operations
SAFETY: Channel send is thread-safe
```

### 42. chunk::set_bookmark - 11 LOC
**File**: `src/srpc/misc/marshal.hpp:327-338`
```
@unsafe - Returns pointer to heap data
SAFETY: Returns pointer into data->ptr array which outlives function
```

### 43. chunk::shared_copy - 4 LOC
**File**: `src/srpc/misc/marshal.hpp:307-311`
```
@unsafe - Creates new chunk sharing same data buffer
SAFETY: shared_ptr handles reference counting
```

### 44. bookmark destructor/move - 10-15 LOC
**File**: `src/srpc/misc/marshal.hpp:530-545`
```
@unsafe - Uses delete[]
SAFETY: Proper ownership transfer in move operations
```

---

## Risk Categories

### HIGH RISK (Memory Safety Critical)
1. `Epoll::Wait` - Raw pointer casting from userdata
2. `do_add_pollable` - Raw pointer cast for epoll
3. `SparseInt::dump/load` - Raw pointer arithmetic
4. `chunk::write_to_fd/read_from_fd` - I/O with raw pointers

### MEDIUM RISK (Thread Safety)
1. `Queue<T>` methods - Pthread primitives
2. `SpinMutex/SpinLock` - Atomic operations
3. `Log` methods - Global mutex
4. `PollThreadWorker::process_commands` - Channel operations

### LOW RISK (Wrapper/Boilerplate)
1. Marshal operators - Type-safe wrappers
2. Timer/Rand - System call wrappers
3. Log variadic functions - va_list handling
4. SpinMutexGuard - RAII lock guard

---

## Migration Status

### Completed Migrations
- `Event::status_` -> `rusty::Cell<EventStatus>`
- `Coroutine::status_` -> `rusty::Cell<Status>`
- `Event::wp_coro_` -> `rusty::rc::Weak<Coroutine>`
- `Reactor::Loop()` -> Now @safe
- `ContinueCoro()` -> Now @safe
- `Event::Test()` -> Now @safe
- Removed ThreadSafeIntEvent (unused)

### Pending Migrations
- Marshal chunk operations (complex ownership)
- Epoll wrapper (system interface)
- PollThreadWorker (thread boundary)
- SparseInt (performance-critical)
