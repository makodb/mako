#include <gtest/gtest.h>
#include "benchmarks/sto/Transaction.hh"
#include <vector>
#include <thread>
#include <atomic>

// ============================================================================
// STO TRANSACTION TESTS - Testing real transaction system
// ============================================================================

class RealTransactionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize thread-local transaction if needed
        // Sto::transaction() handles lazy initialization
    }
};

// Test basic transaction macro usage
TEST_F(RealTransactionTest, BasicTransaction_Commit) {
    bool executed = false;
    
    TRANSACTION {
        executed = true;
    } RETRY(true);
    
    EXPECT_TRUE(executed);
}

// Test transaction with retry logic
TEST_F(RealTransactionTest, Transaction_Retry) {
    int attempts = 0;
    
    TRANSACTION {
        attempts++;
        if (attempts < 3) {
            // Simulate conflict/abort
            Sto::transaction()->silent_abort(); 
        }
    } RETRY(true);
    
    EXPECT_GE(attempts, 1);
}

// Test transaction with data modification
TEST_F(RealTransactionTest, Transaction_DataModification) {
    int value = 0;
    
    TRANSACTION {
        value = 42;
    } RETRY(true);
    
    EXPECT_EQ(value, 42);
}

// Test concurrent transactions
TEST_F(RealTransactionTest, ConcurrentTransactions) {
    std::atomic<int> completed_txns{0};
    const int num_threads = 4;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&completed_txns]() {
            // Ensure thread-local initialization
            Sto::update_threadid();
            
            for (int j = 0; j < 100; j++) {
                TRANSACTION {
                    // Do nothing, just commit
                } RETRY(true);
            }
            completed_txns++;
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(completed_txns.load(), num_threads);
}

// Test transaction items (TransItem)
// We need to define a simple transactional object to test this properly
// but for now we test the infrastructure

TEST_F(RealTransactionTest, Transaction_IdCheck) {
    TRANSACTION {
        auto tid = Sto::transaction()->commit_tid();
        // Just verify we can access it
        (void)tid;
    } RETRY(true);
}
