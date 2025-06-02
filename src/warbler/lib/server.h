#ifndef _LIB_SERVER_H_
#define _LIB_SERVER_H_

#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include "lib/fasttransport.h"
#include "lib/timestamp.h"
#include "lib/common.h"
#include "benchmarks/abstract_db.h"
#include "benchmarks/abstract_ordered_index.h"
#include "lib/helper_queue.h"

void register_sync_util_ss(std::function<int()>);

namespace srolis
{
    using namespace std;

    class ShardReceiver : TransportReceiver
    {
    public:
        ShardReceiver(std::string file);
        void Register(abstract_db *db,
                 const map<string, abstract_ordered_index *> &open_tables,
                 const map<int, abstract_ordered_index *> &open_tables_table_id,
                 const map<string, vector<abstract_ordered_index *>> &partitions,
                 const map<string, vector<abstract_ordered_index *>> &dummy_partitions);

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

    protected:
        inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

    private:
        transport::Configuration config;

        // std::vector<uint64_t> latency_get;
        // std::vector<uint64_t> latency_prepare;
        // std::vector<uint64_t> latency_commit;

        // store layer
        abstract_db *db;
        map<string, abstract_ordered_index *> open_tables;
        map<int, abstract_ordered_index *> open_tables_table_id;
        map<string, vector<abstract_ordered_index *>> partitions;
        map<string, vector<abstract_ordered_index *>> dummy_partitions;

        uint64_t txn_flags = 0;
        std::string txn_obj_buf;
        str_arena arena;

        string obj_key0;
        string obj_key1;
        string obj_v;

        int current_term ;
    };

    class ShardServer
    {
    public:
        ShardServer(std::string file, int clientShardIndex, int shardIndex, int par_id);
        void Register(abstract_db *db,
                 srolis::HelperQueue *queue,
                 srolis::HelperQueue *queue_res,
                 const map<string, abstract_ordered_index *> &open_tables,
                 const map<string, vector<abstract_ordered_index *>> &partitions,
                 const map<string, vector<abstract_ordered_index *>> &dummy_partitions);
        void Run();

    protected:
        transport::Configuration config;
        srolis::ShardReceiver *shardReceiver;
        // create a shard-server on {clientShardIndex} to receive a client request from 
        //  a TPC-C worker thread <shardIndex, par-id>
        int clientShardIndex;
        int serverShardIndex;
        int par_id;

        // store layer
        abstract_db *db;
        srolis::HelperQueue *queue;
        srolis::HelperQueue *queue_response;
        map<string, abstract_ordered_index *> open_tables;
        map<int, abstract_ordered_index *> open_tables_table_id;
        map<string, vector<abstract_ordered_index *>> partitions;
        map<string, vector<abstract_ordered_index *>> dummy_partitions;
    };
}
#endif