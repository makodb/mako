// Public native adapter contracts. Transaction-manager algorithms are tested
// independently by sto-core and sto-test-datatypes without Masstree.
#include "benchmarks/bench.h"
#include "storage/rust_sto_tpcc_wrapper.hh"
#include "storage/sharded_ordered_index.hh"
#include <gtest/gtest.h>
import std;

namespace {
rust_sto_tpcc_wrapper& database() {
    static auto* db = [] {
        auto& config = BenchmarkConfig::getInstance();
        config.setConfig(nullptr);
        config.setNthreads(1);
        config.setNshards(1);
        config.setShardIndex(0);
        config.setIsReplicated(0);
        config.setPaxosProcName(mako::LOCALHOST_CENTER);
        auto* result = new rust_sto_tpcc_wrapper;
        result->init();
        return result;
    }();
    return *db;
}
void rust_thread_init() { database().thread_init(true); }
class RustNonTxnApi : public ::testing::Test {
protected:
    void SetUp() override { rust_thread_init(); }
    void TearDown() override { database().thread_end(); }
    abstract_ordered_index* make_table(const std::string& name) {
        return database().open_index(name);
    }
};
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

TEST_F(RustNonTxnApi, L3PutGetRoundTripThroughBasePointer) {
    abstract_ordered_index* tbl = make_table("l3_roundtrip");

    // Raw bytes in, raw bytes out: the L3 non-txn ops own the
    // Encode/strip boundary internally.
    EXPECT_TRUE(tbl->put(lcdf::Str("k"), "l3-value"));

    std::string out;
    EXPECT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "l3-value");

    EXPECT_FALSE(tbl->get(lcdf::Str("missing"), out, std::string::npos));
}

TEST_F(RustNonTxnApi, L3PutOverwrites) {
    abstract_ordered_index* tbl = make_table("l3_overwrite");

    EXPECT_TRUE(tbl->put(lcdf::Str("k"), "one"));
    EXPECT_FALSE(tbl->put(lcdf::Str("k"), "two"));  // existed

    std::string out;
    ASSERT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "two");
}

TEST_F(RustNonTxnApi, L3InsertIsExclusive) {
    abstract_ordered_index* tbl = make_table("l3_insert");

    EXPECT_TRUE(tbl->insert(lcdf::Str("k"), "one"));
    EXPECT_FALSE(tbl->insert(lcdf::Str("k"), "two"));

    std::string out;
    ASSERT_TRUE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_EQ(out, "one");
}

TEST_F(RustNonTxnApi, L3RemoveSemantics) {
    abstract_ordered_index* tbl = make_table("l3_remove");

    ASSERT_TRUE(tbl->put(lcdf::Str("k"), "v"));
    EXPECT_TRUE(tbl->remove(lcdf::Str("k")));
    std::string out;
    EXPECT_FALSE(tbl->get(lcdf::Str("k"), out, std::string::npos));
    EXPECT_FALSE(tbl->remove(lcdf::Str("k")));  // second remove: absent
}

