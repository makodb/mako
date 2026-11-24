#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdexcept>
#include <rusty/cell.hpp>

#include "lib/fasttransport.h"
#include "lib/client.h"
#include "lib/common.h"
#include "runtime/worker_context.hh"

import std;

std::function<int()> sc_callback_ = nullptr;
void register_sync_util_sc(std::function<int()> cb) {
    sc_callback_ = cb;
}

/**
 * IMPORTANT: one transaction has to be handle by the same partition-id thread on all shards
 */
namespace mako
{
    using namespace std;

    namespace {
        // @safe - Affine lane identity and counters outlive Client attachments.
        thread_local rusty::Cell<uint64_t> anonymous_incarnation{0};
        thread_local rusty::Cell<uint64_t> lane_attempt{0};
        thread_local rusty::Cell<uint64_t> lane_sequence{0};
    }

    // @unsafe - Attaches the legacy transport to this OS thread's RPC identity.
    Client::Client(std::string file,
                   Transport *transport,
                   uint64_t clientid) : config(file),
                                        transport(transport)
    {
        this->clientid = clientid;
        if (this->clientid == 0) {
            if (anonymous_incarnation.get() == 0) {
                std::random_device rd;
                std::mt19937_64 gen(rd());
                std::uniform_int_distribution<uint64_t> dis;
                uint64_t incarnation = 0;
                while (incarnation == 0) incarnation = dis(gen);
                anonymous_incarnation.set(incarnation);
            }
            this->clientid = anonymous_incarnation.get();
            Debug("start a Client ID: %lu", this->clientid);
        }
        current_term = 0;
    }

    // @unsafe - Affine legacy RPC boundary; one begin per coordinator attempt.
    void Client::BeginAttempt(uint32_t term) {
        if (term > 9 || lane_attempt.get() == UINT64_MAX)
            throw std::overflow_error("RPC attempt identity exhausted");
        lane_attempt.set(lane_attempt.get() + 1);
        attempt_identity.incarnation = clientid;
        attempt_identity.attempt = lane_attempt.get();
        attempt_identity.term = term;
        current_term = term;
    }

    // @unsafe - The affine caller must never reuse a wrapped sequence.
    uint64_t Client::NextRequestSequence() {
        if (lane_sequence.get() == UINT64_MAX)
            throw std::overflow_error("RPC request sequence exhausted");
        lane_sequence.set(lane_sequence.get() + 1);
        return lane_sequence.get();
    }

    // @unsafe - Reads native wire headers before invoking legacy continuations.
    void Client::ReceiveResponse(uint8_t reqType, char *respBuf)
    {
        // @unsafe - Every participant response has this fixed header prefix.
        if (reqType >= getReqType && reqType <= batchLockReqType) {
            const auto *response = reinterpret_cast<const basic_response_t *>(respBuf);
            if (!(response->identity == pending_identity) || !num_response_waiting)
                return;
        }
        Debug("[%lu] received response, type: %d", clientid, reqType);
        switch (reqType)
        {
        case getReqType:
            HandleGetReply(respBuf);
            break;
        case scanReqType:
            HandleScanReply(respBuf);
            break;
        case fullScanReqType:
            // The generic reply handler reads only the shared req_nr prefix.
            HandleBatchLockReply(respBuf);
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
        case nontxnPutReqType:
        case nontxnInsertReqType:
        case nontxnRemoveReqType:
        case nontxnGetReqType:
            HandleNontxnWriteReply(respBuf);
            break;
        default:
            Warning("Unrecognized request type: %d\n", reqType);
        }
    }

