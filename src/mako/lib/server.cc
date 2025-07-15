#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include "lib/fasttransport.h"
#include "lib/timestamp.h"
#include "lib/server.h"
#include "lib/common.h"
#include "benchmarks/sto/Interface.hh"
#include "benchmarks/common.h"
#include "benchmarks/bench.h"
#include "benchmarks/tpcc.h"
#include <x86intrin.h>
#include "deptran/s_main.h"
#include "benchmarks/sto/sync_util.hh"

std::function<int()> ss_callback_ = nullptr;
void register_sync_util_ss(std::function<int()> cb) {
    ss_callback_ = cb;
}

namespace srolis
{
    using namespace std;

    ShardReceiver::ShardReceiver(std::string file) : config(file)
    {
        current_term = 0;
    }

    void ShardReceiver::Register(abstract_db *dbX,
                                 const map<string, abstract_ordered_index *> &open_tablesX,
                                 const map<int, abstract_ordered_index *> &open_tables_table_idX,
                                 const map<string, vector<abstract_ordered_index *>> &partitionsX,
                                 const map<string, vector<abstract_ordered_index *>> &dummy_partitionsX)
    {
        db = dbX;
        open_tables = open_tablesX;
        open_tables_table_id = open_tables_table_idX;
        partitions = partitionsX;
        dummy_partitions = dummy_partitionsX;

        txn_obj_buf.reserve(str_arena::MinStrReserveLength);
        txn_obj_buf.resize(db->sizeof_txn_object(0));
        db->shard_reset(); // initialize
        obj_key0.reserve(128);
        obj_key1.reserve(128);
        obj_v.reserve(256);
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
            std::vector<uint32_t> ret;
            decode_vec_uint32(req->value, TThread::get_nshards()).swap(ret);
            db->shard_install(ret);
            db->shard_serialize_util(ret);
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
        // unsigned int ui;
		// result = __rdtscp(&ui);
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
                    for (int i=0; i<srolis::mega_batch_size; i++) {
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
                    for (int i=0; i<srolis::mega_batch_size; i++) {
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
                for (int i=0; i<srolis::mega_batch_size; i++) {
                   k_s_new.i_id = base_ol_i_id + i;
                   ret = open_tables_table_id[req->table_id]->shard_get(EncodeK(obj_key0, k_s_new), obj_v);
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
        //Warning("the remoteGET,len:%d,table_id:%d,keys:%s,key_len:%d,val_len:%d",obj_v.length(),req->table_id,srolis::getStringAsBit(obj_key0).c_str(),req->len,obj_v.length());
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
        //int tol_len = srolis::mega_batch_size* srolis::size_per_stock_value;  
        int tol_len = 1* srolis::size_per_stock_value;  
        std::string c_v;
        int offset = 0;
        c_v.resize(tol_len);

        int status = ErrorCode::SUCCESS;
        if (req->table_id > 0) {
            try {
                bool ret = true;
                int base_ol_i_id = k_s->s_i_id;
                stock::key k_s_new(*k_s); 
                for (int i=0; i<srolis::mega_batch_size; i++) {
                   k_s_new.s_i_id = base_ol_i_id + i;
                   ret = open_tables_table_id[req->table_id]->shard_get(EncodeK(obj_key0, k_s_new), obj_v);
                   memcpy((char*)c_v.c_str()+offset,obj_v.c_str(),srolis::size_per_stock_value);
                   //offset += srolis::size_per_stock_value;
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
        //Warning("the remoteGET,len:%d,table_id:%d,keys:%s,key_len:%d,val_len:%d",obj_v.length(),req->table_id,srolis::getStringAsBit(obj_key0).c_str(),req->len,obj_v.length());
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
            try {
                bool ret = open_tables_table_id[req->table_id]->shard_get(obj_key0, obj_v);
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
        respLen = sizeof(get_response_t) - max_value_length + obj_v.length();
        ALWAYS_ASSERT(max_value_length>=obj_v.length());
        resp->status = (current_term > req->req_nr % 10)? ErrorCode::ABORT: status; // If a reqest comes from old epoch, reject it.;
        resp->req_nr = req->req_nr;
        resp->len = obj_v.length();
        //Warning("the remoteGET,len:%d,table_id:%d,keys:%s,key_len:%d,val_len:%d",obj_v.length(),req->table_id,srolis::getStringAsBit(obj_key0).c_str(),req->len,obj_v.length());
        memcpy(resp->value, obj_v.c_str(), obj_v.length());
#endif
    }

    /**
     * file: configuration fileName
     * par_id: to distinguish the running thread
     */
    ShardServer::ShardServer(std::string file, int clientShardIndex, int serverShardIndex, int par_id) : config(file),
                                                                                                         serverShardIndex(serverShardIndex),
                                                                                                         clientShardIndex(clientShardIndex),
                                                                                                         par_id(par_id)
    {
        shardReceiver = new srolis::ShardReceiver(file);
    }

    void ShardServer::Register(abstract_db *dbX,
                               srolis::HelperQueue *queueX,
                               srolis::HelperQueue *queueY,
                               const map<string, abstract_ordered_index *> &open_tablesX,
                               const map<string, vector<abstract_ordered_index *>> &partitionsX,
                               const map<string, vector<abstract_ordered_index *>> &dummy_partitionsX)
    {
        db = dbX;
        queue = queueX;
        queue_response = queueY;
        open_tables = open_tablesX;
        partitions = partitionsX;
        dummy_partitions = dummy_partitionsX;

        for (auto &t : open_tablesX) {
            open_tables_table_id[t.second->get_table_id()] = t.second;
        }

        shardReceiver->Register(db, open_tables, open_tables_table_id, partitions, dummy_partitions);
    }

    void ShardServer::Run()
    {
        while(true) {
            queue->suspend();

            while (!queue->is_req_buffer_empty()) {
                erpc::ReqHandle *handle;
                size_t msg_size;
                queue->fetch_one_req(&handle, msg_size); /* for the requst, msg_size is 0*/
                if (!handle) {
                    Panic("the pointer is invalid, p:%s, rIdx:%d, wIdx:%d, count:%d",
                            (void*)handle,
                                queue->req_buffer_reader_idx,queue->req_buffer_writer_idx, 
                                queue->req_cnt);

                }
                size_t msgLen = shardReceiver->ReceiveRequest(handle->get_req_msgbuf()->get_req_type(),
                                               reinterpret_cast<char *>(handle->get_req_msgbuf()->buf_),
                                               reinterpret_cast<char *>(handle->pre_resp_msgbuf_.buf_));
                queue_response->add_one_req(handle, msgLen);
                //queue->free_one_req();
            }
        }
    }
}
