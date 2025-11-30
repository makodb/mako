
#ifndef _LIB_SHARDCLIENT_H_
#define _LIB_SHARDCLIENT_H_

#include "lib/fasttransport.h"
#include "lib/client.h"
#include "lib/promise.h"
#include "lib/common.h"

namespace mako
{
    using namespace std;

    class ShardClient
    {
    public:
        ShardClient(std::string file, string cluster, int shardIndex, int par_id);
        int remoteGet(int remote_table_id, std::string key, std::string &value);
        int remoteScan(int remote_table_id, std::string start_key, std::string end_key, std::string &value);
        // Single timestamp interfaces
        int remoteGetTimestamp(uint32_t &timestamp);
        int remoteExchangeWatermark(uint32_t &watermark, uint64_t set_bits);
        int remoteControl(int control, uint32_t value, uint32_t &ret_value, uint64_t set_bits);
        int remoteAbort();
        int remoteLock(int remote_table_id, std::string key, std::string &value);
        int remoteBatchLock(vector<int> &remote_table_id_batch, vector<string> &key_batch, vector<string> &value_batch);
        int remoteValidate(uint32_t &watermark);
        int remoteInstall(uint32_t timestamp);
        int remoteUnLock();
        int warmupRequest(uint32_t req_val, uint8_t centerId, uint32_t &ret_value, uint64_t set_bits);
        int remoteInvokeSerializeUtil(uint32_t timestamp);
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
        mako::Client *client;
        int shardIndex;
        std::string cluster;
        int clusterRole;
        int par_id;  // in mako, each worker thread has a partition
        Promise *waiting; // waiting thread
        int tid;

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

    };

}

#endif
