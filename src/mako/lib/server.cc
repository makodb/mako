#include "lib/fasttransport.h"
#include "lib/timestamp.h"
#include "lib/server.h"
#include "lib/common.h"
#include "lib/transport_request_handle.h"
#include "sto/Interface.hh"
#include "benchmarks/common.h"
#include "benchmarks/bench.h"
#include "benchmarks/tpcc.h"
// After bench.h so the textual std headers are already in (the
// `import std;` below would otherwise make ticker.h/rcu.h's
// unqualified lock_guard/allocator references ambiguous).
#include "sto/Transaction.hh"
#include "storage/mbta_wrapper.hh"
#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
#endif
#include "deptran/s_main.h"
#include "sto/sync_util.hh"

import std;

std::function<int()> ss_callback_ = nullptr;
void register_sync_util_ss(std::function<int()> cb) {
    ss_callback_ = cb;
}

namespace mako
{
    using namespace std;

    ShardReceiver::ShardReceiver(std::string file) : config(file)
    {
        current_term = 0;
    }

    void ShardReceiver::Register(abstract_db *dbX,
                                 const map<int, abstract_ordered_index *> &open_tables_table_idX /*,
                                 const map<string, vector<abstract_ordered_index *>> &partitionsX,
                                 const map<string, vector<abstract_ordered_index *>> &remote_partitionsX*/)
    {
        db = dbX;
        owner_ = TThread::get_shard_index();
        {
            auto tables = tables_.lock().unwrap();
            *tables = open_tables_table_idX;
        }

        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
        // Establish the idle-participant invariant (txn in_progress
        // and empty) — a mode-1 concept for helper threads serving 2PC
        // RPCs. Standalone receivers (ClientTcpServer) are registered
        // from mode-0 threads, where a lingering in_progress txn would
        // trip the next one-op op's start_transaction assert.
        if (TThread::mode() == 1) {
            db->shard_reset(); // initialize
        }
        obj_key0.reserve(128);
        obj_key1.reserve(128);
        obj_v.reserve(256);
    }

    void ShardReceiver::UpdateTableEntry(int table_id, abstract_ordered_index *table)
    {
        if (table_id <= 0 || !table)
            return;
        auto tables = tables_.lock().unwrap();
        (*tables)[table_id] = table;
    }

    // @unsafe - only map traversal is locked; indexes have process lifetime.
    abstract_ordered_index* ShardReceiver::table_for(int id) const {
        auto tables = tables_.lock().unwrap();
        auto found = tables->find(id);
        return found == tables->end() ? nullptr : found->second;
    }
    // @unsafe - missing lazily adopted indexes are an explicit failed request.
    abstract_ordered_index* ShardReceiver::require_table(int id) const {
        auto* table = table_for(id);
        if (!table) throw abstract_db::abstract_abort_exception();
        return table;
    }

    // @unsafe - dispatch has already bound identity and canonical physical index.
    void ShardReceiver::HandleFullScanRequest(char* input, char* output,
                                              size_t& length) {
        const auto* request = reinterpret_cast<const full_scan_request_t*>(input);
        auto* response = reinterpret_cast<full_scan_response_t*>(output);
        *response = {};
        response->req_nr = request->req_nr;
        response->status = ErrorCode::ERROR;
        // The backend allocates/reads this exact bounded response size.
        length = sizeof(*response);
        try {
            if (request->length > full_scan_request_capacity
                || current_term > request->req_nr % 10)
                throw abstract_db::abstract_abort_exception();
            auto* index = dynamic_cast<mbta_ordered_index*>(require_table(request->table_id));
            if (!index || index->get_is_remote())
                throw abstract_db::abstract_abort_exception();
            size_t page_length = 0;
            oi_mbta_full_scan_page(index->mbta, request->sharding,
                request->payload, request->length, response->payload,
                sizeof(response->payload), &page_length);
            response->length = static_cast<uint32_t>(page_length);
            response->status = ErrorCode::SUCCESS;
        } catch (const abstract_db::abstract_abort_exception&) {
            db->shard_abort_txn(nullptr);
            if (!sharding_leases_enabled()) db->shard_reset();
        }
    }

    // @unsafe - constructs the correct legacy wire shape without executing an op.
    static size_t sharding_error_reply(uint8_t kind, uint32_t number,
                                       int status, char* output) {
        if (kind == fullScanReqType) {
            auto* r = reinterpret_cast<full_scan_response_t*>(output);
            *r = {};
            r->req_nr = number; r->status = status;
            return sizeof(*r);
        }
        if (kind >= nontxnPutReqType && kind <= nontxnGetReqType) {
            auto* r = reinterpret_cast<client_kv_response_t*>(output);
            r->req_nr = number; r->status = status; r->vlen = 0;
            return offsetof(client_kv_response_t, value);
        }
        if (kind == getReqType || kind == scanReqType) {
            auto* r = reinterpret_cast<get_response_t*>(output);
            r->req_nr = number; r->status = status; r->len = 0;
            return offsetof(get_response_t, value);
        }
        if (kind == validateReqType) {
            auto* r = reinterpret_cast<get_int_response_t*>(output);
            r->req_nr = number; r->status = status; r->result = 0;
            r->shard_index = TThread::get_shard_index();
            return sizeof(*r);
        }
        auto* r = reinterpret_cast<basic_response_t*>(output);
        r->req_nr = number; r->status = status;
        return sizeof(*r);
    }

