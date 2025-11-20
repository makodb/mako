# Masstree RustyCpp Memory Safety Migration

## Overview
This document outlines the plan to migrate the Masstree B+ tree library to use RustyCpp borrow checking.

---

## Quick Start

### Build and Test
```bash
# Build tests
mkdir -p build && cd build
cmake ..
make test_masstree perf_masstree_string

# Run functional tests
./test_masstree

# Run performance tests (baseline)
./perf_masstree_string 1000000
```

### Run Borrow Checker
```bash
# Check all Masstree files
make borrow_check_masstree

# Check specific test
make borrow_check_all_test_masstree
```

### Migration Workflow
1. **Baseline performance** before changes
2. **Annotate one file** with @safe/@unsafe
3. **Run tests** to verify correctness
4. **Run borrow checker** to catch issues
5. **Measure performance** to detect regressions
6. **Commit** with descriptive message
7. Repeat for next file

---

## Phase 1: Infrastructure Setup ✅ COMPLETE

- ✅ RustyCpp is already integrated in CMake
- ✅ Borrow checking infrastructure exists
- ✅ Test harness available


---

## Phase 2: Tests and Baseline (Current Phase)

### 2.1 Create Masstree Functional Tests
**Priority**: CRITICAL - Need tests before migration

Tests created:
- [x] `test_masstree.cc` - Consolidated test suite (string infrastructure + tree operations) **✅ WORKING**

Location: `test/masstree/` directory

**Status**: 
- ✅ String tests are complete and passing (24/24 tests pass)
- ⚠️ Tree operation tests are commented-out templates (use actual Masstree query_masstree.hh API in Phase 5)
- ⚠️ See mttest.cc for reference on Masstree table operations
- ✅ All tests consolidated into single file for easier maintenance

**Note**: The tree operation tests (basic, concurrent, scan) are currently commented-out placeholder templates in the same file. They need to be updated to use the actual Masstree API once we start Phase 5 (Core Tree Structure). For now, focus on Phase 3 (String Infrastructure) which has working tests.

### 2.2 Create Performance Baseline
**Priority**: HIGH - Measure impact of annotations

Performance tests to create:
- [ ] `perf_masstree_insert.cc` - Insertion throughput
- [ ] `perf_masstree_lookup.cc` - Lookup latency
- [ ] `perf_masstree_mixed.cc` - Mixed workload
- [ ] `perf_masstree_concurrent.cc` - Multi-threaded performance

Location: `test/masstree/perf/` directory

**Metrics to track**:
- Operations per second
- Latency (p50, p99, p999)
- Memory usage
- CPU utilization

**Target**: < 5% performance regression from annotations

---

## Phase 3: String Infrastructure

**Priority**: CRITICAL - Foundation for everything else  
**Files**: 10 files in `src/mako/masstree/`

### Migration Order:

#### 3.1 Basic String Types
| File | Lines | Status |
|------|-------|--------|
| `str.hh` | ~160 | ✅ COMPLETE |
| `str.cc` | ~60 | ✅ COMPLETE |
| `string_base.hh` | ~300 | ✅ COMPLETE |
| `string.hh` | ~400 | ✅ COMPLETE |
| `string.cc` | ~1400 | ✅ COMPLETE |

**Key Challenges**:
- Manual memory management in String class
- Reference counting without RefCell
- Shared substring lifetime management

**Migration Strategy**:
- Mark simple accessors as `@safe`
- Mark memory allocation/deallocation as `@unsafe` with documentation
- Use `@unsafe` for reinterpret_cast and const_cast operations
- Add SAFETY: comments explaining invariants

**Example Annotation Pattern** (from RRR migration):
```cpp
// @safe - Lexicographically compares two strings
// SAFETY: Operates only on const char arrays with explicit lengths
int String_generic::compare(const char* a, int a_len, const char* b, int b_len) {
    // ...
}

// @unsafe - Allocates raw memory for string storage
// SAFETY: Uses reinterpret_cast, caller must manage lifetime
String::memo_type* String::create_memo(int capacity) {
    // @unsafe
    return reinterpret_cast<memo_type*>(new char[capacity]);
}
```

