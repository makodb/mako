#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace helloworld_client {

class HelloworldClientService: public rrr::Service {
public:
    enum {
        TXN_READ = 0x167b15c5,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(TXN_READ, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(TXN_READ);
        return ret;
    }
    // @safe - Virtual dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case TXN_READ: __txn_read__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
    virtual void txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val, rrr::DeferredReply defer) = 0;
private:
    void __txn_read__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<rrr::i64>* in_0 = new std::vector<rrr::i64>;
        req->m >> *in_0;
        rrr::i32* out_0 = new rrr::i32;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
            m << *out_0;
        };
        auto __cleanup__ = [=] {
            delete in_0;
            delete out_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_read(*in_0, out_0, std::move(__defer__));
    }
};

class HelloworldClientProxy {
protected:
    rrr::Client* __cl__;
public:
    HelloworldClientProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::FutureResult async_txn_read(const std::vector<rrr::i64>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(HelloworldClientService::TXN_READ, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_read(const std::vector<rrr::i64>& _req, rrr::i32* val) {
        auto __fu_result__ = this->async_txn_read(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *val;
        }
        // Arc auto-released
        return __ret__;
    }
};

} // namespace helloworld_client



