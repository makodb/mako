/**
 * rocksdbInterfaceTest.cc
 *
 * Comprehensive integration test for the ITable/IDatabase RocksDB-like interface.
 *
 * Covers:
 *   I1.1 Scan           - forward range scan
 *   I1.2 ReverseScan    - reverse range scan
 *   I1.3 Exists         - key existence check
 *   I1.4 Insert         - put-if-not-exists
 *   I1.5 GetApproximateSize - approximate entry count
 *   I1.6 ListTables     - list all opened tables
 *   I1.7 Non-txn API    - Put/Get/Insert/Delete/Exists without a txn handle
 *   Full integration    - open/close, put/get/delete/scan lifecycle
 */

#include <stddef.h>
#include <stdio.h>

#include <atomic>
#include <thread>

#include <mako.hh>
#include "rocks_interface/db.hh"
#include <examples/common.h>

import std;

// ============================================================================
// Helpers
// ============================================================================

// Zero-pad integer to a fixed-width decimal string for lexicographic ordering
static std::string padded(int n) {
    std::ostringstream ss;
    ss << std::setw(3) << std::setfill('0') << n;
    return ss.str();
}

static std::string scan_key(int n) {
    return "scan_key_" + padded(n);
}

// ============================================================================
// Test I1.1: Scan
// ============================================================================
void test_scan(mako::IDatabase* db) {
    printf("\n--- Test I1.1: Scan (Forward Range Query) ---\n");

    mako::ITable* table = db->GetTable("scan_table");
    VERIFY(table != nullptr, "GetTable returns valid table for scan test");

    // Insert 100 keys
    {
        // Use named encoded values to avoid StringWrapper aliasing bug
        std::vector<std::string> encoded_values;
        encoded_values.reserve(100);
        for (int i = 0; i < 100; ++i) {
            encoded_values.push_back(mako::Encode("scan_val_" + padded(i)));
        }

        void* txn = db->BeginTransaction();
        for (int i = 0; i < 100; ++i) {
            mako::Status s = table->Put(txn, scan_key(i), encoded_values[i]);
            VERIFY(s.ok(), ("Put scan_key_" + padded(i)).c_str());
        }
        db->Commit(txn);
    }
    VERIFY_PASS("Insert 100 scan keys");

    // Scan from scan_key_020 to scan_key_040 (exclusive), expect keys 020..039
    {
        std::string start = scan_key(20);
        std::string end   = scan_key(40);

        std::vector<std::string> scanned_keys;
        void* txn = db->BeginTransaction();
        mako::Status s = table->Scan(txn, start, &end,
            [&](const std::string& key, const std::string& /*value*/) -> bool {
                scanned_keys.push_back(key);
                return true;
            });
        db->Commit(txn);

        VERIFY(s.ok(), "Scan returns OK");
        VERIFY_EQ((int)scanned_keys.size(), 20, "Scan returns exactly 20 keys");

        // Verify keys are in ascending order: scan_key_020 .. scan_key_039
        bool ordered = true;
        for (int i = 0; i < (int)scanned_keys.size(); ++i) {
            if (scanned_keys[i] != scan_key(20 + i)) {
                ordered = false;
                break;
            }
        }
        VERIFY(ordered, "Scanned keys are in ascending order (020..039)");
    }
    VERIFY_PASS("Test I1.1: Scan PASSED");
}

// ============================================================================
// Test I1.2: ReverseScan
// ============================================================================
void test_rscan(mako::IDatabase* db) {
    printf("\n--- Test I1.2: ReverseScan ---\n");

    mako::ITable* table = db->GetTable("scan_table");
    VERIFY(table != nullptr, "GetTable returns valid table for rscan test");

    // Reverse scan from scan_key_040 down to scan_key_020 (exclusive lower bound)
    // Expects keys 040, 039, ..., 021  (20 keys; end is exclusive)
    {
        std::string start = scan_key(40);
        std::string end   = scan_key(20);

        std::vector<std::string> scanned_keys;
        void* txn = db->BeginTransaction();
        mako::Status s = table->ReverseScan(txn, start, &end,
            [&](const std::string& key, const std::string& /*value*/) -> bool {
                scanned_keys.push_back(key);
                return true;
            });
        db->Commit(txn);

        VERIFY(s.ok(), "ReverseScan returns OK");
        VERIFY_EQ((int)scanned_keys.size(), 20, "ReverseScan returns exactly 20 keys");

        // Verify keys are in descending order: scan_key_040 .. scan_key_021
        bool ordered = true;
        for (int i = 0; i < (int)scanned_keys.size(); ++i) {
            if (scanned_keys[i] != scan_key(40 - i)) {
                ordered = false;
                break;
            }
        }
        VERIFY(ordered, "ReverseScan keys are in descending order (040..021)");
    }
    VERIFY_PASS("Test I1.2: ReverseScan PASSED");
}

