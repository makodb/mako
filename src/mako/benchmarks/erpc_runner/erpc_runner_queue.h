#pragma once

#include "rpc.h"
#include <mutex>
#include <condition_variable>
#include "common.h"
#include "configuration.h"



/* used as tag */
struct MessageBuffer {
    DATA_TYPE sender_client_id;
    DATA_TYPE req_type;
    erpc::MsgBuffer req;
    erpc::MsgBuffer resp;
};


class ERPCMessageClientQueue {
public:
    ERPCMessageClientQueue(
        DATA_TYPE client_id, 
        DATA_TYPE shard_id, 
        DATA_TYPE replica_id
    );

    bool is_req_buffer_full();

    bool is_req_buffer_empty();

    bool is_resp_buffer_full();

    bool is_resp_buffer_empty();

    bool add_one_resp(MessageBuffer *tag);

    bool fetch_one_resp(void **resp, DATA_TYPE *req_type);

    bool free_one_resp();

    bool add_one_req(void *req, uint target_shards_bitset, DATA_TYPE req_type);

    bool fetch_one_req(void **req, uint *target_shards_bitset, DATA_TYPE *req_type);

    bool free_one_req();

    void set_rpc(erpc::Rpc<erpc::CTransport> *rpc);

private:
    DATA_TYPE client_id, shard_id, replica_id;

    void *req_buffer[ERPC_QUEUE_SIZE];
    uint target_shards_bitset_buffer[ERPC_QUEUE_SIZE];
    DATA_TYPE req_type_buffer[ERPC_QUEUE_SIZE];
    int req_buffer_reader_idx, req_buffer_writer_idx, req_cnt;
    std::mutex req_buffer_mutex;

    /* write by the eRPC runner, read by the eRPC client */
    MessageBuffer *resp_buffer[ERPC_QUEUE_SIZE];
    int resp_buffer_reader_idx, resp_buffer_writer_idx, resp_cnt;
    std::mutex resp_buffer_mutex;

    erpc::Rpc<erpc::CTransport> *rpc;
};

class ERPCMessageServerQueue {
public:
    ERPCMessageServerQueue(
        DATA_TYPE server_id, 
        DATA_TYPE shard_id, 
        DATA_TYPE replica_id
    );

    bool is_req_buffer_full();

    bool is_req_buffer_empty();

    bool is_resp_buffer_full();

    bool is_resp_buffer_empty();
    
    bool add_one_req(erpc::ReqHandle *req_handle);

    bool fetch_one_req(void **req, void **resp, DATA_TYPE *req_type);

    bool free_one_req(bool queue_it_to_resp);

    bool fetch_one_resp(erpc::ReqHandle **req_handle);

    bool free_one_resp();

    void suspend();

    void wakeup();

private:
    DATA_TYPE server_id, shard_id, replica_id;

    erpc::ReqHandle *req_buffer[ERPC_QUEUE_SIZE];
    int req_buffer_reader_idx, req_buffer_writer_idx, req_cnt;
    std::mutex req_buffer_mutex;

    erpc::ReqHandle *resp_buffer[ERPC_QUEUE_SIZE];
    int resp_buffer_reader_idx, resp_buffer_writer_idx, resp_cnt;
    std::mutex resp_buffer_mutex;

    /* used for wakeup*/
    std::condition_variable cv;
};

