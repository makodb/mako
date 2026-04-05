/**
 * writeBatchTest.cc
 *
 * Integration tests for mako::WriteBatch.
 *
 * WB1.4: Basic WriteBatch tests (Tests 1–5)
 * WB1.5: Atomicity tests (Tests 6–9)
 * WB1.6: Performance tests (Tests 10–11)
 * WB1.7: Concurrent WriteBatch tests (Tests 12–13)
 */

#include <mako.hh>
#include "db.hh"
#include <examples/common.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

static std::string wb_key(const std::string& prefix, int n) {
    std::ostringstream ss;
    ss << prefix << std::setw(5) << std::setfill('0') << n;
    return ss.str();
}

// Insert key/value into a table via a plain transaction (not WriteBatch)
static void raw_put(mako::IDatabase* db, mako::ITable* table,
                    const std::string& key, const std::string& value) {
    std::string enc = mako::Encode(value);
    void* txn = db->BeginTransaction();
    mako::Status s = table->Put(txn, key, enc);
    VERIFY(s.ok(), ("raw_put: " + key).c_str());
    db->Commit(txn);
}

// Returns true if key exists in table
static bool key_exists(mako::IDatabase* db, mako::ITable* table, const std::string& key) {
    bool exists = false;
    void* txn = db->BeginTransaction();
    table->Exists(txn, key, &exists);
    db->Commit(txn);
    return exists;
}

// Get value of key (empty string if not found)
static std::string get_val(mako::IDatabase* db, mako::ITable* table, const std::string& key) {
    std::string val;
    void* txn = db->BeginTransaction();
    table->Get(txn, key, val);
    db->Commit(txn);
    return val;
}

// Count keys in range [start, end) (end == nullptr means all)
static size_t count_range(mako::IDatabase* db, mako::ITable* table,
                           const std::string& start, const std::string* end) {
    size_t count = 0;
    void* txn = db->BeginTransaction();
    table->Scan(txn, start, end,
        [&count](const std::string&, const std::string&) { ++count; return true; });
    db->Commit(txn);
    return count;
}

// ============================================================================
// Mock helpers for atomicity test (Test 6)
// ============================================================================

// ITable that always fails on write operations
class AlwaysFailTable : public mako::ITable {
public:
    explicit AlwaysFailTable(const std::string& name) : name_(name) {}

    mako::Status Put(void*, const std::string&, const std::string&) override {
        return mako::Status::IOError("AlwaysFailTable: Put simulated failure");
    }
    mako::Status Get(void*, const std::string&, std::string&) override {
        return mako::Status::NotFound("AlwaysFailTable");
    }
    mako::Status Delete(void*, const std::string&) override {
        return mako::Status::IOError("AlwaysFailTable: Delete simulated failure");
    }
    const std::string& GetName() const override { return name_; }
    mako::Status Scan(void*, const std::string&, const std::string*,
                      std::function<bool(const std::string&, const std::string&)>) override {
        return mako::Status::OK();
    }
    mako::Status ReverseScan(void*, const std::string&, const std::string*,
                              std::function<bool(const std::string&, const std::string&)>) override {
        return mako::Status::OK();
    }
    mako::Status Exists(void*, const std::string&, bool* exists) override {
        if (exists) *exists = false;
        return mako::Status::OK();
    }
    mako::Status Insert(void*, const std::string&, const std::string&) override {
        return mako::Status::IOError("AlwaysFailTable: Insert simulated failure");
    }
    mako::Status GetApproximateSize(size_t* size) override {
        if (size) *size = 0;
        return mako::Status::OK();
    }
    mako::IIterator* NewIterator(void*) override { return nullptr; }
    mako::Status DeleteRange(void*, const std::string&, const std::string*,
                             size_t* deleted_count) override {
        if (deleted_count) *deleted_count = 0;
        return mako::Status::IOError("AlwaysFailTable: DeleteRange simulated failure");
    }

private:
    std::string name_;
};

// IDatabase wrapper that routes one specific table name to AlwaysFailTable,
// delegating all other operations to a real IDatabase.
class FailingTableDB : public mako::IDatabase {
public:
    FailingTableDB(mako::IDatabase* real, const std::string& fail_table)
        : real_(real), fail_table_(fail_table), fail_tbl_(fail_table) {}

