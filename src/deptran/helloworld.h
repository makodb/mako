#pragma once

import rrr;

#include <errno.h>
#include <memory>


namespace helloworld_client {

class HelloworldClientService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcTxnReadRequest {
        std::vector<rrr::i64> _req;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTxnReadRequest& o) {
        m << o._req;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTxnReadRequest& o) {
        m >> o._req;
        return m;
    }

    struct RpcTxnReadResponse {
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTxnReadResponse& o) {
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTxnReadResponse& o) {
        m >> o.val;
        return m;
    }

    enum {
        TXN_READ = 0x4e5916a6,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(TXN_READ, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(TXN_READ);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case TXN_READ: __txn_read__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void txn_read(const RpcTxnReadRequest& req, RpcTxnReadResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __txn_read__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnReadRequest __typed_req__;
            req->m >> __typed_req__._req;
            auto __typed_resp__ = std::make_shared<RpcTxnReadResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->val;
                },
                []() {});
            this->txn_read(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class HelloworldClientProxy {
protected:
    rrr::Client* __cl__;
public:
    HelloworldClientProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcTxnReadRequest = HelloworldClientService::RpcTxnReadRequest;
    using RpcTxnReadResponse = HelloworldClientService::RpcTxnReadResponse;
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
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcTxnReadResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<txn_readTypedFuture, rrr::i32> async_txn_read(const RpcTxnReadRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(HelloworldClientService::TXN_READ, __fu_attr__, [&](rrr::Marshal& __m__) {
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
};

} // namespace helloworld_client



