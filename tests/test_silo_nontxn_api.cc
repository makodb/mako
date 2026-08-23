// Gating tests for docs/storage-interface.md — the
// non-transactional (Masstree-shape) API added to Silo's layers:
//
//   1. MassTrans level:   insert / scan / rscan (Phase 1) plus the
//                          pre-existing put / get / remove.
//   2. L3 level:          abstract_ordered_index's six non-txn virtual
//                          methods, dispatched through a base pointer
//                          into mbta_ordered_index (Phase 2).
//   3. Sharded level:     mbta_sharded_ordered_index's routing mirrors.
//   4. Interleaving:      non-txn reads vs. staged/committed txn writes.
//   5. Default impls:     backends without overrides abort loudly.
//
// Setup mirrors the essential parts of mbta_wrapper::thread_init()
// without requiring a transport::Configuration: TThread id + mode,
// MassTrans static_init + per-thread thread_init.

#include <stdlib.h>

#include "benchmarks/bench.h"
#include "storage/mbta_wrapper.hh"
#include "storage/mbta_sharded_ordered_index.hh"
#include "lib/common.h"

#include <gtest/gtest.h>

import std;

namespace {

using mbta_type = mbta_table;

std::atomic<int> g_tid_counter{0};

// Per-thread Silo/STO initialization. Every thread that touches
// MassTrans (directly or via the wrappers) must call this once.
void silo_thread_init() {
    TThread::set_id(g_tid_counter.fetch_add(1));
    TThread::set_mode(0);
    // These tests exercise the single-version Silo path used by mako-local.
    // The process-wide default is Mako's distributed multiversion role.
    TThread::disable_multiversion();
    TThread::readset_shard_bits = 0;
    TThread::writeset_shard_bits = 0;
    TThread::transget_without_throw = false;
    TThread::transget_without_stable = false;
    mbta_type::thread_init();
}

// Process-wide init, once.
void silo_static_init() {
    static bool done = false;
    if (!done) {
        done = true;
        mbta_type::static_init();
        silo_thread_init();  // the gtest main thread
    }
}

class SiloNonTxnApi : public ::testing::Test {
protected:
    void SetUp() override {
        silo_static_init();
    }

    // Fresh table per test so key spaces don't collide.
    // Leaked deliberately: MassTrans teardown wants RCU quiescence
    // that a unit test can't easily provide; tables are small.
    mbta_ordered_index* make_table(const std::string& name) {
        static long table_id = 100;
        return mbta_index_build(name, table_id++);
    }
};

// A oi_scan_callback that collects (key, value) pairs and optionally
// stops early after `limit` entries.
class CollectCallback : public oi_scan_callback {
public:
    explicit CollectCallback(size_t limit = SIZE_MAX) : limit_(limit) {}
    bool invoke(const char* keyp, size_t keylen,
                const std::string& value) override {
        pairs.emplace_back(std::string(keyp, keylen), value);
        return pairs.size() < limit_;
    }
    std::vector<std::pair<std::string, std::string>> pairs;
private:
    size_t limit_;
};

// ===========================================================================
// 1. MassTrans level
// ===========================================================================

// Direct MassTrans instances are heap-allocated and deliberately
// leaked (same reason as make_table: teardown wants RCU quiescence).
static mbta_type& make_masstrans(long id, const char* name) {
    auto* mt = new mbta_type();
    mt->set_table_id(id);
    mt->set_is_remote(false);
    mt->set_table_name(name);
    return *mt;
}

TEST_F(SiloNonTxnApi, MassTransPutGetRoundTrip) {
    mbta_type& mt = make_masstrans(9001, "mt_direct");

    const std::string val = mako::Encode("hello-masstrans");
    EXPECT_TRUE(mt.put(lcdf::Str("k1"), val));

    std::string out;
    EXPECT_TRUE(mt.get(lcdf::Str("k1"), out));
    // MassTrans returns the raw stored value (Encode padding intact):
    // compare the payload prefix.
    ASSERT_GE(out.size(), std::string("hello-masstrans").size());
    EXPECT_EQ(out.substr(0, 15), "hello-masstrans");

    EXPECT_FALSE(mt.get(lcdf::Str("absent"), out));
}

TEST_F(SiloNonTxnApi, MassTransInsertIsPutIfAbsent) {
    mbta_type& mt = make_masstrans(9002, "mt_insert");

    const std::string v1 = mako::Encode("first");
    const std::string v2 = mako::Encode("second");

    EXPECT_TRUE(mt.insert(lcdf::Str("dup"), v1));   // new key
    EXPECT_FALSE(mt.insert(lcdf::Str("dup"), v2));  // existing key

    std::string out;
    ASSERT_TRUE(mt.get(lcdf::Str("dup"), out));
    EXPECT_EQ(out.substr(0, 5), "first");  // second insert must not overwrite
}

TEST_F(SiloNonTxnApi, MassTransRemovePresentAndAbsent) {
    mbta_type& mt = make_masstrans(9003, "mt_remove");

    const std::string val = mako::Encode("gone-soon");
    ASSERT_TRUE(mt.put(lcdf::Str("victim"), val));

    EXPECT_TRUE(mt.remove(lcdf::Str("victim")));
    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("victim"), out));

    // Absent-key remove returns false and must not crash (guarded
    // deallocate in MassTrans::remove).
    EXPECT_FALSE(mt.remove(lcdf::Str("never-existed")));
}

