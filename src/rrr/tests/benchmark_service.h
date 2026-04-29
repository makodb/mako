#pragma once

#include "rrr/rrr.hpp"
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

inline rrr::Marshal& operator <<(rrr::Marshal& m, const point3& o) {
    m << o.x;
    m << o.y;
    m << o.z;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, point3& o) {
    m >> o.x;
    m >> o.y;
    m >> o.z;
    return m;
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const point3& o) {
    ar << o.x;
    ar << o.y;
    ar << o.z;
    return ar;
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, point3& o) {
    ar >> o.x;
    ar >> o.y;
    ar >> o.z;
    return ar;
}

class BenchmarkService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcFastPrimeRequest {
        rrr::i32 n;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastPrimeRequest& o) {
        m << o.n;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastPrimeRequest& o) {
        m >> o.n;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastPrimeRequest& o) {
        ar << o.n;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastPrimeRequest& o) {
        ar >> o.n;
        return ar;
    }

    struct RpcFastPrimeResponse {
        rrr::i8 flag;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastPrimeResponse& o) {
        m << o.flag;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastPrimeResponse& o) {
        m >> o.flag;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastPrimeResponse& o) {
        ar << o.flag;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastPrimeResponse& o) {
        ar >> o.flag;
        return ar;
    }

    struct RpcFastDotProdRequest {
        point3 p1;
        point3 p2;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastDotProdRequest& o) {
        m << o.p1;
        m << o.p2;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastDotProdRequest& o) {
        m >> o.p1;
        m >> o.p2;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastDotProdRequest& o) {
        ar << o.p1;
        ar << o.p2;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastDotProdRequest& o) {
        ar >> o.p1;
        ar >> o.p2;
        return ar;
    }

    struct RpcFastDotProdResponse {
        double v;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastDotProdResponse& o) {
        m << o.v;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastDotProdResponse& o) {
        m >> o.v;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastDotProdResponse& o) {
        ar << o.v;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastDotProdResponse& o) {
        ar >> o.v;
        return ar;
    }

    struct RpcFastAddRequest {
        rrr::v32 a;
        rrr::v32 b;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastAddRequest& o) {
        m << o.a;
        m << o.b;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastAddRequest& o) {
        m >> o.a;
        m >> o.b;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastAddRequest& o) {
        ar << o.a;
        ar << o.b;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastAddRequest& o) {
        ar >> o.a;
        ar >> o.b;
        return ar;
    }

    struct RpcFastAddResponse {
        rrr::v32 a_add_b;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastAddResponse& o) {
        m << o.a_add_b;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastAddResponse& o) {
        m >> o.a_add_b;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastAddResponse& o) {
        ar << o.a_add_b;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastAddResponse& o) {
        ar >> o.a_add_b;
        return ar;
    }

    struct RpcFastNopRequest {
        std::string in_0;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastNopRequest& o) {
        m << o.in_0;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastNopRequest& o) {
        m >> o.in_0;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastNopRequest& o) {
        ar << o.in_0;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastNopRequest& o) {
        ar >> o.in_0;
        return ar;
    }

    struct RpcFastNopResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastNopResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastNopResponse& o) {
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastNopResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastNopResponse& o) {
        return ar;
    }

    struct RpcFastVecRequest {
        rrr::i32 n;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastVecRequest& o) {
        m << o.n;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastVecRequest& o) {
        m >> o.n;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastVecRequest& o) {
        ar << o.n;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastVecRequest& o) {
        ar >> o.n;
        return ar;
    }

    struct RpcFastVecResponse {
        std::vector<rrr::i64> v;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastVecResponse& o) {
        m << o.v;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastVecResponse& o) {
        m >> o.v;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastVecResponse& o) {
        ar << o.v;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastVecResponse& o) {
        ar >> o.v;
        return ar;
    }

    struct RpcPrimeRequest {
        rrr::i32 n;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrimeRequest& o) {
        m << o.n;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrimeRequest& o) {
        m >> o.n;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrimeRequest& o) {
        ar << o.n;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrimeRequest& o) {
        ar >> o.n;
        return ar;
    }

    struct RpcPrimeResponse {
        rrr::i8 flag;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrimeResponse& o) {
        m << o.flag;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrimeResponse& o) {
        m >> o.flag;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrimeResponse& o) {
        ar << o.flag;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrimeResponse& o) {
        ar >> o.flag;
        return ar;
    }

    struct RpcDotProdRequest {
        point3 p1;
        point3 p2;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDotProdRequest& o) {
        m << o.p1;
        m << o.p2;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDotProdRequest& o) {
        m >> o.p1;
        m >> o.p2;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDotProdRequest& o) {
        ar << o.p1;
        ar << o.p2;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDotProdRequest& o) {
        ar >> o.p1;
        ar >> o.p2;
        return ar;
    }

    struct RpcDotProdResponse {
        double v;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDotProdResponse& o) {
        m << o.v;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDotProdResponse& o) {
        m >> o.v;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDotProdResponse& o) {
        ar << o.v;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDotProdResponse& o) {
        ar >> o.v;
        return ar;
    }

    struct RpcAddRequest {
        rrr::v32 a;
        rrr::v32 b;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAddRequest& o) {
        m << o.a;
        m << o.b;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAddRequest& o) {
        m >> o.a;
        m >> o.b;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAddRequest& o) {
        ar << o.a;
        ar << o.b;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAddRequest& o) {
        ar >> o.a;
        ar >> o.b;
        return ar;
    }

    struct RpcAddResponse {
        rrr::v32 a_add_b;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAddResponse& o) {
        m << o.a_add_b;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAddResponse& o) {
        m >> o.a_add_b;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAddResponse& o) {
        ar << o.a_add_b;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAddResponse& o) {
        ar >> o.a_add_b;
        return ar;
    }

    struct RpcNopRequest {
        std::string in_0;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcNopRequest& o) {
        m << o.in_0;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcNopRequest& o) {
        m >> o.in_0;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcNopRequest& o) {
        ar << o.in_0;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcNopRequest& o) {
        ar >> o.in_0;
        return ar;
    }

    struct RpcNopResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcNopResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcNopResponse& o) {
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcNopResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcNopResponse& o) {
        return ar;
    }

    struct RpcAsyncNopRequest {
        std::string in_0;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAsyncNopRequest& o) {
        m << o.in_0;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAsyncNopRequest& o) {
        m >> o.in_0;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAsyncNopRequest& o) {
        ar << o.in_0;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAsyncNopRequest& o) {
        ar >> o.in_0;
        return ar;
    }

    struct RpcAsyncNopResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAsyncNopResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAsyncNopResponse& o) {
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAsyncNopResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAsyncNopResponse& o) {
        return ar;
    }

    struct RpcSleepRequest {
        double sec;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSleepRequest& o) {
        m << o.sec;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSleepRequest& o) {
        m >> o.sec;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSleepRequest& o) {
        ar << o.sec;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSleepRequest& o) {
        ar >> o.sec;
        return ar;
    }

    struct RpcSleepResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSleepResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSleepResponse& o) {
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSleepResponse& o) {
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSleepResponse& o) {
        return ar;
    }

    struct RpcDeferredEchoRequest {
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDeferredEchoRequest& o) {
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDeferredEchoRequest& o) {
        m >> o.val;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDeferredEchoRequest& o) {
        ar << o.val;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDeferredEchoRequest& o) {
        ar >> o.val;
        return ar;
    }

    struct RpcDeferredEchoResponse {
        rrr::i32 result;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDeferredEchoResponse& o) {
        m << o.result;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDeferredEchoResponse& o) {
        m >> o.result;
        return m;
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDeferredEchoResponse& o) {
        ar << o.result;
        return ar;
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDeferredEchoResponse& o) {
        ar >> o.result;
        return ar;
    }

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
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    virtual rusty::Result<RpcFastPrimeResponse, rrr::i32> fast_prime(const RpcFastPrimeRequest& req);
    // @safe
    virtual rusty::Result<RpcFastDotProdResponse, rrr::i32> fast_dot_prod(const RpcFastDotProdRequest& req);
    // @safe
    virtual rusty::Result<RpcFastAddResponse, rrr::i32> fast_add(const RpcFastAddRequest& req);
    // @safe
    virtual rusty::Result<RpcFastNopResponse, rrr::i32> fast_nop(const RpcFastNopRequest& req);
    // @safe
    virtual rusty::Result<RpcFastVecResponse, rrr::i32> fast_vec(const RpcFastVecRequest& req);
    // @safe
    virtual rusty::Result<RpcPrimeResponse, rrr::i32> prime(const RpcPrimeRequest& req);
    // @safe
    virtual rusty::Result<RpcDotProdResponse, rrr::i32> dot_prod(const RpcDotProdRequest& req);
    // @safe
    virtual rusty::Result<RpcAddResponse, rrr::i32> add(const RpcAddRequest& req);
    // @safe
    virtual rusty::Result<RpcNopResponse, rrr::i32> nop(const RpcNopRequest& req);
    // @safe
    virtual rusty::Task<rusty::Result<RpcAsyncNopResponse, rrr::i32>> async_nop(const RpcAsyncNopRequest& req);
    // @safe
    virtual rusty::Result<RpcSleepResponse, rrr::i32> sleep(const RpcSleepRequest& req);
    // @safe
    virtual void deferred_echo(const RpcDeferredEchoRequest& req, RpcDeferredEchoResponse& resp, rrr::DeferredReply defer);
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __fast_prime__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastPrimeRequest __typed_req__;
            req->m >> __typed_req__.n;
            auto __typed_result__ = this->fast_prime(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.flag;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_dot_prod__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastDotProdRequest __typed_req__;
            req->m >> __typed_req__.p1;
            req->m >> __typed_req__.p2;
            auto __typed_result__ = this->fast_dot_prod(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.v;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_add__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastAddRequest __typed_req__;
            req->m >> __typed_req__.a;
            req->m >> __typed_req__.b;
            auto __typed_result__ = this->fast_add(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.a_add_b;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastNopRequest __typed_req__;
            req->m >> __typed_req__.in_0;
            auto __typed_result__ = this->fast_nop(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    (void)__typed_resp__;
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __fast_vec__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastVecRequest __typed_req__;
            req->m >> __typed_req__.n;
            auto __typed_result__ = this->fast_vec(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.v;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __prime__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrimeRequest __typed_req__;
            req->m >> __typed_req__.n;
            auto __typed_result__ = this->prime(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.flag;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __dot_prod__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDotProdRequest __typed_req__;
            req->m >> __typed_req__.p1;
            req->m >> __typed_req__.p2;
            auto __typed_result__ = this->dot_prod(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.v;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __add__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAddRequest __typed_req__;
            req->m >> __typed_req__.a;
            req->m >> __typed_req__.b;
            auto __typed_result__ = this->add(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                        m << __typed_resp__.a_add_b;
                    });
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcNopRequest __typed_req__;
            req->m >> __typed_req__.in_0;
            auto __typed_result__ = this->nop(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    (void)__typed_resp__;
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __async_nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAsyncNopRequest __typed_req__;
            req->m >> __typed_req__.in_0;
            auto __async_req__ = std::move(req);
            auto __async_weak_sconn__ = weak_sconn;
            auto __async_task__ = this->async_nop(__typed_req__);
            rrr::Reactor::get_reactor()->spawn_stackless_task_with_result(std::move(__async_task__), [__async_req__ = std::move(__async_req__), __async_weak_sconn__](auto __typed_result__) mutable {
                auto sconn_opt = __async_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__, __typed_result__.unwrap_err());
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        (void)__typed_resp__;
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__);
                    }
                }
            });
        }
    }
    // @safe
    void __sleep__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSleepRequest __typed_req__;
            req->m >> __typed_req__.sec;
            auto __typed_result__ = this->sleep(__typed_req__);
            auto sconn_opt = weak_sconn.upgrade();
            if (sconn_opt.is_some()) {
                auto sconn = sconn_opt.unwrap();
                if (__typed_result__.is_err()) {
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());
                } else {
                    auto __typed_resp__ = __typed_result__.unwrap();
                    (void)__typed_resp__;
                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
                }
            }
            // req automatically cleaned up by rusty::Box
        }
    }
    // @safe
    void __deferred_echo__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDeferredEchoRequest __typed_req__;
            req->m >> __typed_req__.val;
            auto __typed_resp__ = std::make_shared<RpcDeferredEchoResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->result;
                },
                []() {});
            this->deferred_echo(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class BenchmarkProxy {
protected:
    rrr::Client* __cl__;
public:
    BenchmarkProxy(rrr::Client* cl): __cl__(cl) { }
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit fast_primeTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastPrimeResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastPrimeResponse, rrr::i32>::Err(__ret__);
            }
            RpcFastPrimeResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.flag;
            return rusty::Result<RpcFastPrimeResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<fast_primeTypedFuture, rrr::i32> async_fast_prime(const RpcFastPrimeRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_PRIME, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.n;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_primeTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_primeTypedFuture, rrr::i32>::Ok(fast_primeTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<fast_primeTypedFuture> await_fast_prime(const RpcFastPrimeRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_fast_prime(req, __fu_attr__));
    }
    rusty::Result<RpcFastPrimeResponse, rrr::i32> fast_prime(const RpcFastPrimeRequest& req) {
        auto __typed_fu_result__ = this->async_fast_prime(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastPrimeResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_dot_prodTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit fast_dot_prodTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastDotProdResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastDotProdResponse, rrr::i32>::Err(__ret__);
            }
            RpcFastDotProdResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.v;
            return rusty::Result<RpcFastDotProdResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<fast_dot_prodTypedFuture, rrr::i32> async_fast_dot_prod(const RpcFastDotProdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_DOT_PROD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.p1;
            __m__ << req.p2;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_dot_prodTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_dot_prodTypedFuture, rrr::i32>::Ok(fast_dot_prodTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<fast_dot_prodTypedFuture> await_fast_dot_prod(const RpcFastDotProdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_fast_dot_prod(req, __fu_attr__));
    }
    rusty::Result<RpcFastDotProdResponse, rrr::i32> fast_dot_prod(const RpcFastDotProdRequest& req) {
        auto __typed_fu_result__ = this->async_fast_dot_prod(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastDotProdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_addTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit fast_addTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastAddResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastAddResponse, rrr::i32>::Err(__ret__);
            }
            RpcFastAddResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.a_add_b;
            return rusty::Result<RpcFastAddResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<fast_addTypedFuture, rrr::i32> async_fast_add(const RpcFastAddRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_ADD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.a;
            __m__ << req.b;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_addTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_addTypedFuture, rrr::i32>::Ok(fast_addTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<fast_addTypedFuture> await_fast_add(const RpcFastAddRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_fast_add(req, __fu_attr__));
    }
    rusty::Result<RpcFastAddResponse, rrr::i32> fast_add(const RpcFastAddRequest& req) {
        auto __typed_fu_result__ = this->async_fast_add(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastAddResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_nopTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit fast_nopTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastNopResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastNopResponse, rrr::i32>::Err(__ret__);
            }
            RpcFastNopResponse __typed_resp__;
            return rusty::Result<RpcFastNopResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<fast_nopTypedFuture, rrr::i32> async_fast_nop(const RpcFastNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_NOP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.in_0;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_nopTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_nopTypedFuture, rrr::i32>::Ok(fast_nopTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<fast_nopTypedFuture> await_fast_nop(const RpcFastNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_fast_nop(req, __fu_attr__));
    }
    rusty::Result<RpcFastNopResponse, rrr::i32> fast_nop(const RpcFastNopRequest& req) {
        auto __typed_fu_result__ = this->async_fast_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastNopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class fast_vecTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit fast_vecTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastVecResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastVecResponse, rrr::i32>::Err(__ret__);
            }
            RpcFastVecResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.v;
            return rusty::Result<RpcFastVecResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<fast_vecTypedFuture, rrr::i32> async_fast_vec(const RpcFastVecRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::FAST_VEC, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.n;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<fast_vecTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<fast_vecTypedFuture, rrr::i32>::Ok(fast_vecTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<fast_vecTypedFuture> await_fast_vec(const RpcFastVecRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_fast_vec(req, __fu_attr__));
    }
    rusty::Result<RpcFastVecResponse, rrr::i32> fast_vec(const RpcFastVecRequest& req) {
        auto __typed_fu_result__ = this->async_fast_vec(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastVecResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class primeTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit primeTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcPrimeResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrimeResponse, rrr::i32>::Err(__ret__);
            }
            RpcPrimeResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.flag;
            return rusty::Result<RpcPrimeResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<primeTypedFuture, rrr::i32> async_prime(const RpcPrimeRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::PRIME, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.n;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<primeTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<primeTypedFuture, rrr::i32>::Ok(primeTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<primeTypedFuture> await_prime(const RpcPrimeRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_prime(req, __fu_attr__));
    }
    rusty::Result<RpcPrimeResponse, rrr::i32> prime(const RpcPrimeRequest& req) {
        auto __typed_fu_result__ = this->async_prime(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrimeResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class dot_prodTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit dot_prodTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcDotProdResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDotProdResponse, rrr::i32>::Err(__ret__);
            }
            RpcDotProdResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.v;
            return rusty::Result<RpcDotProdResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<dot_prodTypedFuture, rrr::i32> async_dot_prod(const RpcDotProdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::DOT_PROD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.p1;
            __m__ << req.p2;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<dot_prodTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<dot_prodTypedFuture, rrr::i32>::Ok(dot_prodTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<dot_prodTypedFuture> await_dot_prod(const RpcDotProdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_dot_prod(req, __fu_attr__));
    }
    rusty::Result<RpcDotProdResponse, rrr::i32> dot_prod(const RpcDotProdRequest& req) {
        auto __typed_fu_result__ = this->async_dot_prod(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDotProdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class addTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit addTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAddResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAddResponse, rrr::i32>::Err(__ret__);
            }
            RpcAddResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.a_add_b;
            return rusty::Result<RpcAddResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<addTypedFuture, rrr::i32> async_add(const RpcAddRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::ADD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.a;
            __m__ << req.b;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<addTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<addTypedFuture, rrr::i32>::Ok(addTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<addTypedFuture> await_add(const RpcAddRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_add(req, __fu_attr__));
    }
    rusty::Result<RpcAddResponse, rrr::i32> add(const RpcAddRequest& req) {
        auto __typed_fu_result__ = this->async_add(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAddResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class nopTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit nopTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcNopResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcNopResponse, rrr::i32>::Err(__ret__);
            }
            RpcNopResponse __typed_resp__;
            return rusty::Result<RpcNopResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<nopTypedFuture, rrr::i32> async_nop(const RpcNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::NOP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.in_0;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<nopTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<nopTypedFuture, rrr::i32>::Ok(nopTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<nopTypedFuture> await_nop(const RpcNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_nop(req, __fu_attr__));
    }
    rusty::Result<RpcNopResponse, rrr::i32> nop(const RpcNopRequest& req) {
        auto __typed_fu_result__ = this->async_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcNopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class async_nopTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit async_nopTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAsyncNopResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAsyncNopResponse, rrr::i32>::Err(__ret__);
            }
            RpcAsyncNopResponse __typed_resp__;
            return rusty::Result<RpcAsyncNopResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<async_nopTypedFuture, rrr::i32> async_async_nop(const RpcAsyncNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::ASYNC_NOP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.in_0;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<async_nopTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<async_nopTypedFuture, rrr::i32>::Ok(async_nopTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<async_nopTypedFuture> await_async_nop(const RpcAsyncNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_async_nop(req, __fu_attr__));
    }
    rusty::Result<RpcAsyncNopResponse, rrr::i32> async_nop(const RpcAsyncNopRequest& req) {
        auto __typed_fu_result__ = this->async_async_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAsyncNopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class sleepTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit sleepTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSleepResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSleepResponse, rrr::i32>::Err(__ret__);
            }
            RpcSleepResponse __typed_resp__;
            return rusty::Result<RpcSleepResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<sleepTypedFuture, rrr::i32> async_sleep(const RpcSleepRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::SLEEP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.sec;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<sleepTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<sleepTypedFuture, rrr::i32>::Ok(sleepTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<sleepTypedFuture> await_sleep(const RpcSleepRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_sleep(req, __fu_attr__));
    }
    rusty::Result<RpcSleepResponse, rrr::i32> sleep(const RpcSleepRequest& req) {
        auto __typed_fu_result__ = this->async_sleep(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSleepResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class deferred_echoTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit deferred_echoTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcDeferredEchoResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDeferredEchoResponse, rrr::i32>::Err(__ret__);
            }
            RpcDeferredEchoResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.result;
            return rusty::Result<RpcDeferredEchoResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<deferred_echoTypedFuture, rrr::i32> async_deferred_echo(const RpcDeferredEchoRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(BenchmarkService::DEFERRED_ECHO, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.val;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<deferred_echoTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<deferred_echoTypedFuture, rrr::i32>::Ok(deferred_echoTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<deferred_echoTypedFuture> await_deferred_echo(const RpcDeferredEchoRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_deferred_echo(req, __fu_attr__));
    }
    rusty::Result<RpcDeferredEchoResponse, rrr::i32> deferred_echo(const RpcDeferredEchoRequest& req) {
        auto __typed_fu_result__ = this->async_deferred_echo(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDeferredEchoResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace benchmark


// optional %%: marks footer section, code below will be copied into end of generated C++ header

// BenchmarkService methods are implemented in src/rrr/tests/benchmark_service.cc using
// typed Rpc*Request/Rpc*Response signatures.

