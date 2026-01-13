# Fiber API Reference

This document describes the modern Fiber API for working with stackful coroutines in RRR.

## Overview

The Fiber API provides a cleaner, more intuitive interface for coroutine-based programming,
following conventions established by Boost.Fiber. Key features:

- **Clearer Naming**: Uses "Fiber" terminology instead of "Coroutine" (matching industry convention)
- **this_fiber Namespace**: Provides `this_fiber::get_id()`, `this_fiber::yield()`, `this_fiber::sleep_*()` etc.
- **Event Combinators**: `WaitAll`, `WaitAny`, `WaitN` for clear event composition
- **Time API**: Uses `rrr::Time` internally (NOT `std::chrono`)

For conceptual background on coroutines vs fibers and the reactor pattern, see [coroutines_guide.md](coroutines_guide.md).

## Quick Start

```cpp
#include "reactor/fiber.h"

using namespace rrr;

void example() {
    // Create and run a fiber
    auto fiber = Fiber::create_run([]() {
        // Get current fiber ID
        auto id = this_fiber::get_id();

        // Sleep for 100 milliseconds
        this_fiber::sleep_ms(100);

        // Yield to other fibers
        this_fiber::yield();
    });
}
```

## Type Aliases

### Fiber

```cpp
using Fiber = Coroutine;
```

`Fiber` is an alias for `Coroutine`. Use `Fiber` for new code as it more accurately
describes our stackful execution model (C++20 coroutines are stackless; ours are stackful).

### Event Combinators

```cpp
using WaitAll = AndEvent;  // Wait for ALL events (AND semantics)
using WaitAny = OrEvent;   // Wait for ANY event (OR semantics)
using WaitN = NEvent;      // Wait for N of M events
```

## this_fiber Namespace

The `this_fiber` namespace provides functions that operate on the currently executing fiber,
similar to how `std::this_thread` works for threads.

### get_id()

```cpp
uint64_t this_fiber::get_id() noexcept;
```

Returns the unique ID of the currently executing fiber, or 0 if called outside a fiber context.

**Example:**
```cpp
Fiber::create_run([]() {
    auto id = this_fiber::get_id();  // Returns non-zero fiber ID
});
this_fiber::get_id();  // Returns 0 (outside fiber context)
```

### current()

```cpp
rusty::Option<rusty::Rc<Coroutine>> this_fiber::current() noexcept;
```

Returns `Some(fiber)` if called within a fiber context, `None` otherwise.
Useful when you need a reference to the current fiber object.

**Example:**
```cpp
Fiber::create_run([]() {
    auto fiber_opt = this_fiber::current();
    if (fiber_opt.is_some()) {
        auto fiber = fiber_opt.unwrap();
        // Use fiber reference...
    }
});
```

### in_fiber_context()

```cpp
bool this_fiber::in_fiber_context() noexcept;
```

Returns `true` if currently executing within a fiber, `false` otherwise.

**Example:**
```cpp
void my_function() {
    if (this_fiber::in_fiber_context()) {
        // Can safely use fiber-only APIs
        this_fiber::yield();
    } else {
        // Running on main thread
    }
}
```

### yield()

```cpp
void this_fiber::yield() noexcept;
```

Yields execution to other fibers. The reactor can run other ready fibers before
returning to the current one. No-op if called outside a fiber context.

**Note:** After yielding, the fiber must be explicitly continued with `reactor->continue_coro(fiber)`.
Simply calling `reactor->loop()` will not automatically resume yielded fibers.

**Example:**
```cpp
auto fiber = Fiber::create_run([]() {
    // Do some work...
    this_fiber::yield();  // Allow other fibers to run
    // Continue after being resumed...
});

// Fiber is now paused at yield point
reactor->continue_coro(fiber);  // Resume the fiber
```

### sleep_us()

```cpp
void this_fiber::sleep_us(uint64_t microseconds);
```

Suspends the current fiber for the specified number of microseconds.
Uses `rrr::Time` internally.

**Example:**
```cpp
Fiber::create_run([]() {
    this_fiber::sleep_us(1000);  // Sleep for 1 millisecond
});
```

### sleep_ms()

```cpp
void this_fiber::sleep_ms(uint64_t milliseconds);
```

Suspends the current fiber for the specified number of milliseconds.
Equivalent to `sleep_us(milliseconds * 1000)`.