TEST_F(SiloNonTxnApi, MassTransScanInOrderAndRScanReverse) {
    mbta_type& mt = make_masstrans(9004, "mt_scan");

    for (int i = 0; i < 5; i++) {
        std::string k = "scan_" + std::to_string(i);
        ASSERT_TRUE(mt.put(lcdf::Str(k), mako::Encode("v" + std::to_string(i))));
    }

    std::vector<std::string> keys;
    mt.scan(lcdf::Str("scan_0"), lcdf::Str("scan_5"),
            [&](lcdf::Str key, std::string&) {
                keys.emplace_back(key.data(), key.length());
                return true;
            });
    ASSERT_EQ(keys.size(), 5u);
    EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));

    std::vector<std::string> rkeys;
    mt.rscan(lcdf::Str("scan_5"), lcdf::Str("scan_0"),
             [&](lcdf::Str key, std::string&) {
                 rkeys.emplace_back(key.data(), key.length());
                 return true;
             });
    ASSERT_GE(rkeys.size(), 1u);
    EXPECT_TRUE(std::is_sorted(rkeys.rbegin(), rkeys.rend()));
}

TEST_F(SiloNonTxnApi, MassTransTransactionalScanBoundsAndEarlyStop) {
    mbta_type& mt = make_masstrans(9005, "mt_tx_scan_bounds");
    for (const char key : {'a', 'b', 'c', 'd', 'e'}) {
        const std::string value = mako::Encode(std::string("value-") + key);
        ASSERT_TRUE(mt.put(lcdf::Str(&key, 1), value));
    }

    auto collect_forward = [&](lcdf::Str begin, lcdf::Str end,
                               size_t limit = SIZE_MAX) {
        std::vector<std::string> keys;
        mt.transQuery(begin, end, [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return keys.size() < limit;
        });
        return keys;
    };
    auto collect_reverse = [&](lcdf::Str begin, lcdf::Str end,
                               size_t limit = SIZE_MAX) {
        std::vector<std::string> keys;
        mt.transRQuery(begin, end, [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return keys.size() < limit;
        });
        return keys;
    };

    Sto::start_transaction();
    EXPECT_EQ(collect_forward(lcdf::Str("b"), lcdf::Str("e")),
              (std::vector<std::string>{"b", "c", "d"}));
    EXPECT_EQ(collect_reverse(lcdf::Str("e"), lcdf::Str("b")),
              (std::vector<std::string>{"e", "d", "c"}));
    EXPECT_TRUE(collect_forward(lcdf::Str("c"), lcdf::Str("c")).empty());
    EXPECT_TRUE(collect_reverse(lcdf::Str("c"), lcdf::Str("c")).empty());
    EXPECT_EQ(collect_forward(lcdf::Str("d"), lcdf::Str()),
              (std::vector<std::string>{"d", "e"}));
    EXPECT_EQ(collect_reverse(lcdf::Str("c"), lcdf::Str()),
              (std::vector<std::string>{"c", "b", "a"}));

    // A false callback return stops immediately. The API returns void and has
    // no cursor: resuming at the last delivered key repeats it because both
    // scan start bounds are inclusive.
    EXPECT_EQ(collect_forward(lcdf::Str("b"), lcdf::Str(), 2),
              (std::vector<std::string>{"b", "c"}));
    EXPECT_EQ(collect_forward(lcdf::Str("c"), lcdf::Str("e"), 2),
              (std::vector<std::string>{"c", "d"}));
    EXPECT_EQ(collect_reverse(lcdf::Str("e"), lcdf::Str(), 2),
              (std::vector<std::string>{"e", "d"}));
    EXPECT_EQ(collect_reverse(lcdf::Str("d"), lcdf::Str("a"), 2),
              (std::vector<std::string>{"d", "c"}));
    EXPECT_TRUE(Sto::try_commit_no_paxos());
}

#if READ_MY_WRITES
TEST_F(SiloNonTxnApi, MassTransTransactionalScansReadOwnPointMutations) {
    mbta_type& mt = make_masstrans(9006, "mt_tx_scan_ryw");
    const std::string old_a = mako::Encode("old-a");
    const std::string old_c = mako::Encode("old-c");
    const std::string old_e = mako::Encode("old-e");
    const std::string old_g = mako::Encode("old-g");
    const std::string new_a = mako::Encode("new-a");
    const std::string new_b = mako::Encode("new-b");
    const std::string first_d = mako::Encode("first-d");
    const std::string new_d = mako::Encode(std::string(4096, 'd'));
    const std::string transient_e = mako::Encode("transient-e");
    const std::string new_g = mako::Encode("new-g");

    ASSERT_TRUE(mt.put(lcdf::Str("a"), old_a));
    ASSERT_TRUE(mt.put(lcdf::Str("c"), old_c));
    ASSERT_TRUE(mt.put(lcdf::Str("e"), old_e));
    ASSERT_TRUE(mt.put(lcdf::Str("g"), old_g));

    auto stage_and_scan = [&](bool commit) {
        Sto::start_transaction();
        EXPECT_TRUE(mt.transPut(lcdf::Str("a"), StringWrapper(new_a)));
        EXPECT_FALSE(mt.transInsert(lcdf::Str("b"), StringWrapper(new_b)));
        EXPECT_TRUE(mt.transDelete(lcdf::Str("c")));
        EXPECT_FALSE(mt.transInsert(lcdf::Str("d"), StringWrapper(first_d)));
        EXPECT_TRUE(mt.transPut(lcdf::Str("d"), StringWrapper(new_d)));
        EXPECT_TRUE(mt.transPut(lcdf::Str("e"), StringWrapper(transient_e)));
        EXPECT_TRUE(mt.transDelete(lcdf::Str("e")));
        EXPECT_TRUE(mt.transDelete(lcdf::Str("g")));
        EXPECT_FALSE(mt.transPut(lcdf::Str("g"), StringWrapper(new_g)));

        std::vector<std::pair<std::string, std::string>> forward;
        mt.transQuery(lcdf::Str("a"), lcdf::Str("h"),
                      [&](lcdf::Str key, std::string& value) {
            // Both callback arguments are borrowed; retain owned copies only.
            forward.emplace_back(std::string(key.data(), key.length()), value);
            return true;
        });
        const std::vector<std::pair<std::string, std::string>> expected = {
            {"a", new_a}, {"b", new_b}, {"d", new_d}, {"g", new_g},
        };
        EXPECT_EQ(forward, expected);

        std::vector<std::pair<std::string, std::string>> reverse;
        mt.transRQuery(lcdf::Str("h"), lcdf::Str(),
                       [&](lcdf::Str key, std::string& value) {
            reverse.emplace_back(std::string(key.data(), key.length()), value);
            return true;
        });
        EXPECT_EQ(reverse,
                  (std::vector<std::pair<std::string, std::string>>{
                      {"g", new_g}, {"d", new_d}, {"b", new_b}, {"a", new_a},
                  }));

        if (commit)
            EXPECT_TRUE(Sto::try_commit_no_paxos());
        else
            Sto::silent_abort();
    };

    stage_and_scan(false);
    std::string out;
    EXPECT_TRUE(mt.get(lcdf::Str("a"), out));
    EXPECT_EQ(out, old_a);
    EXPECT_FALSE(mt.get(lcdf::Str("b"), out));
    EXPECT_TRUE(mt.get(lcdf::Str("c"), out));
    EXPECT_EQ(out, old_c);
    EXPECT_FALSE(mt.get(lcdf::Str("d"), out));
    EXPECT_TRUE(mt.get(lcdf::Str("e"), out));
    EXPECT_EQ(out, old_e);
    EXPECT_TRUE(mt.get(lcdf::Str("g"), out));
    EXPECT_EQ(out, old_g);

    stage_and_scan(true);
    EXPECT_TRUE(mt.get(lcdf::Str("a"), out));
    EXPECT_EQ(out, new_a);
    EXPECT_TRUE(mt.get(lcdf::Str("b"), out));
    EXPECT_EQ(out, new_b);
    EXPECT_FALSE(mt.get(lcdf::Str("c"), out));
    EXPECT_TRUE(mt.get(lcdf::Str("d"), out));
    EXPECT_EQ(out, new_d);
    EXPECT_FALSE(mt.get(lcdf::Str("e"), out));
    EXPECT_TRUE(mt.get(lcdf::Str("g"), out));
    EXPECT_EQ(out, new_g);
}

