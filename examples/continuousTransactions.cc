//
// Continuous Transaction Test for Mako Database
// Simple test that continuously executes read/write transactions with statistics
//

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <iomanip>
#include <signal.h>
#include <map>
#include <mako.hh>
#include "examples/common.h"
#include "examples/statistics.h"
#include "benchmarks/rpc_setup.h"
#include "../src/mako/spinbarrier.h"
#include "../src/mako/benchmarks/mbta_sharded_ordered_index.hh"

using namespace std;
using namespace mako;

const int MAX_KEYS = 100000;

static Statistics stats;
static volatile bool keep_running = true;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    keep_running = false;
    printf("\nReceived signal %d, shutting down...\n", sig);
}

class ContinuousWorker {
public:
    ContinuousWorker(abstract_db *db, int worker_id, atomic<uint64_t>* worker_commits)
        : db_(db), worker_id_(worker_id), worker_commits_(worker_commits), gen_(worker_id),
          read_write_dist_(0, 99) {
        txn_obj_buf_.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf_.resize(db->sizeof_txn_object(0));

        home_shard_ = BenchmarkConfig::getInstance().getShardIndex();
        num_shards_ = BenchmarkConfig::getInstance().getNshards();
    }

    void executeTransactions() {
        mbta_sharded_ordered_index *table = db_->open_sharded_index("customer_0");
        uint64_t key_counter = 0;

        const int MAX_KEYS = 100000;

        while (keep_running) {
            // Decide if this is a read or write (30% writes, 70% reads)
            bool is_write = (read_write_dist_(gen_) < 30);

            stats.total_attempts++;

            void *txn = db_->new_txn(0, arena_, &txn_obj_buf_[0]);
            bool is_cross_shard = false;

            try {
                if (is_write) {
                    // Write transaction
                    stats.writes++;

                    // Generate key
                    uint64_t key_id = key_counter++ % MAX_KEYS;
                    string key = "key_w" + to_string(worker_id_) + "_" + to_string(key_id);
                    string value = Encode("value_" + to_string(worker_id_) + "_" + to_string(key_counter));

                    // Check which shard this key belongs to
                    int key_shard = table->check_shard(key);
                    if (key_shard != home_shard_) {
                        is_cross_shard = true;
                    }

                    table->put(txn, key, value);
                } else {
                    // Read transaction
                    stats.reads++;

                    // Generate key
                    uint64_t read_key_id = gen_() % MAX_KEYS;
                    string key = "key_w" + to_string(worker_id_) + "_" + to_string(read_key_id);

                    // Check which shard this key belongs to
                    int key_shard = table->check_shard(key);
                    if (key_shard != home_shard_) {
                        is_cross_shard = true;
                    }

                    string value;
                    table->get(txn, key, value);
                }

                db_->commit_txn(txn);
                stats.successful_commits++;
                (*worker_commits_)++;  // Track per-worker commits

                // Track cross-shard vs single-shard after successful commit
                if (is_cross_shard) {
                    stats.cross_shard++;
                } else {
                    stats.single_shard++;
                }

            } catch (abstract_db::abstract_abort_exception &ex) {
                db_->abort_txn(txn);
                stats.aborts++;
            } catch (...) {
                db_->abort_txn(txn);
                stats.aborts++;
            }
        }
    }

public:
    abstract_db *db_;
    int worker_id_;
    int home_shard_;
    int num_shards_;
    atomic<uint64_t>* worker_commits_;

    str_arena arena_;
    string txn_obj_buf_;

    mt19937 gen_;
    uniform_int_distribution<> read_write_dist_;
};

int main(int argc, char **argv) {
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Parse configuration - simplified parameters similar to simpleTransactionRep
    if (argc != 5 && argc != 6) {
        printf("Usage: %s <nshards> <shardIdx> <nthreads> <paxos_proc_name> [is_replicated]\n", argv[0]);
        printf("Example: %s 2 0 4 localhost 0\n", argv[0]);
        return 1;
    }

    int nshards = stoi(argv[1]);
    int shardIdx = stoi(argv[2]);
    int nthreads = stoi(argv[3]);
    string paxos_proc_name = string(argv[4]);
    int is_replicated = argc > 5 ? stoi(argv[5]) : 0;

    // Build config path
    string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards" + to_string(nshards)
            + "-warehouses" + to_string(nthreads) + ".yml";
    vector<string> paxos_config_file{
        get_current_absolute_path() + "../config/1leader_2followers/paxos" + to_string(nthreads) + "_shardidx" + to_string(shardIdx) + ".yml",
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

    init_env();

    printf("=== Continuous Transaction Test ===\n");
    printf("Configuration: 70%% reads, 30%% writes\n");
    printf("Home shard: %d, Total shards: %d, Workers: %d\n",
           shardIdx, nshards, nthreads);
    printf("Note: Cross-shard transactions detected automatically based on key hash\n");
    printf("Press Ctrl+C to stop...\n\n");
    fflush(stdout);

    abstract_db* db = initWithDB();

    if (benchConfig.getLeaderConfig()) {
        // pre-declare sharded tables
        mako::setup_erpc_server();
        mako::setup_helper(db, {});
        mbta_sharded_ordered_index *table = db->open_sharded_index("customer_0");
    }

    mako::NFSSync::mark_shard_up_and_wait();

    // Create per-worker commit counters
    vector<atomic<uint64_t>> worker_commits(nthreads);
    for (int i = 0; i < nthreads; i++) {
        worker_commits[i] = 0;
    }

    // Start statistics printer thread
    thread stats_thread(stats_printer_thread, ref(stats), ref(keep_running), &worker_commits);

    // Start worker threads
    vector<thread> worker_threads;
    vector<unique_ptr<ContinuousWorker>> workers;

    for (int i = 0; i < nthreads; i++) {
        workers.emplace_back(make_unique<ContinuousWorker>(db, i, &worker_commits[i]));
        worker_threads.emplace_back([&workers, i]() {
            mako::initialize_per_thread(workers[i]->db_) ;
            workers[i]->executeTransactions();
        });
    }

    // Wait for all threads to complete
    for (auto &t : worker_threads) {
        t.join();
    }

    keep_running = false;
    stats_thread.join();

    printf("\nTest completed successfully.\n");
    fflush(stdout);

    delete db;
    return 0;
}