# Implementation Plan

- [x] 1. Set up refactoring environment and establish baseline

  - Create refactor-branch from current main branch
  - Run full test suite and document baseline performance
  - Verify build system works correctly with `make clean && make -j32`
  - _Requirements: 8.1, 8.2, 10.1_

- [ ] 2. Refactor core utility files (Foundation Phase)
- [x] 2.1 Refactor src/mako/util.h

  - Remove any TODO/FIXME comments
  - Extract magic numbers to named constants
  - Improve variable naming (single letters to descriptive names)
  - _Requirements: 1.1, 1.4, 7.1_

- [x] 2.2 Refactor src/mako/silo_small_vector.h

  - Clean up unused template parameters or functions
  - Add const correctness where missing

  - Improve iterator naming and documentation
  - _Requirements: 1.3, 6.4, 7.2_

- [x] 2.3 Refactor src/mako/benchmarks/sto/common.hh

  - Remove dead code and unused includes
  - Extract magic numbers to constants
  - Improve function and variable naming
  - _Requirements: 1.1, 1.4, 7.3_

- [x] 2.4 Refactor src/mako/benchmarks/sto/common.cc

  - Remove TODO comments and dead code
  - Split any functions longer than 50 lines
  - Eliminate code duplication
  - _Requirements: 1.1, 2.1, 3.1_

- [ ] 3. Refactor base transaction interfaces (Foundation + Structure Phase)
- [x] 3.1 Refactor src/mako/benchmarks/sto/Interface.hh


  - Clean up TODO/XXX comments (50+ instances)
  - Extract magic numbers like version constants
  - Improve naming of generic variables (t, v, etc.)
  - _Requirements: 1.1, 1.4, 7.1_

- [ ] 3.2 Refactor src/mako/benchmarks/sto/TransItem.hh



  - Remove unused member variables or methods
  - Add const correctness to getter methods
  - Improve variable naming in member functions
  - _Requirements: 1.3, 6.4, 7.2_

- [ ] 3.3 Refactor src/mako/txn.h

  - Remove compiler warning suppressions and fix underlying issues
  - Extract magic numbers to TransactionConfig constants
  - Improve template parameter naming
  - _Requirements: 1.5, 1.4, 7.3_

- [ ] 3.4 Refactor src/mako/benchmarks/sto/Transaction.hh

  - Remove 50+ TODO/XXX/FIXME comments
  - Extract magic numbers (MAX_THREADS=460, etc.) to config constants
  - Improve naming of member variables and parameters
  - _Requirements: 1.1, 1.4, 7.1_

- [ ] 4. Refactor core transaction implementations (Structure Phase)
- [ ] 4.1 Refactor src/mako/benchmarks/sto/Transaction.cc - Part 1 (Foundation)

  - Remove completed TODO comments and dead code
  - Extract magic numbers (usleep(100000), hash*base*=32768, etc.)
  - Fix compiler warnings without suppressing them
  - _Requirements: 1.1, 1.4, 1.5_

- [ ] 4.2 Refactor src/mako/benchmarks/sto/Transaction.cc - Part 2 (Structure)

  - Split Transaction::try_commit() (400+ lines) into 5 smaller functions
  - Split Transaction::stop() (250+ lines) into 3 smaller functions
  - Split shard_serialize_util() (150+ lines) into 3 smaller functions
  - _Requirements: 2.1, 2.2, 2.3_

- [ ] 4.3 Refactor src/mako/benchmarks/sto/Transaction.cc - Part 3 (Duplication)

  - Extract duplicate TransItem access pattern into helper function
  - Extract duplicate lock/unlock logic into common functions
  - Consolidate similar error handling patterns
  - _Requirements: 3.1, 3.2, 3.4_

- [ ] 4.4 Refactor src/mako/txn.cc

  - Split large commit/abort functions into smaller logical pieces
  - Remove duplicate validation logic
  - Improve variable naming in transaction methods
  - _Requirements: 2.1, 3.1, 7.2_

- [ ] 4.5 Refactor src/mako/txn_proto2_impl.cc

  - Clean up thread-local variable usage
  - Extract magic numbers related to epochs and timeouts
  - Split complex template functions into smaller parts
  - _Requirements: 4.1, 1.4, 2.1_

- [ ] 5. Refactor transactional data structures (Structure + Design Phase)
- [ ] 5.1 Refactor src/mako/benchmarks/sto/Hashtable.hh

  - Improve template parameter naming
  - Extract duplicate transactional method patterns
  - Add const correctness to read-only methods
  - _Requirements: 7.3, 3.1, 6.4_

- [ ] 5.2 Refactor src/mako/benchmarks/sto/Vector.hh

  - Split large template methods into smaller functions
  - Remove duplicate bounds checking code
  - Improve iterator and index variable naming
  - _Requirements: 2.1, 3.1, 7.2_

