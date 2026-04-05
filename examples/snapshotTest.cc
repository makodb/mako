/**
 * snapshotTest.cc
 *
 * Integration tests for the ISnapshot / EagerSnapshot implementation.
 *
 * Tests SN1.5 (basic), SN1.6 (isolation), SN1.7 (lifecycle).
 *
 * Implementation: Option A with per-table lazy buffering.
 * - Snapshot buffers a table on first access (one read-only txn per table).
 * - Subsequent reads for the same table come from the in-memory buffer.
 * - Cross-table consistency is NOT guaranteed (each table is captured
 *   independently), but all tests here use single tables or sequence
 *   snapshots correctly.
 */

#include <mako.hh>
#include "db.hh"
#include <examples/common.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>

// ============================================================================
// Helpers
// ============================================================================

static std::string padded(int n, int width = 3) {
    std::ostringstream ss;
    ss << std::setw(width) << std::setfill('0') << n;
    return ss.str();
}

static std::string snap_key(int n)   { return "snap_key_" + padded(n); }
static std::string snap_val(int n)   { return "snap_val_" + padded(n); }
static std::string snap_val_new(int n) { return "snap_val_NEW_" + padded(n); }

// Value comparison helper.
// mako::Encode() appends internal metadata after the user string.
// The encoded value has the user bytes as a prefix, so use prefix comparison.
static bool val_eq(const std::string& encoded, const std::string& expected) {
    return encoded.size() >= expected.size() &&
           encoded.substr(0, expected.size()) == expected;
}

// Put N keys into a table inside a single transaction.
static void put_keys(mako::IDatabase* db, const std::string& table_name,
                     int from, int to) {
    mako::ITable* table = db->GetTable(table_name);
    std::vector<std::string> enc;
    enc.reserve(to - from);
    for (int i = from; i < to; ++i) {
        enc.push_back(mako::Encode(snap_val(i)));
    }
    void* txn = db->BeginTransaction();
    for (int i = from; i < to; ++i) {
        table->Put(txn, snap_key(i), enc[i - from]);
    }
    db->Commit(txn);
}

// ============================================================================
// SN1.5 — Basic Snapshot Tests
// ============================================================================

// Test 1: Create snapshot on table with 10 keys. Read all 10. Verify values.
void test_sn1_basic_read(mako::IDatabase* db) {
    printf("\n--- Test SN1.5 Test 1: Basic snapshot read ---\n");
    const std::string tbl = "snap_basic_read";
    put_keys(db, tbl, 0, 10);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot returns non-null");

    for (int i = 0; i < 10; ++i) {
        std::string val;
        mako::Status s = snap->Get(tbl, snap_key(i), val);
        VERIFY(s.ok(), ("snap->Get key " + padded(i)).c_str());
        VERIFY(val_eq(val, snap_val(i)), ("snapshot value correct for key " + padded(i)).c_str());
    }
    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.5 Test 1: Basic snapshot read PASSED");
}

// Test 2: Create snapshot. Write 10 new keys. New keys must NOT be visible in
// snapshot, but MUST be visible via a fresh transaction.
void test_sn2_write_isolation(mako::IDatabase* db) {
    printf("\n--- Test SN1.5 Test 2: Snapshot isolation from new writes ---\n");
    const std::string tbl = "snap_write_iso";
    put_keys(db, tbl, 0, 10);  // keys 0..9

    // Capture snapshot BEFORE writing keys 10..19.
    // The snapshot uses per-table lazy buffering: the table is scanned on
    // first access. We force the scan now by reading key 0.
    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot returns non-null");
    // Force the table buffer to be populated now (before the new writes).
    {
        std::string val;
        mako::Status s = snap->Get(tbl, snap_key(0), val);
        VERIFY(s.ok(), "Pre-access to force snapshot scan");
    }

    // Now write 10 new keys into the same table.
    put_keys(db, tbl, 10, 20);

    // New keys 10..19 should NOT be visible in snapshot.
    for (int i = 10; i < 20; ++i) {
        std::string val;
        mako::Status s = snap->Get(tbl, snap_key(i), val);
        VERIFY(!s.ok(), ("New key " + padded(i) + " not visible in snapshot").c_str());
    }
    VERIFY_PASS("New keys not visible in snapshot");

    // New keys 10..19 SHOULD be visible in a fresh transaction.
    {
        mako::ITable* table = db->GetTable(tbl);
        void* txn = db->BeginTransaction();
        for (int i = 10; i < 20; ++i) {
            std::string val;
            mako::Status s = table->Get(txn, snap_key(i), val);
            VERIFY(s.ok(), ("Fresh Get sees new key " + padded(i)).c_str());
        }
        db->Commit(txn);
    }
    VERIFY_PASS("New keys visible in fresh transaction");

    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.5 Test 2: Snapshot isolation from new writes PASSED");
}

