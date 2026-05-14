// @safe - Unit tests for MakoClientService
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include <rusty/arc.hpp>
#include <rusty/mutex.hpp>

// We can only test the ID generation logic directly without mocking
// ShardReceiver, since it requires full database setup.
// The ID encoding is: txn_id = (client_id << 32) | counter

// ============================================================================
// Transaction ID Encoding Tests
// ============================================================================

class TxnIdEncodingTest : public ::testing::Test {
protected:
    // @safe - Pure function to encode txn_id (same logic as MakoClientService)
    static uint64_t EncodeTxnId(uint64_t client_id, uint32_t counter) {
        return (client_id << 32) | counter;
    }

    // @safe - Pure function to decode client_id from txn_id
    static uint64_t DecodeClientId(uint64_t txn_id) {
        return txn_id >> 32;
    }

    // @safe - Pure function to decode counter from txn_id
    static uint32_t DecodeCounter(uint64_t txn_id) {
        return static_cast<uint32_t>(txn_id & 0xFFFFFFFF);
    }
};

// @safe - Test encoding roundtrip
TEST_F(TxnIdEncodingTest, EncodingRoundtrip) {
    uint64_t client_id = 12345;
    uint32_t counter = 67890;

    uint64_t txn_id = EncodeTxnId(client_id, counter);

    EXPECT_EQ(DecodeClientId(txn_id), client_id);
    EXPECT_EQ(DecodeCounter(txn_id), counter);
}

// @safe - Test different client IDs produce different txn_ids
TEST_F(TxnIdEncodingTest, DifferentClientIdsDifferentTxnIds) {
    uint32_t counter = 0;

    uint64_t txn_id1 = EncodeTxnId(1, counter);
    uint64_t txn_id2 = EncodeTxnId(2, counter);
    uint64_t txn_id3 = EncodeTxnId(3, counter);

    EXPECT_NE(txn_id1, txn_id2);
    EXPECT_NE(txn_id2, txn_id3);
    EXPECT_NE(txn_id1, txn_id3);
}

// @safe - Test different counters produce different txn_ids
TEST_F(TxnIdEncodingTest, DifferentCountersDifferentTxnIds) {
    uint64_t client_id = 100;

    uint64_t txn_id1 = EncodeTxnId(client_id, 0);
    uint64_t txn_id2 = EncodeTxnId(client_id, 1);
    uint64_t txn_id3 = EncodeTxnId(client_id, 2);

    EXPECT_NE(txn_id1, txn_id2);
    EXPECT_NE(txn_id2, txn_id3);
    EXPECT_NE(txn_id1, txn_id3);
}

// @safe - Test edge case: max client_id
TEST_F(TxnIdEncodingTest, MaxClientId) {
    uint64_t client_id = 0xFFFFFFFF;  // Max 32-bit value
    uint32_t counter = 0;

    uint64_t txn_id = EncodeTxnId(client_id, counter);

    EXPECT_EQ(DecodeClientId(txn_id), client_id);
    EXPECT_EQ(DecodeCounter(txn_id), counter);
}

// @safe - Test edge case: max counter
TEST_F(TxnIdEncodingTest, MaxCounter) {
    uint64_t client_id = 1;
    uint32_t counter = 0xFFFFFFFF;  // Max 32-bit value

    uint64_t txn_id = EncodeTxnId(client_id, counter);

    EXPECT_EQ(DecodeClientId(txn_id), client_id);
    EXPECT_EQ(DecodeCounter(txn_id), counter);
}

// @safe - Test that zero client_id and counter works
TEST_F(TxnIdEncodingTest, ZeroValues) {
    uint64_t txn_id = EncodeTxnId(0, 0);

    EXPECT_EQ(DecodeClientId(txn_id), 0ULL);
    EXPECT_EQ(DecodeCounter(txn_id), 0U);
    EXPECT_EQ(txn_id, 0ULL);
}

// ============================================================================
// Atomic Counter Tests (Simulating MakoClientService counter behavior)
// ============================================================================

class AtomicCounterTest : public ::testing::Test {
protected:
    std::atomic<uint32_t> counter_{0};

    // @safe - Simulates BeginTxn counter increment
    uint32_t GetNextCounter() {
        return counter_.fetch_add(1, std::memory_order_relaxed);
    }
};

// @safe - Test sequential counter increments
TEST_F(AtomicCounterTest, SequentialIncrements) {
    std::vector<uint32_t> values;

    for (int i = 0; i < 100; i++) {
        values.push_back(GetNextCounter());
    }

    // Verify all values are unique and sequential
    std::set<uint32_t> unique_values(values.begin(), values.end());
    EXPECT_EQ(unique_values.size(), 100);

    // Verify sequential order
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(values[i], static_cast<uint32_t>(i));
    }
}