// ============================================================================
// Test I1.3: Exists
// ============================================================================
void test_exists(mako::IDatabase* db) {
    printf("\n--- Test I1.3: Exists (Key Existence Check) ---\n");

    mako::ITable* table = db->GetTable("exists_table");
    VERIFY(table != nullptr, "GetTable returns valid table for exists test");

    std::string enc_val = mako::Encode("exists_value");

    // Insert a key
    {
        void* txn = db->BeginTransaction();
        mako::Status s = table->Put(txn, "exists_key", enc_val);
        VERIFY(s.ok(), "Put exists_key");
        db->Commit(txn);
    }

    // Verify exists_key is found
    {
        bool exists = false;
        void* txn = db->BeginTransaction();
        mako::Status s = table->Exists(txn, "exists_key", &exists);
        db->Commit(txn);
        VERIFY(s.ok(), "Exists returns OK for present key");
        VERIFY(exists, "Exists returns true for present key");
    }

    // Verify non-existent key returns false
    {
        bool exists = true;
        void* txn = db->BeginTransaction();
        mako::Status s = table->Exists(txn, "nonexistent_key", &exists);
        db->Commit(txn);
        VERIFY(s.ok(), "Exists returns OK for absent key");
        VERIFY(!exists, "Exists returns false for absent key");
    }

    // Delete the key then verify false
    {
        void* txn = db->BeginTransaction();
        mako::Status s = table->Delete(txn, "exists_key");
        VERIFY(s.ok(), "Delete exists_key");
        db->Commit(txn);
    }
    {
        bool exists = true;
        void* txn = db->BeginTransaction();
        mako::Status s = table->Exists(txn, "exists_key", &exists);
        db->Commit(txn);
        VERIFY(s.ok(), "Exists returns OK after delete");
        VERIFY(!exists, "Exists returns false after delete");
    }

    VERIFY_PASS("Test I1.3: Exists PASSED");
}

// ============================================================================
// Test I1.4: Insert (Put-If-Not-Exists)
// ============================================================================
void test_insert(mako::IDatabase* db) {
    printf("\n--- Test I1.4: Insert (Put-If-Not-Exists) ---\n");

    mako::ITable* table = db->GetTable("insert_table");
    VERIFY(table != nullptr, "GetTable returns valid table for insert test");

    std::string enc_val1 = mako::Encode("insert_value_1");
    std::string enc_val2 = mako::Encode("insert_value_2");
    std::string enc_val3 = mako::Encode("insert_value_3");

    // Insert a new key - should succeed
    {
        void* txn = db->BeginTransaction();
        mako::Status s = table->Insert(txn, "insert_key_a", enc_val1);
        db->Commit(txn);
        VERIFY(s.ok(), "Insert new key succeeds");
    }

    // Verify it exists
    {
        bool exists = false;
        void* txn = db->BeginTransaction();
        table->Exists(txn, "insert_key_a", &exists);
        db->Commit(txn);
        VERIFY(exists, "Inserted key exists");
    }

    // Try to insert the same key again - should fail
    {
        void* txn = db->BeginTransaction();
        mako::Status s = table->Insert(txn, "insert_key_a", enc_val2);
        db->Commit(txn);
        VERIFY(!s.ok(), "Insert duplicate key fails");
    }

    // Insert a different key - should succeed
    {
        void* txn = db->BeginTransaction();
        mako::Status s = table->Insert(txn, "insert_key_b", enc_val3);
        db->Commit(txn);
        VERIFY(s.ok(), "Insert second distinct key succeeds");
    }

    // Delete insert_key_a, then re-insert it - should succeed
    {
        void* txn = db->BeginTransaction();
        table->Delete(txn, "insert_key_a");
        db->Commit(txn);
    }
    {
        std::string enc_val4 = mako::Encode("insert_value_after_delete");
        void* txn = db->BeginTransaction();
        mako::Status s = table->Insert(txn, "insert_key_a", enc_val4);
        db->Commit(txn);
        VERIFY(s.ok(), "Insert after delete succeeds");
    }

    VERIFY_PASS("Test I1.4: Insert PASSED");
}

