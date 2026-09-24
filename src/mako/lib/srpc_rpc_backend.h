// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * srpc_rpc_backend.h:
 *   srpc/rpc transport backend implementation
 *   TCP/IP-based RPC for portability (10-50 μs latency)
 *
 **********************************************************************/

#ifndef _MAKO_SRPC_RPC_BACKEND_H_
#define _MAKO_SRPC_RPC_BACKEND_H_

#include "transport_request_handle.h"
#include "lib/configuration.h"
#include "lib/transport.h"

#include <rusty/box.hpp>
#include <rusty/option.hpp>
#include <rusty/arc.hpp>

// srpc/rpc library
#include "srpc/srpc.hpp"

#include <map>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

namespace mako {

// Forward declarations
class HelperQueue;
class SrpcRpcBackend;

/**
 * TransportBackendService: Service implementation for SrpcRpcBackend
 *
 * Handles a range of RPC IDs and forwards them to the backend's RequestHandler.
 * Service implementation that avoids std::function type erasure.
 */
class TransportBackendService {
public:
    TransportBackendService(SrpcRpcBackend* backend, srpc::i32 rpc_start, srpc::i32 rpc_end)
        : backend_(backend), rpc_start_(rpc_start), rpc_end_(rpc_end) {}

    // @safe - with @unsafe block for loop
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        // @unsafe - loop iteration
        {
            for (srpc::i32 rpc_id = rpc_start_; rpc_id <= rpc_end_; ++rpc_id) {
                int ret = svr.reg_rpc(rpc_id, svc_index);
                if (ret != 0) {
                    // Unregister on failure
                    for (srpc::i32 id = rpc_start_; id < rpc_id; ++id) {
                        svr.unreg(id);
                    }
                    return ret;
                }
            }
        }
        return 0;
    }

    // @safe
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req,
                      srpc::WeakServerConnection weak_sconn);

private:
    SrpcRpcBackend* backend_;
    srpc::i32 rpc_start_;
    srpc::i32 rpc_end_;
};

/**
 * SrpcRequestHandle: srpc/rpc implementation of TransportRequestHandle
 *
 * Implements the transport-agnostic interface for srpc/rpc requests.
 * Stores request/response data extracted from srpc::Request.
 */

class SrpcRequestHandle : public TransportRequestHandle {
public:
    // Request data buffer (extracted from srpc::Request)
    std::vector<char> request_data;

    // Response buffer (to be filled by worker thread)
    std::vector<char> response_data;

    // Connection to send response back
    rusty::Arc<srpc::ServerConnection> sconn;

    // Original request (needed for begin_reply)
    rusty::Box<srpc::Request> original_request;

    // Request type
    uint8_t req_type;

    // Backend and server ID for response enqueueing
    SrpcRpcBackend* backend;
    uint16_t server_id;

    // Constructor (Box requires move semantics, no default constructor)
    SrpcRequestHandle(rusty::Box<srpc::Request>&& req, rusty::Arc<srpc::ServerConnection> conn, uint8_t type,
                     SrpcRpcBackend* be, uint16_t sid)
        : original_request(std::move(req)), sconn(std::move(conn)), req_type(type), backend(be), server_id(sid) {}

    // Move constructor/assignment (Box is move-only)
    SrpcRequestHandle(SrpcRequestHandle&& other) = default;
    SrpcRequestHandle& operator=(SrpcRequestHandle&& other) = default;

    // Delete copy (Box cannot be copied)
    SrpcRequestHandle(const SrpcRequestHandle&) = delete;
    SrpcRequestHandle& operator=(const SrpcRequestHandle&) = delete;

    // TransportRequestHandle interface implementation
    uint8_t GetRequestType() const override {
        return req_type;
    }

    char* GetRequestBuffer() override {
        return request_data.data();
    }

    char* GetResponseBuffer() override {
        return response_data.data();
    }

    void* GetOpaqueHandle() override {
        return static_cast<void*>(this);
    }

    void EnqueueResponse(size_t msg_size) override;  // Implemented in srpc_rpc_backend.cc
};

