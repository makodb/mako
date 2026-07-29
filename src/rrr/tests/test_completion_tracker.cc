/**
 * Completion Tracker Unit Tests
 * Request Completion Tracking
 *
 * Tests for:
 * - CompletionTrackerConfig: presets and configuration
 * - CompletedEntry: expiration checking
 * - CompletionTracker: mark, query, eviction, TTL
 * - CompletionQueryResult: status and helpers
 */

#include <stdint.h>
#include <stddef.h>

#include <gtest/gtest.h>
#include "../rrr.hpp"

import std;

using namespace rrr;
using namespace std::chrono;

// ===========================================================================
// CompletionTrackerConfig Tests
// ===========================================================================

class CompletionTrackerConfigTest : public ::testing::Test {};

TEST_F(CompletionTrackerConfigTest, DefaultsPreset) {
    auto cfg = CompletionTrackerConfig::defaults();

    EXPECT_EQ(cfg.ttl_ms, 60000);
    EXPECT_EQ(cfg.max_entries, 100000);
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(CompletionTrackerConfigTest, SmallPreset) {
    auto cfg = CompletionTrackerConfig::small();

    EXPECT_EQ(cfg.ttl_ms, 30000);
    EXPECT_EQ(cfg.max_entries, 10000);
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(CompletionTrackerConfigTest, LargePreset) {
    auto cfg = CompletionTrackerConfig::large();

    EXPECT_EQ(cfg.ttl_ms, 300000);
    EXPECT_EQ(cfg.max_entries, 1000000);
    EXPECT_TRUE(cfg.enabled);
}

TEST_F(CompletionTrackerConfigTest, DisabledPreset) {
    auto cfg = CompletionTrackerConfig::disabled();

    EXPECT_FALSE(cfg.enabled);
}

// ===========================================================================
// CompletedEntry Tests
// ===========================================================================

class CompletedEntryTest : public ::testing::Test {};

TEST_F(CompletedEntryTest, ZeroFactory) {
    auto entry = CompletedEntry::new_(0, 0);

    EXPECT_EQ(entry.xid, 0);
    EXPECT_EQ(entry.timestamp_ms, 0);
}

TEST_F(CompletedEntryTest, ExplicitConstruction) {
    auto entry = CompletedEntry::new_(12345, 1000);

    EXPECT_EQ(entry.xid, 12345);
    EXPECT_EQ(entry.timestamp_ms, 1000);
}

TEST_F(CompletedEntryTest, IsExpired) {
    auto entry = CompletedEntry::new_(1, 1000);

    // Not expired
    EXPECT_FALSE(entry.is_expired(1050, 100));  // 1050 <= 1000 + 100 = 1100
    EXPECT_FALSE(entry.is_expired(1100, 100));  // exactly at expiry

    // Expired
    EXPECT_TRUE(entry.is_expired(1101, 100));   // 1101 > 1100
    EXPECT_TRUE(entry.is_expired(2000, 100));

    // No expiration when TTL is 0
    EXPECT_FALSE(entry.is_expired(99999, 0));
}

// ===========================================================================
// CompletionTracker Tests
// ===========================================================================

class CompletionTrackerTest : public ::testing::Test {
protected:
    CompletionTracker tracker_;

    // Explicit noexcept dtor — gtest's `virtual ~Test()` is defaulted in
    // the .cc TU, so it's noexcept(true). `CompletionTracker` transitively
    // owns a `rusty::HashSet<int64_t>` whose RawTable dtor is
    // `noexcept(false)`, which would make this fixture's implicit dtor
    // `noexcept(false)` — strictly laxer than the gtest base, which clang
    // rejects. Promise noexcept and accept that an exception escaping
    // CompletionTracker's drop will terminate (it never does in practice).
    ~CompletionTrackerTest() noexcept override = default;

    void SetUp() override {
        tracker_.set_config(CompletionTrackerConfig::defaults());
    }

    uint64_t current_time_ms() {
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()
        ).count();
    }
};

TEST_F(CompletionTrackerTest, InitialState) {
    EXPECT_TRUE(tracker_.enabled());
    EXPECT_EQ(tracker_.size(), 0);
    EXPECT_EQ(tracker_.total_tracked(), 0);
    EXPECT_EQ(tracker_.queries(), 0);
    EXPECT_EQ(tracker_.query_hits(), 0);
    EXPECT_EQ(tracker_.evictions(), 0);
}

TEST_F(CompletionTrackerTest, MarkAndQuery) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);

    EXPECT_EQ(tracker_.size(), 1);
    EXPECT_EQ(tracker_.total_tracked(), 1);

    bool found = tracker_.is_completed(100, now);
    EXPECT_TRUE(found);
    EXPECT_EQ(tracker_.queries(), 1);
    EXPECT_EQ(tracker_.query_hits(), 1);
}

