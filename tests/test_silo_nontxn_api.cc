// Gating tests for docs/storage-interface.md — the
// non-transactional (Masstree-shape) API added to Silo's layers:
//
//   1. MassTrans level:   insert / erase / scan / rscan (Phase 1) plus the
//                          pre-existing put / get.
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

class ScopedMultiversionMode {
public:
    ScopedMultiversionMode() : was_enabled_(TThread::is_multiversion()) {
        TThread::enable_multiverison();
    }
    ~ScopedMultiversionMode() {
        if (was_enabled_)
            TThread::enable_multiverison();
        else
            TThread::disable_multiversion();
    }
private:
    bool was_enabled_;
};

// Per-thread Silo/STO initialization. Every thread that touches
// MassTrans (directly or via the wrappers) must call this once.
void silo_thread_init() {
    TThread::set_id(g_tid_counter.fetch_add(1));
    TThread::set_mode(0);
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

TEST_F(SiloNonTxnApi, MassTransErasePresentAndAbsent) {
    mbta_type& mt = make_masstrans(9003, "mt_remove");

    const std::string val = mako::Encode("gone-soon");
    ASSERT_TRUE(mt.put(lcdf::Str("victim"), val));

    EXPECT_TRUE(mt.erase(lcdf::Str("victim")));
    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("victim"), out));

    // An absent-key erase returns false and must not stage a decrement.
    EXPECT_FALSE(mt.erase(lcdf::Str("never-existed")));
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

TEST_F(SiloNonTxnApi, MassTransScanExceptionAbortsOwnedTransaction) {
    mbta_type& mt = make_masstrans(9005, "mt_scan_exception");
    ASSERT_TRUE(mt.put(lcdf::Str("a"), mako::Encode("value")));

    EXPECT_THROW(
        mt.scan(lcdf::Str("a"), lcdf::Str("z"),
                [](lcdf::Str, std::string&) -> bool {
                    throw std::runtime_error("callback failure");
                }),
        std::runtime_error);
    ASSERT_NE(TThread::txn, nullptr);
    EXPECT_FALSE(TThread::txn->has_active_state());

    // A throwing callback must not poison the ambient worker transaction.
    EXPECT_TRUE(mt.put(lcdf::Str("b"), mako::Encode("after")));
    std::string out;
    EXPECT_TRUE(mt.get(lcdf::Str("b"), out));
    EXPECT_EQ(out.substr(0, 5), "after");
}

TEST_F(SiloNonTxnApi, MassTransDeleteThenPutResurrectsInMvMode) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9006, "mt_mv_put_resurrection");

    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("old")));
    EXPECT_EQ(mt.approx_size(), 1U);
    ASSERT_TRUE(mt.erase(lcdf::Str("key")));
    EXPECT_EQ(mt.approx_size(), 0U);

    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("key"), out));
    EXPECT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("new")));
    EXPECT_EQ(mt.approx_size(), 1U);
    ASSERT_TRUE(mt.get(lcdf::Str("key"), out));
    EXPECT_EQ(out.substr(0, 3), "new");

    EXPECT_TRUE(mt.erase(lcdf::Str("key")));
    EXPECT_EQ(mt.approx_size(), 0U);
    EXPECT_FALSE(mt.get(lcdf::Str("key"), out));
    EXPECT_FALSE(mt.erase(lcdf::Str("key")));
    EXPECT_EQ(mt.approx_size(), 0U);
}

TEST_F(SiloNonTxnApi, MassTransDeleteThenInsertResurrectsInMvMode) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9007, "mt_mv_insert_resurrection");

    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("old")));
    ASSERT_TRUE(mt.erase(lcdf::Str("key")));
    EXPECT_TRUE(mt.insert(lcdf::Str("key"), mako::Encode("first")));
    EXPECT_EQ(mt.approx_size(), 1U);
    EXPECT_FALSE(mt.insert(lcdf::Str("key"), mako::Encode("second")));

    std::string out;
    ASSERT_TRUE(mt.get(lcdf::Str("key"), out));
    EXPECT_EQ(out.substr(0, 5), "first");
}

