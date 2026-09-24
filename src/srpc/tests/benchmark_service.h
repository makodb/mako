#pragma once

#include "srpc/srpc.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>

// #include <math.h>

// optional %%: marks header section, code above will be copied into begin of generated C++ header
namespace benchmark {

struct point3 {
    double x;
    double y;
    double z;
};

inline void serialize(const point3& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.x, ar);
    srpc::Serialize_::serialize(o.y, ar);
    srpc::Serialize_::serialize(o.z, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const point3& o) { serialize(o, ar); return ar; }

inline void deserialize(point3& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.x, ar);
    srpc::Deserialize_::deserialize(o.y, ar);
    srpc::Deserialize_::deserialize(o.z, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, point3& o) { deserialize(o, ar); return ar; }

class BenchmarkService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcFastPrimeRequest {
        srpc::i32 n;
    };
    friend inline void serialize(const RpcFastPrimeRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.n, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastPrimeRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastPrimeRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.n, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastPrimeRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastPrimeResponse {
        srpc::i8 flag;
    };
    friend inline void serialize(const RpcFastPrimeResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.flag, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastPrimeResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastPrimeResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.flag, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastPrimeResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFastDotProdRequest {
        point3 p1;
        point3 p2;
    };
    friend inline void serialize(const RpcFastDotProdRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.p1, ar);
        srpc::Serialize_::serialize(o.p2, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastDotProdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastDotProdRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.p1, ar);
        srpc::Deserialize_::deserialize(o.p2, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastDotProdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastDotProdResponse {
        double v;
    };
    friend inline void serialize(const RpcFastDotProdResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.v, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastDotProdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastDotProdResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.v, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastDotProdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFastAddRequest {
        srpc::v32 a;
        srpc::v32 b;
    };
    friend inline void serialize(const RpcFastAddRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.a, ar);
        srpc::Serialize_::serialize(o.b, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastAddRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastAddRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.a, ar);
        srpc::Deserialize_::deserialize(o.b, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastAddRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastAddResponse {
        srpc::v32 a_add_b;
    };
    friend inline void serialize(const RpcFastAddResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.a_add_b, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastAddResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastAddResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.a_add_b, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastAddResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFastNopRequest {
        std::string in_0;
    };
    friend inline void serialize(const RpcFastNopRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.in_0, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastNopRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastNopRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.in_0, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastNopRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastNopResponse {
    };
    friend inline void serialize(const RpcFastNopResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastNopResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastNopResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastNopResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFastVecRequest {
        srpc::i32 n;
    };
    friend inline void serialize(const RpcFastVecRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.n, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastVecRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastVecRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.n, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastVecRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastVecResponse {
        std::vector<srpc::i64> v;
    };
    friend inline void serialize(const RpcFastVecResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.v, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastVecResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastVecResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.v, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastVecResponse& o) { deserialize(o, ar); return ar; }

    struct RpcPrimeRequest {
        srpc::i32 n;
    };
    friend inline void serialize(const RpcPrimeRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.n, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrimeRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrimeRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.n, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrimeRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrimeResponse {
        srpc::i8 flag;
    };
    friend inline void serialize(const RpcPrimeResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.flag, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrimeResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrimeResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.flag, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrimeResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDotProdRequest {
        point3 p1;
        point3 p2;
    };
    friend inline void serialize(const RpcDotProdRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.p1, ar);
        srpc::Serialize_::serialize(o.p2, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDotProdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDotProdRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.p1, ar);
        srpc::Deserialize_::deserialize(o.p2, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDotProdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDotProdResponse {
        double v;
    };
    friend inline void serialize(const RpcDotProdResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.v, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDotProdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDotProdResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.v, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDotProdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAddRequest {
        srpc::v32 a;
        srpc::v32 b;
    };
    friend inline void serialize(const RpcAddRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.a, ar);
        srpc::Serialize_::serialize(o.b, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAddRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.a, ar);
        srpc::Deserialize_::deserialize(o.b, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAddRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAddResponse {
        srpc::v32 a_add_b;
    };
    friend inline void serialize(const RpcAddResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.a_add_b, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAddResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.a_add_b, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAddResponse& o) { deserialize(o, ar); return ar; }

    struct RpcNopRequest {
        std::string in_0;
    };
    friend inline void serialize(const RpcNopRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.in_0, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcNopRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNopRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.in_0, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcNopRequest& o) { deserialize(o, ar); return ar; }

    struct RpcNopResponse {
    };
    friend inline void serialize(const RpcNopResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcNopResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNopResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcNopResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAsyncNopRequest {
        std::string in_0;
    };
    friend inline void serialize(const RpcAsyncNopRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.in_0, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAsyncNopRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAsyncNopRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.in_0, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAsyncNopRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAsyncNopResponse {
    };
    friend inline void serialize(const RpcAsyncNopResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAsyncNopResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAsyncNopResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAsyncNopResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSleepRequest {
        double sec;
    };
    friend inline void serialize(const RpcSleepRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.sec, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSleepRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSleepRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.sec, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSleepRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSleepResponse {
    };
    friend inline void serialize(const RpcSleepResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSleepResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSleepResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSleepResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDeferredEchoRequest {
        srpc::i32 val;
    };
    friend inline void serialize(const RpcDeferredEchoRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDeferredEchoRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDeferredEchoRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDeferredEchoRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDeferredEchoResponse {
        srpc::i32 result;
    };
    friend inline void serialize(const RpcDeferredEchoResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.result, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDeferredEchoResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDeferredEchoResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.result, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDeferredEchoResponse& o) { deserialize(o, ar); return ar; }

    enum {
        FAST_PRIME = 0x4f4daa5a,
        FAST_DOT_PROD = 0x36ff5226,
        FAST_ADD = 0x3a24232d,
        FAST_NOP = 0x4b921bd9,
        FAST_VEC = 0x23928fcb,
        PRIME = 0x4e81b3fc,
        DOT_PROD = 0x1f7d12f4,
        ADD = 0x1e8ff45b,
        NOP = 0x327203ee,
        ASYNC_NOP = 0x22654490,
        SLEEP = 0x22cb72f2,
        DEFERRED_ECHO = 0x412ef56f,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_fast_rpc(FAST_PRIME, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_fast_rpc(FAST_DOT_PROD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_fast_rpc(FAST_ADD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_fast_rpc(FAST_NOP, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_fast_rpc(FAST_VEC, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(PRIME, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DOT_PROD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ADD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(NOP, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_fast_rpc(ASYNC_NOP, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SLEEP, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DEFERRED_ECHO, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(FAST_PRIME);
        svr.unreg(FAST_DOT_PROD);
        svr.unreg(FAST_ADD);
        svr.unreg(FAST_NOP);
        svr.unreg(FAST_VEC);
        svr.unreg(PRIME);
        svr.unreg(DOT_PROD);
        svr.unreg(ADD);
        svr.unreg(NOP);
        svr.unreg(ASYNC_NOP);
        svr.unreg(SLEEP);
        svr.unreg(DEFERRED_ECHO);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case FAST_PRIME: __fast_prime__wrapper__(std::move(req), weak_sconn); break;
        case FAST_DOT_PROD: __fast_dot_prod__wrapper__(std::move(req), weak_sconn); break;
        case FAST_ADD: __fast_add__wrapper__(std::move(req), weak_sconn); break;
        case FAST_NOP: __fast_nop__wrapper__(std::move(req), weak_sconn); break;
        case FAST_VEC: __fast_vec__wrapper__(std::move(req), weak_sconn); break;
        case PRIME: __prime__wrapper__(std::move(req), weak_sconn); break;
        case DOT_PROD: __dot_prod__wrapper__(std::move(req), weak_sconn); break;
        case ADD: __add__wrapper__(std::move(req), weak_sconn); break;
        case NOP: __nop__wrapper__(std::move(req), weak_sconn); break;
        case ASYNC_NOP: __async_nop__wrapper__(std::move(req), weak_sconn); break;
        case SLEEP: __sleep__wrapper__(std::move(req), weak_sconn); break;
        case DEFERRED_ECHO: __deferred_echo__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual rusty::Result<RpcFastPrimeResponse, srpc::i32> fast_prime(const RpcFastPrimeRequest& req);
    // @safe
    virtual rusty::Result<RpcFastDotProdResponse, srpc::i32> fast_dot_prod(const RpcFastDotProdRequest& req);
    // @safe
    virtual rusty::Result<RpcFastAddResponse, srpc::i32> fast_add(const RpcFastAddRequest& req);
    // @safe
    virtual rusty::Result<RpcFastNopResponse, srpc::i32> fast_nop(const RpcFastNopRequest& req);
    // @safe
    virtual rusty::Result<RpcFastVecResponse, srpc::i32> fast_vec(const RpcFastVecRequest& req);
    // @safe
    virtual rusty::Result<RpcPrimeResponse, srpc::i32> prime(const RpcPrimeRequest& req);
    // @safe
    virtual rusty::Result<RpcDotProdResponse, srpc::i32> dot_prod(const RpcDotProdRequest& req);
    // @safe
    virtual rusty::Result<RpcAddResponse, srpc::i32> add(const RpcAddRequest& req);
    // @safe
    virtual rusty::Result<RpcNopResponse, srpc::i32> nop(const RpcNopRequest& req);
    // @safe
    virtual rusty::Task<rusty::Result<RpcAsyncNopResponse, srpc::i32>> async_nop(const RpcAsyncNopRequest& req);
    // @safe
    virtual rusty::Result<RpcSleepResponse, srpc::i32> sleep(const RpcSleepRequest& req);
    // @safe
    virtual void deferred_echo(const RpcDeferredEchoRequest& req, RpcDeferredEchoResponse& resp, srpc::DeferredReply defer);
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __fast_prime__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastPrimeRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.n, __req_ar__);
            auto __typed_result__ = this->fast_prime(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.flag, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_dot_prod__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastDotProdRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.p1, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.p2, __req_ar__);
            auto __typed_result__ = this->fast_dot_prod(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.v, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_add__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastAddRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.a, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.b, __req_ar__);
            auto __typed_result__ = this->fast_add(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.a_add_b, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_nop__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastNopRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.in_0, __req_ar__);
            auto __typed_result__ = this->fast_nop(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    (void)__typed_resp__;
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, srpc::ServerReplyFn{});
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_vec__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastVecRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.n, __req_ar__);
            auto __typed_result__ = this->fast_vec(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.v, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __prime__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrimeRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.n, __req_ar__);
            auto __typed_result__ = this->prime(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.flag, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __dot_prod__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDotProdRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.p1, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.p2, __req_ar__);
            auto __typed_result__ = this->dot_prod(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.v, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __add__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAddRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.a, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.b, __req_ar__);
            auto __typed_result__ = this->add(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, [&](srpc::BinaryWriteArchive& m) {
                        srpc::Serialize_::serialize(__typed_resp__.a_add_b, m);
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __nop__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcNopRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.in_0, __req_ar__);
            auto __typed_result__ = this->nop(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    (void)__typed_resp__;
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, srpc::ServerReplyFn{});
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __async_nop__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAsyncNopRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.in_0, __req_ar__);
            auto __async_req__ = std::move(req);
            auto __async_weak_sconn__ = weak_sconn;
            auto __async_task__ = this->async_nop(__typed_req__);
            srpc::reactor_spawn_stackless_task_with_result(*srpc::Reactor::get_reactor(), std::move(__async_task__), [__async_req__ = std::move(__async_req__), __async_weak_sconn__](auto __typed_result__) mutable {
                auto sconn_opt = __async_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__async_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        (void)__typed_resp__;
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__async_req__, 0, srpc::ServerReplyFn{});
                    }
                }
            });
        }
    }
    // @safe
    void __sleep__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSleepRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.sec, __req_ar__);
            auto __typed_result__ = this->sleep(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    (void)__typed_resp__;
                    const_cast<srpc::ServerConnection&>(*sconn).reply(*req, 0, srpc::ServerReplyFn{});
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __deferred_echo__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDeferredEchoRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.val, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDeferredEchoResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->result, m);
                },
                []() {});
            this->deferred_echo(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class BenchmarkProxy {
protected:
    srpc::Client* __cl__;
public:
    BenchmarkProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcFastPrimeRequest = BenchmarkService::RpcFastPrimeRequest;
    using RpcFastPrimeResponse = BenchmarkService::RpcFastPrimeResponse;
    using RpcFastDotProdRequest = BenchmarkService::RpcFastDotProdRequest;
    using RpcFastDotProdResponse = BenchmarkService::RpcFastDotProdResponse;
    using RpcFastAddRequest = BenchmarkService::RpcFastAddRequest;
    using RpcFastAddResponse = BenchmarkService::RpcFastAddResponse;
    using RpcFastNopRequest = BenchmarkService::RpcFastNopRequest;
    using RpcFastNopResponse = BenchmarkService::RpcFastNopResponse;
    using RpcFastVecRequest = BenchmarkService::RpcFastVecRequest;
    using RpcFastVecResponse = BenchmarkService::RpcFastVecResponse;
    using RpcPrimeRequest = BenchmarkService::RpcPrimeRequest;
    using RpcPrimeResponse = BenchmarkService::RpcPrimeResponse;
    using RpcDotProdRequest = BenchmarkService::RpcDotProdRequest;
    using RpcDotProdResponse = BenchmarkService::RpcDotProdResponse;
    using RpcAddRequest = BenchmarkService::RpcAddRequest;
    using RpcAddResponse = BenchmarkService::RpcAddResponse;
    using RpcNopRequest = BenchmarkService::RpcNopRequest;
    using RpcNopResponse = BenchmarkService::RpcNopResponse;
    using RpcAsyncNopRequest = BenchmarkService::RpcAsyncNopRequest;
    using RpcAsyncNopResponse = BenchmarkService::RpcAsyncNopResponse;
    using RpcSleepRequest = BenchmarkService::RpcSleepRequest;
    using RpcSleepResponse = BenchmarkService::RpcSleepResponse;
    using RpcDeferredEchoRequest = BenchmarkService::RpcDeferredEchoRequest;
    using RpcDeferredEchoResponse = BenchmarkService::RpcDeferredEchoResponse;
    class fast_primeTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit fast_primeTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastPrimeResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastPrimeResponse, srpc::i32>::Err(__ret__);
            }
            RpcFastPrimeResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.flag, __reply_ar__);
            return rusty::Result<RpcFastPrimeResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<fast_primeTypedFuture, srpc::i32> async_fast_prime(const RpcFastPrimeRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_PRIME, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.n, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_primeTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_primeTypedFuture, srpc::i32>::Ok(fast_primeTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastPrimeResponse, srpc::i32> fast_prime(const RpcFastPrimeRequest& req) {
        auto __typed_fu_result__ = this->async_fast_prime(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastPrimeResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_dot_prodTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit fast_dot_prodTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastDotProdResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastDotProdResponse, srpc::i32>::Err(__ret__);
            }
            RpcFastDotProdResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.v, __reply_ar__);
            return rusty::Result<RpcFastDotProdResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<fast_dot_prodTypedFuture, srpc::i32> async_fast_dot_prod(const RpcFastDotProdRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_DOT_PROD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.p1, __m__);
            srpc::Serialize_::serialize(req.p2, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_dot_prodTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_dot_prodTypedFuture, srpc::i32>::Ok(fast_dot_prodTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastDotProdResponse, srpc::i32> fast_dot_prod(const RpcFastDotProdRequest& req) {
        auto __typed_fu_result__ = this->async_fast_dot_prod(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastDotProdResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_addTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit fast_addTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastAddResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastAddResponse, srpc::i32>::Err(__ret__);
            }
            RpcFastAddResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.a_add_b, __reply_ar__);
            return rusty::Result<RpcFastAddResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<fast_addTypedFuture, srpc::i32> async_fast_add(const RpcFastAddRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_ADD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.a, __m__);
            srpc::Serialize_::serialize(req.b, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_addTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_addTypedFuture, srpc::i32>::Ok(fast_addTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastAddResponse, srpc::i32> fast_add(const RpcFastAddRequest& req) {
        auto __typed_fu_result__ = this->async_fast_add(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastAddResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_nopTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit fast_nopTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastNopResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastNopResponse, srpc::i32>::Err(__ret__);
            }
            RpcFastNopResponse __typed_resp__;
            return rusty::Result<RpcFastNopResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<fast_nopTypedFuture, srpc::i32> async_fast_nop(const RpcFastNopRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_NOP, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.in_0, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_nopTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_nopTypedFuture, srpc::i32>::Ok(fast_nopTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastNopResponse, srpc::i32> fast_nop(const RpcFastNopRequest& req) {
        auto __typed_fu_result__ = this->async_fast_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastNopResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_vecTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit fast_vecTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastVecResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastVecResponse, srpc::i32>::Err(__ret__);
            }
            RpcFastVecResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.v, __reply_ar__);
            return rusty::Result<RpcFastVecResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<fast_vecTypedFuture, srpc::i32> async_fast_vec(const RpcFastVecRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_VEC, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.n, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_vecTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_vecTypedFuture, srpc::i32>::Ok(fast_vecTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastVecResponse, srpc::i32> fast_vec(const RpcFastVecRequest& req) {
        auto __typed_fu_result__ = this->async_fast_vec(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastVecResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class primeTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit primeTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcPrimeResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrimeResponse, srpc::i32>::Err(__ret__);
            }
            RpcPrimeResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.flag, __reply_ar__);
            return rusty::Result<RpcPrimeResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<primeTypedFuture, srpc::i32> async_prime(const RpcPrimeRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::PRIME, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.n, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<primeTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<primeTypedFuture, srpc::i32>::Ok(primeTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPrimeResponse, srpc::i32> prime(const RpcPrimeRequest& req) {
        auto __typed_fu_result__ = this->async_prime(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrimeResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class dot_prodTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit dot_prodTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcDotProdResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDotProdResponse, srpc::i32>::Err(__ret__);
            }
            RpcDotProdResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.v, __reply_ar__);
            return rusty::Result<RpcDotProdResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<dot_prodTypedFuture, srpc::i32> async_dot_prod(const RpcDotProdRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::DOT_PROD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.p1, __m__);
            srpc::Serialize_::serialize(req.p2, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<dot_prodTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<dot_prodTypedFuture, srpc::i32>::Ok(dot_prodTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDotProdResponse, srpc::i32> dot_prod(const RpcDotProdRequest& req) {
        auto __typed_fu_result__ = this->async_dot_prod(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDotProdResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class addTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit addTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAddResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAddResponse, srpc::i32>::Err(__ret__);
            }
            RpcAddResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.a_add_b, __reply_ar__);
            return rusty::Result<RpcAddResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<addTypedFuture, srpc::i32> async_add(const RpcAddRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::ADD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.a, __m__);
            srpc::Serialize_::serialize(req.b, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<addTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<addTypedFuture, srpc::i32>::Ok(addTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAddResponse, srpc::i32> add(const RpcAddRequest& req) {
        auto __typed_fu_result__ = this->async_add(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAddResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class nopTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit nopTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcNopResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcNopResponse, srpc::i32>::Err(__ret__);
            }
            RpcNopResponse __typed_resp__;
            return rusty::Result<RpcNopResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<nopTypedFuture, srpc::i32> async_nop(const RpcNopRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::NOP, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.in_0, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<nopTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<nopTypedFuture, srpc::i32>::Ok(nopTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcNopResponse, srpc::i32> nop(const RpcNopRequest& req) {
        auto __typed_fu_result__ = this->async_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcNopResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class async_nopTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit async_nopTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAsyncNopResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAsyncNopResponse, srpc::i32>::Err(__ret__);
            }
            RpcAsyncNopResponse __typed_resp__;
            return rusty::Result<RpcAsyncNopResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<async_nopTypedFuture, srpc::i32> async_async_nop(const RpcAsyncNopRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::ASYNC_NOP, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.in_0, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<async_nopTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<async_nopTypedFuture, srpc::i32>::Ok(async_nopTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAsyncNopResponse, srpc::i32> async_nop(const RpcAsyncNopRequest& req) {
        auto __typed_fu_result__ = this->async_async_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAsyncNopResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class sleepTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit sleepTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSleepResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSleepResponse, srpc::i32>::Err(__ret__);
            }
            RpcSleepResponse __typed_resp__;
            return rusty::Result<RpcSleepResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<sleepTypedFuture, srpc::i32> async_sleep(const RpcSleepRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::SLEEP, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.sec, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<sleepTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<sleepTypedFuture, srpc::i32>::Ok(sleepTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSleepResponse, srpc::i32> sleep(const RpcSleepRequest& req) {
        auto __typed_fu_result__ = this->async_sleep(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSleepResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class deferred_echoTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit deferred_echoTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcDeferredEchoResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDeferredEchoResponse, srpc::i32>::Err(__ret__);
            }
            RpcDeferredEchoResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.result, __reply_ar__);
            return rusty::Result<RpcDeferredEchoResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<deferred_echoTypedFuture, srpc::i32> async_deferred_echo(const RpcDeferredEchoRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::DEFERRED_ECHO, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.val, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<deferred_echoTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<deferred_echoTypedFuture, srpc::i32>::Ok(deferred_echoTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDeferredEchoResponse, srpc::i32> deferred_echo(const RpcDeferredEchoRequest& req) {
        auto __typed_fu_result__ = this->async_deferred_echo(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDeferredEchoResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace benchmark


// optional %%: marks footer section, code below will be copied into end of generated C++ header

// BenchmarkService methods are implemented in src/srpc/tests/benchmark_service.cc using
// typed Rpc*Request/Rpc*Response signatures.

