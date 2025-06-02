
#include <iostream>
#include "lib/fasttransport.h"
#include "lib/promise.h"
#include "lib/client.h"
#include "lib/shardClient.h"
#include "lib/configuration.h"
#include "lib/common.h"
#include "benchmarks/sto/Interface.hh"

namespace srolis
{
    using namespace std;

    /**
     * file: configuration fileName
     * shardIndex: at which shard the running client locates
     * par_id: to distinguish the running thread
     */
    ShardClient::ShardClient(std::string file,
                             std::string cluster,
                             int shardIndex,
                             int par_id,
                             int workload_type) : config(file), cluster(cluster), shardIndex(shardIndex), par_id(par_id), workload_type(workload_type)
    {
        clusterRole = srolis::convertCluster(cluster);
        std::string local_uri = config.shard(shardIndex, clusterRole).host;
        int id=par_id;
        // 0. initialize transport
        transport = new FastTransport(file,
                                      local_uri, // local_uri
                                      cluster,
                                      1, 0,       // nr_req_types (for client, setup to 0)
                                      0,       // physPort
                                      0, // shardIndex % 2 // numa node
                                      shardIndex,
                                      id);

        // 1. initialize Client
        client = new srolis::Client(config.configFile,
                                    transport,
                                    0); // 0 => generate a random client-id

        tid=0;
        int_received.resize(TThread::get_nshards());
        stopped = false;
        isBreakTimeout = false;
        isBlocking = true; // If there is a timeout, we can't abort it, we should retry it util it is successful.
    }

    void ShardClient::stop() {
        FastTransport *ftport= (FastTransport *)transport;
        ftport->Stop();
        stopped = true;
    }

    void ShardClient::setBreakTimeout(bool bt=false) {
        FastTransport *ftport= (FastTransport *)transport;
        ftport->setBreakTimeout(bt);
        isBreakTimeout=bt;
    }

    void ShardClient::setBlocking(bool pd=false) {
        isBlocking=pd;
    }

    bool ShardClient::getBreakTimeout() {
        return isBreakTimeout;
    }

    void ShardClient::GetCallback(char *respBuf) {
        /* Replies back from a shard. */
        auto *resp = reinterpret_cast<srolis::get_response_t *>(respBuf);
        if (waiting != NULL) {
            Promise *w = waiting;
            waiting = NULL;
            w->Reply(resp->status, std::string(resp->value, resp->len));
        } else {
            Debug("Waiting is null!");
        }
    }

    void ShardClient::ScanCallback(char *respBuf) {
        /* Replies back from a shard. */
        auto *resp = reinterpret_cast<srolis::scan_response_t *>(respBuf);
        if (waiting != NULL) {
            Promise *w = waiting;
            waiting = NULL;
            w->Reply(resp->status, std::string(resp->value, resp->len));
        } else {
            Debug("Waiting is null!");
        }
    }

    void ShardClient::BasicCallBack(char *respBuf) {
        /* Replies back from a shard. */
        auto *resp = reinterpret_cast<srolis::basic_response_t *>(respBuf);
        if (waiting != NULL) {
            Promise *w = waiting;
            waiting = NULL;
            w->Reply(resp->status);
        } else {
            Debug("Waiting is null!");
        }
    }

    void ShardClient::GiveUpTimeout() {
        Debug("GiveupTimeout called.");
        if (waiting != nullptr) {
            Promise *w = waiting;
            waiting = nullptr;
            w->Reply(ErrorCode::TIMEOUT);
        }
    }

    void ShardClient::SendToAllStatusCallBack(char *respBuf) {
        auto *resp = reinterpret_cast<srolis::basic_response_t *>(respBuf);
        status_received.push_back((int) resp->status);
    }

    void ShardClient::SendToAllIntCallBack(char *respBuf) {
        auto *resp = reinterpret_cast<srolis::get_int_response_t *>(respBuf);
        status_received.push_back((int) resp->status);
        if (resp->shard_index>=TThread::get_nshards()||resp->shard_index<0){
            Warning("In SendToAllIntCallBack, the shard_idx is overflow: %d", resp->shard_index);
        }else{
            int_received[resp->shard_index] = resp->result;
        }
    }

    void ShardClient::SendToAllGiveUpTimeout() {
        status_received.push_back((int) ErrorCode::TIMEOUT);
    }

    bool ShardClient::is_all_response_ok() {
        bool ok = true;
        for (auto code: status_received) ok &= (code == ErrorCode::SUCCESS);
        status_received.clear();
        for (int i=0;i<(int)int_received.size(); i++)
            int_received[i] = 0;
        return ok ? ErrorCode::SUCCESS : ErrorCode::ERROR;
    }

