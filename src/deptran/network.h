#pragma once

#include "rrr/rrr.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>


namespace network_client {

class NetworkClientService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcTxnRmwRequest {
        std::vector<rrr::i64> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnRmwRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnRmwRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnRmwResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnRmwResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnRmwResponse& o) {
        return ar;
    }

    struct RpcTxnReadRequest {
        std::vector<rrr::i64> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnReadRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnReadRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnReadResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnReadResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnReadResponse& o) {
        return ar;
    }

    struct RpcTxnNewOrderRequest {
        std::vector<int32_t> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnNewOrderRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnNewOrderRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnNewOrderResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnNewOrderResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnNewOrderResponse& o) {
        return ar;
    }

    struct RpcTxnPaymentRequest {
        std::vector<int32_t> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnPaymentRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnPaymentRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnPaymentResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnPaymentResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnPaymentResponse& o) {
        return ar;
    }

    struct RpcTxnDeliveryRequest {
        std::vector<int32_t> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnDeliveryRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnDeliveryRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnDeliveryResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnDeliveryResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnDeliveryResponse& o) {
        return ar;
    }

    struct RpcTxnOrderStatusRequest {
        std::vector<int32_t> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnOrderStatusRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnOrderStatusRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnOrderStatusResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnOrderStatusResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnOrderStatusResponse& o) {
        return ar;
    }

    struct RpcTxnStockLevelRequest {
        std::vector<int32_t> _req;
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnStockLevelRequest& o) {
        ar << o._req;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnStockLevelRequest& o) {
        ar >> o._req;
        return ar;
    }

