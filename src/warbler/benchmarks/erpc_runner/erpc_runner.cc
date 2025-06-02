#include "erpc_runner.h"
#include <iostream>
#include <ctime>
#include <cstdio>

using namespace std;

// A basic session management handler that expects successful responses
static void basic_sm_handler(int session_num, erpc::SmEventType sm_event_type,
                             erpc::SmErrType sm_err_type, void *context) {
    // Assert(sm_err_type == erpc::SmErrType::kNoError);
    if (!(sm_event_type == erpc::SmEventType::kConnected ||
          sm_event_type == erpc::SmEventType::kDisconnected)) {
        throw std::runtime_error("Received unexpected SM event.");
    }
}

static void receive_resp(void *context, void *tag) {
    cout << "[-] ERPCRunner: enter receive_resp" << endl;

    auto *message_buffer = (MessageBuffer *) tag;
    auto *app_context = (AppContext *) context;
    auto sender_client_id = message_buffer->sender_client_id;
    auto *client_queue = app_context->client_holder->find(sender_client_id)->second;

    bool success = false;
    while (!success) {
        success = client_queue->add_one_resp(message_buffer);
    }
}

static void receive_req(erpc::ReqHandle *req_handle, void *context) {
    cout << "[-] ERPCRunner: enter receive_req" << endl;

    auto *target_server_id_reader = (TargetServerIDReader *)req_handle->get_req_msgbuf()->buf_;
    auto target_server_id = target_server_id_reader->targert_server_id;
    auto *app_context = (AppContext *) context;
    auto *server_queue = app_context->server_holder->find(target_server_id)->second;

    bool success = false;
    cout << "[-] ERPCRunner: add one req to server_queue" << endl;
    while (!success) {
        success = server_queue->add_one_req(req_handle);
    }
    cout << "[-] ERPCRunner: wakeup server" << endl;
    server_queue->wakeup();
};

ERPCRunner::ERPCRunner(const string &file, DATA_TYPE erpc_runner_id, DATA_TYPE replica_id, DATA_TYPE shard_id) {
    this->config = Configuration(file);
    this->erpc_runner_id = erpc_runner_id;
    this->replica_id = replica_id;
    this->shard_id = shard_id;
    this->app_context.client_holder = &client_holder;
    this->app_context.server_holder = &server_holder;
    this->stopped = false;
    this->is_loading_finished = false;

    auto uri = config.get_erpc_runner_uri(
        shard_id,
        replica_id,
        erpc_runner_id
    );

    nexus = new erpc::Nexus(uri);
    for (int i = 0; i < REQ_NUM; i++)
        nexus->register_req_func(i, receive_req);

    rpc = new erpc::Rpc<erpc::CTransport>(
            nexus, 
            &app_context /* context */, 
            this->erpc_runner_id /* rpc_id */, 
            basic_sm_handler, 0 /* phy port */
        );

    set_color(F_PURPLE);
    cout << "[+] Initialized erpc runner | uri:" << uri << " replica_id:" << (int)replica_id << " shard_id:" << (int)shard_id << " erpc_runner_id:" << (int)erpc_runner_id << endl;
    reset_color();
}

DATA_TYPE ERPCRunner::get_session(DATA_TYPE target_shard_id, DATA_TYPE target_server_id) {
    auto session_key = pair<int, int>(target_shard_id, target_server_id);
    const auto iter = sessions.find(session_key);
    if (iter == sessions.end()) {
        unique_lock<mutex> lock(erpc_sessions_mutex);
        auto target_erpc_runner_id = config.get_erpc_runner_id_server_belongs_to(target_server_id);
        auto uri = config.get_erpc_runner_uri(target_shard_id, this->replica_id, target_erpc_runner_id);
        auto session_id = rpc->create_session(uri, target_erpc_runner_id);
        while (!rpc->is_connected(session_id))
            rpc->run_event_loop_once();
        sessions[session_key] = session_id;
        return session_id;
    }
    return iter->second;
}