TEST_F(SiloNonTxnApi, MassTransTransactionalScanCallbackCannotMutateStagedWrites) {
    mbta_type& mt = make_masstrans(9010, "mt_tx_scan_staged_lifetime");
    const std::string original = mako::Encode("original");
    ASSERT_TRUE(mt.put(lcdf::Str("update"), original));

    std::string staged_update = mako::Encode("staged-update");
    std::string staged_insert = mako::Encode("staged-insert");
    Sto::start_transaction();
    EXPECT_TRUE(mt.transPut(lcdf::Str("update"), StringWrapper(staged_update)));
    EXPECT_FALSE(mt.transInsert(lcdf::Str("insert"), StringWrapper(staged_insert)));

    auto strip_callback_copy = [&](lcdf::Str, std::string& value) {
        EXPECT_GE(value.size(), mako::EXTRA_BITS_FOR_VALUE);
        if (value.size() >= mako::EXTRA_BITS_FOR_VALUE)
            value.resize(value.size() - mako::EXTRA_BITS_FOR_VALUE);
        return true;
    };
    mt.transQuery(lcdf::Str("a"), lcdf::Str("z"), strip_callback_copy);
    mt.transRQuery(lcdf::Str("z"), lcdf::Str("a"), strip_callback_copy);

    // The callback's mutable value reference is scratch storage. Mutating it
    // must not mutate StringWrapper's caller-owned buffer or the staged value.
    std::string out;
    ASSERT_TRUE(mt.transGet(lcdf::Str("update"), out));
    EXPECT_EQ(out, staged_update);
    ASSERT_TRUE(mt.transGet(lcdf::Str("insert"), out));
    EXPECT_EQ(out, staged_insert);
    ASSERT_TRUE(Sto::try_commit_no_paxos());

    ASSERT_TRUE(mt.get(lcdf::Str("update"), out));
    EXPECT_EQ(out, staged_update);
    ASSERT_TRUE(mt.get(lcdf::Str("insert"), out));
    EXPECT_EQ(out, staged_insert);
}

TEST_F(SiloNonTxnApi, MassTransBoundedTransactionalScansAccountNewItems) {
    mbta_type& mt = make_masstrans(9011, "mt_tx_scan_budget");
    for (const char key : {'a', 'b', 'c', 'd', 'e'}) {
        const std::string value = mako::Encode(std::string("value-") + key);
        ASSERT_TRUE(mt.put(lcdf::Str(&key, 1), value));
    }

    // A complete first pass charges its leaf predicates and row reads. A
    // repeated pass over the same range creates no new TransItems, so a zero
    // remaining budget is sufficient.
    Sto::start_transaction();
    size_t remaining = 32;
    size_t visited = 0;
    EXPECT_TRUE(mt.transQueryBounded(
        lcdf::Str("a"), lcdf::Str("f"),
        [&](lcdf::Str, std::string&) {
            ++visited;
            return true;
        }, remaining));
    EXPECT_EQ(visited, 5u);
    EXPECT_LT(remaining, 32u);

    remaining = 0;
    visited = 0;
    EXPECT_TRUE(mt.transQueryBounded(
        lcdf::Str("a"), lcdf::Str("f"),
        [&](lcdf::Str, std::string&) {
            ++visited;
            return true;
        }, remaining));
    EXPECT_EQ(visited, 5u);
    EXPECT_EQ(remaining, 0u);
    Sto::silent_abort();

    // Callback-stop is successful and distinguishable from budget exhaustion.
    Sto::start_transaction();
    remaining = 32;
    visited = 0;
    EXPECT_TRUE(mt.transRQueryBounded(
        lcdf::Str("e"), lcdf::Str("a"),
        [&](lcdf::Str, std::string&) {
            ++visited;
            return false;
        }, remaining));
    EXPECT_EQ(visited, 1u);
    EXPECT_LT(remaining, 32u);
    Sto::silent_abort();

    Sto::start_transaction();
    remaining = 0;
    visited = 0;
    EXPECT_FALSE(mt.transRQueryBounded(
        lcdf::Str("e"), lcdf::Str("a"),
        [&](lcdf::Str, std::string&) {
            ++visited;
            return true;
        }, remaining));
    EXPECT_EQ(visited, 0u);
    EXPECT_EQ(remaining, 0u);
    Sto::silent_abort();
}

