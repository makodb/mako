#include "erpc_runner_queue.h"
#include <iostream>

using namespace std;


ERPCMessageClientQueue::ERPCMessageClientQueue(
    DATA_TYPE client_id, 
    DATA_TYPE shard_id, 
    DATA_TYPE replica_id
) {
    this->resp_buffer_reader_idx = 0;
    this->resp_buffer_writer_idx = 0;
    this->resp_cnt = 0;

    this->req_buffer_reader_idx = 0;
    this->req_buffer_writer_idx = 0;
    this->req_cnt = 0;        

    this->client_id = client_id;
    this->shard_id = shard_id;
    this->replica_id = replica_id;

    this->rpc = nullptr;
}

void ERPCMessageClientQueue::set_rpc(erpc::Rpc<erpc::CTransport> *rpc) {
    this->rpc = rpc;
}

bool ERPCMessageClientQueue::is_req_buffer_full() {
    return req_cnt == ERPC_QUEUE_SIZE;
}

bool ERPCMessageClientQueue::is_req_buffer_empty() {
    return req_cnt == 0;
}

bool ERPCMessageClientQueue::is_resp_buffer_full() {
    return resp_cnt == ERPC_QUEUE_SIZE;
}

bool ERPCMessageClientQueue::is_resp_buffer_empty() {
    return resp_cnt == 0;
}

bool ERPCMessageClientQueue::add_one_resp(MessageBuffer *tag) {
    if (is_resp_buffer_full())
        return false;

    cout << "[-] ERPCMessageClientQueue: enter add_one_resp" << endl;
    
    unique_lock<mutex> lock(resp_buffer_mutex);
    resp_buffer[resp_buffer_writer_idx] = tag;
    resp_cnt ++;
    resp_buffer_writer_idx ++;
    resp_buffer_writer_idx %= ERPC_QUEUE_SIZE;

    return true;
}

bool ERPCMessageClientQueue::fetch_one_resp(void **resp, DATA_TYPE *req_type) {
    if (is_resp_buffer_empty())
        return false;

    MessageBuffer *one_tag = resp_buffer[resp_buffer_reader_idx];
    *resp = one_tag->resp.buf_;
    *req_type = one_tag->req_type;

    return true;
}

bool ERPCMessageClientQueue::free_one_resp() {
    if (is_resp_buffer_empty())
        return false;

    MessageBuffer *one_tag = resp_buffer[resp_buffer_reader_idx];
    rpc->free_msg_buffer(one_tag->resp);
    rpc->free_msg_buffer(one_tag->req);
    delete one_tag;

    unique_lock<mutex> lock(resp_buffer_mutex);
    resp_cnt --;
    resp_buffer_reader_idx ++;
    resp_buffer_reader_idx %= ERPC_QUEUE_SIZE;

    return true;
}

bool ERPCMessageClientQueue::add_one_req(void *req, uint target_shards_bitset, DATA_TYPE req_type) {
    if (is_req_buffer_full())
        return false;

    req_buffer[req_buffer_writer_idx] = req;
    target_shards_bitset_buffer[req_buffer_writer_idx] = target_shards_bitset;
    req_type_buffer[req_buffer_writer_idx] = req_type;

    unique_lock<mutex> lock(req_buffer_mutex);
    req_cnt ++;
    req_buffer_writer_idx ++;
    req_buffer_writer_idx %= ERPC_QUEUE_SIZE;

    return true;
}

bool ERPCMessageClientQueue::fetch_one_req(void **req, uint *target_shards_bitset, DATA_TYPE *req_type) {
    if (is_req_buffer_empty())
        return false;

    *req = req_buffer[req_buffer_reader_idx];
    *target_shards_bitset = target_shards_bitset_buffer[req_buffer_reader_idx];
    *req_type = req_type_buffer[req_buffer_reader_idx];

    return true;
}

bool ERPCMessageClientQueue::free_one_req() {
    if (is_req_buffer_empty())
        return false;

    unique_lock<mutex> lock(req_buffer_mutex);
    req_cnt --;
    req_buffer_reader_idx ++;
    req_buffer_reader_idx %= ERPC_QUEUE_SIZE;

    return true;
}

// ------------------------------------------------------------------------------------------