    void ShardClient::calculate_num_response_waiting(int shards_to_send_bits) {
        int num_response_waiting = 0;
        for (int dstShardIndex = 0; dstShardIndex < config.nshards; dstShardIndex++) {
            if (dstShardIndex == shardIndex) continue;
            if ((shards_to_send_bits >> dstShardIndex) % 2 == 0) continue;
            num_response_waiting ++;
        }
        client->SetNumResponseWaiting(num_response_waiting);
    }

    // without skipping
    void ShardClient::calculate_num_response_waiting_no_skip(int shards_to_send_bits) {
        int num_response_waiting = 0;
        for (int dstShardIndex = 0; dstShardIndex < config.nshards; dstShardIndex++) {
            if ((shards_to_send_bits >> dstShardIndex) % 2 == 0) continue;
            num_response_waiting ++;
        }
        client->SetNumResponseWaiting(num_response_waiting);
    }

    int* ShardClient::convert_dummy_table_id_debug(int dummy_table_id) {
        static int r[2]; // (table_id, dst_shard_id)
        if (dummy_table_id==0) { // for debugging
            r[0] = 0;
            r[1] = 1;
            return r;
        }

        /**
         *  simpleShards.cc
         *  table-name: checking, table_id: 1
            table-name: saving, table_id: 2
            table-name: dummy_0, table_id: 3  => checking
            table-name: dummy_1, table_id: 4  => saving
        */
        if (dummy_table_id == 3) {
            r[0] = 1; // table_id
            r[1] = 1; // dstShardIndex
        } else {
            r[0] = 2; // table_id
            r[1] = 1; // dstShardIndex
        }
        return r;
    }

    int* ShardClient::convert_dummy_table_id_tpcc(int dummy_table_id) {
        static int r[2]; // (table_id, dst_shard_id)
        int total_num_warehouse = TThread::get_nshards() * TThread::get_warehouses();
        int num_warehouses = TThread::get_warehouses();
        int nshards = TThread::get_nshards();
        int g_wid = (dummy_table_id - (11 * num_warehouses + 1) - 1) % total_num_warehouse + 1;

        int table_id=0;
        if (dummy_table_id > (11 * num_warehouses + 1) + (nshards * num_warehouses * 11) ) {  // item
            table_id = 11 * num_warehouses + 1;
        } else {
            table_id = num_warehouses * ((dummy_table_id - (11 * num_warehouses + 1) - 1) / total_num_warehouse)
            + (dummy_table_id - (11 * num_warehouses + 1) - 1) % num_warehouses + 1;
        }

        int dstShardIndex = (g_wid-1)/num_warehouses;

        r[0] = table_id;
        r[1] = dstShardIndex;
        // Warning("###here,nshards:%d,nwarehouses:%d,dstShardId:%d,dummy_table_id:%d",
        //                 nshards,num_warehouses,dstShardIndex,dummy_table_id);
        return r;
    }

    int ShardClient::remoteScan(int dummy_table_id, std::string start_key, std::string end_key, std::string &value) {
        int *p;
        if (likely(workload_type == 1))
            p = convert_dummy_table_id_tpcc(dummy_table_id);
        else
            p = convert_dummy_table_id_debug(dummy_table_id);
        int table_id = *(p);
        int dstShardIndex = *(p+1);
        TThread::readset_shard_bits |= (1 << dstShardIndex);
        Promise promise(GET_TIMEOUT);
        waiting = &promise;

        const int timeout = promise.GetTimeout();
        uint16_t server_id = shardIndex*config.warehouses+par_id;

        client->SetNumResponseWaiting(1);

        client->InvokeScan(++tid,  // txn_nr
                    dstShardIndex,  // shardIdx
                    server_id,
                    start_key, 
                    end_key,
                    table_id,
                    bind(&ShardClient::ScanCallback, this,
                        placeholders::_1),
                    bind(&ShardClient::GiveUpTimeout, this),
                timeout);
        value = promise.GetValue();
        int ret = promise.GetReply();
        if (ret>0){
            TThread::trans_nosend_abort |= (1 << dstShardIndex);
        }
        return ret;
    }

    void ShardClient::statistics() {
        //Warning("Info for current shardClient, shardIdx: %d, cluster: %s, par_id: %d", shardIndex, cluster.c_str(), par_id);
        transport->Statistics();
    }