    // @unsafe - fixed sequential worker streams; the native participant owns lease
    // admission. This boundary retains opaque replies and serializes ambient STO.
    size_t ShardReceiver::ReceiveRequest(uint8_t kind, char* input, char* output) {
        const bool standalone = kind >= nontxnPutReqType && kind <= nontxnGetReqType;
        const bool piece = kind == getReqType || kind == scanReqType
            || kind == lockReqType || kind == batchLockReqType || kind == fullScanReqType;
        const bool terminal = kind == abortReqType || kind == installReqType
            || kind == unLockReqType || standalone;
        if (!(piece || terminal || kind == validateReqType))
            return DispatchRequest(kind, input, output);

        // All shard transaction requests share this prefix by construction.
        const auto* header = reinterpret_cast<const basic_request_t*>(input);
        const auto request = header->sharding;
        const uint32_t number = header->req_nr;
        // Forwarded gateway calls retain their ingress sequence even with the
        // native directory disabled, so a lost reply cannot repeat an effect.
        if (!sharding_leases_enabled()
            && !(standalone && (request.transaction.client & (uint64_t{1} << 63))))
            return DispatchRequest(kind, input, output);
        auto peers = sharding_peers_.lock().unwrap();
        auto& peer = (*peers)[request.transaction.client];
        if (request.transaction.sequence < peer.sequence)
            return sharding_error_reply(kind, number, ErrorCode::ERROR, output);
        if (request.transaction.sequence == peer.sequence) {
            if (kind == abortReqType && peer.terminal && peer.aborted)
                return sharding_error_reply(kind, number, ErrorCode::SUCCESS, output);
            if (!peer.reply.empty() && kind == peer.kind
                && (peer.terminal || number == peer.request)) {
                std::memcpy(output, peer.reply.data(), peer.reply.size());
                std::memcpy(output, &number, sizeof(number));
                return peer.reply.size();
            }
            if (peer.terminal || number <= peer.request)
                return sharding_error_reply(kind, number, ErrorCode::ERROR, output);
        }
        if (request.transaction.sequence > peer.sequence && peer.sequence != 0 && !peer.terminal)
            return sharding_error_reply(kind, number, ErrorCode::SERVER_BUSY, output);
        if (current_term > number % 10)
            return sharding_error_reply(kind, number, ErrorCode::ERROR, output);
        if (kind == abortReqType && request.transaction.sequence > peer.sequence) {
            // This stream never started this transaction here (possibly its first
            // read was rejected while another stream occupied the engine). Retain
            // a cancellation fence without aborting that other stream's engine.
            const size_t length = sharding_error_reply(
                kind, number, ErrorCode::SUCCESS, output);
            peer.sequence = request.transaction.sequence;
            peer.request = number;
            peer.kind = kind;
            peer.terminal = true;
            peer.aborted = true;
            peer.reply.assign(output, length);
            return length;
        }
        // Allocate reply retention before any engine effect. A successful effect
        // must not become unreplayable because buffering its receipt allocates.
        const size_t reply_capacity = kind == fullScanReqType ? sizeof(full_scan_response_t)
            : standalone ? sizeof(client_kv_response_t)
            : (kind == getReqType || kind == scanReqType) ? sizeof(get_response_t)
            : kind == validateReqType ? sizeof(get_int_response_t)
            : sizeof(basic_response_t);
        peer.reply.reserve(reply_capacity);
        if (!sharding_bind_request(request))
            return sharding_error_reply(kind, number, ErrorCode::ERROR, output);
        // Publish participation before entering legacy code: an exception must
        // never let a later abort mistake a partially staged engine for absence.
        if (peer.sequence != request.transaction.sequence) {
            peer.reply.clear();
            peer.request = 0;
        }
        peer.sequence = request.transaction.sequence;
        peer.terminal = false;
        peer.aborted = false;

        size_t length;
        try {
            // Replace only the physical selector, never the canonical wire address
            // or original owner+epoch grant. Each batch row resolves independently.
            if (kind == fullScanReqType) {
                auto* r = reinterpret_cast<full_scan_request_t*>(input);
                r->table_id = sharding_request_table(request, r->table_id);
            }
            if (kind == getReqType) {
                auto* r = reinterpret_cast<get_request_t*>(input);
                r->table_id = sharding_request_table(request, r->table_id);
            } else if (kind == scanReqType) {
                auto* r = reinterpret_cast<scan_request_t*>(input);
                r->table_id = sharding_request_table(request, r->table_id);
            } else if (kind == lockReqType) {
                auto* r = reinterpret_cast<lock_request_t*>(input);
                r->table_id = sharding_request_table(request, r->table_id);
            } else if (standalone) {
                auto* r = reinterpret_cast<nontxn_write_request_t*>(input);
                r->table_id = sharding_request_table(request, r->table_id);
            }
            length = DispatchRequest(kind, input, output);
        } catch (const abstract_db::abstract_abort_exception&) {
            db->shard_abort_txn(nullptr);
            length = sharding_error_reply(kind, number, ErrorCode::ERROR, output);
        }

        // Engine-aborted pieces are terminal too. No terminal response is retained
        // until all physical unlock/cleanup effects have actually completed.
        const bool completed = terminal || !TThread::txn || !TThread::txn->in_progress();
        if (completed) {
            if (TThread::txn && TThread::txn->in_progress())
                db->shard_abort_txn(nullptr);
            sharding_finish_request();
        }
        peer.sequence = request.transaction.sequence;
        peer.request = number;
        peer.kind = kind;
        peer.terminal = completed;
        peer.aborted = completed && kind != installReqType && !standalone;
        peer.reply.assign(output, length);
        if (completed && TThread::mode() == 1) db->shard_reset();
        return length;
    }