/**
 * srpc/rpc Transport Backend
 *
 * TCP/IP-based RPC implementation using the srpc/rpc library.
 * Provides portable communication with moderate latency (~10-50 μs).
 *
 * Thread safety: Individual methods are NOT thread-safe. Caller must
 * ensure proper synchronization if using from multiple threads.
 */
class SrpcRpcBackend {
    // Grant access to RequestHandler for dispatch
    friend class TransportBackendService;

public:
    SrpcRpcBackend(const transport::Configuration& config,
                  int shard_idx,
                  uint16_t id,
                  const std::string& cluster);

    ~SrpcRpcBackend();

    // srpc/rpc transport operations (formerly the TransportBackend interface)
    int Initialize(const std::string& local_uri,
                   uint8_t numa_node,
                   uint8_t phy_port,
                   uint8_t st_nr_req_types,
                   uint8_t end_nr_req_types);

    void Shutdown();

    char* AllocRequestBuffer(size_t req_len, size_t resp_len);
    void FreeRequestBuffer();

    bool SendToShard(TransportReceiver* src,
                    uint8_t req_type,
                    uint8_t shard_idx,
                    uint16_t server_id,
                    size_t msg_len);

    bool SendToAll(TransportReceiver* src,
                   uint8_t req_type,
                   int shards_bit_set,
                   uint16_t server_id,
                   size_t resp_len,
                   size_t req_len,
                   int force_center = -1);

    bool SendBatchToAll(TransportReceiver* src,
                       uint8_t req_type,
                       uint16_t server_id,
                       size_t resp_len,
                       const std::map<int, std::pair<char*, size_t>>& data);

    void RunEventLoop();
    void Stop();

    void PrintStats();

    // Set helper queues (for server-side request handling)
    void SetHelperQueues(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues) {
        queue_holders_ = queues;
    }

    void SetHelperQueuesResponse(const std::unordered_map<uint16_t, mako::HelperQueue*>& queues) {
        queue_holders_response_ = queues;
    }

    const std::unordered_map<uint16_t, mako::HelperQueue*>& GetHelperQueues() const {
        return queue_holders_;
    }

    const std::unordered_map<uint16_t, mako::HelperQueue*>& GetHelperQueuesResponse() const {
        return queue_holders_response_;
    }

private:
    // Configuration
    transport::Configuration config_;
    int shard_idx_;
    uint16_t id_;
    std::string cluster_;
    int cluster_role_;

    // srpc/rpc state
    rusty::Option<rusty::Arc<srpc::PollThread>> poll_thread_worker_;
    srpc::Server* server_{nullptr};

    // Client connections: {(cluster_role, shard_idx, server_id) -> Client}
    std::map<std::tuple<uint8_t, uint8_t, uint16_t>, rusty::Arc<srpc::Client>> clients_;
    std::mutex clients_lock_;

    // Runtime state
    std::atomic<bool> stop_{false};
    std::atomic<bool> event_loop_running_{false};

    // Helper queues for server-side processing
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_;
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_response_;

    // Statistics - atomic for thread-safe concurrent updates from network threads
    // @unsafe { std::atomic is not borrow-checked but required for multi-threaded counters }
    std::atomic<uint64_t> msg_size_req_sent_{0};
    std::atomic<int> msg_counter_req_sent_{0};
    std::atomic<uint64_t> msg_size_resp_sent_{0};
    std::atomic<int> msg_counter_resp_sent_{0};

    // Srpc request handle storage for helper queue processing
    // Maps the opaque request token (used as key) to SrpcRequestHandle data
    std::map<void*, std::unique_ptr<SrpcRequestHandle>> srpc_request_map_;
    std::mutex srpc_request_map_lock_;

    // Internal helper methods
    rusty::Option<rusty::Arc<srpc::Client>> GetOrCreateClient(uint8_t shard_idx, uint16_t server_id, int force_center = -1);

    // Static request handler for srpc::Server
    static void RequestHandler(uint8_t req_type, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn, SrpcRpcBackend* backend);
};

} // namespace mako

#endif  /* _MAKO_SRPC_RPC_BACKEND_H_ */