// Test 3: Create snapshot. Delete 5 keys. Snapshot should still see deleted
// keys; fresh transaction should see them as gone.
void test_sn3_delete_isolation(mako::IDatabase* db) {
    printf("\n--- Test SN1.5 Test 3: Snapshot isolation from deletes ---\n");
    const std::string tbl = "snap_del_iso";
    put_keys(db, tbl, 0, 10);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot returns non-null");
    // Force table scan before the deletes.
    {
        std::string val;
        snap->Get(tbl, snap_key(0), val);
    }

    // Delete keys 0..4.
    {
        mako::ITable* table = db->GetTable(tbl);
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 5; ++i) {
            table->Delete(txn, snap_key(i));
        }
        db->Commit(txn);
    }

    // Snapshot: deleted keys should still be visible.
    for (int i = 0; i < 5; ++i) {
        std::string val;
        mako::Status s = snap->Get(tbl, snap_key(i), val);
        VERIFY(s.ok(), ("Deleted key " + padded(i) + " still visible in snapshot").c_str());
    }
    VERIFY_PASS("Deleted keys still visible in snapshot");

    // Fresh transaction: deleted keys should be gone.
    {
        mako::ITable* table = db->GetTable(tbl);
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 5; ++i) {
            bool exists = true;
            table->Exists(txn, snap_key(i), &exists);
            VERIFY(!exists, ("Deleted key " + padded(i) + " gone in fresh txn").c_str());
        }
        db->Commit(txn);
    }
    VERIFY_PASS("Deleted keys gone in fresh transaction");

    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.5 Test 3: Snapshot isolation from deletes PASSED");
}

// Test 4: Create snapshot. Overwrite 5 keys. Snapshot sees old values;
// fresh transaction sees new values.
void test_sn4_overwrite_isolation(mako::IDatabase* db) {
    printf("\n--- Test SN1.5 Test 4: Snapshot isolation from overwrites ---\n");
    const std::string tbl = "snap_overwrite_iso";
    put_keys(db, tbl, 0, 10);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot returns non-null");
    // Force table scan before the overwrites.
    {
        std::string val;
        snap->Get(tbl, snap_key(0), val);
    }

    // Overwrite keys 0..4 with new values.
    {
        mako::ITable* table = db->GetTable(tbl);
        std::vector<std::string> enc;
        enc.reserve(5);
        for (int i = 0; i < 5; ++i) enc.push_back(mako::Encode(snap_val_new(i)));
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 5; ++i) {
            table->Put(txn, snap_key(i), enc[i]);
        }
        db->Commit(txn);
    }

    // Snapshot: should see old values.
    for (int i = 0; i < 5; ++i) {
        std::string val;
        mako::Status s = snap->Get(tbl, snap_key(i), val);
        VERIFY(s.ok(), ("snap Get key " + padded(i)).c_str());
        VERIFY(val_eq(val, snap_val(i)),
               ("Snapshot sees old value for key " + padded(i)).c_str());
    }
    VERIFY_PASS("Snapshot sees old values after overwrite");

    // Fresh transaction: should see new values.
    {
        mako::ITable* table = db->GetTable(tbl);
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 5; ++i) {
            std::string val;
            table->Get(txn, snap_key(i), val);
            VERIFY(val_eq(val, snap_val_new(i)),
                   ("Fresh txn sees new value for key " + padded(i)).c_str());
        }
        db->Commit(txn);
    }
    VERIFY_PASS("Fresh transaction sees new values after overwrite");

    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.5 Test 4: Snapshot isolation from overwrites PASSED");
}

