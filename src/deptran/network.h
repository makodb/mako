#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace network_client {

class NetworkClientService: public rrr::Service {
public:
    enum {
        TXN_RMW = 0x501a4bd6,
        TXN_READ = 0x29021dfd,
        TXN_NEW_ORDER = 0x56a4f50a,
        TXN_PAYMENT = 0x4d46bb07,
        TXN_DELIVERY = 0x2d078ab2,
        TXN_ORDER_STATUS = 0x6da87d5d,
        TXN_STOCK_LEVEL = 0x2a79c256,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(TXN_RMW, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TXN_READ, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TXN_NEW_ORDER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TXN_PAYMENT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TXN_DELIVERY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TXN_ORDER_STATUS, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TXN_STOCK_LEVEL, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(TXN_RMW);
        svr.unreg(TXN_READ);
        svr.unreg(TXN_NEW_ORDER);
        svr.unreg(TXN_PAYMENT);
        svr.unreg(TXN_DELIVERY);
        svr.unreg(TXN_ORDER_STATUS);
        svr.unreg(TXN_STOCK_LEVEL);
        return ret;
    }
    // @safe - Virtual dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case TXN_RMW: __txn_rmw__wrapper__(std::move(req), weak_sconn); break;
        case TXN_READ: __txn_read__wrapper__(std::move(req), weak_sconn); break;
        case TXN_NEW_ORDER: __txn_new_order__wrapper__(std::move(req), weak_sconn); break;
        case TXN_PAYMENT: __txn_payment__wrapper__(std::move(req), weak_sconn); break;
        case TXN_DELIVERY: __txn_delivery__wrapper__(std::move(req), weak_sconn); break;
        case TXN_ORDER_STATUS: __txn_order_status__wrapper__(std::move(req), weak_sconn); break;
        case TXN_STOCK_LEVEL: __txn_stock_level__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
    virtual void txn_rmw(const std::vector<rrr::i64>& _req, rrr::DeferredReply defer) = 0;
    virtual void txn_read(const std::vector<rrr::i64>& _req, rrr::DeferredReply defer) = 0;
    virtual void txn_new_order(const std::vector<int32_t>& _req, rrr::DeferredReply defer) = 0;
    virtual void txn_payment(const std::vector<int32_t>& _req, rrr::DeferredReply defer) = 0;
    virtual void txn_delivery(const std::vector<int32_t>& _req, rrr::DeferredReply defer) = 0;
    virtual void txn_order_status(const std::vector<int32_t>& _req, rrr::DeferredReply defer) = 0;
    virtual void txn_stock_level(const std::vector<int32_t>& _req, rrr::DeferredReply defer) = 0;
private:
    void __txn_rmw__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<rrr::i64>* in_0 = new std::vector<rrr::i64>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_rmw(*in_0, std::move(__defer__));
    }
    void __txn_read__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<rrr::i64>* in_0 = new std::vector<rrr::i64>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_read(*in_0, std::move(__defer__));
    }
    void __txn_new_order__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_new_order(*in_0, std::move(__defer__));
    }
    void __txn_payment__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_payment(*in_0, std::move(__defer__));
    }
    void __txn_delivery__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_delivery(*in_0, std::move(__defer__));
    }
    void __txn_order_status__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_order_status(*in_0, std::move(__defer__));
    }
    void __txn_stock_level__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::vector<int32_t>* in_0 = new std::vector<int32_t>;
        req->m >> *in_0;
        auto __marshal_reply__ = [=](rrr::Marshal& m) {
        };
        auto __cleanup__ = [=] {
            delete in_0;
        };
        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
        this->txn_stock_level(*in_0, std::move(__defer__));
    }
};

class NetworkClientProxy {
protected:
    rrr::Client* __cl__;
public:
    NetworkClientProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::FutureResult async_txn_rmw(const std::vector<rrr::i64>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_RMW, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_rmw(const std::vector<rrr::i64>& _req) {
        auto __fu_result__ = this->async_txn_rmw(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_txn_read(const std::vector<rrr::i64>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_READ, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_read(const std::vector<rrr::i64>& _req) {
        auto __fu_result__ = this->async_txn_read(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_txn_new_order(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_NEW_ORDER, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_new_order(const std::vector<int32_t>& _req) {
        auto __fu_result__ = this->async_txn_new_order(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_txn_payment(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_PAYMENT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_payment(const std::vector<int32_t>& _req) {
        auto __fu_result__ = this->async_txn_payment(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_txn_delivery(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_DELIVERY, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_delivery(const std::vector<int32_t>& _req) {
        auto __fu_result__ = this->async_txn_delivery(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_txn_order_status(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_ORDER_STATUS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_order_status(const std::vector<int32_t>& _req) {
        auto __fu_result__ = this->async_txn_order_status(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_txn_stock_level(const std::vector<int32_t>& _req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(NetworkClientService::TXN_STOCK_LEVEL, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << _req;
        });
    }
    rrr::i32 txn_stock_level(const std::vector<int32_t>& _req) {
        auto __fu_result__ = this->async_txn_stock_level(_req);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
};

} // namespace network_client



