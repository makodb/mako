#include "network_impl.h"


import std;

static inline long long getCurrentTimeMillis2() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace network_client {

NetworkClientServiceImpl::NetworkClientServiceImpl() {}

void NetworkClientServiceImpl::txn_rmw(
    const NetworkClientService::RpcTxnRmwRequest& rpc_req,
    NetworkClientService::RpcTxnRmwResponse& resp,
    srpc::DeferredReply defer) {
  const auto& req = rpc_req._req;
  if (counter_rmw % 100 == 0) {
    std::cout << "rpc to be received:" << (getCurrentTimeMillis2() - req.back())
              << std::endl;
  }
  counter_rmw += 1;
  defer.reply();
}

void NetworkClientServiceImpl::txn_read(
    const NetworkClientService::RpcTxnReadRequest& rpc_req,
    NetworkClientService::RpcTxnReadResponse& resp,
    srpc::DeferredReply defer) {
  const auto& req = rpc_req._req;
  counter_read += 1;
  if (counter_read % 100 == 0) {
    std::cout << "rpc to be received:" << getCurrentTimeMillis2() - req.back()
              << std::endl;
  }
  defer.reply();
}

void NetworkClientServiceImpl::txn_new_order(
    const NetworkClientService::RpcTxnNewOrderRequest& rpc_req,
    NetworkClientService::RpcTxnNewOrderResponse& resp,
    srpc::DeferredReply defer) {
  (void)resp;
  new_order_requests.push_back(rpc_req._req);
  counter_new_order++;
  defer.reply();
}

void NetworkClientServiceImpl::txn_payment(
    const NetworkClientService::RpcTxnPaymentRequest& rpc_req,
    NetworkClientService::RpcTxnPaymentResponse& resp,
    srpc::DeferredReply defer) {
  (void)resp;
  payment_requests.push_back(rpc_req._req);
  counter_payement++;
  defer.reply();
}

void NetworkClientServiceImpl::txn_delivery(
    const NetworkClientService::RpcTxnDeliveryRequest& rpc_req,
    NetworkClientService::RpcTxnDeliveryResponse& resp,
    srpc::DeferredReply defer) {
  (void)resp;
  delivery_requests.push_back(rpc_req._req);
  counter_delivery++;
  defer.reply();
}

void NetworkClientServiceImpl::txn_order_status(
    const NetworkClientService::RpcTxnOrderStatusRequest& rpc_req,
    NetworkClientService::RpcTxnOrderStatusResponse& resp,
    srpc::DeferredReply defer) {
  (void)resp;
  order_status_requests.push_back(rpc_req._req);
  counter_order_status++;
  defer.reply();
}

void NetworkClientServiceImpl::txn_stock_level(
    const NetworkClientService::RpcTxnStockLevelRequest& rpc_req,
    NetworkClientService::RpcTxnStockLevelResponse& resp,
    srpc::DeferredReply defer) {
  (void)resp;
  stock_level_requests.push_back(rpc_req._req);
  counter_stock_level++;
  defer.reply();
}

}  // namespace network_client