// ============================================================================
// Test I1.5: GetApproximateSize
// ============================================================================
void test_approx_size(mako::IDatabase* db) {
    printf("\n--- Test I1.5: GetApproximateSize ---\n");

    mako::ITable* table = db->GetTable("size_table");
    VERIFY(table != nullptr, "GetTable returns valid table for size test");

    // Empty table: size should be 0
    {
        size_t sz = 999;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize on empty table returns OK");
        VERIFY_EQ((int)sz, 0, "Empty table size is 0");
    }

    // Insert 100 keys
    {
        std::vector<std::string> encoded;
        encoded.reserve(100);
        for (int i = 0; i < 100; ++i) {
            encoded.push_back(mako::Encode("size_val_" + padded(i)));
        }
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 100; ++i) {
            table->Put(txn, "size_key_" + padded(i), encoded[i]);
        }
        db->Commit(txn);
    }

    {
        size_t sz = 0;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize after 100 inserts returns OK");
        printf("  Approximate size after 100 inserts: %zu\n", sz);
        VERIFY(sz >= 90 && sz <= 110, "Size is approximately 100 after 100 inserts");
    }

    // Delete 50 keys
    {
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 50; ++i) {
            table->Delete(txn, "size_key_" + padded(i));
        }
        db->Commit(txn);
    }

    {
        size_t sz = 0;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize after 50 deletes returns OK");
        printf("  Approximate size after 50 deletes: %zu\n", sz);
        VERIFY(sz >= 40 && sz <= 60, "Size is approximately 50 after 50 deletes");
    }

    VERIFY_PASS("Test I1.5: GetApproximateSize PASSED");
}

// ============================================================================
// Stress Test: GetApproximateSize with 1000 keys
// ============================================================================
void test_approx_size_stress(mako::IDatabase* db) {
    printf("\n--- Stress Test: GetApproximateSize (1000 keys) ---\n");

    mako::ITable* table = db->GetTable("stress_size_table");
    VERIFY(table != nullptr, "GetTable returns valid table for stress size test");

    // Insert 1000 keys in batches of 100
    {
        for (int batch = 0; batch < 10; ++batch) {
            std::vector<std::string> encoded;
            encoded.reserve(100);
            for (int i = 0; i < 100; ++i) {
                int key_idx = batch * 100 + i;
                encoded.push_back(mako::Encode("stress_val_" + padded(key_idx)));
            }
            void* txn = db->BeginTransaction();
            for (int i = 0; i < 100; ++i) {
                int key_idx = batch * 100 + i;
                table->Put(txn, "stress_key_" + padded(key_idx), encoded[i]);
            }
            db->Commit(txn);
        }
    }

    {
        size_t sz = 0;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize after 1000 inserts returns OK");
        printf("  Approximate size after 1000 inserts: %zu\n", sz);
        VERIFY(sz >= 900 && sz <= 1100, "Size is approximately 1000 after 1000 inserts");
    }

    // Delete 500 keys
    {
        for (int batch = 0; batch < 5; ++batch) {
            void* txn = db->BeginTransaction();
            for (int i = 0; i < 100; ++i) {
                int key_idx = batch * 100 + i;
                table->Delete(txn, "stress_key_" + padded(key_idx));
            }
            db->Commit(txn);
        }
    }

    {
        size_t sz = 0;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize after 500 deletes returns OK");
        printf("  Approximate size after 500 deletes: %zu\n", sz);
        VERIFY(sz >= 400 && sz <= 600, "Size is approximately 500 after 500 deletes");
    }

    VERIFY_PASS("Stress Test: GetApproximateSize PASSED");
}