TEST_F(CompletionTrackerTest, QueryMiss) {
    uint64_t now = current_time_ms();

    bool found = tracker_.is_completed(100, now);
    EXPECT_FALSE(found);
    EXPECT_EQ(tracker_.queries(), 1);
    EXPECT_EQ(tracker_.query_hits(), 0);
}

TEST_F(CompletionTrackerTest, MarkDuplicateIgnored) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);
    tracker_.mark_completed(100, now + 1000);  // Same XID again

    EXPECT_EQ(tracker_.size(), 1);
    EXPECT_EQ(tracker_.total_tracked(), 1);  // Only counted once
}

TEST_F(CompletionTrackerTest, MultipleXIDs) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);
    tracker_.mark_completed(200, now);
    tracker_.mark_completed(300, now);

    EXPECT_EQ(tracker_.size(), 3);
    EXPECT_EQ(tracker_.total_tracked(), 3);

    EXPECT_TRUE(tracker_.is_completed(100, now));
    EXPECT_TRUE(tracker_.is_completed(200, now));
    EXPECT_TRUE(tracker_.is_completed(300, now));
    EXPECT_FALSE(tracker_.is_completed(400, now));
}

TEST_F(CompletionTrackerTest, TTLExpiration) {
    auto cfg = CompletionTrackerConfig::new_();
    cfg.ttl_ms = 100;  // 100ms TTL
    tracker_.set_config(cfg);

    uint64_t now = current_time_ms();
    tracker_.mark_completed(100, now);

    // Should find immediately
    EXPECT_TRUE(tracker_.is_completed(100, now));

    // Should not find after TTL
    EXPECT_FALSE(tracker_.is_completed(100, now + 200));
}

TEST_F(CompletionTrackerTest, EvictionOnCapacity) {
    auto cfg = CompletionTrackerConfig::new_();
    cfg.max_entries = 3;
    cfg.ttl_ms = 60000;
    tracker_.set_config(cfg);

    uint64_t now = current_time_ms();

    // Add 4 entries (max is 3)
    tracker_.mark_completed(100, now);
    tracker_.mark_completed(200, now);
    tracker_.mark_completed(300, now);
    tracker_.mark_completed(400, now);  // Should evict oldest (100)

    EXPECT_EQ(tracker_.size(), 3);
    EXPECT_EQ(tracker_.evictions(), 1);

    // XID 100 should be evicted
    EXPECT_FALSE(tracker_.is_completed(100, now));

    // XIDs 200, 300, 400 should exist
    EXPECT_TRUE(tracker_.is_completed(200, now));
    EXPECT_TRUE(tracker_.is_completed(300, now));
    EXPECT_TRUE(tracker_.is_completed(400, now));
}

TEST_F(CompletionTrackerTest, Remove) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);
    EXPECT_EQ(tracker_.size(), 1);

    bool removed = tracker_.remove(100);
    EXPECT_TRUE(removed);
    EXPECT_EQ(tracker_.size(), 0);

    // Remove non-existent
    removed = tracker_.remove(100);
    EXPECT_FALSE(removed);
}

TEST_F(CompletionTrackerTest, Clear) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);
    tracker_.mark_completed(200, now);
    tracker_.mark_completed(300, now);

    EXPECT_EQ(tracker_.size(), 3);

    tracker_.clear();
    EXPECT_EQ(tracker_.size(), 0);
}

TEST_F(CompletionTrackerTest, DisabledTrackerDoesNotStore) {
    tracker_.set_config(CompletionTrackerConfig::disabled());

    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);
    EXPECT_EQ(tracker_.size(), 0);

    EXPECT_FALSE(tracker_.is_completed(100, now));
}

TEST_F(CompletionTrackerTest, HitRateCalculation) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);

    // 2 hits
    tracker_.is_completed(100, now);
    tracker_.is_completed(100, now);

    // 1 miss
    tracker_.is_completed(999, now);

    EXPECT_EQ(tracker_.queries(), 3);
    EXPECT_EQ(tracker_.query_hits(), 2);
    EXPECT_NEAR(tracker_.hit_rate(), 2.0/3.0, 0.01);
}

