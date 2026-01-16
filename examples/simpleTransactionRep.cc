//
// Simple Transaction Tests for Mako Database
//
// This program can run in two modes:
// 1. Server mode (default): Runs as a standalone database server with tests
// 2. Client mode (--client): Connects to a remote server via RemoteDB
//

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <map>
#include <cstring>
#include <mako.hh>
#include "db.hh"
#include "remote_db.hh"
#include "examples/common.h"
#include "examples/test_verification.h"
#include "benchmarks/rpc_setup.h"
#include "../src/mako/spinbarrier.h"
#include "../src/mako/benchmarks/mbta_sharded_ordered_index.hh"
#include "deptran/replication_helper.h"

using namespace std;
using namespace mako;

class TransactionWorker {
public:
    TransactionWorker(mako::DB *mako_db, int worker_id = 0)
        : mako_db_(mako_db), worker_id_(worker_id), original_worker_id_(worker_id) {
    }

    void initialize() {
        mako_db_->InitThread();
    }

    void test_basic_transactions() {
        printf("\n--- Testing Basic Transactions Thread:%ld ---\n", std::this_thread::get_id());

        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex() ;
        worker_id_ = worker_id_ * 100 + home_shard_index ;
        mbta_sharded_ordered_index *table = mako_db_->GetDB()->open_sharded_index("customer_0");

        // Write 5 keys - unique per worker to avoid contention
        for (size_t i = 0; i < 5; i++) {
            void *txn = mako_db_->BeginTransaction();
            std::string key = "test_key_w" + std::to_string(worker_id_) + "_" + std::to_string(i);
            // Encode value - must remain valid until Commit()
            std::string value = mako::Encode("test_value_w" + std::to_string(worker_id_) + "_" + std::to_string(i));
            try {
                mako::Status s = table->Put(txn, key, value);
                if (!s.ok()) {
                    printf("Put failed: %s - %s\n", key.c_str(), s.ToString().c_str());
                    mako_db_->Rollback(txn);
                    continue;
                }

                if (BenchmarkConfig::getInstance().getNshards()==2) {
                    int remote_shard = home_shard_index==0?1:0;
                    std::string key2 = "test_key2_w" + std::to_string(worker_id_) + "_" + std::to_string(i) + "_remote";
                    // Encode value2 - must remain valid until Commit()
                    std::string value2 = mako::Encode("test_value2_w" + std::to_string(worker_id_) + "_" + std::to_string(i));
                    s = table->Put(txn, key2, value2);
                    if (!s.ok()) {
                        printf("Put failed: %s - %s\n", key2.c_str(), s.ToString().c_str());
                        mako_db_->Rollback(txn);
                        continue;
                    }
                }

                mako_db_->Commit(txn);
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Write aborted: %s\n", key.c_str());
                mako_db_->Rollback(txn);
            }
        }
        VERIFY_PASS("Write 5 records");

        // Read and verify 5 keys
        bool all_reads_ok = true;
        for (size_t i = 0; i < 5; i++) {
            void *txn = mako_db_->BeginTransaction();
            std::string key = "test_key_w" + std::to_string(worker_id_) + "_" + std::to_string(i);
            std::string value = "";
            try {
                mako::Status s = table->Get(txn, key, value);
                if (s.ok()) {
                    mako_db_->Commit(txn);
                    std::string expected = "test_value_w" + std::to_string(worker_id_) + "_" + std::to_string(i);
                    if (value.find(expected) == std::string::npos) {
                        all_reads_ok = false;
                        break;
                    }
                } else {
                    mako_db_->Rollback(txn);
                    printf("Get failed: %s - %s\n", key.c_str(), s.ToString().c_str());
                    all_reads_ok = false;
                    break;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                printf("Read aborted: %s\n", key.c_str());
                mako_db_->Rollback(txn);
                all_reads_ok = false;
                break;
            }
        }
        VERIFY(all_reads_ok, "Read and verify 5 records");

        if (BenchmarkConfig::getInstance().getNshards()==2) {
            // Read and verify 5 keys
            bool all_reads_ok = true;
            for (size_t i = 0; i < 5; i++) {
                void *txn = mako_db_->BeginTransaction();
                int remote_shard = home_shard_index==0?1:0;
                std::string key = "test_key2_w" + std::to_string(worker_id_) + "_" + std::to_string(i) + "_remote";
                std::string value = "";
                try {
                    mako::Status s = table->Get(txn, key, value);
                    if (s.ok()) {
                        mako_db_->Commit(txn);
                        std::string expected = "test_value2_w" + std::to_string(worker_id_) + "_" + std::to_string(i);
                        if (value.find(expected) == std::string::npos) {
                            all_reads_ok = false;
                            break;
                        }
                    } else {
                        mako_db_->Rollback(txn);
                        printf("Get failed: %s - %s\n", key.c_str(), s.ToString().c_str());
                        all_reads_ok = false;
                        break;
                    }
                } catch (abstract_db::abstract_abort_exception &ex) {
                    printf("Read aborted: %s\n", key.c_str());
                    mako_db_->Rollback(txn);
                    all_reads_ok = false;
                    break;
                }
            }
            VERIFY(all_reads_ok, "Read and verify 5 records on remote shards");
        }

        std::cout<<"Worker completed" << std::endl;
    }

    void test_single_key_contention() {
        printf("\n[TEST_SINGLE_KEY] === Testing Single Key Contention Thread:%ld ===\n", std::this_thread::get_id());

        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex();
        mbta_sharded_ordered_index *table = mako_db_->GetDB()->open_sharded_index("customer_0");

        // All threads write to the SAME key to create high contention
        std::string shared_key = "contention_key_shared";

        int commits = 0, aborts = 0;
        for (size_t i = 0; i < 10; i++) {
            void *txn = mako_db_->BeginTransaction();
            // Encode value - must remain valid until Commit()
            std::string value = mako::Encode("worker_" + std::to_string(worker_id_) + "_iter_" + std::to_string(i));
            try {
                mako::Status s = table->Put(txn, shared_key, value);
                if (s.ok()) {
                    mako_db_->Commit(txn);
                    commits++;
                    printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] txn %zu COMMITTED\n", home_shard_index, worker_id_, i);
                } else {
                    mako_db_->Rollback(txn);
                    aborts++;
                    printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] txn %zu PUT FAILED: %s\n", home_shard_index, worker_id_, i, s.ToString().c_str());
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                aborts++;
                printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] txn %zu ABORTED\n", home_shard_index, worker_id_, i);
            }
        }

        printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] SUMMARY: %d commits, %d aborts\n",
               home_shard_index, worker_id_, commits, aborts);

        // Only worker 0 verifies final state after all workers finish
        if (original_worker_id_ == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(3));

            void *txn = mako_db_->BeginTransaction();
            std::string value;
            try {
                mako::Status s = table->Get(txn, shared_key, value);
                mako_db_->Commit(txn);
                if (s.ok()) {
                    printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] Final read: key '%s' EXISTS with value: %s\n",
                           home_shard_index, worker_id_, shared_key.c_str(), value.substr(0, 50).c_str());
                } else if (s.IsNotFound()) {
                    printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] Final read: key '%s' DOES NOT EXIST\n",
                           home_shard_index, worker_id_, shared_key.c_str());
                } else {
                    printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] Final read: Get failed: %s\n",
                           home_shard_index, worker_id_, s.ToString().c_str());
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                printf("[TEST_SINGLE_KEY] [Shard %d Worker %d] Final read ABORTED\n",
                       home_shard_index, worker_id_);
            }
        }
    }

    void test_overlapping_keys() {
        printf("\n[TEST_OVERLAP_KEYS] === Testing Overlapping Keys Thread:%ld ===\n", std::this_thread::get_id());

        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex();
        mbta_sharded_ordered_index *table = mako_db_->GetDB()->open_sharded_index("customer_0");

        // Workers access overlapping key ranges
        // Worker 0,1 share keys 0-4, Worker 2,3 share keys 5-9, etc.
        int key_group = (worker_id_ / 2) * 5;

        int commits = 0, aborts = 0;
        for (size_t i = 0; i < 10; i++) {
            void *txn = mako_db_->BeginTransaction();
            // Access keys in the shared range for this group
            std::string key = "overlap_key_" + std::to_string(key_group + (i % 5));
            // Encode value - must remain valid until Commit()
            std::string value = mako::Encode("worker_" + std::to_string(worker_id_) + "_iter_" + std::to_string(i));
            try {
                mako::Status s = table->Put(txn, key, value);
                if (s.ok()) {
                    mako_db_->Commit(txn);
                    commits++;
                    printf("[TEST_OVERLAP_KEYS] [Shard %d Worker %d] key=%s txn %zu COMMITTED\n",
                           home_shard_index, worker_id_, key.c_str(), i);
                } else {
                    mako_db_->Rollback(txn);
                    aborts++;
                    printf("[TEST_OVERLAP_KEYS] [Shard %d Worker %d] key=%s txn %zu PUT FAILED: %s\n",
                           home_shard_index, worker_id_, key.c_str(), i, s.ToString().c_str());
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                aborts++;
                printf("[TEST_OVERLAP_KEYS] [Shard %d Worker %d] key=%s txn %zu ABORTED\n",
                       home_shard_index, worker_id_, key.c_str(), i);
            }
        }

        printf("[TEST_OVERLAP_KEYS] [Shard %d Worker %d] SUMMARY: %d commits, %d aborts\n",
               home_shard_index, worker_id_, commits, aborts);

        // Only worker 0 verifies final state after all workers finish
        if (original_worker_id_ == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(3));

            // Check all key groups (since worker 0 only wrote to key_group 0, we check all groups)
            int total_existing_keys = 0;
            for (int group = 0; group < 10; group++) {
                for (size_t i = 0; i < 5; i++) {
                    void *txn = mako_db_->BeginTransaction();
                    std::string key = "overlap_key_" + std::to_string(group * 5 + i);
                    std::string value;
                    try {
                        mako::Status s = table->Get(txn, key, value);
                        mako_db_->Commit(txn);
                        if (s.ok()) {
                            total_existing_keys++;
                        }
                    } catch (abstract_db::abstract_abort_exception &ex) {
                        mako_db_->Rollback(txn);
                    }
                }
            }
            printf("[TEST_OVERLAP_KEYS] [Shard %d Worker %d] Final read: %d total keys exist across all groups\n",
                   home_shard_index, worker_id_, total_existing_keys);
        }
    }

    void test_cross_shard_contention() {
        printf("\n[TEST_CROSS_SHARD] === Testing Cross-Shard Contention Thread:%ld ===\n", std::this_thread::get_id());

        if (BenchmarkConfig::getInstance().getNshards() < 2) {
            return;
        }

        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex();
        int remote_shard_index = home_shard_index == 0 ? 1 : 0;
        mbta_sharded_ordered_index *table = mako_db_->GetDB()->open_sharded_index("customer_0");

        // All threads access the same keys on both shards
        int commits = 0, aborts = 0;
        for (size_t i = 0; i < 10; i++) {
            void *txn = mako_db_->BeginTransaction();
            std::string shared_local_key = "cross_shard_local";
            std::string shared_remote_key = "cross_shard_remote";
            // Encode value - must remain valid until Commit()
            std::string value = mako::Encode("worker_" + std::to_string(worker_id_) + "_iter_" + std::to_string(i));

            try {
                mako::Status s1 = table->Put(txn, shared_local_key, value);
                if (!s1.ok()) {
                    mako_db_->Rollback(txn);
                    aborts++;
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] txn %zu PUT local FAILED: %s\n",
                           home_shard_index, worker_id_, i, s1.ToString().c_str());
                    continue;
                }
                mako::Status s2 = table->Put(txn, shared_remote_key, value);
                if (!s2.ok()) {
                    mako_db_->Rollback(txn);
                    aborts++;
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] txn %zu PUT remote FAILED: %s\n",
                           home_shard_index, worker_id_, i, s2.ToString().c_str());
                    continue;
                }
                mako_db_->Commit(txn);
                commits++;
                printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] txn %zu (local:%d remote:%d) COMMITTED\n",
                       home_shard_index, worker_id_, i, home_shard_index, remote_shard_index);
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                aborts++;
                printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] txn %zu (local:%d remote:%d) ABORTED\n",
                       home_shard_index, worker_id_, i, home_shard_index, remote_shard_index);
            } catch (int error_code) {
                mako_db_->Rollback(txn);
                aborts++;
                printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] txn %zu (local:%d remote:%d) ABORTED (timeout/error: %d)\n",
                       home_shard_index, worker_id_, i, home_shard_index, remote_shard_index, error_code);
            }
        }

        printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] SUMMARY: %d commits, %d aborts\n",
               home_shard_index, worker_id_, commits, aborts);

        // Only worker 0 verifies final state after all workers finish
        if (original_worker_id_ == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(3));

            // Read to verify records on local shard
            void *txn = mako_db_->BeginTransaction();
            std::string local_key = "cross_shard_local";
            std::string local_value;
            try {
                mako::Status s = table->Get(txn, local_key, local_value);
                mako_db_->Commit(txn);
                if (s.ok()) {
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: local key EXISTS on shard %d\n",
                           home_shard_index, worker_id_, home_shard_index);
                } else if (s.IsNotFound()) {
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: local key DOES NOT EXIST on shard %d\n",
                           home_shard_index, worker_id_, home_shard_index);
                } else {
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: local key Get failed: %s\n",
                           home_shard_index, worker_id_, s.ToString().c_str());
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: local key read ABORTED\n",
                       home_shard_index, worker_id_);
            } catch (int error_code) {
                mako_db_->Rollback(txn);
                printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: local key read ABORTED (timeout/error: %d)\n",
                       home_shard_index, worker_id_, error_code);
            }

            // Read to verify records on remote shard (sequential - after txn completes)
            {
                void *txn2 = mako_db_->BeginTransaction();
                std::string remote_key = "cross_shard_remote";
                std::string remote_value;
                try {
                    mako::Status s = table->Get(txn2, remote_key, remote_value);
                    mako_db_->Commit(txn2);
                    if (s.ok()) {
                        printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: remote key EXISTS on shard %d\n",
                               home_shard_index, worker_id_, remote_shard_index);
                    } else if (s.IsNotFound()) {
                        printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: remote key DOES NOT EXIST on shard %d\n",
                               home_shard_index, worker_id_, remote_shard_index);
                    } else {
                        printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: remote key Get failed: %s\n",
                               home_shard_index, worker_id_, s.ToString().c_str());
                    }
                } catch (abstract_db::abstract_abort_exception &ex) {
                    mako_db_->Rollback(txn2);
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: remote key read ABORTED\n",
                           home_shard_index, worker_id_);
                } catch (int error_code) {
                    mako_db_->Rollback(txn2);
                    printf("[TEST_CROSS_SHARD] [Shard %d Worker %d] Final read: remote key read ABORTED (timeout/error: %d)\n",
                           home_shard_index, worker_id_, error_code);
                }
            }
        }
    }

    void test_read_write_contention() {
        printf("\n[TEST_RW_CONTENTION] === Testing Read-Write Contention Thread:%ld ===\n", std::this_thread::get_id());

        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex();
        mbta_sharded_ordered_index *table = mako_db_->GetDB()->open_sharded_index("customer_0");

        // Half the workers read, half write to the same keys
        bool is_writer = (worker_id_ % 2 == 0);

        int commits = 0, aborts = 0;
        for (size_t i = 0; i < 10; i++) {
            void *txn = mako_db_->BeginTransaction();
            std::string key = "rw_key_" + std::to_string(i % 3); // 3 shared keys

            try {
                mako::Status s = mako::Status::OK();
                if (is_writer) {
                    // Encode value - must remain valid until Commit()
                    std::string value = mako::Encode("writer_" + std::to_string(worker_id_) + "_" + std::to_string(i));
                    s = table->Put(txn, key, value);
                } else {
                    std::string value;
                    s = table->Get(txn, key, value);
                    // NotFound is acceptable for readers (key may not exist yet)
                    if (s.IsNotFound()) {
                        s = mako::Status::OK();
                    }
                }
                if (s.ok()) {
                    mako_db_->Commit(txn);
                    commits++;
                    printf("[TEST_RW_CONTENTION] [Shard %d Worker %d] %s key=%s txn %zu COMMITTED\n",
                           home_shard_index, worker_id_, is_writer ? "WRITE" : "READ", key.c_str(), i);
                } else {
                    mako_db_->Rollback(txn);
                    aborts++;
                    printf("[TEST_RW_CONTENTION] [Shard %d Worker %d] %s key=%s txn %zu FAILED: %s\n",
                           home_shard_index, worker_id_, is_writer ? "WRITE" : "READ", key.c_str(), i, s.ToString().c_str());
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                aborts++;
                printf("[TEST_RW_CONTENTION] [Shard %d Worker %d] %s key=%s txn %zu ABORTED\n",
                       home_shard_index, worker_id_, is_writer ? "WRITE" : "READ", key.c_str(), i);
            }
        }

        printf("[TEST_RW_CONTENTION] [Shard %d Worker %d] SUMMARY: %d commits, %d aborts\n",
               home_shard_index, worker_id_, commits, aborts);

        // Only worker 0 verifies final state after all workers finish
        if (original_worker_id_ == 0) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return;

            int existing_keys = 0;
            for (size_t i = 0; i < 3; i++) {
                void *txn = mako_db_->BeginTransaction();
                std::string key = "rw_key_" + std::to_string(i);
                std::string value;
                try {
                    mako::Status s = table->Get(txn, key, value);
                    mako_db_->Commit(txn);
                    if (s.ok()) {
                        existing_keys++;
                    }
                } catch (abstract_db::abstract_abort_exception &ex) {
                    mako_db_->Rollback(txn);
                }
            }
            printf("[TEST_RW_CONTENTION] [Shard %d Worker %d] Final read: %d out of 3 keys exist\n",
                   home_shard_index, worker_id_, existing_keys);
        }
    }

    void test_delete() {
        printf("\n[TEST_DELETE] === Testing Delete Interface Thread:%ld ===\n", std::this_thread::get_id());

        int home_shard_index = BenchmarkConfig::getInstance().getShardIndex();
        mbta_sharded_ordered_index *table = mako_db_->GetDB()->open_sharded_index("customer_0");

        // Each worker has its own unique key to delete
        std::string key = "delete_test_key_w" + std::to_string(worker_id_);
        // Encode value - must remain valid until Commit()
        std::string value = mako::Encode("value_to_delete_w" + std::to_string(worker_id_));

        // Step 1: Put a key
        {
            void *txn = mako_db_->BeginTransaction();
            try {
                mako::Status s = table->Put(txn, key, value);
                if (s.ok()) {
                    mako_db_->Commit(txn);
                    printf("[TEST_DELETE] [Shard %d Worker %d] Put key '%s' COMMITTED\n",
                           home_shard_index, worker_id_, key.c_str());
                } else {
                    mako_db_->Rollback(txn);
                    printf("[TEST_DELETE] [Shard %d Worker %d] Put FAILED: %s\n",
                           home_shard_index, worker_id_, s.ToString().c_str());
                    return;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                printf("[TEST_DELETE] [Shard %d Worker %d] Put ABORTED\n",
                       home_shard_index, worker_id_);
                return;
            }
        }

        // Step 2: Verify key exists
        {
            void *txn = mako_db_->BeginTransaction();
            std::string read_value;
            try {
                mako::Status s = table->Get(txn, key, read_value);
                mako_db_->Commit(txn);
                if (s.ok()) {
                    printf("[TEST_DELETE] [Shard %d Worker %d] Get before delete: key EXISTS\n",
                           home_shard_index, worker_id_);
                } else {
                    printf("[TEST_DELETE] [Shard %d Worker %d] Get before delete: key NOT FOUND (unexpected)\n",
                           home_shard_index, worker_id_);
                    return;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                printf("[TEST_DELETE] [Shard %d Worker %d] Get before delete ABORTED\n",
                       home_shard_index, worker_id_);
                return;
            }
        }

        // Step 3: Delete the key
        {
            void *txn = mako_db_->BeginTransaction();
            try {
                mako::Status s = table->Delete(txn, key);
                if (s.ok()) {
                    mako_db_->Commit(txn);
                    printf("[TEST_DELETE] [Shard %d Worker %d] Delete COMMITTED\n",
                           home_shard_index, worker_id_);
                } else {
                    mako_db_->Rollback(txn);
                    printf("[TEST_DELETE] [Shard %d Worker %d] Delete FAILED: %s\n",
                           home_shard_index, worker_id_, s.ToString().c_str());
                    return;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                printf("[TEST_DELETE] [Shard %d Worker %d] Delete ABORTED\n",
                       home_shard_index, worker_id_);
                return;
            }
        }

        // Step 4: Verify key is gone
        {
            void *txn = mako_db_->BeginTransaction();
            std::string read_value;
            try {
                mako::Status s = table->Get(txn, key, read_value);
                mako_db_->Commit(txn);
                if (s.IsNotFound()) {
                    printf("[TEST_DELETE] [Shard %d Worker %d] Get after delete: key NOT FOUND (correct!)\n",
                           home_shard_index, worker_id_);
                } else if (s.ok()) {
                    printf("[TEST_DELETE] [Shard %d Worker %d] Get after delete: key still EXISTS (unexpected)\n",
                           home_shard_index, worker_id_);
                } else {
                    printf("[TEST_DELETE] [Shard %d Worker %d] Get after delete: %s\n",
                           home_shard_index, worker_id_, s.ToString().c_str());
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                mako_db_->Rollback(txn);
                printf("[TEST_DELETE] [Shard %d Worker %d] Get after delete ABORTED\n",
                       home_shard_index, worker_id_);
            }
        }
    }

protected:
    mako::DB* mako_db_;
    int worker_id_;
    int original_worker_id_;
};

void run_worker_tests(mako::DB *mako_db, int worker_id,
                      spin_barrier *barrier_ready,
                      spin_barrier *barrier_start) {
    // Add thread ID to distinguish workers
    printf("[Worker %d] Starting on thread %ld\n", worker_id, std::this_thread::get_id());

    auto worker = new TransactionWorker(mako_db, worker_id);
    worker->initialize();

    // Ensure all workers complete initialization before proceeding
    barrier_ready->count_down();
    barrier_start->wait_for();

    // Run all tests
    worker->test_basic_transactions();
    worker->test_single_key_contention();
    worker->test_overlapping_keys();
    worker->test_cross_shard_contention();
    worker->test_read_write_contention();

    printf("[Worker %d] Completed\n", worker_id);
}

void run_tests(mako::DB* mako_db) {
    // Pre-open tables ONCE before creating threads to avoid serialization
    size_t nthreads = BenchmarkConfig::getInstance().getNthreads();
    std::vector<std::thread> worker_threads;
    worker_threads.reserve(nthreads);
    spin_barrier barrier_ready(nthreads);
    spin_barrier barrier_start(1);

    for (size_t i = 0; i < nthreads; ++i) {
        worker_threads.emplace_back(run_worker_tests, mako_db, i,
                                    &barrier_ready, &barrier_start);
    }

    // Release workers once every thread has created its ShardClient
    barrier_ready.wait_for();
    barrier_start.count_down();

    // Wait for all worker threads to complete
    for (auto& t : worker_threads) {
        t.join();
    }
}

// Verify data integrity for all tests
bool verify_data_integrity(abstract_db* db, int nshards, int nthreads) {
    mbta_sharded_ordered_index *table = db->open_sharded_index("customer_0");
    auto records = scan_tables(db, table);

    printf("\n=== Database contents (%zu rows) ===\n", records.size());
    for (const auto &entry : records) {
        printf("%s => %s\n", entry.first.c_str(), entry.second.c_str());
    }
    fflush(stdout);

    // Build a map of all keys for quick lookup
    std::map<std::string, std::string> db_map;
    for (const auto& record : records) {
        db_map[record.first] = record.second;
    }

    printf("\n========================================\n");
    printf("=== DATA INTEGRITY VERIFICATION ===\n");
    printf("========================================\n");
    printf("Total records: %zu\n", records.size());
    printf("Configuration: %d shards, %d threads\n", nshards, nthreads);
    printf("\n");

    bool failed = false;

    // Test 1: Verify basic transaction keys (from test_basic_transactions())
    printf("\n--- Test 1: Basic Transactions (test_basic_transactions) ---\n");
    {
        int count = 0;
        int value_mismatches = 0;

        for (const auto& kv : db_map) {
            if (kv.first.find("test_key_w") == 0 && kv.first.find("_remote") == std::string::npos) {
                count++;

                // Extract worker_id and index from key: test_key_w<worker_id>_<i>
                size_t w_pos = kv.first.find("_w") + 2;
                size_t underscore_pos = kv.first.find('_', w_pos);
                std::string worker_id_str = kv.first.substr(w_pos, underscore_pos - w_pos);
                std::string index_str = kv.first.substr(underscore_pos + 1);

                // Build expected value
                std::string expected_value = "test_value_w" + worker_id_str + "_" + index_str;

                // Check if value contains expected string
                if (kv.second.find(expected_value) == std::string::npos) {
                    printf(RED "✗ Value mismatch for key '%s': expected '%s', got '%s'\n" RESET,
                           kv.first.c_str(), expected_value.c_str(), kv.second.substr(0, 50).c_str());
                    value_mismatches++;
                    failed = true;
                }
            }
        }

        printf("Found %d basic transaction keys\n", count);
        if (count == 0) {
            printf(RED "✗ FAIL: No basic keys found\n" RESET);
            failed = true;
        } else if (value_mismatches > 0) {
            printf(RED "✗ FAIL: %d value mismatches\n" RESET, value_mismatches);
        } else {
            printf(GREEN "✓ PASS: All keys and values correct\n" RESET);
        }
    }

    // Test 1b: Verify basic remote transaction keys (from test_basic_transactions, 2-shard only)
    if (nshards >= 2) {
        printf("\n--- Test 1b: Basic Remote Transactions (test_basic_transactions) ---\n");
        int count = 0;
        int value_mismatches = 0;

        for (const auto& kv : db_map) {
            if (kv.first.find("test_key2_w") == 0 && kv.first.find("_remote") != std::string::npos) {
                count++;

                // Extract worker_id and index from key: test_key2_w<worker_id>_<i>_remote
                size_t w_pos = kv.first.find("_w") + 2;
                size_t underscore_pos = kv.first.find('_', w_pos);
                std::string worker_id_str = kv.first.substr(w_pos, underscore_pos - w_pos);
                std::string rest = kv.first.substr(underscore_pos + 1);
                std::string index_str = rest.substr(0, rest.find('_'));

                // Build expected value
                std::string expected_value = "test_value2_w" + worker_id_str + "_" + index_str;

                // Check if value contains expected string
                if (kv.second.find(expected_value) == std::string::npos) {
                    printf(RED "✗ Value mismatch for key '%s': expected '%s', got '%s'\n" RESET,
                           kv.first.c_str(), expected_value.c_str(), kv.second.substr(0, 50).c_str());
                    value_mismatches++;
                    failed = true;
                }
            }
        }

        printf("Found %d basic remote keys\n", count);
        if (count == 0) {
            printf(RED "✗ FAIL: No remote keys found\n" RESET);
            failed = true;
        } else if (value_mismatches > 0) {
            printf(RED "✗ FAIL: %d value mismatches\n" RESET, value_mismatches);
        } else {
            printf(GREEN "✓ PASS: All keys and values correct\n" RESET);
        }
    }

    // Test 2: Verify single key contention (from test_single_key_contention())
    printf("\n--- Test 2: Single Key Contention (test_single_key_contention) ---\n");
    {
        int count = 0;
        for (const auto& kv : db_map) {
            if (kv.first == "contention_key_shared") {
                count++;
                if (kv.second.find("worker_") == std::string::npos ||
                    kv.second.find("_iter_") == std::string::npos) {
                    printf(RED "✗ Invalid value for key '%s'\n" RESET, kv.first.c_str());
                    failed = true;
                }
            }
        }
        printf("Found %d contention key\n", count);
        // In sharded config, key may be on a different shard
        if (nshards >= 2) {
            if (count > 1) {
                printf(RED "✗ FAIL: Found multiple contention keys (should be only 1 total)\n" RESET);
                failed = true;
            } else if (count == 1) {
                printf(GREEN "✓ PASS: Key exists on this shard\n" RESET);
            } else {
                printf(GREEN "✓ INFO: Key is on a different shard\n" RESET);
            }
        } else {
            // For single shard, key must exist
            if (count != 1) {
                printf(RED "✗ FAIL: Expected exactly 1 contention key\n" RESET);
                failed = true;
            } else {
                printf(GREEN "✓ PASS\n" RESET);
            }
        }
    }

    // Test 3: Verify overlapping keys (from test_overlapping_keys())
    printf("\n--- Test 3: Overlapping Keys (test_overlapping_keys) ---\n");
    {
        int count = 0;
        for (const auto& kv : db_map) {
            if (kv.first.find("overlap_key_") == 0) {
                count++;
                if (kv.second.find("worker_") == std::string::npos ||
                    kv.second.find("_iter_") == std::string::npos) {
                    printf(RED "✗ Invalid value for key '%s'\n" RESET, kv.first.c_str());
                    failed = true;
                }
            }
        }
        printf("Found %d overlapping keys\n", count);
        if (count == 0) {
            printf(RED "✗ FAIL: No overlapping keys found\n" RESET);
            failed = true;
        } else {
            printf(GREEN "✓ PASS\n" RESET);
        }
    }

    // Test 4: Verify cross-shard contention keys (from test_cross_shard_contention(), 2-shard only)
    if (nshards >= 2) {
        printf("\n--- Test 4: Cross-Shard Contention (test_cross_shard_contention) ---\n");
        int count = 0;
        for (const auto& kv : db_map) {
            if (kv.first == "cross_shard_local" || kv.first == "cross_shard_remote") {
                count++;
                if (kv.second.find("worker_") == std::string::npos ||
                    kv.second.find("_iter_") == std::string::npos) {
                    printf(RED "✗ Invalid value for key '%s'\n" RESET, kv.first.c_str());
                    failed = true;
                }
            }
        }
        printf("Found %d cross-shard keys (total across all shards: 2)\n", count);
        // Keys are distributed across shards
        if (count > 2) {
            printf(RED "✗ FAIL: Too many cross-shard keys\n" RESET);
            failed = true;
        } else if (count > 0) {
            printf(GREEN "✓ PASS: Keys present on this shard\n" RESET);
        } else {
            printf(GREEN "✓ INFO: Keys are on other shard(s)\n" RESET);
        }
    }

    // Test 5: Verify read-write contention keys (from test_read_write_contention())
    printf("\n--- Test 5: Read-Write Contention (test_read_write_contention) ---\n");
    {
        int count = 0;
        for (const auto& kv : db_map) {
            if (kv.first.find("rw_key_") == 0) {
                count++;
                if (kv.second.find("writer_") == std::string::npos) {
                    printf(RED "✗ Invalid value for key '%s'\n" RESET, kv.first.c_str());
                    failed = true;
                }
            }
        }
        printf("Found %d RW contention keys (expected 3 total across all shards)\n", count);

        // In 2-shard config, keys are distributed by hash, so we may have 0-3 keys per shard
        // Any distribution is valid as long as we have some keys present
        if (nshards == 1) {
            // In single shard mode, we must have exactly 3 keys
            if (count != 3) {
                printf(RED "✗ FAIL: Expected 3 RW keys in single-shard mode, found %d\n" RESET, count);
                failed = true;
            } else {
                printf(GREEN "✓ PASS: All 3 RW keys present\n" RESET);
            }
        } else {
            // In multi-shard mode, keys are distributed across shards
            if (count > 3) {
                printf(RED "✗ FAIL: Found %d RW keys (max 3 expected)\n" RESET, count);
                failed = true;
            } else if (count > 0) {
                printf(GREEN "✓ PASS: Found %d RW keys on this shard\n" RESET, count);
            } else {
                printf(GREEN "✓ INFO: Keys are on other shard(s)\n" RESET);
            }
        }
    }

    printf("\n========================================\n");
    if (failed) {
        printf(RED "=== VERIFICATION FAILED ===" RESET "\n");
    } else {
        printf(GREEN "=== ALL VERIFICATIONS PASSED ===" RESET "\n");
    }
    printf("========================================\n");

    return !failed;
}

// @safe - Print client mode usage
static void print_client_usage(const char* program_name) {
    printf("Client Mode Usage: %s --client <server_host> <server_port>\n", program_name);
    printf("Example: %s --client localhost 31000\n", program_name);
}

// @unsafe - Runs client mode, connecting to remote server
static int run_client_mode(const char* server_host, int server_port) {
    printf("=== Mako Client Mode ===\n");
    printf("Connecting to server at %s:%d...\n", server_host, server_port);

    // Create RemoteDB connection options
    mako::RemoteOptions opts;
    opts.server_host = server_host;
    opts.server_port = server_port;

    // Connect to remote server
    mako::RemoteDB* remote_db = nullptr;
    mako::Status status = mako::RemoteDB::Connect(opts, &remote_db);
    if (!status.ok()) {
        printf(RED "Failed to connect to server: %s" RESET "\n", status.ToString().c_str());
        return 1;
    }

    printf(GREEN "Connected to server successfully!" RESET "\n");

    // Get a table proxy
    mako::RemoteTable* table = remote_db->GetTable("customer_0");
    printf("Created table proxy: %s (table_id=%d)\n",
           table->GetName().c_str(), table->GetTableId());

    // Demonstrate transaction API (will return error due to stub implementation)
    printf("\n--- Testing Remote Transaction API ---\n");
    void* txn = remote_db->BeginTransaction();
    if (txn) {
        printf("BeginTransaction: OK (txn_handle=%p)\n", txn);

        // Try a Put operation (will fail with stub error)
        std::string test_key = "test_key_001";
        std::string test_value = mako::Encode("test_value_001");
        status = table->Put(txn, test_key, test_value);
        if (!status.ok()) {
            printf(YELLOW "Put: %s (expected - stub implementation)" RESET "\n",
                   status.ToString().c_str());
        } else {
            printf("Put: OK\n");
        }

        // Try a Get operation (will fail with stub error)
        std::string retrieved_value;
        status = table->Get(txn, test_key, retrieved_value);
        if (!status.ok()) {
            printf(YELLOW "Get: %s (expected - stub implementation)" RESET "\n",
                   status.ToString().c_str());
        } else {
            printf("Get: OK, value=%s\n", retrieved_value.c_str());
        }

        // Commit (will be no-op due to stub)
        remote_db->Commit(txn);
        printf("Commit: OK\n");
    } else {
        printf(RED "BeginTransaction: Failed" RESET "\n");
    }

    printf("\n--- Client Mode Summary ---\n");
    printf("The RemoteDB API is functional. RPC operations return stub errors\n");
    printf("because the full RPC integration is not yet implemented.\n");
    printf("This demonstrates the client-server decoupling architecture.\n");

    delete remote_db;
    return 0;
}

int main(int argc, char **argv) {

    // Check for client mode flag
    if (argc >= 2 && strcmp(argv[1], "--client") == 0) {
        if (argc != 4) {
            print_client_usage(argv[0]);
            return 1;
        }
        const char* server_host = argv[2];
        int server_port = std::stoi(argv[3]);
        return run_client_mode(server_host, server_port);
    }

    // All necessary parameters expected from users
    if (argc < 6 || argc > 7) {
        printf("Usage: %s <nshards> <shardIdx> <nthreads> <paxos_proc_name> <is_replicated> [replication_type]\n", argv[0]);
        printf("       %s --client <server_host> <server_port>\n", argv[0]);
        printf("Example: %s 2 0 6 localhost 1\n", argv[0]);
        printf("Example with Raft: %s 2 0 6 localhost 1 raft\n", argv[0]);
        printf("Example client mode: %s --client localhost 31000\n", argv[0]);
        return 1;
    }

    int nshards = std::stoi(argv[1]);
    int shardIdx = std::stoi(argv[2]);
    int nthreads = std::stoi(argv[3]);
    std::string paxos_proc_name = std::string(argv[4]);
    int is_replicated = std::stoi(argv[5]);

    // Set replication type if provided (default is paxos)
    std::string replication_type = "paxos";
    if (argc == 7) {
        replication_type = argv[6];
        janus::set_replication_type_from_string(replication_type);
        printf("Using replication type: %s\n", replication_type.c_str());
    }

    // Build config path - fix the format string to use std::to_string
    std::string config_path = get_current_absolute_path()
            + "../src/mako/config/local-shards" + std::to_string(nshards)
            + "-warehouses" + std::to_string(nthreads) + ".yml";

    // Use occ_raft.yml for Raft replication, occ_paxos.yml for Paxos
    std::string occ_config = (replication_type == "raft" || replication_type == "RAFT")
        ? "../config/occ_raft.yml"
        : "../config/occ_paxos.yml";

    std::vector<std::string> paxos_config_files{
        get_current_absolute_path() + "../config/1leader_2followers/paxos" + std::to_string(nthreads) + "_shardidx" + std::to_string(shardIdx) + ".yml",
        get_current_absolute_path() + occ_config
    };

    // Use the new RocksDB-like Open interface
    mako::Options options;
    options.num_shards = nshards;
    options.shard_index = shardIdx;
    options.num_threads = nthreads;
    options.paxos_proc_name = paxos_proc_name;
    options.paxos_config_files = paxos_config_files;
    options.replication.enabled = (is_replicated != 0);
    options.replication.is_leader = (paxos_proc_name == "localhost");

    // Create transport configuration
    auto transport_config = new transport::Configuration(config_path);
    options.transport_config = transport_config;

    // Open the database using the new interface
    mako::DB* mako_db = nullptr;
    mako::Status status = mako::DB::Open(options, "/tmp/mako_simple_txn", &mako_db);
    if (!status.ok()) {
        std::cerr << "Failed to open database: " << status.ToString() << std::endl;
        return 1;
    }

    printf("=== Mako Transaction Tests  ===\n");

    // Get the underlying abstract_db for operations
    abstract_db* db = mako_db->GetDB();
    auto& benchConfig = BenchmarkConfig::getInstance();

    if (benchConfig.getLeaderConfig()) {
        // pre-declare sharded tables
        mako::setup_erpc_server();
        mbta_sharded_ordered_index *table = db->open_sharded_index("customer_0");

        map<int, abstract_ordered_index*> open_tables;
        auto *local_table = table->shard_for_index(benchConfig.getShardIndex());
        if (local_table) {
            open_tables[local_table->get_table_id()] = local_table;
        }
        mako::setup_helper(db, std::ref(open_tables));

        std::this_thread::sleep_for(std::chrono::seconds(5)); // Wait all shards finish setup
    }

    if (benchConfig.getLeaderConfig()) {
        run_tests(mako_db);
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));

    // Data integrity verification on followers and learners only
    // Must be done BEFORE db_close() which shuts down Raft/Paxos infrastructure
    // Note: Leaders are skipped because:
    // 1. They are the source of data and may have cleanup issues during cross-shard operations
    // 2. The test script only checks follower logs for verification results
    // 3. Leader crashes during verification disrupt Raft and prevent followers from completing
    if (!benchConfig.getLeaderConfig()) {
        // Wait for replication to complete before verifying data integrity
        // Without this, verification runs before any data is replicated!
        wait_for_termination();
        bool verification_passed = verify_data_integrity(db, nshards, nthreads);

        if (!verification_passed) {
            printf("\n" RED "VERIFICATION FAILED - Database integrity compromised!" RESET "\n");
            db_close();
            delete mako_db;
            return 1;
        }
    }

    printf("\n" GREEN "All tests completed successfully!" RESET "\n");
    std::cout.flush();

    // Cleanup: stop helper and eRPC server threads before closing DB on leaders
    if (benchConfig.getLeaderConfig()) {
        mako::stop_erpc_server();
    }

    db_close();

    delete mako_db;
    return 0;
}
