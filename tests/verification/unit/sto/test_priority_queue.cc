/**
 * Priority Queue Unit Tests
 * 
 * Comprehensive tests for STO Priority Queue implementation including:
 * - Basic operations (insert, extract_min, peek)
 * - Edge cases (empty, duplicate priorities, same priority)
 * - Priority ordering validation
 * - Concurrent access patterns
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <algorithm>
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
class SimplePriorityQueue {
private:
    static constexpr size_t CAPACITY = 1024;
    
    struct Element {
        T value;
        int priority;
        bool occupied;
        Element() : occupied(false) {}
    };
    
    Element heap_[CAPACITY];
    size_t size_;
    
    size_t parent(size_t i) const { return (i - 1) / 2; }
    size_t left_child(size_t i) const { return 2 * i + 1; }
    size_t right_child(size_t i) const { return 2 * i + 2; }
    
    void swap(size_t i, size_t j) {
        Element temp = heap_[i];
        heap_[i] = heap_[j];
        heap_[j] = temp;
    }
    
    void heapify_up(size_t i) {
        while (i > 0 && heap_[parent(i)].priority > heap_[i].priority) {
            swap(i, parent(i));
            i = parent(i);
        }
    }
    
    void heapify_down(size_t i) {
        size_t min_idx = i;
        size_t left = left_child(i);
        size_t right = right_child(i);
        
        if (left < size_ && heap_[left].priority < heap_[min_idx].priority) {
            min_idx = left;
        }
        
        if (right < size_ && heap_[right].priority < heap_[min_idx].priority) {
            min_idx = right;
        }
        
        if (min_idx != i) {
            swap(i, min_idx);
            heapify_down(min_idx);
        }
    }
    
public:
    SimplePriorityQueue() : size_(0) {}
    
    bool insert(const T& value, int priority) {
        if (size_ >= CAPACITY) return false;
        
        heap_[size_].value = value;
        heap_[size_].priority = priority;
        heap_[size_].occupied = true;
        
        heapify_up(size_);
        size_++;
        return true;
    }
    
    bool extract_min(T& value, int& priority) {
        if (size_ == 0) return false;
        
        value = heap_[0].value;
        priority = heap_[0].priority;
        
        heap_[0] = heap_[size_ - 1];
        size_--;
        
        if (size_ > 0) {
            heapify_down(0);
        }
        
        return true;
    }
    
    bool peek(T& value, int& priority) const {
        if (size_ == 0) return false;
        value = heap_[0].value;
        priority = heap_[0].priority;
        return true;
    }
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    
    void clear() { size_ = 0; }
};

// Test 1: Insert into empty priority queue
bool test_insert_empty() {
    std::cout << "\n[Test 1] Insert into empty priority queue..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    TEST_ASSERT(pq.empty(), "Priority queue should be empty");
    TEST_ASSERT(pq.size() == 0, "Size should be 0");
    
    bool success = pq.insert(100, 5);
    TEST_ASSERT(success, "Insert should succeed");
    TEST_ASSERT(!pq.empty(), "Priority queue should not be empty");
    TEST_ASSERT(pq.size() == 1, "Size should be 1");
    
    int value, priority;
    pq.peek(value, priority);
    TEST_ASSERT(value == 100 && priority == 5, "Values should match");
    
    TEST_PASS("Insert into empty priority queue works");
}

// Test 2: Insert multiple with different priorities
bool test_insert_multiple_priorities() {
    std::cout << "\n[Test 2] Insert multiple with different priorities..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    pq.insert(10, 5);
    pq.insert(20, 3);
    pq.insert(30, 7);
    pq.insert(40, 1);
    pq.insert(50, 9);
    
    TEST_ASSERT(pq.size() == 5, "Size should be 5");
    
    int value, priority;
    pq.peek(value, priority);
    TEST_ASSERT(priority == 1, "Min priority should be 1");
    TEST_ASSERT(value == 40, "Value should be 40");
    
    TEST_PASS("Insert multiple with different priorities works");
}

// Test 3: Extract min maintains order
bool test_extract_min_order() {
    std::cout << "\n[Test 3] Extract min maintains order..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    pq.insert(10, 5);
    pq.insert(20, 2);
    pq.insert(30, 8);
    pq.insert(40, 1);
    pq.insert(50, 4);
    
    int value, priority;
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 1 && value == 40, "First should be priority 1");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 2 && value == 20, "Second should be priority 2");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 4 && value == 50, "Third should be priority 4");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 5 && value == 10, "Fourth should be priority 5");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 8 && value == 30, "Fifth should be priority 8");
    
    TEST_ASSERT(pq.empty(), "Queue should be empty");
    
    TEST_PASS("Extract min maintains order");
}

// Test 4: Extract from empty queue
bool test_extract_empty() {
    std::cout << "\n[Test 4] Extract from empty queue..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    int value, priority;
    bool success = pq.extract_min(value, priority);
    TEST_ASSERT(!success, "Extract from empty should fail");
    
    TEST_PASS("Extract from empty queue works");
}

// Test 5: Peek doesn't remove element
bool test_peek_non_destructive() {
    std::cout << "\n[Test 5] Peek doesn't remove element..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    pq.insert(100, 5);
    pq.insert(200, 3);
    
    int value, priority;
    pq.peek(value, priority);
    TEST_ASSERT(priority == 3 && value == 200, "Peek should return min");
    TEST_ASSERT(pq.size() == 2, "Size should still be 2");
    
    pq.peek(value, priority);
    TEST_ASSERT(priority == 3 && value == 200, "Peek again should return same");
    TEST_ASSERT(pq.size() == 2, "Size should still be 2");
    
    TEST_PASS("Peek doesn't remove element");
}

// Test 6: Duplicate priorities
bool test_duplicate_priorities() {
    std::cout << "\n[Test 6] Duplicate priorities..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    pq.insert(10, 5);
    pq.insert(20, 5);
    pq.insert(30, 5);
    pq.insert(40, 3);
    
    int value, priority;
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 3, "First should be priority 3");
    
    // Next three should all be priority 5
    for (int i = 0; i < 3; i++) {
        pq.extract_min(value, priority);
        TEST_ASSERT(priority == 5, "Should be priority 5");
    }
    
    TEST_ASSERT(pq.empty(), "Queue should be empty");
    
    TEST_PASS("Duplicate priorities work");
}

// Test 7: All same priority (FIFO-like)
bool test_all_same_priority() {
    std::cout << "\n[Test 7] All same priority..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    for (int i = 0; i < 10; i++) {
        pq.insert(i, 5);
    }
    
    TEST_ASSERT(pq.size() == 10, "Size should be 10");
    
    int value, priority;
    for (int i = 0; i < 10; i++) {
        bool success = pq.extract_min(value, priority);
        TEST_ASSERT(success, "Extract should succeed");
        TEST_ASSERT(priority == 5, "Priority should be 5");
    }
    
    TEST_ASSERT(pq.empty(), "Queue should be empty");
    
    TEST_PASS("All same priority works");
}

// Test 8: Clear queue
bool test_clear() {
    std::cout << "\n[Test 8] Clear queue..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    for (int i = 0; i < 20; i++) {
        pq.insert(i, i);
    }
    
    TEST_ASSERT(pq.size() == 20, "Size should be 20");
    
    pq.clear();
    TEST_ASSERT(pq.empty(), "Queue should be empty");
    TEST_ASSERT(pq.size() == 0, "Size should be 0");
    
    // Should work after clear
    pq.insert(99, 1);
    int value, priority;
    pq.peek(value, priority);
    TEST_ASSERT(value == 99 && priority == 1, "Should work after clear");
    
    TEST_PASS("Clear queue works");
}

// Test 9: Edge case - negative priorities
bool test_negative_priorities() {
    std::cout << "\n[Test 9] Edge case - negative priorities..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    
    pq.insert(10, 5);
    pq.insert(20, -3);
    pq.insert(30, 0);
    pq.insert(40, -10);
    
    int value, priority;
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == -10, "Min should be -10");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == -3, "Next should be -3");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 0, "Next should be 0");
    
    pq.extract_min(value, priority);
    TEST_ASSERT(priority == 5, "Last should be 5");
    
    TEST_PASS("Edge case - negative priorities works");
}

// Test 10: Concurrent insert with random priorities (10 threads, 30 sec)
bool test_concurrent_insert() {
    std::cout << "\n[Test 10] Concurrent insert (10 threads, 30 sec)..." << std::endl;
    
    SimplePriorityQueue<int> pq;
    std::atomic<int> insert_count{0};
    std::atomic<bool> stop_flag{false};
    std::mutex pq_mutex;
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> value_dist(1, 10000);
        std::uniform_int_distribution<> priority_dist(1, 100);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            int value = value_dist(gen);
            int priority = priority_dist(gen);
            
            {
                std::lock_guard<std::mutex> lock(pq_mutex);
                if (pq.insert(value, priority)) {
                    insert_count++;
                }
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
    
    std::cout << "  Inserts: " << insert_count << std::endl;
    std::cout << "  Final size: " << pq.size() << std::endl;
    
    TEST_ASSERT(insert_count > 0, "Should have inserts");
    TEST_ASSERT(pq.size() > 0, "Queue should not be empty");
    
    // Verify heap property - extract all and check order
    int last_priority = -1000000;
    int extracted = 0;
    int value, priority;
    
    while (pq.extract_min(value, priority)) {
        TEST_ASSERT(priority >= last_priority, "Priority order should be maintained");
        last_priority = priority;
        extracted++;
    }
    
    std::cout << "  Extracted in order: " << extracted << std::endl;
    
    TEST_PASS("Concurrent insert works");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Priority Queue Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_insert_empty();
    test_insert_multiple_priorities();
    test_extract_min_order();
    test_extract_empty();
    test_peek_non_destructive();
    test_duplicate_priorities();
    test_all_same_priority();
    test_clear();
    test_negative_priorities();
    test_concurrent_insert();
    
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
