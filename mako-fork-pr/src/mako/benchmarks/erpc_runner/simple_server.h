#include "common.h"
#include "erpc_runner_queue.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <vector>
#include <ctime>

using namespace std;

class Server {
public:
    Server(
        DATA_TYPE replica_id,
        DATA_TYPE shard_id,
        DATA_TYPE server_id,
        ERPCMessageServerQueue *message_queue
    ) {
        this->replica_id = replica_id;
        this->shard_id = shard_id;
        this->server_id = server_id;
        this->message_queue = message_queue;
        this->stopped = false;

        set_color(F_RED);
        cout << "[+] Initialized server | replica_id:" << (int)replica_id << " shard_id:" << (int)shard_id << " sever_id:" << (int)server_id << endl;
        reset_color();
    }

    void stop() {
        this->stopped = true;
    }

    bool request_callback(void *req, void *resp, DATA_TYPE req_type) {
        cout << "[-] Server: enter request_callback" << endl;

        switch (req_type)
        {
        case TYPE_A:
        {
            if (0) /* anomaly request */
                return false;

            auto tagged_req = (RequestA *)req;
            ResponseA tagged_resp;
            tagged_resp.req_nr = tagged_req->req_nr;
            tagged_resp.data[0] = tagged_req->data[0] + 10;
            tagged_resp.msg_len = 1;
            memcpy((char *)resp, (char *)&tagged_resp, calculate_resp_msg_size(&tagged_resp, req_type));

            cout << "[-] Server: memcpy over" << endl;

            return true;
            break;
        }
        case TYPE_B:
        {
            if (0) /* anomaly request */
                return false;

            auto tagged_req = (RequestB *)req;
            ResponseB tagged_resp;
            tagged_resp.req_nr = tagged_req->req_nr;
            tagged_resp.data[0] = tagged_req->data[0] + 10;
            tagged_resp.msg_len = 1;
            memcpy((char *)resp, (char *)&tagged_resp, calculate_resp_msg_size(&tagged_resp, req_type));

            return true;
            break;
        }
        default:
            break;
        }
        return false;
    }

    void run() {
        while (!stopped) {
            this->message_queue->suspend();
            cout << "[-] Server: waken up" << endl;
            while (!this->message_queue->is_req_buffer_empty()) {
                void *req, *resp;
                DATA_TYPE req_type;
                this->message_queue->fetch_one_req(&req, &resp, &req_type);
                request_callback(req, resp, req_type);
                this->message_queue->free_one_req(true);
            }
            cout << "[-] Server: back to sleep" << endl;
        }
    }

private:
    DATA_TYPE replica_id, shard_id, server_id;
    ERPCMessageServerQueue *message_queue;

    bool stopped;
};

