# RPC Safety Conversion Plan

> Historical conversion record. Current ownership and Goal-0 status live in
> `src/rrr/RUST_CANARY.md`; `rpc/utils.cpp` has since been replaced by
> canonical `src/rrr/src/utils.rs` plus the terminal `srpc_net.c` kernel.

This document tracks the plan to convert unsafe code in `src/rrr/rpc/` to safe code.

## Current State

| Category | LOC |
|----------|-----|
| Safe functions | ~850 |
| Unsafe functions/classes | ~350 |
| Safe functions with @unsafe blocks | ~150 |

## Conversion Priority

### Priority 1: Trivial (annotation changes only) ✅ COMPLETED

| Item | Location | Current Reason | Fix | Status |
|------|----------|----------------|-----|--------|
| `Arc::make` calls | Future::create(), Client::create() | Marked @unsafe by convention | Remove @unsafe - Arc::make is memory-safe | ✅ Done |
| String copy | ClientConnection::host() | `{ return host_; }` | Remove @unsafe block | ✅ Done |
| Template forwarding | request() overloads (6 places) | Calls to main request() | Remove @unsafe blocks | ✅ Done |

### Priority 2: Easy (in-place annotation additions) ✅ COMPLETED

| Item | Location | Current Reason | Fix | Status |
|------|----------|----------------|-----|--------|
| `Coroutine::CreateRun` | coroutine.h:58-65 | Not annotated | Add @safe annotation with rationale | ✅ Done |
| `Reactor::Loop()` | reactor.h:178-182 | Marked @unsafe | Changed to @safe with memory-safety rationale | ✅ Done |
| Caller sites | server.cpp, client.cpp | @unsafe blocks | Removed @unsafe blocks from callers | ✅ Done |

### Priority 3: Medium (minor refactoring) ✅ COMPLETED

| Item | Location | Current Reason | Fix | Status |
|------|----------|----------------|-----|--------|
| Stats functions | ~~stat_server_batching(), stat_server_rpc_counting()~~ | Global mutable state | Superseded 2026-08-01: the whole `#ifdef RPC_STATISTICS` block was **deleted** rather than annotated. It was never compiled (the macro is defined nowhere in the tree, in any CMakeLists, or in any CI script), and it had rotted past the point of being restorable — enabling the flag produced 4 hard compile errors, so "just turn it back on" was never a real fallback. | ✅ Deleted |
| `const_cast` in Client::connect() | client.cpp:421-444 | Init-before-sharing pattern | Replaced with `Arc::get_mut()` for Rust-idiomatic exclusive access | ✅ Done |
| `DeferredReply::reply()` | server.hpp:418-441 | weak_ptr + const_cast | Made `ServerConnection::reply()` const using interior mutability (SpinMutex) | ✅ Done |

### Priority 4: Harder (design changes)

| Item | Location | Current Reason | Fix | Status |
|------|----------|----------------|-----|--------|
| `Future::get_reply()` | client.hpp:165-170 | Returns &Marshal through temporary guard | Return `RefMut<Marshal>` guard directly | ✅ Done |
| `ClientConnection::connect()` | client.cpp:193-228 | Socket syscalls + addrinfo raw pointer | AddrInfo RAII wrapper (syscalls remain @unsafe) | ✅ Done |
| `ServerListener` | server.hpp/cpp | addrinfo raw pointer + syscalls | AddrInfo RAII member (syscalls remain @unsafe) | ✅ Done |

### Inherently Unsafe (keep as unsafe)

These are FFI calls to the OS and cannot be made safe:

| Item | Location | Reason |
|------|----------|--------|
| `set_nonblocking()` | utils.cpp:25-37 | fcntl FFI |
| `find_open_port()` | utils.cpp:39-87 | socket/bind/getsockname FFI |
| `get_host_name()` | utils.cpp:89-97 | gethostname FFI |
| `::close()` calls | Throughout | System call |
| `::accept()` | ServerListener::handle_read() | System call |
| `::connect()` | ClientConnection::connect() | System call |
| `setsockopt()` | Various | System call |

## Expected Results

| Priority | Items | LOC Reduction | Effort |
|----------|-------|---------------|--------|
| 1. Trivial | Arc::make, string copy, forwarding | ~30 LOC | 5 min |
| 2. Easy | Coroutine, Reactor, STL annotations | ~80 LOC | 30 min |
| 3. Medium | const_cast cleanup, stats | ~60 LOC | 2 hours |
| 4. Harder | get_reply(), connect() refactor | ~100 LOC | 1 day |
| Keep unsafe | System calls | ~120 LOC | N/A |

