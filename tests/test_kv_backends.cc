// Gating tests for the namespace-switchable KV surface
// (docs/mako-nontxn-api-plan.md Phase 5; src/mako/kv_backends.hh).
//
// Two proofs:
//   1. TYPED_TEST over tag structs — the SAME test body runs against
//      kv_masstree, kv_silo, and kv_mako and asserts identical
//      observable behavior for the single-node-reachable subset.
//   2. Literal namespace switching — the same function body compiles
//      three times with only `namespace kv = ...;` changed.
//
// Scan boundary keys are chosen strictly BETWEEN stored keys so the
// expected result set is identical under any inclusive/exclusive
// boundary convention a backend might have.

#include <stdlib.h>

#include "benchmarks/bench.h"
#include "kv_backends.hh"

#include <gtest/gtest.h>

import std;

namespace {

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
// Backend tags: one type per namespace, same members.
// ---------------------------------------------------------------------------
struct MasstreeBackend {
    using Table = kv_masstree::Table;
    static Table* open(const std::string& n) { return kv_masstree::open(n); }
    static void thread_init() { kv_masstree::thread_init(); }
    static const char* prefix() { return "mt"; }
};
struct SiloBackend {
    using Table = kv_silo::Table;
    static Table* open(const std::string& n) { return kv_silo::open(n); }
    static void thread_init() { kv_silo::thread_init(); }
    static const char* prefix() { return "silo"; }
};
struct MakoBackend {
    using Table = kv_mako::Table;
    static Table* open(const std::string& n) { return kv_mako::open(n); }
    static void thread_init() { kv_mako::thread_init(); }
    static const char* prefix() { return "mako"; }
};

template <typename B>
class KvBackends : public ::testing::Test {
protected:
    void SetUp() override { B::thread_init(); }

    // Unique table per (backend, test) so key spaces never collide.
    typename B::Table* fresh_table(const std::string& tag) {
        return B::open(std::string(B::prefix()) + "_" + tag);
    }
};

using Backends = ::testing::Types<MasstreeBackend, SiloBackend, MakoBackend>;
TYPED_TEST_SUITE(KvBackends, Backends);

// ---------------------------------------------------------------------------
// Point ops: identical semantics on all three backends.
// ---------------------------------------------------------------------------
TYPED_TEST(KvBackends, PutGetRoundTripRawBytes) {
    auto* t = this->fresh_table("roundtrip");

    EXPECT_TRUE(t->put(lcdf::Str("k1"), "plain-value"));
    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("k1"), out));
    EXPECT_EQ(out, "plain-value");  // raw bytes back, no Encode leakage

    EXPECT_FALSE(t->get(lcdf::Str("missing"), out));
}

TYPED_TEST(KvBackends, PutReturnsNewlyInsertedAndOverwrites) {
    auto* t = this->fresh_table("putret");

    EXPECT_TRUE(t->put(lcdf::Str("k"), "one"));
    EXPECT_FALSE(t->put(lcdf::Str("k"), "two"));  // existed

    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("k"), out));
    EXPECT_EQ(out, "two");
}

TYPED_TEST(KvBackends, InsertIsPutIfAbsent) {
    auto* t = this->fresh_table("insert");

    EXPECT_TRUE(t->insert(lcdf::Str("k"), "first"));
    EXPECT_FALSE(t->insert(lcdf::Str("k"), "second"));

    std::string out;
    ASSERT_TRUE(t->get(lcdf::Str("k"), out));
    EXPECT_EQ(out, "first");  // dup insert must not overwrite
}

TYPED_TEST(KvBackends, RemoveSemantics) {
    auto* t = this->fresh_table("remove");

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
    auto* t = this->fresh_table("longval");

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
    auto* t = this->fresh_table("scan");
    for (int i = 0; i < 6; i++) {
        std::string k = "s" + std::to_string(i);
        ASSERT_TRUE(t->put(lcdf::Str(k), "val" + std::to_string(i)));
    }

    // ("s0", "s4") exclusive-ish boundaries: s1, s2, s3 on any convention.
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
    auto* t = this->fresh_table("scanstop");
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
    auto* t = this->fresh_table("rscan");
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

TYPED_TEST(KvBackends, OpenIsFindOrCreate) {
    auto* a = this->fresh_table("registry");
    auto* b = this->fresh_table("registry");
    EXPECT_EQ(a, b);

    ASSERT_TRUE(a->put(lcdf::Str("shared"), "seen-through-b"));
    std::string out;
    ASSERT_TRUE(b->get(lcdf::Str("shared"), out));
    EXPECT_EQ(out, "seen-through-b");
}

// ---------------------------------------------------------------------------
// The literal proof: the same body, three namespaces, one alias line.
// ---------------------------------------------------------------------------
#define KV_SMOKE_BODY(tbl_name)                                   \
    kv::thread_init();                                            \
    kv::Table* t = kv::open(tbl_name);                            \
    ASSERT_TRUE(t->put(lcdf::Str("nk"), "nv"));                   \
    std::string out;                                              \
    ASSERT_TRUE(t->get(lcdf::Str("nk"), out));                    \
    EXPECT_EQ(out, "nv");                                         \
    EXPECT_TRUE(t->remove(lcdf::Str("nk")));                      \
    EXPECT_FALSE(t->get(lcdf::Str("nk"), out));

TEST(KvNamespaceSwitch, Masstree) {
    namespace kv = kv_masstree;
    KV_SMOKE_BODY("ns_smoke_masstree")
}

TEST(KvNamespaceSwitch, Silo) {
    namespace kv = kv_silo;
    KV_SMOKE_BODY("ns_smoke_silo")
}

TEST(KvNamespaceSwitch, Mako) {
    namespace kv = kv_mako;
    KV_SMOKE_BODY("ns_smoke_mako")
}

}  // namespace