    // Message handlers.
    size_t ShardReceiver::DispatchRequest(uint8_t reqType, char *reqBuf, char *respBuf)
    {
        Debug("server deal with reqType: %d", reqType);
        size_t respLen;
        switch (reqType)
        {
        case getReqType:
            HandleGetRequest(reqBuf, respBuf, respLen);
            break;
        case nontxnPutReqType:
        case nontxnInsertReqType:
        case nontxnRemoveReqType:
        case nontxnGetReqType:
            HandleNontxnWriteRequest(reqType, reqBuf, respBuf, respLen);
            break;
        case fullScanReqType:
            HandleFullScanRequest(reqBuf, respBuf, respLen);
            break;
        case scanReqType:
            HandleScanRequest(reqBuf, respBuf, respLen);
            break;
        case lockReqType:
            HandleLockRequest(reqBuf, respBuf, respLen);
            break;
        case validateReqType:
            HandleValidateRequest(reqBuf, respBuf, respLen);
            break;
        case getTimestampReqType:
            HandleGetTimestampRequest(reqBuf, respBuf, respLen);
            break;
        case serializeUtilReqType:
            HandleSerializeUtilRequest(reqBuf, respBuf, respLen);
            break;
        case installReqType:
            HandleInstallRequest(reqBuf, respBuf, respLen);
            break;
        case unLockReqType:
            HandleUnLockRequest(reqBuf, respBuf, respLen);
            break;
        case abortReqType:
            HandleAbortRequest(reqBuf, respBuf, respLen);
            break;
        case batchLockReqType:
            HandleBatchLockRequest(reqBuf, respBuf, respLen);
            break;
        // Client API handlers (for decoupled client-server mode)
        case clientBeginTxnReqType:
            HandleClientBeginTxnRequest(reqBuf, respBuf, respLen);
            break;
        case clientCommitReqType:
            HandleClientCommitRequest(reqBuf, respBuf, respLen);
            break;
        case clientRollbackReqType:
            HandleClientRollbackRequest(reqBuf, respBuf, respLen);
            break;
        case clientPutReqType:
            HandleClientPutRequest(reqBuf, respBuf, respLen);
            break;
        case clientGetReqType:
            HandleClientGetRequest(reqBuf, respBuf, respLen);
            break;
        case clientDeleteReqType:
            HandleClientDeleteRequest(reqBuf, respBuf, respLen);
            break;
        case clientRouteReqType:
            HandleClientRouteRequest(reqBuf, respBuf, respLen);
            break;
        case clientInsertReqType:
            HandleClientInsertRequest(reqBuf, respBuf, respLen);
            break;
        default:
            Warning("Unrecognized rquest type: %d", reqType);
        }

        return respLen;
    }