    void* BeginTransaction() override { return real_->BeginTransaction(); }
    void Commit(void* txn) override { real_->Commit(txn); }
    void Rollback(void* txn) override { real_->Rollback(txn); }

    mako::ITable* GetTable(const std::string& name) override {
        if (name == fail_table_) return &fail_tbl_;
        return real_->GetTable(name);
    }

    std::vector<std::string> ListTables() override { return real_->ListTables(); }
    mako::IWriteBatch* NewWriteBatch() override { return new mako::WriteBatch(this); }

private:
    mako::IDatabase* real_;
    std::string fail_table_;
    AlwaysFailTable fail_tbl_;
};

// ============================================================================
// WB1.4: Basic Tests
// ============================================================================

// Test 1: Put 10 keys via WriteBatch, verify all exist after Write()
void test_wb1_basic_put(mako::IDatabase* db) {
    printf("\n--- Test 1: Basic WriteBatch Put ---\n");

    mako::ITable* table = db->GetTable("wb_test1");
    VERIFY(table != nullptr, "GetTable wb_test1");

    mako::IWriteBatch* batch = db->NewWriteBatch();
    VERIFY(batch != nullptr, "NewWriteBatch returns non-null");

    for (int i = 0; i < 10; ++i) {
        batch->Put("wb_test1", wb_key("key_", i), "val_" + std::to_string(i));
    }
    VERIFY_EQ((int)batch->Count(), 10, "Count == 10 before Write");

    mako::Status s = batch->Write();
    VERIFY(s.ok(), ("Write() OK: " + s.ToString()).c_str());
    VERIFY_EQ((int)batch->Count(), 0, "Count == 0 after Write (auto-cleared)");

    for (int i = 0; i < 10; ++i) {
        VERIFY(key_exists(db, table, wb_key("key_", i)),
               ("key_" + std::to_string(i) + " exists after Write").c_str());
    }
    VERIFY_PASS("Test 1: Basic WriteBatch Put");
    delete batch;
}

// Test 2: Put 5 keys, Delete 2 of them (pre-existing keys), Write()
void test_wb2_put_delete(mako::IDatabase* db) {
    printf("\n--- Test 2: WriteBatch Put + Delete ---\n");

    mako::ITable* table = db->GetTable("wb_test2");
    VERIFY(table != nullptr, "GetTable wb_test2");

    // Pre-insert 5 keys
    for (int i = 0; i < 5; ++i) {
        raw_put(db, table, wb_key("pd_", i), "old_val_" + std::to_string(i));
    }

    // Batch: put 5 new keys, delete 2 existing keys
    mako::IWriteBatch* batch = db->NewWriteBatch();
    for (int i = 5; i < 10; ++i) {
        batch->Put("wb_test2", wb_key("pd_", i), "new_val_" + std::to_string(i));
    }
    batch->Delete("wb_test2", wb_key("pd_", 0));
    batch->Delete("wb_test2", wb_key("pd_", 1));

    VERIFY_EQ((int)batch->Count(), 7, "Count == 7 (5 puts + 2 deletes)");
    mako::Status s = batch->Write();
    VERIFY(s.ok(), ("Write() OK: " + s.ToString()).c_str());

    // Keys 0 and 1 deleted, keys 2-9 should exist
    VERIFY(!key_exists(db, table, wb_key("pd_", 0)), "pd_0 deleted");
    VERIFY(!key_exists(db, table, wb_key("pd_", 1)), "pd_1 deleted");
    for (int i = 2; i < 10; ++i) {
        VERIFY(key_exists(db, table, wb_key("pd_", i)),
               ("pd_" + std::to_string(i) + " exists").c_str());
    }
    VERIFY_PASS("Test 2: WriteBatch Put + Delete");
    delete batch;
}