TEST_F(RustNonTxnApi, L3ScanOrderAndEarlyStop) {
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

// @unsafe - callbacks cross the engine boundary; exceptions must close the read txn.
TEST_F(RustNonTxnApi, ScanCallbackExceptionReleasesTransaction) {
    struct ScanFailure {};
    struct ThrowingCallback : oi_scan_callback {
        // @unsafe - deliberately exercises the engine's callback exception boundary.
        bool invoke(const char*, size_t, const std::string&) override {
            throw ScanFailure{};
        }
    } callback;
    auto* table = make_table("throwing_scan");
    ASSERT_TRUE(table->put(lcdf::Str("key"), "before"));
    const std::string lower = "a", upper = "z";
    EXPECT_THROW(table->scan(lower, &upper, callback, nullptr), ScanFailure);
    ASSERT_FALSE(rust_sto_tpcc_wrapper::current_transaction_active());
    EXPECT_THROW(table->rscan(upper, &lower, callback, nullptr), ScanFailure);
    ASSERT_FALSE(rust_sto_tpcc_wrapper::current_transaction_active());
    EXPECT_FALSE(table->put(lcdf::Str("key"), "after"));
    std::string value;
    ASSERT_TRUE(table->get(lcdf::Str("key"), value, std::string::npos));
    EXPECT_EQ(value, "after");
}

// @unsafe - exercises exact binary bounds through the legacy engine interface.
TEST_F(RustNonTxnApi, L3ScanExactBinaryBounds) {
    abstract_ordered_index* table = make_table("binary_scan_bounds");
    const std::array<std::string, 7> keys{
        "", std::string("\0", 1), std::string("\0\xff", 2), "a",
        std::string("a\0", 2), std::string("a\xff", 2), std::string("\xff", 1)};
    for (size_t i = 0; i < keys.size(); ++i)
        ASSERT_TRUE(table->put(lcdf::Str(keys[i]), "value" + std::to_string(i)));

    // @unsafe - callbacks must observe the exact half-open or reversed interval.
    auto check = [&](bool reverse, const std::string& start, const std::string* end,
                     std::initializer_list<size_t> expected) {
        CollectCallback callback;
        if (reverse) table->rscan(start, end, callback, nullptr);
        else table->scan(start, end, callback, nullptr);
        ASSERT_EQ(callback.pairs.size(), expected.size());
        size_t position = 0;
        for (const auto i : expected) {
            EXPECT_EQ(callback.pairs[position].first, keys[i]);
            EXPECT_EQ(callback.pairs[position].second, "value" + std::to_string(i));
            ++position;
        }
    };
    check(false, keys[1], &keys[5], {1, 2, 3, 4});
    check(true, keys[5], &keys[1], {5, 4, 3, 2});
    check(false, keys[0], nullptr, {0, 1, 2, 3, 4, 5, 6});
    check(false, keys[0], &keys[0], {});
    check(true, keys[0], nullptr, {0});
    check(true, keys[6], &keys[0], {6, 5, 4, 3, 2, 1});
    check(true, keys[6], nullptr, {6, 5, 4, 3, 2, 1, 0});
    check(false, keys[3], &keys[3], {});
    check(true, keys[3], &keys[3], {});
    check(false, keys[5], &keys[1], {});
    check(true, keys[1], &keys[5], {});
}

// @unsafe - retained callback borrows must belong to the caller's live arena.
TEST_F(RustNonTxnApi, L3ScanArenaValuesSurviveLaterDeliveries) {
    abstract_ordered_index* table = make_table("arena_scan_values");
    const std::array<std::string, 3> values{
        std::string(300, 'a'), std::string(450, 'b'), std::string(600, 'c')};
    for (size_t i = 0; i < values.size(); ++i)
        ASSERT_TRUE(table->put(lcdf::Str("k" + std::to_string(i)), values[i]));
    str_arena arena;
    struct RetainValues : oi_scan_callback {
        std::array<const std::string*, 6> values{};
        size_t count = 0;
        // @unsafe - borrow arena-owned strings, never callback stack temporaries.
        bool invoke(const char* key, size_t length, const std::string& value) override {
            if (count == values.size()) {
                ADD_FAILURE() << "scan delivered an unexpected extra row";
                return false;
            }
            const size_t index = count < 3 ? count : 5 - count;
            EXPECT_EQ(std::string(key, length), "k" + std::to_string(index));
            values[count++] = &value;
            return true;
        }
    } callback;
    table->scan("k0", nullptr, callback, &arena);
    ASSERT_EQ(callback.count, 3u);
    table->rscan("k2", nullptr, callback, &arena);
    ASSERT_EQ(callback.count, 6u);
    *arena() = "subsequent allocation";
    for (size_t i = 0; i < callback.values.size(); ++i) {
        ASSERT_TRUE(arena.manages(callback.values[i]));
        EXPECT_EQ(*callback.values[i], values[i < 3 ? i : 5 - i]);
    }
}

// ===========================================================================
// 3. Sharded level
// ===========================================================================

TEST_F(RustNonTxnApi, ShardedRoutesNonTxnOps) {
    std::vector<abstract_ordered_index*> shards;
    shards.push_back(make_table("sharded_0"));
    sharded_ordered_index sharded("sharded", shards);

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
TEST_F(RustNonTxnApi, NonTxnGetDoesNotSeeUncommittedTxnWrite) {
    abstract_ordered_index* tbl = make_table("interleave");

    // Non-txn put takes raw bytes; the txn'd put below keeps the
    // caller-Encodes convention (it stores a pointer until commit).
    ASSERT_TRUE(tbl->put(lcdf::Str("k"), "committed"));

    std::atomic<int> stage{0};  // 0=init, 1=staged, 2=main-checked, 3=committed
    const std::string staged_val = mako::Encode("staged");

    std::thread writer([&] {
        rust_thread_init();
        str_arena arena;
        auto* txn = database().new_txn(0, arena, nullptr);
        // Stage a write in the open transaction via the txn'd path.
        tx_put(tbl, txn, lcdf::Str("k"), staged_val);
        stage.store(1);
        while (stage.load() != 2) std::this_thread::yield();
        EXPECT_TRUE(database().commit_txn(txn));
        database().thread_end();
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
TEST_F(RustNonTxnApi, ConcurrentNonTxnOpsAllSucceed) {
    abstract_ordered_index* tbl = make_table("concurrent");

    constexpr int kThreads = 4;
    constexpr int kKeysPerThread = 200;
    std::vector<std::thread> workers;
    std::atomic<int> failures{0};

    for (int t = 0; t < kThreads; t++) {
        workers.emplace_back([&, t] {
            rust_thread_init();
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
            database().thread_end();
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}


} // namespace
