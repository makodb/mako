#include "common.h"

using namespace std;
erpc::Rpc<erpc::CTransport> *rpc;
erpc::ReqHandle *g_req_handle;
atomic<int> outstanding(0);
atomic<int> counter(0);

// this function will be called when we receive a request
void hello_wrapper_request_f(erpc::ReqHandle *req_handle, void *) {
  std::string s = reinterpret_cast<char *>(req_handle->get_req_msgbuf()->buf_);
  std::cout << "received a request from the client: " << s << std::endl;
  counter++;

  auto &resp = req_handle->pre_resp_msgbuf_;
  std::string v="server:"+to_string(counter);
  rpc->resize_msg_buffer(&resp, strlen(v.data()));
  sprintf(reinterpret_cast<char *>(resp.buf_), v.data());
  g_req_handle = req_handle;
  outstanding++;
  sleep(1);
}

// void hello_wrapper_request_b(erpc::ReqHandle *req_handle, void *) {
//   std::cout << "receive a message from the background" << std::endl;
// }

void enqueue_thread() {
  //while (true)
    if (outstanding > 0) {
        auto &resp = g_req_handle->pre_resp_msgbuf_;
        std::cout << "send back a response back to the client, enqueue_thread is outstand: " << outstanding << std::endl;
        rpc->enqueue_response(g_req_handle, &resp);
        outstanding--;
    }
}

int main() {
  std::string server_uri = kServerHostname + ":" + std::to_string(kUDPPort);
  std::cout << "server_uri: " <<  server_uri << std::endl;
  erpc::Nexus nexus(server_uri);

  nexus.register_req_func(1, hello_wrapper_request_f, erpc::ReqFuncType::kForeground);
  //nexus.register_req_func(2, hello_wrapper_request_b, erpc::ReqFuncType::kBackground);

  //auto t=std::thread(enqueue_thread);
  //t.detach();

  //Nexus *nexus, void *context, uint8_t rpc_id,
  //            sm_handler_t sm_handler, uint8_t phy_port
  rpc = new erpc::Rpc<erpc::CTransport>(&nexus, nullptr, 100, nullptr);

  while (true) {
    int num_pkts = rpc->run_event_loop_once();
    enqueue_thread();
  }
}