#### 3.2 String Utilities
| File | Lines |
|------|-------|
| `straccum.hh` | ~200 |
| `straccum.cc` | ~600 |
| `string_slice.hh` | ~100 |
| `string_slice.cc` | ~200 |
| `stringbag.hh` | ~150 |

**Success Criteria**:
- [ ] All string files compile with borrow checking enabled
- [ ] `test_masstree` passes (all string tests passing)
- [ ] No memory leaks detected
- [ ] Performance within 2% of baseline

---

## Phase 4: Core Utilities

**Priority**: HIGH - Required by tree structures  
**Files**: 10 files

### Migration Order:

#### 4.1 Compiler and System Utilities
| File | Lines |
|------|-------|
| `compiler.hh` | ~300 |
| `compiler.cc` | ~100 |
| `misc.hh` | ~200 |
| `misc.cc` | ~300 |
| `circular_int.hh` | ~100 |
| `timestamp.hh` | ~150 |

**Key Challenges**:
- Compiler intrinsics and platform-specific code
- likely/unlikely macros
- Atomic operations

**Migration Strategy**:
- Mark compiler hints as `@unsafe` (they use __builtin functions)
- Simple utility functions can be `@safe`
- Atomic operations need careful review

#### 4.2 Memory and Thread Management
| File | Lines |
|------|-------|
| `kvthread.hh` | ~200 |
| `kvthread.cc` | ~400 |
| `memdebug.hh` | ~100 |
| `memdebug.cc` | ~200 |

**Key Challenges**:
- Thread-local storage
- Per-thread allocators
- RCU integration
- Memory debugging hooks

---

## Phase 5: Core Tree Structure

**Priority**: CRITICAL - Main Masstree functionality  
**Files**: 15 files

### Migration Order:

#### 5.1 Version and Permission Management
| File | Lines |
|------|-------|
| `nodeversion.hh` | ~300 |
| `kpermuter.hh` | ~400 |
| `ksearch.hh` | ~300 |

**Key Challenges**:
- Lock-free version counters
- Atomic operations with memory ordering
- Bit manipulation for permutation arrays
- SIMD operations for key search

#### 5.2 Node Structure
| File | Lines |
|------|-------|
| `masstree_struct.hh` | ~800 |
| `masstree_key.hh` | ~400 |
| `btree_leaflink.hh` | ~150 |

**Key Challenges**:
- Node pointer ownership model
- Parent/child pointer safety
- Lock-free node access
- Concurrent modification protocols

#### 5.3 Tree Operations
| File | Lines |
|------|-------|
| `masstree.hh` | ~300 |
| `masstree_tcursor.hh` | ~400 |
| `masstree_get.hh` | ~200 |
| `masstree_insert.hh` | ~600 |
| `masstree_remove.hh` | ~500 |
| `masstree_scan.hh` | ~400 |
| `masstree_split.hh` | ~300 |
| `masstree_print.hh` | ~200 |

**Key Challenges**:
- Cursor lifetime management
- Optimistic concurrency control
- Version validation
- Lock acquisition ordering
- Node split/merge safety

---

## Phase 6: Value Types

**Priority**: MEDIUM  
**Files**: 10 files

| File | Lines |
|------|-------|
| `value_array.hh/cc` | ~300 |
| `value_bag.hh` | ~150 |
| `value_string.hh/cc` | ~200 |
| `value_versioned_array.hh/cc` | ~400 |
| `small_vector.hh` | ~200 |
| `kvrow.hh` | ~300 |
| `kvstats.hh` | ~100 |

---

## Phase 7: I/O and Serialization

**Priority**: MEDIUM  
**Files**: 10 files

