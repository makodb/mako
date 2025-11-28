# Mako Silo/STO Test Suite

Comprehensive unit tests and performance benchmarks for Mako's Silo and STO (Software Transactional Objects) components.

## Overview

This test suite provides:
- **Unit Tests**: Comprehensive functional tests for core Silo and STO components
- **Performance Benchmarks**: Throughput and latency measurements using Google Benchmark
- **Automated Test Runner**: Easy-to-use script for running all tests

## Test Coverage

### Silo Tests (`tests/silo/`)

#### `test_varint.cc` - Variable Integer Encoding Tests
Tests for varint encoding/decoding functions covering:
- **Basic Operations**: Single-byte, multi-byte encoding
- **Boundary Conditions**: Min/max values, edge cases
- **Round-trip Testing**: Encode → Decode verification
- **Failsafe Functions**: Error handling for insufficient buffers
- **Size Prediction**: Verifying `size_uvint32()` accuracy
- **Performance Indicators**: Encoding efficiency tests

**Key Test Cases:**
- `WriteUvint32_SingleByte` - Tests 0-127 range
- `WriteUvint32_MaxValue` - Tests 0xFFFFFFFF encoding
- `ReadUvint32_*` - Decoding verification
- `RoundTrip_RandomValues` - End-to-end correctness
- `FailsafeRead_InsufficientBytes` - Error handling
- `EdgeCase_ConsecutiveWrites` - Stream encoding/decoding

#### `bench_varint.cc` - Varint Performance Benchmarks
Micro-benchmarks measuring:
- Individual operation latency (write, read, size, skip)
- Throughput for batch operations (1000+ values)
- Compression ratio analysis
- Small vs. large value performance comparison

### STO Tests (`tests/sto/`)

#### `test_transaction.cc` - Transaction System Tests
Tests for STO transaction management:
- **Lifecycle Tests**: Initialization, commit, state management
- **Concurrency Tests**: Multi-threaded transaction execution
- **RCU Tests**: Read-Copy-Update mechanism verification
- **Edge Cases**: Multiple initialization, empty transactions
- **Batch Operations**: Sequential commit patterns

**Key Test Cases:**
- `Initialize_Success` - Basic initialization
- `StartCommit_EmptyTransaction` - Minimal transaction flow
- `ConcurrentTransactions_NoConflict` - Thread safety
- `Perf_1000EmptyCommits` - Throughput measurement
- `RcuCleanup_EpochProgression` - Memory management
- `BatchCommits_Sequential` - Batch processing

#### `bench_transaction.cc` - Transaction Performance Benchmarks
Benchmarks for:
- Empty transaction commit latency
- Concurrent transaction throughput (1-16 threads)
- Initialization overhead
- Batch commit scaling (10-1000 transactions)
- End-to-end latency measurement

## Prerequisites

- C++17 compiler (g++ or clang++)
- CMake 3.10+
- Google Test (included in project)
- Google Benchmark (for performance tests)
- pthreads

## Building Tests

### Option 1: Using the test runner script

```bash
cd tests
chmod +x run_tests.sh
./run_tests.sh build
```

### Option 2: Manual build

```bash
# From project root
cmake -S . -B build
cmake --build build --target test_silo_varint test_sto_transaction bench_silo_varint bench_sto_transaction -j$(nproc)
```

## Running Tests

### Quick Start - Run Everything

```bash
cd tests
./run_tests.sh all
```

### Run Unit Tests Only

```bash
./run_tests.sh unit
```

Expected output:
```
========================================
Running Unit Tests
========================================
→ Running Silo Varint Tests...
[==========] Running 25 tests from 1 test suite.
...
[  PASSED  ] 25 tests.
✓ Silo Varint Tests PASSED

→ Running STO Transaction Tests...
[==========] Running 12 tests from 2 test suites.
...
[  PASSED  ] 12 tests.
✓ STO Transaction Tests PASSED
```

### Run Performance Benchmarks

```bash
./run_tests.sh benchmark
```

Example output:
```
========================================
Running Performance Benchmarks
========================================
→ Running Silo Varint Benchmarks...
-----------------------------------------------------------------
Benchmark                       Time             CPU   Iterations
-----------------------------------------------------------------
BM_WriteUvint32_Small        3.21 ns         3.21 ns    217804533
BM_WriteUvint32_Medium       4.87 ns         4.87 ns    143812094
BM_WriteUvint32_Large        8.92 ns         8.92 ns     78432198
BM_ReadUvint32_Small         2.98 ns         2.98 ns    234891203
BM_EncodeThroughput        3142 ns         3142 ns       222715
```