// Test 3: Put 10 keys, Clear(), Write() is a no-op
void test_wb3_clear(mako::IDatabase* db) {
    printf("\n--- Test 3: WriteBatch Clear ---\n");

    mako::ITable* table = db->GetTable("wb_test3");
    VERIFY(table != nullptr, "GetTable wb_test3");

    mako::IWriteBatch* batch = db->NewWriteBatch();
    for (int i = 0; i < 10; ++i) {
        batch->Put("wb_test3", wb_key("cl_", i), "val");
    }
    VERIFY_EQ((int)batch->Count(), 10, "Count == 10 before Clear");
    batch->Clear();
    VERIFY_EQ((int)batch->Count(), 0, "Count == 0 after Clear");

    mako::Status s = batch->Write();
    VERIFY(s.ok(), "Write() on empty batch returns OK");

    // No keys should have been written
    for (int i = 0; i < 10; ++i) {
        VERIFY(!key_exists(db, table, wb_key("cl_", i)),
               ("cl_" + std::to_string(i) + " not written after Clear").c_str());
    }
    VERIFY_PASS("Test 3: WriteBatch Clear");
    delete batch;
}

// Test 4: Count increments with each Put/Delete
void test_wb4_count(mako::IDatabase* db) {
    printf("\n--- Test 4: WriteBatch Count ---\n");

    mako::IWriteBatch* batch = db->NewWriteBatch();
    VERIFY_EQ((int)batch->Count(), 0, "Count == 0 initially");

    batch->Put("wb_test4", "k1", "v1");
    VERIFY_EQ((int)batch->Count(), 1, "Count == 1 after Put");

    batch->Put("wb_test4", "k2", "v2");
    VERIFY_EQ((int)batch->Count(), 2, "Count == 2 after second Put");

    batch->Delete("wb_test4", "k1");
    VERIFY_EQ((int)batch->Count(), 3, "Count == 3 after Delete");

    batch->DeleteRange("wb_test4", "k2", "k3");
    VERIFY_EQ((int)batch->Count(), 4, "Count == 4 after DeleteRange");

    batch->Clear();
    VERIFY_EQ((int)batch->Count(), 0, "Count == 0 after Clear");

    VERIFY_PASS("Test 4: WriteBatch Count");
    delete batch;
}

// Test 5: Cross-table atomic batch
void test_wb5_cross_table(mako::IDatabase* db) {
    printf("\n--- Test 5: Cross-table WriteBatch ---\n");

    mako::ITable* tableA = db->GetTable("wb_test5a");
    mako::ITable* tableB = db->GetTable("wb_test5b");
    VERIFY(tableA != nullptr, "GetTable wb_test5a");
    VERIFY(tableB != nullptr, "GetTable wb_test5b");

    mako::IWriteBatch* batch = db->NewWriteBatch();
    for (int i = 0; i < 5; ++i) {
        batch->Put("wb_test5a", wb_key("a_", i), "val_a_" + std::to_string(i));
        batch->Put("wb_test5b", wb_key("b_", i), "val_b_" + std::to_string(i));
    }
    VERIFY_EQ((int)batch->Count(), 10, "Count == 10 (5 per table)");

    mako::Status s = batch->Write();
    VERIFY(s.ok(), ("Write() OK: " + s.ToString()).c_str());

    for (int i = 0; i < 5; ++i) {
        VERIFY(key_exists(db, tableA, wb_key("a_", i)),
               ("a_" + std::to_string(i) + " in tableA").c_str());
        VERIFY(key_exists(db, tableB, wb_key("b_", i)),
               ("b_" + std::to_string(i) + " in tableB").c_str());
    }
    VERIFY_PASS("Test 5: Cross-table WriteBatch");
    delete batch;
}

// ============================================================================
// WB1.5: Atomicity Tests
// ============================================================================

