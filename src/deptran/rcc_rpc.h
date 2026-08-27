#pragma once

#include "rrr/rrr.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>

#include "constants.h"
#include "mako_commands.h"
#include <string>
using rrr::Fiber;
namespace janus {

class MultiPaxosService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcForwardToLearnerServerRequest {
        rrr::i32 par_id;
        uint64_t slot;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcForwardToLearnerServerRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.par_id, ar);
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcForwardToLearnerServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.par_id, ar);
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcForwardToLearnerServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcForwardToLearnerServerResponse {
        uint64_t ret_slot;
        ballot_t ret_ballot;
    };
    friend inline void serialize(const RpcForwardToLearnerServerResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ret_slot, ar);
        rrr::Serialize_::serialize(o.ret_ballot, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcForwardToLearnerServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ret_slot, ar);
        rrr::Deserialize_::deserialize(o.ret_ballot, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcForwardToLearnerServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline void serialize(const RpcBulkAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcSyncLogRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSyncLogRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSyncLogRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogResponse {
        rrr::i32 ballot;
        rrr::i32 val;
        Command ret;
    };
    friend inline void serialize(const RpcSyncLogResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
        rrr::Serialize_::serialize(o.ret, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSyncLogResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
        rrr::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSyncLogResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkDecideRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline void serialize(const RpcBulkDecideResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkDecideResponse& o) { deserialize(o, ar); return ar; }

    enum {
        FORWARDTOLEARNERSERVER = 0x4aedcaeb,
        BULKACCEPT = 0x18c1d1f9,
        SYNCLOG = 0x510c8e22,
        BULKDECIDE = 0x11c83c3a,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(FORWARDTOLEARNERSERVER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SYNCLOG, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKDECIDE, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(FORWARDTOLEARNERSERVER);
        svr.unreg(BULKACCEPT);
        svr.unreg(SYNCLOG);
        svr.unreg(BULKDECIDE);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case FORWARDTOLEARNERSERVER: __ForwardToLearnerServer__wrapper__(std::move(req), weak_sconn); break;
        case BULKACCEPT: __BulkAccept__wrapper__(std::move(req), weak_sconn); break;
        case SYNCLOG: __SyncLog__wrapper__(std::move(req), weak_sconn); break;
        case BULKDECIDE: __BulkDecide__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkAccept(const RpcBulkAcceptRequest& req, RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SyncLog(const RpcSyncLogRequest& req, RpcSyncLogResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkDecide(const RpcBulkDecideRequest& req, RpcBulkDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __ForwardToLearnerServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardToLearnerServerRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcForwardToLearnerServerResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ret_slot, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret_ballot, m);
                },
                []() {});
            this->ForwardToLearnerServer(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkAcceptRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcBulkAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncLog__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncLogRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSyncLogResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->SyncLog(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkDecide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkDecideRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcBulkDecideResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class MultiPaxosProxy {
protected:
    rrr::Client* __cl__;
public:
    MultiPaxosProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcForwardToLearnerServerRequest = MultiPaxosService::RpcForwardToLearnerServerRequest;
    using RpcForwardToLearnerServerResponse = MultiPaxosService::RpcForwardToLearnerServerResponse;
    using RpcBulkAcceptRequest = MultiPaxosService::RpcBulkAcceptRequest;
    using RpcBulkAcceptResponse = MultiPaxosService::RpcBulkAcceptResponse;
    using RpcSyncLogRequest = MultiPaxosService::RpcSyncLogRequest;
    using RpcSyncLogResponse = MultiPaxosService::RpcSyncLogResponse;
    using RpcBulkDecideRequest = MultiPaxosService::RpcBulkDecideRequest;
    using RpcBulkDecideResponse = MultiPaxosService::RpcBulkDecideResponse;
    class ForwardToLearnerServerTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ForwardToLearnerServerTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Err(__ret__);
            }
            RpcForwardToLearnerServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ret_slot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_ballot, __reply_ar__);
            return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32> async_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::FORWARDTOLEARNERSERVER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.par_id, __m__);
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32>::Ok(ForwardToLearnerServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32> ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req) {
        auto __typed_fu_result__ = this->async_ForwardToLearnerServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit BulkAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcBulkAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcBulkAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkAcceptTypedFuture, rrr::i32> async_BulkAccept(const RpcBulkAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkAcceptTypedFuture, rrr::i32>::Ok(BulkAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkAcceptResponse, rrr::i32> BulkAccept(const RpcBulkAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_BulkAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SyncLogTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SyncLogTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSyncLogResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncLogResponse, rrr::i32>::Err(__ret__);
            }
            RpcSyncLogResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcSyncLogResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncLogTypedFuture, rrr::i32> async_SyncLog(const RpcSyncLogRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCLOG, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncLogTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncLogTypedFuture, rrr::i32>::Ok(SyncLogTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncLogResponse, rrr::i32> SyncLog(const RpcSyncLogRequest& req) {
        auto __typed_fu_result__ = this->async_SyncLog(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncLogResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkDecideTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit BulkDecideTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcBulkDecideResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Err(__ret__);
            }
            RpcBulkDecideResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkDecideTypedFuture, rrr::i32> async_BulkDecide(const RpcBulkDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKDECIDE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkDecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkDecideTypedFuture, rrr::i32>::Ok(BulkDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkDecideResponse, rrr::i32> BulkDecide(const RpcBulkDecideRequest& req) {
        auto __typed_fu_result__ = this->async_BulkDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class RaftService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcVoteRequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        siteid_t site_id;
        ballot_t cur_term;
    };
    friend inline void serialize(const RpcVoteRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.lst_log_idx, ar);
        rrr::Serialize_::serialize(o.lst_log_term, ar);
        rrr::Serialize_::serialize(o.site_id, ar);
        rrr::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.lst_log_idx, ar);
        rrr::Deserialize_::deserialize(o.lst_log_term, ar);
        rrr::Deserialize_::deserialize(o.site_id, ar);
        rrr::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVoteResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteResponse& o) { deserialize(o, ar); return ar; }

    struct RpcVoteDurableRequest {
        ballot_t term;
        siteid_t voter_id;
    };
    friend inline void serialize(const RpcVoteDurableRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.voter_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteDurableRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteDurableRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.voter_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteDurableRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteDurableResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcVoteDurableResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteDurableResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteDurableResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteDurableResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        siteid_t leaderSiteId;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        Command cmd;
        uint64_t leaderNextLogTerm;
    };
    friend inline void serialize(const RpcAppendEntriesRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.leaderCurrentTerm, ar);
        rrr::Serialize_::serialize(o.leaderSiteId, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        rrr::Serialize_::serialize(o.leaderCommitIndex, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
        rrr::Serialize_::serialize(o.leaderNextLogTerm, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderSiteId, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
        rrr::Deserialize_::deserialize(o.leaderNextLogTerm, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline void serialize(const RpcAppendEntriesResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerAppendOK, ar);
        rrr::Serialize_::serialize(o.followerCurrentTerm, ar);
        rrr::Serialize_::serialize(o.followerLastLogIndex, ar);
        rrr::Serialize_::serialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerAppendOK, ar);
        rrr::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.followerLastLogIndex, ar);
        rrr::Deserialize_::deserialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        siteid_t leaderSiteId;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        bool_t trigger_election_now;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.leaderCurrentTerm, ar);
        rrr::Serialize_::serialize(o.leaderSiteId, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        rrr::Serialize_::serialize(o.leaderCommitIndex, ar);
        rrr::Serialize_::serialize(o.trigger_election_now, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderSiteId, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        rrr::Deserialize_::deserialize(o.trigger_election_now, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcEmptyAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerAppendOK, ar);
        rrr::Serialize_::serialize(o.followerCurrentTerm, ar);
        rrr::Serialize_::serialize(o.followerLastLogIndex, ar);
        rrr::Serialize_::serialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerAppendOK, ar);
        rrr::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.followerLastLogIndex, ar);
        rrr::Deserialize_::deserialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcEmptyAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesDurableRequest {
        ballot_t term;
        siteid_t follower_id;
        uint64_t lastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntriesDurableRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.follower_id, ar);
        rrr::Serialize_::serialize(o.lastLogIndex, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesDurableRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesDurableRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.follower_id, ar);
        rrr::Deserialize_::deserialize(o.lastLogIndex, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesDurableRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesDurableResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcAppendEntriesDurableResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesDurableResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesDurableResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesDurableResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTimeoutNowRequest {
        uint64_t leaderTerm;
        siteid_t leaderSiteId;
    };
    friend inline void serialize(const RpcTimeoutNowRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.leaderTerm, ar);
        rrr::Serialize_::serialize(o.leaderSiteId, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTimeoutNowRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTimeoutNowRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.leaderTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderSiteId, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTimeoutNowRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTimeoutNowResponse {
        uint64_t followerTerm;
        bool_t success;
    };
    friend inline void serialize(const RpcTimeoutNowResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerTerm, ar);
        rrr::Serialize_::serialize(o.success, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTimeoutNowResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTimeoutNowResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerTerm, ar);
        rrr::Deserialize_::deserialize(o.success, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTimeoutNowResponse& o) { deserialize(o, ar); return ar; }

    struct RpcNotifyRestartRequest {
        siteid_t restartedSiteId;
    };
    friend inline void serialize(const RpcNotifyRestartRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.restartedSiteId, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcNotifyRestartRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNotifyRestartRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.restartedSiteId, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcNotifyRestartRequest& o) { deserialize(o, ar); return ar; }

    struct RpcNotifyRestartResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcNotifyRestartResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcNotifyRestartResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNotifyRestartResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcNotifyRestartResponse& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotRequest {
        uint64_t term;
        uint64_t leader_id;
        uint64_t last_included_index;
        uint64_t last_included_term;
        std::string data;
    };
    friend inline void serialize(const RpcInstallSnapshotRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.leader_id, ar);
        rrr::Serialize_::serialize(o.last_included_index, ar);
        rrr::Serialize_::serialize(o.last_included_term, ar);
        rrr::Serialize_::serialize(o.data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcInstallSnapshotRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.leader_id, ar);
        rrr::Deserialize_::deserialize(o.last_included_index, ar);
        rrr::Deserialize_::deserialize(o.last_included_term, ar);
        rrr::Deserialize_::deserialize(o.data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcInstallSnapshotRequest& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotResponse {
        uint64_t term_out;
    };
    friend inline void serialize(const RpcInstallSnapshotResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term_out, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcInstallSnapshotResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term_out, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcInstallSnapshotResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAddServerRequest {
        uint64_t term;
        uint64_t new_server_id;
        std::string new_server_addr;
    };
    friend inline void serialize(const RpcAddServerRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.new_server_id, ar);
        rrr::Serialize_::serialize(o.new_server_addr, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAddServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddServerRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.new_server_id, ar);
        rrr::Deserialize_::deserialize(o.new_server_addr, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAddServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAddServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline void serialize(const RpcAddServerResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.success, ar);
        rrr::Serialize_::serialize(o.error_msg, ar);
        rrr::Serialize_::serialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAddServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddServerResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.success, ar);
        rrr::Deserialize_::deserialize(o.error_msg, ar);
        rrr::Deserialize_::deserialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAddServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRemoveServerRequest {
        uint64_t term;
        uint64_t server_id;
    };
    friend inline void serialize(const RpcRemoveServerRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.server_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRemoveServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRemoveServerRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.server_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRemoveServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRemoveServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline void serialize(const RpcRemoveServerResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.success, ar);
        rrr::Serialize_::serialize(o.error_msg, ar);
        rrr::Serialize_::serialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRemoveServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRemoveServerResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.success, ar);
        rrr::Deserialize_::deserialize(o.error_msg, ar);
        rrr::Deserialize_::deserialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRemoveServerResponse& o) { deserialize(o, ar); return ar; }

    enum {
        VOTE = 0x2802b911,
        VOTEDURABLE = 0x538739a2,
        APPENDENTRIES = 0x3935326f,
        EMPTYAPPENDENTRIES = 0x6e089268,
        APPENDENTRIESDURABLE = 0x1e8b9027,
        TIMEOUTNOW = 0x59a6a5f9,
        NOTIFYRESTART = 0x3df03452,
        INSTALLSNAPSHOT = 0x5276442f,
        ADDSERVER = 0x10e10b20,
        REMOVESERVER = 0x68ea2fc0,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(VOTE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(VOTEDURABLE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(EMPTYAPPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIESDURABLE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TIMEOUTNOW, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(NOTIFYRESTART, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(INSTALLSNAPSHOT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ADDSERVER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(REMOVESERVER, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(VOTE);
        svr.unreg(VOTEDURABLE);
        svr.unreg(APPENDENTRIES);
        svr.unreg(EMPTYAPPENDENTRIES);
        svr.unreg(APPENDENTRIESDURABLE);
        svr.unreg(TIMEOUTNOW);
        svr.unreg(NOTIFYRESTART);
        svr.unreg(INSTALLSNAPSHOT);
        svr.unreg(ADDSERVER);
        svr.unreg(REMOVESERVER);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case VOTE: __Vote__wrapper__(std::move(req), weak_sconn); break;
        case VOTEDURABLE: __VoteDurable__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES: __AppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case EMPTYAPPENDENTRIES: __EmptyAppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIESDURABLE: __AppendEntriesDurable__wrapper__(std::move(req), weak_sconn); break;
        case TIMEOUTNOW: __TimeoutNow__wrapper__(std::move(req), weak_sconn); break;
        case NOTIFYRESTART: __NotifyRestart__wrapper__(std::move(req), weak_sconn); break;
        case INSTALLSNAPSHOT: __InstallSnapshot__wrapper__(std::move(req), weak_sconn); break;
        case ADDSERVER: __AddServer__wrapper__(std::move(req), weak_sconn); break;
        case REMOVESERVER: __RemoveServer__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual rusty::Result<RpcVoteResponse, rrr::i32> Vote(const RpcVoteRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcVoteDurableResponse, rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesResponse, rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcTimeoutNowResponse, rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcNotifyRestartResponse, rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcInstallSnapshotResponse, rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAddServerResponse, rrr::i32> AddServer(const RpcAddServerRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcRemoveServerResponse, rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Vote__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.site_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->Vote(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.max_ballot, m);
                            rrr::Serialize_::serialize(__typed_resp__.vote_granted, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __VoteDurable__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteDurableRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.voter_id, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->VoteDurable(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderNextLogTerm, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerAckType, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __EmptyAppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcEmptyAppendEntriesRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.trigger_election_now, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->EmptyAppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerAckType, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntriesDurable__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesDurableRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.follower_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.lastLogIndex, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntriesDurable(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __TimeoutNow__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTimeoutNowRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.leaderTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->TimeoutNow(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.followerTerm, m);
                            rrr::Serialize_::serialize(__typed_resp__.success, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __NotifyRestart__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcNotifyRestartRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.restartedSiteId, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->NotifyRestart(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __InstallSnapshot__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcInstallSnapshotRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leader_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.last_included_index, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.last_included_term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.data, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->InstallSnapshot(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.term_out, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AddServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAddServerRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.new_server_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.new_server_addr, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AddServer(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.success, m);
                            rrr::Serialize_::serialize(__typed_resp__.error_msg, m);
                            rrr::Serialize_::serialize(__typed_resp__.leader_hint, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __RemoveServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRemoveServerRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.server_id, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->RemoveServer(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.success, m);
                            rrr::Serialize_::serialize(__typed_resp__.error_msg, m);
                            rrr::Serialize_::serialize(__typed_resp__.leader_hint, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
};

class RaftProxy {
protected:
    rrr::Client* __cl__;
public:
    RaftProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcVoteRequest = RaftService::RpcVoteRequest;
    using RpcVoteResponse = RaftService::RpcVoteResponse;
    using RpcVoteDurableRequest = RaftService::RpcVoteDurableRequest;
    using RpcVoteDurableResponse = RaftService::RpcVoteDurableResponse;
    using RpcAppendEntriesRequest = RaftService::RpcAppendEntriesRequest;
    using RpcAppendEntriesResponse = RaftService::RpcAppendEntriesResponse;
    using RpcEmptyAppendEntriesRequest = RaftService::RpcEmptyAppendEntriesRequest;
    using RpcEmptyAppendEntriesResponse = RaftService::RpcEmptyAppendEntriesResponse;
    using RpcAppendEntriesDurableRequest = RaftService::RpcAppendEntriesDurableRequest;
    using RpcAppendEntriesDurableResponse = RaftService::RpcAppendEntriesDurableResponse;
    using RpcTimeoutNowRequest = RaftService::RpcTimeoutNowRequest;
    using RpcTimeoutNowResponse = RaftService::RpcTimeoutNowResponse;
    using RpcNotifyRestartRequest = RaftService::RpcNotifyRestartRequest;
    using RpcNotifyRestartResponse = RaftService::RpcNotifyRestartResponse;
    using RpcInstallSnapshotRequest = RaftService::RpcInstallSnapshotRequest;
    using RpcInstallSnapshotResponse = RaftService::RpcInstallSnapshotResponse;
    using RpcAddServerRequest = RaftService::RpcAddServerRequest;
    using RpcAddServerResponse = RaftService::RpcAddServerResponse;
    using RpcRemoveServerRequest = RaftService::RpcRemoveServerRequest;
    using RpcRemoveServerResponse = RaftService::RpcRemoveServerResponse;
    class VoteTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit VoteTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcVoteResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteResponse, rrr::i32>::Err(__ret__);
            }
            RpcVoteResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, rrr::i32> async_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.lst_log_idx, __m__);
            rrr::Serialize_::serialize(req.lst_log_term, __m__);
            rrr::Serialize_::serialize(req.site_id, __m__);
            rrr::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteTypedFuture, rrr::i32>::Ok(VoteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteResponse, rrr::i32> Vote(const RpcVoteRequest& req) {
        auto __typed_fu_result__ = this->async_Vote(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class VoteDurableTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit VoteDurableTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcVoteDurableResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Err(__ret__);
            }
            RpcVoteDurableResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteDurableTypedFuture, rrr::i32> async_VoteDurable(const RpcVoteDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTEDURABLE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.voter_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteDurableTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteDurableTypedFuture, rrr::i32>::Ok(VoteDurableTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteDurableResponse, rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) {
        auto __typed_fu_result__ = this->async_VoteDurable(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AppendEntriesTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAppendEntriesResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Err(__ret__);
            }
            RpcAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, rrr::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIES, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            rrr::Serialize_::serialize(req.leaderSiteId, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            rrr::Serialize_::serialize(req.leaderCommitIndex, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
            rrr::Serialize_::serialize(req.leaderNextLogTerm, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesTypedFuture, rrr::i32>::Ok(AppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesResponse, rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class EmptyAppendEntriesTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit EmptyAppendEntriesTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Err(__ret__);
            }
            RpcEmptyAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32> async_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::EMPTYAPPENDENTRIES, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            rrr::Serialize_::serialize(req.leaderSiteId, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            rrr::Serialize_::serialize(req.leaderCommitIndex, __m__);
            rrr::Serialize_::serialize(req.trigger_election_now, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32>::Ok(EmptyAppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_EmptyAppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesDurableTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AppendEntriesDurableTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Err(__ret__);
            }
            RpcAppendEntriesDurableResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32> async_AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIESDURABLE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.follower_id, __m__);
            rrr::Serialize_::serialize(req.lastLogIndex, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32>::Ok(AppendEntriesDurableTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntriesDurable(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TimeoutNowTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit TimeoutNowTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcTimeoutNowResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Err(__ret__);
            }
            RpcTimeoutNowResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.followerTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TimeoutNowTypedFuture, rrr::i32> async_TimeoutNow(const RpcTimeoutNowRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::TIMEOUTNOW, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.leaderTerm, __m__);
            rrr::Serialize_::serialize(req.leaderSiteId, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TimeoutNowTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TimeoutNowTypedFuture, rrr::i32>::Ok(TimeoutNowTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTimeoutNowResponse, rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) {
        auto __typed_fu_result__ = this->async_TimeoutNow(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class NotifyRestartTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit NotifyRestartTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcNotifyRestartResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Err(__ret__);
            }
            RpcNotifyRestartResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<NotifyRestartTypedFuture, rrr::i32> async_NotifyRestart(const RpcNotifyRestartRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::NOTIFYRESTART, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.restartedSiteId, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<NotifyRestartTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<NotifyRestartTypedFuture, rrr::i32>::Ok(NotifyRestartTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcNotifyRestartResponse, rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) {
        auto __typed_fu_result__ = this->async_NotifyRestart(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class InstallSnapshotTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit InstallSnapshotTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcInstallSnapshotResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Err(__ret__);
            }
            RpcInstallSnapshotResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.term_out, __reply_ar__);
            return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<InstallSnapshotTypedFuture, rrr::i32> async_InstallSnapshot(const RpcInstallSnapshotRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::INSTALLSNAPSHOT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.leader_id, __m__);
            rrr::Serialize_::serialize(req.last_included_index, __m__);
            rrr::Serialize_::serialize(req.last_included_term, __m__);
            rrr::Serialize_::serialize(req.data, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<InstallSnapshotTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<InstallSnapshotTypedFuture, rrr::i32>::Ok(InstallSnapshotTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcInstallSnapshotResponse, rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) {
        auto __typed_fu_result__ = this->async_InstallSnapshot(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AddServerTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AddServerTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAddServerResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAddServerResponse, rrr::i32>::Err(__ret__);
            }
            RpcAddServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcAddServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AddServerTypedFuture, rrr::i32> async_AddServer(const RpcAddServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::ADDSERVER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.new_server_id, __m__);
            rrr::Serialize_::serialize(req.new_server_addr, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AddServerTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AddServerTypedFuture, rrr::i32>::Ok(AddServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAddServerResponse, rrr::i32> AddServer(const RpcAddServerRequest& req) {
        auto __typed_fu_result__ = this->async_AddServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAddServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RemoveServerTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RemoveServerTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRemoveServerResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Err(__ret__);
            }
            RpcRemoveServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RemoveServerTypedFuture, rrr::i32> async_RemoveServer(const RpcRemoveServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::REMOVESERVER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.server_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RemoveServerTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RemoveServerTypedFuture, rrr::i32>::Ok(RemoveServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRemoveServerResponse, rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) {
        auto __typed_fu_result__ = this->async_RemoveServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ServerControlService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcServerShutdownRequest {
    };
    friend inline void serialize(const RpcServerShutdownRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerShutdownRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerShutdownRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerShutdownResponse {
    };
    friend inline void serialize(const RpcServerShutdownResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerShutdownResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerShutdownResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyRequest {
    };
    friend inline void serialize(const RpcServerReadyRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerReadyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerReadyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcServerReadyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerReadyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerReadyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatRequest {
    };
    friend inline void serialize(const RpcServerHeartBeatRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerHeartBeatRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerHeartBeatRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatResponse {
    };
    friend inline void serialize(const RpcServerHeartBeatResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerHeartBeatResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerHeartBeatResponse& o) { deserialize(o, ar); return ar; }

    enum {
        SERVER_SHUTDOWN = 0x10af16ed,
        SERVER_READY = 0x4780016f,
        SERVER_HEART_BEAT = 0x174c78b8,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(SERVER_SHUTDOWN, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SERVER_READY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SERVER_HEART_BEAT, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(SERVER_SHUTDOWN);
        svr.unreg(SERVER_READY);
        svr.unreg(SERVER_HEART_BEAT);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case SERVER_SHUTDOWN: __server_shutdown__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_READY: __server_ready__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_HEART_BEAT: __server_heart_beat__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void server_shutdown(const RpcServerShutdownRequest& req, RpcServerShutdownResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void server_ready(const RpcServerReadyRequest& req, RpcServerReadyResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void server_heart_beat(const RpcServerHeartBeatRequest& req, RpcServerHeartBeatResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __server_shutdown__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerShutdownRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerShutdownResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_shutdown(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_ready__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerReadyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerReadyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->server_ready(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_heart_beat__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerHeartBeatRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerHeartBeatResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_heart_beat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ServerControlProxy {
protected:
    rrr::Client* __cl__;
public:
    ServerControlProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcServerShutdownRequest = ServerControlService::RpcServerShutdownRequest;
    using RpcServerShutdownResponse = ServerControlService::RpcServerShutdownResponse;
    using RpcServerReadyRequest = ServerControlService::RpcServerReadyRequest;
    using RpcServerReadyResponse = ServerControlService::RpcServerReadyResponse;
    using RpcServerHeartBeatRequest = ServerControlService::RpcServerHeartBeatRequest;
    using RpcServerHeartBeatResponse = ServerControlService::RpcServerHeartBeatResponse;
    class server_shutdownTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_shutdownTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcServerShutdownResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerShutdownResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerShutdownResponse __typed_resp__;
            return rusty::Result<RpcServerShutdownResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_shutdownTypedFuture, rrr::i32> async_server_shutdown(const RpcServerShutdownRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_SHUTDOWN, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_shutdownTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_shutdownTypedFuture, rrr::i32>::Ok(server_shutdownTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerShutdownResponse, rrr::i32> server_shutdown(const RpcServerShutdownRequest& req) {
        auto __typed_fu_result__ = this->async_server_shutdown(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerShutdownResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_readyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_readyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcServerReadyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerReadyResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerReadyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_readyTypedFuture, rrr::i32> async_server_ready(const RpcServerReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_READY, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_readyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_readyTypedFuture, rrr::i32>::Ok(server_readyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerReadyResponse, rrr::i32> server_ready(const RpcServerReadyRequest& req) {
        auto __typed_fu_result__ = this->async_server_ready(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerReadyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_heart_beatTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_heart_beatTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcServerHeartBeatResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerHeartBeatResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerHeartBeatResponse __typed_resp__;
            return rusty::Result<RpcServerHeartBeatResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beatTypedFuture, rrr::i32> async_server_heart_beat(const RpcServerHeartBeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_heart_beatTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_heart_beatTypedFuture, rrr::i32>::Ok(server_heart_beatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerHeartBeatResponse, rrr::i32> server_heart_beat(const RpcServerHeartBeatRequest& req) {
        auto __typed_fu_result__ = this->async_server_heart_beat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerHeartBeatResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ConfigKvServiceService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcReadConfigKeyRequest {
        std::string key;
    };
    friend inline void serialize(const RpcReadConfigKeyRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.key, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcReadConfigKeyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.key, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcReadConfigKeyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcReadConfigKeyResponse {
        rrr::i32 found;
        std::string value;
    };
    friend inline void serialize(const RpcReadConfigKeyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.found, ar);
        rrr::Serialize_::serialize(o.value, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcReadConfigKeyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.found, ar);
        rrr::Deserialize_::deserialize(o.value, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcReadConfigKeyResponse& o) { deserialize(o, ar); return ar; }

    enum {
        READCONFIGKEY = 0x584cdad1,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(READCONFIGKEY, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(READCONFIGKEY);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case READCONFIGKEY: __ReadConfigKey__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void ReadConfigKey(const RpcReadConfigKeyRequest& req, RpcReadConfigKeyResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __ReadConfigKey__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcReadConfigKeyRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.key, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcReadConfigKeyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->found, m);
                    rrr::Serialize_::serialize(__typed_resp__->value, m);
                },
                []() {});
            this->ReadConfigKey(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ConfigKvServiceProxy {
protected:
    rrr::Client* __cl__;
public:
    ConfigKvServiceProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcReadConfigKeyRequest = ConfigKvServiceService::RpcReadConfigKeyRequest;
    using RpcReadConfigKeyResponse = ConfigKvServiceService::RpcReadConfigKeyResponse;
    class ReadConfigKeyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ReadConfigKeyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcReadConfigKeyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcReadConfigKeyResponse, rrr::i32>::Err(__ret__);
            }
            RpcReadConfigKeyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.found, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.value, __reply_ar__);
            return rusty::Result<RpcReadConfigKeyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReadConfigKeyTypedFuture, rrr::i32> async_ReadConfigKey(const RpcReadConfigKeyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigKvServiceService::READCONFIGKEY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.key, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ReadConfigKeyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ReadConfigKeyTypedFuture, rrr::i32>::Ok(ReadConfigKeyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcReadConfigKeyResponse, rrr::i32> ReadConfigKey(const RpcReadConfigKeyRequest& req) {
        auto __typed_fu_result__ = this->async_ReadConfigKey(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReadConfigKeyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace janus



