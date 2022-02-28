#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace network_client {

class NetworkClientService: public rrr::Service {
public:
    enum {
        TXN_RMW = 0x49362608,
        TXN_READ = 0x604cd5aa,
    };
    int __reg_to__(rrr::Server* svr) {
        int ret = 0;
        if ((ret = svr->reg(TXN_RMW, this, &NetworkClientService::__txn_rmw__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(TXN_READ, this, &NetworkClientService::__txn_read__wrapper__)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr->unreg(TXN_RMW);
        svr->unreg(TXN_READ);
        return ret;
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, remember to reply req, delete req, and sconn->release(); use sconn->run_async for heavy job
    virtual void txn_rmw(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) = 0;
    virtual void txn_read(const uint64_t& k0, const uint64_t& k1, const uint64_t& k2, const uint64_t& k3) = 0;
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
};

} // namespace network_client