**Example:**
```cpp
Fiber::create_run([]() {
    this_fiber::sleep_ms(100);  // Sleep for 100 milliseconds
});
```

### sleep_s()

```cpp
void this_fiber::sleep_s(uint64_t seconds);
```

Suspends the current fiber for the specified number of seconds.
Equivalent to `sleep_us(seconds * Time::RRR_USEC_PER_SEC)`.

**Example:**
```cpp
Fiber::create_run([]() {
    this_fiber::sleep_s(1);  // Sleep for 1 second
});
```

### sleep_until_us()

```cpp
void this_fiber::sleep_until_us(uint64_t abs_time_us);
```

Suspends the current fiber until the specified absolute time (in microseconds since epoch).
If the specified time has already passed, returns immediately.
Uses `rrr::Time::now()` to get the current time.

**Example:**
```cpp
Fiber::create_run([]() {
    uint64_t wake_time = Time::now(true) + 5000000;  // 5 seconds from now
    this_fiber::sleep_until_us(wake_time);
});
```

## Event Combinators

### WaitAll (AndEvent)

Waits for ALL child events to complete.

```cpp
auto event1 = Reactor::create_sp_event<IntEvent>();
auto event2 = Reactor::create_sp_event<IntEvent>();
auto wait_all = Reactor::create_sp_event<WaitAll>(event1, event2);

Fiber::create_run([&]() {
    wait_all->wait();  // Blocks until BOTH event1 AND event2 are ready
});
```

### WaitAny (OrEvent)

Waits for ANY child event to complete.

```cpp
auto event1 = Reactor::create_sp_event<IntEvent>();
auto event2 = Reactor::create_sp_event<IntEvent>();
auto wait_any = Reactor::create_sp_event<WaitAny>(event1, event2);

Fiber::create_run([&]() {
    wait_any->wait();  // Blocks until EITHER event1 OR event2 is ready
});
```

### WaitN (NEvent)

Waits for N of M child events to complete.

```cpp
auto wait_n = Reactor::create_sp_event<WaitN>(2);  // Wait for 2 events
auto event1 = Reactor::create_sp_event<IntEvent>();
auto event2 = Reactor::create_sp_event<IntEvent>();
auto event3 = Reactor::create_sp_event<IntEvent>();

wait_n->add(event1);
wait_n->add(event2);
wait_n->add(event3);

Fiber::create_run([&]() {
    wait_n->wait();  // Blocks until ANY 2 of the 3 events are ready
});
```

## Time API Notes

**Important:** The Fiber API uses `rrr::Time` for all time-related operations, NOT `std::chrono`.

- `Time::now(true)` returns the current time in microseconds (accurate mode)
- `Time::now(false)` returns cached time (faster, less accurate)
- `Time::RRR_USEC_PER_SEC` is the number of microseconds per second (1,000,000)

This is consistent with the rest of the RRR codebase and avoids mixing time representations.

## RustyCpp Safety

All functions in the Fiber API are annotated with `@safe` or `@unsafe` markers:

- **@safe**: Function uses only safe operations (Option checks, type aliases)
- **@unsafe**: Function calls legacy code or performs operations that can't be statically verified

The type aliases (`Fiber`, `WaitAll`, `WaitAny`, `WaitN`) are inherently `@safe` as they're just
compile-time type definitions.

## Migration from Coroutine API

If you have existing code using `Coroutine`, you can migrate to `Fiber`:

| Old API | New API |
|---------|---------|
| `Coroutine::create_run(...)` | `Fiber::create_run(...)` |
| `Coroutine::current_coroutine()` | `this_fiber::current()` |
| `coro->yield_()` | `this_fiber::yield()` |
| `Coroutine::sleep(us)` | `this_fiber::sleep_us(us)` |
| `AndEvent` | `WaitAll` |
| `OrEvent` | `WaitAny` |
| `NEvent` | `WaitN` |

Both APIs work identically as `Fiber = Coroutine` is just a type alias.
Existing code continues to work; use the new names in new code for clarity.

## See Also

- [coroutines_guide.md](coroutines_guide.md) - Conceptual guide to coroutines and the reactor pattern
- [fiber_api_refactoring_plan.md](fiber_api_refactoring_plan.md) - Migration plan for the Fiber API
