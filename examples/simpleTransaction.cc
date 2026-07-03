//
// Simple Transaction Tests for Mako Database
//

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include <mako.hh>
#include <examples/common.h>

import std;

using namespace std;

class TransactionWorker {
public:
    TransactionWorker(abstract_db *db) : db(db) {
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
    }

    void initialize() {
        scoped_db_thread_ctx ctx(db, false);
        // force multiversion
        TThread::enable_multiverison();
    }

    void test_basic_transactions() {
        printf("\n--- Testing Basic Transactions ---\n");
        static abstract_ordered_index *table = db->open_index("customer_0");
        static abstract_ordered_index *table2 = db->open_index("customer_0"); // table and table2 are the exactly same!
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Write 5 keys
        for (size_t i = 0; i < 5; i++) {
            void *txn = db->new_txn(0, arena, txn_buf());
            std::string key = "test_key_" + std::to_string(i);
            std::string value = mako::Encode("test_value_" + std::to_string(i));
            try {
                if (i%2==0)
                    table->tx_put(txn, key, value);
                else
                    table2->tx_put(txn, key, value);
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Write aborted: %s\n", key.c_str());
                db->abort_txn(txn);
            }
        }
        VERIFY_PASS("Write 5 records");

        // Read and verify 5 keys
        bool all_reads_ok = true;
        for (size_t i = 0; i < 5; i++) {
            void *txn = db->new_txn(0, arena, txn_buf());
            std::string key = "test_key_" + std::to_string(i);
            std::string value = "";
            try {
                table->tx_get(txn, key, value);
                db->commit_txn(txn);
                
                std::string expected = "test_value_" + std::to_string(i);
                if (value.substr(0, expected.length()) != expected) {
                    all_reads_ok = false;
                    break;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Read aborted: %s\n", key.c_str());
                db->abort_txn(txn);
                all_reads_ok = false;
                break;
            }
        }
        VERIFY(all_reads_ok, "Read and verify 5 records");

        // Scan and verify table
        auto scan_results = scan_tables(db, table);
        bool scan_ok = true;
        for (int i = 0; i < 5; i++) {
            std::string expected_key = "test_key_" + std::to_string(i);
            std::string expected_value = "test_value_" + std::to_string(i);
            
            if (scan_results[i].first != expected_key ||
                scan_results[i].second.substr(0, expected_value.length()) != expected_value) {
                scan_ok = false;
                break;
            }
        }
        VERIFY(scan_ok, "Table scan verification");
    }

    void test_overwritten_operations() {
        printf("\n--- Testing OverwrittenOperations ---\n");
        static abstract_ordered_index *table = db->open_index("overwritten_table");

        // Write initial value
        // Add more extra bits in DO_STRUCT_COMMON_VALUE in previous codebase
        {
            void *txn = db->new_txn(0, arena, txn_buf());
            scoped_str_arena s_arena(arena);
            std::string key = "overwrite_key";
            std::string value = mako::Encode("initial_2000");
            try {
                table->tx_put(txn, key, value);
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Write aborted: %s\n", key.c_str());
                db->abort_txn(txn);
            }
        }

        // Overwrite with new value
        {
            void *txn = db->new_txn(0, arena, txn_buf());
            scoped_str_arena s_arena(arena);
            std::string key = "overwrite_key";
            std::string value = mako::Encode("updated_1000");
            try {
                table->tx_put(txn, key, value);
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Update aborted: %s\n", key.c_str());
                db->abort_txn(txn);
            }
        }

        {
            void *txn = db->new_txn(0, arena, txn_buf());
            scoped_str_arena s_arena(arena);
            std::string key = "overwrite_key";
            std::string value = mako::Encode("updated_0000");
            try {
                table->tx_put(txn, key, value);
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Update aborted: %s\n", key.c_str());
                db->abort_txn(txn);
            }
        }

        {
            void *txn = db->new_txn(0, arena, txn_buf());
            std::string key = "overwrite_key" ;
            std::string value = "";
            try {
                table->tx_get(txn, key, value);
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
            }

            std::string expected0 = "updated_0000";
            VERIFY(value==expected0, "value check");
        }
    }

    void test_different_length_overwrites() {
        printf("\n--- Testing DifferentLengthOverwrites ---\n");
        static abstract_ordered_index *table = db->open_index("difflen_table");

        // Regression test for MassTrans.hh overwrite bug:
        // Before fix, overwriting a key with a LARGER value that crosses
        // a Masstree allocation boundary caused a spurious OCC abort due
        // to a stale TransItem left in the transaction set.

        // Test 1: Small → Large (growing value, crosses allocation boundary)
        {
            std::string key = "difflen_key_1";
            // Write small value (1 byte raw → ~21 bytes encoded)
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                std::string value = mako::Encode("A");
                try {
                    table->tx_put(txn, key, value);
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                    VERIFY(false, "Small value write should not abort");
                }
            }
            // Overwrite with large value (100 bytes raw → ~120 bytes encoded)
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                std::string large(100, 'B');
                std::string value = mako::Encode(large);
                try {
                    table->tx_put(txn, key, value);
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                    VERIFY(false, "Growing overwrite should not abort (MassTrans fix)");
                }
            }
            // Verify the large value was written
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                std::string readback = "";
                try {
                    table->tx_get(txn, key, readback);
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                }
                std::string expected(100, 'B');
                VERIFY(readback.substr(0, expected.length()) == expected,
                       "Growing overwrite value check");
            }
        }

        // Test 2: Multiple size transitions (the full cycle)
        {
            std::string key = "difflen_key_2";
            struct { const char* label; std::string val; } steps[] = {
                {"tiny(1B)",      std::string(1, 'X')},
                {"small(10B)",    std::string(10, 'Y')},
                {"medium(50B)",   std::string(50, 'Z')},
                {"large(200B)",   std::string(200, 'W')},
                {"shrink(5B)",    std::string(5, 'V')},
                {"grow_again(500B)", std::string(500, 'U')},
            };

            for (auto& step : steps) {
                void *txn = db->new_txn(0, arena, txn_buf());
                std::string value = mako::Encode(step.val);
                try {
                    table->tx_put(txn, key, value);
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                    printf("  ABORT on step: %s\n", step.label);
                    VERIFY(false, "Size transition should not abort");
                }
            }
            // Verify final value
            {
                void *txn = db->new_txn(0, arena, txn_buf());
                std::string readback = "";
                try {
                    table->tx_get(txn, key, readback);
                    db->commit_txn(txn);
                } catch (abstract_db::abstract_abort_exception &ex) {
                    db->abort_txn(txn);
                }
                std::string expected(500, 'U');
                VERIFY(readback.substr(0, expected.length()) == expected,
                       "Size transition final value check");
            }
        }
        VERIFY_PASS("Different-length overwrites (regression test)");
    }

protected:
    abstract_db *const db;
    str_arena arena;
    std::string txn_obj_buf;
    inline void *txn_buf() { return (void *)txn_obj_buf.data(); }
};

void run_tests(abstract_db *db) {
    auto worker = new TransactionWorker(db);
    worker->initialize();
    worker->test_basic_transactions();
    worker->test_overwritten_operations();
    worker->test_different_length_overwrites();
    delete worker;
}

int main() {
    abstract_db *db = new mbta_wrapper;
    db->init() ;
    printf("=== Mako Transaction Tests  ===\n");

    const char* config_env = std::getenv("MAKO_CONFIG");
    std::string config_path = config_env
        ? config_env
        : get_current_absolute_path() + "../src/mako/config/local-shards2-warehouses1.yml";
    auto config = new transport::Configuration(config_path);
    BenchmarkConfig::getInstance().setConfig(config);
    
    run_tests(db);
    
    delete db;
    
    printf("\n" GREEN "All tests completed successfully!" RESET "\n");
    return 0;
}
