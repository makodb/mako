// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * transport_request_handle.h:
 *   Abstract interface for transport-agnostic request handling
 *
 * Purpose:
 *   Provides a common interface for accessing request/response buffers
 *   across different transport backends (eRPC, rrr/rpc, etc.)
 *
 **********************************************************************/

#ifndef _MAKO_TRANSPORT_REQUEST_HANDLE_H_
#define _MAKO_TRANSPORT_REQUEST_HANDLE_H_

#include <cstdint>
#include <cstddef>

namespace mako {

/**
 * Abstract interface for transport request handles
 *
 * This interface allows worker threads to process requests without
 * knowing the underlying transport implementation (eRPC, rrr/rpc, etc.)
 */
class TransportRequestHandle {
public:
    virtual ~TransportRequestHandle() = default;

    /**
     * Get the request type
     * @return Request type identifier
     */
    virtual uint8_t GetRequestType() const = 0;

    /**
     * Get pointer to request buffer
     * @return Pointer to request data
     */
    virtual char* GetRequestBuffer() = 0;

    /**
     * Get pointer to response buffer
     * @return Pointer to response data (to be filled by worker)
     */
    virtual char* GetResponseBuffer() = 0;

    /**
     * Get the underlying handle pointer (for queue operations)
     * This returns an opaque pointer that can be passed back to the
     * transport backend via helper queues.
     * @return Opaque pointer to underlying transport-specific handle
     */
    virtual void* GetOpaqueHandle() = 0;

    /**
     * Enqueue response for sending back to client
     * Called by worker thread after processing request and filling response buffer.
     * For eRPC: No-op (eRPC handles response enqueueing automatically)
     * For rrr/rpc: Enqueues to response queue for RunEventLoop to process
     * @param msg_size Size of response data in bytes
     */
    virtual void EnqueueResponse(size_t msg_size) = 0;
};

} // namespace mako

#endif  /* _MAKO_TRANSPORT_REQUEST_HANDLE_H_ */