TEST_F(SiloNonTxnApi, MassTransBoundedScanExplicitBoundsSupportResume) {
    mbta_type& mt = make_masstrans(9012, "mt_tx_scan_resume");
    const std::string value = mako::Encode("value");
    for (const char key : {'a', 'b', 'c', 'd', 'e'})
        ASSERT_TRUE(mt.put(lcdf::Str(&key, 1), value));

    Sto::start_transaction();
    size_t remaining = 32;
    std::vector<std::string> keys;
    EXPECT_TRUE(mt.transQueryBounded(
        lcdf::Str("a"), lcdf::Str("f"),
        [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return keys.size() < 2;
        }, remaining, true /* include a */, false /* exclude f */));
    EXPECT_EQ(keys, (std::vector<std::string>{"a", "b"}));

    keys.clear();
    EXPECT_TRUE(mt.transQueryBounded(
        lcdf::Str("b"), lcdf::Str("f"),
        [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return keys.size() < 2;
        }, remaining, false /* resume after b */, false /* exclude f */));
    EXPECT_EQ(keys, (std::vector<std::string>{"c", "d"}));

    // Reverse uses the same logical [lower, upper) range as forward by making
    // the traversal's upper/start bound exclusive and lower/end bound
    // inclusive. Resuming makes the last-delivered start key exclusive.
    keys.clear();
    EXPECT_TRUE(mt.transRQueryBounded(
        lcdf::Str("f"), lcdf::Str("a"),
        [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return keys.size() < 2;
        }, remaining, false /* exclude f */, true /* include a */));
    EXPECT_EQ(keys, (std::vector<std::string>{"e", "d"}));

    keys.clear();
    EXPECT_TRUE(mt.transRQueryBounded(
        lcdf::Str("d"), lcdf::Str("a"),
        [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return true;
        }, remaining, false /* resume after d */, true /* include a */));
    EXPECT_EQ(keys, (std::vector<std::string>{"c", "b", "a"}));
    EXPECT_TRUE(Sto::try_commit_no_paxos());
}

TEST_F(SiloNonTxnApi, MassTransBoundedScanResumesAtArbitraryBinaryKeys) {
    mbta_type& mt = make_masstrans(9013, "mt_tx_scan_binary_resume");
    const std::string value = mako::Encode("value");
    const std::string empty;
    const std::string nul(1, '\0');
    const std::string nul_ff("\0\xff", 2);
    const std::string ascii = "a";
    // The local ABI's maximum admitted key size; resume does not synthesize a
    // longer successor even at this boundary.
    const std::string long_key(1024, 'z');
    const std::vector<std::string> ordered = {
        empty, nul, nul_ff, ascii, long_key,
    };
    for (const std::string& key : ordered)
        ASSERT_TRUE(mt.put(lcdf::Str(key), value));

    Sto::start_transaction();
    size_t remaining = 64;
    std::vector<std::string> keys;
    EXPECT_TRUE(mt.transQueryBounded(
        lcdf::Str(empty), lcdf::Str(),
        [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return true;
        }, remaining, false /* resume after the empty key */));
    EXPECT_EQ(keys, (std::vector<std::string>{nul, nul_ff, ascii, long_key}));

    keys.clear();
    EXPECT_TRUE(mt.transRQueryBounded(
        lcdf::Str(long_key), lcdf::Str(),
        [&](lcdf::Str key, std::string&) {
            keys.emplace_back(key.data(), key.length());
            return true;
        }, remaining, false /* resume after the exact long key */));
    EXPECT_EQ(keys, (std::vector<std::string>{ascii, nul_ff, nul, empty}));
    EXPECT_TRUE(Sto::try_commit_no_paxos());
}

TEST_F(SiloNonTxnApi, MassTransBoundedScanInclusivityAcrossMasstreeLeaves) {
    mbta_type& mt = make_masstrans(9014, "mt_tx_scan_multileaf_bounds");
    auto key_for = [](size_t i) {
        char key[48];
        snprintf(key, sizeof(key), "long-common-prefix-%04zu", i);
        return std::string(key);
    };
    const std::string value = mako::Encode("value");
    for (size_t i = 0; i < 120; ++i)
        ASSERT_TRUE(mt.put(lcdf::Str(key_for(i)), value));

    for (bool begin_inclusive : {false, true}) {
        for (bool end_inclusive : {false, true}) {
            SCOPED_TRACE(std::string(begin_inclusive ? "begin-inclusive "
                                                     : "begin-exclusive ") +
                         (end_inclusive ? "end-inclusive" : "end-exclusive"));
            const size_t first = begin_inclusive ? 20 : 21;
            const size_t last = end_inclusive ? 80 : 79;
            std::vector<std::string> expected;
            for (size_t i = first; i <= last; ++i)
                expected.push_back(key_for(i));

            Sto::start_transaction();
            size_t remaining = Transaction::tset_initial_capacity;
            std::vector<std::string> forward;
            EXPECT_TRUE(mt.transQueryBounded(
                lcdf::Str(key_for(20)), lcdf::Str(key_for(80)),
                [&](lcdf::Str key, std::string&) {
                    forward.emplace_back(key.data(), key.length());
                    return true;
                }, remaining, begin_inclusive, end_inclusive));
            EXPECT_EQ(forward, expected);

            std::vector<std::string> reverse;
            EXPECT_TRUE(mt.transRQueryBounded(
                lcdf::Str(key_for(80)), lcdf::Str(key_for(20)),
                [&](lcdf::Str key, std::string&) {
                    reverse.emplace_back(key.data(), key.length());
                    return true;
                }, remaining, end_inclusive, begin_inclusive));
            std::reverse(expected.begin(), expected.end());
            EXPECT_EQ(reverse, expected);
            EXPECT_TRUE(Sto::try_commit_no_paxos());
        }
    }
}

