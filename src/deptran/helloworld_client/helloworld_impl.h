//#pragma once
// similar to service.h

#include "__dep__.h"
#include "constants.h"
#include "../rcc/graph.h"
#include "../rcc/graph_marshaler.h"
#include "../command.h"
#include "deptran/procedure.h"
#include "../command_marshaler.h"
#include "../helloworld.h"

namespace helloworld_client {
    class HelloworldClientServiceImpl : public HelloworldClientService {

    public: 
        HelloworldClientServiceImpl() ;
        
    public:
        int counter_read = 0;
        std::shared_ptr<Fiber> first_req {};
    
  // BEGIN typed-rpc-decls (HelloworldClientServiceImpl)
  // Typed RPC interface overrides (new API).
  void txn_read(const HelloworldClientService::RpcTxnReadRequest& req, HelloworldClientService::RpcTxnReadResponse& resp, srpc::DeferredReply defer) override;
  // END typed-rpc-decls (HelloworldClientServiceImpl)
} ;
} // namespace helloworld_client
