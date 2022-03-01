#include "network_impl.h"

namespace network_client {
    NetworkClientServiceImpl::NetworkClientServiceImpl() {  }

    void NetworkClientServiceImpl::txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        counter += 1;
    }

    void NetworkClientServiceImpl::txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        counter += 1;
    }

    void NetworkClientServiceImpl::txn_new_order(const std::vector<int32_t>& _req) {
        std::vector<int> data;
        data.push_back(_req[0]);
        new_order_requests.push_back(data);
    };

    void NetworkClientServiceImpl::txn_payment() {
        counter_payement++;
    };

    void NetworkClientServiceImpl::txn_delivery() {
        counter_delivery++;
    };

    void NetworkClientServiceImpl::txn_order_status() {
        counter_order_status++;
    };
    
    void NetworkClientServiceImpl::txn_stock_level() {
        counter_stock_level++;
    };
}