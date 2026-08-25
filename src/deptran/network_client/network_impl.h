//#pragma once
// similar to service.h

#include "__dep__.h"
#include "constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "deptran/procedure.h"
#include "../command_marshaler.h"
#include "../network.h"

namespace network_client {
    class NetworkClientServiceImpl : public NetworkClientService {

    public: 
        NetworkClientServiceImpl() ;

    public:
        int counter = 0;

        int counter_new_order=0;
        int counter_payement=0;
        int counter_delivery=0;
        int counter_order_status=0;
        int counter_stock_level=0;

        std::vector<std::vector<int>> new_order_requests;
        std::vector<std::vector<int>> payment_requests;
        std::vector<std::vector<int>> delivery_requests;
        std::vector<std::vector<int>> order_status_requests;
        std::vector<std::vector<int>> stock_level_requests;

        int counter_rmw=0;
        int counter_read=0;
        std::vector<std::vector<int>> rmw_requests;
        std::vector<std::vector<int>> read_requests;
    
  // BEGIN typed-rpc-decls (NetworkClientServiceImpl)
  // Typed RPC interface overrides (new API).
  void txn_rmw(const NetworkClientService::RpcTxnRmwRequest& req, NetworkClientService::RpcTxnRmwResponse& resp, srpc::DeferredReply defer) override;
  void txn_read(const NetworkClientService::RpcTxnReadRequest& req, NetworkClientService::RpcTxnReadResponse& resp, srpc::DeferredReply defer) override;
  void txn_new_order(const NetworkClientService::RpcTxnNewOrderRequest& req, NetworkClientService::RpcTxnNewOrderResponse& resp, srpc::DeferredReply defer) override;
  void txn_payment(const NetworkClientService::RpcTxnPaymentRequest& req, NetworkClientService::RpcTxnPaymentResponse& resp, srpc::DeferredReply defer) override;
  void txn_delivery(const NetworkClientService::RpcTxnDeliveryRequest& req, NetworkClientService::RpcTxnDeliveryResponse& resp, srpc::DeferredReply defer) override;
  void txn_order_status(const NetworkClientService::RpcTxnOrderStatusRequest& req, NetworkClientService::RpcTxnOrderStatusResponse& resp, srpc::DeferredReply defer) override;
  void txn_stock_level(const NetworkClientService::RpcTxnStockLevelRequest& req, NetworkClientService::RpcTxnStockLevelResponse& resp, srpc::DeferredReply defer) override;
  // END typed-rpc-decls (NetworkClientServiceImpl)
} ;
} // namespace network_client
