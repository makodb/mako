#pragma once

#include "base/all.hpp"
#include <memory>
#include <cstddef>

namespace rrr {

/**
 * Transport abstraction for RDMA only.
 * TCP continues to use existing socket code directly.
 * This interface is only used when "rdma://" prefix is detected.
 */
class Transport {
public:
    virtual ~Transport() = default;

    /**
     * Connect to a remote address (client side).
     * @param addr Address in format "host:port" (rdma:// prefix already stripped)
     * @return 0 on success, error code on failure
     */
    virtual int connect(const char* addr) = 0;

    /**
     * Bind to a local address (server side).
     * @param addr Address in format "host:port" (rdma:// prefix already stripped)
     * @return 0 on success, error code on failure
     */
    virtual int bind(const char* addr) = 0;

    /**
     * Start listening for incoming connections (server side).
     * @return 0 on success, error code on failure
     */
    virtual int listen() = 0;

    /**
     * Accept an incoming connection (server side).
     * @return Shared pointer to new Transport for the accepted connection, or nullptr on error
     */
    virtual std::shared_ptr<Transport> accept() = 0;

    /**
     * Read data from the transport.
     * @param buf Buffer to read into
     * @param len Maximum number of bytes to read
     * @return Number of bytes read, or -1 on error
     */
    virtual ssize_t read(void* buf, size_t len) = 0;

    /**
     * Write data to the transport.
     * @param buf Buffer to write from
     * @param len Number of bytes to write
     * @return Number of bytes written, or -1 on error
     */
    virtual ssize_t write(const void* buf, size_t len) = 0;

    /**
     * Close the transport connection.
     */
    virtual void close() = 0;

    /**
     * Get file descriptor for polling (epoll/kqueue).
     * For RDMA: returns completion queue file descriptor
     * @return File descriptor, or -1 if not available
     */
    virtual int fd() = 0;

    /**
     * Check if transport is connected/ready.
     * @return true if connected, false otherwise
     */
    virtual bool is_connected() = 0;
};

/**
 * Factory function to create RDMA Transport.
 * Only called when "rdma://" prefix is detected.
 * @param addr Address string (rdma:// prefix already stripped)
 * @return Shared pointer to RDMATransport, or nullptr on error
 */
std::shared_ptr<Transport> create_rdma_transport(const char* addr);

} // namespace rrr

