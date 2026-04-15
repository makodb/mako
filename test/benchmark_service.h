#pragma once

#include "rrr.hpp"

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

class BenchmarkService : public rrr::Service {
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

    struct RpcFastNopResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastNopResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastNopResponse& o) {
        return m;
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

    struct RpcNopResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcNopResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcNopResponse& o) {
        return m;
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

    struct RpcSleepResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSleepResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSleepResponse& o) {
        return m;
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
        SLEEP = 0x22cb72f2,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(FAST_PRIME, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAST_DOT_PROD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAST_ADD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAST_NOP, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAST_VEC, svc_index)) != 0) {
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
        if ((ret = svr.reg_rpc(SLEEP, svc_index)) != 0) {
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
        svr.unreg(SLEEP);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
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
        case SLEEP: __sleep__wrapper__(std::move(req), weak_sconn); break;
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
    virtual rusty::Result<RpcSleepResponse, rrr::i32> sleep(const RpcSleepRequest& req);
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
    using RpcSleepRequest = BenchmarkService::RpcSleepRequest;
    using RpcSleepResponse = BenchmarkService::RpcSleepResponse;
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
    rusty::Result<RpcFastPrimeResponse, rrr::i32> fast_prime(const RpcFastPrimeRequest& req) {
        auto __typed_fu_result__ = this->async_fast_prime(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastPrimeResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_fast_prime(const RpcFastPrimeRequest&) instead")]]
    rrr::FutureResult async_fast_prime(const rrr::i32& n, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFastPrimeRequest __req__;
        __req__.n = n;
        auto __typed_result__ = this->async_fast_prime(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed fast_prime(const RpcFastPrimeRequest&) instead")]]
    rrr::i32 fast_prime(const rrr::i32& n, rrr::i8* flag) {
        RpcFastPrimeRequest __req__;
        __req__.n = n;
        auto __typed_result__ = this->fast_prime(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (flag) *flag = __resp__.flag;
        return 0;
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
    rusty::Result<RpcFastDotProdResponse, rrr::i32> fast_dot_prod(const RpcFastDotProdRequest& req) {
        auto __typed_fu_result__ = this->async_fast_dot_prod(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastDotProdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_fast_dot_prod(const RpcFastDotProdRequest&) instead")]]
    rrr::FutureResult async_fast_dot_prod(const point3& p1, const point3& p2, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFastDotProdRequest __req__;
        __req__.p1 = p1;
        __req__.p2 = p2;
        auto __typed_result__ = this->async_fast_dot_prod(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed fast_dot_prod(const RpcFastDotProdRequest&) instead")]]
    rrr::i32 fast_dot_prod(const point3& p1, const point3& p2, double* v) {
        RpcFastDotProdRequest __req__;
        __req__.p1 = p1;
        __req__.p2 = p2;
        auto __typed_result__ = this->fast_dot_prod(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (v) *v = __resp__.v;
        return 0;
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
    rusty::Result<RpcFastAddResponse, rrr::i32> fast_add(const RpcFastAddRequest& req) {
        auto __typed_fu_result__ = this->async_fast_add(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastAddResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_fast_add(const RpcFastAddRequest&) instead")]]
    rrr::FutureResult async_fast_add(const rrr::v32& a, const rrr::v32& b, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFastAddRequest __req__;
        __req__.a = a;
        __req__.b = b;
        auto __typed_result__ = this->async_fast_add(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed fast_add(const RpcFastAddRequest&) instead")]]
    rrr::i32 fast_add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b) {
        RpcFastAddRequest __req__;
        __req__.a = a;
        __req__.b = b;
        auto __typed_result__ = this->fast_add(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (a_add_b) *a_add_b = __resp__.a_add_b;
        return 0;
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
    rusty::Result<RpcFastNopResponse, rrr::i32> fast_nop(const RpcFastNopRequest& req) {
        auto __typed_fu_result__ = this->async_fast_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastNopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_fast_nop(const RpcFastNopRequest&) instead")]]
    rrr::FutureResult async_fast_nop(const std::string& in_0, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFastNopRequest __req__;
        __req__.in_0 = in_0;
        auto __typed_result__ = this->async_fast_nop(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed fast_nop(const RpcFastNopRequest&) instead")]]
    rrr::i32 fast_nop(const std::string& in_0) {
        RpcFastNopRequest __req__;
        __req__.in_0 = in_0;
        auto __typed_result__ = this->fast_nop(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
    rusty::Result<RpcFastVecResponse, rrr::i32> fast_vec(const RpcFastVecRequest& req) {
        auto __typed_fu_result__ = this->async_fast_vec(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastVecResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_fast_vec(const RpcFastVecRequest&) instead")]]
    rrr::FutureResult async_fast_vec(const rrr::i32& n, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFastVecRequest __req__;
        __req__.n = n;
        auto __typed_result__ = this->async_fast_vec(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed fast_vec(const RpcFastVecRequest&) instead")]]
    rrr::i32 fast_vec(const rrr::i32& n, std::vector<rrr::i64>* v) {
        RpcFastVecRequest __req__;
        __req__.n = n;
        auto __typed_result__ = this->fast_vec(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (v) *v = __resp__.v;
        return 0;
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
    rusty::Result<RpcPrimeResponse, rrr::i32> prime(const RpcPrimeRequest& req) {
        auto __typed_fu_result__ = this->async_prime(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrimeResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_prime(const RpcPrimeRequest&) instead")]]
    rrr::FutureResult async_prime(const rrr::i32& n, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcPrimeRequest __req__;
        __req__.n = n;
        auto __typed_result__ = this->async_prime(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed prime(const RpcPrimeRequest&) instead")]]
    rrr::i32 prime(const rrr::i32& n, rrr::i8* flag) {
        RpcPrimeRequest __req__;
        __req__.n = n;
        auto __typed_result__ = this->prime(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (flag) *flag = __resp__.flag;
        return 0;
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
    rusty::Result<RpcDotProdResponse, rrr::i32> dot_prod(const RpcDotProdRequest& req) {
        auto __typed_fu_result__ = this->async_dot_prod(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDotProdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_dot_prod(const RpcDotProdRequest&) instead")]]
    rrr::FutureResult async_dot_prod(const point3& p1, const point3& p2, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcDotProdRequest __req__;
        __req__.p1 = p1;
        __req__.p2 = p2;
        auto __typed_result__ = this->async_dot_prod(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed dot_prod(const RpcDotProdRequest&) instead")]]
    rrr::i32 dot_prod(const point3& p1, const point3& p2, double* v) {
        RpcDotProdRequest __req__;
        __req__.p1 = p1;
        __req__.p2 = p2;
        auto __typed_result__ = this->dot_prod(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (v) *v = __resp__.v;
        return 0;
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
    rusty::Result<RpcAddResponse, rrr::i32> add(const RpcAddRequest& req) {
        auto __typed_fu_result__ = this->async_add(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAddResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_add(const RpcAddRequest&) instead")]]
    rrr::FutureResult async_add(const rrr::v32& a, const rrr::v32& b, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAddRequest __req__;
        __req__.a = a;
        __req__.b = b;
        auto __typed_result__ = this->async_add(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed add(const RpcAddRequest&) instead")]]
    rrr::i32 add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b) {
        RpcAddRequest __req__;
        __req__.a = a;
        __req__.b = b;
        auto __typed_result__ = this->add(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (a_add_b) *a_add_b = __resp__.a_add_b;
        return 0;
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
    rusty::Result<RpcNopResponse, rrr::i32> nop(const RpcNopRequest& req) {
        auto __typed_fu_result__ = this->async_nop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcNopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_nop(const RpcNopRequest&) instead")]]
    rrr::FutureResult async_nop(const std::string& in_0, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcNopRequest __req__;
        __req__.in_0 = in_0;
        auto __typed_result__ = this->async_nop(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed nop(const RpcNopRequest&) instead")]]
    rrr::i32 nop(const std::string& in_0) {
        RpcNopRequest __req__;
        __req__.in_0 = in_0;
        auto __typed_result__ = this->nop(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
    rusty::Result<RpcSleepResponse, rrr::i32> sleep(const RpcSleepRequest& req) {
        auto __typed_fu_result__ = this->async_sleep(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSleepResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_sleep(const RpcSleepRequest&) instead")]]
    rrr::FutureResult async_sleep(const double& sec, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSleepRequest __req__;
        __req__.sec = sec;
        auto __typed_result__ = this->async_sleep(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed sleep(const RpcSleepRequest&) instead")]]
    rrr::i32 sleep(const double& sec) {
        RpcSleepRequest __req__;
        __req__.sec = sec;
        auto __typed_result__ = this->sleep(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

} // namespace benchmark


// optional %%: marks footer section, code below will be copied into end of generated C++ header

// BenchmarkService methods are implemented in test/benchmark_service.cc using
// typed Rpc*Request/Rpc*Response signatures.

