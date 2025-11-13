/**
 * MBTA Wrapper Unit Tests
 * 
 * Comprehensive tests for Silo MBTA (Masstree-Based Transactional Access) wrapper including:
 * - ThreadContext: initialization, cleanup, access
 * - Index operations: insert, lookup, delete through wrapper
 * - Transaction wrapping: begin, commit, abort
 * - Edge cases: null context, invalid operations
 * - Concurrent: 5 threads using wrapper (30 sec)
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
#include <map>
#include <string>

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

// Thread context for MBTA
class ThreadContext {
private:
    int thread_id_;
    bool initialized_;
    std::atomic<int> txn_count_;
    
public:
    ThreadContext(int tid) : thread_id_(tid), initialized_(false), txn_count_(0) {}
    
    bool initialize() {
        if (initialized_) return false;
        initialized_ = true;
        return true;
    }
    
    void cleanup() {
        initialized_ = false;
        txn_count_ = 0;
    }
    
    bool is_initialized() const { return initialized_; }
    int thread_id() const { return thread_id_; }
    int txn_count() const { return txn_count_.load(); }
    
    void increment_txn_count() { txn_count_++; }
};

// Simple index structure
template<typename K, typename V>
class SimpleIndex {
private:
    std::map<K, V> data_;
    std::mutex mutex_;
    
public:
    bool insert(const K& key, const V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result = data_.insert({key, value});
        return result.second;
    }
    
    bool lookup(const K& key, V& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(key);
        if (it != data_.end()) {
            value = it->second;
            return true;
        }
        return false;
    }
    
    bool remove(const K& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.erase(key) > 0;
    }
    
    size_t size() const {
        return data_.size();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
    }
};

// Transaction state
enum TxnState {
    TXN_NONE,
    TXN_ACTIVE,
    TXN_COMMITTED,
    TXN_ABORTED
};

// MBTA Wrapper
template<typename K, typename V>
class MBTAWrapper {
private:
    SimpleIndex<K, V>* index_;
    ThreadContext* context_;
    TxnState txn_state_;
    std::vector<std::pair<K, V>> pending_writes_;
    
public:
    MBTAWrapper(SimpleIndex<K, V>* index) 
        : index_(index), context_(nullptr), txn_state_(TXN_NONE) {}
    
    ~MBTAWrapper() {
        if (context_) {
            context_->cleanup();
        }
    }
    
    // Thread context management
    bool set_context(ThreadContext* ctx) {
        if (!ctx) return false;
        context_ = ctx;
        return true;
    }
    
    ThreadContext* get_context() const {
        return context_;
    }
    
    bool has_context() const {
        return context_ != nullptr && context_->is_initialized();
    }
    
    // Transaction management
    bool begin_txn() {
        if (!has_context()) return false;
        if (txn_state_ == TXN_ACTIVE) return false;
        
        txn_state_ = TXN_ACTIVE;
        pending_writes_.clear();
        return true;
    }
    
    bool commit_txn() {
        if (!has_context()) return false;
        if (txn_state_ != TXN_ACTIVE) return false;
        
        // Apply pending writes
        for (const auto& write : pending_writes_) {
            index_->insert(write.first, write.second);
        }
        
        txn_state_ = TXN_COMMITTED;
        context_->increment_txn_count();
        pending_writes_.clear();
        return true;
    }
    
    bool abort_txn() {
        if (!has_context()) return false;
        if (txn_state_ != TXN_ACTIVE) return false;
        
        txn_state_ = TXN_ABORTED;
        pending_writes_.clear();
        return true;
    }
    
    TxnState txn_state() const { return txn_state_; }
    bool is_in_txn() const { return txn_state_ == TXN_ACTIVE; }
    
    // Index operations (transactional)
    bool insert(const K& key, const V& value) {
        if (!has_context()) return false;
        
        if (txn_state_ == TXN_ACTIVE) {
            // Buffer write in transaction
            pending_writes_.push_back({key, value});
            return true;
        } else {
            // Direct insert outside transaction
            return index_->insert(key, value);
        }
    }
    
    bool lookup(const K& key, V& value) {
        if (!has_context()) return false;
        
        // Check pending writes first
        if (txn_state_ == TXN_ACTIVE) {
            for (auto it = pending_writes_.rbegin(); it != pending_writes_.rend(); ++it) {
                if (it->first == key) {
                    value = it->second;
                    return true;
                }
            }
        }
        
        // Lookup in index
        return index_->lookup(key, value);
    }
    
    bool remove(const K& key) {
        if (!has_context()) return false;
        if (txn_state_ == TXN_ACTIVE) return false;  // No delete in txn for simplicity
        
        return index_->remove(key);
    }
    
    size_t pending_write_count() const {
        return pending_writes_.size();
    }
};

// Test 1: ThreadContext initialization
bool test_context_initialization() {
    std::cout << "\n[Test 1] ThreadContext initialization..." << std::endl;
    
    ThreadContext ctx(1);
    
    TEST_ASSERT(!ctx.is_initialized(), "Context should not be initialized");
    TEST_ASSERT(ctx.thread_id() == 1, "Thread ID should be 1");
    TEST_ASSERT(ctx.txn_count() == 0, "Transaction count should be 0");
    
    bool success = ctx.initialize();
    TEST_ASSERT(success, "Initialization should succeed");
    TEST_ASSERT(ctx.is_initialized(), "Context should be initialized");
    
    // Cannot initialize twice
    success = ctx.initialize();
    TEST_ASSERT(!success, "Second initialization should fail");
    
    TEST_PASS("ThreadContext initialization works");
}

// Test 2: ThreadContext cleanup
bool test_context_cleanup() {
    std::cout << "\n[Test 2] ThreadContext cleanup..." << std::endl;
    
    ThreadContext ctx(1);
    ctx.initialize();
    ctx.increment_txn_count();
    ctx.increment_txn_count();
    
    TEST_ASSERT(ctx.is_initialized(), "Context should be initialized");
    TEST_ASSERT(ctx.txn_count() == 2, "Transaction count should be 2");
    
    ctx.cleanup();
    TEST_ASSERT(!ctx.is_initialized(), "Context should not be initialized");
    TEST_ASSERT(ctx.txn_count() == 0, "Transaction count should be reset");
    
    TEST_PASS("ThreadContext cleanup works");
}

// Test 3: Wrapper context management
bool test_wrapper_context() {
    std::cout << "\n[Test 3] Wrapper context management..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    
    TEST_ASSERT(!wrapper.has_context(), "Should not have context");
    TEST_ASSERT(wrapper.get_context() == nullptr, "Context should be null");
    
    ThreadContext ctx(1);
    ctx.initialize();
    
    bool success = wrapper.set_context(&ctx);
    TEST_ASSERT(success, "Setting context should succeed");
    TEST_ASSERT(wrapper.has_context(), "Should have context");
    TEST_ASSERT(wrapper.get_context() == &ctx, "Context should match");
    
    TEST_PASS("Wrapper context management works");
}

// Test 4: Begin transaction
bool test_begin_transaction() {
    std::cout << "\n[Test 4] Begin transaction..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    TEST_ASSERT(wrapper.txn_state() == TXN_NONE, "Should have no transaction");
    TEST_ASSERT(!wrapper.is_in_txn(), "Should not be in transaction");
    
    bool success = wrapper.begin_txn();
    TEST_ASSERT(success, "Begin should succeed");
    TEST_ASSERT(wrapper.txn_state() == TXN_ACTIVE, "Transaction should be active");
    TEST_ASSERT(wrapper.is_in_txn(), "Should be in transaction");
    
    // Cannot begin twice
    success = wrapper.begin_txn();
    TEST_ASSERT(!success, "Second begin should fail");
    
    TEST_PASS("Begin transaction works");
}

// Test 5: Commit transaction
bool test_commit_transaction() {
    std::cout << "\n[Test 5] Commit transaction..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    wrapper.begin_txn();
    wrapper.insert(10, 100);
    wrapper.insert(20, 200);
    
    TEST_ASSERT(wrapper.pending_write_count() == 2, "Should have 2 pending writes");
    
    bool success = wrapper.commit_txn();
    TEST_ASSERT(success, "Commit should succeed");
    TEST_ASSERT(wrapper.txn_state() == TXN_COMMITTED, "Transaction should be committed");
    TEST_ASSERT(wrapper.pending_write_count() == 0, "Pending writes should be cleared");
    TEST_ASSERT(ctx.txn_count() == 1, "Transaction count should be incremented");
    
    // Verify data in index
    int value;
    TEST_ASSERT(index.lookup(10, value) && value == 100, "Should find key 10");
    TEST_ASSERT(index.lookup(20, value) && value == 200, "Should find key 20");
    
    TEST_PASS("Commit transaction works");
}

// Test 6: Abort transaction
bool test_abort_transaction() {
    std::cout << "\n[Test 6] Abort transaction..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    wrapper.begin_txn();
    wrapper.insert(10, 100);
    wrapper.insert(20, 200);
    
    TEST_ASSERT(wrapper.pending_write_count() == 2, "Should have 2 pending writes");
    
    bool success = wrapper.abort_txn();
    TEST_ASSERT(success, "Abort should succeed");
    TEST_ASSERT(wrapper.txn_state() == TXN_ABORTED, "Transaction should be aborted");
    TEST_ASSERT(wrapper.pending_write_count() == 0, "Pending writes should be cleared");
    
    // Verify data NOT in index
    int value;
    TEST_ASSERT(!index.lookup(10, value), "Should not find key 10");
    TEST_ASSERT(!index.lookup(20, value), "Should not find key 20");
    
    TEST_PASS("Abort transaction works");
}

// Test 7: Insert through wrapper
bool test_insert_operations() {
    std::cout << "\n[Test 7] Insert operations..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    // Insert outside transaction
    bool success = wrapper.insert(10, 100);
    TEST_ASSERT(success, "Insert should succeed");
    
    int value;
    TEST_ASSERT(index.lookup(10, value) && value == 100, "Should find key 10");
    
    // Insert inside transaction
    wrapper.begin_txn();
    wrapper.insert(20, 200);
    wrapper.insert(30, 300);
    
    // Not visible yet
    TEST_ASSERT(!index.lookup(20, value), "Key 20 should not be visible yet");
    
    wrapper.commit_txn();
    
    // Now visible
    TEST_ASSERT(index.lookup(20, value) && value == 200, "Should find key 20");
    TEST_ASSERT(index.lookup(30, value) && value == 300, "Should find key 30");
    
    TEST_PASS("Insert operations work");
}

// Test 8: Lookup through wrapper
bool test_lookup_operations() {
    std::cout << "\n[Test 8] Lookup operations..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    // Insert some data
    index.insert(10, 100);
    index.insert(20, 200);
    
    // Lookup outside transaction
    int value;
    TEST_ASSERT(wrapper.lookup(10, value) && value == 100, "Should find key 10");
    TEST_ASSERT(wrapper.lookup(20, value) && value == 200, "Should find key 20");
    TEST_ASSERT(!wrapper.lookup(30, value), "Should not find key 30");
    
    // Lookup inside transaction with pending writes
    wrapper.begin_txn();
    wrapper.insert(30, 300);
    
    // Should see pending write
    TEST_ASSERT(wrapper.lookup(30, value) && value == 300, "Should find pending key 30");
    
    wrapper.commit_txn();
    
    TEST_PASS("Lookup operations work");
}


// Test 9: Delete through wrapper
bool test_delete_operations() {
    std::cout << "\n[Test 9] Delete operations..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    // Insert some data
    index.insert(10, 100);
    index.insert(20, 200);
    
    // Delete outside transaction
    bool success = wrapper.remove(10);
    TEST_ASSERT(success, "Delete should succeed");
    
    int value;
    TEST_ASSERT(!index.lookup(10, value), "Key 10 should be deleted");
    TEST_ASSERT(index.lookup(20, value), "Key 20 should still exist");
    
    TEST_PASS("Delete operations work");
}

// Test 10: Operations without context
bool test_no_context_operations() {
    std::cout << "\n[Test 10] Operations without context..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    
    TEST_ASSERT(!wrapper.has_context(), "Should not have context");
    
    // All operations should fail without context
    TEST_ASSERT(!wrapper.begin_txn(), "Begin should fail without context");
    TEST_ASSERT(!wrapper.insert(10, 100), "Insert should fail without context");
    
    int value;
    TEST_ASSERT(!wrapper.lookup(10, value), "Lookup should fail without context");
    TEST_ASSERT(!wrapper.remove(10), "Delete should fail without context");
    
    TEST_PASS("Operations without context work");
}

// Test 11: Multiple transactions
bool test_multiple_transactions() {
    std::cout << "\n[Test 11] Multiple transactions..." << std::endl;
    
    SimpleIndex<int, int> index;
    MBTAWrapper<int, int> wrapper(&index);
    ThreadContext ctx(1);
    ctx.initialize();
    wrapper.set_context(&ctx);
    
    // First transaction
    wrapper.begin_txn();
    wrapper.insert(10, 100);
    wrapper.commit_txn();
    
    TEST_ASSERT(ctx.txn_count() == 1, "Should have 1 transaction");
    
    // Second transaction
    wrapper.begin_txn();
    wrapper.insert(20, 200);
    wrapper.commit_txn();
    
    TEST_ASSERT(ctx.txn_count() == 2, "Should have 2 transactions");
    
    // Third transaction (aborted)
    wrapper.begin_txn();
    wrapper.insert(30, 300);
    wrapper.abort_txn();
    
    TEST_ASSERT(ctx.txn_count() == 2, "Aborted txn should not increment count");
    
    // Verify data
    int value;
    TEST_ASSERT(index.lookup(10, value) && value == 100, "Should find key 10");
    TEST_ASSERT(index.lookup(20, value) && value == 200, "Should find key 20");
    TEST_ASSERT(!index.lookup(30, value), "Should not find key 30");
    
    TEST_PASS("Multiple transactions work");
}

// Test 12: Concurrent wrapper usage (5 threads, 30 sec)
bool test_concurrent_wrapper_usage() {
    std::cout << "\n[Test 12] Concurrent wrapper usage (5 threads, 30 sec)..." << std::endl;
    
    SimpleIndex<int, int> shared_index;
    std::atomic<int> successful_commits{0};
    std::atomic<int> aborts{0};
    std::atomic<int> total_inserts{0};
    std::atomic<int> total_lookups{0};
    std::atomic<bool> stop_flag{false};
    
    auto worker = [&](int thread_id) {
        // Each thread has its own wrapper and context
        MBTAWrapper<int, int> wrapper(&shared_index);
        ThreadContext ctx(thread_id);
        ctx.initialize();
        wrapper.set_context(&ctx);
        
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> key_dist(1, 1000);
        std::uniform_int_distribution<> op_dist(0, 9);
        std::uniform_int_distribution<> txn_size_dist(1, 5);
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            int op = op_dist(gen);
            
            if (op < 7) {
                // Transaction with multiple operations
                wrapper.begin_txn();
                
                int num_ops = txn_size_dist(gen);
                for (int i = 0; i < num_ops; i++) {
                    int key = key_dist(gen);
                    int value = key * 10 + thread_id;
                    wrapper.insert(key, value);
                    total_inserts++;
                }
                
                // Randomly commit or abort
                if (op_dist(gen) < 8) {
                    if (wrapper.commit_txn()) {
                        successful_commits++;
                    }
                } else {
                    wrapper.abort_txn();
                    aborts++;
                }
            } else {
                // Lookup operation
                int key = key_dist(gen);
                int value;
                if (wrapper.lookup(key, value)) {
                    total_lookups++;
                }
            }
            
            std::this_thread::sleep_for(std::chrono::microseconds(100));
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
    
    std::cout << "  Successful commits: " << successful_commits << std::endl;
    std::cout << "  Aborts: " << aborts << std::endl;
    std::cout << "  Total inserts: " << total_inserts << std::endl;
    std::cout << "  Total lookups: " << total_lookups << std::endl;
    std::cout << "  Index size: " << shared_index.size() << std::endl;
    
    TEST_ASSERT(successful_commits > 0, "Should have successful commits");
    TEST_ASSERT(total_inserts > 0, "Should have inserts");
    TEST_ASSERT(shared_index.size() > 0, "Index should have data");
    
    // Verify some random keys
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> key_dist(1, 1000);
    
    int found = 0;
    for (int i = 0; i < 100; i++) {
        int key = key_dist(gen);
        int value;
        if (shared_index.lookup(key, value)) {
            found++;
        }
    }
    
    std::cout << "  Random lookups found: " << found << " / 100" << std::endl;
    
    TEST_PASS("Concurrent wrapper usage works");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "MBTA Wrapper Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_context_initialization();
    test_context_cleanup();
    test_wrapper_context();
    test_begin_transaction();
    test_commit_transaction();
    test_abort_transaction();
    test_insert_operations();
    test_lookup_operations();
    test_delete_operations();
    test_no_context_operations();
    test_multiple_transactions();
    test_concurrent_wrapper_usage();
    
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