// Exercise the table-wide size aggregate while independent record locks
// publish inserts and deletes in parallel. This test is part of the exact TSan
// lifecycle lane.
void test_approx_size_concurrent(mako::IDatabase* db) {
    printf("\n--- Concurrent Test: GetApproximateSize ---\n");

    mako::ITable* table = db->GetTable("concurrent_size_table");
    VERIFY(table != nullptr,
           "GetTable returns valid table for concurrent size test");

    constexpr int worker_count = 3;
    constexpr int keys_per_worker = 128;
    static std::atomic<unsigned> invocation{0};
    const std::string shared_key =
        "concurrent_shared_" +
        std::to_string(invocation.fetch_add(1, std::memory_order_relaxed));
    VERIFY(table->Put(shared_key, "shared").ok(),
           "shared contention key is present before delete race");
    std::atomic<int> ready{0};
    std::atomic<int> finished{0};
    std::atomic<int> shared_delete_successes{0};
    std::atomic<int> shared_delete_misses{0};
    std::atomic<bool> start{false};
    std::atomic<bool> workers_ok{true};
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            bool announced_ready = false;
            try {
                mako::ScopedDatabaseThreadContext thread_context(*db);
                ready.fetch_add(1, std::memory_order_release);
                announced_ready = true;
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const mako::Status shared_delete = table->Delete(shared_key);
                if (shared_delete.ok()) {
                    shared_delete_successes.fetch_add(
                        1, std::memory_order_relaxed);
                } else if (shared_delete.IsNotFound()) {
                    shared_delete_misses.fetch_add(1,
                                                   std::memory_order_relaxed);
                } else {
                    workers_ok.store(false, std::memory_order_release);
                }
                for (int key_index = 0; key_index < keys_per_worker;
                     ++key_index) {
                    const std::string key =
                        "concurrent_size_" + std::to_string(worker) + "_" +
                        std::to_string(key_index);
                    if (!table->Put(key, "value").ok()) {
                        workers_ok.store(false, std::memory_order_release);
                    }
                    if ((key_index % 2) == 0 && !table->Delete(key).ok()) {
                        workers_ok.store(false, std::memory_order_release);
                    }
                }
            } catch (...) {
                if (!announced_ready) {
                    ready.fetch_add(1, std::memory_order_release);
                }
                workers_ok.store(false, std::memory_order_release);
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }

    while (ready.load(std::memory_order_acquire) != worker_count) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    while (finished.load(std::memory_order_acquire) != worker_count) {
        size_t observed = 0;
        if (!table->GetApproximateSize(&observed).ok()) {
            workers_ok.store(false, std::memory_order_release);
        }
        std::this_thread::yield();
    }
    for (auto& worker : workers) {
        worker.join();
    }

    size_t final_size = 0;
    mako::Status status = table->GetApproximateSize(&final_size);
    VERIFY(status.ok(), "concurrent GetApproximateSize returns OK");
    VERIFY(workers_ok.load(std::memory_order_acquire),
           "concurrent size workers complete without an operation error");
    VERIFY_EQ(shared_delete_successes.load(std::memory_order_relaxed), 1,
              "exactly one contending delete removes the shared key");
    VERIFY_EQ(shared_delete_misses.load(std::memory_order_relaxed),
              worker_count - 1,
              "remaining contending deletes observe the committed absence");
    VERIFY_EQ(static_cast<int>(final_size),
              worker_count * (keys_per_worker / 2),
              "concurrent size aggregate retains every committed delta");
    VERIFY_PASS("Concurrent Test: GetApproximateSize PASSED");
}

// ============================================================================
// Test I1.6: ListTables
// ============================================================================
void test_list_tables(mako::IDatabase* db) {
    printf("\n--- Test I1.6: ListTables ---\n");

    // Access 3 tables (some may already be open from earlier tests, so use fresh names)
    db->GetTable("list_table_alpha");
    db->GetTable("list_table_beta");
    db->GetTable("list_table_gamma");

    std::vector<std::string> tables = db->ListTables();

    // Verify all 3 list_table_* names are present
    auto has = [&](const std::string& name) {
        return std::find(tables.begin(), tables.end(), name) != tables.end();
    };
    VERIFY(has("list_table_alpha"), "ListTables contains list_table_alpha");
    VERIFY(has("list_table_beta"),  "ListTables contains list_table_beta");
    VERIFY(has("list_table_gamma"), "ListTables contains list_table_gamma");
    printf("  Total tables in cache: %zu\n", tables.size());

    VERIFY_PASS("Test I1.6: ListTables PASSED");
}