    void ShardReceiver::HandleAbortRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        int status = ErrorCode::SUCCESS;
        auto *req = reinterpret_cast<basic_request_t *>(reqBuf);
        db->shard_abort_txn(nullptr);

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        if (!sharding_leases_enabled()) db->shard_reset();

    }

    void ShardReceiver::HandleUnLockRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        Panic("Deprecated!");
        int status = ErrorCode::SUCCESS;
        auto *req = reinterpret_cast<basic_request_t *>(reqBuf);
        try {
            db->shard_unlock(true);
        } catch (abstract_db::abstract_abort_exception &ex) {
            //db->shard_abort_txn(nullptr);
            status = ErrorCode::ABORT;
            Warning("HandleUnLockRequest error");
        }

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        if (!sharding_leases_enabled()) db->shard_reset();
    }

    void ShardReceiver::HandleInstallRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        int status = ErrorCode::SUCCESS;
        auto *req = reinterpret_cast<vector_int_request_t *>(reqBuf);
        try {
            // Single timestamp system: decode single timestamp directly
            uint32_t timestamp = decode_single_timestamp(req->value);
            db->shard_install(timestamp);
            db->shard_serialize_util(timestamp);
            db->shard_unlock(true);
        } catch (abstract_db::abstract_abort_exception &ex) {
            //db->shard_abort_txn(nullptr);
            status = ErrorCode::ABORT;
            Warning("HandleInstallRequest error");
        }

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        if (!sharding_leases_enabled()) db->shard_reset();
    }

    void ShardReceiver::HandleValidateRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        int status = ErrorCode::SUCCESS;
        auto *req = reinterpret_cast<basic_request_t *>(reqBuf);
        try {
            status = db->shard_validate();
            if (status>0){
                //db->shard_abort_txn(nullptr); // early reject, unlock the key earlier
            }
        } catch (abstract_db::abstract_abort_exception &ex) {
            //db->shard_abort_txn(nullptr);
            status = ErrorCode::ABORT;
            Warning("HandleValidateRequest error");
        }

        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        respLen = sizeof(get_int_response_t);
        resp->result = sync_util::sync_logger::retrieveShardW();
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->shard_index = TThread::get_shard_index();
        resp->req_nr = req->req_nr;
    }

    void ShardReceiver::HandleGetTimestampRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        int status = ErrorCode::SUCCESS;
        uint32_t result = 0;
        auto *req = reinterpret_cast<basic_request_t*>(reqBuf);
        auto *resp = reinterpret_cast<get_int_response_t *>(respBuf);
        resp->shard_index = TThread::get_shard_index();
        resp->req_nr = req->req_nr;
        respLen = sizeof(get_int_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->result = __sync_fetch_and_add(&sync_util::sync_logger::local_replica_id, 1);;
    }

    void ShardReceiver::HandleSerializeUtilRequest(char *reqBuf, char *respBuf, size_t &respLen) {
        Panic("Deprecated");
        // int status = ErrorCode::SUCCESS;
        // auto *req = reinterpret_cast<vector_int_request_t *>(reqBuf);
        // std::vector<uint32_t> ret;
        // decode_vec_uint32(req->value, TThread::get_nshards()).swap(ret);
        // db->shard_serialize_util(ret);

        // auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        // respLen = sizeof(basic_response_t);
        // resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        // resp->req_nr = req->req_nr;
    }

    // @unsafe - the engine owns staged values and real delete/absent-key items.
    static void stage_batch_write(abstract_ordered_index* table, lcdf::Str key,
                                  const std::string& value, uint8_t operation) {
        if (operation == 0) {
            table->shard_put(key, value);
        } else if (operation == 1) {
            auto* index = dynamic_cast<mbta_ordered_index*>(table);
            if (!index || index->get_is_remote())
                throw abstract_db::abstract_abort_exception();
            oi_mbta_shard_remove(index->mbta, key);
        } else {
            throw abstract_db::abstract_abort_exception();
        }
    }

    void ShardReceiver::HandleBatchLockMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<batch_lock_request_t *>(reqBuf);
        int status = ErrorCode::SUCCESS;

        uint16_t table_id, klen, vlen;
        uint8_t operation;
        char *k_ptr, *v_ptr;
        auto wrapper = BatchLockRequestWrapper(reqBuf);
        
        while (!wrapper.all_request_handled()) {
            wrapper.read_one_request(&k_ptr, &klen, &v_ptr, &vlen, &table_id, &operation);
            //string key(k_ptr, klen);
            obj_key0.assign(k_ptr, klen);
            //string value(v_ptr, vlen);
            obj_v.assign(v_ptr, vlen);
            item_micro::key v_s_temp;
            const item_micro::key *k_s = Decode(obj_key0, v_s_temp);
            if (table_id > 0) {
                try {
                    int base_ol_i_id = k_s->i_id;
                    item_micro::key k_s_new(*k_s);
                    for (int i=0; i<mako::mega_batch_size; i++) {
                        k_s_new.i_id = base_ol_i_id + i;
                        stage_batch_write(require_table(table_id), EncodeK(obj_key0, k_s_new), obj_v, operation);
                    }
                } catch (abstract_db::abstract_abort_exception &ex) {
                   status = ErrorCode::ABORT;
                   Debug("HandleBatchLockMicroMegaRequest: fail to lock a key");
                }
            }
        }

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
    }

    void ShardReceiver::HandleBatchLockMegaRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<batch_lock_request_t *>(reqBuf);
        int status = ErrorCode::SUCCESS;

        uint16_t table_id, klen, vlen;
        uint8_t operation;
        char *k_ptr, *v_ptr;
        auto wrapper = BatchLockRequestWrapper(reqBuf);
        
        while (!wrapper.all_request_handled()) {
            wrapper.read_one_request(&k_ptr, &klen, &v_ptr, &vlen, &table_id, &operation);
            //string key(k_ptr, klen);
            obj_key0.assign(k_ptr, klen);
            //string value(v_ptr, vlen);
            obj_v.assign(v_ptr, vlen);
            stock::key v_s_temp;
            const stock::key *k_s = Decode(obj_key0, v_s_temp);
            if (table_id > 0) {
                try {
                    int base_ol_i_id = k_s->s_i_id;
                    stock::key k_s_new(*k_s);
                    for (int i=0; i<mako::mega_batch_size; i++) {
                        k_s_new.s_i_id = base_ol_i_id + i;
                        stage_batch_write(require_table(table_id), EncodeK(obj_key0, k_s_new), obj_v, operation);
                    }
                } catch (abstract_db::abstract_abort_exception &ex) {
                   //db->shard_abort_txn(nullptr);
                   status = ErrorCode::ABORT;
                   Debug("HandleLockRequest: fail to lock a key");
                }
            }
        }

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
    }

    void ShardReceiver::HandleBatchLockRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
#if defined(MEGA_BENCHMARK)
        HandleBatchLockMegaRequest(reqBuf, respBuf, respLen);
#elif defined(MEGA_BENCHMARK_MICRO)
        HandleBatchLockMicroMegaRequest(reqBuf, respBuf, respLen);