TEST_F(SiloNonTxnApi, AbortedMvResurrectionPreservesCommittedTombstone) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9008, "mt_mv_abort_resurrection");

    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("old")));
    ASSERT_TRUE(mt.erase(lcdf::Str("key")));
    ASSERT_EQ(mt.approx_size(), 0U);

    const std::string staged = mako::Encode("aborted");
    Sto::start_transaction();
    EXPECT_FALSE(mt.transPut(lcdf::Str("key"), staged));
    Sto::silent_abort();
    EXPECT_FALSE(TThread::txn->has_active_state());
    EXPECT_EQ(mt.approx_size(), 0U);

    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("key"), out));
    EXPECT_TRUE(mt.insert(lcdf::Str("key"), mako::Encode("after")));
    ASSERT_TRUE(mt.get(lcdf::Str("key"), out));
    EXPECT_EQ(out.substr(0, 5), "after");
}

TEST_F(SiloNonTxnApi, AbortedMvDeletePreservesValueAndCount) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9011, "mt_mv_abort_delete");
    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("present")));
    ASSERT_EQ(mt.approx_size(), 1U);

    Sto::start_transaction();
    EXPECT_TRUE(mt.transDelete(lcdf::Str("key")));
    Sto::silent_abort();
    EXPECT_FALSE(TThread::txn->has_active_state());
    EXPECT_EQ(mt.approx_size(), 1U);

    std::string out;
    ASSERT_TRUE(mt.get(lcdf::Str("key"), out));
    EXPECT_EQ(out.substr(0, 7), "present");
}

TEST_F(SiloNonTxnApi, ConcurrentMvDeleteAndResurrectionHaveSerialOutcome) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9012, "mt_mv_delete_resurrection_race");
    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("old")));
    ASSERT_TRUE(mt.erase(lcdf::Str("key")));
    ASSERT_EQ(mt.approx_size(), 0U);

    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> resurrected{false};
    std::atomic<bool> deleted{false};
    std::atomic<unsigned> failures{0};

    // MassTrans exposes a single OCC attempt. The storage facade above it
    // supplies the retry loop; mirror that contract here so a normal lock or
    // validation conflict does not masquerade as a resurrection failure.
    auto retry_abort = [](auto&& operation) {
        while (true) {
            try {
                return operation();
            } catch (Transaction::Abort&) {
            }
        }
    };
    auto finish_worker = [] {
        if (TThread::txn != nullptr) {
            delete TThread::txn;
            TThread::txn = nullptr;
        }
        Transaction::rcu_quiesce();
    };
    std::thread resurrection_worker([&] {
        silo_thread_init();
        TThread::enable_multiverison();
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            resurrected.store(
                retry_abort([&] {
                    return mt.put(lcdf::Str("key"), mako::Encode("new"));
                }),
                std::memory_order_release);
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        finish_worker();
    });
    std::thread delete_worker([&] {
        silo_thread_init();
        TThread::enable_multiverison();
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            deleted.store(retry_abort([&] {
                              return mt.erase(lcdf::Str("key"));
                          }),
                          std::memory_order_release);
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        finish_worker();
    });

    while (ready.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    start.store(true, std::memory_order_release);
    resurrection_worker.join();
    delete_worker.join();

    ASSERT_EQ(failures.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(resurrected.load(std::memory_order_acquire));
    std::string out;
    if (deleted.load(std::memory_order_acquire)) {
        EXPECT_EQ(mt.approx_size(), 0U);
        EXPECT_FALSE(mt.get(lcdf::Str("key"), out));
    } else {
        EXPECT_EQ(mt.approx_size(), 1U);
        ASSERT_TRUE(mt.get(lcdf::Str("key"), out));
        EXPECT_EQ(out.substr(0, 3), "new");
    }
}

enum class PresentRowSecondOperation {
    Delete,
    Update,
};

void expect_present_row_delete_conflict(PresentRowSecondOperation second_op,
                                        long table_id,
                                        const char* table_name) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(table_id, table_name);
    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("old")));
    ASSERT_EQ(mt.approx_size(), 1U);

    std::atomic<unsigned> staged_count{0};
    std::atomic<bool> release_first{false};
    std::atomic<bool> first_done{false};
    std::atomic<bool> first_staged{false};
    std::atomic<bool> second_staged{false};
    std::atomic<bool> first_committed{false};
    std::atomic<bool> second_committed{false};
    std::atomic<unsigned> failures{0};
    const std::string updated_value = mako::Encode("updated");

    auto finish_worker = [] {
        if (TThread::txn != nullptr) {
            if (TThread::txn->has_active_state())
                TThread::txn->silent_abort();
            delete TThread::txn;
            TThread::txn = nullptr;
        }
        Transaction::rcu_quiesce();
    };

    std::thread first([&] {
        silo_thread_init();
        TThread::enable_multiverison();
        try {
            Sto::start_transaction();
            first_staged.store(mt.transDelete(lcdf::Str("key")),
                               std::memory_order_release);
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        staged_count.fetch_add(1, std::memory_order_release);
        while (!release_first.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            if (first_staged.load(std::memory_order_acquire))
                first_committed.store(Sto::try_commit(),
                                      std::memory_order_release);
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        first_done.store(true, std::memory_order_release);
        finish_worker();
    });

    std::thread second([&] {
        silo_thread_init();
        TThread::enable_multiverison();
        try {
            Sto::start_transaction();
            const bool staged = second_op == PresentRowSecondOperation::Delete
                ? mt.transDelete(lcdf::Str("key"))
                : mt.transPut(lcdf::Str("key"), updated_value);
            second_staged.store(staged, std::memory_order_release);
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        staged_count.fetch_add(1, std::memory_order_release);
        while (!first_done.load(std::memory_order_acquire))
            std::this_thread::yield();
        try {
            if (second_staged.load(std::memory_order_acquire))
                second_committed.store(Sto::try_commit(),
                                       std::memory_order_release);
        } catch (...) {
            failures.fetch_add(1, std::memory_order_relaxed);
        }
        finish_worker();
    });

    while (staged_count.load(std::memory_order_acquire) != 2)
        std::this_thread::yield();
    release_first.store(true, std::memory_order_release);
    first.join();
    second.join();

    EXPECT_EQ(failures.load(std::memory_order_relaxed), 0U);
    EXPECT_TRUE(first_staged.load(std::memory_order_acquire));
    EXPECT_TRUE(second_staged.load(std::memory_order_acquire));
    EXPECT_TRUE(first_committed.load(std::memory_order_acquire));
    EXPECT_FALSE(second_committed.load(std::memory_order_acquire));
    EXPECT_EQ(mt.approx_size(), 0U);
    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("key"), out));
}