ERPCMessageServerQueue::ERPCMessageServerQueue(
    DATA_TYPE server_id, 
    DATA_TYPE shard_id, 
    DATA_TYPE replica_id
) {
    this->resp_buffer_reader_idx = 0;
    this->resp_buffer_writer_idx = 0;
    this->resp_cnt = 0;

    this->req_buffer_reader_idx = 0;
    this->req_buffer_writer_idx = 0;
    this->req_cnt = 0;        

    this->server_id = server_id;
    this->shard_id = shard_id;
    this->replica_id = replica_id;
}

bool ERPCMessageServerQueue::is_req_buffer_full() {
    return req_cnt == ERPC_QUEUE_SIZE;
}

bool ERPCMessageServerQueue::is_req_buffer_empty() {
    return req_cnt == 0;
}

bool ERPCMessageServerQueue::is_resp_buffer_full() {
    return resp_cnt == ERPC_QUEUE_SIZE;
}

bool ERPCMessageServerQueue::is_resp_buffer_empty() {
    return resp_cnt == 0;
}

bool ERPCMessageServerQueue::add_one_req(erpc::ReqHandle *req_handle) {
    if (is_req_buffer_full())
        return false;

    req_buffer[req_buffer_writer_idx] = req_handle;
    unique_lock<mutex> lock(req_buffer_mutex);
    req_cnt ++;
    req_buffer_writer_idx ++;
    req_buffer_writer_idx %= ERPC_QUEUE_SIZE;

    return true;
}

bool ERPCMessageServerQueue::fetch_one_req(void **req, void **resp, DATA_TYPE *req_type) {
    if (is_req_buffer_empty())
        return false;

    cout << "[-] ERPCMessageServerQueue: enter fetch_one_req" << endl;

    auto *req_handle = req_buffer[req_buffer_reader_idx];
    *req = req_handle->get_req_msgbuf()->buf_;
    *resp = req_handle->pre_resp_msgbuf_.buf_;
    *req_type = req_handle->get_req_msgbuf()->get_req_type();

    return true;
}

bool ERPCMessageServerQueue::free_one_req(bool queue_it_to_resp) {
    if (is_req_buffer_empty())
        return false;

    cout << "[-] ERPCMessageServerQueue: enter free_one_req" << endl;

    if (queue_it_to_resp) {
        if (is_resp_buffer_full())
            return false;

        cout << "[-] ERPCMessageServerQueue: enter queue_it_to_resp" << endl;

        auto *req_handle = req_buffer[req_buffer_reader_idx];
        unique_lock<mutex> lock(req_buffer_mutex);
        req_cnt --;
        req_buffer_reader_idx ++;
        req_buffer_reader_idx %= ERPC_QUEUE_SIZE;

        unique_lock<mutex> lock2(resp_buffer_mutex);
        resp_buffer[resp_buffer_writer_idx] = req_handle;
        resp_cnt ++;
        resp_buffer_writer_idx ++;
        resp_buffer_writer_idx %= ERPC_QUEUE_SIZE;

        cout << "[-] ERPCMessageServerQueue: exit queue_it_to_resp" << endl;
    } else {
        auto *req_handle = req_buffer[req_buffer_reader_idx];
        unique_lock<mutex> lock(req_buffer_mutex);
        req_cnt --;
        req_buffer_reader_idx ++;
        req_buffer_reader_idx %= ERPC_QUEUE_SIZE;
    }

    return true;
}

bool ERPCMessageServerQueue::fetch_one_resp(erpc::ReqHandle **req_handle) {
    if (is_resp_buffer_empty())
        return false;

    cout << "[-] ERPCMessageServerQueue: enter fetch_one_resp" << endl;

    *req_handle = resp_buffer[resp_buffer_reader_idx];
    return true;
}

bool ERPCMessageServerQueue::free_one_resp() {
    if (is_resp_buffer_empty())
        return false;

    cout << "[-] ERPCMessageServerQueue: enter free_one_resp" << endl;

    unique_lock<mutex> lock(resp_buffer_mutex);
    resp_cnt --;
    resp_buffer_reader_idx ++;
    resp_buffer_reader_idx %= ERPC_QUEUE_SIZE;

    return true;
}

void ERPCMessageServerQueue::suspend() {
    mutex tmp;
    unique_lock<mutex> lock(tmp);
    cv.wait(lock, [this]{return !this->is_req_buffer_empty();});
}

void ERPCMessageServerQueue::wakeup() {
    cout << "[-] ERPCMessageServerQueue: enter wakeup" << endl;
    cv.notify_one();
    cout << "[-] ERPCMessageServerQueue: exit wakeup" << endl;
}
