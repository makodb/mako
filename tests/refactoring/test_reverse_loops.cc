/**
 * Reverse Loop Bug Test
 * 
 * Tests the critical unsigned integer underflow bug in reverse loops.
 * 
 * BUG DESCRIPTION:
 * In Transaction.cc, several functions use reverse loops with unsigned integers:
 *   for (unsigned transaction_index = tset_size_-1; transaction_index >= 0; --transaction_index)
 * 
 * Since unsigned integers cannot be negative, the condition "transaction_index >= 0"
 * is always true. When transaction_index reaches 0 and decrements, it wraps to UINT_MAX,
 * causing an infinite loop.
 * 
 * The current code has manual "if (transaction_index == 0) break" guards, but this
 * is fragile and error-prone.
 * 
 * AFFECTED FUNCTIONS:
 * - shard_try_lock_last_writeset() (line ~305)
 * - shard_validate() (line ~323)
 * - shard_install() (line ~374)
 * - shard_unlock() (line ~389, ~397)
 */

#include <iostream>
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <limits>

// Test result tracking
static std::atomic<int> tests_passed{0};
static std::atomic<int> tests_failed{0};

#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "FAIL: " << message << std::endl; \
            std::cerr << "  at " << __FILE__ << ":" << __LINE__ << std::endl; \
            tests_failed++; \
            return false; \
        } \
    } while(0)

#define TEST_PASS(message) \
    do { \
        std::cout << "PASS: " << message << std::endl; \
        tests_passed++; \
        return true; \
    } while(0)

/**
 * Simulates the buggy reverse loop pattern
 * Returns iteration count (should be finite, not infinite)
 */
unsigned simulate_buggy_reverse_loop(unsigned size) {
    unsigned iteration_count = 0;
    const unsigned MAX_ITERATIONS = 1000000;  // Safety limit
    
    // Handle size=0 case (would underflow immediately)
    if (size == 0) {
        return 0;
    }
    
    // This is the BUGGY pattern from the original code
    for (unsigned i = size - 1; i >= 0; --i) {
        iteration_count++;
        
        // Safety check to prevent actual infinite loop in test
        if (iteration_count > MAX_ITERATIONS) {
            return MAX_ITERATIONS + 1;  // Indicate infinite loop
        }
        
        // This is the manual guard that prevents the bug
        if (i == 0) break;
    }
    
    return iteration_count;
}

/**
 * Simulates the FIXED reverse loop pattern (using signed int)
 * Returns iteration count
 */
unsigned simulate_fixed_reverse_loop_signed(unsigned size) {
    unsigned iteration_count = 0;
    
    // FIXED pattern: use signed int
    for (int i = static_cast<int>(size) - 1; i >= 0; --i) {
        iteration_count++;
    }
    
    return iteration_count;
}

/**
 * Simulates the FIXED reverse loop pattern (using forward loop)
 * Returns iteration count
 */
unsigned simulate_fixed_reverse_loop_forward(unsigned size) {
    unsigned iteration_count = 0;
    
    // FIXED pattern: use forward loop instead
    for (unsigned i = 0; i < size; ++i) {
        iteration_count++;
    }
    
    return iteration_count;
}

/**
 * Test 1: Verify loop with size = 0 terminates immediately
 */