- [ ] 5.3 Refactor src/mako/benchmarks/sto/Queue.hh

  - Extract common queue operation patterns
  - Improve naming of internal queue variables
  - Add missing const correctness
  - _Requirements: 3.1, 7.2, 6.4_

- [ ] 5.4 Refactor src/mako/benchmarks/sto/PriorityQueue.hh

  - Split complex priority queue operations
  - Remove duplicate comparison logic
  - Improve variable naming in heap operations
  - _Requirements: 2.1, 3.1, 7.2_

- [ ] 5.5 Refactor src/mako/benchmarks/sto/RBTree.hh

  - Split large tree manipulation functions
  - Extract duplicate tree traversal patterns
  - Improve node and iterator variable naming
  - _Requirements: 2.1, 3.1, 7.2_

- [ ] 6. Refactor integration layer (Design Phase)
- [ ] 6.1 Refactor src/mako/benchmarks/sto/MassTrans.hh

  - Clean up Masstree integration code
  - Remove unused template specializations
  - Improve naming of Masstree-specific variables
  - _Requirements: 1.3, 7.2, 6.4_

- [ ] 6.2 Refactor src/mako/benchmarks/sto/MassTrans.cc

  - Split large Masstree transaction methods
  - Remove duplicate Masstree access patterns
  - Improve error handling in Masstree operations
  - _Requirements: 2.1, 3.1, 6.3_

- [ ] 6.3 Refactor src/mako/benchmarks/mbta_wrapper.hh

  - Organize scattered thread-local variables into ThreadContext struct
  - Extract duplicate index operation patterns
  - Improve naming of wrapper-specific variables
  - _Requirements: 4.1, 3.1, 7.2_

- [ ] 6.4 Refactor src/mako/benchmarks/mbta_wrapper_norm.hh

  - Remove code duplication with main mbta_wrapper
  - Improve naming consistency with main wrapper
  - Clean up unused normalization code
  - _Requirements: 3.4, 7.3, 1.3_

- [ ] 6.5 Refactor src/mako/benchmarks/mbta_wrapper_arena.hh

  - Remove code duplication with other wrapper variants
  - Improve arena-specific variable naming
  - Clean up unused arena allocation code
  - _Requirements: 3.4, 7.2, 1.3_

- [ ] 7. Refactor application code (Design + Polish Phase)
- [ ] 7.1 Refactor src/mako/benchmarks/tpcc.cc

  - Replace #ifdef blocks with cleaner conditional logic where possible
  - Improve naming of TPC-C specific variables
  - Extract duplicate transaction setup patterns
  - _Requirements: 5.1, 7.2, 3.1_

- [ ] 7.2 Refactor src/mako/benchmarks/abstract_db.cc

  - Improve abstract interface method naming
  - Add missing const correctness to read-only methods
  - Clean up unused abstract methods
  - _Requirements: 7.3, 6.4, 1.3_

- [ ] 7.3 Refactor src/mako/tuple.cc

  - Split large tuple manipulation functions
  - Improve column access variable naming
  - Remove duplicate tuple validation logic
  - _Requirements: 2.1, 7.2, 3.1_

- [ ] 7.4 Refactor src/mako/thread.cc

  - Consolidate thread-local variables into organized structures
  - Improve thread management function naming
  - Remove unused thread utility functions
  - _Requirements: 4.1, 7.2, 1.3_

- [ ] 8. Refactor test files (Polish Phase)
- [ ] 8.1 Refactor unit test files (unit-\*.cc)

  - Improve test function naming for clarity
  - Remove duplicate test setup code
  - Add missing const correctness in test data
  - _Requirements: 7.3, 3.1, 6.4_

- [ ] 8.2 Refactor integration test files

  - Clean up test utility functions
  - Improve test variable naming
  - Remove unused test helper code
  - _Requirements: 7.2, 1.3, 3.1_

- [ ] 8.3 Refactor benchmark test files

  - Extract duplicate benchmark setup patterns
  - Improve benchmark result variable naming
  - Clean up unused benchmark configurations
  - _Requirements: 3.1, 7.2, 1.3_

- [ ] 9. Final validation and cleanup
- [ ] 9.1 Run comprehensive test suite

  - Execute `./ci/ci.sh all` to verify all functionality
  - Run performance benchmarks to ensure no regressions
  - Verify build system works with all configurations
  - _Requirements: 8.2, 8.3, 10.5_

- [ ] 9.2 Update build and documentation files

  - Clean up any build warnings introduced during refactoring
  - Update CMakeLists.txt if any file structures changed
  - Verify all refactored code follows project conventions
  - _Requirements: 8.1, 8.4, 10.4_

- [ ] 9.3 Create final refactoring summary
  - Document code quality improvements achieved
  - List all files successfully refactored
  - Report any issues encountered and resolved
  - _Requirements: 10.2, 10.3, 10.5_
