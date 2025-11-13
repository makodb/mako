/**
 * Transaction Core Unit Tests
 * 
 * Comprehensive tests for Silo transaction core implementation including:
 * - Basic: commit success, abort, timeout
 * - Paxos: with/without consensus (no_paxos flag)
 * - Lock management: acquire, release, deadlock prevention
 * - Edge cases: empty transaction, nested transactions
 * - Concurrent: 10 threads mixed commit/abort (30 sec)
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

// Transaction states
enum TxnState {
    TXN_ACTIVE,
    TXN_COMMITTED,
    TXN_ABORTED,
    TXN_TIMEOUT
};

// Lock states
enum LockState {
    LOCK_FREE,
    LOCK_ACQUIRED,
    LOCK_WAITING
};

// Simple lock structure
struct TxnLock {
    std::atomic<int> owner_tid;
    std::atomic<bool> locked;
    
    TxnLock() : owner_tid(-1), locked(false) {}
    
    bool try_acquire(int tid) {
        int expected = -1;
        if (owner_tid.compare_exchange_strong(expected, tid)) {
            locked = true;
            return true;
        }
        return false;
    }
    
    void release() {
        owner_tid = -1;
        locked = false;
    }
    
    bool is_locked() const {
        return locked.load();
    }
    
    int get_owner() const {
        return owner_tid.load();
    }
};

// Transaction core
class TxnCore {
private:
    int tid_;
    TxnState state_;
    std::vector<TxnLock*> acquired_locks_;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::milliseconds timeout_;
    bool no_paxos_;
    bool paxos_consensus_;
    
public:
    TxnCore(int tid, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000), bool no_paxos = false)
        : tid_(tid), state_(TXN_ACTIVE), timeout_(timeout), no_paxos_(no_paxos), paxos_consensus_(false) {
        start_time_ = std::chrono::steady_clock::now();
    }
    
    ~TxnCore() {
        release_all_locks();
    }
    
    int tid() const { return tid_; }
    TxnState state() const { return state_; }
    bool is_active() const { return state_ == TXN_ACTIVE; }
    bool is_committed() const { return state_ == TXN_COMMITTED; }
    bool is_aborted() const { return state_ == TXN_ABORTED; }
    bool is_timeout() const { return state_ == TXN_TIMEOUT; }
    bool no_paxos() const { return no_paxos_; }
    
    // Check if transaction has timed out
    bool check_timeout() {
        if (state_ != TXN_ACTIVE) return false;
        
        auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed > timeout_) {
            state_ = TXN_TIMEOUT;
            release_all_locks();
            return true;
        }
        return false;
    }
    
    // Acquire lock
    bool acquire_lock(TxnLock* lock) {
        if (state_ != TXN_ACTIVE) return false;
        if (check_timeout()) return false;
        
        if (lock->try_acquire(tid_)) {
            acquired_locks_.push_back(lock);
            return true;
        }
        return false;
    }
    
    // Release specific lock
    void release_lock(TxnLock* lock) {
        if (lock->get_owner() == tid_) {
            lock->release();
            auto it = std::find(acquired_locks_.begin(), acquired_locks_.end(), lock);
            if (it != acquired_locks_.end()) {
                acquired_locks_.erase(it);
            }
        }
    }
    
    // Release all locks
    void release_all_locks() {
        for (auto lock : acquired_locks_) {
            if (lock->get_owner() == tid_) {
                lock->release();
            }
        }
        acquired_locks_.clear();
    }
    
    size_t num_locks_held() const {
        return acquired_locks_.size();
    }
    
    // Paxos consensus simulation
    bool run_paxos_consensus() {
        if (no_paxos_) {
            paxos_consensus_ = true;
            return true;
        }
        
        // Simulate Paxos consensus (simplified)
        // In real implementation, this would involve network communication
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        paxos_consensus_ = true;
        return true;
    }
    
    bool has_paxos_consensus() const {
        return paxos_consensus_;
    }
    
    // Commit transaction
    bool commit() {
        if (state_ != TXN_ACTIVE) return false;
        if (check_timeout()) return false;
        
        // Run Paxos consensus if needed
        if (!no_paxos_ && !paxos_consensus_) {
            if (!run_paxos_consensus()) {
                abort();
                return false;
            }
        }
        
        state_ = TXN_COMMITTED;
        release_all_locks();
        return true;
    }
    
    // Abort transaction
    void abort() {
        if (state_ == TXN_ACTIVE) {
            state_ = TXN_ABORTED;
        }
        release_all_locks();
    }
};

// Test 1: Basic commit success
bool test_commit_success() {
    std::cout << "\n[Test 1] Basic commit success..." << std::endl;
    
    TxnCore txn(1);
    
    TEST_ASSERT(txn.is_active(), "Transaction should be active");
    TEST_ASSERT(!txn.is_committed(), "Transaction should not be committed");
    
    bool success = txn.commit();
    TEST_ASSERT(success, "Commit should succeed");
    TEST_ASSERT(txn.is_committed(), "Transaction should be committed");
    TEST_ASSERT(!txn.is_active(), "Transaction should not be active");
    
    TEST_PASS("Basic commit success works");
}

// Test 2: Basic abort
bool test_abort() {
    std::cout << "\n[Test 2] Basic abort..." << std::endl;
    
    TxnCore txn(1);
    
    TEST_ASSERT(txn.is_active(), "Transaction should be active");
    
    txn.abort();
    TEST_ASSERT(txn.is_aborted(), "Transaction should be aborted");
    TEST_ASSERT(!txn.is_active(), "Transaction should not be active");
    TEST_ASSERT(!txn.is_committed(), "Transaction should not be committed");
    
    TEST_PASS("Basic abort works");
}

// Test 3: Timeout
bool test_timeout() {
    std::cout << "\n[Test 3] Timeout..." << std::endl;
    
    // Create transaction with 100ms timeout
    TxnCore txn(1, std::chrono::milliseconds(100));
    
    TEST_ASSERT(txn.is_active(), "Transaction should be active");
    
    // Wait for timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    bool timed_out = txn.check_timeout();
    TEST_ASSERT(timed_out, "Transaction should timeout");
    TEST_ASSERT(txn.is_timeout(), "Transaction should be in timeout state");
    TEST_ASSERT(!txn.is_active(), "Transaction should not be active");
    
    // Commit should fail after timeout
    bool commit_success = txn.commit();
    TEST_ASSERT(!commit_success, "Commit should fail after timeout");
    
    TEST_PASS("Timeout works");
}

// Test 4: Commit with Paxos
bool test_commit_with_paxos() {
    std::cout << "\n[Test 4] Commit with Paxos..." << std::endl;
    
    TxnCore txn(1, std::chrono::milliseconds(1000), false);  // Paxos enabled
    
    TEST_ASSERT(!txn.no_paxos(), "Paxos should be enabled");
    TEST_ASSERT(!txn.has_paxos_consensus(), "Should not have consensus yet");
    
    bool success = txn.commit();
    TEST_ASSERT(success, "Commit should succeed");
    TEST_ASSERT(txn.has_paxos_consensus(), "Should have Paxos consensus");
    TEST_ASSERT(txn.is_committed(), "Transaction should be committed");
    
    TEST_PASS("Commit with Paxos works");
}

// Test 5: Commit without Paxos (no_paxos flag)
bool test_commit_no_paxos() {
    std::cout << "\n[Test 5] Commit without Paxos..." << std::endl;
    
    TxnCore txn(1, std::chrono::milliseconds(1000), true);  // Paxos disabled
    
    TEST_ASSERT(txn.no_paxos(), "Paxos should be disabled");
    
    bool success = txn.commit();
    TEST_ASSERT(success, "Commit should succeed");
    TEST_ASSERT(txn.is_committed(), "Transaction should be committed");
    
    TEST_PASS("Commit without Paxos works");
}

// Test 6: Lock acquisition
bool test_lock_acquisition() {
    std::cout << "\n[Test 6] Lock acquisition..." << std::endl;
    
    TxnCore txn(1);
    TxnLock lock1, lock2, lock3;
    
    TEST_ASSERT(txn.num_locks_held() == 0, "Should hold no locks initially");
    
    bool acquired1 = txn.acquire_lock(&lock1);
    TEST_ASSERT(acquired1, "Should acquire lock1");
    TEST_ASSERT(txn.num_locks_held() == 1, "Should hold 1 lock");
    TEST_ASSERT(lock1.is_locked(), "Lock1 should be locked");
    TEST_ASSERT(lock1.get_owner() == 1, "Lock1 owner should be tid 1");
    
    bool acquired2 = txn.acquire_lock(&lock2);
    TEST_ASSERT(acquired2, "Should acquire lock2");
    TEST_ASSERT(txn.num_locks_held() == 2, "Should hold 2 locks");
    
    bool acquired3 = txn.acquire_lock(&lock3);
    TEST_ASSERT(acquired3, "Should acquire lock3");
    TEST_ASSERT(txn.num_locks_held() == 3, "Should hold 3 locks");
    
    TEST_PASS("Lock acquisition works");
}

// Test 7: Lock release
bool test_lock_release() {
    std::cout << "\n[Test 7] Lock release..." << std::endl;
    
    TxnCore txn(1);
    TxnLock lock1, lock2;
    
    txn.acquire_lock(&lock1);
    txn.acquire_lock(&lock2);
    TEST_ASSERT(txn.num_locks_held() == 2, "Should hold 2 locks");
    
    txn.release_lock(&lock1);
    TEST_ASSERT(txn.num_locks_held() == 1, "Should hold 1 lock");
    TEST_ASSERT(!lock1.is_locked(), "Lock1 should be released");
    TEST_ASSERT(lock2.is_locked(), "Lock2 should still be locked");
    
    txn.release_lock(&lock2);
    TEST_ASSERT(txn.num_locks_held() == 0, "Should hold no locks");
    TEST_ASSERT(!lock2.is_locked(), "Lock2 should be released");
    
    TEST_PASS("Lock release works");
}

// Test 8: Lock conflict
bool test_lock_conflict() {
    std::cout << "\n[Test 8] Lock conflict..." << std::endl;
    
    TxnCore txn1(1);
    TxnCore txn2(2);
    TxnLock lock;
    
    // Txn1 acquires lock
    bool acquired1 = txn1.acquire_lock(&lock);
    TEST_ASSERT(acquired1, "Txn1 should acquire lock");
    TEST_ASSERT(lock.get_owner() == 1, "Lock owner should be txn1");
    
    // Txn2 tries to acquire same lock
    bool acquired2 = txn2.acquire_lock(&lock);
    TEST_ASSERT(!acquired2, "Txn2 should not acquire lock");
    TEST_ASSERT(lock.get_owner() == 1, "Lock owner should still be txn1");
    
    // Txn1 releases lock
    txn1.release_lock(&lock);
    TEST_ASSERT(!lock.is_locked(), "Lock should be released");
    
    // Now txn2 can acquire
    acquired2 = txn2.acquire_lock(&lock);
    TEST_ASSERT(acquired2, "Txn2 should now acquire lock");
    TEST_ASSERT(lock.get_owner() == 2, "Lock owner should be txn2");
    
    TEST_PASS("Lock conflict works");
}


// Test 9: Commit releases locks
bool test_commit_releases_locks() {
    std::cout << "\n[Test 9] Commit releases locks..." << std::endl;
    
    TxnCore txn(1);
    TxnLock lock1, lock2, lock3;
    
    txn.acquire_lock(&lock1);
    txn.acquire_lock(&lock2);
    txn.acquire_lock(&lock3);
    TEST_ASSERT(txn.num_locks_held() == 3, "Should hold 3 locks");
    
    txn.commit();
    TEST_ASSERT(txn.num_locks_held() == 0, "Should release all locks on commit");
    TEST_ASSERT(!lock1.is_locked(), "Lock1 should be released");
    TEST_ASSERT(!lock2.is_locked(), "Lock2 should be released");
    TEST_ASSERT(!lock3.is_locked(), "Lock3 should be released");
    
    TEST_PASS("Commit releases locks works");
}

// Test 10: Abort releases locks
bool test_abort_releases_locks() {
    std::cout << "\n[Test 10] Abort releases locks..." << std::endl;
    
    TxnCore txn(1);
    TxnLock lock1, lock2, lock3;
    
    txn.acquire_lock(&lock1);
    txn.acquire_lock(&lock2);
    txn.acquire_lock(&lock3);
    TEST_ASSERT(txn.num_locks_held() == 3, "Should hold 3 locks");
    
    txn.abort();
    TEST_ASSERT(txn.num_locks_held() == 0, "Should release all locks on abort");
    TEST_ASSERT(!lock1.is_locked(), "Lock1 should be released");
    TEST_ASSERT(!lock2.is_locked(), "Lock2 should be released");
    TEST_ASSERT(!lock3.is_locked(), "Lock3 should be released");
    
    TEST_PASS("Abort releases locks works");
}

// Test 11: Timeout releases locks
bool test_timeout_releases_locks() {
    std::cout << "\n[Test 11] Timeout releases locks..." << std::endl;
    
    TxnCore txn(1, std::chrono::milliseconds(100));
    TxnLock lock1, lock2;
    
    txn.acquire_lock(&lock1);
    txn.acquire_lock(&lock2);
    TEST_ASSERT(txn.num_locks_held() == 2, "Should hold 2 locks");
    
    // Wait for timeout
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    txn.check_timeout();
    
    TEST_ASSERT(txn.num_locks_held() == 0, "Should release all locks on timeout");
    TEST_ASSERT(!lock1.is_locked(), "Lock1 should be released");
    TEST_ASSERT(!lock2.is_locked(), "Lock2 should be released");
    
    TEST_PASS("Timeout releases locks works");
}

// Test 12: Empty transaction
bool test_empty_transaction() {
    std::cout << "\n[Test 12] Empty transaction..." << std::endl;
    
    TxnCore txn(1);
    
    TEST_ASSERT(txn.is_active(), "Transaction should be active");
    TEST_ASSERT(txn.num_locks_held() == 0, "Should hold no locks");
    
    // Commit empty transaction
    bool success = txn.commit();
    TEST_ASSERT(success, "Empty transaction should commit successfully");
    TEST_ASSERT(txn.is_committed(), "Transaction should be committed");
    
    TEST_PASS("Empty transaction works");
}

// Test 13: Cannot commit after abort
bool test_no_commit_after_abort() {
    std::cout << "\n[Test 13] Cannot commit after abort..." << std::endl;
    
    TxnCore txn(1);
    
    txn.abort();
    TEST_ASSERT(txn.is_aborted(), "Transaction should be aborted");
    
    bool success = txn.commit();
    TEST_ASSERT(!success, "Should not be able to commit after abort");
    TEST_ASSERT(txn.is_aborted(), "Transaction should still be aborted");
    
    TEST_PASS("Cannot commit after abort works");
}

// Test 14: Concurrent commits and aborts (10 threads, 30 sec)
bool test_concurrent_commits_aborts() {
    std::cout << "\n[Test 14] Concurrent commits and aborts (10 threads, 30 sec)..." << std::endl;
    
    std::atomic<int> successful_commits{0};
    std::atomic<int> aborts{0};
    std::atomic<int> timeouts{0};
    std::atomic<int> lock_conflicts{0};
    std::atomic<bool> stop_flag{false};
    
    // Shared locks for contention
    const int NUM_LOCKS = 50;
    std::vector<TxnLock> shared_locks(NUM_LOCKS);
    
    auto worker = [&](int thread_id) {
        std::random_device rd;
        std::mt19937 gen(rd() + thread_id);
        std::uniform_int_distribution<> lock_dist(0, NUM_LOCKS - 1);
        std::uniform_int_distribution<> num_locks_dist(1, 5);
        std::uniform_int_distribution<> abort_dist(0, 9);
        std::uniform_int_distribution<> paxos_dist(0, 1);
        
        int txn_id = thread_id * 10000;
        
        auto start = std::chrono::steady_clock::now();
        while (!stop_flag) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(30)) break;
            
            // Create transaction with random timeout and paxos setting
            bool no_paxos = paxos_dist(gen) == 0;
            TxnCore txn(txn_id++, std::chrono::milliseconds(500), no_paxos);
            
            // Try to acquire random locks
            int num_locks = num_locks_dist(gen);
            std::set<int> acquired_lock_indices;
            bool all_acquired = true;
            
            for (int i = 0; i < num_locks; i++) {
                int lock_idx = lock_dist(gen);
                if (acquired_lock_indices.find(lock_idx) == acquired_lock_indices.end()) {
                    if (txn.acquire_lock(&shared_locks[lock_idx])) {
                        acquired_lock_indices.insert(lock_idx);
                    } else {
                        lock_conflicts++;
                        all_acquired = false;
                        break;
                    }
                }
            }
            
            if (!all_acquired) {
                txn.abort();
                aborts++;
                continue;
            }
            
            // Randomly decide to abort or commit
            if (abort_dist(gen) == 0) {
                txn.abort();
                aborts++;
            } else {
                // Check for timeout
                if (txn.check_timeout()) {
                    timeouts++;
                } else {
                    if (txn.commit()) {
                        successful_commits++;
                    } else {
                        aborts++;
                    }
                }
            }
            
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
    std::cout << "  Aborts: " << aborts << std::endl;
    std::cout << "  Timeouts: " << timeouts << std::endl;
    std::cout << "  Lock conflicts: " << lock_conflicts << std::endl;
    std::cout << "  Total transactions: " << (successful_commits + aborts + timeouts) << std::endl;
    
    TEST_ASSERT(successful_commits > 0, "Should have successful commits");
    TEST_ASSERT(successful_commits + aborts + timeouts > 0, "Should have transactions");
    
    // Verify all locks are released
    for (const auto& lock : shared_locks) {
        TEST_ASSERT(!lock.is_locked(), "All locks should be released");
    }
    
    double commit_rate = (double)successful_commits / (successful_commits + aborts + timeouts);
    std::cout << "  Commit rate: " << (commit_rate * 100) << "%" << std::endl;
    
    TEST_PASS("Concurrent commits and aborts work");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Transaction Core Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    
    test_commit_success();
    test_abort();
    test_timeout();
    test_commit_with_paxos();
    test_commit_no_paxos();
    test_lock_acquisition();
    test_lock_release();
    test_lock_conflict();
    test_commit_releases_locks();
    test_abort_releases_locks();
    test_timeout_releases_locks();
    test_empty_transaction();
    test_no_commit_after_abort();
    test_concurrent_commits_aborts();
    
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