| File | Lines |
|------|-------|
| `json.hh/cc` | ~1000 |
| `msgpack.hh/cc` | ~800 |
| `kvio.hh/cc` | ~500 |
| `checkpoint.hh/cc` | ~400 |
| `kvrandom.hh/cc` | ~200 |
| `perfstat.hh/cc` | ~300 |

---

## Implementation Guidelines

### Annotation Rules (from RustyCpp README)

1. **Mark functions explicitly**:
   - `@safe` - Full memory safety, can call other @safe or @unsafe functions
   - `@unsafe` - Explicitly documented risks, can call anything
   - Undeclared - Legacy code, cannot be called from @safe functions

2. **Calling Rules**:
   - `@safe` can call: `@safe` ✅, `@unsafe` ✅, undeclared ❌
   - `@unsafe` can call: anything ✅
   - Undeclared can call: anything ✅

3. **Use SAFETY: comments**:
```cpp
// @safe - Brief description of what the function does
// SAFETY: Explanation of why it's safe (bounds checking, no raw pointers, etc.)
```

4. **Common @unsafe operations**:
   - `reinterpret_cast`
   - `const_cast`
   - Raw pointer arithmetic
   - Manual memory management (new/delete)
   - Type punning
   - System calls
   - Lock-free operations
   - Thread-local storage access

5. **Use Rusty structures when possible**:
   - Prefer `rusty::Vec<T>` over `std::vector<T>` in new code
   - Use `rusty::Box<T>` for ownership
   - Use `rusty::Arc<T>` for shared ownership

### Testing Strategy

1. **Test after each file**:
   - Run unit tests
   - Run borrow checker
   - Check for memory leaks
   - Measure performance

2. **Incremental validation**:
   - Don't move to next file until current file passes
   - Keep a "known good" state in git

3. **Performance tracking**:
   - Baseline before any changes
   - Measure after each phase
   - Document any regressions

### Git Workflow

1. **One PR per file** (for complex files):
   - Title: `feat(masstree): Add @safe/@unsafe annotations to <filename>`
   - Description: List all changes, justify @unsafe blocks

2. **One PR per group** (for simple files):
   - Group related files (e.g., all string slice files)

3. **Commit message format**:
```
feat(masstree): Annotate <filename> with safety markers

- Mark <N> functions as @safe
- Mark <M> functions as @unsafe (list reasons)
- Add SAFETY: comments for all functions
- Tests: <test results>
- Performance: <impact>
```

---

## Success Criteria

### Per-Phase
- [ ] All files in phase compile with borrow checking
- [ ] All tests pass
- [ ] No memory leaks (Valgrind)
- [ ] Performance within target (<2% per phase, <5% total)

### Overall Project
- [ ] All 77 Masstree files annotated
- [ ] Borrow checker enabled for entire Masstree module
- [ ] Full test suite passes
- [ ] Performance regression < 5%
- [ ] No data races (ThreadSanitizer)
- [ ] Documentation complete

---

## Current Status

**Phase**: 3 (String Infrastructure) - IN PROGRESS  
**Next**: Continue with remaining string files

**Files completed**: 5/77 (6.5%) 🎉

### Recent Completions:
- ✅ **str.hh**: Annotated 20+ functions with @safe/@unsafe
  - All constructors marked with appropriate safety levels
  - reinterpret_cast operations marked @unsafe with justification
  - const_cast in mutable_data() marked @unsafe
- ✅ **str.cc**: Annotated static constant with @safe
  - Single static maxkey constant properly documented
- ✅ **string_base.hh**: Annotated 80+ functions and template methods with @safe/@unsafe
  - String_generic static utility functions annotated
  - String_base CRTP template class methods annotated
  - All comparison, search, and encoding functions marked appropriately
  - reinterpret_cast operations marked @unsafe with justification
- ✅ **string.hh / string.cc**: Annotated owning String implementation
  - Constructors, factories, substring helpers, and encoding utilities marked @safe/@unsafe
  - Mutation paths (assign/append/mutable_data) documented with SAFETY notes
  - Borrow checker enabled for these files

---



---

*Status*: Phase 3 in progress

