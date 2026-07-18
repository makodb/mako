/**
 * Unit tests for Log Storage Interface
 *
 * Tests LogEntry serialization and InMemoryLogStorage implementation.
 */

#include <stddef.h>

#include <gtest/gtest.h>

#include "deptran/raft/log_storage.hpp"
#include "deptran/raft/memory_log_storage.hpp"
#include "deptran/classic/tpc_command.h"  // TpcEmptyCommand for nested-command tests
#include "../rrr.hpp"

import std;

using namespace rrr;
using namespace janus::raft;

// ============================================================================
// LogEntry Tests
// ============================================================================

class LogEntryTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(LogEntryTest, DefaultConstruction) {
    LogEntry entry;
    EXPECT_EQ(entry.slot_id, 0u);
    EXPECT_EQ(entry.term, 0u);
    EXPECT_EQ(entry.max_ballot_seen, 0u);
    EXPECT_EQ(entry.max_ballot_accepted, 0u);
    EXPECT_FALSE(entry.command.has_value());
    EXPECT_FALSE(entry.committed);
    EXPECT_FALSE(entry.is_no_op);
}

TEST_F(LogEntryTest, ConstructionWithSlotAndTerm) {
    LogEntry entry(42, 5);
    EXPECT_EQ(entry.slot_id, 42u);
    EXPECT_EQ(entry.term, 5u);
    EXPECT_FALSE(entry.command.has_value());
    EXPECT_FALSE(entry.committed);
}

TEST_F(LogEntryTest, FullConstruction) {
    // 2 step 2.5: use TpcEmptyCommand (a real Serializable in
    // MakoCommands) as the carried payload instead of a Marshallable
    // test fixture; the test exercises LogEntry's command-carrying
    // shape, the choice of T doesn't matter beyond "is a valid
    // Command payload".
    auto cmd = std::make_shared<janus::TpcEmptyCommand>();
    LogEntry entry(10, 3, cmd, true);

    EXPECT_EQ(entry.slot_id, 10u);
    EXPECT_EQ(entry.term, 3u);
    EXPECT_TRUE(entry.command.has_value());
    EXPECT_TRUE(entry.committed);
}

TEST_F(LogEntryTest, Comparison) {
    LogEntry e1(1, 5);
    LogEntry e2(2, 5);
    LogEntry e3(1, 5);

    EXPECT_TRUE(e1 < e2);
    EXPECT_FALSE(e2 < e1);
    EXPECT_EQ(e1, e3);
}

TEST_F(LogEntryTest, SerializationWithoutCommand) {
    LogEntry original(42, 7);
    original.max_ballot_seen = 10;
    original.max_ballot_accepted = 8;
    original.committed = true;
    original.is_no_op = true;

    // LogEntry's `to_marshal`/`from_marshal`
    // were replaced with `save(BinaryWriteArchive&)` /
    // `load(BinaryReadArchive&)`. Drive bytes through the same backing
    // Marshal via MarshalSink/MarshalSource so this test continues to
    // exercise an on-wire round-trip.
    Marshal m;
    {
        rrr::MarshalSink sink(&m);
        rrr::BinaryWriteArchive writer(make_sink_proxy(&sink));
        original.save(writer);
    }

    LogEntry restored;
    {
        rrr::MarshalSource src(&m);
        rrr::BinaryReadArchive reader(make_source_proxy(&src));
        restored.load(reader);
    }

    EXPECT_EQ(restored.slot_id, 42u);
    EXPECT_EQ(restored.term, 7u);
    EXPECT_EQ(restored.max_ballot_seen, 10u);
    EXPECT_EQ(restored.max_ballot_accepted, 8u);
    EXPECT_TRUE(restored.committed);
    EXPECT_TRUE(restored.is_no_op);
    EXPECT_FALSE(restored.command.has_value());
}