ERPCMessageClientQueue* ERPCRunner::register_client(DATA_TYPE client_id, DATA_TYPE replica_id, DATA_TYPE shard_id) {
    auto target_erpc_runner_id = config.get_erpc_runner_id_client_belongs_to(client_id);

    if (replica_id != this->replica_id or shard_id != this->shard_id or target_erpc_runner_id != this->erpc_runner_id) {
        
    }

    if (client_holder.find(client_id) != client_holder.end()) {

    }

    unique_lock<mutex> lock(client_holder_mutex);
    auto *it = new ERPCMessageClientQueue(client_id, shard_id, replica_id);
    it->set_rpc(this->rpc);
    client_holder[client_id] = it;
    return it;
}

ERPCMessageServerQueue* ERPCRunner::register_server(DATA_TYPE server_id, DATA_TYPE replica_id, DATA_TYPE shard_id) {
    auto target_erpc_runner_id = config.get_erpc_runner_id_server_belongs_to(server_id);

    if (replica_id != this->replica_id or shard_id != this->shard_id or target_erpc_runner_id != this->erpc_runner_id) {
        
    }

    if (server_holder.find(server_id) != server_holder.end()) {

    }

    unique_lock<mutex> lock(server_holder_mutex);
    server_holder[server_id] = new ERPCMessageServerQueue(server_id, shard_id, replica_id);
    return server_holder[server_id];
}

void ERPCRunner::send_cached_request() {
    for (auto it: client_holder) {
        auto client_id = it.first;
        auto *client_queue = it.second;

        void *req;
        uint target_shards_bitset;
        DATA_TYPE req_type;

        while (!client_queue->is_req_buffer_empty()) {
            cout << "[-] ERPCRunner: handle cached request" << endl;

            client_queue->fetch_one_req(&req, &target_shards_bitset, &req_type);
            DATA_TYPE target_shard_id = -1;

            cout << "[-] ERPCRunner: target_shards_biset:" << target_shards_bitset << endl;

            while (target_shards_bitset) {
                target_shard_id ++;
                if (target_shards_bitset % 2 == 0) {
                    target_shards_bitset /= 2;
                    continue;
                }
                target_shards_bitset /= 2;

                cout << "[-] ERPCRunner: send request to shard " << (int)target_shard_id << endl;
                
                auto msg_size = calculate_req_msg_size(req, req_type);
                auto *tag = new MessageBuffer;
                
                tag->req_type = req_type;
                tag->sender_client_id = client_id;
                tag->req = rpc->alloc_msg_buffer_or_die(msg_size);
                tag->resp = rpc->alloc_msg_buffer_or_die(rpc->get_max_data_per_pkt());

                auto *target_server_id_reader = (TargetServerIDReader *) req;
                auto target_server_id = config.get_target_server_id(
                    this->shard_id, client_id, target_shard_id
                );
                target_server_id_reader->targert_server_id = target_server_id;
                memcpy((char *)tag->req.buf_, (char *)req, msg_size);

                rpc->enqueue_request(
                    this->get_session(target_shard_id, target_server_id),
                    req_type,
                    &tag->req,
                    &tag->resp,
                    receive_resp,
                    tag
                );
            }

            client_queue->free_one_req();
        }
    }
}

void ERPCRunner::send_cached_response() {
    for (auto it: server_holder) {
        auto server_id = it.first;
        auto *server_queue = it.second;
        erpc::ReqHandle *req_handle;

        while (!server_queue->is_resp_buffer_empty()) {
            cout << "[-] ERPCRunner: handle cached response" << endl;

            server_queue->fetch_one_resp(&req_handle);

            auto &resp = req_handle->pre_resp_msgbuf_;
            auto msg_size = calculate_resp_msg_size((void *)resp.buf_, req_handle->get_req_msgbuf()->get_req_type());
            rpc->resize_msg_buffer(&resp, msg_size);
            rpc->enqueue_response(req_handle, &resp);

            server_queue->free_one_resp();

            cout << "[-] ERPCRunner: send one cached response" << endl;
        }
    }
}

void ERPCRunner::stop() {
    this->stopped = true;
}

void ERPCRunner::loading_finished() {
    this->is_loading_finished = true;
}

void ERPCRunner::run() {
    clock_t block_start, block_end;
    block_start = clock();
    while (!this->is_loading_finished) {
        block_end = clock(); /* somehow necessary */
    }

    while (!stopped) {
        this->send_cached_request();
        this->send_cached_response();
        rpc->run_event_loop_once();
    }
}
