# Mako Silo/STO Test Suite

Comprehensive unit and performance tests for Mako's core Silo and STO components.

## Test Coverage

**Total: 62 tests - All passing ✓**

### Silo Components (49 tests)
- **Varint** (22 tests) - Variable-length integer encoding/decoding
- **RCU** (9 tests) - Read-Copy-Update memory management
- **Allocator** (9 tests) - CPU-aware memory allocation
- **Tuple** (6 tests) - Database tuple management
- **Integration** (3 tests) - Combined component testing

### STO Components (13 tests)
- **TRcu** (13 tests) - Software Transactional Memory RCU

## Quick Start

### Build Tests
```bash
cd /home/shiva/projects/mako
cmake --build build --target test_silo_allocator_tuple test_silo_varint test_silo_rcu_thread test_sto_trcu -j4
```

### Run All Tests
```bash
cd tests
./run_tests.sh all
```

### Run Individual Test Suites
```bash
# From build/tests directory
./test_silo_varint              # Varint tests
./test_silo_rcu_thread           # RCU tests
./test_silo_allocator_tuple      # Allocator + Tuple tests
./test_sto_trcu                  # STO TRcu tests
```

### Run via CTest
```bash
cd build
ctest --output-on-failure
```

## Test Files

- `silo/test_varint.cc` - Varint encoding/decoding tests
- `silo/test_rcu_thread.cc` - RCU memory management tests
- `silo/test_allocator_tuple.cc` - Allocator and tuple tests
- `sto/test_transaction.cc` - STO TRcu tests

## Key Features

- **Real API Testing**: All tests use actual production APIs, no mocks
- **Concurrency Testing**: Multi-threaded scenarios validated
- **Performance Metrics**: Baseline performance indicators included
- **Edge Case Coverage**: Comprehensive boundary and error condition testing

## Test Results

All 62 tests pass consistently:
- ✓ Silo Varint Tests (22/22)
- ✓ Silo RCU Tests (9/9)
- ✓ Silo Allocator+Tuple Tests (18/18)
- ✓ STO TRcu Tests (13/13)

## Build System

- **Framework**: Google Test
- **Build**: CMake
- **CI**: CTest integration
- **Dependencies**: mako, gtest, pthread, numa