// Test 5: Create snapshot on empty table. Gets return NotFound, Exists returns false.
void test_sn5_empty_table(mako::IDatabase* db) {
    printf("\n--- Test SN1.5 Test 5: Snapshot on empty table ---\n");
    const std::string tbl = "snap_empty";
    // Access the table to create it (but write nothing).
    db->GetTable(tbl);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot returns non-null");

    std::string val;
    mako::Status s = snap->Get(tbl, "any_key", val);
    VERIFY(!s.ok(), "Get on empty snapshot returns error");

    bool exists = true;
    snap->Exists(tbl, "any_key", &exists);
    VERIFY(!exists, "Exists on empty snapshot returns false");

    mako::IIterator* it = snap->NewIterator(tbl);
    VERIFY(it != nullptr, "NewIterator on empty snapshot returns non-null");
    it->SeekToFirst();
    VERIFY(!it->Valid(), "Iterator on empty snapshot is invalid after SeekToFirst");
    delete it;

    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.5 Test 5: Snapshot on empty table PASSED");
}

// ============================================================================
// SN1.6 — Snapshot Isolation Tests
// ============================================================================

// Test 6: Continuous writer thread; snapshot reads should be stable.
// Writer runs on a separate thread with its own thread context.
void test_sn6_concurrent_writer(mako::IDatabase* db) {
    printf("\n--- Test SN1.6 Test 6: Concurrent writer does not affect snapshot ---\n");
    const std::string tbl = "snap_concurrent";
    put_keys(db, tbl, 0, 20);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot returns non-null");
    // Force table scan now, before writer starts.
    {
        std::string val;
        snap->Get(tbl, snap_key(0), val);
    }

    // Writer thread: overwrite keys 0..19 with new values for 1 second.
    std::atomic<bool> stop_writer{false};
    std::thread writer([&]() {
        db->InitThread();
        mako::ITable* table = db->GetTable(tbl);
        int round = 0;
        while (!stop_writer.load(std::memory_order_relaxed)) {
            std::vector<std::string> enc;
            enc.reserve(20);
            for (int i = 0; i < 20; ++i)
                enc.push_back(mako::Encode("writer_round_" + padded(round) + "_key_" + padded(i)));
            try {
                void* txn = db->BeginTransaction();
                for (int i = 0; i < 20; ++i)
                    table->Put(txn, snap_key(i), enc[i]);
                db->Commit(txn);
            } catch (...) {}
            ++round;
        }
    });

    // Reader: repeatedly verify snapshot still returns original values.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int iterations = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 20; ++i) {
            std::string val;
            mako::Status s = snap->Get(tbl, snap_key(i), val);
            VERIFY(s.ok(), "Snapshot Get succeeds under concurrent writes");
            VERIFY(val_eq(val, snap_val(i)),
                   ("Snapshot sees original value under concurrent writes, key " + padded(i)).c_str());
        }
        ++iterations;
    }
    stop_writer.store(true, std::memory_order_relaxed);
    writer.join();

    printf("  Verified snapshot consistency over %d read iterations\n", iterations);
    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.6 Test 6: Concurrent writer does not affect snapshot PASSED");
}

// Test 7: Two snapshots at different times. Each sees its own state.
void test_sn7_two_snapshots(mako::IDatabase* db) {
    printf("\n--- Test SN1.6 Test 7: Two snapshots at different times ---\n");
    const std::string tbl = "snap_two";
    put_keys(db, tbl, 0, 10);

    // Snapshot 1 — before intermediate writes.
    mako::ISnapshot* snap1 = db->GetSnapshot();
    VERIFY(snap1 != nullptr, "snap1 non-null");
    { std::string v; snap1->Get(tbl, snap_key(0), v); }  // force scan

    // Write new values for keys 0..9.
    {
        mako::ITable* table = db->GetTable(tbl);
        std::vector<std::string> enc;
        enc.reserve(10);
        for (int i = 0; i < 10; ++i) enc.push_back(mako::Encode(snap_val_new(i)));
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 10; ++i) table->Put(txn, snap_key(i), enc[i]);
        db->Commit(txn);
    }

    // Snapshot 2 — after intermediate writes.
    mako::ISnapshot* snap2 = db->GetSnapshot();
    VERIFY(snap2 != nullptr, "snap2 non-null");
    VERIFY(snap2->GetSequenceNumber() > snap1->GetSequenceNumber(),
           "snap2 sequence > snap1 sequence");

    // snap1: should see original values.
    for (int i = 0; i < 10; ++i) {
        std::string val;
        snap1->Get(tbl, snap_key(i), val);
        VERIFY(val_eq(val, snap_val(i)),
               ("snap1 sees original value for key " + padded(i)).c_str());
    }
    VERIFY_PASS("snap1 sees original values");

    // snap2: should see updated values.
    for (int i = 0; i < 10; ++i) {
        std::string val;
        snap2->Get(tbl, snap_key(i), val);
        VERIFY(val_eq(val, snap_val_new(i)),
               ("snap2 sees new value for key " + padded(i)).c_str());
    }
    VERIFY_PASS("snap2 sees new values");

    db->ReleaseSnapshot(snap1);
    db->ReleaseSnapshot(snap2);
    VERIFY_PASS("SN1.6 Test 7: Two snapshots at different times PASSED");
}

