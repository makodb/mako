// Gating tests for the consolidated three-backend story
// (docs/mako-nontxn-api-plan.md Phase 5, as revised): there is ONE
// non-transactional KV interface — the non-txn ops on
// abstract_ordered_index — with three implementations picked at
// construction time:
//
//     masstree_ordered_index      plain Masstree, no transactions
//     mbta_ordered_index          Silo (one-op OCC txn per op)
//     mbta_sharded_ordered_index  Mako routing over per-key tables
//
// Two proofs:
//   1. TYPED_TEST over factory tags — the SAME test body runs against
//      all three concrete types THROUGH abstract_ordered_index* and
//      asserts identical observable behavior for the
//      single-node-reachable subset.
//   2. exercise_any_backend(): one ordinary (non-template) function
//      taking abstract_ordered_index* serves every backend — the
//      consolidation is plain runtime polymorphism, no per-backend
//      vocabulary.
//
// Values are RAW BYTES per the interface contract: backends needing a
// storage encoding (mbta's EXTRA_BITS) apply it internally.
//
// Scan boundary keys are chosen strictly BETWEEN stored keys so the
// expected result set is identical under any inclusive/exclusive
// boundary convention a backend might have.

#include <stdlib.h>

#include "benchmarks/bench.h"
// masstree_ordered_index must precede mbta_wrapper (MassTrans's
// `#define RCU 1` vs imstring.h's template parameter).
#include "storage/masstree_ordered_index.hh"
#include "storage/mbta_wrapper.hh"
#include "storage/mbta_sharded_ordered_index.hh"

#include <gtest/gtest.h>

import std;

namespace {

using mbta_type = mbta_ordered_index::mbta_type;

std::atomic<int> g_tid_counter{0};
std::atomic<long> g_table_id{7000};

// Per-thread Silo/Masstree bring-up — the same minimal contract
// test_silo_nontxn_api.cc uses. One shared storage runtime serves all
// three backends, so one init covers them (masstree_ordered_index
// needs the threadinfo/RCU part).
void engine_thread_init() {
    static thread_local bool done = false;
    if (done) return;
    done = true;
    static std::once_flag once;
    std::call_once(once, [] { mbta_type::static_init(); });
    TThread::set_id(g_tid_counter.fetch_add(1));
    TThread::set_mode(0);
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    mbta_type::thread_init();
}

// A scan_callback that collects (key, value) pairs and optionally
// stops early after `limit` entries.
class Collect : public abstract_ordered_index::scan_callback {
public:
    explicit Collect(size_t limit = SIZE_MAX) : limit_(limit) {}
    bool invoke(const char* keyp, size_t keylen,
                const std::string& value) override {
        pairs.emplace_back(std::string(keyp, keylen), value);
        return pairs.size() < limit_;
    }
    std::vector<std::pair<std::string, std::string>> pairs;
private:
    size_t limit_;
};

// ---------------------------------------------------------------------------
// Backend tags: each constructs its concrete type; tests only ever see
// abstract_ordered_index*.
// ---------------------------------------------------------------------------
struct MasstreeBackend {
    static abstract_ordered_index* make(const std::string& name) {
        return new masstree_ordered_index(name, g_table_id.fetch_add(1));
    }
    static const char* prefix() { return "mt"; }
};
struct SiloBackend {
    static abstract_ordered_index* make(const std::string& name) {
        return new mbta_ordered_index(name, g_table_id.fetch_add(1),
                                      /*db=*/nullptr);
    }
    static const char* prefix() { return "silo"; }
};
struct MakoBackend {
    static abstract_ordered_index* make(const std::string& name) {
        auto* local = new mbta_ordered_index(name, g_table_id.fetch_add(1),
                                             /*db=*/nullptr);
        return new mbta_sharded_ordered_index(
            name, std::vector<abstract_ordered_index*>{local});
    }
    static const char* prefix() { return "mako"; }
};

template <typename B>
class KvBackends : public ::testing::Test {
protected:
    void SetUp() override { engine_thread_init(); }

    // Fresh table per (backend, test) so key spaces never collide.
    // Leaked deliberately: MassTrans teardown wants RCU quiescence a
    // unit test can't provide; tables are small.
    abstract_ordered_index* fresh_table(const std::string& tag) {
        return B::make(std::string(B::prefix()) + "_" + tag);
    }
};

using Backends = ::testing::Types<MasstreeBackend, SiloBackend, MakoBackend>;
TYPED_TEST_SUITE(KvBackends, Backends);

// ---------------------------------------------------------------------------
// Point ops: identical semantics on all three backends.
// ---------------------------------------------------------------------------
TYPED_TEST(KvBackends, PutGetRoundTripRawBytes) {
    abstract_ordered_index* t = this->fresh_table("roundtrip");

    EXPECT_TRUE(t->put(lcdf::Str("k1"), "plain-value"));
    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("k1"), out));
    EXPECT_EQ(out, "plain-value");  // raw bytes back, no encoding leakage

    EXPECT_FALSE(t->get(lcdf::Str("missing"), out));
}

TYPED_TEST(KvBackends, PutReturnsNewlyInsertedAndOverwrites) {
    abstract_ordered_index* t = this->fresh_table("putret");

    EXPECT_TRUE(t->put(lcdf::Str("k"), "one"));
    EXPECT_FALSE(t->put(lcdf::Str("k"), "two"));  // existed

    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("k"), out));
    EXPECT_EQ(out, "two");
}

