# CPU Limiting for Worker Threads - Implementation Plan

## Goal
Limit the CPU usage of worker threads (e.g., to 5% of a core) to enable simulating large-scale distributed tests with many more worker threads/sites on a single machine.

## Background
Currently, each worker thread runs at full CPU capacity in its main loop (`bench.cc:275-338`). To simulate many nodes on one machine, we need to throttle CPU usage so we can run hundreds of worker threads without saturating the CPU.

## Approach Comparison

| Approach | Pros | Cons | Recommendation |
|----------|------|------|----------------|
| **1. Duty Cycle Throttling** | Portable, per-thread control, configurable | Bursty behavior within cycle | **Recommended** |
| **2. Linux cgroups v2** | Accurate, kernel-enforced | Requires root, complex setup | For production |
| **3. Rate Limiting (TPS)** | Predictable throughput | Not true CPU % limiting | Complementary |
| **4. nice/priority** | Simple | Doesn't limit CPU %, just priority | Not suitable |

## Recommended Approach: Duty Cycle Throttling

### Concept
Work for X ms, sleep for Y ms to achieve target CPU percentage.

Example for 5% CPU:
- Cycle period: 100ms
- Work time: 5ms
- Sleep time: 95ms

### Implementation Steps

### Step 1: Add Configuration to BenchmarkConfig
**File:** `src/mako/benchmarks/benchmark_config.h`

Add new configuration fields:
```cpp
// CPU throttling configuration
double cpu_limit_percent_;    // 0.0-100.0, 0 = no limit (default)
uint32_t throttle_cycle_ms_;  // Duty cycle period in ms (default: 100)
```

Add getter/setter methods:
```cpp
double getCpuLimitPercent() const { return cpu_limit_percent_; }
void setCpuLimitPercent(double pct) { cpu_limit_percent_ = pct; }
uint32_t getThrottleCycleMs() const { return throttle_cycle_ms_; }
void setThrottleCycleMs(uint32_t ms) { throttle_cycle_ms_ = ms; }
bool isCpuThrottlingEnabled() const { return cpu_limit_percent_ > 0.0 && cpu_limit_percent_ < 100.0; }
```

### Step 2: Create CpuThrottler Utility Class
**File:** `src/mako/benchmarks/cpu_throttler.h` (new file)

```cpp
#pragma once
#include <chrono>
#include <thread>

class CpuThrottler {
public:
    CpuThrottler(double cpu_percent, uint32_t cycle_ms)
        : cpu_percent_(cpu_percent), cycle_ms_(cycle_ms),
          enabled_(cpu_percent > 0.0 && cpu_percent < 100.0) {
        if (enabled_) {
            work_budget_us_ = static_cast<uint64_t>(cycle_ms * 1000 * cpu_percent / 100.0);
            cycle_start_ = std::chrono::steady_clock::now();
            work_time_us_ = 0;
        }
    }

    // Call this at start of each transaction
    void begin_work() {
        if (!enabled_) return;
        work_start_ = std::chrono::steady_clock::now();
    }

    // Call this after each transaction, may sleep if budget exhausted
    void end_work() {
        if (!enabled_) return;

        auto now = std::chrono::steady_clock::now();
        auto work_duration = std::chrono::duration_cast<std::chrono::microseconds>(
            now - work_start_).count();
        work_time_us_ += work_duration;

        auto cycle_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - cycle_start_).count();

        // Check if we've exceeded our work budget for this cycle
        if (work_time_us_ >= work_budget_us_) {
            // Sleep until end of cycle
            uint64_t remaining_ms = cycle_ms_ - cycle_elapsed;
            if (remaining_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(remaining_ms));
            }
            // Start new cycle
            cycle_start_ = std::chrono::steady_clock::now();
            work_time_us_ = 0;
        } else if (cycle_elapsed >= cycle_ms_) {
            // Cycle ended but we didn't use all budget - start new cycle
            cycle_start_ = now;
            work_time_us_ = 0;
        }
    }

    bool is_enabled() const { return enabled_; }

private:
    double cpu_percent_;
    uint32_t cycle_ms_;
    bool enabled_;
    uint64_t work_budget_us_;  // microseconds of work allowed per cycle
    uint64_t work_time_us_;    // microseconds of work done in current cycle
    std::chrono::steady_clock::time_point cycle_start_;
    std::chrono::steady_clock::time_point work_start_;
};
```

### Step 3: Integrate Throttler into Worker Loop
**File:** `src/mako/benchmarks/bench.cc`

Modify `bench_worker::run()`:
```cpp
void bench_worker::run() {
    // ... existing initialization ...

    // Create throttler based on config
    CpuThrottler throttler(
        benchConfig.getCpuLimitPercent(),
        benchConfig.getThrottleCycleMs()
    );

    while (benchConfig.isRunning() && ...) {
        throttler.begin_work();  // Start work timing

        // ... existing transaction execution ...

        throttler.end_work();    // End work timing, may sleep
    }
}
```

### Step 4: Add Command-Line Option
**File:** `src/mako/benchmarks/dbtest.cc` (or wherever args are parsed)

Add option: `--cpu-limit <percent>` (e.g., `--cpu-limit 5` for 5%)
Add option: `--throttle-cycle <ms>` (optional, default 100ms)

### Step 5: Update Test Scripts
**File:** `examples/test_*.sh`, `ci/ci.sh`

Add CPU limiting parameter for scale testing:
```bash
# Example: Run with 5% CPU per worker, allowing 20x more workers
./build/dbtest --cpu-limit 5 --num-threads 120 ...
```

### Step 6: Add Documentation
**File:** `doc/cpu_limiting.md`

Document the feature, usage examples, and trade-offs.

## Testing Plan

1. **Unit Test:** Verify CpuThrottler produces expected duty cycle
2. **Integration Test:** Run with `--cpu-limit 50` and verify ~50% CPU usage
3. **Scale Test:** Run with `--cpu-limit 5 --num-threads 100` on a 8-core machine
4. **Correctness Test:** Ensure existing tests pass with throttling enabled

## Implementation Order

1. **Step 1:** Add BenchmarkConfig fields (foundation)
2. **Step 2:** Create CpuThrottler class (core logic)
3. **Step 3:** Integrate into bench.cc worker loop
4. **Step 4:** Add command-line parsing
5. **Step 5:** Update test scripts
6. **Step 6:** Documentation

## Estimated Impact

With 5% CPU limiting:
- 8-core machine: Can simulate ~160 full-speed workers (8 / 0.05 = 160)
- Allows testing with many more shards/replicas than physical cores
- Trade-off: Higher latency per transaction due to sleep periods

## Alternative Enhancements (Future)

1. **Adaptive throttling:** Adjust CPU limit based on system load
2. **Per-shard CPU limits:** Different limits for different shard types
3. **cgroup integration:** For production environments requiring strict enforcement
