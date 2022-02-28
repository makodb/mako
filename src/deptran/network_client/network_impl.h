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
        
        void txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) override;
        void txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) override;
        
    public:
        int counter = 0;
        std::vector<std::tuple<int,int,int,int>> requests;
    } ;
} // namespace network_client