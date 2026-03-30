// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * erpc_request_handle.h:
 *   eRPC-specific implementation of TransportRequestHandle
 *
 **********************************************************************/

#ifndef _MAKO_ERPC_REQUEST_HANDLE_H_
#define _MAKO_ERPC_REQUEST_HANDLE_H_

#include "transport_request_handle.h"
#include "rpc.h"  // eRPC

namespace mako {

// Forward declaration
class ErpcBackend;

/**
 * eRPC implementation of TransportRequestHandle
 *
 * Wraps erpc::ReqHandle* to provide transport-agnostic interface
 */
class ErpcRequestHandle : public TransportRequestHandle {
public:
    ErpcRequestHandle(erpc::ReqHandle* handle, ErpcBackend* backend, uint16_t server_id)
        : handle_(handle), backend_(backend), server_id_(server_id) {}

    ~ErpcRequestHandle() override = default;

    // TransportRequestHandle interface implementation
    uint8_t GetRequestType() const override {
        return handle_->get_req_msgbuf()->get_req_type();
    }

    char* GetRequestBuffer() override {
        return reinterpret_cast<char*>(handle_->get_req_msgbuf()->buf_);
    }

    char* GetResponseBuffer() override {
        return reinterpret_cast<char*>(handle_->pre_resp_msgbuf_.buf_);
    }

    void* GetOpaqueHandle() override {
        // Return the wrapper pointer (this), NOT the underlying eRPC handle
        // This must match the key we stored in the map during RequestHandler
        return static_cast<void*>(this);
    }

    void EnqueueResponse(size_t msg_size) override;  // Implemented in erpc_backend.cc

    // eRPC-specific accessor (for backend use)
    erpc::ReqHandle* GetErpcHandle() const {
        return handle_;
    }

private:
    erpc::ReqHandle* handle_;
    ErpcBackend* backend_;
    uint16_t server_id_;
};

} // namespace mako

#endif  /* _MAKO_ERPC_REQUEST_HANDLE_H_ */