// ============================================================================
// Full Integration Test (Task I1.7)
// ============================================================================
void test_full_integration(mako::IDatabase* db) {
    printf("\n--- Full Integration Test ---\n");

    mako::ITable* table = db->GetTable("integration_table");
    VERIFY(table != nullptr, "GetTable returns valid table");

    // Put 100 keys
    {
        std::vector<std::string> encoded;
        encoded.reserve(100);
        for (int i = 0; i < 100; ++i) {
            encoded.push_back(mako::Encode("integ_val_" + padded(i)));
        }
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 100; ++i) {
            mako::Status s = table->Put(txn, "integ_key_" + padded(i), encoded[i]);
            VERIFY(s.ok(), "Put integ_key");
        }
        db->Commit(txn);
    }
    VERIFY_PASS("Put 100 integration keys");

    // Get all 100 keys, verify values
    {
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 100; ++i) {
            std::string val;
            mako::Status s = table->Get(txn, "integ_key_" + padded(i), val);
            VERIFY(s.ok(), "Get integ_key");
            std::string expected = "integ_val_" + padded(i);
            VERIFY(val.substr(0, expected.size()) == expected, "Get returns correct value");
        }
        db->Commit(txn);
    }
    VERIFY_PASS("Get and verify all 100 integration keys");

    // Scan a range [integ_key_010, integ_key_030): expect 20 keys
    {
        std::string start = "integ_key_010";
        std::string end   = "integ_key_030";
        int count = 0;
        std::string prev;
        void* txn = db->BeginTransaction();
        mako::Status s = table->Scan(txn, start, &end,
            [&](const std::string& key, const std::string&) -> bool {
                VERIFY(prev.empty() || key > prev, "Scan keys in ascending order");
                prev = key;
                ++count;
                return true;
            });
        db->Commit(txn);
        VERIFY(s.ok(), "Integration Scan returns OK");
        VERIFY_EQ(count, 20, "Integration Scan returns 20 keys");
    }
    VERIFY_PASS("Integration Scan range");

    // ReverseScan a range (integ_key_020 down to integ_key_010]: expect 10 keys
    {
        std::string start = "integ_key_020";
        std::string end   = "integ_key_010";
        int count = 0;
        std::string prev;
        void* txn = db->BeginTransaction();
        mako::Status s = table->ReverseScan(txn, start, &end,
            [&](const std::string& key, const std::string&) -> bool {
                VERIFY(prev.empty() || key < prev, "ReverseScan keys in descending order");
                prev = key;
                ++count;
                return true;
            });
        db->Commit(txn);
        VERIFY(s.ok(), "Integration ReverseScan returns OK");
        VERIFY_EQ(count, 10, "Integration ReverseScan returns 10 keys");
    }
    VERIFY_PASS("Integration ReverseScan range");

    // Exists on present and absent keys
    {
        bool exists = false;
        void* txn = db->BeginTransaction();
        table->Exists(txn, "integ_key_050", &exists);
        db->Commit(txn);
        VERIFY(exists, "Exists true for present key");
    }
    {
        bool exists = true;
        void* txn = db->BeginTransaction();
        table->Exists(txn, "integ_key_999", &exists);
        db->Commit(txn);
        VERIFY(!exists, "Exists false for absent key");
    }
    VERIFY_PASS("Exists checks on integration table");

    // Insert a new key (should succeed)
    {
        std::string enc = mako::Encode("integ_new_val");
        void* txn = db->BeginTransaction();
        mako::Status s = table->Insert(txn, "integ_key_new", enc);
        db->Commit(txn);
        VERIFY(s.ok(), "Insert new key succeeds");
    }

    // Insert an existing key (should fail)
    {
        std::string enc = mako::Encode("integ_dup_val");
        void* txn = db->BeginTransaction();
        mako::Status s = table->Insert(txn, "integ_key_000", enc);
        db->Commit(txn);
        VERIFY(!s.ok(), "Insert existing key fails");
    }
    VERIFY_PASS("Insert new and duplicate key");

    // GetApproximateSize - should be ~101 (100 puts + 1 insert)
    {
        size_t sz = 0;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize OK");
        printf("  ApproximateSize after 100+1 inserts: %zu\n", sz);
        VERIFY(sz >= 90 && sz <= 115, "Size is approximately 101 after 100+1 inserts");
    }
    VERIFY_PASS("GetApproximateSize ~101");

    // Delete 50 keys
    {
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 50; ++i) {
            table->Delete(txn, "integ_key_" + padded(i));
        }
        db->Commit(txn);
    }

    // Verify deleted keys are gone, remaining are present
    {
        void* txn = db->BeginTransaction();
        for (int i = 0; i < 50; ++i) {
            bool exists = true;
            table->Exists(txn, "integ_key_" + padded(i), &exists);
            VERIFY(!exists, "Deleted key is gone");
        }
        for (int i = 50; i < 100; ++i) {
            bool exists = false;
            table->Exists(txn, "integ_key_" + padded(i), &exists);
            VERIFY(exists, "Remaining key is present");
        }
        db->Commit(txn);
    }
    VERIFY_PASS("Deleted keys gone, remaining keys present");

    // GetApproximateSize - should be ~51 (101 - 50 deletes)
    {
        size_t sz = 0;
        mako::Status s = table->GetApproximateSize(&sz);
        VERIFY(s.ok(), "GetApproximateSize after deletes OK");
        printf("  ApproximateSize after 50 deletes: %zu\n", sz);
        VERIFY(sz >= 40 && sz <= 65, "Size decreased after 50 deletes");
    }
    VERIFY_PASS("GetApproximateSize decreased after deletes");

    // ListTables
    {
        std::vector<std::string> tables_list = db->ListTables();
        VERIFY(!tables_list.empty(), "ListTables returns non-empty list");
        bool found = std::find(tables_list.begin(), tables_list.end(),
                               "integration_table") != tables_list.end();
        VERIFY(found, "ListTables includes integration_table");
        printf("  Tables in DB: %zu\n", tables_list.size());
    }
    VERIFY_PASS("ListTables in full integration");

    VERIFY_PASS("Full Integration Test PASSED");
}