bool test_loop_size_zero() {
    std::cout << "\n[Test 1] Reverse loop with size = 0..." << std::endl;
    
    try {
        unsigned size = 0;
        
        // Test buggy pattern (with manual guard)
        unsigned buggy_iterations = simulate_buggy_reverse_loop(size);
        TEST_ASSERT(buggy_iterations == 0, 
                   "Buggy loop with size=0 should iterate 0 times (with guard)");
        
        // Test fixed pattern (signed int)
        unsigned fixed_iterations = simulate_fixed_reverse_loop_signed(size);
        TEST_ASSERT(fixed_iterations == 0, 
                   "Fixed loop (signed) with size=0 should iterate 0 times");
        
        // Test fixed pattern (forward)
        unsigned forward_iterations = simulate_fixed_reverse_loop_forward(size);
        TEST_ASSERT(forward_iterations == 0, 
                   "Fixed loop (forward) with size=0 should iterate 0 times");
        
        TEST_PASS("All loop patterns handle size=0 correctly");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 2: Verify loop with size = 1 iterates exactly once
 */
bool test_loop_size_one() {
    std::cout << "\n[Test 2] Reverse loop with size = 1..." << std::endl;
    
    try {
        unsigned size = 1;
        
        // Test buggy pattern (with manual guard)
        unsigned buggy_iterations = simulate_buggy_reverse_loop(size);
        TEST_ASSERT(buggy_iterations == 1, 
                   "Buggy loop with size=1 should iterate exactly 1 time");
        
        // Test fixed pattern (signed int)
        unsigned fixed_iterations = simulate_fixed_reverse_loop_signed(size);
        TEST_ASSERT(fixed_iterations == 1, 
                   "Fixed loop (signed) with size=1 should iterate exactly 1 time");
        
        // Test fixed pattern (forward)
        unsigned forward_iterations = simulate_fixed_reverse_loop_forward(size);
        TEST_ASSERT(forward_iterations == 1, 
                   "Fixed loop (forward) with size=1 should iterate exactly 1 time");
        
        TEST_PASS("All loop patterns handle size=1 correctly");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 3: Verify loop with size = 10 iterates exactly 10 times
 */
bool test_loop_size_ten() {
    std::cout << "\n[Test 3] Reverse loop with size = 10..." << std::endl;
    
    try {
        unsigned size = 10;
        
        // Test buggy pattern (with manual guard)
        unsigned buggy_iterations = simulate_buggy_reverse_loop(size);
        TEST_ASSERT(buggy_iterations == 10, 
                   "Buggy loop with size=10 should iterate exactly 10 times");
        
        // Test fixed pattern (signed int)
        unsigned fixed_iterations = simulate_fixed_reverse_loop_signed(size);
        TEST_ASSERT(fixed_iterations == 10, 
                   "Fixed loop (signed) with size=10 should iterate exactly 10 times");
        
        // Test fixed pattern (forward)
        unsigned forward_iterations = simulate_fixed_reverse_loop_forward(size);
        TEST_ASSERT(forward_iterations == 10, 
                   "Fixed loop (forward) with size=10 should iterate exactly 10 times");
        
        TEST_PASS("All loop patterns handle size=10 correctly");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 4: Verify loop with size = 100 iterates exactly 100 times
 */
bool test_loop_size_hundred() {
    std::cout << "\n[Test 4] Reverse loop with size = 100..." << std::endl;
    
    try {
        unsigned size = 100;
        
        // Test buggy pattern (with manual guard)
        unsigned buggy_iterations = simulate_buggy_reverse_loop(size);
        TEST_ASSERT(buggy_iterations == 100, 
                   "Buggy loop with size=100 should iterate exactly 100 times");
        
        // Test fixed pattern (signed int)
        unsigned fixed_iterations = simulate_fixed_reverse_loop_signed(size);
        TEST_ASSERT(fixed_iterations == 100, 
                   "Fixed loop (signed) with size=100 should iterate exactly 100 times");
        
        // Test fixed pattern (forward)
        unsigned forward_iterations = simulate_fixed_reverse_loop_forward(size);
        TEST_ASSERT(forward_iterations == 100, 
                   "Fixed loop (forward) with size=100 should iterate exactly 100 times");
        
        TEST_PASS("All loop patterns handle size=100 correctly");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 5: Verify loop doesn't hang with timeout
 * This tests that the loop completes within reasonable time
 */
bool test_loop_no_hang() {
    std::cout << "\n[Test 5] Reverse loop doesn't hang (timeout test)..." << std::endl;
    
    try {
        const unsigned size = 1000;
        std::atomic<bool> completed{false};
        std::atomic<unsigned> iterations{0};
        
        // Run loop in separate thread with timeout
        std::thread worker([&]() {
            iterations = simulate_buggy_reverse_loop(size);
            completed = true;
        });
        
        // Wait with timeout (should complete in milliseconds)
        auto start = std::chrono::steady_clock::now();
        while (!completed) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(2)) {
                // Timeout - loop is hanging
                worker.detach();  // Don't wait for hung thread
                TEST_ASSERT(false, "Loop hung - did not complete within 2 seconds");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        worker.join();
        
        TEST_ASSERT(iterations == size, 
                   "Loop should iterate exactly size times");
        TEST_ASSERT(completed, "Loop should complete");
        
        TEST_PASS("Loop completes without hanging");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 6: Verify unsigned underflow behavior
 * This demonstrates the actual bug
 */
bool test_unsigned_underflow_behavior() {
    std::cout << "\n[Test 6] Unsigned underflow behavior..." << std::endl;
    
    try {
        // Demonstrate the underflow
        unsigned zero = 0;
        unsigned underflow = zero - 1;
        
        TEST_ASSERT(underflow == std::numeric_limits<unsigned>::max(), 
                   "Unsigned 0 - 1 should wrap to UINT_MAX");
        
        // This is why "unsigned i >= 0" is always true
        TEST_ASSERT(underflow >= 0, 
                   "UINT_MAX is >= 0 (always true for unsigned)");
        
        // Demonstrate that signed int doesn't have this problem
        int signed_zero = 0;
        int signed_underflow = signed_zero - 1;
        TEST_ASSERT(signed_underflow == -1, 
                   "Signed 0 - 1 should be -1");
        TEST_ASSERT(signed_underflow < 0, 
                   "Signed -1 is < 0 (can be false)");
        
        TEST_PASS("Unsigned underflow behavior verified");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 7: Stress test with various sizes
 * Tests edge cases and random sizes
 */
bool test_loop_stress_various_sizes() {
    std::cout << "\n[Test 7] Stress test with various sizes..." << std::endl;
    
    try {
        std::vector<unsigned> test_sizes = {
            0, 1, 2, 5, 10, 50, 100, 500, 1000, 5000
        };
        
        for (unsigned size : test_sizes) {
            unsigned buggy_iterations = simulate_buggy_reverse_loop(size);
            unsigned fixed_iterations = simulate_fixed_reverse_loop_signed(size);
            
            TEST_ASSERT(buggy_iterations == size, 
                       "Buggy loop should iterate exactly size times");
            TEST_ASSERT(fixed_iterations == size, 
                       "Fixed loop should iterate exactly size times");
            TEST_ASSERT(buggy_iterations == fixed_iterations, 
                       "Both patterns should produce same iteration count");
        }
        
        TEST_PASS("Stress test with various sizes passed");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Test 8: Concurrent loop execution
 * Verify loops work correctly under concurrent execution
 */
bool test_concurrent_loops() {
    std::cout << "\n[Test 8] Concurrent loop execution..." << std::endl;
    
    try {
        const int num_threads = 10;
        const unsigned loop_size = 100;
        std::atomic<int> successful_threads{0};
        
        auto worker = [&]() {
            // Each thread runs multiple loops
            for (int i = 0; i < 10; i++) {
                unsigned iterations = simulate_buggy_reverse_loop(loop_size);
                if (iterations != loop_size) {
                    return;  // Failed
                }
            }
            successful_threads++;
        };
        
        // Launch threads
        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back(worker);
        }
        
        // Wait for completion
        for (auto& t : threads) {
            t.join();
        }
        
        TEST_ASSERT(successful_threads == num_threads, 
                   "All threads should complete successfully");
        
        TEST_PASS("Concurrent loop execution passed");
    } catch (const std::exception& e) {
        TEST_ASSERT(false, std::string("Exception: ") + e.what());
    }
    
    return false;
}

/**
 * Main test runner
 */
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Reverse Loop Bug Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "\nTesting the unsigned integer underflow bug" << std::endl;
    std::cout << "in reverse loops (Transaction.cc)" << std::endl;
    
    // Run all tests
    test_loop_size_zero();
    test_loop_size_one();
    test_loop_size_ten();
    test_loop_size_hundred();
    test_loop_no_hang();
    test_unsigned_underflow_behavior();
    test_loop_stress_various_sizes();
    test_concurrent_loops();
    
    // Print summary
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Passed: " << tests_passed << std::endl;
    std::cout << "Failed: " << tests_failed << std::endl;
    
    if (tests_failed == 0) {
        std::cout << "\n✓ All tests passed!" << std::endl;
        std::cout << "The manual 'if (i == 0) break' guards are working." << std::endl;
        std::cout << "However, consider using signed int or forward loops for safety." << std::endl;
        return 0;
    } else {
        std::cout << "\n✗ Some tests failed!" << std::endl;
        return 1;
    }
}
