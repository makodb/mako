/**
 * Performance tests for Masstree string infrastructure
 * Measures throughput and latency to detect regressions from annotations
 */

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include <iomanip>

#include "mako/masstree/str.hh"
#include "mako/masstree/string.hh"
#include "mako/masstree/straccum.hh"

using namespace lcdf;
using namespace std::chrono;

class PerfTimer {
    high_resolution_clock::time_point start_;
public:
    PerfTimer() : start_(high_resolution_clock::now()) {}
    
    double elapsed_ms() const {
        auto end = high_resolution_clock::now();
        return duration_cast<duration<double, std::milli>>(end - start_).count();
    }
    
    double elapsed_us() const {
        auto end = high_resolution_clock::now();
        return duration_cast<duration<double, std::micro>>(end - start_).count();
    }
};

// Generate random string of given length
std::string random_string(size_t length, std::mt19937& rng) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<> dist(0, sizeof(charset) - 2);
    
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; i++) {
        result += charset[dist(rng)];
    }
    return result;
}

// ============================================================================
// Str Performance Tests
// ============================================================================

void perf_str_construction(size_t iterations) {
    std::cout << "\n=== Str Construction Performance ===" << std::endl;
    std::mt19937 rng(42);
    
    // Generate test data
    std::vector<std::string> test_strings;
    for (size_t i = 0; i < 1000; i++) {
        test_strings.push_back(random_string(50, rng));
    }
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        const std::string& s = test_strings[i % test_strings.size()];
        Str str(s.c_str());
        total += str.length();
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

void perf_str_comparison(size_t iterations) {
    std::cout << "\n=== Str Comparison Performance ===" << std::endl;
    std::mt19937 rng(42);
    
    std::vector<Str> test_strings;
    for (size_t i = 0; i < 1000; i++) {
        std::string s = random_string(50, rng);
        test_strings.push_back(Str(s.c_str(), s.length()));
    }
    
    PerfTimer timer;
    volatile size_t count = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        const Str& a = test_strings[i % test_strings.size()];
        const Str& b = test_strings[(i + 1) % test_strings.size()];
        if (a < b) count++;
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Count: " << count << ")" << std::endl;
}

// ============================================================================
// String Performance Tests
// ============================================================================

void perf_string_construction(size_t iterations) {
    std::cout << "\n=== String Construction Performance ===" << std::endl;
    std::mt19937 rng(42);
    
    std::vector<std::string> test_data;
    for (size_t i = 0; i < 1000; i++) {
        test_data.push_back(random_string(50, rng));
    }
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        String str(test_data[i % test_data.size()].c_str());
        total += str.length();
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

void perf_string_append(size_t iterations) {
    std::cout << "\n=== String Append Performance ===" << std::endl;
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        String str;
        for (int j = 0; j < 10; j++) {
            str += "test";
        }
        total += str.length();
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << " (10 appends each)" << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / (iterations * 10)) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << ((iterations * 10) / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

void perf_string_substring(size_t iterations) {
    std::cout << "\n=== String Substring Performance ===" << std::endl;
    
    String original("The quick brown fox jumps over the lazy dog");
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        String sub = original.substr(4, 5);
        total += sub.length();
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

void perf_string_find(size_t iterations) {
    std::cout << "\n=== String Find Performance ===" << std::endl;
    
    String haystack("The quick brown fox jumps over the lazy dog");
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        int pos = haystack.find_left("fox");
        total += (pos >= 0 ? pos : 0);
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

void perf_string_hashcode(size_t iterations) {
    std::cout << "\n=== String Hashcode Performance ===" << std::endl;
    std::mt19937 rng(42);
    
    std::vector<String> test_strings;
    for (size_t i = 0; i < 1000; i++) {
        test_strings.push_back(String(random_string(50, rng).c_str()));
    }
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        const String& s = test_strings[i % test_strings.size()];
        total += s.hashcode();
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

// ============================================================================
// StringAccum Performance Tests
// ============================================================================

void perf_stringaccum(size_t iterations) {
    std::cout << "\n=== StringAccum Performance ===" << std::endl;
    
    PerfTimer timer;
    volatile size_t total = 0;
    
    for (size_t i = 0; i < iterations; i++) {
        StringAccum sa;
        sa << "Hello " << i << " world";
        String result = sa.take_string();
        total += result.length();
    }
    
    double elapsed = timer.elapsed_us();
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << "Total time: " << std::fixed << std::setprecision(2) << elapsed << " μs" << std::endl;
    std::cout << "Per operation: " << std::fixed << std::setprecision(3) << (elapsed / iterations) << " μs" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << (iterations / elapsed * 1e6) << " ops/sec" << std::endl;
    std::cout << "(Consumed: " << total << ")" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    size_t iterations = 1000000;
    
    if (argc > 1) {
        iterations = std::stoull(argv[1]);
    }
    
    std::cout << "Masstree String Infrastructure Performance Test" << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << "Iterations per test: " << iterations << std::endl;
    
    // Run all performance tests
    perf_str_construction(iterations);
    perf_str_comparison(iterations);
    perf_string_construction(iterations);
    perf_string_append(iterations / 10);  // Slower operation
    perf_string_substring(iterations);
    perf_string_find(iterations);
    perf_string_hashcode(iterations);
    perf_stringaccum(iterations / 10);  // Slower operation
    
    std::cout << "\n=== Performance Test Complete ===" << std::endl;
    std::cout << "\nBaseline these numbers before annotation." << std::endl;
    std::cout << "After annotation, compare to detect regressions." << std::endl;
    std::cout << "Target: < 5% regression acceptable" << std::endl;
    
    return 0;
}

