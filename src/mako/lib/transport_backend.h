// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * transport_backend.h:
 *   Abstract interface for transport backends (eRPC, rrr/rpc, etc.)
 *   Allows runtime/build-time selection of transport implementation
 *
 **********************************************************************/

#ifndef _MAKO_TRANSPORT_BACKEND_H_
#define _MAKO_TRANSPORT_BACKEND_H_

#include <string>
#include <map>
#include <utility>
#include <cstdint>
#include <stdexcept>

// Forward declaration (global namespace)
class TransportReceiver;

namespace mako {

/**
 * Transport backend type enumeration
 *
 * ERPC:    High-performance RDMA-based transport (1-2 μs latency)
 * RRR_RPC: TCP/IP-based transport for portability (10-50 μs latency)
 */
enum class TransportType {
    ERPC = 0,      // eRPC - RDMA-based, high performance
    RRR_RPC = 1    // rrr/rpc - TCP/IP-based, portable
};

/**
 * Abstract transport backend interface
 *
 * This interface abstracts the underlying RPC framework, allowing
 * Mako to support multiple transport implementations. All backends
 * must implement this interface.
 *
 * Design principles:
 * - Maintain API compatibility with existing FastTransport
 * - Support both synchronous and asynchronous operation
 * - Allow raw buffer-based communication (no forced serialization)
 * - Enable backend-specific optimizations
 *
 * Thread safety: Individual methods are backend-dependent. Clients
 * should not assume thread-safety unless documented by the backend.
 */
class TransportBackend {
public:
    virtual ~TransportBackend() = default;

    // ===== Connection Management =====

    /**
     * Initialize the transport backend
     *
     * @param local_uri Local address to bind (e.g., "192.168.1.1:31850")
     * @param numa_node NUMA node for memory allocation (0-N)
     * @param phy_port Physical port for RDMA (eRPC only, 0 for TCP)
     * @param st_nr_req_types Starting request type ID to register
     * @param end_nr_req_types Ending request type ID to register
     * @return 0 on success, error code on failure
     *
     * This method sets up the backend's network endpoint and registers
     * handlers for all request types in the range [st_nr_req_types, end_nr_req_types].
     */
    virtual int Initialize(const std::string& local_uri,
                          uint8_t numa_node,
                          uint8_t phy_port,
                          uint8_t st_nr_req_types,
                          uint8_t end_nr_req_types) = 0;

    /**
     * Shutdown the transport backend
     *
     * Closes all connections, frees resources, and stops event loops.
     * After shutdown, the backend cannot be reused.
     */
    virtual void Shutdown() = 0;

    // ===== Message Buffer Management =====

    /**
     * Allocate a request buffer
     *
     * @param req_len Size of request buffer in bytes
     * @param resp_len Size of response buffer in bytes
     * @return Pointer to request buffer (caller fills this buffer)
     *
     * Returns a buffer that the caller can write request data into.
     * The buffer remains valid until the next AllocRequestBuffer() call
     * or until the request is sent.
     *
     * Note: For compatibility with existing code, this returns a raw char*
     * pointer. Backend implementations may use internal buffering.
     */
    virtual char* AllocRequestBuffer(size_t req_len, size_t resp_len) = 0;

    /**
     * Free the current request buffer
     *
     * Some backends may need explicit buffer cleanup. Call after
     * the request is sent or if the request is cancelled.
     */
    virtual void FreeRequestBuffer() = 0;

    // ===== Send Operations =====

    /**
     * Send a request to a specific shard
     *
     * @param src TransportReceiver that will handle the response
     * @param req_type Request type ID (registered in Initialize)
     * @param shard_idx Destination shard index (0 to nshards-1)
     * @param server_id Server ID within the shard (for parallel connections)
     * @param msg_len Length of message in the request buffer
     * @return true on success, false on failure
     *
     * Sends the request buffer (from AllocRequestBuffer) to the specified
     * shard. The call may block until the response is received, depending
     * on src->Blocked() implementation.
     *
     * Response will be delivered via src->ReceiveResponse(req_type, respBuf)
     */
    virtual bool SendToShard(TransportReceiver* src,
                            uint8_t req_type,
                            uint8_t shard_idx,
                            uint16_t server_id,
                            size_t msg_len) = 0;

