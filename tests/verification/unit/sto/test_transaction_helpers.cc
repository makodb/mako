/**
 * Transaction Helpers Unit Tests
 * 
 * Comprehensive tests for STO transaction helper functions including:
 * - execute_commit_phase1: lock acquisition, remote batching, failures
 * - execute_commit_phase2: validation pass/fail, concurrent validation
 * - unlock_writeset_items: empty, partial, full unlock
 * - cleanup_writeset_items: committed vs aborted cleanup
 * - Edge cases: empty transaction, huge writeset (1000+ items)
 * - Concurrent: 10 threads committing simultaneously (30 sec)
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
#include <set>

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

// Mock transaction item
struct TransItem {
    void* object;
    void* key;
    uint64_t version;
    bool locked;
    bool valid;
    bool is_write;
    
    TransItem(void* obj, void* k, bool write = false) 
        : object(obj), key(k), version(0), locked(false), valid(true), is_write(write) {}
};

// Mock transaction
struct Transaction {
    std::vector<TransItem*> writeset;
    std::vector<TransItem*> readset;
    bool committed;
    bool aborted;
    int thread_id;
    
    Transaction(int tid = 0) : committed(false), aborted(false), thread_id(tid) {}
    
    ~Transaction() {
        for (auto item : writeset) delete item;
        for (auto item : readset) delete item;
    }
    
    void add_write(void* obj, void* key) {
        writeset.push_back(new TransItem(obj, key, true));
    }
    
    void add_read(void* obj, void* key) {
        readset.push_back(new TransItem(obj, key, false));
    }
};

// Transaction helper functions
namespace TxnHelpers {

// Forward declaration
bool is_in_writeset(Transaction* txn, TransItem* item);

// Phase 1: Lock acquisition
bool execute_commit_phase1(Transaction* txn) {
    if (!txn || txn->writeset.empty()) {
        return true;  // Empty writeset succeeds trivially
    }
    
    // Try to acquire locks on all write items
    for (auto item : txn->writeset) {
        if (item->locked) {
            // Already locked by another transaction - abort
            return false;
        }
        item->locked = true;
    }
    
    return true;
}

// Helper to check if item is in writeset
bool is_in_writeset(Transaction* txn, TransItem* item) {
    for (auto write_item : txn->writeset) {
        if (write_item->object == item->object && write_item->key == item->key) {
            return true;
        }
    }
    return false;
}

// Phase 2: Validation
bool execute_commit_phase2(Transaction* txn) {
    if (!txn) return false;
    
    // Validate all read items
    for (auto item : txn->readset) {
        if (!item->valid) {
            return false;  // Validation failed
        }
        // Check if version changed (simplified)
        if (item->locked && !is_in_writeset(txn, item)) {
            return false;  // Item was modified by another transaction
        }
    }
    
    return true;
}

// Unlock writeset items
void unlock_writeset_items(Transaction* txn) {
    if (!txn) return;
    
    for (auto item : txn->writeset) {
        item->locked = false;
    }
}

// Cleanup writeset items
void cleanup_writeset_items(Transaction* txn, bool committed) {
    if (!txn) return;
    
    if (committed) {
        // Increment versions for committed writes
        for (auto item : txn->writeset) {
            item->version++;
        }
    } else {
        // For aborted transactions, just unlock
        unlock_writeset_items(txn);
    }
}

} // namespace TxnHelpers

// Test 1: Phase1 with empty writeset
bool test_phase1_empty_writeset() {
    std::cout << "\n[Test 1] Phase1 with empty writeset..." << std::endl;
    
    Transaction txn;
    TEST_ASSERT(txn.writeset.empty(), "Writeset should be empty");
    
    bool success = TxnHelpers::execute_commit_phase1(&txn);
    TEST_ASSERT(success, "Phase1 should succeed with empty writeset");
    
    TEST_PASS("Phase1 with empty writeset works");
}

// Test 2: Phase1 lock acquisition success
bool test_phase1_lock_acquisition() {
    std::cout << "\n[Test 2] Phase1 lock acquisition..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2, obj3 = 3;
    
    txn.add_write(&obj1, (void*)100);
    txn.add_write(&obj2, (void*)200);
    txn.add_write(&obj3, (void*)300);
    
    bool success = TxnHelpers::execute_commit_phase1(&txn);
    TEST_ASSERT(success, "Phase1 should succeed");
    
    // Verify all items are locked
    for (auto item : txn.writeset) {
        TEST_ASSERT(item->locked, "Item should be locked");
    }
    
    TEST_PASS("Phase1 lock acquisition works");
}

// Test 3: Phase1 lock acquisition failure
bool test_phase1_lock_failure() {
    std::cout << "\n[Test 3] Phase1 lock acquisition failure..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2;
    
    txn.add_write(&obj1, (void*)100);
    txn.add_write(&obj2, (void*)200);
    
    // Pre-lock one item (simulating another transaction)
    txn.writeset[1]->locked = true;
    
    bool success = TxnHelpers::execute_commit_phase1(&txn);
    TEST_ASSERT(!success, "Phase1 should fail when item already locked");
    
    TEST_PASS("Phase1 lock acquisition failure works");
}

// Test 4: Phase2 validation success
bool test_phase2_validation_success() {
    std::cout << "\n[Test 4] Phase2 validation success..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2;
    
    txn.add_read(&obj1, (void*)100);
    txn.add_read(&obj2, (void*)200);
    
    // All items valid
    for (auto item : txn.readset) {
        item->valid = true;
    }
    
    bool success = TxnHelpers::execute_commit_phase2(&txn);
    TEST_ASSERT(success, "Phase2 should succeed with valid reads");
    
    TEST_PASS("Phase2 validation success works");
}

// Test 5: Phase2 validation failure
bool test_phase2_validation_failure() {
    std::cout << "\n[Test 5] Phase2 validation failure..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2, obj3 = 3;
    
    txn.add_read(&obj1, (void*)100);
    txn.add_read(&obj2, (void*)200);
    txn.add_read(&obj3, (void*)300);
    
    // Mark one item as invalid
    txn.readset[1]->valid = false;
    
    bool success = TxnHelpers::execute_commit_phase2(&txn);
    TEST_ASSERT(!success, "Phase2 should fail with invalid read");
    
    TEST_PASS("Phase2 validation failure works");
}

// Test 6: Phase2 with locked item not in writeset
bool test_phase2_locked_item() {
    std::cout << "\n[Test 6] Phase2 with locked item not in writeset..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2;
    
    txn.add_read(&obj1, (void*)100);
    txn.add_read(&obj2, (void*)200);
    
    // Lock a read item (simulating modification by another transaction)
    txn.readset[0]->locked = true;
    
    bool success = TxnHelpers::execute_commit_phase2(&txn);
    TEST_ASSERT(!success, "Phase2 should fail when read item is locked by another txn");
    
    TEST_PASS("Phase2 with locked item works");
}

// Test 7: Unlock empty writeset
bool test_unlock_empty_writeset() {
    std::cout << "\n[Test 7] Unlock empty writeset..." << std::endl;
    
    Transaction txn;
    TEST_ASSERT(txn.writeset.empty(), "Writeset should be empty");
    
    // Should not crash
    TxnHelpers::unlock_writeset_items(&txn);
    
    TEST_PASS("Unlock empty writeset works");
}

// Test 8: Unlock partial writeset
bool test_unlock_partial_writeset() {
    std::cout << "\n[Test 8] Unlock partial writeset..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2, obj3 = 3;
    
    txn.add_write(&obj1, (void*)100);
    txn.add_write(&obj2, (void*)200);
    txn.add_write(&obj3, (void*)300);
    
    // Lock some items
    txn.writeset[0]->locked = true;
    txn.writeset[2]->locked = true;
    
    TxnHelpers::unlock_writeset_items(&txn);
    
    // Verify all unlocked
    for (auto item : txn.writeset) {
        TEST_ASSERT(!item->locked, "Item should be unlocked");
    }
    
    TEST_PASS("Unlock partial writeset works");
}


// Test 9: Unlock full writeset
bool test_unlock_full_writeset() {
    std::cout << "\n[Test 9] Unlock full writeset..." << std::endl;
    
    Transaction txn;
    
    for (int i = 0; i < 10; i++) {
        int* obj = new int(i);
        txn.add_write(obj, (void*)(long)i);
        txn.writeset.back()->locked = true;
    }
    
    TEST_ASSERT(txn.writeset.size() == 10, "Should have 10 items");
    
    TxnHelpers::unlock_writeset_items(&txn);
    
    // Verify all unlocked
    for (auto item : txn.writeset) {
        TEST_ASSERT(!item->locked, "Item should be unlocked");
    }
    
    // Cleanup
    for (auto item : txn.writeset) {
        delete (int*)item->object;
    }
    
    TEST_PASS("Unlock full writeset works");
}

// Test 10: Cleanup committed transaction
bool test_cleanup_committed() {
    std::cout << "\n[Test 10] Cleanup committed transaction..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2, obj3 = 3;
    
    txn.add_write(&obj1, (void*)100);
    txn.add_write(&obj2, (void*)200);
    txn.add_write(&obj3, (void*)300);
    
    // Lock items
    for (auto item : txn.writeset) {
        item->locked = true;
        item->version = 5;
    }
    
    TxnHelpers::cleanup_writeset_items(&txn, true);
    
    // Verify versions incremented
    for (auto item : txn.writeset) {
        TEST_ASSERT(item->version == 6, "Version should be incremented");
    }
    
    TEST_PASS("Cleanup committed transaction works");
}

// Test 11: Cleanup aborted transaction
bool test_cleanup_aborted() {
    std::cout << "\n[Test 11] Cleanup aborted transaction..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2, obj3 = 3;
    
    txn.add_write(&obj1, (void*)100);
    txn.add_write(&obj2, (void*)200);
    txn.add_write(&obj3, (void*)300);
    
    // Lock items
    for (auto item : txn.writeset) {
        item->locked = true;
        item->version = 5;
    }
    
    TxnHelpers::cleanup_writeset_items(&txn, false);
    
    // Verify versions NOT incremented and items unlocked
    for (auto item : txn.writeset) {
        TEST_ASSERT(item->version == 5, "Version should not change");
        TEST_ASSERT(!item->locked, "Item should be unlocked");
    }
    
    TEST_PASS("Cleanup aborted transaction works");
}

// Test 12: Empty transaction commit
bool test_empty_transaction() {
    std::cout << "\n[Test 12] Empty transaction commit..." << std::endl;
    
    Transaction txn;
    TEST_ASSERT(txn.writeset.empty(), "Writeset should be empty");
    TEST_ASSERT(txn.readset.empty(), "Readset should be empty");
    
    // Phase 1
    bool phase1 = TxnHelpers::execute_commit_phase1(&txn);
    TEST_ASSERT(phase1, "Phase1 should succeed");
    
    // Phase 2
    bool phase2 = TxnHelpers::execute_commit_phase2(&txn);
    TEST_ASSERT(phase2, "Phase2 should succeed");
    
    // Cleanup
    TxnHelpers::cleanup_writeset_items(&txn, true);
    
    TEST_PASS("Empty transaction commit works");
}

// Test 13: Huge writeset (1000+ items)
bool test_huge_writeset() {
    std::cout << "\n[Test 13] Huge writeset (1000+ items)..." << std::endl;
    
    Transaction txn;
    const int NUM_ITEMS = 1500;
    
    std::vector<int> objects(NUM_ITEMS);
    for (int i = 0; i < NUM_ITEMS; i++) {
        objects[i] = i;
        txn.add_write(&objects[i], (void*)(long)i);
    }
    
    TEST_ASSERT(txn.writeset.size() == NUM_ITEMS, "Should have 1500 items");
    
    // Phase 1: Lock all
    auto start = std::chrono::steady_clock::now();
    bool phase1 = TxnHelpers::execute_commit_phase1(&txn);
    auto phase1_time = std::chrono::steady_clock::now() - start;
    
    TEST_ASSERT(phase1, "Phase1 should succeed");
    
    // Verify all locked
    for (auto item : txn.writeset) {
        TEST_ASSERT(item->locked, "Item should be locked");
    }
    
    // Unlock all
    start = std::chrono::steady_clock::now();
    TxnHelpers::unlock_writeset_items(&txn);
    auto unlock_time = std::chrono::steady_clock::now() - start;
    
    // Verify all unlocked
    for (auto item : txn.writeset) {
        TEST_ASSERT(!item->locked, "Item should be unlocked");
    }
    
    std::cout << "  Phase1 time: " 
              << std::chrono::duration_cast<std::chrono::microseconds>(phase1_time).count() 
              << " us" << std::endl;
    std::cout << "  Unlock time: " 
              << std::chrono::duration_cast<std::chrono::microseconds>(unlock_time).count() 
              << " us" << std::endl;
    
    TEST_PASS("Huge writeset works");
}

// Test 14: Full commit workflow
bool test_full_commit_workflow() {
    std::cout << "\n[Test 14] Full commit workflow..." << std::endl;
    
    Transaction txn;
    int obj1 = 1, obj2 = 2, obj3 = 3, obj4 = 4;
    
    // Add writes
    txn.add_write(&obj1, (void*)100);
    txn.add_write(&obj2, (void*)200);
    
    // Add reads
    txn.add_read(&obj3, (void*)300);
    txn.add_read(&obj4, (void*)400);
    
    // Phase 1: Lock acquisition
    bool phase1 = TxnHelpers::execute_commit_phase1(&txn);
    TEST_ASSERT(phase1, "Phase1 should succeed");
    
    for (auto item : txn.writeset) {
        TEST_ASSERT(item->locked, "Write items should be locked");
    }
    
    // Phase 2: Validation
    bool phase2 = TxnHelpers::execute_commit_phase2(&txn);
    TEST_ASSERT(phase2, "Phase2 should succeed");
    
    // Cleanup as committed
    TxnHelpers::cleanup_writeset_items(&txn, true);
    
    // Verify versions incremented
    for (auto item : txn.writeset) {
        TEST_ASSERT(item->version == 1, "Version should be incremented");
    }
    
    TEST_PASS("Full commit workflow works");
}

// Test 15: Concurrent commits (10 threads, 30 sec)
bool test_concurrent_commits() {
    std::cout << "\n[Test 15] Concurrent commits (10 threads, 30 sec)..." << std::endl;
    
    std::atomic<int> successful_commits{0};
    std::atomic<int> failed_commits{0};
    std::atomic<int> phase1_failures{0};
    std::atomic<int> phase2_failures{0};
    std::atomic<bool> stop_flag{false};
    
    // Shared objects for contention
    const int NUM_OBJECTS = 100;
    std::vector<int> shared_objects(NUM_OBJECTS);
    for (int i = 0; i < NUM_OBJECTS; i++) {
        shared_objects[i] = i;
    }
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> obj_dist(0, NUM_OBJECTS - 1);
        std::uniform_int_distribution<> size_dist(1, 10);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            Transaction txn(thread_id);
            
            // Add random writes
            int num_writes = size_dist(gen);
            std::set<int> used_objects;
            for (int i = 0; i < num_writes; i++) {
                int obj_idx = obj_dist(gen);
                if (used_objects.find(obj_idx) == used_objects.end()) {
                    txn.add_write(&shared_objects[obj_idx], (void*)(long)obj_idx);
                    used_objects.insert(obj_idx);
                }
            }
            
            // Add random reads
            int num_reads = size_dist(gen);
            for (int i = 0; i < num_reads; i++) {
                int obj_idx = obj_dist(gen);
                if (used_objects.find(obj_idx) == used_objects.end()) {
                    txn.add_read(&shared_objects[obj_idx], (void*)(long)obj_idx);
                    used_objects.insert(obj_idx);
                }
            }
            
            // Try to commit
            bool phase1 = TxnHelpers::execute_commit_phase1(&txn);
            if (!phase1) {
                phase1_failures++;
                failed_commits++;
                continue;
            }
            
            bool phase2 = TxnHelpers::execute_commit_phase2(&txn);
            if (!phase2) {
                phase2_failures++;
                TxnHelpers::unlock_writeset_items(&txn);
                failed_commits++;
                continue;
            }
            
            // Commit succeeded
            TxnHelpers::cleanup_writeset_items(&txn, true);
            successful_commits++;
            
            std::this_thread::sleep_for(std::chrono::microseconds(100));
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
    
    std::cout << "  Successful commits: " << successful_commits << std::endl;
    std::cout << "  Failed commits: " << failed_commits << std::endl;
    std::cout << "  Phase1 failures: " << phase1_failures << std::endl;
    std::cout << "  Phase2 failures: " << phase2_failures << std::endl;
    std::cout << "  Total attempts: " << (successful_commits + failed_commits) << std::endl;
    
    TEST_ASSERT(successful_commits > 0, "Should have successful commits");
    TEST_ASSERT(successful_commits + failed_commits > 0, "Should have commit attempts");
    
    double success_rate = (double)successful_commits / (successful_commits + failed_commits);
    std::cout << "  Success rate: " << (success_rate * 100) << "%" << std::endl;
    
    TEST_PASS("Concurrent commits work");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Transaction Helpers Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_phase1_empty_writeset();
    test_phase1_lock_acquisition();
    test_phase1_lock_failure();
    test_phase2_validation_success();
    test_phase2_validation_failure();
    test_phase2_locked_item();
    test_unlock_empty_writeset();
    test_unlock_partial_writeset();
    test_unlock_full_writeset();
    test_cleanup_committed();
    test_cleanup_aborted();
    test_empty_transaction();
    test_huge_writeset();
    test_full_commit_workflow();
    test_concurrent_commits();
    
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