    struct RpcTxnStockLevelResponse {
    };
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTxnStockLevelResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTxnStockLevelResponse& o) {
        return ar;
    }

    enum {
        TXN_RMW = 0x21524de9,
        TXN_READ = 0x36078b5a,
        TXN_NEW_ORDER = 0x4da58142,
        TXN_PAYMENT = 0x343cf22a,
        TXN_DELIVERY = 0x5cf0aaac,
        TXN_ORDER_STATUS = 0x5711069e,
        TXN_STOCK_LEVEL = 0x69916666,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    // typed service signatures
    // @safe
    virtual void txn_rmw(const RpcTxnRmwRequest& req, RpcTxnRmwResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void txn_read(const RpcTxnReadRequest& req, RpcTxnReadResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void txn_new_order(const RpcTxnNewOrderRequest& req, RpcTxnNewOrderResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void txn_payment(const RpcTxnPaymentRequest& req, RpcTxnPaymentResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void txn_delivery(const RpcTxnDeliveryRequest& req, RpcTxnDeliveryResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void txn_order_status(const RpcTxnOrderStatusRequest& req, RpcTxnOrderStatusResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void txn_stock_level(const RpcTxnStockLevelRequest& req, RpcTxnStockLevelResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __txn_rmw__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnRmwRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnRmwResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_rmw(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __txn_read__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnReadRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnReadResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_read(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __txn_new_order__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnNewOrderRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnNewOrderResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_new_order(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __txn_payment__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnPaymentRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnPaymentResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_payment(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __txn_delivery__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnDeliveryRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnDeliveryResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_delivery(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __txn_order_status__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnOrderStatusRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnOrderStatusResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_order_status(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __txn_stock_level__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnStockLevelRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            __req_ar__ >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnStockLevelResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->txn_stock_level(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class NetworkClientProxy {
protected:
    rrr::Client* __cl__;
public:
    NetworkClientProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcTxnRmwRequest = NetworkClientService::RpcTxnRmwRequest;
    using RpcTxnRmwResponse = NetworkClientService::RpcTxnRmwResponse;
    using RpcTxnReadRequest = NetworkClientService::RpcTxnReadRequest;
    using RpcTxnReadResponse = NetworkClientService::RpcTxnReadResponse;
    using RpcTxnNewOrderRequest = NetworkClientService::RpcTxnNewOrderRequest;
    using RpcTxnNewOrderResponse = NetworkClientService::RpcTxnNewOrderResponse;
    using RpcTxnPaymentRequest = NetworkClientService::RpcTxnPaymentRequest;
    using RpcTxnPaymentResponse = NetworkClientService::RpcTxnPaymentResponse;
    using RpcTxnDeliveryRequest = NetworkClientService::RpcTxnDeliveryRequest;
    using RpcTxnDeliveryResponse = NetworkClientService::RpcTxnDeliveryResponse;
    using RpcTxnOrderStatusRequest = NetworkClientService::RpcTxnOrderStatusRequest;
    using RpcTxnOrderStatusResponse = NetworkClientService::RpcTxnOrderStatusResponse;
    using RpcTxnStockLevelRequest = NetworkClientService::RpcTxnStockLevelRequest;
    using RpcTxnStockLevelResponse = NetworkClientService::RpcTxnStockLevelResponse;
    class txn_rmwTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_rmwTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnRmwResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnRmwResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnRmwResponse __typed_resp__;
            return rusty::Result<RpcTxnRmwResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_rmwTypedFuture, rrr::i32> async_txn_rmw(const RpcTxnRmwRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_RMW, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_rmwTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_rmwTypedFuture, rrr::i32>::Ok(txn_rmwTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_rmwTypedFuture> await_txn_rmw(const RpcTxnRmwRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_rmw(req, __fu_attr__));
    }
    rusty::Result<RpcTxnRmwResponse, rrr::i32> txn_rmw(const RpcTxnRmwRequest& req) {
        auto __typed_fu_result__ = this->async_txn_rmw(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnRmwResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class txn_readTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_readTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnReadResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnReadResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnReadResponse __typed_resp__;
            return rusty::Result<RpcTxnReadResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_readTypedFuture, rrr::i32> async_txn_read(const RpcTxnReadRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_READ, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_readTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_readTypedFuture, rrr::i32>::Ok(txn_readTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_readTypedFuture> await_txn_read(const RpcTxnReadRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_read(req, __fu_attr__));
    }
    rusty::Result<RpcTxnReadResponse, rrr::i32> txn_read(const RpcTxnReadRequest& req) {
        auto __typed_fu_result__ = this->async_txn_read(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnReadResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class txn_new_orderTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_new_orderTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnNewOrderResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnNewOrderResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnNewOrderResponse __typed_resp__;
            return rusty::Result<RpcTxnNewOrderResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_new_orderTypedFuture, rrr::i32> async_txn_new_order(const RpcTxnNewOrderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_NEW_ORDER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_new_orderTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_new_orderTypedFuture, rrr::i32>::Ok(txn_new_orderTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_new_orderTypedFuture> await_txn_new_order(const RpcTxnNewOrderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_new_order(req, __fu_attr__));
    }
    rusty::Result<RpcTxnNewOrderResponse, rrr::i32> txn_new_order(const RpcTxnNewOrderRequest& req) {
        auto __typed_fu_result__ = this->async_txn_new_order(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnNewOrderResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class txn_paymentTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_paymentTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnPaymentResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnPaymentResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnPaymentResponse __typed_resp__;
            return rusty::Result<RpcTxnPaymentResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_paymentTypedFuture, rrr::i32> async_txn_payment(const RpcTxnPaymentRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_PAYMENT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_paymentTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_paymentTypedFuture, rrr::i32>::Ok(txn_paymentTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_paymentTypedFuture> await_txn_payment(const RpcTxnPaymentRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_payment(req, __fu_attr__));
    }
    rusty::Result<RpcTxnPaymentResponse, rrr::i32> txn_payment(const RpcTxnPaymentRequest& req) {
        auto __typed_fu_result__ = this->async_txn_payment(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnPaymentResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class txn_deliveryTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_deliveryTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnDeliveryResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnDeliveryResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnDeliveryResponse __typed_resp__;
            return rusty::Result<RpcTxnDeliveryResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_deliveryTypedFuture, rrr::i32> async_txn_delivery(const RpcTxnDeliveryRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_DELIVERY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_deliveryTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_deliveryTypedFuture, rrr::i32>::Ok(txn_deliveryTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_deliveryTypedFuture> await_txn_delivery(const RpcTxnDeliveryRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_delivery(req, __fu_attr__));
    }
    rusty::Result<RpcTxnDeliveryResponse, rrr::i32> txn_delivery(const RpcTxnDeliveryRequest& req) {
        auto __typed_fu_result__ = this->async_txn_delivery(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnDeliveryResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class txn_order_statusTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_order_statusTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnOrderStatusResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnOrderStatusResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnOrderStatusResponse __typed_resp__;
            return rusty::Result<RpcTxnOrderStatusResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_order_statusTypedFuture, rrr::i32> async_txn_order_status(const RpcTxnOrderStatusRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_ORDER_STATUS, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_order_statusTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_order_statusTypedFuture, rrr::i32>::Ok(txn_order_statusTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_order_statusTypedFuture> await_txn_order_status(const RpcTxnOrderStatusRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_order_status(req, __fu_attr__));
    }
    rusty::Result<RpcTxnOrderStatusResponse, rrr::i32> txn_order_status(const RpcTxnOrderStatusRequest& req) {
        auto __typed_fu_result__ = this->async_txn_order_status(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnOrderStatusResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class txn_stock_levelTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit txn_stock_levelTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnStockLevelResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnStockLevelResponse, rrr::i32>::Err(__ret__);
            }
            RpcTxnStockLevelResponse __typed_resp__;
            return rusty::Result<RpcTxnStockLevelResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_stock_levelTypedFuture, rrr::i32> async_txn_stock_level(const RpcTxnStockLevelRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(NetworkClientService::TXN_STOCK_LEVEL, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            __m__ << req._req;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_stock_levelTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_stock_levelTypedFuture, rrr::i32>::Ok(txn_stock_levelTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<txn_stock_levelTypedFuture> await_txn_stock_level(const RpcTxnStockLevelRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_txn_stock_level(req, __fu_attr__));
    }
    rusty::Result<RpcTxnStockLevelResponse, rrr::i32> txn_stock_level(const RpcTxnStockLevelRequest& req) {
        auto __typed_fu_result__ = this->async_txn_stock_level(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnStockLevelResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace network_client



