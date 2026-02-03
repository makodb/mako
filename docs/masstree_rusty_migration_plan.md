# Masstree RustyCpp Migration Plan

## Overview

This document outlines an incremental plan to migrate the Masstree codebase (~28,782 lines across 78 files) to be rusty-safe. The migration follows a conservative approach with small, reviewable changes.

## Migration Philosophy

1. **Incremental**: Each task changes only a few functions/files
2. **Non-breaking**: No behavioral changes - only safety annotations and type wrappers
3. **Testable**: Each phase can be verified independently
4. **Reversible**: Changes can be rolled back if issues arise

## Priority Order (by file importance to Mako)

### Tier 1: Core Context & Thread Management (smallest, most foundational)
- `masstree_context.h/cc` (~100 lines) - Already partially migrated
- `kvthread.hh/cc` (~400 lines) - Thread-local memory management

### Tier 2: Core B-tree Operations
- `masstree.hh` (~150 lines) - Main table interface
- `masstree_get.hh` (~130 lines) - Point lookups
- `masstree_insert.hh` (~185 lines) - Insertions
- `masstree_scan.hh` (~410 lines) - Range scans
- `masstree_remove.hh` (~375 lines) - Deletions

### Tier 3: Node Structures
- `masstree_struct.hh` (~850 lines) - Node definitions

### Tier 4: Value Types
- `kvrow.hh` (~200 lines) - Row management
- `value_versioned_array.hh/cc` (~400 lines) - MVCC support

### Tier 5: Utilities (lower priority, heavy third-party interaction)
- `string.hh/cc` (~2300 lines) - Reference-counted strings
- `json.hh/cc` (~4460 lines) - JSON handling
- `msgpack.hh/cc` (~1000 lines) - Binary serialization

---

## Phase 1: Audit & Annotate Safe Functions

**Goal**: Identify functions that are already safe (no raw pointer manipulation, no unsafe operations) and mark them with `// @safe`.

### Task 1.1: Audit masstree_context.h/cc
- File: `src/mako/masstree/masstree_context.h`
- File: `src/mako/masstree/masstree_context.cc`
- Actions:
  - Review each function for safety
  - Mark pure getters/setters as `// @safe`
  - Mark functions with raw pointer ops as `// @unsafe`
  - Add safety comments explaining why

### Task 1.2: Audit kvthread.hh - Public Interface
- File: `src/mako/masstree/kvthread.hh`
- Actions:
  - Mark simple accessors as `// @safe` (purpose(), index(), context())
  - Mark memory allocation functions as `// @unsafe`
  - Document each unsafe operation

### Task 1.3: Audit masstree.hh - Table Interface
- File: `src/mako/masstree/masstree.hh`
- Actions:
  - Mark const getters as `// @safe`
  - Mark tree modification methods as `// @unsafe`
  - Document lock-free operations

### Task 1.4: Audit masstree_get.hh
- File: `src/mako/masstree/masstree_get.hh`
- Actions:
  - Mark functions with only const reference params as potentially safe
  - Mark functions with pointer arithmetic as `// @unsafe`

### Task 1.5: Audit masstree_insert.hh
- File: `src/mako/masstree/masstree_insert.hh`
- Actions:
  - All functions likely `// @unsafe` due to tree mutations
  - Document specific unsafe operations

### Task 1.6: Audit masstree_scan.hh
- File: `src/mako/masstree/masstree_scan.hh`
- Actions:
  - Mark read-only traversal as potentially safe
  - Mark functions that modify state as `// @unsafe`

### Task 1.7: Audit masstree_remove.hh
- File: `src/mako/masstree/masstree_remove.hh`
- Actions:
  - All functions likely `// @unsafe` due to node manipulation
  - Document memory reclamation patterns

### Task 1.8: Audit masstree_struct.hh
- File: `src/mako/masstree/masstree_struct.hh`
- Actions:
  - Mark const accessors as `// @safe`
  - Mark node manipulation as `// @unsafe`
  - Document versioning/locking patterns

### Task 1.9: Audit kvrow.hh
- File: `src/mako/masstree/kvrow.hh`
- Actions:
  - Mark row accessors as potentially safe
  - Mark allocation/deallocation as `// @unsafe`

### Task 1.10: Audit value_versioned_array.hh/cc
- File: `src/mako/masstree/value_versioned_array.hh`
- File: `src/mako/masstree/value_versioned_array.cc`
- Actions:
  - Mark version chain traversal as `// @unsafe`
  - Document MVCC safety requirements

---

## Phase 2: Replace Raw Pointers with Ptr/MutPtr Wrappers

**Goal**: Replace `T*` with `rusty::MutPtr<T>` and `const T*` with `rusty::Ptr<T>` in function signatures and local variables.

### Task 2.1: Add rusty/ptr.hpp include to masstree headers
- File: `src/mako/masstree/masstree.hh`
- Actions:
  - Add `#include <rusty/ptr.hpp>`
  - Add `using rusty::Ptr;` and `using rusty::MutPtr;`

### Task 2.2: Convert masstree_context.h pointers
- File: `src/mako/masstree/masstree_context.h`
- Actions:
  - `threadinfo*` → `MutPtr<threadinfo>` in signatures
  - `const threadinfo*` → `Ptr<threadinfo>`
  - Update return types and parameters