    // there are two purposes:
    //  1. validate in the commit phase
    //  2. attache watermark in the result
    // @unsafe - Marshals native SRPC buffers on the affine coordinator lane.
    void Client::InvokeValidate(uint64_t txn_nr,
                                int dstShardIdx,
                                uint16_t server_id,
                                resp_continuation_t continuation,
                                error_continuation_t error_continuation,
                                uint32_t timeout)
    {
        Debug("invoke InvokeValidate\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        blocked = true;
        if (!transport->SendRequestToAll(this,
                                      validateReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      sizeof(get_int_response_t),
                                      sizeof(basic_request_t)) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
    }

    // @unsafe - Marshals native SRPC buffers on the affine coordinator lane.
    void Client::InvokeGetTimestamp(uint64_t txn_nr,
                                int dstShardIdx,
                                uint16_t server_id,
                                resp_continuation_t continuation,
                                error_continuation_t error_continuation,
                                uint32_t timeout)
    {
        Debug("invoke InvokeGetTimestamp\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        blocked = true;
        if (!transport->SendRequestToAll(this,
                                      getTimestampReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      sizeof(get_int_response_t),
                                      sizeof(basic_request_t)) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
    }

    // @unsafe - Marshals native SRPC control buffers.
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
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        reqBuf->control = control;
        reqBuf->value = value;
        blocked = true;
        if (!transport->SendRequestToAll(this,
                                    controlReqType,
                                    dstShardIdx,
                                    is_datacenter_failure? config.warehouses+1: 
                                        config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                    sizeof(get_int_response_t),
                                    sizeof(control_request_t)) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); } 
    }

    // @unsafe - Marshals native SRPC control buffers.
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
       uint64_t reqId = NextRequestSequence();
       

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
        StampRequest(reqBuf, reqId);
        blocked = true;
        if (!transport->SendRequestToAll(this,
                                    warmupReqType,
                                    dstShardIdx,
                                    config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                    sizeof(get_int_response_t),
                                    sizeof(warmup_request_t),
                                    centerId) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); } 
    }


