// Minimal test for Fragile compiler - tests basic rrr library functions
// This file is intentionally simple with no template edge cases

#include <iostream>
#include <functional>
import rrr;

using namespace rrr;

// Test std::function copy (from test_lambda.cc)
uint64_t copy_func(const std::function<uint64_t(void)> &foo) {
    std::function<uint64_t(void)> boo = foo;
    return boo();
}

int main() {
    // Test 1: Basic logging
    Log_info("Fragile minimal test starting...");

    // Test 2: Basic timer
    Timer timer;
    timer.start();

    // Test 3: Simple loop with time measurement
    volatile int sum = 0;
    for (int i = 0; i < 1000; i++) {
        sum += i;
    }

    timer.stop();

    // Test 4: Print results
    Log_info("Sum: %d", sum);
    Log_info("Elapsed: %f seconds", timer.elapsed());

    // Test 5: Test Time::now()
    uint64_t now = Time::now(true);
    Log_info("Current time (ns): %lu", now);

    // Test 6: Test lambda with std::function
    timer.start();
    uint64_t ret = 0;
    for (int i = 0; i < 1000; i++) {
        uint64_t val = i * 2;
        ret += copy_func([val]() -> uint64_t {
            return val;
        });
    }
    timer.stop();
    Log_info("Lambda test sum: %lu, time: %f seconds", ret, timer.elapsed());

    Log_info("All tests passed!");
    return 0;
}
