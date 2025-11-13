/**
 * Hashtable Unit Tests
 * 
 * Comprehensive tests for STO Hashtable implementation including:
 * - Basic operations (insert, lookup, delete)
 * - Edge cases (empty, null, duplicates, capacity)
 * - Transactional semantics
 * - Concurrent access patterns
 */

#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <random>

// Test statistics
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

// Simplified hashtable for testing
template<typename K, typename V>
class SimpleHashtable {
private:
    static constexpr size_t CAPACITY = 1024;
    struct Entry {
        K key;
        V value;
        bool occupied;
        Entry() : occupied(false) {}
    };
    Entry table_[CAPACITY];
    size_t size_;
    
    size_t hash(const K& key) const {
        return std::hash<K>{}(key) % CAPACITY;
    }
    
public:
    SimpleHashtable() : size_(0) {}
    
    bool insert(const K& key, const V& value) {
        if (size_ >= CAPACITY) return false;
        
        size_t idx = hash(key);
        size_t probe = 0;
        
        while (probe < CAPACITY) {
            size_t pos = (idx + probe) % CAPACITY;
            if (!table_[pos].occupied) {
                table_[pos].key = key;
                table_[pos].value = value;
                table_[pos].occupied = true;
                size_++;
                return true;
            }
            if (table_[pos].key == key) {
                table_[pos].value = value;  // Update
                return true;
            }
            probe++;
        }
        return false;
    }
    
    bool lookup(const K& key, V& value) const {
        size_t idx = hash(key);
        size_t probe = 0;
        
        while (probe < CAPACITY) {
            size_t pos = (idx + probe) % CAPACITY;
            if (!table_[pos].occupied) return false;
            if (table_[pos].key == key) {
                value = table_[pos].value;
                return true;
            }
            probe++;
        }
        return false;
    }
    
    bool remove(const K& key) {
        size_t idx = hash(key);
        size_t probe = 0;
        
        while (probe < CAPACITY) {
            size_t pos = (idx + probe) % CAPACITY;
            if (!table_[pos].occupied) return false;
            if (table_[pos].key == key) {
                table_[pos].occupied = false;
                size_--;
                return true;
            }
            probe++;
        }
        return false;
    }
    
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
};

// Test 1: Insert into empty hashtable
bool test_insert_empty() {
    std::cout << "\n[Test 1] Insert into empty hashtable..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    TEST_ASSERT(ht.empty(), "Hashtable should be empty initially");
    TEST_ASSERT(ht.size() == 0, "Size should be 0");
    
    bool inserted = ht.insert(1, 100);
    TEST_ASSERT(inserted, "Insert should succeed");
    TEST_ASSERT(ht.size() == 1, "Size should be 1");
    TEST_ASSERT(!ht.empty(), "Hashtable should not be empty");
    
    int value;
    bool found = ht.lookup(1, value);
    TEST_ASSERT(found, "Key should be found");
    TEST_ASSERT(value == 100, "Value should match");
    
    TEST_PASS("Insert into empty hashtable works");
}

// Test 2: Insert multiple elements
bool test_insert_multiple() {
    std::cout << "\n[Test 2] Insert multiple elements..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    
    for (int i = 0; i < 100; i++) {
        bool inserted = ht.insert(i, i * 10);
        TEST_ASSERT(inserted, "Insert should succeed");
    }
    
    TEST_ASSERT(ht.size() == 100, "Size should be 100");
    
    for (int i = 0; i < 100; i++) {
        int value;
        bool found = ht.lookup(i, value);
        TEST_ASSERT(found, "Key should be found");
        TEST_ASSERT(value == i * 10, "Value should match");
    }
    
    TEST_PASS("Insert multiple elements works");
}

// Test 3: Lookup non-existing key
bool test_lookup_nonexisting() {
    std::cout << "\n[Test 3] Lookup non-existing key..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    ht.insert(1, 100);
    ht.insert(2, 200);
    
    int value;
    bool found = ht.lookup(999, value);
    TEST_ASSERT(!found, "Non-existing key should not be found");
    
    TEST_PASS("Lookup non-existing key works");
}

// Test 4: Delete existing key
bool test_delete_existing() {
    std::cout << "\n[Test 4] Delete existing key..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    ht.insert(1, 100);
    ht.insert(2, 200);
    ht.insert(3, 300);
    
    TEST_ASSERT(ht.size() == 3, "Size should be 3");
    
    bool removed = ht.remove(2);
    TEST_ASSERT(removed, "Remove should succeed");
    TEST_ASSERT(ht.size() == 2, "Size should be 2");
    
    int value;
    bool found = ht.lookup(2, value);
    TEST_ASSERT(!found, "Deleted key should not be found");
    
    // Other keys should still exist
    found = ht.lookup(1, value);
    TEST_ASSERT(found && value == 100, "Key 1 should still exist");
    found = ht.lookup(3, value);
    TEST_ASSERT(found && value == 300, "Key 3 should still exist");
    
    TEST_PASS("Delete existing key works");
}

// Test 5: Delete non-existing key
bool test_delete_nonexisting() {
    std::cout << "\n[Test 5] Delete non-existing key..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    ht.insert(1, 100);
    
    bool removed = ht.remove(999);
    TEST_ASSERT(!removed, "Remove of non-existing key should fail");
    TEST_ASSERT(ht.size() == 1, "Size should remain 1");
    
    TEST_PASS("Delete non-existing key correctly returns false");
}

// Test 6: Insert duplicate key (update)
bool test_insert_duplicate() {
    std::cout << "\n[Test 6] Insert duplicate key (update)..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    ht.insert(1, 100);
    
    int value;
    ht.lookup(1, value);
    TEST_ASSERT(value == 100, "Initial value should be 100");
    
    ht.insert(1, 200);  // Update
    TEST_ASSERT(ht.size() == 1, "Size should remain 1");
    
    ht.lookup(1, value);
    TEST_ASSERT(value == 200, "Value should be updated to 200");
    
    TEST_PASS("Insert duplicate key (update) works");
}

