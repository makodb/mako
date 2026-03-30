#include "common.h"
#include <atomic>
#include <chrono>
#include <iostream>


erpc::Rpc<erpc::CTransport> *rpc;
erpc::MsgBuffer req;
erpc::MsgBuffer resp;
int session_num;
int local_pointer=10;
std::atomic<int64_t> received_num(0);
auto stop = std::chrono::high_resolution_clock::now();
std::atomic<bool> block(true);
int num_requests=1;

// this function is called if we receive a response from the server
void hello_wrapper_response(void *_context, void *_tag) {
  block = false;
  received_num++;
  std::cout << "received_num: " << received_num << std::endl;
  std::cout << "the held pointer(_tag): " << (*(int*)_tag) << std::endl;
  std::cout << "received message: " << (char*)resp.buf_ << std::endl;
  if (received_num == num_requests) {
    stop = std::chrono::high_resolution_clock::now();
  }
}

void sm_handler(int, erpc::SmEventType, erpc::SmErrType, void *) {}

void hello(int i, int req_type) {
  std::string v = std::to_string(i+100000); 
  const char *chr = v.c_str();
  printf("hello_client send size:%d\n", strlen(chr));
  req = rpc->alloc_msg_buffer_or_die(strlen(chr));
  sprintf(reinterpret_cast<char *>(req.buf_), "%s", chr);
  resp = rpc->alloc_msg_buffer_or_die(kMsgSize);

  rpc->enqueue_request(session_num, req_type, &req, &resp, hello_wrapper_response, &local_pointer);
  while (block) {
    int num_pkts = rpc->run_event_loop_once();
  }
  block = true;
}

int main() {
  std::string client_uri =
      kClientHostname + ":" + std::to_string(kUDPPortClient);
  erpc::Nexus nexus(client_uri);

  //Nexus *nexus, void *context, uint8_t rpc_id,
  //            sm_handler_t sm_handler, uint8_t phy_port
  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 0, sm_handler);

  std::string server_uri = kServerHostname + ":" + std::to_string(kUDPPort);
  session_num = rpc->create_session(server_uri, 100);
  std::cout << "create_session returned: " << session_num << std::endl;

  if (session_num < 0) {
    std::cout << "create_session failed with error: " << session_num << std::endl;
    return -1;
  }

  printf("DEBUG: About to check is_connected with session_num=%d\n", session_num);
  while (!rpc->is_connected(session_num)) {
    printf("DEBUG: Session %d not connected yet, running event loop...\n", session_num);
    rpc->run_event_loop_once();
  }
  std::cout << "connected to the server: " << session_num << std::endl;

  // auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < num_requests; i++) {
    hello(i, 1);
    //hello(i, 2); // an error
  }

  while (received_num < num_requests) {
    sleep(1);
  }

  // auto duration =
  //     std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
  // std::cout << "eRPC hello time: " << duration.count() << " microsseconds"
  //           << std::endl;
  std::cout << received_num << std::endl;
  delete rpc;
}