**Estimated total unsafe LOC after all conversions: ~120 LOC** (down from ~350)

---

## Coroutine Safety Improvements

This section tracks the plan to make `Coroutine::CreateRunImpl` and related functions safe.

### Current Unsafe Patterns in CreateRunImpl

| Pattern | Location | Description |
|---------|----------|-------------|
| `const_cast` | reactor.cc:114 | Bypasses Rc's const access to mutate coroutine fields |
| Thread-local mutation | reactor.cc:141,164 | Direct assignment to `sp_running_coro_th_` |
| `Rc::make`/`Rc::clone` | reactor.cc:121,141 | Not marked @safe (unlike Arc::make) |
| `Run()` calls | reactor.cc:156 | Coroutine::Run() is @unsafe |

### Improvement Plan

| Priority | Item | Location | Fix | Status |
|----------|------|----------|-----|--------|
| 1 | `Rc::make`/`Rc::clone` | rusty/rc.hpp | Mark @safe with internal @unsafe blocks | ✅ Done |
| 2 | `mutable` fields | coroutine.h | Replace with `Cell<T>`/`RefCell<T>` | ✅ Done |
| 3 | Thread-local state | reactor.cc:36 | Wrap `sp_running_coro_th_` in `RefCell` | ✅ Done |
| 4 | `Coroutine::Run()` | coroutine.h:93 | Mark @safe with internal @unsafe | ✅ Done |
| 5 | Refactor CreateRunCoroutine | reactor.cc:107-166 | Split into smaller safe functions | ✅ Done |

### Expected Impact

After all improvements:
- `CreateRunImpl` can be marked `@safe`
- `CreateRun` template wrapper no longer needs internal `@unsafe` block
- Eliminates all `const_cast` in coroutine management
- Thread-local state uses explicit interior mutability

---

## Change Log

- 2026-01-01: Priority 4 complete - AddrInfo RAII wrapper
  - Added `AddrInfo` RAII class to utils.hpp with:
    - Automatic `freeaddrinfo()` on destruction
    - Move semantics (no copy)
    - `resolve()` factory returning `Result<AddrInfo, int>`
    - Proper lifetime annotations (`@lifetime: (&'a mut) -> &'a mut` for operator=)
  - Updated `find_open_port()` in utils.cpp to use AddrInfo
  - Updated `ClientConnection::connect()` in client.cpp to use AddrInfo
  - Updated `ServerListener` in server.hpp/cpp:
    - Changed `p_gai_result_` from raw `addrinfo*` to `AddrInfo` member
    - Destructor simplified (RAII handles cleanup)
  - All tests pass (test_rpc: 17, test_future: 12, ctest: 25/26)

- 2026-01-01: Priority 4 partial - Future::get_reply() lifetime safety
  - Changed `get_reply()` to return `RefMut<Marshal>` guard instead of `Marshal&`
  - Caller now holds the guard, ensuring reference can't outlive it (Rust-idiomatic)
  - Added `operator>>` overloads to `RefMut<T>` for seamless streaming
  - Updated all callers (126 occurrences across 26 files) - most unchanged due to operator overload
  - Fixed 4 places that stored in `auto&` or called `.method()` directly
  - All tests pass (test_rpc: 17, test_future: 12)

- 2025-12-31: Priority 3 complete - const_cast elimination and documentation
  - Stats functions: Accepted as inherently unsafe (debug-only under `#ifdef RPC_STATISTICS`)
    - **Superseded 2026-08-01**: block deleted outright — never compiled, and 4 hard errors if enabled.
  - Client::connect(): Replaced const_cast with Arc::get_mut()
    - Arc::get_mut() now returns `Option<T&>` (Rust-idiomatic API)
    - Returns Some when strong_count == 1 (exclusive ownership)
    - Freshly-created Arc always has strong_count == 1
    - Follows Rust's idiomatic init-before-sharing pattern
    - No const_cast needed - proper Rust-style exclusive access
  - DeferredReply::reply(): Eliminated const_cast by making reply() const
    - ServerConnection::reply() now uses interior mutability via SpinMutex
    - SpinMutex::lock() const already supports const access
    - Removed const_cast from DeferredReply::reply()
  - Also updated Rc::get_mut() to return Option<T&> for consistency
  - Updated all callers in janus and rusty-cpp tests/examples
  - All 26 unit tests pass

