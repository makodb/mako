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

// ===========================================================================
// 6. 2PC PARTICIPANT lock lifecycle — the abort path must release row locks
// ===========================================================================
// A cross-shard transaction stages its remote writes on the participant via
// BatchLock, which LOCKS each touched row (shard_put -> transPut +
// shard_try_lock_last_writeset). That lock is held until the coordinator's
// decision: Install (commit) releases it via shard_unlock(true); Abort
// (silent_abort -> stop(false, nullptr, 0)) MUST release it too. A
// regression where the abort path skipped the unlock left the row's version
// lock_bit set forever — every later reader/writer aborts on it, a permanent
// stuck lock (observed live: one stock row locked for 24+ minutes, wedging a
// migration scan). These tests drive the participant lock/unlock protocol
// directly (no RPC, no transport) and assert the row's raw version lock_bit
// (TransactionTid::lock_bit = 0x200) is clear after BOTH outcomes.

namespace {
// TransactionTid::lock_bit (sto/Interface.hh) — bit 9 of the version word.
constexpr uint64_t kLockBit = 0x200;

// Read a row's raw version word without a transaction (no OCC, no retry) so
// a locked or leaked row is INSPECTED, not aborted on. Returns 0 if absent.
uint64_t raw_version(mbta_type* t, const std::string& key) {
    uint64_t found = 0;
    bool got = false;
    t->debugScanVersions(
        mbta_type::Str(key), mbta_type::Str(),
        [&](mbta_type::Str k, uint64_t v) -> bool {
            if (std::string(k.data(), k.length()) == key) { found = v; got = true; return false; }
            return false;  // first emitted row is the smallest key >= `key`
        });
    return got ? found : 0;
}
}  // namespace

class SiloParticipant2PC : public SiloNonTxnApi {
protected:
    // Run `body` on a DEDICATED mode-1 thread. A real helper thread lives its
    // whole life in mode 1 with an ambient participant txn; the mode-0 main
    // thread never sees it. Running the participant sequence here (instead of
    // flipping the main thread's mode) keeps the participant txn lifecycle off
    // the main thread, so a leaked lock can't leave the main thread's txn
    // dangling and the failure reports cleanly.
    template <class Body>
    static void on_participant_thread(Body body) {
        std::thread t([&]() {
            silo_thread_init();          // fresh Silo tid + threadinfo
            TThread::set_mode(1);
            Sto::start_transaction();    // idle participant invariant
            body();
        });
        t.join();
    }
};

// Positive control: the COMMIT path releases the participant lock. Proves the
// harness drives a real lock (so the abort test below isn't vacuously green).
TEST_F(SiloParticipant2PC, CommitReleasesLock) {
    mbta_ordered_index* idx = make_table("part_commit");
    const std::string key = "row";
    const std::string v0 = mako::Encode("v0");
    ASSERT_TRUE(idx->put(lcdf::Str(key), v0));                   // seed the row (mode 0)

    bool locked_after_put = false, locked_after_commit = true;
    on_participant_thread([&]() {
        // shard_put stages a POINTER into `v1`; it must outlive shard_install.
        const std::string v1 = mako::Encode("v1");
        idx->shard_put(lcdf::Str(key), v1);                      // stage + LOCK
        locked_after_put  = (raw_version(idx->mbta, key) & kLockBit) != 0;
        ASSERT_EQ(Sto::shard_validate(), 0);
        Sto::shard_install(1);
        Sto::shard_unlock(true);                                 // commit -> unlock
        locked_after_commit = (raw_version(idx->mbta, key) & kLockBit) != 0;
    });
    EXPECT_TRUE(locked_after_put)     << "shard_put must lock the row";
    EXPECT_FALSE(locked_after_commit) << "commit must clear the lock";
}

// The regression: the ABORT path must also release the participant lock.
// FAILS on the buggy stop() (guard `mode==1 && nwriteset>0` is false on the
// abort path, so the unlock loop is skipped); PASSES once the abort path
// unlocks its needs_unlock items like the commit path does.
TEST_F(SiloParticipant2PC, AbortReleasesLock) {
    mbta_ordered_index* idx = make_table("part_abort");
    const std::string key = "row";
    const std::string v0 = mako::Encode("v0");
    ASSERT_TRUE(idx->put(lcdf::Str(key), v0));  // existing row => UPDATE (cleanup can't save it)

    bool locked_before = false, locked_after_abort = true, reacquired = false;
    on_participant_thread([&]() {
        const std::string v1 = mako::Encode("v1");
        idx->shard_put(lcdf::Str(key), v1);                      // stage + LOCK
        locked_before = (raw_version(idx->mbta, key) & kLockBit) != 0;

        Sto::silent_abort();                                     // coordinator voted ABORT
        Sto::start_transaction();                                // handler's shard_reset re-init

        locked_after_abort = (raw_version(idx->mbta, key) & kLockBit) != 0;

        // Behavioral corollary: the row must be writable again. On the leak
        // this shard_put cannot re-acquire the lock and throws.
        const std::string v2 = mako::Encode("v2");
        try {
            idx->shard_put(lcdf::Str(key), v2);
            ASSERT_EQ(Sto::shard_validate(), 0);
            Sto::shard_install(2);
            Sto::shard_unlock(true);
            reacquired = true;
        } catch (abstract_db::abstract_abort_exception&) {
            Sto::silent_abort();   // discard the failed re-acquire's staged txn
        }
    });

    ASSERT_TRUE(locked_before)       << "precondition: shard_put must lock the row";
    EXPECT_FALSE(locked_after_abort) << "LEAK: participant abort left the row's lock_bit set (eternal lock)";
    EXPECT_TRUE(reacquired)          << "row remained locked after abort -> re-write could not acquire it";

    if (reacquired) {  // value corollary, read on the mode-0 main thread
        std::string out;
        ASSERT_TRUE(idx->get(lcdf::Str(key), out, std::string::npos));
        EXPECT_EQ(out.substr(0, 2), "v2");
    }
}

}  // namespace
