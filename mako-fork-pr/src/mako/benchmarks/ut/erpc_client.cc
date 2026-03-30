#include <iostream>
#include "rpc.h"
#include "rpc_constants.h"
#include "util/numautils.h"
#include "consts.h"
#include "benchmarks/sto/sync_util.hh"
#include "lib/common.h"

using namespace std;

//static int num_clients = 12;
bool ut_running = true;
int ut_run_time = 10; // seconds

void sm_handler(int, erpc::SmEventType, erpc::SmErrType, void *) {}

void transport_response(void *_context, void *_tag) {
    auto *rt = reinterpret_cast<ut_req_tag_t *>(_tag);
    auto *c = reinterpret_cast<ut_context_t *>(_context);
    //std::cout << "receive a response:" << rt->resp_msgbuf.buf_ << std::endl;
    rt->blocked = false;
    const double req_lat_us =
      erpc::to_usec(mako::rdtsc() - c->start_tsc_, c->rpc->get_freq_ghz());
    rt->latency = req_lat_us; 
    c->rpc->free_msg_buffer(rt->req_msgbuf);
    c->rpc->free_msg_buffer(rt->resp_msgbuf);
}

static void client_thread(int thread_id) {
    ut_req_tag_t* thread_tag = new ut_req_tag_t();
    ut_context_t* c = new ut_context_t();

    std::string client_uri = "127.0.0.1:" + std::to_string(clientPort+thread_id);
    erpc::Nexus nexus(client_uri);
    c->rpc = new erpc::Rpc<erpc::CTransport>(&nexus, static_cast<void *>(c), /* context */ thread_id, sm_handler, 0);

    int serverD = serverPort + thread_id % 2;
    std::string server_uri = "127.0.0.1:" + std::to_string(serverD);
    int session_num = c->rpc->create_session(server_uri, 100);

    while (!c->rpc->is_connected(session_num)) 
        c->rpc->run_event_loop_once();
    std::cout << "connected to the server: " << session_num << std::endl;

    int64_t count=0;
    thread_tag->blocked = true;
    size_t tot_latency = 0;
    while (ut_running) {
        count ++;
        thread_tag->req_msgbuf = c->rpc->alloc_msg_buffer_or_die(sizeof(ut_basic_req_t));
        thread_tag->resp_msgbuf = c->rpc->alloc_msg_buffer_or_die(kMsgSize);

        auto *reqBuf = reinterpret_cast<ut_basic_req_t *>(thread_tag->req_msgbuf.buf_);
        reqBuf->tid = thread_id;
        reqBuf->req_nr = count;
        char *value = "XXXXX";
        memcpy(reqBuf->value, value, value_size);
        
        int req_type = rand() % 10;
        //std::cout<<"send a request:"<<reqBuf->req_nr<<std::endl;
        c->start_tsc_ = mako::rdtsc();
        c->rpc->enqueue_request(session_num, req_type, &thread_tag->req_msgbuf, &thread_tag->resp_msgbuf, transport_response, thread_tag /* _tag */);
        while (thread_tag->blocked) {
            c->rpc->run_event_loop_once();
        }
        thread_tag->blocked = true;
        tot_latency += thread_tag->latency;
        sleep(1);
    }
    usleep((rand() % 1000)*1000);
    std::cout << "thread-id: " << thread_id << " sent " << count / (ut_run_time + 0.0) << " requests per second" << ", avg latency: " << tot_latency / (count + 0.0) << std::endl;
    sleep(1);
}

int main() {
    int num_clients;
    std::cout << "Enter an num_clients: ";
    std::cin >> num_clients;

    for (int i=0; i<num_clients; i++) {
        auto t = std::thread(client_thread, i);
        //erpc::bind_to_core(t, 0, i);
        t.detach();
    }
        
    sleep(ut_run_time);
    ut_running = false;
    sleep(1);
    return 0;
}