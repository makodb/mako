#include "network_impl.h"

namespace network_client {
    NetworkClientServiceImpl::NetworkClientServiceImpl() {  }

    void NetworkClientServiceImpl::txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        counter += 1;
    }

    void NetworkClientServiceImpl::txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        counter += 1;
    }

    // XXX, using malloc on the heap???
    void NetworkClientServiceImpl::txn_new_order(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        new_order_requests.push_back(_req);
        counter_new_order++;
    };  // DONE

    void NetworkClientServiceImpl::txn_payment(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        payment_requests.push_back(_req);
        counter_payement++;
    }; // DONE

    void NetworkClientServiceImpl::txn_delivery(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        delivery_requests.push_back(_req);
        counter_delivery++;
    }; // DONE

    void NetworkClientServiceImpl::txn_order_status(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        order_status_requests.push_back(_req);
        counter_order_status++;
    }; // DONE
    
    void NetworkClientServiceImpl::txn_stock_level(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        stock_level_requests.push_back(_req);
        counter_stock_level++;
    }; // DONE
}