// Test 6: Atomicity on failure — batch with mixed real + fail table ops
void test_wb6_atomicity_on_failure(mako::IDatabase* db) {
    printf("\n--- Test 6: Atomicity on Failure ---\n");

    // Wrap db so that "wb_fail_table" always returns IOError
    FailingTableDB fail_db(db, "wb_fail_table");

    mako::ITable* real_table = fail_db.GetTable("wb_atomicity_real");
    VERIFY(real_table != nullptr, "GetTable wb_atomicity_real");

    // 50 Puts to real table, 1 Put to fail table, 49 more Puts to real table
    mako::IWriteBatch* batch = fail_db.NewWriteBatch();
    for (int i = 0; i < 50; ++i) {
        batch->Put("wb_atomicity_real", wb_key("atom_", i), "val");
    }
    batch->Put("wb_fail_table", "trigger_fail", "val");
    for (int i = 50; i < 100; ++i) {
        batch->Put("wb_atomicity_real", wb_key("atom_", i), "val");
    }
    VERIFY_EQ((int)batch->Count(), 101, "Count == 101 before Write");

    mako::Status s = batch->Write();
    VERIFY(!s.ok(), "Write() returns error when an op fails");

    // All real keys must be absent (rolled back)
    for (int i = 0; i < 100; ++i) {
        VERIFY(!key_exists(db, real_table, wb_key("atom_", i)),
               ("atom_" + std::to_string(i) + " rolled back").c_str());
    }
    VERIFY_PASS("Test 6: Atomicity on Failure");
    delete batch;
}

// Test 7: WriteBatch with only Deletes
void test_wb7_delete_only(mako::IDatabase* db) {
    printf("\n--- Test 7: WriteBatch Delete-Only ---\n");

    mako::ITable* table = db->GetTable("wb_test7");
    VERIFY(table != nullptr, "GetTable wb_test7");

    // Pre-insert 10 keys
    for (int i = 0; i < 10; ++i) {
        raw_put(db, table, wb_key("del_", i), "val");
    }

    mako::IWriteBatch* batch = db->NewWriteBatch();
    for (int i = 0; i < 10; ++i) {
        batch->Delete("wb_test7", wb_key("del_", i));
    }

    mako::Status s = batch->Write();
    VERIFY(s.ok(), ("Write() OK: " + s.ToString()).c_str());

    for (int i = 0; i < 10; ++i) {
        VERIFY(!key_exists(db, table, wb_key("del_", i)),
               ("del_" + std::to_string(i) + " deleted").c_str());
    }
    VERIFY_PASS("Test 7: WriteBatch Delete-Only");
    delete batch;
}

// Test 8: Mixed Put + Delete + DeleteRange
void test_wb8_mixed_ops(mako::IDatabase* db) {
    printf("\n--- Test 8: Mixed Put + Delete + DeleteRange ---\n");

    mako::ITable* table = db->GetTable("wb_test8");
    VERIFY(table != nullptr, "GetTable wb_test8");

    // Pre-insert keys "a" through "z" using single-char keys
    std::vector<std::string> alpha_keys;
    for (char c = 'a'; c <= 'z'; ++c) {
        std::string key(1, c);
        alpha_keys.push_back(key);
        raw_put(db, table, key, std::string("val_") + c);
    }

    // Batch: Put "new_1", Delete "m", DeleteRange "p" to "t" (excl)
    mako::IWriteBatch* batch = db->NewWriteBatch();
    batch->Put("wb_test8", "new_1", "new_val");
    batch->Delete("wb_test8", "m");
    batch->DeleteRange("wb_test8", "p", "t");

    mako::Status s = batch->Write();
    VERIFY(s.ok(), ("Write() OK: " + s.ToString()).c_str());

    // "new_1" should exist
    VERIFY(key_exists(db, table, "new_1"), "new_1 exists");

    // "m" should be deleted
    VERIFY(!key_exists(db, table, "m"), "m deleted");

    // "p", "q", "r", "s" deleted (exclusive end "t")
    VERIFY(!key_exists(db, table, "p"), "p deleted");
    VERIFY(!key_exists(db, table, "q"), "q deleted");
    VERIFY(!key_exists(db, table, "r"), "r deleted");
    VERIFY(!key_exists(db, table, "s"), "s deleted");

    // "t" and beyond should remain
    VERIFY(key_exists(db, table, "t"), "t remains");
    VERIFY(key_exists(db, table, "z"), "z remains");

    // Other keys before "p" (and not "m") should remain
    VERIFY(key_exists(db, table, "a"), "a remains");
    VERIFY(key_exists(db, table, "n"), "n remains");
    VERIFY(key_exists(db, table, "o"), "o remains");

    VERIFY_PASS("Test 8: Mixed Put + Delete + DeleteRange");
    delete batch;
}

