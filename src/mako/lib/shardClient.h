
#ifndef _LIB_SHARDCLIENT_H_
#define _LIB_SHARDCLIENT_H_

#include "lib/fasttransport.h"
#include "lib/client.h"
#include "lib/promise.h"
#include "lib/common.h"

namespace srolis
{
    using namespace std;

    class ShardClient
    {
    public:
        ShardClient(std::string file, string cluster, int shardIndex, int par_id, int workload_type);
        int remoteGet(int dummy_table_id, std::string key, std::string &value);
        int remoteScan(int dummy_table_id, std::string start_key, std::string end_key, std::string &value);
        int remoteGetTimestamp(std::vector<uint32_t> &vectorT);
        int remoteExchangeWatermark(std::vector<uint32_t> &vectorW, uint64_t set_bits);
        int remoteControl(int control, uint32_t value, std::vector<uint32_t> &ret_values, uint64_t set_bits);
        int remoteAbort();
        int remoteLock(int dummy_table_id, std::string key, std::string &value);
        int remoteBatchLock(vector<int> &dummy_table_id_batch, vector<string> &key_batch, vector<string> &value_batch);
        int remoteValidate(std::vector<uint32_t> &watermark_v);
        int remoteInstall(std::vector<uint32_t> vectorT);
        int remoteUnLock();
        int warmupRequest(uint32_t req_val, uint8_t centerId, std::vector<uint32_t> &ret_values, uint64_t set_bits);
        int remoteInvokeSerializeUtil(std::vector<uint32_t> vectorT);
        void statistics();
        void stop();
        void setBreakTimeout(bool);
        void setBlocking(bool);
        bool getBreakTimeout();
        bool isBreakTimeout;
        bool isBlocking;
        bool stopped;
    protected:
        transport::Configuration config;
        Transport *transport;
        srolis::Client *client;
        int shardIndex;
        std::string cluster;
        int clusterRole;
        int par_id;  // in srolis, each worker thread has a partition
        Promise *waiting; // waiting thread
        int tid;
        int workload_type;  // 0. simpleShards (debug), 1. tpcc

        int num_response_waiting;
        vector<int> status_received;
        vector<uint64_t> int_received; // indexed by shard

        /* Callbacks for hearing back from a shard for an operation. */
        void GetCallback(char *respBuf);
        void ScanCallback(char *respBuf);
        void BasicCallBack(char *respBuf);

        /* Timeout which only go to one shard. */
        void GiveUpTimeout();
        // void VectorIntCallback(char *respBuf);

        void SendToAllStatusCallBack(char *respBuf);
        void SendToAllIntCallBack(char *respBuf);
        void SendToAllGiveUpTimeout();
        bool is_all_response_ok();
        void calculate_num_response_waiting(int shards_to_send_bits);
        void calculate_num_response_waiting_no_skip(int shards_to_send_bits);

        /* utilities */
        int *convert_dummy_table_id_tpcc(int dummy_table_id);
        int *convert_dummy_table_id_debug(int dummy_table_id);
    };

}

#endif