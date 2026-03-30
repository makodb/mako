#include "rpc.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <map>

using namespace std;

#define SERVER_NUM 2
#define CLIENT_NUM 2

char *SERVER_URI[] = {
    "127.0.0.1:31000", "127.0.0.1:32000", "127.0.0.1:33000", "127.0.0.1:34000"
};

char *CLIENT_URI[] = {
    "127.0.0.1:35000", "127.0.0.1:36000", "127.0.0.1:37000", "127.0.0.1:38000"
};

/* used as context */
struct AppContext {
    struct ServerPlusClient *source;
};

/* used as tag */
struct MessageBuffer {
    erpc::MsgBuffer request;
    erpc::MsgBuffer response;
};

void sm_handler(int, erpc::SmEventType, erpc::SmErrType, void *) {}

void print(string msg) {
    static mutex mtx;
    unique_lock<mutex> lock(mtx);
    cout << msg << endl;
}

struct ServerPlusClient {
    int id;
    bool is_server;
    erpc::Rpc<erpc::CTransport> *rpc;
    erpc::Nexus *nexus;
    map<pair<int, int>, int> sessions; /* record the session id of each communication between server/client */
    bool client_block;
    struct AppContext app_context;


    static void receive_request(erpc::ReqHandle *req_handle, void *context) {
        string request_data = string((char *)(req_handle->get_req_msgbuf()->buf_));
        AppContext *app_context = (AppContext *) context;
        string response_data = request_data + " [+] Message received at Server " + to_string(app_context->source->id);

        auto &response_buffer = req_handle->pre_resp_msgbuf_;
        memcpy((char *)response_buffer.buf_, response_data.c_str(), response_data.size());
        app_context->source->rpc->resize_msg_buffer(&response_buffer, response_data.size());
        app_context->source->rpc->enqueue_response(req_handle, &response_buffer);
    }

    static void receive_response(void *context, void *message_buffer) {
        MessageBuffer *erpc_cache = (MessageBuffer *) message_buffer;
        AppContext *app_context = (AppContext *) context;
        auto resp = (char *)(erpc_cache->response.buf_);

        print(string(resp));

        app_context->source->rpc->free_msg_buffer(erpc_cache->response);
        app_context->source->rpc->free_msg_buffer(erpc_cache->request);
        app_context->source->client_block = false;
    }

    int get_session(int target_server_id) {
        auto session_key = pair<int, int>(id, target_server_id);
        const auto iter = sessions.find(session_key);
        if (iter == sessions.end()) {
            auto session_id = rpc->create_session(SERVER_URI[target_server_id], target_server_id);
            while (!rpc->is_connected(session_id))
                rpc->run_event_loop_once();
            sessions[session_key] = session_id;
            return session_id;
        }
        return iter->second;
    }
    
    ServerPlusClient(int id, bool is_server) {
        this->id = id;
        this->is_server = is_server;
        this->app_context.source = this;

        if (is_server) {
            nexus = new erpc::Nexus(string(SERVER_URI[id]));
            nexus->register_req_func(1, receive_request);
            // this->id += CLIENT_NUM;
        }
        else
            nexus = new erpc::Nexus(string(CLIENT_URI[id]));

        /* The context passed by the event loop to user callbacks */
        rpc = new erpc::Rpc<erpc::CTransport>(
            nexus, &app_context /* context */, this->id /* rpc_id */, sm_handler, 0 /* phy port */
        );

        if (is_server)
            print("[+] Initialized server " + to_string(this->id));
        else
            print("[+] Initialized client " + to_string(this->id));
    }

    void server_run() {
        while (1) {
            rpc->run_event_loop_once();
        }
    }

    void client_run() {
        int msg_cnt = 0;
        while (1) {
            msg_cnt ++;

            for (int server_id = 0; server_id < SERVER_NUM; server_id++) {

                string data = "[+] Message sent from Client " + to_string(this->id);

                auto *erpc_chache = new MessageBuffer;
                erpc_chache->request = rpc->alloc_msg_buffer_or_die(data.size());
                erpc_chache->response = rpc->alloc_msg_buffer_or_die(rpc->get_max_data_per_pkt());
                memcpy((char *)erpc_chache->request.buf_, data.c_str(), data.size());

                client_block = true;

                rpc->enqueue_request(
                    get_session(server_id),
                    1, /* req_type */
                    &erpc_chache->request,
                    &erpc_chache->response,
                    receive_response,
                    erpc_chache /* tag */
                );

                while (client_block)
                    rpc->run_event_loop_once();

                this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void run() {
        switch (this->is_server)
        {
        case true:
            server_run();
            break;
        case false:
            client_run();
            break;
        default:
            break;
        }
    }
};

void run_server(int server_id) {
    ServerPlusClient server(server_id, true);
    server.run();
}

void run_client(int client_id) {
    ServerPlusClient client(client_id, false);
    client.run();
}

int main()
{
    thread server_threads[SERVER_NUM], client_threads[CLIENT_NUM];

    for (int i = 0; i < SERVER_NUM; i++) {
        server_threads[i] = thread(run_server, i);
        this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (int i = 0; i < CLIENT_NUM; i++)
        client_threads[i] = thread(run_client, i);

    for (int i = 0; i < SERVER_NUM; i++) 
        server_threads[i].join();

    for (int i = 0; i < CLIENT_NUM; i++)
        client_threads[i].join();
}

// sudo make erpc_runner && sudo ./out-perf.masstree/benchmarks/erpc_runner_test
