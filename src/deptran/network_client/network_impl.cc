#include "network_impl.h"

namespace network_client {
    NetworkClientServiceImpl::NetworkClientServiceImpl() { }

    void NetworkClientServiceImpl::txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        counter += 1;
        requests.push_back(std::make_tuple(k0,k1,k2,k3));
    }

    void NetworkClientServiceImpl::txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        counter += 1;
        requests.push_back(std::make_tuple(k0,k1,k2,k3));
    }
}