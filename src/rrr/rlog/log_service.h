#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace rlog {

class RLogService: public rrr::Service {
public:
    enum {
        LOG = 0x64f5fb19,
        AGGREGATE_QPS = 0x168dd4e3,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(LOG, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(AGGREGATE_QPS, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(LOG);
        svr.unreg(AGGREGATE_QPS);
        return ret;
    }
    // @safe - Virtual dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case LOG: __log__wrapper__(std::move(req), weak_sconn); break;
        case AGGREGATE_QPS: __aggregate_qps__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
    virtual void log(const rrr::i32& level, const std::string& source, const rrr::i64& msg_id, const std::string& message) = 0;
    virtual void aggregate_qps(const std::string& metric_name, const rrr::i32& increment) = 0;
private:
    void __log__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        rrr::i32 in_0;
        req->m >> in_0;
        std::string in_1;
        req->m >> in_1;
        rrr::i64 in_2;
        req->m >> in_2;
        std::string in_3;
        req->m >> in_3;
        this->log(in_0, in_1, in_2, in_3);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
        }
        // req automatically cleaned up by rusty::Box
    }
    void __aggregate_qps__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        std::string in_0;
        req->m >> in_0;
        rrr::i32 in_1;
        req->m >> in_1;
        this->aggregate_qps(in_0, in_1);
        auto sconn_opt = weak_sconn.upgrade();
        if (sconn_opt.is_some()) {
            auto sconn = sconn_opt.unwrap();
            const_cast<rrr::ServerConnection&>(*sconn).reply(*req);
        }
        // req automatically cleaned up by rusty::Box
    }
};

class RLogProxy {
protected:
    rrr::Client* __cl__;
public:
    RLogProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::FutureResult async_log(const rrr::i32& level, const std::string& source, const rrr::i64& msg_id, const std::string& message, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(RLogService::LOG, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << level;
            __m__ << source;
            __m__ << msg_id;
            __m__ << message;
        });
    }
    rrr::i32 log(const rrr::i32& level, const std::string& source, const rrr::i64& msg_id, const std::string& message) {
        auto __fu_result__ = this->async_log(level, source, msg_id, message);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
    rrr::FutureResult async_aggregate_qps(const std::string& metric_name, const rrr::i32& increment, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return __cl__->request(RLogService::AGGREGATE_QPS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << metric_name;
            __m__ << increment;
        });
    }
    rrr::i32 aggregate_qps(const std::string& metric_name, const rrr::i32& increment) {
        auto __fu_result__ = this->async_aggregate_qps(metric_name, increment);
        if (__fu_result__.is_err()) {
            return __fu_result__.unwrap_err();  // Return error code
        }
        auto __fu__ = __fu_result__.unwrap();
        rrr::i32 __ret__ = __fu__->get_error_code();
        // Arc auto-released
        return __ret__;
    }
};

} // namespace rlog