- 2025-12-28: ServerConnection::handle_read safety improvement
  - Marked `ServerConnection::handle_read()` as `@safe` with internal `@unsafe` blocks
  - After rusty-cpp loop support fix, borrow checker correctly handles:
    - Fresh variables in loop iterations (no false use-after-move)
    - Mutually exclusive branches with use/move patterns
  - Internal @unsafe blocks document actual unsafe operations:
    - STL list operations, Marshal operations
    - `server_->` raw pointer dereference
    - Mutex operations, coroutine creation
    - Reactor operations
  - Note: `read_from_fd()` is @safe - no @unsafe block needed
  - All 26 unit tests pass

- 2025-12-28: Coroutine safety improvement - Priority 5 complete (ALL PRIORITIES DONE!)
  - Refactored `CreateRunCoroutine` into smaller @safe helper functions:
    - `GetOrCreateCoroutine()` - handles coroutine pooling/creation
    - `SaveRunningCoroutine()` - saves current coroutine context for nesting
    - `RestoreRunningCoroutine()` - restores previous coroutine context
    - `SetRunningCoroutine()` - sets the current running coroutine
    - `RegisterCoroutine()` - adds coroutine to active set
  - Main `CreateRunCoroutine` now orchestrates these helpers with clear steps
  - All helper functions marked @safe with internal @unsafe blocks
  - All 26 unit tests pass

- 2025-12-27: Coroutine safety improvement - Priority 4 complete
  - Marked `Coroutine::Run()`, `Yield()`, `Continue()` as `@safe` with internal `@unsafe` blocks
  - Updated header annotations with SAFETY rationale
  - Format: `// @unsafe` on one line, `{` on next line (per documentation)
  - Wrapped all unsafe operations: RefCell, GetReactor, STL, const_cast, std::bind, boost
  - All 26 unit tests pass

- 2025-12-27: Coroutine safety improvement - Priority 3 complete
  - Wrapped `sp_running_coro_th_` thread-local in `RefCell<Option<Rc<Coroutine>>>`
  - Note: Used RefCell instead of Cell because `Option<Rc<T>>` is not trivially copyable
  - Updated all access sites to use `borrow()` / `borrow_mut()`:
    - `CurrentCoroutine()`: reads via `borrow()`
    - `CreateRunCoroutine()`: save/restore pattern with `borrow()` and `borrow_mut()`
    - `ContinueCoro()`: save/restore pattern with `borrow()` and `borrow_mut()`
  - Fixed raft/frame.cc debug logging to use RefCell API
  - All 26 unit tests pass

- 2025-12-27: Coroutine safety improvement - Priority 2 complete
  - Replaced `mutable` fields with `Cell<T>`/`RefCell<T>` in Coroutine class:
    - `status_`: `mutable Status` → `Cell<Status>`
    - `needs_finalize_`: `mutable bool` → `Cell<bool>`
    - `func_`: `mutable rusty::Function<void()>` → `RefCell<rusty::Function<void()>>`
    - `boost_coro_task_`: `mutable rusty::Option<rusty::Box<...>>` → `RefCell<rusty::Option<rusty::Box<...>>>`
    - `boost_coro_yield_`: kept as `mutable` (boost reference type)
  - Updated all access sites in coroutine.cc, reactor.cc, event.cc to use Cell/RefCell API
  - Eliminated `const_cast` in `CreateRunCoroutine` and `Recycle` functions
  - All 26 unit tests pass

- 2025-12-27: Coroutine safety improvement - Priority 1 complete
  - Marked `Rc::make`, `Rc::new_`, copy constructors, `clone()` as @safe with internal @unsafe blocks
  - Marked `Rc::operator*`, `operator->`, `get()` as @safe with internal @unsafe blocks
  - Marked all Rc comparison operators as @safe
  - All 26 unit tests pass

- 2025-12-27: Created plan, completed Priority 1 and 2
  - Priority 1 (Trivial): Removed unnecessary @unsafe blocks from Arc::make, string copy, and template forwarding
  - Priority 2 (Easy): Added @safe annotation to Coroutine::CreateRun and Reactor::Loop with rationale
  - Added external annotations for: rusty::Arc::make, std::forward, std::string::basic_string, etc.
  - Note: ServerConnection::handle_read() remains @unsafe due to server_-> pointer dereferences and complex Box move control flow
  - All 26 unit tests pass