TEST_F(LogEntryTest, SerializationWithCommand) {
    auto cmd = std::make_shared<janus::TpcEmptyCommand>();
    LogEntry original(100, 20, cmd, true);

    // see SerializationWithoutCommand for
    // the to_marshal → save migration rationale.
    Marshal m;
    {
        rrr::MarshalSink sink(&m);
        rrr::BinaryWriteArchive writer(make_sink_proxy(&sink));
        original.save(writer);
    }

    // Verify serialization produced data
    EXPECT_GT(m.content_size(), 0u);

    // Note: Full deserialization of custom commands requires MarshallDeputy registration
    // which is done at application startup. Here we just verify serialization works.
    // The basic fields can still be deserialized:
    LogEntry partial;
    rrr::Deserialize_::deserialize(partial.slot_id, m);
    rrr::Deserialize_::deserialize(partial.term, m);

    EXPECT_EQ(partial.slot_id, 100u);
    EXPECT_EQ(partial.term, 20u);
}

// ============================================================================
// InMemoryLogStorage Tests
// ============================================================================

class InMemoryLogStorageTest : public ::testing::Test {
protected:
    InMemoryLogStorage storage;

    void SetUp() override {}
    void TearDown() override {}

    LogEntry make_entry(slotid_t slot, ballot_t term, bool committed = false) {
        LogEntry entry(slot, term);
        entry.committed = committed;
        return entry;
    }
};

// Single entry operations

TEST_F(InMemoryLogStorageTest, PutAndGet) {
    LogEntry entry = make_entry(1, 5);
    EXPECT_TRUE(storage.put(entry));

    auto result = storage.get(1);
    EXPECT_TRUE(result.is_some());
    auto retrieved = result.unwrap();
    EXPECT_EQ(retrieved.slot_id, 1u);
    EXPECT_EQ(retrieved.term, 5u);
}

TEST_F(InMemoryLogStorageTest, GetNonExistent) {
    auto result = storage.get(999);
    EXPECT_TRUE(result.is_none());
}

TEST_F(InMemoryLogStorageTest, PutOverwrite) {
    storage.put(make_entry(1, 5));
    storage.put(make_entry(1, 10));  // Overwrite

    auto result = storage.get(1);
    EXPECT_TRUE(result.is_some());
    auto entry = result.unwrap();
    EXPECT_EQ(entry.term, 10u);
}

TEST_F(InMemoryLogStorageTest, Remove) {
    storage.put(make_entry(1, 5));
    EXPECT_TRUE(storage.remove(1));
    EXPECT_TRUE(storage.get(1).is_none());
}

TEST_F(InMemoryLogStorageTest, RemoveNonExistent) {
    EXPECT_FALSE(storage.remove(999));
}

// Batch operations

TEST_F(InMemoryLogStorageTest, PutBatch) {
    std::vector<LogEntry> entries = {
        make_entry(1, 1),
        make_entry(2, 2),
        make_entry(3, 3)
    };
    EXPECT_TRUE(storage.put_batch(entries));

    EXPECT_EQ(storage.size(), 3u);
    EXPECT_TRUE(storage.get(1).is_some());
    EXPECT_TRUE(storage.get(2).is_some());
    EXPECT_TRUE(storage.get(3).is_some());
}

TEST_F(InMemoryLogStorageTest, GetRange) {
    storage.put(make_entry(1, 1));
    storage.put(make_entry(3, 3));
    storage.put(make_entry(5, 5));
    storage.put(make_entry(7, 7));

    auto range = storage.get_range(2, 6);
    EXPECT_EQ(range.size(), 2u);  // slots 3 and 5
}

TEST_F(InMemoryLogStorageTest, GetRangeEmpty) {
    storage.put(make_entry(10, 10));

    auto range = storage.get_range(1, 5);
    EXPECT_TRUE(range.empty());
}

