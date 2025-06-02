#include "network_impl.h"
#include <chrono>

long long getCurrentTimeMillis2() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

namespace network_client {
    NetworkClientServiceImpl::NetworkClientServiceImpl() {  }

    void NetworkClientServiceImpl::txn_rmw(const std::vector<int64_t>& _req, rrr::DeferredReply* defer) {
        //rmw_requests.push_back(_req);
        if (counter_rmw%100==0)
        std::cout<<"rpc to be received:"<<(getCurrentTimeMillis2() - _req.back())<<std::endl;
        counter_rmw += 1;
        defer->reply();
    }

    void NetworkClientServiceImpl::txn_read(const std::vector<int64_t>& _req, rrr::DeferredReply* defer) {
        //read_requests.push_back(_req);
        counter_read += 1;
        if (counter_read%100==0)
        std::cout<<"rpc to be received:"<<getCurrentTimeMillis2() - _req.back()<<std::endl;
        defer->reply();
    }

    // XXX, using malloc on the heap???
    void NetworkClientServiceImpl::txn_new_order(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        new_order_requests.push_back(_req);
        counter_new_order++;
        defer->reply();
    };  // DONE

    void NetworkClientServiceImpl::txn_payment(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        payment_requests.push_back(_req);
        counter_payement++;
        defer->reply();
    }; // DONE

    void NetworkClientServiceImpl::txn_delivery(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        delivery_requests.push_back(_req);
        counter_delivery++;
        defer->reply();
    }; // DONE

    void NetworkClientServiceImpl::txn_order_status(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        order_status_requests.push_back(_req);
        counter_order_status++;
        defer->reply();
    }; // DONE
    
    void NetworkClientServiceImpl::txn_stock_level(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) {
        stock_level_requests.push_back(_req);
        counter_stock_level++;
        defer->reply();
    }; // DONE
}