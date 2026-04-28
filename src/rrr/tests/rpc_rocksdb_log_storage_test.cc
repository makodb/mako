/**
 * Unit tests for RocksDB Log Storage
 *
 * Tests RocksDBLogStorage implementation of LogStorage interface.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <filesystem>
#include <optional>

#include <rusty/box.hpp>

#include "deptran/raft/rocksdb_log_storage.hpp"
#include "../rrr.hpp"

using namespace rrr;
using namespace janus::raft;

// ============================================================================
// Test Marshallable Command for testing
// ============================================================================

class TestCommand : public Marshallable {
public:
    std::string data;
    int32_t value{0};

    TestCommand() : Marshallable(MarshallDeputy::CMD_NOOP) {}

    TestCommand(const std::string& d, int32_t v)
        : Marshallable(MarshallDeputy::CMD_NOOP), data(d), value(v) {}

    Marshal& to_marshal(Marshal& m) const override {
        m << data;
        m << value;
        return m;
    }

    Marshal& from_marshal(Marshal& m) override {
        m >> data;
        m >> value;
        return m;
    }
};

// ============================================================================
// RocksDBLogStorage Tests
// ============================================================================

class RocksDBLogStorageTest : public ::testing::Test {
protected:
    std::string db_path_;
    std::optional<rusty::Box<RocksDBLogStorage>> storage_;

    void SetUp() override {
        // Create unique path for each test
        db_path_ = "/tmp/test_rocksdb_log_storage_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + std::to_string(rand());

        // Clean up any existing database
        std::filesystem::remove_all(db_path_);

        storage_ = rusty::make_box<RocksDBLogStorage>(db_path_);
        ASSERT_TRUE((*storage_)->open());
    }

    void TearDown() override {
        if (storage_) {
            (*storage_)->close();
        }
        // Clean up database files
        std::filesystem::remove_all(db_path_);
    }

    LogEntry make_entry(slotid_t slot, ballot_t term, bool committed = false) {
        LogEntry entry(slot, term);
        entry.committed = committed;
        return entry;
    }
};

// ============================================================================
// Basic Operations Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, OpenAndClose) {
    EXPECT_TRUE((*storage_)->is_open());
    EXPECT_TRUE((*storage_)->close());
    EXPECT_FALSE((*storage_)->is_open());
}

TEST_F(RocksDBLogStorageTest, PutAndGet) {
    LogEntry entry = make_entry(1, 5);
    EXPECT_TRUE((*storage_)->put(entry));

    auto result = (*storage_)->get(1);
    EXPECT_TRUE(result.is_some());
    auto retrieved = result.unwrap();
    EXPECT_EQ(retrieved.slot_id, 1u);
    EXPECT_EQ(retrieved.term, 5u);
}

TEST_F(RocksDBLogStorageTest, GetNonExistent) {
    auto result = (*storage_)->get(999);
    EXPECT_TRUE(result.is_none());
}

TEST_F(RocksDBLogStorageTest, PutOverwrite) {
    (*storage_)->put(make_entry(1, 5));
    (*storage_)->put(make_entry(1, 10));  // Overwrite

    auto result = (*storage_)->get(1);
    EXPECT_TRUE(result.is_some());
    auto entry = result.unwrap();
    EXPECT_EQ(entry.term, 10u);
}

TEST_F(RocksDBLogStorageTest, Remove) {
    (*storage_)->put(make_entry(1, 5));
    EXPECT_TRUE((*storage_)->remove(1));
    EXPECT_TRUE((*storage_)->get(1).is_none());
}

TEST_F(RocksDBLogStorageTest, RemoveNonExistent) {
    EXPECT_FALSE((*storage_)->remove(999));
}

// ============================================================================
// Batch Operations Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, PutBatch) {
    std::vector<LogEntry> entries = {
        make_entry(1, 1),
        make_entry(2, 2),
        make_entry(3, 3)
    };
    EXPECT_TRUE((*storage_)->put_batch(entries));

    EXPECT_EQ((*storage_)->size(), 3u);
    EXPECT_TRUE((*storage_)->get(1).is_some());
    EXPECT_TRUE((*storage_)->get(2).is_some());
    EXPECT_TRUE((*storage_)->get(3).is_some());
}

TEST_F(RocksDBLogStorageTest, GetRange) {
    (*storage_)->put(make_entry(1, 1));
    (*storage_)->put(make_entry(3, 3));
    (*storage_)->put(make_entry(5, 5));
    (*storage_)->put(make_entry(7, 7));

    auto range = (*storage_)->get_range(2, 6);
    EXPECT_EQ(range.size(), 2u);  // slots 3 and 5
}

TEST_F(RocksDBLogStorageTest, GetRangeEmpty) {
    (*storage_)->put(make_entry(10, 10));

    auto range = (*storage_)->get_range(1, 5);
    EXPECT_TRUE(range.empty());
}

TEST_F(RocksDBLogStorageTest, RemoveRange) {
    (*storage_)->put(make_entry(1, 1));
    (*storage_)->put(make_entry(2, 2));
    (*storage_)->put(make_entry(3, 3));
    (*storage_)->put(make_entry(4, 4));
    (*storage_)->put(make_entry(5, 5));

    EXPECT_TRUE((*storage_)->remove_range(2, 4));

    EXPECT_TRUE((*storage_)->get(1).is_some());
    EXPECT_TRUE((*storage_)->get(2).is_none());
    EXPECT_TRUE((*storage_)->get(3).is_none());
    EXPECT_TRUE((*storage_)->get(4).is_some());
    EXPECT_TRUE((*storage_)->get(5).is_some());
}

// ============================================================================
// Index Queries Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, GetFirstIndex) {
    EXPECT_EQ((*storage_)->get_first_index(), 0u);  // Empty

    (*storage_)->put(make_entry(5, 1));
    (*storage_)->put(make_entry(3, 1));
    (*storage_)->put(make_entry(7, 1));

    EXPECT_EQ((*storage_)->get_first_index(), 3u);
}

TEST_F(RocksDBLogStorageTest, GetLastIndex) {
    EXPECT_EQ((*storage_)->get_last_index(), 0u);  // Empty

    (*storage_)->put(make_entry(5, 1));
    (*storage_)->put(make_entry(3, 1));
    (*storage_)->put(make_entry(7, 1));

    EXPECT_EQ((*storage_)->get_last_index(), 7u);
}

TEST_F(RocksDBLogStorageTest, GetTerm) {
    (*storage_)->put(make_entry(1, 42));

    auto term = (*storage_)->get_term(1);
    EXPECT_TRUE(term.is_some());
    EXPECT_EQ(term.unwrap(), 42u);

    auto none = (*storage_)->get_term(999);
    EXPECT_TRUE(none.is_none());
}

TEST_F(RocksDBLogStorageTest, SizeAndEmpty) {
    EXPECT_TRUE((*storage_)->empty());
    EXPECT_EQ((*storage_)->size(), 0u);

    (*storage_)->put(make_entry(1, 1));
    EXPECT_FALSE((*storage_)->empty());
    EXPECT_EQ((*storage_)->size(), 1u);

    (*storage_)->put(make_entry(2, 2));
    EXPECT_EQ((*storage_)->size(), 2u);
}

// ============================================================================
// Metadata Operations Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, SetAndGetMetadata) {
    EXPECT_TRUE((*storage_)->set_metadata("term", "5"));
    EXPECT_TRUE((*storage_)->set_metadata("vote", "node1"));

    auto term = (*storage_)->get_metadata("term");
    EXPECT_TRUE(term.is_some());
    EXPECT_EQ(term.unwrap(), "5");

    auto vote = (*storage_)->get_metadata("vote");
    EXPECT_TRUE(vote.is_some());
    EXPECT_EQ(vote.unwrap(), "node1");
}

TEST_F(RocksDBLogStorageTest, GetMetadataNonExistent) {
    auto result = (*storage_)->get_metadata("nonexistent");
    EXPECT_TRUE(result.is_none());
}

TEST_F(RocksDBLogStorageTest, OverwriteMetadata) {
    (*storage_)->set_metadata("key", "value1");
    (*storage_)->set_metadata("key", "value2");

    auto result = (*storage_)->get_metadata("key");
    EXPECT_TRUE(result.is_some());
    EXPECT_EQ(result.unwrap(), "value2");
}

// ============================================================================
// Lifecycle Operations Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, IsOpen) {
    EXPECT_TRUE((*storage_)->is_open());
}

TEST_F(RocksDBLogStorageTest, Sync) {
    (*storage_)->put(make_entry(1, 1));
    EXPECT_TRUE((*storage_)->sync());
}

TEST_F(RocksDBLogStorageTest, CloseIdempotent) {
    EXPECT_TRUE((*storage_)->close());
    EXPECT_FALSE((*storage_)->close());  // Already closed
}

TEST_F(RocksDBLogStorageTest, Clear) {
    (*storage_)->put(make_entry(1, 1));
    (*storage_)->put(make_entry(2, 2));
    (*storage_)->set_metadata("key", "value");

    EXPECT_TRUE((*storage_)->clear());

    EXPECT_TRUE((*storage_)->empty());
    EXPECT_TRUE((*storage_)->get_metadata("key").is_none());
    EXPECT_TRUE((*storage_)->is_open());  // Still open after clear
}

TEST_F(RocksDBLogStorageTest, OperationsOnClosedStorage) {
    (*storage_)->put(make_entry(1, 1));
    (*storage_)->set_metadata("key", "value");

    EXPECT_TRUE((*storage_)->close());
    EXPECT_FALSE((*storage_)->is_open());

    // Operations on closed storage should fail/return empty
    EXPECT_FALSE((*storage_)->put(make_entry(2, 2)));
    EXPECT_TRUE((*storage_)->get(1).is_none());
    EXPECT_TRUE((*storage_)->get_metadata("key").is_none());
}

// ============================================================================
// Persistence Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, PersistenceAcrossReopen) {
    // Write data
    (*storage_)->put(make_entry(1, 10));
    (*storage_)->put(make_entry(2, 20));
    (*storage_)->set_metadata("current_term", "42");

    // Close and reopen
    EXPECT_TRUE((*storage_)->close());
    EXPECT_TRUE((*storage_)->open());

    // Verify data persisted
    auto entry1 = (*storage_)->get(1);
    EXPECT_TRUE(entry1.is_some());
    EXPECT_EQ(entry1.unwrap().term, 10u);

    auto entry2 = (*storage_)->get(2);
    EXPECT_TRUE(entry2.is_some());
    EXPECT_EQ(entry2.unwrap().term, 20u);

    auto term = (*storage_)->get_metadata("current_term");
    EXPECT_TRUE(term.is_some());
    EXPECT_EQ(term.unwrap(), "42");
}

TEST_F(RocksDBLogStorageTest, PersistenceWithFullLogEntry) {
    LogEntry entry(100, 5);
    entry.max_ballot_seen = 10;
    entry.max_ballot_accepted = 8;
    entry.committed = true;
    entry.is_no_op = true;

    EXPECT_TRUE((*storage_)->put(entry));

    // Close and reopen
    EXPECT_TRUE((*storage_)->close());
    EXPECT_TRUE((*storage_)->open());

    auto result = (*storage_)->get(100);
    EXPECT_TRUE(result.is_some());
    auto restored = result.unwrap();
    EXPECT_EQ(restored.slot_id, 100u);
    EXPECT_EQ(restored.term, 5u);
    EXPECT_EQ(restored.max_ballot_seen, 10u);
    EXPECT_EQ(restored.max_ballot_accepted, 8u);
    EXPECT_TRUE(restored.committed);
    EXPECT_TRUE(restored.is_no_op);
}

// ============================================================================
// Edge Cases Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, LargeSlotIds) {
    slotid_t large_slot = 1000000000000ULL;
    (*storage_)->put(make_entry(large_slot, 1));

    auto result = (*storage_)->get(large_slot);
    EXPECT_TRUE(result.is_some());
    auto entry = result.unwrap();
    EXPECT_EQ(entry.slot_id, large_slot);
}

TEST_F(RocksDBLogStorageTest, ZeroSlotId) {
    (*storage_)->put(make_entry(0, 1));

    auto result = (*storage_)->get(0);
    EXPECT_TRUE(result.is_some());
    auto entry = result.unwrap();
    EXPECT_EQ(entry.slot_id, 0u);
}

TEST_F(RocksDBLogStorageTest, GetRangeInvalidRange) {
    (*storage_)->put(make_entry(1, 1));

    // start >= end
    auto range = (*storage_)->get_range(5, 3);
    EXPECT_TRUE(range.empty());
}

TEST_F(RocksDBLogStorageTest, RemoveRangeInvalidRange) {
    (*storage_)->put(make_entry(1, 1));

    // start >= end
    EXPECT_FALSE((*storage_)->remove_range(5, 3));
    EXPECT_EQ((*storage_)->size(), 1u);  // Nothing removed
}

TEST_F(RocksDBLogStorageTest, LargeNumberOfEntries) {
    const int NUM_ENTRIES = 1000;

    // Batch insert
    std::vector<LogEntry> entries;
    for (int i = 0; i < NUM_ENTRIES; i++) {
        entries.push_back(make_entry(i, i % 10));
    }
    EXPECT_TRUE((*storage_)->put_batch(entries));

    EXPECT_EQ((*storage_)->size(), static_cast<size_t>(NUM_ENTRIES));
    EXPECT_EQ((*storage_)->get_first_index(), 0u);
    EXPECT_EQ((*storage_)->get_last_index(), static_cast<slotid_t>(NUM_ENTRIES - 1));

    // Verify some entries
    auto entry0 = (*storage_)->get(0);
    EXPECT_TRUE(entry0.is_some());
    EXPECT_EQ(entry0.unwrap().slot_id, 0u);

    auto entry500 = (*storage_)->get(500);
    EXPECT_TRUE(entry500.is_some());
    EXPECT_EQ(entry500.unwrap().slot_id, 500u);
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, ConcurrentPuts) {
    const int NUM_THREADS = 4;
    const int ENTRIES_PER_THREAD = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < ENTRIES_PER_THREAD; i++) {
                slotid_t slot = t * ENTRIES_PER_THREAD + i;
                (*storage_)->put(make_entry(slot, t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ((*storage_)->size(), static_cast<size_t>(NUM_THREADS * ENTRIES_PER_THREAD));
}

TEST_F(RocksDBLogStorageTest, ConcurrentReadsAndWrites) {
    // Pre-populate some entries
    for (slotid_t i = 0; i < 50; i++) {
        (*storage_)->put(make_entry(i, 1));
    }

    std::atomic<int> read_count{0};
    std::atomic<int> write_count{0};

    const int NUM_THREADS = 4;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([this, t, &read_count, &write_count]() {
            for (int i = 0; i < 100; i++) {
                if (i % 2 == 0) {
                    // Read
                    slotid_t slot = i % 50;
                    auto result = (*storage_)->get(slot);
                    if (result.is_some()) {
                        read_count++;
                    }
                } else {
                    // Write
                    slotid_t slot = 100 + t * 100 + i;
                    (*storage_)->put(make_entry(slot, t));
                    write_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_GT(read_count.load(), 0);
    EXPECT_GT(write_count.load(), 0);
}

TEST_F(RocksDBLogStorageTest, ConcurrentMetadata) {
    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                std::string key = "key_" + std::to_string(t);
                std::string value = "value_" + std::to_string(i);
                (*storage_)->set_metadata(key, value);
                (*storage_)->get_metadata(key);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All thread-specific keys should exist
    for (int t = 0; t < NUM_THREADS; t++) {
        std::string key = "key_" + std::to_string(t);
        EXPECT_TRUE((*storage_)->get_metadata(key).is_some());
    }
}

// ============================================================================
// Static Methods Tests
// ============================================================================

TEST_F(RocksDBLogStorageTest, DestroyDatabase) {
    // Write some data and close
    (*storage_)->put(make_entry(1, 1));
    (*storage_)->close();

    // Destroy database
    EXPECT_TRUE(RocksDBLogStorage::destroy(db_path_));

    // Verify database is gone by trying to open (it should create new)
    storage_ = rusty::make_box<RocksDBLogStorage>(db_path_);
    EXPECT_TRUE((*storage_)->open());
    EXPECT_TRUE((*storage_)->empty());  // Should be empty after destroy
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