    /**
     * Send a request to multiple shards (broadcast)
     *
     * @param src TransportReceiver that will handle responses
     * @param req_type Request type ID
     * @param shards_bit_set Bitmask of shards to send to (bit i = shard i)
     * @param server_id Server ID within each shard
     * @param resp_len Expected response length
     * @param req_len Request length in buffer
     * @param force_center Force sending to specific cluster (-1 for default)
     * @return true on success, false on failure
     *
     * Sends the same request to multiple shards in parallel. The call blocks
     * until all responses are received or src->Blocked() returns false.
     *
     * Responses are delivered via src->ReceiveResponse() as they arrive.
     */
    virtual bool SendToAll(TransportReceiver* src,
                          uint8_t req_type,
                          int shards_bit_set,
                          uint16_t server_id,
                          size_t resp_len,
                          size_t req_len,
                          int force_center = -1) = 0;

    /**
     * Send different requests to multiple shards (batch)
     *
     * @param src TransportReceiver that will handle responses
     * @param req_type Request type ID
     * @param server_id Server ID within each shard
     * @param resp_len Expected response length
     * @param data Map of shard_idx -> (request_buffer, request_length)
     * @return true on success, false on failure
     *
     * Sends different request payloads to different shards in parallel.
     * Used for batched operations where each shard receives different data.
     */
    virtual bool SendBatchToAll(TransportReceiver* src,
                               uint8_t req_type,
                               uint16_t server_id,
                               size_t resp_len,
                               const std::map<int, std::pair<char*, size_t>>& data) = 0;

    // ===== Event Loop =====

    /**
     * Run the event loop
     *
     * Processes network events, handles incoming requests, and delivers
     * responses. This method blocks until Stop() is called.
     *
     * For server-side operation, this must be called to process incoming
     * requests. For client-only operation, some backends may not require
     * this (e.g., if they use separate threads for event processing).
     */
    virtual void RunEventLoop() = 0;

    /**
     * Stop the event loop
     *
     * Signals RunEventLoop() to exit. Thread-safe and can be called
     * from a different thread than the one running RunEventLoop().
     */
    virtual void Stop() = 0;

    // ===== Statistics & Debugging =====

    /**
     * Print transport statistics
     *
     * Outputs backend-specific statistics (throughput, latency, etc.)
     * to the log. Useful for debugging and performance analysis.
     */
    virtual void PrintStats() = 0;

    // ===== Type Information =====

    /**
     * Get the backend type
     *
     * @return TransportType enum value
     *
     * Allows runtime identification of the active backend.
     */
    virtual TransportType GetType() const = 0;

    /**
     * Get a human-readable name for the backend
     *
     * @return Backend name string (e.g., "eRPC", "rrr/rpc")
     */
    virtual const char* GetName() const {
        switch (GetType()) {
            case TransportType::ERPC: return "eRPC";
            case TransportType::RRR_RPC: return "rrr/rpc";
            default: return "unknown";
        }
    }
};

/**
 * Parse transport type from string
 *
 * @param type_str String representation ("erpc" or "rrr")
 * @return TransportType enum value
 * @throws If type_str is invalid
 */
inline TransportType ParseTransportType(const std::string& type_str) {
    if (type_str == "erpc" || type_str == "ERPC") {
        return TransportType::ERPC;
    } else if (type_str == "rrr" || type_str == "RRR" || type_str == "rrr_rpc") {
        return TransportType::RRR_RPC;
    } else {
        throw std::runtime_error("Invalid transport type: " + type_str +
                                " (valid: erpc, rrr)");
    }
}

/**
 * Convert transport type to string
 *
 * @param type TransportType enum value
 * @return String representation ("erpc" or "rrr")
 */
inline const char* TransportTypeToString(TransportType type) {
    switch (type) {
        case TransportType::ERPC: return "erpc";
        case TransportType::RRR_RPC: return "rrr";
        default: return "unknown";
    }
}

} // namespace mako

#endif  /* _MAKO_TRANSPORT_BACKEND_H_ */