TEST_F(SiloNonTxnApi, MassTransBoundedScanStaysWithinEmbeddedSetAfterWrites) {
    mbta_type& mt = make_masstrans(9015, "mt_tx_scan_embedded_budget");
    constexpr size_t staged_count = 500;
    constexpr size_t total_count = 520;
    auto key_for = [](size_t i) {
        char key[32];
        snprintf(key, sizeof(key), "budget-%04zu", i);
        return std::string(key);
    };

    const std::string initial = mako::Encode("initial");
    for (size_t i = 0; i < total_count; ++i)
        ASSERT_TRUE(mt.put(lcdf::Str(key_for(i)), initial));

    std::vector<std::string> staged_values;
    staged_values.reserve(staged_count);
    Sto::start_transaction();
    for (size_t i = 0; i < staged_count; ++i) {
        staged_values.push_back(mako::Encode("updated-" + std::to_string(i)));
        EXPECT_TRUE(mt.transPut(lcdf::Str(key_for(i)),
                                StringWrapper(staged_values.back())));
    }

    // The writes occupy 500 of Transaction's 512 embedded TransItems. Even
    // though most scanned rows already have items, new leaf predicates and the
    // final unstaged rows may consume only the twelve credits left.
    size_t remaining = Transaction::tset_initial_capacity - staged_count;
    size_t visited = 0;
    EXPECT_FALSE(mt.transQueryBounded(
        lcdf::Str("budget-"), lcdf::Str("budget."),
        [&](lcdf::Str, std::string&) {
            ++visited;
            return true;
        }, remaining));
    EXPECT_GT(visited, 0u);
    EXPECT_EQ(remaining, 0u);
    Sto::silent_abort();
}

TEST_F(SiloNonTxnApi, MassTransScanPredicateTracksOwnAndConcurrentPhantoms) {
    mbta_type& mt = make_masstrans(9016, "mt_tx_scan_predicates");
    const std::string value = mako::Encode("value");
    ASSERT_TRUE(mt.put(lcdf::Str("own-0"), value));
    ASSERT_TRUE(mt.put(lcdf::Str("own-9"), value));

    // A scan followed by this transaction's own structural insertion updates
    // the observed leaf version, and the repeated scan sees the inserted row.
    std::string own_value = mako::Encode("own-value");
    Sto::start_transaction();
    size_t visited = 0;
    mt.transQuery(lcdf::Str("own-1"), lcdf::Str("own-8"),
                  [&](lcdf::Str, std::string&) {
        ++visited;
        return true;
    });
    EXPECT_EQ(visited, 0u);
    EXPECT_FALSE(mt.transInsert(lcdf::Str("own-5"), StringWrapper(own_value)));
    mt.transQuery(lcdf::Str("own-1"), lcdf::Str("own-8"),
                  [&](lcdf::Str key, std::string& scanned) {
        ++visited;
        EXPECT_EQ(std::string(key.data(), key.length()), "own-5");
        EXPECT_EQ(scanned, own_value);
        return true;
    });
    EXPECT_EQ(visited, 1u);
    EXPECT_TRUE(Sto::try_commit_no_paxos());

    auto expect_concurrent_phantom_abort = [&](const std::string& prefix,
                                                bool reverse) {
        const std::string low = prefix + "1";
        const std::string high = prefix + "8";
        const std::string phantom = prefix + "5";
        ASSERT_TRUE(mt.put(lcdf::Str(prefix + "0"), value));
        ASSERT_TRUE(mt.put(lcdf::Str(prefix + "9"), value));

        Sto::start_transaction();
        size_t reader_rows = 0;
        auto callback = [&](lcdf::Str, std::string&) {
            ++reader_rows;
            return true;
        };
        if (reverse)
            mt.transRQuery(lcdf::Str(high), lcdf::Str(low), callback);
        else
            mt.transQuery(lcdf::Str(low), lcdf::Str(high), callback);
        EXPECT_EQ(reader_rows, 0u);

        std::atomic<bool> writer_inserted{false};
        std::thread writer([&] {
            silo_thread_init();
            writer_inserted.store(mt.put(lcdf::Str(phantom), value));
        });
        writer.join();
        EXPECT_TRUE(writer_inserted.load());
        EXPECT_FALSE(Sto::try_commit_no_paxos());
    };

    expect_concurrent_phantom_abort("forward-", false);
    expect_concurrent_phantom_abort("reverse-", true);
}

