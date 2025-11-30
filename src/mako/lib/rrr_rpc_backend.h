// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * rrr_rpc_backend.h:
 *   rrr/rpc transport backend implementation
 *   TCP/IP-based RPC for portability (10-50 μs latency)
 *
 **********************************************************************/

#ifndef _MAKO_RRR_RPC_BACKEND_H_
#define _MAKO_RRR_RPC_BACKEND_H_

#include "transport_backend.h"
#include "transport_request_handle.h"
#include "lib/configuration.h"
#include "lib/transport.h"

// rrr/rpc library
#include "rrr/rpc/client.hpp"
#include "rrr/rpc/server.hpp"
#include "rrr/reactor/reactor.h"

#include <map>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <atomic>

namespace mako {

// Forward declarations
class HelperQueue;

/**
 * RrrRequestHandle: rrr/rpc implementation of TransportRequestHandle
 *
 * Implements the transport-agnostic interface for rrr/rpc requests.
 * Stores request/response data extracted from rrr::Request.
 */
// Forward declaration (defined below)
class RrrRpcBackend;

class RrrRequestHandle : public TransportRequestHandle {
public:
    // Request data buffer (extracted from rrr::Request)
    std::vector<char> request_data;

    // Response buffer (to be filled by worker thread)
    std::vector<char> response_data;

    // Connection to send response back
    rusty::Arc<rrr::ServerConnection> sconn;

    // Original request (needed for begin_reply)
    rusty::Box<rrr::Request> original_request;

    // Request type
    uint8_t req_type;

    // Backend and server ID for response enqueueing
    RrrRpcBackend* backend;
    uint16_t server_id;

    // Constructor (Box requires move semantics, no default constructor)
    RrrRequestHandle(rusty::Box<rrr::Request>&& req, rusty::Arc<rrr::ServerConnection> conn, uint8_t type,
                     RrrRpcBackend* be, uint16_t sid)
        : original_request(std::move(req)), sconn(std::move(conn)), req_type(type), backend(be), server_id(sid) {}

    // Move constructor/assignment (Box is move-only)
    RrrRequestHandle(RrrRequestHandle&& other) = default;
    RrrRequestHandle& operator=(RrrRequestHandle&& other) = default;

    // Delete copy (Box cannot be copied)
    RrrRequestHandle(const RrrRequestHandle&) = delete;
    RrrRequestHandle& operator=(const RrrRequestHandle&) = delete;

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

    void EnqueueResponse(size_t msg_size) override;  // Implemented in rrr_rpc_backend.cc
};

/**
 * rrr/rpc Transport Backend
 *
 * TCP/IP-based RPC implementation using the rrr/rpc library.
 * Provides portable communication with moderate latency (~10-50 μs).
 *
 * Thread safety: Individual methods are NOT thread-safe. Caller must
 * ensure proper synchronization if using from multiple threads.
 */
class RrrRpcBackend : public TransportBackend {
public:
    RrrRpcBackend(const transport::Configuration& config,
                  int shard_idx,
                  uint16_t id,
                  const std::string& cluster);

    virtual ~RrrRpcBackend();

    // TransportBackend interface implementation
    int Initialize(const std::string& local_uri,
                   uint8_t numa_node,
                   uint8_t phy_port,
                   uint8_t st_nr_req_types,
                   uint8_t end_nr_req_types) override;

    void Shutdown() override;

    char* AllocRequestBuffer(size_t req_len, size_t resp_len) override;
    void FreeRequestBuffer() override;

    bool SendToShard(TransportReceiver* src,
                    uint8_t req_type,
                    uint8_t shard_idx,
                    uint16_t server_id,
                    size_t msg_len) override;

    bool SendToAll(TransportReceiver* src,
                   uint8_t req_type,
                   int shards_bit_set,
                   uint16_t server_id,
                   size_t resp_len,
                   size_t req_len,
                   int force_center = -1) override;

    bool SendBatchToAll(TransportReceiver* src,
                       uint8_t req_type,
                       uint16_t server_id,
                       size_t resp_len,
                       const std::map<int, std::pair<char*, size_t>>& data) override;

    void RunEventLoop() override;
    void Stop() override;

    void PrintStats() override;

    TransportType GetType() const override { return TransportType::RRR_RPC; }

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

    // rrr/rpc state
    rusty::Option<rusty::Arc<rrr::PollThread>> poll_thread_worker_;
    rrr::Server* server_{nullptr};

    // Client connections: {(cluster_role, shard_idx, server_id) -> Client}
    std::map<std::tuple<uint8_t, uint8_t, uint16_t>, rusty::Arc<rrr::Client>> clients_;
    std::mutex clients_lock_;

    // Runtime state
    std::atomic<bool> stop_{false};
    std::atomic<bool> event_loop_running_{false};

    // Helper queues for server-side processing
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_;
    std::unordered_map<uint16_t, mako::HelperQueue*> queue_holders_response_;

    // Statistics
    uint64_t msg_size_req_sent_{0};
    int msg_counter_req_sent_{0};
    uint64_t msg_size_resp_sent_{0};
    int msg_counter_resp_sent_{0};

    // Rrr request handle storage for helper queue processing
    // Maps erpc::ReqHandle* (used as key) to RrrRequestHandle data
    std::map<void*, std::unique_ptr<RrrRequestHandle>> rrr_request_map_;
    std::mutex rrr_request_map_lock_;

    // Internal helper methods
    rusty::Option<rusty::Arc<rrr::Client>> GetOrCreateClient(uint8_t shard_idx, uint16_t server_id, int force_center = -1);

    // Static request handler for rrr::Server
    static void RequestHandler(uint8_t req_type, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn, RrrRpcBackend* backend);
};

} // namespace mako

#endif  /* _MAKO_RRR_RPC_BACKEND_H_ */