    // @unsafe - Marshals native SRPC control buffers.
    void Client::InvokeExchangeWatermark(uint64_t txn_nr,
                                int dstShardIdx,
                                uint16_t server_id,
                                resp_continuation_t continuation,
                                error_continuation_t error_continuation,
                                uint32_t timeout)
    {
        Debug("invoke InvokeExchangeWatermark\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        blocked = true;
        if (!transport->SendRequestToAll(this,
                                    watermarkReqType,
                                    dstShardIdx,
                                    config.warehouses+1,
                                    sizeof(get_int_response_t),
                                    sizeof(basic_request_t)) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
    }

    // @unsafe - Sends the irreversible decision through native SRPC.
    void Client::InvokeInstall(uint64_t txn_nr,
                               int dstShardIdx,
                               uint16_t server_id,
                               char*cc,
                               resp_continuation_t continuation,
                               error_continuation_t error_continuation,
                               uint32_t timeout)
    {
        Debug("invoke InvokeInstall\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        // `cc` is a 4-byte buffer produced by encode_single_timestamp() (see
        // common.h:482 and shardClient.cc:333-335) — a single uint32_t. The
        // receiver's HandleInstallRequest() (server.cc:153-174) reads exactly
        // one uint32 via decode_single_timestamp(req->value). The previous
        // `* config.nshards` multiplier on both `len` and the memcpy here was
        // dead code from an older "vector of timestamps per shard" wire shape
        // that the install handler migrated away from; under nshards>=2 it
        // memcpy'd 4*N bytes from a 4-byte heap allocation, overrunning into
        // jemalloc's next slab. The corrupted bytes propagated through the
        // wire and eventually showed up as a stack-buffer-overflow in
        // `inline_str_base::operator=` (the customer record's c_first/c_last
        // size byte read as 106), which is what blew up shard2ReplicationRaft.
        // AddressSanitizer pinpointed this on 2026-04-25; see
        // logs/20260425-fix-shard2-asan/asan_report.787.
        reqBuf->len = sizeof(uint32_t);
        memcpy(reqBuf->value, cc, sizeof(uint32_t));
        blocked = true;
        size_t bytes_used = sizeof(vector_int_request_t) - max_vector_int_length + sizeof(uint32_t);
        if (!transport->SendRequestToAll(this,
                                      installReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      sizeof(basic_response_t),
                                      bytes_used) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
    }

    // @unsafe - Legacy serialization RPC boundary.
    void Client::InvokeSerializeUtil(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            char*cc,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout) {
        Debug("invoke InvokeSerializeUtil\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        reqBuf->len = sizeof(uint64_t)*config.nshards;
        memcpy(reqBuf->value, cc, sizeof(uint64_t)*config.nshards);
        blocked = true;
        size_t bytes_used = sizeof(vector_int_request_t) - max_vector_int_length + sizeof(uint64_t)*config.nshards;

        if (!transport->SendRequestToAll(this,
                                      serializeUtilReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      sizeof(basic_response_t),
                                      bytes_used) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }

    }

    // @unsafe - Legacy unlock RPC boundary.
    void Client::InvokeUnLock(uint64_t txn_nr,
                              int dstShardIdx,
                              uint16_t server_id,
                              resp_continuation_t continuation,
                              error_continuation_t error_continuation,
                              uint32_t timeout)
    {
        Debug("invoke InvokeUnLock\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        blocked = true;
        if (!transport->SendRequestToAll(this,
                                      unLockReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      sizeof(basic_response_t),
                                      sizeof(basic_request_t)) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
    }

    // @unsafe - Shares one identity across owned per-destination wire batches.
    void Client::InvokeBatchLock(
        uint64_t txn_nr,
        uint16_t server_id,
        map<int, BatchLockRequestWrapper> &request_batch_per_shard,
        resp_continuation_t continuation,
        error_continuation_t error_continuation,
        uint32_t timeout
    ) {
        Debug("invoke InvokeBatchLock\n");
        uint64_t reqId = NextRequestSequence();
        

        crtReqK =
            PendingRequestK("batchLock",
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        map<int, pair<char*, size_t>> data_to_send;

        for (auto &it: request_batch_per_shard) {
            StampRequest(it.second.get_request_ptr(), reqId);
            sharding_write_control_request(it.second.get_request_ptr()->sharding);
            data_to_send[it.first] = make_pair((char*)it.second.get_request_ptr(), it.second.get_msg_len());
        }
        blocked = true;

        //reqBuf->targert_server_id = server_id;
        if (!transport->SendBatchRequestToAll(this,
        batchLockReqType,
        config.warehouses+5+server_id%TThread::get_num_rpc_server(),
        sizeof(basic_response_t),
        data_to_send) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
    }

    // @unsafe - a timeout, failed send, or reply rejected by the identity fence
    // is an unknown outcome, never SUCCESS.
    void Client::SendShardRequest(uint8_t kind, int shard, uint16_t server, size_t length) {
        try {
            if (transport->SendRequestToShard(this, kind, shard, server, length) && !blocked)
                return;
        } catch (int error) {
            if (error != 1002) throw;
        }
        blocked = false;
        crtReqK.error_continuation(crtReqK.request, ErrorCode{});
    }


    // @unsafe - Marshals native SRPC buffers on the affine coordinator lane;
    // copies the captured scan envelope into the transport-owned buffer.
    void Client::InvokeScan(uint64_t txn_nr,
                           int dstShardIdx,
                           uint16_t server_id,
                           const string &start_key,
                           const string &end_key,
                           uint16_t table_id,
                           const ShardingRequest& sharding,
                           resp_continuation_t continuation,
                           error_continuation_t error_continuation,
                           uint32_t timeout)
    {
        Debug("invoke InvokeScan: %d\n", server_id);
        uint64_t reqId = NextRequestSequence();
        

        crtReqK =
            PendingRequestK(start_key,
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        // TODO: find a way to get sending errors (the backend's enqueue_request
        // function does not return errors)
        auto *reqBuf = reinterpret_cast<scan_request_t *>(
            transport->GetRequestBuf(
                sizeof(scan_request_t),
                sizeof(scan_response_t)));
        reqBuf->targert_server_id = server_id;
        StampRequest(reqBuf, reqId);
        reqBuf->sharding = sharding;
        reqBuf->slen = start_key.size();
        memcpy(reqBuf->start_end_key, start_key.c_str(), start_key.size());
        reqBuf->elen = end_key.size();
        memcpy(reqBuf->start_end_key+start_key.size(), end_key.c_str(), end_key.size());
        reqBuf->table_id = table_id;
        ASSERT_LT(end_key.size()+start_key.size()-1, 128);

        blocked = true;
        SendShardRequest(scanReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      sizeof(scan_request_t));
    }

    // @unsafe - transport owns the request buffer until synchronous completion.
    void Client::InvokeFullScanPage(uint64_t txn_nr, int destination,
                                   uint16_t server_id, int table_id,
                                   const ShardingRequest& sharding,
                                   const uint8_t* payload, size_t length,
                                   resp_continuation_t continuation,
                                   error_continuation_t error_continuation) {
        if (length > full_scan_request_capacity) {
            error_continuation("", ErrorCode{});
            return;
        }
        // A page is a piece of the current participant attempt: it carries the
        // same identity (and term) as the legacy 2PC requests.
        const uint64_t sequence = NextRequestSequence();
        crtReqK = PendingRequestK("", sequence, txn_nr, server_id,
                                 continuation, error_continuation);
        auto* request = reinterpret_cast<full_scan_request_t*>(
            transport->GetRequestBuf(sizeof(full_scan_request_t),
                                     sizeof(full_scan_response_t)));
        *request = {};
        request->targert_server_id = server_id;
        StampRequest(request, sequence);
        request->sharding = sharding;
        request->table_id = table_id;
        request->length = static_cast<uint32_t>(length);
        std::memcpy(request->payload, payload, length);
        blocked = true;
        if (!transport->SendRequestToShard(this, fullScanReqType, destination,
            config.warehouses + 5 + server_id % TThread::get_num_rpc_server(),
            sizeof(full_scan_request_t)) || blocked) {
            blocked = false;
            crtReqK.req_nr = 0;
            error_continuation("", ErrorCode{});
        }
    }

    // @unsafe - Marshals native SRPC buffers on the affine coordinator lane;
    // materializes the storage-selected scope directly in transport storage.
    void Client::InvokeGet(uint64_t txn_nr,
                           int dstShardIdx,
                           uint16_t server_id,
                           const string &key,
                           uint16_t table_id,
                           const ShardingPoint& point,
                           resp_continuation_t continuation,
                           error_continuation_t error_continuation,
                           uint32_t timeout)
    {
        uint64_t reqId = NextRequestSequence();
        

        //Warning("invoke InvokeGet,txn_nr:%d,par_id:%d,id:%d",txn_nr,TThread::getGlobalPartitionID(),TThread::id());
        Debug("invoke InvokeGet\n");
        crtReqK =
            PendingRequestK(key,
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<get_request_t *>(
            transport->GetRequestBuf(
                sizeof(get_request_t),
                sizeof(get_response_t)));
        reqBuf->targert_server_id = server_id;
        StampRequest(reqBuf, reqId);
        point.write_request(reqBuf->sharding);
        reqBuf->len = key.size();
        ASSERT_LT(key.size(), max_key_length);

        memcpy(reqBuf->key, key.c_str(), key.size());
        reqBuf->table_id = table_id;

        size_t bytes_used = sizeof(get_request_t) - max_key_length + reqBuf->len;

        blocked = true;
        SendShardRequest(getReqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      bytes_used);
    }

    // @unsafe - materializes a local point only after allocating its transport buffer.
    void Client::InvokeNontxnWrite(uint64_t txn_nr, int dstShardIdx,
                                   uint16_t server_id, const string& key,
                                   const string& value, uint16_t table_id,
                                   const ShardingPoint& point, uint8_t reqType,
                                   resp_continuation_t continuation,
                                   error_continuation_t error_continuation,
                                   uint32_t timeout) {
        auto* request = PrepareNontxnWrite(txn_nr, server_id, key, value,
            table_id, reqType, continuation, error_continuation);
        point.write_request(request->sharding);
        SendNontxnWrite(reqType, dstShardIdx, server_id, *request);
    }

    // @unsafe - preserves the forwarded wire grant, snapshot and transaction identity.
    void Client::InvokeNontxnWrite(uint64_t txn_nr, int dstShardIdx,
                                   uint16_t server_id, const string& key,
                                   const string& value, uint16_t table_id,
                                   const ShardingRequest& sharding, uint8_t reqType,
                                   resp_continuation_t continuation,
                                   error_continuation_t error_continuation,
                                   uint32_t timeout) {
        auto* request = PrepareNontxnWrite(txn_nr, server_id, key, value,
            table_id, reqType, continuation, error_continuation);
        request->sharding = sharding;
        SendNontxnWrite(reqType, dstShardIdx, server_id, *request);
    }

    // @unsafe - packs shared fields; the caller fills sharding before sending.
    nontxn_write_request_t* Client::PrepareNontxnWrite(
                                   uint64_t txn_nr, uint16_t server_id,
                                   const string& key, const string& value,
                                   uint16_t table_id, uint8_t reqType,
                                   const resp_continuation_t& continuation,
                                   const error_continuation_t& error_continuation)
    {
        uint64_t reqId = NextRequestSequence();
        

        Debug("invoke InvokeNontxnWrite type=%d\n", reqType);
        crtReqK =
            PendingRequestK(key,
                            reqId,
                            txn_nr,
                            server_id,
                            continuation,
                            error_continuation);

        auto *reqBuf = reinterpret_cast<nontxn_write_request_t *>(
            transport->GetRequestBuf(
                sizeof(nontxn_write_request_t),
                sizeof(client_kv_response_t)));
        reqBuf->targert_server_id = server_id;
        StampRequest(reqBuf, reqId);
        // A standalone request belongs to no participant attempt, so its term
        // fence (ShardReceiver::ReceiveRequest) must see this sender's current
        // term, sampled from the same source as Transaction::get_current_term,
        // not the term of whichever attempt this lane began last. An ephemeral
        // gateway forwarder never begins one and would stay at term 0 forever.
        if (sc_callback_) {
            const int term = sc_callback_();
            if (term > current_term) current_term = term;
        }
        reqBuf->identity.term = static_cast<uint32_t>(current_term);
        pending_identity = reqBuf->identity;
        reqBuf->table_id = table_id;
        ASSERT_LT(key.size(), max_key_length);
        ASSERT_LT(value.size(), max_value_length);
        reqBuf->klen = key.size();
        reqBuf->vlen = value.size();
        memcpy(reqBuf->key_and_value, key.data(), key.size());
        memcpy(reqBuf->key_and_value + key.size(), value.data(), value.size());
        return reqBuf;
    }

    // @unsafe - sends the populated transport buffer without copying it.
    void Client::SendNontxnWrite(uint8_t reqType, int dstShardIdx,
                                 uint16_t server_id,
                                 const nontxn_write_request_t& request) {

        size_t bytes_used = sizeof(nontxn_write_request_t)
            - (max_key_length + max_value_length)
            + request.klen + request.vlen;

        blocked = true;
        SendShardRequest(reqType,
                                      dstShardIdx,
                                      config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                      bytes_used);
    }

    // @unsafe - Requests predecision cleanup; missing replies remain failures.
    void Client::InvokeAbort(uint64_t txn_nr,
                             int dstShardIdx,
                             uint16_t server_id,
                             basic_continuation_t continuation,
                             error_continuation_t error_continuation,
                             uint32_t timeout)
    {
        Debug("invoke InvokeAbort\n");
        uint64_t reqId = NextRequestSequence();
        

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
        StampRequest(reqBuf, reqId);
        sharding_write_control_request(reqBuf->sharding);
        blocked = true;
        try {
            if (!transport->SendRequestToAll(this,
                                        abortReqType,
                                        dstShardIdx,
                                        config.warehouses+5+server_id%TThread::get_num_rpc_server(),
                                        sizeof(basic_response_t),
                                        sizeof(basic_request_t)) || blocked) { blocked = false; crtReqK.error_continuation(crtReqK.request, ErrorCode{}); }
        } catch(int n) {
            // for abort itself, we do nothing
        }

    }

    void Client::HandleGetReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<get_response_t *>(respBuf);
        Debug("[%lu]Received get HandleGetReply, to RPC client receives a message, req_nr: %d, crt: %d, status: %d\n", clientid, resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            Debug("Received get reply when no request was pending; req_nr = %lu, crtReqK:%lu,par_id:%d,id:%d", resp->req_nr, crtReqK.req_nr,TThread::getGlobalPartitionID(),TThread::id());
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

    // Reply handler for the three non-txn write types; same
    // req_nr-matching and unblocking protocol as HandleGetReply.
    void Client::HandleNontxnWriteReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<client_kv_response_t *>(respBuf);
        Debug("[%lu] HandleNontxnWriteReply req_nr: %d, crt: %d, status: %d\n",
              clientid, resp->req_nr, crtReqK.req_nr, resp->status);
        if (resp->req_nr != crtReqK.req_nr)
        {
            Debug("Received nontxn-write reply when no request was pending; req_nr = %u", resp->req_nr);
            return;
        }

        crtReqK.resp_continuation(respBuf);
        if (num_response_waiting) num_response_waiting --;
        if (num_response_waiting == 0) {
            blocked = false;
            crtReqK.req_nr = 0;
        }
    }

    void Client::HandleScanReply(char *respBuf)
    {
        auto *resp = reinterpret_cast<scan_response_t *>(respBuf);
        Debug("[%lu]Received get HandleGetReply, to RPC client receives a message, req_nr: %d, crt: %d, status: %d\n", clientid, resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a HandleLockReply, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
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
        Debug("RPC client receives a HandleLockReply, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
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
        Debug("RPC client receives a HandleValidateReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a HandleWatermarkReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a HandleControlReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a HandleWarmupReply, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a HandleGetTimestamp, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a HandleSerializeUtil, req_nr: %d, crt: %d, status: %d\n", resp->req_nr, crtReqK.req_nr, resp->status);
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
        Debug("RPC client receives a message, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
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
        Debug("RPC client receives a message, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
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
        Debug("RPC client receives a message, req_nr: %d, crt: %d\n", resp->req_nr, crtReqK.req_nr);
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
