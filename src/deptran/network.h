#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace network_client {

class NetworkClientService: public rrr::Service {
public:
    enum {
        TXN_RMW = 0x3da2afa2,
        TXN_READ = 0x6f4fb877,
        TXN_NEW_ORDER = 0x215d9020,
        TXN_PAYMENT = 0x1d04b2bd,
        TXN_DELIVERY = 0x24799656,
        TXN_ORDER_STATUS = 0x2dd6e268,
        TXN_STOCK_LEVEL = 0x3548a06f,
    };
    int __reg_to__(rrr::Server* svr) {
        int ret = 0;
        if ((ret = svr->reg(TXN_RMW, this, &NetworkClientService::__txn_rmw__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_READ, this, &NetworkClientService::__txn_read__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_NEW_ORDER, this, &NetworkClientService::__txn_new_order__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_PAYMENT, this, &NetworkClientService::__txn_payment__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_DELIVERY, this, &NetworkClientService::__txn_delivery__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_ORDER_STATUS, this, &NetworkClientService::__txn_order_status__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_STOCK_LEVEL, this, &NetworkClientService::__txn_stock_level__wrapper__)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr->unreg(TXN_RMW);
        svr->unreg(TXN_READ);
        svr->unreg(TXN_NEW_ORDER);
        svr->unreg(TXN_PAYMENT);
        svr->unreg(TXN_DELIVERY);
        svr->unreg(TXN_ORDER_STATUS);
        svr->unreg(TXN_STOCK_LEVEL);
        return ret;
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, remember to reply req, delete req, and sconn->release(); use sconn->run_async for heavy job
    virtual void txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) = 0;
    virtual void txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) = 0;
    virtual void txn_new_order(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) = 0;
    virtual void txn_payment(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) = 0;
    virtual void txn_delivery(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) = 0;
    virtual void txn_order_status(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) = 0;
    virtual void txn_stock_level(const std::vector<int32_t>& _req, rrr::DeferredReply* defer) = 0;
private:
    void __txn_rmw__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        uint64_t in_0;
        req->m >> in_0;
        uint64_t in_1;
        req->m >> in_1;
        uint64_t in_2;
        req->m >> in_2;
        uint64_t in_3;
        req->m >> in_3;
        this->txn_rmw(in_0, in_1, in_2, in_3);
        sconn->begin_reply(req);
        sconn->end_reply();
        delete req;
        sconn->release();
    }
    void __txn_read__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        uint64_t in_0;
        req->m >> in_0;
        uint64_t in_1;
        req->m >> in_1;
        uint64_t in_2;
        req->m >> in_2;
        uint64_t in_3;
        req->m >> in_3;
        this->txn_read(in_0, in_1, in_2, in_3);
        sconn->begin_reply(req);
        sconn->end_reply();
        delete req;
        sconn->release();
    }
    void __txn_new_order__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=] {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->txn_new_order(*in_0, __defer__);
    }
    void __txn_payment__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=] {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->txn_payment(*in_0, __defer__);
    }
    void __txn_delivery__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=] {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->txn_delivery(*in_0, __defer__);
    }
    void __txn_order_status__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=] {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->txn_order_status(*in_0, __defer__);
    }
    void __txn_stock_level__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=] {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->txn_stock_level(*in_0, __defer__);
    }
};

class NetworkClientProxy {
protected:
    rrr::Client* __cl__;
public:
    NetworkClientProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::Future* async_txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_RMW, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << k0;
            *__cl__ << k1;
            *__cl__ << k2;
            *__cl__ << k3;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        rrr::Future* __fu__ = this->async_txn_rmw(k0, k1, k2, k3);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_READ, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << k0;
            *__cl__ << k1;
            *__cl__ << k2;
            *__cl__ << k3;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) {
        rrr::Future* __fu__ = this->async_txn_read(k0, k1, k2, k3);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_txn_new_order(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_NEW_ORDER, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << _req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_new_order(const std::vector<int32_t>& _req) {
        rrr::Future* __fu__ = this->async_txn_new_order(_req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_txn_payment(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_PAYMENT, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << _req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_payment(const std::vector<int32_t>& _req) {
        rrr::Future* __fu__ = this->async_txn_payment(_req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_txn_delivery(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_DELIVERY, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << _req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_delivery(const std::vector<int32_t>& _req) {
        rrr::Future* __fu__ = this->async_txn_delivery(_req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_txn_order_status(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_ORDER_STATUS, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << _req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_order_status(const std::vector<int32_t>& _req) {
        rrr::Future* __fu__ = this->async_txn_order_status(_req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_txn_stock_level(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(NetworkClientService::TXN_STOCK_LEVEL, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << _req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_stock_level(const std::vector<int32_t>& _req) {
        rrr::Future* __fu__ = this->async_txn_stock_level(_req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
};

} // namespace network_client