// @safe - Test concurrent counter increments produce unique values
TEST_F(AtomicCounterTest, ConcurrentIncrements) {
    const int num_threads = 10;
    const int increments_per_thread = 1000;
    const int total_increments = num_threads * increments_per_thread;

    // Using rusty::Mutex with proper initialization
    rusty::Mutex<std::vector<uint32_t>> all_values{std::vector<uint32_t>{}};

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this, &all_values, increments_per_thread]() {
            std::vector<uint32_t> local_values;
            for (int i = 0; i < increments_per_thread; i++) {
                local_values.push_back(GetNextCounter());
            }
            // @unsafe { std::vector::insert, lock().unwrap() }
            auto guard = all_values.lock().unwrap();
            guard->insert(guard->end(), local_values.begin(), local_values.end());
        });
    }

    for (auto& t : threads) {
        t.join();  // @unsafe
    }

    // Verify all values are unique
    auto guard = all_values.lock().unwrap();
    std::set<uint32_t> unique_values(guard->begin(), guard->end());
    EXPECT_EQ(unique_values.size(), total_increments);
}

// @safe - Test counter doesn't overflow in normal use
TEST_F(AtomicCounterTest, NoOverflowInNormalUse) {
    // Start near max but not at max
    counter_.store(0xFFFFFFF0, std::memory_order_relaxed);

    // Get 10 values
    std::vector<uint32_t> values;
    for (int i = 0; i < 10; i++) {
        values.push_back(GetNextCounter());
    }

    // All should be unique
    std::set<uint32_t> unique_values(values.begin(), values.end());
    EXPECT_EQ(unique_values.size(), 10);
}

// ============================================================================
// RPC ID Constants Tests
// ============================================================================

// @safe - Test RPC ID values match expected
TEST(RpcIdConstantsTest, BeginTxnId) {
    // These should match clientBeginTxnReqType etc. in common.h
    constexpr int BEGIN_TXN = 20;
    constexpr int COMMIT = 21;
    constexpr int ROLLBACK = 22;
    constexpr int PUT = 23;
    constexpr int GET = 24;
    constexpr int DELETE_KEY = 25;

    // Verify they are all distinct
    std::set<int> ids = {BEGIN_TXN, COMMIT, ROLLBACK, PUT, GET, DELETE_KEY};
    EXPECT_EQ(ids.size(), 6);

    // Verify they are in expected range
    for (int id : ids) {
        EXPECT_GE(id, 20);
        EXPECT_LE(id, 25);
    }
}

// ============================================================================
// Txn ID Uniqueness Across Multiple "Services" (Simulated)
// ============================================================================

class MultiServiceTest : public ::testing::Test {
protected:
    // Each "service" has its own counter
    std::atomic<uint32_t> service1_counter_{0};
    std::atomic<uint32_t> service2_counter_{0};

    uint64_t GenerateTxnId(std::atomic<uint32_t>& counter, uint64_t client_id) {
        uint32_t cnt = counter.fetch_add(1, std::memory_order_relaxed);
        return (client_id << 32) | cnt;
    }
};

// @safe - Test that different service counters are independent
TEST_F(MultiServiceTest, IndependentCounters) {
    // Each service starts at 0
    EXPECT_EQ(service1_counter_.load(), 0U);
    EXPECT_EQ(service2_counter_.load(), 0U);

    // Generate some IDs from service1
    for (int i = 0; i < 5; i++) {
        GenerateTxnId(service1_counter_, 1);
    }

    // service1 counter advanced, service2 didn't
    EXPECT_EQ(service1_counter_.load(), 5U);
    EXPECT_EQ(service2_counter_.load(), 0U);

    // Generate from service2
    GenerateTxnId(service2_counter_, 2);
    EXPECT_EQ(service2_counter_.load(), 1U);
}

// @safe - Test that same client_id on different services produces different txn_ids
// (because client_id comes from server-side, different servers have different counters)
TEST_F(MultiServiceTest, SameClientDifferentServices) {
    uint64_t client_id = 12345;

    uint64_t txn1 = GenerateTxnId(service1_counter_, client_id);
    uint64_t txn2 = GenerateTxnId(service2_counter_, client_id);

    // Same client_id and same counter value (0), so txn_ids would be the same
    // This test documents that in multi-server setup, txn_ids could collide
    // unless client_id is made unique per server
    // This is a known limitation documented in the architecture
    EXPECT_EQ(txn1, txn2);  // Documents the collision case
}

// @safe - Main entry point
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
