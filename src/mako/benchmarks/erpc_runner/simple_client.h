#include "common.h"
#include "erpc_runner_queue.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <vector>
#include <ctime>

using namespace std;

class Client {
public:
    Client(
        DATA_TYPE replica_id,
        DATA_TYPE shard_id,
        DATA_TYPE client_id,
        ERPCMessageClientQueue *message_queue
    ) {
        this->req_nr = 0;
        this->replica_id = replica_id;
        this->shard_id = shard_id;
        this->client_id = client_id;
        this->message_queue = message_queue;
    }

    void send_request_type_A(uint target_shards_bitset) {
        cout << "[-] Client: enter send_request_type_A" << endl;

        RequestA request;
        request.data[0] = (char) client_id;
        request.msg_len = 1;
        request.req_nr = ++this->req_nr;

        resp_code.clear();
        num_resp_waiting = count_bit(target_shards_bitset);
        add_one_req_till_success((void*)&request, target_shards_bitset, TYPE_A);
        check_response();

        bool check = (resp_code.size() == num_resp_waiting);
        for (auto code: resp_code)
            check &= code;
        
        cout << "[-] Client: receive response " << (check ? "successfully" : "failure") << endl;
    }

    void send_request_type_B(uint target_shards_bitset) {
        RequestB request;
        request.data[0] = (char) client_id;
        request.msg_len = 1;
        request.req_nr = ++this->req_nr;

        resp_code.clear();
        num_resp_waiting = count_bit(target_shards_bitset);
        add_one_req_till_success((void*)&request, target_shards_bitset, TYPE_B);
        check_response();
    }


private:
    bool response_callback(void *resp, DATA_TYPE req_type) {
        cout << "[-] Client: enter response_callback" << endl;

        switch (req_type)
        {
        case TYPE_A:
        {
            cout << "[-] Client: enter TYPE_A response handle" << endl;

            auto tagged_resp = (ResponseA *)resp;
            if (tagged_resp->req_nr != this->req_nr) return false;

            if (tagged_resp->data[0] == this->client_id + 10) {
                resp_code.push_back(SUCCESS);
                return this->resp_code.size() == this->num_resp_waiting;
            }
            return false;
            break;
        }
        case TYPE_B:
        {
            auto tagged_resp = (ResponseB *)resp;
            if (tagged_resp->req_nr != this->req_nr) return false;

            if (tagged_resp->data[0] == this->client_id + 20) {
                resp_code.push_back(SUCCESS);
                return this->resp_code.size() == this->num_resp_waiting;
            }
            return false;
            break;
        }
        default:
            break;
        }
        return false;
    }

    bool check_response() {
        cout << "[-] Client: enter check_response" << endl;

        clock_t start, cur;
        start = clock();

        while (true) {
            cur = clock();

            if (double(cur - start) / CLOCKS_PER_SEC > TIMEOUT)
                break;

            while (!message_queue->is_resp_buffer_empty()) {
                cout << "[-] Client: enter check_response" << endl;
                void *resp; 
                DATA_TYPE req_type;
                message_queue->fetch_one_resp(&resp, &req_type);
                message_queue->free_one_resp();
                if (response_callback(resp, req_type))
                    goto end;
            }
        }
        return false;
        end:
            return true;
    }

    void add_one_req_till_success(void *req, uint target_shards_bitset, DATA_TYPE req_type) {
        cout << "[-] Client: enter add_one_req_till_success" << endl;

        bool success = false;
        while (!success)
            success = message_queue->add_one_req(req, target_shards_bitset, req_type);
    }

    uint req_nr;
    DATA_TYPE replica_id, shard_id, client_id;
    ERPCMessageClientQueue *message_queue;

    vector<int> resp_code;
    int num_resp_waiting;
};
