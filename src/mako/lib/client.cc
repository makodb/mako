#include <iostream>
#include <random>
#include "lib/fasttransport.h"
#include "lib/client.h"
#include "lib/common.h"
#include "benchmarks/sto/Interface.hh"

std::function<int()> sc_callback_ = nullptr;
void register_sync_util_sc(std::function<int()> cb) {
    sc_callback_ = cb;
}

/**
 * IMPORTANT: one transaction has to be handle by the same partition-id thread on all shards
 */
namespace srolis
{
    using namespace std;

    Client::Client(std::string file,
                   Transport *transport,
                   uint64_t clientid) : config(file),
                                        lastReqId(0),
                                        transport(transport)
    {
        this->clientid = clientid;
        // Randomly generate a client ID
        while (this->clientid == 0)
        {
            std::random_device rd;
            std::mt19937_64 gen(rd());
            std::uniform_int_distribution<uint64_t> dis;
            this->clientid = dis(gen);
            Debug("start a Client ID: %lu", this->clientid);
        }
        current_term = 0;
    }

    void Client::ReceiveResponse(uint8_t reqType, char *respBuf)
    {
        Debug("[%lu] received response, type: %d", clientid, reqType);
        switch (reqType)
        {
        case getReqType:
            HandleGetReply(respBuf);
            break;
        case scanReqType:
            HandleScanReply(respBuf);
            break;
        case lockReqType:
            HandleLockReply(respBuf);
            break;
        case validateReqType:
            HandleValidateReply(respBuf);
            break;
        case getTimestampReqType:
            HandleGetTimestamp(respBuf);
            break;
        case serializeUtilReqType:
            HandleSerializeUtil(respBuf);
            break; 
        case installReqType:
            HandleInstallReply(respBuf);
            break;
        case unLockReqType:
            HandleUnLockReply(respBuf);
            break;
        case abortReqType:
            HandleAbortReply(respBuf);
            break;
        case batchLockReqType:
            HandleBatchLockReply(respBuf);
            break;
        case watermarkReqType:
            HandleWatermarkReply(respBuf);
            break;
        case controlReqType:
            HandleControlReply(respBuf);
            break;
        case warmupReqType:
            HandleWarmupReply(respBuf);
            break;
        default:
            Warning("Unrecognized request type: %d\n", reqType);
        }
    }

