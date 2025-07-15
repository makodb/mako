#include <iostream>
#include <bits/stdc++.h>
#include "lib/fasttransport.h"
#include "lib/client.h"
#include "lib/promise.h"
#include "lib/common.h"
#include <stdlib.h>
#include "benchmarks/common.h"
#include <boost/fiber/all.hpp>
#include "unordered_map"
#include "util.h"
#include "benchmarks/sto/sync_util.hh"
#include "benchmarks/common3.h"
using namespace std;
using namespace util;

INIT_SYNC_UTIL_VARS

struct get_request_t
{
    uint64_t req_nr;
    uint16_t table_id;
    uint16_t len;
    char key[700];
};

void request_trim(uint16_t msg_len) {
    static int cnt = 0;
    struct get_request_t origin_request;

    // generate origin message
    // ignore the overflow and the unsigned type conversion
    origin_request.req_nr = rand();
    origin_request.table_id = rand(); 
    origin_request.len = msg_len;
    for (int i = 0; i < msg_len; i++) 
        origin_request.key[i] = rand() % 26 + 'a';

    unsigned byte_length = sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint16_t) + msg_len;

    // generate the char array received
    char *msg_received = new char[byte_length];
    memcpy(msg_received, (char *)&origin_request, byte_length);

    // reconstruct the get_request_t instance
    struct get_request_t request_received;
    memcpy((char *)&request_received, msg_received, byte_length);

    // check is same
    bool is_same = (origin_request.req_nr == request_received.req_nr) &
                        (origin_request.table_id == request_received.table_id) &
                            (origin_request.len == request_received.len);
    for (int i = 0; i < msg_len; i++)
        is_same &= origin_request.key[i] == request_received.key[i];

    cnt += 1;
    cout << "[+] Test: " << cnt << endl;
    cout << "[+] origin   msg | req_nr: " << origin_request.req_nr << " table_id: " 
                    << origin_request.table_id << " len: " << origin_request.len << endl;
    cout << "[+] received msg | req_nr: " << request_received.req_nr << " table_id: " 
                    << request_received.table_id << " len: " << request_received.len << endl;
    cout << "[+] origin   key | ";
    for (int i = 0; i < msg_len; i++)
        cout << origin_request.key[i];
    cout << endl;
    cout << "[+] received key | ";
    for (int i = 0; i < msg_len; i++)
        cout << request_received.key[i];
    cout << endl;
    cout << "[+] data safely reconstructed: " << (is_same ? "yes" : "no") << endl << endl;
}

void request_trim() {
    srand(time(0));

    for (int i = 0; i < 10; i++)
        request_trim(rand() % 690);
}

void erpc_instance_exp(int id) {
    std::string file="./config/local-shards2-warehouses4.yml";
    auto config = new transport::Configuration(file);
    std::string local_uri = config->shard(0).host;
    auto transport = new FastTransport(file,
                                      local_uri, // local_uri
                                      "localhost",
                                      1, 11,
                                      0, // i % 2,  // physPort
                                      0, // i % 2,  // numa node, per numa about 1 Gb
                                      0,  // shardIdx
                                      id);
    system("rdma resource >> b.log");
}

void erpc_instance_exp_seq(int num) {
    std::string file="./config/local-shards2-warehouses4.yml";
    auto config = new transport::Configuration(file);
    std::string local_uri = config->shard(0).host;
    vector<FastTransport*> instances;
    for (size_t i=0; i<num; ++i){
        auto transport = new FastTransport(file,
                                      local_uri, // local_uri
                                      "localhost",
                                      1, 11,
                                      0, // i % 2,  // physPort
                                      0, // i % 2,  // numa node, per numa about 1 Gb
                                      0,  // shardIdx
                                      i+500);
        instances.push_back(transport);
        system("rdma resource >> a.log");
    }
}

void fiber_instance_exp(int id) {
    static __thread int nshards=id;
    //__thread int TThread::the_id;
    static thread_local int a=id;
    static int b=id;
    int c=id;

    std::cout << "enter the function..." << std::endl;
    uint64_t g=0;
    while(g<10000000) {g++;boost::this_fiber::yield();} ;
    std::cout << "id: " << id << ",g:" << g << "," << nshards << "," << a << "," << b << "," << c << std::endl;
}