#else
        auto *req = reinterpret_cast<batch_lock_request_t *>(reqBuf);
        int status = ErrorCode::SUCCESS;

        uint16_t table_id, klen, vlen;
        uint8_t operation;
        char *k_ptr, *v_ptr;
        auto wrapper = BatchLockRequestWrapper(reqBuf);
        
        while (!wrapper.all_request_handled()) {
            wrapper.read_one_request(&k_ptr, &klen, &v_ptr, &vlen, &table_id, &operation);
            //string key(k_ptr, klen);
            obj_key0.assign(k_ptr, klen);
            //string value(v_ptr, vlen);
            obj_v.assign(v_ptr, vlen);

            if (table_id > 0) {
                try {
                    stage_batch_write(require_table(table_id), obj_key0, obj_v, operation);
                } catch (abstract_db::abstract_abort_exception &ex) {
                   //db->shard_abort_txn(nullptr);
                   status = ErrorCode::ABORT;
                   Debug("HandleLockRequest: fail to lock a key");
                }
            }
        }

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
#endif
    }

    void ShardReceiver::HandleLockRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        Panic("Deprecated");
        string val;
        auto *req = reinterpret_cast<lock_request_t *>(reqBuf);
        //std::string key = string(req->key_and_value, req->klen);
        obj_key0.assign(req->key_and_value, req->klen);
        //std::string value = string(req->key_and_value + req->klen, req->vlen);
        obj_v.assign(req->key_and_value + req->klen, req->vlen);

        int table_id = req->table_id;
        int status = ErrorCode::SUCCESS;

        if (table_id > 0) {
            try {
                require_table(table_id)->shard_put(obj_key0, obj_v);
            } catch (abstract_db::abstract_abort_exception &ex) {
                //db->shard_abort_txn(nullptr);
                status = ErrorCode::ABORT;
                Debug("HandleLockRequest: fail to lock a key");
            }
        }

        auto *resp = reinterpret_cast<basic_response_t *>(respBuf);
        respLen = sizeof(basic_response_t);
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
    }

    void ShardReceiver::HandleScanRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        string val;
        scoped_str_arena s_arena(arena);

        auto *req = reinterpret_cast<scan_request_t *>(reqBuf);
        obj_key0.assign(req->start_end_key, req->slen);
        obj_key1.assign(req->start_end_key+req->slen, req->elen);
        // const std::string start_key = string(req->start_end_key, req->slen);
        // const std::string end_key = string(req->start_end_key+req->slen, req->elen);

        int status = ErrorCode::SUCCESS;

        static_limit_callback<512> c(s_arena.get(), true); // probably a safe bet for now, NMaxCustomerIdxScanElems
        if (req->table_id > 0) {
            try {
                require_table(req->table_id)->shard_scan(obj_key0, &obj_key1, c, s_arena.get());
                if (c.size() == 0) {
                    //Warning("# of scan is 0, table_id: %d", (int)req->table_id);
                    throw abstract_db::abstract_abort_exception();
                } else {
                    ALWAYS_ASSERT(c.size() > 0);
                    int index = c.size() / 2;
                    if (c.size() % 2 == 0)
                        index--;
                    val = *c.values[index].second;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                db->shard_abort_txn(nullptr);
                status = ErrorCode::ABORT;
            }
        } else {
            val = "this is a mocked value for the rpc client and rpc server";
        }
        
        auto *resp = reinterpret_cast<scan_response_t *>(respBuf);
        respLen = sizeof(scan_response_t) - max_value_length + val.length();
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        resp->len = val.length();
        memcpy(resp->value, val.c_str(), val.length());
    }

    void ShardReceiver::HandleGetMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<get_request_t *>(reqBuf);
        obj_key0.assign(req->key, req->len);

        // for MicroMega, all get request is for item table
        item_micro::key v_s_temp;
        const item_micro::key *k_s = Decode(obj_key0, v_s_temp);
        std::string c_v;
        int offset = 0;
        int value_size = 8;
        c_v.resize(value_size);

        int status = ErrorCode::SUCCESS;
        if (req->table_id > 0) {
            try {
                bool ret = true;
                int base_ol_i_id = k_s->i_id;
                item_micro::key k_s_new(*k_s); 
                for (int i=0; i<mako::mega_batch_size; i++) {
                   k_s_new.i_id = base_ol_i_id + i;
                   ret = require_table(req->table_id)->shard_get(EncodeK(obj_key0, k_s_new), obj_v, std::string::npos);
                   memcpy((char*)c_v.c_str()+offset,obj_v.c_str(),value_size);
                   offset = 0;
                }
                // abort here,
                if (!ret){ // key not found or found but invalid
                    db->shard_abort_txn(nullptr);
                    status = ErrorCode::ABORT;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                // No need to abort, the client side will issue an abort
                db->shard_abort_txn(nullptr);
                status = ErrorCode::ABORT;
            }
        } else {
            obj_v = "this is a mocked value for the rpc client and rpc server";
        }
        
        auto *resp = reinterpret_cast<get_response_t *>(respBuf);
        respLen = sizeof(get_response_t) - max_value_length + c_v.length();
        ALWAYS_ASSERT(max_value_length>=obj_v.length());
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        resp->len = c_v.length();
        //Warning("the remoteGET,len:%d,table_id:%d,keys:%s,key_len:%d,val_len:%d",obj_v.length(),req->table_id,mako::printStringAsBit(obj_key0).c_str(),req->len,obj_v.length());
        memcpy(resp->value, c_v.c_str(), c_v.length());
    }

    void ShardReceiver::HandleGetMegaRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<get_request_t *>(reqBuf);
        obj_key0.assign(req->key, req->len);

        // for NewOrderMega, all get request is for stock table
        stock::key v_s_temp;
        const stock::key *k_s = Decode(obj_key0, v_s_temp);
        //std::cout<<"HandleGetRequest, base:"<<k_s->s_i_id<<", table-id:"<<req->table_id<<std::endl;
        //int tol_len = mako::mega_batch_size* mako::size_per_stock_value;  
        int tol_len = 1* mako::size_per_stock_value;  
        std::string c_v;
        int offset = 0;
        c_v.resize(tol_len);

        int status = ErrorCode::SUCCESS;
        if (req->table_id > 0) {
            try {
                bool ret = true;
                int base_ol_i_id = k_s->s_i_id;
                stock::key k_s_new(*k_s); 
                for (int i=0; i<mako::mega_batch_size; i++) {
                   k_s_new.s_i_id = base_ol_i_id + i;
                   ret = require_table(req->table_id)->shard_get(EncodeK(obj_key0, k_s_new), obj_v, std::string::npos);
                   memcpy((char*)c_v.c_str()+offset,obj_v.c_str(),mako::size_per_stock_value);
                   //offset += mako::size_per_stock_value;
                   offset = 0;
                }
                // abort here,
                //  "not found a key" maybe a expected behavior
                if (!ret){ // key not found or found but invalid
                    db->shard_abort_txn(nullptr);
                    status = ErrorCode::ABORT;
                }
            } catch (abstract_db::abstract_abort_exception &ex) {
                // No need to abort, the client side will issue an abort
                db->shard_abort_txn(nullptr);
                status = ErrorCode::ABORT;
            }
        } else {
            obj_v = "this is a mocked value for the rpc client and rpc server";
        }
        
        auto *resp = reinterpret_cast<get_response_t *>(respBuf);
        respLen = sizeof(get_response_t) - max_value_length + c_v.length();
        ALWAYS_ASSERT(max_value_length>=obj_v.length());
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        resp->len = c_v.length();
        //Warning("the remoteGET,len:%d,table_id:%d,keys:%s,key_len:%d,val_len:%d",obj_v.length(),req->table_id,mako::printStringAsBit(obj_key0).c_str(),req->len,obj_v.length());
        memcpy(resp->value, c_v.c_str(), c_v.length());
    }

    void ShardReceiver::HandleGetRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