TYPED_TEST(KvBackends, InsertIsPutIfAbsent) {
    abstract_ordered_index* t = this->fresh_table("insert");

    EXPECT_TRUE(t->insert(lcdf::Str("k"), "first"));
    EXPECT_FALSE(t->insert(lcdf::Str("k"), "second"));

    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("k"), out));
    EXPECT_EQ(out, "first");  // dup insert must not overwrite
}

TYPED_TEST(KvBackends, RemoveSemantics) {
    abstract_ordered_index* t = this->fresh_table("remove");

    ASSERT_TRUE(t->put(lcdf::Str("k"), "victim"));
    EXPECT_TRUE(t->remove(lcdf::Str("k")));

    std::string out;
    EXPECT_FALSE(t->get(lcdf::Str("k"), out));
    EXPECT_FALSE(t->remove(lcdf::Str("k")));  // absent

    // Reinsert after remove works and reads back the new value.
    EXPECT_TRUE(t->put(lcdf::Str("k"), "reborn"));
    ASSERT_TRUE(t->get(lcdf::Str("k"), out));
    EXPECT_EQ(out, "reborn");
}

TYPED_TEST(KvBackends, LongValuesSurviveRoundTrip) {
    abstract_ordered_index* t = this->fresh_table("longval");

    // Longer than any internal suffix/prefix convention.
    const std::string big(4096, 'x');
    ASSERT_TRUE(t->put(lcdf::Str("big"), big));
    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("big"), out));
    EXPECT_EQ(out, big);
}

// ---------------------------------------------------------------------------
// Scans: [start, end) ascending; rscan descending. Boundary keys sit
// strictly between stored keys, so expected contents are
// convention-independent.
// ---------------------------------------------------------------------------
TYPED_TEST(KvBackends, ScanForwardSortedWithValues) {
    abstract_ordered_index* t = this->fresh_table("scan");
    for (int i = 0; i < 6; i++) {
        std::string k = "s" + std::to_string(i);
        ASSERT_TRUE(t->put(lcdf::Str(k), "val" + std::to_string(i)));
    }

    // Between s0/s1 up to between s3/s4: s1, s2, s3 on any convention.
    Collect cb;
    const std::string start = "s0z", end = "s3z";
    t->scan(start, &end, cb);
    ASSERT_EQ(cb.pairs.size(), 3u);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(cb.pairs[i].first, "s" + std::to_string(i + 1));
        EXPECT_EQ(cb.pairs[i].second, "val" + std::to_string(i + 1));
    }

    // Open-ended scan from midway: s3, s4, s5.
    Collect cb2;
    const std::string start2 = "s2z";
    t->scan(start2, nullptr, cb2);
    ASSERT_EQ(cb2.pairs.size(), 3u);
    EXPECT_EQ(cb2.pairs.front().first, "s3");
    EXPECT_EQ(cb2.pairs.back().first, "s5");
}

TYPED_TEST(KvBackends, ScanEarlyStop) {
    abstract_ordered_index* t = this->fresh_table("scanstop");
    for (int i = 0; i < 6; i++) {
        std::string k = "e" + std::to_string(i);
        ASSERT_TRUE(t->put(lcdf::Str(k), "v" + std::to_string(i)));
    }

    Collect cb(/*limit=*/2);
    const std::string start = "e0";
    t->scan(start, nullptr, cb);
    EXPECT_EQ(cb.pairs.size(), 2u);
}

TYPED_TEST(KvBackends, RScanDescending) {
    abstract_ordered_index* t = this->fresh_table("rscan");
    for (int i = 0; i < 6; i++) {
        std::string k = "r" + std::to_string(i);
        ASSERT_TRUE(t->put(lcdf::Str(k), "v" + std::to_string(i)));
    }

    // From between r4/r5 down to between r0/r1: r4, r3, r2, r1.
    Collect cb;
    const std::string start = "r4z", end = "r0z";
    t->rscan(start, &end, cb);
    ASSERT_EQ(cb.pairs.size(), 4u);
    for (int i = 0; i < 4; i++) {
        EXPECT_EQ(cb.pairs[i].first, "r" + std::to_string(4 - i));
        EXPECT_EQ(cb.pairs[i].second, "v" + std::to_string(4 - i));
    }
}

// ---------------------------------------------------------------------------
// The consolidation proof: one ordinary function, any backend.
// ---------------------------------------------------------------------------
void exercise_any_backend(abstract_ordered_index* t) {
    ASSERT_TRUE(t->put(lcdf::Str("nk"), "nv"));
    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("nk"), out));
    EXPECT_EQ(out, "nv");
    EXPECT_TRUE(t->remove(lcdf::Str("nk")));
    EXPECT_FALSE(t->get(lcdf::Str("nk"), out));
}

TEST(KvBackendSwitch, OneFunctionServesAllThree) {
    engine_thread_init();
    exercise_any_backend(MasstreeBackend::make("switch_mt"));
    exercise_any_backend(SiloBackend::make("switch_silo"));
    exercise_any_backend(MakoBackend::make("switch_mako"));
}

}  // namespace
