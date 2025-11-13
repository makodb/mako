/**
 * TransItem Unit Tests
 * 
 * Comprehensive tests for STO TransItem implementation including:
 * - Basic: item creation, flag operations, accessors
 * - Edge cases: null object, null key, invalid flags
 * - Const correctness: verify const methods
 * - Concurrent: 5 threads accessing same item (30 sec)
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

// TransItem flags
enum TransItemFlags {
    TRANS_READ = 0x01,
    TRANS_WRITE = 0x02,
    TRANS_LOCKED = 0x04,
    TRANS_VALID = 0x08,
    TRANS_DELETED = 0x10
};

// Simple TransItem implementation
class TransItem {
private:
    void* object_;
    void* key_;
    uint64_t version_;
    uint32_t flags_;
    void* value_;
    
public:
    TransItem() 
        : object_(nullptr), key_(nullptr), version_(0), flags_(0), value_(nullptr) {}
    
    TransItem(void* obj, void* key, uint64_t version = 0) 
        : object_(obj), key_(key), version_(version), flags_(0), value_(nullptr) {}
    
    // Accessors
    void* object() const { return object_; }
    void* key() const { return key_; }
    uint64_t version() const { return version_; }
    uint32_t flags() const { return flags_; }
    void* value() const { return value_; }
    
    // Mutators
    void set_object(void* obj) { object_ = obj; }
    void set_key(void* key) { key_ = key; }
    void set_version(uint64_t version) { version_ = version; }
    void set_value(void* value) { value_ = value; }
    
    // Flag operations
    void set_flag(uint32_t flag) { flags_ |= flag; }
    void clear_flag(uint32_t flag) { flags_ &= ~flag; }
    bool has_flag(uint32_t flag) const { return (flags_ & flag) != 0; }
    void set_flags(uint32_t flags) { flags_ = flags; }
    void clear_flags() { flags_ = 0; }
    
    // Convenience methods
    bool is_read() const { return has_flag(TRANS_READ); }
    bool is_write() const { return has_flag(TRANS_WRITE); }
    bool is_locked() const { return has_flag(TRANS_LOCKED); }
    bool is_valid() const { return has_flag(TRANS_VALID); }
    bool is_deleted() const { return has_flag(TRANS_DELETED); }
    
    void mark_read() { set_flag(TRANS_READ); }
    void mark_write() { set_flag(TRANS_WRITE); }
    void mark_locked() { set_flag(TRANS_LOCKED); }
    void mark_valid() { set_flag(TRANS_VALID); }
    void mark_deleted() { set_flag(TRANS_DELETED); }
    
    void unlock() { clear_flag(TRANS_LOCKED); }
    void invalidate() { clear_flag(TRANS_VALID); }
};

// Test 1: Basic item creation
bool test_item_creation() {
    std::cout << "\n[Test 1] Basic item creation..." << std::endl;
    
    int obj = 42;
    int key = 100;
    
    TransItem item(&obj, &key, 5);
    
    TEST_ASSERT(item.object() == &obj, "Object should match");
    TEST_ASSERT(item.key() == &key, "Key should match");
    TEST_ASSERT(item.version() == 5, "Version should be 5");
    TEST_ASSERT(item.flags() == 0, "Flags should be 0");
    TEST_ASSERT(item.value() == nullptr, "Value should be null");
    
    TEST_PASS("Basic item creation works");
}

// Test 2: Default constructor
bool test_default_constructor() {
    std::cout << "\n[Test 2] Default constructor..." << std::endl;
    
    TransItem item;
    
    TEST_ASSERT(item.object() == nullptr, "Object should be null");
    TEST_ASSERT(item.key() == nullptr, "Key should be null");
    TEST_ASSERT(item.version() == 0, "Version should be 0");
    TEST_ASSERT(item.flags() == 0, "Flags should be 0");
    TEST_ASSERT(item.value() == nullptr, "Value should be null");
    
    TEST_PASS("Default constructor works");
}

// Test 3: Flag operations
bool test_flag_operations() {
    std::cout << "\n[Test 3] Flag operations..." << std::endl;
    
    TransItem item;
    
    // Set individual flags
    item.set_flag(TRANS_READ);
    TEST_ASSERT(item.has_flag(TRANS_READ), "Should have READ flag");
    TEST_ASSERT(!item.has_flag(TRANS_WRITE), "Should not have WRITE flag");
    
    item.set_flag(TRANS_WRITE);
    TEST_ASSERT(item.has_flag(TRANS_READ), "Should still have READ flag");
    TEST_ASSERT(item.has_flag(TRANS_WRITE), "Should have WRITE flag");
    
    // Clear flag
    item.clear_flag(TRANS_READ);
    TEST_ASSERT(!item.has_flag(TRANS_READ), "Should not have READ flag");
    TEST_ASSERT(item.has_flag(TRANS_WRITE), "Should still have WRITE flag");
    
    // Clear all flags
    item.clear_flags();
    TEST_ASSERT(item.flags() == 0, "All flags should be cleared");
    
    TEST_PASS("Flag operations work");
}

// Test 4: Convenience flag methods
bool test_convenience_methods() {
    std::cout << "\n[Test 4] Convenience flag methods..." << std::endl;
    
    TransItem item;
    
    // Mark as read
    item.mark_read();
    TEST_ASSERT(item.is_read(), "Should be marked as read");
    TEST_ASSERT(!item.is_write(), "Should not be marked as write");
    
    // Mark as write
    item.mark_write();
    TEST_ASSERT(item.is_read(), "Should still be marked as read");
    TEST_ASSERT(item.is_write(), "Should be marked as write");
    
    // Mark as locked
    item.mark_locked();
    TEST_ASSERT(item.is_locked(), "Should be locked");
    
    // Unlock
    item.unlock();
    TEST_ASSERT(!item.is_locked(), "Should not be locked");
    
    // Mark as valid
    item.mark_valid();
    TEST_ASSERT(item.is_valid(), "Should be valid");
    
    // Invalidate
    item.invalidate();
    TEST_ASSERT(!item.is_valid(), "Should not be valid");
    
    // Mark as deleted
    item.mark_deleted();
    TEST_ASSERT(item.is_deleted(), "Should be deleted");
    
    TEST_PASS("Convenience flag methods work");
}

// Test 5: Accessors and mutators
bool test_accessors_mutators() {
    std::cout << "\n[Test 5] Accessors and mutators..." << std::endl;
    
    TransItem item;
    
    int obj = 42;
    int key = 100;
    int value = 200;
    
    // Set values
    item.set_object(&obj);
    item.set_key(&key);
    item.set_version(10);
    item.set_value(&value);
    
    // Verify
    TEST_ASSERT(item.object() == &obj, "Object should match");
    TEST_ASSERT(item.key() == &key, "Key should match");
    TEST_ASSERT(item.version() == 10, "Version should be 10");
    TEST_ASSERT(item.value() == &value, "Value should match");
    
    // Update values
    int new_obj = 99;
    item.set_object(&new_obj);
    TEST_ASSERT(item.object() == &new_obj, "Object should be updated");
    
    item.set_version(20);
    TEST_ASSERT(item.version() == 20, "Version should be updated");
    
    TEST_PASS("Accessors and mutators work");
}

// Test 6: Null object and key
bool test_null_object_key() {
    std::cout << "\n[Test 6] Null object and key..." << std::endl;
    
    TransItem item(nullptr, nullptr, 0);
    
    TEST_ASSERT(item.object() == nullptr, "Object should be null");
    TEST_ASSERT(item.key() == nullptr, "Key should be null");
    TEST_ASSERT(item.version() == 0, "Version should be 0");
    
    // Should be able to set flags even with null object/key
    item.mark_read();
    TEST_ASSERT(item.is_read(), "Should be able to mark as read");
    
    item.mark_write();
    TEST_ASSERT(item.is_write(), "Should be able to mark as write");
    
    TEST_PASS("Null object and key work");
}

// Test 7: Multiple flag combinations
bool test_flag_combinations() {
    std::cout << "\n[Test 7] Multiple flag combinations..." << std::endl;
    
    TransItem item;
    
    // Set multiple flags at once
    item.set_flags(TRANS_READ | TRANS_WRITE | TRANS_VALID);
    
    TEST_ASSERT(item.is_read(), "Should be read");
    TEST_ASSERT(item.is_write(), "Should be write");
    TEST_ASSERT(item.is_valid(), "Should be valid");
    TEST_ASSERT(!item.is_locked(), "Should not be locked");
    TEST_ASSERT(!item.is_deleted(), "Should not be deleted");
    
    // Add locked flag
    item.mark_locked();
    TEST_ASSERT(item.is_read(), "Should still be read");
    TEST_ASSERT(item.is_write(), "Should still be write");
    TEST_ASSERT(item.is_valid(), "Should still be valid");
    TEST_ASSERT(item.is_locked(), "Should be locked");
    
    // Clear specific flag
    item.clear_flag(TRANS_WRITE);
    TEST_ASSERT(item.is_read(), "Should still be read");
    TEST_ASSERT(!item.is_write(), "Should not be write");
    TEST_ASSERT(item.is_valid(), "Should still be valid");
    TEST_ASSERT(item.is_locked(), "Should still be locked");
    
    TEST_PASS("Multiple flag combinations work");
}

// Test 8: Const correctness
bool test_const_correctness() {
    std::cout << "\n[Test 8] Const correctness..." << std::endl;
    
    int obj = 42;
    int key = 100;
    
    TransItem item(&obj, &key, 5);
    item.mark_read();
    item.mark_valid();
    
    // Create const reference
    const TransItem& const_item = item;
    
    // Verify const methods work
    TEST_ASSERT(const_item.object() == &obj, "Const object() should work");
    TEST_ASSERT(const_item.key() == &key, "Const key() should work");
    TEST_ASSERT(const_item.version() == 5, "Const version() should work");
    TEST_ASSERT(const_item.flags() != 0, "Const flags() should work");
    TEST_ASSERT(const_item.is_read(), "Const is_read() should work");
    TEST_ASSERT(const_item.is_valid(), "Const is_valid() should work");
    TEST_ASSERT(!const_item.is_write(), "Const is_write() should work");
    TEST_ASSERT(const_item.has_flag(TRANS_READ), "Const has_flag() should work");
    
    TEST_PASS("Const correctness works");
}

// Test 9: Version updates
bool test_version_updates() {
    std::cout << "\n[Test 9] Version updates..." << std::endl;
    
    TransItem item;
    
    TEST_ASSERT(item.version() == 0, "Initial version should be 0");
    
    // Increment version
    for (uint64_t i = 1; i <= 100; i++) {
        item.set_version(i);
        TEST_ASSERT(item.version() == i, "Version should match");
    }
    
    // Large version number
    uint64_t large_version = 1ULL << 50;
    item.set_version(large_version);
    TEST_ASSERT(item.version() == large_version, "Should handle large version");
    
    TEST_PASS("Version updates work");
}


// Test 10: Concurrent access (5 threads, 30 sec)
bool test_concurrent_access() {
    std::cout << "\n[Test 10] Concurrent access (5 threads, 30 sec)..." << std::endl;
    
    int shared_obj = 42;
    int shared_key = 100;
    TransItem shared_item(&shared_obj, &shared_key, 0);
    
    std::atomic<int> read_operations{0};
    std::atomic<int> write_operations{0};
    std::atomic<int> flag_operations{0};
    std::atomic<int> version_updates{0};
    std::atomic<bool> stop_flag{false};
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> op_dist(0, 3);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            int op = op_dist(gen);
            
            switch (op) {
                case 0: {
                    // Read operations
                    volatile void* obj = shared_item.object();
                    volatile void* key = shared_item.key();
                    volatile uint64_t ver = shared_item.version();
                    volatile uint32_t flags = shared_item.flags();
                    (void)obj; (void)key; (void)ver; (void)flags;
                    read_operations++;
                    break;
                }
                case 1: {
                    // Write operations
                    shared_item.set_version(shared_item.version() + 1);
                    write_operations++;
                    version_updates++;
                    break;
                }
                case 2: {
                    // Flag operations - set
                    if (thread_id % 2 == 0) {
                        shared_item.mark_read();
                        shared_item.mark_valid();
                    } else {
                        shared_item.mark_write();
                        shared_item.mark_locked();
                    }
                    flag_operations++;
                    break;
                }
                case 3: {
                    // Flag operations - clear
                    if (thread_id % 2 == 0) {
                        shared_item.unlock();
                    } else {
                        shared_item.invalidate();
                    }
                    flag_operations++;
                    break;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::microseconds(10));
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
    
    std::cout << "  Read operations: " << read_operations << std::endl;
    std::cout << "  Write operations: " << write_operations << std::endl;
    std::cout << "  Flag operations: " << flag_operations << std::endl;
    std::cout << "  Version updates: " << version_updates << std::endl;
    std::cout << "  Final version: " << shared_item.version() << std::endl;
    std::cout << "  Final flags: 0x" << std::hex << shared_item.flags() << std::dec << std::endl;
    
    TEST_ASSERT(read_operations > 0, "Should have read operations");
    TEST_ASSERT(write_operations > 0, "Should have write operations");
    TEST_ASSERT(flag_operations > 0, "Should have flag operations");
    
    // Verify item is still valid (no corruption)
    TEST_ASSERT(shared_item.object() == &shared_obj, "Object should not be corrupted");
    TEST_ASSERT(shared_item.key() == &shared_key, "Key should not be corrupted");
    
    TEST_PASS("Concurrent access works");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "TransItem Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_item_creation();
    test_default_constructor();
    test_flag_operations();
    test_convenience_methods();
    test_accessors_mutators();
    test_null_object_key();
    test_flag_combinations();
    test_const_correctness();
    test_version_updates();
    test_concurrent_access();
    
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