TEST_F(SiloNonTxnApi, ConcurrentMvDeletesOfPresentRowConflict) {
    expect_present_row_delete_conflict(PresentRowSecondOperation::Delete,
                                       9013,
                                       "mt_mv_present_delete_delete");
}

TEST_F(SiloNonTxnApi, ConcurrentMvDeleteInvalidatesStagedUpdate) {
    expect_present_row_delete_conflict(PresentRowSecondOperation::Update,
                                       9014,
                                       "mt_mv_present_delete_update");
}

#if READ_MY_WRITES
TEST_F(SiloNonTxnApi, MassTransInsertThenDeleteCancelsInMvMode) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9009, "mt_mv_insert_delete");
    const std::string value = mako::Encode("value");

    Sto::start_transaction();
    EXPECT_FALSE(mt.transInsert(lcdf::Str("key"), value));
    EXPECT_TRUE(mt.transDelete(lcdf::Str("key")));
    EXPECT_TRUE(Sto::try_commit());
    EXPECT_EQ(mt.approx_size(), 0U);
    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("key"), out));

    Sto::start_transaction();
    EXPECT_FALSE(mt.transInsert(lcdf::Str("aborted"), value));
    EXPECT_TRUE(mt.transDelete(lcdf::Str("aborted")));
    Sto::silent_abort();
    EXPECT_EQ(mt.approx_size(), 0U);
    EXPECT_FALSE(mt.get(lcdf::Str("aborted"), out));
}

TEST_F(SiloNonTxnApi, MassTransResurrectionThenDeleteIsNetZeroInMvMode) {
    ScopedMultiversionMode multiversion;
    mbta_type& mt = make_masstrans(9010, "mt_mv_resurrection_delete");
    ASSERT_TRUE(mt.put(lcdf::Str("key"), mako::Encode("old")));
    ASSERT_TRUE(mt.erase(lcdf::Str("key")));
    const std::string value = mako::Encode("new");

    Sto::start_transaction();
    EXPECT_FALSE(mt.transPut(lcdf::Str("key"), value));
    EXPECT_TRUE(mt.transDelete(lcdf::Str("key")));
    EXPECT_TRUE(Sto::try_commit());
    EXPECT_EQ(mt.approx_size(), 0U);
    std::string out;
    EXPECT_FALSE(mt.get(lcdf::Str("key"), out));
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