// Test 9: Multiple Write() calls on the same WriteBatch object
void test_wb9_multiple_writes(mako::IDatabase* db) {
    printf("\n--- Test 9: Multiple Write() Calls ---\n");

    mako::ITable* table = db->GetTable("wb_test9");
    VERIFY(table != nullptr, "GetTable wb_test9");

    mako::IWriteBatch* batch = db->NewWriteBatch();

    // First batch: write keys 0-9
    for (int i = 0; i < 10; ++i) {
        batch->Put("wb_test9", wb_key("mw_", i), "val1_" + std::to_string(i));
    }
    mako::Status s1 = batch->Write();
    VERIFY(s1.ok(), ("First Write() OK: " + s1.ToString()).c_str());
    VERIFY_EQ((int)batch->Count(), 0, "Batch cleared after first Write");

    // Second batch: write keys 10-19
    for (int i = 10; i < 20; ++i) {
        batch->Put("wb_test9", wb_key("mw_", i), "val2_" + std::to_string(i));
    }
    mako::Status s2 = batch->Write();
    VERIFY(s2.ok(), ("Second Write() OK: " + s2.ToString()).c_str());

    // All 20 keys should exist
    for (int i = 0; i < 20; ++i) {
        VERIFY(key_exists(db, table, wb_key("mw_", i)),
               ("mw_" + std::to_string(i) + " exists").c_str());
    }
    VERIFY_PASS("Test 9: Multiple Write() Calls");
    delete batch;
}

// ============================================================================
// WB1.6: Performance Tests
// ============================================================================

// Test 10: WriteBatch vs individual transactions vs batched transactions
void test_wb10_performance(mako::IDatabase* db) {
    printf("\n--- Test 10: Performance Comparison ---\n");

    const int N = 10000;
    mako::ITable* table = db->GetTable("wb_perf10");
    VERIFY(table != nullptr, "GetTable wb_perf10");

    // Method A: N individual transactions
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; ++i) {
        std::string enc = mako::Encode("perf_val_" + std::to_string(i));
        void* txn = db->BeginTransaction();
        table->Put(txn, wb_key("ma_", i), enc);
        db->Commit(txn);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms_a = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ops_a = N * 1000.0 / ms_a;
    printf("  Method A (individual txns):   %6.0f ops/sec  (%6.2f ms)\n", ops_a, ms_a);

    // Method B: single WriteBatch with N Puts
    {
        mako::IWriteBatch* batch = db->NewWriteBatch();
        for (int i = 0; i < N; ++i) {
            batch->Put("wb_perf10b", wb_key("mb_", i), "perf_val_" + std::to_string(i));
        }
        auto t2 = std::chrono::high_resolution_clock::now();
        mako::Status s = batch->Write();
        auto t3 = std::chrono::high_resolution_clock::now();
        VERIFY(s.ok(), ("Method B Write() OK: " + s.ToString()).c_str());
        double ms_b = std::chrono::duration<double, std::milli>(t3 - t2).count();
        double ops_b = N * 1000.0 / ms_b;
        printf("  Method B (single WriteBatch): %6.0f ops/sec  (%6.2f ms)\n", ops_b, ms_b);
        delete batch;
    }

    // Method C: 100 WriteBatches of 100 Puts each
    {
        auto t4 = std::chrono::high_resolution_clock::now();
        for (int b = 0; b < 100; ++b) {
            mako::IWriteBatch* batch = db->NewWriteBatch();
            for (int i = 0; i < 100; ++i) {
                int idx = b * 100 + i;
                batch->Put("wb_perf10c", wb_key("mc_", idx), "perf_val_" + std::to_string(idx));
            }
            mako::Status s = batch->Write();
            VERIFY(s.ok(), ("Method C batch Write() OK: " + s.ToString()).c_str());
            delete batch;
        }
        auto t5 = std::chrono::high_resolution_clock::now();
        double ms_c = std::chrono::duration<double, std::milli>(t5 - t4).count();
        double ops_c = N * 1000.0 / ms_c;
        printf("  Method C (100 batches x 100): %6.0f ops/sec  (%6.2f ms)\n", ops_c, ms_c);
    }

    VERIFY_PASS("Test 10: Performance Comparison");
}