// https://github.com/shenweihai1/meerkat/blob/master/store/benchmark/benchClient.cc
void fiber_testing() {
    // fiber
    boost::fibers::fiber client_fibers[10];
    for (auto i=0; i< 10; i++) {
        boost::fibers::fiber f(fiber_instance_exp, i+1);
        client_fibers[i] = std::move(f);
    }

    for (auto i=0; i< 10; i++) {
        client_fibers[i].join();
    }
}

__thread HashWrapper *tprops = nullptr;

class tmpObj {
    public:
        bool is_dummy;
        void set_is_dummy(bool is_dummy_t) { is_dummy = is_dummy_t; }
        bool get_is_dummy() { return is_dummy; }
} ;

void testing_map_get() {
    timer t;
    tprops = new HashWrapper();
    tprops->set_tprops("warehouses", 20);
    tprops->set_tprops("nshards", 2);

    int iter = 70 * 10000 * 30 * 2;
    int gk = 0;
    t.lap_nano();
    for (int i=0; i<iter; i++) {
        gk += tprops->get_tprops("warehouses");
    }
    std::cout << "elpased: " << t.lap_nano() << std::endl;;

    auto o = new tmpObj();
    o->set_is_dummy(true);
    bool tk = true;
    t.lap_nano();
    for (int i=0; i<iter; i++) {
        tk |= o->get_is_dummy();
    }

    std::cout << "elpased: " << t.lap_nano() << std::endl;
    std::cout << "gk: " << gk << ", tk: " << tk << std::endl;
}

void vector_code() {
    std::vector<uint64_t> values;
    int t=20;
    values.resize(t);
    for (int i=0;i<t;i++)
        values[i] = (i+i)*10000000000000000;
    char* ss = (char*)malloc(sizeof(uint64_t)*t);
    int pos=0;
    // encode
    for (int i=0;i<t;i++) {
        memcpy(ss+pos, &values[i], sizeof(uint64_t));
        pos+=sizeof(uint64_t);
    }

    // decode
    pos=0;
    for (int i=0;i<t;i++){
        uint64_t tmp =0;
        memcpy(&tmp, ss+pos, sizeof(uint64_t));
        pos+=sizeof(uint64_t);
        std::cout << "tmp: " << tmp << std::endl;
    }
}

static atomic<int> aa(0);

void func_a() {
    int cnt=0;
    while (cnt++ < 10) {
        std::cout << "a-0" << aa << std::endl;
        aa += 10000;
        std::cout << "a-1" << aa << std::endl;
        sleep(1);
    }
}
void func_b() {
    int cnt=0;
    while (cnt++ < 10) {
        std::cout << "b-0" << aa << std::endl;
        aa += 100;
        std::cout << "b-1" << aa << std::endl;
        sleep(1);
    }
}
void func_c() {
    int cnt=0;
    while (cnt++ < 10) {
        std::cout << "c-0" << aa << std::endl;
        aa += 1;
        std::cout << "c-1" << aa << std::endl;
        sleep(1);
    }
}
void static_scope_s(int i) {
    if (i==0) func_a();
    else if(i==1) func_b();
    else func_c();
}
void test_static_scope() {
    std::vector<thread> threads;
    threads.resize(3);
    for (int i=0;i<3;i++) {
        threads[i] = thread(static_scope_s, i);
    }
    for (int i=0;i<3;i++)
        threads[i].join();
    std::cout << "final: " << aa << std::endl;
}


int main(int argc, char ** argv)
{
    test_static_scope();
    fiber_testing();
    request_trim();
    erpc_instance_exp_seq(3);
    testing_map_get();

    if (argc > 1) {
       std::vector<std::thread> threads(atoi(argv[1]));
       for (auto i=0; i< atoi(argv[1]); i++) {
           threads[i] = std::thread(erpc_instance_exp, i+1);
       }
       for (auto i=0; i< atoi(argv[1]); i++) {
           threads[i].join() ;
       }
    }
    vector_code();
}