#if defined(MEGA_BENCHMARK)
        HandleGetMegaRequest(reqBuf, respBuf, respLen);
#elif defined(MEGA_BENCHMARK_MICRO)
        HandleGetMicroMegaRequest(reqBuf, respBuf, respLen);
#else
        auto *req = reinterpret_cast<get_request_t *>(reqBuf);
#if defined(FAIL_NEW_VERSION)
        current_term = ss_callback_();
#endif
        obj_key0.assign(req->key, req->len);

        int status = ErrorCode::SUCCESS;
        if (req->table_id > 0) {
            // Check if table exists (may not exist in micro benchmark mode)
            auto* table = table_for(req->table_id);
            if (!table) {
                db->shard_abort_txn(nullptr);
                status = ErrorCode::ABORT;
            } else {
                try {
                    bool ret = table->shard_get(obj_key0, obj_v, std::string::npos);
                    if (TThread::transget_without_throw) {
                        TThread::transget_without_throw = false;
                        throw abstract_db::abstract_abort_exception();
                    }
                    if (!ret) {
                        // Keep the absent-key read and native lease until 2PC.
                        obj_v.clear();
                        status = ErrorCode::NOT_FOUND;
                    }
                } catch (abstract_db::abstract_abort_exception &ex) {
                    // No need to abort, the client side will issue an abort
                    db->shard_abort_txn(nullptr);
                    status = ErrorCode::ABORT;
                }
            }
        } else {
            obj_v = "this is a mocked value for the rpc client and rpc server";
        }
        
        auto *resp = reinterpret_cast<get_response_t *>(respBuf);
        respLen = sizeof(get_response_t) - max_value_length + obj_v.length();
        ALWAYS_ASSERT(max_value_length>=obj_v.length());
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        resp->len = obj_v.length();
        //Warning("the remoteGET,len:%d,table_id:%d,keys:%s,key_len:%d,val_len:%d",obj_v.length(),req->table_id,mako::printStringAsBit(obj_key0).c_str(),req->len,obj_v.length());
        memcpy(resp->value, obj_v.c_str(), obj_v.length());