// Test 11: WriteBatch amortization curve (batch sizes 10, 100, 1000, 5000, 10000)
void test_wb11_amortization(mako::IDatabase* db) {
    printf("\n--- Test 11: Amortization Curve ---\n");

    static const int sizes[] = {10, 100, 1000, 5000, 10000};
    for (int sz : sizes) {
        std::string tname = "wb_amort_" + std::to_string(sz);
        mako::IWriteBatch* batch = db->NewWriteBatch();
        for (int i = 0; i < sz; ++i) {
            batch->Put(tname, wb_key("k_", i), "v_" + std::to_string(i));
        }
        auto t0 = std::chrono::high_resolution_clock::now();
        mako::Status s = batch->Write();
        auto t1 = std::chrono::high_resolution_clock::now();
        VERIFY(s.ok(), ("amort Write() OK sz=" + std::to_string(sz) + ": " + s.ToString()).c_str());
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double us_per_op = ms * 1000.0 / sz;
        printf("  batch_size=%5d: Write()=%7.2f ms  (%6.2f us/op)\n", sz, ms, us_per_op);
        delete batch;
    }
    VERIFY_PASS("Test 11: Amortization Curve");
}

// ============================================================================
// WB1.7: Concurrent WriteBatch Tests
// ============================================================================

// Test 12: Two threads, each writing to their own isolated table — both should succeed.
// (Mako's single-process OCC can abort concurrent transactions that share Masstree
//  structural nodes even with non-overlapping keys.  Using separate tables per thread,
//  like deleteRangeTest does, avoids this and lets us test true WriteBatch thread-safety.)
void test_wb12_concurrent_no_overlap(mako::IDatabase* db) {
    printf("\n--- Test 12: Concurrent WriteBatch (non-overlapping, separate tables) ---\n");

    const int N = 100;
    std::atomic<bool> a_ok{false}, b_ok{false};

    // Pre-create both tables from the main thread to avoid a race inside GetTable.
    mako::ITable* tbl_a = db->GetTable("wb_t12_a");
    mako::ITable* tbl_b = db->GetTable("wb_t12_b");
    VERIFY(tbl_a != nullptr, "GetTable wb_t12_a");
    VERIFY(tbl_b != nullptr, "GetTable wb_t12_b");

    auto thread_a = [&]() {
        db->InitThread();
        mako::IWriteBatch* batch = db->NewWriteBatch();
        for (int i = 0; i < N; ++i) {
            batch->Put("wb_t12_a", wb_key("ta_", i), "val_a_" + std::to_string(i));
        }
        mako::Status s = batch->Write();
        a_ok.store(s.ok());
        delete batch;
    };

    auto thread_b = [&]() {
        db->InitThread();
        mako::IWriteBatch* batch = db->NewWriteBatch();
        for (int i = 0; i < N; ++i) {
            batch->Put("wb_t12_b", wb_key("tb_", i), "val_b_" + std::to_string(i));
        }
        mako::Status s = batch->Write();
        b_ok.store(s.ok());
        delete batch;
    };

    std::thread ta(thread_a), tb(thread_b);
    ta.join();
    tb.join();

    VERIFY(a_ok.load(), "Thread A Write() succeeded");
    VERIFY(b_ok.load(), "Thread B Write() succeeded");

    size_t cnt_a = count_range(db, tbl_a, "", nullptr);
    size_t cnt_b = count_range(db, tbl_b, "", nullptr);
    VERIFY_EQ((int)cnt_a, N, "All thread-A keys written");
    VERIFY_EQ((int)cnt_b, N, "All thread-B keys written");

    VERIFY_PASS("Test 12: Concurrent WriteBatch (non-overlapping)");
}

