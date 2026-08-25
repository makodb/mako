#pragma once

#include "srpc/srpc.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>


namespace helloworld_client {

class HelloworldClientService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcTxnReadRequest {
        std::vector<srpc::i64> _req;
    };
    friend inline void serialize(const RpcTxnReadRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o._req, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTxnReadRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTxnReadRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o._req, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTxnReadRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTxnReadResponse {
        srpc::i32 val;
    };
    friend inline void serialize(const RpcTxnReadResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTxnReadResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTxnReadResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTxnReadResponse& o) { deserialize(o, ar); return ar; }

    enum {
        TXN_READ = 0x4e5916a6,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
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
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case TXN_READ: __txn_read__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void txn_read(const RpcTxnReadRequest& req, RpcTxnReadResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __txn_read__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTxnReadRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__._req, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTxnReadResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->txn_read(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class HelloworldClientProxy {
protected:
    srpc::Client* __cl__;
public:
    HelloworldClientProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcTxnReadRequest = HelloworldClientService::RpcTxnReadRequest;
    using RpcTxnReadResponse = HelloworldClientService::RpcTxnReadResponse;
    class txn_readTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit txn_readTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTxnReadResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTxnReadResponse, srpc::i32>::Err(__ret__);
            }
            RpcTxnReadResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcTxnReadResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<txn_readTypedFuture, srpc::i32> async_txn_read(const RpcTxnReadRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(HelloworldClientService::TXN_READ, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req._req, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<txn_readTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<txn_readTypedFuture, srpc::i32>::Ok(txn_readTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTxnReadResponse, srpc::i32> txn_read(const RpcTxnReadRequest& req) {
        auto __typed_fu_result__ = this->async_txn_read(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTxnReadResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace helloworld_client