### Task 2.3: Convert kvthread.hh public interface pointers
- File: `src/mako/masstree/kvthread.hh`
- Actions:
  - `threadinfo* next()` → `MutPtr<threadinfo> next()`
  - `MasstreeContext* context()` → `MutPtr<MasstreeContext> context()`
  - Keep internal `next_` as raw for now

### Task 2.4: Convert masstree.hh interface pointers
- File: `src/mako/masstree/masstree.hh`
- Actions:
  - Convert public API return types
  - Keep internal node pointers as raw (performance critical)

### Task 2.5: Convert masstree_get.hh function signatures
- File: `src/mako/masstree/masstree_get.hh`
- Actions:
  - Convert callback function pointer parameters
  - Convert result value pointers

### Task 2.6: Convert masstree_insert.hh function signatures
- File: `src/mako/masstree/masstree_insert.hh`
- Actions:
  - Convert cursor and node pointer parameters where safe

### Task 2.7: Convert masstree_scan.hh function signatures
- File: `src/mako/masstree/masstree_scan.hh`
- Actions:
  - Convert scanner state pointers
  - Convert callback parameters

### Task 2.8: Convert kvrow.hh pointers
- File: `src/mako/masstree/kvrow.hh`
- Actions:
  - Convert row pointer types in public interface

### Task 2.9: Convert value_versioned_array pointers
- File: `src/mako/masstree/value_versioned_array.hh`
- Actions:
  - Convert version chain pointer types
  - Keep internal chaining as raw (RCU patterns)

---

## Phase 3: Rewrite Unsafe Functions to Safe Equivalents

**Goal**: For functions that can be made safe, rewrite them using safe patterns.

### Task 3.1: Convert simple getters to safe functions
- Files: Various
- Actions:
  - Functions that only return member values → `// @safe`
  - Functions that only read const references → `// @safe`

### Task 3.2: Convert threadinfo accessors to safe
- File: `src/mako/masstree/kvthread.hh`
- Actions:
  - `purpose()` → `// @safe` (returns int)
  - `index()` → `// @safe` (returns int)
  - `context()` → `// @safe` (returns MutPtr)

### Task 3.3: Convert masstree_context accessors to safe
- File: `src/mako/masstree/masstree_context.h`
- Actions:
  - `id()` → `// @safe`
  - `get_epoch()` → `// @safe`
  - `get_allthreads()` → needs `// @unsafe` (returns raw linked list)

### Task 3.4: Wrap unsafe operations in explicit unsafe blocks
- Files: Various
- Actions:
  - For functions that must remain unsafe, document why
  - Use `// @unsafe { reason }` blocks around specific operations

### Task 3.5: Convert const traversal functions
- File: `src/mako/masstree/masstree_get.hh`
- Actions:
  - Read-only tree traversal → wrap unsafe node access
  - Version checking → safe (atomic reads)

### Task 3.6: Convert scan iteration to use safe wrappers
- File: `src/mako/masstree/masstree_scan.hh`
- Actions:
  - Create safe iterator wrapper
  - Internal traversal stays unsafe

---

## Phase 4: Enable Borrow Checking

**Goal**: Enable borrow checking for migrated files.

### Task 4.1: Enable borrow checking for masstree_context
- File: `CMakeLists.txt`
- Actions:
  - Uncomment masstree borrow check targets
  - Add `masstree_context.cc` to borrow check list
  - Fix any violations

### Task 4.2: Enable borrow checking for kvthread
- File: `CMakeLists.txt`
- Actions:
  - Add `kvthread.cc` to borrow check list
  - Fix any violations

### Task 4.3: Incrementally enable more files
- Files: One at a time
- Actions:
  - Add file to borrow check
  - Fix violations
  - Document exclusions with reasons

---

## Phase 5: Advanced Safety Patterns (Future)

### Task 5.1: Replace raw allocations with rusty::Box
- Where possible, replace `malloc/new` with `rusty::Box<T>`

### Task 5.2: Convert shared state to rusty::Arc
- Thread-safe shared pointers where applicable

### Task 5.3: Convert interior mutability to rusty::Cell/RefCell
- Global state patterns

### Task 5.4: Document remaining unsafe boundaries
- Create safety documentation for unavoidably unsafe code

---

## Success Criteria

1. **Phase 1 Complete**: All functions annotated with `@safe` or `@unsafe`
2. **Phase 2 Complete**: Public APIs use `Ptr<T>`/`MutPtr<T>` wrappers
3. **Phase 3 Complete**: Maximum functions marked `@safe`
4. **Phase 4 Complete**: Core files pass borrow checking
5. **Phase 5 Complete**: Advanced patterns where beneficial

## Estimated Effort

- Phase 1: ~2-3 hours (audit and annotate)
- Phase 2: ~3-4 hours (mechanical pointer conversion)
- Phase 3: ~4-6 hours (careful rewrites)
- Phase 4: ~2-3 hours (fix borrow check violations)
- Phase 5: ~4-8 hours (advanced patterns)

**Total**: ~15-24 hours of focused work

## Risk Mitigation

1. **Build after each task**: Ensure compilation succeeds
2. **Run tests after each phase**: Verify no behavioral changes
3. **Git commits per task**: Enable easy rollback
4. **Document exclusions**: Explain why certain code can't be made safe
