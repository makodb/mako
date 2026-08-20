# Fiber API Refactoring Plan

> **Historical design proposal.** This is not a current build or ownership
> guide. Current Rust/C++ module ownership is tracked in
> `docs/dev/goal0_completion_plan.md` and `src/rrr/RUST_CANARY.md`.

## Overview

This document describes the plan to refactor the rrr reactor/coroutine API to follow Boost.Fiber conventions for better clarity and industry alignment.

## Background

### Current State

The Mako codebase uses `rrr::Coroutine` for cooperative multitasking. However, this class is implemented using **Boost.Coroutine2**, which provides **stackful** execution contexts. In modern C++ terminology:

- **Stackful coroutines** (what we have) = **Fibers**
- **Stackless coroutines** (C++20) = **Coroutines**

The naming is confusing because:
1. C++20 introduced `co_await`, `co_yield`, `co_return` for stackless coroutines
2. Boost.Fiber is the library for stackful coroutines in Boost ecosystem
3. Industry convention is to call stackful execution contexts "fibers"

### Boost.Fiber API Reference

Boost.Fiber provides a clean, std::thread-like API:

```cpp
// Fiber creation
boost::fibers::fiber f(func);

// Fiber operations
f.join();
f.detach();
f.joinable();
f.get_id();

// this_fiber namespace (like std::this_thread)
boost::this_fiber::get_id();
boost::this_fiber::yield();
boost::this_fiber::sleep_for(duration);
boost::this_fiber::sleep_until(time_point);
```

## Design Principles

### RustyCpp Safety Requirements

All new code MUST follow rusty-safe patterns:

1. **Use rrr::Time, NOT std::chrono** - The codebase uses `rrr::Time::now()` for time operations
2. **Mark all functions with @safe or @unsafe** - No unmarked functions
3. **Use rusty types** - `rusty::Option<T>`, `rusty::Cell<T>`, `rusty::Rc<T>`, etc.
4. **Wrap unsafe operations** - Use `// @unsafe { reason }` blocks
5. **No raw pointers in public API** - Use `rusty::Ptr<T>` / `rusty::MutPtr<T>` where needed

### Time Interface

The codebase uses `rrr::Time`, exported by the `rrr.basetypes` named module
from canonical `src/rrr/src/basetypes.rs`:

```cpp
class Time {
public:
    static const uint64_t RRR_USEC_PER_SEC = 1000000;

    // @unsafe - calls clock_gettime
    static uint64_t now(bool accurate = false);  // Returns microseconds

    // @unsafe - calls select()
    static void sleep(uint64_t t);  // Sleeps for t microseconds
};
```

## Design

### Phase 1: Add Aliases and this_fiber Namespace

Create a new header `src/rrr/reactor/fiber.h` that provides the modern API:

```cpp
#pragma once

#include "coroutine.h"
import rrr.basetypes;  // For rrr::Time
#include <rusty/option.hpp>
#include <rusty/cell.hpp>

namespace rrr {

// Type alias for clarity
using Fiber = Coroutine;

namespace this_fiber {

// @safe - Returns current fiber ID (0 if not in fiber context)
// Uses only safe rusty::Option operations
inline uint64_t get_id() noexcept {
    auto coro = Coroutine::current_coroutine();
    if (coro.is_some()) {
        // @unsafe { accessing Rc internals }
        return coro.unwrap()->id;
    }
    return 0;
}

// @safe - Returns Option<Rc<Coroutine>> for current fiber
// Wraps the existing API without modification
inline rusty::Option<rusty::Rc<Coroutine>> current() noexcept {
    return Coroutine::current_coroutine();
}

// @unsafe - Yields execution to other fibers
// Delegates to coroutine's yield which uses boost internals
inline void yield() noexcept {
    auto coro = Coroutine::current_coroutine();
    if (coro.is_some()) {
        // @unsafe { boost coroutine yield }
        coro.unwrap()->yield_();
    }
}

// @unsafe - Sleeps for specified microseconds
// Uses rrr::Time internally (NOT std::chrono)
inline void sleep_us(uint64_t microseconds) {
    // @unsafe { Coroutine::sleep uses Time internally }
    Coroutine::sleep(microseconds);
}

// @unsafe - Sleeps for specified milliseconds
// Convenience wrapper, uses rrr::Time internally
inline void sleep_ms(uint64_t milliseconds) {
    // @unsafe { Coroutine::sleep }
    Coroutine::sleep(milliseconds * 1000);
}

// @unsafe - Sleeps for specified seconds
// Convenience wrapper, uses rrr::Time internally
inline void sleep_s(uint64_t seconds) {
    // @unsafe { Coroutine::sleep }
    Coroutine::sleep(seconds * Time::RRR_USEC_PER_SEC);
}

// @unsafe - Sleeps until specified absolute time (microseconds since epoch)
// Uses rrr::Time::now() for current time
inline void sleep_until_us(uint64_t abs_time_us) {
    // @unsafe { Time::now }
    uint64_t now = Time::now(true);
    if (abs_time_us > now) {
        // @unsafe { Coroutine::sleep }
        Coroutine::sleep(abs_time_us - now);
    }
}

} // namespace this_fiber

} // namespace rrr
```

