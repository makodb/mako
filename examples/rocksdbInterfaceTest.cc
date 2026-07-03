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
    options.num_threads = 1;
    options.num_shards = 1;
    options.shard_index = 0;

    mako::DB* db = nullptr;
    mako::Status status = mako::DB::Open(options, "/tmp/mako_rocksdb_iface_test", &db);
    VERIFY(status.ok(), "DB::Open succeeds");
    VERIFY(db != nullptr, "DB pointer is valid");

    // InitThread must be called once on the main thread
    db->InitThread();

    // Run individual feature tests
    test_scan(db);
    test_rscan(db);
    test_exists(db);
    test_insert(db);
    test_approx_size(db);
    test_approx_size_stress(db);
    test_list_tables(db);
    test_nontxn_api(db);

    // Run full integration test
    test_full_integration(db);

    // Close database
    mako::Status close_status = db->Close();
    VERIFY(close_status.ok(), "DB::Close succeeds");

    printf("\n=== All RocksDB Interface Tests PASSED ===\n");
    return 0;
}
