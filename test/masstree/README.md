# Masstree Tests

This directory contains tests for the Masstree B+ tree library migration to RustyCpp.

## Test Structure

```
test/masstree/
├── README.md                          # This file
├── test_masstree.cc                   # Consolidated test suite (all functional tests)
└── perf/
    └── perf_masstree_string.cc        # String performance baseline
```

## Running Tests

### Functional Tests

```bash
# Build and run all Masstree tests (currently string infrastructure)
cd build
make test_masstree
./test_masstree

# Run with CTest
ctest -R test_masstree -V
```

### Performance Tests

```bash
# Build performance tests
make perf_masstree_string

# Run with default iterations (1M)
./perf_masstree_string

# Run with custom iterations
./perf_masstree_string 10000000  # 10M iterations

# Save baseline
./perf_masstree_string 1000000 > baseline_string_perf.txt
```

## Performance Baseline Workflow

**Before annotating any file:**

1. Build performance tests
2. Run multiple times to get stable numbers
3. Save output as baseline
4. Record key metrics (ops/sec, latency)

**After annotation:**

1. Rebuild with annotations
2. Run same performance tests
3. Compare to baseline
4. Document any regressions > 2%

### Example:

```bash
# Before annotation
cd build
make perf_masstree_string
./perf_masstree_string 1000000 | tee baseline.txt

# After annotation
make perf_masstree_string
./perf_masstree_string 1000000 | tee annotated.txt

# Compare
diff baseline.txt annotated.txt
# Or use a proper performance comparison tool
```

## Test Coverage

### test_masstree.cc

Consolidated test suite covering:

**Phase 3: String Infrastructure (FUNCTIONAL)**

- **Str** (lightweight string view)
  - Construction from various sources
  - Comparison operations
  - Substring operations
  - Trimming (ltrim, rtrim, trim)
  - Integer conversion

- **String** (owned string with reference counting)
  - Construction and copying
  - Numeric conversions (int, long, double)
  - Append and concatenation
  - Substring extraction
  - Case conversion (upper, lower)
  - Search (find_left, find_right)
  - Trimming
  
- **StringAccum** (string builder)
  - Streaming operations
  - Numeric formatting
  - Buffer management

- **Memory Safety**
  - Out-of-memory handling
  - Shared memory semantics
  - Mutable access
  
- **Edge Cases**
  - Empty strings
  - Null characters
  - Long strings (> 10KB)

### perf_masstree_string.cc

Performance benchmarks:

- Str construction (ops/sec)
- Str comparison (ops/sec)
- String construction (ops/sec)
- String append (ops/sec)
- String substring (ops/sec)
- String find (ops/sec)
- String hashcode (ops/sec)
- StringAccum (ops/sec)

**Target Performance**: < 5% regression from baseline

**Phase 5: Tree Operations (PLACEHOLDER TEMPLATES)**

- Basic operations (insert, lookup, remove) - *commented out, needs API updates*
- Concurrent access patterns - *commented out, needs API updates*  
- Range scanning - *commented out, needs API updates*

**Note**: Tree operation tests are currently placeholder templates. They will be uncommented and updated in Phase 5 when working on core tree structure.

## Adding New Tests

### Functional Tests

All functional tests are consolidated into `test_masstree.cc`:

1. Add new test cases directly to `test_masstree.cc`
2. Follow GTest patterns from existing tests
3. Organize by component with clear comment headers
4. For tree operations, update the commented-out templates when ready

### Performance Tests

1. Add perf file: `perf/perf_masstree_<component>.cc`
2. Follow patterns from `perf_masstree_string.cc`
3. Add to CMakeLists.txt:
```cmake
add_executable(perf_masstree_<component> test/masstree/perf/perf_masstree_<component>.cc)
target_link_libraries(perf_masstree_<component> ${TEST_LINK_LIBS} pthread)
```