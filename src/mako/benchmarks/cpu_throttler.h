#pragma once

#include <chrono>
#include <thread>
#include <cstdint>

// CPU throttling via duty cycle approach
// Work for X ms, sleep for Y ms to achieve target CPU percentage
// Example: 5% CPU with 100ms cycle = 5ms work, 95ms sleep
class CpuThrottler {
public:
    CpuThrottler(double cpu_percent, uint32_t cycle_ms)
        : cpu_percent_(cpu_percent),
          cycle_ms_(cycle_ms),
          enabled_(cpu_percent > 0.0 && cpu_percent < 100.0),
          work_budget_us_(0),
          work_time_us_(0) {
        if (enabled_) {
            // Calculate work budget in microseconds for each cycle
            work_budget_us_ = static_cast<uint64_t>(cycle_ms * 1000 * cpu_percent / 100.0);
            cycle_start_ = std::chrono::steady_clock::now();
        }
    }

    // Call this at start of each transaction/work unit
    void begin_work() {
        if (!enabled_) return;
        work_start_ = std::chrono::steady_clock::now();
    }

    // Call this after each transaction/work unit, may sleep if budget exhausted
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
            int64_t remaining_ms = static_cast<int64_t>(cycle_ms_) - cycle_elapsed;
            if (remaining_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(remaining_ms));
            }
            // Start new cycle
            cycle_start_ = std::chrono::steady_clock::now();
            work_time_us_ = 0;
        } else if (cycle_elapsed >= static_cast<int64_t>(cycle_ms_)) {
            // Cycle ended but we didn't use all budget - start new cycle
            cycle_start_ = now;
            work_time_us_ = 0;
        }
    }

    bool is_enabled() const { return enabled_; }
    double get_cpu_percent() const { return cpu_percent_; }
    uint32_t get_cycle_ms() const { return cycle_ms_; }

private:
    double cpu_percent_;
    uint32_t cycle_ms_;
    bool enabled_;
    uint64_t work_budget_us_;  // microseconds of work allowed per cycle
    uint64_t work_time_us_;    // microseconds of work done in current cycle
    std::chrono::steady_clock::time_point cycle_start_;
    std::chrono::steady_clock::time_point work_start_;
};
