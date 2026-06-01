#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "__dep__.h"
#include <sys/time.h>
#include <unistd.h>
#include "concurrentqueue.h"
#include "helloworld_client/helloworld_impl.h"
#include <pthread.h>

import std;
typedef std::chrono::high_resolution_clock Clock;

using namespace janus;
using namespace helloworld_client;

std::vector<shared_ptr<helloworld_client::HelloworldClientProxy>> nc_clients = {} ;
std::vector<shared_ptr<helloworld_client::HelloworldClientServiceImpl>> nc_services2 = {};

/**
 * ./build/helloworld 1 1 
 * 
 * ./build/helloworld 0 1
*/

int nthreads=1;
int nkeys=1000000;
char *server_ip="127.0.0.1";

struct args {
    int port;
    char* server_ip;
    int par_id;
};

void output_val(i32 val) {
  std::cout<<"[client]received response:"<<val<<std::endl;
}

// client implementation
void *nc_start_client(void *input) {
  int par_id = ((struct args*)input)->par_id;
  FutureAttr fuattr;  // fuattr
  fuattr.callback = [&] (rusty::Arc<Future> fu) {
    i32 val;
    fu->get_reply() >> val;
    output_val(val);
  };
  std::vector<int64_t> ret;
  ret.push_back(1);
  
  std::cout<<"send out first req"<<std::endl;
  HelloworldClientProxy::RpcTxnReadRequest first_req;
  first_req._req = ret;
  auto first_result = nc_clients[par_id]->async_txn_read(first_req, fuattr);
  if (first_result.is_ok()) {
    Future::safe_release(first_result.unwrap().raw_future());
  }

  //sleep(1); 

  ret.push_back(2);
  std::cout<<"send out second req"<<std::endl;
  HelloworldClientProxy::RpcTxnReadRequest second_req;
  second_req._req = ret;
  auto second_result = nc_clients[par_id]->async_txn_read(second_req, fuattr);
  if (second_result.is_ok()) {
    Future::safe_release(second_result.unwrap().raw_future());
  }

  sleep(10);
  return nullptr;
}

void nc_setup_client(int nkeys, int nthreads, int run) {
  for (int i=0; i<nthreads; i++) {
    rrr::PollThread *pm = new rrr::PollThread();
    rrr::Client *client = new rrr::Client(pm);
    auto port_s=std::to_string(10010+i);
    while (client->connect(reinterpret_cast<const int8_t*>((std::string(server_ip)+":"+port_s).c_str()), true)!=0) {
      usleep(100 * 1000); // retry to connect
    }
    HelloworldClientProxy *nc_client_proxy = new HelloworldClientProxy(client);
    nc_clients.push_back(std::shared_ptr<HelloworldClientProxy>(nc_client_proxy));
  }

  // using different threads to issue transactions independently
  for (int par_id=0; par_id<nthreads; par_id++) {
    pthread_t ph_c;
    struct args *ps = (struct args *)malloc(sizeof(struct args));
    ps->par_id=par_id;
    pthread_create(&ph_c, NULL, nc_start_client, (void *)ps);
    pthread_detach(ph_c);
    usleep(10 * 1000);
  }

  sleep(run); 
}


void *nc_start_server2(void *input) {
    auto poll_arc = PollThread::create();
    rrr::Server *server = new rrr::Server(rrr::Server::new_(rusty::Some(poll_arc)));

    server->reg_service_typed(rusty::make_box<HelloworldClientServiceImpl>());
    server->start(reinterpret_cast<const int8_t*>((std::string(((struct args*)input)->server_ip)+std::string(":")+std::to_string(((struct args*)input)->port)).c_str())  );
    // Service is now owned by server
    return nullptr;
}

void nc_setup_server2(int nthreads, std::string host) {
  for (int i=0; i<nthreads; i++) {
    struct args *ps = (struct args *)malloc(sizeof(struct args));
    ps->port=10010+i;
    ps->server_ip=(char*)host.c_str();
    ps->par_id=i;
    pthread_t ph_s;
    pthread_create(&ph_s, NULL, nc_start_server2, (void *)ps);
    pthread_detach(ph_s);
    usleep(10 * 1000); // wait for 10ms
  }
}

int main(int argc, char* argv[]){
    if (argc < 3) return -1;

    unsigned int is_server = atoi(argv[1]) ;
    nthreads = atoi(argv[2]); // # of client threads
    if (argc > 3)
      server_ip = argv[3];

    std::cout << "Using nthreads: " << nthreads << ", on server_ip: " << server_ip << std::endl;

    int runningTime=60;
    if (is_server) {
        nc_setup_server2(nthreads, "127.0.0.1");
        sleep(runningTime);
    } else { // start a client
        sleep(1) ; // wait for server start
        nc_setup_client(100*10000, nthreads, runningTime);
    }
    return 0;
}
