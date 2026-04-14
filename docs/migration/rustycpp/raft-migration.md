# RustyCpp Safe Code Migration - Raft Module

## Goal
Mark each function in the raft module (`src/deptran/raft/`) with `// @safe` or `// @unsafe` annotations to enable borrow checking and track memory safety coverage.

## Quick Reference

### Annotation Syntax
```cpp
// @safe
void safe_function() { ... }

// @unsafe
void unsafe_function() { ... }
```

### What Makes Code Unsafe
A function must be marked `// @unsafe` if it:
1. Uses raw pointers (`int*`, `void*`, `T*`)
2. Calls undeclared/unannotated functions
3. Uses C-style casts to pointers: `(RaftProxy*) p.second`
4. Takes raw pointer parameters
5. Uses `&` to get address of a variable (not references)

### What Can Be Safe
- Functions using only references (`const T&`, `T&`)
- Functions using RustyCpp types (`rusty::Box`, `rusty::Arc`, `rusty::Vec`)
- Functions using `std::shared_ptr`, `std::unique_ptr`
- Functions calling other `@safe` or `@unsafe` functions

## Files in Raft Module
```
src/deptran/raft/
├── commo.cc / commo.h         # RPC communication
├── server.cc / server.h       # Raft server logic
├── coordinator.cc / coordinator.h
├── frame.cc / frame.h
├── service.cc / service.h
├── exec.cc / exec.h
├── raft_worker.cc / raft_worker.h
├── test.cc / test.h
├── testconf.cc / testconf.h
└── macros.h
```

## Process for Each File

1. **Read the header (.h) first** - annotations should be in headers
2. **Annotate each function** with `// @safe` or `// @unsafe`
3. **Document unsafe reasons** - add brief comment: `// @unsafe: raw pointer cast`
4. **Count lines** after completing each file

## Counting Safe vs Unsafe Lines

For each file, count:
- **Safe lines**: Lines inside `@safe` functions (excluding blank/comment-only)
- **Unsafe lines**: Lines inside `@unsafe` functions
- **Unannotated lines**: Lines in functions without annotations

Report format:
```
file.cc: 45 safe / 20 unsafe / 0 unannotated = 69% safe
```

## Current State (commo.cc)

Already annotated:
- `RaftCommo::RaftCommo()` - @safe
- `SendAppendEntries2()` - @safe (comment notes undeclared calls)
- `SendAppendEntries()` - @safe
- `BroadcastVote()` - @safe (comment notes undeclared calls)
- `SendTimeoutNow()` - NOT annotated yet

Header (commo.h):
- `RaftVoteQuorumEvent` methods - @safe
- `SendAppendEntries2` declaration - @unsafe
- `BroadcastVote` declaration - @unsafe

## Running the Checker
```bash
./third-party/rusty-cpp/target/release/rusty-cpp-checker \
    -I src -I src/rrr -I third-party/rusty-cpp/include \
    src/deptran/raft/commo.cc
```

## Notes
- Header annotations override implementation annotations
- Start with obvious @unsafe (raw pointers), then work toward @safe
- Some functions may need refactoring to become @safe (future work)
