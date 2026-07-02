#ifndef _LIB_SERVER_H_
#define _LIB_SERVER_H_

#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "lib/fasttransport.h"
#include "lib/timestamp.h"
#include "lib/common.h"
#include "benchmarks/abstract_db.h"
#include "benchmarks/abstract_ordered_index.h"
#include "lib/helper_queue.h"

void register_sync_util_ss(std::function<int()>);

namespace mako
{
    using namespace std;

    class ShardReceiver : TransportReceiver
    {
    public:
        ShardReceiver(std::string file);
        void Register(abstract_db *db,
                 const map<int, abstract_ordered_index *> &open_tables_table_id /*,
                 const map<string, vector<abstract_ordered_index *>> &partitions,
                 const map<string, vector<abstract_ordered_index *>> &remote_partitions*/);
        void UpdateTableEntry(int table_id, abstract_ordered_index *table);

        // Message handlers.
        size_t ReceiveRequest(uint8_t reqType, char *reqBuf, char *respBuf);

        void ReceiveResponse(uint8_t reqType, char *respBuf) override{}; // TODO: for now, replicas
                                                                         // do not need to communicate
                                                                         // with eachother; they will need
                                                                         // to for synchronization
        bool Blocked() override { return false; };
        // new handlers
        void HandleGetRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleScanRequest(char *reqBuf, char *respBuf, size_t &respLen);
        // Self-contained non-txn writes (docs/mako-nontxn-api-plan.md):
        // runs the op as a local one-op OCC txn via the L3 non-txn API
        // (put / insert / remove selected by reqType).
        void HandleNontxnWriteRequest(uint8_t reqType, char *reqBuf,
                                      char *respBuf, size_t &respLen);
        void HandleLockRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleBatchLockRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleValidateRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleGetTimestampRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleSerializeUtilRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleAbortRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleInstallRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleUnLockRequest(char *reqBuf, char *respBuf, size_t &respLen);

        void HandleGetMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleBatchLockMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleGetMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleBatchLockMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);

        // Client API handlers (for decoupled client-server mode)
        // @unsafe - handles raw buffer pointers from transport layer
        void HandleClientBeginTxnRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientCommitRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientRollbackRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientPutRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientGetRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientDeleteRequest(char *reqBuf, char *respBuf, size_t &respLen);

        // @safe - Get open tables mapping for client service
        const map<int, abstract_ordered_index *>& GetOpenTables() const {
            return open_tables_table_id;
        }

        // @safe - Get database reference
        abstract_db* GetDb() const { return db; }

        // ============================================================================
        // Client Transaction API (for MakoClientService to use)
        // ============================================================================

        /**
         * Begin a client transaction
         * @param client_id Client identifier
         * @param txn_counter Per-client transaction counter (for unique txn_id)
         * @return Generated txn_id = (client_id << 32) | txn_counter
         */
        // @safe - Thread-safe with mutex
        uint64_t BeginClientTransaction(uint64_t client_id, uint32_t txn_counter);

        /**
         * Commit a client transaction
         * @param txn_id Transaction ID from BeginClientTransaction
         * @return ErrorCode::SUCCESS if found and removed, ERROR if not found
         */
        // @safe - Thread-safe with mutex
        int CommitClientTransaction(uint64_t txn_id);

        /**
         * Rollback/abort a client transaction
         * @param txn_id Transaction ID from BeginClientTransaction
         * @return ErrorCode::SUCCESS if found and aborted, ERROR if not found
         */
        // @safe - Thread-safe with mutex
        int RollbackClientTransaction(uint64_t txn_id);

    protected:
        inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

    private:
        transport::Configuration config;

        // std::vector<uint64_t> latency_get;
        // std::vector<uint64_t> latency_prepare;
        // std::vector<uint64_t> latency_commit;

        // store layer
        abstract_db *db;
        map<int, abstract_ordered_index *> open_tables_table_id;
        // map<string, vector<abstract_ordered_index *>> partitions;
        // map<string, vector<abstract_ordered_index *>> remote_partitions;

        uint64_t txn_flags = 0;
        std::string txn_obj_buf;
        str_arena arena;

        string obj_key0;
        string obj_key1;
        string obj_v;

        int current_term ;

        // Client API: Transaction state management
        // Maps client txn_id -> transaction counter (for generating unique server txn IDs)
        // @safe - Protected by client_txn_mutex_
        std::unordered_map<uint64_t, uint64_t> client_transactions_;
        std::mutex client_txn_mutex_;
        std::atomic<uint64_t> server_txn_counter_{0};
    };

    class ShardServer
    {
    public:
        ShardServer(std::string file, int clientShardIndex, int shardIndex, int par_id);
        void Register(abstract_db *db,
                 mako::HelperQueue *queue,
                 mako::HelperQueue *queue_res,
                 const map<int, abstract_ordered_index *> &open_tables /*,
                 const map<string, vector<abstract_ordered_index *>> &partitions,
                 const map<string, vector<abstract_ordered_index *>> &remote_partitions*/);
        void UpdateTable(int table_id, abstract_ordered_index *table);
        void Run();

        // Get the underlying ShardReceiver (for ClientTcpServer integration)
        // @safe - Returns borrowed pointer
        ShardReceiver* GetReceiver() { return shardReceiver; }

    protected:
        transport::Configuration config;
        mako::ShardReceiver *shardReceiver;
        // create a shard-server on {clientShardIndex} to receive a client request from 
        //  a TPC-C worker thread <shardIndex, par-id>
        int clientShardIndex;
        int serverShardIndex;
        int par_id;

        // store layer
        abstract_db *db;
        mako::HelperQueue *queue;
        mako::HelperQueue *queue_response;
        map<int, abstract_ordered_index *> open_tables_table_id;
        // map<string, vector<abstract_ordered_index *>> partitions;
        // map<string, vector<abstract_ordered_index *>> remote_partitions;
    };
}
#endif