### Run via CTest

```bash
./run_tests.sh ctest
```

### Generate Test Report

```bash
./run_tests.sh report
```

This creates `test_report.txt` with detailed results and JSON benchmark data.

## Test Organization

```
tests/
├── CMakeLists.txt          # Test build configuration
├── README.md               # This file
├── run_tests.sh            # Test runner script
├── silo/
│   ├── test_varint.cc      # Varint unit tests
│   └── bench_varint.cc     # Varint benchmarks
└── sto/
    ├── test_transaction.cc # Transaction unit tests
    └── bench_transaction.cc # Transaction benchmarks
```

## Running Individual Tests

### Run specific test executable

```bash
cd build/tests

# Run varint tests with verbose output
./test_silo_varint --gtest_filter="*" --gtest_color=yes

# Run specific test case
./test_silo_varint --gtest_filter="VarintTest.WriteUvint32_SingleByte"

# Run transaction tests
./test_sto_transaction

# Run benchmarks with specific parameters
./bench_silo_varint --benchmark_filter="BM_WriteUvint32.*"
./bench_sto_transaction --benchmark_min_time=1.0
```

### Google Test Options

- `--gtest_filter=PATTERN` - Run tests matching pattern
- `--gtest_repeat=N` - Repeat tests N times
- `--gtest_shuffle` - Randomize test order
- `--gtest_output=xml:file.xml` - Generate XML report
- `--gtest_color=yes` - Colorize output

### Google Benchmark Options

- `--benchmark_filter=PATTERN` - Run benchmarks matching pattern
- `--benchmark_min_time=N` - Minimum time per benchmark (seconds)
- `--benchmark_repetitions=N` - Number of repetitions
- `--benchmark_format=<console|json|csv>` - Output format
- `--benchmark_out=file` - Save results to file

## Interpreting Results

### Unit Test Results

- **[  PASSED  ]** - All assertions passed
- **[ FAILED ]** - One or more assertions failed
- Test failures show file, line number, and failed condition

### Benchmark Results

- **Time** - Wall clock time per iteration
- **CPU** - CPU time per iteration
- **Iterations** - Number of times benchmark was run
- Lower times are better

Performance targets (rough guidelines):
- Varint encode/decode: < 10 ns per operation
- Transaction commit: < 1 μs for empty transaction
- Throughput: > 1M operations/second

## Extending Tests

### Adding New Unit Tests

1. Create/edit test file in `tests/silo/` or `tests/sto/`
2. Use Google Test macros:

```cpp
TEST(TestSuiteName, TestName) {
    // Arrange
    int value = 42;
    
    // Act
    int result = function_under_test(value);
    
    // Assert
    EXPECT_EQ(result, expected_value);
    ASSERT_NE(pointer, nullptr);  // Fails test immediately if false
}
```

3. Add test to `CMakeLists.txt` if creating new file
4. Rebuild and run

### Adding New Benchmarks

1. Create/edit benchmark file
2. Use Google Benchmark macros:

```cpp
static void BM_MyFunction(benchmark::State& state) {
    for (auto _ : state) {
        my_function();
        benchmark::DoNotOptimize(result);  // Prevent optimization
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MyFunction);
```

3. Add benchmark to `CMakeLists.txt`
4. Rebuild and run

## Continuous Integration

To integrate with CI/CD:

```bash
# In your CI script
cd tests
./run_tests.sh unit || exit 1
./run_tests.sh benchmark
```

## Troubleshooting

### Tests fail to build
- Ensure all dependencies are installed
- Check CMake configuration: `cmake -S . -B build`
- Verify C++17 support: `g++ --version`

### Tests crash or hang
- Check for memory leaks: `valgrind ./test_silo_varint`
- Run with sanitizers: `cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fsanitize=undefined"`

### Benchmark results are inconsistent
- Close other applications
- Run benchmarks with `--benchmark_repetitions=10`
- Pin to specific CPU: `taskset -c 0 ./bench_silo_varint`

## Contributing

When adding new features to Silo/STO:
1. Write unit tests for all new functions
2. Add edge case tests (null inputs, max values, etc.)
3. Add performance benchmarks for critical paths
4. Ensure all tests pass before committing

## License

Same as main Mako project.
