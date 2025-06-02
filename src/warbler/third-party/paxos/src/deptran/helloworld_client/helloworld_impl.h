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
        
        void txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val, rrr::DeferredReply* defer) override;
        
    public:
        int counter_read = 0;
        std::shared_ptr<Coroutine> first_req {};
    } ;
} // namespace helloworld_client