#include "helloworld_impl.h"

#include <chrono>
#include <thread>

namespace helloworld_client {

HelloworldClientServiceImpl::HelloworldClientServiceImpl() {}

void HelloworldClientServiceImpl::txn_read(
    const HelloworldClientService::RpcTxnReadRequest& rpc_req,
    HelloworldClientService::RpcTxnReadResponse& resp,
    rrr::DeferredReply defer) {
  const auto& req = rpc_req._req;

  if (req.size() == 1) {
    std::cout << "[server]receive first request" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
  } else {
    std::cout << "before coroutine for second" << std::endl;
    std::cout << "[server]receive second request" << std::endl;
  }

  resp.val = static_cast<rrr::i32>(req.size());
  std::cout << "receive " << req.size() << " request - done" << std::endl;
  defer.reply();
}

}  // namespace helloworld_client
