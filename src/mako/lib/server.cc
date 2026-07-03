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
        open_tables_table_id = open_tables_table_idX;

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
        open_tables_table_id[table_id] = table;
    }

    // Message handlers.
    size_t ShardReceiver::ReceiveRequest(uint8_t reqType, char *reqBuf, char *respBuf)
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
        db->shard_reset();

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
        db->shard_reset();
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
        db->shard_reset();
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

    void ShardReceiver::HandleBatchLockMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<batch_lock_request_t *>(reqBuf);
        int status = ErrorCode::SUCCESS;

        uint16_t table_id, klen, vlen;
        char *k_ptr, *v_ptr;
        auto wrapper = BatchLockRequestWrapper(reqBuf);
        
        while (!wrapper.all_request_handled()) {
            wrapper.read_one_request(&k_ptr, &klen, &v_ptr, &vlen, &table_id);
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
                        open_tables_table_id[table_id]->shard_put(EncodeK(obj_key0, k_s_new), obj_v);
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
        char *k_ptr, *v_ptr;
        auto wrapper = BatchLockRequestWrapper(reqBuf);
        
        while (!wrapper.all_request_handled()) {
            wrapper.read_one_request(&k_ptr, &klen, &v_ptr, &vlen, &table_id);
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
                        open_tables_table_id[table_id]->shard_put(EncodeK(obj_key0, k_s_new), obj_v);
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
        char *k_ptr, *v_ptr;
        auto wrapper = BatchLockRequestWrapper(reqBuf);
        
        while (!wrapper.all_request_handled()) {
            wrapper.read_one_request(&k_ptr, &klen, &v_ptr, &vlen, &table_id);
            //string key(k_ptr, klen);
            obj_key0.assign(k_ptr, klen);
            //string value(v_ptr, vlen);
            obj_v.assign(v_ptr, vlen);

            if (table_id > 0) {
                try {
                    open_tables_table_id[table_id]->shard_put(obj_key0, obj_v);
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
                open_tables_table_id[table_id]->shard_put(obj_key0, obj_v);
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
                open_tables_table_id[req->table_id]->shard_scan(obj_key0, &obj_key1, c, s_arena.get());
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
            val = "this is a mocked value for erpc_client and erpc_server";
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
                   ret = open_tables_table_id[req->table_id]->shard_get(EncodeK(obj_key0, k_s_new), obj_v, std::string::npos);
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
            obj_v = "this is a mocked value for erpc_client and erpc_server";
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
                   ret = open_tables_table_id[req->table_id]->shard_get(EncodeK(obj_key0, k_s_new), obj_v, std::string::npos);
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
            obj_v = "this is a mocked value for erpc_client and erpc_server";
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
            auto it = open_tables_table_id.find(req->table_id);
            if (it == open_tables_table_id.end() || it->second == nullptr) {
                db->shard_abort_txn(nullptr);
                status = ErrorCode::ABORT;
            } else {
                try {
                    bool ret = it->second->shard_get(obj_key0, obj_v, std::string::npos);
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
            }
        } else {
            obj_v = "this is a mocked value for erpc_client and erpc_server";
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
    // See docs/mako-nontxn-api-plan.md.
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

        auto it = open_tables_table_id.find(table_id);
        if (it == open_tables_table_id.end() || it->second == nullptr) {
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
                *op_result = it->second->put(key, value);
                break;
            case nontxnInsertReqType:
                *op_result = it->second->insert(key, value);
                break;
            case nontxnRemoveReqType:
                *op_result = it->second->remove(lcdf::Str(key));
                break;
            case nontxnGetReqType:
                get_out->clear();
                *op_result = it->second->get(lcdf::Str(key), *get_out, std::string::npos);
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
        }

        TThread::set_mode(saved_mode);
        TThread::readset_shard_bits = saved_read_bits;
        TThread::writeset_shard_bits = saved_write_bits;

        // Helper threads (mode 1) expect the idle-participant
        // invariant back: txn in_progress-and-empty (the state
        // shard_reset establishes). Mode-0 threads (ClientTcpServer
        // workers) must NOT get that: a lingering in_progress txn
        // would trip the next op's mode-0 start_transaction assert.
        if (saved_mode == 1) {
            db->shard_reset();
        }
        return status;
    }

    // ============================================================================
    // Client API Handlers (for decoupled client-server mode)
    // ============================================================================

    // @unsafe - handles raw buffer pointers from transport layer
    void ShardReceiver::HandleClientBeginTxnRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<client_begin_txn_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_begin_txn_response_t *>(respBuf);
        respLen = sizeof(client_begin_txn_response_t);

        // Generate unique server-side transaction ID
        uint64_t server_txn_id = ++server_txn_counter_;

        // Store mapping from client txn_id to server txn_id
        // The client txn_id is composed of client_id and request number
        uint64_t client_txn_id = (req->client_id << 32) | req->req_nr;

        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            client_transactions_[client_txn_id] = server_txn_id;
        }

        resp->req_nr = req->req_nr;
        resp->txn_id = client_txn_id;  // Return the client txn_id for tracking
        resp->status = ErrorCode::SUCCESS;

        Debug("HandleClientBeginTxnRequest: client_id=%lu, txn_id=%lu",
              req->client_id, client_txn_id);
    }

    // @unsafe - handles raw buffer pointers from transport layer
    void ShardReceiver::HandleClientCommitRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<client_commit_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_commit_response_t *>(respBuf);
        respLen = sizeof(client_commit_response_t);

        int status = ErrorCode::SUCCESS;

        // Remove transaction from tracking
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            auto it = client_transactions_.find(req->txn_id);
            if (it != client_transactions_.end()) {
                client_transactions_.erase(it);
            } else {
                // Transaction not found - may have already been committed/rolled back
                status = ErrorCode::ERROR;
            }
        }

        resp->req_nr = req->req_nr;
        resp->status = status;

        Debug("HandleClientCommitRequest: txn_id=%lu, status=%d", req->txn_id, status);
    }

    // @unsafe - handles raw buffer pointers from transport layer
    // Note: Mako uses auto-commit semantics. Rollback only removes tracking.
    void ShardReceiver::HandleClientRollbackRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<client_commit_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_commit_response_t *>(respBuf);
        respLen = sizeof(client_commit_response_t);

        int status = ErrorCode::SUCCESS;

        // Remove transaction from tracking (operations already auto-committed)
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            auto it = client_transactions_.find(req->txn_id);
            if (it != client_transactions_.end()) {
                client_transactions_.erase(it);
            } else {
                // Transaction not found
                status = ErrorCode::ERROR;
            }
        }

        resp->req_nr = req->req_nr;
        resp->status = status;

        Debug("HandleClientRollbackRequest: txn_id=%lu, status=%d", req->txn_id, status);
    }

    // ============================================================================
    // Client Transaction API (for MakoClientService to use)
    // ============================================================================

    // @safe - Thread-safe with mutex
    uint64_t ShardReceiver::BeginClientTransaction(uint64_t client_id, uint32_t txn_counter)
    {
        // Generate txn_id using the same encoding as the client
        uint64_t txn_id = (client_id << 32) | txn_counter;

        // Store in transaction tracking map
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            // Map client txn_id to a server transaction counter (for internal tracking)
            client_transactions_[txn_id] = ++server_txn_counter_;
        }

        Debug("BeginClientTransaction: client_id=%lu, counter=%u, txn_id=%lu",
              client_id, txn_counter, txn_id);
        return txn_id;
    }

    // @safe - Thread-safe with mutex
    int ShardReceiver::CommitClientTransaction(uint64_t txn_id)
    {
        int status = ErrorCode::SUCCESS;

        // Remove transaction from tracking
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            auto it = client_transactions_.find(txn_id);
            if (it != client_transactions_.end()) {
                client_transactions_.erase(it);
            } else {
                // Transaction not found - may have already been committed/rolled back
                status = ErrorCode::ERROR;
            }
        }

        Debug("CommitClientTransaction: txn_id=%lu, status=%d", txn_id, status);
        return status;
    }

    // @safe - Thread-safe with mutex
    // Note: Mako uses auto-commit semantics where each Put/Get operation is
    // immediately committed. Rollback only removes the transaction from tracking.
    // It cannot undo already-committed operations.
    int ShardReceiver::RollbackClientTransaction(uint64_t txn_id)
    {
        int status = ErrorCode::SUCCESS;

        // Remove transaction from tracking
        // Note: Since operations are auto-committed, we cannot undo them.
        // Rollback just marks the transaction as aborted for tracking purposes.
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            auto it = client_transactions_.find(txn_id);
            if (it != client_transactions_.end()) {
                // Remove from tracking (operations already auto-committed)
                client_transactions_.erase(it);
            } else {
                // Transaction not found
                status = ErrorCode::ERROR;
            }
        }

        Debug("RollbackClientTransaction: txn_id=%lu, status=%d", txn_id, status);
        return status;
    }

    // @unsafe - handles raw buffer pointers from transport layer
    void ShardReceiver::HandleClientPutRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<client_kv_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_kv_response_t *>(respBuf);
        respLen = sizeof(client_kv_response_t) - max_value_length;  // No value in put response

        int status = ErrorCode::SUCCESS;

        // Verify transaction exists
        bool txn_exists = false;
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            txn_exists = (client_transactions_.find(req->txn_id) != client_transactions_.end());
        }

        if (!txn_exists) {
            resp->req_nr = req->req_nr;
            resp->vlen = 0;
            resp->status = ErrorCode::ERROR;
            return;
        }

        // Extract key and value from request (local copies — several
        // ClientTcpServer workers may run concurrently).
        std::string key(req->key_and_value, req->klen);
        std::string value(req->key_and_value + req->klen, req->vlen);

        // Self-contained non-txn put (one-op OCC txn that commits and
        // replicates) — NOT shard_put, which stages + locks a 2PC
        // participant write that nothing here would ever commit,
        // leaking the lock and never becoming visible or replicated.
        if (req->table_id > 0) {
            bool op_result = false;
            status = RunNontxnOp(nontxnPutReqType, req->table_id,
                                 key, value, &op_result, nullptr);
        }

        resp->req_nr = req->req_nr;
        resp->vlen = 0;
        resp->status = status;

        Debug("HandleClientPutRequest: txn_id=%lu, table=%d, key_len=%d, val_len=%d, status=%d",
              req->txn_id, req->table_id, req->klen, req->vlen, status);
    }

    // @unsafe - handles raw buffer pointers from transport layer
    void ShardReceiver::HandleClientGetRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<client_kv_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_kv_response_t *>(respBuf);

        int status = ErrorCode::SUCCESS;
        obj_v.clear();

        // Verify transaction exists
        bool txn_exists = false;
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            txn_exists = (client_transactions_.find(req->txn_id) != client_transactions_.end());
        }

        if (!txn_exists) {
            resp->req_nr = req->req_nr;
            resp->vlen = 0;
            resp->status = ErrorCode::ERROR;
            respLen = sizeof(client_kv_response_t) - max_value_length;
            return;
        }

        // Extract key from request (local copies — several
        // ClientTcpServer workers may run concurrently).
        std::string key(req->key_and_value, req->klen);
        std::string get_out;

        // Self-contained non-txn get — NOT shard_get, which stages a
        // read-set item in this thread's participant txn that a
        // decoupled client never cleans up. The value arrives with
        // EXTRA_BITS already stripped by the L3 get.
        if (req->table_id > 0) {
            bool op_result = false;
            status = RunNontxnOp(nontxnGetReqType, req->table_id,
                                 key, std::string(), &op_result, &get_out);
        }

        resp->req_nr = req->req_nr;
        resp->vlen = static_cast<uint16_t>(get_out.length());
        resp->status = status;
        if (!get_out.empty() && get_out.length() <= max_value_length) {
            memcpy(resp->value, get_out.c_str(), get_out.length());
        }
        respLen = sizeof(client_kv_response_t) - max_value_length + get_out.length();

        Debug("HandleClientGetRequest: txn_id=%lu, table=%d, key_len=%d, val_len=%d, status=%d",
              req->txn_id, req->table_id, req->klen, resp->vlen, status);
    }

    // @unsafe - handles raw buffer pointers from transport layer
    void ShardReceiver::HandleClientDeleteRequest(char *reqBuf, char *respBuf, size_t &respLen)
    {
        auto *req = reinterpret_cast<client_kv_request_t *>(reqBuf);
        auto *resp = reinterpret_cast<client_kv_response_t *>(respBuf);
        respLen = sizeof(client_kv_response_t) - max_value_length;  // No value in delete response

        int status = ErrorCode::SUCCESS;

        // Verify transaction exists
        bool txn_exists = false;
        {
            std::lock_guard<std::mutex> lock(client_txn_mutex_);
            txn_exists = (client_transactions_.find(req->txn_id) != client_transactions_.end());
        }

        if (!txn_exists) {
            resp->req_nr = req->req_nr;
            resp->vlen = 0;
            resp->status = ErrorCode::ERROR;
            return;
        }

        // Extract key from request (local copy — several
        // ClientTcpServer workers may run concurrently).
        std::string key(req->key_and_value, req->klen);

        // Real non-txn remove — the old path "deleted" by staging a
        // shard_put of an empty value that was never committed
        // (neither a delete nor visible). Absent key is not an error
        // here (blind-delete semantics, matching the txn'd handler).
        if (req->table_id > 0) {
            bool op_result = false;
            status = RunNontxnOp(nontxnRemoveReqType, req->table_id,
                                 key, std::string(), &op_result, nullptr);
        }

        resp->req_nr = req->req_nr;
        resp->vlen = 0;
        resp->status = status;

        Debug("HandleClientDeleteRequest: txn_id=%lu, table=%d, key_len=%d, status=%d",
              req->txn_id, req->table_id, req->klen, status);
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
                erpc::ReqHandle *handle;
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

                // Cast to transport-agnostic interface
                // The backend has enqueued a TransportRequestHandle* (cast to erpc::ReqHandle*)
                mako::TransportRequestHandle* req_handle = reinterpret_cast<mako::TransportRequestHandle*>(handle);

                // Use abstract interface methods instead of eRPC-specific API
                size_t msgLen = shardReceiver->ReceiveRequest(
                    req_handle->GetRequestType(),
                    req_handle->GetRequestBuffer(),
                    req_handle->GetResponseBuffer());

                // Enqueue response via transport-agnostic interface
                // This will call ErpcRequestHandle::EnqueueResponse() or RrrRequestHandle::EnqueueResponse()
                req_handle->EnqueueResponse(msgLen);
            }

            if (queue->should_stop()) {
                break;
            }
        }
    }
}