### Phase 2: Event Combinator Aliases

Add clearer names for event combinators in `event.h`:

```cpp
// In event.h, after class definitions:
namespace rrr {

// @safe - Type aliases (no runtime behavior)
// Clearer names for event combinators
using WaitAll = AndEvent;   // Wait for ALL events to be ready
using WaitAny = OrEvent;    // Wait for ANY event to be ready
using WaitN = NEvent;       // Wait for N events to be ready

} // namespace rrr
```

### Phase 3: Future/Promise Wrappers (Optional)

Create `src/rrr/reactor/future.h` with rusty-safe Future/Promise:

```cpp
#pragma once

#include "event.h"
#include "reactor.h"
#include <rusty/cell.hpp>
#include <rusty/option.hpp>
#include <stdexcept>
#include <memory>

namespace rrr {

template<typename T>
class Future;

// @unsafe - Uses shared_ptr and mutable state
template<typename T>
class Promise {
    std::shared_ptr<BoxEvent<T>> event_;
    rusty::Cell<bool> value_set_{false};

public:
    // @safe - Initializes shared state
    Promise() : event_(std::make_shared<BoxEvent<T>>()) {}

    // @unsafe - Modifies shared state, may throw
    void set_value(T value) {
        if (value_set_.get()) {
            // @unsafe { throw }
            throw std::logic_error("Promise value already set");
        }
        // @unsafe { BoxEvent::set }
        event_->set(std::move(value));
        value_set_.set(true);
    }

    // @safe - Creates Future sharing same event
    Future<T> get_future();

    // Non-copyable, movable
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    Promise(Promise&&) = default;
    Promise& operator=(Promise&&) = default;
};

// @unsafe - Uses shared_ptr and blocking wait
template<typename T>
class Future {
    std::shared_ptr<BoxEvent<T>> event_;

    friend class Promise<T>;

    // @safe - Private constructor from shared event
    explicit Future(std::shared_ptr<BoxEvent<T>> ev) : event_(std::move(ev)) {}

public:
    // @safe - Read-only check
    bool is_ready() const {
        return event_ && event_->is_ready();
    }

    // @unsafe - Blocks fiber until ready
    void wait() {
        if (event_) {
            // @unsafe { Event::wait }
            event_->wait();
        }
    }

    // @unsafe - Blocks and returns value
    T get() {
        // @unsafe { wait() }
        wait();
        // @unsafe { BoxEvent::get }
        return event_->get();
    }

    // @unsafe - Wait with timeout (microseconds)
    // Returns true if ready, false if timed out
    // Uses rrr::Time internally (NOT std::chrono)
    bool wait_for_us(uint64_t timeout_us) {
        // @unsafe { Event::wait with timeout }
        event_->wait(timeout_us);
        return is_ready();
    }

    // @unsafe - Wait with timeout (milliseconds)
    bool wait_for_ms(uint64_t timeout_ms) {
        return wait_for_us(timeout_ms * 1000);
    }

    // Non-copyable, movable
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    Future(Future&&) = default;
    Future& operator=(Future&&) = default;
};

// @safe - Returns Future sharing event with Promise
template<typename T>
Future<T> Promise<T>::get_future() {
    return Future<T>(event_);
}

// @safe - Factory function returning pair
template<typename T>
std::pair<Promise<T>, Future<T>> make_promise() {
    Promise<T> p;
    Future<T> f = p.get_future();
    return {std::move(p), std::move(f)};
}

} // namespace rrr
```

### Phase 4: Internal Rename

1. Rename `coroutine.h` to `fiber_impl.h` (implementation)
2. Create new `coroutine.h` that includes `fiber_impl.h` and provides alias
3. Rename class `Coroutine` to `Fiber` internally
4. Add `using Coroutine = Fiber;` for backward compatibility
5. Update all internal references

All changes must maintain @safe/@unsafe annotations.

### Phase 5: Documentation

Create comprehensive documentation in `doc/fiber_api.md` covering:
1. API reference for all new types and functions
2. Migration guide from old API to new API
3. Examples showing common patterns
4. Note about using `rrr::Time` (not std::chrono)

## Migration Strategy

### Non-Breaking Approach

All changes are additive with aliases for backward compatibility:

```cpp
// Old code still works:
auto coro = Coroutine::create_run([]{ ... });
Coroutine::sleep(1000);  // microseconds

// New code can use cleaner API:
auto fiber = Fiber::spawn([]{ ... });
this_fiber::sleep_ms(1);  // milliseconds
this_fiber::yield();

// Events: old and new names both work
auto wait_all = std::make_shared<AndEvent>();  // Old
auto wait_all = std::make_shared<WaitAll>();   // New (preferred)
```

### Deprecation Timeline