TEST_F(InMemoryLogStorageTest, RemoveRange) {
    storage.put(make_entry(1, 1));
    storage.put(make_entry(2, 2));
    storage.put(make_entry(3, 3));
    storage.put(make_entry(4, 4));
    storage.put(make_entry(5, 5));

    EXPECT_TRUE(storage.remove_range(2, 4));

    EXPECT_TRUE(storage.get(1).is_some());
    EXPECT_TRUE(storage.get(2).is_none());
    EXPECT_TRUE(storage.get(3).is_none());
    EXPECT_TRUE(storage.get(4).is_some());
    EXPECT_TRUE(storage.get(5).is_some());
}

// Index queries

TEST_F(InMemoryLogStorageTest, GetFirstIndex) {
    EXPECT_EQ(storage.get_first_index(), 0u);  // Empty

    storage.put(make_entry(5, 1));
    storage.put(make_entry(3, 1));
    storage.put(make_entry(7, 1));

    EXPECT_EQ(storage.get_first_index(), 3u);
}

TEST_F(InMemoryLogStorageTest, GetLastIndex) {
    EXPECT_EQ(storage.get_last_index(), 0u);  // Empty

    storage.put(make_entry(5, 1));
    storage.put(make_entry(3, 1));
    storage.put(make_entry(7, 1));

    EXPECT_EQ(storage.get_last_index(), 7u);
}

TEST_F(InMemoryLogStorageTest, GetTerm) {
    storage.put(make_entry(1, 42));

    auto term = storage.get_term(1);
    EXPECT_TRUE(term.is_some());
    EXPECT_EQ(term.unwrap(), 42u);

    auto none = storage.get_term(999);
    EXPECT_TRUE(none.is_none());
}

TEST_F(InMemoryLogStorageTest, SizeAndEmpty) {
    EXPECT_TRUE(storage.empty());
    EXPECT_EQ(storage.size(), 0u);

    storage.put(make_entry(1, 1));
    EXPECT_FALSE(storage.empty());
    EXPECT_EQ(storage.size(), 1u);

    storage.put(make_entry(2, 2));
    EXPECT_EQ(storage.size(), 2u);
}

// Metadata operations

TEST_F(InMemoryLogStorageTest, SetAndGetMetadata) {
    EXPECT_TRUE(storage.set_metadata("term", "5"));
    EXPECT_TRUE(storage.set_metadata("vote", "node1"));

    auto term = storage.get_metadata("term");
    EXPECT_TRUE(term.is_some());
    EXPECT_EQ(term.unwrap(), "5");

    auto vote = storage.get_metadata("vote");
    EXPECT_TRUE(vote.is_some());
    EXPECT_EQ(vote.unwrap(), "node1");
}

TEST_F(InMemoryLogStorageTest, GetMetadataNonExistent) {
    auto result = storage.get_metadata("nonexistent");
    EXPECT_TRUE(result.is_none());
}

TEST_F(InMemoryLogStorageTest, OverwriteMetadata) {
    storage.set_metadata("key", "value1");
    storage.set_metadata("key", "value2");

    auto result = storage.get_metadata("key");
    EXPECT_TRUE(result.is_some());
    EXPECT_EQ(result.unwrap(), "value2");
}

// Lifecycle operations

TEST_F(InMemoryLogStorageTest, IsOpen) {
    EXPECT_TRUE(storage.is_open());
}

TEST_F(InMemoryLogStorageTest, Sync) {
    storage.put(make_entry(1, 1));
    EXPECT_TRUE(storage.sync());  // No-op for in-memory
}

TEST_F(InMemoryLogStorageTest, Close) {
    storage.put(make_entry(1, 1));
    storage.set_metadata("key", "value");

    EXPECT_TRUE(storage.close());
    EXPECT_FALSE(storage.is_open());

    // Operations on closed storage should fail/return empty
    EXPECT_FALSE(storage.put(make_entry(2, 2)));
    EXPECT_TRUE(storage.get(1).is_none());
    EXPECT_TRUE(storage.get_metadata("key").is_none());
}

