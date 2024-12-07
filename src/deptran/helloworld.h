#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace helloworld_client {

class HelloworldClientService: public rrr::Service {
public:
    enum {
        TXN_READ = 0x2e6830d2,
    };
    int __reg_to__(rrr::Server* svr) {
        int ret = 0;
        if ((ret = svr->reg(TXN_READ, this, &HelloworldClientService::__txn_read__wrapper__)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr->unreg(TXN_READ);
        return ret;
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, remember to reply req, delete req, and sconn->release(); use sconn->run_async for heavy job
    virtual void txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val, rrr::DeferredReply* defer) = 0;
private:
    void __txn_read__wrapper__(rrr::Request* req, rrr::ServerConnection* sconn) {
        std::vector<rrr::i64>* in_0 = new std::vector<rrr::i64>;
        req->m >> *in_0;
        rrr::i32* out_0 = new rrr::i32;
        auto __marshal_reply__ = [=] {
            *sconn << *out_0;
        };
        auto __cleanup__ = [=] {
            delete in_0;
            delete out_0;
        };
        rrr::DeferredReply* __defer__ = new rrr::DeferredReply(req, sconn, __marshal_reply__, __cleanup__);
        this->txn_read(*in_0, out_0, __defer__);
    }
};

class HelloworldClientProxy {
protected:
    rrr::Client* __cl__;
public:
    HelloworldClientProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::Future* async_txn_read(const std::vector<rrr::i64>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(HelloworldClientService::TXN_READ, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << _req;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val) {
        rrr::Future* __fu__ = this->async_txn_read(_req);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *val;
        }
        __fu__->release();
        return __ret__;
    }
};

} // namespace helloworld_client



