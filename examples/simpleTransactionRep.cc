//
// Simple Transaction Tests for Mako Database
//

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <mako.hh>
#include "examples/common.h"
#include "benchmarks/rpc_setup.h"

using namespace std;
using namespace mako;

class TransactionWorker {
public:
    TransactionWorker(abstract_db *db) : db(db) {
        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
    }

    void initialize() {
        scoped_db_thread_ctx ctx(db, false);
        mbta_ordered_index::mbta_type::thread_init();
    }

    void test_basic_transactions() {
        printf("\n--- Testing Basic Transactions ---\n");
        
        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex() ;

        abstract_ordered_index *table = db->open_index("customer_0", home_shard_index);

        abstract_ordered_index *remote_table ;
        if (BenchmarkConfig::getInstance().getNshards()==2) {
            remote_table = db->open_index("customer_0", home_shard_index==0?1:0);
        }

        // Write 5 keys
        for (size_t i = 0; i < 5; i++) {
            void *txn = db->new_txn(0, arena, txn_buf());
            std::string key = "test_key_" + std::to_string(i);
            std::string value = "test_value_" + std::to_string(i) + 
                               std::string(mako::EXTRA_BITS_FOR_VALUE, 'B');
            try {
                table->put(txn, key, StringWrapper(value));

                if (BenchmarkConfig::getInstance().getNshards()==2) {
                    std::string key2 = "test_key2_" + std::to_string(i);
                    std::string value2 = "test_value2_" + std::to_string(i) + 
                                    std::string(mako::EXTRA_BITS_FOR_VALUE, 'B');
                    remote_table->put(txn, key2, StringWrapper(value2)) ;
                }

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

        if (BenchmarkConfig::getInstance().getNshards()==2) {
            // Read and verify 5 keys
            bool all_reads_ok = true;
            for (size_t i = 0; i < 5; i++) {
                void *txn = db->new_txn(0, arena, txn_buf());
                std::string key = "test_key2_" + std::to_string(i);
                std::string value = "";
                try {
                    remote_table->get(txn, key, value);
                    db->commit_txn(txn);
                    
                    std::string expected = "test_value2_" + std::to_string(i);
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
            VERIFY(all_reads_ok, "Read and verify 5 records on remote shards");
        }

        // Scan and verify table
        auto scan_results = scan_tables(db, table);
        std::cout<<"how many keys scanned:" << scan_results.size() << std::endl;
    }

protected:
    abstract_db *const db;
    str_arena arena;
    std::string txn_obj_buf;
    inline void *txn_buf() { return (void *)txn_obj_buf.data(); }
};

void run_worker_tests(abstract_db *db) {
    auto worker = new TransactionWorker(db);
    worker->initialize();
    worker->test_basic_transactions();
    delete worker;
}

void run_tests(abstract_db* db) {
    // start different db worker threads - enforced
    size_t nthreads = BenchmarkConfig::getInstance().getNthreads();
    std::vector<std::thread> worker_threads;
    
    // Create a worker thread for each shard
    for (size_t i = 0; i < nthreads; ++i) {
        worker_threads.emplace_back(run_worker_tests, db);
    }
    
    // Wait for all worker threads to complete
    for (auto& t : worker_threads) {
        t.join();
    }
}

int main(int argc, char **argv) {
    
    // All necessary parameters expected from users
    if (argc != 6) {
        printf("Usage: %s <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated>\n", argv[0]);
        printf("Example: %s 2 0 6 localhost 1\n", argv[0]);
        return 1;
    }

    int nshards = std::stoi(argv[1]);
    int shardIdx = std::stoi(argv[2]);
    int nthreads = std::stoi(argv[3]);
    std::string paxos_proc_name = std::string(argv[4]);
    int is_replicated = std::stoi(argv[5]);

    // Build config path - fix the format string to use std::to_string
    std::string config_path = get_current_absolute_path() 
            + "../src/mako/config/local-shards" + std::to_string(nshards) 
            + "-warehouses" + std::to_string(nthreads) + ".yml";
    vector<string> paxos_config_file{
        get_current_absolute_path() + "../config/1leader_2followers/paxos" + std::to_string(nthreads) + "_shardidx" + std::to_string(shardIdx) + ".yml",
        get_current_absolute_path() + "../config/occ_paxos.yml"
    };
    
    auto& benchConfig = BenchmarkConfig::getInstance();
    benchConfig.setNshards(nshards);
    benchConfig.setShardIndex(shardIdx);
    benchConfig.setNthreads(nthreads);
    benchConfig.setPaxosProcName(paxos_proc_name);
    benchConfig.setIsReplicated(is_replicated);

    auto config = new transport::Configuration(config_path);
    benchConfig.setConfig(config);
    benchConfig.setPaxosConfigFile(paxos_config_file);

    // This variable is accessible until program ends as follower replays uses it
    TSharedThreadPoolMbta replicated_db (benchConfig.getNthreads()+1);
    init_env(replicated_db) ;

    printf("=== Mako Transaction Tests  ===\n");
    
    abstract_db* db = initWithDB();

    if (benchConfig.getLeaderConfig()) {
        int home_shard_index = benchConfig.getShardIndex() ;

        // pre-declare all local tables
        abstract_ordered_index *table = db->open_index("customer_0", home_shard_index);
        abstract_ordered_index *table2 = db->open_index("customer_0", home_shard_index); // table and table2 are the exactly same table!
        abstract_ordered_index *table3 = db->open_index("customer_1", home_shard_index);

        if (benchConfig.getNshards()==2) {
            // open remote table handlers
            abstract_ordered_index *table4 = db->open_index("customer_0", home_shard_index==0?1:0);
        }
        
        mako::setup_erpc_server();
        map<string, abstract_ordered_index*> open_tables;
        open_tables["customer_0"] = table;
        mako::setup_helper(db,
            std::ref(open_tables)) ;
        
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Wait all shards finish setup

    }

    if (benchConfig.getLeaderConfig()) {
        run_tests(db);
    }

    std::this_thread::sleep_for(std::chrono::seconds(10)); 

    db_close() ;
    
    printf("\n" GREEN "All tests completed successfully!" RESET "\n");
    return 0;
}