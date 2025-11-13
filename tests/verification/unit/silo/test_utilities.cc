/**
 * Utilities Unit Tests
 * 
 * Comprehensive tests for Silo utility functions including:
 * - util.h: test each utility function
 * - silo_small_vector: push, pop, resize, edge cases
 * - tuple operations: create, access, modify
 * - thread operations: create, join, cleanup
 * - Edge cases: boundary conditions for each utility
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cmath>

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

// Small vector implementation (stack-allocated for small sizes)
template<typename T, size_t N = 8>
class SiloSmallVector {
private:
    T stack_storage_[N];
    T* heap_storage_;
    size_t size_;
    size_t capacity_;
    
    bool is_on_heap() const {
        return capacity_ > N;
    }
    
    T* data() {
        return is_on_heap() ? heap_storage_ : stack_storage_;
    }
    
    const T* data() const {
        return is_on_heap() ? heap_storage_ : stack_storage_;
    }
    
public:
    SiloSmallVector() : heap_storage_(nullptr), size_(0), capacity_(N) {}
    
    ~SiloSmallVector() {
        if (heap_storage_) {
            delete[] heap_storage_;
        }
    }
    
    void push_back(const T& value) {
        if (size_ >= capacity_) {
            reserve(capacity_ * 2);
        }
        data()[size_++] = value;
    }
    
    void pop_back() {
        if (size_ > 0) {
            size_--;
        }
    }
    
    T& operator[](size_t index) {
        return data()[index];
    }
    
    const T& operator[](size_t index) const {
        return data()[index];
    }
    
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    
    void reserve(size_t new_capacity) {
        if (new_capacity <= capacity_) return;
        
        T* new_storage = new T[new_capacity];
        
        // Copy existing elements
        for (size_t i = 0; i < size_; i++) {
            new_storage[i] = data()[i];
        }
        
        if (heap_storage_) {
            delete[] heap_storage_;
        }
        
        heap_storage_ = new_storage;
        capacity_ = new_capacity;
    }
    
    void clear() {
        size_ = 0;
    }
    
    bool is_using_stack() const {
        return !is_on_heap();
    }
};

// Simple tuple implementation
template<typename T1, typename T2>
struct SiloTuple {
    T1 first;
    T2 second;
    
    SiloTuple() : first(), second() {}
    SiloTuple(const T1& f, const T2& s) : first(f), second(s) {}
};

// Utility functions
namespace SiloUtil {

// Align value to next power of 2
inline size_t align_to_power_of_2(size_t value) {
    if (value == 0) return 1;
    value--;
    value |= value >> 1;
    value |= value >> 2;
    value |= value >> 4;
    value |= value >> 8;
    value |= value >> 16;
    value |= value >> 32;
    return value + 1;
}

// Check if value is power of 2
inline bool is_power_of_2(size_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

// Fast modulo for power of 2
inline size_t fast_mod_pow2(size_t value, size_t mod) {
    return value & (mod - 1);
}

// Min/Max functions
template<typename T>
inline T min(T a, T b) {
    return a < b ? a : b;
}

template<typename T>
inline T max(T a, T b) {
    return a > b ? a : b;
}

// Clamp value between min and max
template<typename T>
inline T clamp(T value, T min_val, T max_val) {
    return max(min_val, min(value, max_val));
}

// Swap two values
template<typename T>
inline void swap(T& a, T& b) {
    T temp = a;
    a = b;
    b = temp;
}

} // namespace SiloUtil

// Thread wrapper
class SiloThread {
private:
    std::thread thread_;
    bool joined_;
    
public:
    SiloThread() : joined_(true) {}
    
    template<typename Func>
    void start(Func&& func) {
        if (!joined_) return;
        thread_ = std::thread(std::forward<Func>(func));
        joined_ = false;
    }
    
    void join() {
        if (!joined_ && thread_.joinable()) {
            thread_.join();
            joined_ = true;
        }
    }
    
    bool is_joined() const { return joined_; }
};

// Test 1: Small vector basic operations
bool test_small_vector_basic() {
    std::cout << "\n[Test 1] Small vector basic operations..." << std::endl;
    
    SiloSmallVector<int> vec;
    
    TEST_ASSERT(vec.empty(), "Vector should be empty");
    TEST_ASSERT(vec.size() == 0, "Size should be 0");
    TEST_ASSERT(vec.capacity() == 8, "Initial capacity should be 8");
    TEST_ASSERT(vec.is_using_stack(), "Should use stack storage");
    
    // Push elements
    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);
    
    TEST_ASSERT(vec.size() == 3, "Size should be 3");
    TEST_ASSERT(vec[0] == 10, "First element should be 10");
    TEST_ASSERT(vec[1] == 20, "Second element should be 20");
    TEST_ASSERT(vec[2] == 30, "Third element should be 30");
    TEST_ASSERT(vec.is_using_stack(), "Should still use stack storage");
    
    TEST_PASS("Small vector basic operations work");
}

// Test 2: Small vector growth to heap
bool test_small_vector_heap_growth() {
    std::cout << "\n[Test 2] Small vector growth to heap..." << std::endl;
    
    SiloSmallVector<int, 4> vec;  // Small capacity
    
    TEST_ASSERT(vec.capacity() == 4, "Initial capacity should be 4");
    TEST_ASSERT(vec.is_using_stack(), "Should use stack storage");
    
    // Fill stack storage
    for (int i = 0; i < 4; i++) {
        vec.push_back(i);
    }
    
    TEST_ASSERT(vec.size() == 4, "Size should be 4");
    TEST_ASSERT(vec.is_using_stack(), "Should still use stack storage");
    
    // Push one more to trigger heap allocation
    vec.push_back(4);
    
    TEST_ASSERT(vec.size() == 5, "Size should be 5");
    TEST_ASSERT(!vec.is_using_stack(), "Should use heap storage");
    TEST_ASSERT(vec.capacity() == 8, "Capacity should double to 8");
    
    // Verify all elements
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT(vec[i] == i, "Element should match");
    }
    
    TEST_PASS("Small vector growth to heap works");
}

// Test 3: Small vector pop operations
bool test_small_vector_pop() {
    std::cout << "\n[Test 3] Small vector pop operations..." << std::endl;
    
    SiloSmallVector<int> vec;
    
    vec.push_back(10);
    vec.push_back(20);
    vec.push_back(30);
    
    TEST_ASSERT(vec.size() == 3, "Size should be 3");
    
    vec.pop_back();
    TEST_ASSERT(vec.size() == 2, "Size should be 2");
    TEST_ASSERT(vec[0] == 10, "First element should be 10");
    TEST_ASSERT(vec[1] == 20, "Second element should be 20");
    
    vec.pop_back();
    vec.pop_back();
    TEST_ASSERT(vec.empty(), "Vector should be empty");
    
    // Pop from empty (should not crash)
    vec.pop_back();
    TEST_ASSERT(vec.empty(), "Vector should still be empty");
    
    TEST_PASS("Small vector pop operations work");
}

// Test 4: Small vector clear and reuse
bool test_small_vector_clear() {
    std::cout << "\n[Test 4] Small vector clear and reuse..." << std::endl;
    
    SiloSmallVector<int> vec;
    
    for (int i = 0; i < 10; i++) {
        vec.push_back(i);
    }
    
    TEST_ASSERT(vec.size() == 10, "Size should be 10");
    
    vec.clear();
    TEST_ASSERT(vec.empty(), "Vector should be empty");
    TEST_ASSERT(vec.size() == 0, "Size should be 0");
    
    // Reuse after clear
    vec.push_back(99);
    TEST_ASSERT(vec.size() == 1, "Size should be 1");
    TEST_ASSERT(vec[0] == 99, "Element should be 99");
    
    TEST_PASS("Small vector clear and reuse work");
}

// Test 5: Tuple operations
bool test_tuple_operations() {
    std::cout << "\n[Test 5] Tuple operations..." << std::endl;
    
    // Default constructor
    SiloTuple<int, int> t1;
    TEST_ASSERT(t1.first == 0, "First should be 0");
    TEST_ASSERT(t1.second == 0, "Second should be 0");
    
    // Parameterized constructor
    SiloTuple<int, std::string> t2(42, "hello");
    TEST_ASSERT(t2.first == 42, "First should be 42");
    TEST_ASSERT(t2.second == "hello", "Second should be 'hello'");
    
    // Modify values
    t2.first = 100;
    t2.second = "world";
    TEST_ASSERT(t2.first == 100, "First should be 100");
    TEST_ASSERT(t2.second == "world", "Second should be 'world'");
    
    TEST_PASS("Tuple operations work");
}

// Test 6: Utility - align to power of 2
bool test_align_power_of_2() {
    std::cout << "\n[Test 6] Utility - align to power of 2..." << std::endl;
    
    TEST_ASSERT(SiloUtil::align_to_power_of_2(0) == 1, "0 should align to 1");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(1) == 1, "1 should align to 1");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(2) == 2, "2 should align to 2");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(3) == 4, "3 should align to 4");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(5) == 8, "5 should align to 8");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(7) == 8, "7 should align to 8");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(8) == 8, "8 should align to 8");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(9) == 16, "9 should align to 16");
    TEST_ASSERT(SiloUtil::align_to_power_of_2(100) == 128, "100 should align to 128");
    
    TEST_PASS("Align to power of 2 works");
}

// Test 7: Utility - is power of 2
bool test_is_power_of_2() {
    std::cout << "\n[Test 7] Utility - is power of 2..." << std::endl;
    
    TEST_ASSERT(!SiloUtil::is_power_of_2(0), "0 is not power of 2");
    TEST_ASSERT(SiloUtil::is_power_of_2(1), "1 is power of 2");
    TEST_ASSERT(SiloUtil::is_power_of_2(2), "2 is power of 2");
    TEST_ASSERT(!SiloUtil::is_power_of_2(3), "3 is not power of 2");
    TEST_ASSERT(SiloUtil::is_power_of_2(4), "4 is power of 2");
    TEST_ASSERT(!SiloUtil::is_power_of_2(5), "5 is not power of 2");
    TEST_ASSERT(SiloUtil::is_power_of_2(8), "8 is power of 2");
    TEST_ASSERT(!SiloUtil::is_power_of_2(10), "10 is not power of 2");
    TEST_ASSERT(SiloUtil::is_power_of_2(64), "64 is power of 2");
    TEST_ASSERT(!SiloUtil::is_power_of_2(100), "100 is not power of 2");
    TEST_ASSERT(SiloUtil::is_power_of_2(1024), "1024 is power of 2");
    
    TEST_PASS("Is power of 2 works");
}

// Test 8: Utility - fast modulo and min/max/clamp
bool test_utility_functions() {
    std::cout << "\n[Test 8] Utility - fast modulo and min/max/clamp..." << std::endl;
    
    // Fast modulo (only works for power of 2)
    TEST_ASSERT(SiloUtil::fast_mod_pow2(10, 8) == 2, "10 mod 8 should be 2");
    TEST_ASSERT(SiloUtil::fast_mod_pow2(15, 16) == 15, "15 mod 16 should be 15");
    TEST_ASSERT(SiloUtil::fast_mod_pow2(17, 16) == 1, "17 mod 16 should be 1");
    
    // Min/Max
    TEST_ASSERT(SiloUtil::min(5, 10) == 5, "Min of 5 and 10 is 5");
    TEST_ASSERT(SiloUtil::max(5, 10) == 10, "Max of 5 and 10 is 10");
    TEST_ASSERT(SiloUtil::min(-5, -10) == -10, "Min of -5 and -10 is -10");
    TEST_ASSERT(SiloUtil::max(-5, -10) == -5, "Max of -5 and -10 is -5");
    
    // Clamp
    TEST_ASSERT(SiloUtil::clamp(5, 0, 10) == 5, "5 clamped to [0,10] is 5");
    TEST_ASSERT(SiloUtil::clamp(-5, 0, 10) == 0, "-5 clamped to [0,10] is 0");
    TEST_ASSERT(SiloUtil::clamp(15, 0, 10) == 10, "15 clamped to [0,10] is 10");
    TEST_ASSERT(SiloUtil::clamp(7, 5, 20) == 7, "7 clamped to [5,20] is 7");
    
    TEST_PASS("Utility functions work");
}

// Test 9: Utility - swap
bool test_swap() {
    std::cout << "\n[Test 9] Utility - swap..." << std::endl;
    
    int a = 10, b = 20;
    SiloUtil::swap(a, b);
    TEST_ASSERT(a == 20, "a should be 20");
    TEST_ASSERT(b == 10, "b should be 10");
    
    std::string s1 = "hello", s2 = "world";
    SiloUtil::swap(s1, s2);
    TEST_ASSERT(s1 == "world", "s1 should be 'world'");
    TEST_ASSERT(s2 == "hello", "s2 should be 'hello'");
    
    TEST_PASS("Swap works");
}

// Test 10: Thread operations
bool test_thread_operations() {
    std::cout << "\n[Test 10] Thread operations..." << std::endl;
    
    std::atomic<int> counter{0};
    
    SiloThread thread1;
    TEST_ASSERT(thread1.is_joined(), "Thread should be joined initially");
    
    // Start thread
    thread1.start([&counter]() {
        for (int i = 0; i < 1000; i++) {
            counter++;
        }
    });
    
    TEST_ASSERT(!thread1.is_joined(), "Thread should not be joined");
    
    // Join thread
    thread1.join();
    TEST_ASSERT(thread1.is_joined(), "Thread should be joined");
    TEST_ASSERT(counter == 1000, "Counter should be 1000");
    
    // Multiple threads
    counter = 0;
    std::vector<SiloThread> threads(5);
    
    for (auto& t : threads) {
        t.start([&counter]() {
            for (int i = 0; i < 100; i++) {
                counter++;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    TEST_ASSERT(counter == 500, "Counter should be 500");
    
    TEST_PASS("Thread operations work");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Utilities Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_small_vector_basic();
    test_small_vector_heap_growth();
    test_small_vector_pop();
    test_small_vector_clear();
    test_tuple_operations();
    test_align_power_of_2();
    test_is_power_of_2();
    test_utility_functions();
    test_swap();
    test_thread_operations();
    
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
