# Design Document

## Overview

This design document outlines the systematic refactoring approach for the Mako distributed transaction system. The refactoring will be performed in phases, starting with foundational improvements and progressing to more complex architectural changes. The design ensures that all existing functionality is preserved while significantly improving code quality, maintainability, and adherence to modern C++ best practices.

## Architecture

### Simple File-by-File Refactoring Approach

The refactoring will follow a straightforward approach:
1. Process one file at a time in dependency order
2. Apply specific refactoring improvements to existing code
3. Build and test after each file
4. Commit successful changes
5. Move to next file

### Four-Phase Refactoring Strategy

1. **Foundation Phase**: Clean up existing code (remove dead code, extract constants)
2. **Structure Phase**: Improve existing functions (split large functions, remove duplication)
3. **Design Phase**: Enhance existing patterns (replace #ifdef with better code organization)
4. **Polish Phase**: Improve existing names and optimize existing code

## Refactoring Approach

### Per-File Refactoring Tasks

For each file, we will apply these improvements to existing code:

1. **Foundation Improvements**:
   - Remove TODO/XXX/FIXME comments that are completed or obsolete
   - Replace magic numbers with named constants
   - Remove unused variables and functions
   - Fix compiler warnings

2. **Structure Improvements**:
   - Split functions longer than 50 lines into smaller functions
   - Extract duplicate code into helper functions
   - Organize scattered thread-local variables into logical groups

3. **Design Improvements**:
   - Replace complex #ifdef blocks with cleaner conditional logic
   - Improve error handling (replace goto with RAII where appropriate)
   - Add missing const correctness

4. **Polish Improvements**:
   - Rename poorly named variables (n → item_count, it → current_item)
   - Simplify complex conditional expressions
   - Add performance improvements (const&, move semantics)

## Error Handling

### Simple Error Recovery Strategy

1. **Compilation Errors**: Revert changes and report the issue
2. **Test Failures**: Revert changes and report the issue  
3. **Performance Regressions**: Revert changes if >2% performance loss
4. **Build Issues**: Stop and request manual intervention

The approach is conservative - if anything breaks, revert and move to the next file.

## Testing Strategy

### Test Categories

1. **Unit Tests for Refactoring Components**
   - File processor functionality
   - Code transformation accuracy
   - Dependency resolution correctness

2. **Integration Tests for Refactored Code**
   - Existing Mako test suite (ci/ci.sh all)
   - Performance benchmarks
   - Memory leak detection

3. **Regression Tests**
   - Before/after functionality comparison
   - Performance baseline comparison
   - API compatibility verification

### Test Execution Strategy

After refactoring each file:
1. Run `make -j32` to verify compilation
2. Run `./ci/ci.sh simpleTransaction` for quick smoke test
3. If major changes, run `./ci/ci.sh all` for full test suite
4. Commit if all tests pass, revert if any fail

## Implementation Details

### Phase 1: Foundation Refactoring

**Target Files (Priority Order)**:
1. `src/mako/benchmarks/sto/Transaction.hh` - Remove 50+ TODO comments
2. `src/mako/benchmarks/sto/Transaction.cc` - Extract magic numbers
3. `src/mako/silo_small_vector.h` - Clean up unused code
4. `src/mako/txn.h` - Remove compiler warning suppressions

**Specific Transformations**:
- Replace magic numbers with named constants in `TransactionConfig` namespace
- Remove completed TODOs and document remaining ones
- Extract constants like `MAX_THREADS = 460` to configuration headers
- Remove unused variables flagged by compiler warnings

### Phase 2: Structure Refactoring

**Target Functions for Extraction**:
1. `Transaction::try_commit()` (400+ lines) → 5 smaller functions
2. `Transaction::stop()` (250+ lines) → 3 smaller functions
3. `shard_serialize_util()` (150+ lines) → 3 smaller functions

**Function Extraction Strategy**:
```cpp
// BEFORE: Giant function
bool Transaction::try_commit(bool no_paxos) {
    // 400+ lines of mixed concerns
}

// AFTER: Extracted functions
bool Transaction::try_commit(bool no_paxos) {
    if (!acquire_write_locks()) return false;
    if (!validate_read_set()) return false;
    if (!no_paxos && !coordinate_with_paxos()) return false;
    install_writes();
    release_locks();
    return true;
}

private:
    bool acquire_write_locks();
    bool validate_read_set();
    bool coordinate_with_paxos();
    void install_writes();
    void release_locks();
```

**Duplication Elimination**:
- Create `TransItemAccessor` utility for repeated TransItem access patterns
- Extract common lock/unlock logic into `LockManager` helper class
- Consolidate similar serialization code into `SerializationHelper`

### Phase 3: Design Refactoring

**Polymorphism Introduction**:
Replace conditional compilation with strategy pattern:

```cpp
// BEFORE: #ifdef nightmare
#if STO_SORT_WRITESET
    // sorted logic
#else
    // unsorted logic
#endif

// AFTER: Strategy pattern
class CommitStrategy {
public:
    virtual bool execute_commit() = 0;
    virtual ~CommitStrategy() = default;
};

class SortedWritesetStrategy : public CommitStrategy {
    bool execute_commit() override { /* sorted logic */ }
};

class UnsortedWritesetStrategy : public CommitStrategy {
    bool execute_commit() override { /* unsorted logic */ }
};
```

**Abstraction Creation**:
- `SerializationBuffer` class for type-safe buffer operations
- `ThreadContext` struct to consolidate thread-local variables
- `TransactionScope` RAII class for automatic cleanup

### Phase 4: Polish Refactoring

**Naming Improvements**:
- `int n` → `int item_count`
- `TransItem* it` → `TransItem* current_item`
- `unsigned tidx` → `unsigned transaction_index`

**Performance Optimizations**:
- Replace unnecessary copies with references
- Use move semantics for large objects
- Optimize string operations in loops

## File Processing Order

Based on dependency analysis, files will be processed in this order:

### Tier 1: Core Utilities (No dependencies)
1. `src/mako/util.h`
2. `src/mako/silo_small_vector.h`
3. `src/mako/benchmarks/sto/common.hh`
4. `src/mako/benchmarks/sto/common.cc`

### Tier 2: Base Interfaces (Depend on Tier 1)
5. `src/mako/benchmarks/sto/Interface.hh`
6. `src/mako/benchmarks/sto/TransItem.hh`
7. `src/mako/txn.h`
8. `src/mako/benchmarks/sto/Transaction.hh`

### Tier 3: Implementations (Depend on Tier 2)
9. `src/mako/benchmarks/sto/Transaction.cc`
10. `src/mako/txn.cc`
11. `src/mako/txn_proto2_impl.h`
12. `src/mako/txn_proto2_impl.cc`

### Tier 4: Data Structures (Depend on Tier 3)
13. `src/mako/benchmarks/sto/Hashtable.hh`
14. `src/mako/benchmarks/sto/Vector.hh`
15. `src/mako/benchmarks/sto/Queue.hh`
16. `src/mako/benchmarks/sto/PriorityQueue.hh`
17. `src/mako/benchmarks/sto/RBTree.hh`

### Tier 5: Integration Layer (Depend on Tier 4)
18. `src/mako/benchmarks/sto/MassTrans.hh`
19. `src/mako/benchmarks/sto/MassTrans.cc`
20. `src/mako/benchmarks/mbta_wrapper.hh`
21. `src/mako/benchmarks/mbta_wrapper_norm.hh`
22. `src/mako/benchmarks/mbta_wrapper_arena.hh`

### Tier 6: Applications (Depend on Tier 5)
23. `src/mako/benchmarks/tpcc.cc`
24. `src/mako/benchmarks/abstract_db.cc`
25. `src/mako/tuple.cc`
26. `src/mako/thread.cc`

### Tier 7: Tests (Depend on all previous tiers)
27. All unit test files (`unit-*.cc`)
28. Integration test files (`src/mako/benchmarks/ut/*.cc`)
29. Benchmark test files (`concurrent.cc`, `single.cc`, etc.)

## Risk Mitigation

### High-Risk Files (Extra Caution Required)
- `src/mako/silo_small_vector.h` - Used throughout transaction system
- `src/mako/benchmarks/sto/Transaction.hh` - Core STO API
- `src/mako/txn.h` - Core Silo API
- `src/mako/benchmarks/mbta_wrapper.hh` - Main database interface

### Risk Mitigation Strategies
1. **Incremental Changes**: Make small, focused changes with immediate testing
2. **Backup Points**: Create git commits after each successful file refactoring
3. **Rollback Capability**: Maintain ability to quickly revert problematic changes
4. **Parallel Validation**: Run tests continuously during refactoring process
5. **Performance Monitoring**: Track performance metrics throughout refactoring

## Success Metrics

### Code Quality Improvements (Target)
- Cyclomatic complexity: Reduce from ~80 to <15 for all functions
- Function length: Maximum 50 lines, average <30 lines
- Code duplication: Reduce from ~25% to <5%
- Magic numbers: Eliminate all magic numbers (target: 0)
- Thread-local variables: Reduce from 40+ to <20 organized variables

### Maintainability Improvements
- All functions have single responsibility
- Clear separation of concerns
- Comprehensive unit test coverage
- Self-documenting code with descriptive names
- Modern C++ best practices throughout

### Performance Requirements
- No performance regression >2%
- Memory usage remains stable
- Build time improvement through reduced template complexity
- Test execution time remains acceptable