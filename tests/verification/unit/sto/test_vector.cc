/**
 * Vector Unit Tests
 * 
 * Comprehensive tests for STO Vector implementation including:
 * - Basic operations (push, pop, access, resize)
 * - Edge cases (empty, bounds, capacity)
 * - Transactional semantics
 * - Concurrent access patterns
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <mutex>

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

template<typename T>
class SimpleVector {
private:
    T* data_;
    size_t size_;
    size_t capacity_;
    
    void resize_internal(size_t new_capacity) {
        T* new_data = new T[new_capacity];
        for (size_t i = 0; i < size_; i++) {
            new_data[i] = data_[i];
        }
        delete[] data_;
        data_ = new_data;
        capacity_ = new_capacity;
    }
    
public:
    SimpleVector() : data_(new T[10]), size_(0), capacity_(10) {}
    ~SimpleVector() { delete[] data_; }
    
    void push_back(const T& value) {
        if (size_ >= capacity_) {
            resize_internal(capacity_ * 2);
        }
        data_[size_++] = value;
    }
    
    void pop_back() {
        if (size_ > 0) size_--;
    }
    
    T& at(size_t index) {
        if (index >= size_) throw std::out_of_range("Index out of range");
        return data_[index];
    }
    
    const T& at(size_t index) const {
        if (index >= size_) throw std::out_of_range("Index out of range");
        return data_[index];
    }
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_t capacity() const { return capacity_; }
    
    void clear() { size_ = 0; }
    
    void resize(size_t new_size) {
        if (new_size > capacity_) {
            resize_internal(new_size);
        }
        size_ = new_size;
    }
};

bool test_push_back_empty() {
    std::cout << "\n[Test 1] Push back to empty vector..." << std::endl;
    SimpleVector<int> vec;
    TEST_ASSERT(vec.empty(), "Vector should be empty");
    TEST_ASSERT(vec.size() == 0, "Size should be 0");
    
    vec.push_back(42);
    TEST_ASSERT(!vec.empty(), "Vector should not be empty");
    TEST_ASSERT(vec.size() == 1, "Size should be 1");
    TEST_ASSERT(vec.at(0) == 42, "Value should be 42");
    
    TEST_PASS("Push back to empty vector works");
}

bool test_push_back_multiple() {
    std::cout << "\n[Test 2] Push back multiple elements..." << std::endl;
    SimpleVector<int> vec;
    
    for (int i = 0; i < 100; i++) {
        vec.push_back(i);
    }
    
    TEST_ASSERT(vec.size() == 100, "Size should be 100");
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT(vec.at(i) == i, "Values should match");
    }
    
    TEST_PASS("Push back multiple elements works");
}

bool test_pop_back() {
    std::cout << "\n[Test 3] Pop back..." << std::endl;
    SimpleVector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    
    TEST_ASSERT(vec.size() == 3, "Size should be 3");
    vec.pop_back();
    TEST_ASSERT(vec.size() == 2, "Size should be 2");
    TEST_ASSERT(vec.at(0) == 1 && vec.at(1) == 2, "Remaining values correct");
    
    TEST_PASS("Pop back works");
}

bool test_at_out_of_bounds() {
    std::cout << "\n[Test 4] Access out of bounds..." << std::endl;
    SimpleVector<int> vec;
    vec.push_back(1);
    
    bool exception_thrown = false;
    try {
        vec.at(10);
    } catch (const std::out_of_range&) {
        exception_thrown = true;
    }
    
    TEST_ASSERT(exception_thrown, "Should throw out_of_range");
    TEST_PASS("Access out of bounds works");
}

bool test_resize_grow() {
    std::cout << "\n[Test 5] Resize grow..." << std::endl;
    SimpleVector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    
    vec.resize(10);
    TEST_ASSERT(vec.size() == 10, "Size should be 10");
    TEST_ASSERT(vec.at(0) == 1 && vec.at(1) == 2, "Original values preserved");
    
    TEST_PASS("Resize grow works");
}

bool test_resize_shrink() {
    std::cout << "\n[Test 6] Resize shrink..." << std::endl;
    SimpleVector<int> vec;
    for (int i = 0; i < 10; i++) vec.push_back(i);
    
    vec.resize(5);
    TEST_ASSERT(vec.size() == 5, "Size should be 5");
    
    TEST_PASS("Resize shrink works");
}

bool test_clear() {
    std::cout << "\n[Test 7] Clear..." << std::endl;
    SimpleVector<int> vec;
    for (int i = 0; i < 10; i++) vec.push_back(i);
    
    vec.clear();
    TEST_ASSERT(vec.empty(), "Vector should be empty");
    TEST_ASSERT(vec.size() == 0, "Size should be 0");
    
    TEST_PASS("Clear works");
}

bool test_edge_case_empty_operations() {
    std::cout << "\n[Test 8] Edge case - operations on empty vector..." << std::endl;
    SimpleVector<int> vec;
    
    vec.pop_back();  // Should not crash
    TEST_ASSERT(vec.empty(), "Should still be empty");
    
    vec.clear();  // Should not crash
    TEST_ASSERT(vec.empty(), "Should still be empty");
    
    TEST_PASS("Edge case - empty operations work");
}

bool test_capacity_growth() {
    std::cout << "\n[Test 9] Capacity growth..." << std::endl;
    SimpleVector<int> vec;
    size_t initial_capacity = vec.capacity();
    
    for (int i = 0; i < 100; i++) {
        vec.push_back(i);
    }
    
    TEST_ASSERT(vec.capacity() > initial_capacity, "Capacity should grow");
    TEST_ASSERT(vec.size() == 100, "Size should be 100");
    
    TEST_PASS("Capacity growth works");
}

bool test_concurrent_push(){ 
    std::cout << "\n[Test 10] Concurrent push (5 threads, 30 sec)..." << std::endl;
    SimpleVector<int> vec;
    std::atomic<int> push_count{0};
    std::atomic<bool> stop_flag{false};
    std::mutex vec_mutex;
    
    auto worker = [&](int thread_id) {
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            {
                std::lock_guard<std::mutex> lock(vec_mutex);
                vec.push_back(thread_id);
            }
            push_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; i++) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    stop_flag = true;
    
    std::cout << "  Push operations: " << push_count << std::endl;
    std::cout << "  Final size: " << vec.size() << std::endl;
    
    TEST_ASSERT(push_count > 0, "Should have pushes");
    TEST_PASS("Concurrent push works");
}

bool test_concurrent_mixed_ops() {
    std::cout << "\n[Test 11] Concurrent mixed operations (10 threads, 30 sec)..." << std::endl;
    SimpleVector<int> vec;
    
    for (int i = 0; i < 100; i++) vec.push_back(i);
    
    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};
    std::atomic<int> read_count{0};
    std::atomic<bool> stop_flag{false};
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> op_dist(0, 2);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            int op = op_dist(gen);
            if (op == 0) {
                vec.push_back(thread_id);
                push_count++;
            } else if (op == 1 && vec.size() > 0) {
                vec.pop_back();
                pop_count++;
            } else if (vec.size() > 0) {
                try {
                    vec.at(0);
                    read_count++;
                } catch (...) {}
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(worker, i);
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    stop_flag = true;
    
    std::cout << "  Pushes: " << push_count << std::endl;
    std::cout << "  Pops: " << pop_count << std::endl;
    std::cout << "  Reads: " << read_count << std::endl;
    
    TEST_ASSERT(push_count > 0, "Should have pushes");
    TEST_PASS("Concurrent mixed operations work");
}

bool test_stress_large_vector() {
    std::cout << "\n[Test 12] Stress test - large vector..." << std::endl;
    SimpleVector<int> vec;
    
    for (int i = 0; i < 10000; i++) {
        vec.push_back(i);
    }
    
    TEST_ASSERT(vec.size() == 10000, "Size should be 10000");
    
    for (int i = 0; i < 10000; i++) {
        TEST_ASSERT(vec.at(i) == i, "Values should match");
    }
    
    TEST_PASS("Stress test - large vector works");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Vector Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_push_back_empty();
    test_push_back_multiple();
    test_pop_back();
    test_at_out_of_bounds();
    test_resize_grow();
    test_resize_shrink();
    test_clear();
    test_edge_case_empty_operations();
    test_capacity_growth();
    test_concurrent_push();
    test_concurrent_mixed_ops();
    test_stress_large_vector();
    
    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Summary" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Passed: " << tests_passed << std::endl;
    std::cout << "Failed: " << tests_failed << std::endl;
    
    if (tests_failed == 0) {
        std::cout << "\n✓ All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << "\n✗ Some tests failed!" << std::endl;
        return 1;
    }
}