#endif
    }

    // Self-contained non-transactional write (put / insert / remove by
    // reqType). Runs the op through the L3 non-txn API — an internal
    // one-op OCC transaction on this shard, which replicates through
    // the normal commit path (serialize_util) when replication is on.
    // See docs/storage-interface.md.
    //
    // @unsafe - handles raw buffer pointers from transport layer
    void ShardReceiver::HandleNontxnWriteRequest(uint8_t reqType, char *reqBuf,
                                                 char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<nontxn_write_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_kv_response_t *>(respBuf);
        const bool is_get = (reqType == nontxnGetReqType);
        respLen = sizeof(client_kv_response_t) - max_value_length;
        resp->req_nr = req->req_nr;
        resp->vlen = 0;

        // Local copies, not the obj_key0/obj_v members: ClientTcpServer
        // workers may run this concurrently on several threads.
        std::string key(req->key_and_value, req->klen);
        std::string value(req->key_and_value + req->klen, req->vlen);
        bool op_result = false;
        std::string get_out;

        int status = RunNontxnOp(reqType, req->table_id, key, value,
                                 &op_result, &get_out);

        resp->status = status;
        if (status == ErrorCode::SUCCESS) {
            if (is_get) {
                // Value comes back with EXTRA_BITS already stripped by
                // the L3 get — clients must NOT strip again.
                ASSERT_LT(get_out.size(), max_value_length);
                resp->vlen = get_out.size();
                memcpy(resp->value, get_out.data(), get_out.size());
                respLen += get_out.size();
            } else {
                resp->vlen = 1;
                resp->value[0] = op_result ? 1 : 0;
                respLen += 1;
            }
        }
    }

    // See the declaration in server.h for the contract.
    // @unsafe - manipulates Sto thread-local transaction state
    int ShardReceiver::RunNontxnOp(uint8_t opType, uint16_t table_id,
                                   const std::string &key,
                                   const std::string &value,
                                   bool *op_result, std::string *get_out)
    {
        const bool is_get = (opType == nontxnGetReqType);

        // Leader-only writes (plan decision D3): a follower accepting a
        // non-txn write would apply it locally but never submit it to
        // the replication log — silent divergence. Fail loudly instead.
        // (Reads don't mutate, so gets are served regardless.)
        if (!is_get &&
            BenchmarkConfig::getInstance().getIsReplicated() &&
            !BenchmarkConfig::getInstance().getLeaderConfig()) {
            return ErrorCode::ERROR;
        }

        // The thread serving this op may hold a STAGED participant
        // transaction (from 2PC handlers: BatchLock stages writes +
        // locks; Validate/Install arrive as later RPCs). Running our
        // one-op txn now would clobber that staged state. Note the
        // idle-participant invariant: shard_reset() leaves helper
        // threads' txns in_progress but EMPTY between 2PC transactions
        // — that state is safe to borrow (restored below). Only a txn
        // with staged items is busy.
        if (TThread::txn && TThread::txn->has_staged_items()) {
            return ErrorCode::SERVER_BUSY;
        }

        auto* table = table_for(table_id);
        if (!table) {
            return ErrorCode::ERROR;  // table not found
        }

        int status = ErrorCode::SUCCESS;

        // Run the op as a clean mode-0 local commit: participant mode
        // (1) never invokes try_commit (Transaction.cc:242), and
        // leftover shard bits from earlier RPCs would trigger the
        // remote 2PC phases inside try_commit. Save/restore the
        // thread's coordination state around the op.
        int saved_mode = TThread::mode();
        unsigned saved_read_bits = TThread::readset_shard_bits;
        unsigned saved_write_bits = TThread::writeset_shard_bits;
        TThread::set_mode(0);
        TThread::readset_shard_bits = 0;
        TThread::writeset_shard_bits = 0;

        // Close out the idle participant txn (in_progress but empty —
        // guaranteed by the busy guard above) so the op's
        // Sto::start_transaction passes its mode-0 assertion. Aborting
        // an empty txn unwinds nothing.
        if (TThread::txn && TThread::txn->in_progress()) {
            TThread::txn->silent_abort();
        }

        try {
            switch (opType) {
            case nontxnPutReqType:
                *op_result = table->put(key, value);
                break;
            case nontxnInsertReqType:
                *op_result = table->insert(key, value);
                break;
            case nontxnRemoveReqType:
                *op_result = table->remove(lcdf::Str(key));
                break;
            case nontxnGetReqType:
                get_out->clear();
                *op_result = table->get(lcdf::Str(key), *get_out, std::string::npos);
                if (!*op_result)
                    status = ErrorCode::ABORT;  // key not found
                break;
            default:
                status = ErrorCode::ERROR;
                break;
            }
        } catch (...) {
            // The L3 non-txn ops retry OCC aborts internally; anything
            // escaping here is unexpected — surface as an error.
            status = ErrorCode::ERROR;
            if (TThread::txn && TThread::txn->in_progress())
                TThread::txn->silent_abort();
        }

        TThread::set_mode(saved_mode);
        TThread::readset_shard_bits = saved_read_bits;
        TThread::writeset_shard_bits = saved_write_bits;

        // Helper threads (mode 1) expect the idle-participant
        // invariant back: txn in_progress-and-empty (the state
        // shard_reset establishes). Mode-0 threads (ClientTcpServer
        // workers) must NOT get that: a lingering in_progress txn
        // would trip the next op's mode-0 start_transaction assert.
        if (saved_mode == 1 && !sharding_leases_enabled()) {
            db->shard_reset();
        }
        return status;
    }

    // ============================================================================
    // Client API Handlers (for decoupled client-server mode)
    // ============================================================================

    // @unsafe - native Rust owns the session/sequence/retained-result state.
    void ShardReceiver::ClientOperation(const MakoGatewayRequest& req, MakoGatewayResponse& resp)
    {
        mako_gateway_execute(static_cast<uint32_t>(TThread::get_shard_index()),
            &req, this, &ShardReceiver::ExecuteClientOperation, &resp);
    }
    // @unsafe - session controls share the caller-created operation stream.
    void ShardReceiver::BeginClientTransaction(const MakoGatewayRequest& req, MakoGatewayResponse& resp)
    {
        ClientOperation(req, resp);
    }
    // @unsafe - closes tracking only; prior operations are already committed.
    void ShardReceiver::CommitClientTransaction(const MakoGatewayRequest& req, MakoGatewayResponse& resp)
    {
        ClientOperation(req, resp);
    }
    // @unsafe - cannot undo separately committed operations.
    void ShardReceiver::RollbackClientTransaction(const MakoGatewayRequest& req, MakoGatewayResponse& resp)
    {
        ClientOperation(req, resp);
    }

    // @unsafe - transport supplies aligned, length-checked gateway buffers.
    void ShardReceiver::HandleClientBeginTxnRequest(char* req, char* resp, size_t& length)
    {
        BeginClientTransaction(*reinterpret_cast<MakoGatewayRequest*>(req),
                               *reinterpret_cast<MakoGatewayResponse*>(resp));
        length = offsetof(MakoGatewayResponse, value);
    }
    // @unsafe - transport supplies aligned, length-checked gateway buffers.
    void ShardReceiver::HandleClientCommitRequest(char* req, char* resp, size_t& length)
    {
        CommitClientTransaction(*reinterpret_cast<MakoGatewayRequest*>(req),
                                *reinterpret_cast<MakoGatewayResponse*>(resp));
        length = offsetof(MakoGatewayResponse, value);
    }
    // @unsafe - transport supplies aligned, length-checked gateway buffers.
    void ShardReceiver::HandleClientRollbackRequest(char* req, char* resp, size_t& length)
    {
        RollbackClientTransaction(*reinterpret_cast<MakoGatewayRequest*>(req),
                                  *reinterpret_cast<MakoGatewayResponse*>(resp));
        length = offsetof(MakoGatewayResponse, value);
    }
    // @unsafe - common point-operation gateway retains actual terminal results.
    void ShardReceiver::HandleClientPutRequest(char* req, char* resp, size_t& length)
    {
        auto& response = *reinterpret_cast<MakoGatewayResponse*>(resp);
        ClientOperation(*reinterpret_cast<MakoGatewayRequest*>(req), response);
        length = offsetof(MakoGatewayResponse, value) + response.value_length;
    }
    // @unsafe - retained read results include the exact returned bytes.
    void ShardReceiver::HandleClientGetRequest(char* req, char* resp, size_t& length)
    {
        HandleClientPutRequest(req, resp, length);
    }
    // @unsafe - executes real remove, not an empty put.
    void ShardReceiver::HandleClientDeleteRequest(char* req, char* resp, size_t& length)
    {
        HandleClientPutRequest(req, resp, length);
    }
    // @unsafe - retains the actual insert-result bit across lost replies.
    void ShardReceiver::HandleClientInsertRequest(char* req, char* resp, size_t& length)
    {
        HandleClientPutRequest(req, resp, length);
    }
    // @unsafe - discovery does not acquire a lease or consume a stream sequence.
    void ShardReceiver::HandleClientRouteRequest(char* req, char* resp, size_t& length)
    {
        ClientRoute(*reinterpret_cast<MakoGatewayRequest*>(req),
                    *reinterpret_cast<MakoGatewayResponse*>(resp));
        length = offsetof(MakoGatewayResponse, value);
    }

    // ============================================================================
    // End of Client API Handlers
    // ============================================================================

    /**
     * file: configuration fileName
     * par_id: to distinguish the running thread
     */
    ShardServer::ShardServer(std::string file, int clientShardIndex, int serverShardIndex, int par_id) : config(file),
                                                                                                         serverShardIndex(serverShardIndex),
                                                                                                         clientShardIndex(clientShardIndex),
                                                                                                         par_id(par_id)
    {
        shardReceiver = new mako::ShardReceiver(file);
    }

    void ShardServer::Register(abstract_db *dbX,
                               mako::HelperQueue *queueX,
                               mako::HelperQueue *queueY,
                               const map<int, abstract_ordered_index *> &open_tablesX)
    {
        db = dbX;
        queue = queueX;
        queue_response = queueY;
        open_tables_table_id = open_tablesX;
        shardReceiver->Register(db, open_tables_table_id);
    }

    void ShardServer::UpdateTable(int table_id, abstract_ordered_index *table)
    {
        if (table_id > 0 && table) {
            open_tables_table_id[table_id] = table;
        }
        shardReceiver->UpdateTableEntry(table_id, table);
    }

    void ShardServer::Run()
    {
        while (true) {
            queue->suspend();

            while (true) {
                void *handle;
                size_t msg_size;
                if (!queue->fetch_one_req(&handle, msg_size)) {
                    break;
                }
                if (!handle) {
                    Panic("the pointer is invalid, p:%s, rIdx:%d, wIdx:%d, count:%d",
                            (void*)handle,
                                queue->req_buffer_reader_idx,queue->req_buffer_writer_idx,
                                queue->req_cnt);

                }

                // Cast to transport-agnostic interface: the backend enqueued
                // a TransportRequestHandle* as an opaque token.
                mako::TransportRequestHandle* req_handle = reinterpret_cast<mako::TransportRequestHandle*>(handle);

                size_t msgLen = shardReceiver->ReceiveRequest(
                    req_handle->GetRequestType(),
                    req_handle->GetRequestBuffer(),
                    req_handle->GetResponseBuffer());

                // Enqueue response via transport-agnostic interface
                // This calls SrpcRequestHandle::EnqueueResponse().
                req_handle->EnqueueResponse(msgLen);
            }

            if (queue->should_stop()) {
                break;
            }
        }
    }
}