    // there are two purposes:
    //  1. validate in the commit phase
    //  2. attache watermark in the result
    void Client::InvokeValidate(uint64_t txn_nr,
                                int dstShardIdx,
                                uint16_t server_id,
                                resp_continuation_t continuation,
                                error_continuation_t error_continuation,
                                uint32_t timeout)
    {
        Debug("invoke InvokeValidate\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeValidate",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<basic_request_t *>(
            transport->GetRequestBuf(
                sizeof(basic_request_t),
                sizeof(get_int_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        blocked = true;
        transport->SendRequestToAll(this,
                                      validateReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      sizeof(basic_response_t),
                                      sizeof(basic_request_t));
    }

    void Client::InvokeGetTimestamp(uint64_t txn_nr,
                                int dstShardIdx,
                                uint16_t server_id,
                                resp_continuation_t continuation,
                                error_continuation_t error_continuation,
                                uint32_t timeout)
    {
        Debug("invoke InvokeGetTimestamp\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeGetTimestamp",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<basic_request_t *>(
            transport->GetRequestBuf(
                sizeof(basic_request_t),
                sizeof(get_int_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        blocked = true;
        transport->SendRequestToAll(this,
                                      getTimestampReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      sizeof(get_int_response_t),
                                      sizeof(basic_request_t));
    }

    void Client::InvokeControl(uint64_t txn_nr,
                               int control,
                               uint64_t value,
                               int dstShardIdx,
                               uint16_t server_id,
                               resp_continuation_t continuation,
                               error_continuation_t error_continuation,
                               uint32_t timeout)
    {
        Debug("invoke InvokeControl\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        bool is_datacenter_failure = control >= 4;

        crtReqK =
            PendingRequestK("InvokeControl",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<control_request_t*>(
            transport->GetRequestBuf(
                sizeof(control_request_t),
                sizeof(get_int_response_t)));
        reqBuf->targert_server_id = server_id;
        if (is_datacenter_failure){
            reqBuf->targert_server_id = 10000; // reserved for the datacenter failure
        }
        reqBuf->req_nr = reqId + current_term;
        reqBuf->control = control;
        reqBuf->value = value;
        blocked = true;
        transport->SendRequestToAll(this,
                                    controlReqType,
                                    dstShardIdx,
                                    is_datacenter_failure? config.warehouses+1: 
                                        config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                    sizeof(get_int_response_t),
                                    sizeof(control_request_t)); 
    }

    void Client::InvokeWarmup(uint64_t txn_nr,
                               uint32_t req_val,
                               uint8_t centerId,
                               int dstShardIdx,
                               uint16_t server_id,
                               resp_continuation_t continuation,
                               error_continuation_t error_continuation,
                               uint32_t timeout)
    {
       Debug("invoke InvokeWarmup\n");
       uint32_t reqId = ++lastReqId;
       reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeWarmup",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<warmup_request_t*>(
            transport->GetRequestBuf(
                sizeof(warmup_request_t),
                sizeof(get_int_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_val = req_val;
        reqBuf->req_nr = reqId + current_term;
        blocked = true;
        transport->SendRequestToAll(this,
                                    warmupReqType,
                                    dstShardIdx,
                                    config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                    sizeof(get_int_response_t),
                                    sizeof(warmup_request_t),
                                    centerId); 
    }


    void Client::InvokeExchangeWatermark(uint64_t txn_nr,
                                int dstShardIdx,
                                uint16_t server_id,
                                resp_continuation_t continuation,
                                error_continuation_t error_continuation,
                                uint32_t timeout)
    {
        Debug("invoke InvokeExchangeWatermark\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeExchangeWatermark",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<basic_request_t *>(
            transport->GetRequestBuf(
                sizeof(basic_request_t),
                sizeof(get_int_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        blocked = true;
        transport->SendRequestToAll(this,
                                    watermarkReqType,
                                    dstShardIdx,
                                    config.warehouses+1,
                                    sizeof(get_int_response_t),
                                    sizeof(basic_request_t));
    }

    void Client::InvokeInstall(uint64_t txn_nr,
                               int dstShardIdx,
                               uint16_t server_id,
                               char*cc,
                               resp_continuation_t continuation,
                               error_continuation_t error_continuation,
                               uint32_t timeout)
    {
        Debug("invoke InvokeInstall\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeInstall",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<vector_int_request_t *>(
            transport->GetRequestBuf(
                sizeof(vector_int_request_t),
                sizeof(basic_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        reqBuf->len = sizeof(uint32_t)*config.nshards;
        memcpy(reqBuf->value, cc, sizeof(uint32_t)*config.nshards);
        blocked = true;
        size_t bytes_used = sizeof(vector_int_request_t) - max_vector_int_length + sizeof(uint64_t)*config.nshards;
        transport->SendRequestToAll(this,
                                      installReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      sizeof(basic_response_t),
                                      bytes_used);
    }

    void Client::InvokeSerializeUtil(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            char*cc,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout) {
        Debug("invoke InvokeSerializeUtil\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeSerializeUtil",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<vector_int_request_t *>(
            transport->GetRequestBuf(
                sizeof(vector_int_request_t),
                sizeof(basic_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        reqBuf->len = sizeof(uint64_t)*config.nshards;
        memcpy(reqBuf->value, cc, sizeof(uint64_t)*config.nshards);
        blocked = true;
        size_t bytes_used = sizeof(vector_int_request_t) - max_vector_int_length + sizeof(uint64_t)*config.nshards;

        transport->SendRequestToAll(this,
                                      serializeUtilReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      sizeof(basic_response_t),
                                      bytes_used);

    }

    void Client::InvokeUnLock(uint64_t txn_nr,
                              int dstShardIdx,
                              uint16_t server_id,
                              resp_continuation_t continuation,
                              error_continuation_t error_continuation,
                              uint32_t timeout)
    {
        Debug("invoke InvokeUnLock\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeUnLock",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<basic_request_t *>(
            transport->GetRequestBuf(
                sizeof(basic_request_t),
                sizeof(basic_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        blocked = true;
        transport->SendRequestToAll(this,
                                      unLockReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      sizeof(basic_response_t),
                                      sizeof(basic_request_t));
    }

    void Client::InvokeBatchLock(
        uint64_t txn_nr,
        uint16_t server_id,
        map<int, BatchLockRequestWrapper> &request_batch_per_shard,
        resp_continuation_t continuation,
        error_continuation_t error_continuation,
        uint32_t timeout
    ) {
        Debug("invoke InvokeBatchLock\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("batchLock",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        map<int, pair<char*, size_t>> data_to_send;

        for (auto &it: request_batch_per_shard) {
            it.second.set_req_nr(reqId + current_term);
            data_to_send[it.first] = make_pair((char*)it.second.get_request_ptr(), it.second.get_msg_len());
        }
        blocked = true;

        //reqBuf->targert_server_id = server_id;
        transport->SendBatchRequestToAll(
            this,
            batchLockReqType,
            config.warehouses+5+server_id%TThread::get_num_erpc_server(),
            sizeof(basic_response_t),
            data_to_send
        );
    }

    void Client::InvokeLock(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            const string &key,
                            const string &value,
                            uint16_t table_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout)
    {
        Debug("invoke InvokeLock\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK(key,
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        // TODO: find a way to get sending errors (the eRPC's enqueue_request
        // function does not return errors)
        auto *reqBuf = reinterpret_cast<lock_request_t *>(
            transport->GetRequestBuf(
                sizeof(lock_request_t),
                sizeof(basic_response_t)));
        reqBuf->targert_server_id = server_id;

        reqBuf->req_nr = reqId + current_term;
        reqBuf->klen = key.size();
        memcpy(reqBuf->key_and_value, key.c_str(), key.size());
        reqBuf->vlen = value.size();
        ASSERT_LT(value.size()+key.size()-1, max_value_length);
        memcpy(reqBuf->key_and_value+key.size(), value.c_str(), value.size());
        reqBuf->table_id = table_id;
        blocked = true;

        size_t used_bytes = sizeof(lock_request_t) - max_key_length - max_value_length + key.size() + value.size();
        transport->SendRequestToShard(this,
                                      lockReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      used_bytes);
    }

    void Client::InvokeScan(uint64_t txn_nr,
                           int dstShardIdx,
                           uint16_t server_id,
                           const string &start_key,
                           const string &end_key,
                           uint16_t table_id,
                           resp_continuation_t continuation,
                           error_continuation_t error_continuation,
                           uint32_t timeout)
    {
        Debug("invoke InvokeScan: %d\n", server_id);
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK(start_key,
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        // TODO: find a way to get sending errors (the eRPC's enqueue_request
        // function does not return errors)
        auto *reqBuf = reinterpret_cast<scan_request_t *>(
            transport->GetRequestBuf(
                sizeof(scan_request_t),
                sizeof(scan_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        reqBuf->slen = start_key.size();
        memcpy(reqBuf->start_end_key, start_key.c_str(), start_key.size());
        reqBuf->elen = end_key.size();
        memcpy(reqBuf->start_end_key+start_key.size(), end_key.c_str(), end_key.size());
        reqBuf->table_id = table_id;
        ASSERT_LT(end_key.size()+start_key.size()-1, 128);

        blocked = true;
        transport->SendRequestToShard(this,
                                      scanReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      sizeof(scan_request_t));
    }

    void Client::InvokeGet(uint64_t txn_nr,
                           int dstShardIdx,
                           uint16_t server_id,
                           const string &key,
                           uint16_t table_id,
                           resp_continuation_t continuation,
                           error_continuation_t error_continuation,
                           uint32_t timeout)
    {
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        //Warning("invoke InvokeGet,txn_nr:%d,par_id:%d,id:%d",txn_nr,TThread::getPartitionID(),TThread::id());
        Debug("invoke InvokeGet\n");
        crtReqK =
            PendingRequestK(key,
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        // TODO: find a way to get sending errors (the eRPC's enqueue_request
        // function does not return errors)

        // We intialize epoch number once a remoteGET is called as we have it for every transaction.
#if defined(FAIL_NEW_VERSION)
        current_term = sc_callback_();
#endif
        auto *reqBuf = reinterpret_cast<get_request_t *>(
            transport->GetRequestBuf(
                sizeof(get_request_t),
                sizeof(get_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        reqBuf->len = key.size();
        ASSERT_LT(key.size(), max_key_length);

        memcpy(reqBuf->key, key.c_str(), key.size());
        reqBuf->table_id = table_id;

        size_t bytes_used = sizeof(get_request_t) - max_key_length + reqBuf->len;

        blocked = true;
        transport->SendRequestToShard(this,
                                      getReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                      bytes_used);
    }

    void Client::InvokeAbort(uint64_t txn_nr,
                             int dstShardIdx,
                             uint16_t server_id,
                             basic_continuation_t continuation,
                             error_continuation_t error_continuation,
                             uint32_t timeout)
    {
        Debug("invoke InvokeAbort\n");
        uint32_t reqId = ++lastReqId;
        reqId *= 10;

        crtReqK =
            PendingRequestK("InvokeAbort",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<basic_request_t *>(
            transport->GetRequestBuf(
                sizeof(basic_request_t),
                sizeof(basic_response_t)));
        reqBuf->targert_server_id = server_id;
        reqBuf->req_nr = reqId + current_term;
        blocked = true;
        try {
            transport->SendRequestToAll(this,
                                        abortReqType,
                                        dstShardIdx,
                                        config.warehouses+5+server_id%TThread::get_num_erpc_server(),
                                        sizeof(basic_response_t),
                                        sizeof(basic_request_t));
        } catch(int n) {
            // for abort itself, we do nothing
        }

    }

    void Client::HandleGetReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<get_response_t *>(respBuf);
        Debug("[%lu]Received get HandleGetReply, to eRPC client receives a message, req_nr: %d, crt: %d, status: %d\n", clientid, resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            Debug("Received get reply when no request was pending; req_nr = %lu, crtReqK:%lu,par_id:%d,id:%d", resp->req_nr, crtReqK.req_nr,TThread::getPartitionID(),TThread::id());
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleScanReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<scan_response_t *>(respBuf);
        Debug("[%lu]Received get HandleGetReply, to eRPC client receives a message, req_nr: %d, crt: %d, status: %d\n", clientid, resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received get reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleLockReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        Debug("eRPC client receives a HandleLockReply, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received lock reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleBatchLockReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        Debug("eRPC client receives a HandleLockReply, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received lock reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        Debug("[%lu] Received lock reply", clientid);

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleValidateReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        Debug("eRPC client receives a HandleValidateReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received validate reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }
    
    void Client::HandleWatermarkReply(char *respBuf) {
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        Debug("eRPC client receives a HandleWatermarkReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received watermark reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleControlReply(char *respBuf) {
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        Debug("eRPC client receives a HandleControlReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received control reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleWarmupReply(char *respBuf) {
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        Debug("eRPC client receives a HandleWarmupReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received warmup reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
        }
    }

    void Client::HandleGetTimestamp(char *respBuf)
    {
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        Debug("eRPC client receives a HandleGetTimestamp, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received get timestamp reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleSerializeUtil(char *respBuf)
    {
        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        Debug("eRPC client receives a HandleSerializeUtil, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received get timestamp reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleInstallReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        Debug("eRPC client receives a message, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received install reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleUnLockReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        Debug("eRPC client receives a message, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received unlock reply when no request was pending; req_nr = %lu", resp->req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleAbortReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        Debug("eRPC client receives a message, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
        if (resp->req_nr != crtReqK.req_nr)
        {
            //Warning("Received abort reply when no request was pending; req_nr = %lu, crtReqK:%lu", resp->req_nr, crtReqK.req_nr);
            return;
        }

        // invoke application callback
        crtReqK.resp_continuation(respBuf);
        // remove from pending list
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }
}
