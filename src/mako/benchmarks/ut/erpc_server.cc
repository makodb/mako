#include <iostream>
#include "rpc.h"
#include "rpc_constants.h"
#include "consts.h"

using namespace std;

int counter = 0;
erpc::Rpc<erpc::CTransport> *rpc;
erpc::MsgBuffer resp;

int counter1 = 0;
erpc::Rpc<erpc::CTransport> *rpc1;
erpc::MsgBuffer resp1;

void transport_request(erpc::ReqHandle *req_handle, void *) {
    basic_req_t *rvalue = reinterpret_cast<basic_req_t *>(req_handle->get_req_msgbuf()->buf_);
    counter ++;
    auto &resp = req_handle->pre_resp_msgbuf_;
    std::string v="request was processed;counter:"+to_string(counter)+";req_nr:"+to_string(rvalue->req_nr)+";tid:"+to_string(rvalue->tid);
    rpc->resize_msg_buffer(&resp, strlen(v.data()));
    sprintf(reinterpret_cast<char *>(resp.buf_), v.data());
    //std::cout<<"send response:"<<rvalue->req_nr<<std::endl;
    rpc->enqueue_response(req_handle, &resp);
    //std::cout<<"send response-2:"<<rvalue->req_nr<<std::endl;
}

void transport_request1(erpc::ReqHandle *req_handle, void *) {
    basic_req_t *rvalue = reinterpret_cast<basic_req_t *>(req_handle->get_req_msgbuf()->buf_);
    counter1 ++;
    auto &resp1 = req_handle->pre_resp_msgbuf_;
    std::string v="request was processed;counter:"+to_string(counter1)+";req_nr:"+to_string(rvalue->req_nr)+";tid:"+to_string(rvalue->tid);
    rpc1->resize_msg_buffer(&resp1, strlen(v.data()));
    sprintf(reinterpret_cast<char *>(resp1.buf_), v.data());
    rpc1->enqueue_response(req_handle, &resp1);
}

void start_server(int thread_id) {
    if (thread_id==0) {
        std::string server_uri = "127.0.0.1:" + std::to_string(serverPort);
        std::cout<<"server_uri:"<<server_uri<<std::endl;
        erpc::Nexus nexus(server_uri);

        for (int i=0; i<10; i++) {
            nexus.register_req_func(i, transport_request, erpc::ReqFuncType::kForeground);
        }
        rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 100, nullptr, 0);
        while (true) {
            rpc->run_event_loop_once();
        }
    } else {
        std::string server_uri = "127.0.0.1:" + std::to_string(serverPort+1);
        std::cout<<"server_uri:"<<server_uri<<std::endl;
        erpc::Nexus nexus(server_uri);

        for (int i=0; i<10; i++) {
            nexus.register_req_func(i, transport_request1, erpc::ReqFuncType::kForeground);
        }

        rpc1 = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 100, nullptr, 0);
        while (true) {
            rpc1->run_event_loop_once();
        }
    }
}

int main() {
    for (int i=0;i<2;i++) {
        auto t=std::thread(start_server, i);
        t.detach();
    }

    sleep(10000);
}