TEST_F(InMemoryLogStorageTest, CloseIdempotent) {
    EXPECT_TRUE(storage.close());
    EXPECT_FALSE(storage.close());  // Already closed
}

TEST_F(InMemoryLogStorageTest, Clear) {
    storage.put(make_entry(1, 1));
    storage.put(make_entry(2, 2));
    storage.set_metadata("key", "value");

    EXPECT_TRUE(storage.clear());

    EXPECT_TRUE(storage.empty());
    EXPECT_TRUE(storage.get_metadata("key").is_none());
    EXPECT_TRUE(storage.is_open());  // Still open after clear
}

TEST_F(InMemoryLogStorageTest, Reopen) {
    storage.put(make_entry(1, 1));
    storage.close();
    storage.reopen();

    EXPECT_TRUE(storage.is_open());
    EXPECT_TRUE(storage.empty());  // Data was cleared on close
    EXPECT_TRUE(storage.put(make_entry(2, 2)));
}

// Thread safety tests

TEST_F(InMemoryLogStorageTest, ConcurrentPuts) {
    const int NUM_THREADS = 4;
    const int ENTRIES_PER_THREAD = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < ENTRIES_PER_THREAD; i++) {
                slotid_t slot = t * ENTRIES_PER_THREAD + i;
                storage.put(make_entry(slot, t));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(storage.size(), static_cast<size_t>(NUM_THREADS * ENTRIES_PER_THREAD));
}

TEST_F(InMemoryLogStorageTest, ConcurrentReadsAndWrites) {
    // Pre-populate some entries
    for (slotid_t i = 0; i < 50; i++) {
        storage.put(make_entry(i, 1));
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
                    auto result = storage.get(slot);
                    if (result.is_some()) {
                        read_count++;
                    }
                } else {
                    // Write
                    slotid_t slot = 100 + t * 100 + i;
                    storage.put(make_entry(slot, t));
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

TEST_F(InMemoryLogStorageTest, ConcurrentMetadata) {
    const int NUM_THREADS = 4;
    const int OPS_PER_THREAD = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([this, t]() {
            for (int i = 0; i < OPS_PER_THREAD; i++) {
                std::string key = "key_" + std::to_string(t);
                std::string value = "value_" + std::to_string(i);
                storage.set_metadata(key, value);
                storage.get_metadata(key);
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // All thread-specific keys should exist
    for (int t = 0; t < NUM_THREADS; t++) {
        std::string key = "key_" + std::to_string(t);
        EXPECT_TRUE(storage.get_metadata(key).is_some());
    }
}

// Edge cases

TEST_F(InMemoryLogStorageTest, LargeSlotIds) {
    slotid_t large_slot = 1000000000000ULL;
    storage.put(make_entry(large_slot, 1));

    auto result = storage.get(large_slot);
    EXPECT_TRUE(result.is_some());
    auto entry = result.unwrap();
    EXPECT_EQ(entry.slot_id, large_slot);
}

TEST_F(InMemoryLogStorageTest, ZeroSlotId) {
    storage.put(make_entry(0, 1));

    auto result = storage.get(0);
    EXPECT_TRUE(result.is_some());
    auto entry = result.unwrap();
    EXPECT_EQ(entry.slot_id, 0u);
}

TEST_F(InMemoryLogStorageTest, GetRangeInvalidRange) {
    storage.put(make_entry(1, 1));

    // start >= end
    auto range = storage.get_range(5, 3);
    EXPECT_TRUE(range.empty());
}

TEST_F(InMemoryLogStorageTest, RemoveRangeInvalidRange) {
    storage.put(make_entry(1, 1));

    // start >= end
    EXPECT_FALSE(storage.remove_range(5, 3));
    EXPECT_EQ(storage.size(), 1u);  // Nothing removed
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