1. **Phase 1-3**: Add new API alongside old
2. **Phase 4**: Internal rename with aliases
3. **Phase 5**: Document new API as preferred
4. **Future**: Mark old names as `[[deprecated]]` (not in this phase)

## Files to Create/Modify

| File | Action | Description |
|------|--------|-------------|
| `src/rrr/reactor/fiber.h` | Create | New public API header with this_fiber namespace |
| `src/rrr/reactor/future.h` | Create | Future/Promise implementation |
| `src/rrr/reactor/event.h` | Modify | Add WaitAll/WaitAny/WaitN aliases |
| `src/rrr/reactor/coroutine.h` | Modify | Internal rename, add Coroutine alias |
| `src/rrr/reactor/reactor.h` | Modify | Update internal references |
| `test/fiber_test.cc` | Create | Tests for new API |
| `doc/fiber_api.md` | Create | API documentation |

## RustyCpp Compliance Checklist

- [ ] All functions have @safe or @unsafe annotations
- [ ] All unsafe operations wrapped in `// @unsafe { reason }` blocks
- [ ] Uses `rrr::Time::now()` instead of `std::chrono`
- [ ] Uses `rusty::Cell<T>` for interior mutability of primitives
- [ ] Uses `rusty::Option<T>` instead of nullable pointers
- [ ] Uses `rusty::Rc<T>` for single-threaded reference counting
- [ ] No raw pointers in public API
- [ ] All new files added to borrow checking in CMakeLists.txt

## Testing Plan

1. **Unit Tests** (must be @safe or properly annotated @unsafe):
   - `this_fiber::get_id()` returns correct ID
   - `this_fiber::yield()` yields correctly
   - `this_fiber::sleep_us/ms/s()` sleeps correct duration
   - `this_fiber::sleep_until_us()` sleeps until time point
   - `Future/Promise` value passing
   - `WaitAll/WaitAny/WaitN` aliases work

2. **Integration Tests**:
   - Existing coroutine tests pass with no changes
   - New fiber API works in RPC handlers
   - Event combinators work with new names

3. **Backward Compatibility Tests**:
   - All existing code compiles unchanged
   - Old `Coroutine::create_run()` still works
   - Old `Coroutine::sleep()` (microseconds) still works
   - Old `AndEvent`, `OrEvent`, `NEvent` still work

## Success Criteria

1. New `this_fiber` namespace is functional
2. Event combinator aliases are available
3. Optional `Future<T>`/`Promise<T>` work correctly
4. **All code passes RustyCpp borrow checking**
5. **All functions have @safe/@unsafe annotations**
6. **Uses rrr::Time, not std::chrono**
7. All existing tests pass unchanged
8. New API has comprehensive tests
9. Documentation is complete
10. No performance regression

## API Mapping Reference

| Current API | New API | Notes |
|-------------|---------|-------|
| `Coroutine` | `Fiber` | Alias provided for compatibility |
| `Coroutine::create_run(func)` | `Fiber::spawn(func)` | Same semantics |
| `Coroutine::current_coroutine()` | `this_fiber::current()` | Returns Option<Rc<Coroutine>> |
| N/A | `this_fiber::get_id()` | Returns uint64_t ID |
| `Coroutine::sleep(us)` | `this_fiber::sleep_us(us)` | Microseconds |
| N/A | `this_fiber::sleep_ms(ms)` | Milliseconds (convenience) |
| N/A | `this_fiber::sleep_s(s)` | Seconds (convenience) |
| `coro->yield_()` | `this_fiber::yield()` | Free function |
| `AndEvent` | `WaitAll` | Alias provided |
| `OrEvent` | `WaitAny` | Alias provided |
| `NEvent` | `WaitN` | Alias provided |
| `BoxEvent<T>` | `Future<T>` / `Promise<T>` | Wrapper provided |

## What to Keep (Unique Value)

- `QuorumEvent` - Essential for distributed consensus
- `DispatchEvent` - RPC dispatch coordination
- `IntEvent`, `SharedIntEvent` - Counter-based synchronization
- `TimeoutEvent` - Timeout handling with rrr::Time
- RustyCpp safety annotations throughout

## Estimated Effort

| Phase | Estimated LOC | Estimated Time |
|-------|---------------|----------------|
| Phase 1: this_fiber | ~80 | 1-2 hours |
| Phase 2: Event aliases | ~20 | 30 minutes |
| Phase 3: Future/Promise | ~150 | 2-3 hours |
| Phase 4: Internal rename | ~300 | 3-4 hours |
| Phase 5: Documentation | ~100 | 1-2 hours |
| **Total** | **~650** | **8-12 hours** |

## References

- [Boost.Fiber Documentation](https://www.boost.org/doc/libs/1_87_0/libs/fiber/doc/html/fiber/overview.html)
- [this_fiber Namespace](https://www.boost.org/doc/libs/1_87_0/libs/fiber/doc/html/fiber/fiber_mgmt/this_fiber.html)
- `src/rrr/src/basetypes.rs` - canonical rrr::Time source
- `CLAUDE.md` - RustyCpp safety requirements
