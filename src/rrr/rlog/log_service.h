#pragma once

#include "rrr.hpp"

#include <errno.h>


namespace rlog {

class RLogService: public rrr::Service {
public:
    enum {
        LOG = 0x58b97d46,
        AGGREGATE_QPS = 0x4beb608a,
    };
    int __reg_to__(rrr::Server* svr) {
        int ret = 0;
        if ((ret = svr->reg(LOG, this, &RLogService::__log__wrapper__)) != 0) {
            goto err;
        }
        if ((ret = svr->reg(AGGREGATE_QPS, this, &RLogService::__aggregate_qps__wrapper__)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr->unreg(LOG);
        svr->unreg(AGGREGATE_QPS);
        return ret;
    }
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, remember to reply and handle rusty::Box<Request>; weak_ptr requires lock() before use
    virtual void log(const rrr::i32& level, const std::string& source, const rrr::i64& msg_id, const std::string& message) = 0;
    virtual void aggregate_qps(const std::string& metric_name, const rrr::i32& increment) = 0;
private:
    void __log__wrapper__(rusty::Box<rrr::Request> req, std::weak_ptr<rrr::ServerConnection> weak_sconn) {
        rrr::i32 in_0;
        req->m >> in_0;
        std::string in_1;
        req->m >> in_1;
        rrr::i64 in_2;
        req->m >> in_2;
        std::string in_3;
        req->m >> in_3;
        this->log(in_0, in_1, in_2, in_3);
        auto sconn = weak_sconn.lock();
        if (sconn) {
            sconn->begin_reply(req.get());
            sconn->end_reply();
        }
        // req (Box) automatically deleted when going out of scope
    }
    void __aggregate_qps__wrapper__(rusty::Box<rrr::Request> req, std::weak_ptr<rrr::ServerConnection> weak_sconn) {
        std::string in_0;
        req->m >> in_0;
        rrr::i32 in_1;
        req->m >> in_1;
        this->aggregate_qps(in_0, in_1);
        auto sconn = weak_sconn.lock();
        if (sconn) {
            sconn->begin_reply(req.get());
            sconn->end_reply();
        }
        // req (Box) automatically deleted when going out of scope
    }
};

class RLogProxy {
protected:
    rrr::Client* __cl__;
public:
    RLogProxy(rrr::Client* cl): __cl__(cl) { }
    rrr::Future* async_log(const rrr::i32& level, const std::string& source, const rrr::i64& msg_id, const std::string& message, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(RLogService::LOG, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << level;
            *__cl__ << source;
            *__cl__ << msg_id;
            *__cl__ << message;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 log(const rrr::i32& level, const std::string& source, const rrr::i64& msg_id, const std::string& message) {
        rrr::Future* __fu__ = this->async_log(level, source, msg_id, message);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
    rrr::Future* async_aggregate_qps(const std::string& metric_name, const rrr::i32& increment, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        rrr::Future* __fu__ = __cl__->begin_request(RLogService::AGGREGATE_QPS, __fu_attr__);
        if (__fu__ != nullptr) {
            *__cl__ << metric_name;
            *__cl__ << increment;
        }
        __cl__->end_request();
        return __fu__;
    }
    rrr::i32 aggregate_qps(const std::string& metric_name, const rrr::i32& increment) {
        rrr::Future* __fu__ = this->async_aggregate_qps(metric_name, increment);
        if (__fu__ == nullptr) {
            return ENOTCONN;
        }
        rrr::i32 __ret__ = __fu__->get_error_code();
        __fu__->release();
        return __ret__;
    }
};

} // namespace rlog



