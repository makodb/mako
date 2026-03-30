#pragma once

#include "rrr.hpp"

#include <errno.h>

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

class BenchmarkService: public rrr::Service {
public:
    enum {
        FAST_PRIME = 0x2a7259ef,
        FAST_DOT_PROD = 0x4dd80f8c,
        FAST_ADD = 0x5db68837,
        FAST_NOP = 0x21bfc0ab,
        FAST_VEC = 0x5322e011,
        PRIME = 0x3ec8d31a,
        DOT_PROD = 0x53be0a74,
        ADD = 0x6008b4f5,
        NOP = 0x12d59407,
        SLEEP = 0x364f2762,
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
    // @safe - Virtual dispatch for RPC requests
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
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
    virtual void fast_prime(const rrr::i32& n, rrr::i8* flag);
    virtual void fast_dot_prod(const point3& p1, const point3& p2, double* v);
    virtual void fast_add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b);
    virtual void fast_nop(const std::string&);
    virtual void fast_vec(const rrr::i32& n, std::vector<rrr::i64>* v);
    virtual void prime(const rrr::i32& n, rrr::i8* flag);
    virtual void dot_prod(const point3& p1, const point3& p2, double* v);
    virtual void add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b);
    virtual void nop(const std::string&);
    virtual void sleep(const double& sec);
private:
    void __fast_prime__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        rrr::i32 in_0;
        req->m >> in_0;
        rrr::i8 out_0;
        this->fast_prime(in_0, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __fast_dot_prod__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        point3 in_0;
        req->m >> in_0;
        point3 in_1;
        req->m >> in_1;
        double out_0;
        this->fast_dot_prod(in_0, in_1, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __fast_add__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        rrr::v32 in_0;
        req->m >> in_0;
        rrr::v32 in_1;
        req->m >> in_1;
        rrr::v32 out_0;
        this->fast_add(in_0, in_1, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __fast_nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::string in_0;
        req->m >> in_0;
        this->fast_nop(in_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
        }
        // req automatically cleaned up by rusty::Box
    }
    void __fast_vec__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        rrr::i32 in_0;
        req->m >> in_0;
        std::vector<rrr::i64> out_0;
        this->fast_vec(in_0, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __prime__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        rrr::i32 in_0;
        req->m >> in_0;
        rrr::i8 out_0;
        this->prime(in_0, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __dot_prod__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        point3 in_0;
        req->m >> in_0;
        point3 in_1;
        req->m >> in_1;
        double out_0;
        this->dot_prod(in_0, in_1, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __add__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        rrr::v32 in_0;
        req->m >> in_0;
        rrr::v32 in_1;
        req->m >> in_1;
        rrr::v32 out_0;
        this->add(in_0, in_1, &out_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {
                m << out_0;
            });
        }
        // req automatically cleaned up by rusty::Box
    }
    void __nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::string in_0;
        req->m >> in_0;
        this->nop(in_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
        }
        // req automatically cleaned up by rusty::Box
    }
    void __sleep__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        double in_0;
        req->m >> in_0;
        this->sleep(in_0);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
        }
        // req automatically cleaned up by rusty::Box
    }
};

class BenchmarkProxy {
protected:
    rrr::Client* __cl__;
public:
    BenchmarkProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::FutureResult async_fast_prime(const rrr::i32& n, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::FAST_PRIME, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << n;
        });
    }
    rrr::i32 fast_prime(const rrr::i32& n, rrr::i8* flag) {
        auto __fu_result__ = this->async_fast_prime(n);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *flag;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_fast_dot_prod(const point3& p1, const point3& p2, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::FAST_DOT_PROD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << p1;
            __m__ << p2;
        });
    }
    rrr::i32 fast_dot_prod(const point3& p1, const point3& p2, double* v) {
        auto __fu_result__ = this->async_fast_dot_prod(p1, p2);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *v;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_fast_add(const rrr::v32& a, const rrr::v32& b, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::FAST_ADD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << a;
            __m__ << b;
        });
    }
    rrr::i32 fast_add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b) {
        auto __fu_result__ = this->async_fast_add(a, b);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *a_add_b;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_fast_nop(const std::string& in_0, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::FAST_NOP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << in_0;
        });
    }
    rrr::i32 fast_nop(const std::string& in_0) {
        auto __fu_result__ = this->async_fast_nop(in_0);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_fast_vec(const rrr::i32& n, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::FAST_VEC, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << n;
        });
    }
    rrr::i32 fast_vec(const rrr::i32& n, std::vector<rrr::i64>* v) {
        auto __fu_result__ = this->async_fast_vec(n);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *v;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_prime(const rrr::i32& n, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::PRIME, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << n;
        });
    }
    rrr::i32 prime(const rrr::i32& n, rrr::i8* flag) {
        auto __fu_result__ = this->async_prime(n);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *flag;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_dot_prod(const point3& p1, const point3& p2, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::DOT_PROD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << p1;
            __m__ << p2;
        });
    }
    rrr::i32 dot_prod(const point3& p1, const point3& p2, double* v) {
        auto __fu_result__ = this->async_dot_prod(p1, p2);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *v;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_add(const rrr::v32& a, const rrr::v32& b, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::ADD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << a;
            __m__ << b;
        });
    }
    rrr::i32 add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b) {
        auto __fu_result__ = this->async_add(a, b);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        if (__ret__ == 0) {
            __fu__->get_reply() >> *a_add_b;
        }
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_nop(const std::string& in_0, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::NOP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << in_0;
        });
    }
    rrr::i32 nop(const std::string& in_0) {
        auto __fu_result__ = this->async_nop(in_0);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_sleep(const double& sec, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(BenchmarkService::SLEEP, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << sec;
        });
    }
    rrr::i32 sleep(const double& sec) {
        auto __fu_result__ = this->async_sleep(sec);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
};

} // namespace benchmark


// optional %%: marks footer section, code below will be copied into end of generated C++ header

namespace benchmark {

inline void BenchmarkService::fast_dot_prod(const point3& p1, const point3& p2, double* v) {
    *v = p1.x * p2.x + p1.y * p2.y + p1.z * p2.z;
}

inline void BenchmarkService::fast_add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b) {
    a_add_b->set(a.get() + b.get());
}

inline void BenchmarkService::prime(const rrr::i32& n, rrr::i8* flag) {
    return fast_prime(n, flag);
}

inline void BenchmarkService::dot_prod(const point3& p1, const point3& p2, double *v) {
    *v = p1.x * p2.x + p1.y * p2.y + p1.z * p2.z;
}

inline void BenchmarkService::add(const rrr::v32& a, const rrr::v32& b, rrr::v32* a_add_b) {
    a_add_b->set(a.get() + b.get());
}

inline void BenchmarkService::fast_nop(const std::string& str) {
    nop(str);
}

} // namespace benchmark