TEST_F(SiloNonTxnApi, MassTransInsertThenRepeatedGrowingUpdatesCommitAndAbort) {
    mbta_type& mt = make_masstrans(9017, "mt_repeat_growth");
    const std::string initial = mako::Encode("i");
    const std::string medium = mako::Encode(std::string(4096, 'm'));
    const std::string large = mako::Encode(std::string(32768, 'l'));

    auto stage = [&](const char* key, bool commit) {
        SCOPED_TRACE(key);
        Sto::start_transaction();
        try {
            // Raw MassTrans returns `existed`, so false means insertion.
            EXPECT_FALSE(mt.transInsert(lcdf::Str(key), StringWrapper(initial)));
            std::string out;
            EXPECT_TRUE(mt.transGet(lcdf::Str(key), out));
            EXPECT_EQ(out, initial);
            EXPECT_TRUE(mt.transPut(lcdf::Str(key), StringWrapper(medium)));
            EXPECT_TRUE(mt.transGet(lcdf::Str(key), out));
            EXPECT_EQ(out, medium);
            EXPECT_TRUE(mt.transPut(lcdf::Str(key), StringWrapper(large)));
            EXPECT_TRUE(mt.transGet(lcdf::Str(key), out));
            EXPECT_EQ(out, large);
            if (commit)
                EXPECT_TRUE(Sto::try_commit_no_paxos());
            else
                Sto::silent_abort();
        } catch (...) {
            Sto::silent_abort();
            throw;
        }
    };

    stage("repeat-grow-commit", true);
    std::string out;
    ASSERT_TRUE(mt.get(lcdf::Str("repeat-grow-commit"), out));
    EXPECT_EQ(out, large);

    stage("repeat-grow-abort", false);
    EXPECT_FALSE(mt.get(lcdf::Str("repeat-grow-abort"), out));
    EXPECT_TRUE(mt.put(lcdf::Str("repeat-grow-abort"), initial));
}

TEST_F(SiloNonTxnApi, MassTransSameKeyCompositionExhaustiveTriples) {
    mbta_type& mt = make_masstrans(9018, "mt_same_key_matrix");
    const std::string initial = mako::Encode("initial");
    const std::string small = mako::Encode("small");
    const std::string large = mako::Encode(std::string(32768, 'L'));

    enum class Operation {
        Get,
        PutSmall,
        PutLarge,
        InsertSmall,
        InsertLarge,
        Delete,
    };
    constexpr Operation operations[] = {
        Operation::Get,
        Operation::PutSmall,
        Operation::PutLarge,
        Operation::InsertSmall,
        Operation::InsertLarge,
        Operation::Delete,
    };

    auto operation_name = [](Operation operation) {
        switch (operation) {
        case Operation::Get:
            return "get";
        case Operation::PutSmall:
            return "put-small";
        case Operation::PutLarge:
            return "put-large";
        case Operation::InsertSmall:
            return "insert-small";
        case Operation::InsertLarge:
            return "insert-large";
        case Operation::Delete:
            return "delete";
        }
        return "unknown";
    };

    size_t sequence_id = 0;
    for (bool initially_present : {false, true}) {
        for (bool commit : {false, true}) {
            for (Operation first : operations) {
                for (Operation second : operations) {
                    for (Operation third : operations) {
                        const std::string key =
                            "composition-" + std::to_string(sequence_id++);
                        SCOPED_TRACE(
                            std::string(initially_present ? "existing: " : "absent: ") +
                            operation_name(first) + " -> " +
                            operation_name(second) + " -> " +
                            operation_name(third) +
                            (commit ? " (commit)" : " (abort)"));

                        if (initially_present)
                            ASSERT_TRUE(mt.put(lcdf::Str(key), initial));

                        bool present = initially_present;
                        std::string expected = initial;
                        bool transaction_completed = false;
                        Sto::start_transaction();
                        try {
                            for (Operation operation : {first, second, third}) {
                                std::string out;
                                switch (operation) {
                                case Operation::Get:
                                    EXPECT_EQ(mt.transGet(lcdf::Str(key), out), present);
                                    if (present)
                                        EXPECT_EQ(out, expected);
                                    break;
                                case Operation::PutSmall:
                                case Operation::PutLarge: {
                                    const std::string& value =
                                        operation == Operation::PutSmall ? small : large;
                                    EXPECT_EQ(
                                        mt.transPut(lcdf::Str(key), StringWrapper(value)),
                                        present);
                                    present = true;
                                    expected = value;
                                    break;
                                }
                                case Operation::InsertSmall:
                                case Operation::InsertLarge: {
                                    const std::string& value =
                                        operation == Operation::InsertSmall ? small : large;
                                    EXPECT_EQ(
                                        mt.transInsert(lcdf::Str(key), StringWrapper(value)),
                                        present);
                                    if (!present) {
                                        present = true;
                                        expected = value;
                                    }
                                    break;
                                }
                                case Operation::Delete:
                                    EXPECT_EQ(mt.transDelete(lcdf::Str(key)), present);
                                    present = false;
                                    expected.clear();
                                    break;
                                }

                                // Every operation is followed by a point read so the
                                // matrix checks both operation composition and RYW.
                                out.clear();
                                EXPECT_EQ(mt.transGet(lcdf::Str(key), out), present);
                                if (present)
                                    EXPECT_EQ(out, expected);
                            }

                            if (commit) {
                                transaction_completed = Sto::try_commit_no_paxos();
                            } else {
                                Sto::silent_abort();
                                transaction_completed = true;
                            }
                        } catch (const Transaction::Abort&) {
                            Sto::silent_abort();
                        } catch (...) {
                            Sto::silent_abort();
                            throw;
                        }
                        ASSERT_TRUE(transaction_completed);

                        const bool final_present = commit ? present : initially_present;
                        const std::string& final_value =
                            commit ? expected : initial;
                        std::string out;
                        EXPECT_EQ(mt.get(lcdf::Str(key), out), final_present);
                        if (final_present)
                            EXPECT_EQ(out, final_value);
                    }
                }
            }
        }
    }
    EXPECT_EQ(sequence_id, 864u);
}