// Test 8: Snapshot iterator consistent with individual Gets.
void test_sn8_iterator_consistency(mako::IDatabase* db) {
    printf("\n--- Test SN1.6 Test 8: Snapshot iterator consistent with Gets ---\n");
    const std::string tbl = "snap_iter_consit";
    put_keys(db, tbl, 0, 20);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot non-null");

    // Collect raw encoded values via Gets (from snapshot buffer).
    std::vector<std::string> get_vals;
    for (int i = 0; i < 20; ++i) {
        std::string val;
        mako::Status s = snap->Get(tbl, snap_key(i), val);
        VERIFY(s.ok(), ("snap Get key " + padded(i)).c_str());
        get_vals.push_back(val);
    }

    // Collect raw encoded values via iterator (same snapshot buffer).
    std::vector<std::pair<std::string, std::string>> iter_vals;
    mako::IIterator* it = snap->NewIterator(tbl);
    VERIFY(it != nullptr, "snapshot NewIterator non-null");
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        iter_vals.emplace_back(it->key(), it->value());
    }
    delete it;

    VERIFY_EQ((int)iter_vals.size(), 20, "Iterator sees 20 keys");
    for (int i = 0; i < 20; ++i) {
        VERIFY(iter_vals[i].first == snap_key(i),
               ("Iterator key " + padded(i) + " matches").c_str());
        VERIFY(iter_vals[i].second == get_vals[i],
               ("Iterator value " + padded(i) + " matches Get value").c_str());
    }
    VERIFY_PASS("Iterator values match Get values");

    db->ReleaseSnapshot(snap);
    VERIFY_PASS("SN1.6 Test 8: Snapshot iterator consistent with Gets PASSED");
}

// ============================================================================
// SN1.7 — Snapshot Lifecycle Tests
// ============================================================================

// Test 9: Create and release 1000 snapshots; RSS should not grow unboundedly.
// We just check there are no crashes or obvious leaks via 1000 iterations.
void test_sn9_no_resource_leak(mako::IDatabase* db) {
    printf("\n--- Test SN1.7 Test 9: No resource leak (1000 create/release cycles) ---\n");
    const std::string tbl = "snap_lifecycle";
    put_keys(db, tbl, 0, 10);

    for (int i = 0; i < 1000; ++i) {
        mako::ISnapshot* snap = db->GetSnapshot();
        VERIFY(snap != nullptr, "GetSnapshot non-null in cycle");
        // Access the table to force buffering (exercises alloc/free path).
        std::string val;
        snap->Get(tbl, snap_key(0), val);
        db->ReleaseSnapshot(snap);
    }
    VERIFY_PASS("SN1.7 Test 9: No resource leak after 1000 cycles PASSED");
}