    int ShardClient::remoteGet(int dummy_table_id, std::string key, std::string &value) {
        int *p;
        if (likely(workload_type) == 1)
            p = convert_dummy_table_id_tpcc(dummy_table_id);
        else
            p = convert_dummy_table_id_debug(dummy_table_id);
        int table_id = *(p);
        int dstShardIndex = *(p+1);
        TThread::readset_shard_bits |= (1 << dstShardIndex) ;
        Promise promise(GET_TIMEOUT);
        waiting = &promise;

        client->SetNumResponseWaiting(1);

        const int timeout = promise.GetTimeout();
        uint16_t server_id = shardIndex*config.warehouses+par_id;

        client->InvokeGet(++tid,  // txn_nr
                    dstShardIndex,  // shardIdx
                    server_id,
                    key, 
                    table_id,
                    bind(&ShardClient::GetCallback, this,
                        placeholders::_1),
                    bind(&ShardClient::GiveUpTimeout, this),
                timeout);
        //Warning("remoteGET: key:%s,table_id:%d,key_len:%d",srolis::getStringAsBit(key).c_str(),table_id,key.length());
        value = promise.GetValue();
        int ret = promise.GetReply();
        if (ret>0){
            TThread::trans_nosend_abort |= (1 << dstShardIndex);
        }
        return ret;
    }

    int ShardClient::remoteBatchLock(
        vector<int> &dummy_table_id_batch,
        vector<string> &key_batch,
        vector<string> &value_batch
    ) {
        if (dummy_table_id_batch.empty())
            return ErrorCode::SUCCESS;

        map<int, BatchLockRequestWrapper> request_batch_per_shard;
        uint16_t server_id = shardIndex * config.warehouses + par_id;
        int shards_to_send_bits = 0;
        for (int i = 0; i < dummy_table_id_batch.size(); i++) {
            int *p;
            int dummy_table_id = dummy_table_id_batch[i];
            if (likely(workload_type) == 1)
                p = convert_dummy_table_id_tpcc(dummy_table_id);
            else
                p = convert_dummy_table_id_debug(dummy_table_id);
            int table_id = *(p);
            int dst_shard_idx = *(p + 1);

            // after combine remoteLock + remoteValidate, this step might need to be skipped
            TThread::writeset_shard_bits |= (1 << dst_shard_idx) ;
            
            shards_to_send_bits |= (1 << dst_shard_idx);
            request_batch_per_shard[dst_shard_idx].add_request(key_batch[i], value_batch[i], table_id, server_id);
        }

        Promise promise(BASIC_TIMEOUT);
        waiting = &promise;
        
        const int timeout = promise.GetTimeout();
        calculate_num_response_waiting(shards_to_send_bits);
        client->InvokeBatchLock(
            ++tid,
            server_id,
            request_batch_per_shard,
            bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
            bind(&ShardClient::SendToAllGiveUpTimeout, this),
            timeout
        );

        return is_all_response_ok();
    }

    int ShardClient::remoteLock(int dummy_table_id, std::string key, std::string &value) {
        Panic("Deprecated!");
        int *p;
        if (likely(workload_type) == 1)
            p = convert_dummy_table_id_tpcc(dummy_table_id);
        else
            p = convert_dummy_table_id_debug(dummy_table_id);
        int table_id = *(p);
        int dstShardIndex = *(p+1);
        TThread::writeset_shard_bits |= (1 << dstShardIndex) ;
        Promise promise(BASIC_TIMEOUT);
        waiting = &promise;

        client->SetNumResponseWaiting(1);

        const int timeout = promise.GetTimeout();
        uint16_t server_id = shardIndex*config.warehouses+par_id;

        client->InvokeLock(++tid,  // txn_nr
                    dstShardIndex,  // shardIdx
                    server_id,
                    key,
                    value,
                    table_id,
                    bind(&ShardClient::BasicCallBack, this,
                        placeholders::_1),
                    bind(&ShardClient::GiveUpTimeout, this),
                timeout);
        return promise.GetReply();
    }

    int ShardClient::remoteValidate(std::vector<uint32_t> &watermark_v) {
        int shards_to_send_bits = TThread::writeset_shard_bits;
        if (!shards_to_send_bits) return ErrorCode::SUCCESS;
        calculate_num_response_waiting(shards_to_send_bits);
        uint16_t server_id = shardIndex * config.warehouses + par_id;

        for (int i=0;i<int_received.size();i++) int_received[i]=0;
        client->InvokeValidate(++tid,  // txn_nr
                                shards_to_send_bits,
                                server_id,
                                bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                                bind(&ShardClient::SendToAllGiveUpTimeout, this),
                                BASIC_TIMEOUT);
        for (int i=0; i<(int)int_received.size(); i++)
            watermark_v[i] = int_received[i];
        return is_all_response_ok();
    }

