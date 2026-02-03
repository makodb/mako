# RustyCpp Migration Plan for Raft Module

**Last Updated**: 2025-10-30
**Status**: Phase 2 - Ownership Migration (COMPLETE), Phase 3 BLOCKED
**Owner**: Migration team
**Original Estimated Duration**: 5-6 weeks
**Actual Progress**: Phases 0-2 complete (3 weeks ahead of schedule)

**⚠️ IMPORTANT**: Phase 3 (migrate to rusty structures) is **BLOCKED** because `shared_ptr<Marshallable>` is used in virtual function interfaces (`Coordinator::Submit()`) that are shared across ALL protocol modules (Paxos, MongoDB, Copilot, Janus, etc.). Migrating Raft alone would break polymorphism. See [Phase 3](#phase-3-migrate-data-structures-week-3) for details.

---

## 🚨 QUICK START FOR NEW CONTRIBUTORS

**Current Phase**: Phase 3 - Data Structures (Ready to start)
**What's been done**:
- ✅ All .cc file functions annotated with `@safe`/`@unsafe`
- ✅ Memory leaks fixed (Timer, Frame destructor)
- ✅ Frame ownership migrated to smart pointers (unique_ptr)
**What to do next**: Migrate `shared_ptr` → `rusty::Arc` in data structures OR cleanup code
**How to test**: `cd build && make borrow_check_raft`
**See**: [Current Progress](#current-progress) and [What's Next](#whats-next)

---

## Table of Contents
1. [Current Progress](#current-progress)
2. [RustyCpp Checker - Comprehensive Guide](#rustycpp-checker-comprehensive-guide)
3. [Executive Summary](#executive-summary)
4. [Current State Analysis](#current-state-analysis)
5. [Available RustyCpp Types](#available-rustycpp-types)
6. [Migration Strategy](#migration-strategy)
7. [Detailed Phase-by-Phase Plan](#detailed-phase-by-phase-plan)
8. [Testing Strategy](#testing-strategy)
9. [Rollback Plan](#rollback-plan)
10. [Success Criteria](#success-criteria)

---

## Current Progress

### ✅ Phase status
- **Phase 0 - Annotations**: ✅ COMPLETE
  - **Phase 0.1 – Environment setup**: Borrow checker wired into CMake ✅
  - **Phase 0.2 – Dependency analysis**: `FUNCTION_DEPENDENCY_ANALYSIS.md` captured call graph ✅
  - **Phase 0.3 – Per-function annotations**: All raft `.cc` files annotated and pass checker ✅
  - **Phase 0.4 – Header/inline coverage**: ⚠️ BLOCKED (waiting for rusty-cpp template fix - see TODO below)

- **Phase 1 - Fix Memory Leaks**: ✅ COMPLETE (2025-10-30)
  - **Step 1.1**: Timer leak fixed using `rusty::Box<Timer>` ✅
  - **Step 1.2**: RaftFrame destructor added ✅

- **Phase 2 - Migrate Frame Ownership**: ✅ COMPLETE (2025-10-30)
  - **Step 2.1**: `commo_` migrated to `std::unique_ptr<RaftCommo>` ✅
  - **Step 2.2**: `svr_` migrated to `std::unique_ptr<RaftServer>` ✅
  - **Step 2.3**: Ownership semantics documented ✅

- **Phase 3 - Migrate Data Structures**: ⚠️ BLOCKED (requires system-wide changes - see Phase 3 details)
- **Phase 4 - Cleanup**: 📍 READY TO START (recommended next step)
- **Phase 5 - Enable More Safety**: Deferred (most functions already annotated)

### ✅ Checker status
Running the dedicated target or individual invocations such as
```bash
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  --compile-commands build/compile_commands.json \
  src/deptran/raft/server.cc
```
now completes with **no violations** for every file under `src/deptran/raft/`.

### 📊 Per-file annotation snapshot
The tables below reflect the exact annotations currently in-tree. “Reason” entries summarise why a function remains `@unsafe` (often raw pointer outs, shared ownership of reactor state, or template gaps). Safe functions are listed when they are noteworthy (constructors, primary entry-points). Any function not listed is still **undeclared** and should be audited before marking safe.

#### `exec.cc`
- **@safe**: `RaftExecutor::Prepare`, `RaftExecutor::Accept`, `RaftExecutor::AppendEntries`, `RaftExecutor::Decide`  
  _Rationale_: stubbed implementations that immediately `verify(0)`; no ownership or pointer work.
- **@unsafe**: _none_

#### `service.cc`
- **@safe**: `RaftServiceImpl::RaftServiceImpl`, `HandleVote`, `HandleAppendEntries`, `HandleEmptyAppendEntries`  
  _Notes_: `Handle*` helpers dispatch onto the scheduler via `Coroutine::CreateRun`. The coroutine helper is annotated in `rrr` and the lambdas avoid raw pointer manipulation.
- **@unsafe**: _none_

#### `frame.cc`
- **@safe**: `RaftFrame::RaftFrame`, `CreateExecutor`, `CreateScheduler`, `CreateCommo`, `CreateRpcServices`  
  _Notes_: Allocation via `new` is permitted in safe code; logging helpers are already marked `@unsafe` upstream.
- **@unsafe**: `RaftFrame::CreateCoordinator` – takes the address of `slot_hint_` for out-parameters and mixes in `Config::GetPartitionSize` (still undeclared). Requires structural refactor before it can be audited safe.

#### `commo.cc`
- **@safe**: `RaftCommo::RaftCommo`, `SendAppendEntries2`, `SendAppendEntries`, `BroadcastVote`  
  _Notes_: External annotations were added for `Reactor::CreateSpEvent`, async RPC stubs, and STL helpers so the dispatcher stays safe.
- **@unsafe**: _none_

#### `coordinator.cc`
- **@safe**: `CoordinatorRaft::CoordinatorRaft`, `IsLeader`, `IsFPGALeader`, `Submit`, `Commit`, `LeaderLearn`, `GotoNextPhase`  
  _Notes_: `Submit` now carries explicit external annotations for `std::dynamic_pointer_cast` and `std::shared_ptr::operator bool`.
- **@unsafe**: `CoordinatorRaft::AppendEntries` – still uses raw pointer output parameters (`&index`, `&term`) and template factories (`Reactor::CreateSpEvent`). Requires API rewrite (see TODO in file).

#### `server.cc`
- **@safe**: `RaftServer::RaftServer`, `Setup`, `Disconnect`, `IsDisconnected`, `~RaftServer`, `RequestVote`, `OnRequestVote`, `StartElectionTimer`, `OnAppendEntries`, `removeCmd`
- **@unsafe**:  
  • `RaftServer::setIsLeader` – mutates shared state via raw address-of and constructs shared state for commo (`std::make_shared`)  
  • `RaftServer::applyLogs` – drives the user callback `app_next_` and clears slots via pointers/borrowed references  
  • `RaftServer::HeartbeatLoop` – owns the reactor event pointer, flips it between `nullptr`/`CreateSpEvent`, and batches commands that the checker treats as “moved”  
  • `RaftServer::Start` – writes back through `uint64_t*` out-params returned by the scheduler

#### `coordinator.cc` / **view interop**
- `view.h` now exposes `View& operator=(const View&) = default; // @safe` so assignments in `CoordinatorRaft::Submit` pass the checker.

### 📌 Remaining gaps / follow-ups
- Header-only helpers (notably `server.h`, `commo.h`, `coordinator.h`) remain **undeclared**. Phase 0.4 should cover them once we have additional coverage tests.
- The earlier switch-statement limitation has not reappeared since `AppendEntries` is marked unsafe, but manual reviews of complex control-flow remain recommended until CFG work lands upstream.
- Several `@unsafe` functions (see above) require interface changes (e.g., eliminate raw pointer out-parameters) before we can consider auditing them safe.

#### 📚 Related Files Annotated (Outside Raft Module)

##### src/rrr/reactor/coroutine.h
- `Coroutine::CreateRun()` template - **@unsafe** (annotated for documentation, but checker ignores it - see template limitation)
  - Despite annotation, this appears as "undeclared" when called from any @safe function
  - This is due to template limitation in the borrow checker

##### src/rrr/base/logging.hpp
- **ALL logging functions marked @unsafe** (critical for frame.cc to pass)
  - `Log::info()` (both overloads) - **@unsafe**
  - `Log::debug()` (both overloads) - **@unsafe**
  - `Log::error()` (both overloads) - **@unsafe**
  - `Log::warn()` (both overloads) - **@unsafe**
  - `Log::fatal()` (both overloads) - **@unsafe**
  - **Key insight**: Each function declaration needs its own `// @unsafe` annotation, not just a group comment
  - **Why unsafe**: Variadic functions using va_list

##### src/deptran/config.h
- `Config::GetConfig()` - **@safe** (returns singleton instance)
- `Config::GetPartitionSize()` - **NOT annotated** (still undeclared, causes CreateCoordinator to be @unsafe)

### 🎯 What's Next

**Completed:**
1. ✅ All .cc file annotations complete
2. ✅ Timer memory leak fixed
3. ✅ Frame destructor added
4. ✅ Frame ownership migrated to smart pointers

**TODO - Blocked by RustyCpp limitations:**
- 📌 **Phase 0.4 - Header/inline coverage**: BLOCKED until rusty-cpp fixes template support
  - Cannot annotate inline functions in headers that call template functions
  - Template functions are completely ignored by the borrow checker (see [RustyCpp Checker Guide](#rustycpp-checker-comprehensive-guide))
  - Once rusty-cpp adds template support, return to annotate inline functions in:
    - `server.h` (inline helpers like `resetTimerBatch()`)
    - `commo.h` (inline wrappers)
    - `coordinator.h` (inline accessors like `commo()`)

**Ready to Start:**
- **Recommended**: Phase 4 - Code cleanup (remove commented code, improve docs)
- **Blocked**: Phase 3 - Migrate data structures (requires system-wide Coordinator interface changes)
- **Alternative**: Document current state and success metrics

### 🎯 Old Next Steps (Archived)
1. ✅ ~~Complete exec.cc annotation~~ - DONE
2. ✅ ~~Complete service.cc annotation~~ - DONE
3. ✅ ~~Complete frame.cc annotation~~ - DONE
4. ✅ ~~Complete commo.cc annotation~~ - DONE
5. ✅ ~~Complete coordinator.cc annotation~~ - DONE (but see checker bug warning)
6. **🔄 CURRENT: Verify server.cc annotations are complete**
7. After all .cc functions are marked, move to Phase 0.4: Handle inline header functions
8. Run comprehensive tests after all annotations
9. **⚠️ NEW: Manual audit of switch statement function calls** across all files to verify checker bug doesn't hide issues

### 🔍 Borrow Checker Status
**Command**: `cd build && make borrow_check_raft` OR directly run checker:
```bash
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  --compile-commands build/compile_commands.json \
  src/deptran/raft/<file>.cc
```
**Current Result**: ✅ **PASSING** for exec.cc, service.cc, frame.cc, and commo.cc (no violations found)

---

## RustyCpp Checker - Comprehensive Guide

> ⚠️ **CRITICAL**: This section contains hard-won knowledge about how the rusty-cpp borrow checker actually works. Read this carefully before continuing!

### How the Checker Actually Works

#### What Files Are Analyzed
```cmake
# CMakeLists.txt configuration
file(GLOB RAFT_BORROW_SRC
    "src/deptran/raft/*.cc"    # ✅ These ARE analyzed
    "src/deptran/raft/*.cpp"   # ✅ These ARE analyzed
    # "src/deptran/raft/*.h"   # ❌ Do NOT add - headers can't be analyzed standalone
)
```

**Key Insight #1**: The checker only analyzes `.cc`/`.cpp` files, NOT `.h` files standalone.

**Key Insight #2**: When analyzing a `.cc` file, the checker DOES parse all `#include` headers and checks:
- Function declarations with annotations
- Inline functions that are CALLED from the .cc file

**Key Insight #3**: Unused inline functions are NOT checked (only analyzed when instantiated/called)

#### Annotation Propagation

```cpp
// foo.h
// @safe
void doSomething();  // Declaration with annotation

// foo.cc
void doSomething() { // ✅ Annotation from header applies here
    // implementation
}
```

**Declaration annotations in headers automatically apply to implementations in .cc files.**

#### Inline Functions - The Tricky Part

```cpp
// server.h
// @safe
inline void resetTimerBatch() {  // Inline function
    resetTimer();
}

// server.cc
#include "server.h"

void SomeFunction() {
    resetTimerBatch();  // ✅ NOW resetTimerBatch() is checked (because it's called)
}
```

**Inline functions ARE checked, but ONLY when they're called from a .cc file being analyzed!**

If `resetTimerBatch()` is never called from any .cc file → it will NOT be checked, even if annotated.

#### Template Functions - Critical Limitation ⚠️

**🚨 DISCOVERED ISSUE**: Template function annotations are **COMPLETELY IGNORED** by the borrow checker!

```cpp
// coroutine.h
class Coroutine {
  // @unsafe - Delegates to CreateRunImpl which manages coroutine lifecycle
  template <typename Func>
  static std::shared_ptr<Coroutine> CreateRun(Func&& func, ...) {
    return CreateRunImpl(...);
  }
};
```

**What happens when analyzing a `.cc` file:**
```cpp
// service.cc
// @safe  ❌ WILL FAIL!
void MyFunction() {
    Coroutine::CreateRun([]() { ... });
    // ERROR: Calling undeclared function 'Coroutine::CreateRun'
}
```

**Why it fails:**
1. Checker parses `coroutine.h`
2. Sees `template <typename Func>` keyword
3. **SKIPS the entire declaration** (templates not supported - see RustyCpp CLAUDE.md)
4. Never stores `CreateRun` in symbol table
5. When analyzing `service.cc`, `CreateRun` is **UNDECLARED** (not @safe, not @unsafe, just undeclared)

**Calling Rules Reminder:**
```
@safe function CAN call:
  ✅ @safe functions
  ✅ @unsafe functions
  ❌ undeclared functions (WILL FAIL)

@unsafe function CAN call:
  ✅ anything (including undeclared)
```

**The Workaround:**
Any function calling a template function MUST be marked `@unsafe`:

```cpp
// @unsafe - Calls Coroutine::CreateRun() template function
// CreateRun is a template (template <typename Func>) which the borrow checker cannot parse
// Templates are skipped during analysis, so CreateRun appears as "undeclared"
// Safe functions cannot call undeclared functions, hence this must be @unsafe
void HandleAppendEntries(...) {
    Coroutine::CreateRun([&] () { ... });
}
```

**Key Insight #4**: Template functions are ALWAYS undeclared to the borrow checker, regardless of annotations. Any caller must be marked `@unsafe`.

**Affected Functions in Raft Module:**
- `Coroutine::CreateRun()` - Most common template called
- Any `std::` template functions without explicit external annotations
- Any custom template functions

**From RustyCpp Documentation:**
```
### What's Not Implemented Yet ❌
- ❌ **Templates**
  - Template declarations ignored
  - No instantiation tracking
  - Generic code goes unchecked
```

#### Annotation Requirements - Critical Discovery ⚠️

**Key Insight #5**: Each function declaration needs its own individual `// @unsafe` or `// @safe` annotation. Group comments DO NOT work!

**❌ DOESN'T WORK:**
```cpp
// @unsafe - All these logging functions
static void info(int line, const char* file, const char* fmt, ...);
static void debug(int line, const char* file, const char* fmt, ...);
static void error(int line, const char* file, const char* fmt, ...);
```

**✅ WORKS:**
```cpp
// @unsafe
static void info(int line, const char* file, const char* fmt, ...);
// @unsafe
static void debug(int line, const char* file, const char* fmt, ...);
// @unsafe
static void error(int line, const char* file, const char* fmt, ...);
```

**Why this matters:**
- The borrow checker looks for annotations **directly before** each function signature
- A single comment at the top of a group is NOT parsed as applying to all functions
- This was the root cause of `frame.cc` failing with "undeclared function rrr::Log::info" errors
- Required annotating ALL 10 logging function overloads in `logging.hpp` individually

**Example from logging.hpp fix:**
- Before: 5 functions with 1 group comment → All appeared "undeclared"
- After: 5 functions each with `// @unsafe` → All properly recognized

### What `@safe` Actually Checks

The RustyCpp checker is **NOT** as strict as Rust! It's pragmatic for C++ migration.

#### ✅ DOES Check (Will Fail Build)
1. **Calling undeclared functions from `@safe` context**
   ```cpp
   // @safe
   void foo() {
       bar();  // ❌ ERROR: bar() is undeclared - must be @safe or @unsafe
   }
   ```

2. **Borrowing violations**
   ```cpp
   // @safe
   void foo() {
       int x = 5;
       int& ref1 = x;
       int& ref2 = x;  // Checked for multiple mutable borrows
   }
   ```

3. **Use-after-move violations**
4. **Lifetime violations** (dangling references)

#### ❌ does NOT Forbid (Will Pass)
1. **Raw pointer parameters** (too common in C++ APIs)
   ```cpp
   // @safe - THIS IS ALLOWED
   void foo(Frame* frame) {  // ✅ Raw pointer param is OK
       frame_ = frame;       // ✅ Raw pointer assignment is OK
   }
   ```

2. **`new` operator**
   ```cpp
   // @safe - THIS IS ALLOWED
   void foo() {
       timer_ = new Timer();  // ✅ 'new' is allowed in @safe
   }
   ```

3. **Simple pointer operations** (assignment, nullptr checks)

**Why?** The checker focuses on **borrowing safety**, not **all memory safety**. Forbidding all raw pointers would make migration impossible in large C++ codebases.

### Calling Rules

```
@safe function can call:
  ✅ @safe functions
  ✅ @unsafe functions
  ❌ undeclared functions (WILL FAIL)

@unsafe function can call:
  ✅ anything (no restrictions)

undeclared function:
  ⚠️ Cannot be called from @safe (WILL FAIL)
  ✅ Can be called from @unsafe
```

### How to Run the Checker

```bash
# Recommended: Use make target
cd build
make borrow_check_raft

# Direct invocation (for single file)
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
    --compile-commands build/compile_commands.json \
    src/deptran/raft/server.cc

# Check if checker binary exists
ls -lh ./third-party/rusty-cpp/target/release/rusty-cpp-checker
```

### Common Errors and Solutions

#### Error: "Calling undeclared function"
```
In function 'janus::RaftServer::resetTimerBatch':
Calling undeclared function 'janus::RaftServer::resetTimer' at line 944
- must be explicitly marked @safe or @unsafe
```

**Solution**: Mark the called function as `@safe` or `@unsafe`, or change the calling function to `@unsafe`.

#### Error: "Fatal error: 'deptran/communicator.h' file not found"
**Cause**: Trying to analyze a `.h` file standalone (not in compile_commands.json)
**Solution**: Only analyze `.cc` files. Remove `.h` from RAFT_BORROW_SRC in CMakeLists.txt

#### Error: "Attempt to add a custom rule to output ... which already has a custom rule"
**Cause**: Duplicate stamp files when both `.h` and `.cc` files have same basename
**Solution**: Don't add `.h` files to RAFT_BORROW_SRC

---

## Current Strategy: .cc Files First, .h Files Later

### Why Focus on .cc Files First?

1. **.cc files are always analyzed** - guaranteed to be checked
2. **Immediate feedback** - violations caught right away
3. **Most code is in .cc** - bigger impact
4. **Easier to test** - compile_commands.json includes them

### Why Defer .h Inline Functions?

1. **Only checked if called** - harder to verify coverage
2. **Many are unused** - wasted effort annotating dead code
3. **Can batch later** - after .cc foundation is solid

### The Plan

**Phase 0A (Current)**: Annotate all .cc file functions
- Start with leaf functions (Level 0)
- Move to Level 1 (functions calling Level 0)
- Progress upward through dependency levels
- Test continuously with `make borrow_check_raft`

**Phase 0B (Future)**: Handle .h inline functions
- Identify which are actually called from .cc files
- Annotate only the used ones
- Document unused ones for potential removal

**Phase 1+**: Smart pointer migration (see detailed plan below)

---

## Executive Summary

### Goal
Migrate the Raft consensus implementation (`src/deptran/raft/`) to use RustyCpp smart pointers and enable borrow checking for memory safety guarantees.

### Why This Matters
- **Memory Leaks Found**: Timer object not properly deleted (line 17 in `server.cc`)
- **Unclear Ownership**: Raw pointers throughout (Frame*, RaftCommo*, RaftServer*)
- **Safety Guarantees**: Enable compile-time memory safety checks
- **Consistency**: Align with project-wide RustyCpp migration (RRR already uses Arc)

### Scope
- **~4000 lines of code** across 9 files
- **4 core classes**: RaftServer, CoordinatorRaft, RaftCommo, RaftFrame
- **No functional changes**: Pure refactoring for safety

### Key Principle
> "Make small incremental changes - Change one field/function at a time, Test after each change"
> — From project CLAUDE.md

---

## Current State Analysis

### File Structure
```
src/deptran/raft/
├── server.h/cc      (~1200 lines) - Core Raft algorithm
├── coordinator.h/cc (~450 lines)  - Transaction coordination
├── commo.h/cc       (~350 lines)  - RPC communication
├── frame.h/cc       (~300 lines)  - Component factory
├── service.h/cc     (~400 lines)  - RPC service handlers
├── exec.h/cc        (~100 lines)  - Command execution
├── test.h/cc        (~800 lines)  - Test infrastructure
├── testconf.h/cc    (~400 lines)  - Test configuration
└── macros.h         (~50 lines)   - Helper macros
```

### Component Dependencies
```
RaftFrame (Factory)
    │
    ├──> creates RaftCommo (new, stored as raw pointer)
    ├──> creates RaftServer (new, stored as raw pointer)
    └──> creates CoordinatorRaft
             └──> holds RaftServer* (non-owning reference)

RaftServer
    ├──> holds Frame* (non-owning reference to creator)
    ├──> holds Timer* (owns, MEMORY LEAK - no delete!)
    └──> holds map<slotid_t, shared_ptr<RaftData>> (already safe)

RaftCommo
    ├──> takes rusty::Arc<PollThreadWorker> (already RustyCpp!)
    └──> returns shared_ptr<IntEvent> (needs migration)
```

### Memory Management Issues Found

#### 🔴 CRITICAL - Memory Leak
```cpp
// server.cc:17
RaftServer::RaftServer(Frame * frame) {
  timer_ = new Timer();  // ❌ NEVER DELETED!
}

// server.cc:554
RaftServer::~RaftServer() {
  // No delete timer_!
}
```

#### 🟡 MEDIUM - Unclear Ownership
```cpp
// frame.cc:CreateScheduler()
svr_ = new RaftServer(this);  // Who deletes this?

// frame.cc:CreateCommo()
commo_ = new RaftCommo(poll_thread_worker);  // Who deletes this?
```
**Finding**: No destructor in RaftFrame, no delete calls → **MEMORY LEAK**

#### 🟢 GOOD - Already Using Smart Pointers
```cpp
// server.h:152, 164
map<slotid_t, shared_ptr<RaftData>> logs_{};           // ✅
shared_ptr<IntEvent> ready_for_replication_;           // ✅

// commo.cc:111
auto res = std::make_shared<SendAppendEntriesResults>(); // ✅
```

### Current Smart Pointer Usage

| Type | Count | Location | Status |
|------|-------|----------|--------|
| `shared_ptr<Marshallable>` | ~15 | Throughout | Needs migration to Arc |
| `shared_ptr<RaftData>` | 2 maps | server.h | Needs migration to Arc |
| `shared_ptr<IntEvent>` | ~5 | commo.cc, server.h | Needs migration to Arc |
| `rusty::Arc<PollThreadWorker>` | 1 | commo.cc | ✅ Already RustyCpp! |
| Raw pointers | ~6 | server.h, frame.h | ❌ Needs migration |

---

## Available RustyCpp Types

### Confirmed Available in `third-party/rusty-cpp/include/rusty/`

#### Smart Pointers
- ✅ **`rusty::Box<T>`** - Single ownership (like `std::unique_ptr`)
  - Use for: Timer, owned objects
  - API: `Box::new_(value)`, `box->method()`, move semantics

- ✅ **`rusty::Arc<T>`** - Thread-safe shared ownership (like `std::shared_ptr` with atomics)
  - Use for: Marshallable, RaftData, IntEvent (if shared across threads)
  - API: `Arc::new_(value)`, `arc.clone()`, `arc->method()`

- ✅ **`rusty::Rc<T>`** - Single-thread shared ownership (cheaper than Arc)
  - Use for: RaftData if only accessed from one thread
  - API: Same as Arc but not thread-safe

- ✅ **`rusty::Weak<T>`** - Weak references (for Arc/Rc)
  - Use for: Breaking circular references
  - API: `weak.upgrade()` returns Option

#### Synchronization
- ✅ **`rusty::Mutex<T>`** - Interior mutability with locking
  - Wraps std::mutex but with RAII guard
  - API: `auto guard = mutex.lock(); guard->method();`

#### Collections
- ✅ **`rusty::Vec<T>`** - Dynamic array (alias to std::vector for now)
- ✅ **`rusty::HashMap<K,V>`** - Hash map
- ✅ **`rusty::BTreeMap<K,V>`** - Ordered map (could replace std::map)

#### Utility
- ✅ **`rusty::Option<T>`** - Optional values
- ✅ **`rusty::Result<T,E>`** - Result with error handling
- ✅ **`rusty::Cell<T>`** - Interior mutability for Copy types

### Migration Mapping

| Current Type | RustyCpp Type | Rationale |
|--------------|---------------|-----------|
| `Timer*` | `rusty::Box<Timer>` | Single ownership, no sharing |
| `Frame*` | Keep as raw `Frame&` | Non-owning reference to creator |
| `RaftCommo*` (in Frame) | `rusty::Box<RaftCommo>` | Frame owns it |
| `RaftServer*` (in Frame) | `rusty::Box<RaftServer>` | Frame owns it |
| `RaftServer*` (in Coordinator) | Keep as raw `RaftServer*` | Non-owning reference |
| `shared_ptr<Marshallable>` | `rusty::Arc<Marshallable>` | Shared, likely cross-thread |
| `shared_ptr<RaftData>` | `rusty::Arc<RaftData>` | Shared, likely cross-thread |
| `shared_ptr<IntEvent>` | `rusty::Arc<IntEvent>` | Events are shared |
| `std::map<K,V>` | `rusty::BTreeMap<K,V>` or keep std::map | Gradual migration |
| `std::recursive_mutex` | Keep for now | RustyCpp Mutex not recursive |

---

## Migration Strategy

### Principles
1. **Incremental**: One field/function at a time
2. **Test-Driven**: Run tests after EVERY change
3. **Reversible**: Keep git commits small for easy rollback
4. **Document**: Comment ownership semantics
5. **Safety First**: Fix memory leaks before optimization

### Constraints
- ✅ **No functional changes** to Raft algorithm
- ✅ **Maintain API compatibility** with parent classes
- ✅ **Keep tests passing** at each step
- ❌ **Do NOT change parent classes** (TxLogServer, Coordinator, Communicator) yet
- ❌ **Do NOT enable borrow checking** until Phase 5

### Test Commands
```bash
# After each change, rebuild using CMake:
rm -rf build && mkdir build
cmake -B build -DRAFT_TEST=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j32     # Full build (10-30 min initial, 2-10 min incremental)

# Or use the convenience script:
bash compile-cmake.sh

# Run Raft lab tests (coroutine-based testing):
cmake -B build -DRAFT_TEST=ON
cmake --build build -j32
./build/deptran_server -f config/raft_lab_test.yml

# Run production Raft tests (basic 3-replica setup):
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -d 30 -m 100 -P localhost

# Run Raft with higher concurrency (stress test):
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -m 100 -P localhost
```

**Important**:
- Always use `cmake --build build -j32`, NOT `make -j32` alone (won't regenerate config)
- After modifying CMakeLists.txt, always do `rm -rf build && mkdir build` first
- The `-DREUSE_CORO` flag is **CRITICAL for Raft stability** (enabled by default in CMakeLists.txt line 281)
  - Without this flag, Raft will crash with term mismatch errors
  - Verify it's enabled: `grep -o "\-DREUSE_CORO" build/compile_commands.json | wc -l` (should show ~250+)
- Build times: 10-30 minutes for initial build, 2-10 minutes for incremental
- Test `-DRAFT_TEST=ON` for lab tests, `-DRAFT_TEST=OFF` for production

---

## Detailed Phase-by-Phase Plan

### Phase 1: Fix Memory Leaks (Week 1)
**Goal**: Eliminate manual memory management, fix critical bugs

**Status**: ✅ COMPLETE (2025-10-30)
**Risk**: Low
**Actual Time**: 1 day

#### Step 1.1: Fix Timer Leak in RaftServer ✅ COMPLETE
**Files**: `server.h`, `server.cc`
**Completed**: 2025-10-30

**Before (Original):**
```cpp
// server.h:49
Timer *timer_;

// server.cc:17
timer_ = new Timer();

// server.cc:554
~RaftServer() {
  // No delete timer_!
}
```

**After (Implemented):**
```cpp
// server.h:49
#include <rusty/box.hpp>
rusty::Box<Timer> timer_;  // Owned timer, auto-cleaned on destruction

// server.cc:23
timer_(rusty::Box<Timer>::make(Timer()))  // Initialize Box in member initializer list

// server.cc:573
~RaftServer() {
  // timer_ automatically deleted by Box destructor ✅
}
```

**Note**: Used `Box::make()` instead of `Box::new_()` (correct API)

**Changes Required**:
1. Add `#include <rusty/box.hpp>` to `server.h`
2. Change `Timer* timer_` → `rusty::Box<Timer> timer_`
3. Change `new Timer()` → `rusty::Box<Timer>::new_(Timer())`
4. Update all usages: `timer_->method()` stays the same (Box has `->` operator)
5. **Special case**: Check `timer_->start()`, `timer_->elapsed()` calls

**Testing**:
```bash
rm -rf build && mkdir build
cmake -B build -DRAFT_TEST=ON
cmake --build build -j32
./build/deptran_server -f config/raft_lab_test.yml
```

**Success Criteria**:
- ✅ Code compiles
- ✅ Tests pass
- ✅ No memory leak in Timer (verify with valgrind if available)

**Rollback**:
```bash
git revert HEAD
```

---

#### Step 1.2: Add RaftFrame Destructor ✅ COMPLETE
**Files**: `frame.h`, `frame.cc`
**Completed**: 2025-10-30

**Issue Fixed**: Frame created RaftCommo and RaftServer with `new` but never deleted them

**Before (Original):**
```cpp
// frame.h
class RaftFrame : public Frame {
  RaftCommo *commo_ = nullptr;
  RaftServer *svr_ = nullptr;
  // No destructor!
};
```

**After (Implemented - Phase 2 superseded this):**
```cpp
// frame.h
class RaftFrame : public Frame {
  std::unique_ptr<RaftCommo> commo_;   // ✅ Migrated in Phase 2
  std::unique_ptr<RaftServer> svr_;    // ✅ Migrated in Phase 2
  ~RaftFrame() override;
};

// frame.cc (Phase 2 final version)
RaftFrame::~RaftFrame() {
  // commo_ and svr_ automatically cleaned up by unique_ptr ✅
}
```

**Success Criteria**: ✅ ACHIEVED
- ✅ No memory leaks for RaftCommo and RaftServer
- ✅ Automatic cleanup with smart pointers (Phase 2)

---

### Phase 2: Migrate Frame Ownership (Week 2)
**Goal**: Use smart pointers for Frame-owned components

**Status**: ✅ COMPLETE (2025-10-30)
**Risk**: Medium (affects component lifecycle)
**Actual Time**: 1 day
**Decision**: Used `std::unique_ptr` instead of `rusty::Box` due to lazy initialization pattern

#### Step 2.1: Migrate RaftCommo Ownership in Frame ✅ COMPLETE
**Files**: `frame.h`, `frame.cc`
**Completed**: 2025-10-30

**Important Decision**: Used `std::unique_ptr` instead of `rusty::Box` because:
- `rusty::Box` has no default constructor (non-nullable by design)
- RaftCommo uses lazy initialization pattern (`commo_ = nullptr` until `CreateCommo()` called)
- `std::unique_ptr` supports nullptr and has similar ownership semantics

**Before (Original):**
```cpp
// frame.h
RaftCommo *commo_ = nullptr;

// frame.cc
commo_ = new RaftCommo(poll_thread_worker);

// Destructor from Step 1.2
delete commo_;
```

**After (Implemented):**
```cpp
// frame.h
#include <memory>
std::unique_ptr<RaftCommo> commo_;  // Owned RaftCommo, automatically cleaned up

// frame.cc
commo_ = std::make_unique<RaftCommo>(poll_thread_worker);  // ✅

// Destructor
~RaftFrame() {
  // commo_ automatically deleted by unique_ptr ✅
}
```

**Changes Made**:
1. ✅ Changed `RaftCommo* commo_` → `std::unique_ptr<RaftCommo> commo_`
2. ✅ Updated `CreateCommo()`:
   ```cpp
   if (commo_ == nullptr) {
     commo_ = std::make_unique<RaftCommo>(poll_thread_worker);
   }
   return commo_.get();  // Return raw pointer for compatibility
   ```
3. ✅ Updated `CreateCoordinator()`: `coo->commo_ = commo_.get()`
4. ✅ All `commo_->method()` calls work as-is with unique_ptr's `->` operator
5. ✅ Removed `delete commo_;` from destructor
6. ✅ `testconf.cc` usages work automatically

**Success Criteria**: ✅ ALL ACHIEVED
- ✅ Code compiles
- ✅ Tests pass
- ✅ No change in behavior
- ✅ Memory automatically managed

---

#### Step 2.2: Migrate RaftServer Ownership in Frame ✅ COMPLETE
**Files**: `frame.h`, `frame.cc`, `testconf.cc`
**Completed**: 2025-10-30

**Before (Original):**
```cpp
// frame.h
RaftServer *svr_ = nullptr;

// frame.cc
svr_ = new RaftServer(this);
```

**After (Implemented):**
```cpp
// frame.h
std::unique_ptr<RaftServer> svr_;  // Owned RaftServer, automatically cleaned up

// frame.cc
svr_ = std::make_unique<RaftServer>(this);  // ✅

// Destructor
~RaftFrame() {
  // commo_ and svr_ automatically cleaned up by unique_ptr ✅
}
```

**Changes Made**:
1. ✅ Changed `RaftServer* svr_` → `std::unique_ptr<RaftServer> svr_`
2. ✅ Updated `CreateScheduler()`:
   ```cpp
   if (svr_ == nullptr) {
     svr_ = std::make_unique<RaftServer>(this);
   }
   return svr_.get();  // Return raw pointer for compatibility
   ```
3. ✅ Updated `CreateCoordinator()`: `coo->svr_ = this->svr_.get()`
4. ✅ Fixed `testconf.cc::GetServer()`: `return replicas[svr]->svr_.get();`
5. ✅ All `svr_->method()` calls work as-is with unique_ptr's `->` operator
6. ✅ Removed `delete svr_;` from destructor

**Success Criteria**: ✅ ALL ACHIEVED
- ✅ Code compiles
- ✅ Tests pass
- ✅ No change in behavior
- ✅ Memory automatically managed

---

#### Step 2.3: Document Ownership Semantics ✅ COMPLETE
**Files**: All Raft files
**Completed**: 2025-10-30

**Action**: Added comments explaining ownership ✅

**Implemented Documentation:**
```cpp
// frame.h (actual implementation)
class RaftFrame : public Frame {
  // Owns RaftCommo - created in CreateCommo(), destroyed with Frame ✅
  std::unique_ptr<RaftCommo> commo_;  // Owned RaftCommo, automatically cleaned up

  // Owns RaftServer - created in CreateScheduler(), destroyed with Frame ✅
  std::unique_ptr<RaftServer> svr_;  // Owned RaftServer, automatically cleaned up
};

// coordinator.h (unchanged - non-owning)
class CoordinatorRaft : public Coordinator {
  // Non-owning reference to RaftServer (owned by RaftFrame) ✅
  // Lifetime: RaftFrame outlives all Coordinators
  RaftServer* svr_ = nullptr;
};

// server.h (actual implementation)
class RaftServer : public TxLogServer {
  // Non-owning reference to creating Frame ✅
  // Lifetime: Frame creates RaftServer, so Frame outlives us
  Frame* frame_;

  // Owns Timer - destroyed with RaftServer ✅
  rusty::Box<Timer> timer_;  // Owned timer, auto-cleaned on destruction
};
```

**Testing**: ✅ No code changes, documentation complete

---

### Phase 3: Migrate Data Structures (Week 3)
**Goal**: Replace std::shared_ptr with rusty::Arc in data structures

**Status**: ⚠️ BLOCKED - Cannot proceed without system-wide changes
**Risk**: Medium-High (lots of shared_ptr usage)
**Estimated Time**: 7-10 days (if unblocked)

**🚨 BLOCKER IDENTIFIED (2025-10-30):**

**Cannot migrate `shared_ptr<Marshallable>` → `rusty::Arc<Marshallable>` in Raft module alone.**

**Root Cause**: Polymorphic interface in base class
```cpp
// src/deptran/coordinator.h (BASE CLASS used by ALL protocol modules)
class Coordinator {
  virtual void Submit(shared_ptr<Marshallable>& cmd, ...);  // ← Interface contract
  virtual void BulkSubmit(shared_ptr<Marshallable>& cmd, ...);
};

// src/deptran/raft/coordinator.h (RAFT SUBCLASS)
class CoordinatorRaft : public Coordinator {
  void Submit(shared_ptr<Marshallable>& cmd, ...) override;  // ← Must match base signature!
};
```

**Impact Analysis**:
- ❌ Changing Raft's Submit signature would break virtual function override
- ❌ Cannot change base class without affecting ALL protocol modules:
  - `paxos/coordinator.h`
  - `mongodb/coordinator.h`
  - `copilot/coordinator.h`
  - `janus/coordinator.h`
  - 10+ other modules
- ❌ `shared_ptr<Marshallable>` is used in 20+ files across the codebase

**Affected Data Structures** (all blocked):
- `CoordinatorRaft::cmd_` - Passed to/from virtual Submit()
- `RaftData::log_`, `accepted_cmd_`, `committed_cmd_` - Passed to Submit()
- `RaftServer::Start()` parameters - Receives from Submit()
- `shared_ptr<IntEvent> ready_for_replication_` - Created by Reactor API

**Unblocking Options**:
1. **System-wide migration** - Change base Coordinator interface + all subclasses (2-3 weeks, high risk)
2. **Type adapter pattern** - Convert between shared_ptr and Arc at boundaries (complex, performance overhead)
3. **Defer indefinitely** - `std::shared_ptr` is already safe for shared ownership

**Decision**: Phase 3 deferred. Focus on self-contained improvements (Phase 4 - Cleanup).

#### Step 3.1: Migrate RaftData Smart Pointers ⚠️ BLOCKED
**Files**: `server.h`, `server.cc`

**Blocker**: `RaftData::log_` is used in `Submit(shared_ptr<Marshallable>&)` which is a virtual function inherited from base `Coordinator` class. Changing this requires system-wide migration.

**Before (Original):**
```cpp
struct RaftData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};
  ballot_t term;
  shared_ptr<Marshallable> log_{nullptr};
  // ... other fields
};
```

**After** (ONE FIELD AT A TIME):
```cpp
#include <rusty/arc.hpp>

struct RaftData {
  ballot_t max_ballot_seen_ = 0;
  ballot_t max_ballot_accepted_ = 0;
  rusty::Arc<Marshallable> accepted_cmd_{};      // Step 3.1a
  shared_ptr<Marshallable> committed_cmd_{nullptr}; // Step 3.1b (next)
  ballot_t term;
  shared_ptr<Marshallable> log_{nullptr};        // Step 3.1c (last)
  // ... other fields
};
```

**Sub-steps**:
- **3.1a**: Migrate `accepted_cmd_` only
  - Change type
  - Update all reads: `accepted_cmd_->method()` stays same
  - Update all writes: `accepted_cmd_ = rusty::Arc<Marshallable>::new_(value)`
  - Test

- **3.1b**: Migrate `committed_cmd_`
  - Same process as 3.1a
  - Test

- **3.1c**: Migrate `log_`
  - Same process
  - Test

**Changes Required** (for each field):
1. Find all assignments to the field
2. Change `make_shared<X>(...)` → `rusty::Arc<X>::new_(X(...))`
3. Change `shared_ptr<X>` → `rusty::Arc<X>`
4. Check for `nullptr` checks: `if (ptr)` still works
5. Check for `.get()`, `.reset()` - Arc has different API

**Testing**: After EACH sub-step

**Success Criteria**:
- ✅ All 3 fields migrated
- ✅ Tests pass after each field
- ✅ No crashes or memory leaks

---

#### Step 3.2: Migrate Marshallable Parameters ⚠️ BLOCKED
**Files**: `server.h`, `server.cc`, `commo.h`, `commo.cc`, `coordinator.h`, `coordinator.cc`

**Blocker**: Same as Step 3.1 - all Marshallable pointers flow through the virtual `Submit()` interface.

**Scope**: Function parameters and return values

**Before (Original):**
```cpp
// server.h:178
bool Start(shared_ptr<Marshallable> &cmd, ...);

// server.h:186
void SetLocalAppend(shared_ptr<Marshallable>& cmd, ...);

// commo.h:68
shared_ptr<IntEvent> SendAppendEntries2(..., shared_ptr<Marshallable> cmd, ...);
```

**After**:
```cpp
// server.h:178
bool Start(rusty::Arc<Marshallable> &cmd, ...);

// server.h:186
void SetLocalAppend(rusty::Arc<Marshallable>& cmd, ...);

// commo.h:68
rusty::Arc<IntEvent> SendAppendEntries2(..., rusty::Arc<Marshallable> cmd, ...);
```

**Strategy**:
1. Identify all functions with `shared_ptr<Marshallable>` parameters
2. Create a list (use grep):
   ```bash
   grep -n "shared_ptr<Marshallable>" src/deptran/raft/*.h
   ```
3. Change ONE function at a time:
   - Update declaration
   - Update definition
   - Update all call sites
   - Test

**Testing**: After each function migration

**Warning**: This might require changes to callers outside Raft module
- Check if parent classes call these functions
- May need to keep compatibility shims temporarily

---

#### Step 3.3: Migrate IntEvent Smart Pointers ⚠️ BLOCKED
**Files**: `commo.h`, `commo.cc`, `server.h`

**Blocker**: `shared_ptr<IntEvent>` is created by `Reactor::CreateSpEvent<IntEvent>()` which is part of the RRR reactor framework API (outside Raft module). Cannot change without modifying reactor interface.

**Before (Original)**:
```cpp
shared_ptr<IntEvent> ready_for_replication_;

shared_ptr<IntEvent> SendAppendEntries2(...);
```

**After**:
```cpp
rusty::Arc<IntEvent> ready_for_replication_;

rusty::Arc<IntEvent> SendAppendEntries2(...);
```

**Note**: Check if IntEvent is defined in RRR framework
- RRR already uses RustyCpp (we saw `rusty::Arc<PollThreadWorker>`)
- IntEvent might already have RustyCpp support
- Check `src/rrr/` for IntEvent definition

**Testing**: Same as previous steps

---

#### Step 3.4: Migrate Log Storage Maps
**Files**: `server.h`, `server.cc`

**Before**:
```cpp
map<slotid_t, shared_ptr<RaftData>> logs_{};
map<slotid_t, shared_ptr<RaftData>> raft_logs_{};
```

**After** (Option A - Keep std::map, change value type):
```cpp
map<slotid_t, rusty::Arc<RaftData>> logs_{};
map<slotid_t, rusty::Arc<RaftData>> raft_logs_{};
```

**After** (Option B - Full migration):
```cpp
rusty::BTreeMap<slotid_t, rusty::Arc<RaftData>> logs_{};
rusty::BTreeMap<slotid_t, rusty::Arc<RaftData>> raft_logs_{};
```

**Recommendation**: Start with Option A (less risky)
- Migrate value type first (done in Step 3.1)
- Test thoroughly
- Later (Phase 4), optionally migrate to BTreeMap

**Changes Required** (if doing Option A):
1. Already done in Step 3.1 (RaftData uses Arc internally)
2. Change `GetInstance()` and `GetRaftInstance()` to use Arc
3. Update all map accesses

**Testing**: Standard

---

### Phase 4: Cleanup and Optimization (Week 4)
**Goal**: Code cleanup, remove technical debt

**Status**: Not Started
**Risk**: Low
**Estimated Time**: 3-5 days

#### Step 4.1: Remove Commented Code
**Files**: All Raft files

**Action**: Delete all commented-out code blocks

**Example** (from server.cc):
```cpp
// Lines 87-100 - Commented-out old setIsLeader implementation
// DELETE THIS

// Lines 243-250 - Commented-out GetRaftInstance
// DELETE THIS
```

**Process**:
1. Review each commented block
2. Verify it's truly obsolete
3. Delete it
4. Commit with message: "Remove obsolete commented code from [file]"

**Testing**: Code should compile and tests pass

---

#### Step 4.2: Migrate to BTreeMap (Optional)
**Files**: `server.h`, `server.cc`

**Before**:
```cpp
map<slotid_t, rusty::Arc<RaftData>> logs_{};
```

**After**:
```cpp
rusty::BTreeMap<slotid_t, rusty::Arc<RaftData>> logs_{};
```

**Rationale**:
- RustyCpp BTreeMap might have better safety guarantees
- Consistent with RustyCpp ecosystem
- Performance likely similar

**Risk**: Medium
- API might differ from std::map
- Iterator invalidation rules might differ
- Only do this if comfortable with BTreeMap API

**Testing**: Extensive testing required

---

#### Step 4.3: Replace std::recursive_mutex (Research)
**Files**: `server.h`, potentially others

**Current**:
```cpp
std::recursive_mutex mtx_;
```

**Issue**: RustyCpp Mutex is NOT recursive

**Options**:
1. **Keep std::recursive_mutex** (Recommended for now)
   - Least risky
   - Works fine
   - Revisit later

2. **Refactor to avoid recursion** (Future work)
   - Analyze why recursive mutex is needed
   - Refactor code to eliminate recursive locking
   - Use rusty::Mutex

3. **Implement recursive wrapper**
   - Wrap std::recursive_mutex in RustyCpp-compatible API
   - More work, uncertain benefit

**Recommendation**: Keep std::recursive_mutex for now, mark as TODO

---

### Phase 5: Enable Safety Checks (Week 5)
**Goal**: Add @safe annotations and enable borrow checking

**Status**: Not Started
**Risk**: High (Will surface new issues)
**Estimated Time**: 5-6 days (using Mark-All-Then-Fix approach) or 10-14 days (incremental)

**Quick Strategy**:
1. ⚡ **Mark all functions** as `@safe` (30-60 minutes)
2. 🔍 **Run borrow checker** to see all violations
3. 🔧 **Fix easy ones**, mark hard ones as `@unsafe` with reasons
4. ✅ **Iterate** until all `@safe` functions pass checker

**Key Insight**: Don't aim for 100% @safe. Goal is 50-70% @safe, rest @unsafe (documented).

#### Step 5.1: Add @safe to Simple Functions
**Files**: All Raft files

### Understanding @safe Annotations

**Three Safety States**:
1. **`@safe`** - Borrow checking enabled, strict calling rules
2. **`@unsafe`** - Explicitly unsafe, documented risks
3. **Undeclared** (default) - Legacy code, not checked

**Calling Rules**:
- `@safe` CAN call: @safe ✅, @unsafe ✅, undeclared ❌
- `@unsafe` CAN call: anything ✅
- `undeclared` CAN call: anything ✅

**What @safe Checks**:
- ✅ Borrow rules (no multiple mutable borrows)
- ✅ Ownership (no use-after-move)
- ✅ Pointer safety (no raw pointers)
- ✅ Lifetimes (no dangling references)

### Start with Simple Functions

**Example - Getters (easiest)**:
```cpp
// server.h
// @safe
bool IsLeader() const {
  return is_leader_;  // Simple read, no borrows
}

// @safe
uint64_t GetCurrentTerm() const {
  return currentTerm;  // Simple read
}
```

**Example - Functions with Locks**:
```cpp
// @safe
void GetState(bool *is_leader, uint64_t *term) {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  *is_leader = IsLeader();  // Calls @safe function - OK
  *term = currentTerm;
}
```

**Example - Functions That Need @unsafe**:
```cpp
// @unsafe - Uses raw pointer parameters
void SetTimerPointer(Timer* timer) {
  timer_ = timer;  // Raw pointer operation
}

// Better: Refactor to avoid raw pointers, then mark @safe
// @safe
void SetTimer(rusty::Box<Timer> timer) {
  timer_ = std::move(timer);
}
```

### Process

**Step 5.1a: Identify Safe Candidates**
```bash
# Find simple getter functions
grep -n "bool.*() const\|get.*() const" src/deptran/raft/*.h

# Find functions that don't use pointers
grep -v "\*" src/deptran/raft/*.h | grep "^\s*\w\+.*().*{"
```

**Step 5.1b: Add @safe to All Functions (Recommended Fast Approach)**

**Quick Script to Mark All Functions** (or do manually):
```bash
# This is a conceptual example - adapt as needed
cd src/deptran/raft

# For each header file, add @safe above function declarations
# Manual approach: Open each file in editor, add // @safe above each function

# OR use a simple script to add @safe above function definitions
# (Be careful with regex - review changes!)
for file in *.h *.cc; do
  # Add @safe before function definitions (simple regex example)
  # You'll need to review and adjust this
  sed -i 's/^\(\s*\)\(.*\s\+\w\+\s*(.*)\s*{.*\)/\1\/\/ @safe\n\1\2/' "$file"
done
```

**Manual Approach** (Safer, Recommended):
1. Open `server.h` in your editor
2. Before each public function, add `// @safe`
3. Save file
4. Repeat for `server.cc`, `coordinator.h`, `coordinator.cc`, etc.
5. Takes ~30-60 minutes to mark all functions in all files

**Then Run Borrow Checker and See All Issues**:
```bash
# Now see all violations at once
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc -- -I src -I third-party > violations.txt 2>&1

# Review the violations
less violations.txt

# Count how many violations
grep "^error:" violations.txt | wc -l
```

**Alternative: Incremental Approach** (Slower but More Conservative)
1. Start with 5-10 simple getters
2. Add `// @safe` comment above each
3. Build and run borrow checker (see Step 5.2)
4. Fix violations one at a time
5. Commit: "Add @safe to [function names]"
6. Repeat with next batch

**Step 5.1c: Document Why @unsafe**
```cpp
// @unsafe - Reason: Accesses raw pointer from parent class
void AccessFrameData() {
  frame_->site_id;  // frame_ is Frame* (parent class uses raw pointers)
}

// @unsafe - Reason: Performance-critical, uses manual memory management
void FastPathOptimization(char* buffer, size_t len) {
  // Direct buffer manipulation for speed
  memcpy(dest, buffer, len);
}
```

### Expected Violations and Fixes

**Violation 1: Multiple Mutable Borrows**
```cpp
// @safe - WRONG
void BadFunction() {
  int value = 42;
  int& ref1 = value;  // First mutable borrow
  int& ref2 = value;  // ERROR: Second mutable borrow!
}

// Fix: Use only one mutable borrow
// @safe
void GoodFunction() {
  int value = 42;
  int& ref = value;   // Only one mutable borrow
  ref = 100;
}
```

**Violation 2: Raw Pointer Operations**
```cpp
// @safe - WRONG
void BadPointer() {
  int x = 5;
  int* ptr = &x;  // ERROR: Raw pointer in @safe function
}

// Fix: Use references
// @safe
void GoodReference() {
  int x = 5;
  int& ref = x;  // OK: References are safe
}

// Or mark as unsafe if truly needed
// @unsafe - Uses raw pointers for FFI
void ExternalAPI(int* ptr) {
  external_c_function(ptr);
}
```

**Violation 3: Calling Undeclared Functions**
```cpp
// Assume helper() is not annotated (undeclared)
void helper() { /* ... */ }

// @safe - WRONG
void caller() {
  helper();  // ERROR: @safe cannot call undeclared
}

// Fix 1: Mark helper as @safe
// @safe
void helper() { /* ... */ }

// @safe
void caller() {
  helper();  // OK now
}

// Fix 2: Mark helper as @unsafe if it does unsafe things
// @unsafe
void helper() { /* uses pointers */ }

// @safe
void caller() {
  helper();  // OK: @safe can call @unsafe
}
```

### Migration Strategy

**Recommended: Mark-All-Then-Fix Approach** (Fastest):
1. **Mark ALL functions as @safe** (batch operation, 5 minutes)
2. **Run borrow checker** (will show all violations)
3. **Fix violations one by one** OR mark specific functions @unsafe
4. **Re-run checker, repeat until clean**

**Why This Works Better**:
- ✅ Reveals all issues upfront (no surprises later)
- ✅ You can see the full scope immediately
- ✅ Easier to prioritize what to fix vs mark @unsafe
- ✅ Faster iteration (batch mark, then fix)

**Alternative: Bottom-Up Approach** (More Conservative):
1. Start with leaf functions (no dependencies)
2. Mark them @safe
3. Work up the call graph
4. Functions that call @safe functions become @safe

**Example of Mark-All-Then-Fix**:
```bash
# Step 1: Add @safe to all functions (automated)
# Use a script or do manually

# Step 2: Run borrow checker
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc -- -I src -I third-party

# Output shows 47 violations

# Step 3: Fix easy ones (pure functions, getters)
# Keep complex ones as @unsafe with reasons

# Step 4: Re-run checker
# Output shows 12 violations (progress!)

# Step 5: Repeat until acceptable
```

**Example Order for Bottom-Up** (if you prefer conservative):
```cpp
// Step 1: Mark leaf functions
// @safe
bool IsLeader() const { return is_leader_; }

// @safe
uint64_t GetTerm() const { return currentTerm; }

// Step 2: Mark functions that call @safe functions
// @safe
void GetState(bool* leader, uint64_t* term) {
  *leader = IsLeader();   // Calls @safe - OK
  *term = GetTerm();      // Calls @safe - OK
}

// Step 3: Complex functions
// @safe
void ProcessVote() {
  if (IsLeader()) {      // Calls @safe - OK
    // ... more logic
  }
}
```

### Workflow Summary

**Fast Track (Mark-All-Then-Fix)**:
```
Day 1: Mark all functions as @safe (1 hour)
Day 1: Run borrow checker, save violations (10 min)
Day 2-5: Fix easy violations, mark hard ones @unsafe (4 days)
Day 6: Re-run checker, verify all @safe functions pass
Total: ~5-6 days
```

**Conservative Track (Incremental)**:
```
Day 1: Mark 10 simple functions @safe, fix violations
Day 2: Mark 10 more functions @safe, fix violations
...
Day 10-14: All functions either @safe or @unsafe
Total: ~10-14 days
```

### Warning
- Borrow checker WILL find issues (that's the point!)
- **Don't aim for 100% @safe** - it's OK to have @unsafe functions
- Goal: ~50-70% @safe, rest @unsafe with documented reasons
- Some functions may need to stay @unsafe forever (document why)

---

#### Step 5.2: Run Borrow Checker

### How to Enable Borrow Checking

The RustyCpp borrow checker is a **standalone static analyzer** located in `third-party/rusty-cpp/`.

**Option 1: Using CMake (if integrated)**
```bash
# Check if borrow checking is available in CMake
grep -r "ENABLE_BORROW_CHECKING\|borrow_check" CMakeLists.txt

# If available:
cmake -B build -DENABLE_BORROW_CHECKING=ON
cmake --build build -j32
```

**Option 2: Run rusty-cpp-checker Directly**
```bash
# Check if the checker binary exists
ls third-party/rusty-cpp/target/release/rusty-cpp-checker
# or
ls third-party/rusty-cpp/dist/cpp-borrow-checker-*/cpp-borrow-checker

# Run on a specific file
third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party

# Run on all Raft files
for file in src/deptran/raft/*.cc; do
  echo "Checking $file..."
  third-party/rusty-cpp/target/release/rusty-cpp-checker "$file" \
    -- -I src -I third-party
done
```

**Option 3: Build the Checker if Not Available**
```bash
cd third-party/rusty-cpp
cargo build --release
cd ../../..

# Now run it
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party
```

### Understanding Borrow Checker Output

**Example Error - Multiple Mutable Borrows**:
```
error: cannot borrow `value` as mutable because it is also borrowed as mutable
  --> src/deptran/raft/server.cc:123:5
   |
122 |     int& ref1 = value;
   |                 ----- first mutable borrow occurs here
123 |     int& ref2 = value;
   |          ^^^^ second mutable borrow occurs here
124 |     ref1 = 10;
   |     ----- first borrow later used here
```

**Example Error - Use After Move**:
```
error: use of moved value: `cmd_`
  --> src/deptran/raft/coordinator.cc:45:10
   |
43  |     auto cmd2 = std::move(cmd_);
   |                 --------------- value moved here
44  |     Submit(cmd2);
45  |     cmd_->Execute();
   |          ^^^ value used here after move
```

**Example Error - Raw Pointer in @safe Function**:
```
error: raw pointer operation not allowed in @safe function
  --> src/deptran/raft/server.cc:67:5
   |
67  |     Timer* t = &timer;
   |          ^ raw pointer creation requires @unsafe context
   |
help: mark function as @unsafe or use a reference instead
```

### Process for Fixing Violations

**Step 1: Run Checker on One File**
```bash
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party > borrow_check_results.txt 2>&1

# Review errors
cat borrow_check_results.txt
```

**Step 2: Fix Highest Priority Violations First**
1. Raw pointer operations in @safe functions (easy to fix or mark @unsafe)
2. Use-after-move (usually indicates actual bug)
3. Multiple mutable borrows (may require refactoring)
4. Lifetime violations (harder to fix)

**Step 3: Fix Violations OR Mark as @unsafe**

For each violation, decide:
- **Option A**: Fix it (if simple)
- **Option B**: Change `@safe` to `@unsafe` with a reason comment

```bash
# Example: Violation in IsLeader()
# Option A: Fix the code to be safe
vim src/deptran/raft/server.cc
# (make the fix)

# Option B: Mark as @unsafe if can't easily fix
# Change from:
#   // @safe
#   void UseFramePointer() { ... }
# To:
#   // @unsafe - Uses raw pointer member frame_ (parent class dependency)
#   void UseFramePointer() { ... }

# Re-run checker
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/server.cc \
  -- -I src -I third-party

# Repeat until acceptable (doesn't have to be zero violations)
# Goal: All @safe functions pass, @unsafe functions documented
```

**Step 4: Move to Next File**
```bash
# server.cc clean? Move to commo.cc
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
  src/deptran/raft/commo.cc \
  -- -I src -I third-party
```

### Expected Issues

**Issue 1: Undeclared Function Calls**
```cpp
// RaftServer calls parent class methods
void RaftServer::SomeMethod() {
  this->OnExecute(...);  // OnExecute() is in TxLogServer (parent)
}
```

**Solution**: Mark parent class methods as @safe or @unsafe
- Either migrate parent class (out of scope for now)
- Or mark RaftServer methods as @unsafe when they call parent
```cpp
// @unsafe - Calls parent class methods that are not yet annotated
void RaftServer::SomeMethod() {
  this->OnExecute(...);
}
```

**Issue 2: Raw Pointer Members**
```cpp
class RaftServer {
  Frame* frame_;  // Raw pointer member

  // @safe - WRONG
  void UseFrame() {
    frame_->site_id;  // Uses raw pointer!
  }
}
```

**Solution**: Mark as @unsafe until Frame is migrated
```cpp
  // @unsafe - Uses raw pointer member frame_ (parent class dependency)
  void UseFrame() {
    frame_->site_id;
  }
```

**Issue 3: Mutex and Locks**
```cpp
// @safe
void LockedFunction() {
  std::lock_guard<std::recursive_mutex> lock(mtx_);
  // ... work with protected data
}
```

**Status**: std::recursive_mutex is likely **undeclared**
- RustyCpp has `rusty::Mutex` but it's not recursive
- May need to mark functions using std::recursive_mutex as @unsafe temporarily

### Tracking Progress

**Create a checklist**:
```bash
# Count total @safe annotations
grep -r "// @safe" src/deptran/raft/ | wc -l

# Count total @unsafe annotations
grep -r "// @unsafe" src/deptran/raft/ | wc -l

# Count functions without annotations
# (Harder - need to count all functions minus annotated ones)
```

**Goal**:
- Start: 0% @safe
- Phase 5 end: >50% @safe, rest either @unsafe (documented) or being worked on

### Success Criteria
- ✅ Borrow checker runs without errors on @safe functions
- ✅ All @unsafe annotations have comments explaining why
- ✅ At least 50% of functions marked @safe or @unsafe
- ✅ No silent failures or ignored errors

---

#### Step 5.3: Mark Complex Functions @safe
**Goal**: Gradually expand @safe coverage

**Strategy**:
1. Start with functions that only call @safe functions
2. Work bottom-up (leaf functions first)
3. Mark @unsafe for functions that truly need it
4. Document why each @unsafe is needed

**Example**:
```cpp
// @unsafe - Uses raw pointer arithmetic for performance
void ApplyBatch(char* buffer, size_t len) {
  // Pointer arithmetic here
}
```

---

### Phase 6: Integration and Testing (Week 6)
**Goal**: Comprehensive testing and performance validation

**Status**: Not Started
**Risk**: Low (if previous phases passed)
**Estimated Time**: 5-7 days

#### Step 6.1: Run Full Test Suite
**Commands**:
```bash
# Build for both lab and production testing
cmake -B build -DRAFT_TEST=ON
cmake --build build -j32

# Run Raft lab tests
./build/deptran_server -f config/raft_lab_test.yml

# Rebuild for production testing
cmake -B build -DRAFT_TEST=OFF
cmake --build build -j32

# Run basic Raft test
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -d 30 -m 100 -P localhost

# Run high concurrency test (stress test)
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -m 100 -P localhost

# Run Raft with Jetpack failover
./build/deptran_server \
  -f config/rule_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -f config/failover.yml \
  -d 30 -m 100 -P localhost
```

**Success Criteria**:
- ✅ Lab tests complete without crashes
- ✅ Production tests run for full 30 seconds
- ✅ Throughput reported in logs
- ✅ No election storms or term mismatches
- ✅ No new warnings or errors

---

#### Step 6.2: Memory Leak Testing (if tools available)
**Commands**:
```bash
# Valgrind
valgrind --leak-check=full ./build/raft_test

# AddressSanitizer (if built with -fsanitize=address)
ASAN_OPTIONS=detect_leaks=1 ./build/raft_test
```

**Success Criteria**:
- ✅ No memory leaks
- ✅ No use-after-free
- ✅ No double-free

---

#### Step 6.3: Performance Benchmarking
**Goal**: Ensure no performance regression

**Baseline** (before migration):
```bash
# Run benchmark and save results (using high concurrency for better metrics)
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -m 100 -P localhost > baseline.txt 2>&1

# Extract throughput from output
grep -i "throughput\|tps\|latency" baseline.txt
```

**After migration**:
```bash
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -m 100 -P localhost > migrated.txt 2>&1

# Compare metrics
grep -i "throughput\|tps\|latency" migrated.txt
diff baseline.txt migrated.txt
```

**Expected**:
- RustyCpp has **zero runtime overhead** (compile-time only)
- Performance should be identical
- If slower, investigate why

**Success Criteria**:
- ✅ Throughput within 5% of baseline
- ✅ Latency within 5% of baseline
- ✅ No performance regression

---

## Testing Strategy

### Test Levels

#### Level 1: Compilation
**After every change**:
```bash
rm -rf build && mkdir build
cmake -B build -DRAFT_TEST=OFF
cmake --build build -j32
```
**Success**: Code compiles without errors or warnings

#### Level 2: Raft Lab Tests
```bash
cmake -B build -DRAFT_TEST=ON
cmake --build build -j32
./build/deptran_server -f config/raft_lab_test.yml
```
**Success**: Lab tests complete without crashes

#### Level 3: Production Raft Tests
```bash
# Basic 3-replica test (30 second run)
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -d 30 -m 100 -P localhost

# High concurrency test (stress test)
./build/deptran_server \
  -f config/none_raft.yml \
  -f config/12c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_12.yml \
  -d 30 -m 100 -P localhost
```
**Success**: Tests complete, check throughput/latency in output

#### Level 4: Raft with Jetpack (Failure Recovery)
```bash
./build/deptran_server \
  -f config/rule_raft.yml \
  -f config/1c1s3r1p.yml \
  -f config/rw.yml \
  -f config/client_closed.yml \
  -f config/concurrent_1.yml \
  -f config/failover.yml \
  -d 30 -m 100 -P localhost
```
**Success**: Failover scenarios handled correctly

### Test Frequency
- **After each field change**: Level 1 + Level 2
- **After each function change**: Level 1 + Level 2 + Level 3
- **After each phase**: All levels
- **Before merging**: All levels + performance benchmarks

### Regression Detection
- Keep test output logs
- Compare before/after for each phase
- Flag any new failures immediately

---

## Rollback Plan

### Git Strategy
```bash
# Before each phase, create a branch
git checkout -b rustycpp-phase-1
# Make changes
git add .
git commit -m "Phase 1.1: Migrate Timer to Box"
# Test
# If tests fail:
git revert HEAD
# Or if multiple commits:
git reset --hard origin/main
```

### Rollback Triggers
- Tests fail after multiple fix attempts (>2 hours stuck)
- Performance regression >10%
- New crashes or memory corruption
- Borrow checker finds unfixable violations

### Safe Points
After each phase completion:
```bash
git tag rustycpp-phase-N-complete
git push origin rustycpp-phase-N-complete
```

Can always return to last safe point:
```bash
git checkout rustycpp-phase-N-complete
```

---

## Success Criteria

### Phase Completion Criteria
- ✅ All files compile without errors
- ✅ All tests pass
- ✅ No new memory leaks (valgrind clean)
- ✅ Code review approved
- ✅ Documentation updated

### Overall Migration Success
- ✅ All 6 phases completed
- ✅ Zero memory leaks in Raft module
- ✅ Borrow checker enabled on all @safe functions
- ✅ Performance within 5% of baseline
- ✅ Full test suite passes
- ✅ Code coverage maintained or improved

### Definition of Done
1. All raw pointers replaced with RustyCpp smart pointers OR documented as intentional
2. All std::shared_ptr replaced with rusty::Arc
3. Memory leak from Timer fixed
4. RaftFrame properly manages owned resources
5. @safe annotations on >50% of functions
6. Borrow checker runs without errors on @safe code
7. All tests passing
8. Performance validated
9. Documentation complete
10. Code reviewed and merged to main branch

---

## Risk Management

### Identified Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| API incompatibility with Arc | Medium | High | Test each function migration individually |
| Performance regression | Low | High | Benchmark after each phase |
| Borrow checker finds unfixable issues | Medium | Medium | Mark problematic code @unsafe with justification |
| Parent class compatibility | High | Medium | Keep raw pointer interfaces for compatibility |
| Timer migration breaks timing | Low | High | Extensive testing of election/heartbeat |
| Test infrastructure broken | Medium | High | Fix tests before migration, ensure they pass |

### Contingency Plans
- If Arc migration fails: Fall back to std::shared_ptr, focus on Box for leaks
- If borrow checker too strict: Mark more code @unsafe, revisit later
- If performance regresses: Profile and optimize hot paths
- If blocked by parent classes: Migrate parent classes first (escalate)

---

## Dependencies and Blockers

### Upstream Dependencies
- ✅ RustyCpp library available in `third-party/rusty-cpp/`
- ✅ RRR framework already uses `rusty::Arc<PollThreadWorker>`
- ⚠️ Parent classes (TxLogServer, Coordinator, Communicator) use raw pointers
  - **Decision**: Keep raw pointer interfaces for now, migrate internals only

### Downstream Impact
- CoordinatorRaft holds `RaftServer*` - verify this still works
- RPC service handlers in service.cc - verify compatibility
- Test code might need updates

### Blockers
- None identified yet
- If tests are broken before migration, fix them first
- If build system doesn't support RustyCpp includes, fix that first

---

## Progress Tracking

### Checklist

#### Phase 1: Fix Memory Leaks
- [ ] 1.1: Migrate Timer to Box
- [ ] 1.2: Add RaftFrame destructor
- [ ] Phase 1 tests passed
- [ ] Phase 1 code review

#### Phase 2: Migrate Frame Ownership
- [ ] 2.1: Migrate RaftCommo to Box
- [ ] 2.2: Migrate RaftServer to Box
- [ ] 2.3: Document ownership
- [ ] Phase 2 tests passed
- [ ] Phase 2 code review

#### Phase 3: Migrate Data Structures
- [ ] 3.1: Migrate RaftData (3 fields)
- [ ] 3.2: Migrate Marshallable parameters
- [ ] 3.3: Migrate IntEvent
- [ ] 3.4: Migrate log storage
- [ ] Phase 3 tests passed
- [ ] Phase 3 code review

#### Phase 4: Cleanup
- [ ] 4.1: Remove commented code
- [ ] 4.2: (Optional) Migrate to BTreeMap
- [ ] 4.3: Research recursive mutex
- [ ] Phase 4 tests passed

#### Phase 5: Enable Safety
- [ ] 5.1: Add @safe to simple functions
- [ ] 5.2: Run borrow checker
- [ ] 5.3: Mark complex functions @safe
- [ ] Phase 5 tests passed

#### Phase 6: Integration
- [ ] 6.1: Full test suite
- [ ] 6.2: Memory leak testing
- [ ] 6.3: Performance benchmarking
- [ ] Final code review
- [ ] Merge to main

---

## Appendix

### Useful Commands

#### Finding Usage Patterns
```bash
# Find all Timer* usage
grep -rn "timer_" src/deptran/raft/

# Find all shared_ptr<Marshallable>
grep -rn "shared_ptr<Marshallable>" src/deptran/raft/

# Find all new/delete
grep -rn "new \|delete " src/deptran/raft/

# Find all raw pointer members
grep -n "\*.*;" src/deptran/raft/*.h
```

#### Building with Warnings
```bash
make clean
CXXFLAGS="-Wall -Wextra -Wpedantic" make -j32
```

#### Memory Leak Detection
```bash
valgrind --leak-check=full --show-leak-kinds=all ./build/raft_test
```

### Resources
- RustyCpp documentation: `third-party/rusty-cpp/README.md`
- Project migration guide: `CLAUDE.md`
- RustyCpp examples: `third-party/rusty-cpp/examples/`
- RRR framework (already migrated): `src/rrr/`

### Contact
- Questions: [Add contact info]
- Code review: [Add reviewers]
- Issues: Track in [issue tracker]

---

## Change Log

| Date | Author | Changes |
|------|--------|---------|
| 2025-10-24 | Claude Code | Initial plan created |
|  |  |  |

---

**End of Migration Plan**
