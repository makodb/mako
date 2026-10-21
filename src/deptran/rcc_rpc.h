#pragma once

#include "srpc/srpc.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>

#include "constants.h"
#include "mako_commands.h"
#include <string>
using srpc::Fiber;
namespace janus {

class MultiPaxosService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcForwardToLearnerServerRequest {
        srpc::i32 par_id;
        uint64_t slot;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcForwardToLearnerServerRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.par_id, ar);
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcForwardToLearnerServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.par_id, ar);
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcForwardToLearnerServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcForwardToLearnerServerResponse {
        uint64_t ret_slot;
        ballot_t ret_ballot;
    };
    friend inline void serialize(const RpcForwardToLearnerServerResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ret_slot, ar);
        srpc::Serialize_::serialize(o.ret_ballot, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcForwardToLearnerServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ret_slot, ar);
        srpc::Deserialize_::deserialize(o.ret_ballot, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcForwardToLearnerServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptResponse {
        srpc::i32 ballot;
        srpc::i32 val;
    };
    friend inline void serialize(const RpcBulkAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcSyncLogRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSyncLogRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSyncLogRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogResponse {
        srpc::i32 ballot;
        srpc::i32 val;
        Command ret;
    };
    friend inline void serialize(const RpcSyncLogResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
        srpc::Serialize_::serialize(o.ret, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSyncLogResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
        srpc::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSyncLogResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkDecideRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideResponse {
        srpc::i32 ballot;
        srpc::i32 val;
    };
    friend inline void serialize(const RpcBulkDecideResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkDecideResponse& o) { deserialize(o, ar); return ar; }

    enum {
        FORWARDTOLEARNERSERVER = 0x4aedcaeb,
        BULKACCEPT = 0x18c1d1f9,
        SYNCLOG = 0x510c8e22,
        BULKDECIDE = 0x11c83c3a,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
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
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
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
    virtual void ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, RpcForwardToLearnerServerResponse& resp, srpc::DeferredReply defer) const = 0;
    // @safe
    virtual void BulkAccept(const RpcBulkAcceptRequest& req, RpcBulkAcceptResponse& resp, srpc::DeferredReply defer) const = 0;
    // @safe
    virtual void SyncLog(const RpcSyncLogRequest& req, RpcSyncLogResponse& resp, srpc::DeferredReply defer) const = 0;
    // @safe
    virtual void BulkDecide(const RpcBulkDecideRequest& req, RpcBulkDecideResponse& resp, srpc::DeferredReply defer) const = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __ForwardToLearnerServer__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcForwardToLearnerServerRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __typed_resp__ = std::make_shared<RpcForwardToLearnerServerResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ret_slot, m);
                    srpc::Serialize_::serialize(__typed_resp__->ret_ballot, m);
                },
                []() {});
            this->ForwardToLearnerServer(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcBulkAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __typed_resp__ = std::make_shared<RpcBulkAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncLog__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcSyncLogRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __typed_resp__ = std::make_shared<RpcSyncLogResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                    srpc::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->SyncLog(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkDecide__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcBulkDecideRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __typed_resp__ = std::make_shared<RpcBulkDecideResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class MultiPaxosProxy {
protected:
    srpc::Client* __cl__;
public:
    MultiPaxosProxy(srpc::Client* cl): __cl__(cl) { }
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
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit ForwardToLearnerServerTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32>::Err(__ret__);
            }
            RpcForwardToLearnerServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ret_slot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.ret_ballot, __reply_ar__);
            return rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardToLearnerServerTypedFuture, srpc::i32> async_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::FORWARDTOLEARNERSERVER, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.par_id, __m__);
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardToLearnerServerTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardToLearnerServerTypedFuture, srpc::i32>::Ok(ForwardToLearnerServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32> ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req) {
        auto __typed_fu_result__ = this->async_ForwardToLearnerServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit BulkAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcBulkAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcBulkAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkAcceptTypedFuture, srpc::i32> async_BulkAccept(const RpcBulkAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkAcceptTypedFuture, srpc::i32>::Ok(BulkAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkAcceptResponse, srpc::i32> BulkAccept(const RpcBulkAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_BulkAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SyncLogTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit SyncLogTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSyncLogResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncLogResponse, srpc::i32>::Err(__ret__);
            }
            RpcSyncLogResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcSyncLogResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncLogTypedFuture, srpc::i32> async_SyncLog(const RpcSyncLogRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCLOG, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncLogTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncLogTypedFuture, srpc::i32>::Ok(SyncLogTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncLogResponse, srpc::i32> SyncLog(const RpcSyncLogRequest& req) {
        auto __typed_fu_result__ = this->async_SyncLog(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncLogResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkDecideTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit BulkDecideTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcBulkDecideResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkDecideResponse, srpc::i32>::Err(__ret__);
            }
            RpcBulkDecideResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkDecideResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkDecideTypedFuture, srpc::i32> async_BulkDecide(const RpcBulkDecideRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKDECIDE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkDecideTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkDecideTypedFuture, srpc::i32>::Ok(BulkDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkDecideResponse, srpc::i32> BulkDecide(const RpcBulkDecideRequest& req) {
        auto __typed_fu_result__ = this->async_BulkDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkDecideResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcVoteRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.lst_log_idx, ar);
        srpc::Serialize_::serialize(o.lst_log_term, ar);
        srpc::Serialize_::serialize(o.site_id, ar);
        srpc::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.lst_log_idx, ar);
        srpc::Deserialize_::deserialize(o.lst_log_term, ar);
        srpc::Deserialize_::deserialize(o.site_id, ar);
        srpc::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVoteResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteResponse& o) { deserialize(o, ar); return ar; }

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
    friend inline void serialize(const RpcAppendEntriesRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.leaderCurrentTerm, ar);
        srpc::Serialize_::serialize(o.leaderSiteId, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        srpc::Serialize_::serialize(o.leaderCommitIndex, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
        srpc::Serialize_::serialize(o.leaderNextLogTerm, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderSiteId, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
        srpc::Deserialize_::deserialize(o.leaderNextLogTerm, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntriesResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerAppendOK, ar);
        srpc::Serialize_::serialize(o.followerCurrentTerm, ar);
        srpc::Serialize_::serialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerAppendOK, ar);
        srpc::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        siteid_t leaderSiteId;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.leaderCurrentTerm, ar);
        srpc::Serialize_::serialize(o.leaderSiteId, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        srpc::Serialize_::serialize(o.leaderCommitIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderSiteId, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderCommitIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcEmptyAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerAppendOK, ar);
        srpc::Serialize_::serialize(o.followerCurrentTerm, ar);
        srpc::Serialize_::serialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerAppendOK, ar);
        srpc::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcEmptyAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotRequest {
        uint64_t term;
        uint64_t leader_id;
        uint64_t last_included_index;
        uint64_t last_included_term;
        std::string data;
    };
    friend inline void serialize(const RpcInstallSnapshotRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term, ar);
        srpc::Serialize_::serialize(o.leader_id, ar);
        srpc::Serialize_::serialize(o.last_included_index, ar);
        srpc::Serialize_::serialize(o.last_included_term, ar);
        srpc::Serialize_::serialize(o.data, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcInstallSnapshotRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term, ar);
        srpc::Deserialize_::deserialize(o.leader_id, ar);
        srpc::Deserialize_::deserialize(o.last_included_index, ar);
        srpc::Deserialize_::deserialize(o.last_included_term, ar);
        srpc::Deserialize_::deserialize(o.data, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcInstallSnapshotRequest& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotResponse {
        uint64_t term_out;
    };
    friend inline void serialize(const RpcInstallSnapshotResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term_out, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcInstallSnapshotResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term_out, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcInstallSnapshotResponse& o) { deserialize(o, ar); return ar; }

    enum {
        VOTE = 0x2802b911,
        APPENDENTRIES = 0x3935326f,
        EMPTYAPPENDENTRIES = 0x6e089268,
        INSTALLSNAPSHOT = 0x5276442f,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(VOTE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(EMPTYAPPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(INSTALLSNAPSHOT, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(VOTE);
        svr.unreg(APPENDENTRIES);
        svr.unreg(EMPTYAPPENDENTRIES);
        svr.unreg(INSTALLSNAPSHOT);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        switch (rpc_id) {
        case VOTE: __Vote__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES: __AppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case EMPTYAPPENDENTRIES: __EmptyAppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case INSTALLSNAPSHOT: __InstallSnapshot__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual rusty::Result<RpcVoteResponse, srpc::i32> Vote(const RpcVoteRequest& req) const = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesResponse, srpc::i32> AppendEntries(const RpcAppendEntriesRequest& req) const = 0;
    // @safe
    virtual rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) const = 0;
    // @safe
    virtual rusty::Result<RpcInstallSnapshotResponse, srpc::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) const = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Vote__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.site_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->Vote(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.max_ballot, m);
                            srpc::Serialize_::serialize(__typed_resp__.vote_granted, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderNextLogTerm, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __EmptyAppendEntries__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcEmptyAppendEntriesRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->EmptyAppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __InstallSnapshot__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcInstallSnapshotRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leader_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.last_included_index, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.last_included_term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.data, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->InstallSnapshot(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.term_out, m);
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
    srpc::Client* __cl__;
public:
    RaftProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcVoteRequest = RaftService::RpcVoteRequest;
    using RpcVoteResponse = RaftService::RpcVoteResponse;
    using RpcAppendEntriesRequest = RaftService::RpcAppendEntriesRequest;
    using RpcAppendEntriesResponse = RaftService::RpcAppendEntriesResponse;
    using RpcEmptyAppendEntriesRequest = RaftService::RpcEmptyAppendEntriesRequest;
    using RpcEmptyAppendEntriesResponse = RaftService::RpcEmptyAppendEntriesResponse;
    using RpcInstallSnapshotRequest = RaftService::RpcInstallSnapshotRequest;
    using RpcInstallSnapshotResponse = RaftService::RpcInstallSnapshotResponse;
    class VoteTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit VoteTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcVoteResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteResponse, srpc::i32>::Err(__ret__);
            }
            RpcVoteResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, srpc::i32> async_Vote(const RpcVoteRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.lst_log_idx, __m__);
            srpc::Serialize_::serialize(req.lst_log_term, __m__);
            srpc::Serialize_::serialize(req.site_id, __m__);
            srpc::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteTypedFuture, srpc::i32>::Ok(VoteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteResponse, srpc::i32> Vote(const RpcVoteRequest& req) {
        auto __typed_fu_result__ = this->async_Vote(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AppendEntriesTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAppendEntriesResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Err(__ret__);
            }
            RpcAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, srpc::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIES, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            srpc::Serialize_::serialize(req.leaderSiteId, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            srpc::Serialize_::serialize(req.leaderCommitIndex, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
            srpc::Serialize_::serialize(req.leaderNextLogTerm, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesTypedFuture, srpc::i32>::Ok(AppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesResponse, srpc::i32> AppendEntries(const RpcAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class EmptyAppendEntriesTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit EmptyAppendEntriesTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Err(__ret__);
            }
            RpcEmptyAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            return rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EmptyAppendEntriesTypedFuture, srpc::i32> async_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::EMPTYAPPENDENTRIES, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            srpc::Serialize_::serialize(req.leaderSiteId, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            srpc::Serialize_::serialize(req.leaderCommitIndex, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<EmptyAppendEntriesTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<EmptyAppendEntriesTypedFuture, srpc::i32>::Ok(EmptyAppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_EmptyAppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class InstallSnapshotTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit InstallSnapshotTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcInstallSnapshotResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcInstallSnapshotResponse, srpc::i32>::Err(__ret__);
            }
            RpcInstallSnapshotResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.term_out, __reply_ar__);
            return rusty::Result<RpcInstallSnapshotResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<InstallSnapshotTypedFuture, srpc::i32> async_InstallSnapshot(const RpcInstallSnapshotRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::INSTALLSNAPSHOT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.term, __m__);
            srpc::Serialize_::serialize(req.leader_id, __m__);
            srpc::Serialize_::serialize(req.last_included_index, __m__);
            srpc::Serialize_::serialize(req.last_included_term, __m__);
            srpc::Serialize_::serialize(req.data, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<InstallSnapshotTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<InstallSnapshotTypedFuture, srpc::i32>::Ok(InstallSnapshotTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcInstallSnapshotResponse, srpc::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) {
        auto __typed_fu_result__ = this->async_InstallSnapshot(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcInstallSnapshotResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ServerControlService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcServerShutdownRequest {
    };
    friend inline void serialize(const RpcServerShutdownRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerShutdownRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerShutdownRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerShutdownResponse {
    };
    friend inline void serialize(const RpcServerShutdownResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerShutdownResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerShutdownResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyRequest {
    };
    friend inline void serialize(const RpcServerReadyRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerReadyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerReadyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcServerReadyResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerReadyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerReadyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatRequest {
    };
    friend inline void serialize(const RpcServerHeartBeatRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerHeartBeatRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerHeartBeatRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatResponse {
    };
    friend inline void serialize(const RpcServerHeartBeatResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerHeartBeatResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerHeartBeatResponse& o) { deserialize(o, ar); return ar; }

    enum {
        SERVER_SHUTDOWN = 0x10af16ed,
        SERVER_READY = 0x4780016f,
        SERVER_HEART_BEAT = 0x174c78b8,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
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
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        switch (rpc_id) {
        case SERVER_SHUTDOWN: __server_shutdown__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_READY: __server_ready__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_HEART_BEAT: __server_heart_beat__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void server_shutdown(const RpcServerShutdownRequest& req, RpcServerShutdownResponse& resp, srpc::DeferredReply defer) const = 0;
    // @safe
    virtual void server_ready(const RpcServerReadyRequest& req, RpcServerReadyResponse& resp, srpc::DeferredReply defer) const = 0;
    // @safe
    virtual void server_heart_beat(const RpcServerHeartBeatRequest& req, RpcServerHeartBeatResponse& resp, srpc::DeferredReply defer) const = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __server_shutdown__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcServerShutdownRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerShutdownResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_shutdown(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_ready__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcServerReadyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerReadyResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->server_ready(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_heart_beat__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcServerHeartBeatRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerHeartBeatResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_heart_beat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ServerControlProxy {
protected:
    srpc::Client* __cl__;
public:
    ServerControlProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcServerShutdownRequest = ServerControlService::RpcServerShutdownRequest;
    using RpcServerShutdownResponse = ServerControlService::RpcServerShutdownResponse;
    using RpcServerReadyRequest = ServerControlService::RpcServerReadyRequest;
    using RpcServerReadyResponse = ServerControlService::RpcServerReadyResponse;
    using RpcServerHeartBeatRequest = ServerControlService::RpcServerHeartBeatRequest;
    using RpcServerHeartBeatResponse = ServerControlService::RpcServerHeartBeatResponse;
    class server_shutdownTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_shutdownTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcServerShutdownResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerShutdownResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerShutdownResponse __typed_resp__;
            return rusty::Result<RpcServerShutdownResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_shutdownTypedFuture, srpc::i32> async_server_shutdown(const RpcServerShutdownRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_SHUTDOWN, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_shutdownTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_shutdownTypedFuture, srpc::i32>::Ok(server_shutdownTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerShutdownResponse, srpc::i32> server_shutdown(const RpcServerShutdownRequest& req) {
        auto __typed_fu_result__ = this->async_server_shutdown(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerShutdownResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_readyTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_readyTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcServerReadyResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerReadyResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerReadyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerReadyResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_readyTypedFuture, srpc::i32> async_server_ready(const RpcServerReadyRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_READY, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_readyTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_readyTypedFuture, srpc::i32>::Ok(server_readyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerReadyResponse, srpc::i32> server_ready(const RpcServerReadyRequest& req) {
        auto __typed_fu_result__ = this->async_server_ready(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerReadyResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_heart_beatTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_heart_beatTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcServerHeartBeatResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerHeartBeatResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerHeartBeatResponse __typed_resp__;
            return rusty::Result<RpcServerHeartBeatResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beatTypedFuture, srpc::i32> async_server_heart_beat(const RpcServerHeartBeatRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_heart_beatTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_heart_beatTypedFuture, srpc::i32>::Ok(server_heart_beatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerHeartBeatResponse, srpc::i32> server_heart_beat(const RpcServerHeartBeatRequest& req) {
        auto __typed_fu_result__ = this->async_server_heart_beat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerHeartBeatResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcReadConfigKeyRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.key, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcReadConfigKeyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.key, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcReadConfigKeyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcReadConfigKeyResponse {
        srpc::i32 found;
        std::string value;
    };
    friend inline void serialize(const RpcReadConfigKeyResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.found, ar);
        srpc::Serialize_::serialize(o.value, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcReadConfigKeyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.found, ar);
        srpc::Deserialize_::deserialize(o.value, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcReadConfigKeyResponse& o) { deserialize(o, ar); return ar; }

    enum {
        READCONFIGKEY = 0x584cdad1,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
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
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        switch (rpc_id) {
        case READCONFIGKEY: __ReadConfigKey__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void ReadConfigKey(const RpcReadConfigKeyRequest& req, RpcReadConfigKeyResponse& resp, srpc::DeferredReply defer) const = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __ReadConfigKey__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) const {
        // @unsafe
        {
            RpcReadConfigKeyRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.key, __req_ar__);
            if (__req_ar__.failed()) {
                srpc::reject_malformed_request(*req, weak_sconn);
                return;
            }
            auto __typed_resp__ = std::make_shared<RpcReadConfigKeyResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->found, m);
                    srpc::Serialize_::serialize(__typed_resp__->value, m);
                },
                []() {});
            this->ReadConfigKey(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ConfigKvServiceProxy {
protected:
    srpc::Client* __cl__;
public:
    ConfigKvServiceProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcReadConfigKeyRequest = ConfigKvServiceService::RpcReadConfigKeyRequest;
    using RpcReadConfigKeyResponse = ConfigKvServiceService::RpcReadConfigKeyResponse;
    class ReadConfigKeyTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit ReadConfigKeyTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcReadConfigKeyResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcReadConfigKeyResponse, srpc::i32>::Err(__ret__);
            }
            RpcReadConfigKeyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.found, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.value, __reply_ar__);
            return rusty::Result<RpcReadConfigKeyResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReadConfigKeyTypedFuture, srpc::i32> async_ReadConfigKey(const RpcReadConfigKeyRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigKvServiceService::READCONFIGKEY, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.key, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ReadConfigKeyTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ReadConfigKeyTypedFuture, srpc::i32>::Ok(ReadConfigKeyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcReadConfigKeyResponse, srpc::i32> ReadConfigKey(const RpcReadConfigKeyRequest& req) {
        auto __typed_fu_result__ = this->async_ReadConfigKey(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReadConfigKeyResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace janus