TEST_F(CompletionTrackerTest, ResetStats) {
    uint64_t now = current_time_ms();

    tracker_.mark_completed(100, now);
    tracker_.is_completed(100, now);
    tracker_.is_completed(999, now);

    EXPECT_GT(tracker_.total_tracked(), 0);
    EXPECT_GT(tracker_.queries(), 0);

    tracker_.reset_stats();

    EXPECT_EQ(tracker_.total_tracked(), 0);
    EXPECT_EQ(tracker_.queries(), 0);
    EXPECT_EQ(tracker_.query_hits(), 0);
    EXPECT_EQ(tracker_.evictions(), 0);

    // Size should not be affected
    EXPECT_EQ(tracker_.size(), 1);
}

TEST_F(CompletionTrackerTest, EvictExpired) {
    auto cfg = CompletionTrackerConfig::new_();
    cfg.ttl_ms = 100;  // 100ms TTL
    tracker_.set_config(cfg);

    uint64_t base_time = 1000000;

    // Add entries at different times (50ms apart)
    for (int i = 1; i <= 5; i++) {
        tracker_.mark_completed(i * 100, base_time + i * 50);
    }

    EXPECT_EQ(tracker_.size(), 5);

    // Evict at base_time + 251
    // Entry 100 (base+50) expires at base+150 - evicted
    // Entry 200 (base+100) expires at base+200 - evicted
    // Entry 300 (base+150) expires at base+250 - evicted
    // Entry 400 (base+200) expires at base+300 - kept
    // Entry 500 (base+250) expires at base+350 - kept
    size_t evicted = tracker_.evict_expired(base_time + 251);

    EXPECT_EQ(evicted, 3);
    EXPECT_EQ(tracker_.size(), 2);
}

TEST_F(CompletionTrackerTest, ThreadSafety) {
    const int num_threads = 4;
    const int ops_per_thread = 1000;
    std::atomic<int> completed{0};

    auto worker = [&](int thread_id) {
        int64_t base_xid = thread_id * ops_per_thread;
        uint64_t now = current_time_ms();

        for (int i = 0; i < ops_per_thread; i++) {
            tracker_.mark_completed(base_xid + i, now);
            tracker_.is_completed(base_xid + i, now);
        }

        completed++;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i);
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(completed, num_threads);
    // All operations should have succeeded without crashes
    EXPECT_LE(tracker_.size(), tracker_.config().max_entries);
}

// ===========================================================================
// CompletionQueryResult Tests
// ===========================================================================

class CompletionQueryResultTest : public ::testing::Test {};

TEST_F(CompletionQueryResultTest, FactoryNew) {
    auto result = CompletionQueryResult::new_();

    EXPECT_EQ(result.status, CompletionStatus::NOT_FOUND);
    EXPECT_EQ(result.error_code, 0);
    EXPECT_FALSE(result.has_cached_response);
    EXPECT_FALSE(result.is_completed());
}

TEST_F(CompletionQueryResultTest, NotFound) {
    auto result = CompletionQueryResult::not_found();

    EXPECT_EQ(result.status, CompletionStatus::NOT_FOUND);
    EXPECT_FALSE(result.is_completed());
}

TEST_F(CompletionQueryResultTest, CompletedSuccess) {
    auto result = CompletionQueryResult::completed(0, true);

    EXPECT_EQ(result.status, CompletionStatus::COMPLETED);
    EXPECT_EQ(result.error_code, 0);
    EXPECT_TRUE(result.has_cached_response);
    EXPECT_TRUE(result.is_completed());
}

TEST_F(CompletionQueryResultTest, CompletedWithError) {
    auto result = CompletionQueryResult::completed(-1, false);

    EXPECT_EQ(result.status, CompletionStatus::COMPLETED_WITH_ERROR);
    EXPECT_EQ(result.error_code, -1);
    EXPECT_FALSE(result.has_cached_response);
    EXPECT_TRUE(result.is_completed());
}

TEST_F(CompletionQueryResultTest, Expired) {
    auto result = CompletionQueryResult::expired();

    EXPECT_EQ(result.status, CompletionStatus::EXPIRED);
    EXPECT_FALSE(result.is_completed());
}

TEST_F(CompletionQueryResultTest, StatusToString) {
    EXPECT_STREQ(completion_status_to_string(CompletionStatus::NOT_FOUND), "NOT_FOUND");
    EXPECT_STREQ(completion_status_to_string(CompletionStatus::COMPLETED), "COMPLETED");
    EXPECT_STREQ(completion_status_to_string(CompletionStatus::COMPLETED_WITH_ERROR), "COMPLETED_WITH_ERROR");
    EXPECT_STREQ(completion_status_to_string(CompletionStatus::EXPIRED), "EXPIRED");
    EXPECT_STREQ(completion_status_to_string(static_cast<CompletionStatus>(99)), "UNKNOWN");
}

// ===========================================================================
// Main
// ===========================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