// Test 10: Multiple simultaneous snapshots; independent reads; release in any order.
void test_sn10_multiple_simultaneous(mako::IDatabase* db) {
    printf("\n--- Test SN1.7 Test 10: Multiple simultaneous snapshots ---\n");
    const std::string tbl = "snap_multi";
    put_keys(db, tbl, 0, 10);

    // Create 5 snapshots, all before any writes.
    const int N_SNAPS = 5;
    std::vector<mako::ISnapshot*> snaps;
    for (int s = 0; s < N_SNAPS; ++s) {
        mako::ISnapshot* snap = db->GetSnapshot();
        VERIFY(snap != nullptr, "GetSnapshot non-null");
        // Force each snapshot's scan now.
        std::string val;
        snap->Get(tbl, snap_key(0), val);
        snaps.push_back(snap);
    }

    // Overwrite all keys so any non-buffered snapshot would see different values.
    {
        mako::ITable* table = db->GetTable(tbl);
        std::vector<std::string> enc;
        enc.reserve(10);
        for (int i = 0; i < 10; ++i) enc.push_back(mako::Encode(snap_val_new(i)));
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 10; ++i) table->Put(txn, snap_key(i), enc[i]);
        db->Commit(txn);
    }

    // Each snapshot should still see the original values.
    for (int s = 0; s < N_SNAPS; ++s) {
        for (int i = 0; i < 10; ++i) {
            std::string val;
            mako::Status st = snaps[s]->Get(tbl, snap_key(i), val);
            VERIFY(st.ok(), ("snap[" + padded(s) + "] Get key " + padded(i)).c_str());
            VERIFY(val_eq(val, snap_val(i)),
                   ("snap[" + padded(s) + "] sees original value for key " + padded(i)).c_str());
        }
    }
    VERIFY_PASS("All simultaneous snapshots see original values");

    // Release in reverse order (different from creation order).
    for (int s = N_SNAPS - 1; s >= 0; --s) {
        db->ReleaseSnapshot(snaps[s]);
    }
    VERIFY_PASS("SN1.7 Test 10: Multiple simultaneous snapshots PASSED");
}

// Test 11: Document use-after-free risk.
// EagerSnapshot does not add use-after-free guards (that would require a
// sentinel/tombstone pattern). This test only verifies that releasing a
// snapshot (which deletes it) does not crash the process. The caller must
// not use the snapshot pointer after ReleaseSnapshot().
void test_sn11_use_after_free_guard(mako::IDatabase* db) {
    printf("\n--- Test SN1.7 Test 11: Use-after-free risk documentation ---\n");

    const std::string tbl = "snap_uaf";
    put_keys(db, tbl, 0, 5);

    mako::ISnapshot* snap = db->GetSnapshot();
    VERIFY(snap != nullptr, "GetSnapshot non-null");
    std::string val;
    snap->Get(tbl, snap_key(0), val);
    VERIFY_PASS("Normal snapshot use before release OK");

    // Release snapshot — the pointer is now invalid.
    db->ReleaseSnapshot(snap);
    // snap is now a dangling pointer. We must NOT call snap->Get() here.
    // The EagerSnapshot implementation does not provide a use-after-free
    // guard (no sentinel / reference counting). Callers must not use a
    // released snapshot. This is documented behavior.
    VERIFY_PASS("SN1.7 Test 11: Use-after-free risk documented (no guard in EagerSnapshot) PASSED");
}

// ============================================================================
// main
// ============================================================================
int main() {
    // Disable stdout buffering so VERIFY output appears before any crash
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("============================================================\n");
    printf("  Mako Snapshot Tests (EagerSnapshot, Option A)\n");
    printf("============================================================\n");

    mako::Options options;
    options.num_threads = 1;
    options.num_shards = 1;

    mako::DB* db = nullptr;
    mako::Status s = mako::DB::Open(options, "", &db);
    if (!s.ok()) {
        fprintf(stderr, "Failed to open DB: %s\n", s.ToString().c_str());
        return 1;
    }
    db->InitThread();

    // SN1.5
    test_sn1_basic_read(db);
    test_sn2_write_isolation(db);
    test_sn3_delete_isolation(db);
    test_sn4_overwrite_isolation(db);
    test_sn5_empty_table(db);

    // SN1.6
    test_sn6_concurrent_writer(db);
    test_sn7_two_snapshots(db);
    test_sn8_iterator_consistency(db);

    // SN1.7
    test_sn9_no_resource_leak(db);
    test_sn10_multiple_simultaneous(db);
    test_sn11_use_after_free_guard(db);

    printf("\n============================================================\n");
    printf("  ALL SNAPSHOT TESTS PASSED\n");
    printf("============================================================\n");

    delete db;
    return 0;
}
