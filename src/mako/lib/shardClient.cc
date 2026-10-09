
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "lib/fasttransport.h"
#include "lib/promise.h"
#include "lib/client.h"
#include "lib/shardClient.h"
#include "lib/configuration.h"
#include "lib/common.h"
import cluster;   // config/sharding metadata module (was #include "cluster/...")
#include "sto/Interface.hh"

import std;

namespace mako
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
                             int par_id, bool ephemeral_client)
        : config(file), cluster(cluster), shardIndex(shardIndex), par_id(par_id)
    {
        waiting = nullptr;
        num_response_waiting = 0;
        clusterRole = mako::convertCluster(cluster);
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
                                      id, ephemeral_client);

        // 1. initialize Client
        client = new mako::Client(config.configFile,
                                    transport,
                                    0); // 0 => generate a random client-id

        tid=0;
        int_received.resize(TThread::get_nshards());
        stopped = false;
        isBreakTimeout = false;
        isBlocking = true; // If there is a timeout, we can't abort it, we should retry it util it is successful.
    }

    // @unsafe - sole ownership of the two legacy allocations from the constructor.
    ShardClient::~ShardClient() {
        stop();
        delete transport;
        delete client;
    }

    // @unsafe - forwards one opaque logical request; ingress owns retry/dedup.
    int ShardClient::forwardNontxn(const ShardingRequest& request, uint8_t kind,
                                  uint16_t legacy_table, const std::string& key,
                                  const std::string& value, bool* result,
                                  std::string* output) {
        if (request.grant.owner >= static_cast<uint32_t>(config.nshards)
            || kind < nontxnPutReqType || kind > nontxnGetReqType)
            return ErrorCode::ERROR;
        Promise promise(GET_TIMEOUT);
        waiting = &promise;
        client->SetNumResponseWaiting(1);
        const uint16_t server_id = shardIndex * config.warehouses + par_id;
        client->InvokeNontxnWrite(++tid, request.grant.owner, server_id,
            key, value, legacy_table, request, kind,
            bind(&ShardClient::NontxnWriteCallback, this, placeholders::_1),
            bind(&ShardClient::GiveUpTimeout, this), promise.GetTimeout());
        const std::string reply = promise.GetValue();
        const int status = promise.GetReply();
        if (output) *output = reply;
        if (result) *result = kind == nontxnGetReqType
            ? status == ErrorCode::SUCCESS : reply.size() == 1 && reply[0] != 0;
        return status;
    }

    void ShardClient::stop() {
        if (stopped) {
            return;
        }
        stopped = true;
        auto *ftport = static_cast<FastTransport *>(transport);
        ftport->Stop();
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
        auto *resp = reinterpret_cast<mako::get_response_t *>(respBuf);
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
        auto *resp = reinterpret_cast<mako::scan_response_t *>(respBuf);
        if (waiting != NULL) {
            Promise *w = waiting;
            waiting = NULL;
            w->Reply(resp->status, std::string(resp->value, resp->len));
        } else {
            Debug("Waiting is null!");
        }
    }

    // @unsafe - lengths are checked before exposing opaque bytes to Rust.
    void ShardClient::FullScanCallback(char* respBuf) {
        const auto* response = reinterpret_cast<const full_scan_response_t*>(respBuf);
        if (!waiting) return;
        Promise* promise = waiting;
        waiting = nullptr;
        if (response->length > full_scan_page_capacity) {
            promise->Reply(ErrorCode::ERROR);
            return;
        }
        promise->Reply(response->status,
            std::string(reinterpret_cast<const char*>(response->payload),
                        response->length));
    }

    void ShardClient::BasicCallBack(char *respBuf) {
        /* Replies back from a shard. */
        auto *resp = reinterpret_cast<mako::basic_response_t *>(respBuf);
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
        auto *resp = reinterpret_cast<mako::basic_response_t *>(respBuf);
        status_received.push_back((int) resp->status);
    }

    void ShardClient::SendToAllIntCallBack(char *respBuf) {
        auto *resp = reinterpret_cast<mako::get_int_response_t *>(respBuf);
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
        bool ok = status_received.size() == static_cast<size_t>(num_response_waiting);
        for (auto code: status_received) ok &= (code == ErrorCode::SUCCESS);
        status_received.clear();
        for (int i=0;i<(int)int_received.size(); i++)
            int_received[i] = 0;
        return ok ? ErrorCode::SUCCESS : ErrorCode::ERROR;
    }

    void ShardClient::calculate_num_response_waiting(int shards_to_send_bits) {
        num_response_waiting = 0;
        for (int dstShardIndex = 0; dstShardIndex < config.nshards; dstShardIndex++) {
            if (dstShardIndex == shardIndex) continue;
            if ((shards_to_send_bits >> dstShardIndex) % 2 == 0) continue;
            num_response_waiting ++;
        }
        client->SetNumResponseWaiting(num_response_waiting);
    }

    // without skipping
    void ShardClient::calculate_num_response_waiting_no_skip(int shards_to_send_bits) {
        num_response_waiting = 0;
        for (int dstShardIndex = 0; dstShardIndex < config.nshards; dstShardIndex++) {
            if ((shards_to_send_bits >> dstShardIndex) % 2 == 0) continue;
            num_response_waiting ++;
        }
        client->SetNumResponseWaiting(num_response_waiting);
    }


    int ShardClient::remoteScan(int remote_table_id, std::string start_key, std::string end_key, std::string &value) {

        int table_id = remote_table_id;
        // Use policy-based routing if available, otherwise fall back to table-ID-based
        ShardingRequest request{};
        int dstShardIndex = sharding_route_request(table_id, start_key, request);
        if (dstShardIndex < 0) return ErrorCode::ABORT;

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

    // @unsafe - captured canonical address and full grant survive every page.
    int ShardClient::fullScanPage(int table_id, const ShardingRequest& request,
                                 const uint8_t* payload, size_t length,
                                 std::string& response) {
        const uint32_t destination = request.grant.owner;
        if (destination >= static_cast<uint32_t>(config.nshards)
            || destination >= sizeof(TThread::readset_shard_bits) * 8
            || length > full_scan_request_capacity) return ErrorCode::ERROR;
        // Even an ambiguous timeout must be terminally cleaned up. Do not add
        // this shard to trans_nosend_abort merely because a reply was lost.
        TThread::readset_shard_bits |= (1u << destination);
        sharding_use_outgoing_request(request);
        Promise promise(GET_TIMEOUT);
        waiting = &promise;
        client->SetNumResponseWaiting(1);
        try {
            client->InvokeFullScanPage(++tid, destination,
                shardIndex * config.warehouses + par_id, table_id, request,
                payload, length,
                [this](char* reply) { FullScanCallback(reply); },
                [this](const std::string&, ErrorCode) { GiveUpTimeout(); });
            response = promise.GetValue();
            return promise.GetReply();
        } catch (...) {
            waiting = nullptr;
            return ErrorCode::TIMEOUT;
        }
    }

    void ShardClient::statistics() {
        //Warning("Info for current shardClient, shardIdx: %d, cluster: %s, par_id: %d", shardIndex, cluster.c_str(), par_id);
        transport->Statistics();
    }

    int ShardClient::remoteGet(int remote_table_id, std::string key, std::string &value) {

        int table_id = remote_table_id;
        // Use policy-based routing if available, otherwise fall back to table-ID-based
        ShardingRequest request{};
        int dstShardIndex = sharding_route_request(table_id, key, request);
        if (dstShardIndex < 0) return ErrorCode::ABORT;

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
        //Warning("remoteGET: key:%s,table_id:%d,key_len:%d",mako::printStringAsBit(key).c_str(),table_id,key.length());
        value = promise.GetValue();
        int ret = promise.GetReply();
        if (ret > 0 && ret != ErrorCode::NOT_FOUND) {
            TThread::trans_nosend_abort |= (1 << dstShardIndex);
        }
        return ret;
    }

    // Reply parser for the non-txn write ops: status + the op's
    // boolean result in value[0] (vlen==1) per the wire contract in
    // common.h.
    void ShardClient::NontxnWriteCallback(char *respBuf) {
        auto *resp = reinterpret_cast<mako::client_kv_response_t *>(respBuf);
        if (waiting != NULL) {
            Promise *w = waiting;
            waiting = NULL;
            w->Reply(resp->status,
                     std::string(resp->value, resp->vlen));
        } else {
            Debug("Waiting is null!");
        }
    }

    // @unsafe - shared body; blocks on a Promise like remoteGet
    int ShardClient::nontxnWrite(uint8_t reqType, int remote_table_id,
                                 const std::string &key,
                                 const std::string &value,
                                 bool *op_result) {
        int table_id = remote_table_id;
        ShardingRequest request{};
        int dstShardIndex = sharding_route_request(table_id, key, request);
        if (dstShardIndex < 0) return ErrorCode::ERROR;

        Promise promise(GET_TIMEOUT);
        waiting = &promise;

        client->SetNumResponseWaiting(1);

        const int timeout = promise.GetTimeout();
        uint16_t server_id = shardIndex*config.warehouses+par_id;

        client->InvokeNontxnWrite(++tid,
                    dstShardIndex,
                    server_id,
                    key,
                    value,
                    table_id,
                    request,
                    reqType,
                    bind(&ShardClient::NontxnWriteCallback, this,
                        placeholders::_1),
                    bind(&ShardClient::GiveUpTimeout, this),
                timeout);

        std::string result_byte = promise.GetValue();
        int ret = promise.GetReply();
        if (op_result != nullptr) {
            *op_result = (result_byte.size() == 1) && (result_byte[0] != 0);
        }
        return ret;
    }

    // Self-contained non-txn read: unlike remoteGet (getReqType), the
    // server stages nothing in its participant txn, so no follow-up
    // abort/commit is owed and no shard tracking bits are set here.
    // Returns SUCCESS with the raw stored bytes in `value`, or ABORT
    // when the key is absent.
    // @unsafe - blocks on a Promise like remoteGet
    int ShardClient::nontxnGet(int remote_table_id, const std::string &key,
                               std::string &value) {
        int table_id = remote_table_id;
        ShardingRequest request{};
        int dstShardIndex = sharding_route_request(table_id, key, request);
        if (dstShardIndex < 0) return ErrorCode::ERROR;

        Promise promise(GET_TIMEOUT);
        waiting = &promise;

        client->SetNumResponseWaiting(1);

        const int timeout = promise.GetTimeout();
        uint16_t server_id = shardIndex*config.warehouses+par_id;

        client->InvokeNontxnWrite(++tid,
                    dstShardIndex,
                    server_id,
                    key,
                    std::string(),
                    table_id,
                    request,
                    mako::nontxnGetReqType,
                    bind(&ShardClient::NontxnWriteCallback, this,
                        placeholders::_1),
                    bind(&ShardClient::GiveUpTimeout, this),
                timeout);

        value = promise.GetValue();
        return promise.GetReply();
    }

    int ShardClient::nontxnPut(int remote_table_id, const std::string &key,
                               const std::string &value, bool *op_result) {
        return nontxnWrite(mako::nontxnPutReqType, remote_table_id, key, value, op_result);
    }

    int ShardClient::nontxnInsert(int remote_table_id, const std::string &key,
                                  const std::string &value, bool *op_result) {
        return nontxnWrite(mako::nontxnInsertReqType, remote_table_id, key, value, op_result);
    }

    int ShardClient::nontxnRemove(int remote_table_id, const std::string &key,
                                  bool *op_result) {
        return nontxnWrite(mako::nontxnRemoveReqType, remote_table_id, key,
                           std::string(), op_result);
    }

    int ShardClient::remoteBatchLock(
        rusty::Vec<int>& remote_table_id_batch,
        rusty::Vec<string>& key_batch,
        rusty::Vec<string>& value_batch,
        rusty::Vec<uint8_t>& operation_batch
    ) {
        if (remote_table_id_batch.is_empty())
            return ErrorCode::SUCCESS;

        map<int, BatchLockRequestWrapper> request_batch_per_shard;
        uint16_t server_id = shardIndex * config.warehouses + par_id;
        int shards_to_send_bits = 0;
        for (size_t i = 0; i < remote_table_id_batch.len(); i++) {
            int remote_table_id = remote_table_id_batch[i];
            int table_id = remote_table_id;
            // Use policy-based routing if available, otherwise fall back to table-ID-based
            ShardingRequest request{};
            int dst_shard_idx = sharding_route_request(table_id, key_batch[i], request);
            if (dst_shard_idx < 0) return ErrorCode::ABORT;

            // after combine remoteLock + remoteValidate, this step might need to be skipped
            TThread::writeset_shard_bits |= (1 << dst_shard_idx) ;
            
            shards_to_send_bits |= (1 << dst_shard_idx);
            request_batch_per_shard[dst_shard_idx].add_request(
                key_batch[i], value_batch[i], table_id, server_id, request, operation_batch[i]);
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

    int ShardClient::remoteLock(int remote_table_id, std::string key, std::string &value) {
        Panic("Deprecated!");

        int table_id = remote_table_id;
        // Use policy-based routing if available, otherwise fall back to table-ID-based
        ShardingRequest request{};
        int dstShardIndex = sharding_route_request(table_id, key, request);
        if (dstShardIndex < 0) return ErrorCode::ABORT;
        
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

    int ShardClient::remoteValidate(uint32_t &watermark) {
        int shards_to_send_bits = TThread::writeset_shard_bits | TThread::readset_shard_bits;
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
        // Single timestamp system: use maximum watermark from all shards
        watermark = 0;
        for (int i=0; i<(int)int_received.size(); i++) {
            if (int_received[i] > watermark) {
                watermark = int_received[i];
            }
        }
        return is_all_response_ok();
    }

    // @unsafe - an irrevocable native decision retains identity until every
    // participant acknowledges actual engine completion; retries replay receipts.
    int ShardClient::remoteInstall(uint32_t timestamp) {
        const int shards = TThread::writeset_shard_bits | TThread::readset_shard_bits;
        if (!shards) return ErrorCode::SUCCESS;
        char encoded[sizeof(timestamp)];
        std::memcpy(encoded, &timestamp, sizeof(timestamp));
        const uint16_t server_id = shardIndex * config.warehouses + par_id;
        while (true) {
            calculate_num_response_waiting(shards);
            try {
                client->InvokeInstall(++tid, shards, server_id, encoded,
                    bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                    bind(&ShardClient::SendToAllGiveUpTimeout, this), BASIC_TIMEOUT);
            } catch (int) {
                if (!sharding_leases_enabled()) throw;
                status_received.clear();
                usleep(1000);
                continue;
            }
            const int result = is_all_response_ok();
            if (result == ErrorCode::SUCCESS || !sharding_leases_enabled()) return result;
            usleep(1000);
        }
    }

    int ShardClient::warmupRequest(uint32_t req_val, uint8_t centerId, uint32_t &ret_value, uint64_t set_bits) {
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
        ret_value = 0;
        for (int i=0; i<(int)int_received.size(); i++) {
            ret_value += int_received[i];
        }
        return is_all_response_ok();
    }

    int ShardClient::checkRemoteShardReady(int dstShardIndex) {
        // Use warmup mechanism to ping a specific remote shard
        // If the shard responds, it's ready; otherwise timeout/error

        return mako::ErrorCode::SUCCESS;

        // TO FIX: a server is ready on other shards, but this warmup rpc is frequently TIMEOUT!
        /*
        uint32_t ret_value = 0;
        uint64_t set_bits = (1ULL << dstShardIndex);  // Target only this shard
        uint8_t centerId = clusterRole;  // Use our cluster role

        // Use a shorter timeout for readiness check (1 second)
        calculate_num_response_waiting_no_skip(set_bits);
        uint16_t server_id = 0;  // Readiness check doesn't need specific server

        for (int i=0; i<(int)int_received.size(); i++) int_received[i]=0;
        try {
            client->InvokeWarmup(++tid,
                                0,  // req_val = 0 for readiness check
                                centerId,
                                set_bits,
                                server_id,
                                bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                                bind(&ShardClient::SendToAllGiveUpTimeout, this),
                                1000);  // 1 second timeout for readiness check
        } catch (int n) {
            Warning("Timeout on InvokeWarmup with error-no:%d!", n);
            return mako::ErrorCode::TIMEOUT;
        }
        return is_all_response_ok(); */
    }

    int ShardClient::remoteControl(int control, uint32_t value, uint32_t &ret_value, uint64_t set_bits) {
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
        ret_value = 0;
        for (int i=0; i<(int)int_received.size(); i++) {
            ret_value += int_received[i];
        }
        return is_all_response_ok(); 
    }

    int ShardClient::remoteExchangeWatermark(uint32_t &watermark, uint64_t set_bits) {
        calculate_num_response_waiting(set_bits);
        uint16_t server_id = 0; // to locate which helper_queue, does not matter

        for (int i=0;i<int_received.size();i++) int_received[i]=0;
        client->InvokeExchangeWatermark(++tid,  // txn_nr
                            set_bits,
                            server_id,
                            bind(&ShardClient::SendToAllIntCallBack, this, placeholders::_1),
                            bind(&ShardClient::SendToAllGiveUpTimeout, this),
                            BASIC_TIMEOUT);
        // Single timestamp system: use maximum watermark from all shards
        watermark = 0;
        for (int i=0; i<(int)int_received.size(); i++) {
            if (int_received[i] > watermark) {
                watermark = int_received[i];
            }
        }
        return is_all_response_ok();
    }

    // @unsafe - unlock is a terminal operation for read and write participants.
    int ShardClient::remoteUnLock() {
        const int shards = TThread::writeset_shard_bits | TThread::readset_shard_bits;
        if (!shards) return ErrorCode::SUCCESS;
        const uint16_t server_id = shardIndex * config.warehouses + par_id;
        while (true) {
            calculate_num_response_waiting(shards);
            try {
                client->InvokeUnLock(++tid, shards, server_id,
                    bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                    bind(&ShardClient::SendToAllGiveUpTimeout, this), BASIC_TIMEOUT);
            } catch (int) {
                if (!sharding_leases_enabled()) throw;
                status_received.clear();
                usleep(1000);
                continue;
            }
            const int result = is_all_response_ok();
            if (result == ErrorCode::SUCCESS || !sharding_leases_enabled()) return result;
            usleep(1000);
        }
    }

    int ShardClient::remoteGetTimestamp(uint32_t &timestamp) {
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
        // Single timestamp system: use maximum timestamp from all shards
        timestamp = 0;
        for (int i=0; i<(int)int_received.size(); i++) {
            if (int_received[i] > timestamp) {
                timestamp = int_received[i];
            }
        }
        return is_all_response_ok();
    }

    int ShardClient::remoteInvokeSerializeUtil(uint32_t timestamp) {
        // Single timestamp encoding - no vector needed
        char *cc = encode_single_timestamp(timestamp);
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

    // @unsafe - a lost read reply can still own an engine lease. Native abort
    // therefore contacts every touched shard, including uncertain/rejected reads.
    int ShardClient::remoteAbort() {
        int shards = TThread::writeset_shard_bits | TThread::readset_shard_bits;
        if (!sharding_leases_enabled())
            shards &= ~TThread::trans_nosend_abort;
        if (!shards) return ErrorCode::SUCCESS;
        const uint16_t server_id = shardIndex * config.warehouses + par_id;
        while (true) {
            calculate_num_response_waiting(shards);
            try {
                client->InvokeAbort(++tid, shards, server_id,
                    bind(&ShardClient::SendToAllStatusCallBack, this, placeholders::_1),
                    bind(&ShardClient::SendToAllGiveUpTimeout, this), ABORT_TIMEOUT);
            } catch (int) {
                if (!sharding_leases_enabled()) throw;
                status_received.clear();
                usleep(1000);
                continue;
            }
            const int result = is_all_response_ok();
            if (result == ErrorCode::SUCCESS || !sharding_leases_enabled()) return result;
            usleep(1000);
        }
    }
}
