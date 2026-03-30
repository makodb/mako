# CPU Throttling for Worker Threads

This document describes the CPU throttling feature that allows limiting CPU usage of worker threads to simulate large-scale distributed deployments on a single machine.

## Overview

CPU throttling enables running many worker threads on limited hardware by restricting each thread's CPU usage to a configurable percentage. This is useful for:

- **Scale testing**: Simulate 100+ nodes on a single 8-core machine
- **Resource-constrained environments**: Test behavior under CPU pressure
- **Reproducible benchmarks**: Achieve consistent throughput regardless of hardware

## How It Works

### Userspace Duty Cycle Approach

The throttling is implemented entirely in userspace using a **duty cycle** mechanism. Each worker thread independently manages its own CPU budget using the `CpuThrottler` class.

**Algorithm:**
1. Define a cycle period (default: 100ms)
2. Calculate work budget based on target CPU percentage
3. Track cumulative work time per cycle
4. Sleep when budget is exhausted until cycle ends

**Example for 5% CPU with 100ms cycle:**
- Work budget: 5ms per cycle
- Thread executes transactions for up to 5ms
- When budget exhausted, sleeps for remaining ~95ms
- New cycle begins with fresh 5ms budget

### Implementation Details

**Core class:** `src/mako/benchmarks/cpu_throttler.h`

```cpp
class CpuThrottler {
public:
    CpuThrottler(double cpu_percent, uint32_t cycle_ms);

    void begin_work();  // Call before each transaction
    void end_work();    // Call after each transaction (may sleep)

    bool is_enabled() const;
};
```

**Integration point:** `src/mako/benchmarks/bench.cc`

```cpp
// In worker thread's main loop
CpuThrottler throttler(cpu_limit_percent, throttle_cycle_ms);

while (running) {
    throttler.begin_work();
    // ... execute transaction ...
    throttler.end_work();  // May sleep if budget exhausted
}
```

### Key Characteristics

| Property | Value |
|----------|-------|
| Scope | Per-thread (each worker has independent throttler) |
| Granularity | Per-transaction |
| Implementation | Userspace (no kernel/cgroup involvement) |
| Default cycle | 100ms |
| Overhead | Minimal when disabled |

## Usage

### Command-Line Options

```bash
./build/dbtest \
    --cpu-limit <percent>      # CPU limit (0-100, 0=disabled)
    --throttle-cycle <ms>      # Duty cycle period (default: 100ms)
    ...
```

### Examples

**Run with 5% CPU per worker thread:**
```bash
./build/dbtest --num-threads 6 \
    --shard-config src/mako/config/local-shards2-warehouses6.yml \
    -P localhost -L 0,1 \
    --cpu-limit 5
```

**Run with 40% CPU and custom cycle:**
```bash
./build/dbtest --num-threads 6 \
    --shard-config src/mako/config/local-shards2-warehouses6.yml \
    -P localhost -L 0,1 \
    --cpu-limit 40 \
    --throttle-cycle 50
```

**Scale test with many threads:**
```bash
# Simulate 120 workers on 8 cores (5% CPU each = 6 full cores)
./build/dbtest --num-threads 120 \
    --shard-config src/mako/config/local-shards12-warehouses120.yml \
    -P localhost -L 0,1,2,3,4,5,6,7,8,9,10,11 \
    --cpu-limit 5
```

## Benchmark Results

### Throughput Scaling Test

Configuration:
- 2 shards, 6 threads per shard (12 total worker threads)
- Multi-shard single-process mode
- 20 seconds runtime per test

| CPU Limit | Throughput (ops/sec) | Scaling Factor |
|-----------|---------------------|----------------|
| 1%        | 185                 | -              |
| 2%        | 303                 | 1.63x          |
| 4%        | 622                 | 2.05x          |
| 8%        | 1,189               | 1.91x          |

**Key finding:** Throughput roughly doubles when CPU limit doubles, confirming linear scaling behavior. This validates that:
1. The throttling mechanism works correctly
2. Throughput is CPU-bound (not I/O or contention limited at these low rates)
3. The duty cycle approach provides predictable resource control

### CI Test

A CI test validates CPU throttling scaling:

```bash
./ci/ci.sh cpuThrottlingScaling
```

This test:
1. Runs 4 tests with 1%, 2%, 4%, 8% CPU limits
2. Measures throughput for each configuration
3. Verifies scaling factor is between 1.3x and 3.0x (roughly 2x expected)
4. Fails if scaling deviates significantly from expected 2x

Test script: `ci/test_cpu_throttling_scaling.sh`

## Design Rationale

### Why Userspace Throttling (Not cgroups)?

| Aspect | Userspace Duty Cycle | Linux cgroups |
|--------|---------------------|---------------|
| Portability | Works everywhere | Linux only, requires privileges |
| Setup | No configuration needed | Requires root/container setup |
| Precision | Transaction-aware | May interrupt mid-transaction |
| Scope | Worker threads only | Entire process/container |
| Overhead | Per-transaction check | Kernel enforcement |

The userspace approach was chosen for:
1. **Portability**: Works on any OS without special privileges
2. **Simplicity**: No external dependencies or configuration
3. **Transaction awareness**: Never interrupts mid-transaction

### Trade-offs

**Pros:**
- Simple, portable implementation
- Per-thread independent control
- Transaction-boundary aware
- Zero overhead when disabled

**Cons:**
- Only throttles worker threads (not transport, background threads)
- Bursty behavior within each cycle
- Requires cooperative integration in worker loop

## Configuration Reference

### BenchmarkConfig Settings

```cpp
// In benchmark_config.h
double cpu_limit_percent_;      // 0.0-100.0, 0 = no limit (default)
uint32_t throttle_cycle_ms_;    // Duty cycle period, default: 100ms
```

### Getters/Setters

```cpp
double getCpuLimitPercent() const;
void setCpuLimitPercent(double pct);
uint32_t getThrottleCycleMs() const;
void setThrottleCycleMs(uint32_t ms);
bool isCpuThrottlingEnabled() const;  // Returns true if limit in (0, 100)
```

## Files

| File | Purpose |
|------|---------|
| `src/mako/benchmarks/cpu_throttler.h` | CpuThrottler class implementation |
| `src/mako/benchmarks/benchmark_config.h` | Configuration storage |
| `src/mako/benchmarks/bench.cc` | Worker loop integration |
| `src/mako/benchmarks/dbtest.cc` | Command-line parsing |
| `ci/test_cpu_throttling_scaling.sh` | CI test script |

## See Also

- [Multi-Shard Single Process Mode](multi_shard_single_process.md) - Running multiple shards in one process
- [CPU Limiting Plan](cpu_limiting_plan.md) - Original design document