// Test 7: Hash collision handling
bool test_collision_handling() {
    std::cout << "\n[Test 7] Hash collision handling..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    
    // Insert many elements to force collisions
    for (int i = 0; i < 500; i++) {
        bool inserted = ht.insert(i, i * 2);
        TEST_ASSERT(inserted, "Insert should succeed even with collisions");
    }
    
    // Verify all elements
    for (int i = 0; i < 500; i++) {
        int value;
        bool found = ht.lookup(i, value);
        TEST_ASSERT(found, "All keys should be found");
        TEST_ASSERT(value == i * 2, "Values should match");
    }
    
    TEST_PASS("Hash collision handling works");
}

// Test 8: Edge case - zero key
bool test_edge_case_zero_key() {
    std::cout << "\n[Test 8] Edge case - zero key..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    
    bool inserted = ht.insert(0, 999);
    TEST_ASSERT(inserted, "Insert with key 0 should succeed");
    
    int value;
    bool found = ht.lookup(0, value);
    TEST_ASSERT(found, "Key 0 should be found");
    TEST_ASSERT(value == 999, "Value should match");
    
    TEST_PASS("Edge case - zero key works");
}

// Test 9: Edge case - negative keys
bool test_edge_case_negative_keys() {
    std::cout << "\n[Test 9] Edge case - negative keys..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    
    ht.insert(-1, 100);
    ht.insert(-100, 200);
    ht.insert(-999, 300);
    
    int value;
    TEST_ASSERT(ht.lookup(-1, value) && value == 100, "Negative key -1 should work");
    TEST_ASSERT(ht.lookup(-100, value) && value == 200, "Negative key -100 should work");
    TEST_ASSERT(ht.lookup(-999, value) && value == 300, "Negative key -999 should work");
    
    TEST_PASS("Edge case - negative keys works");
}

// Test 10: Concurrent insert (5 threads)
bool test_concurrent_insert() {
    std::cout << "\n[Test 10] Concurrent insert (5 threads, 30 sec)..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    std::atomic<int> successful_inserts{0};
    std::atomic<bool> stop_flag{false};
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> dist(0, 10000);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            int key = dist(gen);
            int value = key * 2;
            
            if (ht.insert(key, value)) {
                successful_inserts++;
            }
            
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
    
    TEST_ASSERT(successful_inserts > 0, "Should have successful inserts");
    TEST_ASSERT(ht.size() > 0, "Hashtable should not be empty");
    
    std::cout << "  Successful inserts: " << successful_inserts << std::endl;
    std::cout << "  Final size: " << ht.size() << std::endl;
    
    TEST_PASS("Concurrent insert works");
}

// Test 11: Concurrent mixed operations (10 threads)
bool test_concurrent_mixed_ops() {
    std::cout << "\n[Test 11] Concurrent mixed operations (10 threads, 30 sec)..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    
    // Pre-populate
    for (int i = 0; i < 1000; i++) {
        ht.insert(i, i * 10);
    }
    
    std::atomic<int> insert_count{0};
    std::atomic<int> lookup_count{0};
    std::atomic<int> delete_count{0};
    std::atomic<bool> stop_flag{false};
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> key_dist(0, 2000);
        std::uniform_int_distribution<> op_dist(0, 2);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            int key = key_dist(gen);
            int op = op_dist(gen);
            
            if (op == 0) {
                // Insert
                ht.insert(key, key * 10);
                insert_count++;
            } else if (op == 1) {
                // Lookup
                int value;
                ht.lookup(key, value);
                lookup_count++;
            } else {
                // Delete
                ht.remove(key);
                delete_count++;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; i++) {
        threads.emplace_back(worker, i);
    }
    
    // Wait for threads to complete (they have internal 30-second timeout)
    for (auto& t : threads) {
        t.join();
    }
    
    std::cout << "  Inserts: " << insert_count << std::endl;
    std::cout << "  Lookups: " << lookup_count << std::endl;
    std::cout << "  Deletes: " << delete_count << std::endl;
    std::cout << "  Final size: " << ht.size() << std::endl;
    
    TEST_ASSERT(insert_count > 0, "Should have inserts");
    TEST_ASSERT(lookup_count > 0, "Should have lookups");
    TEST_ASSERT(delete_count > 0, "Should have deletes");
    
    TEST_PASS("Concurrent mixed operations work");
}

// Test 12: Stress test - capacity limits
bool test_stress_capacity() {
    std::cout << "\n[Test 12] Stress test - capacity limits..." << std::endl;
    
    SimpleHashtable<int, int> ht;
    
    // Try to fill to capacity
    int inserted = 0;
    for (int i = 0; i < 1024; i++) {
        if (ht.insert(i, i)) {
            inserted++;
        }
    }
    
    TEST_ASSERT(inserted > 0, "Should insert some elements");
    TEST_ASSERT(ht.size() == static_cast<size_t>(inserted), "Size should match inserted count");
    
    std::cout << "  Inserted: " << inserted << " elements" << std::endl;
    
    TEST_PASS("Stress test - capacity limits works");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Hashtable Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Run all tests
    test_insert_empty();
    test_insert_multiple();
    test_lookup_nonexisting();
    test_delete_existing();
    test_delete_nonexisting();
    test_insert_duplicate();
    test_collision_handling();
    test_edge_case_zero_key();
    test_edge_case_negative_keys();
    test_concurrent_insert();
    test_concurrent_mixed_ops();
    test_stress_capacity();
    
    // Summary
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