TEST_F(SiloNonTxnApi, MassTransRywCompositionIsLocalSingleVersionOnly) {
    mbta_type& mt = make_masstrans(9019, "mt_ryw_scope");
    const std::string initial = mako::Encode("i");
    const std::string large = mako::Encode(std::string(4096, 'l'));
    const std::string scan_initial = mako::Encode("scan-old");
    const std::string scan_update = mako::Encode("scan-new");
    ASSERT_TRUE(mt.put(lcdf::Str("scan-update"), scan_initial));
    ASSERT_TRUE(mt.put(lcdf::Str("scan-delete"), scan_initial));

    struct RestoreLocalSingleVersion {
        mbta_type& table;
        ~RestoreLocalSingleVersion() {
            Sto::silent_abort();
            table.set_is_remote(false);
            TThread::disable_multiversion();
        }
    } restore{mt};

    auto expect_growing_own_insert_to_abort = [&](const char* key) {
        Sto::start_transaction();
        EXPECT_FALSE(mt.transInsert(lcdf::Str(key), StringWrapper(initial)));
        bool aborted = false;
        try {
            (void)mt.transPut(lcdf::Str(key), StringWrapper(large));
        } catch (const Transaction::Abort&) {
            aborted = true;
        }
        EXPECT_TRUE(aborted);
        Sto::silent_abort();
    };

    auto expect_own_insert_read_to_abort = [&](const char* key) {
        Sto::start_transaction();
        EXPECT_FALSE(mt.transInsert(lcdf::Str(key), StringWrapper(initial)));
        std::string out;
        TThread::transget_without_throw = false;
        EXPECT_FALSE(mt.transGet(lcdf::Str(key), out));
        EXPECT_TRUE(TThread::transget_without_throw);
        TThread::transget_without_throw = false;
        Sto::silent_abort();
    };

    auto expect_scan_without_overlay = [&] {
        Sto::start_transaction();
        EXPECT_TRUE(mt.transPut(lcdf::Str("scan-update"),
                                StringWrapper(scan_update)));
        EXPECT_TRUE(mt.transDelete(lcdf::Str("scan-delete")));
        std::vector<std::pair<std::string, std::string>> rows;
        mt.transQuery(lcdf::Str("scan-"), lcdf::Str("scan."),
                      [&](lcdf::Str key, std::string& value) {
            rows.emplace_back(std::string(key.data(), key.length()), value);
            return true;
        });
        EXPECT_EQ(rows,
                  (std::vector<std::pair<std::string, std::string>>{
                      {"scan-delete", scan_initial},
                      {"scan-update", scan_initial},
                  }));
        Sto::silent_abort();
    };

    TThread::enable_multiverison();
    expect_growing_own_insert_to_abort("ryw-disabled-multiversion");
    expect_own_insert_read_to_abort("ryw-read-disabled-multiversion");
    expect_scan_without_overlay();

    TThread::disable_multiversion();
    mt.set_is_remote(true);
    expect_growing_own_insert_to_abort("ryw-disabled-remote");
    expect_own_insert_read_to_abort("ryw-read-disabled-remote");
    expect_scan_without_overlay();

    mt.set_is_remote(false);
    Sto::start_transaction();
    EXPECT_TRUE(mt.transPut(lcdf::Str("scan-update"),
                            StringWrapper(scan_update)));
    EXPECT_TRUE(mt.transDelete(lcdf::Str("scan-delete")));
    std::vector<std::pair<std::string, std::string>> rows;
    mt.transQuery(lcdf::Str("scan-"), lcdf::Str("scan."),
                  [&](lcdf::Str key, std::string& value) {
        rows.emplace_back(std::string(key.data(), key.length()), value);
        return true;
    });
    EXPECT_EQ(rows,
              (std::vector<std::pair<std::string, std::string>>{
                  {"scan-update", scan_update},
              }));
    Sto::silent_abort();

    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("ryw-disabled-multiversion"), out));
    EXPECT_FALSE(mt.get(lcdf::Str("ryw-read-disabled-multiversion"), out));
    EXPECT_FALSE(mt.get(lcdf::Str("ryw-disabled-remote"), out));
    EXPECT_FALSE(mt.get(lcdf::Str("ryw-read-disabled-remote"), out));
}
#endif

// ===========================================================================
// 2. L3 level — through abstract_ordered_index* (virtual dispatch)
// ===========================================================================

TEST_F(SiloNonTxnApi, L3PutGetRoundTripThroughBasePointer) {
    abstract_ordered_index* tbl = make_table("l3_roundtrip");

    // Raw bytes in, raw bytes out: the L3 non-txn ops own the
    // Encode/strip boundary internally.
    EXPECT_TRUE(tbl->put(lcdf::Str("k"), "l3-value"));

    std::string out;
    EXPECT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "l3-value");

    EXPECT_FALSE(tbl->get(lcdf::Str("missing"), out, std::string::npos));
}

TEST_F(SiloNonTxnApi, L3PutOverwrites) {
    abstract_ordered_index* tbl = make_table("l3_overwrite");

    EXPECT_TRUE(tbl->put(lcdf::Str("k"), "one"));
    EXPECT_FALSE(tbl->put(lcdf::Str("k"), "two"));  // existed

    std::string out;
    ASSERT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "two");
}

TEST_F(SiloNonTxnApi, L3InsertIsExclusive) {
    abstract_ordered_index* tbl = make_table("l3_insert");

    EXPECT_TRUE(tbl->insert(lcdf::Str("k"), "one"));
    EXPECT_FALSE(tbl->insert(lcdf::Str("k"), "two"));

    std::string out;
    ASSERT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "one");
}

TEST_F(SiloNonTxnApi, L3RemoveSemantics) {
    abstract_ordered_index* tbl = make_table("l3_remove");

    ASSERT_TRUE(tbl->put(lcdf::Str("k"), "v"));
    EXPECT_TRUE(tbl->remove(lcdf::Str("k")));
    std::string out;
    EXPECT_FALSE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_FALSE(tbl->remove(lcdf::Str("k")));  // second remove: absent
}

