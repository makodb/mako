//
// Simple Transaction Tests for Mako Database
//

#include <iostream>
#include <chrono>
#include <thread>
#include <mako.hh>
#include <examples/common.h>

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
                    table->put(txn, key, value);
                else
                    table2->put(txn, key, value);
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
                table->get(txn, key, value);
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
                table->put(txn, key, value);
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
                table->put(txn, key, value);
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
                table->put(txn, key, value);
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
                table->get(txn, key, value);
                db->commit_txn(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->abort_txn(txn);
            }

            std::string expected0 = "updated_0000";
            VERIFY(value==expected0, "value check");
        }
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
    delete worker;
}

int main() {
    abstract_db *db = new mbta_wrapper;
    db->init() ;
    printf("=== Mako Transaction Tests  ===\n");
    
    auto config = new transport::Configuration(
        get_current_absolute_path() + "../src/mako/config/local-shards2-warehouses1.yml"
    );
    BenchmarkConfig::getInstance().setConfig(config);
    
    run_tests(db);
    
    delete db;
    
    printf("\n" GREEN "All tests completed successfully!" RESET "\n");
    return 0;
}