    int ShardClient::remoteInstall(std::vector<uint32_t> vectorT) {
        char *cc=encode_vec_uint32(vectorT, config.nshards);
        int shards_to_send_bits = TThread::writeset_shard_bits;
        if (!shards_to_send_bits) return ErrorCode::SUCCESS;
        calculate_num_response_waiting(shards_to_send_bits);
        uint16_t server_id = shardIndex * config.warehouses + par_id;

        client->InvokeInstall(++tid,  // txn_nr
                            shards_to_send_bits,
                            server_id,
                            cc,
                            bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        free(cc);
        return is_all_response_ok();
    }


    int ShardClient::warmupRequest(uint32_t req_val, uint8_t centerId, std::vector<uint32_t> &ret_values, uint64_t set_bits) {
        calculate_num_response_waiting_no_skip(set_bits);
        uint16_t server_id = req_val; // we don't forward to a helper queue;

        for (int i=0;i<int_received.size();i++) int_received[i]=0;
        client->InvokeWarmup(++tid,  // txn_nr
                            req_val,
                            centerId,
                            set_bits,
                            server_id,
                            bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        for (int i=0; i<(int)int_received.size(); i++)
            ret_values[i] = int_received[i];
        return is_all_response_ok(); 
    }

    int ShardClient::remoteControl(int control, uint32_t value, std::vector<uint32_t> &ret_values, uint64_t set_bits) {
        calculate_num_response_waiting_no_skip(set_bits);
        uint16_t server_id = 0; // to locate which helper_queue

        for (int i=0;i<int_received.size();i++) int_received[i]=0;
        client->InvokeControl(++tid,  // txn_nr
                            control,
                            value,
                            set_bits,
                            server_id,
                            bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        for (int i=0; i<(int)int_received.size(); i++)
            ret_values[i] = int_received[i];
        return is_all_response_ok(); 
    }

    int ShardClient::remoteExchangeWatermark(std::vector<uint32_t> &vectorW, uint64_t set_bits) {
        calculate_num_response_waiting(set_bits);
        uint16_t server_id = 0; // to locate which helper_queue, does not matter

        for (int i=0;i<int_received.size();i++) int_received[i]=0;
        client->InvokeExchangeWatermark(++tid,  // txn_nr
                            set_bits,
                            server_id,
                            bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        for (int i=0; i<(int)int_received.size(); i++)
            vectorW[i] = int_received[i];
        return is_all_response_ok();
    }

    int ShardClient::remoteUnLock() {
        int shards_to_send_bits = TThread::writeset_shard_bits;
        if (!shards_to_send_bits) return ErrorCode::SUCCESS;
        calculate_num_response_waiting(shards_to_send_bits);
        uint16_t server_id = shardIndex * config.warehouses + par_id;

        client->InvokeUnLock(++tid,  // txn_nr
                            shards_to_send_bits,
                            server_id,
                            bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        return is_all_response_ok();
    }

    int ShardClient::remoteGetTimestamp(std::vector<uint32_t> &vectorT) {
        int shards_to_send_bits = TThread::writeset_shard_bits;
        if (!shards_to_send_bits) return ErrorCode::SUCCESS;
        calculate_num_response_waiting(shards_to_send_bits);
        uint16_t server_id = shardIndex * config.warehouses + par_id;

        for (int i=0;i<int_received.size();i++) int_received[i]=0;
        client->InvokeGetTimestamp(++tid,  // txn_nr
                            shards_to_send_bits,
                            server_id,
                            bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        for (int i=0; i<(int)int_received.size(); i++)
            vectorT[i] = int_received[i];
        return is_all_response_ok();
    }

    int ShardClient::remoteInvokeSerializeUtil(std::vector<uint32_t> vectorT) {
        char *cc=encode_vec_uint32(vectorT, config.nshards);
        int shards_to_send_bits = TThread::writeset_shard_bits;
        if (!shards_to_send_bits) return ErrorCode::SUCCESS;
        calculate_num_response_waiting(shards_to_send_bits);
        uint16_t server_id = shardIndex * config.warehouses + par_id;

        client->InvokeSerializeUtil(++tid,  // txn_nr
                            shards_to_send_bits,
                            server_id,
                            cc,
                            bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        free(cc);
        return is_all_response_ok();
    }

    int ShardClient::remoteAbort() {
        int shards_to_send_bits = TThread::writeset_shard_bits | TThread::readset_shard_bits;
        if (TThread::trans_nosend_abort > 0){
            shards_to_send_bits = shards_to_send_bits ^ TThread::trans_nosend_abort;
        }
        if (!shards_to_send_bits) return ErrorCode::SUCCESS;
        calculate_num_response_waiting(shards_to_send_bits);
        uint16_t server_id = shardIndex * config.warehouses + par_id;

        client->InvokeAbort(++tid,  // txn_nr
                            shards_to_send_bits,
                            server_id,
                            bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            ABORT_TIMEOUT);
        return is_all_response_ok();
    }
}