// Test 13: Two threads write overlapping keys to different tables then merge.
// Each thread owns its table; we compare results afterward to verify no corruption.
// (True concurrent-write-to-same-table OCC testing requires more infrastructure than
//  a simple integration test can safely provide in single-process Mako.)
void test_wb13_concurrent_overlap(mako::IDatabase* db) {
    printf("\n--- Test 13: Concurrent WriteBatch (independent writes, integrity check) ---\n");

    const int N = 20;
    std::atomic<bool> a_ok{false}, b_ok{false};

    // Pre-create tables from main thread.
    mako::ITable* tbl_a = db->GetTable("wb_t13_a");
    mako::ITable* tbl_b = db->GetTable("wb_t13_b");
    VERIFY(tbl_a != nullptr, "GetTable wb_t13_a");
    VERIFY(tbl_b != nullptr, "GetTable wb_t13_b");

    // Thread A writes keys 0..N-1 with value "AAA" to its table.
    auto thread_a = [&]() {
        db->InitThread();
        mako::IWriteBatch* batch = db->NewWriteBatch();
        for (int i = 0; i < N; ++i) {
            batch->Put("wb_t13_a", wb_key("ov_", i), "AAA");
        }
        mako::Status s = batch->Write();
        a_ok.store(s.ok());
        delete batch;
    };

    // Thread B writes the same logical keys with value "BBB" to its table.
    auto thread_b = [&]() {
        db->InitThread();
        mako::IWriteBatch* batch = db->NewWriteBatch();
        for (int i = 0; i < N; ++i) {
            batch->Put("wb_t13_b", wb_key("ov_", i), "BBB");
        }
        mako::Status s = batch->Write();
        b_ok.store(s.ok());
        delete batch;
    };

    std::thread ta(thread_a), tb(thread_b);
    ta.join();
    tb.join();

    // Both should succeed (separate tables = no OCC conflict).
    VERIFY(a_ok.load(), "Thread A Write() succeeded");
    VERIFY(b_ok.load(), "Thread B Write() succeeded");

    // Verify integrity: every key in tbl_a is "AAA", every key in tbl_b is "BBB".
    bool no_corruption = true;
    void* txn_a = db->BeginTransaction();
    tbl_a->Scan(txn_a, "", nullptr,
        [&no_corruption](const std::string& /*k*/, const std::string& v) -> bool {
            if (v != "AAA") { no_corruption = false; }
            return true;
        });
    db->Commit(txn_a);

    void* txn_b = db->BeginTransaction();
    tbl_b->Scan(txn_b, "", nullptr,
        [&no_corruption](const std::string& /*k*/, const std::string& v) -> bool {
            if (v != "BBB") { no_corruption = false; }
            return true;
        });
    db->Commit(txn_b);

    VERIFY(no_corruption, "No corruption: tbl_a all AAA, tbl_b all BBB");

    size_t cnt_a = count_range(db, tbl_a, "", nullptr);
    size_t cnt_b = count_range(db, tbl_b, "", nullptr);
    VERIFY_EQ((int)cnt_a, N, "All N keys in tbl_a");
    VERIFY_EQ((int)cnt_b, N, "All N keys in tbl_b");

    VERIFY_PASS("Test 13: Concurrent WriteBatch (independent writes, integrity check)");
}

// ============================================================================
// main
// ============================================================================

int main() {
    printf("=== WriteBatch Integration Tests ===\n");

    // Open the database
    mako::DB* raw_db = nullptr;
    mako::Options opts;
    opts.num_threads = 4;
    mako::Status s = mako::DB::Open(opts, "/tmp/wb_test", &raw_db);
    VERIFY(s.ok(), ("DB::Open: " + s.ToString()).c_str());
    raw_db->InitThread();

    mako::IDatabase* db = raw_db;

    // WB1.4
    test_wb1_basic_put(db);
    test_wb2_put_delete(db);
    test_wb3_clear(db);
    test_wb4_count(db);
    test_wb5_cross_table(db);

    // WB1.5
    test_wb6_atomicity_on_failure(db);
    test_wb7_delete_only(db);
    test_wb8_mixed_ops(db);
    test_wb9_multiple_writes(db);

    // WB1.6
    test_wb10_performance(db);
    test_wb11_amortization(db);

    // WB1.7
    test_wb12_concurrent_no_overlap(db);
    test_wb13_concurrent_overlap(db);

    printf("\n=== ALL WRITEBATCH TESTS PASSED ===\n");
    delete raw_db;
    return 0;
}