// ============================================================================
// Test I1.7: Non-transactional API (docs/storage-interface.md)
// ============================================================================
void test_nontxn_api(mako::IDatabase* db) {
    printf("\n--- Test I1.7: Non-transactional API ---\n");

    mako::ITable* table = db->GetTable("nontxn_table");
    VERIFY(table != nullptr, "GetTable succeeds");

    // Each op is self-contained (an internal one-op OCC txn): no
    // BeginTransaction/Commit around it.
    // Non-txn ops take RAW bytes (the backend encodes internally) —
    // unlike the txn'd Put/Insert above, which require caller-Encoded
    // values whose lifetime spans the commit.
    mako::Status s = table->Put("nt_key", "nt_value");
    VERIFY(s.ok(), "non-txn Put succeeds");

    std::string val;
    s = table->Get("nt_key", val);
    VERIFY(s.ok(), "non-txn Get finds the key");
    VERIFY(val == "nt_value", "non-txn Get returns the decoded value");

    s = table->Get("nt_missing", val);
    VERIFY(s.IsNotFound(), "non-txn Get on absent key is NotFound");

    s = table->Insert("nt_once", "first");
    VERIFY(s.ok(), "non-txn Insert on fresh key succeeds");
    s = table->Insert("nt_once", "second");
    VERIFY(s.IsInvalidArgument(), "non-txn Insert on existing key rejected");
    s = table->Get("nt_once", val);
    VERIFY(s.ok() && val == "first", "existing value untouched by dup Insert");

    bool exists = false;
    s = table->Exists("nt_once", &exists);
    VERIFY(s.ok() && exists, "non-txn Exists true for present key");

    s = table->Delete("nt_once");
    VERIFY(s.ok(), "non-txn Delete removes the key");
    s = table->Delete("nt_once");
    VERIFY(s.IsNotFound(), "non-txn Delete on absent key is NotFound");
    s = table->Exists("nt_once", &exists);
    VERIFY(s.ok() && !exists, "non-txn Exists false after Delete");

    printf("Test I1.7 PASSED\n");
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char* argv[]) {
    printf("=== RocksDB Interface Integration Test ===\n");

    // Open database
    mako::Options options;
    // One main facade worker plus three concurrent size-counter workers.
    options.num_threads = 4;
    options.num_shards = 1;
    options.shard_index = 0;

    mako::Options unsupported_engine_options = options;
    unsupported_engine_options.storage_engine = "rust";
    mako::DB* unsupported_engine_db = nullptr;
    mako::Status unsupported_engine_status = mako::DB::Open(
        unsupported_engine_options, "/tmp/mako_unsupported_engine",
        &unsupported_engine_db);
    VERIFY(unsupported_engine_status.IsNotSupported(),
           "DB::Open rejects the Rust TPC-C adapter on the C++ facade");
    VERIFY(unsupported_engine_db == nullptr,
           "unsupported engine rejection leaves output null and admission available");

    mako::Options invalid_options = options;
    invalid_options.num_shards = 32;
    mako::DB* invalid_db = nullptr;
    mako::Status invalid_status =
        mako::DB::Open(invalid_options, "/tmp/mako_invalid_topology", &invalid_db);
    VERIFY(invalid_status.IsInvalidArgument(),
           "DB::Open rejects a topology larger than the protocol bitset");
    VERIFY(invalid_db == nullptr,
           "invalid DB::Open leaves its output null and does not consume admission");

    mako::DB* db = nullptr;
    mako::Status status = mako::DB::Open(options, "/tmp/mako_rocksdb_iface_test", &db);
    VERIFY(status.ok(), "DB::Open succeeds");
    VERIFY(db != nullptr, "DB pointer is valid");

    mako::DB* duplicate_db = nullptr;
    mako::Status duplicate_status =
        mako::DB::Open(options, "/tmp/mako_rocksdb_iface_test_2", &duplicate_db);
    VERIFY(duplicate_status.IsBusy(),
           "a second process-local DB facade is rejected");
    VERIFY(duplicate_db == nullptr,
           "rejected second DB::Open leaves its output null");

    bool unattached_begin_rejected = false;
    try {
        db->BeginTransaction();
    } catch (const std::logic_error&) {
        unattached_begin_rejected = true;
    }
    VERIFY(unattached_begin_rejected,
           "BeginTransaction rejects a thread without DB::InitThread");

    bool unmatched_end_rejected = false;
    try {
        db->EndThread();
    } catch (const std::logic_error&) {
        unmatched_end_rejected = true;
    }
    VERIFY(unmatched_end_rejected,
           "DB::EndThread rejects a thread without a matching InitThread");

    mako::ITable* lifecycle_table = db->GetTable("lifecycle_table");
    VERIFY(lifecycle_table != nullptr, "lifecycle table opens before attachment");
    VERIFY(lifecycle_table->Put("unattached", mako::Encode("value"))
               .IsInvalidArgument(),
           "local table operations reject an unattached thread");

    int first_thread_id = -1;
    int first_local_pid = -1;
    {
        mako::ScopedDatabaseThreadContext thread_context(*db);
        first_thread_id = TThread::id();
        first_local_pid = TThread::getLocalPartitionID();

        bool nested_init_rejected = false;
        try {
            db->InitThread();
        } catch (const std::logic_error&) {
            nested_init_rejected = true;
        }
        VERIFY(nested_init_rejected,
               "nested DB::InitThread is rejected before mutating TLS state");

        mako::Status busy_close = db->Close();
        VERIFY(busy_close.IsBusy(),
               "DB::Close rejects a live database thread context");
        VERIFY(db->IsOpen(), "a rejected DB::Close leaves the database open");

        void* lifecycle_txn = db->BeginTransaction();
        VERIFY(lifecycle_txn != nullptr,
               "BeginTransaction returns a non-null local token");

        bool nested_begin_rejected = false;
        try {
            db->BeginTransaction();
        } catch (const std::logic_error&) {
            nested_begin_rejected = true;
        }
        VERIFY(nested_begin_rejected,
               "nested BeginTransaction is rejected without replacing the active transaction");
        VERIFY(TThread::txn != nullptr && TThread::txn->has_active_state(),
               "rejected nested BeginTransaction preserves the original transaction");

        int foreign_token_storage = 0;
        std::string validation_value;
        bool validation_exists = false;
        VERIFY(lifecycle_table->Put(nullptr, "null-token", mako::Encode("v"))
                   .IsInvalidArgument(),
               "transactional Put rejects a null token");
        VERIFY(lifecycle_table->Get(&foreign_token_storage, "foreign-token",
                                    validation_value)
                   .IsInvalidArgument(),
               "transactional Get rejects a foreign token");
        VERIFY(lifecycle_table->Exists(nullptr, "null-token",
                                       &validation_exists)
                   .IsInvalidArgument(),
               "transactional Exists rejects a null token");
        VERIFY(lifecycle_table->Put("nested-nontxn", "v").IsInvalidArgument(),
               "non-transactional operations reject an active transaction");

        db->Commit(lifecycle_txn);
        bool duplicate_commit_rejected = false;
        try {
            db->Commit(lifecycle_txn);
        } catch (const std::logic_error&) {
            duplicate_commit_rejected = true;
        }
        VERIFY(duplicate_commit_rejected,
               "Commit rejects an already-resolved transaction token");
        VERIFY(lifecycle_table->Get(lifecycle_txn, "inactive-token",
                                    validation_value)
                   .IsInvalidArgument(),
               "table operations reject an inactive transaction token");

        void* rollback_txn = db->BeginTransaction();
        db->Rollback(rollback_txn);
        bool duplicate_rollback_rejected = false;
        try {
            db->Rollback(rollback_txn);
        } catch (const std::logic_error&) {
            duplicate_rollback_rejected = true;
        }
        VERIFY(duplicate_rollback_rejected,
               "Rollback rejects an already-resolved transaction token");

        // Run individual feature tests
        test_scan(db);
        test_rscan(db);
        test_exists(db);
        test_insert(db);
        test_approx_size(db);
        test_approx_size_stress(db);
        test_approx_size_concurrent(db);
        test_list_tables(db);
        test_nontxn_api(db);

        // Run full integration test
        test_full_integration(db);
    }

    // The configured worker topology is fixed for the process. A different OS
    // thread cannot silently alias this one-worker database's local PID or
    // ShardClient port after the original thread detaches.
    std::atomic<bool> over_admission_rejected{false};
    std::atomic<bool> failed_admission_rolled_back{false};
    std::thread extra_worker([&] {
        try {
            db->InitThread();
            db->EndThread();
        } catch (const std::runtime_error&) {
            over_admission_rejected.store(true, std::memory_order_release);
            failed_admission_rolled_back.store(
                !db->HasThreadContext(), std::memory_order_release);
        }
    });
    extra_worker.join();
    VERIFY(over_admission_rejected.load(std::memory_order_acquire),
           "a distinct worker beyond the fixed topology is rejected");
    VERIFY(failed_admission_rolled_back.load(std::memory_order_acquire),
           "failed worker admission rolls back the facade reservation");

    // A raw transaction must be resolved by its owner. InitThread rejects it
    // before changing the stable ID, mode, topology, or facade reservation.
    Sto::start_transaction();
    const int active_transaction_thread_id = TThread::id();
    bool active_transaction_rejected = false;
    try {
        db->InitThread();
    } catch (const std::logic_error&) {
        active_transaction_rejected = true;
    }
    VERIFY(active_transaction_rejected,
           "DB::InitThread rejects an already-active raw transaction");
    VERIFY(TThread::id() == active_transaction_thread_id,
           "rejected attachment does not change the transaction thread ID");
    VERIFY(!db->HasThreadContext(),
           "rejected active-transaction attachment reserves no facade context");
    Sto::silent_abort();

    // Reattach on the same OS thread. This reuses its quiesced RCU slot and
    // Sto TLS Transaction without consuming another finite worker ID.
    {
        mako::ScopedDatabaseThreadContext thread_context(*db);
        void* txn = db->BeginTransaction();
        VERIFY(TThread::id() == first_thread_id,
               "reattachment preserves the OS thread's native STO ID");
        VERIFY(TThread::getLocalPartitionID() == first_local_pid,
               "reattachment preserves the OS thread's local partition ID");
        VERIFY(TThread::txn != nullptr &&
                   TThread::txn->threadid() == first_thread_id,
               "reattachment refreshes Sto's cached native thread ID");
        db->Rollback(txn);
    }

    // The native catalog is fixed-capacity. Exhaustion is reported as nullptr
    // without terminating the process or corrupting existing mappings.
    const size_t catalog_capacity = mako::NUM_TABLES_PER_SHARD;
    size_t catalog_size = db->ListTables().size();
    for (size_t index = catalog_size; index < catalog_capacity; ++index) {
        mako::ITable* table =
            db->GetTable("capacity_fill_" + std::to_string(index));
        VERIFY(table != nullptr, "table catalog fills through its documented capacity");
    }
    VERIFY(db->ListTables().size() == catalog_capacity,
           "table catalog reaches the exact per-shard capacity");
    VERIFY(db->GetTable("capacity_overflow") == nullptr,
           "table catalog exhaustion returns nullptr");
    VERIFY(db->GetTable("capacity_overflow_again") == nullptr,
           "repeated table catalog exhaustion remains harmless");
    VERIFY(db->GetTable("lifecycle_table") == lifecycle_table,
           "existing table lookup remains stable at capacity");
    {
        mako::ScopedDatabaseThreadContext thread_context(*db);
        VERIFY(lifecycle_table->Put("capacity-existing", "still-usable").ok(),
               "existing table data operations remain usable at capacity");
    }

    // Close database
    mako::Status close_status = db->Close();
    VERIFY(close_status.ok(), "DB::Close succeeds");
    VERIFY(!db->Connect().ok(), "DB::Connect rejects a closed local facade");
    VERIFY(db->GetDB() == nullptr,
           "DB::GetDB stops publishing the native pointer after close");
    VERIFY(db->GetTable("after_close") == nullptr,
           "DB::GetTable rejects a closed local facade");

    mako::DB* reopened_db = nullptr;
    mako::Status reopen_status =
        mako::DB::Open(options, "/tmp/mako_rocksdb_iface_test_3", &reopened_db);
    VERIFY(reopen_status.IsBusy(),
           "local DB reopen is rejected while native state is process-lived");
    VERIFY(reopened_db == nullptr,
           "rejected local DB reopen leaves its output null");

    delete db;

    printf("\n=== All RocksDB Interface Tests PASSED ===\n");
    return 0;
}