TEST_F(SiloNonTxnApi, L3ScanOrderAndEarlyStop) {
    abstract_ordered_index* tbl = make_table("l3_scan");

    for (int i = 0; i < 8; i++) {
        std::string k = "s" + std::to_string(i);
        ASSERT_TRUE(tbl->put(lcdf::Str(k), "val" + std::to_string(i)));
    }

    // Full forward scan: sorted keys, stripped values.
    {
        CollectCallback cb;
        std::string start = "s0";
        std::string end = "s9";
        tbl->scan(start, &end, cb, nullptr);
        ASSERT_EQ(cb.pairs.size(), 8u);
        for (int i = 0; i < 8; i++) {
            EXPECT_EQ(cb.pairs[i].first, "s" + std::to_string(i));
            EXPECT_EQ(cb.pairs[i].second, "val" + std::to_string(i));
        }
    }

    // Early stop after 3.
    {
        CollectCallback cb(/*limit=*/3);
        std::string start = "s0";
        tbl->scan(start, nullptr, cb, nullptr);
        EXPECT_EQ(cb.pairs.size(), 3u);
    }

    // Reverse scan: descending order.
    {
        CollectCallback cb;
        std::string start = "s9";
        std::string end = "s0";
        tbl->rscan(start, &end, cb, nullptr);
        ASSERT_GE(cb.pairs.size(), 1u);
        for (size_t i = 1; i < cb.pairs.size(); i++) {
            EXPECT_GT(cb.pairs[i - 1].first, cb.pairs[i].first);
        }
    }
}

// ===========================================================================
// 3. Sharded level
// ===========================================================================

TEST_F(SiloNonTxnApi, ShardedRoutesNonTxnOps) {
    std::vector<abstract_ordered_index*> shards;
    shards.push_back(make_table("sharded_0"));
    mbta_sharded_ordered_index sharded("sharded", shards);

    EXPECT_TRUE(sharded.put(lcdf::Str("a"), "va"));
    EXPECT_TRUE(sharded.insert(lcdf::Str("b"), "vb"));
    EXPECT_FALSE(sharded.insert(lcdf::Str("b"), "vb2"));

    std::string out;
    EXPECT_TRUE(sharded.get(lcdf::Str("a"), out, std::string::npos));
    EXPECT_EQ(out, "va");
    EXPECT_TRUE(sharded.get(lcdf::Str("b"), out, std::string::npos));
    EXPECT_EQ(out, "vb");

    CollectCallback cb;
    std::string start = "a";
    sharded.scan(start, nullptr, cb, nullptr);
    ASSERT_EQ(cb.pairs.size(), 2u);
    EXPECT_EQ(cb.pairs[0].first, "a");
    EXPECT_EQ(cb.pairs[1].first, "b");

    EXPECT_TRUE(sharded.remove(lcdf::Str("a")));
    EXPECT_FALSE(sharded.get(lcdf::Str("a"), out, std::string::npos));
}

// ===========================================================================
// 4. Interleaving with transactions
// ===========================================================================

// A transactional write staged on another thread must be invisible to
// non-txn reads until that transaction commits, and visible after.
TEST_F(SiloNonTxnApi, NonTxnGetDoesNotSeeUncommittedTxnWrite) {
    abstract_ordered_index* tbl = make_table("interleave");

    // Non-txn put takes raw bytes; the txn'd put below keeps the
    // caller-Encodes convention (it stores a pointer until commit).
    ASSERT_TRUE(tbl->put(lcdf::Str("k"), "committed"));

    std::atomic<int> stage{0};  // 0=init, 1=staged, 2=main-checked, 3=committed
    const std::string staged_val = mako::Encode("staged");

    std::thread writer([&] {
        silo_thread_init();
        Sto::start_transaction();
        auto* mbta_tbl = static_cast<mbta_ordered_index*>(tbl);
        // Stage a write in the open transaction via the txn'd path.
        tx_put(mbta_tbl, /*txn=*/nullptr, lcdf::Str("k"), staged_val);
        stage.store(1);
        while (stage.load() != 2) std::this_thread::yield();
        Sto::commit();
        stage.store(3);
    });

    while (stage.load() != 1) std::this_thread::yield();

    // Uncommitted write must be invisible.
    std::string out;
    ASSERT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "committed");

    stage.store(2);
    while (stage.load() != 3) std::this_thread::yield();
    writer.join();

    // Committed write now visible.
    ASSERT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "staged");
}

// Concurrent non-txn writers on distinct keys + readers: all writes
// round-trip; the internal retry loop absorbs OCC conflicts.
TEST_F(SiloNonTxnApi, ConcurrentNonTxnOpsAllSucceed) {
    abstract_ordered_index* tbl = make_table("concurrent");

    constexpr int kThreads = 4;
    constexpr int kKeysPerThread = 200;
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};

    for (int t = 0; t < kThreads; t++) {
        workers.emplace_back([&, t] {
            silo_thread_init();
            for (int i = 0; i < kKeysPerThread; i++) {
                std::string k = "t" + std::to_string(t) + "_" + std::to_string(i);
                std::string v = "v" + std::to_string(t * 1000 + i);
                if (!tbl->put(lcdf::Str(k), v)) {
                    // put returns false only if the key existed — keys are
                    // distinct per thread, so this would be a bug.
                    failures.fetch_add(1);
                }
            }
            for (int i = 0; i < kKeysPerThread; i++) {
                std::string k = "t" + std::to_string(t) + "_" + std::to_string(i);
                std::string out;
                if (!tbl->get(lcdf::Str(k), out, std::string::npos) ||
                    out != "v" + std::to_string(t * 1000 + i)) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

// ===========================================================================
// 5. Unimplemented surface is a COMPILE-TIME fact now
// ===========================================================================
// The old bridge supplied aborting defaults for the non-txn ops, and
// this section death-tested them. After the de-overloading campaign
// the interface is exactly the three traits: a backend that does not
// implement the non-txn surface simply does not implement
// OrderedIndex, and instantiating it fails to compile — there is
// nothing left to abort at runtime. (Legacy ht_*/ndb/kvdb backends
// carry explicit aborting stubs; see storage/mbta_wrapper.hh.)

}  // namespace
