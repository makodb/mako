#include "transport/transport.h"
#include "../base/all.hpp"
#ifdef ENABLE_RDMA
#include "transport/rdma_transport.h"
#endif
#include <cstring>
#include <cstdlib>

namespace rrr {

std::shared_ptr<Transport> create_rdma_transport(const char* addr) {
    if (addr == nullptr) {
        return nullptr;
    }

#ifdef ENABLE_RDMA
    return std::make_shared<RDMATransport>();
#else
    Log_error("RDMA transport not enabled (compile with -DENABLE_RDMA=ON)");
    return nullptr;
#endif
}

} // namespace rrr

